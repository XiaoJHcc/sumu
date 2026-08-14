# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# On-demand video thumbnails for the web-streaming directory index. Extracts one frame (at ~1/3
# of the duration) with ffmpeg and caches it as a small JPEG under %TEMP%/sumu-thumbs. Ported
# from D:\Git\simple-http-video-server (the reference front-end, kept visually in sync); a
# missing ffmpeg/ffprobe simply yields None so the front-end falls back to its inline SVG
# placeholder. Stdlib-only + ffmpeg/ffprobe on PATH (ffprobe is already a soft dependency of
# sumu.ai.utils.video_utils; ffmpeg of sumu.webstream.encoder).
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import threading

THUMB_WIDTH = 480

_ffmpeg_ok: bool | None = None
_ffmpeg_lock = threading.Lock()

# Per-cache-file locks so concurrent browser <img> requests for the same video generate the
# thumbnail once instead of racing several identical ffmpeg processes. Entries are tiny Lock
# objects and are never evicted (bounded by the number of distinct videos, fine for a home
# library).
_jobs: dict[str, threading.Lock] = {}
_jobs_lock = threading.Lock()


def _startupinfo():
    if sys.platform != "win32":
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    return si


def thumb_cache_dir() -> str:
    d = os.path.join(tempfile.gettempdir(), "sumu-thumbs")
    os.makedirs(d, exist_ok=True)
    return d


def ffmpeg_available() -> bool:
    global _ffmpeg_ok
    with _ffmpeg_lock:
        if _ffmpeg_ok is None:
            try:
                p = subprocess.run(["ffmpeg", "-version"], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, startupinfo=_startupinfo(),
                                   timeout=30, check=False)
                _ffmpeg_ok = p.returncode == 0
            except Exception:  # noqa: BLE001 -- probe failure == unavailable
                _ffmpeg_ok = False
        return _ffmpeg_ok


def _probe(path: str) -> tuple[float | None, int | None]:
    """Return (duration_seconds, main_video_stream_index). Skips embedded cover art (attached_pic)
    and prefers the largest non-cover video stream, matching the reference server's probe."""
    try:
        p = subprocess.run(
            ["ffprobe", "-v", "error",
             "-show_entries", "format=duration",
             "-show_entries", "stream=index,codec_type,width,height,disposition",
             "-of", "json", path],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            startupinfo=_startupinfo(), timeout=30, check=False,
        )
        if p.returncode != 0:
            return None, None
        data = json.loads(p.stdout.decode("utf-8", "replace"))
    except Exception:  # noqa: BLE001 -- any probe failure degrades to a default seek
        return None, None

    streams = data.get("streams") or []
    videos = [s for s in streams if s.get("codec_type") == "video"
              and not int((s.get("disposition") or {}).get("attached_pic") or 0)]
    videos.sort(key=lambda s: -(int(s.get("width") or 0) * int(s.get("height") or 0)))
    main = videos[0] if videos else None

    duration = None
    try:
        dur = float((data.get("format") or {}).get("duration") or 0)
        if dur > 0:
            duration = dur
    except (TypeError, ValueError):
        pass
    return duration, (int(main["index"]) if main and main.get("index") is not None else None)


def _cache_path(path: str, st) -> str:
    key = hashlib.sha1(
        f"{os.path.abspath(path)}|{st.st_size}|{int(st.st_mtime * 1000)}|{THUMB_WIDTH}"
        .encode("utf-8")
    ).hexdigest()
    return os.path.join(thumb_cache_dir(), key + ".jpg")


def _extract_frame(path: str, seek: float, stream_idx: int | None, tmp: str) -> None:
    map_args = ["-map", f"0:{stream_idx}"] if stream_idx is not None else ["-map", "0:v:0"]
    p = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error",
         "-ss", f"{seek:.3f}", "-i", path, *map_args,
         "-frames:v", "1", "-an", "-sn",
         "-vf", f"scale={THUMB_WIDTH}:-2:flags=fast_bilinear",
         "-q:v", "4", "-y", tmp],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        startupinfo=_startupinfo(), timeout=60, check=False,
    )
    if p.returncode != 0:
        raise RuntimeError(f"ffmpeg exited {p.returncode}")


def generate_video_thumb(path: str, st) -> str | None:
    """Extract + cache a thumbnail frame for `path`. Returns the cached .jpg path, or None when
    ffmpeg is unavailable or extraction fails (front-end falls back to its placeholder)."""
    if not ffmpeg_available():
        return None
    out = _cache_path(path, st)
    if os.path.isfile(out):
        return out

    with _jobs_lock:
        lock = _jobs.setdefault(out, threading.Lock())
    with lock:
        if os.path.isfile(out):  # another thread may have finished while we waited
            return out
        duration, stream_idx = _probe(path)
        if duration and duration > 0:
            seek = max(0.0, duration / 3.0) if duration >= 3 else max(0.0, duration * 0.5)
        else:
            seek = 3.0
        tmp = f"{out}.{os.getpid()}.{threading.get_ident()}.tmp.jpg"
        try:
            _extract_frame(path, seek, stream_idx, tmp)
            os.replace(tmp, out)
            return out
        except Exception:  # noqa: BLE001 -- thumbnail failure must never crash the request
            try:
                os.remove(tmp)
            except OSError:
                pass
            return None
