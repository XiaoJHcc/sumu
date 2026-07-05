# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Spike 1 driver: generates torch CUDA RGBA8 frames and pushes them through the
# sumu_present.Presenter C++ extension, which bridges them into a D3D11 present-face
# texture with zero main-memory round trip (see src/presenter.cpp for the actual bridge).
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe spike1_driver.py [--seconds 50] [--fps 60]
#                                                          [--width 3840] [--height 2160]
#                                                          [--run 4k] [--maximized]

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import torch  # noqa: E402
import sumu_present  # noqa: E402


def make_frame(w, h, frame_idx, device):
    """Deterministic, visibly-animated RGBA8 (H,W,4) CUDA tensor: a moving vertical bar
    over a per-frame-shifting gradient background, entirely computed on GPU with torch
    ops (no host round trip on the hot path -- only .contiguous() may touch layout, not
    data location)."""
    # background: horizontal gradient that slowly cycles through frame_idx
    xs = torch.arange(w, device=device, dtype=torch.float32)
    shift = float(frame_idx % w)
    grad = ((xs + shift) % w) / float(w - 1) * 255.0  # (W,)
    bg = grad.to(torch.uint8).view(1, w, 1).expand(h, w, 3).clone()

    # moving vertical bar, bright white-ish, position cycles across width
    bar_w = max(4, w // 100)
    bar_x = frame_idx % w
    lo = bar_x
    hi = min(w, bar_x + bar_w)
    bg[:, lo:hi, :] = 255

    alpha = torch.full((h, w, 1), 255, device=device, dtype=torch.uint8)
    frame = torch.cat([bg, alpha], dim=2)  # (H,W,4) RGBA8
    return frame.contiguous()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=50.0)
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--width", type=int, default=3840)
    ap.add_argument("--height", type=int, default=2160)
    ap.add_argument("--run", type=str, default="4k")
    ap.add_argument("--maximized", action="store_true")
    ap.add_argument("--verify-frame", type=int, default=100)
    ap.add_argument("--trace-dir", type=str,
                     default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "trace"))
    args = ap.parse_args()

    assert torch.cuda.is_available(), "CUDA not available -- spike 1 requires a CUDA GPU + torch cu build"
    device = torch.device("cuda", 0)

    # Trigger torch's lazy CUDA runtime init / primary context creation for device 0
    # *before* constructing Presenter, to exercise the "torch touches CUDA first" ordering
    # (the reverse order is expected to work too -- primary contexts are refcounted
    # per-process-per-device singletons -- but this is the more realistic sumu ordering:
    # the AI/torch side of the pipeline is already alive before the presenter spins up).
    _ = torch.zeros(1, device=device)
    torch.cuda.synchronize()
    print(f"spike1: torch CUDA initialized, device={torch.cuda.get_device_name(0)}", file=sys.stderr)

    os.makedirs(args.trace_dir, exist_ok=True)

    presenter = sumu_present.Presenter(args.width, args.height, args.maximized)
    print(f"spike1: Presenter created {args.width}x{args.height} maximized={args.maximized}", file=sys.stderr)

    frame_budget_s = 1.0 / args.fps
    verify_result = None
    verify_frame_tensor = None

    t_start = time.perf_counter()
    frame_idx = 0
    next_deadline = t_start
    quit_requested = False

    while True:
        now = time.perf_counter()
        elapsed = now - t_start
        if elapsed >= args.seconds:
            print(f"spike1: reached --seconds {args.seconds}, stopping", file=sys.stderr)
            break

        presenter.pump_messages()
        if presenter.should_quit():
            print("spike1: window closed, stopping", file=sys.stderr)
            quit_requested = True
            break

        frame = make_frame(args.width, args.height, frame_idx, device)
        pitch_bytes = frame.stride(0) * frame.element_size()  # W*4 for contiguous RGBA8

        capture = (frame_idx == args.verify_frame)
        presenter.push_cuda_frame(frame.data_ptr(), args.width, args.height, pitch_bytes, capture)

        if capture:
            # independent CPU-side readback of the SAME tensor, for comparison against
            # what verify_readback() sees on the D3D11 side.
            verify_frame_tensor = frame.detach().cpu()
            verify_result = presenter.verify_readback()

        frame_idx += 1

        # pace to an absolute-origin schedule (not a relative Sleep delta) so a single
        # overshoot self-corrects instead of drifting -- same pacing strategy as spike 0.
        next_deadline = t_start + frame_idx * frame_budget_s
        while True:
            remaining = next_deadline - time.perf_counter()
            if remaining <= 0:
                break
            if remaining > 0.002:
                time.sleep(remaining - 0.001)
            else:
                pass  # busy-wait the last <2ms for precision

        if frame_idx % 300 == 0:
            print(f"spike1: {frame_idx} frames pushed, elapsed={elapsed:.1f}s", file=sys.stderr)

    presenter.pump_messages()

    # ---- verify_readback pixel comparison -------------------------------------------------
    if verify_result is not None and verify_result["captured"]:
        print("\nspike1: verify_readback comparison (torch tensor vs D3D11 backbuffer):", file=sys.stderr)
        all_pass = True
        for px in verify_result["pixels"]:
            x, y = px["x"], px["y"]
            expected = verify_frame_tensor[y, x, :].tolist()  # [r,g,b,a]
            got = [px["r"], px["g"], px["b"], px["a"]]
            diff = [abs(a - b) for a, b in zip(expected, got)]
            ok = all(d <= 1 for d in diff)
            all_pass = all_pass and ok
            print(f"  ({x:5d},{y:5d}) expected={expected} got={got} diff={diff} {'OK' if ok else 'MISMATCH'}",
                  file=sys.stderr)
        print(f"\nspike1: verify_readback: {'PASS' if all_pass else 'FAIL'}", file=sys.stderr)
    else:
        print("\nspike1: verify_readback: FAIL (frame never captured -- did the run reach "
              f"--verify-frame {args.verify_frame} before quitting?)", file=sys.stderr)
        all_pass = False

    # ---- timing stats -----------------------------------------------------------------------
    stats = presenter.stats()
    print("\nspike1: timing stats over the whole run:", file=sys.stderr)
    for key in ("sync_ms", "interop_ms", "frame_ms"):
        s = stats[key]
        print(f"  {key:12s} n={s['n']:6d} median={s['median_ms']:.4f}ms p99={s['p99_ms']:.4f}ms "
              f"max={s['max_ms']:.4f}ms mean={s['mean_ms']:.4f}ms", file=sys.stderr)

    trace_path = os.path.join(args.trace_dir, f"present_spike1_{args.run}.csv")
    presenter.present_trace_dump(trace_path)
    print(f"spike1: wrote present trace to {trace_path}", file=sys.stderr)

    presenter.close()

    print("\nspike1: SUMMARY", file=sys.stderr)
    print(f"  verify_readback: {'PASS' if all_pass else 'FAIL'}", file=sys.stderr)
    print(f"  interop (map+copy+unmap) median={stats['interop_ms']['median_ms']:.4f}ms "
          f"p99={stats['interop_ms']['p99_ms']:.4f}ms "
          f"(budget for {args.fps:.0f}fps = {1000.0/args.fps:.2f}ms)", file=sys.stderr)
    print(f"  trace_csv: {trace_path}", file=sys.stderr)

    return 0 if (all_pass and not quit_requested) else (0 if all_pass else 1)


if __name__ == "__main__":
    sys.exit(main())
