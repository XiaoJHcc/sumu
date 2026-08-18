# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Offline export: a sequential job QUEUE over the shared TranscodeEngine (one transcode at a time,
# enforced by the engine's _run_lock). Export is an exclusive full-screen mode (see app.py): while
# the queue runs, playback and web-streaming are disabled, so the engine has exactly one consumer
# and there is no cross-feature cancel pollution.
#
# The queue owns item lifecycle (add/remove/cancel/move) + a single worker thread that runs items
# strictly one-after-another. Each item's actual transcode is delegated to the app-provided
# `runner(item)` callback, which knows how to build the quality-first export config + EncodeOptions
# (clip_length longer, per-frame region cap removed, HEVC/CQ/p7 quality flags) -- keeping this
# module free of any scheduler/torch dependency.
from __future__ import annotations

import os
import threading
from urllib.parse import urlparse

_DEFAULT_SUFFIX = "_Decensored"  # per-preset output naming: <stem><suffix>.mp4 (preset overrides)


def default_out_path(source: str, global_dir: str = "", suffix: str = "") -> str:
    """Resolve an output path: `<stem><suffix>.mp4` (suffix defaults to `_Decensored`, but the
    active preset supplies its own). Local files go next to the source (or under `global_dir` when
    given); http(s) sources fall back to the cwd (no meaningful dir)."""
    if source.startswith("http://") or source.startswith("https://"):
        name = os.path.basename(urlparse(source).path) or "output.mp4"
        base = os.getcwd()
    else:
        name = os.path.basename(source) or "output.mp4"
        base = global_dir or os.path.dirname(os.path.abspath(source)) or os.getcwd()
    stem, _ext = os.path.splitext(name)
    return os.path.join(base, f"{stem}{suffix or _DEFAULT_SUFFIX}.mp4")


class ExportQueueItem:
    """One queued video. `preset_idx` indexes the app's preset list (clamped on load)."""

    def __init__(self, item_id: int, source: str, out_path: str, out_mode: str,
                 preset_idx: int):
        self.id = item_id
        self.source = source
        self.out_path = out_path
        self.out_mode = out_mode          # "auto" | "global" | "custom"
        self.preset_idx = preset_idx
        self.status = "pending"           # pending|running|done|failed|cancelled|interrupted
        self.error: str | None = None
        self.frames = 0
        self.total = 0

    def progress(self) -> float | None:
        if self.total > 0:
            return min(1.0, self.frames / self.total)
        return None

    def snapshot(self) -> dict:
        return {
            "id": self.id,
            "source": self.source,
            "out_path": self.out_path,
            "out_mode": self.out_mode,
            "preset_idx": self.preset_idx,
            "status": self.status,
            "progress": self.progress(),
            "frames": self.frames,
            "total": self.total,
            "error": self.error or "",
        }


class ExportQueue:
    """Sequential offline-export queue. `runner(item)` performs one item's transcode (raises on
    failure/cancel); `engine` is the shared TranscodeEngine, used only for cancel()."""

    def __init__(self, engine, runner):
        self.engine = engine
        self.runner = runner
        self.items: list[ExportQueueItem] = []
        self._next_id = 1
        self.running = False
        self._current_id: int | None = None
        self._cancel_requested = False
        self._thread: threading.Thread | None = None
        self._lock = threading.Lock()

    # ---- structure (main thread) -------------------------------------------------------

    def add(self, source: str, preset_idx: int, out_mode: str = "auto",
            custom_out: str = "", global_dir: str = "", suffix: str = "") -> int:
        out_path = self._resolve_out(source, out_mode, custom_out, global_dir, suffix)
        item = ExportQueueItem(self._next_id, source, out_path, out_mode, preset_idx)
        self._next_id += 1
        with self._lock:
            self.items.append(item)
        return item.id

    def remove(self, item_id: int) -> None:
        """Remove an item. A running item is cancelled first (engine stops) then dropped."""
        with self._lock:
            item = self._find(item_id)
            if item is None:
                return
            if item.status == "running":
                self._cancel_requested = True
                self.engine.cancel()
            self.items = [it for it in self.items if it.id != item_id]

    def cancel(self, item_id: int) -> None:
        """Cancel a pending item (skip) or the running item (stop the engine)."""
        with self._lock:
            item = self._find(item_id)
            if item is None:
                return
            if item.status == "running":
                self._cancel_requested = True
                self.engine.cancel()
            elif item.status == "pending":
                item.status = "cancelled"

    def move_to(self, item_id: int, target_id: int) -> None:
        """Drag-reorder a pending item: insert it before target_id (-1 == queue end)."""
        with self._lock:
            idx = next((i for i, it in enumerate(self.items) if it.id == item_id), None)
            if idx is None or self.items[idx].status != "pending":
                return
            item = self.items.pop(idx)
            t = next((i for i, it in enumerate(self.items) if it.id == target_id), None)
            if target_id < 0 or t is None:
                self.items.append(item)
                return
            self.items.insert(t, item)

    def start(self) -> None:
        """Begin processing pending items sequentially (no-op if already running)."""
        if self.running:
            return
        self.running = True
        self._thread = threading.Thread(target=self._worker, name="sumu-export-q", daemon=True)
        self._thread.start()

    def cancel_all(self) -> None:
        """Stop the running item (engine cancel) and skip every pending item."""
        with self._lock:
            self._cancel_requested = True
            if self.engine is not None:  # None until model warmup wires the engine in
                self.engine.cancel()
            for item in self.items:
                if item.status == "pending":
                    item.status = "cancelled"

    # ---- worker ------------------------------------------------------------------------

    def _worker(self) -> None:
        try:
            while True:
                item = self._next_pending()
                if item is None:
                    break
                with self._lock:
                    item.status = "running"
                    self._current_id = item.id
                try:
                    self.runner(item)
                except Exception as e:  # noqa: BLE001 -- a failed item must not kill the queue
                    with self._lock:
                        if self._cancel_requested:
                            item.status = "cancelled"
                            item.error = None
                            self._cancel_requested = False
                        else:
                            item.status = "failed"
                            item.error = str(e)
                else:
                    with self._lock:
                        item.status = "done"
                finally:
                    with self._lock:
                        self._current_id = None
        finally:
            self.running = False

    def _next_pending(self):
        with self._lock:
            for item in self.items:
                if item.status == "pending":
                    return item
            return None

    # ---- persistence / snapshot --------------------------------------------------------

    def to_persist(self) -> list[dict]:
        """Serializable queue state: pending items + the in-flight item (marked 'interrupted'
        so a crash/restart shows it without auto-resuming). Done/failed/cancelled are dropped."""
        with self._lock:
            out = []
            for it in self.items:
                if it.status == "pending":
                    status = "pending"
                elif it.status == "running":
                    status = "interrupted"
                else:
                    continue
                out.append({
                    "source": it.source, "out_path": it.out_path, "out_mode": it.out_mode,
                    "preset_idx": it.preset_idx, "status": status,
                })
            return out

    def load_persisted(self, data: list[dict], preset_count: int) -> None:
        """Restore a saved queue (pending → pending; interrupted → interrupted, not auto-run)."""
        with self._lock:
            for d in data or []:
                if not isinstance(d, dict) or not d.get("source"):
                    continue
                pidx = int(d.get("preset_idx") or 0)
                if pidx < 0 or pidx >= max(1, preset_count):
                    pidx = 0
                item = ExportQueueItem(self._next_id, d["source"], d.get("out_path") or "",
                                       d.get("out_mode") or "auto", pidx)
                self._next_id += 1
                if d.get("status") == "interrupted":
                    item.status = "interrupted"
                self.items.append(item)

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "running": self.running,
                "current_id": self._current_id,
                "items": [it.snapshot() for it in self.items],
            }

    # ---- internals ----------------------------------------------------------------------

    def _find(self, item_id: int):
        for it in self.items:
            if it.id == item_id:
                return it
        return None

    @staticmethod
    def _resolve_out(source: str, out_mode: str, custom_out: str, global_dir: str,
                     suffix: str) -> str:
        if out_mode == "custom":
            return custom_out or default_out_path(source, "", suffix)
        if out_mode == "global":
            return default_out_path(source, global_dir, suffix)
        return default_out_path(source, "", suffix)
