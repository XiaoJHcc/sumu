# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Web streaming server: a stdlib-only ThreadingHTTPServer that serves (a) a directory-browser
# front-end over a source folder, and (b) per-video HLS live-transcode streams produced by
# TranscodeEngine (headless decode -> decensor -> NVENC). Token auth guards every route except
# OPTIONS. Designed to run on a dedicated daemon thread inside the sumu app; callers drive it
# via start()/stop()/status().
#
# Concurrency model (v1): one transcode session at a time (the AI models are shared and
# BasicVSR is GPU-bound). A finished video's HLS output is cached on disk and re-served without
# re-transcoding; a request for a *different* video while one is transcoding returns 503.
from __future__ import annotations

import os
import re
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse, parse_qs

from . import index_page


class _Session:
    def __init__(self, rel: str, video_path: str, out_dir: str, engine, bitrate: str):
        self.rel = rel
        self.video_path = video_path
        self.out_dir = out_dir
        self.engine = engine
        self.bitrate = bitrate
        self.error = None
        self.done = False
        self.frames = 0
        self.total = 0
        self.thread = None

    def start(self):
        self.thread = threading.Thread(target=self._run, name="sumu-stream-session", daemon=True)
        self.thread.start()

    def _run(self):
        try:
            self.engine.run(
                self.video_path, self.out_dir, "hls", bitrate=self.bitrate,
                progress_cb=lambda f, total: self._progress(f, total),
            )
        except Exception as e:  # noqa: BLE001
            self.error = str(e)
        finally:
            self.done = True

    def _progress(self, fnum, total):
        self.frames = fnum + 1
        self.total = total or 0

    def status(self) -> dict:
        return {
            "rel": self.rel,
            "video_path": self.video_path,
            "done": self.done,
            "error": self.error,
            "frames": self.frames,
            "total": self.total,
        }


class StreamManager:
    def __init__(self, engine, root: str, cache_dir: str, bitrate: str = "8M"):
        self.engine = engine
        self.root = os.path.abspath(root)
        self.cache_dir = cache_dir
        self.bitrate = bitrate
        self.sessions: dict[str, _Session] = {}
        self._lock = threading.Lock()

    def get_or_start(self, rel: str, video_path: str) -> "_Session | None":
        with self._lock:
            s = self.sessions.get(rel)
            if s is not None:
                return s
            for o in self.sessions.values():
                if not o.done:
                    return None  # busy: one transcode at a time
            out_dir = os.path.join(self.cache_dir, _safe_key(rel))
            os.makedirs(out_dir, exist_ok=True)
            s = _Session(rel, video_path, out_dir, self.engine, self.bitrate)
            self.sessions[rel] = s
            s.start()
            return s

    def cancel_all(self):
        self.engine.cancel()
        for s in self.sessions.values():
            if not s.done:
                s.done = True

    def status(self) -> list[dict]:
        with self._lock:
            return [s.status() for s in self.sessions.values() if not s.done]


class StreamingServer:
    def __init__(self, root: str, port: int, engine, host: str = "0.0.0.0",
                 token: str = "", bitrate: str = "8M", cache_dir: str | None = None):
        self.root = os.path.abspath(root)
        self.port = int(port)
        self.host = host
        self.token = token or secrets.token_urlsafe(18)
        self.bitrate = bitrate
        self.cache_dir = cache_dir or os.path.join(os.path.dirname(self.root) or self.root,
                                                   ".sumu_stream_cache")
        os.makedirs(self.cache_dir, exist_ok=True)
        self.manager = StreamManager(engine, self.root, self.cache_dir, bitrate)
        self.httpd = None
        self._thread = None

    # ---- lifecycle ---------------------------------------------------------------

    def start(self):
        handler = _make_handler(self)
        self.httpd = ThreadingHTTPServer((self.host, self.port), handler)
        self.httpd.daemon_threads = True
        self._thread = threading.Thread(target=self.httpd.serve_forever,
                                        name="sumu-stream-http", daemon=True)
        self._thread.start()

    def stop(self):
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
            items.append({"name": name, "is_dir": is_dir, "rel": rel,
                          "size": st.st_size if not is_dir else None})
        items.sort(key=lambda it: (not it["is_dir"], it["name"].lower()))
        return items


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
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

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
                return self._send_html(404, index_page.render_message("404", "Not Found"))
            except ValueError:
                return self._send_plain(403, "Forbidden")

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
            m3u8 = os.path.join(session.out_dir, "index.m3u8")
            deadline = time.monotonic() + 30.0
            while not os.path.isfile(m3u8) and time.monotonic() < deadline:
                time.sleep(0.2)
            if not os.path.isfile(m3u8):
                if session.error:
                    return self._send_html(500, index_page.render_message("错误", session.error))
                return self._send_html(504, index_page.render_message("超时", "转码启动超时，请重试"))
            with open(m3u8, "rb") as f:
                body = f.read()
            if server.token:
                # ffmpeg writes bare segment filenames; inject ?token= so the client's segment
                # requests (which resolve relative to the playlist URL, dropping any query) still
                # carry auth.
                body = re.sub(rb"(?m)^([A-Za-z0-9._-]+\.ts)\s*$",
                              (b"\\1?token=" + server.token.encode()), body)
            return self._send(200, body, "application/vnd.apple.mpegurl",
                              {"Cache-Control": "no-cache"})

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
            seg_path = os.path.join(session.out_dir, seg)
            if not os.path.isfile(seg_path):
                return self._send_plain(404, "Not Found")
            self._send_file(seg_path, "video/mp2t", head_only)

        def _send_file(self, path: str, ctype: str, head_only: bool):
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
                    except (BrokenPipeError, ConnectionResetError):
                        return
                    remaining -= len(chunk)

    return Handler


def _crumbs(rel: str) -> list[tuple[str, str]]:
    parts = [p for p in rel.split("/") if p]
    out = []
    for p in parts:
        out.append((p, p))
    return out
