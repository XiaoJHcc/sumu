// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "decoder.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

Decoder::~Decoder()
{
    close();
}

void Decoder::close()
{
    {
        // Free any audio packets still sitting in the queue -- by the time close() runs,
        // Player has already joined both the decode thread (producer) and its own audio
        // thread (consumer), so no concurrent access is actually possible here; the lock is
        // just cheap insurance, consistent with the rest of this class's style.
        std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
        while (!audio_pkt_queue_.empty()) {
            AudioPacketEntry e = audio_pkt_queue_.front();
            audio_pkt_queue_.pop_front();
            av_packet_free(&e.pkt);
        }
    }
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (hw_device_ref_) av_buffer_unref(&hw_device_ref_);
    if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
    video_stream_idx_ = -1;
    audio_stream_idx_ = -1;
    // Reset session-derived state so a subsequent open() (reopen path, or a retry after a
    // partial open failure) starts from a clean slate -- without this, have_first_pts_ stays
    // true and the next session would inherit the previous file's PTS origin (I5 break).
    fps_ = 60.0;
    width_ = 0;
    height_ = 0;
    frame_count_ = 0;
    have_first_pts_.store(false, std::memory_order_relaxed);
    first_pts_seconds_.store(0.0, std::memory_order_relaxed);
    loop_offset_seconds_ = 0.0;
    last_out_pts_seconds_ = -1.0;
    d3d_device_ = nullptr;
    is_network_ = false;
}

AVPixelFormat Decoder::get_hw_format_thunk(AVCodecContext* ctx, const AVPixelFormat* fmts)
{
    Decoder* self = static_cast<Decoder*>(ctx->opaque);
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) {
            if (self->ensure_hw_frames_ctx(ctx))
                return AV_PIX_FMT_D3D11;
        }
    }
    return AV_PIX_FMT_NONE;
}

bool Decoder::ensure_hw_frames_ctx(AVCodecContext* ctx)
{
    if (ctx->hw_frames_ctx)
        return true;

    AVBufferRef* frames_ref = nullptr;
    int ret = avcodec_get_hw_frames_parameters(ctx, hw_device_ref_, AV_PIX_FMT_D3D11, &frames_ref);
    if (ret < 0) {
        fprintf(stderr, "avcodec_get_hw_frames_parameters failed: %d\n", ret);
        return false;
    }

    AVHWFramesContext* frames_ctx = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
    AVD3D11VAFramesContext* frames_hwctx = reinterpret_cast<AVD3D11VAFramesContext*>(frames_ctx->hwctx);

    // We need to be able to build an SRV directly on the decoder's own texture array
    // slices -- that's the whole point of sharing the D3D11 device with the swapchain (no
    // readback, no copy). D3D11_BIND_DECODER is already implied by the decoder; add
    // SHADER_RESOURCE so the same texture can be bound as a shader input.
    frames_hwctx->BindFlags |= D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    frames_hwctx->MiscFlags = 0;

    // A couple of extra slices of headroom beyond what the decoder asked for so the
    // present/copy side reading the "current" slice never races a decoder that wants to
    // reuse it immediately.
    if (frames_ctx->initial_pool_size > 0)
        frames_ctx->initial_pool_size += 4;

    ret = av_hwframe_ctx_init(frames_ref);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "av_hwframe_ctx_init failed: %s\n", errbuf);
        av_buffer_unref(&frames_ref);
        return false;
    }

    ctx->hw_frames_ctx = frames_ref; // transfers ownership
    return true;
}

bool Decoder::looks_like_network_url(const std::string& path)
{
    // Minimal scheme check for the network IO profile. Emby/Jellyfin DirectPlay and plain
    // http.server both land here; file paths and UNC shares stay on the local profile.
    // Case-insensitive prefix only -- no full URL parse (FFmpeg does the real open).
    auto starts_with_ci = [](const std::string& s, const char* prefix) {
        const size_t n = std::strlen(prefix);
        if (s.size() < n) return false;
        for (size_t i = 0; i < n; ++i) {
            if (std::tolower(static_cast<unsigned char>(s[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        }
        return true;
    };
    return starts_with_ci(path, "http://") || starts_with_ci(path, "https://");
}

bool Decoder::open(const std::string& path, ID3D11Device* device, std::string& error)
{
    // Always start from a clean slate -- a previous partial failure (or a reopen after
    // close_session) must not leave half-allocated FFmpeg contexts for the next attempt.
    close();
    d3d_device_ = device;
    is_network_ = looks_like_network_url(path);

    // Network sources: timeout + reconnect + smaller probe. Local files keep FFmpeg defaults
    // (deep probe is fine on disk and preserves existing open latency characteristics).
    // Options dict is consumed/owned by avformat_open_input on success; free on all paths.
    AVDictionary* fmt_opts = nullptr;
    if (is_network_) {
        // rw_timeout is microseconds for the FFmpeg network protocols.
        av_dict_set(&fmt_opts, "rw_timeout", "15000000", 0);          // 15s
        av_dict_set(&fmt_opts, "reconnect", "1", 0);
        av_dict_set(&fmt_opts, "reconnect_streamed", "1", 0);
        av_dict_set(&fmt_opts, "reconnect_on_network_error", "1", 0);
        av_dict_set(&fmt_opts, "reconnect_delay_max", "5", 0);
        // Faster open on remote: don't read multi-MB probes the way local defaults may.
        av_dict_set(&fmt_opts, "probesize", "1048576", 0);            // 1 MiB
        av_dict_set(&fmt_opts, "analyzeduration", "2000000", 0);      // 2s (µs)
        // Hint that HTTP Range seeks are expected (DirectPlay / static file servers).
        av_dict_set(&fmt_opts, "seekable", "1", 0);
        fprintf(stderr, "[sumu] decoder open network URL (timeout/reconnect/shallow probe)\n");
    }

    if (avformat_open_input(&fmt_ctx_, path.c_str(), nullptr, &fmt_opts) < 0) {
        av_dict_free(&fmt_opts);
        error = "avformat_open_input failed for " + path;
        close();
        return false;
    }
    av_dict_free(&fmt_opts);

    if (is_network_ && fmt_ctx_) {
        // Belt-and-suspenders: also clamp on the context in case a demuxer ignores dict keys.
        fmt_ctx_->probesize = 1 * 1024 * 1024;
        fmt_ctx_->max_analyze_duration = 2 * AV_TIME_BASE; // 2s
    }

    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        error = "avformat_find_stream_info failed";
        close();
        return false;
    }

    const AVCodec* decoder = nullptr;
    int idx = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (idx < 0) {
        error = "no video stream found";
        close();
        return false;
    }
    video_stream_idx_ = idx;
    AVStream* stream = fmt_ctx_->streams[idx];
    time_base_ = stream->time_base;
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
        fps_ = av_q2d(stream->avg_frame_rate);
    width_ = stream->codecpar->width;
    height_ = stream->codecpar->height;

    // Audio (spike, additive): demux-only, best-effort. related_stream=idx nudges FFmpeg to
    // prefer an audio stream muxed alongside the chosen video stream when a container has
    // several. No audio track is a legitimate, silent result (-1), not an error -- callers
    // (Player::open()) must treat has_audio()==false the same as "no audio in this file".
    audio_stream_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, idx, nullptr, 0);

    // Best-effort total frame count: prefer the container's own count, else derive from
    // duration * fps. Used only for clamping seek targets -- 0 (unknown) is handled by
    // callers as "no known upper bound", not a fatal condition.
    if (stream->nb_frames > 0) {
        frame_count_ = stream->nb_frames;
    } else {
        double dur_s = -1.0;
        if (stream->duration != AV_NOPTS_VALUE)
            dur_s = stream->duration * av_q2d(stream->time_base);
        else if (fmt_ctx_->duration != AV_NOPTS_VALUE)
            dur_s = static_cast<double>(fmt_ctx_->duration) / AV_TIME_BASE;
        frame_count_ = (dur_s > 0) ? static_cast<int64_t>(std::llround(dur_s * fps_)) : 0;
    }

    codec_ctx_ = avcodec_alloc_context3(decoder);
    if (!codec_ctx_) {
        error = "avcodec_alloc_context3 failed";
        close();
        return false;
    }
    if (avcodec_parameters_to_context(codec_ctx_, stream->codecpar) < 0) {
        error = "avcodec_parameters_to_context failed";
        close();
        return false;
    }

    // Build an AVHWDeviceContext wrapping OUR d3d device instead of letting FFmpeg create
    // its own -- this is what makes the decoded texture live on the same device as our
    // swapchain so we can sample/copy it with zero copy.
    hw_device_ref_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_device_ref_) {
        error = "av_hwdevice_ctx_alloc failed";
        close();
        return false;
    }
    AVHWDeviceContext* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_ref_->data);
    AVD3D11VADeviceContext* d3d11_ctx = reinterpret_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);
    d3d_device_->AddRef();
    d3d11_ctx->device = d3d_device_;

    if (av_hwdevice_ctx_init(hw_device_ref_) < 0) {
        error = "av_hwdevice_ctx_init failed";
        close();
        return false;
    }

    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ref_);
    codec_ctx_->opaque = this;
    codec_ctx_->get_format = &Decoder::get_hw_format_thunk;
    codec_ctx_->thread_count = 1; // single-threaded decode: keeps this on one D3D11 call sequence

    AVDictionary* opts = nullptr;
    if (avcodec_open2(codec_ctx_, decoder, &opts) < 0) {
        av_dict_free(&opts);
        error = "avcodec_open2 failed";
        close();
        return false;
    }
    av_dict_free(&opts);

    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    return true;
}


bool Decoder::seek_to_start()
{
    avcodec_flush_buffers(codec_ctx_);
    int ret = av_seek_frame(fmt_ctx_, video_stream_idx_, 0, AVSEEK_FLAG_BACKWARD);
    return ret >= 0;
}

int Decoder::pump_one_raw_frame(DecodedFrame& out, double& raw_pts_s_out)
{
    for (;;) {
        av_frame_unref(frame_); // release previous frame's ref on the decoder pool slice

        int recv = avcodec_receive_frame(codec_ctx_, frame_);
        if (recv == 0) {
            if (frame_->format != AV_PIX_FMT_D3D11) {
                fprintf(stderr, "unexpected non-hw frame format %d\n", frame_->format);
                continue;
            }
            out.texture = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
            out.array_slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame_->data[1]));
            raw_pts_s_out = (frame_->pts != AV_NOPTS_VALUE)
                ? frame_->pts * av_q2d(time_base_)
                : (last_out_pts_seconds_ + 1.0 / fps_);
            return 1;
        }
        if (recv == AVERROR(EAGAIN)) {
            int read = av_read_frame(fmt_ctx_, pkt_);
            if (read >= 0) {
                if (pkt_->stream_index == video_stream_idx_) {
                    int send = avcodec_send_packet(codec_ctx_, pkt_);
                    if (send < 0 && send != AVERROR(EAGAIN)) {
                        av_packet_unref(pkt_);
                        fprintf(stderr, "avcodec_send_packet failed: %d\n", send);
                        return -1;
                    }
                } else if (audio_stream_idx_ >= 0 && pkt_->stream_index == audio_stream_idx_) {
                    // Demux-only: hand this packet off to Player's audio thread via the
                    // bounded queue instead of decoding it here (see file header / decoder.h).
                    // av_packet_move_ref() resets pkt_ to empty, so the generic
                    // av_packet_unref(pkt_) below is a harmless no-op for this branch.
                    AVPacket* apkt = av_packet_alloc();
                    if (apkt) {
                        av_packet_move_ref(apkt, pkt_);
                        std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
                        audio_pkt_queue_.push_back(AudioPacketEntry{ apkt, loop_offset_seconds_ });
                        if (audio_pkt_queue_.size() > kAudioQueueMaxPackets) {
                            AudioPacketEntry drop = audio_pkt_queue_.front();
                            audio_pkt_queue_.pop_front();
                            av_packet_free(&drop.pkt);
                            ++audio_pkt_dropped_;
                        }
                    }
                }
                av_packet_unref(pkt_);
                continue;
            }
            // EOF: flush the decoder and try to drain one last frame.
            avcodec_send_packet(codec_ctx_, nullptr);
            int drain = avcodec_receive_frame(codec_ctx_, frame_);
            if (drain == 0) {
                out.texture = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
                out.array_slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame_->data[1]));
                raw_pts_s_out = (frame_->pts != AV_NOPTS_VALUE)
                    ? frame_->pts * av_q2d(time_base_)
                    : (last_out_pts_seconds_ + 1.0 / fps_);
                return 1; // caller sees this as a normal frame; next pump call will see EOF again
            }
            return 0; // genuinely nothing left
        }
        if (recv == AVERROR_EOF) {
            // Decoder is fully drained (this is the steady-state EOF signal once the drain
            // path above has already run out): genuinely nothing left, same as the drain==0
            // case a few lines up. Must NOT fall through to the generic failure below.
            return 0;
        }
        fprintf(stderr, "avcodec_receive_frame failed: %d\n", recv);
        return -1;
    }
}

bool Decoder::next_frame(DecodedFrame& out)
{
    double raw_pts_s = 0.0;
    int r = pump_one_raw_frame(out, raw_pts_s);
    if (r < 0) return false;
    if (r == 0) {
        // EOF: do NOT loop back to t=0. Product policy is pause-on-last-frame (present_loop
        // clamps the clock and clears playing_); decode_loop treats false as "idle until seek
        // repositions". Loop-on-EOF used to bump loop_offset_seconds_ past the real timeline,
        // so the seekbar clock ran past frame_count and seek targets mapped into the wrong
        // PTS domain.
        return false;
    }

    if (!have_first_pts_) {
        first_pts_seconds_ = raw_pts_s;
        have_first_pts_ = true;
    }
    double pts_s = loop_offset_seconds_ + (raw_pts_s - first_pts_seconds_);
    out.pts_seconds = pts_s;
    last_out_pts_seconds_ = pts_s;
    return true;
}

bool Decoder::seek_to_frame(int64_t target_frame, DecodedFrame& out, std::string& error,
                            bool nearest_keyframe)
{
    if (!have_first_pts_) {
        error = "seek_to_frame called before any frame decoded (no PTS origin yet)";
        return false;
    }
    if (target_frame < 0) target_frame = 0;

    // Map the requested content frame number back into the raw stream PTS domain using the
    // ORIGINAL first_pts_seconds_/loop_offset_ established at open() -- frame numbers stay
    // globally anchored to that one origin across any number of seeks (I5). A seek never
    // touches loop_offset_seconds_/first_pts_seconds_ (loop_offset stays 0 for the session
    // under pause-on-last-frame; the term remains in the formula for a single code path).
    double target_seconds_local = static_cast<double>(target_frame) / fps_; // in loop_offset_ domain
    double target_seconds_raw = (target_seconds_local - loop_offset_seconds_) + first_pts_seconds_;
    int64_t seek_ts = static_cast<int64_t>(std::llround(target_seconds_raw / av_q2d(time_base_)));

    avcodec_flush_buffers(codec_ctx_);
    int ret = av_seek_frame(fmt_ctx_, video_stream_idx_, seek_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        error = "av_seek_frame failed";
        return false;
    }

    // Decode forward from the keyframe av_seek_frame landed on until the REAL decoded PTS
    // (not the nominal target) reaches target_frame (I5). Bounded loop: a HEVC GOP is
    // essentially never long enough to need more than a few hundred frames of forward decode
    // even on a deep seek, since av_seek_frame itself did the O(log n) jump to the nearest
    // keyframe regardless of how far away target_frame is.
    // Coarse-preview fast path: take the keyframe av_seek_frame landed on (one decode) and stop.
    if (nearest_keyframe) {
        double raw_pts_s = 0.0;
        DecodedFrame df;
        int r = pump_one_raw_frame(df, raw_pts_s);
        if (r <= 0) { error = "nearest_keyframe: no frame decoded after seek"; return false; }
        double pts_s = loop_offset_seconds_ + (raw_pts_s - first_pts_seconds_);
        last_out_pts_seconds_ = pts_s;
        out = df;
        out.pts_seconds = pts_s;
        return true;
    }

    constexpr int kMaxForwardFrames = 600;
    bool have_any = false;
    DecodedFrame last_good{};
    double last_good_pts_s = 0.0;

    for (int i = 0; i < kMaxForwardFrames; ++i) {
        double raw_pts_s = 0.0;
        DecodedFrame df;
        int r = pump_one_raw_frame(df, raw_pts_s);
        if (r < 0) {
            error = "pump_one_raw_frame failed during seek_to_frame";
            return false;
        }
        if (r == 0) {
            // Hit EOF while decoding forward toward target_frame (near-tail seek): clamp to
            // whatever we already have instead of failing the whole seek.
            break;
        }
        double pts_s = loop_offset_seconds_ + (raw_pts_s - first_pts_seconds_);
        last_out_pts_seconds_ = pts_s;
        last_good = df;
        last_good_pts_s = pts_s;
        have_any = true;

        int64_t derived_frame = static_cast<int64_t>(std::llround(pts_s * fps_));
        if (derived_frame >= target_frame) {
            out = df;
            out.pts_seconds = pts_s;
            return true;
        }
    }

    if (have_any) {
        // Ran out of forward-decode budget or hit EOF before reaching target_frame exactly
        // -- land on the last frame we actually got (near-tail-of-video or pathologically
        // long-GOP case). Graceful degradation, never a hard failure.
        out = last_good;
        out.pts_seconds = last_good_pts_s;
        return true;
    }

    error = "seek_to_frame: no frame could be decoded after seek";
    return false;
}

bool Decoder::pop_audio_packet(AVPacket* dst, double& loop_offset_out)
{
    std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
    if (audio_pkt_queue_.empty()) return false;
    AudioPacketEntry e = audio_pkt_queue_.front();
    audio_pkt_queue_.pop_front();
    av_packet_move_ref(dst, e.pkt);
    av_packet_free(&e.pkt);
    loop_offset_out = e.loop_offset;
    return true;
}

void Decoder::flush_audio_queue()
{
    std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
    while (!audio_pkt_queue_.empty()) {
        AudioPacketEntry e = audio_pkt_queue_.front();
        audio_pkt_queue_.pop_front();
        av_packet_free(&e.pkt);
    }
}

size_t Decoder::audio_queue_depth() const
{
    std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
    return audio_pkt_queue_.size();
}
