// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Spike 0: prove that native D3D11VA hardware decode + a self-driven present loop can
// play 4K60 HEVC with stable on-screen frame pacing, with ZERO AI in the loop and ZERO
// GPU->host->GPU round trips per frame. See DESIGN.md / spikes/README.md for the full
// contract this spike has to satisfy.
//
// Architecture, deliberately as simple as the contract allows ("先做最简单的"):
//   - One thread. No decode thread, no present thread -- decode-a-frame-then-present-it,
//     paced against the frame's own PTS with QueryPerformanceCounter, Present(1,0) for
//     the actual vsync wait/throttle.
//   - Present face: real flip-model D3D11 swapchain (DXGI_SWAP_EFFECT_FLIP_DISCARD) on a
//     plain Win32 window. No Qt, no GL.
//   - Decode: FFmpeg libavcodec/d3d11va, hw_device_ctx built from OUR ID3D11Device so the
//     decoded NV12 texture lives on the same device as the swapchain. We ask FFmpeg to
//     allocate the frame pool with D3D11_BIND_SHADER_RESOURCE (in addition to the
//     BIND_DECODER it always needs) so an SRV can be created directly on the decoder's
//     own texture array slice -- no copy, no readback.
//   - present timestamps recorded into an in-memory vector (QueryPerformanceCounter ->
//     ns), written to CSV once at the end (never during the hot loop).

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

#include "decoder.h"

using Microsoft::WRL::ComPtr;

namespace {

const char* kShaderSrc = R"HLSL(
Texture2DArray<float>  texY  : register(t0);
Texture2DArray<float2> texUV : register(t1);
SamplerState           samp0 : register(s0);

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

float4 PSMain(VSOut i) : SV_Target
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
)HLSL";

struct Args
{
    std::string video_path;
    double seconds = 50.0;
    bool maximized = false;
    std::string run = "default";
    std::string trace_dir = "d:/Git/sumu/spikes/spike0_d3d11_present/trace/";
};

bool parse_args(int argc, char** argv, Args& a, std::string& err)
{
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--seconds" && i + 1 < argc) {
            a.seconds = atof(argv[++i]);
        } else if (arg == "--maximized") {
            a.maximized = true;
        } else if (arg == "--run" && i + 1 < argc) {
            a.run = argv[++i];
        } else if (arg == "--trace-dir" && i + 1 < argc) {
            a.trace_dir = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            err = "unknown flag: " + arg;
            return false;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.empty()) {
        err = "missing required video path argument";
        return false;
    }
    a.video_path = positional[0];
    return true;
}

// ---- global state touched by WndProc -------------------------------------------------
bool g_quit = false;
bool g_resize_pending = false;
UINT g_pending_w = 0, g_pending_h = 0;

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
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_pending_w = LOWORD(lp);
            g_pending_h = HIWORD(lp);
            g_resize_pending = (g_pending_w > 0 && g_pending_h > 0);
        }
        return 0;
    case WM_SYSKEYDOWN:
        // swallow Alt+Enter; we don't implement an exclusive-fullscreen transition, a
        // maximized window is all this spike needs.
        if (wp == VK_RETURN) return 0;
        break;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int64_t now_ns(double ns_per_tick)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(static_cast<double>(c.QuadPart) * ns_per_tick + 0.5);
}

void wait_until_qpc(LARGE_INTEGER target, double ticks_per_ms)
{
    LARGE_INTEGER now;
    for (;;) {
        QueryPerformanceCounter(&now);
        double remaining_ms = static_cast<double>(target.QuadPart - now.QuadPart) / ticks_per_ms;
        if (remaining_ms <= 0.0) return;
        if (remaining_ms > 2.0) {
            Sleep(static_cast<DWORD>(remaining_ms - 1.0));
        } else {
            YieldProcessor();
        }
    }
}

struct SliceViews
{
    ComPtr<ID3D11ShaderResourceView> y;
    ComPtr<ID3D11ShaderResourceView> uv;
};

} // namespace

int main(int argc, char** argv)
{
    Args args;
    std::string err;
    if (!parse_args(argc, argv, args, err)) {
        fprintf(stderr, "spike0: %s\nusage: spike0.exe <video_path> [--seconds N] [--maximized] [--run name]\n", err.c_str());
        return 1;
    }

    // Windows' default system timer resolution is ~15.6ms (the 64Hz clock tick). Our
    // present-pacing loop below issues short Sleep() calls while waiting for a frame's
    // PTS deadline; without requesting a finer timer resolution those Sleep() calls
    // round up to that ~15.6ms grid, which shows up downstream as an every-Nth-frame
    // ~2x-budget present (the deficit gets paid back on the next frame since we pace
    // against an absolute clock, producing a "long frame, short frame" beat instead of
    // a single steady cadence). timeBeginPeriod(1) fixes this at the source.
    timeBeginPeriod(1);

    CreateDirectoryA(args.trace_dir.c_str(), nullptr);

    // ---- Win32 window -----------------------------------------------------------------
    HINSTANCE hinst = GetModuleHandle(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "Sumu_Spike0_WndClass";
    RegisterClassExA(&wc);

    RECT rc{0, 0, 1280, 720};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "sumu spike0 - native D3D11 present",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
    if (!hwnd) {
        fprintf(stderr, "CreateWindowEx failed: %lu\n", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, args.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd);

    // ---- D3D11 device + flip-model swapchain ------------------------------------------
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got_level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
        &device, &got_level, &context);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D11CreateDevice failed: 0x%08lx\n", hr);
        return 1;
    }

    // Defensive: make the immediate context safe even though this spike is single
    // threaded end-to-end (decode + render both issued from this same thread).
    {
        ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE);
    }

    ComPtr<IDXGIDevice> dxgi_device;
    device.As(&dxgi_device);
    ComPtr<IDXGIAdapter> adapter;
    dxgi_device->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    RECT client_rc;
    GetClientRect(hwnd, &client_rc);
    UINT width = std::max<LONG>(1, client_rc.right - client_rc.left);
    UINT height = std::max<LONG>(1, client_rc.bottom - client_rc.top);

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = width;
    scd.Height = height;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scd.Scaling = DXGI_SCALING_STRETCH;

    ComPtr<IDXGISwapChain1> swapchain;
    hr = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &scd, nullptr, nullptr, &swapchain);
    if (FAILED(hr)) {
        fprintf(stderr, "CreateSwapChainForHwnd failed: 0x%08lx\n", hr);
        return 1;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11RenderTargetView> rtv;
    auto create_rtv = [&]() {
        rtv.Reset();
        ComPtr<ID3D11Texture2D> backbuffer;
        swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
        device->CreateRenderTargetView(backbuffer.Get(), nullptr, &rtv);
    };
    create_rtv();

    // ---- shader / pipeline state --------------------------------------------------------
    ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;
    hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "nv12_to_rgb.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", 0, 0, &vs_blob, &err_blob);
    if (FAILED(hr)) {
        fprintf(stderr, "VS compile failed: %s\n", err_blob ? (const char*)err_blob->GetBufferPointer() : "?");
        return 1;
    }
    hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "nv12_to_rgb.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_0", 0, 0, &ps_blob, &err_blob);
    if (FAILED(hr)) {
        fprintf(stderr, "PS compile failed: %s\n", err_blob ? (const char*)err_blob->GetBufferPointer() : "?");
        return 1;
    }

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    device->CreateSamplerState(&sd, &sampler);

    struct SliceCB { UINT arraySlice; UINT pad[3]; };
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(SliceCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> slice_cb;
    device->CreateBuffer(&cbd, nullptr, &slice_cb);

    // ---- decoder -------------------------------------------------------------------------
    Decoder decoder;
    std::string decode_err;
    if (!decoder.open(args.video_path, device.Get(), decode_err)) {
        fprintf(stderr, "decoder.open failed: %s\n", decode_err.c_str());
        return 1;
    }
    fprintf(stderr, "spike0: opened %s, fps=%.3f, run=%s, seconds=%.1f, maximized=%d\n",
        args.video_path.c_str(), decoder.fps(), args.run.c_str(), args.seconds, (int)args.maximized);

    // SRV cache keyed by array slice; the decoder pool is one physical texture array so
    // the slice index is stable across the whole run (texture pointer doesn't change).
    std::vector<SliceViews> srv_cache;
    ID3D11Texture2D* cached_texture = nullptr;

    // ---- present trace (in-memory only; flushed to CSV at the very end) ------------------
    std::vector<int64_t> present_ns;
    present_ns.reserve(1u << 16);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double ticks_per_ms = static_cast<double>(freq.QuadPart) / 1000.0;
    const double ns_per_tick = 1e9 / static_cast<double>(freq.QuadPart);

    LARGE_INTEGER base_qpc;
    QueryPerformanceCounter(&base_qpc);
    bool have_base = false;

    LARGE_INTEGER run_start;
    QueryPerformanceCounter(&run_start);

    auto write_trace = [&]() {
        std::string path = args.trace_dir + "present_spike0_" + args.run + ".csv";
        std::ofstream f(path, std::ios::trunc);
        f << "qpc_ns\n";
        for (int64_t v : present_ns) f << v << "\n";
        fprintf(stderr, "spike0: wrote %zu present timestamps to %s\n", present_ns.size(), path.c_str());
    };

    MSG msg{};
    uint64_t frame_count = 0;
    while (!g_quit) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (g_quit) break;

        LARGE_INTEGER t_now;
        QueryPerformanceCounter(&t_now);
        double elapsed_s = static_cast<double>(t_now.QuadPart - run_start.QuadPart) / freq.QuadPart;
        if (elapsed_s >= args.seconds) {
            fprintf(stderr, "spike0: reached --seconds %.1f, stopping\n", args.seconds);
            break;
        }

        if (g_resize_pending) {
            rtv.Reset();
            swapchain->ResizeBuffers(0, g_pending_w, g_pending_h, DXGI_FORMAT_UNKNOWN, 0);
            create_rtv();
            width = g_pending_w;
            height = g_pending_h;
            g_resize_pending = false;
        }

        DecodedFrame df;
        if (!decoder.next_frame(df)) {
            fprintf(stderr, "spike0: decoder EOF/error with no more frames, stopping\n");
            break;
        }

        if (df.texture != cached_texture) {
            // pool re-created (shouldn't normally happen mid-run) -- drop stale views.
            srv_cache.clear();
            cached_texture = df.texture;
        }
        if (df.array_slice >= srv_cache.size())
            srv_cache.resize(df.array_slice + 1);
        SliceViews& views = srv_cache[df.array_slice];
        if (!views.y) {
            D3D11_SHADER_RESOURCE_VIEW_DESC yd{};
            yd.Format = DXGI_FORMAT_R8_UNORM;
            yd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            yd.Texture2DArray.MostDetailedMip = 0;
            yd.Texture2DArray.MipLevels = 1;
            yd.Texture2DArray.FirstArraySlice = df.array_slice;
            yd.Texture2DArray.ArraySize = 1;
            hr = device->CreateShaderResourceView(df.texture, &yd, &views.y);
            if (FAILED(hr)) fprintf(stderr, "CreateSRV(Y) failed: 0x%08lx\n", hr);

            D3D11_SHADER_RESOURCE_VIEW_DESC uvd = yd;
            uvd.Format = DXGI_FORMAT_R8G8_UNORM;
            hr = device->CreateShaderResourceView(df.texture, &uvd, &views.uv);
            if (FAILED(hr)) fprintf(stderr, "CreateSRV(UV) failed: 0x%08lx\n", hr);
        }

        // pace to the frame's own PTS -- this is what makes the loop clock-driven rather
        // than "present as fast as the 150Hz compositor will allow".
        if (!have_base) {
            QueryPerformanceCounter(&base_qpc);
            have_base = true;
        }
        LARGE_INTEGER target;
        target.QuadPart = base_qpc.QuadPart + static_cast<LONGLONG>(df.pts_seconds * freq.QuadPart);
        wait_until_qpc(target, ticks_per_ms);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(slice_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            SliceCB cb{ df.array_slice, {0,0,0} };
            memcpy(mapped.pData, &cb, sizeof(cb));
            context->Unmap(slice_cb.Get(), 0);
        }

        D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        context->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
        context->OMSetRenderTargets(1, rtvs, nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { views.y.Get(), views.uv.Get() };
        context->PSSetShaderResources(0, 2, srvs);
        ID3D11SamplerState* samplers[] = { sampler.Get() };
        context->PSSetSamplers(0, 1, samplers);
        ID3D11Buffer* cbs[] = { slice_cb.Get() };
        context->PSSetConstantBuffers(0, 1, cbs);
        context->Draw(3, 0);

        swapchain->Present(1, 0);
        present_ns.push_back(now_ns(ns_per_tick));
        ++frame_count;

        if ((frame_count % 300) == 0) {
            fprintf(stderr, "spike0: %llu frames presented, elapsed=%.1fs, pts=%.2fs\n",
                (unsigned long long)frame_count, elapsed_s, df.pts_seconds);
        }
    }

    write_trace();
    decoder.close();
    timeEndPeriod(1);
    return 0;
}
