// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "media_source.h"
#include "video_decoder.h"

extern "C" {
#include <libavutil/log.h>
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

// 8-bit 4:2:0 (and its 10-bit P010 / planar variants) are the only sources the D3D11VA path can
// hand to the normalizer; everything else goes straight to software decode, which converts to the
// same NV12 contract with libswscale.
bool is_hardware_candidate(AVPixelFormat format)
{
    switch (format) {
    case AV_PIX_FMT_NONE: // not known yet (raw/live): try hardware, the first frame decides
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_YUV420P10LE:
        return true;
    default:
        return false;
    }
}

int pixel_depth(AVPixelFormat format)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
    return desc && desc->comp[0].depth > 0 ? desc->comp[0].depth : 8;
}

bool is_jpeg_range_format(AVPixelFormat format)
{
    return format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P ||
        format == AV_PIX_FMT_YUVJ440P || format == AV_PIX_FMT_YUVJ444P;
}

struct ColourTags
{
    bool full_range = false;
    VideoMatrix matrix = VideoMatrix::Bt709;
    VideoTransfer transfer = VideoTransfer::Sdr;
    bool primaries_2020 = false;
};

// Container/bitstream colour tags -> the vocabulary the GPU normalizer understands. Explicit tags
// win; an untagged stream keeps the documented BT.709 / limited / SDR contract (so untagged 4:2:0
// material passes through without a re-matrix), and unmarked HDR is BT.2020 by convention.
ColourTags interpret_colour(AVColorRange range, AVColorSpace space,
                            AVColorTransferCharacteristic trc, AVColorPrimaries primaries,
                            AVPixelFormat format)
{
    ColourTags tags{};
    if (range == AVCOL_RANGE_JPEG)
        tags.full_range = true;
    else if (range == AVCOL_RANGE_MPEG)
        tags.full_range = false;
    else
        tags.full_range = is_jpeg_range_format(format); // legacy JPEG-range pixel formats

    switch (space) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        tags.matrix = VideoMatrix::Bt601;
        break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: // constant-luminance content is vanishingly rare; treat as NCL
        tags.matrix = VideoMatrix::Bt2020;
        break;
    default:
        break;
    }

    if (trc == AVCOL_TRC_SMPTE2084) tags.transfer = VideoTransfer::Pq;
    else if (trc == AVCOL_TRC_ARIB_STD_B67) tags.transfer = VideoTransfer::Hlg;

    tags.primaries_2020 = primaries == AVCOL_PRI_BT2020;
    if (primaries == AVCOL_PRI_UNSPECIFIED)
        tags.primaries_2020 = tags.transfer != VideoTransfer::Sdr || tags.matrix == VideoMatrix::Bt2020;
    return tags;
}

// Presentation timestamp for a just-received AVFrame. Prefer best_effort_timestamp: for files with
// B-frames whose container stamped PTS==DTS (common bad remux), frame->pts stays in decode order
// and would map frame_num backwards (visible as nearby-frame flicker). best_effort_timestamp is
// FFmpeg's presentation-order estimate (equals pts when the container timestamps are correct).
// `fallback_raw_pts` is the previous frame's raw PTS plus one frame interval.
double frame_raw_pts_seconds(const AVFrame* frame, AVRational time_base, double fallback_raw_pts)
{
    int64_t ts = AV_NOPTS_VALUE;
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) ts = frame->best_effort_timestamp;
    else if (frame->pts != AV_NOPTS_VALUE) ts = frame->pts;
    if (ts != AV_NOPTS_VALUE) return ts * av_q2d(time_base);
    return fallback_raw_pts;
}

bool desc_matches(const VideoNormalizeDesc& a, const VideoNormalizeDesc& b)
{
    return a.src_width == b.src_width && a.src_height == b.src_height &&
        a.dst_width == b.dst_width && a.dst_height == b.dst_height &&
        a.bit_depth == b.bit_depth && a.full_range == b.full_range && a.matrix == b.matrix &&
        a.transfer == b.transfer && a.primaries_2020 == b.primaries_2020 &&
        a.rotation_cw == b.rotation_cw && a.hflip == b.hflip;
}

} // namespace

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
    if (corrupt_messages_ == reported_messages_ && corrupt_packets_ == reported_packets_) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < next_log_summary_) return;
    fprintf(stderr, "[sumu] recoverable media data: summary: ffmpeg_messages=%llu "
        "packet_errors=%llu%s\n", static_cast<unsigned long long>(corrupt_messages_),
        static_cast<unsigned long long>(corrupt_packets_), force ? " (session closed)" : "");
    reported_messages_ = corrupt_messages_;
    reported_packets_ = corrupt_packets_;
    next_log_summary_ = now + std::chrono::seconds(5);
}

Decoder::Decoder() = default;

Decoder::~Decoder()
{
    close();
}

void Decoder::close()
{
    normalizer_.reset();
    needs_normalization_ = false;
    normalize_desc_ = VideoNormalizeDesc{};
    if (pkt_) av_packet_free(&pkt_);
    video_.reset();
    source_.reset();
    flush_corrupt_logs(true);
    corrupt_messages_ = 0;
    corrupt_packets_ = 0;
    reported_messages_ = 0;
    reported_packets_ = 0;
    next_log_summary_ = {};
    // Reset session-derived state so a subsequent open() (reopen path, or a retry after a partial
    // open failure) starts from a clean slate -- without this, have_first_pts_ stays true and the
    // next session would inherit the previous file's PTS origin (I5 break).
    fps_ = 60.0;
    time_base_ = AVRational{1, 1};
    width_ = 0;
    height_ = 0;
    coded_width_ = 0;
    coded_height_ = 0;
    pixel_aspect_ = 1.0;
    frame_count_ = 0;
    display_rotation_cw_ = 0;
    display_hflip_ = false;
    reported_anamorphic_ = false;
    reported_rotation_ = false;
    have_first_pts_.store(false, std::memory_order_relaxed);
    first_pts_seconds_.store(0.0, std::memory_order_relaxed);
    at_eof_.store(false, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    config_generation_.store(0, std::memory_order_relaxed);
    draining_ = false;
    have_delivered_frame_ = false;
    software_fallback_used_ = false;
    loop_offset_seconds_ = 0.0;
    last_out_pts_seconds_ = -1.0;
    last_raw_pts_seconds_ = 0.0;
    discontinuity_offset_ = 0.0;
    segmented_timeline_ = false;
    d3d_device_ = nullptr;
    last_error_.clear();
    set_state(SourceState::Ended);
}

bool Decoder::looks_like_network_url(const std::string& path)
{
    return MediaSource::looks_like_network_url(path);
}

void Decoder::request_interrupt()
{
    if (source_) source_->request_interrupt();
}

bool Decoder::is_network() const
{
    return source_ && source_->is_network();
}

bool Decoder::is_live() const
{
    return source_ && source_->is_network() && !source_->info().duration_known;
}

TimelineKind Decoder::timeline_kind() const
{
    if (segmented_timeline_) return TimelineKind::Segmented;
    return is_live() ? TimelineKind::Live : TimelineKind::Finite;
}

VideoBackendKind Decoder::backend() const
{
    return video_ ? video_->kind() : VideoBackendKind::None;
}

bool Decoder::has_audio() const
{
    return source_ && source_->has_audio();
}

AVCodecParameters* Decoder::audio_codecpar() const
{
    return source_ ? source_->audio_codecpar() : nullptr;
}

AVRational Decoder::audio_time_base() const
{
    return source_ ? source_->audio_time_base() : AVRational{1, 1};
}

bool Decoder::pop_audio_packet(AVPacket* dst, double& loop_offset_out)
{
    return source_ && source_->pop_audio_packet(dst, loop_offset_out);
}

void Decoder::flush_audio_queue()
{
    if (source_) source_->flush_audio_queue();
}

size_t Decoder::audio_queue_depth() const
{
    return source_ ? source_->audio_queue_depth() : 0;
}

bool Decoder::open(const std::string& path, ID3D11Device* device, std::string& error,
                   const std::string& headers, const std::string& cookies)
{
    static std::once_flag log_once;
    std::call_once(log_once, [] { av_log_set_callback(&Decoder::log_callback); });
    LogScope log_scope(*this);
    // Always start from a clean slate -- a previous partial failure (or a reopen) must not leave
    // half-allocated FFmpeg contexts for the next attempt.
    close();
    set_state(SourceState::Opening);
    d3d_device_ = device;
    if (!device) {
        error = "null D3D11 device";
        set_state(SourceState::Failed);
        return false;
    }

    source_ = std::make_unique<MediaSource>();
    if (!source_->open(path, headers, cookies, error)) {
        close();
        set_state(SourceState::Failed);
        return false;
    }
    const MediaStreamInfo& info = source_->info();
    fps_ = info.fps;
    time_base_ = info.time_base;
    coded_width_ = info.coded_width;
    coded_height_ = info.coded_height;
    width_ = coded_width_; // replaced by configure_output() with the display size
    height_ = coded_height_;
    pixel_aspect_ = info.pixel_aspect;
    frame_count_ = info.frame_count;
    display_rotation_cw_ = info.rotation_cw;
    display_hflip_ = info.hflip;

    pkt_ = av_packet_alloc();
    if (!pkt_) {
        error = "failed to allocate decoder packet";
        close();
        set_state(SourceState::Failed);
        return false;
    }

    if (!open_backend(is_hardware_candidate(info.pix_fmt), error)) {
        close();
        set_state(SourceState::Failed);
        return false;
    }
    // Frame contract: everything downstream (present / AI / headless / scrub) consumes
    // display-oriented BT.709 limited 8-bit NV12. Build the GPU normalizer here when the source is
    // anything else (10-bit, full range, non-709 matrix, HDR, display-matrix rotation) so an
    // unsupported combination fails the open with a reason instead of producing a wrong image.
    if (!configure_output(initial_source_format(), error)) {
        error = "frame normalization failed: " + error;
        close();
        set_state(SourceState::Failed);
        return false;
    }
    set_state(SourceState::Buffering);
    return true;
}

bool Decoder::open_backend(bool allow_hardware, std::string& error)
{
    const MediaStreamInfo& info = source_->info();
    const AVCodecParameters* params = source_->video_codecpar();

    if (allow_hardware) {
        std::unique_ptr<VideoDecoder> hardware = create_hardware_video_decoder();
        std::string hardware_error;
        if (hardware->open(params, info.time_base, info.frame_rate, d3d_device_, hardware_error)) {
            video_ = std::move(hardware);
            return true;
        }
        fprintf(stderr, "[sumu] d3d11va decode unavailable (%s); falling back to software decode\n",
            hardware_error.c_str());
    } else {
        const char* name = av_get_pix_fmt_name(info.pix_fmt);
        fprintf(stderr, "[sumu] software decode for pixel format %s (hardware path takes 4:2:0)\n",
            name ? name : "unknown");
    }

    std::unique_ptr<VideoDecoder> software = create_software_video_decoder();
    std::string software_error;
    if (!software->open(params, info.time_base, info.frame_rate, d3d_device_, software_error)) {
        error = allow_hardware
            ? "no usable video decoder (software fallback: " + software_error + ")"
            : "no usable video decoder: " + software_error;
        return false;
    }
    video_ = std::move(software);
    return true;
}

bool Decoder::switch_to_software(const std::string& reason, std::string& error)
{
    // Exactly one fallback per session: a hardware failure that survives it is final, so this can
    // never turn into a retry loop.
    if (software_fallback_used_ || !source_ || !video_) {
        error = reason;
        return false;
    }
    software_fallback_used_ = true;
    const MediaStreamInfo& info = source_->info();
    std::unique_ptr<VideoDecoder> software = create_software_video_decoder();
    std::string software_error;
    if (!software->open(source_->video_codecpar(), info.time_base, info.frame_rate, d3d_device_,
            software_error)) {
        error = "hardware decode failed (" + reason + ") and software decode is unavailable: " +
            software_error;
        return false;
    }
    // The picture the hardware path died on was never delivered: resume one frame interval past
    // the last delivered one so the caller sees a continuous timeline.
    const double step = 1.0 / (fps_ > 0.0 ? fps_ : 1.0);
    std::string resume_error;
    const bool resumed = is_live() ? source_->reconnect(resume_error, true)
                                   : resume_at(last_raw_pts_seconds_ + step, resume_error);
    if (!resumed) {
        error = "hardware decode failed (" + reason + ") and the stream cannot be repositioned "
            "for the software fallback: " + resume_error;
        return false;
    }
    video_ = std::move(software);
    draining_ = false;
    failed_.store(false, std::memory_order_relaxed);
    config_generation_.fetch_add(1, std::memory_order_release);
    set_state(SourceState::Recovering);
    fprintf(stderr, "[sumu] decoder: hardware decode failed (%s); continuing with software decode\n",
        reason.c_str());
    return true;
}

bool Decoder::resume_at(double raw_pts_seconds, std::string& error)
{
    draining_ = false;
    at_eof_.store(false, std::memory_order_relaxed);
    if (video_) video_->flush();
    return source_->seek_to(raw_pts_seconds, error);
}

Decoder::SourceFormat Decoder::initial_source_format() const
{
    SourceFormat format{};
    if (video_ && video_->kind() == VideoBackendKind::Software) {
        // The software path hands the normalizer its own 8-bit BT.709 limited NV12, so only the
        // geometry (and a display matrix) can still need GPU work.
        format.width = (coded_width_ + 1) & ~1;
        format.height = (coded_height_ + 1) & ~1;
        format.pix_fmt = AV_PIX_FMT_NV12;
        format.bit_depth = 8;
        return format;
    }
    const AVCodecParameters* params = source_ ? source_->video_codecpar() : nullptr;
    format.width = coded_width_;
    format.height = coded_height_;
    format.pix_fmt = params ? static_cast<AVPixelFormat>(params->format) : AV_PIX_FMT_NONE;
    format.bit_depth = pixel_depth(format.pix_fmt);
    if (params) {
        const ColourTags tags = interpret_colour(params->color_range, params->color_space,
            params->color_trc, params->color_primaries, format.pix_fmt);
        format.full_range = tags.full_range;
        format.matrix = tags.matrix;
        format.transfer = tags.transfer;
        format.primaries_2020 = tags.primaries_2020;
    }
    return format;
}

Decoder::SourceFormat Decoder::derive_source_format(const VideoPicture& picture) const
{
    SourceFormat format{};
    const AVFrame* frame = picture.frame;
    if (picture.software) {
        // libswscale already produced the downstream contract; only even-dimension rounding can
        // add padding past the visible picture.
        format.width = (frame->width + 1) & ~1;
        format.height = (frame->height + 1) & ~1;
        format.pix_fmt = AV_PIX_FMT_NV12;
        format.bit_depth = 8;
        return format;
    }
    format.width = frame->width;
    format.height = frame->height;
    format.pix_fmt = video_ ? video_->produced_format() : AV_PIX_FMT_NONE;
    format.bit_depth = pixel_depth(format.pix_fmt);
    // The frame carries the tags that actually describe it (and shows a mid-stream change), but
    // decoders that never copy the container's tags into the frame leave them unspecified -- fall
    // back to the stream's codec parameters there instead of silently assuming BT.709 limited SDR.
    AVColorRange range = frame->color_range;
    AVColorSpace space = frame->colorspace;
    AVColorTransferCharacteristic trc = frame->color_trc;
    AVColorPrimaries primaries = frame->color_primaries;
    if (const AVCodecParameters* params = source_ ? source_->video_codecpar() : nullptr) {
        if (range == AVCOL_RANGE_UNSPECIFIED) range = params->color_range;
        if (space == AVCOL_SPC_UNSPECIFIED) space = params->color_space;
        if (trc == AVCOL_TRC_UNSPECIFIED) trc = params->color_trc;
        if (primaries == AVCOL_PRI_UNSPECIFIED) primaries = params->color_primaries;
    }
    const ColourTags tags = interpret_colour(range, space, trc, primaries, format.pix_fmt);
    format.full_range = tags.full_range;
    format.matrix = tags.matrix;
    format.transfer = tags.transfer;
    format.primaries_2020 = tags.primaries_2020;
    return format;
}

bool Decoder::configure_output(const SourceFormat& format, std::string& error)
{
    VideoNormalizeDesc desc{};
    desc.src_width = format.width;
    desc.src_height = format.height;
    desc.bit_depth = format.bit_depth;
    desc.full_range = format.full_range;
    desc.matrix = format.matrix;
    desc.transfer = format.transfer;
    desc.primaries_2020 = format.primaries_2020;
    desc.rotation_cw = display_rotation_cw_;
    desc.hflip = display_hflip_;
    desc.fps = fps_;
    // The normalized frame is still stored in the coded grid -- only its on-screen shape changes,
    // so a 90/270 display matrix swaps the reported display size.
    const bool swap = (display_rotation_cw_ % 180) != 0;
    desc.dst_width = swap ? format.height : format.width;
    desc.dst_height = swap ? format.width : format.height;

    const bool needed = video_normalize_required(desc);
    const bool ready = !needs_normalization_ || std::strcmp(normalizer_.path_name(), "none") != 0;
    if (needed == needs_normalization_ && ready && desc_matches(normalize_desc_, desc)) {
        width_ = desc.dst_width;
        height_ = desc.dst_height;
        return true;
    }

    width_ = desc.dst_width;
    height_ = desc.dst_height;
    normalize_desc_ = desc;
    needs_normalization_ = needed;
    if (!needed) {
        normalizer_.reset();
    } else if (!normalizer_.initialize(d3d_device_, desc, error)) {
        // Never leave a half-configured normalizer in place: the caller must not get a wrong image.
        normalizer_.reset();
        needs_normalization_ = false;
        return false;
    }

    // A geometry/colour/backend change invalidates anything a consumer derived from the previous
    // picture description.
    config_generation_.fetch_add(1, std::memory_order_release);

    if (pixel_aspect_ != 1.0 && !reported_anamorphic_) {
        reported_anamorphic_ = true;
        const double dar = swap
            ? static_cast<double>(width_) / (static_cast<double>(height_) * pixel_aspect_)
            : (static_cast<double>(width_) * pixel_aspect_) / static_cast<double>(height_);
        fprintf(stderr, "[sumu] anamorphic source: pixel aspect %.4f, %dx%d coded -> display aspect %.4f\n",
            pixel_aspect_, width_, height_, dar);
    }
    if (needs_normalization_ && (display_rotation_cw_ != 0 || display_hflip_) &&
        !reported_rotation_) {
        reported_rotation_ = true;
        fprintf(stderr, "[sumu] display matrix: rotate %d deg clockwise%s, %dx%d -> %dx%d\n",
            display_rotation_cw_, display_hflip_ ? " + horizontal flip" : "",
            coded_width_, coded_height_, width_, height_);
    }
    return true;
}

bool Decoder::prepare_nv12_frame(DecodedFrame& frame, std::string& error)
{
    if (frame.software && !frame.texture) {
        if (!video_) {
            error = "decoder is not open";
            return false;
        }
        // The upload is a GPU write: it happens here (under the caller's D3D serialization), never
        // on the bare decode call.
        VideoPicture uploaded;
        if (!video_->upload(frame.software_serial, uploaded, error)) return false;
        frame.texture = uploaded.texture;
        frame.array_slice = uploaded.array_slice;
    }
    if (!frame.texture) {
        error = "decoded frame has no GPU surface";
        return false;
    }
    if (!needs_normalization_) return true; // already the downstream contract
    if (!normalizer_.process(frame.texture, frame.array_slice, error)) return false;
    frame.texture = normalizer_.output_texture();
    frame.array_slice = 0;
    return true;
}

DecodeStatus Decoder::pump_one_picture(VideoPicture& picture, double& raw_pts_s,
                                       std::string& error)
{
    LogScope log_scope(*this);
    constexpr int kMaxRecoverableErrors = 256;
    const double step = 1.0 / (fps_ > 0.0 ? fps_ : 1.0);
    int recoverable = 0;
    for (;;) {
        if (!source_ || !video_ || !pkt_) {
            error = "decoder is not open";
            return DecodeStatus::FatalError;
        }
        // MediaSource's interrupt flag also gates av_read_frame(), but a decode step can run a
        // while before the next read: check it here so request_interrupt() aborts this loop at
        // the next boundary instead of after the ring drains.
        if (source_->interrupt_requested()) {
            error = "decode interrupted";
            return DecodeStatus::FatalError;
        }
        const DecodeStatus decoded = video_->decode(picture, error);
        switch (decoded) {
        case DecodeStatus::Frame:
            raw_pts_s = frame_raw_pts_seconds(picture.frame, time_base_,
                last_raw_pts_seconds_ + step);
            last_raw_pts_seconds_ = raw_pts_s;
            return decoded;
        case DecodeStatus::Eof:
            return decoded;
        case DecodeStatus::WouldBlock:
            break; // the backend needs more input
        case DecodeStatus::RecoverableIoError:
            record_corrupt_data(error.c_str(), true);
            if (++recoverable > kMaxRecoverableErrors) {
                error = "too many corrupt packets: " + error;
                return DecodeStatus::RecoverableIoError;
            }
            continue;
        default:
            return decoded; // RetryWithSoftware / FatalError
        }

        if (draining_) {
            // Input requested after end-of-stream was signalled: that really is the end.
            return DecodeStatus::Eof;
        }
        const double timeline_offset = loop_offset_seconds_ + discontinuity_offset_;
        const MediaReadStatus read = source_->read_video_packet(pkt_, timeline_offset, error);
        switch (read) {
        case MediaReadStatus::Packet:
            video_->send_packet(pkt_, error); // a failure surfaces through the next decode()
            av_packet_unref(pkt_);
            break;
        case MediaReadStatus::Eof:
            draining_ = true;
            video_->send_packet(nullptr, error);
            break;
        case MediaReadStatus::WouldBlock:
            return DecodeStatus::WouldBlock;
        case MediaReadStatus::CorruptData:
            record_corrupt_data(error.c_str(), true);
            if (++recoverable > kMaxRecoverableErrors) return DecodeStatus::RecoverableIoError;
            break;
        case MediaReadStatus::RecoverableIoError:
            // Transport trouble is not answerable by reading again (every attempt can burn the
            // full rw_timeout): surface it so the caller's reconnect/backoff path owns the retry.
            return DecodeStatus::RecoverableIoError;
        case MediaReadStatus::Interrupted:
            if (error.empty()) error = "decode interrupted";
            return DecodeStatus::FatalError;
        case MediaReadStatus::FatalError:
        default:
            return DecodeStatus::FatalError;
        }
    }
}

DecodeStatus Decoder::describe_picture(const VideoPicture& picture, DecodedFrame& out,
                                       std::string& error)
{
    const AVFrame* frame = picture.frame;
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        error = "decoder produced an invalid picture";
        return DecodeStatus::FatalError;
    }

    out = DecodedFrame{};
    out.software = picture.software;
    out.source_frame = picture.software ? picture.frame : nullptr;
    out.software_serial = picture.serial;
    out.cpu_plane[0] = picture.cpu_plane[0];
    out.cpu_plane[1] = picture.cpu_plane[1];
    out.cpu_linesize[0] = picture.cpu_linesize[0];
    out.cpu_linesize[1] = picture.cpu_linesize[1];
    out.width = frame->width;
    out.height = frame->height;
    const double step = 1.0 / (fps_ > 0.0 ? fps_ : 1.0);
    // Hold length on the source timeline: whatever the container declared for this picture, else
    // one frame interval. Never invented for a gap.
    out.duration_seconds = frame->duration > 0
        ? static_cast<double>(frame->duration) * av_q2d(time_base_)
        : step;

    const SourceFormat format = derive_source_format(picture);
    out.pix_fmt = format.pix_fmt;
    out.bit_depth = format.bit_depth;
    out.full_range = format.full_range;
    out.matrix = format.matrix;
    out.transfer = format.transfer;
    out.primaries_2020 = format.primaries_2020;

    if (picture.software) {
        if (!picture.cpu_plane[0] || !picture.cpu_plane[1] ||
            picture.cpu_linesize[0] < out.width) {
            error = "software picture has no NV12 CPU planes";
            return DecodeStatus::FatalError;
        }
    } else {
        if (!picture.texture) {
            error = "hardware decoder produced no D3D11 texture";
            return DecodeStatus::RetryWithSoftware;
        }
        D3D11_TEXTURE2D_DESC td{};
        picture.texture->GetDesc(&td);
        const DXGI_FORMAT expected =
            format.bit_depth > 8 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        if (td.Format != expected) {
            // CopySubresourceRegion never converts formats: a surface that is not the promised one
            // would silently produce a black/garbled picture, so treat it as a decoder fault.
            char buf[192];
            snprintf(buf, sizeof(buf),
                "hardware decoder surface format %d, expected %d for a %d-bit stream",
                static_cast<int>(td.Format), static_cast<int>(expected), format.bit_depth);
            error = buf;
            return DecodeStatus::RetryWithSoftware;
        }
        if (picture.array_slice >= td.ArraySize ||
            static_cast<int>(td.Width) < out.width ||
            static_cast<int>(td.Height) < out.height) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                "hardware decoder surface %ux%u[%u] cannot hold a %dx%d picture",
                static_cast<unsigned>(td.Width), static_cast<unsigned>(td.Height),
                picture.array_slice, out.width, out.height);
            error = buf;
            return DecodeStatus::RetryWithSoftware;
        }
        out.texture = picture.texture;
        out.array_slice = picture.array_slice;
    }

    // Geometry: the first picture silently re-adopts the container's metadata (bitstream-accurate
    // cropping), anything after that is a real reconfiguration the caller has to follow.
    bool reconfigured = false;
    if (out.width != coded_width_ || out.height != coded_height_) {
        reconfigured = have_delivered_frame_;
        coded_width_ = out.width;
        coded_height_ = out.height;
        if (source_) source_->adopt_frame_size(out.width, out.height);
    }
    const uint64_t generation_before = config_generation_.load(std::memory_order_acquire);
    if (!configure_output(format, error)) {
        error = "frame normalization failed: " + error;
        return DecodeStatus::FatalError;
    }
    if (have_delivered_frame_ && config_generation_.load(std::memory_order_acquire) != generation_before)
        reconfigured = true;
    return reconfigured ? DecodeStatus::NeedReconfigure : DecodeStatus::Frame;
}

DecodeStatus Decoder::pull_described(DecodedFrame& out, double& raw_pts_s, std::string& error)
{
    for (;;) {
        VideoPicture picture;
        double raw = 0.0;
        DecodeStatus status = pump_one_picture(picture, raw, error);
        if (status == DecodeStatus::Frame) {
            status = describe_picture(picture, out, error);
            if (status == DecodeStatus::Frame || status == DecodeStatus::NeedReconfigure) {
                raw_pts_s = raw;
                return status;
            }
        }
        if (status == DecodeStatus::RetryWithSoftware) {
            const std::string reason = error;
            std::string switch_error;
            if (switch_to_software(reason, switch_error)) continue;
            error = switch_error;
            return DecodeStatus::FatalError;
        }
        return status;
    }
}

DecodeResult Decoder::fail(DecodeStatus status, const std::string& error)
{
    last_error_ = error;
    failed_.store(status == DecodeStatus::FatalError, std::memory_order_release);
    set_state(status == DecodeStatus::FatalError ? SourceState::Failed
                                                 : SourceState::Recovering);
    if (!error.empty()) fprintf(stderr, "[sumu] decode error: %s\n", error.c_str());
    DecodeResult result;
    result.status = status;
    result.error = error;
    return result;
}

DecodeResult Decoder::next_frame(DecodedFrame& out)
{
    if (!source_ || !video_ || !pkt_) {
        DecodeResult result;
        result.status = at_eof() ? DecodeStatus::Eof : DecodeStatus::FatalError;
        if (result.status == DecodeStatus::FatalError) result.error = "decoder is not open";
        return result;
    }
    if (at_eof()) {
        DecodeResult result;
        result.status = DecodeStatus::Eof;
        return result;
    }
    if (failed()) {
        DecodeResult result;
        result.status = DecodeStatus::FatalError;
        result.error = last_error_;
        return result;
    }

    // A buffered (known-duration) source normally never stalls: give a short network hiccup a few
    // hundred milliseconds before reporting it. A live/unknown-duration source answers WouldBlock
    // straight away instead -- there, waiting is the normal state, not an error.
    constexpr int kMaxStarveRetries = 256;
    const bool buffered = source_->info().duration_known;
    int starve_retries = 0;

    for (;;) {
        std::string error;
        double raw_pts_s = 0.0;
        const DecodeStatus status = pull_described(out, raw_pts_s, error);
        switch (status) {
        case DecodeStatus::Frame:
        case DecodeStatus::NeedReconfigure:
            if (!have_first_pts_) {
                first_pts_seconds_.store(raw_pts_s, std::memory_order_relaxed);
                have_first_pts_.store(true, std::memory_order_release);
            }
            out.pts_seconds = map_timeline_pts(raw_pts_s);
            have_delivered_frame_ = true;
            {
                DecodeResult result;
                result.status = status;
                if (status == DecodeStatus::NeedReconfigure) {
                    result.error = "stream geometry changed";
                    set_state(SourceState::Reconfiguring);
                } else {
                    set_state(SourceState::Ready);
                }
                return result;
            }
        case DecodeStatus::WouldBlock:
            if (buffered && starve_retries++ < kMaxStarveRetries) {
                Sleep(1);
                continue;
            }
            set_state(buffered ? SourceState::Recovering : SourceState::Starved);
            {
                DecodeResult result;
                result.status = buffered ? DecodeStatus::RecoverableIoError
                                         : DecodeStatus::WouldBlock;
                if (buffered) result.error = "source stopped delivering data";
                return result;
            }
        case DecodeStatus::RecoverableIoError:
            set_state(SourceState::Recovering);
            {
                DecodeResult result;
                result.status = DecodeStatus::RecoverableIoError;
                result.error = error;
                return result;
            }
        case DecodeStatus::Eof:
            at_eof_.store(true, std::memory_order_release);
            set_state(SourceState::Ended);
            {
                DecodeResult result;
                result.status = DecodeStatus::Eof;
                return result;
            }
        case DecodeStatus::RetryWithSoftware: // consumed inside pull_described()
        case DecodeStatus::FatalError:
        default:
            return fail(DecodeStatus::FatalError, error);
        }
    }
}

// Raw stream PTS -> playback timeline, the single place every DecodedFrame's pts_seconds is
// produced from (next_frame / seek / nearest_keyframe).
//
// The monotonic guard handles streams whose timestamps restart or jump backwards mid-file
// (concatenated TS segments, restarted encoders): a backwards jump beyond a generous threshold
// shifts the whole remaining stream forward so the timeline keeps advancing instead of rewinding
// into already-presented grid positions (which froze playback at the join). Small
// non-monotonicities (B-frame reordering residue, VFR jitter) pass through untouched, and forward
// holes are real missing content -- they are never compressed away.
double Decoder::map_timeline_pts(double raw_pts_s)
{
    double pts_s = loop_offset_seconds_ + (raw_pts_s - first_pts_seconds_) + discontinuity_offset_;
    const double step = 1.0 / (fps_ > 0.0 ? fps_ : 1.0);
    // windows.h's min/max macros are live in this TU (no NOMINMAX here): no std::max call.
    double back_jump = 8.0 * step;
    if (back_jump < 0.5) back_jump = 0.5;
    if (last_out_pts_seconds_ >= 0.0 && pts_s < last_out_pts_seconds_ - back_jump) {
        segmented_timeline_ = true;
        discontinuity_offset_ += (last_out_pts_seconds_ + step) - pts_s;
        pts_s = last_out_pts_seconds_ + step;
    }
    last_out_pts_seconds_ = pts_s;
    return pts_s;
}

bool Decoder::seek_to_frame(int64_t target_frame, DecodedFrame& out, std::string& error,
                            bool nearest_keyframe)
{
    LogScope log_scope(*this);
    if (!source_ || !video_) {
        error = "decoder is not open";
        return false;
    }
    if (!have_first_pts_) {
        error = "seek_to_frame called before any frame decoded (no PTS origin yet)";
        return false;
    }
    if (target_frame < 0) target_frame = 0;

    // seek = reposition: leave the EOF state, a later next_frame() produces pictures again (and
    // re-latches EOF if the stream really ends there).
    at_eof_.store(false, std::memory_order_relaxed);

    // Map the requested content frame number back into the raw stream PTS domain using the
    // ORIGINAL first_pts_seconds_ established at open() -- frame numbers stay globally anchored to
    // that one origin across any number of seeks (I5).
    const double target_seconds_local = static_cast<double>(target_frame) / fps_;
    const double target_seconds_raw =
        (target_seconds_local - loop_offset_seconds_) + first_pts_seconds_;

    draining_ = false;
    failed_.store(false, std::memory_order_relaxed);
    video_->flush();
    if (!source_->seek_to(target_seconds_raw, error)) return false;
    // A reposition legitimately jumps the timeline backwards: drop the monotonic guard and any
    // discontinuity correction accumulated before the seek (it only described the old position).
    discontinuity_offset_ = 0.0;
    last_out_pts_seconds_ = -1.0;
    set_state(SourceState::Buffering);

    constexpr int kMaxStarveRetries = 256;
    constexpr int kMaxForwardFrames = 600;
    int starve_retries = 0;
    bool have_any = false;
    DecodedFrame last_good{};
    double last_good_pts_s = 0.0;

    // Coarse-preview fast path: take the keyframe av_seek_frame landed on (one decode) and stop.
    int64_t budget = nearest_keyframe ? 1 : kMaxForwardFrames;
    while (budget-- > 0) {
        DecodedFrame df;
        double raw_pts_s = 0.0;
        const DecodeStatus status = pull_described(df, raw_pts_s, error);
        if (status == DecodeStatus::WouldBlock) {
            if (++starve_retries > kMaxStarveRetries) {
                error = "seek stalled: source stopped delivering data";
                set_state(SourceState::Starved);
                return false;
            }
            Sleep(1);
            ++budget; // waiting for data does not consume forward-decode budget
            continue;
        }
        if (status == DecodeStatus::Eof) {
            // Hit EOF while decoding forward toward target_frame (near-tail seek): clamp to
            // whatever we already have instead of failing the whole seek.
            break;
        }
        if (status != DecodeStatus::Frame && status != DecodeStatus::NeedReconfigure) {
            if (error.empty()) error = "seek_to_frame: decode failed";
            return false;
        }
        const double pts_s = map_timeline_pts(raw_pts_s);
        df.pts_seconds = pts_s;
        if (nearest_keyframe) {
            out = df;
            set_state(SourceState::Ready);
            return true;
        }
        last_good = df;
        last_good_pts_s = pts_s;
        have_any = true;

        // The picture covers [ceil(pts*fps), ceil((pts+duration)*fps)-1] of the fixed grid (I5),
        // so a VFR picture answers for its whole hold and forward decoding stops on that picture.
        const int64_t grid_start = static_cast<int64_t>(std::ceil(pts_s * fps_ - 1e-5));
        const int64_t grid_end = (df.duration_seconds > 0.0)
            ? static_cast<int64_t>(std::ceil((pts_s + df.duration_seconds) * fps_ - 1e-5)) - 1
            : grid_start;
        // Covered by this picture, or this picture starts past the target (a gap with no picture
        // at target_frame -- the next sample is the closest answer there is).
        if (target_frame <= grid_end || grid_start > target_frame) {
            out = df;
            set_state(SourceState::Ready);
            return true;
        }
    }

    if (nearest_keyframe && !have_any) {
        error = "nearest_keyframe: no frame decoded after seek";
        return false;
    }
    if (have_any) {
        // Ran out of forward-decode budget or hit EOF before reaching target_frame exactly: land
        // on the last frame we actually got (near-tail-of-video or pathologically long-GOP case).
        // Graceful degradation, never a hard failure.
        out = last_good;
        out.pts_seconds = last_good_pts_s;
        set_state(SourceState::Ready);
        return true;
    }
    error = "seek_to_frame: no frame could be decoded after seek";
    return false;
}

bool Decoder::reconnect_to_frame(int64_t target_frame, DecodedFrame& out, std::string& error)
{
    LogScope log_scope(*this);
    if (!source_ || !video_) {
        error = "decoder is not open";
        return false;
    }
    if (!source_->reconnect(error)) return false;
    set_state(SourceState::Recovering);
    // The transport was swapped; MediaSource validated that the codec/geometry/audio still match,
    // so the decoder keeps running and only needs its buffered state dropped.
    video_->flush();
    draining_ = false;
    at_eof_.store(false, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    return seek_to_frame(target_frame, out, error);
}

bool Decoder::reconnect_live(std::string& error)
{
    LogScope log_scope(*this);
    if (!is_live()) {
        error = "input is not a live network source";
        return false;
    }
    if (!source_->reconnect(error, true)) return false;
    video_->flush();
    draining_ = false;
    at_eof_.store(false, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    set_state(SourceState::Recovering);
    return true;
}
