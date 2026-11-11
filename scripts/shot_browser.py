# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Visual check for the embedded browser page (native/src/player_ui_overlays.cpp
# build_browser_page + native/src/browser_view.cpp). Writes four PNGs into native/trace/:
#   browser_home.png   the page's first-screen form (title bar + address bar + centered form)
#   browser_page.png   a real WebView2 page at the local server, address bar synced to it
#   browser_play.png   a page carrying a <video>: the address bar with its (always available)
#                      play button -- the manual native entry for whatever URL the bar holds
#   browser_favorites.png
#                      the address-bar favorites dropdown open OVER the live page: it is drawn in
#                      its own top-level popup window (native/src/popup_layer.cpp), above the
#                      WebView2 child window that covers the main swapchain
#   browser_native.png the page's Native content with the floating address bar revealed (cursor
#                      in the top strip): the video is letterboxed below the bar
#   browser_native_bottom.png
#                      the same Native content with the cursor at the bottom: the address bar is
#                      hidden (floating chrome) and the normal bottom bar is shown
#   browser_settings.png
#                      the settings panel drawn OVER a captured snapshot of the live page
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

_failures = 0


def url_is(url, want):
    return (url or "").split("#")[0].rstrip("/") == want.rstrip("/")


def check(name, cond, detail=""):
    global _failures
    print(f"[{'PASS' if cond else 'FAIL'}] {name}{(' -- ' + detail) if detail else ''}",
          file=sys.stderr)
    if not cond:
        _failures += 1

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
        if path == "/noext" and os.path.isfile(TEST_VIDEO):
            self._serve_file(TEST_VIDEO)
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


def navigate(p, url, seconds=3.0):
    """Navigate using the app's browser entry point."""
    p.browser_navigate(url)
    run(p, seconds)


def play_native(p, flow, seconds=25.0):
    """Complete automatic media handover using the app's native open path."""
    intents = {}
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 6.0:
        run(p, 0.05)
        got = p.take_ui_intents()
        if got.get("open_path"):
            intents = got
            break
    if not intents:
        return {}, False
    ok = False
    try:
        args = dict(headers=intents.get("open_headers") or "",
                    cookies=intents.get("open_cookies") or "")
        if flow["opened"]:
            p.reopen(intents["open_path"], **args)
        else:
            p.open(intents["open_path"], **args)
        ok = True
    except Exception as e:  # noqa: BLE001 -- a failed manual open is the app's error path
        print(f"  native open failed: {e!r}"[:140], file=sys.stderr)
    flow["opened"] = ok
    p.notify_open_url_finished(ok)
    run(p, seconds if ok else 2.0)
    return intents, ok


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
    # Layout guard for the first screen: the page surface must span the client and the Home
    # form must be centered inside it. (A leaked ImGui "next window" size hint used to clamp the
    # page surface to the right-click menu width, which clipped the whole form -- the real-app
    # "Home is scrambled" bug.)
    st = p.browser_state()
    prect, hrect = st["page_rect"], st["home_rect"]
    check("home: page surface spans the client width",
          abs(prect[0]) <= 1 and prect[2] >= hrect[0] + hrect[2] and prect[2] > 200,
          f"page_rect={prect} home_rect={hrect}")
    check("home: form is centered in the page surface",
          hrect[2] > 100 and abs((hrect[0] + hrect[2] * 0.5) - (prect[0] + prect[2] * 0.5)) <= 2,
          f"page_rect={prect} home_rect={hrect}")

    # 2. a real (text) page through the address bar (a plain navigation), then let WebView2 paint
    flow = {"opened": False}
    navigate(p, f"{base}/page")
    for _ in range(150):
        run(p, 0.1)
        state = p.browser_state()
        if state["available"] and state["url"].startswith(f"{base}/page"):
            break
    run(p, 1.0)
    user32.SetCursorPos(*park)
    run(p, 0.3)
    grab("browser_page.png")

    # 2b. the favorites dropdown over the live page. It is a top-level window owned by the player
    #     (popup_layer.h), so it composites ABOVE the WebView2 child -- this composited desktop
    #     grab is exactly what the user sees. Eyeball: rows start under the field with its left
    #     edge and width, one line per row (title + URL, or a truncated title), the X column at
    #     the right, panel tone + hairline outline.
    st = p.browser_state()
    p.set_browser_favorites([
        {"url": st["url"], "title": "sumu browser page"},
        {"url": f"{base}/video", "title": "sumu video page whose title is long enough to be cut"},
        {"url": f"{base}/noext", "title": ""},  # no title -> the URL is the label
    ])
    run(p, 0.3)
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    click(rc.left + int((rc.right - rc.left) * 0.45), rc.top + int(54 * SCALE))
    run(p, 1.0)
    st = p.browser_state()
    pop = st["popups"]["favorites"]
    check("favorites: dropdown open above the live page",
          st["favorites_open"] and pop["visible"] and pop["frames"] > 0, f"{pop}")
    # Park the cursor below the list (still inside the page) so no row shows its hover wash.
    user32.SetCursorPos(rc.left + int((rc.right - rc.left) * 0.45),
                        rc.top + int((rc.bottom - rc.top) * 0.75))
    run(p, 0.4)
    grab("browser_favorites.png")
    user32.keybd_event(0x1B, 0, 0, 0)  # Esc closes the dropdown (it does not quit the player)
    user32.keybd_event(0x1B, 0, 0x0002, 0)
    run(p, 0.6)
    check("favorites: Esc closed the dropdown",
          not p.browser_state()["favorites_open"], f"{p.browser_state()['favorites_open']}")
    p.set_browser_favorites([])
    run(p, 0.2)

    # 3. a page with a <video>: it keeps playing inside the page (nothing switches to Native) and
    #    the address bar's play button stays available as the manual native entry
    navigate(p, f"{base}/video")
    for _ in range(150):
        run(p, 0.1)
        st = p.browser_state()
        if st["web_visible"] and st["url"].startswith(f"{base}/video"):
            break
    run(p, 1.0)
    user32.SetCursorPos(*park)
    run(p, 0.3)
    grab("browser_play.png")

    # Top-level media opens automatically; the Native address bar keeps its reveal behavior.
    navigate(p, f"{base}/noext")
    intents, opened = play_native(p, flow)
    check("native: video navigation automatically opens the media URL", opened,
          f"opened={opened} path={intents.get('open_path')!r}")
    check("native: page shows the session video", p.browser_state()["native"],
          f"{p.browser_state()}")
    p.play()
    run(p, 1.0)
    p.pause()
    # The native open re-fits the window to the video, so re-read the rect before steering.
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    top = (rc.left + int((rc.right - rc.left) * 0.5), rc.top + 8)
    bottom = (rc.left + int((rc.right - rc.left) * 0.5), rc.bottom - 14)
    user32.SetCursorPos(*top)
    run(p, 0.8)
    st = p.browser_state()
    check("native: address bar revealed in the top strip", st["addr_visible"], f"{st}")
    grab("browser_native.png")
    user32.SetCursorPos(*bottom)
    run(p, 0.8)
    st_low = p.browser_state()
    check("native: address bar hidden away from the top", not st_low["addr_visible"], f"{st_low}")
    check("native: the bar does not move the video viewport",
          st["viewport"] == st_low["viewport"], f"{st['viewport']} {st_low['viewport']}")
    grab("browser_native_bottom.png")
    print("native state:", st_low, file=sys.stderr)

    # 5. settings over a captured snapshot of the live page (the page itself must stay alive).
    navigate(p, f"{base}/page")
    ok = False
    for _ in range(150):
        run(p, 0.1)
        if p.browser_state()["web_visible"]:
            ok = True
            break
    check("settings base: live page up", ok, f"{p.browser_state()}")
    before = p.browser_state()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    user32.SetCursorPos(rc.left + int(18 * SCALE), rc.top + int(18 * SCALE))
    time.sleep(0.1)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    for _ in range(120):
        run(p, 0.05)
        if p.browser_state()["settings_backdrop"]:
            break
    st = p.browser_state()
    check("settings drawn over the captured page",
          st["settings_open"] and st["settings_backdrop"], f"{st}")
    check("settings keeps the page and its URL",
          url_is(st["url"], f"{base}/page") and st["url"] == before["url"], f"{st}")
    user32.SetCursorPos(rc.left + int((rc.right - rc.left) * 0.5), rc.top + 8)
    run(p, 0.4)
    grab("browser_settings.png")
    # close settings: back onto the gear, then click
    user32.SetCursorPos(rc.left + int(18 * SCALE), rc.top + int(18 * SCALE))
    time.sleep(0.1)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    run(p, 1.2)
    st = p.browser_state()
    check("closing settings restores the live page",
          not st["settings_open"] and st["web_visible"], f"{st}")

    p.close()
    server.shutdown()
    server.server_close()
    if _failures:
        print(f"{_failures} layout check(s) FAILED", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
