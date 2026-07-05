// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Spike 1: prove that a torch CUDA tensor (device memory, produced by an AI model) can be
// blitted directly into a D3D11 present-face texture with ZERO main-memory round trip, and
// that the CUDA<->D3D11 interop overhead per frame is far below the 16.6ms/frame (60fps)
// budget. This is the load-bearing precondition for sumu's "AI insertion point" design
// (I3: all-GPU pipeline) -- if this doesn't hold, AI-assisted dehaze/demosaic can never be
// inserted into the real-time preview path without breaking the zero-copy contract.
//
// Architecture:
//   - A pybind11 extension module (sumu_present) exposing a single `Presenter` class.
//   - D3D11 device + flip-model swapchain + plain Win32 window: same pattern as spike 0
//     (D3D11CreateDevice hardware, DXGI_SWAP_EFFECT_FLIP_DISCARD, CreateSwapChainForHwnd).
//   - One persistent D3D11 texture ("AI frame target", DXGI_FORMAT_R8G8B8A8_UNORM, default
//     usage, BIND_SHADER_RESOURCE) that torch CUDA frames get copied into every frame.
//   - CUDA DRIVER API only (cuInit/cuDeviceGet/cuDevicePrimaryCtxRetain/cuCtxSetCurrent) --
//     deliberately retains the DEVICE'S PRIMARY CONTEXT, which is the exact same context
//     the CUDA RUNTIME API (and therefore torch) lazily creates/uses for a given device.
//     Primary contexts are per-process-per-device singletons (refcounted), so it does not
//     matter whether torch.cuda or this retain call happens first.
//   - cuGraphicsD3D11RegisterResource() registers the target texture ONCE at construction
//     (not per frame -- registration is expensive and is explicitly NOT part of the
//     per-frame interop budget we're measuring).
//   - Per frame: cuGraphicsMapResources -> cuGraphicsSubResourceGetMappedArray ->
//     cuMemcpy2D(device pointer -> mapped CUDA array) -> cuGraphicsUnmapResources. Both
//     sides of that memcpy are GPU memory (CU_MEMORYTYPE_DEVICE src, CU_MEMORYTYPE_ARRAY
//     dst) -- there is no cuMemcpyDtoH/HtoD anywhere in this file. That absence is the
//     actual proof of "zero main-memory round trip", not just a claim.
//   - A trivial fullscreen-triangle shader samples the target texture straight into the
//     backbuffer (point sampling, 1:1 texel mapping -- exact pixel correspondence matters
//     for verify_readback()).

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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace py = pybind11;
using Microsoft::WRL::ComPtr;

namespace {

// ---- error helpers ---------------------------------------------------------------------

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

const char* kShaderSrc = R"HLSL(
Texture2D    tex   : register(t0);
SamplerState samp0 : register(s0);

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

float4 PSMain(VSOut i) : SV_Target
{
    return tex.Sample(samp0, i.uv);
}
)HLSL";

// ---- global Win32 state (one window per process, fine for a spike) ---------------------
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
        d["n"] = 0;
        d["median_ms"] = 0.0;
        d["p99_ms"] = 0.0;
        d["max_ms"] = 0.0;
        d["mean_ms"] = 0.0;
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

} // namespace

// ------------------------------------------------------------------------------------------
class Presenter
{
public:
    Presenter(int width, int height, bool maximized)
        : width_(width), height_(height)
    {
        // Windows' default ~15.6ms system timer resolution rounds Sleep()-based pacing in
        // the python driver's frame-rate throttle; request 1ms resolution like spike 0 did.
        timeBeginPeriod(1);
        timer_period_set_ = true;

        create_window(maximized);
        create_device_and_swapchain();
        create_shader_pipeline();
        create_target_texture();
        init_cuda_and_register();
    }

    ~Presenter()
    {
        close();
    }

    void close()
    {
        if (closed_) return;
        closed_ = true;

        if (cu_res_) {
            // best-effort; ignore failures during teardown
            cuGraphicsUnregisterResource(cu_res_);
            cu_res_ = nullptr;
        }
        if (cu_ctx_) {
            cuCtxSetCurrent(nullptr);
            cuDevicePrimaryCtxRelease(cu_dev_);
            cu_ctx_ = nullptr;
        }
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (timer_period_set_) {
            timeEndPeriod(1);
            timer_period_set_ = false;
        }
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

    // dev_ptr: CUdeviceptr (torch tensor's data_ptr()) for a contiguous uint8 RGBA8
    // (H, W, 4) tensor. pitch_bytes: row pitch in bytes (W*4 for a contiguous tensor).
    void push_cuda_frame(uint64_t dev_ptr, int fwidth, int fheight, size_t pitch_bytes, bool capture)
    {
        if (fwidth != width_ || fheight != height_) {
            throw std::runtime_error("push_cuda_frame: frame size does not match target texture size");
        }

        LARGE_INTEGER t0, t1, t2, t3;
        QueryPerformanceCounter(&t0);

        // Make sure whatever CUDA kernel(s) wrote dev_ptr have actually finished. In the
        // real pipeline this would be a stream-ordered wait on an event the AI stage
        // signals; here (single default stream, both torch and our driver-API calls
        // implicitly on the same legacy default stream) an explicit sync keeps the spike
        // unambiguous. Deliberately NOT counted in the interop timing below -- it is
        // "wait for AI to finish", not "cost of the CUDA<->D3D11 bridge" itself.
        check_cu(cuCtxSynchronize(), "cuCtxSynchronize (pre-copy)");

        QueryPerformanceCounter(&t1);

        // ---- the actual zero-copy bridge: device memory -> mapped D3D11 texture --------
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
        // NOTE: both src (CU_MEMORYTYPE_DEVICE) and dst (CU_MEMORYTYPE_ARRAY) live in GPU
        // memory. There is no host pointer anywhere in this struct -- this is the
        // "no main-memory round trip" claim, made checkable in code rather than asserted.
        check_cu(cuMemcpy2D(&cp), "cuMemcpy2D (device -> mapped array)");

        check_cu(cuGraphicsUnmapResources(1, &cu_res_, 0), "cuGraphicsUnmapResources");

        QueryPerformanceCounter(&t2);

        // ---- D3D11 render: sample the just-updated texture straight into the backbuffer
        D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f };
        context_->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rtvs[] = { rtv_.Get() };
        context_->OMSetRenderTargets(1, rtvs, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { target_srv_.Get() };
        context_->PSSetShaderResources(0, 1, srvs);
        ID3D11SamplerState* samplers[] = { sampler_.Get() };
        context_->PSSetSamplers(0, 1, samplers);
        context_->Draw(3, 0);

        if (capture) {
            ComPtr<ID3D11Texture2D> backbuffer;
            swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
            context_->CopyResource(verify_staging_.Get(), backbuffer.Get());
            captured_ = true;
        }

        swapchain_->Present(1, 0);
        QueryPerformanceCounter(&t3);

        present_qpc_ns_.push_back(qpc_to_ns(t3));
        sync_ms_.push_back(qpc_delta_ms(t0, t1));
        interop_ms_.push_back(qpc_delta_ms(t1, t2));
        frame_ms_.push_back(qpc_delta_ms(t0, t3));
        ++frame_count_;
    }

    py::dict verify_readback()
    {
        py::dict result;
        result["captured"] = captured_;
        py::list pixels;
        if (!captured_) {
            result["pixels"] = pixels;
            return result;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = context_->Map(verify_staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        check_hr(hr, "Map(verify_staging_)");

        auto sample_at = [&](int x, int y) {
            const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
            const uint8_t* px = row + static_cast<size_t>(x) * 4;
            py::dict d;
            d["x"] = x; d["y"] = y;
            d["r"] = px[0]; d["g"] = px[1]; d["b"] = px[2]; d["a"] = px[3];
            return d;
        };

        pixels.append(sample_at(0, 0));
        pixels.append(sample_at(width_ - 1, 0));
        pixels.append(sample_at(0, height_ - 1));
        pixels.append(sample_at(width_ - 1, height_ - 1));
        pixels.append(sample_at(width_ / 2, height_ / 2));
        pixels.append(sample_at(width_ / 4, height_ / 2));
        pixels.append(sample_at((width_ * 3) / 4, height_ / 2));

        context_->Unmap(verify_staging_.Get(), 0);

        result["pixels"] = pixels;
        return result;
    }

    py::dict stats()
    {
        py::dict d;
        d["frame_count"] = frame_count_;
        d["sync_ms"] = summarize(sync_ms_);
        d["interop_ms"] = summarize(interop_ms_);
        d["frame_ms"] = summarize(frame_ms_);
        return d;
    }

    void present_trace_dump(const std::string& path)
    {
        std::ofstream f(path, std::ios::trunc);
        f << "qpc_ns\n";
        for (int64_t v : present_qpc_ns_) f << v << "\n";
    }

private:
    void create_window(bool maximized)
    {
        HINSTANCE hinst = GetModuleHandle(nullptr);
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "Sumu_Spike1_WndClass";
        RegisterClassExA(&wc);

        RECT rc{ 0, 0, width_, height_ };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExA(0, wc.lpszClassName, "sumu spike1 - CUDA->D3D11 zero-copy interop",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
        if (!hwnd_) throw std::runtime_error("CreateWindowEx failed");
        ShowWindow(hwnd_, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
        UpdateWindow(hwnd_);
    }

    void create_device_and_swapchain()
    {
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &got, &context_);
        check_hr(hr, "D3D11CreateDevice");

        {
            ComPtr<ID3D11Multithread> mt;
            if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);
        }

        ComPtr<IDXGIDevice> dxgi_device;
        device_.As(&dxgi_device);
        dxgi_device->GetAdapter(&adapter_);
        ComPtr<IDXGIFactory2> factory;
        adapter_->GetParent(IID_PPV_ARGS(&factory));

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = width_;
        scd.Height = height_;
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
        hr = device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &rtv_);
        check_hr(hr, "CreateRenderTargetView");

        // persistent CPU-readable staging texture for verify_readback()
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = width_;
        sd.Height = height_;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device_->CreateTexture2D(&sd, nullptr, &verify_staging_);
        check_hr(hr, "CreateTexture2D(verify_staging_)");
    }

    void create_shader_pipeline()
    {
        ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;
        HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "sumu_present.hlsl", nullptr, nullptr,
            "VSMain", "vs_5_0", 0, 0, &vs_blob, &err_blob);
        if (FAILED(hr)) {
            std::string msg = "VS compile failed: ";
            msg += err_blob ? (const char*)err_blob->GetBufferPointer() : "?";
            throw std::runtime_error(msg);
        }
        hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "sumu_present.hlsl", nullptr, nullptr,
            "PSMain", "ps_5_0", 0, 0, &ps_blob, &err_blob);
        if (FAILED(hr)) {
            std::string msg = "PS compile failed: ";
            msg += err_blob ? (const char*)err_blob->GetBufferPointer() : "?";
            throw std::runtime_error(msg);
        }
        check_hr(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_),
            "CreateVertexShader");
        check_hr(device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps_),
            "CreatePixelShader");

        D3D11_SAMPLER_DESC samp{};
        // point sampling: target texture size == swapchain size, so this is an exact
        // 1:1 texel copy -- important so verify_readback() pixel comparisons aren't
        // fuzzed by bilinear blending.
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samp.MaxLOD = D3D11_FLOAT32_MAX;
        check_hr(device_->CreateSamplerState(&samp, &sampler_), "CreateSamplerState");
    }

    void create_target_texture()
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width_;
        td.Height = height_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device_->CreateTexture2D(&td, nullptr, &target_tex_);
        check_hr(hr, "CreateTexture2D(target_tex_)");
        hr = device_->CreateShaderResourceView(target_tex_.Get(), nullptr, &target_srv_);
        check_hr(hr, "CreateShaderResourceView(target_tex_)");
    }

    void init_cuda_and_register()
    {
        check_cu(cuInit(0), "cuInit");
        check_cu(cuDeviceGet(&cu_dev_, 0), "cuDeviceGet");

        // Validate that the D3D11 adapter we're rendering on is the SAME physical GPU as
        // CUDA device 0. On this (single-4080) machine these must match; on a multi-GPU
        // machine a real implementation would pick the CUDA device that matches the D3D
        // adapter instead of hardcoding device 0.
        CUdevice dev_from_adapter = -1;
        CUresult r = cuD3D11GetDevice(&dev_from_adapter, adapter_.Get());
        if (r != CUDA_SUCCESS || dev_from_adapter != cu_dev_) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "cuD3D11GetDevice mismatch: D3D adapter maps to CUDA device %d, expected %d (CUresult=%d)",
                (int)dev_from_adapter, (int)cu_dev_, (int)r);
            throw std::runtime_error(buf);
        }

        // Retain device 0's PRIMARY context -- the same context torch's CUDA runtime API
        // calls lazily create/use for device 0. Primary contexts are refcounted per
        // process+device singletons, so this is safe regardless of whether torch has
        // already touched CUDA or not.
        check_cu(cuDevicePrimaryCtxRetain(&cu_ctx_, cu_dev_), "cuDevicePrimaryCtxRetain");
        check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent");

        // Register the AI target texture ONCE (not per frame -- registration cost is
        // explicitly excluded from the per-frame interop budget being measured).
        check_cu(cuGraphicsD3D11RegisterResource(&cu_res_, target_tex_.Get(), CU_GRAPHICS_REGISTER_FLAGS_NONE),
            "cuGraphicsD3D11RegisterResource");
    }

    static double qpc_delta_ms(LARGE_INTEGER a, LARGE_INTEGER b)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    }

    static int64_t qpc_to_ns(LARGE_INTEGER c)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        double ns_per_tick = 1e9 / static_cast<double>(freq.QuadPart);
        return static_cast<int64_t>(static_cast<double>(c.QuadPart) * ns_per_tick + 0.5);
    }

    int width_ = 0;
    int height_ = 0;
    HWND hwnd_ = nullptr;
    bool timer_period_set_ = false;
    bool closed_ = false;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIAdapter> adapter_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> sampler_;

    ComPtr<ID3D11Texture2D> target_tex_;
    ComPtr<ID3D11ShaderResourceView> target_srv_;
    ComPtr<ID3D11Texture2D> verify_staging_;
    bool captured_ = false;

    CUdevice cu_dev_ = 0;
    CUcontext cu_ctx_ = nullptr;
    CUgraphicsResource cu_res_ = nullptr;

    uint64_t frame_count_ = 0;
    std::vector<int64_t> present_qpc_ns_;
    std::vector<double> sync_ms_;
    std::vector<double> interop_ms_;
    std::vector<double> frame_ms_;
};

PYBIND11_MODULE(sumu_present, m)
{
    m.doc() = "sumu spike 1: torch CUDA tensor -> D3D11 present face, zero main-memory round trip";

    py::class_<Presenter>(m, "Presenter")
        .def(py::init<int, int, bool>(), py::arg("width"), py::arg("height"), py::arg("maximized") = false)
        .def("push_cuda_frame", &Presenter::push_cuda_frame,
            py::arg("dev_ptr"), py::arg("width"), py::arg("height"), py::arg("pitch_bytes"),
            py::arg("capture") = false)
        .def("verify_readback", &Presenter::verify_readback)
        .def("stats", &Presenter::stats)
        .def("present_trace_dump", &Presenter::present_trace_dump, py::arg("path"))
        .def("pump_messages", &Presenter::pump_messages)
        .def("should_quit", &Presenter::should_quit)
        .def("close", &Presenter::close);
}
