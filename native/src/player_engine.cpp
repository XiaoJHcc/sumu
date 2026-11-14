// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "player.h"
#include "cuda_util.h"

namespace {

// ---- AI input bridge shader (additive, Part B) --------------------------------------------
// Compiled inline via D3DCompile (NOT routed through native/cmake/embed_shader.cmake's
// present.hlsl pipeline -- that file is promoted/validated code and stays untouched). Ported
// verbatim from spikes/spike3_nv12_interop/src/interop.cpp's validated kShaderSrc: a
// point-sampled fullscreen triangle that does an exact 1:1 texel identity copy (no filtering,
// no colour conversion -- this is purely a format-reinterpretation blit, see the plane-SRV-
// override technique documented in docs/spike3_nv12_interop.md).
const char* kAiInputBlitShaderSrc = R"HLSL(
Texture2D    tex   : register(t0);
SamplerState samp0 : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    float2 uv[3]  = { float2(0.0, 1.0),   float2(0.0, -1.0), float2(2.0, 1.0) };
    VSOut o;
    o.pos = float4(pos[vid], 0.0, 1.0);
    o.uv = uv[vid];
    return o;
}

float PSMain_Y(VSOut i) : SV_Target
{
    return tex.Sample(samp0, i.uv).r;
}

float2 PSMain_UV(VSOut i) : SV_Target
{
    return tex.Sample(samp0, i.uv).rg;
}
)HLSL";

double pctile(std::vector<double>& sorted_ms, double q)
{
    if (sorted_ms.empty()) return 0.0;
    double pos = q * (sorted_ms.size() - 1);
    size_t lo = static_cast<size_t>(pos);
    size_t hi = std::min(lo + 1, sorted_ms.size() - 1);
    double frac = pos - lo;
    return sorted_ms[lo] * (1.0 - frac) + sorted_ms[hi] * frac;
}

py::dict summarize(std::vector<double> samples_ms)
{
    py::dict d;
    if (samples_ms.empty()) {
        d["n"] = 0; d["median_ms"] = 0.0; d["p99_ms"] = 0.0; d["max_ms"] = 0.0; d["mean_ms"] = 0.0;
        return d;
    }
    std::sort(samples_ms.begin(), samples_ms.end());
    double sum = 0.0;
    for (double v : samples_ms) sum += v;
    d["n"] = samples_ms.size();
    d["median_ms"] = pctile(samples_ms, 0.5);
    d["p99_ms"] = pctile(samples_ms, 0.99);
    d["max_ms"] = samples_ms.back();
    d["min_ms"] = samples_ms.front();
    d["mean_ms"] = sum / samples_ms.size();
    return d;
}

} // namespace

// I6: seek = reposition, never teardown. Never touches decode_thread_/present_thread_/
// decoder_/swapchain_ lifetime. Synchronous cost is dominated by Decoder::seek_to_frame's
// own av_seek_frame + forward-decode-to-target (measured in docs/native_core.md); this
// function does a bounded, small amount of extra D3D11/bookkeeping work on top of that
// and returns -- it does NOT wait for steady playback to resume (that happens
// organically on the decode/present threads, per I2/I9, never blocking).
//
// Returns the ACTUAL landed frame number on the fixed-fps grid. The decoder hands back the
// landed picture's real PTS (I5) plus its duration, i.e. the grid interval that picture covers:
// when the requested target falls inside that interval the target IS the landed frame (a VFR
// picture holds many grid frames -- pinning the clock back to the interval's first frame would
// move the playhead backwards), otherwise the interval's own start is as close as the decoder
// could get (GOP boundary / EOF / a grid gap with no picture).
int64_t Player::seek(int64_t frame_num){
    require_open();
    if (frame_count_ > 0) {
        frame_num = std::max<int64_t>(0, std::min<int64_t>(frame_num, frame_count_ - 1));
    } else {
        frame_num = std::max<int64_t>(0, frame_num);
    }

    // Widened lock: makes "seek the decoder, copy the landed frame into the ring, tag
    // it ready" one atomic unit with respect to decode_loop()'s own "decode one frame,
    // copy it, tag it" unit -- see the file header for the race this closes.
    std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);

    // Audio (spike->M-B, additive): discard whatever pre-seek audio packets decode_loop()
    // had queued, BEFORE seek_to_frame()'s own forward-decode-to-target loop (below, same
    // thread, same decoder_lock) has a chance to push fresh packets for the new position --
    // ordering matters here (flush first, then seek_to_frame), or we'd wipe out the very
    // packets we just decoded. audio_loop() (a different thread, never touches
    // decoder_mutex_) notices this seek happened via seek_version_ below and does its own
    // held_frame/fifo/codec-buffer reset independently -- see audio_loop()'s seek handling.
    decoder_.flush_audio_queue();

    // Session frame -> source frame for the decoder (which still speaks source timeline).
    int div = fps_div_.load(std::memory_order_relaxed);
    if (div < 1) div = 1;
    int64_t source_target = frame_num * div;

    DecodedFrame df;
    std::string err;
    if (!decoder_.seek_to_frame(source_target, df, err))
        throw std::runtime_error("seek_to_frame failed: " + err);
    if (decoder_.config_generation() != decoder_config_generation_) {
        reconfigure_video_session(df, frame_num);
        return current_frame();
    }

    // Same grid mapping decode_loop() uses (real PTS + duration -> [start, end] grid
    // interval), so the tagged slot answers find_pt_slot_locked() for every frame the
    // landed picture is on screen for. Without a declared duration the picture only claims
    // its own single grid frame -- gaps are never invented.
    const double duration = df.duration_seconds > 0.0 ? df.duration_seconds
        : (source_fps_ > 0.0 ? 1.0 / source_fps_ : 1.0 / (fps_ > 0.0 ? fps_ : 60.0));
    int64_t grid_start = static_cast<int64_t>(std::ceil(df.pts_seconds * fps_ - 1e-5));
    int64_t grid_end = static_cast<int64_t>(std::ceil((df.pts_seconds + duration) * fps_ - 1e-5)) - 1;
    if (grid_start < 0) grid_start = 0;
    int64_t actual_frame = (frame_num >= grid_start && frame_num <= grid_end) ? frame_num : grid_start;
    if (frame_count_ > 0) {
        grid_end = std::min(grid_end, frame_count_ - 1);
        grid_start = std::min(grid_start, grid_end);
        actual_frame = std::max<int64_t>(0, std::min(actual_frame, grid_end));
    }
    if (grid_end < grid_start) grid_end = grid_start;
    if (actual_frame > grid_end) actual_frame = grid_end;
    UINT slot = wrap_pt_slot(grid_start);

    {
        std::lock_guard<std::mutex> d3d_lock(d3d_mutex_);
        if (!decoder_.prepare_nv12_frame(df, err))
            throw std::runtime_error("video normalization conversion failed during seek: " + err);
        context_->CopySubresourceRegion(pt_ring_tex_.Get(), slot, 0, 0, 0,
            df.texture, df.array_slice, nullptr);
    }
    {
        // Clear the WHOLE ready-map (both rings): a seek invalidates essentially all of
        // a ~1s-deep buffer relative to the new position anyway (I6's "reset" step), and
        // clearing unconditionally is simplest-and-safe (present's fallback path never
        // blocks even if a slot briefly shows stale content, so being conservative here
        // costs nothing but a few repeated-frame present ticks while decode refills).
        std::lock_guard<std::mutex> lk(ready_mutex_);
        std::fill(pt_tag_.begin(), pt_tag_.end(), -1);
        std::fill(pt_end_tag_.begin(), pt_end_tag_.end(), -1);
        std::fill(ai_tag_.begin(), ai_tag_.end(), -1);
        if (static_cast<size_t>(slot) < pt_tag_.size()) {
            pt_tag_[slot] = grid_start;
            pt_end_tag_[slot] = grid_end;
        }
    }

    // High water = the last grid frame the sought picture covers, so decode's lookahead
    // throttle measures timeline coverage from the new position, not the old one.
    pt_high_water_.store(grid_end, std::memory_order_relaxed);
    present_head_frame_.store(actual_frame, std::memory_order_relaxed);

    // Reposition the present clock anchor. If playing, present continues from here at
    // full speed; if paused, the frozen frame moves to actual_frame.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    anchor_qpc_ticks_.store(now.QuadPart, std::memory_order_relaxed);
    anchor_frame_.store(actual_frame, std::memory_order_relaxed);
    clock_frame_.store(actual_frame, std::memory_order_relaxed);

    // Tell the freshly-seeked slot/source to present_loop's stale-repeat fallback too,
    // so a post-seek tick that (due to scheduling) lands past actual_frame before decode
    // catches up repeats the CORRECT just-sought frame, not whatever was on screen pre-
    // seek. Guarded by ready_mutex_ alongside the tag arrays it's read next to.
    {
        std::lock_guard<std::mutex> lk(ready_mutex_);
        seek_slot_hint_.store(static_cast<int64_t>(slot), std::memory_order_relaxed);
    }
    seek_version_.fetch_add(1, std::memory_order_relaxed);
    timeline_generation_.fetch_add(1, std::memory_order_relaxed);

    return actual_frame;
}

// dev_ptr: CUdeviceptr (torch tensor's data_ptr()) for a contiguous uint8 RGBA8 (H,W,4)
// tensor representing the AI-processed frame for content frame number `frame_num`. Never
// blocks the present thread -- only ever touches the small landing texture via CUDA,
// then a single D3D11 copy into the ready-map slot, then flips that slot's tag.
void Player::push_ai_frame(int64_t frame_num, uint64_t dev_ptr, int fwidth, int fheight,
                           size_t pitch_bytes, uint64_t generation){
    require_open();
    if (generation != 0 && generation != timeline_generation()) return;
    if (fwidth != src_width_ || fheight != src_height_)
        throw std::runtime_error("push_ai_frame: frame size does not match source video size");

    std::lock_guard<std::mutex> push_lock(push_mutex_); // defensive: single-producer assumed

    // push_ai_frame is called from whatever OS thread Python's AI-producer thread is --
    // driver-API calls issued directly here do not get CUDA runtime's lazy implicit
    // context attach, so make it explicit and unconditional.
    check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent (push_ai_frame)");

    std::lock_guard<std::mutex> d3d_lock(d3d_mutex_); // serializes ALL D3D11/CUDA-interop use of the shared context

    // M-C2 fix: reopen() flips session_active_ false (release) before it can touch
    // close_session()'s teardown of cu_res_/ai_ring_tex_/landing_tex_ (see close_session()'s
    // own d3d_lock, which this call either runs fully before or waits behind) -- once this
    // load observes false, those resources are being (or about to be) torn down, so bail as
    // a no-op rather than mapping/copying into them. acquire pairs with reopen()'s release.
    if (!session_active_.load(std::memory_order_acquire)) return;
    if (generation != 0 && generation != timeline_generation()) return;

    try {
        check_cu(cuGraphicsMapResources(1, &cu_res_, 0), "cuGraphicsMapResources");
        // H3: Map 之后任一 throw 都由 guard 析构 best-effort Unmap（此前 catch 只 rethrow，
        // 资源留 mapped 状态）。正常路径显式 Unmap 后 dismiss。
        CuGraphicsUnmapGuard unmap_guard(&cu_res_, 1);
        CUarray cu_arr = nullptr;
        check_cu(cuGraphicsSubResourceGetMappedArray(&cu_arr, cu_res_, 0, 0),
            "cuGraphicsSubResourceGetMappedArray");

        CUDA_MEMCPY2D cp{};
        cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.srcDevice = static_cast<CUdeviceptr>(dev_ptr);
        cp.srcPitch = pitch_bytes;
        cp.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        cp.dstArray = cu_arr;
        cp.WidthInBytes = static_cast<size_t>(fwidth) * 4;
        cp.Height = static_cast<size_t>(fheight);
        check_cu(cuMemcpy2D(&cp), "cuMemcpy2D (device -> mapped landing array)");

        check_cu(cuGraphicsUnmapResources(1, &cu_res_, 0), "cuGraphicsUnmapResources");
        unmap_guard.dismiss();

        UINT slot = wrap_ai_slot(frame_num);
        context_->CopySubresourceRegion(ai_ring_tex_.Get(), slot, 0, 0, 0,
            landing_tex_.Get(), 0, nullptr);

        {
            std::lock_guard<std::mutex> lk(ready_mutex_);
            ai_tag_[slot] = frame_num; // only mark ready AFTER the copy above is issued
        }
        ++ai_push_count_;
    } catch (const std::exception& e) {
        HRESULT removed = device_ ? device_->GetDeviceRemovedReason() : S_OK;
        char buf[768];
        snprintf(buf, sizeof(buf), "%s [device_removed_reason=0x%08lx]", e.what(),
            static_cast<unsigned long>(removed));
        throw std::runtime_error(buf);
    }
}

// ---- AI input bridge (additive, Part B) -----------------------------------------------
//
// Reverse direction of push_ai_frame(): decoder-produced D3D11 NV12 (already landed in
// pt_ring_tex_ by decode_loop()/seek(), unmodified) -> CUDA, for Python's AI-consumer
// thread to pull frames by number. Productionizes the technique validated in
// spikes/spike3_nv12_interop (see docs/spike3_nv12_interop.md) and follows that spike's
// own sumu_core API recommendation: keep this bridge and the colour-convert step separate
// (colour-convert -- `_nv12_to_bgr_hwc_gpu`, python/sumu/ai/utils/video_utils.py -- is
// NOT done here), and expose a frame-number-indexed, non-blocking API rather than a bare
// "next decoded frame" (I5's ready-map contract).
//
// Genuine simplification vs. spike3: spike3 blitted directly off the DECODER's own
// hw_frames_ctx texture, which FFmpeg's d3d11va hwaccel allocates at the macroblock-
// padded CODED size (not the display size) -- that spike had to lazily probe the real
// texture size via GetDesc() before sizing its plane targets, to avoid a silent vertical-
// stretch bug (see spike3's "macroblock-padded decode texture" writeup). Player's own
// pt_ring_tex_ has ALREADY been cropped to src_width_/src_height_ (display size) by
// decode_loop()'s/seek()'s own CopySubresourceRegion calls (existing, unmodified code) --
// so sourcing this bridge's identity blit from pt_ring_tex_'s existing per-slot SRVs
// (pt_srv_y_/pt_srv_uv_, already created in create_ring_resources()) instead of the raw
// decoder texture sidesteps that whole bug class, and lets the plane targets here be
// created EAGERLY at open() time (create_ai_input_bridge()) rather than lazily.
//
// Never blocks present (I1/I2/I9): a cache miss (decode hasn't reached frame_num yet, or
// it already fell out of the ~1s ring) returns {"ready": False} immediately without ever
// touching d3d_mutex_. A cache hit takes d3d_mutex_ for a brief blit + CUDA map/copy/
// unmap, joining the SAME already-accepted contention pattern push_ai_frame's CUDA path
// already has with decode_loop/seek/present (see the file header: "Decode/push threads
// are the ones who may occasionally wait a fraction of a frame for this lock, never the
// reverse" -- this method is now one more member of that "occasionally waits" set).
//
// Returns a raw dict (dev_ptr as a plain uint64 CUdeviceptr, no __cuda_array_interface__/
// DLPack machinery in C++ -- that wrapping is done Python-side, see
// python/sumu/ai/utils/video_utils.py / the dlpack helper, per the same "avoid
// __cuda_array_interface__/DLPack entirely in the native layer" preference spike 1 and
// spike 3 both followed). The destination buffer (`ai_in_cu_buf_`) is a SINGLE persistent,
// tightly-packed NV12-stacked-layout buffer reused on every call (mirrors push_ai_frame's
// single persistent landing_tex_ in the reverse direction) -- callers must finish
// consuming/copying it (e.g. via the ported `_nv12_to_bgr_hwc_gpu`) before the next call
// to this method, since that call will overwrite the same memory.
py::dict Player::get_cuda_nv12_by_frame(int64_t frame_num, uint64_t generation){
    require_open();
    UINT slot = UINT_MAX;
    auto miss = [frame_num]() {
        py::dict result;
        result["ready"] = false;
        result["frame_num"] = frame_num;
        return result;
    };
    {
        std::lock_guard<std::mutex> lk(ready_mutex_);
        if (!session_active_.load(std::memory_order_acquire)) return miss();
        if (generation != 0 && generation != timeline_generation()) return miss();
        slot = find_pt_slot_locked(frame_num);
        if (slot == UINT_MAX) return miss();
    }
    std::lock_guard<std::mutex> d3d_lock(d3d_mutex_);
    if (!session_active_.load(std::memory_order_acquire)) return miss();
    if (generation != 0 && generation != timeline_generation()) return miss();
    {
        std::lock_guard<std::mutex> lk(ready_mutex_);
        slot = find_pt_slot_locked(frame_num);
        if (slot == UINT_MAX) return miss();
    }

    try {
        // Identity blit: pt_ring_tex_'s existing per-slot SRVs -> our persistent,
        // CUDA-registered plane targets (see create_ai_input_bridge()).
        D3D11_VIEWPORT vp_y{ 0.0f, 0.0f, static_cast<float>(src_width_), static_cast<float>(src_height_), 0.0f, 1.0f };
        context_->RSSetViewports(1, &vp_y);
        ID3D11RenderTargetView* rtv_y[] = { ai_in_y_rtv_.Get() };
        context_->OMSetRenderTargets(1, rtv_y, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(ai_in_vs_.Get(), nullptr, 0);
        context_->PSSetShader(ai_in_ps_y_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv_y[] = { pt_srv_y_[slot].Get() };
        context_->PSSetShaderResources(0, 1, srv_y);
        ID3D11SamplerState* samplers[] = { ai_in_sampler_.Get() };
        context_->PSSetSamplers(0, 1, samplers);
        context_->Draw(3, 0);

        D3D11_VIEWPORT vp_uv{ 0.0f, 0.0f, static_cast<float>(src_width_ / 2), static_cast<float>(src_height_ / 2), 0.0f, 1.0f };
        context_->RSSetViewports(1, &vp_uv);
        ID3D11RenderTargetView* rtv_uv[] = { ai_in_uv_rtv_.Get() };
        context_->OMSetRenderTargets(1, rtv_uv, nullptr);
        context_->PSSetShader(ai_in_ps_uv_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv_uv[] = { pt_srv_uv_[slot].Get() };
        context_->PSSetShaderResources(0, 1, srv_uv);
        context_->Draw(3, 0);

        context_->Flush();

        // Called from whatever OS thread Python's AI-consumer thread is -- driver-API
        // calls issued directly here do not get an implicit context attach (same reason
        // push_ai_frame does this).
        check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent (get_cuda_nv12_by_frame)");

        CUgraphicsResource res[2] = { ai_in_cu_res_y_, ai_in_cu_res_uv_ };
        check_cu(cuGraphicsMapResources(2, res, 0), "cuGraphicsMapResources(ai_in)");
        // H3: 同 push_ai_frame —— 异常路径由 guard 析构 best-effort Unmap。
        CuGraphicsUnmapGuard unmap_guard(res, 2);

        CUarray cu_arr_y = nullptr, cu_arr_uv = nullptr;
        check_cu(cuGraphicsSubResourceGetMappedArray(&cu_arr_y, ai_in_cu_res_y_, 0, 0),
            "cuGraphicsSubResourceGetMappedArray(ai_in y)");
        check_cu(cuGraphicsSubResourceGetMappedArray(&cu_arr_uv, ai_in_cu_res_uv_, 0, 0),
            "cuGraphicsSubResourceGetMappedArray(ai_in uv)");

        // Stacked NV12 layout matching PyAV's to_ndarray('nv12') / lada's
        // _nv12_to_bgr_hwc_gpu contract: rows [0,h) = luma, rows [h, h*3/2) = interleaved
        // chroma, both at row-pitch ai_in_cu_buf_pitch_ (== src_width_, tightly packed).
        CUDA_MEMCPY2D cp_y{};
        cp_y.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        cp_y.srcArray = cu_arr_y;
        cp_y.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cp_y.dstDevice = ai_in_cu_buf_;
        cp_y.dstPitch = ai_in_cu_buf_pitch_;
        cp_y.WidthInBytes = static_cast<size_t>(src_width_); // R8_UNORM: 1 byte/texel
        cp_y.Height = static_cast<size_t>(src_height_);
        check_cu(cuMemcpy2D(&cp_y), "cuMemcpy2D (ai_in Y: array -> device)");

        CUDA_MEMCPY2D cp_uv{};
        cp_uv.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        cp_uv.srcArray = cu_arr_uv;
        cp_uv.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cp_uv.dstDevice = ai_in_cu_buf_ + static_cast<size_t>(src_height_) * ai_in_cu_buf_pitch_;
        cp_uv.dstPitch = ai_in_cu_buf_pitch_;
        cp_uv.WidthInBytes = static_cast<size_t>(src_width_); // R8G8_UNORM: 2 B/texel * (w/2) texels/row == w bytes
        cp_uv.Height = static_cast<size_t>(src_height_) / 2;
        check_cu(cuMemcpy2D(&cp_uv), "cuMemcpy2D (ai_in UV: array -> device)");

        check_cu(cuGraphicsUnmapResources(2, res, 0), "cuGraphicsUnmapResources(ai_in)");
        unmap_guard.dismiss();
    } catch (const std::exception& e) {
        HRESULT removed = device_ ? device_->GetDeviceRemovedReason() : S_OK;
        char buf[768];
        snprintf(buf, sizeof(buf), "%s [device_removed_reason=0x%08lx]", e.what(),
            static_cast<unsigned long>(removed));
        throw std::runtime_error(buf);
    }

    py::dict out;
    out["ready"] = true;
    out["dev_ptr"] = static_cast<uint64_t>(ai_in_cu_buf_);
    out["width"] = src_width_;
    out["height"] = src_height_;
    out["pitch_bytes"] = ai_in_cu_buf_pitch_;
    out["frame_num"] = frame_num;
    return out;
}

double Player::ai_hit_rate() const{
    uint64_t pc = present_count_.load(std::memory_order_relaxed);
    if (pc == 0) return 0.0;
    return static_cast<double>(n_ai_fresh_.load(std::memory_order_relaxed)) / static_cast<double>(pc);
}

py::dict Player::stats(){
    py::dict d;
    d["fps"] = fps_;
    d["frame_count"] = frame_count_;
    d["present_count"] = present_count_.load(std::memory_order_relaxed);
    d["decode_frame_count"] = decode_frame_count_.load(std::memory_order_relaxed);
    d["ai_push_count"] = ai_push_count_.load(std::memory_order_relaxed);
    d["n_ai_fresh"] = n_ai_fresh_.load(std::memory_order_relaxed);
    d["n_ai_stale"] = n_ai_stale_.load(std::memory_order_relaxed);
    d["n_pt_fresh"] = n_pt_fresh_.load(std::memory_order_relaxed);
    d["n_pt_stale"] = n_pt_stale_.load(std::memory_order_relaxed);
    d["n_no_session"] = n_no_session_.load(std::memory_order_relaxed); // M-C1: splash-phase present ticks
    d["ai_hit_rate"] = ai_hit_rate();
    d["is_playing"] = playing_.load(std::memory_order_relaxed);
    d["current_frame"] = current_frame();
    d["ring_capacity"] = ring_capacity_;
    d["ai_ring_capacity"] = ai_ring_capacity_;
    d["decode_ahead_max"] = decode_ahead_max_;
    d["network_recovery_attempts"] = network_recovery_attempts_.load(std::memory_order_relaxed);
    d["network_recovery_successes"] = network_recovery_successes_.load(std::memory_order_relaxed);
    d["network_recovering"] = network_recovering_.load(std::memory_order_relaxed);
    d["config_generation"] = timeline_generation();
    d["source_state"] = static_cast<int>(decoder_.source_state());
    // VFR diagnostics (no UI consumer): how many passthrough slots currently hold a picture,
    // and the longest grid interval one picture stays on screen for. 1 == plain CFR.
    int64_t ready_slots = 0, hold_max = 0;
    {
        std::lock_guard<std::mutex> lk(ready_mutex_);
        const size_t n = std::min(pt_tag_.size(), pt_end_tag_.size());
        for (size_t i = 0; i < n; ++i) {
            if (pt_tag_[i] < 0) continue;
            ++ready_slots;
            hold_max = std::max(hold_max, pt_end_tag_[i] - pt_tag_[i] + 1);
        }
    }
    d["pt_ready_slots"] = ready_slots;
    d["pt_hold_max"] = hold_max;
    return d;
}

// Present-interval (ms) median/p99/etc, computed on demand from the in-memory trace so
// far -- safe to call while playing (trace_mutex_ guards the vectors present_loop()
// appends to every tick).
py::dict Player::present_stats(){
    std::vector<double> intervals_ms;
    {
        std::lock_guard<std::mutex> lk(trace_mutex_);
        intervals_ms.reserve(present_qpc_ns_.size());
        for (size_t i = 1; i < present_qpc_ns_.size(); ++i)
            intervals_ms.push_back(static_cast<double>(present_qpc_ns_[i] - present_qpc_ns_[i - 1]) / 1e6);
    }
    return summarize(std::move(intervals_ms));
}

// trace CSV: qpc_ns,source,frame_num -- analyze_present.py (--format ns) only reads
// column 0, so this stays compatible with the shared analysis tool while also carrying
// the per-tick source tag for switch-point / seek-recovery analysis.
void Player::dump_present_trace(const std::string& path){
    std::lock_guard<std::mutex> lk(trace_mutex_);
    std::ofstream f(path, std::ios::trunc);
    f << "qpc_ns,source,frame_num\n";
    for (size_t i = 0; i < present_qpc_ns_.size(); ++i) {
        f << present_qpc_ns_[i] << "," << static_cast<int>(present_source_[i]) << ","
          << present_frame_num_[i] << "\n";
    }
}

// M4: quantize a raw content frame to a scrub bucket so a pixel-by-pixel hover drag doesn't
// re-decode every intermediate frame. ~1 second granularity (stride = round(fps)); a scrub
// preview doesn't need finer, and seek_to_frame() lands on the nearest keyframe at/before the
// target anyway. Clamped to a valid frame.
int64_t Player::scrub_bucket_for_frame(int64_t frame_num) const{
    int64_t stride = std::max<int64_t>(1, std::llround(fps_ > 0.0 ? fps_ : 1.0));
    int64_t bucket = (frame_num / stride) * stride;
    if (frame_count_ > 0)
        bucket = std::max<int64_t>(0, std::min<int64_t>(bucket, frame_count_ - 1));
    else
        bucket = std::max<int64_t>(0, bucket);
    return bucket;
}

// M4: scrub thumbnail SRV for a given content frame number. Called ONLY from
// build_bottom_bar() on the MAIN thread inside ui_tick() (which never touches d3d_mutex_, see
// ui_tick()) -- this must never block or do GPU work, so it only reads the ready cache and
// posts the hovered bucket to scrub_thread_. Anti-flicker: on an exact miss it returns the
// NEAREST already-decoded thumbnail (not nullptr) so a moving hover never blanks between
// buckets; nullptr only when the cache is still completely empty. The returned raw SRV stays
// valid until close_session() (which joins scrub_thread_ and clears the cache on this same
// main thread), long past the present thread rendering the ImGui draw snapshot it gets baked
// into.
ID3D11ShaderResourceView* Player::get_thumbnail(int64_t frame_num){
    if (!session_active_.load(std::memory_order_relaxed)) return nullptr;
    int64_t bucket = scrub_bucket_for_frame(frame_num);

    // Record where the user is looking (grid fills nearest-to-hover first) and, on the first
    // ever hover this session, arm + kick the background grid fill.
    scrub_last_hover_frame_.store(frame_num, std::memory_order_relaxed);
    if (!scrub_grid_wanted_.exchange(true, std::memory_order_relaxed))
        scrub_cv_.notify_one();

    ID3D11ShaderResourceView* exact = nullptr;
    ID3D11ShaderResourceView* nearest = nullptr;
    int64_t nearest_dist = INT64_MAX;
    auto scan = [&](const std::vector<ThumbEntry>& v) {
        for (auto& e : v) {
            if (e.bucket < 0 || !e.srv) continue;
            if (e.bucket == bucket) { exact = e.srv.Get(); return; }
            int64_t diff = e.bucket - bucket;
            int64_t d = diff < 0 ? -diff : diff;
            if (d < nearest_dist) { nearest_dist = d; nearest = e.srv.Get(); }
        }
    };
    {
        std::lock_guard<std::mutex> lk(scrub_cache_mutex_);
        scan(scrub_cache_);              // exact hits live in the on-demand ring
        if (!exact) scan(scrub_grid_);   // else the coarse grid bounds how far "nearest" can be
    }
    if (!exact) {
        // Exact miss: request this bucket (coalescing -- newest hover wins) and wake the scrub
        // thread so it refines to the exact frame. Return the NEAREST cached thumbnail in the
        // meantime -- returning nullptr blanks the image for the ~15-64ms the seek+blit takes
        // (flicker), and the grid tier keeps that nearest within scrub_grid_stride_/2 of the
        // target so a far jump shows an approximately-correct frame, not a wildly-off one.
        scrub_request_frame_.store(bucket, std::memory_order_relaxed);
        scrub_cv_.notify_one();
    }
    return exact ? exact : nearest;
}

// M4: render the NV12 blit source (already holding the just-decoded scrub frame) into a small
// RGBA8 thumbnail RTV, reusing draw_and_present()'s exact NV12->RGB pipeline (vs_ + ps_nv12_ +
// sampler_ + slice_cb_). Caller MUST hold d3d_mutex_ (shared immediate context). Sets its own
// full pipeline state; draw_and_present() re-sets everything each frame so nothing leaks.
void Player::render_thumbnail(ID3D11RenderTargetView* rtv){
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ID3D11RenderTargetView* rtvs[] = { rtv };
    context_->OMSetRenderTargets(1, rtvs, nullptr);
    // Letterbox inside the fixed 160x90 card by the source's DISPLAY shape, exactly like
    // fit_viewport() does for the main picture: an anamorphic 3:2 grid or a 4:3 source must not be
    // stretched to the card's 16:9. The clear makes the bars black instead of whatever this reused
    // texture still holds from the previous blit.
    const float thumb_w = static_cast<float>(kThumbW);
    const float thumb_h = static_cast<float>(kThumbH);
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context_->ClearRenderTargetView(rtv, black);
    float dw = thumb_w;
    float dh = thumb_h;
    if (display_aspect_ > 0.0) {
        const float aspect = static_cast<float>(display_aspect_);
        if (thumb_w / thumb_h < aspect) {
            dw = thumb_w;
            dh = thumb_w / aspect;
        } else {
            dh = thumb_h;
            dw = thumb_h * aspect;
        }
    }
    D3D11_VIEWPORT vp{ (thumb_w - dw) * 0.5f, (thumb_h - dh) * 0.5f, dw, dh, 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);

    // Single-slice scrub SRVs -> arraySlice 0 (the shader clamps an out-of-range slice anyway).
    struct SliceCB { UINT arraySlice; UINT pad[3]; };
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context_->Map(slice_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        SliceCB cb{ 0, {0, 0, 0} };
        memcpy(mapped.pData, &cb, sizeof(cb));
        context_->Unmap(slice_cb_.Get(), 0);
    }
    ID3D11Buffer* cbs[] = { slice_cb_.Get() };
    context_->PSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = { sampler_.Get() };
    context_->PSSetSamplers(0, 1, samplers);
    context_->PSSetShader(ps_nv12_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { scrub_srv_y_.Get(), scrub_srv_uv_.Get() };
    context_->PSSetShaderResources(0, 2, srvs);
    context_->Draw(3, 0);

    // Unbind: the thumb texture is about to be sampled as an SRV by ImGui (ui_render_drawdata),
    // so it must not be left bound as a render target.
    ID3D11ShaderResourceView* nullsrv[2] = { nullptr, nullptr };
    context_->PSSetShaderResources(0, 2, nullsrv);
    ID3D11RenderTargetView* nullrtv[1] = { nullptr };
    context_->OMSetRenderTargets(1, nullrtv, nullptr);
}

// M4: scrub thumbnail producer. Sleeps on scrub_cv_ until the main thread posts a new hovered
// bucket, then: (1) slow seek on the INDEPENDENT scrub_decoder_ with NO Player lock held (its
// own d3d11va pool self-serializes at the device level, exactly like decode_loop()'s decode);
// (2) a brief d3d_mutex_ critical section to blit NV12->RGBA into a reused thumbnail texture;
// (3) publish it into scrub_cache_ under scrub_cache_mutex_. d3d_mutex_ and scrub_cache_mutex_
// are never held together, so there is no lock cycle with get_thumbnail() (main thread,
// scrub_cache_mutex_ only) or the present thread (d3d_mutex_ only). Session-scoped: started by
// open_session(), joined by close_session() (which sets session_stop_ + notifies the cv).
// Blit an already-decoded scrub frame into a thumb slot's RTV. Takes d3d_mutex_ ONLY for the
// brief copy + single-triangle NV12->RGB draw (same pipeline as draw_and_present()). Returns
// false if the session resources were torn down underneath it. Never held together with
// scrub_cache_mutex_ -> no lock cycle with get_thumbnail()/present.
bool Player::blit_thumbnail(const DecodedFrame& df, ID3D11RenderTargetView* dst_rtv){
    std::lock_guard<std::mutex> d3d_lock(d3d_mutex_);
    if (!scrub_nv12_tex_ || !dst_rtv) return false;
    DecodedFrame nv12_df = df;
    std::string err;
    if (!scrub_decoder_.prepare_nv12_frame(nv12_df, err)) {
        fprintf(stderr, "[sumu] video normalization thumbnail conversion failed: %s\n", err.c_str());
        return false;
    }
    context_->CopySubresourceRegion(scrub_nv12_tex_.Get(), 0, 0, 0, 0,
        nv12_df.texture, nv12_df.array_slice, nullptr);
    render_thumbnail(dst_rtv);
    return true;
}

// Serve one hovered bucket into the on-demand RING tier (the exact preview). Slow seek holds no
// Player lock; brief d3d_mutex_ for the blit; scrub_cache_mutex_ only to pick a slot / publish.
void Player::serve_hover_bucket(int64_t bucket){
    {
        std::lock_guard<std::mutex> lk(scrub_cache_mutex_);
        for (auto& e : scrub_cache_)
            if (e.bucket == bucket) return; // re-hover of a still-cached bucket
    }
    // bucket is a session frame; scrub decoder speaks source timeline.
    int div = fps_div_.load(std::memory_order_relaxed);
    if (div < 1) div = 1;
    DecodedFrame df; std::string err;
    if (!scrub_decoder_.seek_to_frame(bucket * div, df, err)) return; // best-effort
    if (scrub_request_frame_.load(std::memory_order_relaxed) != bucket) return; // coalesced away
    size_t slot; ComPtr<ID3D11RenderTargetView> rtv;
    {
        std::lock_guard<std::mutex> lk(scrub_cache_mutex_);
        if (scrub_cache_.empty()) return; // torn down
        slot = scrub_cache_next_;
        rtv = scrub_cache_[slot].rtv;
    }
    if (!blit_thumbnail(df, rtv.Get())) return;
    {
        std::lock_guard<std::mutex> lk(scrub_cache_mutex_);
        if (scrub_cache_.empty() || slot >= scrub_cache_.size()) return;
        scrub_cache_[slot].bucket = bucket;
        scrub_cache_next_ = (scrub_cache_next_ + 1) % scrub_cache_.size();
    }
}

// Fill ONE coarse-GRID slot -- the still-unfilled slot nearest the last hover, so coverage
// grows outward from where the user is looking. scrub_grid_done_ (scrub-thread-only) advances
// on every definitive outcome so the loop knows when the grid is complete.
void Player::fill_next_grid_point(){
    int64_t hover = scrub_last_hover_frame_.load(std::memory_order_relaxed);
    size_t pick = SIZE_MAX; int64_t pick_frame = 0;
    uint32_t best_oct = UINT32_MAX; int64_t best_dist = INT64_MAX;
    ComPtr<ID3D11RenderTargetView> rtv;
    {
        std::lock_guard<std::mutex> lk(scrub_cache_mutex_);
        int64_t span = (frame_count_ > 0) ? frame_count_ - 1 : 0;
        for (size_t i = 0; i < scrub_grid_.size(); ++i) {
            if (scrub_grid_[i].bucket != -1) continue; // already filled (>=0) or failed (-2)
            // Coarse-to-fine: lowest octave first (whole-video coverage before densifying);
            // within an octave, nearest the hover first. Map slot i uniformly across the WHOLE
            // video [0, span] (endpoints included) so the tail is covered -- a plain i*stride
            // undershoots the end by up to one stride and leaves a large nearest gap there.
            uint32_t oct = (i < scrub_grid_octave_.size()) ? scrub_grid_octave_[i] : 0;
            int64_t f = (scrub_grid_slots_ > 1) ? static_cast<int64_t>(i) * span / static_cast<int64_t>(scrub_grid_slots_ - 1) : 0;
            int64_t diff = f - hover; int64_t d = diff < 0 ? -diff : diff;
            if (oct < best_oct || (oct == best_oct && d < best_dist)) {
                best_oct = oct; best_dist = d; pick = i; pick_frame = f;
            }
        }
        if (pick != SIZE_MAX) rtv = scrub_grid_[pick].rtv;
    }
    if (pick == SIZE_MAX) { scrub_grid_done_ = scrub_grid_slots_; return; } // nothing left to attempt

    // Coarse preview: land on the nearest keyframe (one decode), not a full-GOP forward-decode
    // to exact -- keeps background fill cheap enough to run during playback (see docs sweep).
    // pick_frame is session index; scrub decoder needs source frame.
    int div = fps_div_.load(std::memory_order_relaxed);
    if (div < 1) div = 1;
    DecodedFrame df; std::string err;
    if (!scrub_decoder_.seek_to_frame(pick_frame * div, df, err, /*nearest_keyframe=*/true)) {
        std::lock_guard<std::mutex> lk(scrub_cache_mutex_);
        if (pick < scrub_grid_.size()) scrub_grid_[pick].bucket = -2; // give up on this slot
        scrub_grid_done_++;
        return;
    }
    if (!blit_thumbnail(df, rtv.Get())) return; // torn down -- leave slot -1, session ending
    // Store the ACTUAL landed session frame (source PTS / div).
    int64_t landed_src = (source_fps_ > 0.0)
        ? static_cast<int64_t>(std::llround(df.pts_seconds * source_fps_)) : (pick_frame * div);
    int64_t landed = landed_src / div;
    std::lock_guard<std::mutex> lk(scrub_cache_mutex_);
    if (pick < scrub_grid_.size()) scrub_grid_[pick].bucket = landed;
    scrub_grid_done_++;
}

// M4 scrub producer. After priming the PTS origin, it loops: serve the newest hovered bucket
// (exact, into the ring) the instant one arrives, else -- once scrubbing has started -- fill
// ONE coarse-grid slot for bounded nearest coverage, else sleep. Hover always preempts grid
// fill: only one grid slot is decoded before the loop re-checks for a hover.
void Player::scrub_loop(){
    // Prime the PTS origin BEFORE serving any hover. Decoder::seek_to_frame() refuses to run
    // until at least one frame has been decoded through next_frame() -- that is the ONLY place
    // have_first_pts_/first_pts_seconds_ get set (see decoder.cpp). The scrub decoder only ever
    // seeks, never plays, so without this one throwaway decode of frame 0 EVERY seek_to_frame()
    // bails at its !have_first_pts_ guard and no thumbnail is ever produced (the "thumbnail
    // never shows" bug). This runs off the main thread on the independent scrub_decoder_ (its
    // own d3d11va pool self-serializes, exactly like decode_loop()), so it never touches
    // d3d_mutex_ nor adds to open_session() latency.
    {
        DecodedFrame prime;
        if (!scrub_decoder_.next_frame(prime)) {
            fprintf(stderr, "[sumu] scrub decoder prime decode failed -- hover thumbnails disabled\n");
            return;
        }
    }
    int64_t last_handled = -1;
    while (true) {
        if (session_stop_.load(std::memory_order_relaxed) || scrub_stop_.load(std::memory_order_relaxed) ||
                           stop_.load(std::memory_order_relaxed))
            return;

        int64_t req = scrub_request_frame_.load(std::memory_order_relaxed);
        if (req != last_handled) {          // a (possibly new) hover -- serve it, exact
            last_handled = req;
            if (req >= 0) serve_hover_bucket(req);
            continue;
        }

        // Background grid fill runs DURING PLAYBACK too. What makes this affordable is that each
        // grid point is decoded nearest-keyframe-only (one frame, not a whole-GOP forward-decode
        // -- see fill_next_grid_point / Decoder::seek_to_frame): the old full-decode fill
        // jittered present to stddev 3.17. While PLAYING we still yield grid_play_throttle_ms_
        // between fills (resolution-aware: ~2ms @1080p, ~12ms @4K -- measured, a 0ms busy-loop
        // jitters 4K present to stddev 2.23 while any small yield holds baseline ~0.1); while
        // PAUSED the foreground is idle so we fill flat out (~600-830/s at 1080p).
        if (scrub_grid_wanted_.load(std::memory_order_relaxed) &&
            scrub_grid_done_ < scrub_grid_slots_) {
            fill_next_grid_point();         // background coverage; re-check hover after each one
            if (playing_.load(std::memory_order_relaxed)) {
                std::unique_lock<std::mutex> lk(scrub_cv_mutex_);
                scrub_cv_.wait_for(lk, std::chrono::milliseconds(grid_play_throttle_ms_), [&] {
                    return session_stop_.load(std::memory_order_relaxed) ||
                           scrub_stop_.load(std::memory_order_relaxed) ||
                           stop_.load(std::memory_order_relaxed) ||
                           scrub_request_frame_.load(std::memory_order_relaxed) != last_handled;
                });
            }
            continue;
        }

        // Fully idle (grid complete or not yet armed): sleep until a hover arrives or grid work
        // becomes possible.
        std::unique_lock<std::mutex> lk(scrub_cv_mutex_);
        scrub_cv_.wait_for(lk, std::chrono::milliseconds(200), [&] {
            return session_stop_.load(std::memory_order_relaxed) ||
                   scrub_stop_.load(std::memory_order_relaxed) ||
                   stop_.load(std::memory_order_relaxed) ||
                   scrub_request_frame_.load(std::memory_order_relaxed) != last_handled ||
                   (scrub_grid_wanted_.load(std::memory_order_relaxed) &&
                    scrub_grid_done_ < scrub_grid_slots_);
        });
    }
}

void Player::create_ring_resources(){
    D3D11_TEXTURE2D_DESC pt_desc{};
    pt_desc.Width = src_width_;
    pt_desc.Height = src_height_;
    pt_desc.MipLevels = 1;
    pt_desc.ArraySize = ring_capacity_;
    pt_desc.Format = DXGI_FORMAT_NV12;
    pt_desc.SampleDesc.Count = 1;
    pt_desc.Usage = D3D11_USAGE_DEFAULT;
    // NV12 is a video format; on this driver CreateTexture2D rejects it (E_INVALIDARG)
    // with BIND_SHADER_RESOURCE alone -- matching the decoder's own hw_frames_ctx bind
    // flags (BIND_DECODER | BIND_SHADER_RESOURCE) makes it succeed. We never actually
    // use this as a decode target, just as a same-format GPU-side copy destination + SRV
    // source.
    pt_desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    check_hr(device_->CreateTexture2D(&pt_desc, nullptr, &pt_ring_tex_), "CreateTexture2D(pt_ring_tex_)");

    pt_srv_y_.resize(ring_capacity_);
    pt_srv_uv_.resize(ring_capacity_);
    for (UINT i = 0; i < ring_capacity_; ++i) {
        D3D11_SHADER_RESOURCE_VIEW_DESC yd{};
        yd.Format = DXGI_FORMAT_R8_UNORM;
        yd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        yd.Texture2DArray.MostDetailedMip = 0;
        yd.Texture2DArray.MipLevels = 1;
        yd.Texture2DArray.FirstArraySlice = i;
        yd.Texture2DArray.ArraySize = 1;
        check_hr(device_->CreateShaderResourceView(pt_ring_tex_.Get(), &yd, &pt_srv_y_[i]), "CreateSRV(pt Y)");

        D3D11_SHADER_RESOURCE_VIEW_DESC uvd = yd;
        uvd.Format = DXGI_FORMAT_R8G8_UNORM;
        check_hr(device_->CreateShaderResourceView(pt_ring_tex_.Get(), &uvd, &pt_srv_uv_[i]), "CreateSRV(pt UV)");
    }

    D3D11_TEXTURE2D_DESC ai_desc{};
    ai_desc.Width = src_width_;
    ai_desc.Height = src_height_;
    ai_desc.MipLevels = 1;
    ai_desc.ArraySize = ai_ring_capacity_;
    ai_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ai_desc.SampleDesc.Count = 1;
    ai_desc.Usage = D3D11_USAGE_DEFAULT;
    ai_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    check_hr(device_->CreateTexture2D(&ai_desc, nullptr, &ai_ring_tex_), "CreateTexture2D(ai_ring_tex_)");

    ai_srv_.resize(ai_ring_capacity_);
    for (UINT i = 0; i < ai_ring_capacity_; ++i) {
        D3D11_SHADER_RESOURCE_VIEW_DESC ad{};
        ad.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ad.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        ad.Texture2DArray.MostDetailedMip = 0;
        ad.Texture2DArray.MipLevels = 1;
        ad.Texture2DArray.FirstArraySlice = i;
        ad.Texture2DArray.ArraySize = 1;
        check_hr(device_->CreateShaderResourceView(ai_ring_tex_.Get(), &ad, &ai_srv_[i]), "CreateSRV(ai)");
    }

    create_scrub_resources();
}

// M4: single-slice NV12 blit source + reused RGBA8 thumbnail pool for the seekbar hover
// preview. Same NV12/SRV recipe as pt_ring_tex_ above (BIND_DECODER needed alongside
// BIND_SHADER_RESOURCE for the driver to accept an NV12 texture), just ArraySize==1. Built
// here (session scope, sized to this video) and torn down in close_session().
void Player::create_scrub_resources(){
    D3D11_TEXTURE2D_DESC nv{};
    nv.Width = src_width_;
    nv.Height = src_height_;
    nv.MipLevels = 1;
    nv.ArraySize = 1;
    nv.Format = DXGI_FORMAT_NV12;
    nv.SampleDesc.Count = 1;
    nv.Usage = D3D11_USAGE_DEFAULT;
    nv.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    check_hr(device_->CreateTexture2D(&nv, nullptr, &scrub_nv12_tex_), "CreateTexture2D(scrub_nv12_tex_)");

    D3D11_SHADER_RESOURCE_VIEW_DESC yd{};
    yd.Format = DXGI_FORMAT_R8_UNORM;
    yd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    yd.Texture2DArray.MostDetailedMip = 0;
    yd.Texture2DArray.MipLevels = 1;
    yd.Texture2DArray.FirstArraySlice = 0;
    yd.Texture2DArray.ArraySize = 1;
    check_hr(device_->CreateShaderResourceView(scrub_nv12_tex_.Get(), &yd, &scrub_srv_y_), "CreateSRV(scrub Y)");
    D3D11_SHADER_RESOURCE_VIEW_DESC uvd = yd;
    uvd.Format = DXGI_FORMAT_R8G8_UNORM;
    check_hr(device_->CreateShaderResourceView(scrub_nv12_tex_.Get(), &uvd, &scrub_srv_uv_), "CreateSRV(scrub UV)");

    auto alloc_thumb = [&](ThumbEntry& e) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kThumbW;
        td.Height = kThumbH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        check_hr(device_->CreateTexture2D(&td, nullptr, &e.tex), "CreateTexture2D(thumb)");
        check_hr(device_->CreateRenderTargetView(e.tex.Get(), nullptr, &e.rtv), "CreateRTV(thumb)");
        check_hr(device_->CreateShaderResourceView(e.tex.Get(), nullptr, &e.srv), "CreateSRV(thumb)");
        e.bucket = -1;
    };

    scrub_cache_.clear();
    scrub_cache_.resize(kScrubCacheCap);
    scrub_cache_next_ = 0;
    for (auto& e : scrub_cache_) alloc_thumb(e);

    // Coarse grid tier: uniform coverage of the whole video, slot count scaled to duration
    // (~one thumb per kGridTargetSpacingSec) but clamped to [kGridSlotsMin, kGridSlotsMax] --
    // a short clip gets the floor (denser), anything past ~68min flattens at the 1024 ceil (the
    // ring tier refines exact on hover, so a longer film's coarser grid spacing is acceptable).
    double dur_s = (fps_ > 0.0 && frame_count_ > 0) ? frame_count_ / fps_ : 0.0;
    size_t want = static_cast<size_t>(dur_s / kGridTargetSpacingSec);
    scrub_grid_slots_ = std::min(kGridSlotsMax, std::max(kGridSlotsMin, want));
    scrub_grid_.clear();
    scrub_grid_.resize(scrub_grid_slots_);
    for (auto& e : scrub_grid_) alloc_thumb(e);

    // Progressive (coarse-to-fine) fill order: rank[i] = bit-reversal of the slot index over
    // enough bits to cover all slots. Filling by ascending rank visits 0, mid, quarters,
    // eighths... so the whole timeline is coarsely covered first (level 64) and each subsequent
    // batch bisects the gaps (128/256/512). octave[i] = highest set bit of rank[i]: the scan
    // ties within an octave break toward the hover, so the scrubbed region densifies first
    // without starving the global coarse pass.
    scrub_grid_rank_.assign(scrub_grid_slots_, 0);
    scrub_grid_octave_.assign(scrub_grid_slots_, 0);
    int bits = 0;
    while ((static_cast<size_t>(1) << bits) < scrub_grid_slots_) ++bits;
    for (size_t i = 0; i < scrub_grid_slots_; ++i) {
        uint32_t v = static_cast<uint32_t>(i), r = 0;
        for (int b = 0; b < bits; ++b) { r = (r << 1) | (v & 1u); v >>= 1; }
        scrub_grid_rank_[i] = r;
        uint8_t oct = 0;
        for (uint32_t t = r; t > 1; t >>= 1) ++oct; // floor(log2(r)); 0 for r<=1
        scrub_grid_octave_[i] = oct;
    }

    // Resolution-aware inter-fill yield while playing (paused fills flat out): 4K needs a small
    // yield or a back-to-back scrub decode jitters present; 1080p tolerates near-full speed.
    grid_play_throttle_ms_ = (static_cast<int64_t>(src_width_) * src_height_ > kHiResPixels)
        ? kGridPlayThrottleHiRes : kGridPlayThrottleLoRes;

    // Reported/nominal spacing between grid points (points span [0, frame_count_-1]).
    scrub_grid_stride_ = std::max<int64_t>(1, (frame_count_ > 1 ? frame_count_ - 1 : 1) /
        static_cast<int64_t>(scrub_grid_slots_ > 1 ? scrub_grid_slots_ - 1 : 1));
    scrub_grid_done_ = 0;
    scrub_grid_wanted_.store(false, std::memory_order_relaxed);
}

void Player::create_landing_texture(){
    D3D11_TEXTURE2D_DESC td{};
    td.Width = src_width_;
    td.Height = src_height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    check_hr(device_->CreateTexture2D(&td, nullptr, &landing_tex_), "CreateTexture2D(landing_tex_)");
}

void Player::init_cuda_driver(){
    check_cu(cuInit(0), "cuInit");
    check_cu(cuDeviceGet(&cu_dev_, 0), "cuDeviceGet");

    CUdevice dev_from_adapter = -1;
    CUresult r = cuD3D11GetDevice(&dev_from_adapter, adapter_.Get());
    if (r != CUDA_SUCCESS || dev_from_adapter != cu_dev_) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "cuD3D11GetDevice mismatch: D3D adapter maps to CUDA device %d, expected %d (CUresult=%d)",
            (int)dev_from_adapter, (int)cu_dev_, (int)r);
        throw std::runtime_error(buf);
    }

    check_cu(cuDevicePrimaryCtxRetain(&cu_ctx_, cu_dev_), "cuDevicePrimaryCtxRetain");
    check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent");
}

void Player::register_landing_with_cuda(){
    check_cu(cuGraphicsD3D11RegisterResource(&cu_res_, landing_tex_.Get(), CU_GRAPHICS_REGISTER_FLAGS_NONE),
        "cuGraphicsD3D11RegisterResource");
}

// ---- AI input bridge setup (additive, Part B; see get_cuda_nv12_by_frame's header
// comment for the full design rationale) --------------------------------------------
void Player::create_ai_input_bridge(){
    // Own small shader program for the identity blit, compiled inline (NOT routed
    // through native/cmake/embed_shader.cmake's present.hlsl pipeline -- that stays
    // exactly as promoted/validated; this is a separate, additive program).
    ComPtr<ID3DBlob> vs_blob, ps_y_blob, ps_uv_blob, err_blob;
    HRESULT hr = D3DCompile(kAiInputBlitShaderSrc, strlen(kAiInputBlitShaderSrc), "ai_input_blit.hlsl",
        nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs_blob, &err_blob);
    if (FAILED(hr)) throw std::runtime_error("AI-input VS compile failed: " +
        std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));
    hr = D3DCompile(kAiInputBlitShaderSrc, strlen(kAiInputBlitShaderSrc), "ai_input_blit.hlsl",
        nullptr, nullptr, "PSMain_Y", "ps_5_0", 0, 0, &ps_y_blob, &err_blob);
    if (FAILED(hr)) throw std::runtime_error("AI-input PS(Y) compile failed: " +
        std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));
    hr = D3DCompile(kAiInputBlitShaderSrc, strlen(kAiInputBlitShaderSrc), "ai_input_blit.hlsl",
        nullptr, nullptr, "PSMain_UV", "ps_5_0", 0, 0, &ps_uv_blob, &err_blob);
    if (FAILED(hr)) throw std::runtime_error("AI-input PS(UV) compile failed: " +
        std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));

    check_hr(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &ai_in_vs_),
        "CreateVertexShader(ai_in)");
    check_hr(device_->CreatePixelShader(ps_y_blob->GetBufferPointer(), ps_y_blob->GetBufferSize(), nullptr, &ai_in_ps_y_),
        "CreatePixelShader(ai_in Y)");
    check_hr(device_->CreatePixelShader(ps_uv_blob->GetBufferPointer(), ps_uv_blob->GetBufferSize(), nullptr, &ai_in_ps_uv_),
        "CreatePixelShader(ai_in UV)");

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // exact 1:1 texel identity copy, no filtering
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    check_hr(device_->CreateSamplerState(&sd, &ai_in_sampler_), "CreateSamplerState(ai_in)");

    // Sized to src_width_/src_height_ -- the DISPLAY size, matching pt_ring_tex_ itself.
    // Unlike spike3 (which blitted straight off the decoder's own macroblock-padded
    // hw_frames_ctx texture and needed a lazy real-size probe, see
    // docs/spike3_nv12_interop.md), pt_ring_tex_ has ALREADY been cropped to display size
    // by decode_loop()'s/seek()'s own CopySubresourceRegion (existing, unmodified code)
    // -- so these targets can be created eagerly, right here, with no padding concern.
    D3D11_TEXTURE2D_DESC yd{};
    yd.Width = static_cast<UINT>(src_width_);
    yd.Height = static_cast<UINT>(src_height_);
    yd.MipLevels = 1;
    yd.ArraySize = 1;
    yd.Format = DXGI_FORMAT_R8_UNORM;
    yd.SampleDesc.Count = 1;
    yd.Usage = D3D11_USAGE_DEFAULT;
    yd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    check_hr(device_->CreateTexture2D(&yd, nullptr, &ai_in_y_tex_), "CreateTexture2D(ai_in_y_tex_)");
    check_hr(device_->CreateRenderTargetView(ai_in_y_tex_.Get(), nullptr, &ai_in_y_rtv_), "CreateRTV(ai_in_y)");

    D3D11_TEXTURE2D_DESC uvd = yd;
    uvd.Width = static_cast<UINT>(src_width_) / 2;
    uvd.Height = static_cast<UINT>(src_height_) / 2;
    uvd.Format = DXGI_FORMAT_R8G8_UNORM;
    check_hr(device_->CreateTexture2D(&uvd, nullptr, &ai_in_uv_tex_), "CreateTexture2D(ai_in_uv_tex_)");
    check_hr(device_->CreateRenderTargetView(ai_in_uv_tex_.Get(), nullptr, &ai_in_uv_rtv_), "CreateRTV(ai_in_uv)");

    check_cu(cuGraphicsD3D11RegisterResource(&ai_in_cu_res_y_, ai_in_y_tex_.Get(), CU_GRAPHICS_REGISTER_FLAGS_NONE),
        "cuGraphicsD3D11RegisterResource(ai_in_y_tex_)");
    check_cu(cuGraphicsD3D11RegisterResource(&ai_in_cu_res_uv_, ai_in_uv_tex_.Get(), CU_GRAPHICS_REGISTER_FLAGS_NONE),
        "cuGraphicsD3D11RegisterResource(ai_in_uv_tex_)");

    // Persistent, tightly-packed CUDA destination buffer (stacked NV12 layout, see
    // get_cuda_nv12_by_frame()). Tightly packed (pitch == src_width_) so the Python-side
    // DLPack wrap can describe it as a contiguous tensor (NULL strides).
    ai_in_cu_buf_pitch_ = static_cast<size_t>(src_width_);
    ai_in_cu_buf_size_ = ai_in_cu_buf_pitch_ * static_cast<size_t>(src_height_) * 3 / 2;
    check_cu(cuMemAlloc(&ai_in_cu_buf_, ai_in_cu_buf_size_), "cuMemAlloc(ai_in_cu_buf_)");
}

// Session frame number -> the passthrough ring slot showing that picture. A decoded picture
// owns the fixed-fps grid interval [pt_tag_[i], pt_end_tag_[i]] derived from its real PTS and
// duration, so one slot answers for every frame of a VFR hold; CFR keeps the cheap exact-slot
// path. Overlapping claims (a duration running past the next picture's start) resolve to the
// LATEST start -- the picture the timeline says is on screen at frame_num. Caller must hold
// ready_mutex_. UINT_MAX == miss (frame not decoded yet, fell out of the ring, or ring torn
// down); callers treat that as "nothing to show", never as an error.
UINT Player::find_pt_slot_locked(int64_t frame_num) const{
    const size_t n = std::min(pt_tag_.size(), pt_end_tag_.size());
    if (n == 0 || frame_num < 0) return UINT_MAX;
    const UINT exact = wrap_pt_slot(frame_num);
    if (static_cast<size_t>(exact) < n && pt_tag_[exact] == frame_num && pt_end_tag_[exact] >= frame_num)
        return exact;
    UINT best = UINT_MAX;
    int64_t best_start = -1;
    for (size_t i = 0; i < n; ++i) {
        const int64_t start = pt_tag_[i];
        if (start < 0 || start > frame_num || pt_end_tag_[i] < frame_num) continue;
        if (start > best_start) { best_start = start; best = static_cast<UINT>(i); }
    }
    return best;
}

void Player::decode_loop(){
    auto next_recovery = std::chrono::steady_clock::now();
    unsigned recovery_failures = 0;
    uint64_t recovery_seek_version = seek_version_.load(std::memory_order_relaxed);
    while (!(stop_.load(std::memory_order_relaxed) || session_stop_.load(std::memory_order_relaxed))) {
        for (;;) {
            if (stop_.load(std::memory_order_relaxed) || session_stop_.load(std::memory_order_relaxed)) return;
            const int64_t head = present_head_frame_.load(std::memory_order_relaxed);
            const int64_t high = pt_high_water_.load(std::memory_order_relaxed);
            if (high - head < decode_ahead_max_) break;
            Sleep(1);
        }
        {
            std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
            if (stop_.load(std::memory_order_relaxed) || session_stop_.load(std::memory_order_relaxed)) return;
            const uint64_t version = seek_version_.load(std::memory_order_relaxed);
            if (version != recovery_seek_version) {
                recovery_seek_version = version;
                next_recovery = std::chrono::steady_clock::now();
                recovery_failures = 0;
                network_recovering_.store(false, std::memory_order_relaxed);
            }

            DecodedFrame df;
            const DecodeResult decoded = decoder_.next_frame(df);
            if (decoded.status == DecodeStatus::WouldBlock)
                goto decode_idle;
            if (decoded.status == DecodeStatus::NeedReconfigure) {
                try {
                    reconfigure_video_session(df);
                } catch (const std::exception& e) {
                    fprintf(stderr, "[sumu] video reconfiguration failed: %s\n", e.what());
                    playing_.store(false, std::memory_order_relaxed);
                    return;
                }
                continue;
            }
            if (decoded.status != DecodeStatus::Frame) {
                const int64_t high = pt_high_water_.load(std::memory_order_relaxed);
                const int64_t head = present_head_frame_.load(std::memory_order_relaxed);
                const auto now = std::chrono::steady_clock::now();
                const bool playing = playing_.load(std::memory_order_relaxed);
                if (!playing)
                    network_recovering_.store(false, std::memory_order_relaxed); // nothing to recover while paused
                const bool live = decoder_.is_live();
                if (!playing || !decoder_.have_first_pts() ||
                    (!live && (frame_count_ <= 0 || high >= frame_count_ - 1 ||
                               head >= frame_count_ - 1)) || now < next_recovery)
                    goto decode_idle;
                const int div = std::max(1, fps_div_.load(std::memory_order_relaxed));
                std::string err;
                if (network_source_) {
                    network_recovering_.store(true, std::memory_order_relaxed);
                    network_recovery_attempts_.fetch_add(1, std::memory_order_relaxed);
                }
                const int64_t target = live ? 0 : std::min(head + 2, frame_count_ - 1) * div;
                const bool recovered = live
                    ? decoder_.reconnect_live(err)
                    : network_source_ ? decoder_.reconnect_to_frame(target, df, err)
                                      : decoder_.seek_to_frame(target, df, err);
                if (!recovered) {
                    recovery_failures = std::min(recovery_failures + 1, 4u);
                    const unsigned delay_ms = network_source_ ? std::min(2000u, 500u << recovery_failures) : 250u;
                    next_recovery = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
                    if (network_source_)
                        fprintf(stderr, "[sumu] network recovery failed (%s); retry in %ums\n", err.c_str(), delay_ms);
                    goto decode_idle;
                }
                next_recovery = std::chrono::steady_clock::now() + std::chrono::milliseconds(network_source_ ? 1000 : 250);
                if (network_source_) {
                    network_recovery_successes_.fetch_add(1, std::memory_order_relaxed);
                    network_recovering_.store(false, std::memory_order_relaxed);
                    if (live)
                        fprintf(stderr, "[sumu] live network source reopened\n");
                    else
                        fprintf(stderr, "[sumu] network playback recovered at %.3fs\n", df.pts_seconds);
                }
                recovery_failures = 0;
                if (live) continue;
            }

            // Real-PTS grid mapping: a picture occupies [start, end] of the fixed-fps timeline,
            // start from its PTS and end from its own declared duration (nothing is invented for
            // a gap). An interval that contains no grid sample point (fps_div dropped it) is
            // dropped whole; only a picture with no usable duration falls back to its own single
            // grid frame. Fractional frames at the boundary are collapsed by the ceil(...-eps)
            // on both edges, so fps_div keeps exactly the div-aligned pictures.
            const int64_t frame_num = static_cast<int64_t>(std::ceil(df.pts_seconds * fps_ - 1e-5));
            int64_t frame_end = frame_num;
            if (df.duration_seconds > 0.0) {
                frame_end = static_cast<int64_t>(std::ceil((df.pts_seconds + df.duration_seconds) * fps_ - 1e-5)) - 1;
                if (frame_end < frame_num) continue;
            }
            if (decoder_.timeline_kind() == TimelineKind::Segmented && source_fps_ > 0.0) {
                const int64_t source_end = static_cast<int64_t>(std::ceil(
                    (df.pts_seconds + df.duration_seconds) * source_fps_ - 1e-5)) - 1;
                if (source_end >= source_frame_count_) {
                    source_frame_count_ = source_end + 1;
                    recompute_session_rate();
                    if (!network_source_) invalidate_scrub_timeline();
                }
            }
            if (frame_count_ > 0) frame_end = std::min(frame_end, frame_count_ - 1);
            if (frame_num < 0 || frame_num > frame_end) continue;
            UINT slot = wrap_pt_slot(frame_num);
            {
                std::lock_guard<std::mutex> lk(ready_mutex_);
                const int64_t head = present_head_frame_.load(std::memory_order_relaxed);
                if (pt_tag_[slot] >= 0 && pt_end_tag_[slot] >= head && pt_tag_[slot] != frame_num) {
                    for (UINT i = 0; i < pt_tag_.size(); ++i) {
                        if (pt_tag_[i] < 0 || pt_end_tag_[i] < head) {
                            slot = i;
                            break;
                        }
                    }
                }
            }
            {
                std::lock_guard<std::mutex> d3d_lock(d3d_mutex_);
                std::string err;
                if (!decoder_.prepare_nv12_frame(df, err)) {
                    fprintf(stderr, "[sumu] video normalization failed: %s\n", err.c_str());
                    goto decode_idle;
                }
                context_->CopySubresourceRegion(pt_ring_tex_.Get(), slot, 0, 0, 0,
                    df.texture, df.array_slice, nullptr);
                std::lock_guard<std::mutex> lk(ready_mutex_);
                pt_tag_[slot] = frame_num;
                pt_end_tag_[slot] = frame_end;
            }
            pt_high_water_.store(std::max(pt_high_water_.load(std::memory_order_relaxed), frame_end), std::memory_order_relaxed);
            ++decode_frame_count_;
            continue;
        }
    decode_idle:
        if (stop_.load(std::memory_order_relaxed) || session_stop_.load(std::memory_order_relaxed)) return;
        Sleep(5);
    }
}

void Player::present_loop(){
    const double ticks_per_ms = static_cast<double>(freq_.QuadPart) / 1000.0;
    int64_t last_frame_local = -1;
    Source last_actual_source = Source::PassthroughFresh;
    UINT last_actual_slot = UINT_MAX;
    // Owner tag of the frame actually on screen: the passthrough interval START that picture
    // answered for (VFR: several grid frames share one slot, so the owner is what identifies
    // the content -- comparing against the timeline frame would call every held frame stale),
    // or the pushed frame number for an AI frame (that ring is frame-exact).
    int64_t last_actual_owner = -1;
    uint64_t local_seek_version = seek_version_.load(std::memory_order_relaxed);
    int64_t tick_idx = 0;
    uint64_t local_pace_epoch = pace_epoch_.load(std::memory_order_acquire);

    {
        std::lock_guard<std::mutex> lk(trace_mutex_);
        present_qpc_ns_.reserve(1u << 16);
        present_source_.reserve(1u << 16);
        present_frame_num_.reserve(1u << 16);
    }

    while (!stop_.load(std::memory_order_relaxed)) {
        // Pacing engine: fixed cadence at fps_, independent of play/pause/seek state so
        // this thread never busy-spins and never needs tearing down/rebuilding across
        // transport changes.
        //
        // M-C2 fix: fps_ is per-session and CHANGES on reopen() (e.g. 4K60 -> 1080p30) and
        // on the first open() (default 60.0 -> the file's real fps). The formula below is
        // pace_origin_qpc_ + tick_idx/fps_, and tick_idx accumulates over the WHOLE present-
        // thread lifetime -- so a bare fps_ change with origin/tick_idx untouched makes the
        // next target_qpc jump seconds into the future (fps_ dropped) or the past (fps_
        // rose), freezing or bursting present until wall-clock realigns (observed: multi-
        // second present freeze on every reopen that lowers fps, even with no AI running).
        // open_session() bumps pace_epoch_ (release) right after assigning the new fps_;
        // this thread -- the SOLE writer of pace_origin_qpc_/tick_idx -- re-anchors here on
        // observing the bump: origin := now, tick_idx := 0, so the cadence re-baselines
        // cleanly at the new fps_. Fires exactly once per open_session() (play/pause/seek
        // never touch pace_epoch_), so steady-state cadence is unchanged.
        uint64_t pe = pace_epoch_.load(std::memory_order_acquire);
        if (pe != local_pace_epoch) {
            local_pace_epoch = pe;
            QueryPerformanceCounter(&pace_origin_qpc_);
            tick_idx = 0;
            // New open_session() (incl. reopen): drop the previous file's present head so
            // the monotonic guard below cannot force the new timeline to last_frame+1 of
            // the old video (seekbar past end / green-slot / "fast-forward" on drop-open).
            last_frame_local = -1;
            last_actual_slot = UINT_MAX;
            last_actual_owner = -1;
            last_actual_source = Source::PassthroughFresh;
            local_seek_version = seek_version_.load(std::memory_order_relaxed);
        }
        ++tick_idx;
        LARGE_INTEGER target_qpc;
        target_qpc.QuadPart = pace_origin_qpc_.QuadPart +
            static_cast<LONGLONG>(static_cast<double>(tick_idx) / fps_ * freq_.QuadPart);
        if (!wait_until_qpc_or_stop(target_qpc, ticks_per_ms)) break;

        uint64_t sv = seek_version_.load(std::memory_order_relaxed);
        bool just_seeked = (sv != local_seek_version);
        local_seek_version = sv;

        if (just_seeked) {
            // Bypass the monotonic guard below for exactly this tick (a seek can
            // legitimately move backward or far forward), and repoint the stale-repeat
            // fallback at the freshly-seeked slot so an immediately-following tick that
            // races ahead of decode never repeats pre-seek content. The owner tag is read
            // from that slot (not from anchor_frame_) because seek() tags the slot with the
            // landed picture's grid interval start, which can precede the target it covers.
            std::lock_guard<std::mutex> lk(ready_mutex_);
            const UINT hint = static_cast<UINT>(seek_slot_hint_.load(std::memory_order_relaxed));
            last_actual_slot = (hint < pt_tag_.size()) ? hint : UINT_MAX;
            last_actual_owner = (hint < pt_tag_.size()) ? pt_tag_[hint] : -1;
            last_actual_source = Source::PassthroughFresh;
        }

        int64_t frame;
        Source source;
        UINT use_slot = UINT_MAX;
        int64_t use_owner = -1;

        // M-C1: no session open yet (or one was just closed) -- skip frame-picking and
        // draw_and_present() entirely (every ring/tag/SRV they touch may not exist, or may
        // be mid-close_session()), draw a splash instead. frame/source/use_slot are hoisted
        // out of the two branches below so the trailing trace/counting bookkeeping (kept
        // textually unchanged) is shared by both; frame is recorded as -1, a clear "no real
        // frame" marker in the trace. present_head_frame_/clock_frame_/last_frame_local are
        // NOT touched here -- decode_thread_ isn't running while !session_active_ anyway
        // (see close_session()/open_session()), so there is nothing for them to throttle.
        if (!session_active_.load(std::memory_order_relaxed)) {
            frame = -1;
            source = Source::NoSession;
            // M-C2: committed to the splash branch for this whole iteration (session_active_
            // was just observed false) -- signal reopen()'s spin-wait that draw_and_present()
            // cannot be in flight anymore (see reopen()'s header comment for the full
            // argument). release pairs with reopen()'s acquire load.
            present_in_splash_.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> d3d_lock(d3d_mutex_);
            draw_splash();
        } else {
            // M-C2: committed to the normal draw_and_present() branch for this whole
            // iteration (session_active_ was just observed true) -- clear the splash flag
            // before doing any real work below, per the task brief ("在读 session_active_ 之
            // 后、绘制之前"). relaxed: nothing spin-waits for this to become false.
            present_in_splash_.store(false, std::memory_order_relaxed);
            if (playing_.load(std::memory_order_relaxed)) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                int64_t aq = anchor_qpc_ticks_.load(std::memory_order_relaxed);
                int64_t af = anchor_frame_.load(std::memory_order_relaxed);
                double elapsed_s = static_cast<double>(now.QuadPart - aq) / freq_.QuadPart;
                int64_t computed = af + static_cast<int64_t>(std::llround(elapsed_s * fps_));
                if (!just_seeked && computed <= last_frame_local) computed = last_frame_local + 1;
                frame = computed;
                int64_t end_frame = frame_count_ > 0 ? frame_count_ - 1 : INT64_MAX;
                if (frame_count_ <= 0 && decoder_.at_eof() && !decoder_.is_live()) {
                    const int64_t decoded_end = pt_high_water_.load(std::memory_order_relaxed);
                    if (decoded_end >= 0) end_frame = decoded_end;
                }
                if (frame >= end_frame) {
                    frame = end_frame;
                    anchor_frame_.store(frame, std::memory_order_relaxed);
                    playing_.store(false, std::memory_order_relaxed);
                }
            } else {
                // Paused: frozen at anchor_frame_, present thread keeps redrawing it so the
                // heartbeat (present cadence) never stops even though content doesn't advance.
                frame = anchor_frame_.load(std::memory_order_relaxed);
                if (frame_count_ > 0)
                    frame = std::min(frame, frame_count_ - 1);
            }
            last_frame_local = frame;
            clock_frame_.store(frame, std::memory_order_relaxed);
            present_head_frame_.store(frame, std::memory_order_relaxed);

            // Source pick + draw in ONE d3d_mutex_ section. Tag writers (decode_loop(),
            // push_ai_frame()) publish slot contents and the slot's tag/interval together
            // inside their own d3d_mutex_ section, so while we hold it a slot we just
            // validated cannot be rewritten under us before the draw -- that is the
            // re-validation the pick needs, not a second guess after the fact. present still
            // never waits on the DECODER (decode_mutex_ is not involved), only on the same
            // brief immediate-context sections it always waited for.
            {
                // Serializes ALL D3D11/CUDA-interop use of the shared context (see file
                // header). Decode/push threads are the ones who may occasionally wait a
                // fraction of a frame for this lock, never the reverse.
                std::lock_guard<std::mutex> d3d_lock(d3d_mutex_);
                {
                    std::lock_guard<std::mutex> lk(ready_mutex_);
                    // Phase 6 M-D: gates the AI pick on the de-mosaic-on/off toggle, read
                    // once here under the same lock as the tag arrays it's compared against.
                    // When ai_enabled_==true (the default) `ai_on && x` is identical to the
                    // bare `x` this replaced, so steady-state behavior when de-mosaic stays
                    // on is unchanged.
                    const bool ai_on = ai_enabled_.load(std::memory_order_relaxed);
                    const UINT ai_slot = wrap_ai_slot(frame);

                    if (ai_on && frame >= 0 && ai_slot < ai_tag_.size() && ai_tag_[ai_slot] == frame) {
                        source = Source::AiFresh;
                        use_slot = ai_slot;
                        use_owner = frame;
                    } else if ((use_slot = find_pt_slot_locked(frame)) != UINT_MAX) {
                        // A picture covering this grid frame is on the GPU: fresh, whether or
                        // not the picture's own PTS landed exactly on it (VFR hold).
                        source = Source::PassthroughFresh;
                        use_owner = pt_tag_[use_slot];
                    } else {
                        // Nothing covers this frame (decode has not reached it, it fell out of
                        // the ring, or the timeline has a gap with no picture). NEVER block --
                        // re-present whatever was actually shown last tick (I2/I9), but only if
                        // that slot still holds the SAME picture: a ring lap or a seek may have
                        // overwritten it, and stale content must never be mistaken for the last
                        // valid frame. When de-mosaic is off, never repeat an AI frame either.
                        source = (ai_on && is_ai(last_actual_source)) ? Source::AiStale
                                                                      : Source::PassthroughStale;
                        const auto& tags = is_ai(source) ? ai_tag_ : pt_tag_;
                        use_slot = (last_actual_owner >= 0 && last_actual_slot < tags.size() &&
                                    tags[last_actual_slot] == last_actual_owner)
                            ? last_actual_slot : UINT_MAX;
                    }
                }
                draw_and_present(source, use_slot);
            }
        }

        LARGE_INTEGER pres_now;
        QueryPerformanceCounter(&pres_now);
        {
            std::lock_guard<std::mutex> lk(trace_mutex_);
            present_qpc_ns_.push_back(qpc_to_ns(pres_now));
            present_source_.push_back(static_cast<int8_t>(source));
            present_frame_num_.push_back(frame);
            // L2 修复：trace vector 全生命周期只增不清（~5-10MB/h）。超过上限丢弃最旧
            // 一半 —— trace 只在调试/分析时读取（dump_present_trace/present_stats），
            // 保留最近窗口足够。erase 前半段是 O(n) memmove，但每 64K tick（60fps 下
            // ~18 分钟）才发生一次、单次移动 <2MB，热路径开销可忽略。
            constexpr size_t kPresentTraceCap = 1u << 16;
            if (present_qpc_ns_.size() > kPresentTraceCap) {
                const size_t drop = kPresentTraceCap / 2;
                present_qpc_ns_.erase(present_qpc_ns_.begin(), present_qpc_ns_.begin() + drop);
                present_source_.erase(present_source_.begin(), present_source_.begin() + drop);
                present_frame_num_.erase(present_frame_num_.begin(), present_frame_num_.begin() + drop);
            }
        }
        present_count_.fetch_add(1, std::memory_order_relaxed);
        switch (source) {
        case Source::AiFresh: n_ai_fresh_.fetch_add(1, std::memory_order_relaxed); break;
        case Source::AiStale: n_ai_stale_.fetch_add(1, std::memory_order_relaxed); break;
        case Source::PassthroughFresh: n_pt_fresh_.fetch_add(1, std::memory_order_relaxed); break;
        case Source::PassthroughStale: n_pt_stale_.fetch_add(1, std::memory_order_relaxed); break;
        case Source::NoSession: n_no_session_.fetch_add(1, std::memory_order_relaxed); break;
        }

        if (source == Source::AiFresh || source == Source::PassthroughFresh) {
            // Only a validated fresh draw becomes "the last valid frame" -- the stale path may
            // have resolved to UINT_MAX (one black tick), which must never be remembered as
            // something to repeat.
            last_actual_source = source;
            last_actual_slot = use_slot;
            last_actual_owner = use_owner;
        }
    }
}

// Aspect-preserving "fit inside the window" viewport (letterbox/pillarbox): scales the
// video's DISPLAY shape by the largest factor that still fits both window dimensions, centered --
// the leftover is filled with black bars by the ClearRenderTargetView in the draw paths below. The
// VS emits a full-NDC (-1..1) fullscreen triangle, so shrinking the viewport just scales+centers
// that triangle into this sub-rect while UVs still span the whole frame 0..1 -- no
// vertex/UV/shader change needed. Degenerate dims (pre-open / a 0-size window mid-resize) fall
// back to the full window so we never divide by zero or emit a zero-area viewport. Letterboxing by
// display_aspect_ rather than the coded grid is what keeps anamorphic sources (720x480 SAR
// 853:720 == 16:9) from being drawn as 3:2.
D3D11_VIEWPORT Player::fit_viewport() const{
    // shape = display_aspect_ : 1.0 -- the SAR-corrected shape, never the coded pixel count.
    float sw = display_aspect_ > 0.0 ? static_cast<float>(display_aspect_) : 1.0f;
    float sh = 1.0f;
    float ww = static_cast<float>(win_width_);
    // Reserve the top strip for the self-drawn title bar (top_bar_h(), drawn by build_top_bar())
    // so the video is letterboxed into the area BELOW it rather than being partly covered by
    // it -- the leftover strip is left black by draw_and_present()'s full-backbuffer clear and
    // the opaque top-bar ImGui window paints over it. The letterbox math runs against this
    // reduced region; region_y offsets the result downward past the bar. In fullscreen the
    // bar auto-hides (build_top_bar()) and only overlays on hover, exactly like the bottom
    // bar -- so no strip is reserved there and the video fills the whole screen.
    //
    // The URL page's address bar is floating chrome: it reserves NO strip (revealing or hiding
    // it never moves the video), so a Native page uses exactly the same viewport as ordinary
    // playback.
    float bar = fullscreen_.load(std::memory_order_relaxed) ? 0.0f : top_bar_h();
    float region_y = bar;
    float wh = static_cast<float>(win_height_) - bar;
    if (src_width_ <= 0 || src_height_ <= 0 || ww <= 0.0f || wh <= 0.0f)
        return D3D11_VIEWPORT{ 0.0f, 0.0f, static_cast<float>(win_width_),
                               static_cast<float>(win_height_), 0.0f, 1.0f };
    float scale = (ww / sw < wh / sh) ? (ww / sw) : (wh / sh);
    float dw = sw * scale;
    float dh = sh * scale;
    return D3D11_VIEWPORT{ (ww - dw) * 0.5f, region_y + (wh - dh) * 0.5f, dw, dh, 0.0f, 1.0f };
}

void Player::draw_and_present(Source source, UINT slot){
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);

    ID3D11RenderTargetView* rtvs[] = { backbuffer_rtv_.Get() };
    context_->OMSetRenderTargets(1, rtvs, nullptr);
    // Letterbox: clear the whole backbuffer to black first (the bars), then draw the video
    // into the aspect-fit sub-viewport. Previously this drew straight into a full-window
    // viewport, which stretched the video to the window's aspect ratio (non-uniform scale on
    // resize). Clearing is cheap and now mandatory since the video no longer covers every
    // pixel of the backbuffer.
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context_->ClearRenderTargetView(backbuffer_rtv_.Get(), black);
    if (slot == UINT_MAX) {
        ui_render_drawdata();
        ui_render_popups();
        swapchain_->Present(1, 0);
        return;
    }
    D3D11_VIEWPORT vp = fit_viewport();
    context_->RSSetViewports(1, &vp);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);

    struct SliceCB { UINT arraySlice; UINT pad[3]; };
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context_->Map(slice_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        SliceCB cb{ slot, {0, 0, 0} };
        memcpy(mapped.pData, &cb, sizeof(cb));
        context_->Unmap(slice_cb_.Get(), 0);
    }
    ID3D11Buffer* cbs[] = { slice_cb_.Get() };
    context_->PSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = { sampler_.Get() };
    context_->PSSetSamplers(0, 1, samplers);

    if (is_ai(source)) {
        context_->PSSetShader(ps_ai_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { ai_srv_[slot].Get() };
        context_->PSSetShaderResources(2, 1, srvs);
    } else {
        context_->PSSetShader(ps_nv12_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { pt_srv_y_[slot].Get(), pt_srv_uv_[slot].Get() };
        context_->PSSetShaderResources(0, 2, srvs);
    }

    context_->Draw(3, 0);

    // M2: present thread only renders an already-published draw-data snapshot (built on
    // the main thread by ui_tick()) -- see ui_render_drawdata()'s header comment. Called
    // here, after the video Draw(3,0) and before Present(1,0), still inside d3d_mutex_.
    ui_render_drawdata();
    ui_render_popups(); // popup layers (own swapchains, Present(0,0))

    swapchain_->Present(1, 0);
}

// M-C1: called from present_loop()'s splash branch (see below), already inside d3d_mutex_,
// whenever !session_active_ -- draws a plain dark background plus whatever ui_tick()
// published (build_splash_overlay()'s "loading" text) instead of draw_and_present()'s video
// path. Must NEVER touch any session-scoped resource (pt_ring_tex_/ai_ring_tex_/
// landing_tex_/ai_in_*/the decoder) -- only backbuffer_rtv_ (device-layer, alive for the
// whole Player lifetime) and ui_render_drawdata() (tolerates an empty/not-yet-published
// snapshot, see its own header comment). draw_and_present() itself is left unmodified.
void Player::draw_splash(){
    D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(win_width_), static_cast<float>(win_height_), 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);
    ID3D11RenderTargetView* rtvs[] = { backbuffer_rtv_.Get() };
    context_->OMSetRenderTargets(1, rtvs, nullptr);
    // Dark gray (not pure black) so first-screen chrome sits on a readable base rather than
    // a void. Matches the title bar's WindowBg tone above.
    const float clear_color[4] = { 0.14f, 0.14f, 0.16f, 1.0f };
    context_->ClearRenderTargetView(backbuffer_rtv_.Get(), clear_color);

    ui_render_drawdata();
    ui_render_popups(); // popup layers keep working without a session (they belong to the UI)

    swapchain_->Present(1, 0);
}

// ---- audio thread (WASAPI) ----------------------------------------------------------------
//
// Wholly implemented in player_audio.cpp (audio_loop()): it owns the WASAPI endpoint life
// cycle -- open/teardown/re-open inside one COM lifetime on the audio thread, default-output
// switch detection, error/stop-interruptible rebuild backoff -- plus the decode/resample/FIFO
// gating that keeps audio a pure slave of the clock below. compute_master_s() stays here
// because present_loop() reads the very same formula.

double Player::compute_master_s() const{
    if (playing_.load(std::memory_order_relaxed)) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        int64_t aq = anchor_qpc_ticks_.load(std::memory_order_relaxed);
        int64_t af = anchor_frame_.load(std::memory_order_relaxed);
        double elapsed_s = static_cast<double>(now.QuadPart - aq) / freq_.QuadPart;
        return static_cast<double>(af) / fps_ + elapsed_s;
    }
    // Paused: frozen at anchor_frame_, same as present_loop()'s own paused branch.
    return static_cast<double>(anchor_frame_.load(std::memory_order_relaxed)) / fps_;
}

void Player::recompute_session_rate(){
    int div = fps_div_.load(std::memory_order_relaxed);
    if (div < 1) div = 1;
    // Session fps/count: dense retimed timeline. source_fps_ stays fixed; source_frame_count_
    // may grow when a segmented source reveals more pictures than its container declared.
    if (source_fps_ > 0.0)
        fps_ = source_fps_ / static_cast<double>(div);
    else
        fps_ = 60.0 / static_cast<double>(div);
    if (source_frame_count_ > 0)
        frame_count_ = (source_frame_count_ + div - 1) / div; // ceil
    else
        frame_count_ = 0;
}

// Drop scrub ring + coarse-grid bucket tags after a session-timeline change (fps_div).
// Textures stay allocated; fill_next_grid_point / serve_hover_bucket rewrite content.
// Safe to call from the main thread while scrub_thread_ is running (same mutex as producers).
void Player::invalidate_scrub_timeline(){
    {
        std::lock_guard<std::mutex> lk(scrub_cache_mutex_);
        for (auto& e : scrub_cache_) e.bucket = -1;
        for (auto& e : scrub_grid_) e.bucket = -1;
        scrub_cache_next_ = 0;
        scrub_grid_done_ = 0;
        scrub_grid_stride_ = std::max<int64_t>(1, (frame_count_ > 1 ? frame_count_ - 1 : 1) /
            static_cast<int64_t>(scrub_grid_slots_ > 1 ? scrub_grid_slots_ - 1 : 1));
    }
    scrub_request_frame_.store(-1, std::memory_order_relaxed);
    scrub_grid_wanted_.store(true, std::memory_order_relaxed);
    scrub_cv_.notify_one();
}

UINT Player::wrap_pt_slot(int64_t frame_num) const{
    const int64_t cap = static_cast<int64_t>(ring_capacity_ > 0 ? ring_capacity_ : kRingCapacityDefault);
    int64_t m = frame_num % cap;
    if (m < 0) m += cap;
    return static_cast<UINT>(m);
}

UINT Player::wrap_ai_slot(int64_t frame_num) const{
    const int64_t cap = static_cast<int64_t>(ai_ring_capacity_ > 0 ? ai_ring_capacity_ : kRingCapacityDefault);
    int64_t m = frame_num % cap;
    if (m < 0) m += cap;
    return static_cast<UINT>(m);
}

double Player::qpc_freq_d(){
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
}

int64_t Player::qpc_to_ns(LARGE_INTEGER c){
    double ns_per_tick = 1e9 / qpc_freq_d();
    return static_cast<int64_t>(static_cast<double>(c.QuadPart) * ns_per_tick + 0.5);
}

// Hybrid sleep+spin wait (same strategy as spike 0/2), periodically checking stop_ so
// close() doesn't have to wait out a full frame interval. Returns false if stop was
// requested before the deadline.
bool Player::wait_until_qpc_or_stop(LARGE_INTEGER target, double ticks_per_ms){
    LARGE_INTEGER now;
    for (;;) {
        if (stop_.load(std::memory_order_relaxed)) return false;
        QueryPerformanceCounter(&now);
        double remaining_ms = static_cast<double>(target.QuadPart - now.QuadPart) / ticks_per_ms;
        if (remaining_ms <= 0.0) return true;
        if (remaining_ms > 2.0) {
            Sleep(static_cast<DWORD>(std::min(remaining_ms - 1.0, 5.0)));
        } else {
            YieldProcessor();
        }
    }
}

