# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Daily-use player entrypoint -- unlike scripts/run_player.py (verification scaffolding: fixed
# --seconds auto-exit, --seek-test, --correctness, trace dump), this has no timeout, no forced
# seek, no trace dump. Runs until the user closes the window (Player.should_quit()) or Ctrl-C.
import argparse
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "python"))
sys.path.insert(0, os.path.join(_REPO, "python", "sumu"))
sys.path.insert(0, _HERE)

import sumu_core  # noqa: E402
import run_player as rp  # noqa: E402 -- reuse build_models(), no re-plumbing
from sumu import settings as settings_mod  # noqa: E402 -- M-E: persisted volume/mute/recent/resume


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

    video = args.video
    if not video:
        video = player.pick_open_file()
        if not video:
            print("[play] no file selected, exiting", file=sys.stderr)
            player.close()
            return

    # M-E: current_path tracks whichever file is actually loaded (initial open, then whatever
    # apply_ui_intents' reopen path swaps it to) -- both the reopen handler and the loop-exit
    # finally use it to know which file's position to persist.
    current_path = video
    settings.push_recent(current_path)

    # M-C1: present_thread_ is already running (started in the Player ctor) and draws a "loading"
    # splash while !session_active_. Publish a splash frame now, THEN do the whole heavy startup
    # (torch import + build_models + player.open()'s decoder-open/ring-buffer-prime) with the
    # splash on screen -- session_active_ only flips true at the very end of player.open() below,
    # so the window shows "加载中…" for the ENTIRE load instead of a blank/frozen window. We build
    # the AI models BEFORE player.open() deliberately: that keeps the splash covering the slow
    # model load too, and once open() returns we start the scheduler + play() within ~1s, so the
    # user goes splash -> real playback with no lingering frozen first frame. A couple of ticks
    # (not one) so ImGui's first-frame font-atlas build is already done before the load blocks the
    # main thread (the splash frame present_thread_ keeps redrawing stays "加载中…" throughout).
    player.pump_messages()
    player.ui_tick()
    player.pump_messages()
    player.ui_tick()

    import torch
    from sumu.ai.utils.video_utils import get_video_meta_data
    from sumu.scheduler import Scheduler, SchedulerConfig

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    fp16 = device.type == "cuda"
    print(f"== env == torch {torch.__version__} device {device} "
          f"{torch.cuda.get_device_name(0) if device.type == 'cuda' else ''}", file=sys.stderr)

    t_load0 = time.perf_counter()
    det_model, res_model, pad_mode = rp.build_models(device, fp16)
    print(f"== load_models == {time.perf_counter()-t_load0:.2f}s pad_mode={pad_mode}",
          file=sys.stderr)

    player.open(video)
    print(f"== player.open == fps={player.fps():.4f} frames={player.frame_count()} "
          f"dims={player.dims()}", file=sys.stderr)

    video_meta = get_video_meta_data(video)
    config = SchedulerConfig()
    scheduler = Scheduler(player, det_model, res_model, pad_mode, video_meta, config)
    scheduler.start()

    def maybe_resume(path):
        """M-E: seek back to where the user left off in `path`, if settings has a "meaningful
        mid-file" position for it (settings_mod.is_resumable_frame -- skips near-start/near-end
        and unknown fps/frame_count). Same seek order as the UI seek-intent path below
        (scheduler.notify_seek() then player.seek()). No-op if there's no stored position or it
        doesn't clear the resume gate."""
        frame = settings.get_position(path)
        if frame is None:
            return
        if settings_mod.is_resumable_frame(frame, player.fps(), player.frame_count()):
            scheduler.notify_seek(frame)
            player.seek(frame)

    maybe_resume(current_path)

    def apply_ui_intents(intents):
        """Same semantics as run_player.py's apply_ui_intents (toggle_play / seek /
        clip_length|max_regions rebuild) -- see run_player.py:183 for the full rationale.
        M-C2 adds reopen: a dropped file (open_path) or the top-bar "open" button
        (open_dialog, answered here with the blocking pick_open_file() dialog -- present keeps
        showing the current video while it's up) triggers player.reopen(path), which swaps the
        playing file without tearing down present_thread_/the window (see player.cpp's
        Player::reopen()). The scheduler is torn down/rebuilt exactly like the clip_length/
        max_regions path above (model objects reused, only the scheduler + its video_meta are
        file-specific)."""
        nonlocal scheduler, config, video_meta, current_path

        if intents["toggle_play"]:
            if player.is_playing():
                player.pause()
            else:
                player.play()

        seek = intents["seek"]
        if seek is not None:
            scheduler.notify_seek(seek)
            player.seek(seek)

        clip_length = intents["clip_length"]
        max_regions = intents["max_regions"]
        if clip_length is not None or max_regions is not None:
            scheduler.stop()
            if clip_length is not None:
                config.clip_length = clip_length
            if max_regions is not None:
                config.max_regions_per_frame = max_regions
            scheduler = Scheduler(player, det_model, res_model, pad_mode, video_meta, config)
            scheduler.start()

        path = None
        if intents["open_dialog"]:
            path = player.pick_open_file()  # modal, main thread; present keeps showing the
                                             # current video the whole time this is up
        elif intents["open_path"]:
            path = intents["open_path"]
        if path:
            # M-E: snapshot the outgoing file's position before it's swapped away.
            settings.set_position(current_path, player.current_frame())
            scheduler.stop()
            new_frame_count = player.reopen(path)
            print(f"== player.reopen == frames={new_frame_count} dims={player.dims()}",
                  file=sys.stderr)
            video_meta = get_video_meta_data(path)
            current_path = path
            settings.push_recent(current_path)
            scheduler = Scheduler(player, det_model, res_model, pad_mode, video_meta, config)
            scheduler.start()
            maybe_resume(current_path)
            player.play()  # open_session() starts paused; a freshly reopened file auto-plays
                           # (matches main()'s open->scheduler.start()->play() sequence below)

    player.play()
    try:
        while not player.should_quit():
            player.pump_messages()
            player.set_ui_config(config.clip_length, config.max_regions_per_frame)
            player.ui_tick()
            apply_ui_intents(player.take_ui_intents())
            # 50Hz main loop. NOT 0.008 (125Hz): measured regression (see run_player.py:236 /
            # docs/native_core.md) -- a 125Hz loop starves the present thread, breaking present
            # cadence. Keep 0.02.
            time.sleep(0.02)
    finally:
        # M-E: persist the outgoing file's position and save settings.json. current_frame() is
        # guarded -- the player may already be mid-close by the time we get here, and persistence
        # must never turn a clean shutdown into a crash.
        try:
            settings.set_position(current_path, player.current_frame())
        except Exception:  # noqa: BLE001 -- persistence must never crash shutdown
            pass
        settings_mod.save(settings)
        scheduler.stop()
        player.close()


if __name__ == "__main__":
    main()
