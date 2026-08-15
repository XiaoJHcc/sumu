# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# End-to-end verification of the transcode pipeline: HeadlessDecode -> DecensorProcessor
# (YOLO + BasicVSR decensor) -> NvencEncoder -> MP4 / HLS, on a short clip of test_video.mp4.
# The full movie is too slow for a smoke test (BasicVSR is the bottleneck), so this extracts a
# few seconds and asserts: output is h264 at the source resolution, the right number of frames
# came out, and the AI actually restored >= 1 clip (test_video.mp4 is fully mosaic, so zero
# restored clips means the decensor never ran).
#
# Usage (dev venv):
#   .venv/Scripts/python.exe scripts/verify_transcode_ai.py --seconds 8
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "python"))
sys.path.insert(0, os.path.join(_REPO, "python", "sumu"))

import torch  # noqa: E402


def _startupinfo():
    if sys.platform != "win32":
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    return si


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", default=os.path.join(_REPO, "test_video.mp4"))
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--out", default=None)
    ap.add_argument("--clip-length", type=int, default=30)
    args = ap.parse_args()

    out = args.out or os.path.join(_REPO, "scripts", "trace", "transcode_ai")
    os.makedirs(out, exist_ok=True)
    clip = os.path.join(out, "clip.mp4")

    # Short clip (stream copy, starts at keyframe 0).
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", args.video,
         "-t", str(args.seconds), "-c", "copy", clip],
        check=True, startupinfo=_startupinfo())

    # Models (torch import is slow; load-only, no TRT compile).
    from sumu.pipeline import build_models  # noqa: E402
    from sumu.scheduler import SchedulerConfig  # noqa: E402
    from sumu.ai.utils.video_utils import get_video_meta_data  # noqa: E402

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    fp16 = device.type == "cuda"
    print(f"== load models == device={device} fp16={fp16}", file=sys.stderr)
    det_model, res_model, pad_mode = build_models(device, fp16, allow_trt_compile=False)

    meta = get_video_meta_data(clip)
    cfg = SchedulerConfig(clip_length=args.clip_length, max_regions_per_frame=1)

    from sumu.webstream import TranscodeEngine  # noqa: E402
    eng = TranscodeEngine(det_model, res_model, pad_mode, cfg)

    t0 = time.perf_counter()
    last = {"t": t0}

    def progress(fnum, fc):
        now = time.perf_counter()
        if now - last["t"] >= 1.0 or (fc and fnum >= fc - 1):
            last["t"] = now
            print(f"  emit {fnum + 1}/{fc if fc else '?'} frames", file=sys.stderr)

    mp4_out = os.path.join(out, "out.mp4")
    n = eng.run(clip, mp4_out, "mp4", progress_cb=progress)
    dt = time.perf_counter() - t0
    st = eng.last_stats or {}
    print(f"== mp4 == frames={n} wall={dt:.2f}s ({n / dt:.2f} fps) stats={st}",
          file=sys.stderr)

    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries",
         "stream=codec_name,width,height,nb_frames", "-of", "csv=p=0", mp4_out],
        capture_output=True, text=True, startupinfo=_startupinfo())
    print(f"== probe mp4 == {probe.stdout.strip()}", file=sys.stderr)

    # Colour metadata: the AI path must tag output as BT.709 (colour fix, see encoder.py).
    probe_color = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=color_space,color_primaries,color_transfer,color_range",
         "-of", "csv=p=0", mp4_out],
        capture_output=True, text=True, startupinfo=_startupinfo())
    print(f"== colour == {probe_color.stdout.strip()}", file=sys.stderr)

    # HLS
    t1 = time.perf_counter()
    hls_dir = os.path.join(out, "hls")
    n2 = eng.run(clip, hls_dir, "hls", progress_cb=progress)
    dt2 = time.perf_counter() - t1
    segs = [f for f in os.listdir(hls_dir) if f.endswith(".ts")]
    print(f"== hls == frames={n2} wall={dt2:.2f}s segments={len(segs)}", file=sys.stderr)

    # Seek: reposition (I6) to 4s and transcode to EOF -- expect fewer frames than the full run
    # but still >= 1 restored clip (test_video.mp4 is fully mosaic).
    t2 = time.perf_counter()
    seek_hls_dir = os.path.join(out, "hls_seek")
    n_seek = eng.run(clip, seek_hls_dir, "hls", progress_cb=progress, start_seconds=4.0)
    dt3 = time.perf_counter() - t2
    print(f"== hls(seek 4s) == frames={n_seek} wall={dt3:.2f}s", file=sys.stderr)

    ok = (
        n == n2 > 0
        and n_seek > 0
        and n_seek < n
        and "h264" in probe.stdout
        and str(meta.video_width) in probe.stdout
        and "bt709" in probe_color.stdout
        and st.get("clips_restored", 0) > 0
    )
    print(f"== RESULT == {'PASS' if ok else 'FAIL'}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
