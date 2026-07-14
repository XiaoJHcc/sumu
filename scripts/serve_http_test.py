# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Minimal local HTTP file server for sumu network-playback spike.
# Serves the repo root (or --root) so test videos are reachable as:
#   http://127.0.0.1:8000/test_video.mp4
#
# IMPORTANT: stock http.server.SimpleHTTPRequestHandler does NOT implement
# HTTP Range (RFC 7233). FFmpeg needs Range to pull moov from the end of many
# MP4s and for every seek -- without it you get "moov atom not found" /
# Unexpected offset. This script implements single-range 206 responses.
#
# Usage (two terminals):
#   .venv\Scripts\python.exe scripts\serve_http_test.py
#   .venv\Scripts\python.exe scripts\play.py http://127.0.0.1:8000/test_video.mp4
#
# Expected stderr from player (network profile):
#   [sumu] decoder open network URL ...
#   [sumu] pt_ring=48 ai_ring=32 decode_ahead_max=... network
#   [sumu] network source: scrub decoder disabled ...
#   == player.open == ... network
from __future__ import annotations

import argparse
import mimetypes
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse


_RANGE_RE = re.compile(r"bytes=(\d*)-(\d*)", re.I)


class RangeRequestHandler(BaseHTTPRequestHandler):
    """Static file server with single-byte-range support (enough for FFmpeg HTTP)."""

    # Class attribute set in main() before serving -- BaseHTTPRequestHandler.__init__
    # only accepts (request, client_address, server), so functools.partial(directory=...)
    # cannot be used here.
    directory: str = os.getcwd()

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _safe_path(self) -> str | None:
        parsed = urlparse(self.path)
        rel = unquote(parsed.path).lstrip("/").replace("\\", "/")
        if not rel or rel.endswith("/"):
            # Directory listing not needed for this spike.
            return None
        # Block path traversal.
        candidate = os.path.normpath(os.path.join(self.directory, rel))
        root = os.path.abspath(self.directory)
        if not candidate.startswith(root + os.sep) and candidate != root:
            return None
        if not os.path.isfile(candidate):
            return None
        return candidate

    def do_HEAD(self) -> None:  # noqa: N802
        self._serve(head_only=True)

    def do_GET(self) -> None:  # noqa: N802
        self._serve(head_only=False)

    def _serve(self, head_only: bool) -> None:
        path = self._safe_path()
        if path is None:
            self.send_error(404, "File not found")
            return

        size = os.path.getsize(path)
        ctype = mimetypes.guess_type(path)[0] or "application/octet-stream"
        range_hdr = self.headers.get("Range")

        start, end = 0, size - 1
        status = 200
        if range_hdr:
            m = _RANGE_RE.match(range_hdr.strip())
            if not m:
                self.send_error(416, "Invalid Range")
                return
            a, b = m.group(1), m.group(2)
            try:
                if a == "" and b == "":
                    self.send_error(416, "Invalid Range")
                    return
                if a == "":
                    # suffix: bytes=-N
                    length = int(b)
                    if length <= 0:
                        self.send_error(416, "Invalid Range")
                        return
                    start = max(0, size - length)
                    end = size - 1
                else:
                    start = int(a)
                    end = int(b) if b != "" else size - 1
            except ValueError:
                self.send_error(416, "Invalid Range")
                return
            if start >= size or start < 0 or end < start:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.end_headers()
                return
            end = min(end, size - 1)
            status = 206

        length = end - start + 1
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Connection", "close")
        self.end_headers()
        if head_only:
            return

        with open(path, "rb") as f:
            f.seek(start)
            remaining = length
            buf = 64 * 1024
            while remaining > 0:
                chunk = f.read(min(buf, remaining))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return
                remaining -= len(chunk)


def main() -> int:
    ap = argparse.ArgumentParser(description="Serve a directory over HTTP (Range) for sumu network open tests")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument(
        "--root",
        default=None,
        help="Directory to serve (default: repo root, parent of scripts/)",
    )
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(args.root or os.path.dirname(here))
    if not os.path.isdir(root):
        print(f"root is not a directory: {root}", file=sys.stderr)
        return 2

    RangeRequestHandler.directory = root
    httpd = ThreadingHTTPServer((args.host, args.port), RangeRequestHandler)
    url = f"http://{args.host}:{args.port}/"
    print(f"serving {root} (HTTP Range enabled)", file=sys.stderr)
    print(f"  base:  {url}", file=sys.stderr)
    for name in ("test_video.mp4", "test_video_4k.mp4", "test_video_long.mp4"):
        p = os.path.join(root, name)
        if os.path.isfile(p):
            print(f"  try:   {url}{name}", file=sys.stderr)
    print("Ctrl-C to stop", file=sys.stderr)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped", file=sys.stderr)
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
