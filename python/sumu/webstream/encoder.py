# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# NVENC encoder sink for the transcode pipeline: converts BGR (H,W,3) uint8 numpy frames to
# YUV420p (BT.709 by default, the inverse of the decode side's NV12->BGR matrix) and streams that
# over a rawvideo stdin pipe to `ffmpeg.exe -c:v h264_nvenc`, muxing to either HLS (live, .ts
# segments + index.m3u8) or MP4 (+faststart). Audio, when present, is pulled by ffmpeg directly
# from the source file/URL as a second input (seeked with -ss to match a seeked video segment)
# and muxed as AAC (decensor never touches audio).
#
# NVENC is Nvidia-only (matching sumu's documented Nvidia-only target, RTX 4080 / CUDA 12.8).
# The one D2H copy per frame here is the documented, deliberate relaxation of DESIGN.md I3 for
# this separate transcode consumer -- the AI compute itself stays all-GPU.
#
# Verified by scripts/verify_transcode.py (Phase 0b): on this machine NVENC encodes ~4.5-6x
# realtime at 1080p30 and ~3.6x at 4K30, so encode is never the bottleneck (BasicVSR is).
from __future__ import annotations

import os
import subprocess
import sys

import numpy as np

# ffmpeg binary name. Production relies on ffmpeg.exe being on PATH (already a soft dependency:
# python/sumu/ai/utils/video_utils.py shells out to ffprobe); the frozen bundle stages a full
# ffmpeg.exe next to the executable (see docs/packaging.md).
FFMPEG = "ffmpeg"


def _bgr_to_yuv420(arr, bt709: bool = True, full_range: bool = False) -> np.ndarray:
    """Convert a full-range BGR (H,W,3) uint8 numpy frame to planar YUV420p (Y then U then V
    planes, H*W*3/2 bytes), using the SAME BT.601/BT.709 matrix and limited/full-range math as
    `_nv12_to_bgr_hwc_gpu` (its exact inverse). This fixes the AI path's colour bug: the old
    encoder fed bgr24 rawvideo to ffmpeg, which then used libswscale's BT.601 default to convert
    BGR->YUV, while the decode side converted NV12->BGR with BT.709 -- the double conversion
    shifted colour. Feeding YUV420p directly means ffmpeg/NVENC do NO RGB<->YUV matrix conversion
    (yuv420p->nv12 is a lossless chroma relayout), so the round trip is matrix-consistent."""
    r = arr[..., 2].astype(np.float32) / 255.0
    g = arr[..., 1].astype(np.float32) / 255.0
    b = arr[..., 0].astype(np.float32) / 255.0
    kr, kb = (0.2126, 0.0722) if bt709 else (0.299, 0.114)
    kg = 1.0 - kr - kb
    y = kr * r + kg * g + kb * b
    if full_range:
        yy = y * 255.0
        uu = ((b - y) / (2.0 - 2.0 * kb) + 0.5) * 255.0
        vv = ((r - y) / (2.0 - 2.0 * kr) + 0.5) * 255.0
    else:
        yy = 16.0 + 219.0 * y
        uu = 128.0 + 224.0 * (b - y) / (2.0 - 2.0 * kb)
        vv = 128.0 + 224.0 * (r - y) / (2.0 - 2.0 * kr)
    yy = np.clip(yy, 0.0, 255.0).round().astype(np.uint8)
    uu = np.clip(uu, 0.0, 255.0).round().astype(np.uint8)
    vv = np.clip(vv, 0.0, 255.0).round().astype(np.uint8)
    h, w = yy.shape
    # 4:2:0 box downsample of chroma (2x2 average), matching standard 4:2:0 subsampling.
    u = (uu[0:h:2, 0:w:2].astype(np.uint32) + uu[1:h:2, 0:w:2]
         + uu[0:h:2, 1:w:2] + uu[1:h:2, 1:w:2] + 2) >> 2
    v = (vv[0:h:2, 0:w:2].astype(np.uint32) + vv[1:h:2, 0:w:2]
         + vv[0:h:2, 1:w:2] + vv[1:h:2, 1:w:2] + 2) >> 2
    return np.concatenate([yy.ravel(), u.astype(np.uint8).ravel(),
                           v.astype(np.uint8).ravel()])


def _startupinfo():
    if sys.platform != "win32":
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    return si


class EncoderError(RuntimeError):
    """ffmpeg failed to start or exited early / non-zero."""


class NvencEncoder:
    """One ffmpeg h264_nvenc process fed BGR frames (converted to YUV420p) over stdin.

    mode: "hls" -> segments + index.m3u8 under `out`; "mp4" -> single `out` file.
    `audio_source`: optional path/URL of the source media; its first audio stream is muxed as AAC.
    `start_seconds`: seek the audio input to this offset (matches a seeked video segment).
    `bt709`/`full_range`: colour matrix/range for the BGR->YUV conversion + output tags.
    """

    def __init__(self, width: int, height: int, fps: float, mode: str, out: str,
                 bitrate: str = "8M", audio_source: str | None = None,
                 preset: str = "p4", gop_seconds: float = 2.0,
                 start_seconds: float = 0.0, bt709: bool = True,
                 full_range: bool = False):
        if mode not in ("hls", "mp4"):
            raise ValueError(f"bad mode {mode!r}")
        w, h = int(width), int(height)
        gop = max(1, int(round(float(fps) * gop_seconds)))
        if mode == "hls":
            os.makedirs(out, exist_ok=True)

        cmd = [FFMPEG, "-hide_banner", "-y", "-loglevel", "warning"]
        if audio_source:
            # Audio is a separate input whose PTS must align with the (0-based) rawvideo pipe's
            # PTS. -ss BEFORE -i seeks the audio input AND rebases its output timestamps to 0
            # (ffmpeg's default, no -copyts), so a seeked AI segment's audio starts at 0 alongside
            # the video pipe -- matching the video side's frame-exact seek in HeadlessDecode.
            if start_seconds > 0.0:
                cmd += ["-ss", f"{start_seconds:.3f}"]
            cmd += ["-i", audio_source]
        # Colour tags go on the rawvideo INPUT (before -i): the pipe frames carry no colour
        # metadata, so tagging the input makes ffmpeg/NVENC propagate primaries + transfer +
        # matrix + range into the output VUI. (As OUTPUT options only matrix/range take effect --
        # primaries/transfer stay "unknown" because the rawvideo input's own unknown values
        # override them.) The matrix choice matches the BGR->YUV conversion in write_frame().
        primaries = "bt709" if bt709 else "bt470bg"
        transfer = "bt709" if bt709 else "bt470bg"
        matrix = "bt709" if bt709 else "bt470bg"
        cmd += [
            "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", f"{w}x{h}",
            "-r", f"{float(fps):.6f}",
            "-color_primaries", primaries, "-color_trc", transfer,
            "-colorspace", matrix, "-color_range", "pc" if full_range else "tv",
            "-i", "pipe:0",
        ]
        if audio_source:
            # rawvideo is input #1; source audio is input #0's first audio stream (optional `?`).
            cmd += ["-map", "1:v:0", "-map", "0:a:0?"]
        else:
            cmd += ["-map", "0:v:0"]
        cmd += ["-c:v", "h264_nvenc", "-preset", preset, "-b:v", bitrate, "-g", str(gop)]
        if audio_source:
            cmd += ["-c:a", "aac", "-shortest"]
        if mode == "hls":
            cmd += ["-f", "hls", "-hls_playlist_type", "event", "-hls_time", "2",
                    "-hls_list_size", "0", "-hls_flags", "independent_segments",
                    os.path.join(out, "index.m3u8")]
        else:
            cmd += ["-movflags", "+faststart", out]

        self._mode = mode
        self._cmd = cmd
        self._frames = 0
        self._bt709 = bt709
        self._full_range = full_range
        try:
            self._proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                          stderr=subprocess.PIPE, startupinfo=_startupinfo())
        except OSError as e:
            raise EncoderError(f"failed to launch {FFMPEG}: {e!r}") from e

    @property
    def cmd(self) -> list[str]:
        return list(self._cmd)

    def write_frame(self, arr) -> None:
        """Convert one full-range BGR (H,W,3) uint8 numpy frame to YUV420p (BT.709/BT.601,
        matching the decode side's matrix) and feed it to ffmpeg. Raises EncoderError if ffmpeg
        died."""
        if self._proc.poll() is not None:
            err = self._proc.stderr.read().decode("utf-8", "replace")
            raise EncoderError(
                f"ffmpeg exited early (rc={self._proc.returncode}) after {self._frames} frames:\n"
                f"{err[-2000:]}")
        try:
            self._proc.stdin.write(_bgr_to_yuv420(arr, self._bt709, self._full_range).tobytes())
        except OSError as e:
            err = ""
            try:
                err = self._proc.stderr.read().decode("utf-8", "replace")
            except Exception:  # noqa: BLE001
                pass
            raise EncoderError(
                f"pipe write failed after {self._frames} frames ({e!r}); ffmpeg rc="
                f"{self._proc.poll()} stderr tail:\n{err[-2000:]}") from e
        self._frames += 1

    def finish(self) -> tuple[int, str]:
        """Close stdin, wait for ffmpeg, return (returncode, stderr)."""
        if self._proc.stdin:
            try:
                self._proc.stdin.close()
            except OSError:
                pass
        err = self._proc.stderr.read().decode("utf-8", "replace")
        rc = self._proc.wait()
        return rc, err

    def abort(self) -> None:
        """Kill ffmpeg (used on cancel); does not raise."""
        try:
            self._proc.kill()
        except Exception:  # noqa: BLE001
            pass
