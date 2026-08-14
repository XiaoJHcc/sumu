# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Offline export: run the TranscodeEngine to a single MP4 (+faststart) file, decensored.
# A thin background-job wrapper around TranscodeEngine.run() with progress + cancel -- the
# heavy lifting (headless decode -> decensor -> NVENC -> mux) is entirely in transcode.py.
from __future__ import annotations

import threading


class ExportJob:
    """One offline-export job. Reuses the shared engine (one transcode at a time, enforced by
    the app's coordinator); run() blocks on a daemon thread, polled via status()."""

    def __init__(self, engine, source: str, out_path: str, bitrate: str = "8M"):
        self.engine = engine
        self.source = source
        self.out_path = out_path
        self.bitrate = bitrate
        self.done = False
        self.error = None
        self.frames = 0
        self.total = 0
        self.thread = None

    def start(self):
        self.thread = threading.Thread(target=self._run, name="sumu-export", daemon=True)
        self.thread.start()

    def cancel(self):
        self.engine.cancel()

    def _run(self):
        try:
            self.engine.run(self.source, self.out_path, "mp4", bitrate=self.bitrate,
                            progress_cb=self._progress)
        except Exception as e:  # noqa: BLE001
            self.error = str(e)
        finally:
            self.done = True

    def _progress(self, fnum: int, total: int):
        self.frames = fnum + 1
        self.total = total or 0

    def progress(self) -> float | None:
        """0..1 fraction, or None if the total frame count is unknown."""
        if self.total > 0:
            return min(1.0, self.frames / self.total)
        return None

    def status(self) -> dict:
        return {
            "done": self.done,
            "error": self.error,
            "frames": self.frames,
            "total": self.total,
            "progress": self.progress(),
            "out_path": self.out_path,
        }
