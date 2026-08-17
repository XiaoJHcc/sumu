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
import re
import subprocess
import sys
from dataclasses import dataclass

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


def _bgr_to_yuv420_gpu(bgr, bt709: bool = True, full_range: bool = False):
    """GPU twin of `_bgr_to_yuv420`: convert a full-range BGR (H,W,3) uint8 CUDA tensor to the
    SAME planar YUV420p byte layout (flat uint8 tensor of H*W*3/2 elements) using the SAME
    matrix/range math, so the round trip stays matrix-consistent with the decode side's
    `_nv12_to_bgr_hwc_gpu`. The ~15-25ms-per-frame numpy float conversion above becomes a handful
    of CUDA kernels (~1ms) AND the D2H copy shrinks from 3 bytes/pixel (BGR) to 1.5 (YUV420) --
    both matter because the encoder feed runs on the transcode thread, and every ms it spends on
    CPU colour math is a ms the GPU sits idle between AI frames."""
    import torch
    r = bgr[..., 2].to(torch.float32) / 255.0
    g = bgr[..., 1].to(torch.float32) / 255.0
    b = bgr[..., 0].to(torch.float32) / 255.0
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
    yy = torch.clamp(yy, 0.0, 255.0).round_().to(torch.uint8)
    uu = torch.clamp(uu, 0.0, 255.0).round_().to(torch.uint8)
    vv = torch.clamp(vv, 0.0, 255.0).round_().to(torch.uint8)
    h, w = yy.shape
    # 4:2:0 box downsample of chroma (2x2 average, +2>>2 rounding) -- identical to the numpy
    # path above so a CPU/GPU split never shifts colour between the two code paths. Cast to int64
    # first: torch has no uint32<->uint8 promotion rule, and int64 keeps the +2>>2 sum exact.
    uu64 = uu.to(torch.int64)
    vv64 = vv.to(torch.int64)
    u = ((uu64[0:h:2, 0:w:2] + uu64[1:h:2, 0:w:2]
          + uu64[0:h:2, 1:w:2] + uu64[1:h:2, 1:w:2] + 2) >> 2).to(torch.uint8)
    v = ((vv64[0:h:2, 0:w:2] + vv64[1:h:2, 0:w:2]
          + vv64[0:h:2, 1:w:2] + vv64[1:h:2, 1:w:2] + 2) >> 2).to(torch.uint8)
    return torch.cat([yy.reshape(-1), u.reshape(-1), v.reshape(-1)])


def _is_cuda_tensor(x) -> bool:
    try:
        return bool(x.is_cuda)
    except AttributeError:
        return False


def _startupinfo():
    if sys.platform != "win32":
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    return si


class EncoderError(RuntimeError):
    """ffmpeg failed to start or exited early / non-zero."""


@dataclass
class EncodeOptions:
    """User-facing encode knobs for the offline-export presets. CQ / bitrate / maxrate are
    INDEPENDENT, each gated by its *_enabled flag -- matching NVENC's VBR rate control, where
    targetQuality (-cq), averageBitRate (-b:v) and maxBitRate (-maxrate) coexist (the user's bat
    uses all three together). Bitrates are int kbps. Serialized as plain dicts into settings.json
    (see settings.EXPORT_PRESET_DEFAULTS for the shipped presets)."""

    codec: str = "hevc"
    preset: str = "p7"            # NVENC preset p1..p7 (p7 = slowest / best quality)
    cq_enabled: bool = True
    cq: int = 33                  # 0..51 (NVENC targetQuality)
    bitrate_enabled: bool = False
    bitrate: int = 0              # kbps (averageBitRate)
    maxrate_enabled: bool = False
    maxrate: int = 0              # kbps (maxBitRate)
    audio_copy: bool = True
    audio_bitrate: int = 256      # kbps (audio_copy=False only)
    subtitle: bool = True

    def codec_ffmpeg(self) -> str:
        return "hevc_nvenc" if self.codec == "hevc" else "h264_nvenc"

    @classmethod
    def from_dict(cls, d: dict | None) -> "EncodeOptions":
        d = d or {}
        return cls(
            codec="h264" if d.get("codec") == "h264" else "hevc",
            preset=str(d.get("preset") or "p7"),
            cq_enabled=bool(d.get("cq_enabled", True)),
            cq=max(0, min(51, int(d.get("cq") or 0))),
            bitrate_enabled=bool(d.get("bitrate_enabled", False)),
            bitrate=max(0, int(d.get("bitrate") or 0)),
            maxrate_enabled=bool(d.get("maxrate_enabled", False)),
            maxrate=max(0, int(d.get("maxrate") or 0)),
            audio_copy=bool(d.get("audio_copy", True)),
            audio_bitrate=max(0, int(d.get("audio_bitrate") or 256)),
            subtitle=bool(d.get("subtitle", True)),
        )


def _parse_kbps(bitrate) -> int:
    """Parse a bitrate like "8M" / "8000k" / "1.3M" / "1300" (or a bare int kbps) → int kbps.
    Empty / unparseable → 0 (means "no bitrate"). Used by the live-streaming fallback, whose
    `bitrate` is a config string ("8M")."""
    if isinstance(bitrate, int):
        return max(0, bitrate)
    m = re.match(r"^\s*(\d+(?:\.\d+)?)\s*([kKmM]?)\s*$", str(bitrate or ""))
    if not m:
        return 0
    val = float(m.group(1))
    if m.group(2).lower() == "m":
        val *= 1000.0
    return max(0, int(round(val)))


class NvencEncoder:
    """One ffmpeg h264_nvenc process fed BGR frames (converted to YUV420p) over stdin.

    mode: "hls" -> segments + index.m3u8 under `out`; "mp4" -> single `out` file.
    `audio_source`: optional path/URL of the source media; its first audio stream is muxed as AAC.
    `start_seconds`: seek the audio input to this offset (matches a seeked video segment).
    `bt709`/`full_range`: colour matrix/range for the BGR->YUV conversion + output tags.
    """

    def __init__(self, width: int, height: int, fps: float, mode: str, out: str,
                 encode: "EncodeOptions | None" = None, quality_first: bool = False,
                 audio_source: str | None = None, gop_seconds: float = 2.0,
                 start_seconds: float = 0.0, bt709: bool = True,
                 full_range: bool = False):
        if mode not in ("hls", "mp4"):
            raise ValueError(f"bad mode {mode!r}")
        # Default = the live-streaming profile (h264 / 8 Mbps / p4 / AAC re-encode): browser HLS
        # wants h264 + AAC, and p4 keeps encode latency low. Offline export passes a quality-first
        # EncodeOptions (HEVC / CQ / p7 / copy) + quality_first=True instead.
        enc = encode if encode is not None else EncodeOptions(
            codec="h264", preset="p4", cq_enabled=False,
            bitrate_enabled=True, bitrate=8000,
            audio_copy=False, subtitle=False)
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
            # rawvideo is input #1; source audio is input #0's first audio stream (optional `?`);
            # subtitles are mapped + converted to mov_text (MP4-compatible) when enabled.
            cmd += ["-map", "1:v:0", "-map", "0:a:0?"]
            if enc.subtitle:
                # Standard MP4 subtitle copy: text subs (srt/ass) convert to mov_text. Bitmap subs
                # (PGS/VobSub) cannot become mov_text and make ffmpeg exit non-zero -- surfaced as
                # the item's error, and the user turns the subtitle toggle off to retry.
                cmd += ["-map", "0:s?"]
        else:
            cmd += ["-map", "0:v:0"]

        # Video encode: codec + preset + independent CQ/bitrate/maxrate knobs (each gated by its
        # *_enabled flag). NVENC VBR treats -cq (targetQuality), -b:v (averageBitRate) and
        # -maxrate (maxBitRate) as coexisting parameters, so any subset may be set together.
        cmd += ["-c:v", enc.codec_ffmpeg(), "-preset", enc.preset]
        if enc.codec == "hevc":
            # Apple/QuickTime compatibility tag (the user's bat habit; not set for H.264).
            cmd += ["-tag:v", "hvc1"]
        if enc.cq_enabled:
            cmd += ["-cq", str(int(enc.cq))]
        if enc.bitrate_enabled and enc.bitrate > 0:
            cmd += ["-b:v", f"{int(enc.bitrate)}k"]
        if enc.maxrate_enabled and enc.maxrate > 0:
            cmd += ["-maxrate", f"{int(enc.maxrate)}k"]
        if quality_first:
            # Offline-export "always-on" quality flags (deliberately NOT in the preset panel):
            # max spatial/temporal AQ, B-frame references, full lookahead, and the hq tune. Even
            # at maximum these cost far less than the AI decensor pipeline, so there is no reason
            # to leave quality headroom on the table for a non-realtime export.
            cmd += ["-tune", "hq", "-b_ref_mode", "middle",
                    "-spatial_aq", "1", "-temporal_aq", "1", "-rc-lookahead", "32"]
        cmd += ["-g", str(gop)]

        if audio_source:
            if enc.audio_copy:
                cmd += ["-c:a", "copy"]
            else:
                cmd += ["-c:a", "aac"]
                if quality_first:
                    cmd += ["-b:a", f"{int(enc.audio_bitrate)}k"]
            # -shortest ends the output at the shorter of the video pipe and the source audio, so
            # an audio-longer-than-video source doesn't leave a trailing audio-only tail. When
            # subtitles are mapped it also counts the subtitle stream; a sparse/short subtitle
            # track could truncate the tail (rare -- accepted; turn subtitles off to work around).
            cmd += ["-shortest"]
            if enc.subtitle:
                cmd += ["-c:s", "mov_text"]

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

    def write_frame(self, frame) -> None:
        """Feed one frame to ffmpeg. Accepts a full-range BGR (H,W,3) uint8 numpy array (CPU) or
        a full-range BGR (H,W,3) uint8 CUDA tensor. CUDA frames are converted to YUV420p ON THE
        GPU (see `_bgr_to_yuv420_gpu`) so the numpy float conversion never lands on the transcode
        thread; the single D2H copy + pipe write happen here. Raises EncoderError if ffmpeg died."""
        if self._proc.poll() is not None:
            err = self._proc.stderr.read().decode("utf-8", "replace")
            raise EncoderError(
                f"ffmpeg exited early (rc={self._proc.returncode}) after {self._frames} frames:\n"
                f"{err[-2000:]}")
        try:
            if _is_cuda_tensor(frame):
                yuv = _bgr_to_yuv420_gpu(frame, self._bt709, self._full_range)
                data = yuv.cpu().numpy().tobytes()
            else:
                data = _bgr_to_yuv420(frame, self._bt709, self._full_range).tobytes()
            self._proc.stdin.write(data)
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
