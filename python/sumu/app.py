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
# "正在预热中…" status float (native build_status_float(), driven by set_status_text()) tracks
# warmup progress. Opening a file no longer waits on the models -- player.open() plus play()
# starts original-passthrough playback immediately (present's AI-absent fallback, see
# DESIGN.md I9); the Scheduler is only constructed once warmup finishes, at which point AI
# frames start covering the passthrough ones with no playback interruption.
import argparse
import sys
import threading
import time

import sumu_core  # noqa: E402
from sumu.pipeline import build_models  # noqa: E402
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
        det_model, res_model, pad_mode = build_models(device, fp16)
        print(f"== load_models == {time.perf_counter()-t_load0:.2f}s pad_mode={pad_mode}",
              file=sys.stderr)

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
            state.ready = True
    except Exception as e:  # noqa: BLE001 -- warmup failure must never crash the player
        print(f"== warmup failed == {e!r}", file=sys.stderr)
        with state.lock:
            state.error = e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video", nargs="?", default=None)
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--maximized", action="store_true", default=True)
    args = ap.parse_args()

    settings = settings_mod.load()

    player = sumu_core.Player(args.width, args.height, args.maximized)
    player.set_volume(settings.volume)
    player.set_muted(settings.muted)

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
    scheduler = None
    det_model = res_model = pad_mode = None

    def do_open(path):
        """First-ever open (opened is False): player.open() is decode-only and fast (no model
        dependency), so this starts original/passthrough playback immediately -- the Scheduler
        is deliberately NOT built here, only once warmup finishes (see the main loop's
        "延迟建 scheduler" step below)."""
        nonlocal opened, current_path
        player.open(path)
        print(f"== player.open == fps={player.fps():.4f} frames={player.frame_count()} "
              f"dims={player.dims()}", file=sys.stderr)
        opened = True
        current_path = path
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
        scheduler" step below."""
        nonlocal scheduler, current_path
        settings.set_position(current_path, player.current_frame())
        if scheduler is not None:
            scheduler.stop()
            scheduler = None
        new_frame_count = player.reopen(path)
        print(f"== player.reopen == frames={new_frame_count} dims={player.dims()}",
              file=sys.stderr)
        # video_meta recomputed lazily at scheduler-build time (see do_open's note) -- tearing the
        # scheduler down above forces the build step below to re-run against the new file.
        current_path = path
        settings.push_recent(current_path)
        player.play()  # open_session() starts paused; a freshly reopened file auto-plays
                       # (matches do_open()'s open->play() sequence above)

    def maybe_resume(path):
        """M-E: seek back to where the user left off in `path`, if settings has a "meaningful
        mid-file" position for it (settings_mod.is_resumable_frame -- skips near-start/near-end
        and unknown fps/frame_count). Same seek order as the UI seek-intent path below
        (scheduler.notify_seek() then player.seek()). No-op if there's no stored position or it
        doesn't clear the resume gate. Only called once scheduler is already built, so
        scheduler.notify_seek() is always valid here."""
        frame = settings.get_position(path)
        if frame is None:
            return
        if settings_mod.is_resumable_frame(frame, player.fps(), player.frame_count()):
            scheduler.notify_seek(frame)
            player.seek(frame)

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

            if scheduler is not None:
                status_text = ""
            elif warm_error is not None:
                status_text = "预热失败（将播放原片）"
            elif warm_ready:
                status_text = ""
            else:
                status_text = "正在预热中…"
            player.set_status_text(status_text)

            player.set_ui_config(cfg_clip_length, cfg_max_regions)
            player.ui_tick()

            intents = player.take_ui_intents()

            if intents["toggle_play"]:
                if opened:
                    if player.is_playing():
                        player.pause()
                    else:
                        player.play()

            seek = intents["seek"]
            if seek is not None and opened:
                if scheduler is not None:
                    scheduler.notify_seek(seek)
                player.seek(seek)

            clip_length = intents["clip_length"]
            max_regions = intents["max_regions"]
            if (clip_length is not None or max_regions is not None) and scheduler is not None:
                # scheduler is not None => warmup already succeeded, so warm_sched_cls/cfg_cls are
                # set (captured in the build step below). Rebuild against a fresh SchedulerConfig
                # carrying the updated knobs; video_meta is unchanged (same open file).
                scheduler.stop()
                if clip_length is not None:
                    cfg_clip_length = clip_length
                if max_regions is not None:
                    cfg_max_regions = max_regions
                config = warm_sched_cfg_cls(clip_length=cfg_clip_length,
                                            max_regions_per_frame=cfg_max_regions)
                scheduler = warm_sched_cls(player, det_model, res_model, pad_mode, video_meta, config)
                scheduler.start()

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
                if not opened:
                    do_open(path)
                else:
                    do_reopen(path)

            # 延迟建 scheduler: only once a file is open AND warmup has succeeded AND no
            # scheduler is already running. Deliberately re-checked every tick (not just right
            # after do_open()/do_reopen()) since warmup can finish on its own schedule, well
            # after either of those.
            if opened and warm_ready and scheduler is None and warm_error is None:
                det_model, res_model, pad_mode = warm_models
                # Lazy, off the startup path: compute video_meta now (get_video_meta_data came from
                # the warmup worker) and build the config from the committed int knobs.
                video_meta = warm_meta_fn(current_path)
                config = warm_sched_cfg_cls(clip_length=cfg_clip_length,
                                            max_regions_per_frame=cfg_max_regions)
                scheduler = warm_sched_cls(player, det_model, res_model, pad_mode, video_meta, config)
                scheduler.start()
                maybe_resume(current_path)

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
