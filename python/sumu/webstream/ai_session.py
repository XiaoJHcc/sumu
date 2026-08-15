# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# AiStreamSession: the AI 去码 counterpart to passthrough.py's PassthroughSession -- one
# repositionable AI decensor transcode for a single video, with the SAME public interface
# (start/seek/apply_seek/stop/touch/idle_seconds/running/finished/status/out_dir/m3u8_path) so
# the web server routes the two modes through one code path. Backed by the SHARED TranscodeEngine
# (headless decode -> YOLO + BasicVSR -> NVENC -> HLS), so unlike passthrough there is at most ONE
# active transcode at a time (the shared AI models are GPU-bound) -- the server's busy gate
# enforces that.
#
# The three AI v1 blockers this fixes, mapped onto the transcode process (same ideas as
# passthrough):
#   1. 色彩 (colour): fixed in encoder.py -- BGR is converted back to YUV420p with the SAME
#      BT.709 matrix the decode side used (no BT.601 round-trip).
#   2. seek: seek = cancel the current transcode and restart it at the new position, using
#      HeadlessDecode.seek_to_frame (keyframe-accurate reposition, ~1-2s to re-anchor) + a fresh
#      DecensorProcessor (scene/clip state resets, matching DESIGN.md I6).
#   3. 停转: stop() cancels the shared engine and joins the worker; the server's idle sweeper
#      stops a session after AI_IDLE_TIMEOUT (short: GPU burn must stop promptly on browser close)
#      without a client fetch.
from __future__ import annotations

import os
import threading
import time

from .passthrough import AI_IDLE_TIMEOUT, _next_run_id, _probe


class AiStreamSession:
    """One repositionable AI decensor transcode for a single video (single-viewer, like
    PassthroughSession). All public entry points guard `_lock`; the engine run itself happens on
    a daemon worker thread so start()/seek() never block the HTTP request."""

    def __init__(self, rel: str, video_path: str, base_dir: str, engine,
                 bitrate: str = "8M"):
        self.rel = rel
        self.video_path = video_path
        self.base_dir = base_dir
        self.engine = engine
        self.bitrate = bitrate
        self.duration = 0.0
        self.fps = 0.0
        self.start_seconds = 0.0
        self.error: str | None = None
        self._out_dir: str | None = None
        self._thread: threading.Thread | None = None
        self._nonce = 0
        self._run_id = _next_run_id()
        self._lock = threading.Lock()
        self._meta_done = False
        self._last_activity = time.monotonic()
        # Monotonic "seek generation" stamped by the client's `_` cache-buster (same contract as
        # PassthroughSession): rejects out-of-order stale reposition requests.
        self._last_seq = 0.0
        self._frames = 0
        self._total = 0
        # AI transcode burns the GPU, so this session is swept after a shorter idle window than
        # passthrough (see passthrough.AI_IDLE_TIMEOUT) -- closing the browser must free the GPU
        # promptly even when the /stop beacon never fires (hard kill / crash).
        self.idle_timeout = AI_IDLE_TIMEOUT
        # Terminal state for the CURRENT position, written by the worker thread / stop().
        self._done = False        # worker thread exited (position terminal)
        self._finished = False    # transcoded to EOF successfully (ENDLIST written)
        # True while a reposition/stop is cancelling the worker, so its TranscodeError(cancel)
        # is NOT surfaced as a user-facing error.
        self._repositioning = False

    # ---- metadata ------------------------------------------------------------------

    def probe_meta(self) -> None:
        """Lazily ffprobe duration + fps once (idempotent)."""
        if self._meta_done:
            return
        self._meta_done = True
        self.duration, self.fps = _probe(self.video_path)

    # ---- position / output paths ---------------------------------------------------

    def _position_dir(self, seconds: float) -> str:
        # run_id (per session) + nonce (per start/seek) make each position's dir unique so a
        # restart/seek never collides with stale cache output of the previous position.
        return os.path.join(self.base_dir,
                            f"p{int(round(seconds * 1000)):07d}_{self._run_id}_{self._nonce}")

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
        """(Re)start the AI transcode at `start_seconds` (cancels + joins any running worker).
        Returns the absolute path of the (not-yet-written) index.m3u8 it will produce."""
        self.probe_meta()
        self._cancel_previous()
        with self._lock:
            self.start_seconds = max(0.0, float(start_seconds))
            self._nonce += 1
            out_dir = self._position_dir(self.start_seconds)
            os.makedirs(out_dir, exist_ok=True)
            self._out_dir = out_dir
            self._done = False
            self._finished = False
            self.error = None
            self._frames = 0
            self._total = 0
            self._last_activity = time.monotonic()
            self._thread = threading.Thread(target=self._run, name="sumu-stream-ai", daemon=True)
        self._thread.start()
        return os.path.join(out_dir, "index.m3u8")

    def seek(self, seconds: float) -> str:
        """Reposition: same semantics as start() (cancel current + restart at `seconds`)."""
        return self.start(seconds)

    def apply_seek(self, seconds: float, seq: float | None) -> str | None:
        """Reposition to `seconds` unless `seq` is older than the last applied seek."""
        with self._lock:
            if seq is not None and seq < self._last_seq:
                return None
            if seq is not None:
                self._last_seq = seq
        return self.start(seconds)

    def stop(self) -> None:
        """Cancel the current transcode and wait for the worker to exit (frees the shared GPU).
        Keeps already-written segments on disk; a later start()/seek() resumes."""
        self._cancel_previous()

    def touch(self) -> None:
        self._last_activity = time.monotonic()

    def idle_seconds(self) -> float:
        return time.monotonic() - self._last_activity

    def _cancel_previous(self) -> None:
        """Cancel the shared engine and join the prior worker, WITHOUT holding `_lock` during the
        join (the worker's finally acquires `_lock` to set `_done` -- holding it would deadlock)."""
        with self._lock:
            t = self._thread
            self._repositioning = True
        self.engine.cancel()
        if t is not None and t.is_alive():
            t.join(timeout=5.0)
        with self._lock:
            self._repositioning = False

    def _run(self) -> None:
        try:
            self.engine.run(self.video_path, self._out_dir, "hls", bitrate=self.bitrate,
                            start_seconds=self.start_seconds, progress_cb=self._progress)
        except Exception as e:  # noqa: BLE001 -- cancel/error must not kill the worker thread
            with self._lock:
                if not self._repositioning:
                    self.error = str(e)
        else:
            with self._lock:
                self._finished = True
        finally:
            with self._lock:
                self._done = True

    def _progress(self, fnum: int, total: int) -> None:
        self._frames = fnum + 1
        self._total = total or 0

    # ---- state ----------------------------------------------------------------------

    def running(self) -> bool:
        with self._lock:
            return self._thread is not None and self._thread.is_alive()

    def finished(self) -> bool:
        """True once the transcode ran to EOF successfully (ENDLIST written)."""
        with self._lock:
            return self._finished

    def refresh_error(self) -> None:
        """No-op for interface parity with PassthroughSession (AI error is set eagerly by the
        worker thread, no ffmpeg.log to re-read)."""

    def status(self) -> dict:
        with self._lock:
            return {
                "rel": self.rel,
                "position": self.start_seconds,
                "duration": self.duration,
                "running": self._thread is not None and self._thread.is_alive(),
                "finished": self._finished,
                "error": self.error,
                "frames": self._frames,
                "total": self._total,
            }
