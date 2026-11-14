// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "video_decoder.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {

std::string av_error_text(int averror)
{
    char message[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(averror, message, sizeof(message));
    return message;
}

ID3D11DeviceContext* immediate_context(ID3D11Device* device)
{
    ID3D11DeviceContext* context = nullptr;
    if (device) device->GetImmediateContext(&context);
    return context; // reference handed to the caller
}

} // namespace

// ---- VideoDecoder: codec lifecycle shared by both backends ------------------------------------

VideoDecoder::~VideoDecoder()
{
    // No virtual call here: each backend's own destructor runs close(), which releases the backend
    // resources while the derived type is still alive.
    if (frame_) av_frame_free(&frame_);
    if (ctx_) avcodec_free_context(&ctx_);
}

bool VideoDecoder::open(const AVCodecParameters* params, AVRational time_base, AVRational framerate,
                        ID3D11Device* device, std::string& error)
{
    close();
    if (!params) {
        error = "no codec parameters";
        return false;
    }
    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) {
        const char* name = avcodec_get_name(params->codec_id);
        error = std::string("no decoder available for ") + (name ? name : "this codec");
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) {
        error = "avcodec_alloc_context3 failed";
        return false;
    }
    if (avcodec_parameters_to_context(ctx_, params) < 0) {
        error = "avcodec_parameters_to_context failed";
        close();
        return false;
    }
    // Required for correct pts/dts -> frame timestamp conversion (esp. B-frames). Without this,
    // files whose container stamps PTS==DTS despite has_b_frames>0 can leave frame->pts in decode
    // order; best_effort_timestamp then can't recover a presentation timeline either.
    ctx_->pkt_timebase = time_base;
    if (framerate.num > 0 && framerate.den > 0) ctx_->framerate = framerate;

    if (!prepare_codec(ctx_, device, error)) {
        close();
        return false;
    }
    int ret = avcodec_open2(ctx_, codec, nullptr);
    if (ret < 0) {
        error = "avcodec_open2 failed: " + av_error_text(ret);
        close();
        return false;
    }
    frame_ = av_frame_alloc();
    if (!frame_) {
        error = "failed to allocate decoder frame";
        close();
        return false;
    }
    return true;
}

void VideoDecoder::close()
{
    if (frame_) av_frame_free(&frame_);
    close_backend();
    if (ctx_) avcodec_free_context(&ctx_);
    has_pending_failure_ = false;
    pending_status_ = DecodeStatus::FatalError;
    pending_error_.clear();
}

void VideoDecoder::flush()
{
    if (ctx_) avcodec_flush_buffers(ctx_);
    if (frame_) av_frame_unref(frame_);
}

DecodeStatus VideoDecoder::failure_status(int averror) const
{
    // Corrupt bitstream is a sample-level problem: more input may well decode fine.
    if (averror == AVERROR_INVALIDDATA) return DecodeStatus::RecoverableIoError;
    // A hardware fault is worth exactly one software retry (the facade owns that decision and
    // only ever takes it once per session); a software fault is final.
    return kind() == VideoBackendKind::Software ? DecodeStatus::FatalError
                                                : DecodeStatus::RetryWithSoftware;
}

void VideoDecoder::record_failure(int averror, const std::string& message)
{
    has_pending_failure_ = true;
    pending_status_ = failure_status(averror);
    pending_error_ = message;
}

bool VideoDecoder::send_packet(const AVPacket* pkt, std::string& error)
{
    if (!ctx_) {
        error = "decoder is not open";
        return false;
    }
    const int ret = avcodec_send_packet(ctx_, pkt);
    if (ret >= 0 || ret == AVERROR_EOF) return true;
    error = "avcodec_send_packet: " + av_error_text(ret);
    record_failure(ret, error);
    return false;
}

DecodeStatus VideoDecoder::decode(VideoPicture& out, std::string& error)
{
    if (!ctx_ || !frame_) {
        error = "decoder is not open";
        return DecodeStatus::FatalError;
    }
    // A recorded send failure is reported by the next decode: callers see exactly one status for
    // any failure, and hardware faults get their single software-retry chance.
    if (has_pending_failure_) {
        has_pending_failure_ = false;
        error = pending_error_;
        pending_error_.clear();
        const DecodeStatus status = pending_status_;
        pending_status_ = DecodeStatus::FatalError;
        return status;
    }

    av_frame_unref(frame_);
    const int ret = avcodec_receive_frame(ctx_, frame_);
    if (ret == 0) return publish(frame_, out, error);
    if (ret == AVERROR_EOF) return DecodeStatus::Eof;
    if (ret == AVERROR(EAGAIN)) return DecodeStatus::WouldBlock;
    error = "avcodec_receive_frame: " + av_error_text(ret);
    return failure_status(ret);
}

bool VideoDecoder::upload(uint64_t /*serial*/, VideoPicture& /*out*/, std::string& error)
{
    error = "this decoder backend has no CPU picture to upload";
    return false;
}

// ---- hardware backend (FFmpeg d3d11va into the caller's device) -------------------------------

namespace {

class HardwareVideoDecoder final : public VideoDecoder
{
public:
    ~HardwareVideoDecoder() override { close(); }

    VideoBackendKind kind() const override { return VideoBackendKind::Hardware; }

    AVPixelFormat produced_format() const override
    {
        if (!ctx_ || !ctx_->hw_frames_ctx) return sw_format_;
        const AVHWFramesContext* frames =
            reinterpret_cast<const AVHWFramesContext*>(ctx_->hw_frames_ctx->data);
        return frames ? frames->sw_format : sw_format_;
    }

protected:
    bool prepare_codec(AVCodecContext* ctx, ID3D11Device* device, std::string& error) override;
    DecodeStatus publish(AVFrame* frame, VideoPicture& out, std::string& error) override;
    void close_backend() override;
    DecodeStatus failure_status(int averror) const override;

private:
    static AVPixelFormat get_format_thunk(AVCodecContext* ctx, const AVPixelFormat* formats);
    bool ensure_hw_frames_ctx(AVCodecContext* ctx);

    ID3D11Device* device_ = nullptr; // not owned
    AVBufferRef* hw_device_ref_ = nullptr;
    AVPixelFormat sw_format_ = AV_PIX_FMT_NONE;
};

bool HardwareVideoDecoder::prepare_codec(AVCodecContext* ctx, ID3D11Device* device,
                                        std::string& error)
{
    if (!device) {
        error = "hardware decode needs a D3D11 device";
        return false;
    }
    device_ = device;
    // Build an AVHWDeviceContext wrapping OUR d3d device instead of letting FFmpeg create its own
    // -- this is what makes the decoded texture live on the same device as our swapchain so we can
    // sample/copy it with zero copy.
    hw_device_ref_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_device_ref_) {
        error = "av_hwdevice_ctx_alloc failed";
        return false;
    }
    AVHWDeviceContext* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_ref_->data);
    AVD3D11VADeviceContext* d3d11_ctx =
        reinterpret_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);
    device_->AddRef(); // released by av_buffer_unref -> the hw device context's free callback
    d3d11_ctx->device = device_;

    if (av_hwdevice_ctx_init(hw_device_ref_) < 0) {
        error = "av_hwdevice_ctx_init failed";
        return false;
    }
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ref_);
    ctx->opaque = this;
    ctx->get_format = &HardwareVideoDecoder::get_format_thunk;
    ctx->thread_count = 1; // single-threaded decode: keeps this on one D3D11 call sequence
    return true;
}

AVPixelFormat HardwareVideoDecoder::get_format_thunk(AVCodecContext* ctx,
                                                    const AVPixelFormat* formats)
{
    HardwareVideoDecoder* self = static_cast<HardwareVideoDecoder*>(ctx->opaque);
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) {
            if (self->ensure_hw_frames_ctx(ctx)) return AV_PIX_FMT_D3D11;
        }
    }
    return AV_PIX_FMT_NONE;
}

bool HardwareVideoDecoder::ensure_hw_frames_ctx(AVCodecContext* ctx)
{
    if (ctx->hw_frames_ctx) return true;

    AVBufferRef* frames_ref = nullptr;
    int ret = avcodec_get_hw_frames_parameters(ctx, hw_device_ref_, AV_PIX_FMT_D3D11,
                                              &frames_ref);
    if (ret < 0) {
        fprintf(stderr, "avcodec_get_hw_frames_parameters failed: %d\n", ret);
        return false;
    }

    AVHWFramesContext* frames_ctx = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
    AVD3D11VAFramesContext* frames_hwctx =
        reinterpret_cast<AVD3D11VAFramesContext*>(frames_ctx->hwctx);

    // We need to be able to build SRVs directly on the decoder's own texture array slices -- that's
    // the whole point of sharing the D3D11 device with the swapchain (no readback, no copy).
    // D3D11_BIND_DECODER is already implied by the decoder; add SHADER_RESOURCE so the same texture
    // can be bound as a shader input.
    frames_hwctx->BindFlags |= D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    frames_hwctx->MiscFlags = 0;

    // A couple of extra slices of headroom beyond what the decoder asked for so the present/copy
    // side reading the "current" slice never races a decoder that wants to reuse it immediately.
    if (frames_ctx->initial_pool_size > 0) frames_ctx->initial_pool_size += 4;

    ret = av_hwframe_ctx_init(frames_ref);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "av_hwframe_ctx_init failed: %s\n", errbuf);
        av_buffer_unref(&frames_ref);
        return false;
    }
    sw_format_ = frames_ctx->sw_format;
    ctx->hw_frames_ctx = frames_ref; // transfers ownership
    return true;
}

DecodeStatus HardwareVideoDecoder::publish(AVFrame* frame, VideoPicture& out, std::string& error)
{
    // AVFrame with format == AV_PIX_FMT_D3D11:
    //   data[0] -> ID3D11Texture2D* (a texture ARRAY; one physical texture backs the whole pool)
    //   data[1] -> array slice index for this frame, an integer cast to a pointer, not a pointer.
    if (!frame || frame->format != AV_PIX_FMT_D3D11 || !frame->data[0]) {
        const char* name = frame ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format))
                                 : nullptr;
        char buf[192];
        snprintf(buf, sizeof(buf),
            "hardware decoder produced an unexpected surface (format %s, texture %s)",
            name ? name : "none", (frame && frame->data[0]) ? "present" : "missing");
        error = buf;
        return DecodeStatus::RetryWithSoftware;
    }
    out = VideoPicture{};
    out.frame = frame;
    out.texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    out.array_slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
    out.software = false;
    return DecodeStatus::Frame;
}

DecodeStatus HardwareVideoDecoder::failure_status(int averror) const
{
    if (averror == AVERROR_INVALIDDATA) return DecodeStatus::RecoverableIoError;
    // Device removal, an unsupported mid-stream parameter change, a decoder that cannot finish on
    // this GPU: all of those can still be played in software.
    return DecodeStatus::RetryWithSoftware;
}

void HardwareVideoDecoder::close_backend()
{
    if (hw_device_ref_) av_buffer_unref(&hw_device_ref_);
    sw_format_ = AV_PIX_FMT_NONE;
    device_ = nullptr;
}

// ---- software backend (libavcodec -> libswscale -> one NV12 texture) --------------------------

class SoftwareVideoDecoder final : public VideoDecoder
{
public:
    ~SoftwareVideoDecoder() override { close(); }

    VideoBackendKind kind() const override { return VideoBackendKind::Software; }
    AVPixelFormat produced_format() const override { return AV_PIX_FMT_NV12; }

    bool upload(uint64_t serial, VideoPicture& out, std::string& error) override;

protected:
    bool prepare_codec(AVCodecContext* ctx, ID3D11Device* device, std::string& error) override;
    DecodeStatus publish(AVFrame* frame, VideoPicture& out, std::string& error) override;
    void close_backend() override;

private:
    // Colour interpretation the current swscale context was built with.
    struct ScalerState
    {
        AVPixelFormat format = AV_PIX_FMT_NONE;
        int width = 0;
        int height = 0;
        int dst_width = 0;
        int dst_height = 0;
        int matrix = 0;
        bool full_range = false;
    };

    bool ensure_target(int width, int height, std::string& error);
    bool ensure_scaler(const AVFrame* frame, int dst_width, int dst_height, std::string& error);

    ID3D11Device* device_ = nullptr; // not owned
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    // GPU surface handed to the pipeline, plus the CPU-writable twin its pictures are staged through.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    int texture_width_ = 0;
    int texture_height_ = 0;
    std::vector<uint8_t> buffer_; // packed NV12: luma rows, then interleaved chroma rows
    SwsContext* scaler_ = nullptr;
    ScalerState scaler_state_{};
    uint64_t next_serial_ = 0;
    uint64_t published_serial_ = 0;
};

bool SoftwareVideoDecoder::prepare_codec(AVCodecContext* ctx, ID3D11Device* device,
                                        std::string& error)
{
    // The upload target lives on the caller's device; without one a software frame could never
    // reach the present path, so fail here instead of at the first frame.
    if (!device) {
        error = "software decode needs a D3D11 device for the NV12 upload target";
        return false;
    }
    device_ = device;
    // CPU decode: let FFmpeg pick the thread count (frame threading is pure throughput here, and
    // this backend is only ever driven from the single decode thread).
    ctx->thread_count = 0;
    return true;
}

bool SoftwareVideoDecoder::ensure_target(int width, int height, std::string& error)
{
    if (texture_ && texture_width_ == width && texture_height_ == height) return true;
    texture_.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    // The normalizer samples this surface when the source carries a display matrix, and the
    // decode-side copy reads it either way.
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &texture_);
    if (FAILED(hr)) {
        char buf[192];
        snprintf(buf, sizeof(buf), "CreateTexture2D(NV12 %dx%d upload target) failed: hr=0x%08lx",
            width, height, static_cast<unsigned long>(hr));
        error = buf;
        return false;
    }
    // Staging twin: CPU-visible so each decoded picture can be written into the driver's real
    // (aligned) plane pitches and then handed to the GPU surface in one copy. UpdateSubresource
    // with a tightly packed NV12 buffer does not reliably land on every plane/pitch here.
    D3D11_TEXTURE2D_DESC staging = td;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateTexture2D(&staging, nullptr, &staging_);
    if (FAILED(hr)) {
        char buf[192];
        snprintf(buf, sizeof(buf), "CreateTexture2D(NV12 %dx%d staging) failed: hr=0x%08lx",
            width, height, static_cast<unsigned long>(hr));
        error = buf;
        texture_.Reset();
        return false;
    }
    buffer_.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2, 0);
    texture_width_ = width;
    texture_height_ = height;
    return true;
}

bool SoftwareVideoDecoder::ensure_scaler(const AVFrame* frame, int dst_width, int dst_height,
                                        std::string& error)
{
    // Untagged content keeps the project's BT.709 contract (see decoder.cpp's interpret_colour):
    // the software path emits the same BT.709 limited NV12 the hardware path normalizes to, so an
    // untagged 4:2:0 source must not be re-matrixed here either.
    int matrix = SWS_CS_ITU709;
    switch (frame->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        matrix = SWS_CS_ITU601;
        break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: // constant-luminance content is vanishingly rare; treat as NCL
        matrix = SWS_CS_BT2020;
        break;
    default:
        break;
    }
    const bool full_range = frame->color_range == AVCOL_RANGE_JPEG;
    const ScalerState want{static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
        dst_width, dst_height, matrix, full_range};
    const bool same = scaler_ && scaler_state_.format == want.format &&
        scaler_state_.width == want.width && scaler_state_.height == want.height &&
        scaler_state_.dst_width == want.dst_width && scaler_state_.dst_height == want.dst_height &&
        scaler_state_.matrix == want.matrix && scaler_state_.full_range == want.full_range;
    if (same) return true;
    if (scaler_) sws_freeContext(scaler_);
    scaler_ = nullptr;
    // No scaling in the common case; SWS_BILINEAR covers the odd-dimension rounding and any
    // mid-stream size change without a second code path.
    scaler_ = sws_getContext(frame->width, frame->height, want.format, dst_width, dst_height,
        AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scaler_) {
        const char* name = av_get_pix_fmt_name(want.format);
        error = std::string("libswscale cannot convert ") + (name ? name : "this format") +
            " to NV12";
        return false;
    }
    // Interpret the source with its own matrix/range and emit the downstream contract: BT.709,
    // limited range, 8 bit. Transfer/primaries are NOT tone mapped here (compatibility mode) --
    // the hardware path keeps that precision, the software path stays playable.
    sws_setColorspaceDetails(scaler_, sws_getCoefficients(matrix), full_range ? 1 : 0,
        sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);
    scaler_state_ = want;
    return true;
}

DecodeStatus SoftwareVideoDecoder::publish(AVFrame* frame, VideoPicture& out, std::string& error)
{
    if (!frame || frame->format == AV_PIX_FMT_NONE || frame->width <= 0 || frame->height <= 0) {
        error = "software decoder produced an invalid frame";
        return DecodeStatus::FatalError;
    }
    // NV12 needs even dimensions; a visible odd width/height keeps its last column/row as padding
    // (the normalizer crops to the visible size when it runs).
    const int target_width = (frame->width + 1) & ~1;
    const int target_height = (frame->height + 1) & ~1;
    if (!ensure_scaler(frame, target_width, target_height, error)) {
        error = "software decode: " + error;
        return DecodeStatus::FatalError;
    }
    if (!ensure_target(target_width, target_height, error)) return DecodeStatus::FatalError;

    uint8_t* dst[4] = {buffer_.data(),
        buffer_.data() + static_cast<size_t>(target_width) * static_cast<size_t>(target_height),
        nullptr, nullptr};
    const int dst_linesize[4] = {target_width, target_width, 0, 0};
    const int rows = sws_scale(scaler_, frame->data, frame->linesize, 0, frame->height, dst,
        dst_linesize);
    if (rows <= 0) {
        error = "libswscale produced no rows";
        return DecodeStatus::FatalError;
    }

    out = VideoPicture{};
    out.frame = frame;
    out.texture = nullptr; // uploaded by upload(), under the caller's D3D serialization
    out.array_slice = 0;
    out.software = true;
    out.serial = ++next_serial_;
    out.cpu_plane[0] = buffer_.data();
    out.cpu_plane[1] = dst[1];
    out.cpu_linesize[0] = target_width;
    out.cpu_linesize[1] = target_width;
    published_serial_ = out.serial;
    return DecodeStatus::Frame;
}

bool SoftwareVideoDecoder::upload(uint64_t serial, VideoPicture& out, std::string& error)
{
    if (!texture_ || serial == 0 || serial != published_serial_) {
        error = "software frame is no longer the decoded picture (stale upload)";
        return false;
    }
    if (!context_) {
        context_.Attach(immediate_context(device_)); // takes over the reference
        if (!context_) {
            error = "software decode: no immediate D3D11 context";
            return false;
        }
    }
    // Write the decoded picture into the staging surface honouring its real (driver-aligned) plane
    // pitch, then hand the whole subresource to the GPU texture in one copy. Map reports plane 0 in
    // pData/RowPitch; the chroma plane follows the luma plane directly (height rows of RowPitch),
    // shares that pitch and is half as tall (4:2:0). DepthPitch is not used: for video formats it
    // is left at 0 by the runtime.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) {
        error = "software decode: Map(NV12 staging) failed";
        return false;
    }
    auto* luma = static_cast<uint8_t*>(mapped.pData);
    auto* chroma = luma + static_cast<size_t>(mapped.RowPitch) * static_cast<size_t>(texture_height_);
    const size_t row_bytes = static_cast<size_t>(texture_width_);
    for (int y = 0; y < texture_height_; ++y)
        std::memcpy(luma + static_cast<size_t>(y) * mapped.RowPitch,
            buffer_.data() + static_cast<size_t>(y) * row_bytes, row_bytes);
    for (int y = 0; y < texture_height_ / 2; ++y)
        std::memcpy(chroma + static_cast<size_t>(y) * mapped.RowPitch,
            buffer_.data() + static_cast<size_t>(texture_height_) * row_bytes +
                static_cast<size_t>(y) * row_bytes,
            row_bytes);
    context_->Unmap(staging_.Get(), 0);
    context_->CopyResource(texture_.Get(), staging_.Get());
    out.texture = texture_.Get();
    out.array_slice = 0;
    return true;
}

void SoftwareVideoDecoder::close_backend()
{
    if (scaler_) sws_freeContext(scaler_);
    scaler_ = nullptr;
    scaler_state_ = ScalerState{};
    texture_.Reset();
    staging_.Reset();
    context_.Reset();
    buffer_.clear();
    buffer_.shrink_to_fit();
    texture_width_ = 0;
    texture_height_ = 0;
    next_serial_ = 0;
    published_serial_ = 0;
    device_ = nullptr;
}

} // namespace

std::unique_ptr<VideoDecoder> create_hardware_video_decoder()
{
    return std::make_unique<HardwareVideoDecoder>();
}

std::unique_ptr<VideoDecoder> create_software_video_decoder()
{
    return std::make_unique<SoftwareVideoDecoder>();
}
