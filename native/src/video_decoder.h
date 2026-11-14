// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// The codec half of a decode session: one VideoDecoder decodes one stream, either
//
//   * hardware  -- FFmpeg d3d11va into NV12/P010 D3D11 textures on the CALLER's device (shared
//                  with the swapchain: no readback, no re-upload), or
//   * software  -- libavcodec to CPU frames, libswscale to BT.709 limited 8-bit NV12, then one
//                  upload into a reusable NV12 texture on that same device.
//
// Demuxing, seeking and reconnect live in MediaSource. A decoder is fed packets and produces
// pictures; it never knows where they came from. Pictures are only valid until the next
// decode()/flush()/close() call on the same object ("one picture in flight" bounds VRAM use and
// decoder pool pressure).
//
// The software picture's GPU upload is deferred to upload(): it writes GPU state through the
// immediate context, so it must run under the caller's D3D context serialization
// (Decoder::prepare_nv12_frame), never from the bare decode call.

#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

// One picture produced by a backend. `frame` stays owned by the decoder; `texture` is the
// GPU-resident surface holding it (hardware: the decoder's own pool slice; software: null until
// upload() writes the CPU planes into the reusable NV12 texture).
struct VideoPicture
{
    AVFrame* frame = nullptr;
    ID3D11Texture2D* texture = nullptr;
    UINT array_slice = 0;
    bool software = false;
    // Identifies the contents of the software CPU buffer: upload() refuses a picture that is no
    // longer the one just published, so a stale DecodedFrame can never upload the wrong pixels.
    uint64_t serial = 0;
    const uint8_t* cpu_plane[2] = {nullptr, nullptr}; // NV12 luma, interleaved chroma
    int cpu_linesize[2] = {0, 0};
};

class VideoDecoder
{
public:
    virtual ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    virtual VideoBackendKind kind() const = 0;

    // Pixel format of the decoded pictures: hardware -- the D3D11VA frames context's sw_format
    // (NV12 / P010); software -- AV_PIX_FMT_NV12, the uploaded contract.
    virtual AVPixelFormat produced_format() const = 0;

    // `device` may be null for the software backend; the hardware backend requires it.
    bool open(const AVCodecParameters* params, AVRational time_base, AVRational framerate,
              ID3D11Device* device, std::string& error);
    void close();
    bool is_open() const { return ctx_ != nullptr; }

    // Packet feed. `pkt` == nullptr signals end of stream (drain). A false return only records the
    // failure: its disposition comes from the next decode(), which is the one place a hardware
    // fault can be turned into RetryWithSoftware.
    bool send_packet(const AVPacket* pkt, std::string& error);

    // Frame | WouldBlock (needs more input) | Eof | RetryWithSoftware | RecoverableIoError |
    // FatalError. RecoverableIoError means the bitstream was corrupt: feed more input and keep
    // going.
    DecodeStatus decode(VideoPicture& out, std::string& error);

    void flush();

    // Software backends: writes the picture identified by `serial` into the reusable NV12 texture
    // and reports it through `out`. Hardware backends return false (their pictures are already
    // GPU-resident). Caller must hold the D3D context serialization.
    virtual bool upload(uint64_t serial, VideoPicture& out, std::string& error);

protected:
    VideoDecoder() = default;

    // Called with a configured AVCodecContext, before avcodec_open2(): a hardware backend installs
    // its device/frames context here, the software backend its thread setup.
    virtual bool prepare_codec(AVCodecContext* ctx, ID3D11Device* device,
                               std::string& error) = 0;
    // Turns a freshly received AVFrame into `out`.
    virtual DecodeStatus publish(AVFrame* frame, VideoPicture& out, std::string& error) = 0;
    virtual void close_backend() = 0;
    // Disposition of a codec-level error: corrupt data stays recoverable, a hardware fault is worth
    // one software retry, a software fault is final.
    virtual DecodeStatus failure_status(int averror) const;

    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;

private:
    // Disposition of a failed send: reported by the next decode() so there is exactly one place
    // that decides between RetryWithSoftware and FatalError.
    void record_failure(int averror, const std::string& message);

    bool has_pending_failure_ = false;
    DecodeStatus pending_status_ = DecodeStatus::FatalError;
    std::string pending_error_;
};

std::unique_ptr<VideoDecoder> create_hardware_video_decoder();
std::unique_ptr<VideoDecoder> create_software_video_decoder();
