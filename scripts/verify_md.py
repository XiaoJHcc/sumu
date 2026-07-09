# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Throwaway verification driver for Phase 6 M-D (present-side view control: de-mosaic on/off,
# a native atomic -- see native/src/player.cpp's set_ai_enabled/is_ai_enabled). Reuses
# run_player.py's build_models()/print_status() and the Scheduler wiring, same import pattern
# as stress_reopen.py -- does NOT modify run_player.py or stress_seek_ai.py.
#
# Sequence (per task brief):
#   a. open test_video_4k.mp4, build models, Scheduler, start, play.
#   b. pump ~5s with ai_enabled_ default True -- confirm n_ai_fresh > 0.
#   c. set_ai_enabled(False); pump ~5s -- confirm n_ai_fresh barely moves and n_pt_fresh grows.
#   d. dump present trace, print present_stats() (median/p99/max).
#
# Usage:
#   .venv/Scripts/python.exe scripts/verify_md.py
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "python"))
sys.path.insert(0, os.path.join(_REPO, "python", "sumu"))
sys.path.insert(0, _HERE)

import sumu_core  # noqa: E402
import run_player as rp  # noqa: E402 -- reuse build_models()/print_status(), no re-plumbing

TRACE_DIR = rp.TRACE_DIR


def pump_for(player, scheduler, seconds, tag, t_start_ref):
    """Same 50Hz pump loop as run_player.py's main loop / stress_reopen.py's pump_for -- 0.02s
    sleep, NOT 0.008 (measured to break the present cadence, see run_player.py's own comment)."""
    t_start = time.perf_counter()
    last_print = 0.0
    while time.perf_counter() - t_start < seconds:
        player.pump_messages()
        player.ui_tick()
        if player.should_quit():
            return True
        now = time.perf_counter() - t_start
        if now - last_print >= 1.0:
            rp.print_status(tag, player, scheduler, t_start_ref)
            last_print = now
        time.sleep(0.02)
    player.pump_messages()
    return player.should_quit()


def main():
    video = os.path.join(_REPO, "test_video_4k.mp4")

    import torch
    from sumu.ai.utils.video_utils import get_video_meta_data
    from sumu.scheduler import Scheduler, SchedulerConfig

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    fp16 = device.type == "cuda"
    print(f"== env == torch {torch.__version__} device {device} "
          f"{torch.cuda.get_device_name(0) if device.type == 'cuda' else ''}", file=sys.stderr)

    t_load0 = time.perf_counter()
    det_model, res_model, pad_mode = rp.build_models(device, fp16, allow_trt_compile=True)
    print(f"== load_models == {time.perf_counter()-t_load0:.2f}s pad_mode={pad_mode}", file=sys.stderr)

    video_meta = get_video_meta_data(video)
    print(f"== video == {video} {video_meta.video_width}x{video_meta.video_height} "
          f"fps={video_meta.video_fps:.3f} frames={video_meta.frames_count}", file=sys.stderr)

    # a. open, build Scheduler, start, play.
    player = sumu_core.Player(3840, 2160, True)
    player.open(video)
    print(f"== player.open == fps={player.fps():.4f} frames={player.frame_count()} "
          f"dims={player.dims()}", file=sys.stderr)

    config = SchedulerConfig()
    scheduler = Scheduler(player, det_model, res_model, pad_mode, video_meta, config)
    scheduler.start()

    t0 = time.perf_counter()
    player.play()

    quit_early = False
    result = {"steps": {}}

    # b. ai_enabled_ default True -- pump ~5s, confirm n_ai_fresh > 0.
    st_b0 = player.stats()
    quit_early = pump_for(player, scheduler, 5.0, "b-ai-on", t0)
    st_b1 = player.stats()
    result["steps"]["b_default_ai_on"] = {
        "ai_enabled_before": player.is_ai_enabled(),
        "n_ai_fresh_before": st_b0["n_ai_fresh"],
        "n_ai_fresh_after": st_b1["n_ai_fresh"],
        "n_ai_fresh_delta": st_b1["n_ai_fresh"] - st_b0["n_ai_fresh"],
        "present_count_after": st_b1["present_count"],
    }
    print(f"[step b] n_ai_fresh {st_b0['n_ai_fresh']} -> {st_b1['n_ai_fresh']} "
          f"(delta={st_b1['n_ai_fresh']-st_b0['n_ai_fresh']})", file=sys.stderr)

    # c. set_ai_enabled(False) -- pump ~5s, confirm n_ai_fresh barely moves, n_pt_fresh grows.
    if not quit_early:
        player.set_ai_enabled(False)
        st_c0 = player.stats()
        quit_early = pump_for(player, scheduler, 5.0, "c-ai-off", t0)
        st_c1 = player.stats()
        result["steps"]["c_ai_off"] = {
            "ai_enabled_after_toggle": player.is_ai_enabled(),
            "n_ai_fresh_before": st_c0["n_ai_fresh"],
            "n_ai_fresh_after": st_c1["n_ai_fresh"],
            "n_ai_fresh_delta": st_c1["n_ai_fresh"] - st_c0["n_ai_fresh"],
            "n_pt_fresh_before": st_c0["n_pt_fresh"],
            "n_pt_fresh_after": st_c1["n_pt_fresh"],
            "n_pt_fresh_delta": st_c1["n_pt_fresh"] - st_c0["n_pt_fresh"],
        }
        print(f"[step c] ai_enabled={player.is_ai_enabled()} "
              f"n_ai_fresh delta={st_c1['n_ai_fresh']-st_c0['n_ai_fresh']} "
              f"n_pt_fresh delta={st_c1['n_pt_fresh']-st_c0['n_pt_fresh']}", file=sys.stderr)

    # d. dump present trace + present_stats().
    final_stats = player.stats()
    present_stats = player.present_stats()
    trace_out = os.path.join(TRACE_DIR, "present_verify_md.csv")
    try:
        player.dump_present_trace(trace_out)
    except Exception as e:  # noqa: BLE001
        print(f"dump_present_trace failed: {e!r}", file=sys.stderr)

    scheduler.stop()
    player.close()

    result["quit_early"] = quit_early
    result["final_stats"] = final_stats
    result["present_stats"] = present_stats
    result["trace_path"] = trace_out

    print(f"\n== present_stats == n={present_stats['n']} "
          f"median_ms={present_stats['median_ms']:.3f} p99_ms={present_stats['p99_ms']:.3f} "
          f"max_ms={present_stats['max_ms']:.3f} min_ms={present_stats['min_ms']:.3f} "
          f"mean_ms={present_stats['mean_ms']:.3f}", file=sys.stderr)
    print(f"== final_stats == present_count={final_stats['present_count']} "
          f"n_ai_fresh={final_stats['n_ai_fresh']} n_pt_fresh={final_stats['n_pt_fresh']} "
          f"n_ai_stale={final_stats['n_ai_stale']} n_pt_stale={final_stats['n_pt_stale']}",
          file=sys.stderr)
    print("== DONE ==")
    import json
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
