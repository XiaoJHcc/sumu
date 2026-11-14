// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "media_source.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/dict.h>
}

MediaSource::~MediaSource()
{
    close();
}

int MediaSource::interrupt_cb(void* opaque)
{
    const MediaSource* self = static_cast<const MediaSource*>(opaque);
    return self->interrupt_requested_.load(std::memory_order_relaxed) ? 1 : 0;
}

bool MediaSource::looks_like_network_url(const std::string& path)
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

void MediaSource::apply_network_options(AVDictionary** options) const
{
    if (!info_.is_network) return;
    // rw_timeout is microseconds for the FFmpeg network protocols.
    av_dict_set(options, "rw_timeout", "15000000", 0); // 15s
    av_dict_set(options, "reconnect", "1", 0);
    av_dict_set(options, "reconnect_streamed", "1", 0);
    av_dict_set(options, "reconnect_on_network_error", "1", 0);
    av_dict_set(options, "reconnect_delay_max", "5", 0);
    // Faster open on remote: don't read multi-MB probes the way local defaults may.
    av_dict_set(options, "probesize", "1048576", 0);       // 1 MiB
    av_dict_set(options, "analyzeduration", "2000000", 0); // 2s (us)
    // Hint that HTTP Range seeks are expected (DirectPlay / static file servers).
    av_dict_set(options, "seekable", "1", 0);
    // Browser-page request context: FFmpeg's HTTP protocol takes extra header lines
    // ("Name: value\r\n"...) and the Cookie header value separately. Only non-empty values are
    // set, so a plain URL open keeps FFmpeg's defaults. Never echoed to the log.
    if (!http_headers_.empty()) av_dict_set(options, "headers", http_headers_.c_str(), 0);
    if (!http_cookies_.empty()) av_dict_set(options, "cookies", http_cookies_.c_str(), 0);
}

void MediaSource::resolve_display_transform(AVStream* stream)
{
    info_.rotation_cw = 0;
    info_.hflip = false;
    if (!stream || !stream->codecpar) return;

    // AV_PKT_DATA_DISPLAYMATRIX (libavutil/display.h) is the container's presentation transform,
    // stored as (a, b, u, c, d, v, x, y, w) 16.16 fixed point with
    //   p' = a*p + c*q + x,  q' = b*p + d*q + y
    // i.e. the linear part is [[a, c], [b, d]] in a y-down frame. It is the transform that has to
    // be applied to the decoded frame for correct presentation.
    const AVPacketSideData* side = av_packet_side_data_get(stream->codecpar->coded_side_data,
        stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (!side || side->size < 9 * static_cast<int>(sizeof(int32_t))) return;

    int32_t matrix[9] = {};
    std::memcpy(matrix, side->data, sizeof(matrix));
    const double a = matrix[0] / 65536.0;
    const double b = matrix[1] / 65536.0;
    const double c = matrix[3] / 65536.0;
    const double d = matrix[4] / 65536.0;
    const double det = a * d - b * c;
    if (!std::isfinite(det) || std::fabs(det) < 1e-6) {
        fprintf(stderr, "[sumu] ignoring singular display matrix\n");
        return;
    }
    // det < 0 means the matrix mirrors; decompose as rotate-then-display-flip, i.e.
    // A = R(theta) * F_hflip, which for a pure horizontal flip yields theta == 0.
    const double degrees = det > 0
        ? std::atan2(b, a) * 180.0 / 3.14159265358979323846
        : std::atan2(-b, -a) * 180.0 / 3.14159265358979323846;
    const double snapped = std::round(degrees / 90.0) * 90.0;
    if (std::fabs(degrees - snapped) > 2.0)
        fprintf(stderr, "[sumu] display matrix rotates %.2f degrees; snapping to %.0f "
            "(only axis-aligned rotations are supported)\n", degrees, snapped);
    int rotation = static_cast<int>(std::llround(snapped)) % 360;
    if (rotation < 0) rotation += 360;
    info_.rotation_cw = rotation;
    info_.hflip = det < 0;
}

bool MediaSource::open(const std::string& path, const std::string& headers,
                       const std::string& cookies, std::string& error)
{
    // Always start from a clean slate -- a previous partial failure must not leave half-allocated
    // FFmpeg contexts for the next attempt.
    close();
    path_ = path;
    info_.is_network = looks_like_network_url(path);
    // Keep the request context for reconnect() (a mid-playback reconnect must replay the same
    // Referer/Cookie or a hotlink-protected source answers 403). Never logged.
    http_headers_ = info_.is_network ? headers : std::string();
    http_cookies_ = info_.is_network ? cookies : std::string();
    interrupt_requested_.store(false, std::memory_order_relaxed);

    // Pre-allocate the format context so we can install AVIOInterruptCB BEFORE
    // avformat_open_input: request_interrupt() then aborts a blocked network open at FFmpeg's
    // next IO checkpoint instead of waiting out rw_timeout (URL page cancel).
    // avformat_close_input() frees the context on every exit path, opened or not.
    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        error = "avformat_alloc_context failed";
        return false;
    }
    fmt_ctx_->interrupt_callback.callback = &MediaSource::interrupt_cb;
    fmt_ctx_->interrupt_callback.opaque = this;

    // Options dict is consumed/owned by avformat_open_input on success; free on all paths.
    AVDictionary* fmt_opts = nullptr;
    apply_network_options(&fmt_opts);
    if (info_.is_network)
        fprintf(stderr, "[sumu] decoder open network URL (timeout/reconnect/shallow probe%s)\n",
            (http_headers_.empty() && http_cookies_.empty()) ? "" : ", page request context");

    if (avformat_open_input(&fmt_ctx_, path.c_str(), nullptr, &fmt_opts) < 0) {
        av_dict_free(&fmt_opts);
        error = "avformat_open_input failed for " + path;
        close();
        return false;
    }
    av_dict_free(&fmt_opts);

    if (info_.is_network && fmt_ctx_) {
        // Belt-and-suspenders: also clamp on the context in case a demuxer ignores dict keys.
        fmt_ctx_->probesize = 1 * 1024 * 1024;
        fmt_ctx_->max_analyze_duration = 2 * AV_TIME_BASE; // 2s
    }

    const int probe = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (interrupt_requested_.load(std::memory_order_relaxed)) {
        error = "video open cancelled";
        close();
        return false;
    }
    if (probe < 0)
        fprintf(stderr, "[sumu] incomplete stream probe (%d), trying available video metadata\n",
            probe);

    int idx = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (idx < 0) {
        error = "no video stream found";
        close();
        return false;
    }
    info_.video_index = idx;
    AVStream* stream = fmt_ctx_->streams[idx];
    info_.time_base = stream->time_base;
    AVRational rate = av_guess_frame_rate(fmt_ctx_, stream, nullptr);
    if (rate.num > 0 && rate.den > 0) info_.fps = av_q2d(rate);
    info_.frame_rate = stream->avg_frame_rate;
    info_.coded_width = stream->codecpar->width;
    info_.coded_height = stream->codecpar->height;
    info_.pix_fmt = static_cast<AVPixelFormat>(stream->codecpar->format);
    // Anamorphic sources: the coded grid is not the picture's shape. Prefer the video stream's own
    // sample_aspect_ratio, fall back to FFmpeg's guess (container/bitstream tags), and read a
    // missing or implausible ratio as square pixels -- a bogus 1:1000 tag would otherwise collapse
    // the picture into a sliver.
    AVRational sar = stream->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) sar = stream->codecpar->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0)
        sar = av_guess_sample_aspect_ratio(fmt_ctx_, stream, nullptr);
    info_.pixel_aspect = (sar.num > 0 && sar.den > 0) ? av_q2d(sar) : 1.0;
    if (!(info_.pixel_aspect > 0.25 && info_.pixel_aspect < 4.0)) info_.pixel_aspect = 1.0;
    if (info_.coded_width <= 0 || info_.coded_height <= 0 || info_.time_base.num <= 0 ||
        info_.time_base.den <= 0) {
        error = "video metadata is incomplete";
        close();
        return false;
    }

    resolve_display_transform(stream);

    // Audio (demux-only, best-effort): related_stream=idx nudges FFmpeg to prefer an audio stream
    // muxed alongside the chosen video stream when a container has several. No audio track is a
    // legitimate, silent result (-1), not an error.
    info_.audio_index = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, idx, nullptr, 0);

    double dur_s = -1.0;
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
        dur_s = stream->duration * av_q2d(stream->time_base);
    else if (fmt_ctx_->duration != AV_NOPTS_VALUE && fmt_ctx_->duration > 0)
        dur_s = static_cast<double>(fmt_ctx_->duration) / AV_TIME_BASE;
    info_.duration_known = dur_s > 0.0;
    info_.frame_count = dur_s > 0
        ? static_cast<int64_t>(std::llround(dur_s * info_.fps))
        : (stream->nb_frames > 0 ? stream->nb_frames : 0);

    // Own copy of the video codec parameters: the codec backend is configured from it, and it must
    // stay valid across reconnect() swapping the format context underneath.
    video_params_ = avcodec_parameters_alloc();
    if (!video_params_ || avcodec_parameters_copy(video_params_, stream->codecpar) < 0) {
        error = "failed to copy video codec parameters";
        close();
        return false;
    }
    return true;
}

void MediaSource::close()
{
    {
        // Free any audio packets still queued -- by the time close() runs, Player has already
        // joined both the decode thread (producer) and its own audio thread (consumer), so no
        // concurrent access is actually possible here; the lock is cheap insurance.
        std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
        while (!audio_pkt_queue_.empty()) {
            AudioPacketEntry e = audio_pkt_queue_.front();
            audio_pkt_queue_.pop_front();
            av_packet_free(&e.pkt);
        }
        audio_pkt_dropped_ = 0;
    }
    if (video_params_) avcodec_parameters_free(&video_params_);
    if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
    info_ = MediaStreamInfo{};
    path_.clear();
    http_headers_.clear();
    http_cookies_.clear();
}

void MediaSource::adopt_frame_size(int width, int height)
{
    info_.coded_width = width;
    info_.coded_height = height;
}

AVCodecParameters* MediaSource::audio_codecpar() const
{
    return has_audio() ? fmt_ctx_->streams[info_.audio_index]->codecpar : nullptr;
}

AVRational MediaSource::audio_time_base() const
{
    return has_audio() ? fmt_ctx_->streams[info_.audio_index]->time_base : AVRational{1, 1};
}

void MediaSource::queue_audio_packet(AVPacket* pkt, double timeline_offset)
{
    AVPacket* apkt = av_packet_alloc();
    if (!apkt) return;
    av_packet_move_ref(apkt, pkt);
    std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
    // Stamped with the discontinuity correction in effect at this demux position, so audio rides
    // the same timeline as the video frames decoded around it (a cut shifts both tracks together
    // instead of desyncing them).
    audio_pkt_queue_.push_back(AudioPacketEntry{apkt, timeline_offset});
    if (audio_pkt_queue_.size() > kAudioQueueMaxPackets) {
        AudioPacketEntry drop = audio_pkt_queue_.front();
        audio_pkt_queue_.pop_front();
        av_packet_free(&drop.pkt);
        ++audio_pkt_dropped_;
    }
}

MediaReadStatus MediaSource::read_video_packet(AVPacket* dst, double timeline_offset,
                                               std::string& error)
{
    if (!fmt_ctx_ || !dst || info_.video_index < 0) {
        error = "media source is not open";
        return MediaReadStatus::FatalError;
    }
    for (;;) {
        if (interrupt_requested_.load(std::memory_order_relaxed)) {
            av_packet_unref(dst);
            error = "decode interrupted";
            return MediaReadStatus::Interrupted;
        }
        const int read = av_read_frame(fmt_ctx_, dst);
        if (read >= 0) {
            if (dst->stream_index == info_.video_index) return MediaReadStatus::Packet;
            if (info_.audio_index >= 0 && dst->stream_index == info_.audio_index)
                queue_audio_packet(dst, timeline_offset);
            av_packet_unref(dst);
            continue;
        }
        av_packet_unref(dst);
        // A truncated local file ends with EIO: treat it as end-of-stream so the clock keeps
        // running on the pictures that really decoded (see docs/robustness_4e.md).
        if (read == AVERROR_EOF || read == AVERROR(EIO)) return MediaReadStatus::Eof;
        if (read == AVERROR(EAGAIN)) return MediaReadStatus::WouldBlock;
        if (read == AVERROR_EXIT) {
            error = "decode interrupted";
            return MediaReadStatus::Interrupted;
        }
        char message[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(read, message, sizeof(message));
        if (read == AVERROR_INVALIDDATA) {
            error = "av_read_frame: invalid data";
            return MediaReadStatus::CorruptData;
        }
        if (info_.is_network) {
            // Transport trouble is not a decoder fault: the caller may reconnect and resume.
            error = std::string("av_read_frame network error: ") + message;
            return MediaReadStatus::RecoverableIoError;
        }
        error = std::string("av_read_frame failed: ") + message;
        return MediaReadStatus::FatalError;
    }
}

bool MediaSource::seek_to(double seconds, std::string& error)
{
    if (!fmt_ctx_ || info_.video_index < 0) {
        error = "media source is not open";
        return false;
    }
    if (fmt_ctx_->pb) {
        // A previous EOF/EIO latches on the AVIOContext and would make the next read fail
        // immediately even though the seek moved the demuxer.
        fmt_ctx_->pb->eof_reached = 0;
        fmt_ctx_->pb->error = 0;
    }
    const double tb = av_q2d(info_.time_base);
    if (!(tb > 0.0)) {
        error = "media source has no usable time base";
        return false;
    }
    const int64_t ts = static_cast<int64_t>(std::llround(seconds / tb));
    if (av_seek_frame(fmt_ctx_, info_.video_index, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        error = "av_seek_frame failed";
        return false;
    }
    return true;
}

bool MediaSource::reconnect(std::string& error, bool live)
{
    if (!info_.is_network || !fmt_ctx_ || !fmt_ctx_->url ||
        (!live && !fmt_ctx_->pb) || (fmt_ctx_->iformat->flags & AVFMT_NOFILE)) {
        error = live ? "live input cannot be reopened" : "network input cannot be reconnected as a seekable file";
        return false;
    }
    const std::string url = fmt_ctx_->url;
    AVFormatContext* replacement = avformat_alloc_context();
    if (!replacement) {
        error = "network reconnect could not allocate a format context";
        return false;
    }
    replacement->interrupt_callback = fmt_ctx_->interrupt_callback;
    AVDictionary* options = nullptr;
    apply_network_options(&options);
    int ret = avformat_open_input(&replacement, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (ret >= 0) ret = avformat_find_stream_info(replacement, nullptr);
    if (ret < 0 || interrupt_requested_.load(std::memory_order_relaxed)) {
        char message[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret < 0 ? ret : AVERROR_EXIT, message, sizeof(message));
        error = std::string("network reconnect failed: ") + message;
        avformat_close_input(&replacement);
        return false;
    }
    const int video = av_find_best_stream(replacement, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audio = video >= 0
        ? av_find_best_stream(replacement, AVMEDIA_TYPE_AUDIO, -1, video, nullptr, 0)
        : -1;
    const AVCodecParameters* params =
        video >= 0 ? replacement->streams[video]->codecpar : nullptr;
    const AVCodecParameters* old_audio = audio_codecpar();
    const AVCodecParameters* new_audio =
        audio >= 0 ? replacement->streams[audio]->codecpar : nullptr;
    const bool audio_matches = old_audio && new_audio
        ? old_audio->codec_id == new_audio->codec_id &&
          old_audio->sample_rate == new_audio->sample_rate &&
          av_channel_layout_compare(&old_audio->ch_layout, &new_audio->ch_layout) == 0 &&
          av_cmp_q(audio_time_base(), replacement->streams[audio]->time_base) == 0
        : !old_audio && !new_audio;
    if (!params || !video_params_ || params->codec_id != video_params_->codec_id ||
        (!live && (params->width != info_.coded_width || params->height != info_.coded_height)) ||
        av_cmp_q(info_.time_base, replacement->streams[video]->time_base) != 0 || !audio_matches ||
        (!live && !(replacement->pb && (replacement->pb->seekable & AVIO_SEEKABLE_NORMAL)))) {
        error = live ? "live source changed codec or audio format" : "network source changed format or is no longer seekable";
        avformat_close_input(&replacement);
        return false;
    }
    // Copy the fresh codec parameters BEFORE swapping the transport: avcodec_parameters_copy
    // frees/reallocates its inner buffers, so a failure there must not leave the session holding
    // half-written parameters for a demuxer that is already gone.
    AVCodecParameters* params_copy = avcodec_parameters_alloc();
    if (!params_copy) {
        error = "network reconnect could not allocate codec parameters";
        avformat_close_input(&replacement);
        return false;
    }
    if (avcodec_parameters_copy(params_copy, params) < 0) {
        error = "network reconnect could not copy codec parameters";
        avcodec_parameters_free(&params_copy);
        avformat_close_input(&replacement);
        return false;
    }
    // Demuxers can retain their own IO pointers; replace the transport and demuxer together.
    avformat_close_input(&fmt_ctx_);
    fmt_ctx_ = replacement;
    info_.video_index = video;
    info_.audio_index = audio;
    if (live) {
        info_.coded_width = params->width;
        info_.coded_height = params->height;
    }
    avcodec_parameters_free(&video_params_);
    video_params_ = params_copy;
    flush_audio_queue();
    return true;
}

bool MediaSource::pop_audio_packet(AVPacket* dst, double& loop_offset_out)
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

void MediaSource::flush_audio_queue()
{
    std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
    while (!audio_pkt_queue_.empty()) {
        AudioPacketEntry e = audio_pkt_queue_.front();
        audio_pkt_queue_.pop_front();
        av_packet_free(&e.pkt);
    }
}

size_t MediaSource::audio_queue_depth() const
{
    std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
    return audio_pkt_queue_.size();
}
