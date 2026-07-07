# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Phase 6 M-C2 robustness item -- the single highest-concurrency-risk call in this milestone:
# Player::reopen() (native/src/player.cpp) swaps the playing file WITHOUT ever stopping/
# rebuilding present_thread_ or the window (unlike close()/~Player()). This repeatedly reopens
# between two differently-sized videos (1080p <-> 2160p, both directions) N times, playing a
# few seconds of each after every (re)open, watching the whole time for the one failure mode
# that matters here: present_count silently stalling (present_loop() wedged in the
# present-detach handshake, see reopen()'s own header comment for the argument for why that
# should be impossible) or a crash/exception out of reopen() itself.
#
# Deadlock defense: faulthandler.dump_traceback_later() is armed immediately before every
# player.reopen() call and disarmed right after it returns -- same pattern as
# scripts/stress_seek_ai.py's seek() guard. If reopen() (or present_loop(), stuck mid
# close_session()/open_session() teardown-then-rebuild) hangs past --hang-timeout seconds,
# this dumps every thread's Python stack to stderr and hard-exits the process -- that dump is
# the primary artifact for an architecture-level escalation if this test ever finds one.
#
# Usage:
#   .venv/Scripts/python.exe scripts/stress_reopen.py --rounds 12
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video-a", default=os.path.join(_REPO, "test_video.mp4"), help="1080p")
    ap.add_argument("--video-b", default=os.path.join(_REPO, "test_video_4k.mp4"), help="2160p")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--rounds", type=int, default=12,
                     help=">=10 per task brief; each round is one reopen (alternates a<->b, "
                          "so this also covers both resize directions)")
    ap.add_argument("--play-seconds", type=float, default=3.0,
                     help="playback window after the initial open and after each reopen")
    ap.add_argument("--hang-timeout", type=float, default=20.0,
                     help="seconds before a reopen() call is declared hung (dumps all thread "
                          "stacks + hard-exits the process)")
    ap.add_argument("--print-interval", type=float, default=2.0)
    ap.add_argument("--no-trt", action="store_true")
    ap.add_argument("--out", default=os.path.join(TRACE_DIR, "stress_reopen_result.json"))
    ap.add_argument("--trace-out", default=os.path.join(TRACE_DIR, "present_stress_reopen.csv"))
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
    # Models loaded exactly once for the whole run (per task brief) -- reopen() only swaps the
    # native decode/present session, never the AI models, so det_model/res_model/pad_mode are
    # reused across every reopen below, same as play.py/run_player.py reuse them across a
    # clip_length/max_regions scheduler rebuild.
    det_model, res_model, pad_mode = rp.build_models(device, fp16, allow_trt_compile=not args.no_trt)
    print(f"== load_models == {time.perf_counter()-t_load0:.2f}s pad_mode={pad_mode}", file=sys.stderr)

    videos = [args.video_a, args.video_b]
    metas = {}
    for v in videos:
        metas[v] = get_video_meta_data(v)
        print(f"== video == {v} {metas[v].video_width}x{metas[v].video_height} "
              f"fps={metas[v].video_fps:.3f} frames={metas[v].frames_count}", file=sys.stderr)

    player = sumu_core.Player(args.width, args.height, True)
    config = SchedulerConfig()

    def pump_for(seconds, tag, scheduler, t_start_ref):
        t_start = time.perf_counter()
        last_print = 0.0
        while time.perf_counter() - t_start < seconds:
            player.pump_messages()
            player.ui_tick()
            if player.should_quit():
                return True
            now = time.perf_counter() - t_start
            if now - last_print >= args.print_interval:
                rp.print_status(tag, player, scheduler, t_start_ref)
                last_print = now
            time.sleep(0.02)
        player.pump_messages()
        return player.should_quit()

    records = []
    crashed = False
    stalled = False
    quit_early = False

    t0 = time.perf_counter()

    # ---- initial open (video A), NOT reopen -- session #1 is an ordinary player.open(), same
    # as run_player.py/stress_seek_ai.py. reopen() is only meaningful on an already-open Player.
    video_meta = metas[videos[0]]
    player.open(videos[0])
    print(f"== player.open == fps={player.fps():.4f} frames={player.frame_count()} "
          f"dims={player.dims()}", file=sys.stderr)
    scheduler = Scheduler(player, det_model, res_model, pad_mode, video_meta, config)
    scheduler.start()
    player.play()

    pc0 = player.stats()["present_count"]
    quit_early = pump_for(args.play_seconds, "open0", scheduler, t0)
    last_present_count = player.stats()["present_count"]
    if last_present_count < pc0:
        stalled = True  # present_count must never regress

    current_idx = 0
    if not quit_early:
        for i in range(args.rounds):
            current_idx = 1 - current_idx
            target_path = videos[current_idx]
            expected_meta = metas[target_path]

            # Same stop-then-rebuild-then-start pattern run_player.py/play.py already use for a
            # clip_length/max_regions config change (scheduler.stop() joins the daemon producer
            # thread, with a 2s timeout, before returning -- see python/sumu/scheduler.py's
            # Scheduler.stop()) -- reopen() is not a new concurrency contract on the Python side,
            # it reuses the exact stop-before-native-call precedent already trusted there.
            scheduler.stop()

            pc_before = player.stats()["present_count"]
            err = None
            new_frame_count = None
            t_reopen0 = time.perf_counter()

            # Deadlock defense: same faulthandler.dump_traceback_later() pattern as
            # stress_seek_ai.py's seek() guard, armed only around the one call under test.
            faulthandler.dump_traceback_later(args.hang_timeout, repeat=False, exit=True)
            try:
                new_frame_count = player.reopen(target_path)
            except Exception as e:  # noqa: BLE001
                err = repr(e)
                crashed = True
            faulthandler.cancel_dump_traceback_later()
            reopen_latency_ms = (time.perf_counter() - t_reopen0) * 1000.0

            dims_actual = None
            dims_ok = None
            if err is None:
                dims_actual = player.dims()
                dims_ok = (dims_actual[0] == expected_meta.video_width and
                           dims_actual[1] == expected_meta.video_height)

            print(f"[reopen #{i+1}] -> {os.path.basename(target_path)} "
                  f"latency_ms={reopen_latency_ms:.2f} frame_count={new_frame_count} "
                  f"dims={dims_actual} dims_ok={dims_ok} err={err}", file=sys.stderr)

            records.append({
                "reopen_i": i + 1,
                "target": target_path,
                "expected_dims": [expected_meta.video_width, expected_meta.video_height],
                "dims_actual": list(dims_actual) if dims_actual is not None else None,
                "dims_ok": dims_ok,
                "frame_count": new_frame_count,
                "latency_ms": reopen_latency_ms,
                "err": err,
                "present_count_before": pc_before,
            })

            if err is not None:
                break

            video_meta = expected_meta
            scheduler = Scheduler(player, det_model, res_model, pad_mode, video_meta, config)
            scheduler.start()
            # reopen()'s open_session() always starts the new session paused (see
            # Player::open_session()'s own doc comment) -- explicitly resume here so this stress
            # driver actually exercises present_count advancement + the audio heartbeat under
            # realistic playback (play.py's own reopen wiring deliberately leaves this decision
            # to the architect, see final report -- this script needs continued playback to
            # produce a meaningful signal, so it opts in).
            player.play()

            q = pump_for(args.play_seconds, f"reopen{i+1}", scheduler, t0)
            pc_after = player.stats()["present_count"]
            records[-1]["present_count_after"] = pc_after
            records[-1]["present_ticks_advanced"] = pc_after - pc_before
            if pc_after <= last_present_count:
                stalled = True
            last_present_count = pc_after

            if q or player.should_quit():
                quit_early = True
                break

    final_stats = player.stats()
    present_stats = player.present_stats()
    scheduler.stop()
    try:
        player.dump_present_trace(args.trace_out)
    except Exception as e:  # noqa: BLE001
        print(f"dump_present_trace failed: {e!r}", file=sys.stderr)
    player.close()

    n_attempted = len(records)
    n_ok = sum(1 for r in records if r["err"] is None)
    n_dims_ok = sum(1 for r in records if r.get("dims_ok"))
    out = {
        "n_reopens_attempted": n_attempted,
        "n_reopens_ok": n_ok,
        "n_dims_ok": n_dims_ok,
        "crashed": crashed,
        "stalled": stalled,
        "quit_early": quit_early,
        "final_stats": final_stats,
        "present_stats_cumulative": present_stats,
        "records": records,
        "trace_path": args.trace_out,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)
    print(f"\n== SUMMARY == n_reopens={n_attempted} ok={n_ok}/{n_attempted} "
          f"dims_ok={n_dims_ok}/{n_attempted} crashed={crashed} stalled={stalled} "
          f"quit_early={quit_early} final_present_count={final_stats['present_count']}",
          file=sys.stderr)
    print(f"== DONE == wrote {args.out}", file=sys.stderr)
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()
