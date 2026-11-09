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
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace {

bool is_corrupt_media_message(const char* format)
{
    static const char* patterns[] = {
        "Invalid NAL unit size", "missing picture in access unit",
        "Error splitting the input into NAL units", "co located POCs unavailable",
        "mmco: unref short failure", "reference picture missing during reorder",
        "Missing reference picture", "decode_slice_header error", "no frame!",
        "error while decoding MB", "concealing ", "corrupt decoded frame",
        "Packet corrupt", "partial file", "File ended prematurely",
    };
    for (const char* pattern : patterns)
        if (std::strstr(format, pattern)) return true;
    return false;
}

}

thread_local Decoder* Decoder::logging_decoder_ = nullptr;

class Decoder::LogScope
{
public:
    explicit LogScope(Decoder& decoder) : decoder_(decoder), previous_(logging_decoder_)
    {
        logging_decoder_ = &decoder_;
    }
    ~LogScope()
    {
        decoder_.flush_corrupt_logs();
        logging_decoder_ = previous_;
    }
private:
    Decoder& decoder_;
    Decoder* previous_;
};

void Decoder::log_callback(void* context, int level, const char* format, va_list args)
{
    if (level > av_log_get_level()) return;
    if (logging_decoder_ && (level == AV_LOG_WARNING || level == AV_LOG_ERROR) &&
        format && is_corrupt_media_message(format)) {
        char message[512] = {};
        if (logging_decoder_->corrupt_messages_ == 0 && logging_decoder_->corrupt_packets_ == 0)
            vsnprintf(message, sizeof(message), format, args);
        logging_decoder_->record_corrupt_data(message);
        return;
    }
    av_log_default_callback(context, level, format, args);
}

void Decoder::record_corrupt_data(const char* message, bool packet_error)
{
    const bool first = corrupt_messages_ == 0 && corrupt_packets_ == 0;
    if (packet_error) ++corrupt_packets_;
    else ++corrupt_messages_;
    if (first) {
        const size_t length = std::strcspn(message, "\r\n");
        fprintf(stderr, "[sumu] recoverable media data: first: %.*s "
            "(ffmpeg_messages=%llu packet_errors=%llu; summaries every 5s)\n",
            static_cast<int>(length), message,
            static_cast<unsigned long long>(corrupt_messages_),
            static_cast<unsigned long long>(corrupt_packets_));
        reported_messages_ = corrupt_messages_;
        reported_packets_ = corrupt_packets_;
        next_log_summary_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    } else {
        flush_corrupt_logs();
    }
}

void Decoder::flush_corrupt_logs(bool force)
{
    if (corrupt_messages_ == reported_messages_ && corrupt_packets_ == reported_packets_)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < next_log_summary_) return;
    fprintf(stderr, "[sumu] recoverable media data: summary: ffmpeg_messages=%llu "
        "packet_errors=%llu%s\n", static_cast<unsigned long long>(corrupt_messages_),
        static_cast<unsigned long long>(corrupt_packets_), force ? " (session closed)" : "");
    reported_messages_ = corrupt_messages_;
    reported_packets_ = corrupt_packets_;
    next_log_summary_ = now + std::chrono::seconds(5);
}

Decoder::~Decoder()
{
    close();
}

int Decoder::interrupt_cb(void* opaque)
{
    const Decoder* self = static_cast<const Decoder*>(opaque);
    return self->interrupt_requested_.load(std::memory_order_relaxed) ? 1 : 0;
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
    normalizer_.reset();
    needs_normalization_ = false;
    normalize_desc_ = {};
    coded_width_ = 0;
    coded_height_ = 0;
    display_rotation_cw_ = 0;
    display_hflip_ = false;
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (hw_device_ref_) av_buffer_unref(&hw_device_ref_);
    if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
    flush_corrupt_logs(true);
    corrupt_messages_ = 0;
    corrupt_packets_ = 0;
    reported_messages_ = 0;
    reported_packets_ = 0;
    next_log_summary_ = {};
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
    at_eof_.store(false, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    draining_ = false;
    loop_offset_seconds_ = 0.0;
    last_out_pts_seconds_ = -1.0;
    last_raw_pts_seconds_ = 0.0;
    discontinuity_offset_ = 0.0;
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

void Decoder::resolve_display_transform(AVStream* stream)
{
    display_rotation_cw_ = 0;
    display_hflip_ = false;
    if (!stream || !stream->codecpar) return;

    // AV_PKT_DATA_DISPLAYMATRIX (libavutil/display.h) is the container's presentation
    // transform, stored as (a, b, u, c, d, v, x, y, w) 16.16 fixed point with
    //   p' = a*p + c*q + x,  q' = b*p + d*q + y
    // i.e. the linear part is [[a, c], [b, d]] in a y-down frame. It is the transform that has
    // to be applied to the decoded frame for correct presentation.
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
    display_rotation_cw_ = rotation;
    display_hflip_ = det < 0;
}

bool Decoder::configure_frame_normalization(std::string& error)
{
    const AVPixelFormat coded_fmt =
        static_cast<AVPixelFormat>(fmt_ctx_->streams[video_stream_idx_]->codecpar->format);
    const bool ten_bit = coded_fmt == AV_PIX_FMT_YUV420P10LE || coded_fmt == AV_PIX_FMT_P010LE;

    // Range: an unmarked stream is limited, matching the (already untagged) 709 assumption
    // below; the legacy JPEG-range pixel formats carry it implicitly.
    bool full_range = codec_ctx_->color_range == AVCOL_RANGE_JPEG;
    if (codec_ctx_->color_range == AVCOL_RANGE_UNSPECIFIED)
        full_range = coded_fmt == AV_PIX_FMT_YUVJ420P;
    else if (codec_ctx_->color_range != AVCOL_RANGE_MPEG && codec_ctx_->color_range != AVCOL_RANGE_JPEG)
        full_range = coded_fmt == AV_PIX_FMT_YUVJ420P;

    // Matrix: an untagged stream keeps the historical BT.709 contract, an explicit BT.470BG/
    // SMPTE 170M tag is honoured as BT.601.
    VideoMatrix matrix_kind = VideoMatrix::Bt709;
    switch (codec_ctx_->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        matrix_kind = VideoMatrix::Bt601;
        break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: // constant-luminance content is vanishingly rare; treat as NCL
        matrix_kind = VideoMatrix::Bt2020;
        break;
    default:
        break;
    }

    VideoTransfer transfer = VideoTransfer::Sdr;
    if (codec_ctx_->color_trc == AVCOL_TRC_SMPTE2084)
        transfer = VideoTransfer::Pq;
    else if (codec_ctx_->color_trc == AVCOL_TRC_ARIB_STD_B67)
        transfer = VideoTransfer::Hlg;

    // Primaries: an unmarked HDR stream is BT.2020 by convention, an unmarked SDR stream stays
    // BT.709 (no primary conversion, which is what the present shader has always assumed).
    bool primaries_2020 = codec_ctx_->color_primaries == AVCOL_PRI_BT2020;
    if (codec_ctx_->color_primaries == AVCOL_PRI_UNSPECIFIED)
        primaries_2020 = transfer != VideoTransfer::Sdr || matrix_kind == VideoMatrix::Bt2020;

    normalize_desc_ = {};
    normalize_desc_.src_width = coded_width_;
    normalize_desc_.src_height = coded_height_;
    normalize_desc_.bit_depth = ten_bit ? 10 : 8;
    normalize_desc_.full_range = full_range;
    normalize_desc_.matrix = matrix_kind;
    normalize_desc_.transfer = transfer;
    normalize_desc_.primaries_2020 = primaries_2020;
    normalize_desc_.rotation_cw = display_rotation_cw_;
    normalize_desc_.hflip = display_hflip_;
    normalize_desc_.fps = fps_;
    const bool swap = (display_rotation_cw_ % 180) != 0;
    width_ = swap ? coded_height_ : coded_width_;
    height_ = swap ? coded_width_ : coded_height_;
    normalize_desc_.dst_width = width_;
    normalize_desc_.dst_height = height_;

    needs_normalization_ = video_normalize_required(normalize_desc_);
    if (!needs_normalization_) return true;

    if (!normalizer_.initialize(d3d_device_, normalize_desc_, error))
        return false;
    if (display_rotation_cw_ != 0 || display_hflip_)
        fprintf(stderr, "[sumu] display matrix: rotate %d deg clockwise%s, %dx%d -> %dx%d\n",
            display_rotation_cw_, display_hflip_ ? " + horizontal flip" : "",
            coded_width_, coded_height_, width_, height_);
    return true;
}

bool Decoder::prepare_nv12_frame(DecodedFrame& frame, std::string& error)
{
    if (!needs_normalization_) return true; // already the downstream contract
    if (!normalizer_.process(frame.texture, frame.array_slice, error)) return false;
    frame.texture = normalizer_.output_texture();
    frame.array_slice = 0;
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
    static std::once_flag log_once;
    std::call_once(log_once, [] { av_log_set_callback(&Decoder::log_callback); });
    LogScope log_scope(*this);
    // Always start from a clean slate -- a previous partial failure (or a reopen after
    // close_session) must not leave half-allocated FFmpeg contexts for the next attempt.
    close();
    d3d_device_ = device;
    is_network_ = looks_like_network_url(path);
    // Fresh slate for the interrupt flag -- the callback installed below also gates
    // av_read_frame() during normal decode, so a stale "cancelled" from a previous aborted
    // open must not leak into the new session.
    interrupt_requested_.store(false, std::memory_order_relaxed);

    // Pre-allocate the format context so we can install AVIOInterruptCB BEFORE
    // avformat_open_input: request_interrupt() then aborts a blocked network open at
    // FFmpeg's next IO checkpoint instead of waiting out rw_timeout (URL float cancel).
    // avformat_close_input() frees the context on every exit path, opened or not.
    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        error = "avformat_alloc_context failed";
        return false;
    }
    fmt_ctx_->interrupt_callback.callback = &Decoder::interrupt_cb;
    fmt_ctx_->interrupt_callback.opaque = this;

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

    const int probe = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (interrupt_requested_.load(std::memory_order_relaxed)) {
        error = "video open cancelled";
        close();
        return false;
    }
    if (probe < 0)
        fprintf(stderr, "[sumu] incomplete stream probe (%d), trying available video metadata\n", probe);

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
    AVRational rate = av_guess_frame_rate(fmt_ctx_, stream, nullptr);
    if (rate.num > 0 && rate.den > 0)
        fps_ = av_q2d(rate);
    coded_width_ = stream->codecpar->width;
    coded_height_ = stream->codecpar->height;
    width_ = coded_width_;
    height_ = coded_height_;
    if (width_ <= 0 || height_ <= 0 || time_base_.num <= 0 || time_base_.den <= 0) {
        error = "video metadata is incomplete";
        close();
        return false;
    }

    // 下游环缓冲/scrub/AI 桥/headless 统一消费 8-bit BT.709 limited NV12。8-bit 4:2:0 与
    // 10-bit 4:2:0（P010）都接入同一契约：后者由 VideoNormalizer 在 GPU 上按色彩 tag 归一化
    // （含 HDR tone mapping / 旋转），不是简单降位深；其余 4:2:2 / 4:4:4 等格式在 open 路径
    // 显式拒绝，避免 CopySubresourceRegion 不转换格式而静默黑屏。判定用 codecpar->format：
    // avformat_find_stream_info 后 parser 已确定位深/色度格式，codec_ctx_->sw_pix_fmt
    // 此时仍可能未确定。
    {
        const AVPixelFormat coded_fmt = static_cast<AVPixelFormat>(stream->codecpar->format);
        const bool ten_bit_420 = coded_fmt == AV_PIX_FMT_YUV420P10LE ||
            coded_fmt == AV_PIX_FMT_P010LE;
        if (coded_fmt != AV_PIX_FMT_NV12 &&
            coded_fmt != AV_PIX_FMT_YUV420P &&
            coded_fmt != AV_PIX_FMT_YUVJ420P &&
            !ten_bit_420) {
            const char* fmt_name = av_get_pix_fmt_name(coded_fmt);
            error = "unsupported_pix_fmt:" + std::string(fmt_name ? fmt_name : "unknown");
            close();
            return false;
        }
    }

    resolve_display_transform(stream);

    // Audio (spike, additive): demux-only, best-effort. related_stream=idx nudges FFmpeg to
    // prefer an audio stream muxed alongside the chosen video stream when a container has
    // several. No audio track is a legitimate, silent result (-1), not an error -- callers
    // (Player::open()) must treat has_audio()==false the same as "no audio in this file".
    audio_stream_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, idx, nullptr, 0);

    {
        double dur_s = -1.0;
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
            dur_s = stream->duration * av_q2d(stream->time_base);
        else if (fmt_ctx_->duration != AV_NOPTS_VALUE && fmt_ctx_->duration > 0)
            dur_s = static_cast<double>(fmt_ctx_->duration) / AV_TIME_BASE;
        frame_count_ = dur_s > 0
            ? static_cast<int64_t>(std::llround(dur_s * fps_))
            : std::max<int64_t>(0, stream->nb_frames);
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
    // Required for correct pts/dts → frame timestamp conversion (esp. B-frames). Without this,
    // files whose container stamps PTS==DTS despite has_b_frames>0 can leave frame->pts in
    // decode order; best_effort_timestamp then can't recover a presentation timeline either.
    codec_ctx_->pkt_timebase = stream->time_base;
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
        codec_ctx_->framerate = stream->avg_frame_rate;

    AVDictionary* opts = nullptr;
    if (avcodec_open2(codec_ctx_, decoder, &opts) < 0) {
        av_dict_free(&opts);
        error = "avcodec_open2 failed";
        close();
        return false;
    }
    av_dict_free(&opts);

    // Frame contract: everything downstream (present / AI / headless / scrub) consumes
    // display-oriented BT.709 limited 8-bit NV12. Build the GPU normalizer here when the source
    // is anything else (10-bit, full range, non-709 matrix, HDR, display-matrix rotation) so an
    // unsupported combination fails the open with a reason instead of producing a wrong image.
    if (!configure_frame_normalization(error)) {
        error = "frame normalization failed: " + error;
        close();
        return false;
    }

    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!pkt_ || !frame_) {
        error = "failed to allocate decoder packet/frame";
        close();
        return false;
    }
    return true;
}


bool Decoder::seek_to_start()
{
    avcodec_flush_buffers(codec_ctx_);
    int ret = av_seek_frame(fmt_ctx_, video_stream_idx_, 0, AVSEEK_FLAG_BACKWARD);
    return ret >= 0;
}

// Presentation timestamp for a just-received AVFrame.
// Prefer best_effort_timestamp: for files with B-frames whose container stamped PTS==DTS
// (common bad remux), frame->pts stays in decode order and would map frame_num backwards
// (visible as nearby-frame flicker). best_effort_timestamp is FFmpeg's presentation-order
// estimate (equals pts when the container timestamps are already correct).
// `fallback_raw_pts` is the previous frame's raw PTS plus one frame interval: the frame's own
// raw PTS from ffprobe is already in the origin-shifted domain, so the fallback must be too.
static double frame_raw_pts_seconds(const AVFrame* frame, AVRational time_base,
                                    double fallback_raw_pts)
{
    int64_t ts = AV_NOPTS_VALUE;
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
        ts = frame->best_effort_timestamp;
    else if (frame->pts != AV_NOPTS_VALUE)
        ts = frame->pts;
    if (ts != AV_NOPTS_VALUE)
        return ts * av_q2d(time_base);
    return fallback_raw_pts;
}

// Raw stream PTS -> playback timeline, the single place every DecodedFrame's pts_seconds is
// produced from (next_frame / seek_to_frame / nearest_keyframe).
//
// The monotonic guard handles streams whose timestamps restart or jump backwards mid-file
// (concatenated TS segments, restarted encoders): a backwards jump beyond a generous threshold
// shifts the whole remaining stream forward so the timeline keeps advancing instead of
// rewinding into already-presented grid positions (which froze playback at the join). Small
// non-monotonicities (B-frame reordering residue, VFR jitter) pass through untouched, and
// forward holes are real missing content -- they are never compressed away.
double Decoder::map_timeline_pts(double raw_pts_s)
{
    double pts_s = loop_offset_seconds_ + (raw_pts_s - first_pts_seconds_) + discontinuity_offset_;
    const double step = 1.0 / (fps_ > 0.0 ? fps_ : 1.0);
    // windows.h's min/max macros are live in this TU (no NOMINMAX here): no std::max call.
    double back_jump = 8.0 * step;
    if (back_jump < 0.5) back_jump = 0.5;
    if (last_out_pts_seconds_ >= 0.0 && pts_s < last_out_pts_seconds_ - back_jump) {
        discontinuity_offset_ += (last_out_pts_seconds_ + step) - pts_s;
        pts_s = last_out_pts_seconds_ + step;
    }
    last_out_pts_seconds_ = pts_s;
    return pts_s;
}

int Decoder::pump_one_raw_frame(DecodedFrame& out, double& raw_pts_s_out)
{
    LogScope log_scope(*this);
    constexpr int kMaxRecoverableErrors = 256;
    int errors = 0;
    for (;;) {
        if (interrupt_requested_.load(std::memory_order_relaxed)) return -1;
        av_frame_unref(frame_);

        int recv = avcodec_receive_frame(codec_ctx_, frame_);
        if (recv == 0) {
            if (frame_->format != AV_PIX_FMT_D3D11 || !frame_->data[0]) {
                fprintf(stderr, "unexpected decoder output format %d\n", frame_->format);
                return -1;
            }
            out.texture = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
            out.array_slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame_->data[1]));
            const double step = 1.0 / (fps_ > 0.0 ? fps_ : 1.0);
            raw_pts_s_out = frame_raw_pts_seconds(frame_, time_base_, last_raw_pts_seconds_ + step);
            last_raw_pts_seconds_ = raw_pts_s_out;
            // Hold length on the source timeline: whatever the container declared for this
            // packet, else one frame interval. Never invented for a gap.
            out.duration_seconds = (frame_->duration > 0)
                ? static_cast<double>(frame_->duration) * av_q2d(time_base_)
                : step;
            return 1;
        }
        if (recv == AVERROR_EOF) return 0;
        if (recv == AVERROR_INVALIDDATA) {
            record_corrupt_data("avcodec_receive_frame: invalid data", true);
            if (++errors <= kMaxRecoverableErrors) continue;
            return -1;
        }
        if (recv != AVERROR(EAGAIN)) {
            fprintf(stderr, "avcodec_receive_frame failed: %d\n", recv);
            return -1;
        }
        if (draining_) return 0;

        int read = av_read_frame(fmt_ctx_, pkt_);
        if (read < 0) {
            av_packet_unref(pkt_);
            if (read == AVERROR_INVALIDDATA) {
                record_corrupt_data("av_read_frame: invalid data", true);
                if (++errors <= kMaxRecoverableErrors) continue;
                return -1;
            }
            if (read == AVERROR(EAGAIN) && ++errors <= kMaxRecoverableErrors) {
                Sleep(1);
                continue;
            }
            if (read == AVERROR_EXIT || interrupt_requested_.load(std::memory_order_relaxed))
                return -1;
            if (read != AVERROR_EOF && read != AVERROR(EIO)) {
                fprintf(stderr, "av_read_frame failed: %d\n", read);
                return -1;
            }
            draining_ = true;
            int send = avcodec_send_packet(codec_ctx_, nullptr);
            if (send < 0 && send != AVERROR_EOF) return -1;
            continue;
        }
        if (pkt_->stream_index == video_stream_idx_) {
            int send = avcodec_send_packet(codec_ctx_, pkt_);
            av_packet_unref(pkt_);
            if (send < 0) {
                if (send == AVERROR_INVALIDDATA) {
                    record_corrupt_data("avcodec_send_packet: invalid data", true);
                    if (++errors <= kMaxRecoverableErrors) continue;
                    return -1;
                }
                fprintf(stderr, "avcodec_send_packet failed: %d\n", send);
                return -1;
            }
        } else {
            if (audio_stream_idx_ >= 0 && pkt_->stream_index == audio_stream_idx_) {
                AVPacket* apkt = av_packet_alloc();
                if (apkt) {
                    av_packet_move_ref(apkt, pkt_);
                    std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
                    // Stamped with the discontinuity correction in effect at this demux
                    // position, so audio rides the same timeline as the video frames decoded
                    // around it (a cut shifts both tracks together instead of desyncing them).
                    audio_pkt_queue_.push_back(AudioPacketEntry{ apkt,
                        loop_offset_seconds_ + discontinuity_offset_ });
                    if (audio_pkt_queue_.size() > kAudioQueueMaxPackets) {
                        AudioPacketEntry drop = audio_pkt_queue_.front();
                        audio_pkt_queue_.pop_front();
                        av_packet_free(&drop.pkt);
                        ++audio_pkt_dropped_;
                    }
                }
            }
            av_packet_unref(pkt_);
        }
    }
}

bool Decoder::next_frame(DecodedFrame& out)
{
    if (at_eof() || failed()) return false;
    double raw_pts_s = 0.0;
    int r = pump_one_raw_frame(out, raw_pts_s);
    if (r < 0) {
        failed_.store(true, std::memory_order_release);
        return false;
    }
    if (r == 0) {
        at_eof_.store(true, std::memory_order_release);
        return false;
    }

    if (!have_first_pts_) {
        first_pts_seconds_ = raw_pts_s;
        have_first_pts_ = true;
    }
    out.pts_seconds = map_timeline_pts(raw_pts_s);
    return true;
}

bool Decoder::reconnect_to_frame(int64_t target_frame, DecodedFrame& out, std::string& error)
{
    LogScope log_scope(*this);
    if (!is_network_ || !fmt_ctx_ || !fmt_ctx_->pb || !fmt_ctx_->url ||
        (fmt_ctx_->iformat->flags & AVFMT_NOFILE)) {
        error = "network input cannot be reconnected as a seekable file";
        return false;
    }
    AVFormatContext* replacement = avformat_alloc_context();
    if (!replacement) {
        error = "network reconnect could not allocate a format context";
        return false;
    }
    replacement->interrupt_callback = fmt_ctx_->interrupt_callback;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rw_timeout", "15000000", 0);
    av_dict_set(&options, "reconnect", "1", 0);
    av_dict_set(&options, "reconnect_streamed", "1", 0);
    av_dict_set(&options, "reconnect_on_network_error", "1", 0);
    av_dict_set(&options, "reconnect_delay_max", "5", 0);
    av_dict_set(&options, "seekable", "1", 0);
    av_dict_set(&options, "probesize", "1048576", 0);
    av_dict_set(&options, "analyzeduration", "2000000", 0);
    int ret = avformat_open_input(&replacement, fmt_ctx_->url, nullptr, &options);
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
        ? av_find_best_stream(replacement, AVMEDIA_TYPE_AUDIO, -1, video, nullptr, 0) : -1;
    const AVCodecParameters* params = video >= 0 ? replacement->streams[video]->codecpar : nullptr;
    const AVCodecParameters* old_audio = audio_codecpar();
    const AVCodecParameters* new_audio = audio >= 0 ? replacement->streams[audio]->codecpar : nullptr;
    const bool audio_matches = old_audio && new_audio
        ? old_audio->codec_id == new_audio->codec_id &&
          old_audio->sample_rate == new_audio->sample_rate &&
          av_channel_layout_compare(&old_audio->ch_layout, &new_audio->ch_layout) == 0 &&
          av_cmp_q(audio_time_base(), replacement->streams[audio]->time_base) == 0
        : !old_audio && !new_audio;
    if (!params || params->codec_id != codec_ctx_->codec_id ||
        params->width != coded_width_ || params->height != coded_height_ ||
        av_cmp_q(time_base_, replacement->streams[video]->time_base) != 0 || !audio_matches ||
        !(replacement->pb && (replacement->pb->seekable & AVIO_SEEKABLE_NORMAL))) {
        error = "network source changed format or is no longer seekable";
        avformat_close_input(&replacement);
        return false;
    }
    // Demuxers can retain their own IO pointers; replace the transport and demuxer together.
    avformat_close_input(&fmt_ctx_);
    fmt_ctx_ = replacement;
    video_stream_idx_ = video;
    audio_stream_idx_ = audio;
    flush_audio_queue();
    return seek_to_frame(target_frame, out, error);
}

bool Decoder::seek_to_frame(int64_t target_frame, DecodedFrame& out, std::string& error,
                            bool nearest_keyframe)
{
    LogScope log_scope(*this);
    if (!have_first_pts_) {
        error = "seek_to_frame called before any frame decoded (no PTS origin yet)";
        return false;
    }
    if (target_frame < 0) target_frame = 0;

    // seek = reposition：离开 EOF 状态，之后 next_frame() 会重新产帧（或再次走到 EOF 时
    // 重新置位）。即便下面 av_seek_frame 失败，flush 后的位置也已不再是「确定的 EOF」，
    // 清掉仍是正确信号。
    at_eof_.store(false, std::memory_order_relaxed);

    // Map the requested content frame number back into the raw stream PTS domain using the
    // ORIGINAL first_pts_seconds_/loop_offset_ established at open() -- frame numbers stay
    // globally anchored to that one origin across any number of seeks (I5). A seek never
    // touches loop_offset_seconds_/first_pts_seconds_ (loop_offset stays 0 for the session
    // under pause-on-last-frame; the term remains in the formula for a single code path).
    // The discontinuity correction is reset below, so this linear mapping is the plain origin
    // relation: after a seek the timeline is re-anchored at the raw stream axis.
    double target_seconds_local = static_cast<double>(target_frame) / fps_; // in loop_offset_ domain
    double target_seconds_raw = (target_seconds_local - loop_offset_seconds_) + first_pts_seconds_;
    int64_t seek_ts = static_cast<int64_t>(std::llround(target_seconds_raw / av_q2d(time_base_)));

    draining_ = false;
    failed_.store(false, std::memory_order_relaxed);
    if (fmt_ctx_->pb) {
        fmt_ctx_->pb->eof_reached = 0;
        fmt_ctx_->pb->error = 0;
    }
    avcodec_flush_buffers(codec_ctx_);
    int ret = av_seek_frame(fmt_ctx_, video_stream_idx_, seek_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        error = "av_seek_frame failed";
        return false;
    }
    // A reposition legitimately jumps the timeline backwards: drop the monotonic guard and any
    // discontinuity correction accumulated before the seek (it only described the old position;
    // a cut encountered again after the seek is detected again from here on).
    discontinuity_offset_ = 0.0;
    last_out_pts_seconds_ = -1.0;

    // Decode forward from the keyframe av_seek_frame landed on until the picture ON SCREEN at
    // target_frame is found (I5): a picture covers [ceil(pts*fps), ceil((pts+duration)*fps)-1]
    // of the fixed grid, so a VFR picture answers for its whole hold and forward decoding stops
    // on that picture instead of running on to the next one. Bounded loop: a HEVC GOP is
    // essentially never long enough to need more than a few hundred frames of forward decode
    // even on a deep seek, since av_seek_frame itself did the O(log n) jump to the nearest
    // keyframe regardless of how far away target_frame is.
    // Coarse-preview fast path: take the keyframe av_seek_frame landed on (one decode) and stop.
    if (nearest_keyframe) {
        double raw_pts_s = 0.0;
        DecodedFrame df;
        int r = pump_one_raw_frame(df, raw_pts_s);
        if (r <= 0) { error = "nearest_keyframe: no frame decoded after seek"; return false; }
        out = df;
        out.pts_seconds = map_timeline_pts(raw_pts_s);
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
        const double pts_s = map_timeline_pts(raw_pts_s);
        df.pts_seconds = pts_s;
        last_good = df;
        last_good_pts_s = pts_s;
        have_any = true;

        const int64_t grid_start = static_cast<int64_t>(std::ceil(pts_s * fps_ - 1e-5));
        const int64_t grid_end = (df.duration_seconds > 0.0)
            ? static_cast<int64_t>(std::ceil((pts_s + df.duration_seconds) * fps_ - 1e-5)) - 1
            : grid_start;
        // Covered by this picture, or this picture starts past the target (a gap with no
        // picture at target_frame -- the next sample is the closest answer there is).
        if (target_frame <= grid_end || grid_start > target_frame) {
            out = df;
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
