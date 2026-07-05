# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Phase 4c AI-core port verification (照搬 lada AI compute core).
# Exercises the three ported items end-to-end and prints real numbers:
#   item 5: load_models  (TRT vs PyTorch, load time, warmup)
#   item 1: YOLO detection (Yolo11SegmentationModel) on a real 1080p frame + fps
#   item 3: BasicVSR++ restore (BasicvsrppMosaicRestorer) on a 60-frame clip, TRT + PyTorch
#
# Real frames: decoded one-shot with PyAV (CPU) purely as test input — this is NOT
# sumu's production decode path (that comes from the native decode head).
#
# Run:  PYTHONPATH=python .venv/Scripts/python.exe scripts/verify_ai_core.py [video]
import os
import sys
import time

import numpy as np
import torch

DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")
VIDEO = sys.argv[1] if len(sys.argv) > 1 else "test_video.mp4"


def decode_frames_bgr(path, n):
    """One-shot PyAV CPU decode -> list of (H,W,C) BGR uint8 numpy frames (test input only)."""
    import av
    frames = []
    with av.open(path) as container:
        for frame in container.decode(video=0):
            frames.append(frame.to_ndarray(format="bgr24"))
            if len(frames) >= n:
                break
    return frames


def main():
    print(f"== env == torch {torch.__version__}  device {DEVICE} "
          f"{torch.cuda.get_device_name(0) if torch.cuda.is_available() else ''} "
          f"cc {torch.cuda.get_device_capability(0) if torch.cuda.is_available() else '-'}")

    from sumu.ai import ModelFiles
    from sumu.ai.restorationpipeline import load_models

    rp = ModelFiles.get_restoration_model_by_name("basicvsrpp-v1.2").path
    dp = ModelFiles.get_detection_model_by_name("v4-fast").path
    fp16 = DEVICE.type == "cuda"

    t0 = time.time()
    det, res, pad = load_models(DEVICE, "basicvsrpp-v1.2", rp, None, dp,
                                fp16=fp16, detect_face_mosaics=False, allow_trt_compile=True)
    print(f"\n== item5 load_models == {time.time()-t0:.2f}s  pad={pad}")
    trt_on = res._split_forward is not None
    print(f"   restoration TRT path: {trt_on} ({type(res._split_forward).__name__ if trt_on else 'PyTorch'})")
    print(f"   detection: {type(det).__name__} imgsz={det.imgsz}")

    # ---- item 1: YOLO ----
    print("\n== item1 YOLO ==")
    frames = decode_frames_bgr(VIDEO, 64)
    print(f"   decoded {len(frames)} real frames from {VIDEO}, shape {frames[0].shape}")
    frame0 = frames[0]

    # CPU-input path
    cpu_in = [torch.from_numpy(frame0)]
    pre = det.preprocess(cpu_in)
    res_cpu = det.inference_and_postprocess(pre, cpu_in)[0]
    nb = 0 if res_cpu.boxes is None else len(res_cpu.boxes)
    nm = 0 if res_cpu.masks is None else len(res_cpu.masks)
    print(f"   CPU-input : boxes={nb} masks={nm}")

    if DEVICE.type == "cuda":
        gpu_in = [torch.from_numpy(frame0).to(DEVICE)]
        pre_g = det.preprocess(gpu_in)  # exercises _preprocess_gpu + PyTorchLetterBox rebuild
        res_gpu = det.inference_and_postprocess(pre_g, gpu_in)[0]
        nbg = 0 if res_gpu.boxes is None else len(res_gpu.boxes)
        nmg = 0 if res_gpu.masks is None else len(res_gpu.masks)
        print(f"   GPU-input : boxes={nbg} masks={nmg}  (letterbox={type(det.letterbox).__name__})")

        # fps over N frames, batch=4 (realtime detector batch), GPU-resident input
        N = 64
        batch = 4
        gframes = [torch.from_numpy(frames[i % len(frames)]).to(DEVICE) for i in range(N)]
        torch.cuda.synchronize(); t = time.time()
        for i in range(0, N, batch):
            b = gframes[i:i+batch]
            det.inference_and_postprocess(det.preprocess(b), b)
        torch.cuda.synchronize()
        el = time.time() - t
        print(f"   fps (GPU, batch={batch}, {N} frames): {N/el:.1f} fps ({1000*el/N:.2f} ms/frame)")

    # ---- item 3: BasicVSR++ ----
    print("\n== item3 BasicVSR++ ==")
    T = 60
    clip = [torch.randint(0, 256, (256, 256, 3), dtype=torch.uint8) for _ in range(T)]

    def bench_restore(restorer, label, iters=3):
        # warm
        out = restorer.restore(clip)
        if DEVICE.type == "cuda":
            torch.cuda.synchronize()
        t = time.time()
        for _ in range(iters):
            out = restorer.restore(clip)
        if DEVICE.type == "cuda":
            torch.cuda.synchronize()
        el = (time.time() - t) / iters
        o = out[0]
        stacked = torch.stack(out)
        print(f"   {label}: {el*1000:.1f} ms/clip ({T} frames, {el*1000/T:.2f} ms/frame) "
              f"| out len={len(out)} shape={tuple(o.shape)} dtype={o.dtype} "
              f"range=[{int(stacked.min())},{int(stacked.max())}]")
        return out

    if trt_on:
        bench_restore(res, "TRT   ")
        # Force PyTorch path on the same loaded model for comparison
        saved = res._split_forward
        res._split_forward = None
        try:
            bench_restore(res, "PyTorch")
        finally:
            res._split_forward = saved
    else:
        bench_restore(res, "PyTorch")

    print("\nOK")


if __name__ == "__main__":
    main()
