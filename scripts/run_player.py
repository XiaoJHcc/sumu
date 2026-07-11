# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# First end-to-end smoke driver wiring the native Player (present + decode + AI-input/output
# bridges, native/src/player.cpp) to the Python AI orchestration (python/sumu/scheduler.py).
# Top-level flow, deliberately minimal per the task brief:
#   load_models -> Player.open(video) -> Scheduler.start() -> play() -> main-thread
#   pump_messages loop, printing ai_hit_rate/present stats periodically.
#
# Also drives (as flags) the two other required smoke scenarios:
#   --seek-test   : mid-playback notify_seek()+player.seek() and recovery observation.
#   --correctness : captures a handful of (original, blended, rgba) samples from the
#                   scheduler and cross-checks them against an independent PyAV CPU decode
#                   of the same frame numbers (channel order + "mosaic actually changed").
#
# Usage:
#   .venv/Scripts/python.exe scripts/run_player.py test_video.mp4 --seconds 40 --seek-test --correctness
import argparse
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "python"))
sys.path.insert(0, os.path.join(_REPO, "python", "sumu"))

import sumu_core  # noqa: E402
from sumu.pipeline import build_models  # noqa: E402

TRACE_DIR = os.path.join(_HERE, "trace")
# run_player.py is dev-only smoke/verification tooling; it is NOT part of the frozen
# daily-player import closure (scripts/sumu_main.py -> sumu.app never imports it), so
# creating the trace dir at import time is safe here and keeps the dev sibling scripts
# (stress_reopen.py / stress_seek_ai.py / verify_md.py) that read rp.TRACE_DIR working
# without calling main().
os.makedirs(TRACE_DIR, exist_ok=True)


def print_status(tag, player, scheduler, t0):
    st = player.stats()
    sc = scheduler.get_stats()
    print(
        f"[{tag}] t={time.perf_counter()-t0:6.1f}s  cur_frame={st['current_frame']:6d}  "
        f"ai_hit_rate={st['ai_hit_rate']:.3f}  present_count={st['present_count']}  "
        f"ai_push_count={st['ai_push_count']}  n_ai_fresh={st['n_ai_fresh']}  "
        f"n_pt_stale={st['n_pt_stale']}  || frontier={sc['ai_frontier']} "
        f"detected={sc['frames_detected']} clips={sc['clips_restored']} "
        f"pushed={sc['frames_pushed']} cache_miss={sc['frame_cache_misses']} "
        f"seek_resets={sc['seek_resets']} backlog_resyncs={sc['backlog_resyncs']} "
        f"cold_start_s={sc['cold_start_s']}",
        file=sys.stderr,
    )


def run_correctness_check(video, scheduler):
    """Cross-check the scheduler's captured (original_bgr, final_bgr, rgba) samples
    (python/sumu/scheduler.py's capture_correctness_samples hook) against an independent
    PyAV CPU decode of the exact same frame numbers. Reports real numbers, no fabrication:
      1. MAE(original_bgr, PyAV bgr24 reference)  -- confirms the scheduler's own
         NV12->BGR acquisition (get_cuda_nv12_by_frame + _nv12_to_bgr_hwc_gpu) matches an
         independent reference at this frame_num (same method as docs/native_ai_input.md).
      2. rgba channel-order check: rgba[...,0]==final_bgr[...,2] (R), [...,1]==[...,1] (G),
         [...,2]==[...,0] (B), [...,3]==255 -- exact equality, no tolerance. This is the
         concrete "not a blue face" check for the packing scheduler.py:_to_rgba does right
         before push_ai_frame.
      3. max|final_bgr - original_bgr| and fraction of changed pixels -- confirms the
         mosaic region was actually restored (non-zero difference vs the untouched
         passthrough content), same method as scripts/verify_scene_clip_blend.py.
    """
    import av
    import numpy as np
    import torch

    samples = scheduler.correctness_samples
    if not samples:
        print("[correctness] no samples captured (no clip completed during the run window "
              "-- try a longer --seconds or a video with earlier mosaic content)", file=sys.stderr)
        return {"n_samples": 0}

    frame_nums = sorted({s["frame_num"] for s in samples})
    max_needed = max(frame_nums)
    ref_frames = {}
    with av.open(video) as container:
        for i, frame in enumerate(container.decode(video=0)):
            if i in frame_nums:
                ref_frames[i] = frame.to_ndarray(format="bgr24")
            if i >= max_needed:
                break

    results = []
    for s in samples:
        fn = s["frame_num"]
        ref = ref_frames.get(fn)
        record = {"frame_num": fn}
        if ref is None:
            record["error"] = "reference frame not decoded (out of range?)"
            results.append(record)
            continue

        original = s["original_bgr"].numpy()
        final = s["final_bgr"].numpy()
        rgba = s["rgba"].numpy()

        mae = float(np.abs(original.astype(np.float32) - ref.astype(np.float32)).mean())
        record["mae_vs_pyav_reference"] = mae

        r_ok = bool(np.array_equal(rgba[..., 0], final[..., 2]))
        g_ok = bool(np.array_equal(rgba[..., 1], final[..., 1]))
        b_ok = bool(np.array_equal(rgba[..., 2], final[..., 0]))
        a_ok = bool(np.all(rgba[..., 3] == 255))
        record["channel_order_ok"] = r_ok and g_ok and b_ok and a_ok

        diff = np.abs(final.astype(np.int16) - original.astype(np.int16))
        record["max_abs_diff"] = int(diff.max())
        record["changed_pixel_fraction"] = float((diff.sum(axis=-1) > 0).mean())
        record["mosaic_fix_detected"] = record["max_abs_diff"] > 0

        results.append(record)
        print(f"[correctness] frame={fn} mae_vs_pyav={mae:.4f} "
              f"channel_order_ok={record['channel_order_ok']} "
              f"max_abs_diff={record['max_abs_diff']} "
              f"changed_px_frac={record['changed_pixel_fraction']:.4f}", file=sys.stderr)

    return {"n_samples": len(samples), "results": results}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video", nargs="?", default=os.path.join(_REPO, "test_video.mp4"))
    ap.add_argument("--seconds", type=float, default=40.0)
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--maximized", action="store_true", default=True)
    ap.add_argument("--print-interval", type=float, default=2.0)
    ap.add_argument("--seek-test", action="store_true")
    ap.add_argument("--seek-frac", type=float, default=0.5)
    ap.add_argument("--seek-observe-seconds", type=float, default=15.0)
    ap.add_argument("--correctness", action="store_true")
    ap.add_argument("--capture-samples", type=int, default=5)
    ap.add_argument("--trace-out", default=os.path.join(TRACE_DIR, "present_run_player.csv"))
    ap.add_argument("--out", default=os.path.join(TRACE_DIR, "run_player_result.json"))
    ap.add_argument("--no-trt", action="store_true", help="skip TRT compile (load-only)")
    args = ap.parse_args()

    import torch
    from sumu.ai.utils.video_utils import get_video_meta_data
    from sumu.scheduler import Scheduler, SchedulerConfig

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    fp16 = device.type == "cuda"
    print(f"== env == torch {torch.__version__} device {device} "
          f"{torch.cuda.get_device_name(0) if device.type == 'cuda' else ''}", file=sys.stderr)

    t_load0 = time.perf_counter()
    det_model, res_model, pad_mode = build_models(device, fp16, allow_trt_compile=not args.no_trt)
    print(f"== load_models == {time.perf_counter()-t_load0:.2f}s pad_mode={pad_mode} "
          f"trt={res_model._split_forward is not None}", file=sys.stderr)

    video_meta = get_video_meta_data(args.video)
    print(f"== video == {args.video} {video_meta.video_width}x{video_meta.video_height} "
          f"fps={video_meta.video_fps:.3f} frames={video_meta.frames_count}", file=sys.stderr)

    player = sumu_core.Player(args.width, args.height, args.maximized)
    player.open(args.video)
    print(f"== player.open == fps={player.fps():.4f} frames={player.frame_count()} "
          f"dims={player.dims()}", file=sys.stderr)

    config = SchedulerConfig()
    scheduler = Scheduler(
        player, det_model, res_model, pad_mode, video_meta, config,
        capture_correctness_samples=args.capture_samples if args.correctness else 0,
    )
    scheduler.start()

    def apply_ui_intents(intents):
        """Execute one drained player.take_ui_intents() dict (native/src/player.cpp's
        Player::take_ui_intents()). Python's main thread is the SOLE executor of transport/
        config changes -- native's present thread only ever renders already-published UI
        snapshots, never touches seek/play/pause/scheduler (see player.cpp's file header, I2).
        Rebinds the enclosing scheduler/config on a clip_length/max_regions change: there is
        no in-place resize on SchedulerConfig, so a config change means stop the old scheduler,
        mutate config, and construct a brand new Scheduler (same construction call as above).
        """
        nonlocal scheduler, config

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
        cold_start_s = intents.get("cold_start_s")
        if clip_length is not None or max_regions is not None or cold_start_s is not None:
            scheduler.stop()
            if clip_length is not None:
                config.clip_length = clip_length
            if max_regions is not None:
                config.max_regions_per_frame = max_regions
            if cold_start_s is not None:
                config.cold_start_s = float(cold_start_s)
            scheduler = Scheduler(
                player, det_model, res_model, pad_mode, video_meta, config,
                capture_correctness_samples=args.capture_samples if args.correctness else 0,
            )
            scheduler.start()

    t0 = time.perf_counter()
    player.play()

    last_print = 0.0
    quit_early = False
    while time.perf_counter() - t0 < args.seconds:
        player.pump_messages()
        player.set_ui_config(config.clip_length, config.max_regions_per_frame, config.cold_start_s)
        player.ui_tick()
        apply_ui_intents(player.take_ui_intents())
        if player.should_quit():
            quit_early = True
            break
        now = time.perf_counter() - t0
        if now - last_print >= args.print_interval:
            print_status("main", player, scheduler, t0)
            last_print = now
        # 50Hz main loop. NOT 0.008 (125Hz): measured (M3 cadence forensics, docs/native_core.md)
        # a 125Hz loop starves the present thread of CPU/scheduling under timeBeginPeriod(1),
        # producing a periodic ~30ms present hitch (steady stddev 0.23->4.39, one 177ms tick) in
        # pure passthrough -- i.e. it breaks the crown present-cadence invariant. 20ms input
        # latency is imperceptible for transport/seek/keyboard, so 0.02 stays. See also smoke_player
        # (no ui_tick) staying pristine on the same busy desktop, which localized the cause here.
        time.sleep(0.02)
    player.pump_messages()
    print_status("main-end", player, scheduler, t0)

    seek_result = None
    if args.seek_test and not quit_early:
        fc = player.frame_count()
        target = int(fc * args.seek_frac)
        st_before = player.stats()
        pc_before = st_before["present_count"]
        t_seek0 = time.perf_counter()
        scheduler.notify_seek(target)
        actual = player.seek(target)
        seek_latency_ms = (time.perf_counter() - t_seek0) * 1000.0
        print(f"[seek-test] target={target} actual={actual} latency_ms={seek_latency_ms:.2f}",
              file=sys.stderr)

        t_seek_obs0 = time.perf_counter()
        recovered_hit_rate = None
        while time.perf_counter() - t_seek_obs0 < args.seek_observe_seconds:
            player.pump_messages()
            player.set_ui_config(config.clip_length, config.max_regions_per_frame, config.cold_start_s)
            player.ui_tick()
            apply_ui_intents(player.take_ui_intents())
            if player.should_quit():
                quit_early = True
                break
            now = time.perf_counter() - t_seek_obs0
            if now - last_print >= args.print_interval or now < 0.001:
                print_status("seek-recovery", player, scheduler, t_seek_obs0)
                last_print = now
            # 50Hz main loop. NOT 0.008 (125Hz): measured (M3 cadence forensics, docs/native_core.md)
            # a 125Hz loop starves the present thread of CPU/scheduling under timeBeginPeriod(1),
            # producing a periodic ~30ms present hitch (steady stddev 0.23->4.39, one 177ms tick) in
            # pure passthrough -- i.e. it breaks the crown present-cadence invariant. 20ms input
            # latency is imperceptible for transport/seek/keyboard, so 0.02 stays. See also smoke_player
            # (no ui_tick) staying pristine on the same busy desktop, which localized the cause here.
            time.sleep(0.02)
        player.pump_messages()
        st_after = player.stats()
        seek_result = {
            "target": target, "actual": actual, "latency_ms": seek_latency_ms,
            "present_count_before": pc_before, "present_count_after": st_after["present_count"],
            "ticks_during_and_after": st_after["present_count"] - pc_before,
            "ai_hit_rate_after_recovery_window": st_after["ai_hit_rate"],
            "scheduler_stats_after": scheduler.get_stats(),
        }
        print(f"[seek-test] after {args.seek_observe_seconds:.0f}s recovery window: "
              f"ai_hit_rate={st_after['ai_hit_rate']:.3f} "
              f"present_count advanced by {seek_result['ticks_during_and_after']}", file=sys.stderr)

    correctness_result = None
    if args.correctness:
        correctness_result = run_correctness_check(args.video, scheduler)

    final_stats = player.stats()
    present_stats = player.present_stats()
    scheduler.stop()
    try:
        player.dump_present_trace(args.trace_out)
    except Exception as e:  # noqa: BLE001
        print(f"dump_present_trace failed: {e!r}", file=sys.stderr)
    player.close()

    out = {
        "quit_early": quit_early,
        "final_stats": final_stats,
        "present_stats_cumulative": present_stats,
        "scheduler_stats": scheduler.get_stats(),
        "seek_result": seek_result,
        "correctness_result": correctness_result,
        "trace_path": args.trace_out,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)
    print(f"\n== DONE == wrote {args.out}", file=sys.stderr)
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()
