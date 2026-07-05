# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Spike 2 driver: drives sumu_rt.RealtimePresenter, which runs its own C++ wall-clock
# present loop (decode thread + present thread, both inside the extension -- unlike spikes
# 0/1, this Python script does NOT pace the present loop itself). This script's only job is
# to simulate an AI producer: a Python thread that, for each content frame number, randomly
# either
#   - misses entirely (never calls push_ai_frame for that frame_num), or
#   - produces "fast" (often before the present head reaches that frame_num -> AI hit), or
#   - produces "slow" (usually after the present head has already moved past -> miss,
#     wasted push, exercises the late-arrival path),
# each time pushing a torch CUDA RGBA8 tensor with an obvious cyan tint + moving white bar
# so an "AI frame" is visually distinguishable on screen from a passthrough (real decoded
# NV12->RGB) frame, without needing to actually run any AI model in this spike.
#
# Two rounds are what actually answers spike 2's question (see docs/spike_results.md):
#   --mode mixed        : producer thread on, ready-map randomly populated/holed.
#   --mode passthrough  : producer thread off, ready-map always empty, pure fallback path.
# The present-cadence distributions of these two runs must be statistically indistinguishable
# for the "player is master, AI is a servant that never causes a hitch" model to hold.
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe spike2_driver.py --mode mixed --seconds 50 --run mixed
#   d:/Git/sumu/.venv/Scripts/python.exe spike2_driver.py --mode passthrough --seconds 50 --run passthrough

import argparse
import os
import random
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import torch  # noqa: E402
import sumu_rt  # noqa: E402


def make_ai_frame(w, h, frame_num, device):
    """A torch CUDA RGBA8 (H,W,4) frame with an obvious cyan tint (so it reads as
    "AI/dehazed" on screen, distinct from the passthrough NV12->RGB video content) plus a
    moving white bar so motion/liveness is visible even while the AI source is showing."""
    bg = torch.zeros(h, w, 3, device=device, dtype=torch.uint8)
    bg[..., 1] = 200  # G
    bg[..., 2] = 200  # B -- flat cyan-ish tint
    bar_w = max(4, w // 100)
    bar_x = frame_num % w
    lo = bar_x
    hi = min(w, bar_x + bar_w)
    bg[:, lo:hi, :] = 255
    alpha = torch.full((h, w, 1), 255, device=device, dtype=torch.uint8)
    frame = torch.cat([bg, alpha], dim=2)
    return frame.contiguous()


def producer_loop(presenter, width, height, fps, seconds, stop_event, device, seed, stats_out):
    """Simulates an AI worker whose output readiness is randomly fast/slow/missing relative
    to each content frame's ideal deadline (frame_num / fps from t_start). Delay is bounded
    (well under the ring buffer's ~3s capacity) and anchored to a FIXED ideal_time per
    frame_num -- not accumulated -- so lateness never grows unbounded over a long run; a
    late push is simply wasted (the present head has already moved on and never looks
    backwards), which is exactly the real-world "AI dropped this one" case.

    t_start is anchored to presenter.start_time_s() -- the presenter's ACTUAL frame-0
    wall-clock instant (on the same QueryPerformanceCounter-backed clock as Python's
    time.perf_counter() on Windows) -- rather than to time.perf_counter() captured fresh
    when this thread starts. The latter is always somewhat later than the presenter's real
    start (constructor setup + decode buffering already happened before Python got control
    back), which silently shifts every "fast" push late enough to miss far more often than
    intended. See RealtimePresenter::start_time_s() in presenter.cpp for the full story.

    IMPORTANT queueing note: this loop is a single sequential "AI worker" that visits every
    frame_num in order and, for each one, waits until that frame_num's own absolute target
    (t_start + frame_num/fps + delay) before pushing. Diagnostic runs showed this behaves
    like a G/G/1 queue, not like independent per-frame jitter: because each frame's target is
    an INDEPENDENT random draw rather than a small perturbation of the previous frame's
    target, a wide delay distribution (originally slow = uniform(20,150)ms against a 16.7ms
    frame period) creates a large variance in consecutive-target spacing, and Lindley's
    recursion (W_i = max(0, W_i-1 - (target_i - target_i-1))) then produces a stationary
    *average* backlog on the order of variance / (2 * mean drift) -- empirically ~70-120ms
    here, even though no single delay draws that high and the model never "accumulates"
    lateness in the sense the original comment above (kept for the non-cascading intent)
    assumed. Narrowing the slow bucket's range (below) keeps the intended semantics --
    fast=usually-hits, slow=usually-misses, missing=never-tried -- while keeping consecutive-
    target variance low enough that this queueing effect stays small relative to the frame
    period, so ai_hit_rate actually reflects the fast/slow/missing mix instead of being
    dominated by this artifact of sequential simulation.
    """
    rng = random.Random(seed)
    t_start = presenter.start_time_s()
    frame_num = 0
    n_missing = 0
    n_fast = 0
    n_slow = 0
    n_pushed = 0
    total_frames_estimate = int(seconds * fps) + int(fps)  # small margin past the run length

    while not stop_event.is_set() and frame_num < total_frames_estimate:
        if time.perf_counter() - t_start > seconds + 1.0:
            break

        ideal_time = frame_num / fps
        r = rng.random()
        if r < 0.30:
            # missing forever: this content frame's AI output never arrives.
            n_missing += 1
            frame_num += 1
            continue
        elif r < 0.70:
            # fast: usually ready before or right around the deadline -> AI hit.
            delay = rng.uniform(-0.060, 0.005)
            n_fast += 1
        else:
            # slow: ready after the present head has already moved past, usually a miss.
            # Kept narrow (see the queueing note above) -- a few frame periods late is
            # already enough to miss reliably; a much wider tail just inflates the
            # sequential-queueing backlog without adding anything to what's being tested.
            delay = rng.uniform(0.020, 0.050)
            n_slow += 1

        target = t_start + ideal_time + delay
        # Plain time.sleep(target-now) was measured (see spike2 diagnostics) to overshoot by
        # tens of ms on this machine under concurrent present/decode-thread load -- OS
        # scheduler/timer jitter, not anything wrong with the interop path itself (make_ai_frame
        # and push_ai_frame each measured sub-millisecond). That jitter silently swallowed most
        # of the intended "fast" bucket's margin before the AI frame ever reached push_ai_frame,
        # tanking ai_hit_rate for reasons unrelated to the actual mixing/ready-map logic under
        # test. Use the same hybrid sleep+spin precision technique as the C++ present loop
        # (spikes 0/2) to keep the simulated delay categories meaningful.
        now = time.perf_counter()
        coarse_budget = target - now - 0.003
        if coarse_budget > 0:
            time.sleep(coarse_budget)
        while True:
            if stop_event.is_set():
                break
            now = time.perf_counter()
            if now >= target:
                break
        if stop_event.is_set():
            break

        t_before_make = time.perf_counter()
        frame = make_ai_frame(width, height, frame_num, device)
        pitch_bytes = frame.stride(0) * frame.element_size()
        t_before_push = time.perf_counter()
        try:
            presenter.push_ai_frame(frame_num, frame.data_ptr(), width, height, pitch_bytes)
            n_pushed += 1
            done = time.perf_counter()
            lateness_ms = (done - (t_start + ideal_time)) * 1000.0
            stats_out.setdefault("_lateness_samples", []).append(lateness_ms)
            stats_out.setdefault("_make_ms_samples", []).append((t_before_push - t_before_make) * 1000.0)
            stats_out.setdefault("_push_ms_samples", []).append((done - t_before_push) * 1000.0)
        except Exception as e:  # noqa: BLE001
            print(f"spike2: push_ai_frame failed for frame {frame_num}: {e}", file=sys.stderr)

        frame_num += 1

    stats_out["n_missing"] = n_missing
    stats_out["n_fast"] = n_fast
    stats_out["n_slow"] = n_slow
    stats_out["n_pushed"] = n_pushed
    stats_out["frame_num_reached"] = frame_num


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", type=str, default="d:/Git/sumu/test_video_4k.mp4")
    ap.add_argument("--width", type=int, default=3840)
    ap.add_argument("--height", type=int, default=2160)
    ap.add_argument("--seconds", type=float, default=50.0)
    ap.add_argument("--mode", choices=["mixed", "passthrough"], default="mixed")
    ap.add_argument("--run", type=str, default="mixed")
    ap.add_argument("--maximized", action="store_true")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--trace-dir", type=str,
                     default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "trace"))
    args = ap.parse_args()

    assert torch.cuda.is_available(), "CUDA not available -- spike 2 requires a CUDA GPU + torch cu build"
    device = torch.device("cuda", 0)
    _ = torch.zeros(1, device=device)
    torch.cuda.synchronize()
    print(f"spike2: torch CUDA initialized, device={torch.cuda.get_device_name(0)}", file=sys.stderr)

    os.makedirs(args.trace_dir, exist_ok=True)

    presenter = sumu_rt.RealtimePresenter(args.video, args.width, args.height, args.maximized)
    fps = presenter.fps()
    print(f"spike2: Presenter created {args.width}x{args.height} fps={fps:.3f} mode={args.mode} "
          f"run={args.run} seconds={args.seconds}", file=sys.stderr)

    stop_event = threading.Event()
    producer_stats = {}
    producer_thread = None
    if args.mode == "mixed":
        producer_thread = threading.Thread(
            target=producer_loop,
            args=(presenter, args.width, args.height, fps, args.seconds, stop_event, device, args.seed, producer_stats),
            daemon=True,
        )
        producer_thread.start()
        print("spike2: AI producer thread started (mixed mode)", file=sys.stderr)
    else:
        print("spike2: AI producer NOT started (passthrough-only mode) -- ready-map stays empty", file=sys.stderr)

    t_start = time.perf_counter()
    quit_requested = False
    while True:
        elapsed = time.perf_counter() - t_start
        if elapsed >= args.seconds:
            print(f"spike2: reached --seconds {args.seconds}, stopping", file=sys.stderr)
            break
        presenter.pump_messages()
        if presenter.should_quit():
            print("spike2: window closed, stopping", file=sys.stderr)
            quit_requested = True
            break
        time.sleep(0.05)

    stop_event.set()
    if producer_thread is not None:
        producer_thread.join(timeout=3.0)

    presenter.pump_messages()

    stats = presenter.stats()
    hit_rate = presenter.ai_hit_rate()
    print("\nspike2: presenter stats:", file=sys.stderr)
    for k in ("present_count", "decode_frame_count", "ai_push_count",
              "n_ai_fresh", "n_ai_stale", "n_pt_fresh", "n_pt_stale"):
        print(f"  {k:20s} = {stats[k]}", file=sys.stderr)
    print(f"  ai_hit_rate          = {hit_rate:.4f}", file=sys.stderr)

    if producer_stats:
        print("\nspike2: producer stats:", file=sys.stderr)
        for k, v in producer_stats.items():
            if k.startswith("_"):
                continue
            print(f"  {k:20s} = {v}", file=sys.stderr)

        def _dist(name, samples):
            if not samples:
                return
            s = sorted(samples)
            n = len(s)
            print(f"  {name}: n={n} min={s[0]:.2f} p50={s[n//2]:.2f} p90={s[int(n*0.9)]:.2f} max={s[-1]:.2f}",
                  file=sys.stderr)

        _dist("lateness_ms (push completion vs ideal_time, +=late)", producer_stats.get("_lateness_samples", []))
        _dist("make_ai_frame_ms", producer_stats.get("_make_ms_samples", []))
        _dist("push_ai_frame_ms", producer_stats.get("_push_ms_samples", []))

    trace_path = os.path.join(args.trace_dir, f"present_spike2_{args.run}.csv")
    presenter.dump_trace(trace_path)
    print(f"spike2: wrote present trace to {trace_path}", file=sys.stderr)

    presenter.close()

    print("\nspike2: SUMMARY", file=sys.stderr)
    print(f"  mode: {args.mode}  run: {args.run}", file=sys.stderr)
    print(f"  present_count={stats['present_count']} ai_hit_rate={hit_rate:.4f} "
          f"n_pt_stale={stats['n_pt_stale']} n_ai_stale={stats['n_ai_stale']}", file=sys.stderr)
    print(f"  trace_csv: {trace_path}", file=sys.stderr)

    return 0 if not quit_requested else 1


if __name__ == "__main__":
    sys.exit(main())
