# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# One-off build-time measurement harness for the BasicVSR++ TensorRT sub-engines.
# Compiles all 6 sub-engines with the exact settings the on-demand prompt uses
# (optimization_level=5, max_clip_size=BASICVSRPP_TRT_MAX_CLIP_SIZE, workspace=free*0.95)
# so the [trt-timing] lines reflect real player behavior.
#
# Takes the restoration weights path as argv[1] so it can point at a throwaway COPY of
# the weights -- engines land next to that copy (<stem>_sub_engines/), leaving the real
# cached engines untouched. Not wired into the app; invoke directly:
#   PYTHONPATH=python python scripts/measure_trt_build.py /tmp/measure_weights/model.pth
import argparse
import sys
import time

import torch

from sumu.ai.models.basicvsrpp.inference import load_model
from sumu.ai.restorationpipeline import BASICVSRPP_TRT_MAX_CLIP_SIZE
from sumu.ai.restorationpipeline.basicvsrpp_sub_engines import compile_basicvsrpp_sub_engines
from sumu.pipeline import default_restoration_model_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("weights", nargs="?", default=None)
    parser.add_argument("--pin-vram", type=float, default=0.0,
                        help="GiB of VRAM to pin (same process) before compiling, to simulate "
                             "'compile while playing' memory pressure.")
    args = parser.parse_args()

    if not torch.cuda.is_available():
        print("CUDA not available; nothing to measure.", file=sys.stderr)
        return 1

    weights_path = args.weights or default_restoration_model_path()
    device = torch.device("cuda")
    fp16 = True

    # Same-process VRAM pin: allocating here (not in a separate nohup process) guarantees the
    # buffer lives for the whole compile, so get_workspace_size_bytes() sees the pressured free.
    pin_buf = None
    if args.pin_vram > 0:
        n = int(args.pin_vram * 1024**3 // 4)
        pin_buf = torch.empty(n, dtype=torch.float32, device=device).fill_(1.0)
        torch.cuda.synchronize()

    free, total = torch.cuda.mem_get_info()
    print(f"[measure] start weights={weights_path} pin_vram={args.pin_vram:.1f}GiB "
          f"free={free / 1024**3:.2f}GiB total={total / 1024**3:.2f}GiB "
          f"max_clip={BASICVSRPP_TRT_MAX_CLIP_SIZE} opt=5", flush=True)

    model = load_model(None, weights_path, device, fp16)

    t0 = time.perf_counter()
    compile_basicvsrpp_sub_engines(
        model=model, device=device, fp16=fp16, model_weights_path=weights_path,
        max_clip_size=BASICVSRPP_TRT_MAX_CLIP_SIZE, optimization_level=5,
    )
    dt = time.perf_counter() - t0
    print(f"[measure] done total_wall={dt:.1f}s", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
