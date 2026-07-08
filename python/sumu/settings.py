# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Phase 6 M-E: persisted user state for the daily entrypoint (scripts/play.py) -- last volume,
# mute state, recent-files list, and per-file last playback position (resume). Deliberately
# stdlib-only (json/os/pathlib/tempfile + dataclasses/typing), no dependency on sumu_core/torch,
# so this module is importable and testable in complete isolation (see scripts/verify_settings.py).
#
# Crash-safety invariant: settings.json is user-editable/deletable state living outside the repo.
# A missing, empty, or corrupt file must NEVER turn a clean run into a crash -- load() always
# returns a usable Settings (falling back to per-field defaults), and save() writes atomically
# (temp file in the same dir + os.replace()) so a crash mid-write can never leave a torn/partial
# settings.json behind.
from __future__ import annotations

import json
import os
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

RECENT_CAP = 10


def default_path() -> Path:
    """%APPDATA%/sumu/settings.json on Windows; ~/.sumu/settings.json if APPDATA is unset
    (e.g. non-Windows dev/test runs)."""
    appdata = os.environ.get("APPDATA")
    if appdata:
        return Path(appdata) / "sumu" / "settings.json"
    return Path.home() / ".sumu" / "settings.json"


def _norm_key(path: str) -> str:
    """Normalize a path for use as a `positions` dict key / `recent` dedup comparison --
    Windows paths are case-insensitive, so plain string equality would treat "C:\\a.mp4" and
    "c:\\A.MP4" as different files. Only used by the push_recent/set_position/get_position
    accessors below -- raw Settings.recent/.positions storage (as loaded/saved) is untouched."""
    return os.path.normcase(os.path.abspath(path))


@dataclass
class Settings:
    volume: float = 1.0
    muted: bool = False
    recent: list[str] = field(default_factory=list)
    positions: dict[str, int] = field(default_factory=dict)
    # Cached "can this machine run TRT at all" (cuda + fp16). None = never determined (first run).
    # The daily player needs this on the MAIN thread, before the first overlay frame, to decide
    # whether to show the first-screen "compile engines" prompt -- but the real check needs torch
    # (torch.cuda.is_available()), which is exactly the multi-second startup cost we moved off the
    # main thread. So we cache the last run's answer (optimistic True on first run, since sumu
    # targets Nvidia) and reconcile against the real value once background warmup finishes.
    trt_applicable: Optional[bool] = None

    def push_recent(self, path: str) -> None:
        """Move-to-front, dedup by normcase, cap at RECENT_CAP entries (oldest dropped).
        Stores a real usable absolute path (case preserved) -- only the dedup/ordering
        comparison uses the case-insensitive normalized key, per _norm_key's contract."""
        stored = os.path.abspath(path)
        key = _norm_key(path)
        self.recent = [p for p in self.recent if _norm_key(p) != key]
        self.recent.insert(0, stored)
        del self.recent[RECENT_CAP:]

    def set_position(self, path: str, frame: int) -> None:
        self.positions[_norm_key(path)] = int(frame)

    def get_position(self, path: str) -> Optional[int]:
        return self.positions.get(_norm_key(path))


def _clamp_volume(value) -> float:
    try:
        v = float(value)
    except (TypeError, ValueError):
        return 1.0
    if v != v:  # NaN
        return 1.0
    return max(0.0, min(1.0, v))


def _coerce_bool(value, default: bool) -> bool:
    return value if isinstance(value, bool) else default


def _coerce_opt_bool(value) -> Optional[bool]:
    """Like _coerce_bool but preserves the None ("never determined") tri-state -- anything that
    isn't a real bool (including missing/null) collapses to None, not a made-up default."""
    return value if isinstance(value, bool) else None


def _coerce_recent(value) -> list[str]:
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, str) and item][:RECENT_CAP]


def _coerce_positions(value) -> dict[str, int]:
    if not isinstance(value, dict):
        return {}
    out: dict[str, int] = {}
    for k, v in value.items():
        if not isinstance(k, str):
            continue
        try:
            out[k] = int(v)
        except (TypeError, ValueError):
            continue
    return out


def load(path: Optional[str | Path] = None) -> Settings:
    """Read+parse settings.json. NEVER raises: a missing file, unreadable file, or malformed/
    partial JSON all yield an all-defaults Settings (or, for partial JSON that parses but has
    junk in one field, defaults for just that field -- coercion is per-field, not all-or-nothing
    once the top level is a valid dict)."""
    p = Path(path) if path is not None else default_path()
    try:
        raw = p.read_text(encoding="utf-8")
        data = json.loads(raw)
        if not isinstance(data, dict):
            return Settings()
        return Settings(
            volume=_clamp_volume(data.get("volume", 1.0)),
            muted=_coerce_bool(data.get("muted"), False),
            recent=_coerce_recent(data.get("recent")),
            positions=_coerce_positions(data.get("positions")),
            trt_applicable=_coerce_opt_bool(data.get("trt_applicable")),
        )
    except Exception:  # noqa: BLE001 -- a corrupt/unreadable settings file must never crash the player
        return Settings()


def save(settings: Settings, path: Optional[str | Path] = None) -> None:
    """Atomic write: serialize to a temp file in the same directory, then os.replace() onto the
    target -- a crash/power-loss mid-write can never leave a torn settings.json behind (the
    rename is atomic on the same filesystem). NEVER raises: an unwritable directory (or any other
    failure) is logged to stderr and swallowed -- persistence failing must never crash the player."""
    p = Path(path) if path is not None else default_path()
    tmp_path: Optional[str] = None
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        data = {
            "volume": _clamp_volume(settings.volume),
            "muted": bool(settings.muted),
            "recent": list(settings.recent)[:RECENT_CAP],
            "positions": dict(settings.positions),
            "trt_applicable": settings.trt_applicable,
        }
        fd, tmp_path = tempfile.mkstemp(prefix=".settings-", suffix=".tmp", dir=str(p.parent))
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
        os.replace(tmp_path, str(p))
        tmp_path = None
    except Exception as e:  # noqa: BLE001 -- persistence must never crash the player
        print(f"[sumu.settings] save failed: {e!r}", file=sys.stderr)
    finally:
        if tmp_path is not None:
            try:
                os.remove(tmp_path)
            except OSError:
                pass


def is_resumable_frame(frame: Optional[int], fps: Optional[float], frame_count: Optional[int]) -> bool:
    """Resume-gate policy: is `frame` a "meaningful mid-file" position worth seeking back to?
    Skips near-start/near-end positions (more than 5s from both ends required) and unknown/zero
    fps or frame_count. Pure + stdlib-only so it's directly unit-testable without a Player
    (see scripts/verify_settings.py) -- play.py's maybe_resume() is the only caller."""
    if frame is None or not fps or fps <= 0 or not frame_count or frame_count <= 0:
        return False
    margin = 5.0 * fps
    return margin < frame < (frame_count - margin)
