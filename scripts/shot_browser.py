# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Visual check for the embedded browser page (native/src/player_ui_overlays.cpp
# build_browser_page + native/src/browser_view.cpp). Writes three PNGs into native/trace/:
#   browser_home.png  the page's first-screen form (title bar + address bar + centered form)
#   browser_page.png  a real WebView2 page at the local server, address bar synced to it
#   browser_play.png  a page carrying a <video>: the address bar's native-play button turns
#                     active once the page's own media request has been captured
# Layout is eyeballed -- no assertions here; scripts/verify_browser.py owns the functional
# checks.
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe scripts/shot_browser.py
import ctypes
import http.server
import os
import re
import sys
import threading
import time
import urllib.parse
from ctypes import wintypes
from PIL import ImageGrab

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))
sys.path.insert(0, os.path.join(ROOT, "python", "sumu"))

import sumu_core  # noqa: E402
from sumu import i18n  # noqa: E402

OUTDIR = os.path.join(ROOT, "native", "trace")
SCALE = 1.5  # the shot harness runs at SUMU_UI_SCALE=1.5, same as shot_modals.py
TEST_VIDEO = os.path.join(ROOT, "test_video.mp4")

DEMO_HTML = """<!doctype html>
<html><head><meta charset="utf-8"><title>sumu</title></head>
<body style="margin:0;background:#141518;color:#e6e6e6;font:16px 'Segoe UI',sans-serif">
<div style="padding:24px 32px">
  <h1 style="font-size:28px;margin:0 0 8px">sumu browser page</h1>
  <p style="color:#9aa0a6;margin:0 0 20px">real WebView2 content below the address bar</p>
  <div style="display:flex;gap:12px">
    <div style="width:180px;height:110px;background:#3965a8;border-radius:10px"></div>
    <div style="width:180px;height:110px;background:#2c2f36;border-radius:10px"></div>
    <div style="width:180px;height:110px;background:#2c2f36;border-radius:10px"></div>
  </div>
</div>
</body></html>
"""

VIDEO_HTML = """<!doctype html>
<html><head><meta charset="utf-8"><title>sumu</title></head>
<body style="margin:0;background:#0e0f12;color:#e6e6e6;font:16px 'Segoe UI',sans-serif">
<div style="padding:20px 32px">
  <h1 style="font-size:24px;margin:0 0 12px">sumu video page</h1>
  <video src="/movie.mp4" muted autoplay playsinline width="720"
         style="border-radius:10px;background:#000"></video>
</div>
</body></html>
"""


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def handle_one_request(self):
        # The browser/FFmpeg routinely drop keep-alive connections mid-probe; that is not an
        # error worth a traceback here.
        try:
            super().handle_one_request()
        except OSError:
            self.close_connection = True

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        path = urllib.parse.urlparse(self.path).path
        if path == "/page":
            self._send(200, DEMO_HTML.encode("utf-8"), "text/html; charset=utf-8")
            return
        if path == "/video":
            self._send(200, VIDEO_HTML.encode("utf-8"), "text/html; charset=utf-8")
            return
        if path == "/movie.mp4" and os.path.isfile(TEST_VIDEO):
            self._serve_file(TEST_VIDEO)
            return
        self._send(404, b"not found", "text/plain")

    def _serve_file(self, filename):
        size = os.path.getsize(filename)
        rng = self.headers.get("Range")
        start, end, partial = 0, size - 1, False
        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)", rng.strip())
            if m:
                if m.group(1):
                    start = int(m.group(1))
                if m.group(2):
                    end = min(int(m.group(2)), size - 1)
                partial = True
        length = max(0, end - start + 1)
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        with open(filename, "rb") as f:
            f.seek(start)
            left = length
            while left > 0:
                chunk = f.read(min(65536, left))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except OSError:
                    return
                left -= len(chunk)


class _Server(http.server.ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def run(p, seconds):
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < seconds:
        p.pump_messages()
        p.ui_tick()
        time.sleep(0.02)


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    os.environ.setdefault("SUMU_UI_SCALE", str(SCALE))
    server = _Server(("127.0.0.1", 0), _Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"

    p = sumu_core.Player(1300, 800, False)
    i18n.apply_to_player(p)
    run(p, 1.5)

    user32 = ctypes.windll.user32
    my_pid = os.getpid()
    hwnd = 0

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def _enum(h, lp):
        nonlocal hwnd
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, buf, 256)
        if buf.value == "sumu":
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == my_pid:
                hwnd = h
                return False
        return True

    user32.EnumWindows(_enum, 0)
    if not hwnd:
        print("own sumu window not found!", file=sys.stderr)
        return

    rc = wintypes.RECT()
    user32.SetWindowPos(hwnd, -1, 60, 10, 0, 0, 0x1)  # TOPMOST
    user32.SetForegroundWindow(hwnd)
    run(p, 0.3)
    user32.GetWindowRect(hwnd, ctypes.byref(rc))

    def grab(name):
        ImageGrab.grab(bbox=(rc.left, rc.top, rc.right, rc.bottom)).save(
            os.path.join(OUTDIR, name))
        print("wrote", name, file=sys.stderr)

    def click(cx, cy):
        user32.SetCursorPos(cx, cy)
        user32.mouse_event(0x0002, 0, 0, 0, 0)
        user32.mouse_event(0x0004, 0, 0, 0, 0)

    def icon_x(idx):
        return rc.left + int((2 + 16) * SCALE + idx * (32 + 2) * SCALE)

    ty = rc.top + int(18 * SCALE)
    park = (rc.left + 900, rc.top + 900)

    # 1. enter the page through the real top-bar button (icon index 2 = URL) -> first screen
    click(icon_x(2), ty)
    run(p, 1.5)
    user32.SetCursorPos(*park)
    run(p, 0.3)
    grab("browser_home.png")

    # 2. a real (text) page: the address bar's own Go path, then let WebView2 paint it
    p.browser_navigate(f"{base}/page")
    for _ in range(150):
        run(p, 0.1)
        state = p.browser_state()
        if state["available"] and state["url"].startswith(f"{base}/page"):
            break
    run(p, 1.0)
    user32.SetCursorPos(*park)
    run(p, 0.3)
    grab("browser_page.png")

    # 3. a page with a <video>: the native-play button lights up once its media request is in
    p.browser_navigate(f"{base}/video")
    for _ in range(150):
        run(p, 0.1)
        if p.browser_state()["candidate"]:
            break
    run(p, 1.0)
    user32.SetCursorPos(*park)
    run(p, 0.3)
    grab("browser_play.png")

    p.close()
    server.shutdown()
    server.server_close()


if __name__ == "__main__":
    main()
