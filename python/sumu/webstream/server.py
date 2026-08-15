# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Web streaming server: a stdlib-only ThreadingHTTPServer that serves (a) a directory-browser
# front-end over a source folder, and (b) per-video HLS live-transcode streams in two modes:
#   - 原片直出 (passthrough): pure ffmpeg -ss + NVENC (PassthroughSession, one ffmpeg per video).
#   - AI 去码: TranscodeEngine (headless decode -> decensor -> NVENC) via AiStreamSession.
# Both session types share one interface, so the routes are mode-agnostic (start/seek/stop/
# idle-sweep/status all work for both). Token auth guards every route except OPTIONS. Designed to
# run on a dedicated daemon thread inside the sumu app; callers drive it via start()/stop()/
# status().
#
# Concurrency model: passthrough runs one independent ffmpeg per video (no gate); the AI path is
# one transcode at a time (shared models, BasicVSR is GPU-bound). A finished video's HLS output is
# cached on disk and re-served without re-transcoding; a request for a *different* video while the
# AI path is busy returns 503.
from __future__ import annotations

import json
import os
import re
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse, parse_qs

from . import index_page, thumbnail
from .ai_session import AiStreamSession
from .passthrough import IDLE_TIMEOUT, PassthroughSession, _probe

# Vendored hls.js (Apache-2.0) served to desktop browsers that lack native HLS. Loaded once at
# server construction so a frozen bundle can read it from the same dir as this module.
_HLS_JS_PATH = os.path.join(os.path.dirname(__file__), "hls.min.js")


def _load_hls_js() -> bytes:
    try:
        with open(_HLS_JS_PATH, "rb") as f:
            return f.read()
    except OSError:
        return (b"console.error('hls.js is missing from the sumu bundle');"
                b"window.Hls=undefined;")


class StreamManager:
    """Owns the per-video stream sessions. Passthrough and AI sessions share one interface
    (start/seek/apply_seek/stop/touch/idle_seconds/running/finished/status/out_dir/m3u8_path), so
    the server routes are mode-agnostic. The one semantic difference lives in get_or_start: the AI
    path enforces a busy gate (one transcode at a time -- shared models, BasicVSR is GPU-bound),
    while passthrough runs one independent ffmpeg per video."""

    def __init__(self, engine, root: str, cache_dir: str, bitrate: str = "8M",
                 passthrough: bool = True, hwaccel: str | None = None):
        self.engine = engine
        self.root = os.path.abspath(root)
        self.cache_dir = cache_dir
        self.bitrate = bitrate
        self.passthrough = passthrough
        self.hwaccel = hwaccel
        self.sessions: dict[str, "PassthroughSession | AiStreamSession"] = {}
        self._lock = threading.Lock()

    def _make_session(self, rel: str, video_path: str):
        base = os.path.join(self.cache_dir, _safe_key(rel))
        if self.passthrough:
            # 原片直出: one independent ffmpeg process per video (NVENC is cheap, no shared AI
            # models), started lazily on the first m3u8/segment fetch -- no busy gate.
            return PassthroughSession(rel, video_path, base, bitrate=self.bitrate,
                                      hwaccel=self.hwaccel)
        # AI 去码: one repositionable transcode backed by the shared engine.
        return AiStreamSession(rel, video_path, base, self.engine, bitrate=self.bitrate)

    def get_or_start(self, rel: str, video_path: str):
        with self._lock:
            s = self.sessions.get(rel)
            if s is not None:
                return s
            if not self.passthrough:
                # AI: one transcode at a time (shared models, BasicVSR is GPU-bound).
                for o in self.sessions.values():
                    if o.running():
                        return None  # busy
            s = self._make_session(rel, video_path)
            self.sessions[rel] = s
            return s

    # ---- unified session operations (both modes) ---------------------------------------

    def seek(self, rel: str, video_path: str, seconds: float):
        s = self.get_or_start(rel, video_path)
        if s is None:
            return None
        s.seek(seconds)
        return s

    def stop(self, rel: str):
        s = self.sessions.get(rel)
        if s is not None:
            s.stop()

    def meta(self, rel: str, video_path: str):
        s = self.sessions.get(rel)
        if s is not None:
            s.probe_meta()
            return {"duration": s.duration, "position": s.start_seconds, "fps": s.fps,
                    "running": s.running()}
        # No session yet: probe directly (ffprobe is independent of the busy GPU gate).
        duration, fps = _probe(video_path)
        return {"duration": duration, "position": 0.0, "fps": fps, "running": False}

    def touch(self, rel: str):
        s = self.sessions.get(rel)
        if s is not None:
            s.touch()

    def sweep_idle(self):
        """Stop any session that has run IDLE_TIMEOUT without a client fetch -- the safety net
        beyond the client's explicit pause/close signals (works for both passthrough and AI)."""
        for s in list(self.sessions.values()):
            if s.running() and s.idle_seconds() > IDLE_TIMEOUT:
                s.stop()

    def cancel_all(self):
        # Both session types own their stop(): passthrough kills its ffmpeg, AI cancels the
        # shared engine and joins its worker. Stopping is what frees the GPU/process.
        for s in list(self.sessions.values()):
            if s.running():
                s.stop()

    def set_passthrough(self, passthrough: bool) -> None:
        """Switch 直出/AI 去码 mode at runtime: stop every running session (they are the wrong
        mode now) and drop them, so the browser's next m3u8/segment fetch recreates a session in
        the new mode. The server keeps its port/token; the live-HLS client re-fetches and picks
        up the switch (repositioning to the ?start= position it already carries)."""
        with self._lock:
            sessions = list(self.sessions.values())
        # Stop OUTSIDE the lock: a session's stop() can join its worker for up to ~5s (AI).
        for s in sessions:
            if s.running():
                s.stop()
        with self._lock:
            self.sessions.clear()
            self.passthrough = passthrough

    def set_engine(self, engine) -> None:
        """Fill in the shared AI engine reference after construction (used when the server started
        in passthrough mode with engine=None, then warmup finishes and a web-UI switch to AI needs
        the engine)."""
        self.engine = engine

    def status(self) -> list[dict]:
        with self._lock:
            return [s.status() for s in self.sessions.values()]


class StreamingServer:
    def __init__(self, root: str, port: int, engine, host: str = "0.0.0.0",
                 token: str = "", no_token: bool = False, bitrate: str = "8M",
                 cache_dir: str | None = None, passthrough: bool = True,
                 hwaccel: str | None = None, on_mode_change=None):
        self.root = os.path.abspath(root)
        self.port = int(port)
        self.host = host
        # no_token disables auth entirely (empty token, _token_ok short-circuits); otherwise an
        # empty token means "generate a random one per run".
        self.token = "" if no_token else (token or secrets.token_urlsafe(18))
        self.bitrate = bitrate
        self.passthrough = passthrough
        self.engine = engine  # shared AI engine; may be None (passthrough start) then filled later
        self.on_mode_change = on_mode_change  # called(passthrough) after a web-UI mode switch
        self.cache_dir = cache_dir or os.path.join(os.path.dirname(self.root) or self.root,
                                                   ".sumu_stream_cache")
        os.makedirs(self.cache_dir, exist_ok=True)
        self.manager = StreamManager(engine, self.root, self.cache_dir, bitrate,
                                     passthrough=passthrough, hwaccel=hwaccel)
        self.httpd = None
        self._thread = None
        self._sweep_thread = None
        self._sweep_stop = threading.Event()
        self._hls_js = _load_hls_js()

    # ---- lifecycle ---------------------------------------------------------------

    def start(self):
        handler = _make_handler(self)
        self.httpd = ThreadingHTTPServer((self.host, self.port), handler)
        self.httpd.daemon_threads = True
        self._thread = threading.Thread(target=self.httpd.serve_forever,
                                        name="sumu-stream-http", daemon=True)
        self._thread.start()
        # Idle sweep runs for BOTH modes: passthrough stops its ffmpeg, AI cancels its shared
        # engine -- a dead client (crashed / closed without a signal) must never leave the GPU
        # burning indefinitely.
        self._sweep_stop.clear()
        self._sweep_thread = threading.Thread(target=self._sweep_loop,
                                              name="sumu-stream-sweep", daemon=True)
        self._sweep_thread.start()

    def _sweep_loop(self):
        while not self._sweep_stop.wait(5.0):
            try:
                self.manager.sweep_idle()
            except Exception:  # noqa: BLE001 -- a sweep failure must never kill the thread
                pass

    def stop(self):
        self._sweep_stop.set()
        self.manager.cancel_all()
        if self.httpd is not None:
            self.httpd.shutdown()
            self.httpd.server_close()
            self.httpd = None

    def access_url(self) -> str:
        host = "127.0.0.1" if self.host == "0.0.0.0" else self.host
        base = f"http://{host}:{self.port}/"
        return base + (f"?token={self.token}" if self.token else "")

    def status(self) -> list[dict]:
        return self.manager.status()

    def set_passthrough(self, passthrough: bool) -> None:
        """Runtime 直出/AI 去码 mode switch (keeps port/token; stops + drops live sessions)."""
        self.passthrough = passthrough
        self.manager.set_passthrough(passthrough)
        if self.on_mode_change is not None:
            try:
                self.on_mode_change(passthrough)
            except Exception:  # noqa: BLE001 -- a broken persistence hook must not break the switch
                pass

    def set_engine(self, engine) -> None:
        """Update the shared AI engine reference (e.g. warmup finishing after a passthrough start)."""
        self.engine = engine
        self.manager.engine = engine

    # ---- routing helpers ----------------------------------------------------------

    def _resolve(self, rel: str) -> str:
        """Join a URL-decoded relative path onto root with traversal protection."""
        rel = rel.replace("\\", "/").strip("/")
        candidate = os.path.normpath(os.path.join(self.root, *rel.split("/")))
        if candidate != self.root and not candidate.startswith(self.root + os.sep):
            raise ValueError("path escapes root")
        return candidate

    def _list_dir(self, abs_dir: str, rel_prefix: str) -> list[dict]:
        items = []
        for name in os.listdir(abs_dir):
            if name.startswith("."):
                continue
            full = os.path.join(abs_dir, name)
            rel = (rel_prefix + "/" + name) if rel_prefix else name
            try:
                st = os.stat(full)
            except OSError:
                continue
            is_dir = os.path.isdir(full)
            if not is_dir and not index_page.is_video(name):
                continue  # only folders + videos in the grid (v1)
            if is_dir:
                items.append({"name": name, "is_dir": True, "rel": rel, "kind": "folder",
                              "size": None, "thumb": self._first_video_in_dir(full, rel)})
            else:
                items.append({"name": name, "is_dir": False, "rel": rel, "kind": "video",
                              "size": st.st_size, "thumb": rel})
        items.sort(key=lambda it: (not it["is_dir"], it["name"].lower()))
        return items

    def _first_video_in_dir(self, abs_dir: str, rel_prefix: str) -> str | None:
        """First direct video child (non-recursive) used as a folder's cover thumbnail."""
        try:
            names = sorted(n for n in os.listdir(abs_dir) if not n.startswith("."))
        except OSError:
            return None
        for name in names:
            if not index_page.is_video(name):
                continue
            if os.path.isfile(os.path.join(abs_dir, name)):
                return (rel_prefix + "/" + name) if rel_prefix else name
        return None


def _safe_key(rel: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]", "_", rel)


def _make_handler(server: "StreamingServer"):
    class Handler(BaseHTTPRequestHandler):
        server_version = "sumu-stream/1.0"

        def log_message(self, fmt, *args):  # quiet
            pass

        def _token_ok(self) -> bool:
            if not server.token:
                return True
            q = parse_qs(urlparse(self.path).query)
            return q.get("token", [""])[0] == server.token

        def _send(self, code, body: bytes, ctype: str, extra: dict | None = None):
            try:
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                for k, v in (extra or {}).items():
                    self.send_header(k, v)
                self.end_headers()
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                # Client aborted mid-response (hls.js destroys + reloads the stream on seek;
                # a browser closing the tab aborts in-flight segment/playlist fetches). Not an
                # error worth logging -- just drop the dead socket.
                pass

        def _send_html(self, code, body: str, extra: dict | None = None):
            self._send(code, body.encode("utf-8"), "text/html; charset=utf-8", extra)

        def _send_plain(self, code, body: str):
            self._send(code, body.encode("utf-8"), "text/plain; charset=utf-8")

        def _rel_after(self, prefix: str) -> str:
            path = unquote(urlparse(self.path).path)
            return path[len(prefix):]

        # ---- routes -----------------------------------------------------------------

        def do_OPTIONS(self):  # noqa: N802
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Range")
            self.end_headers()

        def do_HEAD(self):  # noqa: N802
            self._route(head_only=True)

        def do_GET(self):  # noqa: N802
            self._route(head_only=False)

        def do_POST(self):  # noqa: N802
            # Only used by the player's sendBeacon(/stop) on pause/close. sendBeacon sends a POST
            # (no body or a tiny text/plain body); we drain it and stop the transcode.
            if not self._token_ok():
                return self._send_plain(401, "Unauthorized")
            path = urlparse(self.path).path
            if path.startswith("/stream/") and path.endswith("/stop"):
                rel = path[len("/stream/"):-len("/stop")].strip("/")
                server.manager.stop(rel)
                try:
                    length = int(self.headers.get("Content-Length") or 0)
                    if length:
                        self.rfile.read(min(length, 65536))
                except Exception:  # noqa: BLE001
                    pass
                self.send_response(204)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                return
            return self._send_plain(404, "Not Found")

        def _route(self, head_only: bool):
            if not self._token_ok():
                return self._send_plain(401, "Unauthorized: missing or invalid token (?token=)")
            path = urlparse(self.path).path
            try:
                if path == "/":
                    return self._index("", [])
                if path.startswith("/browse/"):
                    rel = self._rel_after("/browse/").strip("/")
                    return self._index(rel, _crumbs(rel))
                if path.startswith("/play/"):
                    rel = self._rel_after("/play/").strip("/")
                    return self._player(rel)
                if path.startswith("/stream/"):
                    rest = self._rel_after("/stream/")
                    return self._stream(rest, head_only)
                if path.startswith("/__thumb__/"):
                    rel = self._rel_after("/__thumb__/").strip("/")
                    return self._thumb(rel, head_only)
                if path == "/static/hls.min.js":
                    return self._hls_js(head_only)
                if path == "/mode":
                    return self._mode_route()
                return self._send_html(404, index_page.render_message("404", "Not Found"))
            except ValueError:
                return self._send_plain(403, "Forbidden")

        def _thumb(self, rel: str, head_only: bool):
            """Serve (generating on demand) a cached JPEG thumbnail for a video. 404 on failure so
            the front-end's <img onerror> falls back to its inline SVG placeholder."""
            try:
                abs_path = server._resolve(rel)
            except ValueError:
                return self._send_plain(403, "Forbidden")
            if not os.path.isfile(abs_path) or not index_page.is_video(os.path.basename(abs_path)):
                return self._send_plain(404, "Not Found")
            try:
                st = os.stat(abs_path)
            except OSError:
                return self._send_plain(404, "Not Found")
            jpg = thumbnail.generate_video_thumb(abs_path, st)
            if jpg is None or not os.path.isfile(jpg):
                return self._send_plain(404, "Not Found")
            self._send_file(jpg, "image/jpeg", head_only)

        def _hls_js(self, head_only: bool):
            self._send(200, server._hls_js, "application/javascript; charset=utf-8",
                       {"Cache-Control": "public, max-age=86400"})

        def _mode_route(self):
            """GET /mode -> current {passthrough}. GET /mode?passthrough=0|1 -> switch mode
            (immediately: stops + drops live sessions; the client re-fetches the m3u8 and picks up
            the new mode). Switching to AI (passthrough=0) while the engine isn't warm yet returns
            503 so the UI can surface "warming up"."""
            q = parse_qs(urlparse(self.path).query)
            if "passthrough" in q:
                raw = q["passthrough"][0].strip().lower()
                if raw in ("1", "true", "on", "yes"):
                    new_passthrough = True
                elif raw in ("0", "false", "off", "no"):
                    new_passthrough = False
                else:
                    return self._send_plain(400, "Bad passthrough value")
                if not new_passthrough and server.engine is None:
                    return self._send(503, json.dumps(
                        {"passthrough": server.passthrough, "error": "warmup"}
                    ).encode("utf-8"), "application/json", {"Cache-Control": "no-store"})
                server.set_passthrough(new_passthrough)
            return self._send(200, json.dumps(
                {"passthrough": server.passthrough}
            ).encode("utf-8"), "application/json", {"Cache-Control": "no-store"})

        def _finalize_playlist(self, body: bytes) -> bytes:
            """Post-process a served HLS playlist: inject ?token= into segment URIs (auth) and an
            EXT-X-START:TIME-OFFSET=0 tag. The latter makes hls.js (and native players that honor
            it) begin the *growing* EVENT playlist at offset 0 rather than at the live edge, so
            `video.currentTime` is a clean 0-based offset within this stream. The front-end then
            computes absolute playback time as basePos (the server's start/seek position) +
            currentTime, which keeps the custom seekbar correct across reposition seeks."""
            if b"#EXT-X-START:" not in body:
                body = re.sub(rb"(?m)^#EXTM3U\s*$",
                              b"#EXTM3U\n#EXT-X-START:TIME-OFFSET=0", body, count=1)
            if server.token:
                body = re.sub(rb"(?m)^([A-Za-z0-9._-]+\.ts)\s*$",
                              (b"\\1?token=" + server.token.encode()), body)
            return body

        def _index(self, rel: str, crumbs: list[tuple[str, str]]):
            try:
                abs_dir = server._resolve(rel)
            except ValueError:
                return self._send_plain(403, "Forbidden")
            if not os.path.isdir(abs_dir):
                return self._send_html(404, index_page.render_message("404", "Not a directory"))
            items = server._list_dir(abs_dir, rel)
            title = rel.split("/")[-1] if rel else os.path.basename(server.root) or "/"
            return self._send_html(200, index_page.render_index(title, crumbs, items, server.token),
                                   {"Cache-Control": "no-store"})

        def _player(self, rel: str):
            try:
                abs_path = server._resolve(rel)
            except ValueError:
                return self._send_plain(403, "Forbidden")
            if not os.path.isfile(abs_path) or not index_page.is_video(os.path.basename(abs_path)):
                return self._send_html(404, index_page.render_message("404", "Not a video"))
            title = os.path.basename(rel)
            return self._send_html(200, index_page.render_player(title, rel, server.token),
                                   {"Cache-Control": "no-store"})

        def _stream(self, rest: str, head_only: bool):
            rest = rest.strip("/")
            q = parse_qs(urlparse(self.path).query)
            if rest.endswith("/meta"):
                return self._stream_meta(rest[:-len("/meta")])
            if rest.endswith("/seek"):
                rel = rest[:-len("/seek")]
                try:
                    secs = float(q.get("t", ["0"])[0])
                except ValueError:
                    return self._send_plain(400, "Bad t")
                return self._stream_seek(rel, secs)
            if rest.endswith("/stop"):
                return self._stream_stop(rest[:-len("/stop")])
            if rest.endswith("/index.m3u8"):
                rel = rest[:-len("/index.m3u8")]
                return self._stream_playlist(rel)
            if "/" in rest:
                rel, seg = rest.rsplit("/", 1)
                return self._stream_segment(rel, seg, head_only)
            return self._send_plain(404, "Not Found")

        def _stream_playlist(self, rel: str):
            try:
                abs_path = server._resolve(rel)
            except ValueError:
                return self._send_plain(403, "Forbidden")
            if not os.path.isfile(abs_path):
                return self._send_html(404, index_page.render_message("404", "Not found"))
            session = server.manager.get_or_start(rel, abs_path)
            if session is None:
                return self._send_html(503, index_page.render_message(
                    "忙", "正在处理其它视频，请稍后刷新。"), {"Retry-After": "5"})
            # Seek is carried on the m3u8 URL as ?start=<seconds> (so the client can set
            # video.src and call play() synchronously, inside the user gesture). Reposition only
            # when it actually changed -- a live HLS re-fetch re-sends the same ?start= and must
            # NOT restart the transcode. `_` is the client's per-seek cache-buster timestamp, used
            # as a monotonic generation so an out-of-order stale request (from a player that was
            # destroyed+reloaded on seek) can't regress the transcode. Works for BOTH modes.
            q = parse_qs(urlparse(self.path).query)
            start = q.get("start", [None])[0]
            if start is not None:
                try:
                    s = float(start)
                except ValueError:
                    s = None
                if s is not None:
                    seq = None
                    seq_raw = q.get("_", [None])[0]
                    if seq_raw is not None:
                        try:
                            seq = float(seq_raw)
                        except ValueError:
                            seq = None
                    if abs(s - session.start_seconds) > 0.5:
                        session.apply_seek(s, seq)
            session.touch()
            # (Re)start only when there is no producer at all. A *finished* session (ran to EOF and
            # wrote a complete VOD playlist with ENDLIST) is served as-is -- a later m3u8 refresh /
            # second viewer would otherwise restart the transcode and regress to a live stream.
            if not session.running() and not session.finished():
                session.start(session.start_seconds)
            m3u8 = session.m3u8_path
            deadline = time.monotonic() + 30.0
            while not os.path.isfile(m3u8) and time.monotonic() < deadline:
                if session.error:
                    return self._send_html(500, index_page.render_message("错误", session.error))
                if session.finished():
                    session.refresh_error()
                    return self._send_html(500, index_page.render_message(
                        "错误", session.error or "转码未能产生输出"))
                time.sleep(0.2)
            if not os.path.isfile(m3u8):
                if session.error:
                    return self._send_html(500, index_page.render_message("错误", session.error))
                return self._send_html(504, index_page.render_message("超时", "转码启动超时，请重试"))
            with open(m3u8, "rb") as f:
                body = f.read()
            body = self._finalize_playlist(body)
            return self._send(200, body, "application/vnd.apple.mpegurl",
                              {"Cache-Control": "no-cache"})

        def _stream_meta(self, rel: str):
            try:
                abs_path = server._resolve(rel)
            except ValueError:
                return self._send_plain(403, "Forbidden")
            if not os.path.isfile(abs_path):
                return self._send_plain(404, "Not Found")
            m = server.manager.meta(rel, abs_path)
            if m is None:
                return self._send_plain(503, "Busy")
            return self._send(200, json.dumps(m).encode("utf-8"), "application/json",
                              {"Cache-Control": "no-store"})

        def _stream_seek(self, rel: str, seconds: float):
            try:
                abs_path = server._resolve(rel)
            except ValueError:
                return self._send_plain(403, "Forbidden")
            if not os.path.isfile(abs_path):
                return self._send_plain(404, "Not Found")
            s = server.manager.seek(rel, abs_path, seconds)
            if s is None:
                return self._send_plain(503, "Busy")
            return self._send(200, json.dumps({
                "position": s.start_seconds, "duration": s.duration,
            }).encode("utf-8"), "application/json", {"Cache-Control": "no-store"})

        def _stream_stop(self, rel: str):
            server.manager.stop(rel)
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()

        def _stream_segment(self, rel: str, seg: str, head_only: bool):
            try:
                abs_path = server._resolve(rel)
            except ValueError:
                return self._send_plain(403, "Forbidden")
            session = server.manager.get_or_start(rel, abs_path)
            if session is None:
                return self._send_plain(503, "Busy")
            if not re.fullmatch(r"[A-Za-z0-9._-]+", seg):
                return self._send_plain(404, "Not Found")
            session.touch()
            # Serve only segments already on disk; do NOT auto-(re)start the producer here -- that
            # would restart the transcode from `start_seconds` (position 0) on a late segment fetch
            # after an idle sweep, desyncing the client. Only the m3u8 route (which carries the
            # authoritative ?start=) starts/repositions the producer.
            out_dir = session.out_dir
            if not out_dir:
                return self._send_plain(404, "Not Found")
            seg_path = os.path.join(out_dir, seg)
            if not os.path.isfile(seg_path):
                return self._send_plain(404, "Not Found")
            # no-store: a seek reuses bare segment names in a new position dir; never let the
            # browser serve a stale same-named segment from the previous seek.
            return self._send_file(seg_path, "video/mp2t", head_only, cache_control="no-store")

        def _send_file(self, path: str, ctype: str, head_only: bool,
                       cache_control: str | None = None):
            size = os.path.getsize(path)
            rng = self.headers.get("Range")
            start, end, status = 0, size - 1, 200
            if rng:
                m = re.match(r"bytes=(\d*)-(\d*)", rng.strip())
                if m:
                    a, b = m.group(1), m.group(2)
                    if a == "" and b:
                        start = max(0, size - int(b))
                    else:
                        start = int(a) if a else 0
                        end = int(b) if b else size - 1
                    end = min(end, size - 1)
                    status = 206
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Accept-Ranges", "bytes")
            if cache_control:
                self.send_header("Cache-Control", cache_control)
            length = end - start + 1
            self.send_header("Content-Length", str(length))
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            if head_only:
                return
            with open(path, "rb") as f:
                f.seek(start)
                remaining = length
                while remaining > 0:
                    chunk = f.read(min(65536, remaining))
                    if not chunk:
                        break
                    try:
                        self.wfile.write(chunk)
                    except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                        return
                    remaining -= len(chunk)

    return Handler


def _crumbs(rel: str) -> list[tuple[str, str]]:
    parts = [p for p in rel.split("/") if p]
    out = []
    for p in parts:
        out.append((p, p))
    return out
