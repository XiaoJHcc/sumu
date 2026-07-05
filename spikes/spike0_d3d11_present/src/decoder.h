// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Spike 0: thin wrapper around libavcodec/libavformat doing D3D11VA hardware decode of
// an HEVC file. The FFmpeg hw_device_ctx is created FROM our own ID3D11Device (passed
// in), so the decoded AVFrame's D3D11 texture (frame->data[0]) lives on the SAME device
// as our swapchain and can be sampled directly by a shader -- no readback, no re-upload.
//
// AVFrame with format == AV_PIX_FMT_D3D11:
//   frame->data[0] -> ID3D11Texture2D*  (a texture ARRAY; one physical texture backs the
//                                         whole decode pool)
//   frame->data[1] -> array slice index for this frame, stored as an integer cast to a
//                     pointer (intptr_t), NOT a real pointer.
//
// We ask FFmpeg to build the hw_frames_ctx with BindFlags including
// D3D11_BIND_SHADER_RESOURCE (in addition to the D3D11_BIND_DECODER it always needs) so
// we can create SRVs straight on the decoder's own texture array slices.

#pragma once

#include <d3d11.h>
#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}

// One decoded frame, still GPU-resident. `texture`/`array_slice` identify the D3D11
// subresource to sample; `pts_seconds` is the presentation timestamp in seconds
// (stream time_base already applied), relative to the first frame of the current run
// (see Decoder::first_pts_seconds).
struct DecodedFrame
{
    ID3D11Texture2D* texture = nullptr; // NOT owned; alive as long as `owner_frame` is
    UINT array_slice = 0;
    double pts_seconds = 0.0;
    bool looped = false; // true on the first frame after we wrapped back to time 0
};

class Decoder
{
public:
    Decoder() = default;
    ~Decoder();

    // device: our D3D11 device (already created, same one used for the swapchain).
    // Returns false + fills `error` on failure.
    bool open(const std::string& path, ID3D11Device* device, std::string& error);

    // Decode (and internally loop back to the start on EOF) until one frame is
    // available. Returns false only on unrecoverable error. The returned DecodedFrame
    // is only valid until the next call to next_frame() (we keep exactly one AVFrame
    // "in flight" to bound VRAM use / decoder pool pressure).
    bool next_frame(DecodedFrame& out);

    double fps() const { return fps_; }

    void close();

private:
    static AVPixelFormat get_hw_format_thunk(AVCodecContext* ctx, const AVPixelFormat* fmts);
    bool ensure_hw_frames_ctx(AVCodecContext* ctx);
    bool seek_to_start();

    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVBufferRef* hw_device_ref_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    int video_stream_idx_ = -1;
    double fps_ = 60.0;
    AVRational time_base_{1, 1};

    bool have_first_pts_ = false;
    double first_pts_seconds_ = 0.0;
    double loop_offset_seconds_ = 0.0; // added to raw pts so the clock keeps climbing across loops
    double last_out_pts_seconds_ = -1.0;

    ID3D11Device* d3d_device_ = nullptr; // not owned
};
