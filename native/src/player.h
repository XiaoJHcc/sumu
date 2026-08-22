// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// sumu's promoted native present kernel (pybind11 module `sumu_core`, class `Player`).
// Promoted from the three validated spikes (see docs/spike_results.md):
//   - spike0_d3d11_present: D3D11 flip-model swapchain + FFmpeg d3d11va zero-copy decode.
//   - spike1_cuda_interop:  CUDA driver-API + torch primary context, zero-copy CUDA<->D3D11.
//   - spike2_clock_mixing:  three-thread clock-driven mixing kernel (decode thread + original
//                           ring buffer, AI-push thread + ready-map, present thread wall-clock
//                           frame picking, single explicit d3d_mutex_ serializing ALL threads'
//                           touches of the shared D3D11/CUDA-interop context -- this was
//                           spike2's key architectural finding and is preserved verbatim).
//
// New in this promotion: seek(frame_num) as a REPOSITION, not a teardown (I6) -- see the
// seek() method below and docs/native_core.md for the full design writeup and measurements.
//
// Threading model (unchanged from spike2, see spike2's presenter.cpp header for the full
// story of why a single d3d_mutex_ is required on top of ID3D11Multithread::SetMultithread-
// Protected(TRUE)):
//   - decode thread:  FFmpeg d3d11va decode -> CopySubresourceRegion into a persistent NV12
//                     "passthrough ring" Texture2DArray, throttled to stay within
//                     decode_ahead_max_ frames of the present head.
//   - AI-push thread (Python, not owned here): push_ai_frame() does the CUDA<->D3D11
//                     zero-copy bridge into a landing texture, then a CopySubresourceRegion
//                     into the AI ready-map's Texture2DArray slot, THEN marks that slot ready.
//   - present thread: wall-clock QPC pacing, picks AI-fresh -> passthrough-fresh -> repeat-
//                     last-shown every tick, NEVER blocks on decode or AI-push.
//
// seek()'s concurrency contract (the new part): `decoder_mutex_` now wraps not just the
// Decoder object itself but the WHOLE "produce one frame, copy it into the ring, tag it
// ready" sequence in BOTH decode_loop() and seek() -- widened from spike2's narrower "just
// guard the Decoder object" scope. This makes "decode one frame and publish it" an atomic
// unit with respect to seek(), which is what closes a real race: without this widening, a
// frame the decode thread had already pulled from the OLD position (in flight between
// Decoder::next_frame() returning and its ring-write+tag landing) could still land in the
// ring AFTER seek()'s own fresh write to the same slot, silently overwriting the just-sought
// frame with stale content while its tag looked fine. See docs/native_core.md for the full
// writeup and the seek-stress measurements that exercised this path.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX // windows.h's min/max macros otherwise break std::min/std::max call sites below
#endif
#include <windows.h>
#include <dwmapi.h> // DwmSetWindowAttribute / DwmExtendFrameIntoClientArea (Win11 chrome)
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <timeapi.h>
#include <commdlg.h> // GetOpenFileNameW (Player::pick_open_file)
#include <shellapi.h> // DragAcceptFiles/DragQueryFileW/DragFinish (M-C2: WM_DROPFILES)
#include <shlobj.h> // SHBrowseForFolderW (Player::pick_folder, web-stream server root)
#include <windowsx.h> // GET_X_LPARAM/GET_Y_LPARAM (WndProc's WM_NCHITTEST)
#pragma comment(lib, "winmm.lib")

// Win11 DWM corner preference (SDK may predate these; values match dwmapi.h since 10.0.22000).
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DEFAULT
#define DWMWCP_DEFAULT      0
#define DWMWCP_DONOTROUND   1
#define DWMWCP_ROUND        2 // system default rounded corners (~8px at 96 DPI)
#define DWMWCP_ROUNDSMALL   3
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif

// Audio (spike, additive): WASAPI. initguid.h MUST precede mmdeviceapi.h/audioclient.h/
// ksmedia.h so their DEFINE_GUID macros actually instantiate storage for
// CLSID_MMDeviceEnumerator / IID_IAudioClient / IID_IAudioRenderClient /
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT etc. in THIS translation unit -- avoids the classic
// unresolved-external-symbol pitfall for those GUIDs without needing extra import libs.
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>  // WAVEFORMATEXTENSIBLE
#include <ksmedia.h> // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT / _PCM

#include <cuda.h>
#include <cudaD3D11.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// imgui_impl_win32.h wraps this declaration in `#if 0` (to avoid dragging a <windows.h>
// dependency into that header) and its own comment directs callers to copy the line into
// their .cpp -- windows.h is already included above, so this is safe here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "decoder.h"
#include "headless_decode.h" // transcode/export headless decode bridge (see headless_decode.cpp)
#include "ui/widgets.h" // ui::IconButtonResult / ui::IconButton (Phase 2A design system)
#include "present_hlsl.h" // generated by native/cmake/embed_shader.cmake from native/shaders/present.hlsl
#include "logo_rgba.h"    // generated by native/cmake/embed_binary.cmake from assets/generated/sumu-logo-256.rgba

// Audio (spike, additive): software resample only (decode itself uses plain avcodec_*, no
// hwaccel) -- see audio_loop() below.
extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace py = pybind11;
using Microsoft::WRL::ComPtr;

// ---- error helpers (defined in player_session.cpp) -----------------------------
void check_hr(HRESULT hr, const char* what);
void check_cu(CUresult res, const char* what);

// Present-tick "source" tags recorded in the trace, distinguishing a freshly-drawn frame
// from a repeat-of-last-frame fallback (the latter should be ~never once steady, except
// briefly right after a seek while decode catches up to the new position).
enum class Source : int8_t {
    PassthroughFresh = 0,
    AiFresh = 1,
    PassthroughStale = 2, // re-presented because neither map had this exact frame_num
    AiStale = 3,
    NoSession = 4, // M-C1: present ticked but no session is open yet (or was just closed) --
                   // draw_splash() ran instead of draw_and_present(), see present_loop().
};

inline bool is_ai(Source s) { return s == Source::AiFresh || s == Source::AiStale; }

// ---- UI draw-data snapshot (M2: main-thread NewFrame/Render -> present-thread RenderDrawData
// handoff) --------------------------------------------------------------------------------
//
// ImGui::GetDrawData()'s ImDrawList*s are owned by the ImGuiContext and get overwritten by the
// next NewFrame() -- unsafe to hand across threads by pointer. CloneOutput() (imgui_draw.cpp)
// deep-copies each ImDrawList's CmdBuffer/IdxBuffer/VtxBuffer into a freshly IM_NEW'd
// ImDrawList, so a snapshot built from CloneOutput() is safe for the present thread to read
// after the main thread has moved on to its next frame. IM_NEW/IM_DELETE route through
// ImGui::MemAlloc/MemFree (imgui.h's IM_NEW/IM_DELETE macros), so cloned lists must be freed
// with IM_DELETE, not plain `delete` -- matches ImGuiViewportP::~ImGuiViewportP's own use of
// IM_DELETE on BgFgDrawLists (imgui.h:1988), the only other place in this vendored tree that
// owns/frees a raw ImDrawList*.
struct UiDrawSnapshot {
    ImVector<ImDrawList*> cmd_lists; // each entry is a CloneOutput() result -- owned here
    int total_vtx = 0;
    int total_idx = 0;
    ImVec2 display_pos{};
    ImVec2 display_size{};
    ImVec2 framebuffer_scale{};

    ~UiDrawSnapshot()
    {
        for (ImDrawList* dl : cmd_lists) IM_DELETE(dl);
    }
};

// ---- UI intent channel (M3) -----------------------------------------------------------
// Main-thread-exclusive state recording what the UI wants to happen next, drained once per
// Python main-loop tick by Player::take_ui_intents(). Deliberately NOT mutex-guarded: WndProc
// (reached via pump_messages()) and ui_tick()/build_*() all run on the SAME Python-owned main
// thread, and so does take_ui_intents() -- the present thread never reads this struct. Adding
// a lock here would be the wrong signal (that this got used across threads), see DESIGN.md I2
// (present never touches transport/config).
struct UiIntents {
    std::optional<int64_t> seek;
    bool toggle_play = false;
    std::optional<int> clip_length;
    std::optional<int> max_regions;
    std::optional<float> cold_start_s; // cold-start skip seconds (0–3); Python clamps
    std::optional<int> lead; // AI buffer window (frames, 1–180); Python clamps
    std::optional<int> target_fps; // 0=original, 30, 60; Python maps to per-file fps_div
    // M-C2: a file the user wants opened (reopen), recorded by either the WM_DROPFILES handler
    // (open_path, UTF-8 path of the dropped file -- empty means none pending) or the top-bar
    // "open" button (open_dialog -- Python responds by calling the blocking pick_open_file()
    // dialog itself, see take_ui_intents()'s header comment for why this stays main-thread-only
    // same as every other field here).
    std::string open_path;
    bool open_dialog = false;
    // Title-bar X / ESC while park_on_close_ is set: the user asked to close the window, but
    // native no longer decides to quit -- Python drains this and chooses between parking
    // (hide window, keep the warm models for a fast reopen) and a real exit. See app.py's
    // park handling; with park_on_close_ unset WM_CLOSE/ESC keep their legacy quit behavior.
    bool close_request = false;
    // First-screen "compile TRT acceleration engines" button (open-prompt overlay only). Python
    // responds by spawning the blocking compile off the main thread and driving the compile UI
    // via set_compile_ui(). Doubles as the "retry" click in the failed state.
    bool compile_engine = false;
    // Web-streaming server (Phase 2). The stream popup writes these; Python drains them.
    bool stream_start = false;
    int stream_port = 0;
    std::string stream_root;
    bool stream_no_token = false;
    std::string stream_token;
    bool stream_stop = false;
    // Offline-export full-screen mode (Phase 2 extension): enter/exit + queue + preset intents.
    bool export_enter = false;
    bool export_exit = false;
    bool export_add_files = false;    // "添加文件" button -> Python opens the file picker
    bool export_start = false;        // "开始导出" -> run the queue
    bool export_pick_global = false;  // pick the global output dir
    int export_pick_custom = -1;      // item id to pick a custom output path for
    int export_remove = -1;           // item id to remove
    int export_cancel = -1;           // item id to cancel
    int export_move_id = -1;          // item id being drag-reordered
    int export_move_to = -2;          // drop target: insert before this item id (-1 = queue end)
    int export_item_preset_id = -1;   // item id whose preset dropdown changed
    int export_item_preset_idx = -1;  // new preset index
    int export_item_out_id = -1;      // item id whose output-mode dropdown changed
    int export_item_out_mode = 0;     // 0 auto / 1 global / 2 custom
    int export_clip_length = -1;      // AI pipeline clip length (commit on release)
    // preset editor (commit-on-save)
    bool export_preset_save = false;
    bool export_preset_delete = false;
    int export_preset_edit_idx = -1;  // -1 none / -2 new / >=0 edit existing
    std::string export_preset_name;
    int export_preset_codec = 0;      // 0 hevc / 1 h264
    bool export_preset_cq_enabled = true;
    int export_preset_cq = 33;
    bool export_preset_vbr_enabled = false;
    int export_preset_bitrate = 0;    // kbps
    int export_preset_maxrate = 0;    // kbps
    int export_preset_quality = 6;    // 0..6 -> p1..p7
    bool export_preset_audio_copy = true;
    int export_preset_audio_bitrate = 256;  // kbps
    bool export_preset_subtitle = true;
    std::string export_preset_suffix;
    int export_set_default = -1;      // preset idx to mark as the default
    // Files dropped on the window while in export mode (multi-file drop -> queue).
    std::vector<std::string> export_drop_paths;
};

// Offline-export screen state pushed by Python via set_export_snapshot() (main-thread only, same
// discipline as set_ui_strings). Plain value copies; the Python-side queue is the source of truth
// and is re-pushed every tick.
struct ExportPresetView {
    std::string name;
    std::string codec;     // "hevc" | "h264"
    std::string preset;    // "p1".."p7"
    bool cq_enabled = true;
    int cq = 0;
    bool vbr_enabled = false;
    int bitrate = 2000;    // kbps
    int maxrate = 2500;    // kbps
    bool audio_copy = true;
    int audio_bitrate = 256;  // kbps
    bool subtitle = true;
    std::string suffix;
};

struct ExportItemView {
    int id = 0;
    std::string source;
    std::string out_path;
    std::string out_mode;   // "auto" | "global" | "custom"
    int preset_idx = 0;
    std::string status;     // pending|running|done|failed|cancelled|interrupted
    float progress = -1.0f; // 0..1, -1 = unknown
    std::string error;
};

class Player
{
public:
    // Ring capacities (frames). Passthrough (NV12) and AI (RGBA) may differ (I8 VRAM):
    // 4K dual×180 RGBA was ~6GB and OOM'd; deep PT alone is ~2.2GB. AI display window can
    // stay short -- Python holds sparse restored crops and JIT-pushes into the AI ring near
    // the present head. Indexing: frame_num % capacity per ring (wrap_pt_slot / wrap_ai_slot).
    // Network profile deliberately stays shallow: remote Range seeks + dual-decoder scrub are
    // the cost drivers, not VRAM (see open_session network branch).
    static constexpr UINT kRingCapacityMax = 180;
    static constexpr UINT kAiRingCapacityMax = 64;  // 4K AI display window (~1s@60)
    static constexpr UINT kRingCapacityDefault = 64; // pre-open / closed
    static constexpr UINT kNetworkRingCapacity = 48; // ~0.8–1.6s@30/60; enough for present + thin AI
    static constexpr UINT kNetworkAiRingCapacity = 32;
    static constexpr int64_t kStartBufferFrames = 5;
    static constexpr int64_t kNetworkStartBufferFrames = 2;

    static void pick_ring_capacities(int width, int height, UINT& pt_cap, UINT& ai_cap,
                                    bool network = false)
    {
        if (network) {
            // Shallow decode-ahead for HTTP(S): less pre-read bandwidth, faster seek reclaim.
            // AI lead will clamp to decode_ahead_max on the Python side.
            pt_cap = kNetworkRingCapacity;
            ai_cap = kNetworkAiRingCapacity;
            return;
        }
        const int64_t px = static_cast<int64_t>(width) * static_cast<int64_t>(height);
        if (px <= 1920LL * 1080LL) {
            // 1080p: dual full-depth still ~2GB -- keep symmetric 180 for max AI stockpile.
            pt_cap = kRingCapacityMax;
            ai_cap = kRingCapacityMax;
        } else if (px <= 2560LL * 1440LL) {
            pt_cap = kRingCapacityMax;
            ai_cap = 90;
        } else {
            // 4K: deep NV12 PT for AI input lead; short RGBA AI for present near-head only.
            pt_cap = kRingCapacityMax;
            ai_cap = kAiRingCapacityMax;
        }
    }

    UINT ring_capacity() const { return ring_capacity_; }       // PT (decode-ahead) depth
    UINT ai_ring_capacity() const { return ai_ring_capacity_; } // AI display ready-map depth
    int64_t decode_ahead_max() const { return decode_ahead_max_; }

    // Base (96-DPI) height of the self-drawn top title bar. Runtime height is
    // top_bar_h() = kTopBarHBase * ui_dpi_scale_ so the strip tracks the monitor scale
    // (fonts/icons/chrome all grow with it; see apply_ui_dpi()). Shared by build_top_bar(),
    // fit_viewport() (reserves this strip so the video is letterboxed BELOW the bar) and
    // resize_window_for_video() (adds it to the target client height).
    static constexpr float kTopBarHBase = 36.0f;

    // Font size ladder (unscaled base @ 96 DPI). FontScaleDpi multiplies at runtime -- pass
    // these to AddFontFromFileTTF / PushFont(NULL, size), never GetFontSize() (would double-scale).
    //   kFontSizeBase -- body / chrome labels / buttons (default)
    //   kFontSizeSm   -- secondary copy (hints, captions)
    static constexpr float kFontSizeBase = 18.0f;
    static constexpr float kFontSizeSm = 16.0f;

    // UI length at the current monitor DPI (96 DPI → 1.0). Multiplies every fixed-pixel chrome
    // constant (bars, buttons, icons, padding) so a 150%/200% Windows scale doesn't leave the
    // overlay looking tiny. Fonts ride style.FontScaleDpi (set by apply_ui_dpi); this helper is
    // for the self-drawn geometry that FontScaleDpi does not touch.
    float ui_s(float v) const { return v * ui_dpi_scale_; }
    float top_bar_h() const { return ui_s(kTopBarHBase); }

    Player(int width_hint, int height_hint, bool maximized);

    ~Player();

    // ---- lifecycle -------------------------------------------------------------------------

    void open(const std::string& video_path);

    void open_session(const std::string& video_path);

    void close_session();

    int64_t reopen(const std::string& video_path);

    void close_current_session();

    void close();

    // Blocking Win32 "Open" file dialog -- only safe to call before open() (no decode/present
    // threads running yet, main thread free to block on a modal dialog). Returns the selected
    // path as UTF-8, or an empty string if the user cancelled.
    //
    // Uses the WIDE API (GetOpenFileNameW) + an explicit UTF-8 conversion, NOT the ANSI
    // GetOpenFileNameA: the ANSI variant returns the path in the process's ANSI codepage
    // (e.g. GBK on zh-CN Windows), not UTF-8 -- that would silently mis-encode a CJK filename/
    // path relative to what basename_of()'s (byte-substr, UTF-8-assuming) display and
    // decoder_.open()'s avformat_open_input (Windows file protocol expects UTF-8) both need,
    // either garbling the title-bar name or failing to open the file. This one call site is
    // fixed at the source (return value is guaranteed UTF-8); the rest of the ANSI Win32 calls
    // elsewhere in this file (CreateWindowExA etc.) are untouched, out of scope here.
    static std::wstring utf8_to_wide(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (n <= 0) return std::wstring();
        std::wstring w(static_cast<size_t>(n - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    std::string pick_folder();

    std::string pick_open_file();

    std::string pick_save_file(const std::string& default_name);

    void pump_messages();

    bool should_quit() const { return quit_.load(std::memory_order_relaxed); }
    void set_quit() { quit_.store(true, std::memory_order_relaxed); }

    // Close-parking (see UiIntents::close_request). With park_on_close_ set, WM_CLOSE/ESC
    // record an intent instead of quitting; Python then hide_window()s and keeps the process
    // (and its warm AI models) alive briefly. Main-thread-only flag, same as UiIntents.
    void set_park_on_close(bool on) { park_on_close_ = on; }
    bool park_on_close() const { return park_on_close_; }

    // Hide/show the window across a park. show_window() additionally tries to take the
    // foreground (failure just flashes the taskbar button -- acceptable). Present keeps
    // running while hidden -- deliberately, see park_on_close_'s comment.
    void hide_window();
    void show_window();

    void on_resize(UINT w, UINT h);

    void resize_window_for_video();

    bool point_in_caption_drag(int x, int y) const;

    bool is_fullscreen() const { return fullscreen_.load(std::memory_order_relaxed); }

    // ---- transport control -------------------------------------------------------------

    void play();

    void pause();

    bool is_playing() const { return playing_.load(std::memory_order_relaxed); }

    // Audio (spike, additive): true iff the file has an audio stream AND its decoder opened
    // successfully (i.e. audio_thread_ was actually started in open()). main-thread-only, no
    // synchronization needed -- set once in open(), never changed afterward.
    bool has_audio() const { return has_audio_; }

    void set_volume(float v);

    float get_volume() const { return volume_.load(std::memory_order_relaxed); }
    void set_muted(bool m) { muted_.store(m, std::memory_order_relaxed); }
    void toggle_mute();

    bool is_muted() const { return muted_.load(std::memory_order_relaxed); }

    void set_ai_enabled(bool v);

    bool is_ai_enabled() const { return ai_enabled_.load(std::memory_order_relaxed); }

    void set_fps_div(int v);

    int fps_div() const { return fps_div_.load(std::memory_order_relaxed); }
    double source_fps() const { return source_fps_; }

    void ui_tick();

    void record_toggle_play();

    void record_seek(int64_t f);

    void record_open_path(const std::string& path);

    void record_export_drop(const std::string& path);

    void record_open_dialog();

    void record_compile_engine();

    void record_close_request();

    void request_stream_popup();

    void set_stream_running(bool running, const std::string& url = "");

    void set_stream_defaults(int port, const std::string& root, bool no_token,
                             const std::string& token);

    void set_export_mode(bool on);

    bool export_mode() const { return export_mode_; }

    void set_export_snapshot(const py::dict& d);

    void request_open_url_popup();

    void notify_open_url_finished(bool ok);

    const std::string& path() const { return video_path_; }
    // True when the current session opened an http(s) URL (shallow ring, no scrub).
    bool is_network() const { return network_source_; }
    bool ui_ready() const { return ui_ready_; }

    void apply_ui_dpi(float scale);

    void set_ui_config(int clip_length, int max_regions, float cold_start_s, int target_fps = 0,
                       float ai_restore_fps = -1.0f, int lead = 180);

    void set_status_text(const std::string& s);

    void set_compile_ui(int state, float progress, const std::string& text);

    void set_ui_strings(const py::dict& d);

    py::dict take_ui_intents();

    int64_t seek(int64_t frame_num);

    // ---- AI insertion point --------------------------------------------------------------

    void push_ai_frame(int64_t frame_num, uint64_t dev_ptr, int fwidth, int fheight, size_t pitch_bytes);

    py::dict get_cuda_nv12_by_frame(int64_t frame_num);

    // ---- introspection -------------------------------------------------------------------

    double fps() const { return fps_; }
    int64_t frame_count() const { return frame_count_; }
    int64_t current_frame() const { return clock_frame_.load(std::memory_order_relaxed); }
    std::pair<int, int> dims() const { return { src_width_, src_height_ }; }

    double ai_hit_rate() const;

    py::dict stats();

    py::dict present_stats();

    void dump_present_trace(const std::string& path);

private:
    void require_open() const
    {
        if (!opened_) throw std::runtime_error("Player.open() has not been called yet");
    }

    // ---- setup -----------------------------------------------------------------------------

    void create_window(int width_hint, int height_hint, bool maximized);

    void create_device_and_swapchain();

    void create_shader_pipeline();

    void ui_init();

    void create_logo_texture();

    void ui_shutdown();

    void ui_render_drawdata();

    void build_ui();

    void build_splash_overlay();

    void build_open_prompt_overlay(float top_bar_h);

    void build_open_url_popup();

    void build_stream_popup();

    // ---- offline-export full-screen mode (Phase 2 extension) ------------------------------
    // The export screen owns the whole window while export_mode_ is set. Python re-pushes the
    // queue/preset snapshot every tick via set_export_snapshot(); this only renders + records
    // intents (never mutates Python state directly).

    const char* export_status_label(const std::string& s) const;

    static std::string export_preset_summary(const ExportPresetView& p);

    static int export_quality_idx_of(const std::string& preset);

    void build_export_screen(float top_bar_h);

    void build_export_preset_editor();

    // Sub-builders of build_export_screen (player_ui_export.cpp).
    void build_export_preset_card(int i, float width);
    void build_export_item_card(ExportItemView& item, float width,
                                const std::vector<const char*>& preset_names,
                                const char* const out_modes[]);

    void open_export_preset_editor(int idx);

    void build_status_float();

    // M-A: self-drawn icon buttons (top/bottom bar), replacing the M3 ASCII-text ImGui::Button
    // labels -- same InvisibleButton + ImDrawList pattern already used by the seekbar above.
    // A plain ImGui::Button() auto-sizes an unspecified axis from its label/frame padding;
    // InvisibleButton has no label to fall back on and asserts on a zero size axis, so callers
    // here always pass an explicit nonzero size. Returns the item's hit-test result plus its
    // screen-space rect and draw list so the caller can immediately paint an icon glyph
    // centered on it, and draws a faint hover highlight (same visual language as ImGui's own
    // button hover state, just manually painted since these aren't real ImGui buttons).
    // The implementation lives in the design-system widget layer (ui/widgets.*); this member
    // is a same-signature delegate kept so the M-A call sites stay untouched.
    using IconButtonResult = ui::IconButtonResult;

    IconButtonResult icon_button(const char* str_id, ImVec2 size, bool disabled = false);
    // Atlas-glyph variant: paints the lucide icon (ui::AppIcon) centered on the hit area.
    IconButtonResult icon_button(const char* str_id, ImVec2 size, ui::AppIcon icon, bool disabled = false);

    void build_top_bar(float& out_height);

    void build_bottom_bar();

    void build_settings_panel(float top_bar_h);

    void apply_dwm_chrome(bool fullscreen);

    void toggle_fullscreen();

    int64_t scrub_bucket_for_frame(int64_t frame_num) const;

    ID3D11ShaderResourceView* get_thumbnail(int64_t frame_num);

    void render_thumbnail(ID3D11RenderTargetView* rtv);

    bool blit_thumbnail(const DecodedFrame& df, ID3D11RenderTargetView* dst_rtv);

    void serve_hover_bucket(int64_t bucket);

    void fill_next_grid_point();

    void scrub_loop();

    void create_ring_resources();

    void create_scrub_resources();

    void create_landing_texture();

    void init_cuda_driver();

    void register_landing_with_cuda();

    void create_ai_input_bridge();

    // ---- decode thread -----------------------------------------------------------------------

    void decode_loop();

    // ---- present thread ------------------------------------------------------------------------

    void present_loop();

    D3D11_VIEWPORT fit_viewport() const;

    void draw_and_present(Source source, UINT slot);

    void draw_splash();

    double compute_master_s() const;

    void audio_loop();

    // ---- helpers -----------------------------------------------------------------------------

    void recompute_session_rate();

    void invalidate_scrub_timeline();

    UINT wrap_pt_slot(int64_t frame_num) const;

    UINT wrap_ai_slot(int64_t frame_num) const;

    static double qpc_freq_d();

    static int64_t qpc_to_ns(LARGE_INTEGER c);

    bool wait_until_qpc_or_stop(LARGE_INTEGER target, double ticks_per_ms);

    // ---- state -------------------------------------------------------------------------------

    int src_width_ = 0;
    int src_height_ = 0;
    // Session ring depths (set in open_session from pick_ring_capacities).
    // PT: decode-ahead + AI input. AI: short present ready-map (may be << PT at 4K).
    UINT ring_capacity_ = kRingCapacityDefault;
    UINT ai_ring_capacity_ = kRingCapacityDefault;
    int64_t decode_ahead_max_ = static_cast<int64_t>(kRingCapacityDefault) - 10;
    UINT win_width_ = 0;
    UINT win_height_ = 0;
    HWND hwnd_ = nullptr;
    bool timer_period_set_ = false;
    bool closed_ = false;
    bool opened_ = false;
    bool ui_ready_ = false;
    bool in_ui_tick_ = false; // re-entrancy guard for ui_tick() (see its header comment)
    // Monitor DPI scale (dpi/96). Written by apply_ui_dpi() on the main thread; read by
    // ui_s()/top_bar_h() on the main thread and by fit_viewport() on the present thread. Plain
    // float is fine: only layout geometry depends on it, and a torn half-update just yields one
    // frame of slightly-off letterboxing until the next present tick.
    float ui_dpi_scale_ = 1.0f;
    // Pre-ScaleAllSizes snapshot of ImGuiStyle (captured once in ui_init after
    // ui::theme::apply_theme). apply_ui_dpi() copies this then ScaleAllSizes(scale) so
    // repeated DPI changes don't compound.
    ImGuiStyle ui_style_base_{};
    // Glyph ranges for the CJK system font (zh common + Japanese). Built in ui_init and
    // must outlive AddFontFromFileTTF until the atlas is built -- ImFontConfig keeps a
    // pointer into this vector.
    ImVector<ImWchar> font_glyph_ranges_;

    // ---- UI draw-data handoff (M2) -- ui_pending_ is main-thread-written/present-moved-out,
    // ui_active_ is present-thread-exclusive (never touched under ui_mutex_ once moved into).
    // ui_mutex_ is only ever held for the O(1) pointer move in ui_tick()/ui_render_drawdata(),
    // never across RenderDrawData -- see UiDrawSnapshot's header comment and both methods.
    std::mutex ui_mutex_;
    std::unique_ptr<UiDrawSnapshot> ui_pending_;
    std::unique_ptr<UiDrawSnapshot> ui_active_;

    // ---- UI product state (M3) -- main-thread-exclusive, see UiIntents' header comment. ------
    std::string video_path_;
    // Session IO profile: set in open_session from decoder_.is_network(). Cleared on failed reopen.
    bool network_source_ = false;
    // open_session sets this; ui_tick on the main thread applies resize_window_for_video().
    std::atomic<bool> pending_resize_for_video_{ false };
    UiIntents ui_intents_;
    int ui_cfg_clip_length_ = 30; // Python-refreshed mirror of the ACTUAL committed config
    int ui_cfg_max_regions_ = 1;  // (settings panel seeds edit buffers from these on open)
    float ui_cfg_cold_start_s_ = 1.0f; // cold-start skip seconds (0–3)
    int ui_cfg_lead_ = 180; // AI buffer window (frames)
    int ui_cfg_target_fps_ = 0; // 0=original, 30, 60 (Python display mirror)
    float ui_cfg_ai_restore_fps_ = -1.0f; // net BasicVSR fps from Scheduler; <0 = unknown
    bool ui_settings_open_ = false;
    // Seekbar scrub: last frame we already recorded a seek for during the current press.
    // -1 when the bar is not active. Suppresses hold-still re-seeks (see build_bottom_bar).
    int64_t seekbar_last_seek_frame_ = -1;
    // Model-warmup status line, see set_status_text()'s header comment -- empty hides
    // build_status_float() entirely, non-empty text shown regardless of session_active_.
    std::string status_text_;

    // First-screen TRT-compile prompt, driven by set_compile_ui() (see its header comment).
    // Only consulted by build_open_prompt_overlay(); 0 == hidden, the default.
    int compile_ui_state_ = 0;
    float compile_ui_progress_ = 0.0f;
    std::string compile_ui_text_;
    // ImGui / open-dialog labels. Empty until Python set_ui_strings() fills them
    // (daily path: apply_to_player right after Player ctor, before first ui_tick).
    // Copy lives only in python/sumu/locales/*.json -- no C++ text duplicates.
    struct UiStrings {
        std::string splash_loading;
        std::string open_prompt;
        std::string open_file;
        std::string open_url;
        std::string open_url_title;
        std::string open_url_hint;
        std::string open_url_ok;
        std::string open_url_cancel;
        std::string open_url_invalid;
        std::string open_url_loading;
        std::string open_url_load_failed;
        std::string compile_retry;
        std::string compile_engine;
        std::string lead_label;
        std::string lead_tooltip;
        std::string clip_length_label;
        std::string clip_length_tooltip;
        std::string max_regions_label;
        std::string max_regions_tooltip;
        std::string cold_start_label;
        std::string cold_start_tooltip;
        std::string target_fps_label;
        std::string target_fps_original;
        std::string target_fps_tooltip;
        std::string diagnostics_title;
        std::string ai_speed;
        std::string ai_speed_unknown;
        std::string dialog_video_files;
        std::string dialog_all_files;
        // Web-stream server + offline export (Phase 2).
        std::string stream_server;
        std::string export_video;
        std::string stream_title;
        std::string stream_port_label;
        std::string stream_root_label;
        std::string stream_pick;
        std::string stream_start;
        std::string stream_stop;
        std::string stream_url_label;
        std::string stream_no_token;
        std::string stream_token_label;
        std::string stream_token_hint;
        std::string export_title;
        std::string export_start;
        std::string cancel;
        std::string export_section_settings;
        std::string export_clip_length_label;
        std::string export_global_dir_label;
        std::string export_pick_dir;
        std::string export_section_queue;
        std::string export_add_files;
        std::string export_out_auto;
        std::string export_out_global;
        std::string export_out_custom;
        std::string export_remove;
        std::string export_up;
        std::string export_down;
        std::string export_empty;
        std::string export_not_ready;
        std::string export_presets_title;
        std::string export_preset_new;
        std::string export_preset_delete;
        std::string export_preset_name_label;
        std::string export_preset_codec_label;
        std::string export_preset_cq_label;
        std::string export_preset_bitrate_label;
        std::string export_preset_maxrate_label;
        std::string export_preset_vbr_label;
        std::string export_preset_quality_label;
        std::string export_preset_audio_label;
        std::string export_preset_audio_copy;
        std::string export_preset_audio_encode;
        std::string export_preset_subtitle_label;
        std::string export_preset_subtitle_none;
        std::string export_preset_subtitle_copy;
        std::string export_preset_suffix_label;
        std::string export_preset_default;
        std::string export_preset_save;
        std::string export_status_pending;
        std::string export_status_running;
        std::string export_status_done;
        std::string export_status_failed;
        std::string export_status_cancelled;
        std::string export_status_interrupted;
    } ui_str_;
    // Open-URL modal (ImGui popup). Main-thread only, same discipline as ui_settings_open_.
    // Confirm writes a trimmed http(s) URL into ui_intents_.open_path (Python reuses the drop path)
    // and flips open_url_loading_ so the float stays open until notify_open_url_finished().
    bool open_url_popup_ = false;
    bool open_url_focus_ = false;
    char open_url_buf_[2048] = {};
    bool open_url_show_error_ = false;      // client-side scheme validation
    bool open_url_show_load_error_ = false; // server/open failure after async open
    bool open_url_loading_ = false;
    bool open_url_close_pending_ = false;   // success → close on next build_open_url_popup
    // Web-stream server / offline export popups (Phase 2). Main-thread only, same discipline.
    bool stream_popup_ = false;
    int stream_port_edit_ = 8080;
    char stream_root_buf_[2048] = {};
    bool stream_no_token_edit_ = false;      // "无 token" checkbox (hide the token input while set)
    char stream_token_buf_[256] = {};        // explicit token; empty = server generates a random one
    bool stream_running_ = false;           // Python flips this (start/stop) for the 停止/启动 toggle
    std::string stream_url_;                // access URL while running (shown as a clickable link)
    // Offline-export full-screen mode (Phase 2 extension). export_mode_ is driven by Python
    // (set_export_mode) after it tears down playback/streaming; the screen is the only UI while
    // set. Views are re-pushed each tick by set_export_snapshot().
    bool export_mode_ = false;
    bool export_engine_ready_ = false;
    bool export_running_ = false;
    bool export_presets_open_ = false;   // one-shot trigger: arms OpenPopup for the preset modal
    int export_clip_length_ = 120;       // committed value (pushed by Python each tick)
    int export_clip_length_edit_ = 120;  // local slider value (seeded once per entry)
    bool export_clip_edit_init_ = false;
    std::string export_global_dir_;
    int export_default_preset_idx_ = 0; // pushed by Python; local radio selection between ticks
    std::vector<ExportPresetView> export_presets_;
    std::vector<ExportItemView> export_items_;
    int export_drag_id_ = -1;           // queue item being drag-reordered (gap-based D&D)
    int export_drag_slot_ = -1;         // open gap position (index among non-dragged items)
    // Preset editor modal state (staged locally, seeded once per open like settings_edit_*).
    int export_preset_edit_idx_ = -1;   // -1 closed / -2 new / >=0 edit
    bool export_preset_edit_init_ = false;
    char export_preset_name_buf_[256] = {};
    int export_preset_codec_idx_ = 0;   // 0 hevc / 1 h264
    bool export_preset_cq_enabled_ = true;
    int export_preset_cq_ = 33;
    bool export_preset_vbr_enabled_ = false;
    int export_preset_bitrate_ = 2000;  // kbps
    int export_preset_maxrate_ = 2500;  // kbps
    int export_preset_quality_idx_ = 6; // 0..6 -> p1..p7
    bool export_preset_audio_copy_ = true;
    int export_preset_audio_bitrate_ = 256;  // kbps
    bool export_preset_subtitle_ = true;
    char export_preset_suffix_buf_[128] = {};
    bool settings_edit_init_ = false;    // one-shot: (re)seed edit buffers from ui_cfg_* the
    int settings_edit_clip_length_ = 30; // moment the settings panel is opened, so an
    int settings_edit_max_regions_ = 1;  // in-progress edit survives Python's per-tick refresh.
    float settings_edit_cold_start_s_ = 1.0f;
    int settings_edit_lead_ = 180;
    int settings_edit_target_fps_idx_ = 0; // 0=original, 1=30, 2=60
    // Atomic: written on the main thread (toggle_fullscreen()) but read on the PRESENT thread by
    // fit_viewport() (whether to reserve the title-bar strip) as well as the main thread
    // (build_top_bar()'s auto-hide, is_fullscreen(), resize_window_for_video()). relaxed is
    // enough -- it only gates a per-frame layout choice, no other state is published through it.
    std::atomic<bool> fullscreen_{ false };
    WINDOWPLACEMENT windowed_placement_{}; // saved pre-fullscreen placement (rect + showCmd)
    LONG windowed_style_ = 0; // saved pre-fullscreen GWL_STYLE, for restore

    // ---- borderless custom-caption drag rect (client-space), published by build_top_bar(),
    // consumed by WndProc's WM_NCHITTEST via point_in_caption_drag() above. Zero-initialized so
    // WM_NCHITTEST is safe before the first UI frame renders (see point_in_caption_drag()).
    float caption_drag_x0_ = 0, caption_drag_x1_ = 0, caption_drag_y1_ = 0;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIAdapter> adapter_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11RenderTargetView> backbuffer_rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_nv12_;
    ComPtr<ID3D11PixelShader> ps_ai_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> slice_cb_;

    // First-screen logo (device-layer, built in ui_init via create_logo_texture).
    ComPtr<ID3D11Texture2D> logo_tex_;
    ComPtr<ID3D11ShaderResourceView> logo_srv_;

    ComPtr<ID3D11Texture2D> pt_ring_tex_;

    std::vector<ComPtr<ID3D11ShaderResourceView>> pt_srv_y_;
    std::vector<ComPtr<ID3D11ShaderResourceView>> pt_srv_uv_;
    std::vector<int64_t> pt_tag_; // guarded by ready_mutex_
    std::atomic<int64_t> pt_high_water_{ -1 };

    ComPtr<ID3D11Texture2D> ai_ring_tex_;
    std::vector<ComPtr<ID3D11ShaderResourceView>> ai_srv_;
    std::vector<int64_t> ai_tag_; // guarded by ready_mutex_

    ComPtr<ID3D11Texture2D> landing_tex_;
    CUdevice cu_dev_ = 0;
    CUcontext cu_ctx_ = nullptr;
    CUgraphicsResource cu_res_ = nullptr;

    // ---- AI input bridge state (additive, Part B) -- see create_ai_input_bridge() and
    // get_cuda_nv12_by_frame(). All D3D11/CUDA-interop touches of these go through
    // d3d_mutex_, same as the rest of the shared device/context (file header).
    ComPtr<ID3D11VertexShader> ai_in_vs_;
    ComPtr<ID3D11PixelShader> ai_in_ps_y_;
    ComPtr<ID3D11PixelShader> ai_in_ps_uv_;
    ComPtr<ID3D11SamplerState> ai_in_sampler_;
    ComPtr<ID3D11Texture2D> ai_in_y_tex_;
    ComPtr<ID3D11RenderTargetView> ai_in_y_rtv_;
    ComPtr<ID3D11Texture2D> ai_in_uv_tex_;
    ComPtr<ID3D11RenderTargetView> ai_in_uv_rtv_;
    CUgraphicsResource ai_in_cu_res_y_ = nullptr;
    CUgraphicsResource ai_in_cu_res_uv_ = nullptr;
    CUdeviceptr ai_in_cu_buf_ = 0;   // persistent, single-slot, tightly-packed NV12 dest buffer
    size_t ai_in_cu_buf_pitch_ = 0;  // == src_width_ (tightly packed)
    size_t ai_in_cu_buf_size_ = 0;

    // ---- M4: seekbar hover scrub-thumbnail preview -----------------------------------------
    // An INDEPENDENT decode path (own Decoder = own NVDEC session) inside this Player -- NOT a
    // second Player instance (see get_thumbnail()'s header for why). get_thumbnail() runs on the
    // MAIN thread inside ui_tick()/build_bottom_bar() (which never touches d3d_mutex_, see
    // ui_tick():867) -- it only reads scrub_cache_ under scrub_cache_mutex_ and posts the hovered
    // bucket to scrub_thread_ via scrub_request_frame_ + scrub_cv_; it NEVER decodes or does GPU
    // work, so hovering can't perturb present pacing. scrub_thread_ does the slow seek WITHOUT
    // any Player lock (Decoder self-serializes its own d3d11va pool, exactly like decode_loop()),
    // then takes d3d_mutex_ ONLY for the brief NV12->RGB blit (same shader pipeline as
    // draw_and_present(): vs_ + ps_nv12_ + sampler_ + slice_cb_), then publishes the result under
    // scrub_cache_mutex_ (never held together with d3d_mutex_ -> no lock cycle).
    static constexpr int kThumbW = 160;          // == seekbar preview draw size (1:1, no down/upscale)
    static constexpr int kThumbH = 90;
    static constexpr size_t kScrubCacheCap = 32;  // on-demand ring; ~1.8MB VRAM. Wider window so
                                                   // nearest-on-miss stays close during a sweep and
                                                   // back-and-forth re-hover rarely re-decodes

    // One pre-allocated, reused RGBA8 thumbnail texture (+ its RTV to render into and SRV to hand
    // to ImGui::AddImage). Ring-recycled: only `bucket` changes as slots are reused; the ComPtrs
    // persist so an SRV baked into the live ImGui draw snapshot stays valid until present renders
    // it. bucket == -1 means the slot is empty.
    struct ThumbEntry {
        int64_t bucket = -1;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11RenderTargetView> rtv;
    };
    std::vector<ThumbEntry> scrub_cache_;   // capacity kScrubCacheCap; guarded by scrub_cache_mutex_
    size_t scrub_cache_next_ = 0;           // round-robin write cursor; guarded by scrub_cache_mutex_

    // Coarse background grid: scrub_grid_slots_ thumbnails uniformly spanning the WHOLE video,
    // filled by scrub_loop() DURING PLAYBACK AND WHEN PAUSED (each point is decoded
    // nearest-keyframe-only -- one frame, not a GOP -- so it stays at present baseline even at
    // 4K60; measured, see docs), and NEVER evicted. Armed at open() so prefetch starts immediately.
    // Bounds get_thumbnail()'s nearest-on-miss error to ~half the grid spacing (+ up to one GOP for
    // the keyframe snap), so a fast jump to a far position shows an approximately-correct frame at
    // once (not a wildly-off nearest) and still refines to exact via the ring tier.
    //
    // PROGRESSIVE (coarse-to-fine) fill order: slots are filled in ascending bit-reversal rank, so
    // the FIRST ~64 decodes are spread evenly across the whole timeline (level 64), the next batch
    // bisects them (128), then 256, 512, 1024 -- every extra decode roughly halves the worst-case
    // nearest gap ANYWHERE in the video, instead of densely covering one region first and leaving
    // the far end blank. Within one octave, ties break toward the last hover so the region being
    // scrubbed densifies first. Measured full-cache-during-playback (1080p30 ~263/s @2ms, 4K60
    // ~75/s @12ms): a full 1024-slot grid completes in ~4s (1080p) / ~14s (4K); the coarse 64-point
    // pass lands in <1s -- see docs/scrub_thumbnail.md. entry.bucket: -1 unfilled, >=0 the frame it
    // holds, -2 failed. Same scrub_cache_mutex_ guards both tiers; get_thumbnail() scans grid+ring.
    static constexpr double kGridTargetSpacingSec = 4.0; // aim ~one grid thumb per 4s of video
    static constexpr size_t kGridSlotsMin = 64;          // floor: coarse whole-video coverage (level 64)
    static constexpr size_t kGridSlotsMax = 1024;        // ceil: ~1024*57KB ~= 58MB VRAM at 160x90; the
                                                         // ring tier refines to exact on hover so a
                                                         // long film's coarser spacing is fine. Full-cache
                                                         // during playback ~= 1024/263 ~= 4s (1080p) /
                                                         // 1024/75 ~= 14s (4K); coarse pass still <1s.
    std::vector<ThumbEntry> scrub_grid_;    // capacity scrub_grid_slots_; guarded by scrub_cache_mutex_
    std::vector<uint32_t> scrub_grid_rank_; // per-slot coarse-to-fine fill order (bit-reversal); const after open
    std::vector<uint8_t> scrub_grid_octave_;// per-slot densification octave (highest set bit of rank); const after open
    size_t scrub_grid_slots_ = 0;           // runtime slot count (set in create_scrub_resources)
    int64_t scrub_grid_stride_ = 0;         // nominal frames between grid points (reporting)
    size_t scrub_grid_done_ = 0;            // # grid slots attempted (scrub thread only); done==slots -> idle
    // Inter-fill yield WHILE PLAYING (0 when paused -- foreground idle, fill flat out). A busy-loop
    // (0ms) while playing 4K jitters present (measured stddev 0.10->2.23, no dropped frames but
    // visible on the trace); ANY small yield restores baseline. 1080p tolerates full speed, so the
    // yield is resolution-aware: set at open() from the source pixel count (see create_scrub_resources).
    static constexpr int kGridPlayThrottleLoRes = 2;   // <=1080p: ~263/s, present stddev 0.09 (measured)
    static constexpr int kGridPlayThrottleHiRes = 12;  // >1080p (4K): ~75/s, present stddev 0.11 (measured)
    static constexpr int64_t kHiResPixels = 2100000;   // ~1920x1080; above this use the hi-res yield
    int grid_play_throttle_ms_ = kGridPlayThrottleLoRes; // set at open() from src_width_*src_height_
    std::atomic<bool> scrub_grid_wanted_{ false }; // flips true on first hover; gates background fill
    std::atomic<int64_t> scrub_last_hover_frame_{ 0 }; // newest hovered frame; grid octave-ties break nearest-first
    std::mutex scrub_cache_mutex_;

    // Single-slice NV12 blit source (scrub decoder's frame is CopySubresourceRegion'd here, then
    // sampled by ps_nv12_) -- session-scoped, sized src_width_ x src_height_, built/torn down
    // alongside pt_ring_tex_ in create_ring_resources()/close_session().
    ComPtr<ID3D11Texture2D> scrub_nv12_tex_;
    ComPtr<ID3D11ShaderResourceView> scrub_srv_y_;
    ComPtr<ID3D11ShaderResourceView> scrub_srv_uv_;

    Decoder scrub_decoder_;
    std::thread scrub_thread_;
    // Latest hovered bucket the scrub thread should serve; -1 = nothing pending. Coalescing: the
    // main thread just overwrites this with the newest bucket, so a fast drag drops intermediate
    // buckets and the thread always chases the most recent hover position.
    std::atomic<int64_t> scrub_request_frame_{ -1 };
    std::condition_variable scrub_cv_;
    std::mutex scrub_cv_mutex_;

    std::mutex ready_mutex_;
    std::mutex push_mutex_;
    std::mutex trace_mutex_;
    // Serializes EVERY thread's touch of the shared ID3D11Device/context (decode's copy,
    // push_ai_frame's CUDA-interop + copy, present's whole draw_and_present call, seek's
    // copy) -- see the file header for why this exists on top of SetMultithreadProtected(TRUE).
    std::mutex d3d_mutex_;
    // Serializes the Decoder object itself AND the ring-write+tag that immediately follows
    // each decode, between decode_loop() and seek() -- see the file header for the race this
    // closes (widened from spike2's narrower "just guard the Decoder object" scope).
    std::mutex decoder_mutex_;

    Decoder decoder_;
    double source_fps_ = 60.0;       // container fps (fixed for the open file)
    int64_t source_frame_count_ = 0; // container frame count
    double fps_ = 60.0;              // session fps = source_fps_ / fps_div (retimed)
    int64_t frame_count_ = 0;        // session frame count after retiming
    // Temporal downsample 1/N. Decode still full-rate; only every Nth source frame becomes a
    // dense session frame. Written by set_fps_div (main); read by decode/seek/open_session.
    std::atomic<int> fps_div_{ 1 };

    std::thread decode_thread_;
    std::thread present_thread_;
    std::atomic<bool> stop_{ false };
    // M-C1: per-instance quit flag (see set_quit()/should_quit(), and the header comment above
    // WndProc's forward declaration) -- replaces a file-level global that used to be silently
    // shared across every Player instance in the process.
    std::atomic<bool> quit_{ false };
    // Close-parking (see UiIntents::close_request / set_park_on_close()). Main-thread-only
    // flag, same discipline as UiIntents itself. NOTE: hide_window() deliberately does NOT
    // idle present_loop() -- an earlier version did (Sleep+continue while hidden) and the
    // CUDA/D3D11 interop state broke during the hidden idle (reopen-after-park then failed
    // with DXGI_ERROR_DEVICE_REMOVED / CUDA_ERROR_INVALID_HANDLE on cuGraphicsD3D11Register
    // Resource; reproduced only with torch resident). Rendering the splash into the hidden
    // window for the <=60s park is near-free, so present simply keeps running.
    bool park_on_close_ = false;
    // M-C1: session-only stop signal for decode_thread_/audio_thread_, distinct from stop_
    // (present_thread_'s own, whole-Player-lifetime stop flag -- present_loop() must keep
    // checking ONLY stop_, never this). Set true by close_session(), then reset back to false
    // once both threads have joined, so a future open_session() starts clean.
    std::atomic<bool> session_stop_{ false };
    // M-C1: true from the last step of open_session() until close_session() begins tearing
    // down -- present_loop() branches on this to draw a "no session" splash instead of picking/
    // presenting real frames (draw_splash() vs. draw_and_present()); see present_loop().
    std::atomic<bool> session_active_{ false };
    // M-C2: present-detach handshake flag, written ONLY by present_loop() (the reader is
    // reopen()'s spin-wait). true means present_loop() has committed to this iteration's
    // draw_splash() branch (session_active_ was observed false) -- i.e. it is NOT mid- or about
    // to run draw_and_present(), and won't again until session_active_ flips back true. false
    // means the opposite: present_loop() has committed to (or is about to run) draw_and_present()
    // for this iteration. See present_loop()'s two store sites and reopen()'s header comment.
    std::atomic<bool> present_in_splash_{ false };

    // Phase 6 M-D: present-side view control -- native-only atomic, written by
    // build_settings_panel() (main thread, ui_tick()) and read directly by present_loop()'s
    // source-pick block on the present thread. Deliberately NOT routed through UiIntents (see
    // that struct's own header comment on why transport/config stays main-thread-exclusive):
    // there is nothing here for present to block on -- the AI Scheduler keeps producing frames
    // into the ready-map regardless of this flag, present merely chooses what to display, so
    // the toggle is instant in both directions with a plain relaxed atomic (same discipline as
    // volume_/muted_ above for audio_loop()).
    // ai_enabled_: false forces present to NEVER show an AI frame (passthrough only), even as a
    // stale-fallback (see present_loop()'s gating of the AiFresh/AiStale picks).
    std::atomic<bool> ai_enabled_{ true };

    // ---- audio (spike, additive) -- slave to the master clock below (present_head_frame_/
    // playing_/anchor_frame_/anchor_qpc_ticks_/fps_/freq_), never the reverse; see audio_loop()
    // for the full design writeup. audio_codec_ctx_ is built once in open() (main thread, no
    // thread-affinity concern for a plain FFmpeg context) and used exclusively by
    // audio_thread_ from then on. has_audio_ is set once in open(), read-only afterward.
    std::thread audio_thread_;
    bool has_audio_ = false;
    AVCodecContext* audio_codec_ctx_ = nullptr; // software decode only, no hw_device_ctx

    // Volume/mute (additive): written by the main thread (bottom-bar slider/mute icon,
    // WndProc's Up/Down/M keys), read lock-free by audio_thread_ right before samples leave it
    // (see audio_loop()'s gain-application block). Deliberately plain atomics, not routed
    // through UiIntents -- see set_volume()/set_muted() above for why.
    std::atomic<float> volume_{ 1.0f };
    std::atomic<bool> muted_{ false };

    LARGE_INTEGER pace_origin_qpc_{};
    LARGE_INTEGER freq_{};
    // M-C2: bumped by open_session() whenever fps_ changes (release); present_loop() (the sole
    // owner of pace_origin_qpc_/its tick_idx) observes the bump (acquire) and re-anchors its
    // pacing baseline to the new fps_. See present_loop()'s pacing comment for the full why.
    std::atomic<uint64_t> pace_epoch_{ 0 };
    std::atomic<int64_t> present_head_frame_{ 0 };

    // Transport/seek state (see play()/pause()/seek()/present_loop()).
    std::atomic<bool> playing_{ false };
    std::atomic<int64_t> anchor_frame_{ 0 };   // content frame_num at anchor_qpc_ticks_
    std::atomic<int64_t> anchor_qpc_ticks_{ 0 }; // QPC ticks corresponding to anchor_frame_
    std::atomic<int64_t> clock_frame_{ 0 };    // last frame_num computed/shown by present_loop
    std::atomic<uint64_t> seek_version_{ 0 };
    std::atomic<int64_t> seek_slot_hint_{ 0 };

    std::vector<int64_t> present_qpc_ns_;   // guarded by trace_mutex_
    std::vector<int8_t> present_source_;    // guarded by trace_mutex_
    std::vector<int64_t> present_frame_num_; // guarded by trace_mutex_

    std::atomic<uint64_t> present_count_{ 0 };
    std::atomic<uint64_t> decode_frame_count_{ 0 };
    std::atomic<uint64_t> ai_push_count_{ 0 };
    std::atomic<uint64_t> n_ai_fresh_{ 0 }, n_ai_stale_{ 0 }, n_pt_fresh_{ 0 }, n_pt_stale_{ 0 };
    // M-C1: counts present ticks that drew the splash (Source::NoSession) -- kept separate from
    // the fresh/stale counters above (which are lifetime-cumulative "real frame" stats) so a
    // splash-phase tick never dilutes them.
    std::atomic<uint64_t> n_no_session_{ 0 };
};
