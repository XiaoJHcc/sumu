// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "player.h"

#include <cstdlib>

// ---- error helpers -----------------------------------------------------------------------
void check_hr(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s failed: hr=0x%08lx", what, hr);
        throw std::runtime_error(buf);
    }
}

void check_cu(CUresult res, const char* what)
{
    if (res != CUDA_SUCCESS) {
        const char* name = nullptr;
        const char* desc = nullptr;
        cuGetErrorName(res, &name);
        cuGetErrorString(res, &desc);
        char buf[512];
        snprintf(buf, sizeof(buf), "%s failed: CUresult=%d (%s: %s)", what,
            static_cast<int>(res), name ? name : "?", desc ? desc : "?");
        throw std::runtime_error(buf);
    }
}

Player::Player(int width_hint, int height_hint, bool maximized){
    timeBeginPeriod(1);
    timer_period_set_ = true;

    create_window(width_hint, height_hint, maximized);
    create_device_and_swapchain();
    create_shader_pipeline();
    ui_init(); // present thread not started yet (starts below) -- no context contention
               // for this one-time ImGui/backend warmup.
    init_cuda_driver(); // device-independent-of-video-size CUDA setup only; landing
                         // texture + registration happen in open_session() once src dims
                         // are known.

    // M-C1: present_thread_ now starts here (device layer only, no video open yet) instead
    // of in open()/open_session() -- present_loop()'s splash branch (session_active_ still
    // false) draws a "loading" splash the whole time Python is loading AI models between
    // constructing Player and calling open(), instead of the window looking frozen for
    // that ~10+ second window. freq_ is a QPC system constant; pace_origin_qpc_ is the
    // pacing epoch, established here once so present_thread_'s tick_idx starts from a valid
    // baseline. open_session() must NOT write pace_origin_qpc_ directly (tick_idx lives in
    // present_loop() and only it may reset the two together) -- instead, when a (re)opened
    // session changes fps_, open_session() bumps pace_epoch_ and present_loop() re-anchors
    // origin+tick_idx itself (see present_loop()'s pacing comment). Originally (M-C1, before
    // reopen and its per-session fps_ changes existed) this note forbade re-anchoring at
    // all; M-C2 made it mandatory, routed through pace_epoch_ to keep present the sole writer.
    QueryPerformanceFrequency(&freq_);
    stop_.store(false, std::memory_order_relaxed);
    QueryPerformanceCounter(&pace_origin_qpc_);
    present_thread_ = std::thread(&Player::present_loop, this);
}

Player::~Player() { close(); }

void Player::open(const std::string& video_path){
    if (opened_) throw std::runtime_error("Player.open() called twice on the same Player");
    open_session(video_path);
}

// M-C1: the actual session-layer setup, split out of open() so open() can stay a thin,
// signature-stable compatibility shell (run_player.py/stress_seek_ai.py call it directly).
// Does NOT start present_thread_ (the constructor's job now, see above) -- only
// decode_thread_/audio_thread_, which are session-scoped. This milestone (M-C1) never
// calls this a second time on the same Player (open()'s opened_ guard above still forbids
// it) -- runtime reopen is M-C2's job, layered on top of this split without needing to
// revisit it.
void Player::open_session(const std::string& video_path){
    // open()/reopen() may run off the Python main thread (async network open). Make cu_ctx_
    // current on whichever thread is building the session -- same pattern as push_ai_frame().
    if (cu_ctx_)
        check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent (open_session)");

    video_path_ = video_path; // UI displays basename_of(video_path_) in the top bar

    std::string decode_err;
    if (!decoder_.open(video_path, device_.Get(), decode_err))
        throw std::runtime_error("decoder.open failed: " + decode_err);
    source_fps_ = decoder_.fps();
    source_frame_count_ = decoder_.frame_count();
    // Session timeline = source retimed by fps_div (1 = identity). Downstream present/AI/
    // wrap_slot see a dense 0..N-1 stream at fps_=source_fps/div -- same as opening a
    // native lower-fps file. Decode still runs every GOP frame; only every Nth is kept.
    recompute_session_rate();
    // M-C2: fps_ just (re)set for this session and may DIFFER from the previous session's
    // (reopen 4K60 <-> 1080p30) or from the constructor's default 60.0 (first open of a
    // 30fps file). present_loop()'s fixed-cadence formula is pace_origin_qpc_ + tick_idx/
    // fps_, with tick_idx accumulated over the whole present-thread lifetime -- so a bare
    // fps_ change desyncs it (target jumps seconds into the future when fps_ drops, freezing
    // present until wall-clock catches up; this was the multi-second reopen freeze). Signal
    // present_thread_ (the only writer of pace_origin_qpc_/tick_idx) to re-anchor. Bumped
    // right after the fps_ write, before any resource build/decode-buffer wait below could
    // let present act on a stale origin. release publishes both fps_ and the epoch.
    pace_epoch_.fetch_add(1, std::memory_order_release);
    src_width_ = decoder_.width();
    src_height_ = decoder_.height();
    if (src_width_ <= 0 || src_height_ <= 0)
        throw std::runtime_error("decoder reported invalid dimensions");

    // Resolution-aware ring depths (I8), plus network shallow profile when source is http(s).
    // Network: smaller PT/AI rings so decode-ahead and AI lead stay thin (less remote
    // pre-read; seek reclaims the ring faster). Local: unchanged deep lookahead.
    const bool network = decoder_.is_network();
    network_source_ = network;
    pick_ring_capacities(src_width_, src_height_, ring_capacity_, ai_ring_capacity_, network);
    if (ring_capacity_ < 16) ring_capacity_ = 16;
    if (ring_capacity_ > kRingCapacityMax) ring_capacity_ = kRingCapacityMax;
    if (ai_ring_capacity_ < 16) ai_ring_capacity_ = 16;
    if (ai_ring_capacity_ > kRingCapacityMax) ai_ring_capacity_ = kRingCapacityMax;
    // Local keeps a 10-frame safety margin under ring capacity; network ring is already
    // shallow so only leave a 4-frame margin (still >= 1).
    const int64_t ahead_margin = network ? 4 : 10;
    decode_ahead_max_ = static_cast<int64_t>(ring_capacity_) - ahead_margin;
    if (decode_ahead_max_ < 1) decode_ahead_max_ = 1;
    fprintf(stderr, "[sumu] pt_ring=%u ai_ring=%u decode_ahead_max=%lld (%dx%d)%s\n",
        ring_capacity_, ai_ring_capacity_, static_cast<long long>(decode_ahead_max_),
        src_width_, src_height_, network ? " network" : "");

    // ---- audio (spike, additive) -- software-decode-only AVCodecContext for the audio
    // stream, if any (never touches d3d11va/hw_device_ctx_). Built here on the main
    // thread: FFmpeg contexts have no thread affinity, so this is safe to hand off to
    // audio_thread_ below (std::thread's launch is a happens-before edge for everything
    // written before it starts). The WASAPI/SwrContext pieces are NOT built here -- they
    // live entirely inside audio_loop() itself, because CoInitializeEx must run on the
    // thread that goes on to use the resulting COM interfaces. No audio stream, no usable
    // decoder for it, or a failed avcodec_open2 all degrade to has_audio_=false: playback
    // proceeds silent, exactly like "this file has no audio track" (never a hard failure).
    has_audio_ = decoder_.has_audio();
    if (has_audio_) {
        AVCodecParameters* apar = decoder_.audio_codecpar();
        const AVCodec* adec = avcodec_find_decoder(apar->codec_id);
        if (!adec) {
            fprintf(stderr, "[audio] no decoder for codec_id=%d, audio disabled\n", static_cast<int>(apar->codec_id));
            has_audio_ = false;
        } else {
            audio_codec_ctx_ = avcodec_alloc_context3(adec);
            if (!audio_codec_ctx_ ||
                avcodec_parameters_to_context(audio_codec_ctx_, apar) < 0 ||
                avcodec_open2(audio_codec_ctx_, adec, nullptr) < 0) {
                fprintf(stderr, "[audio] failed to open audio decoder, audio disabled\n");
                if (audio_codec_ctx_) avcodec_free_context(&audio_codec_ctx_);
                has_audio_ = false;
            }
        }
    }

    create_ring_resources();
    create_landing_texture();
    register_landing_with_cuda();
    create_ai_input_bridge();

    pt_tag_.assign(ring_capacity_, -1);
    ai_tag_.assign(ai_ring_capacity_, -1);
    // Must reset before starting decode_thread_ / the start-buffer wait: otherwise a
    // reopen() inherits the previous session's high-water (e.g. 10000) and the wait
    // returns immediately with an empty ring -- green/black first frames.
    pt_high_water_.store(-1, std::memory_order_relaxed);
    present_head_frame_.store(0, std::memory_order_relaxed);
    anchor_frame_.store(0, std::memory_order_relaxed);
    clock_frame_.store(0, std::memory_order_relaxed);
    playing_.store(false, std::memory_order_relaxed);
    seek_slot_hint_.store(0, std::memory_order_relaxed);
    // Bump seek_version so present_loop/audio_loop treat the new session like a seek
    // (drop last_frame_local / stale slot / audio FIFO from the previous file).
    seek_version_.fetch_add(1, std::memory_order_relaxed);

    // Decode thread starts immediately and races ahead (throttled) of the present head.
    // present_thread_ is already running by this point (started in the constructor, see
    // above) but present_head_frame_ still sits at its default 0 -- present_loop()'s splash
    // branch (session_active_ still false right up until the last line of this function)
    // never touches present_head_frame_ -- so the throttle below simply treats "no real
    // playback position yet" the same as frame 0, letting decode build its initial buffer.
    // session_stop_ was left false by close_session()'s own reset (or is still its
    // default-false on a first open()) -- see decode_loop()/audio_loop()'s exit conditions.
    decode_thread_ = std::thread(&Player::decode_loop, this);

    // M4: independent scrub-decode path for seekbar hover thumbnails. Best-effort -- failing
    // to open the second decoder just means no hover previews (the player is fully functional
    // without them), never a failed session. Its GPU resources were already built above by
    // create_ring_resources() -> create_scrub_resources().
    // Network profile: SKIP the second open entirely. A parallel scrub Decoder means a
    // second HTTP connection + open-time full-timeline keyframe grid seeks -- the single
    // worst remote-IO pattern this player has. Local disk keeps scrub as before.
    if (!network) {
        std::string scrub_err;
        if (scrub_decoder_.open(video_path, device_.Get(), scrub_err))
            scrub_thread_ = std::thread(&Player::scrub_loop, this);
        else
            fprintf(stderr, "[sumu] scrub decoder open failed (%s) -- hover thumbnails disabled\n",
                scrub_err.c_str());
    } else {
        fprintf(stderr, "[sumu] network source: scrub decoder disabled (no second open / no grid prefetch)\n");
    }

    // Network: wait for fewer initial frames and allow a longer open timeout (RTT + probe).
    const int64_t start_buf = network ? kNetworkStartBufferFrames : kStartBufferFrames;
    const double start_timeout_s = network ? 30.0 : 10.0;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    for (;;) {
        if (pt_high_water_.load(std::memory_order_relaxed) >= start_buf - 1)
            break;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double waited_s = static_cast<double>(now.QuadPart - t0.QuadPart) / qpc_freq_d();
        if (waited_s > start_timeout_s)
            throw std::runtime_error("decode thread failed to buffer initial frames within timeout");
        Sleep(1);
    }

    // Start paused at frame 0 -- caller must call play() to start the wall clock.
    // (Clock / playing_ / seek_version_ already zeroed above before decode started.)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    anchor_qpc_ticks_.store(now.QuadPart, std::memory_order_relaxed);
    // Re-assert frame 0 after the start-buffer wait: present was in splash the whole time
    // and must not inherit the previous file's head when session_active_ flips true.
    anchor_frame_.store(0, std::memory_order_relaxed);
    clock_frame_.store(0, std::memory_order_relaxed);
    present_head_frame_.store(0, std::memory_order_relaxed);
    playing_.store(false, std::memory_order_relaxed);
    seek_slot_hint_.store(0, std::memory_order_relaxed);
    seek_version_.fetch_add(1, std::memory_order_relaxed);

    if (has_audio_)
        audio_thread_ = std::thread(&Player::audio_loop, this);

    opened_ = true;
    // Last step, once every session resource above is fully built: present_loop()'s
    // splash branch only stops drawing the splash and starts picking/presenting real
    // frames once this flips true (see present_loop()).
    session_active_.store(true, std::memory_order_relaxed);

    // Arm the coarse-grid prefetch immediately (the player opens paused, so scrub_loop() starts
    // filling right away using the idle NVDEC) rather than lazily on first hover -- seekbar
    // dragging is a high-frequency action and we want coverage ready before the user reaches
    // for it. The fill self-suspends the moment playback starts (see scrub_loop()).
    // Network: scrub thread was never started -- do not request grid fill.
    if (!network) {
        scrub_grid_wanted_.store(true, std::memory_order_relaxed);
        scrub_cv_.notify_one();
    }

    // Auto-size the window to this video (1:1 point-for-point, capped/on-screen -- see the
    // method). Deferred to ui_tick() on the main thread: open_session may run on a worker
    // (async URL open), and SetWindowPos -> synchronous WM_SIZE -> ui_tick re-entrancy from
    // a non-main thread is unsafe. Main applies the flag on the next tick after session_active_.
    pending_resize_for_video_.store(true, std::memory_order_release);
}

// M-C1: joins/tears down only the session-scoped state (decode/audio threads, the D3D11
// session textures/SRVs/tags, the session's CUDA registrations, the audio codec ctx, the
// decoder itself) -- leaves the device layer (device_/context_/swapchain_/shader pipeline/
// ImGui/cu_ctx_) fully intact, since present_thread_ keeps running across this (drawing the
// splash again immediately after). Only ever called from close() in this milestone, always
// with present_thread_ already joined (see close() below) -- so there is no present-detach
// handshake to do here yet; that's an M-C2 concern for a live (running-present) reopen.
void Player::close_session(){
    if (cu_ctx_)
        check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent (close_session)");

    session_stop_.store(true, std::memory_order_relaxed);
    scrub_cv_.notify_one(); // wake scrub_thread_ out of its wait so it observes session_stop_
    if (decode_thread_.joinable()) decode_thread_.join();
    if (audio_thread_.joinable()) audio_thread_.join();
    // scrub_thread_ takes d3d_mutex_ during its blit (like decode_thread_), so join it here
    // BEFORE the d3d_lock block below -- joining it under d3d_mutex_ would risk self-deadlock.
    if (scrub_thread_.joinable()) scrub_thread_.join();
    session_stop_.store(false, std::memory_order_relaxed); // reset so a future open_session() starts clean

    if (audio_codec_ctx_) avcodec_free_context(&audio_codec_ctx_);

    // M-C2 fix (device-removed root cause): reopen() no longer relies on Python's
    // Scheduler.stop() winning a timing race against this teardown. push_ai_frame()/
    // get_cuda_nv12_by_frame() run on Python's own AI-producer/consumer threads (not
    // joined above -- decode_thread_/audio_thread_ are the only threads this function
    // owns) and both take d3d_mutex_ around the exact CUDA-interop/D3D11 resources torn
    // down below (see push_ai_frame():875, get_cuda_nv12_by_frame():978) -- WITHOUT this
    // lock here, close_session() could unregister/Reset() a resource while one of those
    // calls still had it mapped, corrupting driver state (observed: DXGI_ERROR_DEVICE_
    // REMOVED under stress_reopen.py). Both methods now re-check session_active_ (which
    // reopen() already flipped false before calling close_session(), see reopen()'s own
    // comment) immediately after taking their locks and no-op (miss/return) instead of
    // touching session resources, so by the time THIS thread acquires d3d_lock below any
    // push_ai_frame()/get_cuda_nv12_by_frame() call already past that check is operating
    // on the still-valid pre-teardown resources and will finish and release the lock
    // before we can proceed; any call arriving after is turned away by the same check
    // before it ever touches d3d_mutex_-guarded state. NOT taken while joining
    // decode_thread_/audio_thread_ above -- those threads also take d3d_mutex_ themselves
    // (decode_loop's ring writes, audio does not) and joining them under this lock would
    // risk a self-deadlock; the lock only wraps the resource teardown itself.
    {
        std::lock_guard<std::mutex> d3d_lock(d3d_mutex_);

        // CUDA unregister BEFORE releasing the D3D11 textures they wrap (cu_res_ wraps
        // landing_tex_; ai_in_cu_res_y_/uv_ wrap ai_in_y_tex_/ai_in_uv_tex_) -- cu_ctx_
        // itself is a device-layer resource and stays current/alive here (only close()'s
        // final teardown below releases it), same ordering the original close() already
        // used.
        if (cu_res_) { cuGraphicsUnregisterResource(cu_res_); cu_res_ = nullptr; }
        if (ai_in_cu_res_y_) { cuGraphicsUnregisterResource(ai_in_cu_res_y_); ai_in_cu_res_y_ = nullptr; }
        if (ai_in_cu_res_uv_) { cuGraphicsUnregisterResource(ai_in_cu_res_uv_); ai_in_cu_res_uv_ = nullptr; }
        if (ai_in_cu_buf_) { cuMemFree(ai_in_cu_buf_); ai_in_cu_buf_ = 0; }

        // Explicit release of session-scoped D3D11 resources (pre-M-C1 these only ever
        // went away implicitly, via ~Player's ComPtr destructors) -- clears the slate so
        // present_loop's splash branch never indexes a half-torn-down ring while
        // !session_active_, and so a future open_session() rebuilds everything from
        // scratch. pt_tag_/ai_tag_ and the SRV vectors are also read under ready_mutex_
        // by get_cuda_nv12_by_frame()/decode_loop() (a stale pt_tag_[slot] index into an
        // already-.clear()'d vector was the second, independent crash this fix closes) --
        // nested inside d3d_mutex_ here, matching decode_loop()'s/seek()'s existing lock
        // order (d3d_mutex_ outer, ready_mutex_ inner), never the reverse.
        pt_ring_tex_.Reset();
        for (auto& srv : pt_srv_y_) srv.Reset();
        for (auto& srv : pt_srv_uv_) srv.Reset();
        {
            std::lock_guard<std::mutex> ready_lock(ready_mutex_);
            pt_srv_y_.clear();
            pt_srv_uv_.clear();
            pt_tag_.clear();
        }

        ai_ring_tex_.Reset();
        for (auto& srv : ai_srv_) srv.Reset();
        {
            std::lock_guard<std::mutex> ready_lock(ready_mutex_);
            ai_srv_.clear();
            ai_tag_.clear();
        }

        landing_tex_.Reset();

        ai_in_y_tex_.Reset();
        ai_in_y_rtv_.Reset();
        ai_in_uv_tex_.Reset();
        ai_in_uv_rtv_.Reset();

        // M4 scrub thumbnail resources. scrub_thread_ is already joined (above), and reopen()
        // has already put present into its splash branch (so no live ImGui draw snapshot still
        // references a thumbnail SRV) -- safe to release the NV12 blit source and the thumb
        // pool. scrub_cache_ is also read by get_thumbnail() on the main thread; the same main
        // thread runs close_session(), so there is no concurrent reader here, but take
        // scrub_cache_mutex_ anyway to keep every scrub_cache_ access uniformly guarded.
        scrub_nv12_tex_.Reset();
        scrub_srv_y_.Reset();
        scrub_srv_uv_.Reset();
        {
            std::lock_guard<std::mutex> scrub_cache_lock(scrub_cache_mutex_);
            scrub_cache_.clear();
            scrub_cache_next_ = 0;
            scrub_grid_.clear();
            scrub_grid_rank_.clear();
            scrub_grid_octave_.clear();
            scrub_grid_stride_ = 0;
            scrub_grid_done_ = 0;
        }
        scrub_request_frame_.store(-1, std::memory_order_relaxed);
        scrub_grid_wanted_.store(false, std::memory_order_relaxed);
    }

    decoder_.close();
    scrub_decoder_.close();

    session_active_.store(false, std::memory_order_relaxed);
}

// M-C2: runtime file swap -- present_thread_ (and the window) is NEVER stopped/rebuilt for
// this, unlike close()/~Player(). This is the present-detach handshake close_session()'s own
// M-C1 comment flagged as missing: flip session_active_ false so present_loop()'s NEXT
// iteration takes the splash branch (draw_splash(), see present_loop()) instead of
// draw_and_present(), then spin on present_in_splash_ until that has actually been observed.
// present_loop() picks exactly one branch per iteration (it reads session_active_ once at
// the top and commits to either draw_and_present() or draw_splash() for the WHOLE iteration),
// so the moment a splash iteration has stored present_in_splash_ = true, any earlier
// draw_and_present() call has already fully returned (same thread, program order) -- present
// is guaranteed to not be touching pt_ring_tex_/ai_ring_tex_/the SRVs/landing_tex_/decoder_
// again until session_active_ flips back true, which is exactly what close_session() below
// needs to safely tear those down. Bounded spin: present_loop() is on a fixed fps_ cadence
// and never blocks on decode/AI, so this is at most ~1-2 present ticks; a 2s timeout guards
// against a genuinely wedged present thread rather than dead-waiting forever.
//
// close_session()/open_session() run here on the SAME thread (Python's main thread) that
// called open()/close() originally -- cu_ctx_ is already current on this thread from the
// constructor (see init_cuda_driver()), so no CUDA context re-binding is needed, same as
// today's open()/close(). No additional d3d_mutex_ protection was added around
// close_session()'s resource teardown or open_session()'s resource creation: close_session()
// never issues any ID3D11DeviceContext (context_) calls itself (only ComPtr::Reset()/FFmpeg
// teardown, which are thread-safe COM refcounting, not context state mutation), and
// open_session()'s create_ring_resources()/create_landing_texture()/create_ai_input_bridge()
// only call ID3D11Device (device_) resource-creation methods, which the D3D11 runtime
// guarantees are free-threaded regardless of ID3D11Multithread::SetMultithreadProtected (that
// flag only covers the immediate context, i.e. draw_splash()'s/draw_and_present()'s context_
// calls) -- see docs/native_core.md. decode_thread_/audio_thread_ are joined by
// close_session() before any of that teardown runs, so there is no concurrent producer onto
// the resources being torn down either. Flagging this reasoning explicitly per the task brief
// in case a reviewer wants to double-check it.
//
// Returns the new session's frame_count() (Python rebuilds its Scheduler from this plus its
// own get_video_meta_data(path) probe -- see play.py).
//
// Failure contract: close_session() has already torn the previous file down before
// open_session() is attempted. If open_session() throws (unsupported/corrupt/partial file),
// the previous session is gone and opened_ is cleared so require_open() fails cleanly and
// the open-prompt overlay returns -- Python catches the exception and shows "无法打开"
// rather than exiting the process. Deliberately does NOT try to restore the previous file
// (would need a full re-open of a path we no longer hold a live session for).
int64_t Player::reopen(const std::string& video_path){
    require_open(); // reopen is "open a different file into an already-open Player"

    session_active_.store(false, std::memory_order_release);

    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    for (;;) {
        if (present_in_splash_.load(std::memory_order_acquire)) break;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double waited_s = static_cast<double>(now.QuadPart - t0.QuadPart) / qpc_freq_d();
        if (waited_s > 2.0)
            throw std::runtime_error("reopen: present thread failed to detach into splash within 2s");
        Sleep(1);
    }

    close_session();
    try {
        open_session(video_path);
    } catch (...) {
        // Previous session already closed; new one never became active. Drop opened_ so
        // play()/seek()/etc. refuse cleanly and the splash shows the open-prompt again.
        opened_ = false;
        video_path_.clear();
        network_source_ = false;
        throw;
    }
    return frame_count_;
}

// Export-mode teardown: detach present into splash, then tear down the playback session and
// clear opened_ so the open-prompt returns. Mirrors reopen()'s present-detach handshake
// WITHOUT opening a new session. Safe to call with no live session (present is already in
// splash, close_session() handles the empty state).
void Player::close_current_session(){
    session_active_.store(false, std::memory_order_release);
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    for (;;) {
        if (present_in_splash_.load(std::memory_order_acquire)) break;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double waited_s = static_cast<double>(now.QuadPart - t0.QuadPart) / qpc_freq_d();
        if (waited_s > 2.0)
            throw std::runtime_error("close_current_session: present failed to detach within 2s");
        Sleep(1);
    }
    close_session();
    opened_ = false;
    video_path_.clear();
    network_source_ = false;
}

void Player::close(){
    if (closed_) return;
    closed_ = true;
    set_quit(); // this Player is on its way out -- keep should_quit() consistent even if
                // close() is reached by a path other than WM_DESTROY/ESC (e.g. play.py's
                // no-file-selected early exit).
    stop_.store(true, std::memory_order_relaxed);
    if (present_thread_.joinable()) present_thread_.join(); // barrier: present is now fully
                                                              // stopped before anything below
                                                              // touches D3D/CUDA state.
    close_session(); // safe now that present isn't running; also safe to call even if
                      // open_session() was never called (every member it touches is still
                      // at its default-empty/null state).

    // Present thread (the only ImGui-touching thread) has now joined -- safe to tear down
    // ImGui's device objects/context before DestroyWindow below.
    ui_shutdown();

    if (cu_ctx_) { cuCtxSetCurrent(nullptr); cuDevicePrimaryCtxRelease(cu_dev_); cu_ctx_ = nullptr; }

    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (timer_period_set_) { timeEndPeriod(1); timer_period_set_ = false; }
}

// Folder picker for the web-streaming server's video root. SHBrowseForFolderW keeps this
// free of explicit COM apartment setup on the main thread (IFileOpenDialog would need it).
std::string Player::pick_folder(){
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = L"Select video root folder"; // native dialog chrome; kept locale-neutral
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return std::string();
    wchar_t path_buf[MAX_PATH] = {};
    std::string result;
    if (SHGetPathFromIDListW(pidl, path_buf)) {
        int n = WideCharToMultiByte(CP_UTF8, 0, path_buf, -1, nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            result.resize(static_cast<size_t>(n - 1));
            WideCharToMultiByte(CP_UTF8, 0, path_buf, -1, result.data(), n, nullptr, nullptr);
        }
    }
    CoTaskMemFree(pidl);
    return result;
}

std::string Player::pick_open_file(){
    wchar_t file_buf[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    // Double-NUL-terminated filter; labels come from the i18n string table.
    // Must stay alive until GetOpenFileNameW returns (local wstring is fine).
    std::wstring filter;
    auto append_filter_pair = [&](const std::string& label_utf8, const wchar_t* patterns) {
        std::wstring label = utf8_to_wide(label_utf8);
        filter.append(label);
        filter.push_back(L'\0');
        filter.append(patterns);
        filter.push_back(L'\0');
    };
    append_filter_pair(ui_str_.dialog_video_files,
        L"*.mp4;*.mkv;*.mov;*.avi;*.hevc;*.h265;*.ts");
    append_filter_pair(ui_str_.dialog_all_files, L"*.*");
    filter.push_back(L'\0');
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = static_cast<DWORD>(ARRAYSIZE(file_buf));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::string();

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, file_buf, -1, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) return std::string();
    std::string utf8_path(static_cast<size_t>(utf8_len - 1), '\0'); // -1: drop the NUL WideCharToMultiByte counts
    WideCharToMultiByte(CP_UTF8, 0, file_buf, -1, utf8_path.data(), utf8_len, nullptr, nullptr);
    return utf8_path;
}

// Save-file dialog for the export "单独路径" output picker (GetSaveFileNameW + overwrite
// prompt). `default_name` seeds the file name; empty falls back to "output.mp4".
std::string Player::pick_save_file(const std::string& default_name){
    wchar_t file_buf[MAX_PATH] = {};
    std::wstring def = utf8_to_wide(default_name.empty() ? "output.mp4" : default_name);
    size_t n = std::min(def.size(), static_cast<size_t>(ARRAYSIZE(file_buf) - 1));
    for (size_t i = 0; i < n; ++i) file_buf[i] = def[i];
    file_buf[n] = L'\0';

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    std::wstring filter;
    auto append_filter_pair = [&](const std::string& label_utf8, const wchar_t* patterns) {
        std::wstring label = utf8_to_wide(label_utf8);
        filter.append(label);
        filter.push_back(L'\0');
        filter.append(patterns);
        filter.push_back(L'\0');
    };
    append_filter_pair(ui_str_.dialog_video_files, L"*.mp4");
    append_filter_pair(ui_str_.dialog_all_files, L"*.*");
    filter.push_back(L'\0');
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = static_cast<DWORD>(ARRAYSIZE(file_buf));
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return std::string();

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, file_buf, -1, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) return std::string();
    std::string utf8_path(static_cast<size_t>(utf8_len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, file_buf, -1, utf8_path.data(), utf8_len, nullptr, nullptr);
    return utf8_path;
}

void Player::pump_messages(){
    MSG msg{};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { set_quit(); break; }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

// ---- window resize (WM_SIZE) -- main-thread-only (called from WndProc via
// pump_messages()). Rebuilds the swapchain's backbuffer RTV to match the new client area;
// takes d3d_mutex_ to serialize against the present thread's draw_and_present() (see file
// header for the lock's role). Does not touch pacing/anchor state, DXGI_SCALING_STRETCH,
// or seek/CUDA/audio logic.
void Player::on_resize(UINT w, UINT h){
    if (!swapchain_) return;
    if (w == win_width_ && h == win_height_) return;
    std::lock_guard<std::mutex> d3d_lock(d3d_mutex_);
    backbuffer_rtv_.Reset();
    check_hr(swapchain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
    ComPtr<ID3D11Texture2D> backbuffer;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    check_hr(device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &backbuffer_rtv_), "CreateRenderTargetView");
    win_width_ = w;
    win_height_ = h;
}

// ---- auto-size the window to the just-opened video ------------------------------------
// Called from open_session() (main thread) once src_width_/src_height_ are known. Sizes the
// window so the video renders point-for-point (1:1, no scaling) in the letterbox region
// BELOW the title bar: client = src_width_ x (src_height_ + top_bar_h()). Constraints:
//   * width capped at 80% of the CURRENT monitor's work-area width;
//   * the whole window must fit within that monitor's work area (height too);
//   * when the 1:1 size exceeds either budget, the video is scaled down UNIFORMLY (aspect
//     preserved -- fit_viewport() still letterboxes correctly at any window size);
//   * final position clamped so the window sits fully inside the work area.
// DPI / multi-monitor: everything here is in physical pixels on the target monitor. The
// process is Per-Monitor-DPI-Aware-V2 (see create_window()), so GetMonitorInfo's rcWork and
// the D3D backbuffer client size are both physical pixels on the SAME monitor -- no HiDPI
// scaling conversion is needed, and MonitorFromWindow(MONITOR_DEFAULTTONEAREST) picks the
// monitor the window currently sits on. top_bar_h() already tracks ui_dpi_scale_ so the
// reserved strip stays the same physical height as the drawn bar. No-op while maximized or
// fullscreen (the user chose a full-area layout; auto-resizing out from under that would be
// wrong). Because the window is borderless (WM_NCCALCSIZE hollows the frame), window rect ==
// client rect, so the computed client dimensions are passed straight to SetWindowPos.
// SetWindowPos drives WM_SIZE synchronously -> on_resize() (swapchain resize) + the WndProc
// ui_tick() refresh.
void Player::resize_window_for_video(){
    if (!hwnd_ || fullscreen_.load(std::memory_order_relaxed) || IsZoomed(hwnd_)) return;
    if (src_width_ <= 0 || src_height_ <= 0) return;

    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfo(mon, &mi)) return;
    const LONG work_w = mi.rcWork.right - mi.rcWork.left;
    const LONG work_h = mi.rcWork.bottom - mi.rcWork.top;
    if (work_w <= 0 || work_h <= 0) return;

    const float sw = static_cast<float>(src_width_);
    const float sh = static_cast<float>(src_height_);
    const float bar = top_bar_h();

    // Budgets: width <= 80% of work area; total client height <= work area height, which
    // leaves work_h - bar for the video itself.
    const float max_w = static_cast<float>(work_w) * 0.80f;
    const float avail_video_h = static_cast<float>(work_h) - bar;

    float scale = 1.0f; // 1.0 == point-for-point
    if (sw > max_w) scale = std::min(scale, max_w / sw);
    if (avail_video_h > 0.0f && sh > avail_video_h) scale = std::min(scale, avail_video_h / sh);

    int client_w = std::max(1, static_cast<int>(std::lround(sw * scale)));
    int client_h = std::max(1, static_cast<int>(std::lround(sh * scale + bar)));

    // Keep the window's current center where it is (don't force-center on the monitor);
    // only nudge it back onto the work area if the new size would spill off an edge.
    RECT cur{};
    GetWindowRect(hwnd_, &cur);
    int center_x = cur.left + (cur.right - cur.left) / 2;
    int center_y = cur.top + (cur.bottom - cur.top) / 2;
    int x = center_x - client_w / 2;
    int y = center_y - client_h / 2;
    if (x + client_w > mi.rcWork.right)  x = mi.rcWork.right - client_w;
    if (y + client_h > mi.rcWork.bottom) y = mi.rcWork.bottom - client_h;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top)  y = mi.rcWork.top;

    SetWindowPos(hwnd_, nullptr, x, y, client_w, client_h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---- borderless custom-caption support (WM_NCCALCSIZE/WM_NCHITTEST, see WndProc) --------
// caption_drag_x0_/x1_/y1_ are published each frame by build_top_bar() from its
// "##drag_region" InvisibleButton's item rect (client-space, since the top-bar ImGui window
// sits at client (0,0)) and consumed here by WndProc's WM_NCHITTEST to report HTCAPTION over
// that region -- giving OS-native window drag/double-click-to-maximize/Aero-Snap without a
// synthetic WM_NCLBUTTONDOWN SendMessage (which used to leave ImGui's mouse-down state stuck
// until an extra click). Zero-initialized so before the first frame renders (x1_ <= x0_)
// point_in_caption_drag() is always false and WM_NCHITTEST safely falls back to HTCLIENT.
bool Player::point_in_caption_drag(int x, int y) const{
    return caption_drag_x1_ > caption_drag_x0_ && x >= (int)caption_drag_x0_ &&
        x < (int)caption_drag_x1_ && y >= 0 && y < (int)caption_drag_y1_;
}

void Player::play(){
    require_open();
    if (playing_.load(std::memory_order_relaxed)) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    anchor_qpc_ticks_.store(now.QuadPart, std::memory_order_relaxed);
    anchor_frame_.store(clock_frame_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    playing_.store(true, std::memory_order_relaxed);
}

void Player::pause(){
    require_open();
    if (!playing_.load(std::memory_order_relaxed)) return;
    anchor_frame_.store(clock_frame_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    playing_.store(false, std::memory_order_relaxed);
}

// ---- audio: volume/mute (additive) -- pure native audio state, deliberately NOT routed
// through UiIntents/scheduler (see task brief: this needs no scheduler coordination, unlike
// seek/transport/config). Writers are the bottom-bar slider/mute icon and WndProc's
// Up/Down/M keys (both main thread); reader is audio_loop()'s own thread, which applies the
// resulting gain to samples right before handing them to WASAPI. Plain atomics, no lock
// needed on either side.
void Player::set_volume(float v) { volume_.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_relaxed); }

void Player::toggle_mute() { muted_.store(!muted_.load(std::memory_order_relaxed), std::memory_order_relaxed); }

// ---- present-side view controls (Phase 6 M-D, additive) -- see ai_enabled_'s own member
// comment for the full design writeup (native-only atomic, no UiIntents round-trip,
// present_loop() reads it directly). Writer is build_settings_panel()'s checkbox (main
// thread) and WndProc's 'D' key; reader is present_loop() on the present thread. Also
// exposed via pybind for Python-side persistence (M-E) and scripted verification.
void Player::set_ai_enabled(bool v) { ai_enabled_.store(v, std::memory_order_relaxed); }

// Temporal downsample 1/N. Decode still runs every source frame; only every Nth is written
// into the ring as a dense session frame (0,1,2,...). Session fps_/frame_count_ are retimed
// so present + AI look identical to a native source_fps/N file. Apply may require a seek
// remap when a session is already open.
void Player::set_fps_div(int v){
    if (v < 1) v = 1;
    if (v > 4) v = 4;
    int old = fps_div_.load(std::memory_order_relaxed);
    if (v == old) return;
    fps_div_.store(v, std::memory_order_relaxed);
    if (!opened_) return;
    // Map current session position back to source, retime, seek onto the new grid.
    int64_t cur = clock_frame_.load(std::memory_order_relaxed);
    int64_t source_pos = cur * std::max(1, old);
    recompute_session_rate();
    pace_epoch_.fetch_add(1, std::memory_order_release);
    // Scrub thumbs are keyed in session-frame space; buckets filled under the old div map
    // to the wrong wall-clock content (e.g. div 1->2 shows ~half-time until exact hover).
    // Drop both tiers and re-arm the coarse grid so fill_next_grid_point rewrites them.
    invalidate_scrub_timeline();
    int64_t new_session = source_pos / std::max(1, v);
    try {
        seek(new_session);
    } catch (...) {
        // Best-effort: rate already updated; next play tick uses new fps.
    }
}

// ---- UI (M2): main thread only -- NewFrame/build/Render/CloneOutput, publish snapshot ---
//
// Called once per Python-driven tick (pybind's ui_tick()). Never touches d3d_mutex_ (see
// the context-touch audit above ui_init(), private section below -- nothing here issues
// D3D11 calls except the CPU-side font-atlas bookkeeping inside ImGui::NewFrame(), which is
// backend-agnostic). Defined here in the public section (pybind must bind it directly);
// ui_init()/ui_shutdown()/ui_render_drawdata() stay private since only this class's own
// methods call them.
void Player::ui_tick(){
    if (!ui_ready_) return;

    // Re-entrancy guard. A window-size change initiated from INSIDE build_ui() -- the custom
    // title bar's maximize/restore button (ShowWindow()) or the fullscreen button
    // (toggle_fullscreen() -> SetWindowPos()) -- drives a SYNCHRONOUS WM_SIZE, whose WndProc
    // handler calls ui_tick() again to refresh the self-drawn bars live at the new size.
    // Doing that here, while this frame's ImGui::NewFrame() is still open, would begin a
    // nested NewFrame() with no matching Render() -- ImGui state corruption that hard-hangs
    // the main thread (confirmed via repro: in-frame ShowWindow -> "ui_tick RE-ENTRANT" ->
    // window stops pumping messages). Skip the nested refresh: the outer frame finishes and
    // publishes normally, and the next Python-driven tick (~one frame / ~20ms later) lays the
    // bars out at the new size. The live drag-resize refresh is unaffected -- that WM_SIZE
    // comes from the OS modal move/size loop, NOT from within a frame, so this guard is false
    // there and the WndProc ui_tick() runs as before. Main-thread-only, so a plain bool (no
    // atomic) is correct: the re-entrant call is synchronous on this same thread.
    if (in_ui_tick_) return;
    in_ui_tick_ = true;
    struct Guard { bool* f; ~Guard() { *f = false; } } _guard{ &in_ui_tick_ };

    // Apply open_session's deferred window fit on the main thread only (see open_session).
    if (pending_resize_for_video_.exchange(false, std::memory_order_acq_rel))
        resize_window_for_video();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    build_ui(); // M3: real product UI (top bar / bottom bar / settings panel).
    ImGui::Render();

    ImDrawData* dd = ImGui::GetDrawData();
    auto snap = std::make_unique<UiDrawSnapshot>();
    snap->total_vtx = dd->TotalVtxCount;
    snap->total_idx = dd->TotalIdxCount;
    snap->display_pos = dd->DisplayPos;
    snap->display_size = dd->DisplaySize;
    snap->framebuffer_scale = dd->FramebufferScale;
    snap->cmd_lists.resize(dd->CmdLists.Size);
    for (int i = 0; i < dd->CmdLists.Size; ++i)
        snap->cmd_lists[i] = dd->CmdLists[i]->CloneOutput();

    std::lock_guard<std::mutex> lk(ui_mutex_);
    ui_pending_ = std::move(snap); // O(1) pointer move, only moment ui_mutex_ is held here
}

// ---- UI intent recording (M3): main-thread-only. Called from WndProc (keyboard, via
// pump_messages()) and from build_top_bar()/build_bottom_bar() (mouse, via ui_tick()) --
// both run on the SAME Python-owned main thread. NEVER called from the present thread. No
// lock: see UiIntents' own header comment. A second record_seek() before Python drains
// simply overwrites the pending target (last-write-wins). build_bottom_bar only records
// on click / when the drag maps to a new frame -- not every hold-still ui_tick.
void Player::record_toggle_play() { ui_intents_.toggle_play = true; }

void Player::record_seek(int64_t f) { ui_intents_.seek = f; }

// M-C2: WM_DROPFILES (a file dropped on the window) and the top-bar "open" button -- see
// UiIntents' own header comment. Both main-thread-only, same as record_seek() above.
void Player::record_open_path(const std::string& path) { ui_intents_.open_path = path; }

void Player::record_export_drop(const std::string& path) { ui_intents_.export_drop_paths.push_back(path); }

void Player::record_open_dialog() { ui_intents_.open_dialog = true; }

void Player::record_compile_engine() { ui_intents_.compile_engine = true; }

// Web-stream popup request (main-thread-only, see UiIntents header comment). The offline
// export entry is a full-screen mode, driven by ui_intents_.export_enter (see build_top_bar /
// build_open_prompt_overlay) + Python's set_export_mode().
void Player::request_stream_popup() { stream_popup_ = true; }

// Python flips this when it starts/stops the server, so the stream popup shows 停止 vs 启动.
// The access URL is passed on start so the popup can render it as a clickable link (open in
// the default browser) instead of a persistent status float.
void Player::set_stream_running(bool running, const std::string& url){
    stream_running_ = running;
    if (!url.empty()) stream_url_ = url;
}

// Seed the stream root/token/no-token fields from last-used settings (Python persists them).
void Player::set_stream_defaults(int port, const std::string& root, bool no_token,
                         const std::string& token){
    if (port > 0) stream_port_edit_ = port;
    if (!root.empty()) {
        std::memcpy(stream_root_buf_, root.c_str(),
            std::min(root.size(), sizeof(stream_root_buf_) - 1));
        stream_root_buf_[std::min(root.size(), sizeof(stream_root_buf_) - 1)] = '\0';
    }
    stream_no_token_edit_ = no_token;
    if (!token.empty()) {
        std::memcpy(stream_token_buf_, token.c_str(),
            std::min(token.size(), sizeof(stream_token_buf_) - 1));
        stream_token_buf_[std::min(token.size(), sizeof(stream_token_buf_) - 1)] = '\0';
    }
}

// Offline-export full-screen mode (Phase 2 extension). Python flips this after tearing down
// playback/streaming (or on exit); the export screen is the sole UI while set.
void Player::set_export_mode(bool on){
    export_mode_ = on;
    if (on) export_clip_edit_init_ = false; // re-seed the clip slider on entry
    // Never carry a drag across screen transitions (a stale id would commit a phantom
    // move on the first frame back).
    export_drag_id_ = -1;
    export_drag_slot_ = -1;
}

// Python pushes the export screen's state every tick (clip_length, global_dir, presets[],
// items[], running, engine_ready). Plain value copies; the Python queue is the source of truth.
void Player::set_export_snapshot(const py::dict& d){
    auto get_str = [&](const py::dict& dd, const char* k, const char* def) -> std::string {
        if (!dd.contains(k)) return std::string(def);
        try { return py::cast<std::string>(dd[k]); } catch (const py::cast_error&) { return std::string(def); }
    };
    auto get_int = [&](const py::dict& dd, const char* k, int def) -> int {
        if (!dd.contains(k)) return def;
        try { return py::cast<int>(dd[k]); } catch (const py::cast_error&) { return def; }
    };
    auto get_bool = [&](const py::dict& dd, const char* k, bool def) -> bool {
        if (!dd.contains(k)) return def;
        try { return py::cast<bool>(dd[k]); } catch (const py::cast_error&) { return def; }
    };

    export_engine_ready_ = get_bool(d, "engine_ready", false);
    export_running_ = get_bool(d, "running", false);
    export_clip_length_ = get_int(d, "clip_length", 120);
    export_global_dir_ = get_str(d, "global_dir", "");

    export_presets_.clear();
    export_default_preset_idx_ = get_int(d, "default_preset_idx", 0);
    if (d.contains("presets")) {
        try {
            py::list presets = d["presets"].cast<py::list>();
            for (py::handle h : presets) {
                py::dict p = h.cast<py::dict>();
                ExportPresetView v;
                v.name = get_str(p, "name", "");
                v.codec = get_str(p, "codec", "hevc");
                v.preset = get_str(p, "preset", "p7");
                v.cq_enabled = get_bool(p, "cq_enabled", true);
                v.cq = get_int(p, "cq", 0);
                v.bitrate_enabled = get_bool(p, "bitrate_enabled", false);
                v.bitrate = get_int(p, "bitrate", 0);
                v.maxrate_enabled = get_bool(p, "maxrate_enabled", false);
                v.maxrate = get_int(p, "maxrate", 0);
                v.audio_copy = get_bool(p, "audio_copy", true);
                v.audio_bitrate = get_int(p, "audio_bitrate", 256);
                v.subtitle = get_bool(p, "subtitle", true);
                v.suffix = get_str(p, "suffix", "_Decensored");
                export_presets_.push_back(std::move(v));
            }
        } catch (const py::error_already_set&) { PyErr_Clear(); }
    }

    export_items_.clear();
    if (d.contains("items")) {
        try {
            py::list items = d["items"].cast<py::list>();
            for (py::handle h : items) {
                py::dict it = h.cast<py::dict>();
                ExportItemView v;
                v.id = get_int(it, "id", 0);
                v.source = get_str(it, "source", "");
                v.out_path = get_str(it, "out_path", "");
                v.out_mode = get_str(it, "out_mode", "auto");
                v.preset_idx = get_int(it, "preset_idx", 0);
                v.status = get_str(it, "status", "pending");
                v.progress = -1.0f;
                if (it.contains("progress") && !it["progress"].is_none()) {
                    try { v.progress = py::cast<float>(it["progress"]); }
                    catch (const py::cast_error&) { v.progress = -1.0f; }
                }
                v.error = get_str(it, "error", "");
                export_items_.push_back(std::move(v));
            }
        } catch (const py::error_already_set&) { PyErr_Clear(); }
    }
}

void Player::request_open_url_popup(){
    // Don't clobber an in-flight URL open (popup is already open in loading state).
    if (open_url_loading_) return;
    open_url_popup_ = true;
    open_url_focus_ = true;
    open_url_show_error_ = false;
    open_url_show_load_error_ = false;
    open_url_close_pending_ = false;
    open_url_buf_[0] = '\0';
}

// Python calls this on the main thread when an async URL open finishes. Loading keeps the
// modal open; success closes it on the next build_open_url_popup frame; failure restores the
// input form with a load-error line (上一步). No-op if the user already cancelled the modal.
void Player::notify_open_url_finished(bool ok){
    if (ok) {
        if (open_url_loading_)
            open_url_close_pending_ = true;
        open_url_loading_ = false;
        open_url_show_error_ = false;
        open_url_show_load_error_ = false;
    } else if (open_url_loading_) {
        open_url_loading_ = false;
        open_url_show_error_ = false;
        open_url_show_load_error_ = true;
    }
}

// Apply Windows monitor DPI to ImGui fonts + spacing and to our self-drawn chrome.
// scale = dpi/96 (1.0 at 100%, 1.5 at 150%, 2.0 at 200%). Fonts: style.FontScaleDpi is the
// ImGui 1.92 factor that multiplies FontSizeBase without rebuilding the atlas (dynamic font
// sizing). Spacing: ScaleAllSizes on a fresh copy of ui_style_base_ so repeated DPI changes
// don't compound. Chrome (bars/buttons/icons): ui_dpi_scale_ is read by ui_s()/top_bar_h()
// and by fit_viewport()/resize_window_for_video(). Called from ui_init() and WM_DPICHANGED
// (public so WndProc can route the message here, same as on_resize/ui_tick).
void Player::apply_ui_dpi(float scale){
    // Debug/verification hook: SUMU_UI_SCALE=1.5 forces a monitor scale so high-DPI layout
    // bugs are reproducible on a 96-DPI display (scripts/shot_export_ui.py uses this).
    static const float forced = [](){
        const char* e = getenv("SUMU_UI_SCALE");
        return e ? (float)atof(e) : 0.0f;
    }();
    if (forced > 0.0f) scale = forced;
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 4.0f) scale = 4.0f;
    if (std::fabs(scale - ui_dpi_scale_) < 0.001f && ui_ready_) return;

    ui_dpi_scale_ = scale;

    ImGuiStyle& style = ImGui::GetStyle();
    style = ui_style_base_;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    fprintf(stderr, "[sumu] UI DPI scale=%.2f (font + chrome)\n", scale);
}

// ---- UI config/intents channel (M3): main-thread-exclusive, Python-driven ---------------
//
// Python's main-thread loop calls these once per its own tick, bracketing ui_tick():
// set_ui_config() (refresh the settings panel's read-only mirror of the ACTUAL committed
// config -- Python owns the real SchedulerConfig, this is display-only) -> ui_tick()
// (pump input via WndProc/pump_messages(), build UI, record intents) -> take_ui_intents()
// (drain + execute). All three run on Python's single main thread, never the present
// thread, so ui_intents_/ui_cfg_* need no lock.
void Player::set_ui_config(int clip_length, int max_regions, float cold_start_s, int target_fps,
                   float ai_restore_fps, int lead){
    ui_cfg_clip_length_ = clip_length;
    ui_cfg_max_regions_ = max_regions;
    ui_cfg_cold_start_s_ = cold_start_s;
    ui_cfg_target_fps_ = target_fps;
    ui_cfg_ai_restore_fps_ = ai_restore_fps; // Python Scheduler net BasicVSR fps; <0 = unknown
    ui_cfg_lead_ = lead;
}

// Model-warmup-in-background status line (left-bottom float, build_status_float()):
// main-thread-owned, same discipline as ui_cfg_* above -- Python pushes a fresh string every
// tick (empty = hide), no lock needed since only the main thread ever touches it.
void Player::set_status_text(const std::string& s) { status_text_ = s; }

// First-screen TRT-compile prompt state, pushed once per Python tick (same main-thread-only
// publish pattern as set_status_text). state: 0 hidden / 1 idle (offer compile) / 2 running
// (progress bar) / 3 failed (offer retry). progress in [0,1]; text is the line shown under
// the button (idle/failed) or beside the bar (running). Rendered by build_open_prompt_overlay.
void Player::set_compile_ui(int state, float progress, const std::string& text){
    compile_ui_state_ = state;
    compile_ui_progress_ = progress;
    compile_ui_text_ = text;
}

// UI string table for ImGui labels / tooltips / open-file filter. Python owns catalogs
// (sumu.i18n) and pushes the resolved table once at startup (and on language change).
// Main-thread only; missing keys leave the previous/default value in place.
void Player::set_ui_strings(const py::dict& d){
    auto take = [&](const char* key, std::string& dest) {
        if (!d.contains(key)) return;
        try {
            std::string v = py::cast<std::string>(d[key]);
            if (!v.empty()) dest = std::move(v);
        } catch (const py::cast_error&) {
            // ignore non-str values -- keep prior string
        }
    };
    take("splash_loading", ui_str_.splash_loading);
    take("open_prompt", ui_str_.open_prompt);
    take("open_file", ui_str_.open_file);
    take("open_url", ui_str_.open_url);
    take("open_url_title", ui_str_.open_url_title);
    take("open_url_hint", ui_str_.open_url_hint);
    take("open_url_ok", ui_str_.open_url_ok);
    take("open_url_cancel", ui_str_.open_url_cancel);
    take("open_url_invalid", ui_str_.open_url_invalid);
    take("open_url_loading", ui_str_.open_url_loading);
    take("open_url_load_failed", ui_str_.open_url_load_failed);
    take("compile_retry", ui_str_.compile_retry);
    take("compile_engine", ui_str_.compile_engine);
    take("settings_title", ui_str_.settings_title);
    take("lead_label", ui_str_.lead_label);
    take("lead_tooltip", ui_str_.lead_tooltip);
    take("clip_length_label", ui_str_.clip_length_label);
    take("clip_length_tooltip", ui_str_.clip_length_tooltip);
    take("max_regions_label", ui_str_.max_regions_label);
    take("max_regions_tooltip", ui_str_.max_regions_tooltip);
    take("cold_start_label", ui_str_.cold_start_label);
    take("cold_start_tooltip", ui_str_.cold_start_tooltip);
    take("target_fps_label", ui_str_.target_fps_label);
    take("target_fps_original", ui_str_.target_fps_original);
    take("target_fps_tooltip", ui_str_.target_fps_tooltip);
    take("diagnostics_title", ui_str_.diagnostics_title);
    take("ai_speed", ui_str_.ai_speed);
    take("ai_speed_unknown", ui_str_.ai_speed_unknown);
    take("dialog_video_files", ui_str_.dialog_video_files);
    take("dialog_all_files", ui_str_.dialog_all_files);
    take("stream_server", ui_str_.stream_server);
    take("export_video", ui_str_.export_video);
    take("stream_title", ui_str_.stream_title);
    take("stream_port_label", ui_str_.stream_port_label);
    take("stream_root_label", ui_str_.stream_root_label);
    take("stream_pick", ui_str_.stream_pick);
    take("stream_start", ui_str_.stream_start);
    take("stream_stop", ui_str_.stream_stop);
    take("stream_url_label", ui_str_.stream_url_label);
    take("stream_no_token", ui_str_.stream_no_token);
    take("stream_token_label", ui_str_.stream_token_label);
    take("stream_token_hint", ui_str_.stream_token_hint);
    take("export_title", ui_str_.export_title);
    take("export_start", ui_str_.export_start);
    take("cancel", ui_str_.cancel);
    take("export_section_ai", ui_str_.export_section_ai);
    take("export_clip_length_label", ui_str_.export_clip_length_label);
    take("export_section_path", ui_str_.export_section_path);
    take("export_global_dir_label", ui_str_.export_global_dir_label);
    take("export_pick_dir", ui_str_.export_pick_dir);
    take("export_section_queue", ui_str_.export_section_queue);
    take("export_add_files", ui_str_.export_add_files);
    take("export_out_auto", ui_str_.export_out_auto);
    take("export_out_global", ui_str_.export_out_global);
    take("export_out_custom", ui_str_.export_out_custom);
    take("export_remove", ui_str_.export_remove);
    take("export_up", ui_str_.export_up);
    take("export_down", ui_str_.export_down);
    take("export_empty", ui_str_.export_empty);
    take("export_not_ready", ui_str_.export_not_ready);
    take("export_presets_title", ui_str_.export_presets_title);
    take("export_preset_new", ui_str_.export_preset_new);
    take("export_preset_delete", ui_str_.export_preset_delete);
    take("export_preset_name_label", ui_str_.export_preset_name_label);
    take("export_preset_codec_label", ui_str_.export_preset_codec_label);
    take("export_preset_cq_label", ui_str_.export_preset_cq_label);
    take("export_preset_bitrate_label", ui_str_.export_preset_bitrate_label);
    take("export_preset_maxrate_label", ui_str_.export_preset_maxrate_label);
    take("export_preset_quality_label", ui_str_.export_preset_quality_label);
    take("export_preset_audio_label", ui_str_.export_preset_audio_label);
    take("export_preset_audio_copy", ui_str_.export_preset_audio_copy);
    take("export_preset_audio_encode", ui_str_.export_preset_audio_encode);
    take("export_preset_subtitle_label", ui_str_.export_preset_subtitle_label);
    take("export_preset_suffix_label", ui_str_.export_preset_suffix_label);
    take("export_preset_default", ui_str_.export_preset_default);
    take("export_preset_save", ui_str_.export_preset_save);
    take("export_status_pending", ui_str_.export_status_pending);
    take("export_status_running", ui_str_.export_status_running);
    take("export_status_done", ui_str_.export_status_done);
    take("export_status_failed", ui_str_.export_status_failed);
    take("export_status_cancelled", ui_str_.export_status_cancelled);
    take("export_status_interrupted", ui_str_.export_status_interrupted);
}

py::dict Player::take_ui_intents(){
    py::dict d;
    d["seek"] = ui_intents_.seek.has_value() ? py::cast(*ui_intents_.seek) : py::none();
    d["toggle_play"] = ui_intents_.toggle_play;
    d["clip_length"] = ui_intents_.clip_length.has_value() ? py::cast(*ui_intents_.clip_length) : py::none();
    d["max_regions"] = ui_intents_.max_regions.has_value() ? py::cast(*ui_intents_.max_regions) : py::none();
    d["cold_start_s"] = ui_intents_.cold_start_s.has_value() ? py::cast(*ui_intents_.cold_start_s) : py::none();
    d["lead"] = ui_intents_.lead.has_value() ? py::cast(*ui_intents_.lead) : py::none();
    d["target_fps"] = ui_intents_.target_fps.has_value() ? py::cast(*ui_intents_.target_fps) : py::none();
    d["open_path"] = ui_intents_.open_path; // M-C2: "" == no drop pending
    d["open_dialog"] = ui_intents_.open_dialog; // M-C2: top-bar "open" button clicked
    d["compile_engine"] = ui_intents_.compile_engine; // first-screen TRT compile / retry click
    d["stream_start"] = ui_intents_.stream_start; // web-stream server: start with port/root
    d["stream_port"] = ui_intents_.stream_port;
    d["stream_root"] = ui_intents_.stream_root;
    d["stream_no_token"] = ui_intents_.stream_no_token;
    d["stream_token"] = ui_intents_.stream_token;
    d["stream_stop"] = ui_intents_.stream_stop; // web-stream server: stop
    d["export_enter"] = ui_intents_.export_enter;
    d["export_exit"] = ui_intents_.export_exit;
    d["export_add_files"] = ui_intents_.export_add_files;
    d["export_start"] = ui_intents_.export_start;
    d["export_pick_global"] = ui_intents_.export_pick_global;
    d["export_pick_custom"] = ui_intents_.export_pick_custom;
    d["export_remove"] = ui_intents_.export_remove;
    d["export_cancel"] = ui_intents_.export_cancel;
    d["export_move_id"] = ui_intents_.export_move_id;
    d["export_move_to"] = ui_intents_.export_move_to;
    d["export_item_preset_id"] = ui_intents_.export_item_preset_id;
    d["export_item_preset_idx"] = ui_intents_.export_item_preset_idx;
    d["export_item_out_id"] = ui_intents_.export_item_out_id;
    d["export_item_out_mode"] = ui_intents_.export_item_out_mode;
    d["export_clip_length"] = ui_intents_.export_clip_length;
    d["export_preset_save"] = ui_intents_.export_preset_save;
    d["export_preset_delete"] = ui_intents_.export_preset_delete;
    d["export_preset_edit_idx"] = ui_intents_.export_preset_edit_idx;
    d["export_preset_name"] = ui_intents_.export_preset_name;
    d["export_preset_codec"] = ui_intents_.export_preset_codec;
    d["export_preset_cq_enabled"] = ui_intents_.export_preset_cq_enabled;
    d["export_preset_cq"] = ui_intents_.export_preset_cq;
    d["export_preset_bitrate_enabled"] = ui_intents_.export_preset_bitrate_enabled;
    d["export_preset_bitrate"] = ui_intents_.export_preset_bitrate;
    d["export_preset_maxrate_enabled"] = ui_intents_.export_preset_maxrate_enabled;
    d["export_preset_maxrate"] = ui_intents_.export_preset_maxrate;
    d["export_preset_quality"] = ui_intents_.export_preset_quality;
    d["export_preset_audio_copy"] = ui_intents_.export_preset_audio_copy;
    d["export_preset_audio_bitrate"] = ui_intents_.export_preset_audio_bitrate;
    d["export_preset_subtitle"] = ui_intents_.export_preset_subtitle;
    d["export_preset_suffix"] = ui_intents_.export_preset_suffix;
    d["export_set_default"] = ui_intents_.export_set_default;
    d["export_drop_paths"] = ui_intents_.export_drop_paths;
    ui_intents_ = UiIntents{}; // drain
    return d;
}

