// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "decoder.h"

#include <cstdio>

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
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (hw_device_ref_) av_buffer_unref(&hw_device_ref_);
    if (fmt_ctx_) avformat_close_input(&fmt_ctx_);
    video_stream_idx_ = -1;
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
    // slices -- that's the whole point of sharing the D3D11 device with the swapchain
    // (no readback, no copy). D3D11_BIND_DECODER is already implied by the decoder;
    // add SHADER_RESOURCE so the same texture can be bound as a shader input.
    frames_hwctx->BindFlags |= D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    frames_hwctx->MiscFlags = 0;

    // A couple of extra slices of headroom beyond what the decoder asked for so the
    // present side reading the "current" slice never races a decoder that wants to
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

bool Decoder::open(const std::string& path, ID3D11Device* device, std::string& error)
{
    d3d_device_ = device;

    if (avformat_open_input(&fmt_ctx_, path.c_str(), nullptr, nullptr) < 0) {
        error = "avformat_open_input failed for " + path;
        return false;
    }
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        error = "avformat_find_stream_info failed";
        return false;
    }

    const AVCodec* decoder = nullptr;
    int idx = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (idx < 0) {
        error = "no video stream found";
        return false;
    }
    video_stream_idx_ = idx;
    AVStream* stream = fmt_ctx_->streams[idx];
    time_base_ = stream->time_base;
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
        fps_ = av_q2d(stream->avg_frame_rate);

    codec_ctx_ = avcodec_alloc_context3(decoder);
    if (!codec_ctx_) {
        error = "avcodec_alloc_context3 failed";
        return false;
    }
    if (avcodec_parameters_to_context(codec_ctx_, stream->codecpar) < 0) {
        error = "avcodec_parameters_to_context failed";
        return false;
    }

    // Build an AVHWDeviceContext wrapping OUR d3d device instead of letting FFmpeg
    // create its own -- this is what makes the decoded texture live on the same device
    // as our swapchain so we can sample it with zero copy.
    hw_device_ref_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_device_ref_) {
        error = "av_hwdevice_ctx_alloc failed";
        return false;
    }
    AVHWDeviceContext* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_ref_->data);
    AVD3D11VADeviceContext* d3d11_ctx = reinterpret_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);
    d3d_device_->AddRef();
    d3d11_ctx->device = d3d_device_;

    if (av_hwdevice_ctx_init(hw_device_ref_) < 0) {
        error = "av_hwdevice_ctx_init failed";
        return false;
    }

    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ref_);
    codec_ctx_->opaque = this;
    codec_ctx_->get_format = &Decoder::get_hw_format_thunk;
    codec_ctx_->thread_count = 1; // single-threaded decode: keeps this whole spike on one D3D11 call sequence

    AVDictionary* opts = nullptr;
    if (avcodec_open2(codec_ctx_, decoder, &opts) < 0) {
        av_dict_free(&opts);
        error = "avcodec_open2 failed";
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
    have_first_pts_ = false; // re-anchor the pts->seconds origin to the new first frame
    return ret >= 0;
}

bool Decoder::next_frame(DecodedFrame& out)
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

            double raw_pts_s = (frame_->pts != AV_NOPTS_VALUE)
                ? frame_->pts * av_q2d(time_base_)
                : (last_out_pts_seconds_ + 1.0 / fps_);

            if (!have_first_pts_) {
                first_pts_seconds_ = raw_pts_s;
                have_first_pts_ = true;
            }
            double pts_s = loop_offset_seconds_ + (raw_pts_s - first_pts_seconds_);
            out.looped = false;
            out.pts_seconds = pts_s;
            last_out_pts_seconds_ = pts_s;
            return true;
        }
        if (recv == AVERROR(EAGAIN)) {
            // need more packets
            int read = av_read_frame(fmt_ctx_, pkt_);
            if (read >= 0) {
                if (pkt_->stream_index == video_stream_idx_) {
                    int send = avcodec_send_packet(codec_ctx_, pkt_);
                    if (send < 0 && send != AVERROR(EAGAIN)) {
                        av_packet_unref(pkt_);
                        fprintf(stderr, "avcodec_send_packet failed: %d\n", send);
                        return false;
                    }
                }
                av_packet_unref(pkt_);
                continue;
            }
            // EOF (or read error): flush decoder, then loop playback back to time 0.
            avcodec_send_packet(codec_ctx_, nullptr);
            int drain = avcodec_receive_frame(codec_ctx_, frame_);
            if (drain == 0) {
                out.texture = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
                out.array_slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame_->data[1]));
                double raw_pts_s = (frame_->pts != AV_NOPTS_VALUE)
                    ? frame_->pts * av_q2d(time_base_)
                    : (last_out_pts_seconds_ + 1.0 / fps_);
                double pts_s = loop_offset_seconds_ + (raw_pts_s - first_pts_seconds_);
                out.looped = false;
                out.pts_seconds = pts_s;
                last_out_pts_seconds_ = pts_s;
                // Prime the loop for next time: after this drained frame we still need
                // to seek back, do it lazily on the NEXT call by falling through once
                // more (handled below via a pending-seek flag would be cleaner, but for
                // this spike we just seek right away and return this last frame now).
                seek_to_start();
                loop_offset_seconds_ = last_out_pts_seconds_ + 1.0 / fps_;
                return true;
            }
            // Nothing left to drain: seek back to start and keep looping.
            if (!seek_to_start()) {
                fprintf(stderr, "seek_to_start failed, stopping\n");
                return false;
            }
            loop_offset_seconds_ = last_out_pts_seconds_ + 1.0 / fps_;
            continue;
        }
        fprintf(stderr, "avcodec_receive_frame failed: %d\n", recv);
        return false;
    }
}
