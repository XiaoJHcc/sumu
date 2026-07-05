// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Spike 3: prove that a decoder-produced NV12 D3D11 texture (one array slice of the
// decoder's own texture-array pool, see native/src/decoder.h) can be brought into CUDA and
// wrapped as a torch tensor with ZERO main-memory round trip -- the reverse direction of
// spike 1's CUDA->D3D11 bridge.
//
// The hard blocker documented in cudaD3D11.h's cuGraphicsD3D11RegisterResource() comment:
// only single-plane 1/2/4-channel 8/16/32-bit formats may be registered with CUDA. DXGI_
// FORMAT_NV12 (a bi-planar YUV 4:2:0 format) is NOT in that list, so the decoder's own NV12
// texture can never be registered directly. The established workaround (already used
// elsewhere in this repo, see spike0/spike2 decoder.cpp / presenter.cpp comments) is that
// D3D11 lets you create a Shader Resource View directly on an NV12 texture's luma plane
// (format override DXGI_FORMAT_R8_UNORM, full WxH) or chroma plane (DXGI_FORMAT_R8G8_UNORM,
// half-res W/2xH/2) -- both ARE CUDA-interop-supported formats. So:
//   1. per decoded frame: two fullscreen-triangle draws sample those SRVs and write straight
//      (point sampling, 1:1 texel copy, a GPU-side "identity blit") into two PERSISTENT plain
//      (non-array, non-planar) render targets: y_tex_ (R8_UNORM, WxH) and uv_tex_ (R8G8_
//      UNORM, W/2xH/2). Both bound D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE.
//   2. those two plain textures are registered with CUDA ONCE at construction (registration
//      is expensive, explicitly excluded from the per-frame budget, same policy as spike 1).
//   3. per frame: cuGraphicsMapResources -> cuGraphicsSubResourceGetMappedArray (one CUarray
//      per plane) -> cuMemcpy2D(ARRAY -> DEVICE) into a caller-supplied CUDA device buffer,
//      at the row offsets that reproduce PyAV's/lada's nv12 stacked layout: rows [0,h) = luma,
//      rows [h, h*3/2) = interleaved chroma (each row w bytes, i.e. w/2 UV pixel pairs) --
//      -> cuGraphicsUnmapResources. Both memcpy sides are GPU memory (ARRAY src, DEVICE dst):
//      no cuMemcpyDtoH/HtoD anywhere in this file, matching spike 1's zero-copy proof style.
//   4. The caller-supplied buffer is just a torch.empty((h*3//2, w), dtype=uint8, device=cuda)
//      tensor's own data_ptr() -- so the whole bridge writes straight into a tensor python
//      already owns (mirrors spike 1's push_cuda_frame(dev_ptr, ...) convention exactly,
//      just with the copy direction reversed), avoiding any need for __cuda_array_interface__/
//      DLPack plumbing.
//
// No window/swapchain here (pure decode->CUDA compute bridge, nothing is presented), so
// unlike spikes 0/1/2 there's no Win32 window boilerplate.

#ifndef NOMINMAX
#define NOMINMAX // windows.h's min/max macros otherwise break std::min/std::max call sites below
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cuda.h>
#include <cudaD3D11.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoder.h"

namespace py = pybind11;
using Microsoft::WRL::ComPtr;

namespace {

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

// Two fullscreen-triangle passthrough shaders: PSMain_Y samples a single-channel (luma) SRV
// and writes it straight to an R8_UNORM render target; PSMain_UV samples a two-channel
// (chroma) SRV and writes it straight to an R8G8_UNORM render target. Point sampling, exact
// 1:1 texel correspondence -- this is a GPU-side identity copy, not a real colour operation.
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

float PSMain_Y(VSOut i) : SV_Target
{
    return tex.Sample(samp0, i.uv).r;
}

float2 PSMain_UV(VSOut i) : SV_Target
{
    return tex.Sample(samp0, i.uv).rg;
}
)HLSL";

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
        d["n"] = 0; d["median_ms"] = 0.0; d["p99_ms"] = 0.0; d["max_ms"] = 0.0;
        d["min_ms"] = 0.0; d["mean_ms"] = 0.0;
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

double qpc_delta_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

} // namespace

// ------------------------------------------------------------------------------------------
class Interop
{
public:
    Interop() = default;
    ~Interop() { close(); }

    void open(const std::string& path)
    {
        create_device();

        std::string error;
        if (!decoder_.open(path, device_.Get(), error))
            throw std::runtime_error("Decoder::open failed: " + error);

        width_ = decoder_.width();
        height_ = decoder_.height();
        if (width_ <= 0 || height_ <= 0 || (width_ % 2) != 0 || (height_ % 2) != 0)
            throw std::runtime_error("Interop: unsupported odd/zero dimensions for NV12 4:2:0");

        create_shader_pipeline();
        // NOTE: plane render targets + CUDA registration are created LAZILY on the first
        // next_frame_into() call, once we know the decoder's REAL underlying texture size
        // (see the crop-padding bug this fixed, documented at plane_targets_created_ below).
        opened_ = true;
    }

    void close()
    {
        if (closed_) return;
        closed_ = true;
        if (cu_res_y_) { cuGraphicsUnregisterResource(cu_res_y_); cu_res_y_ = nullptr; }
        if (cu_res_uv_) { cuGraphicsUnregisterResource(cu_res_uv_); cu_res_uv_ = nullptr; }
        if (cu_ctx_) {
            cuCtxSetCurrent(nullptr);
            cuDevicePrimaryCtxRelease(cu_dev_);
            cu_ctx_ = nullptr;
        }
        decoder_.close();
    }

    int width() const { return width_; }
    int height() const { return height_; }
    double fps() const { return decoder_.fps(); }
    int64_t frame_count() const { return decoder_.frame_count(); }

    // Decodes one frame and writes it, GPU-side only, into the caller's CUDA buffer at
    // dev_ptr as a (height()*3/2, width()) uint8 NV12-stacked layout (row pitch = pitch_bytes
    // for every row, including the chroma rows -- matches lada's `_nv12_to_bgr_hwc_gpu` input
    // contract exactly, see docs/spike3_nv12_interop.md). Returns per-frame metadata + a
    // stage-by-stage timing breakdown (all in this call only -- accumulated into stats() too).
    py::dict next_frame_into(uint64_t dev_ptr, size_t pitch_bytes)
    {
        if (!opened_) throw std::runtime_error("Interop: open() not called");
        if (pitch_bytes < static_cast<size_t>(width_))
            throw std::runtime_error("next_frame_into: pitch_bytes smaller than width");

        LARGE_INTEGER t0, t1, t2, t3, t4, t5, t6;
        QueryPerformanceCounter(&t0);

        DecodedFrame df;
        if (!decoder_.next_frame(df))
            throw std::runtime_error("Decoder::next_frame failed (stream ended without loop?)");

        if (!plane_targets_created_) {
            // Hardware decoders (d3d11va) allocate their frame-pool texture at the CODED size,
            // which for heights not a multiple of 16 (e.g. 1080 -> padded to 1088) is LARGER
            // than the stream's visible/display size (width_/height_ from codecpar). The
            // identity-blit plane targets must be sized to match this REAL texture size --
            // otherwise a fullscreen-triangle blit (UV [0,1] over the full source extent, into
            // a viewport sized to the smaller visible height) silently vertically STRETCHES the
            // image by (real_height/height_), corrupting every row below roughly the point
            // where that sub-pixel drift becomes visible against scene content. Discovered via
            // a raw NV12-plane MAE cross-check against PyAV during spike bring-up (mean_MAE
            // ~8.5 on frame 0 alone, error onset partway down the frame, before this fix).
            D3D11_TEXTURE2D_DESC real_desc{};
            df.texture->GetDesc(&real_desc);
            real_width_ = static_cast<int>(real_desc.Width);
            real_height_ = static_cast<int>(real_desc.Height);
            if (real_width_ < width_ || real_height_ < height_)
                throw std::runtime_error("Interop: decoder texture smaller than reported display size");
            create_plane_targets();
            init_cuda_and_register();
            plane_targets_created_ = true;
        }

        QueryPerformanceCounter(&t1);

        auto srvs = get_or_create_plane_srvs(df.texture, df.array_slice);

        QueryPerformanceCounter(&t2);

        // ---- render passes: sample decoder's NV12 plane views -> our plain plane targets ---
        // Viewports use the REAL (possibly macroblock-padded) texture size, matching the plane
        // render targets -- a true 1:1 texel identity blit, no stretch. The CUDA copy below is
        // what crops down to the visible width_/height_ (assumes top-left-anchored crop, i.e.
        // any padding is added at the bottom/right, which is how d3d11va/DXVA pools pad).
        D3D11_VIEWPORT vp_y{ 0.0f, 0.0f, static_cast<float>(real_width_), static_cast<float>(real_height_), 0.0f, 1.0f };
        context_->RSSetViewports(1, &vp_y);
        ID3D11RenderTargetView* rtv_y[] = { y_rtv_.Get() };
        context_->OMSetRenderTargets(1, rtv_y, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_y_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv_y[] = { srvs.first.Get() };
        context_->PSSetShaderResources(0, 1, srv_y);
        ID3D11SamplerState* samplers[] = { sampler_.Get() };
        context_->PSSetSamplers(0, 1, samplers);
        context_->Draw(3, 0);

        D3D11_VIEWPORT vp_uv{ 0.0f, 0.0f, static_cast<float>(real_width_ / 2), static_cast<float>(real_height_ / 2), 0.0f, 1.0f };
        context_->RSSetViewports(1, &vp_uv);
        ID3D11RenderTargetView* rtv_uv[] = { uv_rtv_.Get() };
        context_->OMSetRenderTargets(1, rtv_uv, nullptr);
        context_->PSSetShader(ps_uv_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv_uv[] = { srvs.second.Get() };
        context_->PSSetShaderResources(0, 1, srv_uv);
        context_->Draw(3, 0);

        context_->Flush();

        QueryPerformanceCounter(&t3);

        // ---- CUDA interop: map both plane targets, copy ARRAY -> DEVICE, unmap ------------
        CUgraphicsResource res[2] = { cu_res_y_, cu_res_uv_ };
        check_cu(cuGraphicsMapResources(2, res, 0), "cuGraphicsMapResources");

        QueryPerformanceCounter(&t4);

        CUarray cu_arr_y = nullptr, cu_arr_uv = nullptr;
        check_cu(cuGraphicsSubResourceGetMappedArray(&cu_arr_y, cu_res_y_, 0, 0),
            "cuGraphicsSubResourceGetMappedArray(y)");
        check_cu(cuGraphicsSubResourceGetMappedArray(&cu_arr_uv, cu_res_uv_, 0, 0),
            "cuGraphicsSubResourceGetMappedArray(uv)");

        CUDA_MEMCPY2D cp_y{};
        cp_y.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        cp_y.srcArray = cu_arr_y;
        cp_y.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cp_y.dstDevice = static_cast<CUdeviceptr>(dev_ptr);
        cp_y.dstPitch = pitch_bytes;
        cp_y.WidthInBytes = static_cast<size_t>(width_); // R8_UNORM: 1 byte/texel
        cp_y.Height = static_cast<size_t>(height_);
        check_cu(cuMemcpy2D(&cp_y), "cuMemcpy2D (Y: array -> device)");

        CUDA_MEMCPY2D cp_uv{};
        cp_uv.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        cp_uv.srcArray = cu_arr_uv;
        cp_uv.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cp_uv.dstDevice = static_cast<CUdeviceptr>(dev_ptr) + static_cast<size_t>(height_) * pitch_bytes;
        cp_uv.dstPitch = pitch_bytes;
        cp_uv.WidthInBytes = static_cast<size_t>(width_); // R8G8_UNORM: 2 bytes/texel * (width_/2) texels/row = width_ bytes
        cp_uv.Height = static_cast<size_t>(height_) / 2;
        check_cu(cuMemcpy2D(&cp_uv), "cuMemcpy2D (UV: array -> device)");

        QueryPerformanceCounter(&t5);

        check_cu(cuGraphicsUnmapResources(2, res, 0), "cuGraphicsUnmapResources");

        QueryPerformanceCounter(&t6);

        decode_ms_.push_back(qpc_delta_ms(t0, t1));
        srv_ms_.push_back(qpc_delta_ms(t1, t2));
        render_ms_.push_back(qpc_delta_ms(t2, t3));
        map_ms_.push_back(qpc_delta_ms(t3, t4));
        copy_ms_.push_back(qpc_delta_ms(t4, t5));
        unmap_ms_.push_back(qpc_delta_ms(t5, t6));
        total_ms_.push_back(qpc_delta_ms(t0, t6));
        ++frame_count_done_;

        py::dict d;
        d["pts_seconds"] = df.pts_seconds;
        d["width"] = width_;
        d["height"] = height_;
        d["decode_ms"] = qpc_delta_ms(t0, t1);
        d["srv_ms"] = qpc_delta_ms(t1, t2);
        d["render_ms"] = qpc_delta_ms(t2, t3);
        d["map_ms"] = qpc_delta_ms(t3, t4);
        d["copy_ms"] = qpc_delta_ms(t4, t5);
        d["unmap_ms"] = qpc_delta_ms(t5, t6);
        d["total_ms"] = qpc_delta_ms(t0, t6);
        return d;
    }

    py::dict stats()
    {
        py::dict d;
        d["frame_count"] = frame_count_done_;
        d["decode_ms"] = summarize(decode_ms_);
        d["srv_ms"] = summarize(srv_ms_);
        d["render_ms"] = summarize(render_ms_);
        d["map_ms"] = summarize(map_ms_);
        d["copy_ms"] = summarize(copy_ms_);
        d["unmap_ms"] = summarize(unmap_ms_);
        d["total_ms"] = summarize(total_ms_);
        return d;
    }

private:
    void create_device()
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
    }

    void create_shader_pipeline()
    {
        ComPtr<ID3DBlob> vs_blob, ps_y_blob, ps_uv_blob, err_blob;
        HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "spike3_nv12.hlsl", nullptr, nullptr,
            "VSMain", "vs_5_0", 0, 0, &vs_blob, &err_blob);
        if (FAILED(hr)) throw std::runtime_error("VS compile failed: " +
            std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));

        hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "spike3_nv12.hlsl", nullptr, nullptr,
            "PSMain_Y", "ps_5_0", 0, 0, &ps_y_blob, &err_blob);
        if (FAILED(hr)) throw std::runtime_error("PS(Y) compile failed: " +
            std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));

        hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "spike3_nv12.hlsl", nullptr, nullptr,
            "PSMain_UV", "ps_5_0", 0, 0, &ps_uv_blob, &err_blob);
        if (FAILED(hr)) throw std::runtime_error("PS(UV) compile failed: " +
            std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));

        check_hr(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_),
            "CreateVertexShader");
        check_hr(device_->CreatePixelShader(ps_y_blob->GetBufferPointer(), ps_y_blob->GetBufferSize(), nullptr, &ps_y_),
            "CreatePixelShader(Y)");
        check_hr(device_->CreatePixelShader(ps_uv_blob->GetBufferPointer(), ps_uv_blob->GetBufferSize(), nullptr, &ps_uv_),
            "CreatePixelShader(UV)");

        D3D11_SAMPLER_DESC samp{};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samp.MaxLOD = D3D11_FLOAT32_MAX;
        check_hr(device_->CreateSamplerState(&samp, &sampler_), "CreateSamplerState");
    }

    // Sized to real_width_/real_height_ (the decoder's actual, possibly padded, texture size)
    // -- NOT width_/height_ (the stream's visible/display size) -- see the comment at the
    // plane_targets_created_ lazy-init call site for why.
    void create_plane_targets()
    {
        D3D11_TEXTURE2D_DESC yd{};
        yd.Width = real_width_;
        yd.Height = real_height_;
        yd.MipLevels = 1;
        yd.ArraySize = 1;
        yd.Format = DXGI_FORMAT_R8_UNORM;
        yd.SampleDesc.Count = 1;
        yd.Usage = D3D11_USAGE_DEFAULT;
        yd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check_hr(device_->CreateTexture2D(&yd, nullptr, &y_tex_), "CreateTexture2D(y_tex_)");
        check_hr(device_->CreateRenderTargetView(y_tex_.Get(), nullptr, &y_rtv_), "CreateRTV(y)");

        D3D11_TEXTURE2D_DESC uvd{};
        uvd.Width = real_width_ / 2;
        uvd.Height = real_height_ / 2;
        uvd.MipLevels = 1;
        uvd.ArraySize = 1;
        uvd.Format = DXGI_FORMAT_R8G8_UNORM;
        uvd.SampleDesc.Count = 1;
        uvd.Usage = D3D11_USAGE_DEFAULT;
        uvd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check_hr(device_->CreateTexture2D(&uvd, nullptr, &uv_tex_), "CreateTexture2D(uv_tex_)");
        check_hr(device_->CreateRenderTargetView(uv_tex_.Get(), nullptr, &uv_rtv_), "CreateRTV(uv)");
    }

    // Cached per (source texture pointer, array slice): decoder's pool has a small bounded
    // number of distinct slices, and the SRVs only need to exist once per slice (the VIEW is
    // stable even though the pixel CONTENTS the decoder writes into that slice change every
    // time the slot is reused) -- avoids CreateShaderResourceView (a driver call) every frame.
    std::pair<ComPtr<ID3D11ShaderResourceView>, ComPtr<ID3D11ShaderResourceView>>
    get_or_create_plane_srvs(ID3D11Texture2D* tex, UINT array_slice)
    {
        auto key = std::make_pair(tex, array_slice);
        auto it = srv_cache_.find(key);
        if (it != srv_cache_.end()) return it->second;

        D3D11_SHADER_RESOURCE_VIEW_DESC yd{};
        yd.Format = DXGI_FORMAT_R8_UNORM;
        yd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        yd.Texture2DArray.MostDetailedMip = 0;
        yd.Texture2DArray.MipLevels = 1;
        yd.Texture2DArray.FirstArraySlice = array_slice;
        yd.Texture2DArray.ArraySize = 1;
        ComPtr<ID3D11ShaderResourceView> srv_y;
        check_hr(device_->CreateShaderResourceView(tex, &yd, &srv_y), "CreateSRV(src Y)");

        D3D11_SHADER_RESOURCE_VIEW_DESC uvd = yd;
        uvd.Format = DXGI_FORMAT_R8G8_UNORM;
        ComPtr<ID3D11ShaderResourceView> srv_uv;
        check_hr(device_->CreateShaderResourceView(tex, &uvd, &srv_uv), "CreateSRV(src UV)");

        auto pr = std::make_pair(srv_y, srv_uv);
        srv_cache_[key] = pr;
        return pr;
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

        // Retain device 0's PRIMARY context -- same one torch's CUDA runtime API lazily
        // creates/uses (see spike1's presenter.cpp for the full rationale).
        check_cu(cuDevicePrimaryCtxRetain(&cu_ctx_, cu_dev_), "cuDevicePrimaryCtxRetain");
        check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent");

        check_cu(cuGraphicsD3D11RegisterResource(&cu_res_y_, y_tex_.Get(), CU_GRAPHICS_REGISTER_FLAGS_NONE),
            "cuGraphicsD3D11RegisterResource(y_tex_)");
        check_cu(cuGraphicsD3D11RegisterResource(&cu_res_uv_, uv_tex_.Get(), CU_GRAPHICS_REGISTER_FLAGS_NONE),
            "cuGraphicsD3D11RegisterResource(uv_tex_)");
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIAdapter> adapter_;

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_y_;
    ComPtr<ID3D11PixelShader> ps_uv_;
    ComPtr<ID3D11SamplerState> sampler_;

    ComPtr<ID3D11Texture2D> y_tex_;
    ComPtr<ID3D11RenderTargetView> y_rtv_;
    ComPtr<ID3D11Texture2D> uv_tex_;
    ComPtr<ID3D11RenderTargetView> uv_rtv_;

    std::map<std::pair<ID3D11Texture2D*, UINT>,
        std::pair<ComPtr<ID3D11ShaderResourceView>, ComPtr<ID3D11ShaderResourceView>>> srv_cache_;

    Decoder decoder_;
    int width_ = 0;
    int height_ = 0;
    int real_width_ = 0;
    int real_height_ = 0;
    bool opened_ = false;
    bool closed_ = false;
    bool plane_targets_created_ = false;

    CUdevice cu_dev_ = 0;
    CUcontext cu_ctx_ = nullptr;
    CUgraphicsResource cu_res_y_ = nullptr;
    CUgraphicsResource cu_res_uv_ = nullptr;

    uint64_t frame_count_done_ = 0;
    std::vector<double> decode_ms_, srv_ms_, render_ms_, map_ms_, copy_ms_, unmap_ms_, total_ms_;
};

PYBIND11_MODULE(sumu_nv12interop, m)
{
    m.doc() = "sumu spike 3: D3D11 decoded NV12 texture -> CUDA -> torch tensor, zero main-memory round trip";

    py::class_<Interop>(m, "Interop")
        .def(py::init<>())
        .def("open", &Interop::open, py::arg("path"))
        .def("width", &Interop::width)
        .def("height", &Interop::height)
        .def("fps", &Interop::fps)
        .def("frame_count", &Interop::frame_count)
        .def("next_frame_into", &Interop::next_frame_into, py::arg("dev_ptr"), py::arg("pitch_bytes"))
        .def("stats", &Interop::stats)
        .def("close", &Interop::close);
}
