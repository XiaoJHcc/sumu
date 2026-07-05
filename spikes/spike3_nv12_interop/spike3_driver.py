# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Spike 3 driver: exercises sumu_nv12interop.Interop (D3D11 decoded-NV12 -> CUDA -> torch,
# see src/interop.cpp for the actual bridge) end to end:
#
#   1. correctness: decode N frames through the GPU bridge + a GPU nv12->bgr colour convert
#      (adapted from lada-realtime's lada/utils/video_utils.py::_nv12_to_bgr_hwc_gpu -- same
#      math, just skipping the CPU numpy->cuda upload since our input is already a CUDA
#      tensor written straight by interop.next_frame_into()), and diffs each frame against an
#      independent PyAV CPU decode (frame.to_ndarray('bgr24')) of the SAME file. Reports MAE.
#   2. throughput: decode N>=300 frames through the full bridge+convert path back-to-back,
#      reporting fps and a stage-by-stage timing breakdown (decode / SRV setup / render blit /
#      CUDA map / CUDA copy / CUDA unmap / python-side colour convert).
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe spike3_driver.py --video d:/Git/sumu/test_video.mp4 --run 1080p
#   d:/Git/sumu/.venv/Scripts/python.exe spike3_driver.py --video d:/Git/sumu/test_video_4k.mp4 --run 4k

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
import torch  # noqa: E402
import av  # noqa: E402
import sumu_nv12interop  # noqa: E402


def nv12_to_bgr_gpu_from_cuda(nv12_cuda: torch.Tensor, h: int, w: int, bt709: bool, full_range: bool) -> torch.Tensor:
    """GPU nv12->bgr colour convert, adapted from lada-realtime's
    lada/utils/video_utils.py::_nv12_to_bgr_hwc_gpu. That function's input is a CPU numpy
    nv12 array (PyAV can't hand out NVDEC surfaces zero-copy) which it uploads with
    `torch.from_numpy(nv12).to(torch_device)`; here `nv12_cuda` is ALREADY a CUDA tensor
    (interop.next_frame_into() wrote straight into it, GPU-to-GPU), so that upload step is
    skipped and everything else -- fp16 math, nearest-upsampled chroma, BT.601/709 and
    limited/full range handling -- is identical. See that function's docstring for the
    rationale (MAE ~1 vs swscale bgr24, chosen to be AI-transparent)."""
    t = nv12_cuda
    y = t[:h, :].to(torch.float16)
    uv = t[h:, :].reshape(h // 2, w // 2, 2)
    uv = uv.repeat_interleave(2, dim=0).repeat_interleave(2, dim=1).to(torch.float16)
    u = uv[:, :, 0]
    v = uv[:, :, 1]
    kr, kb = (0.2126, 0.0722) if bt709 else (0.299, 0.114)
    kg = 1.0 - kr - kb
    if full_range:
        yv = y; uu = u - 128.0; vv = v - 128.0
    else:
        yv = (y - 16.0) * (255.0 / 219.0)
        uu = (u - 128.0) * (255.0 / 224.0)
        vv = (v - 128.0) * (255.0 / 224.0)
    r = yv + (2.0 * (1.0 - kr)) * vv
    b = yv + (2.0 * (1.0 - kb)) * uu
    g = yv - (2.0 * kr * (1.0 - kr) / kg) * vv - (2.0 * kb * (1.0 - kb) / kg) * uu
    return torch.stack([b, g, r], dim=2).clamp_(0, 255).to(torch.uint8)


def dist(name, samples):
    if not samples:
        print(f"  {name}: (no samples)", file=sys.stderr)
        return
    s = sorted(samples)
    n = len(s)
    mean = sum(s) / n
    print(f"  {name:14s} n={n:5d} median={s[n // 2]:8.4f} p99={s[min(n - 1, int(n * 0.99))]:8.4f} "
          f"max={s[-1]:8.4f} mean={mean:8.4f}", file=sys.stderr)


def print_cpp_stats(stats):
    for key in ("decode_ms", "srv_ms", "render_ms", "map_ms", "copy_ms", "unmap_ms", "total_ms"):
        s = stats[key]
        print(f"  {key:12s} n={s['n']:6d} median={s['median_ms']:.4f}ms p99={s['p99_ms']:.4f}ms "
              f"max={s['max_ms']:.4f}ms mean={s['mean_ms']:.4f}ms", file=sys.stderr)


def run_correctness(video, n_verify, device):
    print(f"\n=== correctness ({n_verify} frames) : {video} ===", file=sys.stderr)

    interop = sumu_nv12interop.Interop()
    interop.open(video)
    w, h = interop.width(), interop.height()
    print(f"decoder: {w}x{h} fps={interop.fps():.3f} frame_count={interop.frame_count()}", file=sys.stderr)

    container = av.open(video)
    container.streams.video[0].thread_type = "AUTO"
    ref_gen = container.decode(video=0)

    maes = []
    max_mae = 0.0
    bt709 = None
    full_range = None

    for i in range(n_verify):
        nv12_buf = torch.empty((h * 3 // 2, w), dtype=torch.uint8, device=device)
        interop.next_frame_into(nv12_buf.data_ptr(), nv12_buf.stride(0))

        ref_frame = next(ref_gen)
        if bt709 is None:
            bt709 = (getattr(ref_frame, "colorspace", None) == 1)      # AVCOL_SPC_BT709
            # NOTE: AVColorRange is AVCOL_RANGE_UNSPECIFIED=0, AVCOL_RANGE_MPEG=1 ("tv",
            # limited), AVCOL_RANGE_JPEG=2 ("pc", full) -- verified against libavutil/pixfmt.h
            # and cross-checked with `ffprobe -show_entries stream=color_range` on this file
            # (reports "tv" while frame.color_range==1). An earlier draft of this driver
            # (mirroring a comment in lada-realtime's video_utils.py that mislabels value 1 as
            # AVCOL_RANGE_JPEG) used `== 1` here, which silently selected the WRONG (full-range)
            # transfer curve for this limited-range content -- inflating MAE from ~1 to ~6-8.
            full_range = (getattr(ref_frame, "color_range", None) == 2)  # AVCOL_RANGE_JPEG
            print(f"colour tags from stream: bt709={bt709} full_range={full_range}", file=sys.stderr)

        bgr_gpu = nv12_to_bgr_gpu_from_cuda(nv12_buf, h, w, bt709, full_range)
        got = bgr_gpu.to(torch.float32).cpu().numpy()
        ref = ref_frame.to_ndarray(format="bgr24").astype(np.float32)

        if got.shape != ref.shape:
            raise RuntimeError(f"shape mismatch at frame {i}: got={got.shape} ref={ref.shape}")

        mae = float(np.mean(np.abs(got - ref)))
        maes.append(mae)
        max_mae = max(max_mae, mae)
        if i < 3 or i == n_verify - 1:
            print(f"  frame {i:4d}: MAE={mae:.4f}", file=sys.stderr)

    container.close()
    interop.close()

    mean_mae = sum(maes) / len(maes)
    print(f"correctness: mean_MAE={mean_mae:.4f} max_MAE={max_mae:.4f} over {len(maes)} frames", file=sys.stderr)
    return mean_mae, max_mae


def run_throughput(video, n_bench, device, trace_dir, run_label):
    print(f"\n=== throughput ({n_bench} frames) : {video} ===", file=sys.stderr)

    interop = sumu_nv12interop.Interop()
    interop.open(video)
    w, h = interop.width(), interop.height()
    print(f"decoder: {w}x{h} fps={interop.fps():.3f}", file=sys.stderr)

    convert_ms = []
    e2e_ms = []

    torch.cuda.synchronize()
    t_start = time.perf_counter()

    for i in range(n_bench):
        t0 = time.perf_counter()
        nv12_buf = torch.empty((h * 3 // 2, w), dtype=torch.uint8, device=device)
        meta = interop.next_frame_into(nv12_buf.data_ptr(), nv12_buf.stride(0))
        t1 = time.perf_counter()
        bgr_gpu = nv12_to_bgr_gpu_from_cuda(nv12_buf, h, w, True, False)
        torch.cuda.synchronize()
        t2 = time.perf_counter()

        convert_ms.append((t2 - t1) * 1000.0)
        e2e_ms.append((t2 - t0) * 1000.0)

        if (i + 1) % 100 == 0:
            print(f"  ... {i + 1}/{n_bench} frames (last pts={meta['pts_seconds']:.3f}s)", file=sys.stderr)

    t_end = time.perf_counter()
    interop.close()

    total_s = t_end - t_start
    fps = n_bench / total_s
    print(f"\nthroughput: {n_bench} frames in {total_s:.3f}s -> {fps:.2f} fps "
          f"(python-loop-synced end-to-end, includes decode+interop+colour-convert)", file=sys.stderr)

    print("\nC++ side stage timings (decode / SRV cache / render blit / CUDA map / CUDA copy / CUDA unmap):",
          file=sys.stderr)
    print_cpp_stats(interop.stats())

    print("\npython-side colour-convert timing:", file=sys.stderr)
    dist("convert_ms", convert_ms)
    dist("e2e_ms (interop+convert)", e2e_ms)

    os.makedirs(trace_dir, exist_ok=True)
    trace_path = os.path.join(trace_dir, f"spike3_throughput_{run_label}.csv")
    with open(trace_path, "w") as f:
        f.write("frame_idx,e2e_ms,convert_ms\n")
        for i, (e2e, conv) in enumerate(zip(e2e_ms, convert_ms)):
            f.write(f"{i},{e2e:.4f},{conv:.4f}\n")
    print(f"wrote per-frame trace to {trace_path}", file=sys.stderr)

    return fps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", type=str, required=True)
    ap.add_argument("--run", type=str, default="run")
    ap.add_argument("--n-verify", type=int, default=60)
    ap.add_argument("--n-bench", type=int, default=300)
    ap.add_argument("--skip-correctness", action="store_true")
    ap.add_argument("--skip-throughput", action="store_true")
    ap.add_argument("--trace-dir", type=str,
                     default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "trace"))
    args = ap.parse_args()

    assert torch.cuda.is_available(), "spike3 requires a CUDA GPU + torch cu build"
    device = torch.device("cuda", 0)
    _ = torch.zeros(1, device=device)
    torch.cuda.synchronize()
    print(f"spike3: torch CUDA initialized, device={torch.cuda.get_device_name(0)}", file=sys.stderr)

    mean_mae = max_mae = None
    fps = None

    if not args.skip_correctness:
        mean_mae, max_mae = run_correctness(args.video, args.n_verify, device)

    if not args.skip_throughput:
        fps = run_throughput(args.video, args.n_bench, device, args.trace_dir, args.run)

    print("\nspike3: SUMMARY", file=sys.stderr)
    print(f"  video: {args.video}  run: {args.run}", file=sys.stderr)
    if mean_mae is not None:
        print(f"  correctness: mean_MAE={mean_mae:.4f} max_MAE={max_mae:.4f} (n={args.n_verify})", file=sys.stderr)
    if fps is not None:
        print(f"  throughput: {fps:.2f} fps (n={args.n_bench}, synced end-to-end)", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
