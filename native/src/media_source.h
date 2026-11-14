// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// MediaSource: the IO/demux half of a decode session -- AVFormatContext lifetime, network options
// and reconnect, stream selection and metadata, packet reading/classification, and the bounded
// audio packet queue Player's audio thread drains. It knows nothing about codecs or D3D11; the
// codec half is VideoDecoder (video_decoder.h) and Decoder (decoder.h) is the facade both sit in.
//
// Packet outcomes are typed so "no data right now" never has to masquerade as end-of-stream:
// WouldBlock is a source with nothing for us yet (network EAGAIN, a live stream between packets),
// Eof is the real end, CorruptData is a demuxer-level bitstream hiccup (more input may demux
// fine), RecoverableIoError is a transport failure the caller can only answer with a reconnect.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

enum class MediaReadStatus
{
    Packet,
    Eof,
    WouldBlock,
    CorruptData,
    RecoverableIoError,
    Interrupted,
    FatalError,
};

struct MediaStreamInfo
{
    int video_index = -1;
    int audio_index = -1; // -1 == no audio stream (legitimate, silent)
    AVRational time_base{1, 1};
    double fps = 60.0;
    int coded_width = 0;
    int coded_height = 0;
    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
    // Container frame rate guess (avg_frame_rate), handed to the codec context.
    AVRational frame_rate{0, 1};
    // Sample aspect ratio (pixel width / pixel height): 1.0 for square pixels.
    double pixel_aspect = 1.0;
    // Clockwise display rotation (0/90/180/270) + horizontal mirror from the container's display
    // matrix. Presentation-only: the coded grid is untouched.
    int rotation_cw = 0;
    bool hflip = false;
    // Timeline length on the fixed-fps grid; 0 == genuinely unknown.
    int64_t frame_count = 0;
    bool is_network = false;
    // False for live/unknown-duration sources: a stall there means "wait", not "broken file".
    bool duration_known = false;
};

class MediaSource
{
public:
    MediaSource() = default;
    ~MediaSource();

    MediaSource(const MediaSource&) = delete;
    MediaSource& operator=(const MediaSource&) = delete;

    // `path` may be a local filesystem path or an http(s) URL. `headers`/`cookies` are the
    // optional HTTP request context a browser page captured for this URL; applied to the HTTP
    // protocol only, never for local files, never logged, and replayed by reconnect().
    bool open(const std::string& path, const std::string& headers, const std::string& cookies,
              std::string& error);
    void close();

    // Asks a blocked open()/read to bail out early: the AVIOInterruptCB installed on the format
    // context makes FFmpeg's next IO checkpoint fail. Atomic store only -- safe from any thread.
    void request_interrupt() { interrupt_requested_.store(true, std::memory_order_relaxed); }
    bool interrupt_requested() const
    {
        return interrupt_requested_.load(std::memory_order_relaxed);
    }
    static bool looks_like_network_url(const std::string& path);

    const MediaStreamInfo& info() const { return info_; }
    bool opened() const { return fmt_ctx_ != nullptr; }
    bool is_network() const { return info_.is_network; }

    // Copy of the selected video stream's codec parameters (owned here, stable across reconnect).
    const AVCodecParameters* video_codecpar() const { return video_params_; }

    // Adopts a mid-stream resolution change so reconnect() keeps validating against reality.
    void adopt_frame_size(int width, int height);

    // Reads the next packet of the VIDEO stream into `dst` (caller-owned, reused; unref'd
    // internally on every non-Packet outcome). Audio packets met along the way are queued for
    // Player's audio thread, stamped with `timeline_offset` (the decoder's current discontinuity
    // correction) so both tracks ride one timeline.
    MediaReadStatus read_video_packet(AVPacket* dst, double timeline_offset, std::string& error);

    // av_seek_frame(BACKWARD) on the video stream, `seconds` in the raw stream PTS domain.
    bool seek_to(double seconds, std::string& error);

    // Re-opens the same URL with the same request context and swaps the transport in. The codec is
    // untouched. VOD requires matching geometry and a seekable transport; live accepts a sliding
    // playlist and leaves geometry validation to the next decoded frame.
    bool reconnect(std::string& error, bool live = false);

    bool has_audio() const { return fmt_ctx_ && info_.audio_index >= 0; }
    AVCodecParameters* audio_codecpar() const;
    AVRational audio_time_base() const;

    // Pops one queued audio packet (FIFO) into `dst`, with the loop_offset stamped when it was
    // queued. Returns false when the queue is empty (dst left untouched). Thread-safe: this and
    // flush_audio_queue() are the only state shared with Player's audio thread.
    bool pop_audio_packet(AVPacket* dst, double& loop_offset_out);
    void flush_audio_queue();
    size_t audio_queue_depth() const;

private:
    struct AudioPacketEntry
    {
        AVPacket* pkt;
        double loop_offset;
    };

    // Bounded so a slow/stalled audio consumer can never grow the queue without limit. 512 packets
    // is ~10s of audio headroom even for small (1024-sample/48kHz AAC) frames.
    static constexpr size_t kAudioQueueMaxPackets = 512;

    static int interrupt_cb(void* opaque);
    // Network sources get timeout/reconnect/smaller probe; local files keep FFmpeg's defaults.
    void apply_network_options(AVDictionary** options) const;
    void resolve_display_transform(AVStream* stream);
    void queue_audio_packet(AVPacket* pkt, double timeline_offset);

    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecParameters* video_params_ = nullptr; // owned copy of the video stream's codecpar
    MediaStreamInfo info_{};

    // Set by the UI thread to abort a blocked network open()/read; cleared at the top of open()
    // because the same callback also gates av_read_frame() during normal decode.
    std::atomic<bool> interrupt_requested_{false};

    std::string path_;
    std::string http_headers_;
    std::string http_cookies_;

    mutable std::mutex audio_pkt_mutex_;
    std::deque<AudioPacketEntry> audio_pkt_queue_; // guarded by audio_pkt_mutex_
    uint64_t audio_pkt_dropped_ = 0;               // guarded by audio_pkt_mutex_
};
