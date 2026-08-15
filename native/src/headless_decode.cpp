// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// HeadlessDecode: a windowless, sequential d3d11va-hard-decode -> CUDA-NV12 source for the
// transcode / web-streaming / offline-export pipeline (python/sumu/webstream/). Reuses the
// exact Decoder (decoder.cpp, unchanged) plus the AI-input-bridge blit technique already
// validated in player.cpp (create_ai_input_bridge + get_cuda_nv12_by_frame, see
// docs/native_ai_input.md), WITHOUT any window / swapchain / present thread / audio machinery.
//
// The model is deliberately different from Player::get_cuda_nv12_by_frame's frame-number-indexed,
// non-blocking ready-map: export/streaming consumes frames strictly in decode order, as fast as
// the GPU allows, so the API is a blocking "give me the next decoded frame as a CUDA NV12 buffer,
// or report EOF". The returned CUDA buffer is a SINGLE persistent, tightly-packed stacked-NV12
// buffer (pitch == width) reused on every call -- the caller must consume or clone it before the
// next call (same single-buffer contract as get_cuda_nv12_by_frame).
//
// Audio is intentionally NOT handled here: the encoder (ffmpeg.exe) reads the source file's /
// URL's audio track directly as a second input. This keeps the native surface video-only.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cuda.h>
#include <cudaD3D11.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "decoder.h"
#include "headless_decode.h"

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

// Copied verbatim from player.cpp (kAiInputBlitShaderSrc): identity blit of a single NV12
// texture's Y plane (.r) and interleaved UV plane (.rg) into R8 / R8G8 render targets, which
// are then CUDA-registered and copied out as a stacked NV12 device buffer.
const char* kAiInputBlitShaderSrc = R"HLSL(
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

class HeadlessDecode
{
public:
    HeadlessDecode()
    {
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &got, &context_);
        check_hr(hr, "D3D11CreateDevice");

        // CUDA primary context: the SAME primary context torch uses (torch's CUDA runtime also
        // retains it), so the returned dev_ptr is directly wrap-able by torch's DLPack / usable
        // by the AI kernels -- no cross-context copy. Mirrors player.cpp's open() init.
        check_cu(cuInit(0), "cuInit");
        CUdevice dev;
        check_cu(cuDeviceGet(&dev, 0), "cuDeviceGet");
        check_cu(cuDevicePrimaryCtxRetain(&cu_ctx_, dev), "cuDevicePrimaryCtxRetain");
        check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent");

        compile_blit_shaders();
    }

    ~HeadlessDecode() { close(); }

    void open(const std::string& path)
    {
        close(); // idempotent reset (mirrors Decoder::open)
        std::string err;
        if (!decoder_.open(path, device_.Get(), err))
            throw std::runtime_error("HeadlessDecode.open: " + err);

        width_ = decoder_.width();
        height_ = decoder_.height();
        fps_ = decoder_.fps();
        frame_count_ = decoder_.frame_count();
        if (width_ <= 0 || height_ <= 0)
            throw std::runtime_error("HeadlessDecode.open: zero/invalid video dimensions");

        create_bridge_resources();
        opened_ = true;
    }

    // Decode the next frame and return it as a CUDA-resident, tightly-packed stacked-NV12
    // buffer. Returns a dict {eof: bool, dev_ptr, width, height, pitch_bytes, frame_num,
    // pts_seconds}. eof=True means no more frames (dev_ptr/width/... absent). Blocks while
    // FFmpeg decodes (local files: ~ms/frame; network URLs may block on IO).
    py::dict next_frame()
    {
        py::dict d;
        if (!opened_) throw std::runtime_error("HeadlessDecode.next_frame: not opened");

        DecodedFrame df;
        if (!decoder_.next_frame(df)) {
            d["eof"] = true;
            return d;
        }

        // Decoder texture -> display-size NV12 intermediate (same nullptr-box full copy the
        // validated decode_loop/scrub blit use), then identity-blit into the CUDA-registered
        // R8/R8G8 plane targets and copy out to the persistent device buffer.
        context_->CopySubresourceRegion(nv12_tex_.Get(), 0, 0, 0, 0,
            df.texture, df.array_slice, nullptr);

        D3D11_VIEWPORT vp_y{ 0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f };
        context_->RSSetViewports(1, &vp_y);
        ID3D11RenderTargetView* rtv_y[] = { y_rtv_.Get() };
        context_->OMSetRenderTargets(1, rtv_y, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_y_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv_y[] = { srv_y_.Get() };
        context_->PSSetShaderResources(0, 1, srv_y);
        ID3D11SamplerState* samplers[] = { sampler_.Get() };
        context_->PSSetSamplers(0, 1, samplers);
        context_->Draw(3, 0);

        D3D11_VIEWPORT vp_uv{ 0.0f, 0.0f, static_cast<float>(width_ / 2), static_cast<float>(height_ / 2), 0.0f, 1.0f };
        context_->RSSetViewports(1, &vp_uv);
        ID3D11RenderTargetView* rtv_uv[] = { uv_rtv_.Get() };
        context_->OMSetRenderTargets(1, rtv_uv, nullptr);
        context_->PSSetShader(ps_uv_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv_uv[] = { srv_uv_.Get() };
        context_->PSSetShaderResources(0, 1, srv_uv);
        context_->Draw(3, 0);

        context_->Flush();

        check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent (next_frame)");
        CUgraphicsResource res[2] = { cu_res_y_, cu_res_uv_ };
        check_cu(cuGraphicsMapResources(2, res, 0), "cuGraphicsMapResources");

        CUarray cu_arr_y = nullptr, cu_arr_uv = nullptr;
        check_cu(cuGraphicsSubResourceGetMappedArray(&cu_arr_y, cu_res_y_, 0, 0),
            "cuGraphicsSubResourceGetMappedArray(y)");
        check_cu(cuGraphicsSubResourceGetMappedArray(&cu_arr_uv, cu_res_uv_, 0, 0),
            "cuGraphicsSubResourceGetMappedArray(uv)");

        CUDA_MEMCPY2D cp_y{};
        cp_y.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        cp_y.srcArray = cu_arr_y;
        cp_y.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cp_y.dstDevice = cu_buf_;
        cp_y.dstPitch = static_cast<size_t>(width_);
        cp_y.WidthInBytes = static_cast<size_t>(width_);
        cp_y.Height = static_cast<size_t>(height_);
        check_cu(cuMemcpy2D(&cp_y), "cuMemcpy2D(Y: array -> device)");

        CUDA_MEMCPY2D cp_uv{};
        cp_uv.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        cp_uv.srcArray = cu_arr_uv;
        cp_uv.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cp_uv.dstDevice = cu_buf_ + static_cast<size_t>(height_) * static_cast<size_t>(width_);
        cp_uv.dstPitch = static_cast<size_t>(width_);
        cp_uv.WidthInBytes = static_cast<size_t>(width_);
        cp_uv.Height = static_cast<size_t>(height_ / 2);
        check_cu(cuMemcpy2D(&cp_uv), "cuMemcpy2D(UV: array -> device)");

        check_cu(cuGraphicsUnmapResources(2, res, 0), "cuGraphicsUnmapResources");

        d["eof"] = false;
        d["dev_ptr"] = static_cast<uint64_t>(cu_buf_);
        d["width"] = width_;
        d["height"] = height_;
        d["pitch_bytes"] = static_cast<size_t>(width_);
        d["frame_num"] = static_cast<int64_t>(std::llround(df.pts_seconds * fps_));
        d["pts_seconds"] = df.pts_seconds;
        return d;
    }

    // I6: seek = reposition, not teardown. Re-anchors sequential decode at `target_frame`
    // (nearest keyframe + forward decode to the exact real-PTS frame, see Decoder::seek_to_frame)
    // and returns the ACTUAL landed frame number (round(pts_seconds * fps)) so the caller can
    // align its emit frontier to real decoded PTS. Throws on failure.
    int64_t seek_to_frame(int64_t target_frame)
    {
        if (!opened_) throw std::runtime_error("HeadlessDecode.seek_to_frame: not opened");
        // Decoder::seek_to_frame requires the PTS origin established by a first decode (its
        // "no PTS origin yet" guard). A transcode consumer seeks right after open() without any
        // prior next_frame(), so decode (and discard) one frame to establish that origin first.
        if (!decoder_.have_first_pts()) {
            DecodedFrame warmup;
            if (!decoder_.next_frame(warmup))
                throw std::runtime_error("HeadlessDecode.seek_to_frame: no first frame to decode");
        }
        DecodedFrame df;
        std::string err;
        if (!decoder_.seek_to_frame(target_frame, df, err))
            throw std::runtime_error("HeadlessDecode.seek_to_frame: " + err);
        return static_cast<int64_t>(std::llround(df.pts_seconds * fps_));
    }

    double fps() const { return fps_; }
    int64_t frame_count() const { return frame_count_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool is_network() const { return decoder_.is_network(); }

    void close()
    {
        if (cu_res_y_) { cuGraphicsUnregisterResource(cu_res_y_); cu_res_y_ = nullptr; }
        if (cu_res_uv_) { cuGraphicsUnregisterResource(cu_res_uv_); cu_res_uv_ = nullptr; }
        if (cu_buf_) { cuMemFree(cu_buf_); cu_buf_ = 0; }
        y_rtv_.Reset(); uv_rtv_.Reset();
        y_tex_.Reset(); uv_tex_.Reset();
        srv_y_.Reset(); srv_uv_.Reset();
        nv12_tex_.Reset();
        decoder_.close();
        width_ = 0; height_ = 0; fps_ = 60.0; frame_count_ = 0;
        opened_ = false;
    }

private:
    void compile_blit_shaders()
    {
        ComPtr<ID3DBlob> vs_blob, ps_y_blob, ps_uv_blob, err_blob;
        HRESULT hr = D3DCompile(kAiInputBlitShaderSrc, strlen(kAiInputBlitShaderSrc),
            "ai_input_blit.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs_blob, &err_blob);
        if (FAILED(hr)) throw std::runtime_error("blit VS compile failed: " +
            std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));
        hr = D3DCompile(kAiInputBlitShaderSrc, strlen(kAiInputBlitShaderSrc),
            "ai_input_blit.hlsl", nullptr, nullptr, "PSMain_Y", "ps_5_0", 0, 0, &ps_y_blob, &err_blob);
        if (FAILED(hr)) throw std::runtime_error("blit PS(Y) compile failed: " +
            std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));
        hr = D3DCompile(kAiInputBlitShaderSrc, strlen(kAiInputBlitShaderSrc),
            "ai_input_blit.hlsl", nullptr, nullptr, "PSMain_UV", "ps_5_0", 0, 0, &ps_uv_blob, &err_blob);
        if (FAILED(hr)) throw std::runtime_error("blit PS(UV) compile failed: " +
            std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));

        check_hr(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
            nullptr, &vs_), "CreateVertexShader(blit)");
        check_hr(device_->CreatePixelShader(ps_y_blob->GetBufferPointer(), ps_y_blob->GetBufferSize(),
            nullptr, &ps_y_), "CreatePixelShader(blit Y)");
        check_hr(device_->CreatePixelShader(ps_uv_blob->GetBufferPointer(), ps_uv_blob->GetBufferSize(),
            nullptr, &ps_uv_), "CreatePixelShader(blit UV)");

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        check_hr(device_->CreateSamplerState(&sd, &sampler_), "CreateSamplerState(blit)");
    }

    void create_bridge_resources()
    {
        // Display-size NV12 intermediate (single slice). Same NV12 recipe as player.cpp's
        // create_ring_resources(): BIND_DECODER | BIND_SHADER_RESOURCE so the driver accepts
        // the video format, then per-plane SRVs (R8 / R8G8).
        D3D11_TEXTURE2D_DESC nd{};
        nd.Width = static_cast<UINT>(width_);
        nd.Height = static_cast<UINT>(height_);
        nd.MipLevels = 1;
        nd.ArraySize = 1;
        nd.Format = DXGI_FORMAT_NV12;
        nd.SampleDesc.Count = 1;
        nd.Usage = D3D11_USAGE_DEFAULT;
        nd.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
        check_hr(device_->CreateTexture2D(&nd, nullptr, &nv12_tex_), "CreateTexture2D(nv12_tex_)");

        D3D11_SHADER_RESOURCE_VIEW_DESC yd{};
        yd.Format = DXGI_FORMAT_R8_UNORM;
        yd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        yd.Texture2D.MostDetailedMip = 0;
        yd.Texture2D.MipLevels = 1;
        check_hr(device_->CreateShaderResourceView(nv12_tex_.Get(), &yd, &srv_y_), "CreateSRV(nv12 Y)");

        D3D11_SHADER_RESOURCE_VIEW_DESC uvd = yd;
        uvd.Format = DXGI_FORMAT_R8G8_UNORM;
        check_hr(device_->CreateShaderResourceView(nv12_tex_.Get(), &uvd, &srv_uv_), "CreateSRV(nv12 UV)");

        // CUDA-registered plane targets (R8 Y, R8G8 UV) + persistent stacked-NV12 device buffer.
        D3D11_TEXTURE2D_DESC t{};
        t.Width = static_cast<UINT>(width_);
        t.Height = static_cast<UINT>(height_);
        t.MipLevels = 1;
        t.ArraySize = 1;
        t.Format = DXGI_FORMAT_R8_UNORM;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check_hr(device_->CreateTexture2D(&t, nullptr, &y_tex_), "CreateTexture2D(y_tex_)");
        check_hr(device_->CreateRenderTargetView(y_tex_.Get(), nullptr, &y_rtv_), "CreateRTV(y)");

        D3D11_TEXTURE2D_DESC ut = t;
        ut.Width = static_cast<UINT>(width_) / 2;
        ut.Height = static_cast<UINT>(height_) / 2;
        ut.Format = DXGI_FORMAT_R8G8_UNORM;
        check_hr(device_->CreateTexture2D(&ut, nullptr, &uv_tex_), "CreateTexture2D(uv_tex_)");
        check_hr(device_->CreateRenderTargetView(uv_tex_.Get(), nullptr, &uv_rtv_), "CreateRTV(uv)");

        check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent (create_bridge_resources)");
        check_cu(cuGraphicsD3D11RegisterResource(&cu_res_y_, y_tex_.Get(), CU_GRAPHICS_REGISTER_FLAGS_NONE),
            "cuGraphicsD3D11RegisterResource(y_tex_)");
        check_cu(cuGraphicsD3D11RegisterResource(&cu_res_uv_, uv_tex_.Get(), CU_GRAPHICS_REGISTER_FLAGS_NONE),
            "cuGraphicsD3D11RegisterResource(uv_tex_)");

        const size_t buf_size = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 3 / 2;
        check_cu(cuMemAlloc(&cu_buf_, buf_size), "cuMemAlloc(cu_buf_)");
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    CUcontext cu_ctx_ = nullptr;

    Decoder decoder_;

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_y_;
    ComPtr<ID3D11PixelShader> ps_uv_;
    ComPtr<ID3D11SamplerState> sampler_;

    ComPtr<ID3D11Texture2D> nv12_tex_;
    ComPtr<ID3D11ShaderResourceView> srv_y_;
    ComPtr<ID3D11ShaderResourceView> srv_uv_;

    ComPtr<ID3D11Texture2D> y_tex_;
    ComPtr<ID3D11RenderTargetView> y_rtv_;
    ComPtr<ID3D11Texture2D> uv_tex_;
    ComPtr<ID3D11RenderTargetView> uv_rtv_;
    CUgraphicsResource cu_res_y_ = nullptr;
    CUgraphicsResource cu_res_uv_ = nullptr;
    CUdeviceptr cu_buf_ = 0;

    int width_ = 0;
    int height_ = 0;
    double fps_ = 60.0;
    int64_t frame_count_ = 0;
    bool opened_ = false;
};

} // namespace

void init_headless_decode(py::module_& m)
{
    py::class_<HeadlessDecode>(m, "HeadlessDecode")
        .def(py::init<>())
        .def("open", &HeadlessDecode::open, py::arg("path"))
        .def("next_frame", &HeadlessDecode::next_frame)
        .def("seek_to_frame", &HeadlessDecode::seek_to_frame, py::arg("target_frame"))
        .def("fps", &HeadlessDecode::fps)
        .def("frame_count", &HeadlessDecode::frame_count)
        .def("width", &HeadlessDecode::width)
        .def("height", &HeadlessDecode::height)
        .def("is_network", &HeadlessDecode::is_network)
        .def("close", &HeadlessDecode::close);
}
