# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Phase 0b spike for the web-streaming / offline-export feature: prove the transcode plumbing
# end-to-end with REAL frames from the native decode path (no AI yet -- the AI restore step is
# already validated independently via the Scheduler's correctness samples, and its output
# `final_bgr` is the same BGR-HWC-uint8 layout these frames produce):
#
#   Player.get_cuda_nv12_by_frame(n)        (d3d11va hard decode -> CUDA NV12, zero-copy)
#     -> wrap_nv12_cuda_buffer_as_tensor    (zero-copy CUDA tensor)
#     -> _nv12_to_bgr_hwc_gpu               (CUDA BGR HWC uint8)
#     -> .cpu().numpy()                     (one D2H copy per frame -- the documented I3
#                                            relaxation for the transcode/encode consumer)
#     -> ffmpeg.exe -c:v h264_nvenc         (NVENC, Nvidia-only)
#     -> HLS (.ts segments + m3u8)  and  MP4 (+faststart)
#
# Usage (dev venv):
#   .venv/Scripts/python.exe scripts/verify_transcode.py test_video.mp4 --frames 90
#
# This script is verification scaffolding; the reusable pieces (frame pull + ffmpeg encode) are
# promoted into python/sumu/webstream/ in the follow-up milestone.
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

import sumu_core  # noqa: E402
import torch  # noqa: E402
from sumu.ai.utils.cuda_dlpack import wrap_nv12_cuda_buffer_as_tensor  # noqa: E402
from sumu.ai.utils.video_utils import _nv12_to_bgr_hwc_gpu  # noqa: E402


def _startupinfo():
    if sys.platform != "win32":
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    return si


def _ffmpeg_cmd():
    exe = "ffmpeg"
    return [exe, "-hide_banner", "-y"]


def encode_bgr_frames(stream, width: int, height: int, fps: float,
                      out: str, mode: str, bitrate: str = "8M"):
    """Stream an iterable of BGR (H,W,3) uint8 numpy frames to ffmpeg h264_nvenc.

    `mode` is "hls" (segments + index.m3u8 under `out/`) or "mp4" (single `out` file).
    Returns (ffmpeg_returncode, stderr_tail)."""
    w, h = int(width), int(height)
    gop = max(1, int(round(fps * 2)))
    cmd = _ffmpeg_cmd()
    cmd += [
        "-f", "rawvideo", "-pix_fmt", "bgr24", "-s", f"{w}x{h}", "-r", f"{fps:.6f}",
        "-i", "pipe:0",
        "-c:v", "h264_nvenc", "-preset", "p4", "-b:v", bitrate, "-g", str(gop),
    ]
    if mode == "hls":
        os.makedirs(out, exist_ok=True)
        cmd += ["-f", "hls", "-hls_time", "2", "-hls_list_size", "0",
                "-hls_flags", "independent_segments", os.path.join(out, "index.m3u8")]
    elif mode == "mp4":
        cmd += ["-movflags", "+faststart", out]
    else:
        raise ValueError(f"bad mode {mode!r}")

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, startupinfo=_startupinfo())
    written = 0
    try:
        for frame in stream:
            data = frame.tobytes()
            proc.stdin.write(data)
            written += 1
            if proc.poll() is not None:
                err = proc.stderr.read().decode("utf-8", "replace")
                raise RuntimeError(f"ffmpeg exited early (rc={proc.returncode}) at frame "
                                   f"{written}:\n{err[-2000:]}")
        proc.stdin.close()
    except OSError as e:
        err = proc.stderr.read().decode("utf-8", "replace") if proc.poll() is not None else ""
        raise RuntimeError(f"pipe write failed at frame {written} ({e!r}); ffmpeg rc="
                           f"{proc.poll()} stderr tail:\n{err[-2000:]}") from e
    err = proc.stderr.read().decode("utf-8", "replace")
    rc = proc.wait()
    return rc, err, written


def pull_frames(player, n_frames: int, poll_timeout_s: float = 10.0, bt709: bool = True,
                full_range: bool = False):
    """Yield (frame_num, bgr_numpy_uint8) for frames 0..n_frames-1 via the native bridge.

    Converts NV12 -> BGR on the GPU, then a single D2H copy to host. Blocks up to
    poll_timeout_s for the decode thread to fill each frame slot."""
    deadline = time.monotonic() + poll_timeout_s
    for n in range(n_frames):
        while True:
            g = player.get_cuda_nv12_by_frame(n)
            if g["ready"]:
                break
            if time.monotonic() > deadline:
                raise TimeoutError(f"frame {n} not ready in {poll_timeout_s}s "
                                   f"(decode ring too shallow or decode stalled)")
            time.sleep(0.002)
        nv12 = wrap_nv12_cuda_buffer_as_tensor(g["dev_ptr"], g["width"], g["height"],
                                               g["pitch_bytes"])
        bgr = _nv12_to_bgr_hwc_gpu(nv12, g["height"], g["width"], bt709=bt709,
                                   full_range=full_range)
        yield n, bgr.cpu().numpy()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video", nargs="?", default=None)
    ap.add_argument("--frames", type=int, default=90)
    ap.add_argument("--out", default=None, help="output dir (default: temp)")
    ap.add_argument("--bitrate", default="8M")
    args = ap.parse_args()

    video = args.video or os.path.join(_REPO, "test_video.mp4")
    # Output goes under the repo (not %TEMP%): child ffmpeg must be able to write it, and the
    # dev sandbox only guarantees writes under the workspace root.
    out = args.out or os.path.join(_REPO, "scripts", "trace",
                                   "transcode_spike_" + str(os.getpid()))
    os.makedirs(out, exist_ok=True)

    print(f"== transcode spike == video={video} frames={args.frames} out={out}",
          file=sys.stderr)

    player = sumu_core.Player(320, 180, False)
    try:
        player.open(video)
        fps = float(player.fps())
        w, h = player.dims()
        fc = int(player.frame_count())
        print(f"== open == fps={fps:.4f} dims={w}x{h} frame_count={fc}", file=sys.stderr)

        n = min(args.frames, fc) if fc > 0 else args.frames

        def stream():
            for _fnum, bgr in pull_frames(player, n):
                yield bgr

        # MP4 first (simpler target), then HLS.
        t0 = time.perf_counter()
        mp4_out = os.path.join(out, "out.mp4")
        rc, err, written = encode_bgr_frames(stream(), w, h, fps, mp4_out, "mp4", args.bitrate)
        dt = time.perf_counter() - t0
        speed = ""
        for line in err.splitlines():
            if "speed=" in line:
                speed = line.strip()
        print(f"== mp4 == rc={rc} frames={written} wall={dt:.2f}s "
              f"({written / dt:.1f} fps incl D2H+convert+encode) {speed}", file=sys.stderr)

        t0 = time.perf_counter()
        hls_dir = os.path.join(out, "hls")
        rc2, err2, written2 = encode_bgr_frames(stream(), w, h, fps, hls_dir, "hls", args.bitrate)
        dt2 = time.perf_counter() - t0
        speed2 = ""
        for line in err2.splitlines():
            if "speed=" in line:
                speed2 = line.strip()
        print(f"== hls  == rc={rc2} frames={written2} wall={dt2:.2f}s "
              f"({written2 / dt2:.1f} fps incl D2H+convert+encode) {speed2}", file=sys.stderr)

        # ffprobe the mp4
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries",
             "stream=codec_name,width,height,r_frame_rate,nb_frames",
             "-of", "csv=p=0", mp4_out],
            capture_output=True, text=True, startupinfo=_startupinfo())
        print(f"== probe mp4 == {probe.stdout.strip()}", file=sys.stderr)

        hls_index = os.path.join(hls_dir, "index.m3u8")
        segs = [f for f in os.listdir(hls_dir) if f.endswith(".ts")]
        print(f"== hls files == index.m3u8 + {len(segs)} ts segments "
              f"({', '.join(sorted(segs))})", file=sys.stderr)
        if os.path.isfile(hls_index):
            print("-- index.m3u8 --", file=sys.stderr)
            print(open(hls_index, encoding="utf-8").read(), file=sys.stderr)
    finally:
        player.close()

    print(f"== done == outputs under {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
