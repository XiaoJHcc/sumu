// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Spike 2: the load-bearing mental model of sumu itself -- "player is the master, AI is
// the servant". A single wall-clock-driven present loop that NEVER stalls: every tick it
// picks the best ready frame for "whatever frame number the wall clock says should be on
// screen right now" -- an AI-dehazed frame from a ready-map if one exists for that frame
// number, otherwise the original decoded frame from a passthrough ring buffer. Present is
// NEVER blocked waiting on the AI producer.
//
// Threading model (three threads, one shared ID3D11Device + one shared immediate
// ID3D11DeviceContext, ID3D11Multithread::SetMultithreadProtected(TRUE)):
//
//   - decode thread:  FFmpeg d3d11va decode (spike0's Decoder, reused verbatim) ->
//                      CopySubresourceRegion() of the decoded NV12 slice into a persistent
//                      NV12 Texture2DArray ("passthrough ring", capacity kRingCapacity,
//                      indexed by frame_num % kRingCapacity). Throttled to stay within
//                      kRingCapacity frames of the present head so it never overwrites a
//                      slot the present thread hasn't had a chance to read yet.
//   - AI push thread (called from Python, NOT owned by this class): push_ai_frame() does
//                      the spike1 zero-copy CUDA<->D3D11 bridge (cuGraphicsMapResources ->
//                      cuMemcpy2D device->array -> cuGraphicsUnmapResources) into a single
//                      persistent "landing" texture, THEN a CopySubresourceRegion from the
//                      landing texture into the AI ready-map's Texture2DArray slot for that
//                      frame_num. Only after that copy is issued does the ready-map tag get
//                      set -- so the present thread can never sample a half-written slot.
//   - present thread: wall-clock QPC pacing (same hybrid sleep+spin as spike 0), recomputes
//                      current_frame = round((now - start) * fps) every tick (so a stall
//                      anywhere causes it to SKIP forward, never fall behind and never
//                      double-present), looks the frame number up in the AI ready-map
//                      first, falls back to the passthrough ring, and as a last resort (only
//                      possible for a few ticks at start-of-run) re-presents the last frame
//                      actually shown. Present(1,0) then records a QPC timestamp + a
//                      "source" tag for later hitch analysis.
//
// Locking around the shared ID3D11Device/context, and a correction made after empirical
// testing: the original design here assumed ID3D11Multithread::SetMultithreadProtected(TRUE)
// alone was sufficient without any additional CPU-side lock, on the theory that decode and
// AI-push threads only ever issue a single, self-contained CopySubresourceRegion call each
// (only the present thread issues a multi-call "set state then Draw" sequence), so the only
// thing that needed cross-thread serialization was already handled by the driver's own
// per-call thread-safety guarantee. That assumption turned out to be unsafe in practice: a
// 50-second stress run (~3000 presents, ~1300 concurrent AI pushes via CUDA/D3D11 interop,
// ~3000 decode-thread copies, all against one shared immediate context) hit a genuine
// DXGI_ERROR_DEVICE_HUNG (0x887a0006) after a few thousand operations -- rare enough to not
// show up in short smoke tests, common enough to be a real correctness bug, not a fluke.
// Rather than try to characterize exactly which interleaving the driver couldn't tolerate,
// this now holds one explicit mutex (d3d_mutex_) around EVERY thread's touch of the shared
// context -- decode's CopySubresourceRegion, push_ai_frame()'s whole CUDA-map/copy/unmap +
// CopySubresourceRegion sequence, and the present thread's entire draw_and_present() call --
// so no two threads ever have D3D11 or CUDA-D3D11-interop calls in flight against this
// context at the same time, period. This costs little: each individual call was already
// measured at sub-millisecond cost (see docs/spike_results.md), so serializing them doesn't
// meaningfully compete with the ~16.7ms frame budget; SetMultithreadProtected(TRUE) is kept
// as defense-in-depth but is no longer the only thing standing between this code and a hang.
//
// Also note: the CUDA interop mapping in push_ai_frame() NEVER touches the big AI ready-map
// array directly -- it only ever maps/writes a single small non-array "landing" texture
// (registered once with cuGraphicsD3D11RegisterResource, same pattern as spike 1). The
// ready-map array itself is only ever touched by ordinary D3D11 copies, never CUDA-mapped,
// so the present thread sampling some OTHER slot of that array while a push is in flight
// against the landing texture can never race against a live CUDA resource mapping (this
// part of the original reasoning held up; d3d_mutex_ is on top of it, not instead of it).

#ifndef NOMINMAX
#define NOMINMAX // windows.h's min/max macros otherwise break std::min/std::max call sites below
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#include <cuda.h>
#include <cudaD3D11.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "decoder.h"

namespace py = pybind11;
using Microsoft::WRL::ComPtr;

namespace {

// ---- error helpers -----------------------------------------------------------------------

void check_hr(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s failed: hr=0x%08lx", what, hr);
        throw std::runtime_error(buf);
    }
}

void check_cu(CUresult res, const char* what)
{
    if (res != CUDA_SUCCESS) {
        const char* name = nullptr;
        const char* desc = nullptr;
        cuGetErrorName(res, &name);
        cuGetErrorString(res, &desc);
        char buf[512];
        snprintf(buf, sizeof(buf), "%s failed: CUresult=%d (%s: %s)", what,
            static_cast<int>(res), name ? name : "?", desc ? desc : "?");
        throw std::runtime_error(buf);
    }
}

// VSMain is shared by both pixel shaders (fullscreen triangle from SV_VertexID, no vertex
// buffer). Distinct registers (t0/t1 for NV12 Y/UV, t2 for the AI RGBA8 array) so both
// entry points can live in one compile unit with zero risk of register-binding conflicts.
const char* kShaderSrc = R"HLSL(
Texture2DArray<float>  texY   : register(t0);
Texture2DArray<float2> texUV  : register(t1);
Texture2DArray<float4> texAI  : register(t2);
SamplerState           samp0  : register(s0);

cbuffer SliceCB : register(b0)
{
    uint arraySlice;
    uint3 _pad;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    float2 uv[3]  = { float2(0.0, 1.0),   float2(0.0, -1.0), float2(2.0, 1.0) };
    VSOut o;
    o.pos = float4(pos[vid], 0.0, 1.0);
    o.uv = uv[vid];
    return o;
}

// passthrough source: decoder's NV12 -> RGB (BT.709 limited range), identical math to
// spike 0's shader.
float4 PSMain_NV12(VSOut i) : SV_Target
{
    float3 uvw = float3(i.uv, (float)arraySlice);
    float y = texY.Sample(samp0, uvw).r * 255.0;
    float2 cbcr = texUV.Sample(samp0, uvw).rg * 255.0;

    float c = y - 16.0;
    float d = cbcr.x - 128.0;
    float e = cbcr.y - 128.0;

    float r = clamp((298.082 * c + 408.583 * e) / 256.0, 0.0, 255.0);
    float g = clamp((298.082 * c - 100.291 * d - 208.120 * e) / 256.0, 0.0, 255.0);
    float b = clamp((298.082 * c + 516.412 * d) / 256.0, 0.0, 255.0);

    return float4(r / 255.0, g / 255.0, b / 255.0, 1.0);
}

// AI source: ready-map's RGBA8 array, straight passthrough sample.
float4 PSMain_AI(VSOut i) : SV_Target
{
    float3 uvw = float3(i.uv, (float)arraySlice);
    return texAI.Sample(samp0, uvw);
}
)HLSL";

// ---- global Win32 state (one window per process, fine for a spike) ------------------------
bool g_quit = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        g_quit = true;
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            g_quit = true;
            PostQuitMessage(0);
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) return 0; // swallow Alt+Enter, no exclusive fullscreen here
        break;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

double pctile(std::vector<double>& sorted_ms, double q)
{
    if (sorted_ms.empty()) return 0.0;
    double pos = q * (sorted_ms.size() - 1);
    size_t lo = static_cast<size_t>(pos);
    size_t hi = std::min(lo + 1, sorted_ms.size() - 1);
    double frac = pos - lo;
    return sorted_ms[lo] * (1.0 - frac) + sorted_ms[hi] * frac;
}

py::dict summarize(std::vector<double> samples_ms)
{
    py::dict d;
    if (samples_ms.empty()) {
        d["n"] = 0; d["median_ms"] = 0.0; d["p99_ms"] = 0.0; d["max_ms"] = 0.0; d["mean_ms"] = 0.0;
        return d;
    }
    std::sort(samples_ms.begin(), samples_ms.end());
    double sum = 0.0;
    for (double v : samples_ms) sum += v;
    d["n"] = samples_ms.size();
    d["median_ms"] = pctile(samples_ms, 0.5);
    d["p99_ms"] = pctile(samples_ms, 0.99);
    d["max_ms"] = samples_ms.back();
    d["min_ms"] = samples_ms.front();
    d["mean_ms"] = sum / samples_ms.size();
    return d;
}

// Present-tick "source" tags recorded in the trace, distinguishing a freshly-drawn frame
// from a repeat-of-last-frame fallback (the latter should be ~never after startup).
enum class Source : int8_t {
    PassthroughFresh = 0,
    AiFresh = 1,
    PassthroughStale = 2, // re-presented because neither map had this exact frame_num
    AiStale = 3,
};

bool is_ai(Source s) { return s == Source::AiFresh || s == Source::AiStale; }

int64_t qpc_delta_ms_d(LARGE_INTEGER a, LARGE_INTEGER b, double freq)
{
    return static_cast<int64_t>((b.QuadPart - a.QuadPart) * 1000.0 / freq);
}

} // namespace

// ---------------------------------------------------------------------------------------
class RealtimePresenter
{
public:
    // Ring capacity in frames (~1s at 60fps): both the passthrough decode-ahead buffer and
    // the AI ready-map share this capacity/indexing scheme. Must comfortably exceed both
    // the decode-ahead distance and the AI producer's max simulated lateness (150ms, ~9
    // frames at 60fps -- 64 frames leaves ample headroom). Originally tried 180 (~3s) per
    // the initial design sketch, but two full-4K texture arrays at that depth (NV12 ring
    // ~2.1GB + RGBA8 ready-map ~5.7GB = ~7.8GB) hit E_OUTOFMEMORY on CreateTexture2D on
    // this machine (16GB 4080, but shared with the desktop compositor + remote-access
    // virtual display adapters per docs/spike_results.md's baseline notes) -- 64 slices
    // (~2.8GB combined) was the fix.
    static constexpr UINT kRingCapacity = 64;
    // Decode thread throttle: stop decoding once this many frames ahead of the present
    // head, so it never overwrites a ring slot the present thread hasn't had a chance to
    // read yet (needs a safety margin below kRingCapacity, not exactly at it).
    static constexpr int64_t kDecodeAheadMax = static_cast<int64_t>(kRingCapacity) - 10;
    // Startup buffering: how many decoded frames to have ready before starting the wall
    // clock, so frame 0's present tick never has to fall back to "nothing decoded yet".
    static constexpr int64_t kStartBufferFrames = 5;

    RealtimePresenter(const std::string& video_path, int width, int height, bool maximized)
        : src_width_(width), src_height_(height)
    {
        timeBeginPeriod(1);
        timer_period_set_ = true;

        create_window(maximized);
        create_device_and_swapchain();
        create_shader_pipeline();

        std::string decode_err;
        if (!decoder_.open(video_path, device_.Get(), decode_err))
            throw std::runtime_error("decoder.open failed: " + decode_err);
        fps_ = decoder_.fps();

        create_ring_resources();
        create_landing_texture();
        init_cuda_and_register();

        pt_tag_.assign(kRingCapacity, -1);
        ai_tag_.assign(kRingCapacity, -1);

        // Decode thread starts immediately and races ahead (throttled) of the present
        // head, which is still -1 (not started) at this point -- the throttle treats a
        // not-yet-started present head as "0" so decode can still build initial buffer.
        decode_thread_ = std::thread(&RealtimePresenter::decode_loop, this);

        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        for (;;) {
            if (pt_high_water_.load(std::memory_order_relaxed) >= kStartBufferFrames - 1)
                break;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double waited_s = static_cast<double>(now.QuadPart - t0.QuadPart) / qpc_freq_d();
            if (waited_s > 10.0)
                throw std::runtime_error("decode thread failed to buffer initial frames within 10s");
            Sleep(1);
        }

        QueryPerformanceFrequency(&freq_);
        QueryPerformanceCounter(&start_qpc_);
        present_thread_ = std::thread(&RealtimePresenter::present_loop, this);
    }

    ~RealtimePresenter() { close(); }

    void close()
    {
        if (closed_) return;
        closed_ = true;
        stop_.store(true, std::memory_order_relaxed);
        if (decode_thread_.joinable()) decode_thread_.join();
        if (present_thread_.joinable()) present_thread_.join();

        if (cu_res_) { cuGraphicsUnregisterResource(cu_res_); cu_res_ = nullptr; }
        if (cu_ctx_) { cuCtxSetCurrent(nullptr); cuDevicePrimaryCtxRelease(cu_dev_); cu_ctx_ = nullptr; }

        decoder_.close();

        if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
        if (timer_period_set_) { timeEndPeriod(1); timer_period_set_ = false; }
    }

    void pump_messages()
    {
        MSG msg{};
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    bool should_quit() const { return g_quit; }

    double fps() const { return fps_; }

    // Seconds, on the same clock as Python's time.perf_counter() -- on Windows, CPython's
    // perf_counter() is implemented via QueryPerformanceCounter, the same monotonic,
    // system-wide (not per-process-offset) counter used for start_qpc_ here. Exposing this
    // lets spike2_driver.py's AI producer thread anchor its "ideal_time = frame_num / fps"
    // schedule to the presenter's ACTUAL frame-0 wall-clock instant, rather than to whatever
    // moment Python happens to spin up the producer thread (which is always somewhat later
    // than start_qpc_, since RealtimePresenter's constructor -- window/device/shader/ring
    // setup, decode-thread spawn, and blocking until the initial decode buffer fills --
    // already completes before control even returns to Python). Without this, every AI push
    // is systematically shifted by that constant startup gap, which quietly turns "fast"
    // pushes into de-facto-late ones and tanks ai_hit_rate for reasons that have nothing to
    // do with the actual mixing logic being tested.
    double start_time_s() const
    {
        return static_cast<double>(start_qpc_.QuadPart) / static_cast<double>(freq_.QuadPart);
    }

    // dev_ptr: CUdeviceptr (torch tensor's data_ptr()) for a contiguous uint8 RGBA8 (H,W,4)
    // tensor representing the AI-processed frame for content frame number `frame_num`.
    // Never blocks the present thread -- only ever touches the small landing texture via
    // CUDA, then a single D3D11 copy into the ready-map slot, then flips that slot's tag.
    void push_ai_frame(int64_t frame_num, uint64_t dev_ptr, int fwidth, int fheight, size_t pitch_bytes)
    {
        if (fwidth != src_width_ || fheight != src_height_)
            throw std::runtime_error("push_ai_frame: frame size does not match source video size");

        std::lock_guard<std::mutex> push_lock(push_mutex_); // defensive: single-producer assumed

        // push_ai_frame is called from whatever OS thread Python's AI-producer thread is --
        // a different native thread than the one that ran the constructor (where cu_ctx_ was
        // originally made current). The CUDA runtime API auto-attaches a thread's implicit
        // context lazily on first use (which is why this mostly "just works" once the caller
        // thread has touched a CUDA tensor), but driver-API calls we issue directly here do
        // not get that lazy attach -- make it explicit and unconditional so there is no
        // window where this thread's current context is unset or stale.
        check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent (push_ai_frame)");

        std::lock_guard<std::mutex> d3d_lock(d3d_mutex_); // see file header: serializes ALL D3D11/CUDA-interop use of the shared context

        try {
        check_cu(cuGraphicsMapResources(1, &cu_res_, 0), "cuGraphicsMapResources");
        CUarray cu_arr = nullptr;
        check_cu(cuGraphicsSubResourceGetMappedArray(&cu_arr, cu_res_, 0, 0),
            "cuGraphicsSubResourceGetMappedArray");

        CUDA_MEMCPY2D cp{};
        cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.srcDevice = static_cast<CUdeviceptr>(dev_ptr);
        cp.srcPitch = pitch_bytes;
        cp.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        cp.dstArray = cu_arr;
        cp.WidthInBytes = static_cast<size_t>(fwidth) * 4;
        cp.Height = static_cast<size_t>(fheight);
        check_cu(cuMemcpy2D(&cp), "cuMemcpy2D (device -> mapped landing array)");

        check_cu(cuGraphicsUnmapResources(1, &cu_res_, 0), "cuGraphicsUnmapResources");

        UINT slot = wrap_slot(frame_num);
        // Still under d3d_lock -- guaranteed no other thread is touching the context.
        context_->CopySubresourceRegion(ai_ring_tex_.Get(), slot, 0, 0, 0,
            landing_tex_.Get(), 0, nullptr);

        {
            std::lock_guard<std::mutex> lk(ready_mutex_);
            ai_tag_[slot] = frame_num; // only mark ready AFTER the copy above is issued
        }
        ++ai_push_count_;
        } catch (const std::exception& e) {
            // Diagnostic aid: CUresult=999 (CUDA_ERROR_UNKNOWN) that appears out of nowhere
            // and then persists for every subsequent call is the classic signature of the
            // D3D11 device having been lost (TDR / DXGI_ERROR_DEVICE_REMOVED), which poisons
            // every CUDA context sharing that device too. Surface the device-removed reason
            // (if any) alongside the original error so this is distinguishable from an
            // ordinary transient CUDA failure without needing a repro session.
            HRESULT removed = device_ ? device_->GetDeviceRemovedReason() : S_OK;
            char buf[768];
            snprintf(buf, sizeof(buf), "%s [device_removed_reason=0x%08lx]", e.what(),
                static_cast<unsigned long>(removed));
            throw std::runtime_error(buf);
        }
    }

    py::dict stats()
    {
        py::dict d;
        d["fps"] = fps_;
        d["present_count"] = present_count_;
        d["decode_frame_count"] = decode_frame_count_;
        d["ai_push_count"] = ai_push_count_.load();
        d["n_ai_fresh"] = n_ai_fresh_;
        d["n_ai_stale"] = n_ai_stale_;
        d["n_pt_fresh"] = n_pt_fresh_;
        d["n_pt_stale"] = n_pt_stale_;
        d["ai_hit_rate"] = ai_hit_rate();
        return d;
    }

    double ai_hit_rate() const
    {
        if (present_count_ == 0) return 0.0;
        return static_cast<double>(n_ai_fresh_) / static_cast<double>(present_count_);
    }

    // trace CSV: qpc_ns,source,frame_num -- analyze_present.py (--format ns) only reads
    // column 0, so this stays compatible with the shared analysis tool while also carrying
    // the per-tick source tag for spike2's own switch-point analysis.
    void dump_trace(const std::string& path)
    {
        std::ofstream f(path, std::ios::trunc);
        f << "qpc_ns,source,frame_num\n";
        for (size_t i = 0; i < present_qpc_ns_.size(); ++i) {
            f << present_qpc_ns_[i] << "," << static_cast<int>(present_source_[i]) << ","
              << present_frame_num_[i] << "\n";
        }
    }

private:
    // ---- setup -----------------------------------------------------------------------------

    void create_window(bool maximized)
    {
        HINSTANCE hinst = GetModuleHandle(nullptr);
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "Sumu_Spike2_WndClass";
        RegisterClassExA(&wc);

        RECT rc{ 0, 0, 1280, 720 };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExA(0, wc.lpszClassName, "sumu spike2 - clock-driven AI/passthrough mixing",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
        if (!hwnd_) throw std::runtime_error("CreateWindowEx failed");
        ShowWindow(hwnd_, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
        UpdateWindow(hwnd_);

        RECT client_rc;
        GetClientRect(hwnd_, &client_rc);
        win_width_ = static_cast<UINT>(std::max<LONG>(1, client_rc.right - client_rc.left));
        win_height_ = static_cast<UINT>(std::max<LONG>(1, client_rc.bottom - client_rc.top));
    }

    void create_device_and_swapchain()
    {
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &got, &context_);
        check_hr(hr, "D3D11CreateDevice");

        // Present thread, decode thread, and the AI push thread (called from Python) all
        // issue calls on this same immediate context concurrently -- this is the mechanism
        // that makes that safe (see file header for why a single external lock isn't also
        // required for the single-call copy paths).
        {
            ComPtr<ID3D11Multithread> mt;
            check_hr(device_.As(&mt), "QueryInterface(ID3D11Multithread)");
            mt->SetMultithreadProtected(TRUE);
        }

        ComPtr<IDXGIDevice> dxgi_device;
        device_.As(&dxgi_device);
        dxgi_device->GetAdapter(&adapter_);
        ComPtr<IDXGIFactory2> factory;
        adapter_->GetParent(IID_PPV_ARGS(&factory));

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = win_width_;
        scd.Height = win_height_;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        scd.Scaling = DXGI_SCALING_STRETCH;

        hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &scd, nullptr, nullptr, &swapchain_);
        check_hr(hr, "CreateSwapChainForHwnd");
        factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

        ComPtr<ID3D11Texture2D> backbuffer;
        swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
        hr = device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &backbuffer_rtv_);
        check_hr(hr, "CreateRenderTargetView");
    }

    void create_shader_pipeline()
    {
        ComPtr<ID3DBlob> vs_blob, ps_nv12_blob, ps_ai_blob, err_blob;
        HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "spike2.hlsl", nullptr, nullptr,
            "VSMain", "vs_5_0", 0, 0, &vs_blob, &err_blob);
        if (FAILED(hr)) throw std::runtime_error("VS compile failed: " +
            std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));

        hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "spike2.hlsl", nullptr, nullptr,
            "PSMain_NV12", "ps_5_0", 0, 0, &ps_nv12_blob, &err_blob);
        if (FAILED(hr)) throw std::runtime_error("PS(NV12) compile failed: " +
            std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));

        hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "spike2.hlsl", nullptr, nullptr,
            "PSMain_AI", "ps_5_0", 0, 0, &ps_ai_blob, &err_blob);
        if (FAILED(hr)) throw std::runtime_error("PS(AI) compile failed: " +
            std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));

        check_hr(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_),
            "CreateVertexShader");
        check_hr(device_->CreatePixelShader(ps_nv12_blob->GetBufferPointer(), ps_nv12_blob->GetBufferSize(), nullptr, &ps_nv12_),
            "CreatePixelShader(NV12)");
        check_hr(device_->CreatePixelShader(ps_ai_blob->GetBufferPointer(), ps_ai_blob->GetBufferSize(), nullptr, &ps_ai_),
            "CreatePixelShader(AI)");

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        check_hr(device_->CreateSamplerState(&sd, &sampler_), "CreateSamplerState");

        struct SliceCB { UINT arraySlice; UINT pad[3]; };
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(SliceCB);
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check_hr(device_->CreateBuffer(&cbd, nullptr, &slice_cb_), "CreateBuffer(slice_cb_)");
    }

    void create_ring_resources()
    {
        // Passthrough ring: NV12 texture array, one physical resource owned by US (not the
        // decoder's own hw_frames_ctx pool), so decode-ahead and present don't fight over
        // the decoder's own limited pool of in-flight slices.
        D3D11_TEXTURE2D_DESC pt_desc{};
        pt_desc.Width = src_width_;
        pt_desc.Height = src_height_;
        pt_desc.MipLevels = 1;
        pt_desc.ArraySize = kRingCapacity;
        pt_desc.Format = DXGI_FORMAT_NV12;
        pt_desc.SampleDesc.Count = 1;
        pt_desc.Usage = D3D11_USAGE_DEFAULT;
        // NV12 is a video format; on this driver CreateTexture2D rejects it (E_INVALIDARG)
        // with BIND_SHADER_RESOURCE alone -- matching the decoder's own hw_frames_ctx bind
        // flags (BIND_DECODER | BIND_SHADER_RESOURCE, see decoder.cpp) makes it succeed.
        // We never actually use this as a decode target, just as a same-format GPU-side
        // copy destination + SRV source.
        pt_desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
        check_hr(device_->CreateTexture2D(&pt_desc, nullptr, &pt_ring_tex_), "CreateTexture2D(pt_ring_tex_)");

        pt_srv_y_.resize(kRingCapacity);
        pt_srv_uv_.resize(kRingCapacity);
        for (UINT i = 0; i < kRingCapacity; ++i) {
            D3D11_SHADER_RESOURCE_VIEW_DESC yd{};
            yd.Format = DXGI_FORMAT_R8_UNORM;
            yd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            yd.Texture2DArray.MostDetailedMip = 0;
            yd.Texture2DArray.MipLevels = 1;
            yd.Texture2DArray.FirstArraySlice = i;
            yd.Texture2DArray.ArraySize = 1;
            check_hr(device_->CreateShaderResourceView(pt_ring_tex_.Get(), &yd, &pt_srv_y_[i]), "CreateSRV(pt Y)");

            D3D11_SHADER_RESOURCE_VIEW_DESC uvd = yd;
            uvd.Format = DXGI_FORMAT_R8G8_UNORM;
            check_hr(device_->CreateShaderResourceView(pt_ring_tex_.Get(), &uvd, &pt_srv_uv_[i]), "CreateSRV(pt UV)");
        }

        // AI ready-map: RGBA8 texture array, same indexing scheme (frame_num % capacity).
        D3D11_TEXTURE2D_DESC ai_desc{};
        ai_desc.Width = src_width_;
        ai_desc.Height = src_height_;
        ai_desc.MipLevels = 1;
        ai_desc.ArraySize = kRingCapacity;
        ai_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ai_desc.SampleDesc.Count = 1;
        ai_desc.Usage = D3D11_USAGE_DEFAULT;
        ai_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        check_hr(device_->CreateTexture2D(&ai_desc, nullptr, &ai_ring_tex_), "CreateTexture2D(ai_ring_tex_)");

        ai_srv_.resize(kRingCapacity);
        for (UINT i = 0; i < kRingCapacity; ++i) {
            D3D11_SHADER_RESOURCE_VIEW_DESC ad{};
            ad.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            ad.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            ad.Texture2DArray.MostDetailedMip = 0;
            ad.Texture2DArray.MipLevels = 1;
            ad.Texture2DArray.FirstArraySlice = i;
            ad.Texture2DArray.ArraySize = 1;
            check_hr(device_->CreateShaderResourceView(ai_ring_tex_.Get(), &ad, &ai_srv_[i]), "CreateSRV(ai)");
        }
    }

    void create_landing_texture()
    {
        // The ONLY texture the CUDA interop ever maps. Kept deliberately separate from
        // ai_ring_tex_ (see file header) so a live CUDA mapping never overlaps a slot the
        // present thread might be sampling from the ready-map array.
        D3D11_TEXTURE2D_DESC td{};
        td.Width = src_width_;
        td.Height = src_height_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        check_hr(device_->CreateTexture2D(&td, nullptr, &landing_tex_), "CreateTexture2D(landing_tex_)");
    }

    void init_cuda_and_register()
    {
        check_cu(cuInit(0), "cuInit");
        check_cu(cuDeviceGet(&cu_dev_, 0), "cuDeviceGet");

        CUdevice dev_from_adapter = -1;
        CUresult r = cuD3D11GetDevice(&dev_from_adapter, adapter_.Get());
        if (r != CUDA_SUCCESS || dev_from_adapter != cu_dev_) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "cuD3D11GetDevice mismatch: D3D adapter maps to CUDA device %d, expected %d (CUresult=%d)",
                (int)dev_from_adapter, (int)cu_dev_, (int)r);
            throw std::runtime_error(buf);
        }

        check_cu(cuDevicePrimaryCtxRetain(&cu_ctx_, cu_dev_), "cuDevicePrimaryCtxRetain");
        check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent");

        check_cu(cuGraphicsD3D11RegisterResource(&cu_res_, landing_tex_.Get(), CU_GRAPHICS_REGISTER_FLAGS_NONE),
            "cuGraphicsD3D11RegisterResource");
    }

    // ---- decode thread -----------------------------------------------------------------------

    void decode_loop()
    {
        while (!stop_.load(std::memory_order_relaxed)) {
            // Throttle: never get more than kDecodeAheadMax frames ahead of the present
            // head, so we never overwrite a ring slot before present has had its one
            // chance to read it. Before the present thread has started, present_head_ is
            // -1; treat that as "0" so initial buffering can still proceed.
            for (;;) {
                if (stop_.load(std::memory_order_relaxed)) return;
                int64_t head = present_head_frame_.load(std::memory_order_relaxed);
                int64_t effective_head = head < 0 ? 0 : head;
                int64_t high = pt_high_water_.load(std::memory_order_relaxed);
                if (high - effective_head < kDecodeAheadMax) break;
                Sleep(1);
            }

            DecodedFrame df;
            if (!decoder_.next_frame(df)) {
                // Unrecoverable decode error (Decoder already loops internally on EOF, so
                // this should only fire on a genuine error). Stop the whole run rather than
                // spin; present thread will keep re-presenting the last good frame forever
                // via the stale-fallback path rather than hitching.
                stop_.store(true, std::memory_order_relaxed);
                return;
            }

            int64_t frame_num = static_cast<int64_t>(std::llround(df.pts_seconds * fps_));
            UINT slot = wrap_slot(frame_num);

            {
                // See file header: serializes ALL D3D11/CUDA-interop use of the shared context.
                std::lock_guard<std::mutex> d3d_lock(d3d_mutex_);
                context_->CopySubresourceRegion(pt_ring_tex_.Get(), slot, 0, 0, 0,
                    df.texture, df.array_slice, nullptr);
            }

            {
                std::lock_guard<std::mutex> lk(ready_mutex_);
                pt_tag_[slot] = frame_num;
            }
            pt_high_water_.store(frame_num, std::memory_order_relaxed);
            ++decode_frame_count_;
        }
    }

    // ---- present thread ------------------------------------------------------------------------

    void present_loop()
    {
        const double ticks_per_ms = static_cast<double>(freq_.QuadPart) / 1000.0;
        int64_t last_frame = -1;
        Source last_actual_source = Source::PassthroughFresh;
        UINT last_actual_slot = 0;

        present_qpc_ns_.reserve(1u << 16);
        present_source_.reserve(1u << 16);
        present_frame_num_.reserve(1u << 16);

        while (!stop_.load(std::memory_order_relaxed)) {
            int64_t target_frame = last_frame + 1;
            LARGE_INTEGER target_qpc;
            target_qpc.QuadPart = start_qpc_.QuadPart +
                static_cast<LONGLONG>(static_cast<double>(target_frame) / fps_ * freq_.QuadPart);
            if (!wait_until_qpc_or_stop(target_qpc, ticks_per_ms)) break;

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double elapsed_s = static_cast<double>(now.QuadPart - start_qpc_.QuadPart) / freq_.QuadPart;
            int64_t current_frame = static_cast<int64_t>(std::llround(elapsed_s * fps_));
            // Clock-driven, never falls behind: if we ever overshot (a stall somewhere),
            // jump forward to wherever the wall clock says we should be -- never try to
            // "catch up" by presenting several backlogged frames back to back.
            if (current_frame <= last_frame) current_frame = last_frame + 1;
            last_frame = current_frame;
            present_head_frame_.store(current_frame, std::memory_order_relaxed);

            UINT slot = wrap_slot(current_frame);
            Source source;
            UINT use_slot;
            {
                std::lock_guard<std::mutex> lk(ready_mutex_);
                if (ai_tag_[slot] == current_frame) {
                    source = Source::AiFresh;
                    use_slot = slot;
                } else if (pt_tag_[slot] == current_frame) {
                    source = Source::PassthroughFresh;
                    use_slot = slot;
                } else {
                    // Neither map has this exact frame number ready. NEVER block -- just
                    // re-present whatever was actually shown last tick.
                    source = is_ai(last_actual_source) ? Source::AiStale : Source::PassthroughStale;
                    use_slot = last_actual_slot;
                }
            }

            {
                // See file header: serializes ALL D3D11/CUDA-interop use of the shared context.
                // Note this does NOT make the present thread wait on AI/decode readiness --
                // it only ensures no other thread's copy is mid-flight while this thread's
                // draw+Present sequence runs; decode/push threads are the ones who may
                // occasionally wait a fraction of a frame for this lock, never the reverse.
                std::lock_guard<std::mutex> d3d_lock(d3d_mutex_);
                draw_and_present(source, use_slot);
            }

            LARGE_INTEGER pres_now;
            QueryPerformanceCounter(&pres_now);
            present_qpc_ns_.push_back(qpc_to_ns(pres_now));
            present_source_.push_back(static_cast<int8_t>(source));
            present_frame_num_.push_back(current_frame);
            ++present_count_;
            switch (source) {
            case Source::AiFresh: ++n_ai_fresh_; break;
            case Source::AiStale: ++n_ai_stale_; break;
            case Source::PassthroughFresh: ++n_pt_fresh_; break;
            case Source::PassthroughStale: ++n_pt_stale_; break;
            }

            if (source == Source::AiFresh || source == Source::PassthroughFresh) {
                last_actual_source = source;
                last_actual_slot = use_slot;
            }
        }
    }

    void draw_and_present(Source source, UINT slot)
    {
        D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(win_width_), static_cast<float>(win_height_), 0.0f, 1.0f };
        context_->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rtvs[] = { backbuffer_rtv_.Get() };
        context_->OMSetRenderTargets(1, rtvs, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);

        struct SliceCB { UINT arraySlice; UINT pad[3]; };
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context_->Map(slice_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            SliceCB cb{ slot, {0, 0, 0} };
            memcpy(mapped.pData, &cb, sizeof(cb));
            context_->Unmap(slice_cb_.Get(), 0);
        }
        ID3D11Buffer* cbs[] = { slice_cb_.Get() };
        context_->PSSetConstantBuffers(0, 1, cbs);
        ID3D11SamplerState* samplers[] = { sampler_.Get() };
        context_->PSSetSamplers(0, 1, samplers);

        if (is_ai(source)) {
            context_->PSSetShader(ps_ai_.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srvs[] = { ai_srv_[slot].Get() };
            context_->PSSetShaderResources(2, 1, srvs);
        } else {
            context_->PSSetShader(ps_nv12_.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srvs[] = { pt_srv_y_[slot].Get(), pt_srv_uv_[slot].Get() };
            context_->PSSetShaderResources(0, 2, srvs);
        }

        context_->Draw(3, 0);
        swapchain_->Present(1, 0);
    }

    // ---- helpers -----------------------------------------------------------------------------

    static UINT wrap_slot(int64_t frame_num)
    {
        int64_t m = frame_num % static_cast<int64_t>(kRingCapacity);
        if (m < 0) m += kRingCapacity;
        return static_cast<UINT>(m);
    }

    static double qpc_freq_d()
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }

    static int64_t qpc_to_ns(LARGE_INTEGER c)
    {
        double ns_per_tick = 1e9 / qpc_freq_d();
        return static_cast<int64_t>(static_cast<double>(c.QuadPart) * ns_per_tick + 0.5);
    }

    // Hybrid sleep+spin wait (same strategy as spike 0's wait_until_qpc), but periodically
    // checks stop_ so close() doesn't have to wait out a full frame interval. Returns false
    // if stop was requested before the deadline.
    bool wait_until_qpc_or_stop(LARGE_INTEGER target, double ticks_per_ms)
    {
        LARGE_INTEGER now;
        for (;;) {
            if (stop_.load(std::memory_order_relaxed)) return false;
            QueryPerformanceCounter(&now);
            double remaining_ms = static_cast<double>(target.QuadPart - now.QuadPart) / ticks_per_ms;
            if (remaining_ms <= 0.0) return true;
            if (remaining_ms > 2.0) {
                Sleep(static_cast<DWORD>(std::min(remaining_ms - 1.0, 5.0)));
            } else {
                YieldProcessor();
            }
        }
    }

    // ---- state -------------------------------------------------------------------------------

    int src_width_ = 0;
    int src_height_ = 0;
    UINT win_width_ = 0;
    UINT win_height_ = 0;
    HWND hwnd_ = nullptr;
    bool timer_period_set_ = false;
    bool closed_ = false;

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

    std::mutex ready_mutex_;
    std::mutex push_mutex_;
    // Serializes EVERY thread's touch of the shared ID3D11Device/context (decode's copy,
    // push_ai_frame's CUDA-interop + copy, present's whole draw_and_present call) -- see the
    // file header for why this exists on top of SetMultithreadProtected(TRUE).
    std::mutex d3d_mutex_;

    Decoder decoder_;
    double fps_ = 60.0;

    std::thread decode_thread_;
    std::thread present_thread_;
    std::atomic<bool> stop_{ false };

    LARGE_INTEGER start_qpc_{};
    LARGE_INTEGER freq_{};
    std::atomic<int64_t> present_head_frame_{ -1 };

    std::vector<int64_t> present_qpc_ns_;
    std::vector<int8_t> present_source_;
    std::vector<int64_t> present_frame_num_;

    uint64_t present_count_ = 0;
    uint64_t decode_frame_count_ = 0;
    std::atomic<uint64_t> ai_push_count_{ 0 };
    uint64_t n_ai_fresh_ = 0, n_ai_stale_ = 0, n_pt_fresh_ = 0, n_pt_stale_ = 0;
};

PYBIND11_MODULE(sumu_rt, m)
{
    m.doc() = "sumu spike 2: clock-driven present loop mixing an AI ready-map with a passthrough ring buffer";

    py::class_<RealtimePresenter>(m, "RealtimePresenter")
        .def(py::init<const std::string&, int, int, bool>(),
            py::arg("video_path"), py::arg("width") = 3840, py::arg("height") = 2160,
            py::arg("maximized") = false)
        .def("push_ai_frame", &RealtimePresenter::push_ai_frame,
            py::arg("frame_num"), py::arg("dev_ptr"), py::arg("width"), py::arg("height"), py::arg("pitch_bytes"))
        .def("fps", &RealtimePresenter::fps)
        .def("start_time_s", &RealtimePresenter::start_time_s)
        .def("stats", &RealtimePresenter::stats)
        .def("ai_hit_rate", &RealtimePresenter::ai_hit_rate)
        .def("dump_trace", &RealtimePresenter::dump_trace, py::arg("path"))
        .def("pump_messages", &RealtimePresenter::pump_messages)
        .def("should_quit", &RealtimePresenter::should_quit)
        .def("close", &RealtimePresenter::close);
}
