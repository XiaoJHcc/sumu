# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# NVENC encoder sink for the transcode pipeline: streams BGR (H,W,3) uint8 numpy frames over a
# rawvideo stdin pipe to `ffmpeg.exe -c:v h264_nvenc`, muxing to either HLS (live, .ts segments +
# index.m3u8) or MP4 (+faststart). Audio, when present, is pulled by ffmpeg directly from the
# source file/URL as a second input and muxed as AAC (decensor never touches audio).
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

# ffmpeg binary name. Production relies on ffmpeg.exe being on PATH (already a soft dependency:
# python/sumu/ai/utils/video_utils.py shells out to ffprobe); the frozen bundle stages a full
# ffmpeg.exe next to the executable (see docs/packaging.md).
FFMPEG = "ffmpeg"


def _startupinfo():
    if sys.platform != "win32":
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    return si


class EncoderError(RuntimeError):
    """ffmpeg failed to start or exited early / non-zero."""


class NvencEncoder:
    """One ffmpeg h264_nvenc process fed BGR frames over stdin.

    mode: "hls" -> segments + index.m3u8 under `out`; "mp4" -> single `out` file.
    `audio_source`: optional path/URL of the source media; its first audio stream is muxed as AAC.
    """

    def __init__(self, width: int, height: int, fps: float, mode: str, out: str,
                 bitrate: str = "8M", audio_source: str | None = None,
                 preset: str = "p4", gop_seconds: float = 2.0):
        if mode not in ("hls", "mp4"):
            raise ValueError(f"bad mode {mode!r}")
        w, h = int(width), int(height)
        gop = max(1, int(round(float(fps) * gop_seconds)))
        if mode == "hls":
            os.makedirs(out, exist_ok=True)

        cmd = [FFMPEG, "-hide_banner", "-y", "-loglevel", "warning"]
        if audio_source:
            cmd += ["-i", audio_source]
        cmd += [
            "-f", "rawvideo", "-pix_fmt", "bgr24", "-s", f"{w}x{h}", "-r", f"{float(fps):.6f}",
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
        try:
            self._proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                          stderr=subprocess.PIPE, startupinfo=_startupinfo())
        except OSError as e:
            raise EncoderError(f"failed to launch {FFMPEG}: {e!r}") from e

    @property
    def cmd(self) -> list[str]:
        return list(self._cmd)

    def write_bgr(self, arr) -> None:
        """Feed one BGR (H,W,3) uint8 numpy frame. Raises EncoderError if ffmpeg died."""
        if self._proc.poll() is not None:
            err = self._proc.stderr.read().decode("utf-8", "replace")
            raise EncoderError(
                f"ffmpeg exited early (rc={self._proc.returncode}) after {self._frames} frames:\n"
                f"{err[-2000:]}")
        try:
            self._proc.stdin.write(arr.tobytes())
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
