// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "player.h"
#include "browser_view.h" // BrowserView (page view state, cookie lookup, media handover)

#include <cctype>
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
    // Popup layers (popup_layer.h): everything that must appear ABOVE the WebView2 page view --
    // the address bar's favorites dropdown, tooltips, the settings panel and the topmost open
    // ImGui popup. Created before the present thread starts.
    fav_popup_ = std::make_unique<PopupLayer>();
    fav_popup_->attach(hwnd_);
    tooltip_popup_ = std::make_unique<PopupLayer>();
    tooltip_popup_->attach(hwnd_);
    settings_popup_ = std::make_unique<PopupLayer>();
    settings_popup_->attach(hwnd_);
    menu_popup_ = std::make_unique<PopupLayer>();
    menu_popup_->attach(hwnd_);
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

void Player::open(const std::string& video_path, const std::string& headers,
                  const std::string& cookies){
    if (opened_) throw std::runtime_error("Player.open() called twice on the same Player");
    open_session(video_path, headers, cookies);
}

// M-C1: the actual session-layer setup, split out of open() so open() can stay a thin,
// signature-stable compatibility shell (run_player.py/stress_seek_ai.py call it directly).
// Does NOT start present_thread_ (the constructor's job now, see above) -- only
// decode_thread_/audio_thread_, which are session-scoped. This milestone (M-C1) never
// calls this a second time on the same Player (open()'s opened_ guard above still forbids
// it) -- runtime reopen is M-C2's job, layered on top of this split without needing to
// revisit it.
void Player::open_session(const std::string& video_path, const std::string& headers,
                          const std::string& cookies){
    // open()/reopen() may run off the Python main thread (async network open). Make cu_ctx_
    // current on whichever thread is building the session -- same pattern as push_ai_frame().
    if (cu_ctx_)
        check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent (open_session)");

    video_path_ = video_path; // UI displays basename_of(video_path_) in the top bar

    std::string decode_err;
    // headers/cookies: the browser page's captured request context (empty for local files and
    // plain URL opens). They only reach FFmpeg's HTTP protocol options -- never the log.
    if (!decoder_.open(video_path, device_.Get(), decode_err, headers, cookies))
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
    pt_end_tag_.assign(ring_capacity_, -1);
    network_recovery_attempts_.store(0, std::memory_order_relaxed);
    network_recovery_successes_.store(0, std::memory_order_relaxed);
    network_recovering_.store(false, std::memory_order_relaxed);
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

    // H1 修复：decode 线程已启动、CUDA 注册与 ring 资源已建 —— 从这里到函数末尾的任何
    // throw（起始缓冲等待超时，或该区域内未来新增的失败点）都不能再裸抛。裸抛会留下孤儿
    // decode/scrub 线程继续对 fmt_ctx_ 跑 av_read_frame：Python 侧 open 失败后用户再选文件
    // → 再次 open_session() → decoder_.open() 内部 close() 释放 fmt_ctx_，旧线程仍在用 →
    // use-after-free；且对已 joinable 的 std::thread 再赋值会直接 std::terminate 崩进程。
    // 统一走 close_session() 拆除：它对未启动的线程（joinable 检查）、未建的资源（nullptr
    // 检查）、未分配的 audio_codec_ctx_ 全部有防护，「本次 open 建到哪算哪」的部分状态也能
    // 安全清理；它还会把 session_stop_ 复位、session_active_ 保持 false，下一次
    // open_session() 才能干净启动（reopen() 的 catch 语义不变：opened_=false 后可再 open）。
    try {
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
            // 短片或截断文件可能不足起始缓冲阈值；解码已结束时保留现有画面，
            // 一帧未出才算打开失败，不等待容器声明的帧数或完整超时。
            if (decoder_.at_eof() || decoder_.failed()) {
                if (pt_high_water_.load(std::memory_order_relaxed) < 0)
                    throw std::runtime_error("no playable video frames (incomplete or damaged file)");
                break;
            }
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
    } catch (...) {
        close_session(); // 见上方 H1 注释；自身全防护，重复调用也安全
        throw;
    }
}

// M-C1: joins/tears down only the session-scoped state (decode/audio threads, the D3D11
// session textures/SRVs/tags, the session's CUDA registrations, the audio codec ctx, the
// decoder itself) -- leaves the device layer (device_/context_/swapchain_/shader pipeline/
// ImGui/cu_ctx_) fully intact, since present_thread_ keeps running across this (drawing the
// splash again immediately after). Callers: close() (with present_thread_ already joined),
// reopen()/close_current_session() (after the present-detach handshake), and open_session()'s
// own failure catch (H1: threads already started, resources partially built -- every teardown
// step below is guarded by joinable()/nullptr checks, so it doubles as the "clean up however
// far this open got" path; see open_session()'s H1 comment).
void Player::close_session(){
    if (cu_ctx_)
        check_cu(cuCtxSetCurrent(cu_ctx_), "cuCtxSetCurrent (close_session)");

    session_stop_.store(true, std::memory_order_relaxed);
    decoder_.request_interrupt();
    scrub_decoder_.request_interrupt();
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

        // H2 修复：UI 快照里可能烘着缩略图 SRV 的裸指针（hover 进度条时 build_bottom_bar()
        // AddImage 进 draw list，见 player_ui_bars.cpp）—— 包括还没被 present 取走的
        // ui_pending_ 和 present 每 tick 都在渲染的 ui_active_。本函数马上要在下面释放
        // scrub 纹理/SRV，而 reopen 期间主线程阻塞在 open_session()（网络源最长 ~30s）、
        // 没有新 ui_tick 发布新快照，present 的 splash 分支会继续渲染 ui_active_ 旧快照
        // → 解引用已释放的 SRV。present 渲染快照必持 d3d_mutex_（draw_splash()/
        // draw_and_present() 都在锁内调 ui_render_drawdata()），所以在此临界区内清空两个
        // 快照后与 present 严格互斥：之后的 splash tick 走 ui_render_drawdata() 的
        // nothing-published 分支不画 UI（合法状态，代价仅是 reopen 期间短暂没有窗口
        // chrome，open_session 返回后 Python 的 ui_tick 立刻发布新快照）。锁序
        // d3d → ui 与 ui_render_drawdata() 自身一致，无新锁序。
        {
            std::lock_guard<std::mutex> ui_lock(ui_mutex_);
            ui_pending_.reset();
            ui_active_.reset();
        }

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
            pt_end_tag_.clear();
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

        // M4 scrub thumbnail resources. scrub_thread_ is already joined (above), and any UI
        // snapshot that could still reference a thumbnail SRV has been cleared at the top of
        // this critical section (H2, see there) -- safe to release the NV12 blit source and
        // the thumb pool. scrub_cache_ is also read by get_thumbnail() on the main thread; the
        // same main thread runs close_session(), so there is no concurrent reader here, but
        // take scrub_cache_mutex_ anyway to keep every scrub_cache_ access uniformly guarded.
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

    // Network-recovery flags are per session: a torn-down session must not report itself as
    // mid-recovery (decode_loop() is joined above, so nothing else can clear this afterwards).
    network_recovering_.store(false, std::memory_order_relaxed);

    decoder_.close();
    scrub_decoder_.close();

    session_active_.store(false, std::memory_order_relaxed);
    // The page's Native content is gone with the session (explicit teardown, export entry, or a
    // failed reopen): fall back to whatever else the page has (Web / Home), never a stale video.
    set_browser_native(false);
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
int64_t Player::reopen(const std::string& video_path, const std::string& headers,
                       const std::string& cookies){
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
        open_session(video_path, headers, cookies);
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
    // M1 修复：先翻 session_active_ 再拆。reopen 路径有这个防护，close 路径此前没有 ——
    // Python 侧 Scheduler.stop() 的 join 超时后 AI 线程可能还活着，在 close_session()
    // 拆除过程中穿过防护索引已 .clear() 的 vector（UB）。push_ai_frame()/
    // get_cuda_nv12_by_frame() 的 acquire 检查读到 false 即 no-op/miss 退出（退出路径
    // 不碰任何会话资源），与 reopen() 的 release 同款配对。
    session_active_.store(false, std::memory_order_release);
    stop_.store(true, std::memory_order_relaxed);
    if (present_thread_.joinable()) present_thread_.join(); // barrier: present is now fully
                                                              // stopped before anything below
                                                              // touches D3D/CUDA state.
    close_session(); // safe now that present isn't running; also safe to call even if
                      // open_session() was never called (every member it touches is still
                      // at its default-empty/null state).

    // Present thread (the only ImGui-touching thread) has now joined -- safe to tear down
    // ImGui's device objects/context before DestroyWindow below.
    // Popup windows first: their snapshots are freed through ImGui's allocator (so before
    // ui_shutdown) and their swapchains belong to the joined present thread.
    if (fav_popup_) {
        fav_popup_->release();
        fav_popup_.reset();
    }
    if (tooltip_popup_) {
        tooltip_popup_->release();
        tooltip_popup_.reset();
    }
    if (settings_popup_) {
        settings_popup_->release();
        settings_popup_.reset();
    }
    if (menu_popup_) {
        menu_popup_->release();
        menu_popup_.reset();
    }
    // The page view is closed FIRST: its child window and WebView2 process must be gone before
    // the parent HWND is destroyed, and its async callbacks are cancelled by the same call.
    if (browser_) {
        browser_->shutdown();
        browser_.reset();
    }
    browser_page_ = false;
    browser_web_ = false;

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
    // Deliberately NO media-extension filter: whether a file is playable is FFmpeg's container
    // probe, not a filename list (that list would silently hide FLV/TS/anything new).
    std::wstring filter;
    auto append_filter_pair = [&](const std::string& label_utf8, const wchar_t* patterns) {
        std::wstring label = utf8_to_wide(label_utf8);
        filter.append(label);
        filter.push_back(L'\0');
        filter.append(patterns);
        filter.push_back(L'\0');
    };
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
    poll_page_view_border();
}

// ---- page-view edge drags ------------------------------------------------------------------
// Forensics (measured on this machine, see docs/native_core.md): the WebView2 controller's window
// chain is
//   Sumu_Player_WndClass -> Chrome_WidgetWin_0            (OUR process: subclassable)
//                        -> Chrome_WidgetWin_1           (WebView2 browser process)
//                        -> Chrome_RenderWidgetHostHWND  (WebView2 browser process)
//                        -> Intermediate D3D Window      (WebView2 GPU process)
// The three deepest windows belong to other processes, so SetWindowSubclass (see
// browser_view.cpp's child_subclass_proc) can only cover the outermost one -- and the OS
// hit-tests the DEEPEST window, so a mouse-down over the page view never reaches this window's
// WM_NCHITTEST and the resize bands are dead. The OS sizing loop cannot be borrowed either:
// the click (and the mouse capture) belongs to the page view's process.
//
// So the drag is driven from here, as a per-tick state machine: while the cursor sits in one of
// this window's resize bands AND the button is down over the page view, each tick moves/resizes
// the window with SetWindowPos (which drives WM_SIZE -> on_resize() + ui_tick(), so the swapchain
// and the page view follow live) until the button is released. Nothing is added around the page
// (no padding, no dead strip) and nothing is intercepted while the cursor is elsewhere.
void Player::poll_page_view_border(){
    const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool usable = hwnd_ && browser_ && browser_->visible() &&
        !fullscreen_.load(std::memory_order_relaxed) && !IsZoomed(hwnd_);

    if (page_resize_active_) {
        if (!down || !usable) { // released (or the page is gone): the drag is over
            page_resize_active_ = false;
            page_resize_armed_ = false;
            return;
        }
        POINT cur{};
        if (!GetCursorPos(&cur)) return;
        const int dx = cur.x - page_resize_start_cursor_.x;
        const int dy = cur.y - page_resize_start_cursor_.y;
        set_resize_cursor(page_resize_hit_);
        if (std::abs(dx) < 2 && std::abs(dy) < 2) return; // a plain click must not nudge
        RECT r = page_resize_start_rect_;
        const LRESULT hit = page_resize_hit_;
        if (hit == HTLEFT || hit == HTTOPLEFT || hit == HTBOTTOMLEFT) r.left += dx;
        if (hit == HTRIGHT || hit == HTTOPRIGHT || hit == HTBOTTOMRIGHT) r.right += dx;
        if (hit == HTTOP || hit == HTTOPLEFT || hit == HTTOPRIGHT) r.top += dy;
        if (hit == HTBOTTOM || hit == HTBOTTOMLEFT || hit == HTBOTTOMRIGHT) r.bottom += dy;
        const UINT dpi = GetDpiForWindow(hwnd_);
        const int min_w = MulDiv(240, dpi, 96);
        const int min_h = MulDiv(135, dpi, 96);
        if (r.right - r.left < min_w) {
            if (hit == HTLEFT || hit == HTTOPLEFT || hit == HTBOTTOMLEFT) r.left = r.right - min_w;
            else r.right = r.left + min_w;
        }
        if (r.bottom - r.top < min_h) {
            if (hit == HTTOP || hit == HTTOPLEFT || hit == HTTOPRIGHT) r.top = r.bottom - min_h;
            else r.bottom = r.top + min_h;
        }
        if (r.left != page_resize_applied_.left || r.top != page_resize_applied_.top ||
            r.right != page_resize_applied_.right || r.bottom != page_resize_applied_.bottom) {
            SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            page_resize_applied_ = r;
        }
        return;
    }

    if (!down) {
        // Hover arming: the arm flag is only ever (re)computed while the button is UP, so a
        // press that began inside the page (text selection, dragging a slider) can never arm --
        // moving to the edge with the button held stays a page drag, not a window resize.
        const LRESULT hover = usable ? page_view_border_at_cursor() : HTNOWHERE;
        page_resize_armed_ = hover != HTNOWHERE;
        if (page_resize_armed_) set_resize_cursor(hover);
        return;
    }
    // Button is down: a drag may only start from a press that began in the band.
    if (!page_resize_armed_ || !usable) {
        page_resize_armed_ = false;
        return;
    }
    const LRESULT hit = page_view_border_at_cursor();
    if (hit == HTNOWHERE) {
        page_resize_armed_ = false;
        return;
    }
    POINT screen{};
    if (!GetCursorPos(&screen)) return;
    page_resize_armed_ = false;
    page_resize_active_ = true;
    page_resize_hit_ = hit;
    page_resize_start_cursor_ = screen;
    if (!GetWindowRect(hwnd_, &page_resize_start_rect_)) {
        page_resize_active_ = false;
        return;
    }
    page_resize_applied_ = page_resize_start_rect_;
    set_resize_cursor(hit);
}

// Which resize band (if any) the cursor is in -- but only where the page view covers the
// client area: everywhere else the window's own WM_NCHITTEST already does the job.
LRESULT Player::page_view_border_at_cursor() const {
    if (!hwnd_ || !browser_ || !browser_->visible()) return HTNOWHERE;
    POINT screen{};
    if (!GetCursorPos(&screen)) return HTNOWHERE;
    POINT client = screen;
    if (!ScreenToClient(hwnd_, &client)) return HTNOWHERE;
    RECT rc{};
    if (!GetClientRect(hwnd_, &rc)) return HTNOWHERE;
    int bounds[4] = { 0, 0, 0, 0 };
    browser_->bounds(bounds);
    if (client.x < bounds[0] || client.x >= bounds[0] + bounds[2] ||
        client.y < bounds[1] || client.y >= bounds[1] + bounds[3])
        return HTNOWHERE;

    const int margin = MulDiv(8, static_cast<int>(GetDpiForWindow(hwnd_)), 96);
    const bool left = client.x < margin;
    const bool right = client.x >= rc.right - margin;
    const bool bottom = client.y >= rc.bottom - margin;
    // The top band is always above the page view (address bar), so it never needs the rescue.
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (bottom) return HTBOTTOM;
    return HTNOWHERE;
}

// The page view's own window proc keeps resetting the cursor to an arrow, so the resize cursor
// has to be re-asserted every tick while hovering/ dragging a band. (Wide resource IDs: this
// translation unit is ANSI, so the IDC_* macros would be narrow.)
void Player::set_resize_cursor(LRESULT hit){
    int id = 32645; // IDC_SIZENS
    switch (hit) {
    case HTLEFT: case HTRIGHT: id = 32644; break;             // IDC_SIZEWE
    case HTTOPLEFT: case HTBOTTOMRIGHT: id = 32642; break;    // IDC_SIZENWSE
    case HTTOPRIGHT: case HTBOTTOMLEFT: id = 32643; break;    // IDC_SIZENESW
    default: break;
    }
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(id)));
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
    // Same strip math as fit_viewport(): only the title bar strip is reserved (the URL page's
    // address bar is floating chrome).
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
    // ---- popup layer routing ---------------------------------------------------------------
    // The page view (WebView2) is a real child HWND, so it composites above everything ImGui
    // draws into the main swapchain: an address-bar dropdown, a tooltip, the settings panel or an
    // open combo list that reaches into the page area would be invisible. While the live page view
    // is actually up, those windows are routed OUT of this frame into their own owned top-level
    // windows (popup_layer.h). In every other state (Home, Native content, export, page view
    // stepped aside, window minimized or the app in the background) routing is off and they draw
    // in the main swapchain exactly as before -- zero behavior change outside the live web view.
    const bool route_popups = ui_ready_ && browser_ && browser_->visible() && !export_mode_ &&
        app_activated_ && hwnd_ && !IsIconic(hwnd_);
    if (fav_popup_) fav_popup_->route(route_popups ? dd : nullptr, "##sumu_favorites");
    if (tooltip_popup_) tooltip_popup_->route(route_popups ? dd : nullptr,
        route_popups ? PopupLayer::active_tooltip_name() : nullptr);
    if (settings_popup_) settings_popup_->route(route_popups ? dd : nullptr, "##sumu_settings");
    if (menu_popup_) menu_popup_->route(route_popups ? dd : nullptr,
        route_popups ? PopupLayer::active_popup_name() : nullptr);
    auto snap = std::make_unique<UiDrawSnapshot>();
    snap->total_vtx = dd->TotalVtxCount;
    snap->total_idx = dd->TotalIdxCount;
    snap->display_pos = dd->DisplayPos;
    snap->display_size = dd->DisplaySize;
    snap->framebuffer_scale = dd->FramebufferScale;
    snap->cmd_lists.resize(dd->CmdLists.Size);
    for (int i = 0; i < dd->CmdLists.Size; ++i)
        snap->cmd_lists[i] = dd->CmdLists[i]->CloneOutput();

    {
        std::lock_guard<std::mutex> lk(ui_mutex_);
        ui_pending_ = std::move(snap); // O(1) pointer move, only moment ui_mutex_ is held here
    }

    // Apply the routing decision to the real windows: position them, and show each one only once
    // the present thread has drawn a frame at its current size.
    if (fav_popup_) fav_popup_->sync();
    if (tooltip_popup_) tooltip_popup_->sync();
    if (settings_popup_) settings_popup_->sync();
    if (menu_popup_) menu_popup_->sync();
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

// park_on_close (see UiIntents::close_request): WM_CLOSE/ESC while parking is enabled.
// Main-thread-only, same as record_seek() above.
void Player::record_close_request() { ui_intents_.close_request = true; }

void Player::hide_window(){
    // Just hide. Do NOT idle present_loop() here -- see park_on_close_'s comment in
    // player.h for the device-removed/invalid-handle failure that caused.
    if (browser_) browser_->set_visible(false); // no page view floating over a hidden window
    if (fav_popup_) fav_popup_->hide();         // nor a popup floating over a hidden window
    if (tooltip_popup_) tooltip_popup_->hide();
    if (settings_popup_) settings_popup_->hide();
    if (menu_popup_) menu_popup_->hide();
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void Player::show_window(){
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        // Best-effort foreground: the request usually comes from a freshly-launched second
        // instance forwarding its video over the single-instance pipe, so foreground rights
        // may be denied -- the taskbar button just flashes then, which is acceptable.
        SetForegroundWindow(hwnd_);
    }
}

// WM_ACTIVATEAPP. Only the popups care: while the app is in the background nothing may float
// above the window the user switched to. ui_tick() also stops routing while inactive, so a
// hidden layer stays hidden until the app comes back.
void Player::on_app_activated(bool active){
    app_activated_ = active;
    if (active) return;
    if (fav_popup_) fav_popup_->hide();
    if (tooltip_popup_) tooltip_popup_->hide();
    if (settings_popup_) settings_popup_->hide();
    if (menu_popup_) menu_popup_->hide();
}

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
    // The export screen owns the whole window while it is up: the page view (a real child
    // window) must step aside -- hidden and muted -- or it would paint over the screen and keep
    // its audio running. Leaving export mode needs no counterpart: the next
    // build_browser_page() tick re-applies the page's own visibility.
    if (on && browser_) browser_->set_visible(false);
    if (on) stream_popup_open_ = false; // the modal cannot survive the screen swap
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
                v.vbr_enabled = get_bool(p, "vbr_enabled", false);
                v.bitrate = get_int(p, "bitrate", 2000);
                v.maxrate = get_int(p, "maxrate", 2500);
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

// ---- embedded browser page (URL bar + WebView2) ------------------------------------------
//
// request_open_url_popup() keeps the old name because it is the one entry point both the
// first-screen button and the top bar's link icon already call; it now ENTERS the browser
// page instead of opening a modal. The page's own state lives in player.h's browser_page_*
// block; this section owns the transitions, the layout lives in build_browser_page()
// (player_ui_overlays.cpp).
void Player::request_open_url_popup(){
    if (open_url_loading_) return; // an open is already in flight from this page
    if (browser_page_) {           // already there: just put the caret in the address bar
        browser_addr_focus_ = true;
        return;
    }
    browser_page_ = true;
    browser_failed_ = false;
    browser_pending_nav_.clear();
    browser_page_url_seen_.clear();
    browser_addr_submitted_.clear();
    browser_addr_focus_ = false;
    browser_home_focus_ = false;
    browser_addr_select_all_ = false;
    browser_home_select_all_ = false;
    browser_cookie_pending_ = false;
    browser_auto_headers_.clear();
    browser_auto_nav_serial_ = 0;
    if (browser_) {
        // Nothing the page view recorded while the page was closed may be taken over on entry.
        browser_->cancel_cookies();
        browser_->cancel_media_navigation();
    }
    open_url_show_error_ = false;
    open_url_show_load_error_ = false;
    open_cancel_requested_.store(false, std::memory_order_relaxed);
    browser_addr_buf_[0] = '\0';
    browser_home_buf_[0] = '\0';
    ensure_browser();
    // Continuity: a page already loaded (the user closed the page earlier) comes back as-is;
    // otherwise the page opens on its first-screen form. A live session is NOT adopted as the
    // page content on entry -- entering the page always means "the page, not the video", which
    // is why browser_enter asks Python to pause.
    browser_web_ = browser_ && browser_->ready() && !browser_->page_url().empty();
    set_browser_native(false);
    if (!browser_web_) browser_home_focus_ = true;
    ui_intents_.browser_enter = true; // Python pauses the old session and remembers it
}

void Player::exit_browser_page(){
    if (!browser_page_) return;
    browser_page_ = false;
    browser_web_ = false;
    set_browser_native(false);
    browser_pending_nav_.clear();
    browser_page_url_seen_.clear();
    browser_addr_submitted_.clear();
    browser_cookie_pending_ = false;
    browser_cookie_url_.clear();
    browser_auto_headers_.clear();
    browser_auto_nav_serial_ = 0;
    browser_addr_focus_ = false;
    browser_home_focus_ = false;
    open_url_show_error_ = false;
    open_url_show_load_error_ = false;
    fav_dropdown_open_ = false; // leaving the page closes the favorites dropdown
    if (browser_) {
        browser_->set_visible(false);
        browser_->cancel_cookies(); // a lookup for a page that is gone must not start an open
        browser_->cancel_media_navigation(); // nor may the page hand a video over after leaving it
        std::string ignored;
        browser_->take_popup_navigation(ignored); // nor navigate to a window request of the old page
    }
    ui_intents_.browser_exit = true; // Python restores playback if it had paused it
}

// Native content: the playing session is what the page shows. The address bar turns into
// floating chrome in this state (see build_browser_page()), so no layout space is reserved for
// it anywhere -- the video viewport is identical whether the bar is revealed or hidden.
void Player::set_browser_native(bool on){
    browser_native_ = on;
    // The address bar is floating chrome in this state: it reserves no layout space and the
    // video viewport is unchanged by showing/hiding it, so nothing here touches the window or
    // the viewport.
}

// Native -> Web: the page view takes the content back and the session must stop rendering its
// clock behind it. Reusing browser_enter is deliberate -- Python pauses whatever is playing and
// remembers it (browser_resume is cleared on every successful native open, so the *new* video
// is what gets recorded here).
void Player::browser_enter_web(){
    if (!browser_native_) return;
    set_browser_native(false);
    browser_page_url_seen_.clear(); // let the address bar resync to the page the view is on
    if (browser_) browser_->cancel_cookies();
    ui_intents_.browser_enter = true;
}

bool Player::browser_overlay_open() const {
    // Only the web-stream modal still replaces the page view with the player background; the
    // settings panel floats above the live page in its own popup layer (see build_browser_page()).
    return stream_popup_open_;
}

// The settings panel is drawn ON the page, not instead of it: it is routed into its own owned
// top-level window (popup_layer.h) while the live page view is up, so the page keeps running,
// clicking and sounding underneath. A layer that could not get its window/swapchain is the one
// exception -- then the page view has to step aside or the panel would be buried under it.
bool Player::settings_popup_available() const {
    return settings_popup_ && !settings_popup_->unavailable();
}

// One reveal rule for the floating top chrome (title bar in fullscreen, address bar whenever the
// page shows the native video): the cursor in the top strip -- the title bar itself plus a band of
// kChromeRevealFraction client heights starting at the bar's bottom edge. The bar height is the
// fixed DPI height, never the current visibility, so revealing/hiding cannot shift the boundary.
// Two copies of this test used to drift apart; keep it here.
bool Player::mouse_in_top_reveal_zone() const {
    const ImGuiIO& io = ImGui::GetIO();
    const float zone_y = top_bar_h() + io.DisplaySize.y * ui::theme::kChromeRevealFraction;
    return io.MousePos.y >= 0.0f && io.MousePos.y <= zone_y &&
        io.MousePos.x >= 0.0f && io.MousePos.x <= io.DisplaySize.x;
}

// One source of truth for "what is the page showing", used by both the layout and the
// scripted state view. Priority: a native open in flight covers everything (its spinner), then
// the native session video, then the page view, then the unavailable placeholder, then Home.
int Player::browser_content_kind() const {
    if (open_url_loading_) return 2;                                    // Loading
    if (browser_native_) return 3;                                      // Native
    if (browser_web_ && browser_ && browser_->ready() && !browser_failed_) return 1; // Web
    if (browser_failed_ || (browser_ && browser_->failed())) return 4;  // Unavailable
    return 0;                                                           // Home
}

void Player::ensure_browser(){
    if (browser_ && browser_->failed()) {
        browser_failed_ = true;
        return;
    }
    if (!browser_) browser_ = std::make_unique<BrowserView>();
    browser_->ensure_started(hwnd_);
    if (browser_->failed()) browser_failed_ = true;
}

// Per-frame poll of the page view (main thread, inside ui_tick()): creation result, the page
// URL the address bar mirrors, a navigation queued while the controller was still starting, the
// answer to a native-play cookie lookup, and a navigation that turned out to be media (see
// poll_media_navigation()). Nothing here blocks.
void Player::sync_browser_page(){
    if (!browser_) return;
    if (browser_->failed()) browser_failed_ = true;
    // WebView2 creates (and may recreate) its child window tree asynchronously; the edge-resize
    // forwarding subclass has to reach every one of those windows, so it is refreshed here --
    // cheap, and idempotent per HWND.
    browser_->keep_child_subclasses();

    // Navigation submitted before the controller finished starting.
    if (!browser_pending_nav_.empty() && browser_->ready() && !browser_failed_) {
        browser_->navigate(browser_pending_nav_);
        browser_pending_nav_.clear();
        browser_web_ = true;
    }

    // A page asking for a new window (target=_blank / window.open) keeps this view: the page view
    // never opens one itself, and the navigation runs here, outside its callbacks. A request from a
    // page that only runs behind the native video is dropped.
    std::string popup_url;
    if (browser_->take_popup_navigation(popup_url)) {
        if (browser_page_ && !browser_native_ && !open_url_loading_ && !browser_cookie_pending_ &&
            browser_->ready() && !browser_failed_) {
            browser_->navigate_popup(popup_url); // replays the click's referrer (a bare Navigate drops it)
            browser_web_ = true;
        }
    }

    // Address bar mirrors the page the engine is actually on (SourceChanged /
    // NavigationCompleted), including redirects after a submitted address. `browser_page_url_seen_`
    // is only ever compared/assigned here: a submission does NOT touch it, so the bar keeps the
    // submitted text until the engine reports landing somewhere else. Skipped while the field
    // holds text the USER typed since submitting, so a load finishing mid-edit can never yank the
    // caret out from under them.
    // While the page shows the session video -- or a native open is still in flight -- the bar
    // belongs to the URL that was handed over, not to whatever page the view has in its history.
    const std::string& page_url = browser_->page_url();
    if (!browser_native_ && !open_url_loading_ && page_url != browser_page_url_seen_) {
        browser_page_url_seen_ = page_url;
        const bool user_owns_field = browser_addr_active_ &&
            std::strcmp(browser_addr_buf_, browser_addr_submitted_.c_str()) != 0;
        if (!page_url.empty() && !user_owns_field) {
            const size_t n = std::min(page_url.size(), sizeof(browser_addr_buf_) - 1);
            std::memcpy(browser_addr_buf_, page_url.c_str(), n);
            browser_addr_buf_[n] = '\0';
        }
    }

    // CookieManager answer for the play button / media handover: consumed only while the page is
    // still up and the answer names the URL the click asked about. BrowserView retires staler
    // answers itself (generation token), so anything that no longer names `browser_cookie_url_` is
    // dropped here too -- a navigation or a page leave must never leave this waiting forever.
    //
    // A handover belongs to one navigation: once the page moved on, its late answer is dropped.
    if (browser_cookie_pending_ && browser_auto_nav_serial_ != 0 &&
        browser_->nav_serial() != browser_auto_nav_serial_) {
        browser_cookie_pending_ = false;
        browser_cookie_url_.clear();
        browser_auto_headers_.clear();
        browser_auto_nav_serial_ = 0;
        browser_->cancel_cookies();
    }
    if (browser_cookie_pending_) {
        std::string url, cookies;
        if (browser_->take_cookies(url, cookies)) {
            browser_cookie_pending_ = false;
            if (browser_page_ && url == browser_cookie_url_) {
                browser_cookie_url_.clear();
                // The handover's captured request context wins; a manual play click has none and
                // derives its Referer from the page it was clicked on.
                std::string headers = browser_auto_headers_;
                browser_auto_headers_.clear();
                browser_auto_nav_serial_ = 0;
                if (headers.empty()) headers = browser_native_headers(url);
                begin_native_open(url, headers, cookies);
            } else {
                browser_cookie_url_.clear();
            }
        } else if (!browser_->cookie_pending()) {
            // The lookup was cancelled (page left, newer action): stop waiting.
            browser_cookie_pending_ = false;
            browser_cookie_url_.clear();
        }
    }

    poll_media_navigation();
}

// Takes over a page navigation whose document turned out to be the video itself: stop the page
// view's own load of it, then reuse the play button's path (CookieManager jar -> native open).
void Player::poll_media_navigation(){
    if (!browser_) return;
    BrowserView::MediaNavigation nav;
    if (!browser_->take_media_navigation(nav)) return;
    // One-shot either way: dropped while an open is in flight, while another handover collects
    // its cookies, or when the page is gone.
    if (!browser_page_ || browser_failed_ || open_url_loading_ || browser_cookie_pending_) return;
    if (nav.url.empty() || !browser_->ready()) return;
    // A cancelled download has nothing left to stop; never Stop() from a WebView2 callback.
    if (!nav.download) browser_->stop_media_navigation();
    browser_auto_headers_ = browser_media_headers(nav.referer, nav.user_agent);
    browser_auto_nav_serial_ = nav.nav_serial;
    browser_cookie_pending_ = true;
    browser_cookie_url_ = nav.url;
    browser_->request_cookies(nav.url);
}

// Replay context for a manually started native open: the page the URL came from as Referer
// (many media hosts reject a bare request), no User-Agent override (FFmpeg's own is fine).
std::string Player::browser_native_headers(const std::string& url){
    if (!browser_) return std::string();
    const std::string& page = browser_->page_url();
    if (page.empty() || page == url) return std::string();
    return browser_media_headers(page, std::string());
}

std::string Player::browser_media_headers(const std::string& referer,
                                          const std::string& user_agent){
    // FFmpeg's HTTP protocol "headers" value: CRLF-terminated "Name: value" lines.
    std::string h;
    if (!referer.empty()) h += "Referer: " + referer + "\r\n";
    if (!user_agent.empty()) h += "User-Agent: " + user_agent + "\r\n";
    return h;
}

// Hands `url` to the normal async open path (Python's do_open/do_reopen) with the request
// context it needs, and puts the page into its loading state until Python calls
// notify_open_url_finished(). Reached from the address bar's play button, the file picker, a
// dropped file, and the page view's media handover -- never from a submitted address (that is a
// navigation, see browser_submit).
void Player::begin_native_open(const std::string& url, const std::string& headers,
                              const std::string& cookies){
    if (url.empty()) return;
    // Any cookie lookup still in flight belongs to the action this one supersedes (an older play
    // click, an older handover): retire it so its late answer can never start a second open and
    // overwrite the URL/headers that are handed over right here.
    browser_cookie_pending_ = false;
    browser_cookie_url_.clear();
    browser_auto_headers_.clear();
    browser_auto_nav_serial_ = 0;
    if (browser_) {
        browser_->cancel_cookies();
        browser_->cancel_media_navigation();
    }
    const size_t n = std::min(url.size(), sizeof(browser_addr_buf_) - 1);
    std::memcpy(browser_addr_buf_, url.c_str(), n);
    browser_addr_buf_[n] = '\0';
    browser_addr_submitted_ = url;
    open_url_show_error_ = false;
    open_url_show_load_error_ = false;
    open_url_loading_ = true;
    open_cancel_requested_.store(false, std::memory_order_relaxed);
    ui_intents_.open_path = url;
    ui_intents_.open_headers = headers;
    ui_intents_.open_cookies = cookies;
}

namespace {

// Address/URL text as the user means it: surrounding whitespace and paste-quotes stripped.
std::string normalize_url_text(std::string url){
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t a = 0, b = url.size();
    while (a < b && is_ws(static_cast<unsigned char>(url[a]))) ++a;
    while (b > a && is_ws(static_cast<unsigned char>(url[b - 1]))) --b;
    url = url.substr(a, b - a);
    // Surrounding quotes from a paste ("…"/'…').
    if (url.size() >= 2 && ((url.front() == '"' && url.back() == '"') ||
                            (url.front() == '\'' && url.back() == '\'')))
        url = url.substr(1, url.size() - 2);
    return url;
}

bool starts_with_http(const std::string& s){
    static const char* const kPrefixes[] = { "http://", "https://" };
    for (const char* prefix : kPrefixes) {
        const size_t n = std::char_traits<char>::length(prefix);
        if (s.size() < n) continue;
        bool match = true;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(s[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i]))) {
                match = false;
                break;
            }
        if (match) return true;
    }
    return false;
}

} // namespace

// The address bar's text, normalized (see normalize_url_text).
std::string Player::browser_addr_text() const {
    return normalize_url_text(std::string(browser_addr_buf_));
}

// Address bar Go / first-screen form Open: a valid http(s) address goes STRAIGHT to the page
// view. No probing, no fallback -- an ordinary web page appears from the moment it is confirmed,
// and nothing about it depends on the media decoder. The one exception is the page view's own
// observation of where the navigation lands: when the document it fetches IS a video (a direct
// stream link, a redirect onto an .mp4), that navigation is handed to the native decoder instead
// of being displayed (see poll_media_navigation()).
void Player::browser_submit(const std::string& raw){
    const std::string url = normalize_url_text(raw);
    if (!starts_with_http(url)) {
        open_url_show_error_ = true;
        open_url_show_load_error_ = false;
        return;
    }
    open_url_show_error_ = false;
    open_url_show_load_error_ = false;
    // Submitting the address is one of the dropdown's close conditions (Esc, Enter, a click
    // outside the field+dropdown, a navigation, leaving the page).
    fav_dropdown_open_ = false;

    // A navigation is the newest action: retire a play click / handover still waiting for its
    // cookie answer, so its late answer can never compete with what the user just asked for.
    browser_cookie_pending_ = false;
    browser_cookie_url_.clear();
    browser_auto_headers_.clear();
    browser_auto_nav_serial_ = 0;
    if (browser_) {
        browser_->cancel_cookies();
        browser_->cancel_media_navigation();
    }

    const size_t n = std::min(url.size(), sizeof(browser_addr_buf_) - 1);
    std::memcpy(browser_addr_buf_, url.c_str(), n);
    browser_addr_buf_[n] = '\0';
    browser_addr_submitted_ = url; // the bar may follow this navigation (redirects included)
    // `browser_page_url_seen_` is deliberately NOT touched: it means "the engine's URL the bar
    // already mirrors", so the bar keeps the submitted text until the page really lands
    // somewhere. A URL the engine cannot display (a video container it downloads instead of
    // navigating) would otherwise revert the bar to the previous page one frame later -- and
    // the play button would hand over the wrong address.

    // Native content gives the page back (the session pauses) and the view navigates.
    browser_enter_web();
    ensure_browser();
    if (browser_ && !browser_failed_) {
        if (browser_->ready()) {
            browser_->navigate(url);
        } else {
            browser_pending_nav_ = url; // controller still starting
        }
        browser_web_ = true;
    }
}

void Player::browser_navigate(const std::string& url){
    if (!browser_page_) return;
    browser_submit(url);
}

// The address bar's play button: the URL the user is looking at goes straight to the native
// decoder, with the page's cookies as its replay jar. No extension or container gate -- whatever
// FFmpeg can demux opens (query strings, percent-escapes, FLV/HLS/... included). A failure stays
// a visible open error: this path never falls back to the page view.
void Player::browser_play_native(){
    if (!browser_page_ || open_url_loading_) return;
    const std::string url = browser_addr_text();
    if (url.empty()) return;
    ensure_browser();
    if (!browser_ || browser_failed_ || !browser_->ready()) {
        // No engine to lend cookies from: open it anyway (the decoder needs no browser).
        begin_native_open(url, std::string(), std::string());
        return;
    }
    browser_cookie_pending_ = true;
    browser_cookie_url_ = url;
    // A manual play is not a media handover: the Referer comes from the page (see
    // browser_native_headers), and any handover of an older navigation is retired.
    browser_auto_headers_.clear();
    browser_auto_nav_serial_ = 0;
    browser_->cancel_media_navigation();
    browser_->request_cookies(url);
}

// Address-bar history/reload actions. Each one hands the content back to the page view first:
// from the Native state that means pausing the session (browser_enter) so its audio/video does
// not keep running behind the page -- and the page the engine is on becomes what gets drawn.
void Player::browser_go_back(){
    if (!browser_page_ || !browser_ || !browser_->ready()) return;
    // No history to go back to: do NOT leave the Native content for nothing.
    if (!browser_->can_go_back()) return;
    browser_enter_web();
    browser_->go_back();
    browser_web_ = true;
}

void Player::browser_go_forward(){
    if (!browser_page_ || !browser_ || !browser_->ready()) return;
    if (!browser_->can_go_forward()) return;
    browser_enter_web();
    browser_->go_forward();
    browser_web_ = true;
}

void Player::browser_reload(){
    // Native: deliberately inert -- reload would drop the URL the live session is authenticated
    // against, and the button is disabled in that state anyway.
    if (!browser_page_ || browser_native_ || !browser_ || !browser_->ready()) return;
    browser_->reload();
    browser_web_ = true;
}

// ---- address-bar favorites -----------------------------------------------------------------
//
// The list itself lives in Python's settings store; this side mirrors it for the star's state
// and the dropdown's rows. A click applies the change locally first (so the star and the list
// react in the frame the click lands) AND records an intent -- Python applies that to the store
// and echoes the authoritative list back through set_browser_favorites().
//
// URL identity is exact string equality after normalize_url_text() (the same normalization the
// address bar applies to submitted text), never a host or prefix match.
void Player::set_browser_favorites(const py::list& favorites){
    browser_favorites_.clear();
    const py::ssize_t count = py::len(favorites);
    for (py::ssize_t i = 0; i < count; ++i) {
        py::handle item = favorites[i];
        if (!py::isinstance<py::dict>(item)) continue;
        py::dict entry = py::reinterpret_borrow<py::dict>(item);
        BrowserFavorite fav;
        try {
            if (entry.contains("url")) fav.url = normalize_url_text(py::cast<std::string>(entry["url"]));
            if (entry.contains("title")) fav.title = py::cast<std::string>(entry["title"]);
        } catch (const py::cast_error&) {
            continue; // a malformed entry is dropped, never fatal
        }
        if (fav.url.empty()) continue;
        if (fav.title.empty()) fav.title = fav.url;
        browser_favorites_.push_back(std::move(fav));
    }
}

// The URL the star acts on: what the address bar stands for as a COMMITTED page, never the text
// the user is still typing. On the Web content that is the page view's own URL; on the Native
// content it is the URL the session was opened from (which is exactly what the bar displays).
// Home / Loading / Unavailable have nothing committed.
std::string Player::browser_committed_url() const {
    if (!browser_page_) return std::string();
    switch (browser_content_kind()) {
    case 1: // Web
        return browser_ ? normalize_url_text(browser_->page_url()) : std::string();
    case 3: // Native
        return normalize_url_text(browser_addr_submitted_);
    default:
        return std::string();
    }
}

std::string Player::browser_committed_title() const {
    const std::string url = browser_committed_url();
    if (url.empty()) return url;
    if (browser_content_kind() == 1 && browser_) {
        const std::string title = normalize_url_text(browser_->page_title());
        if (!title.empty()) return title;
    }
    return url; // a session URL has no document title: the URL is its label
}

bool Player::browser_favorited(const std::string& url) const {
    if (url.empty()) return false;
    for (const BrowserFavorite& fav : browser_favorites_)
        if (fav.url == url) return true;
    return false;
}

void Player::browser_toggle_favorite(){
    const std::string url = browser_committed_url();
    if (url.empty() || open_url_loading_) return; // nothing committed / busy: no-op (star disabled)
    if (browser_favorited(url)) {
        browser_remove_favorite(url);
        return;
    }
    BrowserFavorite fav;
    fav.url = url;
    fav.title = browser_committed_title();
    if (fav.title.empty()) fav.title = url;
    browser_favorites_.insert(browser_favorites_.begin(), fav); // Python's order: newest first
    ui_intents_.favorite_add_url = fav.url;
    ui_intents_.favorite_add_title = fav.title;
}

void Player::browser_remove_favorite(const std::string& url){
    if (url.empty()) return;
    for (auto it = browser_favorites_.begin(); it != browser_favorites_.end(); ++it) {
        if (it->url != url) continue;
        browser_favorites_.erase(it);
        ui_intents_.favorite_remove_url = url;
        return;
    }
}

// WndProc's ESC path (see its WM_KEYDOWN case): while the favorites dropdown is up, ESC closes
// it instead of quitting/parking the player.
bool Player::handle_escape(){
    if (browser_page_ && fav_dropdown_open_) {
        fav_dropdown_open_ = false;
        return true;
    }
    return false;
}

py::dict Player::browser_state() const {
    py::dict d;
    const int kind = browser_content_kind();
    d["page"] = browser_page_;
    d["native"] = browser_native_;
    d["web"] = browser_web_;
    d["content"] = kind ? (kind == 1 ? "web" : kind == 2 ? "loading" : kind == 3 ? "native"
                                                  : "unavailable")
                        : "home";
    d["available"] = browser_ && browser_->ready();
    d["failed"] = browser_failed_ || (browser_ && browser_->failed());
    d["url"] = browser_ ? browser_->page_url() : std::string();
    d["loading"] = open_url_loading_;
    // Page-view audio follows visibility (muted while hidden), and a cookie handoff is in
    // flight while either side still waits -- both observable for scripted checks.
    d["muted"] = browser_ && browser_->muted();
    d["cookie_pending"] = browser_cookie_pending_ ||
        (browser_ && browser_->cookie_pending());
    // Everything a scripted/real-input check needs to reason about the page's chrome.
    d["address"] = std::string(browser_addr_buf_);
    d["web_visible"] = browser_ && browser_->visible();
    d["settings_open"] = ui_settings_open_;
    d["stream_open"] = stream_popup_open_;
    d["export"] = export_mode_;
    d["can_back"] = browser_ && !open_url_loading_ && browser_->can_go_back();
    d["can_forward"] = browser_ && !open_url_loading_ && browser_->can_go_forward();
    // Same predicate the address bar uses for the reload button: something must be loaded, it
    // must not be the Native content (reloading would drop the session's URL) and no open may
    // be in flight.
    d["can_reload"] = browser_ && browser_->ready() && !open_url_loading_ && !browser_native_ &&
        !browser_->page_url().empty();
    // Floating-chrome observables (Native hides the address bar unless revealed) and the video
    // viewport the present thread would use right now (it must not depend on the bar).
    d["addr_visible"] = browser_addr_visible_;
    const D3D11_VIEWPORT vp = fit_viewport();
    d["viewport"] = py::make_tuple(vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
    // Whether the settings panel can float above the live page view at all (see
    // settings_popup_available()): when it cannot, the page view steps aside while it is open.
    d["settings_popup"] = settings_popup_available();
    d["fullscreen"] = fullscreen_.load(std::memory_order_relaxed);
    d["top_bar_visible"] = top_bar_visible_;
    int b[4] = { 0, 0, 0, 0 };
    if (browser_) browser_->bounds(b);
    d["bounds"] = py::make_tuple(b[0], b[1], b[2], b[3]);
    // Layout observables (see the members' comment): the page surface's rect and the Home form
    // block's rect. A page surface clamped narrower than the client clips the entire form.
    d["page_rect"] = py::make_tuple(browser_page_rect_[0], browser_page_rect_[1],
        browser_page_rect_[2], browser_page_rect_[3]);
    d["home_rect"] = py::make_tuple(browser_home_rect_[0], browser_home_rect_[1],
        browser_home_rect_[2], browser_home_rect_[3]);
    // ---- address-bar favorites + popup layers (see popup_layer.h) ---------------------------
    // "favorites" is the display order Python pushed; "favorited" is the star's state for the
    // committed page URL; "favorites_open" is the dropdown's visibility.
    py::list favs;
    for (const BrowserFavorite& fav : browser_favorites_) favs.append(fav.url);
    d["favorites"] = favs;
    d["favorited"] = browser_favorited(browser_committed_url());
    d["favorites_open"] = fav_dropdown_open_;
    // Each popup layer is a real top-level owned window with its own swapchain: scripts use the
    // hwnd/visible/rect trio to prove the dropdown, the tooltips, the settings panel and the open
    // popup really do rise above the page view, "frames" to know the present thread has drawn into
    // it, and "available" to know the layer could get its window at all (false = the owner falls
    // back to the main swapchain, with the page view stepping aside).
    auto popup_dict = [](const PopupLayer* layer) {
        py::dict p;
        int client[4] = { 0, 0, 0, 0 };
        int screen[4] = { 0, 0, 0, 0 };
        if (layer) {
            layer->client_rect(client);
            layer->screen_rect(screen);
        }
        const unsigned long long hwnd_value = layer
            ? static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(layer->hwnd())) : 0ull;
        p["hwnd"] = py::int_(hwnd_value);
        p["visible"] = layer && layer->visible();
        p["routed"] = layer && layer->routed();
        p["available"] = layer && !layer->unavailable();
        p["client_rect"] = py::make_tuple(client[0], client[1], client[2], client[3]);
        p["screen_rect"] = py::make_tuple(screen[0], screen[1], screen[2], screen[3]);
        p["frames"] = layer ? layer->rendered_frames() : 0ull;
        return p;
    };
    py::dict popups;
    popups["favorites"] = popup_dict(fav_popup_.get());
    popups["tooltip"] = popup_dict(tooltip_popup_.get());
    // The settings panel is its own root window, and so is the popup it opens (a combo list, a
    // context menu) -- two layers, not one.
    popups["settings"] = popup_dict(settings_popup_.get());
    popups["menu"] = popup_dict(menu_popup_.get());
    d["popups"] = popups;
    // Last address-field rect the bar laid out (client coords: x, y, w, h). The dropdown is
    // pinned to its left edge and width, so a script can compare the two exactly.
    d["addr_field_rect"] = py::make_tuple(browser_addr_field_rect_[0], browser_addr_field_rect_[1],
        browser_addr_field_rect_[2] - browser_addr_field_rect_[0],
        browser_addr_field_rect_[3] - browser_addr_field_rect_[1]);
    return d;
}

// Loading-state Cancel (see player.h). The atomic store + decoder interrupt are safe to
// do from the UI thread while the worker blocks inside avformat_open_input; the worker then
// unwinds through the normal failure path and Python reaps it as a silent cancellation
// (open_url_cancel_requested()). No page close here -- the loading card greys out its
// Cancel button until notify_open_url_finished() lands.
void Player::cancel_open_url(){
    open_cancel_requested_.store(true, std::memory_order_relaxed);
    decoder_.request_interrupt();
}

// Python calls this on the main thread when an async open finishes. Success keeps the page up
// and turns it into the Native state (the video renders below the address bar, the bottom bar
// comes back) -- the page is never left implicitly. Failure keeps the previous content with the
// open_url_load_failed line, unless the user cancelled, which silently returns to it.
void Player::notify_open_url_finished(bool ok){
    if (ok) {
        open_url_loading_ = false;
        open_url_show_error_ = false;
        open_url_show_load_error_ = false;
        // The session's video is now the page content. Only a page that is actually up adopts
        // it (an ordinary local open keeps the page closed).
        if (browser_page_) {
            set_browser_native(true);
            // The bar names what is now playing, whatever opened it: the address bar's play
            // button goes through begin_native_open() (which already put the URL there), but a
            // file picked with the open-file button or dropped on the window does not -- without
            // this the bar would keep showing the old page URL.
            const std::string& shown = video_path_;
            const size_t n = std::min(shown.size(), sizeof(browser_addr_buf_) - 1);
            std::memcpy(browser_addr_buf_, shown.c_str(), n);
            browser_addr_buf_[n] = '\0';
            browser_addr_submitted_ = shown;
            // Pin the page URL as "already synced" so the page view's own URL cannot overwrite
            // the native address while the page keeps its history in the background.
            if (browser_) browser_page_url_seen_ = browser_->page_url();
        }
    } else if (open_url_loading_) {
        open_url_loading_ = false;
        open_url_show_error_ = false;
        // A manual native open that failed is reported, never silently re-routed to the page
        // view: only an explicit Cancel stays silent.
        open_url_show_load_error_ = !open_cancel_requested_.load(std::memory_order_relaxed);
    }
    open_cancel_requested_.store(false, std::memory_order_relaxed);
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
    ui::set_ui_scale(scale); // the ui layer's fixed-metric arithmetic must use THIS exact
                             // value, not a rasterized-font proxy (see ui::set_ui_scale)

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

void Player::set_trt_engine_status(int status) { trt_engine_status_ = status; }

// First-screen TRT-compile prompt state, pushed once per Python tick (same main-thread-only
// publish pattern as set_status_text). state: 0 hidden / 1 idle (offer compile) / 2 running
// (progress bar) / 3 failed (offer retry). progress in [0,1]; text is the line shown under
// the button (idle/failed) or beside the bar (running). Rendered by build_open_prompt_overlay;
// compile_ui_progress_ is also read by the settings-panel footer's Compiling progress bar.
void Player::set_compile_ui(int state, float progress, const std::string& text, int step, int total){
    compile_ui_state_ = state;
    compile_ui_progress_ = progress;
    compile_ui_text_ = text;
    compile_ui_step_ = step;
    compile_ui_total_ = total;
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
    take("browser_unavailable", ui_str_.browser_unavailable);
    take("browser_go", ui_str_.browser_go);
    take("browser_play", ui_str_.browser_play);
    take("input_cut", ui_str_.input_cut);
    take("input_copy", ui_str_.input_copy);
    take("input_paste", ui_str_.input_paste);
    take("input_paste_open", ui_str_.input_paste_open);
    take("input_select_all", ui_str_.input_select_all);
    take("compile_retry", ui_str_.compile_retry);
    take("compile_engine", ui_str_.compile_engine);
    take("compile_failed", ui_str_.compile_failed);
    take("trt_ready", ui_str_.trt_ready);
    take("trt_not_compiled", ui_str_.trt_not_compiled);
    take("trt_not_applicable", ui_str_.trt_not_applicable);
    take("trt_warming", ui_str_.trt_warming);
    take("trt_compiling", ui_str_.trt_compiling);
    take("trt_not_compiled_hint", ui_str_.trt_not_compiled_hint);
    take("warmup_failed", ui_str_.warmup_failed);
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
    take("export_section_settings", ui_str_.export_section_settings);
    take("export_clip_length_label", ui_str_.export_clip_length_label);
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
    take("export_preset_cq_end_quality", ui_str_.export_preset_cq_end_quality);
    take("export_preset_cq_end_size", ui_str_.export_preset_cq_end_size);
    take("export_preset_bitrate_label", ui_str_.export_preset_bitrate_label);
    take("export_preset_maxrate_label", ui_str_.export_preset_maxrate_label);
    take("export_preset_vbr_label", ui_str_.export_preset_vbr_label);
    take("export_preset_quality_label", ui_str_.export_preset_quality_label);
    take("export_preset_quality_highest", ui_str_.export_preset_quality_highest);
    take("export_preset_audio_label", ui_str_.export_preset_audio_label);
    take("export_preset_audio_copy", ui_str_.export_preset_audio_copy);
    take("export_preset_audio_encode", ui_str_.export_preset_audio_encode);
    take("export_preset_subtitle_label", ui_str_.export_preset_subtitle_label);
    take("export_preset_subtitle_none", ui_str_.export_preset_subtitle_none);
    take("export_preset_subtitle_copy", ui_str_.export_preset_subtitle_copy);
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
    d["open_headers"] = ui_intents_.open_headers; // page-captured HTTP headers ("" == none)
    d["open_cookies"] = ui_intents_.open_cookies; // page-captured cookies ("" == none)
    d["open_dialog"] = ui_intents_.open_dialog; // M-C2: top-bar "open" button clicked
    d["browser_enter"] = ui_intents_.browser_enter; // URL page opened -> pause the old session
    d["browser_exit"] = ui_intents_.browser_exit;   // URL page closed -> restore playback
    d["close_request"] = ui_intents_.close_request; // park_on_close: X/ESC -> Python decides park vs quit
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
    d["export_preset_vbr_enabled"] = ui_intents_.export_preset_vbr_enabled;
    d["export_preset_bitrate"] = ui_intents_.export_preset_bitrate;
    d["export_preset_maxrate"] = ui_intents_.export_preset_maxrate;
    d["export_preset_quality"] = ui_intents_.export_preset_quality;
    d["export_preset_audio_copy"] = ui_intents_.export_preset_audio_copy;
    d["export_preset_audio_bitrate"] = ui_intents_.export_preset_audio_bitrate;
    d["export_preset_subtitle"] = ui_intents_.export_preset_subtitle;
    d["export_preset_suffix"] = ui_intents_.export_preset_suffix;
    d["export_set_default"] = ui_intents_.export_set_default;
    d["export_drop_paths"] = ui_intents_.export_drop_paths;
    // Address-bar favorites ("" == nothing pending). Python owns the list in settings.json; it
    // applies these and echoes the result back with set_browser_favorites().
    d["favorite_add_url"] = ui_intents_.favorite_add_url;
    d["favorite_add_title"] = ui_intents_.favorite_add_title;
    d["favorite_remove_url"] = ui_intents_.favorite_remove_url;
    ui_intents_ = UiIntents{}; // drain
    return d;
}

