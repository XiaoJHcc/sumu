# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Phase 4e robustness item (2) -- the highest-risk untested combination per the task brief:
# a seek storm (20+ deep/shallow/near-tail seeks, same depths as docs/native_core.md's
# seek-stress: 10%/50%/90%/99%/30% of total frames, repeated across rounds) interleaved with
# the AI scheduler (python/sumu/scheduler.py) running continuously on test_video_long.mp4
# (2.1GB, 1920x1080, 29.97fps, 295662 frames). Previously only a single seek was ever
# exercised together with the scheduler (docs/scheduler.md's 45s+1-seek run); this drives many
# seeks back-to-back while push_ai_frame()/get_cuda_nv12_by_frame() keep firing from the
# scheduler's daemon thread, concurrently with player.seek() on the main thread -- exactly the
# lock-ordering combination (decoder_mutex_ -> d3d_mutex_ -> ready_mutex_, docs/native_core.md)
# that was flagged as analytically-argued-but-never-jointly-stress-tested.
#
# Deadlock defense: faulthandler.dump_traceback_later() is armed immediately before every
# player.seek() call and disarmed right after it returns. If seek() (or anything holding a
# lock it needs, e.g. the scheduler thread stuck inside push_ai_frame/get_cuda_nv12_by_frame)
# hangs past --hang-timeout seconds, this dumps every thread's Python stack to stderr and
# hard-exits the process (a deadlocked native call can't be trusted to return cleanly on its
# own) -- that dump is the primary artifact for an architecture-level escalation if this test
# ever finds one.
#
# Usage:
#   .venv/Scripts/python.exe scripts/stress_seek_ai.py test_video_long.mp4 --rounds 5
import argparse
import faulthandler
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "python"))
sys.path.insert(0, os.path.join(_REPO, "python", "sumu"))
sys.path.insert(0, _HERE)

import sumu_core  # noqa: E402
import run_player as rp  # noqa: E402 -- reuse build_models()/print_status()/TRACE_DIR, no re-plumbing

TRACE_DIR = rp.TRACE_DIR

# Same depths as docs/native_core.md's seek-stress (10%/90%/50%/99%/30%), reordered slightly
# but covering the same shallow/deep/near-tail/repeat-shallow mix.
FRACTIONS = [0.10, 0.50, 0.90, 0.99, 0.30]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video", nargs="?", default=os.path.join(_REPO, "test_video_long.mp4"))
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--rounds", type=int, default=5)  # 5 x 5 fractions = 25 seeks (task asks 20+)
    ap.add_argument("--warmup-seconds", type=float, default=6.0)
    ap.add_argument("--between-seek-seconds", type=float, default=4.0,
                     help="AI-concurrent recovery observation window between seeks -- playback "
                          "and the scheduler both keep running here, this IS the stress")
    ap.add_argument("--hang-timeout", type=float, default=20.0,
                     help="seconds before a seek() call is declared hung (dumps all thread "
                          "stacks + hard-exits the process)")
    ap.add_argument("--print-interval", type=float, default=2.0)
    ap.add_argument("--no-trt", action="store_true")
    ap.add_argument("--out", default=os.path.join(TRACE_DIR, "stress_seek_ai_result.json"))
    ap.add_argument("--trace-out", default=os.path.join(TRACE_DIR, "present_stress_seek_ai.csv"))
    args = ap.parse_args()

    faulthandler.enable()

    import torch
    from sumu.ai.utils.video_utils import get_video_meta_data
    from sumu.scheduler import Scheduler, SchedulerConfig

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    fp16 = device.type == "cuda"
    print(f"== env == torch {torch.__version__} device {device} "
          f"{torch.cuda.get_device_name(0) if device.type == 'cuda' else ''}", file=sys.stderr)

    t_load0 = time.perf_counter()
    det_model, res_model, pad_mode = rp.build_models(device, fp16, allow_trt_compile=not args.no_trt)
    print(f"== load_models == {time.perf_counter()-t_load0:.2f}s pad_mode={pad_mode} "
          f"trt={res_model._split_forward is not None}", file=sys.stderr)

    video_meta = get_video_meta_data(args.video)
    print(f"== video == {args.video} {video_meta.video_width}x{video_meta.video_height} "
          f"fps={video_meta.video_fps:.3f} frames={video_meta.frames_count}", file=sys.stderr)

    player = sumu_core.Player(args.width, args.height, True)
    player.open(args.video)
    fc = player.frame_count()
    print(f"== player.open == fps={player.fps():.4f} frames={fc} dims={player.dims()}", file=sys.stderr)

    config = SchedulerConfig()
    scheduler = Scheduler(player, det_model, res_model, pad_mode, video_meta, config)
    scheduler.start()

    player.play()

    def pump_for(seconds, tag):
        last_print = 0.0
        t_start = time.perf_counter()
        while time.perf_counter() - t_start < seconds:
            player.pump_messages()
            if player.should_quit():
                return True
            now = time.perf_counter() - t_start
            if now - last_print >= args.print_interval:
                rp.print_status(tag, player, scheduler, t_start)
                last_print = now
            time.sleep(0.02)
        player.pump_messages()
        return player.should_quit()

    print(f"[warmup] {args.warmup_seconds}s before first seek", file=sys.stderr)
    quit_early = pump_for(args.warmup_seconds, "warmup")

    seek_records = []
    crashed = False
    frozen = False

    seek_i = 0
    if not quit_early:
        for round_i in range(args.rounds):
            for frac in FRACTIONS:
                seek_i += 1
                target = int(fc * frac)
                st_before = player.stats()
                sc_before = scheduler.get_stats()
                pc_before = st_before["present_count"]

                # Arm the deadlock watchdog only around the seek() call itself -- this is the
                # one call in the whole loop that must never be trusted to hang silently given
                # it now races push_ai_frame/get_cuda_nv12_by_frame from the scheduler thread.
                faulthandler.dump_traceback_later(args.hang_timeout, repeat=False, exit=True)
                t_seek0 = time.perf_counter()
                err = None
                actual = None
                try:
                    scheduler.notify_seek(target)
                    actual = player.seek(target)
                except Exception as e:  # noqa: BLE001
                    err = repr(e)
                    crashed = True
                latency_ms = (time.perf_counter() - t_seek0) * 1000.0
                faulthandler.cancel_dump_traceback_later()

                # Let a handful of present ticks land immediately post-seek (matches
                # native_core.md's seek-stress methodology) before the longer AI-recovery window.
                time.sleep(0.15)
                player.pump_messages()
                pc_mid = player.stats()["present_count"]
                ticks_immediate = pc_mid - pc_before
                if ticks_immediate <= 0 and err is None:
                    frozen = True

                print(f"[seek #{seek_i}] round={round_i} frac={frac:.2f} target={target} "
                      f"actual={actual} latency_ms={latency_ms:.2f} ticks_immediate={ticks_immediate} "
                      f"err={err}", file=sys.stderr)

                # AI-concurrent recovery window: playback + scheduler both keep running here --
                # this interleaving (not the seek() call alone) is the scenario under test.
                q = pump_for(args.between_seek_seconds, f"seek{seek_i}-recover")
                st_after = player.stats()
                sc_after = scheduler.get_stats()
                seek_records.append({
                    "seek_i": seek_i, "round": round_i, "frac": frac, "target": target,
                    "actual": actual, "actual_matches_target": (actual == target) if err is None else None,
                    "err": err, "latency_ms": latency_ms, "ticks_immediate": ticks_immediate,
                    "present_count_before": pc_before,
                    "present_count_after_recovery": st_after["present_count"],
                    "ticks_total_incl_recovery": st_after["present_count"] - pc_before,
                    "ai_hit_rate_before": st_before["ai_hit_rate"],
                    "ai_hit_rate_after_recovery": st_after["ai_hit_rate"],
                    "n_pt_stale_before": st_before["n_pt_stale"], "n_pt_stale_after": st_after["n_pt_stale"],
                    "seek_resets_before": sc_before["seek_resets"], "seek_resets_after": sc_after["seek_resets"],
                    "scheduler_stats_after": sc_after,
                })

                if q or player.should_quit():
                    quit_early = True
                    break
                if err is not None:
                    break
            if quit_early or crashed:
                break

    final_stats = player.stats()
    present_stats = player.present_stats()
    final_scheduler_stats = scheduler.get_stats()
    scheduler.stop()
    try:
        player.dump_present_trace(args.trace_out)
    except Exception as e:  # noqa: BLE001
        print(f"dump_present_trace failed: {e!r}", file=sys.stderr)
    player.close()

    n_seeks = len(seek_records)
    n_exact = sum(1 for r in seek_records if r["actual_matches_target"])
    out = {
        "video": args.video, "n_seeks_attempted": n_seeks, "crashed": crashed, "frozen": frozen,
        "quit_early": quit_early,
        "n_seeks_exact_frame": n_exact,
        "final_stats": final_stats,
        "present_stats_cumulative": present_stats,
        "final_scheduler_stats": final_scheduler_stats,
        "seek_records": seek_records,
        "trace_path": args.trace_out,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)
    print(f"\n== SUMMARY == n_seeks={n_seeks} crashed={crashed} frozen={frozen} quit_early={quit_early} "
          f"n_exact_frame={n_exact}/{n_seeks} final_seek_resets={final_scheduler_stats['seek_resets']} "
          f"(expected>={n_seeks})", file=sys.stderr)
    print(f"== DONE == wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
