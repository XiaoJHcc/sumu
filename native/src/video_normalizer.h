// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// VideoNormalizer: GPU normalization of a decoded D3D11VA surface into sumu's downstream frame
// contract -- display-oriented (display matrix rotation/mirror applied), BT.709, limited range,
// 8-bit NV12, on the caller's device, frames never leaving the GPU.
//
// A source that already matches the contract never enters this class (see
// video_normalize_required()). Everything else -- 10-bit P010, full range, BT.601/BT.2020,
// PQ/HLG, a display matrix -- is rendered by two shader passes that sample the decoder's own
// planes and write the Y and UV planes of an NV12 texture through D3D11.1 per-plane render
// targets (ID3D11Device3::CreateRenderTargetView1 + PlaneSlice). If the driver refuses those,
// a fallback renders full-range BT.709 RGB and lets ID3D11VideoProcessor produce the studio
// range NV12 instead. Both failing is reported, never silently degraded.
//
// Threading: process() issues GPU work on the device's immediate context, so the caller must
// already hold whatever serializes that context (Player's d3d_mutex_, HeadlessDecode's single
// thread). initialize() only creates resources and interfaces -- no context state is written
// until the first process() call, for the same reason.

#pragma once

#include <d3d11.h>
#include <d3d11_3.h>
#include <wrl/client.h>

#include <string>
#include <vector>

// Source matrix coefficients (YCbCr -> RGB) as tagged by the container/bitstream.
enum class VideoMatrix
{
    Bt709,
    Bt601,
    Bt2020,
};

// Source transfer characteristic. Pq/Hlg are HDR: inverted, tone mapped to SDR, then re-encoded.
enum class VideoTransfer
{
    Sdr,
    Pq,
    Hlg,
};

struct VideoNormalizeDesc
{
    int src_width = 0;   // visible coded size (what the decoder produced)
    int src_height = 0;
    int dst_width = 0;   // display size -- src swapped for 90/270 rotations
    int dst_height = 0;
    int bit_depth = 8;   // 8 -> NV12 surface, 10 -> P010 surface
    bool full_range = false;
    VideoMatrix matrix = VideoMatrix::Bt709;
    VideoTransfer transfer = VideoTransfer::Sdr;
    bool primaries_2020 = false;
    int rotation_cw = 0;         // clockwise rotation to apply for display: 0/90/180/270
    bool hflip = false;          // mirrored display matrix (applied in source space)
    double fps = 60.0;           // only describes the source to the video processor
};

// True when `desc` is not already the downstream contract, i.e. normalization would change it.
bool video_normalize_required(const VideoNormalizeDesc& desc);

class VideoNormalizer
{
public:
    VideoNormalizer() = default;
    ~VideoNormalizer();

    VideoNormalizer(const VideoNormalizer&) = delete;
    VideoNormalizer& operator=(const VideoNormalizer&) = delete;

    // Creates the output surface and both paths' resources. Returns false + `error` when the
    // conversion cannot be expressed on this device.
    bool initialize(ID3D11Device* device, const VideoNormalizeDesc& desc, std::string& error);

    // Normalizes one decoded surface slice into output_texture(). Caller must hold the context
    // serialization. Returns false + `error` on any D3D11 failure.
    bool process(ID3D11Texture2D* source, UINT array_slice, std::string& error);

    ID3D11Texture2D* output_texture() const { return output_.Get(); }

    // "plane-rtv" | "video-processor" | "none" -- diagnostics only.
    const char* path_name() const;

    void reset();

private:
    enum class Path
    {
        None,
        PlaneRtv,
        VideoProcessor,
    };

    // Mirrors the HLSL cbuffer field for field (9 x float4 = 144 bytes).
    struct ShaderParams
    {
        unsigned int flags[4]; // x = sampled array index (always 0: SRVs are single-slice views),
                               // y = transfer (0 SDR / 1 PQ / 2 HLG), z = source primaries 2020
        float ycs[4];          // x = luma offset, y = luma scale, z = chroma offset, w = scale
        float kcoef[4];        // x/y/z = source Kr/Kg/Kb
        float rot_u[4];        // xyz = inverse display-map row u (a, b, offset)
        float rot_v[4];        // xyz = inverse display-map row v (a, b, offset)
        float src_rect[4];     // xy = visible/texture uv scale, zw = half a destination texel
        float tone[4];         // x = knee, y = softness, z = log normalizer, w = vmax
        float misc[4];         // x = code scale, y = HLG peak nits, z = SDR white nits
    };

    bool compile_pipeline(std::string& error);
    bool create_output(std::string& error);
    bool build_plane_targets(std::string& error);
    bool build_video_processor(std::string& error);
    bool configure_processor_stream(std::string& error); // first process(), not initialize()
    bool ensure_input_views(ID3D11Texture2D* source, std::string& error);
    void fill_shader_params(bool chroma_pass, ShaderParams& params) const;
    bool draw_pass(const ShaderParams& params, ID3D11PixelShader* ps,
                   ID3D11RenderTargetView* rtv, float width, float height,
                   ID3D11ShaderResourceView* const* srvs, UINT srv_count, std::string& error);
    bool processor_blt(std::string& error);
    void unbind_own_state();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Device3> device3_; // optional, enables explicit plane slices
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_y_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_uv_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_rgb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cbuffer_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> y_rtv_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> uv_rtv_;

    // VideoProcessor fallback: RGB stage + processor + views.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rgb_texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rgb_rtv_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> video_enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> video_processor_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> video_input_view_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> video_output_view_;
    bool processor_configured_ = false;

    // Per-slice input SRVs, cached per source texture (one texture array backs a whole session).
    ID3D11Texture2D* input_texture_ = nullptr; // not owned; the SRVs retain it
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> srv_y_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> srv_uv_;
    float src_uv_scale_x_ = 1.0f;
    float src_uv_scale_y_ = 1.0f;

    VideoNormalizeDesc desc_{};
    Path path_ = Path::None;
    bool ten_bit_source_ = false;
};
