// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Popup layer -- one ImGui window rendered into its OWN top-level Win32 window.
//
// Why this exists (the airspace problem): the URL page embeds WebView2 as a real windowed
// child HWND. A child window always composites above its parent's client area, so everything
// ImGui draws in the main swapchain (player_window.cpp CreateSwapChainForHwnd) is covered by
// the page view. An address-bar dropdown, a settings panel or a button tooltip that reaches
// into the page area is therefore invisible. Same problem Chrome solves for its omnibox popup.
//
// The approach is the single-window case of ImGui multi-viewport, hand-rolled:
//
//   * A PopupLayer owns one WS_POPUP window whose OWNER is the Player window. Owned windows
//     always stack above the owner and above the owner's child windows (the page view), and
//     WS_EX_NOACTIVATE + WM_MOUSEACTIVATE/MA_NOACTIVATE keep keyboard focus, the InputText
//     caret and IME exactly where they were.
//   * The ImGui context is SHARED, so theme, fonts, icons, DPI and the font texture are free.
//     The routed ImGui window is built normally in main-client coordinates; Player::ui_tick()
//     hands this layer the finished ImDrawData, which pulls that window's draw lists (its own
//     plus any child window rooted in it) out of the frame and clones them into a snapshot.
//     The snapshot's display_pos/display_size ARE the window's rect, so the same absolute
//     vertex coordinates project onto the popup's backbuffer 1:1.
//   * Rendering happens on the PRESENT thread inside d3d_mutex_, right after the main
//     ui_render_drawdata() and before the main Present(1,0): bind the popup's own swapchain,
//     clear, ImGui_ImplDX11_RenderDrawData, then Present(0,0). Never Present(1) -- a second
//     vblank wait in the video present loop would be a pacing regression (DESIGN I10).
//   * Layers stack among themselves by appearance order: a layer that becomes visible is brought
//     to the top of the owner's owned-window stack, so a dropdown/tooltip opened over a routed
//     panel really draws above it.
//   * Input arrives at the popup window itself and is fed straight into the shared ImGui IO in
//     main-client coordinates (io.AddMousePosEvent/AddMouseButtonEvent/AddMouseWheelEvent).
//     ImGui_ImplWin32_WndProcHandler is deliberately NOT used here: it would TrackMouseEvent
//     the Player window and post spurious WM_MOUSELEAVE for it. A button-down inside the layer
//     hands the Win32 KEYBOARD focus back to the Player window -- the same hand-back the main
//     WndProc does for clicks on the parent chrome, and what lets a routed InputText receive
//     typed characters again after the user clicked into the page view. The popup itself never
//     activates.
//
// Thread ownership: HWND/window placement, routing and snapshots are main-thread state
// (Player::ui_tick's thread, the one that owns the ImGui context); the swapchain and the
// snapshot currently on screen belong to the present thread. The only cross-thread state is a
// handful of atomics plus the snapshot pointer swap under a mutex -- the same handoff
// UiDrawSnapshot already uses for the main swapchain.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX // windows.h's min/max macros otherwise break std::min/std::max in callers
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "imgui.h"

// ---- UI draw-data snapshot (main thread NewFrame/Render -> present thread RenderDrawData) ---
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
//
// A snapshot is used two ways: the whole frame (display_pos/display_size = the main window's
// client area, the main swapchain's draw data) and one routed popup window (display_pos/
// display_size = that window's rect, drawn into the popup's swapchain).
struct UiDrawSnapshot {
    ImVector<ImDrawList*> cmd_lists; // each entry is a CloneOutput() result -- owned here
    int total_vtx = 0;
    int total_idx = 0;
    ImVec2 display_pos{};
    ImVec2 display_size{};
    ImVec2 framebuffer_scale{};
    uint64_t content_hash = 0; // routed popup frames only: vertices/indices/commands fingerprint

    ~UiDrawSnapshot()
    {
        for (ImDrawList* dl : cmd_lists) IM_DELETE(dl);
    }
};

class PopupLayer {
public:
    PopupLayer();
    ~PopupLayer();

    PopupLayer(const PopupLayer&) = delete;
    PopupLayer& operator=(const PopupLayer&) = delete;

    // ---- main thread -----------------------------------------------------------------------
    //
    // Records the Player window this layer belongs to (the popup's owner). Idempotent. The
    // popup HWND itself is created lazily, the first time something is actually routed.
    void attach(HWND owner);

    // Routes the finish-frame draw data: every draw list belonging to ImGui window
    // `window_name` -- its own DrawList plus any child window rooted in it -- is removed from
    // `dd` and cloned into this layer's snapshot. `dd` may be null (routing disabled this
    // frame), and a window that was not drawn ends the routing just the same. The layer's
    // window follows the routed window's rect in main-client coordinates.
    void route(ImDrawData* dd, const char* window_name);

    // Applies the routing decision to the real window: moves it onto the routed window's
    // screen rect, and shows it only once the present thread has actually rendered a frame at
    // that size (showing a swapchain that never presented would flash the desktop through for
    // one composition frame). Hides it when nothing was routed.
    void sync();

    // Hides the window now and stops routing until the next route() (minimize, deactivate,
    // page view gone, teardown).
    void hide();

    // Drops window + swapchain. Must run on the main thread AFTER the present thread has been
    // joined (it frees snapshots through ImGui's allocator, so before ui_shutdown()).
    void release();

    // ---- present thread (inside d3d_mutex_) ------------------------------------------------
    //
    // Renders the newest snapshot into the popup's own swapchain and presents it with a zero
    // sync interval. Skips the frame when the identical content is already on screen, and does
    // nothing at all while nothing is routed.
    void render(ID3D11Device* device, ID3D11DeviceContext* context);

    // ---- introspection (scripts / diagnostics; main thread) --------------------------------
    HWND hwnd() const { return hwnd_; }
    bool visible() const { return shown_; }
    bool routed() const { return routed_; }
    // True once this layer could NOT get its window or its swapchain (sticky). The owner uses
    // it to fall back to drawing the routed window in the main swapchain -- with the page view
    // stepping aside -- instead of ending up with a window nobody can see.
    bool unavailable() const { return unavailable_.load(std::memory_order_relaxed); }
    // Last routed window rect in main-client coordinates (0,0,0,0 when nothing is routed).
    void client_rect(int out[4]) const;
    // Last applied window rect in screen coordinates (0,0,0,0 while never placed).
    void screen_rect(int out[4]) const;
    // Frames the present thread has rendered into this layer (monotonic; 0 = never).
    uint64_t rendered_frames() const { return rendered_gen_.load(std::memory_order_relaxed); }

    // Name of the ImGui tooltip window ("##Tooltip_%02d") that has geometry in the frame just
    // rendered, or nullptr. ImGui creates tooltip windows on demand from ImGui::SetTooltip(), so
    // they can only be found by name, after the fact.
    static const char* active_tooltip_name();

    // Name of the ImGui window of the topmost OPEN popup (combo list, BeginPopupContextItem menu,
    // any BeginPopup*) that has geometry in this frame, or nullptr. Popups are their own root
    // windows, so a layer routing a panel never picks them up -- they need a layer of their own,
    // and this is how that layer finds out what to route.
    static const char* active_popup_name();

private:
    bool ensure_window();
    void feed_mouse(UINT msg, WPARAM wp, LPARAM lp);
    bool ensure_swapchain(ID3D11Device* device, ID3D11DeviceContext* context, int w, int h);
    void release_swapchain();
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    bool shown_ = false;   // the popup HWND is currently visible
    bool routed_ = false;  // something was routed this frame
    uint64_t shown_gen_ = 0; // rendered_frames() value the current window content came from
    int client_rect_[4] = { 0, 0, 0, 0 };         // same, rounded to whole pixels
    int placed_[4] = { 0, 0, 0, 0 };              // last applied screen rect

    // Cross-thread: `wanted_` tells the present thread whether anything should be on screen,
    // `visible_` whether the identical content is already displayed, and `rendered_gen_` /
    // `rendered_w_` / `rendered_h_` tell the main thread when it may show the window.
    std::atomic<bool> wanted_{ false };
    std::atomic<bool> visible_{ false };
    std::atomic<bool> unavailable_{ false };
    std::atomic<uint64_t> rendered_gen_{ 0 };
    std::atomic<int> rendered_w_{ 0 };
    std::atomic<int> rendered_h_{ 0 };

    std::mutex mutex_;
    std::unique_ptr<UiDrawSnapshot> pending_; // main thread publishes, present thread moves out
    std::unique_ptr<UiDrawSnapshot> active_;  // present thread only (never touched under mutex_)

    // Present thread only.
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    int sc_w_ = 0;
    int sc_h_ = 0;
    bool rendered_valid_ = false;  // a frame was rendered into the current surface
    uint64_t rendered_hash_ = 0;   // its content fingerprint (skip an unchanged frame)
    int rendered_rect_[4] = { 0, 0, 0, 0 };
};
