# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# End-to-end verification of the embedded browser page (native/src/browser_view.cpp +
# Player's browser-page state machine). No mocks: a real WebView2 loads a real page over HTTP.
#
#   1. A local Range-capable server hosts
#        /page              HTML <video muted autoplay src="/noext">, replies with
#                           Set-Cookie: sumu_e2e=1 (the cookie the play button must replay)
#        /media/movie.mp4   test_video.mp4, but answers 403 unless the request carries a
#                           Referer pointing at /page AND the sumu_e2e=1 cookie
#        /noext             the MP4 bytes under a name with no extension at all
#   2. Negative control: player.open(media_url) with no request context is rejected -- so a
#      later success really proves the context was carried.
#   3. request_open_url_popup() enters the page (browser_enter intent).
#   4. An address submission is a NAVIGATION: a plain web page appears in WebView2 with no
#      native open, no container probe and no fallback (asserted by draining UiIntents).
#   4b. Embedded video and script fetches stay inside their HTML document.
#   5. Top-level video responses automatically enter native playback, preserving query tokens,
#      percent escapes, cookies and Referer. Redirects and media downloads are covered too;
#      HTML disguised as .mp4 and unsuccessful responses must stay in the browser.
#      The open really plays, and a successful open KEEPS the page
#      up as its Native content (no browser_exit); the address bar names the media URL. The .flv
#      case (URL typed into the bar, play clicked) lives in verify_browser_input.py.
#   6. History: back / forward / reload, with the buttons' availability state. Back from Native
#      hands the content to the page view again and asks Python to pause (browser_enter).
#   R1. A cookie lookup in flight is retired by a navigation: no second handover, no stuck state.
#   D. settings / file-picker intent / export overlay / web-stream modal all stay inside the
#      URL page and give the same page back when they close. Settings floats above the LIVE page
#      in its own owned top-level popup window (the page keeps running, clicking and sounding
#      underneath), so the web view stays visible and keeps navigating while the panel is open.
#   E. Native content turns the address bar into floating chrome: hidden unless the cursor is in
#      the top strip (or the field is in use), and the video viewport never moves.
#   R3. Page view audio follows visibility: muted=false while the page is shown, muted=true
#      after leaving it, muted=false again when the page is restored.
#   9. With the page view forced unavailable (SUMU_WEBVIEW2_OFF, the same dev-hook spirit as
#      SUMU_UI_SCALE): the page degrades to browser_unavailable, navigation is inert, and a
#      plain local open still plays -- "no WebView2" must never affect anything else.
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe scripts/verify_browser.py

import ctypes
import http.server
import io
import os
import re
import sys
import threading
import time
import urllib.parse
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))
sys.path.insert(0, os.path.join(ROOT, "python", "sumu"))

import sumu_core  # noqa: E402
from sumu import i18n  # noqa: E402

TEST_VIDEO = os.path.join(ROOT, "test_video.mp4")

_failures = []
_requests = []  # (path, referer, cookie) of every /media request the server saw
_page_hits = {}  # path -> how many times the HTML page itself was fetched (reload check)

# PAGE_HTML's body background: the "known solid color" the popup-layer pixel checks compare
# against (an overlay that is really composited above the page view cannot show it there).
PAGE_BG = (0x10, 0x10, 0x12)


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}{(' -- ' + detail) if detail else ''}")
    if not cond:
        _failures.append(name)


PAGE_HTML = """<!doctype html>
<html><head><meta charset="utf-8"><title>sumu e2e __TAG__</title></head>
<body style="margin:0;background:#101012;color:#ddd;font:16px sans-serif">
<h3 style="padding:12px">sumu browser e2e __TAG__</h3>
<video src="__MEDIA__" muted autoplay playsinline width="640"></video>
</body></html>
"""

# This page never creates a media element: it FETCHES the video instead. Nothing about it may
# switch the page to Native, exactly like the <video> page above.
CT_HTML = """<!doctype html>
<html><head><meta charset="utf-8"><title>sumu ct</title></head>
<body style="margin:0;background:#101012;color:#ddd">
<h3>media fetched by script (no media element)</h3>
<script>
  fetch("/noext?ct=1").then(function (r) { return r.arrayBuffer(); }).catch(function () {});
</script>
</body></html>
"""

PLAIN_HTML = """<!doctype html>
<html><head><meta charset="utf-8"><title>sumu plain</title></head>
<body style="margin:0;background:#101012;color:#ddd;font:16px sans-serif">
<h3 style="padding:12px">a plain web page without any media</h3>
</body></html>
"""

# /page's media URL has NO extension at all, so nothing can classify it by filename. /page2
# points at the guarded endpoint: it is the page whose cookie + Referer a manual native open has
# to replay. Two different pages keep the history phases unambiguous.
PAGES = {
    "/page": PAGE_HTML.replace("__TAG__", "e2e").replace("__MEDIA__", "/noext"),
    "/page2": PAGE_HTML.replace("__TAG__", "e2e2").replace("__MEDIA__",
                                                           "/media/movie.mp4?t=e2e2"),
    "/plain": PLAIN_HTML,
    "/fake.mp4": PLAIN_HTML,
    "/ctpage": CT_HTML,
    "/pagelink": """<!doctype html><body style="margin:0;background:#101012">
        <a href="/media/movie.mp4?token=click%20video" style="position:fixed;
        left:20px;top:20px;width:600px;height:80px;background:#ddd">Play video</a>
        </body>""",
}
PAGES["/pageblank"] = PAGES["/pagelink"].replace("<a href=", '<a target="_blank" href=')


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # keep the run output readable
        pass

    def handle_one_request(self):
        # A client that drops a keep-alive connection (FFmpeg closing a probe connection, the
        # browser cancelling a range read) is normal here -- do not let socketserver print a
        # traceback for it.
        try:
            super().handle_one_request()
        except OSError:
            self.close_connection = True

    def _plain(self, code, body=b"", extra=()):
        self.send_response(code)
        ctype = next((v for k, v in extra if k.lower() == "content-type"),
                     "text/plain; charset=utf-8")
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            if k.lower() != "content-type":
                self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):  # noqa: N802 -- BaseHTTPRequestHandler's API
        path = urllib.parse.urlparse(self.path).path
        if path in PAGES:
            _page_hits[path] = _page_hits.get(path, 0) + 1
            body = PAGES[path].encode("utf-8")
            self._plain(200, body, (("Content-Type", "text/html; charset=utf-8"),
                                    ("Set-Cookie", "sumu_e2e=1; Path=/")))
            return
        if path in ("/noext", "/HEVC%20-%200.8M.mp4", "/attachment.mp4"):
            self._serve_file(TEST_VIDEO, "application/octet-stream"
                             if path == "/attachment.mp4" else "video/mp4")
            return
        if path == "/redirect":
            self._plain(302, extra=(("Location", "/noext?token=redirect%20video"),))
            return
        if path == "/denied.mp4":
            self._plain(403, b"access denied", (("Content-Type", "video/mp4"),))
            return
        if path == "/slow.mp4":
            time.sleep(1.5)
            self._serve_file(TEST_VIDEO)
            return
        if path == "/media/movie.mp4":
            referer = self.headers.get("Referer") or ""
            cookie = self.headers.get("Cookie") or ""
            _requests.append((path, referer, cookie))
            if "/page" not in referer or "sumu_e2e=1" not in cookie:
                self._plain(403, b"referer/cookie required")
                return
            self._serve_file(TEST_VIDEO)
            return
        self._plain(404, b"not found")

    def _serve_file(self, filename, ctype="video/mp4"):
        size = os.path.getsize(filename)
        rng = self.headers.get("Range")
        start, end = 0, size - 1
        partial = False
        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)", rng.strip())
            if m:
                if m.group(1):
                    start = int(m.group(1))
                if m.group(2):
                    end = min(int(m.group(2)), size - 1)
                partial = True
        if start > end or start >= size:
            self._plain(416, b"bad range")
            return
        length = end - start + 1
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", ctype)
        if ctype == "application/octet-stream":
            self.send_header("Content-Disposition", 'attachment; filename="movie.mp4"')
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
                    return  # client closed mid-transfer (FFmpeg probing)
                left -= len(chunk)


class _Server(http.server.ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def pump(player, seconds, until=None):
    """Drive the UI tick (which also pumps the window messages WebView2's async callbacks need)
    for up to `seconds`; stops early when until() is true. Returns True if until() fired."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        player.pump_messages()
        player.ui_tick()
        if until is not None and until():
            return True
        time.sleep(0.02)
    return until() if until is not None else False


def drain_until(player, predicate, seconds):
    """Pump while merging the drained UiIntents until predicate(intents) is true."""
    merged = {}
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        player.pump_messages()
        player.ui_tick()
        intents = player.take_ui_intents()
        for k, v in intents.items():
            if v:  # keep the newest non-empty value per key
                merged[k] = v
        if predicate(merged):
            return merged, True
        time.sleep(0.02)
    return merged, False


def collect_intents(player, seconds):
    """Pump for `seconds` and return every drained intent dict that carried anything, in order
    (drain_until merges keys, which cannot detect a SECOND handover -- this can)."""
    records = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        player.pump_messages()
        player.ui_tick()
        intents = player.take_ui_intents()
        if any(intents.values()):
            records.append(intents)
        time.sleep(0.02)
    return records


def url_is(url, want):
    """Exact page-URL comparison ("/page" is a prefix of "/page2", so startswith is unusable)."""
    return (url or "").split("#")[0].rstrip("/") == want.rstrip("/")


def find_window():
    """This process's Player window (the script drives real mouse clicks on it for a few
    regression phases; everything else uses the scripted APIs)."""
    user32 = ctypes.windll.user32
    pid = os.getpid()
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def _enum(h, lp):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, buf, 256)
        if buf.value == "sumu":
            p = wintypes.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(p))
            if p.value == pid and user32.IsWindowVisible(h):
                found.append(h)
        return True

    user32.EnumWindows(_enum, 0)
    return found[-1] if found else 0


def client_origin(hwnd):
    """Screen position of the client area's top-left corner, and its size."""
    user32 = ctypes.windll.user32
    pt = wintypes.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    rc = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rc))
    return pt.x, pt.y, rc.right - rc.left, rc.bottom - rc.top


def move_client(hwnd, x, y):
    """Move the cursor to a client-relative point (no click)."""
    user32 = ctypes.windll.user32
    ox, oy, _, _ = client_origin(hwnd)
    user32.SetCursorPos(int(ox + x), int(oy + y))
    time.sleep(0.05)

def screen_pixel(x, y):
    """Color of one desktop pixel as (r, g, b). Reads the COMPOSITED desktop, so it shows what the
    user actually sees -- which is how a popup layer above the page view is proven."""
    user32 = ctypes.windll.user32
    gdi32 = ctypes.windll.gdi32
    hdc = user32.GetDC(0)
    try:
        color = gdi32.GetPixel(hdc, int(x), int(y)) & 0xFFFFFF  # COLORREF 0x00BBGGRR
    finally:
        user32.ReleaseDC(0, hdc)
    return color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF


def ensure_foreground(hwnd, player):
    """Bring this window to the foreground. SetForegroundWindow alone is refused when another
    process owns the foreground (Windows' foreground lock); attaching to the owner's thread helps
    -- and a real click on the window always activates it. Returns whether it really is
    foreground (the ImGui backend only tracks the mouse while the window is)."""
    user32 = ctypes.windll.user32
    if user32.GetForegroundWindow() == hwnd:
        return True
    k32 = ctypes.windll.kernel32
    fg = user32.GetForegroundWindow()
    tid_fg = user32.GetWindowThreadProcessId(fg, None)
    tid_self = k32.GetCurrentThreadId()
    attached = bool(user32.AttachThreadInput(tid_self, tid_fg, True))
    user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0001 | 0x0002)  # NOZORDER off / topmost, NOSIZE|NOMOVE
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    user32.SetFocus(hwnd)
    if attached:
        user32.AttachThreadInput(tid_self, tid_fg, False)
    if user32.GetForegroundWindow() == hwnd:
        return True
    # Activation click (allowed even under the foreground lock): click the middle of the client
    # area -- a plain click on the video/page is harmless.
    _, _, cw, ch = client_origin(hwnd)
    click_client(hwnd, cw * 0.5, ch * 0.5)
    pump(player, 0.5)
    return user32.GetForegroundWindow() == hwnd


def toggle_settings(hwnd, want_open, player):
    """Click the top bar's gear until browser_state()'s settings flag matches `want_open` (a
    single click can be eaten by window activation on the very first interaction)."""
    for _ in range(4):
        if bool(player.browser_state()["settings_open"]) == want_open:
            return True
        click_client(hwnd, *top_bar_icon_center(hwnd, 0))
        pump(player, 0.8)
    return bool(player.browser_state()["settings_open"]) == want_open


def move_until_addr_visible(hwnd, player, y, want_visible, tries=3):
    """Park the cursor at client y and pump until browser_state()'s addr_visible matches.
    A single SetCursorPos can be lost while the window is still settling (foreground change /
    video-driven resize), which used to make this phase flaky."""
    for _ in range(tries):
        _, _, cw, _ = client_origin(hwnd)
        move_client(hwnd, cw * 0.5, y)
        pump(player, 0.6)
        if bool(player.browser_state()["addr_visible"]) == want_visible:
            return True
    return bool(player.browser_state()["addr_visible"]) == want_visible


def click_client(hwnd, x, y):
    """Real left click at a client-relative point."""
    user32 = ctypes.windll.user32
    ox, oy, _, _ = client_origin(hwnd)
    user32.SetCursorPos(int(ox + x), int(oy + y))
    time.sleep(0.05)
    user32.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
    user32.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP


def top_bar_icon_center(hwnd, index):
    """Center of top-bar icon button `index` (settings/open/url/stream/export = 0..4) in client
    coords. The bar's metrics are 96-DPI bases scaled by the window's DPI (2px inset, 32px
    buttons, 2px gaps -- see build_top_bar)."""
    user32 = ctypes.windll.user32
    s = user32.GetDpiForWindow(hwnd) / 96.0
    x = s * (2.0 + index * 34.0 + 16.0)
    y = s * 18.0
    return x, y


def parse_intents(d):
    return d



# app.py's open bookkeeping, shared by every phase that starts a native open through the page:
# "opened" mirrors native's Player::open() state so an already-open player uses reopen().
FLOW = {"opened": False}


def navigate(player, url, settle=3.0):
    """Navigate and collect every intent emitted while the page settles."""
    player.browser_navigate(url)
    records = collect_intents(player, settle)
    return records


def play_native(player, flow, seconds=10.0, manual=True):
    """Complete a manual or automatic handover using the app's real open/reopen path."""
    if manual:
        player.browser_play_native()
    intents, got = drain_until(player, lambda i: i.get("open_path"), seconds)
    if not got:
        return {}, False
    ok = False
    try:
        args = dict(headers=intents.get("open_headers") or "",
                    cookies=intents.get("open_cookies") or "")
        if flow["opened"]:
            player.reopen(intents["open_path"], **args)
        else:
            player.open(intents["open_path"], **args)
        ok = True
    except Exception as e:  # noqa: BLE001 -- a failed manual open now stays a visible error
        print(f"  native open failed: {e!r}"[:160])
    flow["opened"] = ok
    player.notify_open_url_finished(ok)
    pump(player, 1.5)
    return intents, ok


def main():
    check("test video exists", os.path.isfile(TEST_VIDEO), TEST_VIDEO)
    if not os.path.isfile(TEST_VIDEO):
        return 1

    server = _Server(("127.0.0.1", 0), _Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    page_url = f"{base}/page"
    page2_url = f"{base}/page2"
    plain_url = f"{base}/plain"
    media_url = f"{base}/media/movie.mp4"

    flow = FLOW

    player = sumu_core.Player(1280, 720, False)
    i18n.apply_to_player(player)
    pump(player, 0.5)

    # ---- 2. negative control: the guarded media URL needs the page's request context -----
    try:
        player.open(media_url)
        denied = False
    except Exception:  # noqa: BLE001 -- a 403 must surface as a failed open
        denied = True
    check("media URL denied without referer/cookie", denied)
    check("player still closed after the denied open", player.frame_count() == 0)

    # ---- 3. enter the browser page ------------------------------------------------------
    player.request_open_url_popup()
    intents, ok = drain_until(player, lambda i: i.get("browser_enter"), 5.0)
    check("browser page entered (browser_enter intent)", ok)
    state = player.browser_state()
    check("browser page state reports the page", bool(state["page"]))
    check("page starts on its Home content", state["content"] == "home", f"{state}")
    check("reload is unavailable on Home (nothing loaded)", not state["can_reload"],
          f"{state}")
    # Layout regression: the page surface must span the whole client width, and the Home form
    # must be centered inside it. (A leaked ImGui "next window" size constraint used to clamp the
    # page surface to the right-click menu's width, which clipped the entire form and scrolled the
    # empty bar's field into view -- the real-app "Home is scrambled" bug.)
    prect = state["page_rect"]
    hrect = state["home_rect"]
    check("Home: page surface spans the client width",
          prect[2] > 200 and abs(prect[0]) <= 1 and prect[2] >= hrect[0] + hrect[2],
          f"page_rect={prect} home_rect={hrect}")
    check("Home: page surface starts right under the address bar",
          abs(prect[1] - state["bounds"][1]) <= 1, f"page_rect={prect} bounds={state['bounds']}")
    check("Home: form is horizontally centered in the page surface",
          hrect[2] > 100 and abs((hrect[0] + hrect[2] * 0.5) - (prect[0] + prect[2] * 0.5)) <= 2,
          f"page_rect={prect} home_rect={hrect}")
    check("Home: form is not scrolled out of view",
          abs(state["page_rect"][2] - state["bounds"][2]) <= 1, f"{state}")
    pump(player, 15.0, lambda: player.browser_state()["available"]
         or player.browser_state()["failed"])
    state = player.browser_state()
    check("WebView2 controller available", state["available"], f"{state}")
    check("WebView2 not reported as failed", not state["failed"])
    if not state["available"]:
        player.close()
        server.shutdown()
        server.server_close()
        print("\nbrowser unavailable -- the rest of the e2e cannot run on this machine")
        return 1

    # ---- 4. an address submission is a NAVIGATION, never a native open -----------------
    records = navigate(player, plain_url)
    ok = pump(player, 20.0, lambda: url_is(player.browser_state()["url"], plain_url)
              and player.browser_state()["web_visible"])
    state = player.browser_state()
    check("a submitted address starts no native open",
          not any(r.get("open_path") for r in records),
          f"{[r.get('open_path') for r in records]}")
    check("the web page shows in WebView2 right away", ok, f"{state}")
    check("the web page is not (and never becomes) the native content",
          not state["native"] and not state["loading"] and state["content"] == "web",
          f"{state}")
    check("address bar mirrors the submitted page", url_is(state["address"], plain_url),
          state["address"])

    # ---- 4b. in-page media keeps playing INSIDE the page -------------------------------
    records = navigate(player, page_url)
    ok = pump(player, 20.0, lambda: url_is(player.browser_state()["url"], page_url)
              and player.browser_state()["web_visible"])
    pump(player, 3.0)  # long enough for the <video src> request to be seen and acted on
    state = player.browser_state()
    records += collect_intents(player, 1.0)
    check("a page with a video element loads in the page view", ok, f"{state}")
    check("in-page media does not switch the page to native",
          not state["native"] and state["content"] == "web" and state["web_visible"],
          f"{state}")
    check("in-page media is never handed to the decoder",
          not any(r.get("open_path") for r in records),
          f"{[r.get('open_path') for r in records]}")
    check("reload is available once a page is loaded", state["can_reload"], f"{state}")

    ct_url = f"{base}/ctpage"
    records = navigate(player, ct_url)
    ok = pump(player, 20.0, lambda: url_is(player.browser_state()["url"], ct_url))
    pump(player, 2.5)
    state = player.browser_state()
    records += collect_intents(player, 1.0)
    check("a script-fetched video keeps the page view up",
          ok and state["web_visible"], f"{state}")
    check("a script-fetched video does not switch to native",
          not state["native"] and not any(r.get("open_path") for r in records),
          f"{state} {[r.get('open_path') for r in records]}")

    # ---- 5. top-level media automatically hands over, without a play click -------------
    for suffix in ("/fake.mp4", "/denied.mp4"):
        records = navigate(player, base + suffix)
        check(f"non-video response {suffix} never opens natively",
              not any(r.get("open_path") for r in records)
              and not player.browser_state()["native"])

    player.browser_navigate(base + "/slow.mp4")
    pump(player, 0.3)
    records = navigate(player, plain_url, settle=3.0)
    check("superseded slow video response never opens natively",
          not any(r.get("open_path") for r in records)
          and not player.browser_state()["loading"])

    for source, expected in (
        ("/HEVC%20-%200.8M.mp4?token=test%20token", "/HEVC%20-%200.8M.mp4?token=test%20token"),
        ("/attachment.mp4?token=download", "/attachment.mp4?token=download"),
        ("/redirect", "/noext?token=redirect%20video"),
    ):
        navigate(player, plain_url)
        player.browser_navigate(base + source)
        auto, auto_opened = play_native(player, flow, manual=False)
        check(f"automatic video handover: {source}",
              auto_opened and auto.get("open_path") == base + expected, repr(auto))
        check("automatic video handover happens only once",
              not any(r.get("open_path") for r in collect_intents(player, 0.8)))

    for link_page in ("/pagelink", "/pageblank"):
        navigate(player, base + link_page)
        hwnd = find_window()
        check("own window found for real media-link click", bool(hwnd))
        if hwnd:
            ensure_foreground(hwnd, player)
            bounds = player.browser_state()["bounds"]
            scale = ctypes.windll.user32.GetDpiForWindow(hwnd) / 96.0
            click_client(hwnd, bounds[0] + 150 * scale, bounds[1] + 50 * scale)
            linked, linked_opened = play_native(player, flow, manual=False)
            check(f"clicking a media link automatically opens native playback ({link_page})",
                  linked_opened, repr(linked))
            check("link handover preserves Referer and cookies",
                  link_page in (linked.get("open_headers") or "")
                  and "sumu_e2e=1" in (linked.get("open_cookies") or ""), repr(linked))

    navigate(player, page_url)
    media_token_url = f"{base}/noext?token=%E4%B8%AD%E6%96%87&t=automatic"
    player.browser_navigate(media_token_url)
    intents, opened = play_native(player, flow, manual=False)
    check("a video response hands its URL over without clicking play",
          intents.get("open_path") == media_token_url, repr(intents.get("open_path")))
    check("automatic open plays query token + percent-escapes + no extension", opened,
          f"opened={opened} path={intents.get('open_path')!r}")
    check("the automatic open replays the engine's cookie jar",
          "sumu_e2e=1" in (intents.get("open_cookies") or ""),
          repr(intents.get("open_cookies")))
    state = player.browser_state()
    check("a successful automatic open becomes the page's native content",
          state["native"] and state["content"] == "native", f"{state}")
    check("no cookie wait left after the handover",
          not state["cookie_pending"], f"{state}")
    check("address bar keeps the media URL", state["address"] == media_token_url,
          state["address"])
    check("page view hidden + muted while native",
          not state["web_visible"] and state["muted"], f"{state}")
    check("reload is disabled on the native content", not state["can_reload"], f"{state}")
    if opened:
        check("session is network", player.is_network())
        check("has frames from the automatic source", player.frame_count() > 0,
              f"frames={player.frame_count()}")
        player.play()
        pump(player, 1.0)
        check("automatic playback advances", player.current_frame() > 0,
              f"frame={player.current_frame()}")
        player.pause()

    # ---- 5a. Native address bar: floating chrome (reveal) ------------------------------
    hwnd = find_window()
    check("own window found for the reveal test", bool(hwnd))
    if hwnd:
        user32 = ctypes.windll.user32
        user32.SetWindowPos(hwnd, -1, 60, 40, 0, 0, 0x1)
        check("native reveal: window really is foreground", ensure_foreground(hwnd, player))
        pump(player, 0.3)
        _, _, cw, ch = client_origin(hwnd)
        click_client(hwnd, cw * 0.5, ch * 0.5)  # park the cursor away from the top
        pump(player, 1.0)
        mid = player.browser_state()
        revealed = move_until_addr_visible(hwnd, player, 6, True)   # top reveal strip
        top = player.browser_state()
        hidden = move_until_addr_visible(hwnd, player, ch * 0.5, False)
        mid2 = player.browser_state()
        check("native: address bar hidden while the cursor is away from the top",
              mid["content"] == "native" and not mid["addr_visible"], f"{mid}")
        check("native: address bar revealed at the top strip", revealed, f"{top}")
        check("native: address bar hides again when the cursor leaves", hidden, f"{mid2}")
        check("native: showing the address bar does not move the video viewport",
              mid["viewport"] == top["viewport"] == mid2["viewport"],
              f"{mid['viewport']} {top['viewport']} {mid2['viewport']}")
        # Typing keeps the floating bar on screen even with the cursor away from the top.
        move_client(hwnd, cw * 0.5, 6)
        pump(player, 0.5)
        click_client(hwnd, cw * 0.45, (user32.GetDpiForWindow(hwnd) / 96.0) * (36 + 18))
        pump(player, 0.6)
        move_client(hwnd, cw * 0.5, ch * 0.6)
        pump(player, 1.0)
        typing = player.browser_state()
        check("native: the address bar stays while the field is being used",
              typing["addr_visible"], f"{typing}")
        # Leave the field (click the video area) so later phases start clean.
        click_client(hwnd, cw * 0.5, ch * 0.5)
        pump(player, 0.6)

    # ---- 6. back from native: the page view takes the content back ----------------------
    player.browser_go_back()
    records = collect_intents(player, 2.0)
    state = player.browser_state()
    check("back from native leaves the native content", not state["native"], f"{state}")
    check("back from native shows the page view again",
          state["content"] == "web" and state["web_visible"] and not state["muted"], f"{state}")
    check("back from native asks Python to pause (browser_enter)",
          any(r.get("browser_enter") for r in records),
          f"{[r.get('browser_enter') for r in records]}")
    check("address bar follows the page again once the view is back",
          url_is(state["address"], state["url"]), f"{state}")

    # ---- 6b. history: back / forward / reload ------------------------------------------
    navigate(player, page_url)
    pump(player, 20.0, lambda: url_is(player.browser_state()["url"], page_url))
    navigate(player, page2_url)
    state = player.browser_state()
    check("history: back is available", state["can_back"], f"{state}")
    check("history: forward is not", not state["can_forward"], f"{state}")
    player.browser_go_back()
    ok = pump(player, 20.0, lambda: url_is(player.browser_state()["url"], page_url))
    state = player.browser_state()
    check("history: back returns to the previous page", ok, state["url"])
    check("history: forward is available after back", state["can_forward"], f"{state}")
    player.browser_go_forward()
    ok = pump(player, 20.0, lambda: url_is(player.browser_state()["url"], page2_url))
    check("history: forward returns to the later page", ok, player.browser_state()["url"])
    hits = _page_hits.get("/page2", 0)
    player.browser_reload()
    ok = pump(player, 20.0, lambda: _page_hits.get("/page2", 0) > hits)
    check("history: reload re-fetches the page", ok,
          f"hits {hits} -> {_page_hits.get('/page2', 0)}")

    # ---- R1. a navigation retires a cookie lookup in flight -----------------------------
    navigate(player, page_url)
    pump(player, 20.0, lambda: url_is(player.browser_state()["url"], page_url))
    player.browser_play_native()  # starts the CookieManager lookup for the bar's URL
    check("cookie lookup is in flight before the navigation",
          player.browser_state()["cookie_pending"], f"{player.browser_state()}")
    # No pump in between: the navigation is the newest action and must win outright.
    player.browser_navigate(plain_url)
    records = collect_intents(player, 3.0)
    records += collect_intents(player, 2.0)  # give a retired answer time to arrive late
    check("the navigation retires the pending lookup",
          not player.browser_state()["cookie_pending"], f"{player.browser_state()}")
    check("the retired lookup never hands a URL over",
          not any(r.get("open_path") for r in records),
          f"{[r.get('open_path') for r in records]}")
    ok = pump(player, 20.0, lambda: url_is(player.browser_state()["url"], plain_url))
    check("the navigation itself still lands", ok, player.browser_state()["url"])

    # ---- 8. only the page's own exit leaves it -----------------------------------------
    player.exit_browser_page()
    records = collect_intents(player, 1.0)
    check("explicit close leaves the page",
          not player.browser_state()["page"], f"{player.browser_state()}")
    check("explicit close emits browser_exit",
          any(r.get("browser_exit") for r in records),
          f"{[r.get('browser_exit') for r in records]}")

    player.close()

    # ---- 9. no WebView2: the page degrades, everything else still works ------------------
    os.environ["SUMU_WEBVIEW2_OFF"] = "1"
    plain = sumu_core.Player(1024, 640, False)
    i18n.apply_to_player(plain)
    pump(plain, 0.3)
    try:
        plain.request_open_url_popup()
        intents, ok = drain_until(plain, lambda i: i.get("browser_enter"), 5.0)
        check("page still opens without a browser engine", ok)
        pump(plain, 2.0, lambda: plain.browser_state()["failed"])
        state = plain.browser_state()
        check("page reports browser unavailable", state["failed"], f"{state}")
        check("no page URL without an engine", state["url"] == "", repr(state["url"]))
        plain.browser_navigate(page_url)  # must be a no-op, not a crash
        pump(plain, 0.5)
        check("navigating without an engine is inert", plain.browser_state()["url"] == "")
        plain.exit_browser_page()
        pump(plain, 0.3)
        try:
            plain.open(TEST_VIDEO)
            opened_plain = True
        except Exception as e:  # noqa: BLE001
            opened_plain = False
            print(f"  local open error: {e!r}")
        check("local file open unaffected by the missing engine", opened_plain)
        if opened_plain:
            check("local playback has frames", plain.frame_count() > 0,
                  f"frames={plain.frame_count()}")
    finally:
        plain.close()
        del os.environ["SUMU_WEBVIEW2_OFF"]

    # ---- R3. page-view audio follows visibility -----------------------------------------
    mp = sumu_core.Player(1024, 640, False)
    i18n.apply_to_player(mp)
    pump(mp, 0.3)
    try:
        mp.request_open_url_popup()
        pump(mp, 1.0)
        check("page starts muted while hidden (first-screen form)", mp.browser_state()["muted"],
              f"{mp.browser_state()}")
        navigate(mp, page_url)
        ok = pump(mp, 20.0, lambda: url_is(mp.browser_state()["url"], page_url)
                  and not mp.browser_state()["muted"])
        check("page visible -> unmuted", ok, f"{mp.browser_state()}")
        mp.exit_browser_page()
        pump(mp, 0.5)
        check("page closed -> muted", mp.browser_state()["muted"], f"{mp.browser_state()}")
        mp.request_open_url_popup()
        pump(mp, 1.0)
        state = mp.browser_state()
        check("page restored -> unmuted", state["web"] and not state["muted"], f"{state}")
    finally:
        mp.close()

    # ---- D. overlays stay INSIDE the page (settings / file picker / export) -------------
    op = sumu_core.Player(1100, 700, False)
    i18n.apply_to_player(op)
    pump(op, 0.5)
    try:
        op.request_open_url_popup()
        pump(op, 1.0)
        navigate(op, page_url)
        ok = pump(op, 20.0, lambda: op.browser_state()["web_visible"])
        check("overlay base: page + page view up", ok, f"{op.browser_state()}")
        hwnd = find_window()
        check("overlay base: own window found", bool(hwnd))
        if hwnd:
            user32 = ctypes.windll.user32
            user32.SetWindowPos(hwnd, -1, 60, 40, 0, 0, 0x1)
            check("overlay base: window really is foreground", ensure_foreground(hwnd, op))
            pump(op, 0.3)
            ox, oy, _, _ = client_origin(hwnd)
            before = op.browser_state()
            # Settings: real click on the top bar's gear. The panel floats above the LIVE page in
            # its own owned top-level window (native/src/popup_layer.cpp); the page itself keeps
            # running, clicking and sounding underneath.
            check("settings toggled open by the real gear click", toggle_settings(hwnd, True, op))
            ok = pump(op, 6.0, lambda: op.browser_state()["popups"]["settings"]["visible"])
            state = op.browser_state()
            panel = state["popups"]["settings"]
            check("settings panel rises into its own top-level popup window",
                  ok and state["settings_open"] and state["settings_popup"] and panel["hwnd"]
                  and panel["frames"] > 0, f"{panel}")
            check("settings keeps the page and its URL",
                  state["page"] and url_is(state["url"], page_url), f"{state}")
            check("settings keeps the live page view up and audible",
                  state["web_visible"] and not state["muted"], f"{state}")
            check("settings does not rebuild the page view (same history)",
                  state["url"] == before["url"] and state["can_back"] == before["can_back"],
                  f"{before} -> {state}")
            bounds = state["bounds"]
            web_rect = (ox + bounds[0], oy + bounds[1], bounds[2], bounds[3])
            panel_rect = panel["screen_rect"]
            check("the settings panel window is a top-level owner window",
                  user32.GetWindow(panel["hwnd"], 4) == hwnd and
                  user32.IsWindowVisible(panel["hwnd"]),  # GW_OWNER = 4
                  f"owner={user32.GetWindow(panel['hwnd'], 4)} player={hwnd}")
            check("the settings panel overlaps the web view rect",
                  panel_rect[0] < web_rect[0] + web_rect[2] and
                  panel_rect[0] + panel_rect[2] > web_rect[0] and
                  panel_rect[1] < web_rect[1] + web_rect[3] and
                  panel_rect[1] + panel_rect[3] > web_rect[1],
                  f"panel={panel_rect} web={web_rect}")
            sc = user32.GetDpiForWindow(hwnd) / 96.0
            check("the panel window is the settings panel's own 260px width",
                  abs(panel_rect[2] - 260 * sc) <= 2, f"panel={panel_rect} scale={sc}")
            # A pixel inside the panel is the panel's surface, not the page's: the panel really is
            # composited above the page view rather than buried underneath it.
            panel_pixel = screen_pixel(panel_rect[0] + panel_rect[2] * 0.5,
                                       panel_rect[1] + panel_rect[3] * 0.5)
            check("a screen pixel inside the panel is not the page background",
                  panel_pixel != PAGE_BG, f"{panel_pixel} page_bg={PAGE_BG}")
            # Navigating while the panel is open: the page underneath really navigates, and the
            # panel stays up over it (nothing to re-capture, nothing to invalidate).
            navigate(op, plain_url)
            ok = pump(op, 25.0, lambda: url_is(op.browser_state()["url"], plain_url))
            state = op.browser_state()
            check("settings open: the page still navigates underneath",
                  ok and state["page"] and state["settings_open"], f"{state}")
            check("settings open: the panel stays up and the page stays live",
                  state["popups"]["settings"]["visible"] and state["web_visible"]
                  and not state["muted"], f"{state}")
            frames_before = state["popups"]["settings"]["frames"]
            pump(op, 0.5)
            check("settings open: the panel keeps rendering its own frame",
                  op.browser_state()["popups"]["settings"]["frames"] >= frames_before,
                  f"{frames_before} -> {op.browser_state()['popups']['settings']['frames']}")
            toggle_settings(hwnd, False, op)
            pump(op, 1.5)
            state = op.browser_state()
            check("closing settings leaves the live page as it was",
                  state["page"] and not state["settings_open"] and state["web_visible"]
                  and url_is(state["url"], plain_url), f"{state}")
            check("closing settings hides its popup window",
                  not state["popups"]["settings"]["visible"], f"{state}")
            # Back to the page the export assertions below use (same page session, just history).
            navigate(op, page_url)
            ok = pump(op, 25.0, lambda: (op.browser_state()["web_visible"]
                                         and url_is(op.browser_state()["url"], page_url)))
            check("navigation after the panel still works", ok, f"{op.browser_state()}")
            # Open file: the intent is recorded, the page stays; a cancelled picker (no path
            # reaches Python here) must change nothing.
            ok = False
            intents = {}
            for _ in range(3):  # a click can be eaten while the window settles after the panel
                click_client(hwnd, *top_bar_icon_center(hwnd, 1))
                intents, ok = drain_until(op, lambda i: i.get("open_dialog"), 3.0)
                if ok:
                    break
            check("open-file click records the picker intent", ok)
            check("open-file click does not leave the page",
                  op.browser_state()["page"] and not intents.get("open_path"),
                  f"{op.browser_state()}")
            pump(op, 0.5)
            check("cancelled picker keeps the page content",
                  op.browser_state()["web_visible"], f"{op.browser_state()}")
        op.set_export_mode(True)
        pump(op, 0.5)
        state = op.browser_state()
        check("export keeps the URL page alive", state["page"] and state["export"], f"{state}")
        check("page view hidden during export",
              not state["web_visible"] and state["muted"], f"{state}")
        op.set_export_mode(False)
        pump(op, 1.0)
        state = op.browser_state()
        check("leaving export restores the same URL page",
              state["page"] and not state["export"] and state["web_visible"]
              and url_is(state["url"], page_url), f"{state}")
    finally:
        op.close()

    # ---- D2. the web-stream modal also lives inside the page ---------------------------
    sp = sumu_core.Player(1000, 640, False)
    i18n.apply_to_player(sp)
    pump(sp, 0.5)
    try:
        sp.request_open_url_popup()
        pump(sp, 1.0)
        navigate(sp, page_url)
        ok = pump(sp, 20.0, lambda: sp.browser_state()["web_visible"])
        check("stream base: page view up", ok, f"{sp.browser_state()}")
        hwnd = find_window()
        if hwnd:
            user32 = ctypes.windll.user32
            user32.SetWindowPos(hwnd, -1, 60, 40, 0, 0, 0x1)
            check("stream: window really is foreground", ensure_foreground(hwnd, sp))
            pump(sp, 0.3)
            for _ in range(3):
                click_client(hwnd, *top_bar_icon_center(hwnd, 3))  # web server
                pump(sp, 0.8)
                if sp.browser_state()["stream_open"]:
                    break
            state = sp.browser_state()
            check("stream modal opens inside the URL page",
                  state["page"] and state["stream_open"], f"{state}")
            check("page view steps aside for the modal",
                  not state["web_visible"] and state["muted"], f"{state}")
    finally:
        sp.close()

    server.shutdown()
    server.server_close()

    if _failures:
        print(f"\n{len(_failures)} check(s) FAILED: {_failures}")
        return 1
    print("\nall browser page checks passed")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(line_buffering=True)  # type: ignore[attr-defined]
    sys.exit(main())
