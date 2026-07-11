# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Daily-use player entrypoint -- unlike scripts/run_player.py (verification scaffolding: fixed
# --seconds auto-exit, --seek-test, --correctness, trace dump), this has no timeout, no forced
# seek, no trace dump. Runs until the user closes the window (Player.should_quit()) or Ctrl-C.
#
# Frozen-safe: no sys.path hacks here -- the caller (dev shim scripts/play.py, or a future
# PyInstaller spec/entry point) is responsible for making `sumu` and `sumu_core` importable
# before calling main().
#
# Startup-UX (model-warmup-in-background): unlike the old version of this module, main() no
# longer blocks the main thread on model warmup (torch import + build_models(), which can take
# several seconds for TRT compilation) before showing anything. The window appears immediately
# and stays responsive (pump_messages() every tick) while warmup runs on a background daemon
# thread; an open/drop-file prompt is shown until the user picks a file, and a small
# "正在预热模型…" status float (native build_status_float(), driven by set_status_text()) tracks
# warmup progress. Opening a file no longer waits on the models -- player.open() plus play()
# starts original-passthrough playback immediately (present's AI-absent fallback, see
# DESIGN.md I9); the Scheduler is only constructed once warmup finishes, at which point AI
# frames start covering the passthrough ones with no playback interruption.
import argparse
import os
import re
import sys
import threading
import time

import sumu_core  # noqa: E402
from sumu.pipeline import build_models, default_restoration_model_path  # noqa: E402
from sumu import settings as settings_mod  # noqa: E402 -- M-E: persisted volume/mute/recent/resume


class _WarmupState:
    """Cross-thread handoff for the background model-warmup thread below -- guarded by `lock`
    since the main thread reads it every tick while the warmup thread writes it exactly once
    (on success or failure). `models` is the (det_model, res_model, pad_mode) tuple build_models()
    returns; `ready`/`error` are mutually exclusive terminal states (never both set)."""

    def __init__(self):
        self.lock = threading.Lock()
        self.ready = False
        self.models = None
        self.error = None
        # The torch-heavy orchestration imports (Scheduler/SchedulerConfig, get_video_meta_data)
        # are done ON this worker thread too -- see _warmup_worker -- and handed back here so the
        # MAIN thread never pays their import cost (that cost is the ~3s startup black-window: a
        # main-thread `from sumu.scheduler import ...` pulls in torch synchronously before the
        # message loop can pump). Consumed only once ready is True, at scheduler-build time.
        self.sched_cls = None   # Scheduler
        self.cfg_cls = None     # SchedulerConfig
        self.meta_fn = None     # get_video_meta_data
        # TRT startup-UX handoff. trt_applicable: this machine can run TRT at all (cuda + fp16).
        # trt_active: engines were found + loaded (load-only warmup) so the restorer already runs
        # TRT. When applicable but not active, engines are absent -> app offers the on-demand
        # "compile acceleration engines" prompt. res_path/device/fp16 are what that compile needs.
        self.trt_applicable = False
        self.trt_active = False
        self.res_path = None
        self.device = None
        self.fp16 = False


class _CompileState:
    """Cross-thread handoff for the on-demand TRT compile thread (spawned when the user clicks the
    first-screen 'compile acceleration engines' button). The compile thread writes progress
    (step/total, text) and the terminal result (split/ok/error); the main loop reads it every tick
    to drive the native compile UI and, on success, hot-swaps the restorer onto TRT."""

    def __init__(self):
        self.lock = threading.Lock()
        self.running = False
        self.step = 0
        self.total = 6            # 6 BasicVSR++ sub-engines (loop_body x4 + preprocess + upsample)
        self.text = ""
        self.done = False
        self.ok = False
        self.error = None
        self.split = None         # the BasicVSRPlusPlusNetSplit to activate on success


# Native compile-UI states pushed via player.set_compile_ui(state, progress, text).
_COMPILE_UI_HIDDEN = 0
_COMPILE_UI_IDLE = 1       # engines absent: show prompt + "compile" button
_COMPILE_UI_RUNNING = 2    # compiling: show progress bar + step text
_COMPILE_UI_FAILED = 3     # compile failed: show error + "retry" button

# How long the "无法打开" float stays visible after a failed open/reopen (seconds).
_OPEN_ERROR_HOLD_S = 4.0


def _compile_worker(cstate: "_CompileState", res_model, res_path, device, fp16) -> None:
    """Runs the blocking (multi-minute) TRT compile off the main thread. Progress messages from
    the compiler are marshalled into cstate via a load-progress callback; the resulting split
    forward is handed back for the main thread to attach. Any failure is captured, never raised --
    a failed compile must leave the player on the working eager path."""
    from sumu.ai.restorationpipeline import compile_and_activate_trt
    from sumu.ai.restorationpipeline.progress import (
        set_load_progress_callback, clear_load_progress_callback,
    )

    def _on_progress(msg: str) -> None:
        with cstate.lock:
            cstate.text = msg
            m = re.search(r"(\d+)\s*/\s*6", msg)  # "Compiling sub-engine 3/6: …"
            if m:
                cstate.step = int(m.group(1))

    set_load_progress_callback(_on_progress)
    try:
        split = compile_and_activate_trt(res_model, res_path, device, fp16)
        with cstate.lock:
            cstate.split = split
            cstate.ok = split is not None
            cstate.done = True
            cstate.running = False
    except Exception as e:  # noqa: BLE001 -- compile failure must never crash the player
        print(f"== trt compile failed == {e!r}", file=sys.stderr)
        with cstate.lock:
            cstate.error = e
            cstate.ok = False
            cstate.done = True
            cstate.running = False
    finally:
        clear_load_progress_callback()


def _warmup_worker(state: "_WarmupState") -> None:
    """Runs on a daemon thread started right after the Player is constructed. torch (and
    everything build_models() pulls in -- sumu.ai, TRT compilation, ...) is only imported here,
    never on the main thread, so a slow/failing warmup never blocks pump_messages()/ui_tick().
    Any exception is captured rather than propagated: warmup failing must degrade to
    passthrough-only playback, never crash the player (see module docstring)."""
    try:
        import torch

        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        fp16 = device.type == "cuda"
        print(f"== env == torch {torch.__version__} device {device} "
              f"{torch.cuda.get_device_name(0) if device.type == 'cuda' else ''}", file=sys.stderr)

        t_load0 = time.perf_counter()
        # Load-only: use precompiled TRT engines if this machine already has them, otherwise stay
        # on the eager PyTorch path. We deliberately do NOT compile here anymore -- compilation is
        # a multi-minute blocking step now driven explicitly by the user via the first-screen
        # "compile acceleration engines" prompt (see the main loop's compile state machine), so a
        # fresh machine gets a fast, responsive startup on eager instead of a silent long stall.
        det_model, res_model, pad_mode = build_models(device, fp16, allow_trt_compile=False)
        print(f"== load_models == {time.perf_counter()-t_load0:.2f}s pad_mode={pad_mode}",
              file=sys.stderr)

        trt_applicable = device.type == "cuda" and fp16
        trt_active = bool(getattr(res_model, "uses_trt", False))
        res_path = default_restoration_model_path()
        print(f"== trt == applicable={trt_applicable} active={trt_active}", file=sys.stderr)

        # Import the torch-heavy orchestration modules here too, off the main thread. torch is
        # already imported above so these are effectively free now (module cache hit), but doing
        # them on the main thread at startup is exactly what caused the ~3s black window -- so
        # they stay here and the classes/fn are handed back via state (see _WarmupState).
        from sumu.scheduler import Scheduler, SchedulerConfig
        from sumu.ai.utils.video_utils import get_video_meta_data

        with state.lock:
            state.models = (det_model, res_model, pad_mode)
            state.sched_cls = Scheduler
            state.cfg_cls = SchedulerConfig
            state.meta_fn = get_video_meta_data
            state.trt_applicable = trt_applicable
            state.trt_active = trt_active
            state.res_path = res_path
            state.device = device
            state.fp16 = fp16
            state.ready = True
    except Exception as e:  # noqa: BLE001 -- warmup failure must never crash the player
        print(f"== warmup failed == {e!r}", file=sys.stderr)
        with state.lock:
            state.error = e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video", nargs="?", default=None)
    # Initial windowed size for the open-prompt window (before any video is opened). Once a
    # video opens, the native side auto-sizes the window to the video (resize_window_for_video()).
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    # Default to windowed. `--maximized` still forces a maximized start. (This previously
    # defaulted to True, so store_true made the window ALWAYS start maximized -- the flag was a
    # no-op and there was no way to get a windowed start.)
    ap.add_argument("--maximized", action="store_true", default=False)
    args = ap.parse_args()

    settings = settings_mod.load()

    player = sumu_core.Player(args.width, args.height, args.maximized)
    player.set_volume(settings.volume)
    player.set_muted(settings.muted)
    # fps_div is derived per open from target_fps + source_fps (not a global fixed skip).

    # Kick off model warmup in the background immediately -- the window is already up (Player's
    # ctor starts present_thread_) and the main loop below starts pumping messages right away, so
    # the window is draggable/responsive for the whole warmup duration instead of the old
    # synchronous "splash + block main thread" sequence.
    warmup = _WarmupState()
    threading.Thread(target=_warmup_worker, args=(warmup,), name="sumu-warmup", daemon=True).start()

    # NOTE: no torch-touching imports on this (main) thread -- Scheduler/SchedulerConfig and
    # get_video_meta_data are imported on the warmup worker and consumed post-warmup only (they
    # pull in torch, and a main-thread import here blocks the message loop for ~3s = the startup
    # black window). See _warmup_worker / _WarmupState.

    # No more startup pick_open_file() -- an unopened window shows the native
    # build_open_prompt_overlay() (drop-file / "打开文件" button) instead. A video given on the
    # command line is treated as the first pending "open" intent, consumed on the loop's first
    # iteration -- same open path a drop/button open would take, just pre-seeded.
    pending_open_path = args.video

    opened = False
    current_path = None
    video_meta = None
    # Committed config values as plain ints (defaults mirror SchedulerConfig's clip_length /
    # max_regions_per_frame). A real SchedulerConfig is only built at scheduler-build time, once
    # warmup has handed the class over -- so nothing here forces the torch import onto startup.
    cfg_clip_length = 30
    cfg_max_regions = 1
    cfg_cold_start_s = float(settings.cold_start_s)
    cfg_target_fps = int(settings_mod.clamp_target_fps(settings.target_fps))
    scheduler = None
    det_model = res_model = pad_mode = None

    def apply_target_fps_for_open():
        """Map global target_fps + this file's source_fps → native fps_div (1..4)."""
        try:
            src = float(player.source_fps())
        except Exception:  # noqa: BLE001
            src = 0.0
        div = settings_mod.fps_div_for_target(src, cfg_target_fps)
        player.set_fps_div(div)
        return div

    # On-demand TRT compile (first-screen prompt). compile_state is the live handoff while a
    # compile runs (None otherwise); trt_activated flips True once engines have been hot-swapped
    # in (either found at warmup or compiled+activated here), which is what hides the prompt.
    compile_state = None
    trt_activated = False

    # Failed open/reopen status float. open_error_until is a monotonic deadline; empty text /
    # deadline in the past means "no open error to show". Cleared on the next successful open.
    open_error_text = ""
    open_error_until = 0.0

    # Startup fast-path for the first-screen compile prompt. The prompt's visibility depends on
    # (a) is TRT applicable on this machine, (b) are engines already on disk -- and until now both
    # were only known AFTER the background warmup imported torch + probed the disk, so the prompt
    # popped in a few seconds late (open button + status float first, prompt jumping in after).
    # We now decide it on the MAIN thread, before the first overlay frame, torch-free:
    #   - engine presence: a coarse filesystem glob (basicvsrpp_sub_engines_present_fast) -- no
    #     torch/tensorrt needed, matches the user's "engines on disk => assume usable" rule.
    #   - applicability: can't be checked torch-free (needs torch.cuda.is_available()), so we read
    #     last run's cached answer (optimistic True on first run since sumu targets Nvidia).
    # Warmup still runs and reconciles both to the real values (see the loop), but in the steady
    # state (cache warm, engines present-or-absent as expected) the guess already matches, so the
    # first screen renders once and never changes.
    from sumu.ai.restorationpipeline import BASICVSRPP_TRT_MAX_CLIP_SIZE
    from sumu.ai.restorationpipeline.trt_engine_paths import basicvsrpp_sub_engines_present_fast
    try:
        _res_path_fast = default_restoration_model_path()
        trt_present_fast = basicvsrpp_sub_engines_present_fast(_res_path_fast, BASICVSRPP_TRT_MAX_CLIP_SIZE)
    except Exception as e:  # noqa: BLE001 -- a path/glob hiccup must not block startup; assume absent
        print(f"== trt == fast presence probe failed ({e!r}); assuming engines absent", file=sys.stderr)
        trt_present_fast = False
    trt_applicable_guess = settings.trt_applicable if settings.trt_applicable is not None else True
    compile_requested = False   # latches a first-screen "compile" click that lands before warmup
    trt_reconciled = False      # flips once warmup's real applicable/active have been folded in

    def _report_open_failed(path, err):
        """Surface a failed open/reopen without killing the main loop. Incomplete downloads /
        unsupported containers land here (native decoder.open throws RuntimeError)."""
        nonlocal open_error_text, open_error_until
        name = os.path.basename(path) if path else ""
        open_error_text = f"无法打开：{name}" if name else "无法打开该视频"
        open_error_until = time.monotonic() + _OPEN_ERROR_HOLD_S
        print(f"== open failed == {path!r}: {err}", file=sys.stderr)

    def do_open(path):
        """First-ever open (opened is False): player.open() is decode-only and fast (no model
        dependency), so this starts original/passthrough playback immediately -- the Scheduler
        is deliberately NOT built here, only once warmup finishes (see the main loop's
        "延迟建 scheduler" step below)."""
        nonlocal opened, current_path, open_error_text, open_error_until
        try:
            player.open(path)
        except Exception as e:  # noqa: BLE001 -- bad/partial files must not kill the player
            _report_open_failed(path, e)
            return
        apply_target_fps_for_open()
        print(f"== player.open == fps={player.fps():.4f} frames={player.frame_count()} "
              f"dims={player.dims()} source_fps={player.source_fps():.4f} "
              f"fps_div={player.fps_div()} target_fps={cfg_target_fps}",
              file=sys.stderr)
        opened = True
        current_path = path
        open_error_text = ""
        open_error_until = 0.0
        settings.push_recent(current_path)
        # video_meta is NOT computed here (get_video_meta_data lives on the warmup worker and may
        # not be handed over yet -- the user can open a file mid-warmup). It's computed lazily in
        # the "延迟建 scheduler" step below, which only runs post-warmup anyway.
        player.play()

    def do_reopen(path):
        """Same semantics as the old apply_ui_intents' reopen path (run_player.py:183 /
        player.cpp's Player::reopen()): swap the playing file without tearing down
        present_thread_/the window. The scheduler (if any -- warmup may still be in flight) is
        torn down and rebuilt against the new file's video_meta once we get back to the "延迟建
        scheduler" step below.

        On failure (unsupported / half-downloaded file): native reopen() has already closed the
        previous session and left the player unopened (opened_=false); we mirror that here so the
        open-prompt comes back and the user can pick another file -- never crash the process."""
        nonlocal scheduler, current_path, opened, open_error_text, open_error_until
        if current_path is not None:
            try:
                settings.set_position(current_path, player.current_frame())
            except Exception:  # noqa: BLE001 -- position save must not block the reopen attempt
                pass
        if scheduler is not None:
            scheduler.stop()
            scheduler = None
        try:
            new_frame_count = player.reopen(path)
        except Exception as e:  # noqa: BLE001 -- bad/partial files must not kill the player
            # Native close_session() already ran; open_session() failed -> no live session.
            opened = False
            current_path = None
            _report_open_failed(path, e)
            return
        apply_target_fps_for_open()
        print(f"== player.reopen == frames={new_frame_count} dims={player.dims()} "
              f"fps={player.fps():.4f} fps_div={player.fps_div()} target_fps={cfg_target_fps}",
              file=sys.stderr)
        # video_meta recomputed lazily at scheduler-build time (see do_open's note) -- tearing the
        # scheduler down above forces the build step below to re-run against the new file.
        current_path = path
        open_error_text = ""
        open_error_until = 0.0
        settings.push_recent(current_path)
        player.play()  # open_session() starts paused at frame 0; auto-play from the start
                       # (matches do_open()'s open->play() sequence above). Position memory is
                       # still written (set_position above / finally) but not auto-restored --
                       # resume wiring is deferred until a manual "continue watching" UI exists.


    try:
        while not player.should_quit():
            player.pump_messages()

            with warmup.lock:
                warm_ready = warmup.ready
                warm_models = warmup.models
                warm_error = warmup.error
                warm_sched_cls = warmup.sched_cls
                warm_sched_cfg_cls = warmup.cfg_cls
                warm_meta_fn = warmup.meta_fn
                warm_trt_applicable = warmup.trt_applicable
                warm_trt_active = warmup.trt_active
                warm_res_path = warmup.res_path
                warm_device = warmup.device
                warm_fp16 = warmup.fp16

            now_mono = time.monotonic()
            if open_error_text and now_mono < open_error_until:
                status_text = open_error_text
            elif open_error_text and now_mono >= open_error_until:
                open_error_text = ""
                status_text = ""
            elif warm_error is not None:
                status_text = "预热失败（将播放原片）"
            elif scheduler is not None or warm_ready:
                status_text = ""
            elif not opened:
                # First screen (no file open yet): the middle compile-prompt region already
                # conveys startup state and shows from frame 1 -- a separate bottom-left
                # "正在预热模型…" float that appears then vanishes a few seconds later is exactly
                # the startup flicker we're removing, so suppress it here. The float is kept only
                # for the file-open-mid-warmup case below (and the warm_error case above).
                status_text = ""
            else:
                status_text = "正在预热模型…"
            player.set_status_text(status_text)

            # On-demand TRT compile state machine. First consume a finished compile (hot-swap the
            # restorer onto TRT on this main thread -- a single atomic attribute set, see
            # BasicvsrppMosaicRestorer.activate_trt), then derive what the first-screen compile
            # prompt should show this tick.
            if compile_state is not None:
                with compile_state.lock:
                    cs_running = compile_state.running
                    cs_done = compile_state.done
                    cs_ok = compile_state.ok
                    cs_split = compile_state.split
                    cs_step = compile_state.step
                    cs_total = compile_state.total
                if cs_done and cs_ok and cs_split is not None:
                    # warm_models[1] is the same restorer object the (possibly already built)
                    # scheduler holds, so this activates TRT live -- no scheduler rebuild.
                    warm_models[1].activate_trt(cs_split)
                    trt_activated = True
                    compile_state = None
                    print("== trt == compiled + activated (live)", file=sys.stderr)

            # Reconcile the startup fast-path guesses against warmup's real answers, once, the
            # first tick warmup is ready. In the steady state the guess already matched (engines
            # present-or-absent as cached), so the prompt state doesn't change here -- this only
            # bites on a first run whose cached applicability was wrong (e.g. a non-Nvidia box that
            # optimistically defaulted to True), where the prompt correctly disappears now. Persist
            # the real applicability so next launch's guess is exact (save() never raises).
            if warm_ready and not trt_reconciled:
                trt_reconciled = True
                if settings.trt_applicable != warm_trt_applicable:
                    settings.trt_applicable = warm_trt_applicable
                    settings_mod.save(settings)

            # Effective applicability/presence: warmup's real values once ready, else the torch-free
            # startup guesses. This is what lets the prompt render correctly from frame 1 instead of
            # waiting for the multi-second warmup.
            trt_applicable_eff = warm_trt_applicable if warm_ready else trt_applicable_guess
            trt_present_eff = warm_trt_active if warm_ready else trt_present_fast

            if not trt_applicable_eff or trt_present_eff or trt_activated:
                compile_ui_state, compile_progress, compile_ui_text = _COMPILE_UI_HIDDEN, 0.0, ""
            elif compile_state is not None and cs_running:
                frac = (cs_step / cs_total) if cs_total else 0.0
                compile_ui_state = _COMPILE_UI_RUNNING
                compile_progress = frac
                compile_ui_text = f"正在编译加速引擎 {cs_step}/{cs_total}…（首次，需数分钟）"
            elif compile_state is not None and cs_done and not cs_ok:
                compile_ui_state, compile_progress, compile_ui_text = _COMPILE_UI_FAILED, 0.0, "编译失败"
            elif compile_requested:
                # Latched click, waiting on warmup/model readiness before the compile thread can
                # actually spawn (below) -- show the progress bar right away so the button
                # disappears on the very next tick instead of staying clickable while queued.
                compile_ui_state = _COMPILE_UI_RUNNING
                compile_progress = 0.0
                compile_ui_text = "准备编译加速引擎…"
            else:
                compile_ui_state = _COMPILE_UI_IDLE
                compile_progress = 0.0
                compile_ui_text = "尚未为你的显卡编译去码加速引擎（首次约数分钟，编完自动生效）"
            player.set_compile_ui(compile_ui_state, compile_progress, compile_ui_text)

            player.set_ui_config(cfg_clip_length, cfg_max_regions, cfg_cold_start_s, cfg_target_fps)
            player.ui_tick()

            intents = player.take_ui_intents()

            # First-screen "compile acceleration engines" button (or "retry" after a failure).
            # The prompt now shows from frame 1 (before warmup finishes), so a click can land
            # before the models are loaded -- but the compile needs the loaded eager restorer
            # (warm_models[1]). So a click only LATCHES the request; the spawn below fires as soon
            # as warmup is ready (and TRT is applicable, engines aren't already active, nothing is
            # already compiling). This way "click then it works" holds even for an eager clicker,
            # instead of the click silently no-op'ing until warmup catches up.
            if intents.get("compile_engine"):
                compile_requested = True
            compile_busy = compile_state is not None and compile_state.running
            if (compile_requested and warm_ready and warm_trt_applicable
                    and not (warm_trt_active or trt_activated)
                    and not compile_busy and warm_models is not None):
                compile_requested = False
                compile_state = _CompileState()
                compile_state.running = True
                threading.Thread(
                    target=_compile_worker,
                    args=(compile_state, warm_models[1], warm_res_path, warm_device, warm_fp16),
                    name="sumu-trt-compile", daemon=True,
                ).start()
                print("== trt == on-demand compile started", file=sys.stderr)

            if intents["toggle_play"]:
                if opened:
                    if player.is_playing():
                        player.pause()
                    else:
                        player.play()

            path = None
            if intents["open_dialog"]:
                path = player.pick_open_file()  # modal, main thread; present keeps showing the
                                                 # current video (or the open-prompt) meanwhile
            elif intents["open_path"]:
                path = intents["open_path"]
            elif pending_open_path:
                path = pending_open_path
            pending_open_path = None

            if path:
                # Open/reopen first so a same-tick seek intent (from the previous file's
                # seekbar) cannot land on the new session. open_session always starts at 0.
                if not opened:
                    do_open(path)
                else:
                    do_reopen(path)
            else:
                seek = intents["seek"]
                if seek is not None and opened:
                    if scheduler is not None:
                        scheduler.notify_seek(seek)
                    player.seek(seek)

            clip_length = intents["clip_length"]
            max_regions = intents["max_regions"]
            cold_start_s = intents.get("cold_start_s")
            target_fps = intents.get("target_fps")
            # Always commit knobs into the Python-owned cfg_* mirrors (including first-screen
            # Apply before any file is open). Scheduler rebuild is separate and only runs when
            # a scheduler already exists for the current file.
            if clip_length is not None:
                cfg_clip_length = clip_length
            if max_regions is not None:
                cfg_max_regions = max_regions
            if cold_start_s is not None:
                cfg_cold_start_s = float(cold_start_s)
                settings.cold_start_s = cfg_cold_start_s
            if target_fps is not None:
                cfg_target_fps = settings_mod.clamp_target_fps(target_fps)
                settings.target_fps = cfg_target_fps
                if opened:
                    apply_target_fps_for_open()
                    if video_meta is not None:
                        try:
                            from fractions import Fraction
                            sess_fps = float(player.fps())
                            video_meta.video_fps = sess_fps
                            video_meta.average_fps = sess_fps
                            video_meta.video_fps_exact = Fraction(sess_fps).limit_denominator(1001)
                            video_meta.frames_count = int(player.frame_count())
                        except Exception:  # noqa: BLE001
                            pass
            if (clip_length is not None or max_regions is not None or cold_start_s is not None
                    or target_fps is not None) and scheduler is not None:
                # target_fps may retime the session (native fps_div); rebuild scheduler so
                # cold-start/lead recompute against the new player.fps()/frame_count().
                scheduler.stop()
                config = warm_sched_cfg_cls(clip_length=cfg_clip_length,
                                            max_regions_per_frame=cfg_max_regions,
                                            cold_start_s=cfg_cold_start_s)
                scheduler = warm_sched_cls(player, det_model, res_model, pad_mode, video_meta, config)
                scheduler.start()


            # 延迟建 scheduler: only once a file is open AND warmup has succeeded AND no
            # scheduler is already running. Deliberately re-checked every tick (not just right
            # after do_open()/do_reopen()) since warmup can finish on its own schedule, well
            # after either of those.
            if opened and warm_ready and scheduler is None and warm_error is None:
                det_model, res_model, pad_mode = warm_models
                # Lazy, off the startup path: compute video_meta now (get_video_meta_data came from
                # the warmup worker) and build the config from the committed int knobs.
                try:
                    video_meta = warm_meta_fn(current_path)
                except Exception as e:  # noqa: BLE001 -- meta probe failure must not kill playback
                    # File is already open and playing passthrough; just skip AI for this file.
                    print(f"== video_meta failed == {current_path!r}: {e}", file=sys.stderr)
                    video_meta = None
                if video_meta is not None:
                    # Align AI meta with the retimed session (player.fps/frame_count after fps_div).
                    try:
                        from fractions import Fraction
                        sess_fps = float(player.fps())
                        video_meta.video_fps = sess_fps
                        video_meta.average_fps = sess_fps
                        video_meta.video_fps_exact = Fraction(sess_fps).limit_denominator(1001)
                        video_meta.frames_count = int(player.frame_count())
                    except Exception:  # noqa: BLE001
                        pass
                    config = warm_sched_cfg_cls(clip_length=cfg_clip_length,
                                                max_regions_per_frame=cfg_max_regions,
                                                cold_start_s=cfg_cold_start_s)
                    scheduler = warm_sched_cls(player, det_model, res_model, pad_mode, video_meta, config)
                    scheduler.start()
                # Intentionally no auto-resume seek: open/reopen always start at frame 0.
                # settings.positions still records last frame (do_reopen/finally) for a future
                # manual "continue watching" path; is_resumable_frame stays available for that.

            # 50Hz main loop. NOT 0.008 (125Hz): measured regression (see run_player.py:236 /
            # docs/native_core.md) -- a 125Hz loop starves the present thread, breaking present
            # cadence. Keep 0.02.
            time.sleep(0.02)
    finally:
        # M-E: persist the outgoing file's position and save settings.json. current_frame() is
        # guarded -- the player may already be mid-close by the time we get here, and persistence
        # must never turn a clean shutdown into a crash.
        try:
            if current_path is not None:
                settings.set_position(current_path, player.current_frame())
        except Exception:  # noqa: BLE001 -- persistence must never crash shutdown
            pass
        settings_mod.save(settings)
        if scheduler is not None:
            scheduler.stop()
        player.close()
