// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Decoder: the stable facade Player and HeadlessDecode hold. It owns one session -- a MediaSource
// (AVFormat/IO/packets/audio queue/reconnect), a VideoDecoder backend (D3D11VA or software), the
// presentation timeline that anchors every frame number to real decoded PTS (I5), and the GPU
// frame normalizer -- and hands out DecodedFrames plus a typed DecodeStatus.
//
// Frame contract (see prepare_nv12_frame): every consumer always receives display-oriented,
// BT.709, limited-range, 8-bit NV12. width()/height() report that normalized display size; the
// size the codec actually produced is coded_width()/coded_height(). Anamorphic sources keep that
// grid as-is -- the shape correction is presentation-only, reported by pixel_aspect().
//
// Hardware decode keeps full precision into the normalizer (10-bit P010, HDR, full range, display
// matrix are all normalized on the GPU). The software compatibility path (non-4:2:0 sources, or a
// hardware decoder that is unavailable/fails) converts to 8-bit BT.709 limited NV12 in libswscale
// first -- HDR there degrades to a non-tone-mapped 8-bit compatibility image rather than losing
// the session.
//
// Threading: one thread drives decode calls at a time (Player's decode thread under
// decoder_mutex_). next_frame()/seek_to_frame()/reconnect_to_frame() may create GPU resources but
// never write immediate-context state; prepare_nv12_frame() does write context state and must be
// called under the caller's D3D serialization (Player's d3d_mutex_).

#pragma once

#include <d3d11.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "video_normalizer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

class MediaSource;
class VideoDecoder;
struct VideoPicture;

enum class VideoBackendKind
{
    None,
    Hardware,
    Software,
};

inline const char* video_backend_name(VideoBackendKind kind)
{
    switch (kind) {
    case VideoBackendKind::Hardware: return "d3d11va";
    case VideoBackendKind::Software: return "software";
    default: return "none";
    }
}

// Typed outcome of one decode step.
//   Frame              -- out is filled with a new picture.
//   WouldBlock         -- the source has nothing for us right now (live/unknown-duration stream
//                         between packets). Not a failure: call again later.
//   Eof                -- the stream really ended.
//   NeedReconfigure    -- a picture IS handed out, but its geometry/format no longer matches the
//                         session: resources sized from width()/height() must be rebuilt. Only
//                         returned after the first frame (the first frame silently re-adopts).
//   RetryWithSoftware  -- backend hint ("this hardware path cannot continue"); Decoder consumes it
//                         itself and never returns it to callers.
//   RecoverableIoError -- transport/bitstream hiccup; a seek or reconnect can resume.
//   FatalError         -- out is not usable; the session is done.
enum class DecodeStatus
{
    Frame,
    WouldBlock,
    Eof,
    NeedReconfigure,
    RetryWithSoftware,
    RecoverableIoError,
    FatalError,
};

struct DecodeResult
{
    DecodeStatus status = DecodeStatus::FatalError;
    std::string error; // empty for Frame/Eof/WouldBlock; diagnostic otherwise

    // Legacy truthiness: "did I get a picture". Pre-typed callers use it unchanged; anything that
    // must tell WouldBlock/Eof/NeedReconfigure apart switches on `status`.
    explicit operator bool() const { return status == DecodeStatus::Frame; }
};

// Coarse session state, for UI/diagnostics and for telling "still buffering" from "starved".
enum class SourceState
{
    Opening,
    Buffering,
    Ready,
    Starved,
    Reconfiguring,
    Recovering,
    Ended,
    Failed,
};

enum class TimelineKind
{
    Finite,
    Live,
    Segmented,
};

// One decoded frame, still GPU-resident. `texture`/`array_slice` identify the D3D11 subresource to
// sample; `pts_seconds` is the presentation timestamp in seconds (stream time_base already
// applied), relative to the first frame ever decoded in this Decoder's lifetime (I5: the single
// stable origin frame_num = round(pts_seconds * fps) is computed against, seek or no seek).
//
// Lifetime: everything here is valid only until the next next_frame()/seek_to_frame()/
// reconnect_to_frame()/close() -- exactly one picture is kept in flight.
struct DecodedFrame
{
    ID3D11Texture2D* texture = nullptr; // NOT owned; null for a software frame until prepared
    UINT array_slice = 0;
    double pts_seconds = 0.0;
    // How long this picture stays on screen: the frame's own declared duration, or one frame
    // interval (1/fps) when the container does not carry one. Downstream maps it onto the
    // fixed-fps grid so a VFR picture answers for its whole hold interval.
    double duration_seconds = 0.0;

    // ---- actual picture description (what is in `texture`, before normalization) ---------------
    int width = 0;  // visible picture size
    int height = 0;
    // Hardware: the D3D11VA frames sw_format (NV12 / P010). Software: AV_PIX_FMT_NV12, the
    // uploaded compatibility contract. Never the opaque AV_PIX_FMT_D3D11.
    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    int bit_depth = 8;
    bool full_range = false;
    VideoMatrix matrix = VideoMatrix::Bt709;
    VideoTransfer transfer = VideoTransfer::Sdr;
    bool primaries_2020 = false;

    // ---- software compatibility access ---------------------------------------------------------
    // True when the picture was decoded on the CPU. Its GPU texture is produced lazily by
    // prepare_nv12_frame() (the upload is a GPU write and needs the caller's D3D serialization),
    // so `texture` is null until then. cpu_plane[0]/[1] are the packed BT.709 limited 8-bit NV12
    // planes it is uploaded from (luma, then interleaved chroma at half height).
    bool software = false;
    AVFrame* source_frame = nullptr; // backend-owned decoded frame (not to be freed)
    const uint8_t* cpu_plane[2] = {nullptr, nullptr};
    int cpu_linesize[2] = {0, 0};
    uint64_t software_serial = 0; // internal: ties the CPU planes to the picture they came from
};

class Decoder
{
public:
    Decoder();
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // device: caller's D3D11 device (already created, same one used for the swapchain).
    // Returns false + fills `error` on failure. Populates fps()/width()/height()/frame_count().
    // `path` may be a local filesystem path OR an http(s) URL (FFmpeg network protocols).
    // `headers` / `cookies` are the optional HTTP request context the browser page captured for
    // this URL ("Referer: ...\r\nUser-Agent: ...\r\n" / "a=1; b=2"), applied to the HTTP protocol
    // only -- never for local files, never logged, and replayed by reconnect_to_frame().
    // The backend is chosen here: D3D11VA for the 4:2:0 formats it can decode on this device,
    // software for anything else and as the fallback when hardware open fails.
    bool open(const std::string& path, ID3D11Device* device, std::string& error,
              const std::string& headers = std::string(),
              const std::string& cookies = std::string());

    // True when the last successful open() treated `path` as a network URL (http/https).
    // Player uses this to pick the shallow IO profile (smaller ring, no scrub second open).
    bool is_network() const;
    bool is_live() const;

    // Scheme check only -- does not open anything. Shared with Player for early UI decisions.
    static bool looks_like_network_url(const std::string& path);

    // Ask a blocked open()/read to bail out early: the AVIOInterruptCB installed on the format
    // context makes FFmpeg's next network IO checkpoint fail instead of waiting out rw_timeout.
    // Atomic store only -- safe to call from the UI thread while open() blocks a worker.
    void request_interrupt();

    // Decode until one picture is available. Never loops on EOF. A transient source stall is only
    // waited out for buffered (known-duration) sources: a live/unknown-duration source answers
    // WouldBlock instead of spinning or failing.
    DecodeResult next_frame(DecodedFrame& out);

    // Normalize a decoded frame to the one contract every downstream path consumes:
    // display-oriented (display-matrix rotation/mirror applied), BT.709, limited range, 8-bit
    // NV12, array slice 0 -- all on this device. A source that already matches is passed through
    // untouched (frame left exactly as decoded, zero GPU work). Software frames are uploaded to
    // their reusable NV12 texture here. On success `frame` points at either the decoded surface
    // (fast path) or the normalizer's output surface; either way the caller may copy it with
    // CopySubresourceRegion(...., nullptr). Player callers must hold d3d_mutex_ around this call
    // and the subsequent copy; HeadlessDecode uses its single-threaded immediate context.
    // Returns false + `error` on any unsupported/unsatisfiable case (never a silently wrong image).
    bool prepare_nv12_frame(DecodedFrame& frame, std::string& error);

    // EOF state: set when next_frame() reaches the real end of the stream (not on a decoding
    // error); cleared by seek_to_frame()/reconnect_to_frame()/close(). Atomic -- written by the
    // decode thread, read by Player::open_session()'s start-of-session buffering wait in another
    // thread to tell "buffer not full yet" from "the stream is short and already finished".
    bool at_eof() const { return at_eof_.load(std::memory_order_acquire); }
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    // Diagnostic for the last failed()/RecoverableIoError outcome.
    const std::string& last_error() const { return last_error_; }

    // I6: seek = reposition, not teardown. Does NOT touch the transport or codec lifetime at all --
    // av_seek_frame + AVSEEK_FLAG_BACKWARD to the nearest keyframe at or before target_frame,
    // flush, then decode forward until the real decoded PTS (not the nominal target) reaches
    // target_frame (I5). If target_frame lands at/after EOF, clamps to the last frame that could
    // actually be decoded rather than failing outright, so a near-tail seek degrades gracefully.
    // Returns the actual landed frame via `out`; caller derives the actual frame_num from
    // out.pts_seconds (== llround(out.pts_seconds * fps())).
    // nearest_keyframe: for coarse previews (scrub grid) -- after the BACKWARD seek, return the
    // FIRST decoded frame (the keyframe at/before target) instead of decoding forward to the exact
    // target. Cuts per-seek work from a whole-GOP forward-decode to a single frame.
    bool seek_to_frame(int64_t target_frame, DecodedFrame& out, std::string& error,
                       bool nearest_keyframe = false);
    // Re-opens the URL (same headers/cookies) and repositions at target_frame. Only meaningful for
    // network sources whose transport died; fails when the source no longer matches the session.
    bool reconnect_to_frame(int64_t target_frame, DecodedFrame& out, std::string& error);
    // Reopens a live playlist/transport without seeking or resetting the session timeline.
    bool reconnect_live(std::string& error);

    double fps() const { return fps_; }
    // Normalized (display) size: for a stream carrying a 90/270 display matrix this is the coded
    // size swapped, so callers never have to know about rotation.
    int width() const { return width_; }
    int height() const { return height_; }
    // Size the codec actually produced (pre-rotation), for diagnostics only.
    int coded_width() const { return coded_width_; }
    int coded_height() const { return coded_height_; }
    // Pixel aspect ratio (pixel width / pixel height) the stream declares: 1.0 for square pixels.
    // Anamorphic sources are 16:9 on screen even though their coded grid is 3:2, so presentation
    // geometry must scale by this; the pixel paths work in coded pixels and ignore it.
    double pixel_aspect() const { return pixel_aspect_; }
    // Timeline length in the fixed-fps grid the player lays over the stream. 0 means genuinely
    // unknown (rare containers / live sources).
    int64_t frame_count() const { return frame_count_; }

    // Bumped whenever the frame geometry/colour description or the decode backend changes, so a
    // consumer holding derived GPU resources can tell its assumptions are stale.
    uint64_t config_generation() const
    {
        return config_generation_.load(std::memory_order_acquire);
    }
    SourceState source_state() const { return source_state_.load(std::memory_order_acquire); }
    TimelineKind timeline_kind() const;
    VideoBackendKind backend() const;

    // ---- audio (demux-only -- this class never decodes audio itself) --------------------------
    // It only pulls audio AVPackets off the transport (in pump_one_picture(), same av_read_frame()
    // call the video path uses) and queues them for Player's own audio thread to pop, decode,
    // resample and play. No audio stream in the file is legitimate and silent.
    bool has_audio() const;
    AVCodecParameters* audio_codecpar() const;
    AVRational audio_time_base() const;
    // Shared time-axis origin for the audio thread's A/V sync math: the raw PTS (seconds, stream
    // time_base applied) of the very first frame this Decoder ever decoded, established once and
    // never changed by a seek (I5). Atomic: read from Player's audio thread.
    double first_pts_seconds() const { return first_pts_seconds_; }
    bool have_first_pts() const { return have_first_pts_; }
    // See MediaSource::pop_audio_packet.
    bool pop_audio_packet(AVPacket* dst, double& loop_offset_out);
    void flush_audio_queue(); // caller must call under decoder_mutex_ BEFORE seek_to_frame()
    size_t audio_queue_depth() const; // diagnostics only, never control flow

    void close();

private:
    class LogScope;
    static thread_local Decoder* logging_decoder_;
    static void log_callback(void* context, int level, const char* format, va_list args);
    void record_corrupt_data(const char* message, bool packet_error = false);
    void flush_corrupt_logs(bool force = false);
    uint64_t corrupt_messages_ = 0;
    uint64_t corrupt_packets_ = 0;
    uint64_t reported_messages_ = 0;
    uint64_t reported_packets_ = 0;
    std::chrono::steady_clock::time_point next_log_summary_{};

    // Everything the normalizer needs to know about the surfaces arriving from the backend.
    struct SourceFormat
    {
        int width = 0;
        int height = 0;
        AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
        int bit_depth = 8;
        bool full_range = false;
        VideoMatrix matrix = VideoMatrix::Bt709;
        VideoTransfer transfer = VideoTransfer::Sdr;
        bool primaries_2020 = false;
    };

    // Opens the best backend for this stream: hardware when FFmpeg exposes D3D11VA for it and the
    // source is 4:2:0, software otherwise or as the fallback.
    bool open_backend(bool allow_hardware, std::string& error);
    // One-shot hardware -> software fallback inside a session (never a loop).
    bool switch_to_software(const std::string& reason, std::string& error);
    // Re-positions the transport and drops decoder/timeline state after a backend switch.
    bool resume_at(double raw_pts_seconds, std::string& error);

    SourceFormat initial_source_format() const;
    SourceFormat derive_source_format(const VideoPicture& picture) const;
    // Builds/refreshes the GPU normalizer for `format`; no-op when nothing changed.
    bool configure_output(const SourceFormat& format, std::string& error);

    // One picture from the backend, demuxing as needed. Returns Frame/Eof/WouldBlock/... but never
    // touches the timeline or the caller's frame.
    DecodeStatus pump_one_picture(VideoPicture& picture, double& raw_pts_s, std::string& error);
    // pump_one_picture + describe_picture, with the one-shot software fallback applied internally:
    // Frame/NeedReconfigure come back fully described, everything else is passed through.
    DecodeStatus pull_described(DecodedFrame& out, double& raw_pts_s, std::string& error);
    // Fills `out` (description + validation), and reconfigures internally on colour/size changes.
    DecodeStatus describe_picture(const VideoPicture& picture, DecodedFrame& out,
                                  std::string& error);
    DecodeResult fail(DecodeStatus status, const std::string& error);
    void set_state(SourceState state) { source_state_.store(state, std::memory_order_release); }
    // Stream PTS -> playback timeline (origin + discontinuity correction), with the monotonic guard
    // for backwards jumps. Every DecodedFrame goes through this so video and audio stay aligned.
    double map_timeline_pts(double raw_pts_s);

    std::unique_ptr<MediaSource> source_;
    std::unique_ptr<VideoDecoder> video_;
    AVPacket* pkt_ = nullptr;

    double fps_ = 60.0;
    AVRational time_base_{1, 1};
    int width_ = 0;
    int height_ = 0;
    double pixel_aspect_ = 1.0;
    int64_t frame_count_ = 0;

    std::atomic<bool> have_first_pts_{false};
    std::atomic<double> first_pts_seconds_{0.0};
    std::atomic<bool> at_eof_{false};
    std::atomic<bool> failed_{false};
    std::atomic<uint64_t> config_generation_{0};
    std::atomic<SourceState> source_state_{SourceState::Opening};
    std::string last_error_;

    bool draining_ = false;          // end-of-stream already signalled to the backend
    bool have_delivered_frame_ = false;
    bool software_fallback_used_ = false;

    double loop_offset_seconds_ = 0.0; // stays 0 (pause-on-last-frame); kept for pts formula
    // Raw stream PTS (time_base applied, NOT origin-shifted) of the last frame -- the base the
    // no-timestamp fallback steps from, and the resume point of a backend switch.
    double last_raw_pts_seconds_ = 0.0;
    double last_out_pts_seconds_ = -1.0; // last timeline PTS handed out (origin shifted)
    double discontinuity_offset_ = 0.0;  // keeps the timeline continuous across PTS resets
    bool segmented_timeline_ = false;

    ID3D11Device* d3d_device_ = nullptr; // not owned

    // Frame normalization (see prepare_nv12_frame). needs_normalization_ false == the decoded
    // surface is already the contract, so nothing is ever bound or drawn.
    bool needs_normalization_ = false;
    VideoNormalizeDesc normalize_desc_{};
    VideoNormalizer normalizer_;
    int coded_width_ = 0;
    int coded_height_ = 0;
    int display_rotation_cw_ = 0;
    bool display_hflip_ = false;
    bool reported_anamorphic_ = false;
    bool reported_rotation_ = false;
};
