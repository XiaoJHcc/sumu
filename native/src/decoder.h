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
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
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

    // ---- audio (spike, additive) -- demux-only: this class never builds an audio
    // AVCodecContext or decodes audio itself, it only pulls audio AVPackets off fmt_ctx_ (in
    // pump_one_raw_frame(), same av_read_frame() call the video path already uses) and queues
    // them for Player's own audio thread to pop, decode (software), resample, and play. No
    // audio stream in the file is legitimate and silent -- has_audio() is simply false, callers
    // must not treat that as an error (mirrors this class's existing silent-video-is-fine
    // stance elsewhere).
    bool has_audio() const { return audio_stream_idx_ >= 0; }
    AVCodecParameters* audio_codecpar() const
    {
        return has_audio() ? fmt_ctx_->streams[audio_stream_idx_]->codecpar : nullptr;
    }
    AVRational audio_time_base() const
    {
        return has_audio() ? fmt_ctx_->streams[audio_stream_idx_]->time_base : AVRational{ 1, 1 };
    }
    // Shared time-axis origin for the audio thread's A/V sync math (see player.cpp's
    // audio_loop()): the raw PTS (seconds, stream time_base already applied) of the very first
    // frame this Decoder ever decoded, established once in next_frame() and never changed by a
    // seek (I5) -- identical origin present_loop()'s frame numbers are anchored to. Atomic (not
    // just for pump_one_raw_frame()'s existing single-writer-thread use) because the audio
    // thread now reads it from a different thread with no other synchronization.
    double first_pts_seconds() const { return first_pts_seconds_; }
    bool have_first_pts() const { return have_first_pts_; }

    // Pop one queued audio packet (FIFO order) into `dst` (caller-owned AVPacket, typically
    // reused across calls -- av_packet_move_ref resets `dst` first). Returns false if the
    // queue is currently empty (dst left untouched). Thread-safe: this is the ONLY state
    // shared between the decode thread (pump_one_raw_frame(), the producer) and Player's
    // audio thread (the sole consumer) -- present_loop() never calls this.
    bool pop_audio_packet(AVPacket* dst);

    // Best-effort queue depth, for the audio thread's per-second drift/health log only (never
    // used for control flow) -- see player.cpp's audio_loop().
    size_t audio_queue_depth() const;

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

    // std::atomic: written once (single-shot init) by the decode thread inside next_frame(),
    // now also read from Player's audio thread (audio_loop()) with no other synchronization --
    // see first_pts_seconds()'s comment above. Everywhere else in this file these are used
    // exactly as plain double/bool (atomic<T>'s implicit conversion/assignment operators cover
    // every existing read/write site unchanged).
    std::atomic<bool> have_first_pts_{ false };
    std::atomic<double> first_pts_seconds_{ 0.0 };
    double loop_offset_seconds_ = 0.0; // added to raw pts so the clock keeps climbing across loops
    double last_out_pts_seconds_ = -1.0;

    ID3D11Device* d3d_device_ = nullptr; // not owned

    // ---- audio (spike, additive) -----------------------------------------------------------
    int audio_stream_idx_ = -1; // -1 == no audio stream (legitimate, silent)

    // Bounded so a slow/stalled audio consumer can never grow this without limit: 512 packets
    // is far more than the ~4s of audio headroom this needs even for small audio frames (e.g.
    // ~10.9s at a typical 1024-sample/48kHz AAC frame), so steady state should never trip the
    // drop path below -- if it does, that's the signal (see audio_pkt_dropped_).
    static constexpr size_t kAudioQueueMaxPackets = 512;
    mutable std::mutex audio_pkt_mutex_;
    std::deque<AVPacket*> audio_pkt_queue_; // guarded by audio_pkt_mutex_
    uint64_t audio_pkt_dropped_ = 0;        // guarded by audio_pkt_mutex_; oldest-drop counter
};
