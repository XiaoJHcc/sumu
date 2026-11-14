// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// WASAPI audio thread. Ownership: every COM/WASAPI object, SwrContext and AVAudioFifo below is
// touched only by this thread, inside one CoInitializeEx lifetime; the endpoint is torn down and
// re-opened in place on device loss, default-output switch or explicit rebuild request.
// Cross-thread inputs are only player.h's relaxed atomics plus MediaSource's audio queue (its own
// mutex); master time comes from compute_master_s() (player_engine.cpp) -- audio is a slave of
// present's QPC clock and never the reverse. This thread never takes d3d_mutex_/ready_mutex_/
// decoder_mutex_.

#include "player.h"

namespace {

constexpr double kLeadThresholdS = 0.035; // audio this far ahead of master is held back
constexpr double kLagThresholdS = 0.035;  // this far behind is dropped to catch up

constexpr DWORD kWakeWaitMs = 100;           // one event wait; also the stop/rebuild reaction bound
constexpr double kDevicePollS = 0.5;         // default-endpoint liveness re-check cadence
constexpr double kLogIntervalS = 1.0;
constexpr double kFailureLogIntervalS = 5.0; // rate limit for repeated open failures
constexpr double kBackoffMinMs = 100.0;
constexpr double kBackoffMaxMs = 2000.0;
constexpr REFERENCE_TIME kBufferDuration100ns = 200 * 10000; // 200ms shared-mode buffer

std::string wide_to_utf8(const wchar_t* w)
{
    if (!w || !*w) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::string s(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], need, nullptr, nullptr);
    s.resize(static_cast<size_t>(need) - 1);
    return s;
}

std::string hr_text(const char* what, HRESULT hr)
{
    char buf[192];
    snprintf(buf, sizeof(buf), "%s hr=0x%08lx", what, static_cast<unsigned long>(hr));
    return std::string(buf);
}

struct Endpoint {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* mix_format = nullptr;
    HANDLE event = nullptr;
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;
    UINT32 buffer_frames = 0;
    int out_channels = 0;
    int out_sample_rate = 0;
    size_t block_align = 0;
    AVSampleFormat out_fmt = AV_SAMPLE_FMT_NONE;
    std::string device_id;
    bool started = false;

    bool valid() const { return client != nullptr; }
};

// Stop() must precede Release(), render client before audio client; every pointer is cleared so
// a half-built or already closed endpoint can be closed again.
void close_endpoint(Endpoint& ep)
{
    if (ep.client && ep.started) ep.client->Stop();
    ep.started = false;
    if (ep.render) { ep.render->Release(); ep.render = nullptr; }
    if (ep.client) { ep.client->Release(); ep.client = nullptr; }
    if (ep.mix_format) { CoTaskMemFree(ep.mix_format); ep.mix_format = nullptr; }
    if (ep.event) { CloseHandle(ep.event); ep.event = nullptr; }
    if (ep.device) { ep.device->Release(); ep.device = nullptr; }
    if (ep.enumerator) { ep.enumerator->Release(); ep.enumerator = nullptr; }
    if (ep.swr) swr_free(&ep.swr);
    if (ep.fifo) av_audio_fifo_free(ep.fifo);
    ep.fifo = nullptr;
    ep.buffer_frames = 0;
    ep.out_channels = 0;
    ep.out_sample_rate = 0;
    ep.block_align = 0;
    ep.out_fmt = AV_SAMPLE_FMT_NONE;
    ep.device_id.clear();
}

bool open_endpoint(Endpoint& ep, const AVCodecContext* src, std::string& err)
{
    close_endpoint(ep);

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&ep.enumerator));
    if (FAILED(hr)) { err = hr_text("CoCreateInstance(MMDeviceEnumerator)", hr); close_endpoint(ep); return false; }

    hr = ep.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &ep.device);
    if (FAILED(hr)) { err = hr_text("GetDefaultAudioEndpoint", hr); close_endpoint(ep); return false; }

    LPWSTR device_id_w = nullptr;
    if (SUCCEEDED(ep.device->GetId(&device_id_w)) && device_id_w) {
        ep.device_id = wide_to_utf8(device_id_w);
        CoTaskMemFree(device_id_w);
    }

    hr = ep.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&ep.client));
    if (FAILED(hr)) { err = hr_text("IMMDevice::Activate(IAudioClient)", hr); close_endpoint(ep); return false; }

    hr = ep.client->GetMixFormat(&ep.mix_format);
    if (FAILED(hr) || !ep.mix_format) { err = hr_text("GetMixFormat", hr); close_endpoint(ep); return false; }

    ep.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ep.event) { err = "CreateEventW failed"; close_endpoint(ep); return false; }

    hr = ep.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        kBufferDuration100ns, 0, ep.mix_format, nullptr);
    if (FAILED(hr)) { err = hr_text("IAudioClient::Initialize", hr); close_endpoint(ep); return false; }

    hr = ep.client->SetEventHandle(ep.event);
    if (FAILED(hr)) { err = hr_text("SetEventHandle", hr); close_endpoint(ep); return false; }

    hr = ep.client->GetBufferSize(&ep.buffer_frames);
    if (FAILED(hr)) { err = hr_text("GetBufferSize", hr); close_endpoint(ep); return false; }

    hr = ep.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&ep.render));
    if (FAILED(hr)) { err = hr_text("GetService(IAudioRenderClient)", hr); close_endpoint(ep); return false; }

    // Shared-mode mix format is float32 in practice; integer PCM handled too.
    const WAVEFORMATEXTENSIBLE* wfex = nullptr;
    if (ep.mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ep.mix_format->cbSize >= 22)
        wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(ep.mix_format);
    const bool is_float = (ep.mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        (wfex && IsEqualGUID(wfex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    if (is_float) {
        ep.out_fmt = AV_SAMPLE_FMT_FLT;
    } else if (ep.mix_format->wBitsPerSample == 16) {
        ep.out_fmt = AV_SAMPLE_FMT_S16;
    } else if (ep.mix_format->wBitsPerSample == 32) {
        ep.out_fmt = AV_SAMPLE_FMT_S32;
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "unrecognized WASAPI mix format (tag=%u bits=%u)",
            ep.mix_format->wFormatTag, ep.mix_format->wBitsPerSample);
        err = buf;
        close_endpoint(ep);
        return false;
    }

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, static_cast<int>(ep.mix_format->nChannels));
    int swr_ret = swr_alloc_set_opts2(&ep.swr,
        &out_layout, ep.out_fmt, static_cast<int>(ep.mix_format->nSamplesPerSec),
        &src->ch_layout, src->sample_fmt, src->sample_rate,
        0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (swr_ret < 0 || !ep.swr || swr_init(ep.swr) < 0) {
        char buf[160];
        snprintf(buf, sizeof(buf), "swr_alloc_set_opts2/swr_init failed: %d", swr_ret);
        err = buf;
        close_endpoint(ep);
        return false;
    }

    ep.out_channels = static_cast<int>(ep.mix_format->nChannels);
    ep.out_sample_rate = static_cast<int>(ep.mix_format->nSamplesPerSec);
    ep.block_align = ep.mix_format->nBlockAlign;
    ep.fifo = av_audio_fifo_alloc(ep.out_fmt, ep.out_channels, ep.out_sample_rate); // 1s headroom
    if (!ep.fifo) { err = "av_audio_fifo_alloc failed"; close_endpoint(ep); return false; }

    hr = ep.client->Start();
    if (FAILED(hr)) { err = hr_text("IAudioClient::Start", hr); close_endpoint(ep); return false; }
    ep.started = true;
    return true;
}

} // namespace

void Player::audio_loop(){
    // CoInitializeEx on a fresh thread only fails in a broken COM environment; retry briefly so a
    // transient failure does not silence the session, then give up (playback just stays silent).
    HRESULT co_hr = E_FAIL;
    for (int attempt = 0; attempt < 10; ++attempt) {
        co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(co_hr)) break;
        if (stop_.load(std::memory_order_relaxed) ||
            session_stop_.load(std::memory_order_relaxed)) break;
        Sleep(200);
    }
    if (FAILED(co_hr)) {
        fprintf(stderr, "[audio] CoInitializeEx failed: hr=0x%08lx, audio disabled\n",
            static_cast<unsigned long>(co_hr));
        std::lock_guard<std::mutex> lk(audio_info_mutex_);
        audio_last_error_ = hr_text("CoInitializeEx", co_hr);
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!pkt || !frame) {
        fprintf(stderr, "[audio] AVPacket/AVFrame allocation failed, audio disabled\n");
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        CoUninitialize();
        return;
    }

    AVFrame* held_frame = nullptr;        // decoded frame held back until master catches up
    double held_frame_loop_offset = 0.0;

    const AVRational audio_tb = decoder_.audio_time_base();
    const double first_pts_s = decoder_.first_pts_seconds();

    double next_raw_pts_s = first_pts_s;       // fallback estimate for frames without PTS
    double pending_loop_offset = 0.0;
    double last_written_audio_s = first_pts_s; // for the drift log
    float last_peak = 0.0f;
    uint64_t underruns = 0;
    std::vector<uint8_t> resample_buf;
    uint64_t local_seek_version = seek_version_.load(std::memory_order_relaxed);
    uint64_t local_rebuild_request = audio_rebuild_request_.load(std::memory_order_relaxed);

    LARGE_INTEGER log_t0;
    QueryPerformanceCounter(&log_t0);
    double last_log_wall_s = 0.0;
    double last_fail_log_wall_s = -1e9;
    int consecutive_failures = 0;

    Endpoint ep;
    double backoff_ms = kBackoffMinMs;
    bool attempt_now = true;
    LARGE_INTEGER next_attempt_qpc{};
    LARGE_INTEGER next_poll_qpc{};
    const double qpc_to_ms = 1000.0 / static_cast<double>(freq_.QuadPart);
    const LONGLONG poll_ticks = static_cast<LONGLONG>(kDevicePollS * static_cast<double>(freq_.QuadPart));

    auto stopping = [&]() {
        return stop_.load(std::memory_order_relaxed) ||
            session_stop_.load(std::memory_order_relaxed);
    };
    auto qpc_now = [&]() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return n; };
    auto wall_now_s = [&]() {
        LARGE_INTEGER n;
        QueryPerformanceCounter(&n);
        return static_cast<double>(n.QuadPart - log_t0.QuadPart) / static_cast<double>(freq_.QuadPart);
    };
    auto ms_until = [&](const LARGE_INTEGER& deadline) {
        return static_cast<double>(deadline.QuadPart - qpc_now().QuadPart) * qpc_to_ms;
    };
    // Chopped so close()/reopen() (session_stop_ then join) never waits out a backoff.
    auto sleep_interruptible_ms = [&](double ms) {
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        while (!stopping()) {
            const double elapsed_ms = static_cast<double>(qpc_now().QuadPart - t0.QuadPart) * qpc_to_ms;
            if (elapsed_ms >= ms) return;
            Sleep(static_cast<DWORD>(std::min(10.0, ms - elapsed_ms)));
        }
    };
    // Drop position state and re-sync the drift baseline, so nothing from the old position or the
    // old device is ever pushed to the new one.
    auto drop_playback_state = [&](bool reset_fifo) {
        if (held_frame) av_frame_free(&held_frame);
        if (reset_fifo && ep.fifo) av_audio_fifo_reset(ep.fifo);
        if (audio_codec_ctx_) avcodec_flush_buffers(audio_codec_ctx_);
        last_written_audio_s = compute_master_s();
    };
    auto invalidate = [&](const char* why) {
        if (!ep.valid()) return;
        fprintf(stderr, "[audio] endpoint lost (%s) -- rebuilding\n", why);
        close_endpoint(ep);
        audio_device_active_.store(false, std::memory_order_relaxed);
        audio_invalidation_count_.fetch_add(1, std::memory_order_relaxed);
        drop_playback_state(false);
        attempt_now = true;
        consecutive_failures = 0;
        backoff_ms = kBackoffMinMs;
    };
    auto query_default_id = [&](std::string& id_out) -> HRESULT {
        if (!ep.enumerator) return E_FAIL;
        IMMDevice* dev = nullptr;
        HRESULT r = ep.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (FAILED(r)) return r;
        LPWSTR id_w = nullptr;
        r = dev->GetId(&id_w);
        if (SUCCEEDED(r) && id_w) {
            id_out = wide_to_utf8(id_w);
            CoTaskMemFree(id_w);
        }
        dev->Release();
        return r;
    };

    // One device wake: top the shared-mode buffer up with FIFO PCM (playing) or silence (paused).
    // false + `reason` means the endpoint must be rebuilt.
    auto service_wake = [&](std::string& reason) -> bool {
        const bool playing = playing_.load(std::memory_order_relaxed);

        UINT32 padding = 0;
        HRESULT hr = ep.client->GetCurrentPadding(&padding);
        if (FAILED(hr)) { reason = hr_text("GetCurrentPadding", hr); return false; }
        const UINT32 available = (ep.buffer_frames > padding) ? (ep.buffer_frames - padding) : 0;
        if (available == 0) return true;

        if (!playing) {
            // Paused: silence only -- the FIFO and the decoder stay untouched, master is frozen.
            BYTE* data = nullptr;
            hr = ep.render->GetBuffer(available, &data);
            if (FAILED(hr)) { reason = hr_text("GetBuffer (silence)", hr); return false; }
            hr = ep.render->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT);
            if (FAILED(hr)) { reason = hr_text("ReleaseBuffer (silence)", hr); return false; }
            return true;
        }

        while (static_cast<UINT32>(std::max(av_audio_fifo_size(ep.fifo), 0)) < available) {
            if (stopping()) return true; // never hold up close() behind a long catch-up backlog

            AVFrame* cur = nullptr;
            double cur_loop_offset = 0.0;
            if (held_frame) {
                cur = held_frame;
                cur_loop_offset = held_frame_loop_offset;
            } else {
                int recv = avcodec_receive_frame(audio_codec_ctx_, frame);
                if (recv == 0) {
                    cur = frame;
                    cur_loop_offset = pending_loop_offset;
                } else if (recv == AVERROR(EAGAIN)) {
                    double popped_loop_offset = 0.0;
                    if (!decoder_.pop_audio_packet(pkt, popped_loop_offset)) break;
                    pending_loop_offset = popped_loop_offset;
                    int send = avcodec_send_packet(audio_codec_ctx_, pkt);
                    av_packet_unref(pkt);
                    if (send < 0 && send != AVERROR(EAGAIN)) break;
                    continue;
                } else {
                    break; // decoder error/EOF this tick, retry next wake
                }
            }

            double raw_pts_s = (cur->pts != AV_NOPTS_VALUE)
                ? static_cast<double>(cur->pts) * av_q2d(audio_tb)
                : next_raw_pts_s;
            int frame_sr = (cur->sample_rate > 0) ? cur->sample_rate : audio_codec_ctx_->sample_rate;
            next_raw_pts_s = raw_pts_s + static_cast<double>(cur->nb_samples) / static_cast<double>(frame_sr);
            // Per-packet loop_offset keeps audio from a not-yet-drained pre-wrap packet in its own
            // (pre-wrap) time domain instead of the decoder-wide loop_offset_seconds_.
            double audio_s = cur_loop_offset + (raw_pts_s - first_pts_s);
            double master_s = compute_master_s();
            double diff = audio_s - master_s;

            if (diff > kLeadThresholdS) {
                if (!held_frame) {
                    held_frame = av_frame_alloc();
                    if (!held_frame) { av_frame_unref(frame); break; } // cannot hold: drop this frame
                    av_frame_move_ref(held_frame, frame);
                    held_frame_loop_offset = cur_loop_offset;
                }
                break;
            }

            bool was_held = (cur == held_frame);
            if (diff >= -kLagThresholdS) {
                int max_out = swr_get_out_samples(ep.swr, cur->nb_samples);
                if (max_out > 0) {
                    size_t need = static_cast<size_t>(max_out) * static_cast<size_t>(ep.out_channels) *
                        av_get_bytes_per_sample(ep.out_fmt);
                    if (resample_buf.size() < need) resample_buf.resize(need);
                    uint8_t* out_ptrs[1] = { resample_buf.data() };
                    int converted = swr_convert(ep.swr, out_ptrs, max_out,
                        const_cast<const uint8_t**>(cur->data), cur->nb_samples);
                    if (converted > 0) {
                        void* fifo_ptrs[1] = { resample_buf.data() };
                        av_audio_fifo_write(ep.fifo, fifo_ptrs, converted);
                        last_written_audio_s = audio_s;
                    }
                }
            }
            // else: too far behind master -- drop this frame's audio and catch up.

            if (was_held) { av_frame_free(&held_frame); }
            else { av_frame_unref(frame); }
        }

        UINT32 fifo_depth = static_cast<UINT32>(std::max(av_audio_fifo_size(ep.fifo), 0));
        UINT32 to_write = std::min(available, fifo_depth);
        BYTE* data = nullptr;
        hr = ep.render->GetBuffer(available, &data);
        if (FAILED(hr)) { reason = hr_text("GetBuffer", hr); return false; }

        UINT32 got = 0;
        if (to_write > 0) {
            void* out_ptrs[1] = { data };
            int r = av_audio_fifo_read(ep.fifo, out_ptrs, static_cast<int>(to_write));
            got = (r > 0) ? static_cast<UINT32>(r) : 0;
        }
        if (got > 0) {
            // Gain applied as late as possible so volume/mute takes effect within one device wake.
            float gain = muted_.load(std::memory_order_relaxed)
                ? 0.0f
                : std::clamp(volume_.load(std::memory_order_relaxed), 0.0f, 1.0f);
            size_t nsamples = static_cast<size_t>(got) * static_cast<size_t>(ep.out_channels);
            float peak = 0.0f;
            switch (ep.out_fmt) {
            case AV_SAMPLE_FMT_FLT: {
                float* s = reinterpret_cast<float*>(data);
                for (size_t i = 0; i < nsamples; ++i) {
                    s[i] *= gain;
                    peak = std::max(peak, std::fabs(s[i]));
                }
                break;
            }
            case AV_SAMPLE_FMT_S16: {
                int16_t* s = reinterpret_cast<int16_t*>(data);
                for (size_t i = 0; i < nsamples; ++i) {
                    s[i] = static_cast<int16_t>(std::lround(static_cast<float>(s[i]) * gain));
                    peak = std::max(peak, std::fabs(static_cast<float>(s[i]) / 32768.0f));
                }
                break;
            }
            case AV_SAMPLE_FMT_S32: {
                int32_t* s = reinterpret_cast<int32_t*>(data);
                for (size_t i = 0; i < nsamples; ++i) {
                    s[i] = static_cast<int32_t>(std::lround(static_cast<double>(s[i]) * gain));
                    peak = std::max(peak, static_cast<float>(std::fabs(static_cast<double>(s[i]) / 2147483648.0)));
                }
                break;
            }
            default: break;
            }
            last_peak = peak;
            audio_last_peak_.store(peak, std::memory_order_relaxed);
            audio_write_frames_.fetch_add(got, std::memory_order_relaxed);
        }
        if (got < available) {
            memset(data + static_cast<size_t>(got) * ep.block_align, 0,
                static_cast<size_t>(available - got) * ep.block_align);
            ++underruns;
            audio_underrun_count_.store(underruns, std::memory_order_relaxed);
        }
        hr = ep.render->ReleaseBuffer(available, (got == 0) ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
        if (FAILED(hr)) { reason = hr_text("ReleaseBuffer", hr); return false; }
        return true;
    };

    while (!stopping()) {
        // seek: Player::seek() already flushed the audio packet queue before bumping
        // seek_version_; here we drop our own pre-seek position state. Checked even with no
        // endpoint so a rebuild never replays pre-seek audio.
        const uint64_t sv = seek_version_.load(std::memory_order_relaxed);
        if (sv != local_seek_version) {
            local_seek_version = sv;
            drop_playback_state(true);
        }

        const uint64_t rv = audio_rebuild_request_.load(std::memory_order_relaxed);
        if (rv != local_rebuild_request) {
            local_rebuild_request = rv;
            if (ep.valid()) {
                fprintf(stderr, "[audio] rebuild requested -- reopening endpoint\n");
                close_endpoint(ep);
                audio_device_active_.store(false, std::memory_order_relaxed);
                drop_playback_state(false);
            }
            attempt_now = true;
            consecutive_failures = 0;
            backoff_ms = kBackoffMinMs;
        }

        if (!ep.valid()) {
            if (!attempt_now) {
                const double remaining_ms = ms_until(next_attempt_qpc);
                if (remaining_ms > 0.0) {
                    sleep_interruptible_ms(std::min(remaining_ms, 50.0));
                    continue;
                }
            }
            attempt_now = false;
            std::string err;
            if (open_endpoint(ep, audio_codec_ctx_, err)) {
                audio_build_count_.fetch_add(1, std::memory_order_relaxed);
                audio_device_active_.store(true, std::memory_order_relaxed);
                audio_out_sample_rate_.store(ep.out_sample_rate, std::memory_order_relaxed);
                audio_out_channels_.store(ep.out_channels, std::memory_order_relaxed);
                audio_buffer_frames_.store(static_cast<int>(ep.buffer_frames), std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(audio_info_mutex_);
                    audio_device_id_ = ep.device_id;
                    audio_last_error_.clear();
                }
                consecutive_failures = 0;
                backoff_ms = kBackoffMinMs;
                next_poll_qpc = qpc_now();
                next_poll_qpc.QuadPart += poll_ticks;
                drop_playback_state(false);
                fprintf(stderr, "[audio] endpoint ready: id=%s rate=%d ch=%d buffer=%u frames\n",
                    ep.device_id.empty() ? "(unknown)" : ep.device_id.c_str(),
                    ep.out_sample_rate, ep.out_channels, ep.buffer_frames);
                continue;
            }

            audio_failure_count_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(audio_info_mutex_);
                audio_last_error_ = err;
            }
            ++consecutive_failures;
            const double wall_s = wall_now_s();
            if (wall_s - last_fail_log_wall_s >= kFailureLogIntervalS) {
                last_fail_log_wall_s = wall_s;
                fprintf(stderr,
                    "[audio] endpoint open failed (%d consecutive attempts): %s -- retrying in %.0fms\n",
                    consecutive_failures, err.c_str(), backoff_ms);
            }
            next_attempt_qpc = qpc_now();
            next_attempt_qpc.QuadPart +=
                static_cast<LONGLONG>(backoff_ms * static_cast<double>(freq_.QuadPart) / 1000.0);
            backoff_ms = std::min(backoff_ms * 2.0, kBackoffMaxMs);
            audio_device_active_.store(false, std::memory_order_relaxed);
            continue;
        }

        // Liveness poll: catches a device that stopped being the default, was unplugged, or went
        // inactive while still holding the same endpoint id.
        if (qpc_now().QuadPart >= next_poll_qpc.QuadPart) {
            next_poll_qpc = qpc_now();
            next_poll_qpc.QuadPart += poll_ticks;
            DWORD device_state = 0;
            if (FAILED(ep.device->GetState(&device_state)) || device_state != DEVICE_STATE_ACTIVE) {
                invalidate("render endpoint not active");
                continue;
            }
            std::string cur_id;
            if (FAILED(query_default_id(cur_id))) {
                invalidate("no default render endpoint");
                continue;
            }
            if (!cur_id.empty() && cur_id != ep.device_id) {
                audio_device_change_count_.fetch_add(1, std::memory_order_relaxed);
                invalidate("default render endpoint changed");
                continue;
            }
        }

        const DWORD wait_result = WaitForSingleObject(ep.event, kWakeWaitMs);
        if (stopping()) break;
        if (wait_result == WAIT_FAILED) {
            invalidate("WaitForSingleObject failed");
            continue;
        }
        if (wait_result != WAIT_OBJECT_0) {
            // Timeout: a dead endpoint may keep the same id and simply stop signalling, so probe
            // the client itself instead of assuming silence means health.
            UINT32 padding = 0;
            const HRESULT hr_pad = ep.client->GetCurrentPadding(&padding);
            if (FAILED(hr_pad)) {
                std::string reason = hr_text("GetCurrentPadding (liveness probe)", hr_pad);
                invalidate(reason.c_str());
            }
            continue;
        }

        std::string reason;
        if (!service_wake(reason)) {
            invalidate(reason.c_str());
            continue;
        }

        const double wall_s = wall_now_s();
        if (wall_s - last_log_wall_s >= kLogIntervalS) {
            last_log_wall_s = wall_s;
            const double master_s_now = compute_master_s();
            fprintf(stderr, "[audio] master_s=%.3f audio_s=%.3f drift_ms=%.1f queue_depth=%zu underruns=%llu peak=%.4f\n",
                master_s_now, last_written_audio_s, (last_written_audio_s - master_s_now) * 1000.0,
                decoder_.audio_queue_depth(), static_cast<unsigned long long>(underruns), last_peak);
        }
    }

    close_endpoint(ep);
    audio_device_active_.store(false, std::memory_order_relaxed);
    if (held_frame) av_frame_free(&held_frame);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    CoUninitialize();
}

void Player::request_audio_device_rebuild()
{
    audio_rebuild_request_.fetch_add(1, std::memory_order_relaxed);
}

bool Player::audio_device_active() const
{
    return audio_device_active_.load(std::memory_order_relaxed);
}

py::dict Player::audio_stats() const
{
    py::dict d;
    d["active"] = audio_device_active_.load(std::memory_order_relaxed);
    d["builds"] = audio_build_count_.load(std::memory_order_relaxed);
    d["failures"] = audio_failure_count_.load(std::memory_order_relaxed);
    d["invalidations"] = audio_invalidation_count_.load(std::memory_order_relaxed);
    d["device_changes"] = audio_device_change_count_.load(std::memory_order_relaxed);
    d["rebuild_requests"] = audio_rebuild_request_.load(std::memory_order_relaxed);
    d["write_frames"] = audio_write_frames_.load(std::memory_order_relaxed);
    d["underruns"] = audio_underrun_count_.load(std::memory_order_relaxed);
    d["last_peak"] = audio_last_peak_.load(std::memory_order_relaxed);
    d["sample_rate"] = audio_out_sample_rate_.load(std::memory_order_relaxed);
    d["channels"] = audio_out_channels_.load(std::memory_order_relaxed);
    d["buffer_frames"] = audio_buffer_frames_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(audio_info_mutex_);
        d["device_id"] = audio_device_id_;
        d["last_error"] = audio_last_error_;
    }
    d["queue_depth"] = decoder_.audio_queue_depth();
    return d;
}
