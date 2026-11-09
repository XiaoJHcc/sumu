// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "video_normalizer.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

// SDR tone map for HDR sources: luminance-domain log shoulder over linear light expressed in
// SDR-white units (1.0 == kSdrWhiteNits). Identity below the knee (derivative ~1.0 there, the
// softness was picked for that), exactly 1.0 at kToneVmax. PQ diffuse white (100 nits) -> 0.889,
// 1000 nits -> 0.960, 10000 nits -> 1.0. scripts/verify_video_formats.py mirrors these values.
constexpr float kSdrWhiteNits = 100.0f;
constexpr float kHlgPeakNits = 1000.0f;
constexpr float kToneKnee = 0.85f;
constexpr float kToneSoft = 0.0175f;
constexpr float kToneVmax = 100.0f;

// P010 keeps a 10-bit code in the high 10 bits of a 16-bit UNORM sample: sample = code*64/65535.
constexpr float kP010CodeScale = 65535.0f / 65472.0f;

void luma_coefficients(VideoMatrix matrix, float& kr, float& kg, float& kb)
{
    switch (matrix) {
    case VideoMatrix::Bt601:
        kr = 0.299f; kg = 0.587f; kb = 0.114f;
        break;
    case VideoMatrix::Bt2020:
        kr = 0.2627f; kg = 0.6780f; kb = 0.0593f;
        break;
    case VideoMatrix::Bt709:
    default:
        kr = 0.2126f; kg = 0.7152f; kb = 0.0722f;
        break;
    }
}

// One vertex shader (fullscreen triangle from SV_VertexID) and three pixel shaders: the Y and UV
// passes write the two planes of the NV12 output, the RGB pass feeds the video-processor fallback.
// Order of operations: YCbCr -> R'G'B' -> linear -> BT.709 primaries -> tone map -> OETF -> RGB'.
const char kShaderSrc[] = R"HLSL(
Texture2DArray<float>  texY  : register(t0);
Texture2DArray<float2> texUV : register(t1);
SamplerState           samp0 : register(s0);

cbuffer NormalizeCB : register(b0)
{
    uint4  flags;    // x = array index (0: SRVs are single-slice), y = transfer, z = source is BT.2020
    float4 ycs;      // x = luma offset, y = luma scale, z = chroma offset, w = chroma scale
    float4 kcoef;    // x/y/z = source Kr/Kg/Kb
    float4 rotU;     // xyz = inverse display-map row u (a, b, offset)
    float4 rotV;     // xyz = inverse display-map row v (a, b, offset)
    float4 srcRect;  // xy = visible/texture uv scale, zw = half a destination texel
    float4 tone;     // x = knee, y = softness, z = log normalizer, w = vmax
    float4 misc;     // x = code scale, y = HLG peak nits, z = SDR white nits
};

static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);
static const float3 kLuma2020 = float3(0.2627, 0.6780, 0.0593);

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

// Destination uv -> source texture uv: inverse of the display matrix's rotation/mirror, then the
// visible-area crop (decoder surfaces may be padded past the coded size).
float2 affine_uv(float2 uv)
{
    return float2(dot(rotU.xy, uv) + rotU.z, dot(rotV.xy, uv) + rotV.z) * srcRect.xy;
}

// Sampled YCbCr: luma in [0,1], chroma centred on 0 and scaled to +-0.5.
float3 source_ycbcr(float2 uv)
{
    float3 uvw = float3(uv, (float)flags.x);
    float y = texY.Sample(samp0, uvw).r * misc.x;
    float2 c = texUV.Sample(samp0, uvw).rg * misc.x;
    return float3((y - ycs.x) * ycs.y, (c - ycs.z) * ycs.w);
}

float3 ycbcr_to_rgb(float3 yuv)
{
    float r = yuv.x + yuv.z * 2.0 * (1.0 - kcoef.x);
    float b = yuv.x + yuv.y * 2.0 * (1.0 - kcoef.z);
    float g = (yuv.x - kcoef.x * r - kcoef.z * b) / kcoef.y;
    return float3(r, g, b);
}

float sdr_eotf(float v)
{
    return v < 0.081 ? v / 4.5 : pow(max(v + 0.099, 0.0) / 1.099, 1.0 / 0.45);
}

float sdr_oetf(float v)
{
    v = max(v, 0.0);
    return v < 0.018 ? 4.5 * v : 1.099 * pow(v, 0.45) - 0.099;
}

// SMPTE ST 2084 EOTF -> 0..1 of a 10000-nit range.
float pq_eotf(float e)
{
    float p = pow(max(e, 0.0), 1.0 / 78.84375);
    return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

// ARIB STD-B67 inverse OETF -> scene light.
float hlg_inv_oetf(float e)
{
    return e <= 0.5 ? e * e / 3.0 : (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
}

// Non-linear source R'G'B' -> display-referred linear light in SDR-white units.
float3 to_linear(float3 c)
{
    if (flags.y == 1)
        return float3(pq_eotf(c.r), pq_eotf(c.g), pq_eotf(c.b)) * (10000.0 / misc.z);
    if (flags.y == 2) {
        // HLG: inverse OETF to scene light, OOTF with the 1000-nit nominal system gamma (1.2).
        float3 e = float3(hlg_inv_oetf(c.r), hlg_inv_oetf(c.g), hlg_inv_oetf(c.b));
        float ys = dot(e, kLuma2020);
        return misc.y * pow(max(ys, 1e-6), 0.2) * e / misc.z;
    }
    return float3(sdr_eotf(c.r), sdr_eotf(c.g), sdr_eotf(c.b));
}

// BT.2020 -> BT.709 primaries in linear light. Out-of-gamut results are desaturated towards
// their own BT.709 luminance instead of being clamped, so luminance survives the conversion.
float3 to_rec709_primaries(float3 c)
{
    if (flags.z == 0) return c;
    float3 r = float3(
         1.6605 * c.r - 0.5876 * c.g - 0.0728 * c.b,
        -0.1246 * c.r + 1.1329 * c.g - 0.0083 * c.b,
        -0.0182 * c.r - 0.1006 * c.g + 1.1187 * c.b);
    float lum = dot(r, kLuma709);
    float lo = min(r.r, min(r.g, r.b));
    if (lo < 0.0 && lum > 1e-5)
        r = lum + (r - lum) * (lum / max(lum - lo, 1e-6));
    return max(r, 0.0);
}

// HDR only: compress BT.709 linear light into the SDR range, scaling all channels by the
// luminance ratio so hue is preserved. A saturated source can still exceed 1.0 in a single
// channel when only its luminance fits (HDR red at 4x SDR white), so the result is then fitted
// by desaturating around that luminance -- clipping the channel instead would throw away the
// luminance the curve just produced.
float3 tone_map(float3 lin)
{
    if (flags.y == 0) return lin;
    float lum = dot(lin, kLuma709);
    if (lum <= 0.0) return lin;
    float mapped = lum <= tone.x
        ? lum
        : tone.x + (1.0 - tone.x) * log(1.0 + (lum - tone.x) / tone.y) / tone.z;
    float3 rgb = lin * (mapped / lum);
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    if (peak > 1.0 && peak > mapped)
        rgb = mapped + (rgb - mapped) * ((1.0 - mapped) / (peak - mapped));
    return max(rgb, 0.0);
}

float3 rec709_nonlinear(float2 uv)
{
    float3 lin = to_linear(ycbcr_to_rgb(source_ycbcr(affine_uv(uv))));
    lin = tone_map(to_rec709_primaries(lin));
    return saturate(float3(sdr_oetf(lin.r), sdr_oetf(lin.g), sdr_oetf(lin.b)));
}

// The NV12 output is always BT.709 limited range, so its encode is fixed at 16/219 and 128/224.
float PSMain_Y(VSOut i) : SV_Target
{
    float3 v = rec709_nonlinear(i.uv);
    return (16.0 + 219.0 * dot(v, kLuma709)) / 255.0;
}

float2 PSMain_UV(VSOut i) : SV_Target
{
    // Half a destination luma texel shifts the half-resolution pass onto chroma texel centres
    // (both grids are (2k+1)/N aligned), so 90/180/270 rotations keep chroma exact.
    float3 v = rec709_nonlinear(i.uv + srcRect.zw);
    float y = dot(v, kLuma709);
    return float2((128.0 + 224.0 * (v.b - y) / 1.8556) / 255.0,
                  (128.0 + 224.0 * (v.r - y) / 1.5748) / 255.0);
}

float4 PSMain_RGB(VSOut i) : SV_Target
{
    return float4(rec709_nonlinear(i.uv), 1.0);
}
)HLSL";

bool compile_stage(const char* entry, const char* profile, ID3DBlob** blob, std::string& error)
{
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc) - 1, "video_normalizer.hlsl", nullptr,
        nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, blob, &errors);
    if (FAILED(hr)) {
        const char* text = errors ? static_cast<const char*>(errors->GetBufferPointer()) : nullptr;
        char head[256];
        snprintf(head, sizeof(head), "D3DCompile(%s) failed: hr=0x%08lx", entry,
            static_cast<unsigned long>(hr));
        error = head;
        if (text) {
            error += ": ";
            error.append(text, std::strcspn(text, "\r\n"));
        }
        return false;
    }
    return true;
}

std::string hr_message(const char* what, HRESULT hr)
{
    char buf[192];
    snprintf(buf, sizeof(buf), "%s failed: hr=0x%08lx", what, static_cast<unsigned long>(hr));
    return buf;
}

// Per-plane render target view on a planar video surface (D3D11.1). PlaneSlice is explicit when
// the device exposes ID3D11Device3; the plain call relies on the runtime's format->plane mapping.
HRESULT create_plane_rtv(ID3D11Device* device, ID3D11Device3* device3, ID3D11Texture2D* texture,
                         DXGI_FORMAT format, UINT plane, ComPtr<ID3D11RenderTargetView>& out)
{
    if (device3) {
        D3D11_RENDER_TARGET_VIEW_DESC1 d1{};
        d1.Format = format;
        d1.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        d1.Texture2DArray.MipSlice = 0;
        d1.Texture2DArray.FirstArraySlice = 0;
        d1.Texture2DArray.ArraySize = 1;
        d1.Texture2DArray.PlaneSlice = plane;
        ComPtr<ID3D11RenderTargetView1> view1;
        HRESULT hr = device3->CreateRenderTargetView1(texture, &d1, &view1);
        if (SUCCEEDED(hr)) {
            out = view1.Get();
            return hr;
        }
    }
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    d.Format = format;
    d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    d.Texture2DArray.MipSlice = 0;
    d.Texture2DArray.FirstArraySlice = 0;
    d.Texture2DArray.ArraySize = 1;
    return device->CreateRenderTargetView(texture, &d, out.ReleaseAndGetAddressOf());
}

} // namespace

bool video_normalize_required(const VideoNormalizeDesc& desc)
{
    return desc.bit_depth > 8 || desc.full_range || desc.matrix != VideoMatrix::Bt709 ||
        desc.transfer != VideoTransfer::Sdr || desc.primaries_2020 || desc.rotation_cw != 0 ||
        desc.hflip;
}

VideoNormalizer::~VideoNormalizer()
{
    reset();
}

const char* VideoNormalizer::path_name() const
{
    switch (path_) {
    case Path::PlaneRtv: return "plane-rtv";
    case Path::VideoProcessor: return "video-processor";
    default: return "none";
    }
}

void VideoNormalizer::reset()
{
    srv_y_.clear();
    srv_uv_.clear();
    input_texture_ = nullptr;
    processor_configured_ = false;
    video_output_view_.Reset();
    video_input_view_.Reset();
    video_processor_.Reset();
    video_enumerator_.Reset();
    video_context_.Reset();
    video_device_.Reset();
    rgb_rtv_.Reset();
    rgb_texture_.Reset();
    uv_rtv_.Reset();
    y_rtv_.Reset();
    output_.Reset();
    cbuffer_.Reset();
    sampler_.Reset();
    ps_rgb_.Reset();
    ps_uv_.Reset();
    ps_y_.Reset();
    vs_.Reset();
    context_.Reset();
    device3_.Reset();
    device_.Reset();
    desc_ = {};
    path_ = Path::None;
    ten_bit_source_ = false;
    src_uv_scale_x_ = src_uv_scale_y_ = 1.0f;
}

bool VideoNormalizer::initialize(ID3D11Device* device, const VideoNormalizeDesc& desc,
                                 std::string& error)
{
    reset();
    if (!device) {
        error = "video_normalizer: null D3D11 device";
        return false;
    }
    if (desc.src_width <= 0 || desc.src_height <= 0 || desc.dst_width <= 0 ||
        desc.dst_height <= 0) {
        error = "video_normalizer: invalid source/destination dimensions";
        return false;
    }
    const bool swap = (desc.rotation_cw % 180) != 0;
    const int expect_w = swap ? desc.src_height : desc.src_width;
    const int expect_h = swap ? desc.src_width : desc.src_height;
    if (desc.dst_width != expect_w || desc.dst_height != expect_h) {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "video_normalizer: destination %dx%d does not match a %d degree rotation of %dx%d",
            desc.dst_width, desc.dst_height, desc.rotation_cw, desc.src_width, desc.src_height);
        error = buf;
        return false;
    }
    if ((desc.dst_width & 1) || (desc.dst_height & 1)) {
        error = "video_normalizer: NV12 output needs even dimensions";
        return false;
    }

    device_ = device;
    device_.As(&device3_);
    device_->GetImmediateContext(&context_);
    if (!context_) {
        error = "video_normalizer: GetImmediateContext failed";
        return false;
    }
    desc_ = desc;
    ten_bit_source_ = desc.bit_depth > 8;

    if (!compile_pipeline(error) || !create_output(error)) {
        reset();
        return false;
    }
    if (!build_plane_targets(error)) {
        const std::string plane_error = error;
        error.clear();
        if (!build_video_processor(error)) {
            error = plane_error + "; RGB->NV12 fallback " + error;
            reset();
            return false;
        }
        fprintf(stderr, "[sumu] video normalizer: per-plane render targets unavailable (%s); "
            "using the D3D11 video processor for the RGB->NV12 step\n", plane_error.c_str());
    }

    fprintf(stderr,
        "[sumu] video normalizer: %dx%d -> %dx%d, %d-bit, %s/%s, %s primaries, %s range, %s path\n",
        desc_.src_width, desc_.src_height, desc_.dst_width, desc_.dst_height, desc_.bit_depth,
        desc_.matrix == VideoMatrix::Bt2020 ? "BT.2020" :
            (desc_.matrix == VideoMatrix::Bt601 ? "BT.601" : "BT.709"),
        desc_.transfer == VideoTransfer::Pq ? "PQ" :
            (desc_.transfer == VideoTransfer::Hlg ? "HLG" : "SDR"),
        desc_.primaries_2020 ? "BT.2020" : "BT.709",
        desc_.full_range ? "full" : "limited", path_name());
    return true;
}

bool VideoNormalizer::compile_pipeline(std::string& error)
{
    ComPtr<ID3DBlob> blob;
    if (!compile_stage("VSMain", "vs_5_0", &blob, error)) return false;
    HRESULT hr = device_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
        nullptr, &vs_);
    if (FAILED(hr)) {
        error = hr_message("CreateVertexShader", hr);
        return false;
    }

    const struct
    {
        const char* entry;
        ComPtr<ID3D11PixelShader>* shader;
    } stages[] = { { "PSMain_Y", &ps_y_ }, { "PSMain_UV", &ps_uv_ }, { "PSMain_RGB", &ps_rgb_ } };
    for (const auto& stage : stages) {
        if (!compile_stage(stage.entry, "ps_5_0", &blob, error)) return false;
        hr = device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
            stage.shader->ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            char what[64];
            snprintf(what, sizeof(what), "CreatePixelShader(%s)", stage.entry);
            error = hr_message(what, hr);
            return false;
        }
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sd, &sampler_);
    if (FAILED(hr)) {
        error = hr_message("CreateSamplerState", hr);
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ShaderParams);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&bd, nullptr, &cbuffer_);
    if (FAILED(hr)) {
        error = hr_message("CreateBuffer(cbuffer)", hr);
        return false;
    }
    return true;
}

bool VideoNormalizer::create_output(std::string& error)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(desc_.dst_width);
    td.Height = static_cast<UINT>(desc_.dst_height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &output_);
    if (FAILED(hr)) {
        char buf[192];
        snprintf(buf, sizeof(buf), "CreateTexture2D(NV12 %dx%d output) failed: hr=0x%08lx",
            desc_.dst_width, desc_.dst_height, static_cast<unsigned long>(hr));
        error = buf;
        return false;
    }
    return true;
}

bool VideoNormalizer::build_plane_targets(std::string& error)
{
    // Creation, not CheckFormatSupport, decides: the driver's video-format reporting is
    // driver-specific while a failed view is unambiguous.
    HRESULT hr = create_plane_rtv(device_.Get(), device3_.Get(), output_.Get(),
        DXGI_FORMAT_R8_UNORM, 0, y_rtv_);
    if (FAILED(hr)) {
        UINT support = 0;
        device_->CheckFormatSupport(DXGI_FORMAT_NV12, &support);
        char buf[192];
        snprintf(buf, sizeof(buf), "CreateRenderTargetView(NV12 luma plane) failed: "
            "hr=0x%08lx (format support 0x%x)", static_cast<unsigned long>(hr), support);
        error = buf;
        return false;
    }
    hr = create_plane_rtv(device_.Get(), device3_.Get(), output_.Get(),
        DXGI_FORMAT_R8G8_UNORM, 1, uv_rtv_);
    if (FAILED(hr)) {
        error = hr_message("CreateRenderTargetView(NV12 chroma plane)", hr);
        y_rtv_.Reset();
        return false;
    }
    path_ = Path::PlaneRtv;
    return true;
}

// Resources and interfaces only -- no context state, because initialize() runs during
// Decoder::open() while the present thread is already using the shared context.
bool VideoNormalizer::build_video_processor(std::string& error)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(desc_.dst_width);
    td.Height = static_cast<UINT>(desc_.dst_height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &rgb_texture_);
    if (FAILED(hr)) {
        error = hr_message("CreateTexture2D(RGB stage)", hr);
        return false;
    }
    hr = device_->CreateRenderTargetView(rgb_texture_.Get(), nullptr, &rgb_rtv_);
    if (FAILED(hr)) {
        error = hr_message("CreateRenderTargetView(RGB stage)", hr);
        return false;
    }

    hr = device_->QueryInterface(IID_PPV_ARGS(&video_device_));
    if (FAILED(hr)) {
        error = hr_message("QueryInterface(ID3D11VideoDevice)", hr);
        return false;
    }
    hr = context_->QueryInterface(IID_PPV_ARGS(&video_context_));
    if (FAILED(hr)) {
        error = hr_message("QueryInterface(ID3D11VideoContext)", hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = { static_cast<UINT>(desc_.fps > 0.0 ? desc_.fps * 1000.0 + 0.5 : 60000.0), 1000 };
    content.InputWidth = static_cast<UINT>(desc_.dst_width);
    content.InputHeight = static_cast<UINT>(desc_.dst_height);
    content.OutputFrameRate = content.InputFrameRate;
    content.OutputWidth = content.InputWidth;
    content.OutputHeight = content.InputHeight;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    hr = video_device_->CreateVideoProcessorEnumerator(&content, &video_enumerator_);
    if (FAILED(hr)) {
        error = hr_message("CreateVideoProcessorEnumerator(RGB->NV12)", hr);
        return false;
    }

    UINT rgb_flags = 0;
    UINT nv12_flags = 0;
    if (FAILED(video_enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &rgb_flags)) ||
        !(rgb_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        FAILED(video_enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &nv12_flags)) ||
        !(nv12_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        error = "video processor does not support B8G8R8A8 -> NV12";
        return false;
    }
    hr = video_device_->CreateVideoProcessor(video_enumerator_.Get(), 0, &video_processor_);
    if (FAILED(hr)) {
        error = hr_message("CreateVideoProcessor", hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc{};
    in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    in_desc.Texture2D.MipSlice = 0;
    in_desc.Texture2D.ArraySlice = 0;
    hr = video_device_->CreateVideoProcessorInputView(rgb_texture_.Get(), video_enumerator_.Get(),
        &in_desc, &video_input_view_);
    if (FAILED(hr)) {
        error = hr_message("CreateVideoProcessorInputView(RGB)", hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc{};
    out_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    out_desc.Texture2D.MipSlice = 0;
    hr = video_device_->CreateVideoProcessorOutputView(output_.Get(), video_enumerator_.Get(),
        &out_desc, &video_output_view_);
    if (FAILED(hr)) {
        error = hr_message("CreateVideoProcessorOutputView(NV12)", hr);
        return false;
    }

    path_ = Path::VideoProcessor;
    return true;
}

// Deferred to the first process() call so every context write happens under the caller's
// serialization. The shader produced non-linear full-range BT.709 R'G'B'; the processor's job is
// the RGB -> BT.709 studio-range encode.
bool VideoNormalizer::configure_processor_stream(std::string& error)
{
    if (processor_configured_) return true;
    if (!video_context_ || !video_processor_) {
        error = "video_normalizer: video processor is not initialized";
        return false;
    }
    const RECT rect{ 0, 0, desc_.dst_width, desc_.dst_height };
    video_context_->VideoProcessorSetStreamFrameFormat(video_processor_.Get(), 0,
        D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context_->VideoProcessorSetStreamSourceRect(video_processor_.Get(), 0, TRUE, &rect);
    video_context_->VideoProcessorSetStreamDestRect(video_processor_.Get(), 0, TRUE, &rect);
    video_context_->VideoProcessorSetOutputTargetRect(video_processor_.Get(), TRUE, &rect);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_cs{};
    input_cs.Usage = 0;
    input_cs.RGB_Range = 0; // full-range RGB input
    input_cs.YCbCr_Matrix = 0;
    input_cs.YCbCr_xvYCC = 0;
    input_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255; // must agree with RGB_Range
    video_context_->VideoProcessorSetStreamColorSpace(video_processor_.Get(), 0, &input_cs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_cs{};
    output_cs.Usage = 0;
    output_cs.RGB_Range = 0;
    output_cs.YCbCr_Matrix = 1; // BT.709
    output_cs.YCbCr_xvYCC = 0;
    output_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    video_context_->VideoProcessorSetOutputColorSpace(video_processor_.Get(), &output_cs);

    processor_configured_ = true;
    return true;
}

bool VideoNormalizer::ensure_input_views(ID3D11Texture2D* source, std::string& error)
{
    if (!source) {
        error = "video_normalizer: null source texture";
        return false;
    }
    if (source == input_texture_ && !srv_y_.empty()) return true;

    srv_y_.clear();
    srv_uv_.clear();
    input_texture_ = nullptr;

    D3D11_TEXTURE2D_DESC td{};
    source->GetDesc(&td);
    const DXGI_FORMAT expect = ten_bit_source_ ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    if (td.Format != expect) {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "video_normalizer: decoder surface format %d, expected %d for a %d-bit source",
            static_cast<int>(td.Format), static_cast<int>(expect), desc_.bit_depth);
        error = buf;
        return false;
    }

    // One single-slice view per array element and plane; the shader therefore always samples
    // array index 0 (see ShaderParams::flags). For a planar surface the plane follows from the
    // view format.
    const DXGI_FORMAT y_fmt = ten_bit_source_ ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const DXGI_FORMAT uv_fmt = ten_bit_source_ ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    srv_y_.resize(td.ArraySize);
    srv_uv_.resize(td.ArraySize);
    for (UINT i = 0; i < td.ArraySize; ++i) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MostDetailedMip = 0;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.FirstArraySlice = i;
        sd.Texture2DArray.ArraySize = 1;
        sd.Format = y_fmt;
        HRESULT hr = device_->CreateShaderResourceView(source, &sd, &srv_y_[i]);
        if (FAILED(hr)) {
            error = hr_message("CreateShaderResourceView(luma)", hr);
            srv_y_.clear();
            return false;
        }
        sd.Format = uv_fmt;
        hr = device_->CreateShaderResourceView(source, &sd, &srv_uv_[i]);
        if (FAILED(hr)) {
            error = hr_message("CreateShaderResourceView(chroma)", hr);
            srv_y_.clear();
            srv_uv_.clear();
            return false;
        }
    }

    // Decoder pools are often padded past the coded size: crop to the visible area instead of
    // stretching the padding into the picture.
    src_uv_scale_x_ = (td.Width > 0 && desc_.src_width < static_cast<int>(td.Width))
        ? static_cast<float>(desc_.src_width) / static_cast<float>(td.Width) : 1.0f;
    src_uv_scale_y_ = (td.Height > 0 && desc_.src_height < static_cast<int>(td.Height))
        ? static_cast<float>(desc_.src_height) / static_cast<float>(td.Height) : 1.0f;
    input_texture_ = source;
    return true;
}

void VideoNormalizer::fill_shader_params(bool chroma_pass, ShaderParams& params) const
{
    std::memset(&params, 0, sizeof(params));
    params.flags[0] = 0; // single-slice views: the only valid array index
    params.flags[1] = static_cast<unsigned int>(desc_.transfer);
    params.flags[2] = desc_.primaries_2020 ? 1u : 0u;

    const float units = ten_bit_source_ ? 1023.0f : 255.0f;
    const float luma_offset = ten_bit_source_ ? 64.0f : 16.0f;
    const float luma_span = ten_bit_source_ ? 876.0f : 219.0f;
    const float chroma_centre = ten_bit_source_ ? 512.0f : 128.0f;
    const float chroma_span = ten_bit_source_ ? 896.0f : 224.0f;
    params.ycs[0] = desc_.full_range ? 0.0f : luma_offset / units;
    params.ycs[1] = desc_.full_range ? 1.0f : units / luma_span;
    params.ycs[2] = chroma_centre / units;
    params.ycs[3] = desc_.full_range ? 1.0f : units / chroma_span;

    float kr = 0.2126f, kg = 0.7152f, kb = 0.0722f;
    luma_coefficients(desc_.matrix, kr, kg, kb);
    params.kcoef[0] = kr;
    params.kcoef[1] = kg;
    params.kcoef[2] = kb;

    // Inverse display map in normalized destination coordinates:
    //   u_src = 0.5 + B^-1 * (u_dst - 0.5),  B = A scaled by src/dst size (A = R(rot) * F_hflip)
    const double radians = static_cast<double>(desc_.rotation_cw) * 3.14159265358979323846 / 180.0;
    double ma = std::cos(radians), mb = std::sin(radians);
    double mc = -std::sin(radians), md = std::cos(radians);
    if (desc_.hflip) {
        ma = -ma;
        mb = -mb;
    }
    const double wd = static_cast<double>(desc_.dst_width);
    const double hd = static_cast<double>(desc_.dst_height);
    const double bx0 = ma * static_cast<double>(desc_.src_width) / wd;
    const double bx1 = mc * static_cast<double>(desc_.src_height) / wd;
    const double by0 = mb * static_cast<double>(desc_.src_width) / hd;
    const double by1 = md * static_cast<double>(desc_.src_height) / hd;
    const double det = bx0 * by1 - bx1 * by0;
    const double inv_det = std::fabs(det) > 1e-12 ? 1.0 / det : 1.0;
    const double iu0 = by1 * inv_det, iu1 = -bx1 * inv_det;
    const double iv0 = -by0 * inv_det, iv1 = bx0 * inv_det;
    params.rot_u[0] = static_cast<float>(iu0);
    params.rot_u[1] = static_cast<float>(iu1);
    params.rot_u[2] = static_cast<float>(0.5 - 0.5 * (iu0 + iu1));
    params.rot_v[0] = static_cast<float>(iv0);
    params.rot_v[1] = static_cast<float>(iv1);
    params.rot_v[2] = static_cast<float>(0.5 - 0.5 * (iv0 + iv1));

    params.src_rect[0] = src_uv_scale_x_;
    params.src_rect[1] = src_uv_scale_y_;
    params.src_rect[2] = chroma_pass ? 0.5f / static_cast<float>(desc_.dst_width) : 0.0f;
    params.src_rect[3] = chroma_pass ? 0.5f / static_cast<float>(desc_.dst_height) : 0.0f;

    params.tone[0] = kToneKnee;
    params.tone[1] = kToneSoft;
    params.tone[2] = std::log(1.0f + (kToneVmax - kToneKnee) / kToneSoft);
    params.tone[3] = kToneVmax;

    params.misc[0] = ten_bit_source_ ? kP010CodeScale : 1.0f;
    params.misc[1] = kHlgPeakNits;
    params.misc[2] = kSdrWhiteNits;
}

bool VideoNormalizer::draw_pass(const ShaderParams& params, ID3D11PixelShader* ps,
                               ID3D11RenderTargetView* rtv, float width, float height,
                               ID3D11ShaderResourceView* const* srvs, UINT srv_count,
                               std::string& error)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context_->Map(cbuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        error = hr_message("Map(normalizer cbuffer)", hr);
        return false;
    }
    memcpy(mapped.pData, &params, sizeof(params));
    context_->Unmap(cbuffer_.Get(), 0);

    const D3D11_VIEWPORT vp{ 0.0f, 0.0f, width, height, 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);
    ID3D11RenderTargetView* rtvs[] = { rtv };
    context_->OMSetRenderTargets(1, rtvs, nullptr);
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer* buffers[] = { cbuffer_.Get() };
    context_->PSSetConstantBuffers(0, 1, buffers);
    ID3D11SamplerState* samplers[] = { sampler_.Get() };
    context_->PSSetSamplers(0, 1, samplers);
    context_->PSSetShaderResources(0, srv_count, srvs);
    context_->Draw(3, 0);
    return true;
}

bool VideoNormalizer::processor_blt(std::string& error)
{
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = video_input_view_.Get();
    HRESULT hr = video_context_->VideoProcessorBlt(video_processor_.Get(),
        video_output_view_.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        error = hr_message("VideoProcessorBlt(RGB->NV12)", hr);
        return false;
    }
    return true;
}

void VideoNormalizer::unbind_own_state()
{
    ID3D11RenderTargetView* null_rtv[] = { nullptr };
    context_->OMSetRenderTargets(1, null_rtv, nullptr);
    ID3D11ShaderResourceView* null_srvs[2] = { nullptr, nullptr };
    context_->PSSetShaderResources(0, 2, null_srvs);
}

bool VideoNormalizer::process(ID3D11Texture2D* source, UINT array_slice, std::string& error)
{
    if (!output_ || !context_ || path_ == Path::None) {
        error = "video_normalizer: not initialized";
        return false;
    }
    if (!ensure_input_views(source, error)) return false;
    if (array_slice >= srv_y_.size()) {
        char buf[128];
        snprintf(buf, sizeof(buf), "video_normalizer: array slice %u out of range (%zu)",
            array_slice, srv_y_.size());
        error = buf;
        return false;
    }

    ID3D11ShaderResourceView* srvs[] = { srv_y_[array_slice].Get(), srv_uv_[array_slice].Get() };
    const float dst_w = static_cast<float>(desc_.dst_width);
    const float dst_h = static_cast<float>(desc_.dst_height);

    if (path_ == Path::VideoProcessor) {
        if (!configure_processor_stream(error)) return false;
        ShaderParams rgb{};
        fill_shader_params(false, rgb);
        if (!draw_pass(rgb, ps_rgb_.Get(), rgb_rtv_.Get(), dst_w, dst_h, srvs, 2, error)) {
            unbind_own_state();
            return false;
        }
        // The RGB stage must not stay bound as a render target / shader input while the video
        // processor reads it.
        unbind_own_state();
        return processor_blt(error);
    }

    ShaderParams luma{};
    fill_shader_params(false, luma);
    if (!draw_pass(luma, ps_y_.Get(), y_rtv_.Get(), dst_w, dst_h, srvs, 2, error)) {
        unbind_own_state();
        return false;
    }
    ShaderParams chroma{};
    fill_shader_params(true, chroma);
    if (!draw_pass(chroma, ps_uv_.Get(), uv_rtv_.Get(), dst_w * 0.5f, dst_h * 0.5f, srvs, 2,
            error)) {
        unbind_own_state();
        return false;
    }
    unbind_own_state();
    return true;
}
