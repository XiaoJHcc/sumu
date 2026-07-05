// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Promoted from spikes/spike0_d3d11_present/src/decoder.{h,cpp} (verbatim decode path) and
// spikes/spike2_clock_mixing/src/decoder.{h,cpp} (loop-on-EOF behavior), plus a new
// seek_to_frame() for sumu's I6 ("seek = reposition, not teardown"): FFmpeg d3d11va hardware
// decode of HEVC (or anything FFmpeg + the installed D3D11VA hwaccel supports) straight into
// GPU-resident NV12 D3D11 textures shared with the caller's device -- no readback, no re-upload.
//
// AVFrame with format == AV_PIX_FMT_D3D11:
//   frame->data[0] -> ID3D11Texture2D*  (a texture ARRAY; one physical texture backs the
//                                         whole decode pool)
//   frame->data[1] -> array slice index for this frame, stored as an integer cast to a
//                     pointer (intptr_t), NOT a real pointer.
//
// We ask FFmpeg to build the hw_frames_ctx with BindFlags including
// D3D11_BIND_SHADER_RESOURCE (in addition to the D3D11_BIND_DECODER it always needs) so we
// can create SRVs straight on the decoder's own texture array slices.

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
// subresource to sample; `pts_seconds` is the presentation timestamp in seconds (stream
// time_base already applied), relative to the first frame ever decoded in this Decoder's
// lifetime (see Decoder::first_pts_seconds_) -- this is the single stable origin that
// frame_num = round(pts_seconds * fps) is computed against, seek or no seek (I5).
struct DecodedFrame
{
    ID3D11Texture2D* texture = nullptr; // NOT owned; alive as long as `owner_frame` is
    UINT array_slice = 0;
    double pts_seconds = 0.0;
};

class Decoder
{
public:
    Decoder() = default;
    ~Decoder();

    // device: caller's D3D11 device (already created, same one used for the swapchain).
    // Returns false + fills `error` on failure. Populates fps()/width()/height()/frame_count().
    bool open(const std::string& path, ID3D11Device* device, std::string& error);

    // Decode (and internally loop back to the start on EOF) until one frame is available.
    // Returns false only on unrecoverable error. The returned DecodedFrame is only valid
    // until the next call to next_frame() or seek_to_frame() (we keep exactly one AVFrame
    // "in flight" to bound VRAM use / decoder pool pressure).
    bool next_frame(DecodedFrame& out);

    // I6: seek = reposition, not teardown. Does NOT touch fmt_ctx_/codec_ctx_/hw_device_ref_
    // lifetime at all -- av_seek_frame + AVSEEK_FLAG_BACKWARD to the nearest keyframe at or
    // before target_frame, avcodec_flush_buffers, then decodes forward until the real decoded
    // PTS (not the nominal target) reaches target_frame (I5: frame numbers are anchored to
    // real decoded PTS). If target_frame lands at/after EOF, clamps to the last frame that
    // could actually be decoded rather than failing outright, so a near-tail seek degrades
    // gracefully. Returns the actual landed frame via `out`; caller derives the actual
    // frame_num from out.pts_seconds (== llround(out.pts_seconds * fps())).
    bool seek_to_frame(int64_t target_frame, DecodedFrame& out, std::string& error);

    double fps() const { return fps_; }
    int width() const { return width_; }
    int height() const { return height_; }
    // Best-effort total frame count (nb_frames if the container knows it, else derived from
    // stream/format duration * fps). 0 means genuinely unknown (rare containers) -- callers
    // should treat that as "no known upper bound" rather than crash.
    int64_t frame_count() const { return frame_count_; }

    void close();

private:
    static AVPixelFormat get_hw_format_thunk(AVCodecContext* ctx, const AVPixelFormat* fmts);
    bool ensure_hw_frames_ctx(AVCodecContext* ctx);
    bool seek_to_start();

    // Shared "pull one hw frame out of the demuxer/decoder" pump, used by both next_frame()
    // and seek_to_frame()'s forward-decode-to-target loop. Does NOT loop-on-EOF (that policy
    // differs between the two callers -- next_frame() restarts playback, seek_to_frame()
    // clamps). Returns 1 = got a frame (out filled, pts fields NOT yet remapped through
    // loop_offset_/first_pts_ -- caller does that), 0 = EOF, -1 = unrecoverable error.
    int pump_one_raw_frame(DecodedFrame& out, double& raw_pts_s_out);

    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVBufferRef* hw_device_ref_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    int video_stream_idx_ = -1;
    double fps_ = 60.0;
    AVRational time_base_{1, 1};
    int width_ = 0;
    int height_ = 0;
    int64_t frame_count_ = 0;

    bool have_first_pts_ = false;
    double first_pts_seconds_ = 0.0;
    double loop_offset_seconds_ = 0.0; // added to raw pts so the clock keeps climbing across loops
    double last_out_pts_seconds_ = -1.0;

    ID3D11Device* d3d_device_ = nullptr; // not owned
};
