# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# 原片直出 (passthrough) HLS transcoder for the web-streaming server: the acceptance-path
# "no AI" mode that fixes the three blockers of the AI live-transcode v1 --
#
#   1. 色彩 (color): ffmpeg decodes the source to YUV and NVENC re-encodes YUV directly. There is
#      NO NV12->BGR (torch, BT.709) -> YUV (swscale, BT.601) round-trip, so colors match the
#      source exactly. (The AI path's color bug lived entirely in that double conversion.)
#   2. seek: seek = restart ffmpeg at a new `-ss` position (keyframe-accurate, ~2s). NVENC runs
#      ~4.5-6x realtime so a reposition resumes in ~1-2s -- the "reposition, not teardown" idea of
#      DESIGN.md I6 mapped onto the transcode process.
#   3. 停转 (no idle GPU burn): the process is killed on pause / on last-viewer-leave; the server's
#      idle sweeper additionally stops it if no client has fetched a segment for IDLE_TIMEOUT.
#
# Audio is pulled straight from the source by ffmpeg (muxed as AAC); it never touches sumu's AI
# or decode path. Each (re)start writes into its own position subdir with unique `s<nonce>.%05d.ts`
# segment names so a seek can never collide with still-browser-cached segment URLs of the previous
# position.
from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time

FFMPEG = "ffmpeg"

# GOP in seconds (segment / seek granularity). Matches the encoder.py default.
GOP_SECONDS = 2.0

# How long a passthrough session may sit with no segment/m3u8 fetch before the server's sweeper
# stops its ffmpeg process (safety net beyond the client's explicit pause/close signals).
# Set well above any native HLS player's prefetch gap (~30s buffer) so a live client can never be
# swept mid-playback; only a genuinely dead client (crashed / closed without a signal) trips it.
IDLE_TIMEOUT = 120.0

# AI 去码 sessions burn the GPU (YOLO + BasicVSR), so a dead client must be swept much sooner than
# passthrough: a live HLS client fetches a segment every ~2s, so 30s is ~15x the fetch gap and
# still never trips during playback -- it only catches a browser that was killed without firing
# the /stop beacon. This directly addresses the "关了浏览器 GPU 还在烧" complaint.
AI_IDLE_TIMEOUT = 30.0


def _startupinfo():
    if sys.platform != "win32":
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    return si


def _probe(path: str) -> tuple[float, float]:
    """ffprobe `path` -> (duration_seconds, fps). Best-effort: any failure degrades to (0.0, 0.0)
    and the transcoder just omits the -g hint (ffmpeg then forces keyframes per HLS segment)."""
    try:
        p = subprocess.run(
            ["ffprobe", "-v", "error",
             "-show_entries", "format=duration",
             "-show_entries", "stream=codec_type,width,height,avg_frame_rate,disposition",
             "-of", "json", path],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            startupinfo=_startupinfo(), timeout=30, check=False,
        )
        if p.returncode != 0:
            return 0.0, 0.0
        data = json.loads(p.stdout.decode("utf-8", "replace"))
    except Exception:  # noqa: BLE001 -- probe failure must never block playback
        return 0.0, 0.0

    duration = 0.0
    try:
        dur = float((data.get("format") or {}).get("duration") or 0)
        if dur > 0:
            duration = dur
    except (TypeError, ValueError):
        duration = 0.0

    streams = data.get("streams") or []
    videos = [s for s in streams if s.get("codec_type") == "video"
              and not int((s.get("disposition") or {}).get("attached_pic") or 0)]
    videos.sort(key=lambda s: -(int(s.get("width") or 0) * int(s.get("height") or 0)))
    fps = 0.0
    if videos:
        raw = str(videos[0].get("avg_frame_rate") or "0")
        if "/" in raw:
            try:
                a, b = raw.split("/", 1)
                if float(b) != 0:
                    fps = float(a) / float(b)
            except (ValueError, ZeroDivisionError):
                fps = 0.0
        else:
            try:
                fps = float(raw)
            except ValueError:
                fps = 0.0
    return duration, fps


_run_counter = 0
_run_counter_lock = threading.Lock()


def _next_run_id() -> str:
    """Globally-unique-per-process run id: keys each session's output dir so a fresh app run never
    collides with stale `.ts`/m3u8 files left in the cache by a previous run (which caused a
    'restart -> black screen' because the old index.m3u8 was served before the new ffmpeg wrote)."""
    global _run_counter
    with _run_counter_lock:
        _run_counter += 1
        return f"{os.getpid()}_{_run_counter}"


class PassthroughSession:
    """One repositionable ffmpeg passthrough transcode for a single video.

    Threading contract: start()/seek()/stop()/out_dir/m3u8_path/status() are the only public
    entry points and are all guarded by `_lock`. The server serializes the *single* active
    session per video (one viewer at a time, as documented), so there is no multi-viewer
    arbitration here -- just safe reposition/stop under the lock.
    """

    def __init__(self, rel: str, video_path: str, base_dir: str, bitrate: str = "8M",
                 preset: str = "p4", hwaccel: str | None = None):
        self.rel = rel
        self.video_path = video_path
        self.base_dir = base_dir
        self.bitrate = bitrate
        self.preset = preset
        self.hwaccel = hwaccel  # e.g. "d3d11va", or None for software decode (default)
        self.duration = 0.0
        self.fps = 0.0
        self.start_seconds = 0.0
        self.error: str | None = None
        self._proc: subprocess.Popen | None = None
        self._out_dir: str | None = None
        self._log_path: str | None = None
        self._nonce = 0
        self._run_id = _next_run_id()
        self._lock = threading.Lock()
        self._meta_done = False
        self._last_activity = time.monotonic()
        # NVENC is cheap; keep the long idle window. (AI sessions override this with the shorter
        # AI_IDLE_TIMEOUT -- see ai_session.py.) The sweeper reads this per session.
        self.idle_timeout = IDLE_TIMEOUT
        # Monotonic "seek generation" stamped by the client's `_` cache-buster on each reposition.
        # Guards against an out-of-order stale playlist request (from a destroyed+reloaded player)
        # regressing the transcode back to an older position.
        self._last_seq = 0.0

    # ---- metadata -----------------------------------------------------------------

    def probe_meta(self) -> None:
        """Lazily ffprobe duration + fps once (idempotent; a benign re-probe is harmless)."""
        if self._meta_done:
            return
        self._meta_done = True
        self.duration, self.fps = _probe(self.video_path)

    # ---- position / output paths ---------------------------------------------------

    def _position_dir(self, seconds: float) -> str:
        # run_id makes the dir unique per session/run so a restart never reuses stale cache output.
        return os.path.join(self.base_dir, f"p{int(round(seconds * 1000)):07d}_{self._run_id}")

    @property
    def out_dir(self) -> str | None:
        with self._lock:
            return self._out_dir

    @property
    def m3u8_path(self) -> str | None:
        d = self.out_dir
        return os.path.join(d, "index.m3u8") if d else None

    # ---- lifecycle ------------------------------------------------------------------

    def start(self, start_seconds: float) -> str:
        """(Re)start ffmpeg at `start_seconds` (kills any running process first). Returns the
        absolute path of the (not-yet-written) index.m3u8 it will produce."""
        self.probe_meta()
        with self._lock:
            self._kill_locked()
            self.start_seconds = max(0.0, float(start_seconds))
            out_dir = self._position_dir(self.start_seconds)
            os.makedirs(out_dir, exist_ok=True)
            self._nonce += 1
            nonce = self._nonce

            gop = max(1, int(round(self.fps * GOP_SECONDS))) if self.fps > 0 else None
            cmd = self._build_cmd(self.start_seconds, nonce, gop)
            log_path = os.path.join(out_dir, "ffmpeg.log")
            self._out_dir = out_dir
            self._log_path = log_path
            self._last_activity = time.monotonic()
            self.error = None
            try:
                with open(log_path, "wb") as errf:
                    self._proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=errf,
                                                  cwd=out_dir, startupinfo=_startupinfo())
            except Exception as e:  # noqa: BLE001 -- a failed (re)start must not crash the request
                self._proc = None
                self.error = f"failed to start ffmpeg: {e!r}"
            return os.path.join(out_dir, "index.m3u8")

    def seek(self, seconds: float) -> str:
        """Reposition: stop the current process and restart at `seconds`. Returns the new m3u8
        path (same semantics as start())."""
        return self.start(seconds)

    def apply_seek(self, seconds: float, seq: float | None) -> str | None:
        """Reposition to `seconds` unless `seq` is older than the last applied seek (an
        out-of-order stale playlist request from a destroyed+reloaded player). Returns the new
        m3u8 path, or None when the request was rejected as stale."""
        with self._lock:
            if seq is not None and seq < self._last_seq:
                return None  # stale; keep the current (newer) position
            if seq is not None:
                self._last_seq = seq
        return self.start(seconds)

    def stop(self) -> None:
        """Kill the ffmpeg process (pause / last-viewer-left). Keeps already-written segments on
        disk; a later start()/seek() resumes from the requested position."""
        with self._lock:
            self._kill_locked()

    def touch(self) -> None:
        """Mark client activity (a segment/m3u8 fetch), resetting the idle timer."""
        self._last_activity = time.monotonic()

    def idle_seconds(self) -> float:
        return time.monotonic() - self._last_activity

    # ---- state ----------------------------------------------------------------------

    def running(self) -> bool:
        with self._lock:
            return self._proc is not None and self._proc.poll() is None

    def finished(self) -> bool:
        """True once ffmpeg has exited (transcoded to EOF or died)."""
        with self._lock:
            return self._proc is not None and self._proc.poll() is not None

    def refresh_error(self) -> None:
        """Populate self.error from ffmpeg's stderr log when it exited non-zero (best-effort)."""
        with self._lock:
            if self._proc is None or self._proc.poll() is None or self._proc.returncode == 0:
                return
            if self.error is None:
                try:
                    with open(self._log_path, "r", errors="replace") as f:
                        self.error = f.read()[-2000:]
                except OSError:
                    self.error = f"ffmpeg exited rc={self._proc.returncode}"

    def status(self) -> dict:
        self.refresh_error()
        with self._lock:
            rc = self._proc.poll() if self._proc is not None else None
        return {
            "rel": self.rel,
            "position": self.start_seconds,
            "duration": self.duration,
            "running": rc is None,
            "finished": rc is not None and rc == 0,
            "error": self.error,
            "rc": rc,
        }

    # ---- internals -------------------------------------------------------------------

    def _kill_locked(self) -> None:
        if self._proc is not None and self._proc.poll() is None:
            try:
                self._proc.kill()
                self._proc.wait(timeout=5)
            except Exception:  # noqa: BLE001 -- kill must never raise through a request
                pass
        self._proc = None

    def _build_cmd(self, start_seconds: float, nonce: int, gop: int | None) -> list[str]:
        cmd = [FFMPEG, "-hide_banner", "-y", "-loglevel", "warning"]
        if self.hwaccel:
            cmd += ["-hwaccel", self.hwaccel]
        if start_seconds > 0.0:
            # -ss BEFORE -i = fast keyframe seek (O(1), not decode-and-discard), so a deep seek
            # into a long file stays ~1-2s instead of proportional to position. Accuracy is the
            # GOP (~2s), which the client already tolerates as the "second-level" seek granularity.
            cmd += ["-ss", f"{start_seconds:.3f}"]
        cmd += ["-i", self.video_path]
        cmd += ["-map", "0:v:0", "-c:v", "h264_nvenc", "-preset", self.preset,
                "-b:v", self.bitrate]
        if gop:
            cmd += ["-g", str(gop)]
        cmd += ["-map", "0:a:0?", "-c:a", "aac"]
        cmd += ["-f", "hls", "-hls_time", "2",
                "-hls_playlist_type", "event", "-hls_list_size", "0",
                "-hls_flags", "independent_segments",
                # Relative segment pattern (cwd=out_dir) so the playlist lists bare basenames
                # like `s3.00000.ts`, which the server's segment route + ?token= injection expect.
                "-hls_segment_filename", f"s{nonce}.%05d.ts",
                "index.m3u8"]
        return cmd
