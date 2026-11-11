# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Real-input verification for the embedded browser page: the two problems that a scripted API
# cannot see.
#
#   1. Keyboard focus hand-back. The page view is a real child HWND, so clicking it moves the
#      Win32 focus there and the parent never sees a keystroke (the address bar looked
#      "untypeable"). Clicking the page view, then the address bar, then Ctrl+A + typing a new
#      URL + Enter must really navigate -- and the programmatic focus path (re-entering the page
#      while it is already open) must take the focus back as well.
#   1d. The floating chrome's reveal in FULLSCREEN, entered/left through the real top-bar
#       button: both strips hide/show together with the cursor, the video viewport never moves,
#       and leaving restores the windowed rect.
#   1g. The settings panel and the ImGui popup it opens over the LIVE page: both are routed into
#      their own owned top-level windows (native/src/popup_layer.cpp), so the page keeps running
#      underneath. Real clicks: the title-bar gear lifts the panel above the page, a click on a
#      panel number box hands the Win32 keyboard focus back from the page view (Ctrl+A + typing +
#      Enter must record the "lead" intent), the target-fps combo list opens as its own popup
#      window above the page (pixel check) and its rows select (the "target_fps" intent), the
#      address field's right-click menu is routed the same way, and hovering a panel row shows
#      its tooltip in the tooltip layer.
#   2. Window edge resizing. The child window covers the page area, so the parent never receives
#      WM_NCHITTEST over it; the resize bands must still work. A real mouse drag on the right,
#      bottom and LEFT edges must resize the window AND move/resize the page view with it -- and
#      the negative case (press inside the page, drag to the edge) must NOT resize anything.
#   1b. The address bar's back / forward / reload buttons are clicked for real (not through the
#      scripted actions): history navigation and a real re-fetch of the page.
#
# The drag runs on a helper thread: a window-sizing drag starts a modal loop on the UI thread
# (inside our own message pump), so the mouse release has to come from another thread.
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe scripts/verify_browser_input.py

import ctypes
import http.server
import os
import sys
import re
import threading
import time
import urllib.parse
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))
sys.path.insert(0, os.path.join(ROOT, "python", "sumu"))

import sumu_core  # noqa: E402
from sumu import i18n  # noqa: E402

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

_failures = []
_page_hits = {}  # path -> page fetches (reload check)

# PAGE_HTML's body background: the "known solid color" the popup-layer checks compare against.
PAGE_BG = (0x10, 0x10, 0x12)


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}{(' -- ' + detail) if detail else ''}")
    if not cond:
        _failures.append(name)


# Real H.264/AAC FLV generated from test_video.mp4 by ffmpeg (native/trace is gitignored). The
# manual play entry must open it straight from the URL typed into the address bar.
FLV_VIDEO = os.path.join(ROOT, "native", "trace", "sumu_e2e.flv")


def ensure_flv() -> bool:
    """Generate the FLV fixture when ffmpeg is available; returns whether it exists."""
    if os.path.isfile(FLV_VIDEO) and os.path.getsize(FLV_VIDEO) > 0:
        return True
    try:
        os.makedirs(os.path.dirname(FLV_VIDEO), exist_ok=True)
        import subprocess
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i",
             os.path.join(ROOT, "test_video.mp4"), "-t", "4",
             "-c:v", "libx264", "-profile:v", "main", "-pix_fmt", "yuv420p",
             "-c:a", "aac", "-b:a", "128k", FLV_VIDEO],
            check=True, timeout=180, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception as e:  # noqa: BLE001 -- no ffmpeg on PATH is a documented limitation
        print(f"  ffmpeg unavailable for the FLV fixture: {e!r}")
    return os.path.isfile(FLV_VIDEO) and os.path.getsize(FLV_VIDEO) > 0


PAGE_HTML = """<!doctype html>
<html><head><meta charset="utf-8"><title>sumu input __TAG__</title></head>
<body style="margin:0;background:#101012;color:#ddd;font:16px sans-serif">
<h3 style="padding:12px">sumu input __TAG__</h3>
</body></html>
"""
PAGES = {"/page": PAGE_HTML.replace("__TAG__", "one"),
         "/page2": PAGE_HTML.replace("__TAG__", "two")}


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def handle_one_request(self):
        try:
            super().handle_one_request()
        except OSError:
            self.close_connection = True

    def do_GET(self):  # noqa: N802
        path = urllib.parse.urlparse(self.path).path
        if path == "/noext":
            # The real MP4 under an extension-less name: the play button opens whatever the
            # address bar holds, so no filename is ever consulted.
            self._serve_media(os.path.join(ROOT, "test_video.mp4"), "video/mp4")
            return
        if path == "/movie.flv":
            # A real FLV under an extension a web page cannot play, with a query token.
            self._serve_media(FLV_VIDEO, "video/x-flv")
            return
        if path in PAGES:
            _page_hits[path] = _page_hits.get(path, 0) + 1
        body = PAGES.get(path)
        if body is None:
            body = "<!doctype html><title>404</title>not found"
        data = body.encode("utf-8")
        self.send_response(200 if path in PAGES else 404)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_media(self, filename, ctype):
        if not os.path.isfile(filename):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
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
        self.send_header("Content-Type", ctype)
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


# ---- real input helpers ---------------------------------------------------------------------

def pump(player, seconds, until=None):
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        player.pump_messages()
        player.ui_tick()
        if until is not None and until():
            return True
        time.sleep(0.01)
    return until() if until is not None else False


def find_window():
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


def client_rect(hwnd):
    pt = wintypes.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    rc = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rc))
    return pt.x, pt.y, rc.right - rc.left, rc.bottom - rc.top


def click_client(hwnd, x, y):
    ox, oy, _, _ = client_rect(hwnd)
    user32.SetCursorPos(int(ox + x), int(oy + y))
    time.sleep(0.08)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.05)

def right_click_client(hwnd, x, y):
    """Real right click at a client-relative point (the address field's context menu)."""
    ox, oy, _, _ = client_rect(hwnd)
    user32.SetCursorPos(int(ox + x), int(oy + y))
    time.sleep(0.08)
    user32.mouse_event(0x0008, 0, 0, 0, 0)  # RIGHTDOWN
    time.sleep(0.05)
    user32.mouse_event(0x0010, 0, 0, 0, 0)  # RIGHTUP
    time.sleep(0.05)


def ensure_foreground(hwnd, player):
    """Make the Player window the FOREGROUND window; returns whether it really is. Windows'
    foreground lock refuses SetForegroundWindow while another app owns the foreground, and a
    WebView2 page view only takes the keyboard focus inside an ACTIVE window -- so every real
    input phase needs this first. The activation click goes to the title bar (chrome, never the
    page) so it cannot pre-focus the page view the focus checks are about."""
    for _ in range(6):
        fg = user32.GetForegroundWindow()
        if fg == hwnd:
            return True
        tid_fg = user32.GetWindowThreadProcessId(fg, None)
        tid_self = ctypes.windll.kernel32.GetCurrentThreadId()
        attached = bool(user32.AttachThreadInput(tid_self, tid_fg, True))
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)
        user32.SetFocus(hwnd)
        if attached:
            user32.AttachThreadInput(tid_self, tid_fg, False)
        if user32.GetForegroundWindow() == hwnd:
            return True
        _, _, cw, _ = client_rect(hwnd)
        click_client(hwnd, cw * 0.5, user32.GetDpiForWindow(hwnd) / 96.0 * 18.0)  # title bar
        pump(player, 0.5)
    return user32.GetForegroundWindow() == hwnd


class _MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_void_p)]


class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_void_p)]


class _HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wintypes.DWORD), ("wParamL", wintypes.WORD), ("wParamH", wintypes.WORD)]


class _INPUT(ctypes.Structure):
    """Full Win32 INPUT (the union must be complete: SendInput rejects a smaller cbSize)."""

    class _Arg(ctypes.Union):
        _fields_ = [("mi", _MOUSEINPUT), ("ki", _KEYBDINPUT), ("hi", _HARDWAREINPUT)]

    _anonymous_ = ("arg",)
    _fields_ = [("type", wintypes.DWORD), ("arg", _Arg)]


def send_unicode(ch):
    """Type one character as WM_CHAR-level input (KEYEVENTF_UNICODE) -- works for any URL."""
    inp = _INPUT()
    inp.type = 1  # INPUT_KEYBOARD
    inp.ki.wVk = 0
    inp.ki.wScan = ord(ch)
    inp.ki.dwFlags = 0x0004  # KEYEVENTF_UNICODE
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(_INPUT))


def key(vk, up=False):
    user32.keybd_event(vk, 0, 0x0002 if up else 0, 0)


def type_text(player, text):
    for ch in text:
        send_unicode(ch)
        pump(player, 0.02)


def addr_button_center(hwnd, index):
    """Center of an address-bar button counted from the LEFT (0 back / 1 forward / 2 reload) in
    client coords -- same metrics as build_browser_page (2px inset, 32px buttons, 2px gaps, 36px
    bar below the 36px title bar). The close button that used to sit at index 0 is gone."""
    s = user32.GetDpiForWindow(hwnd) / 96.0
    return s * (2.0 + index * 34.0 + 16.0), s * (36.0 + 18.0)


def addr_right_button_center(hwnd, index_from_right):
    """Center of a right-side address-bar button, counted from the RIGHT edge of the bar
    (0 play / 1 star): the field takes the remaining width, so these are anchored to the
    bar's right edge (2px inset + 32px button + 32px per step)."""
    s = user32.GetDpiForWindow(hwnd) / 96.0
    _, _, cw, _ = client_rect(hwnd)
    return cw - s * (2.0 + 16.0 + index_from_right * 34.0), s * (36.0 + 18.0)


PLAY_FROM_RIGHT, STAR_FROM_RIGHT = 0, 1


def screen_pixel(x, y):
    """Color of one desktop pixel as (r, g, b). Reads the COMPOSITED desktop, so it shows what
    the user actually sees -- the whole point of the popup-layer checks (an ImGui window drawn
    in the main swapchain would be hidden behind the WebView2 child window here)."""
    hdc = user32.GetDC(0)
    try:
        color = gdi32.GetPixel(hdc, int(x), int(y)) & 0xFFFFFF  # COLORREF 0x00BBGGRR
    finally:
        user32.ReleaseDC(0, hdc)
    return color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF


def pump_intents(player, seconds, until=None):
    """Pump the player and collect UI intents (drained exactly like app.py's main loop)."""
    intents = {}
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        player.pump_messages()
        player.ui_tick()
        for k, v in player.take_ui_intents().items():
            if v:
                intents[k] = v
        if until is not None and until(intents):
            break
        time.sleep(0.02)
    return intents


def click_right_button(player, hwnd, index_from_right, keys, tries=3):
    """Real click on one of the address bar's right-side buttons until one of `keys` shows up in
    the drained intents (the first click after a state change can be eaten)."""
    intents = {}
    for _ in range(tries):
        click_client(hwnd, *addr_right_button_center(hwnd, index_from_right))
        intents = pump_intents(player, 2.0, lambda i: any(i.get(k) for k in keys))
        if any(intents.get(k) for k in keys):
            return intents
    return intents


def click_until(player, hwnd, pos, predicate, tries=3, settle=4.0):
    """Real click at `pos` (a callable -> (x, y)) until `predicate()` holds. The first click after
    the window settles can be eaten (same retry the settings toggle uses); every attempt is still
    a real mouse click on the real button."""
    for _ in range(tries):
        click_client(hwnd, *pos())
        if pump(player, settle, predicate):
            return True
    return predicate()


def submit_url(player, url, settle=3.0):
    """Navigate and collect intents emitted while the page settles."""
    player.browser_navigate(url)
    t0 = time.monotonic()
    intents = {}
    while time.monotonic() - t0 < settle:
        player.pump_messages()
        player.ui_tick()
        got = player.take_ui_intents()
        for k, v in got.items():
            if v:
                intents[k] = v
        time.sleep(0.02)
    return intents


def complete_native_open(player, flow, intents, settle=2.0):
    """The Python half of a manual native open: run open/reopen with the handover's replay
    context and report the result, exactly like app.py's open state machine."""
    ok = False
    try:
        args = dict(headers=intents.get("open_headers") or "",
                    cookies=intents.get("open_cookies") or "")
        if flow["opened"]:
            player.reopen(intents["open_path"], **args)
        else:
            player.open(intents["open_path"], **args)
        ok = True
    except Exception as e:  # noqa: BLE001 -- a failed manual open is the app's error path
        print(f"  native open failed: {e!r}"[:160])
    flow["opened"] = ok
    player.notify_open_url_finished(ok)
    pump(player, settle)
    return ok


def play_button_click(player, hwnd, flow, settle=2.0, tries=3):
    """Real click on the address bar's play button, then complete the handover like the app.
    Returns (intents, opened)."""
    intents = {}
    for _ in range(tries):
        click_client(hwnd, *addr_right_button_center(hwnd, PLAY_FROM_RIGHT))
        t0 = time.monotonic()
        while time.monotonic() - t0 < 3.0:
            player.pump_messages()
            player.ui_tick()
            got = player.take_ui_intents()
            if got.get("open_path"):
                intents = got
                break
            time.sleep(0.02)
        if intents:
            break
    if not intents:
        return {}, False
    return intents, complete_native_open(player, flow, intents, settle)


def move_client(hwnd, x, y):
    ox, oy, _, _ = client_rect(hwnd)
    user32.SetCursorPos(int(ox + x), int(oy + y))
    time.sleep(0.05)


def check_reveal_boundary(player, hwnd, label):
    _, _, width, height = client_rect(hwnd)
    viewport = player.browser_state()["viewport"]
    # The band starts at the title bar's bottom edge (fixed DPI height 36*scale, independent of
    # whether the bar is currently shown) and spans 15% of the client height below it.
    title_h = 36.0 * user32.GetDpiForWindow(hwnd) / 96.0
    for fraction, visible in ((0.14, True), (0.16, False)):
        move_client(hwnd, width * 0.5, title_h + height * fraction)
        ok = pump(player, 2.0,
                  lambda: bool(player.browser_state()["addr_visible"]) == visible)
        state = player.browser_state()
        check(f"{label}: address reveal at title+{fraction:.0%} is {visible}", ok, f"{state}")
        check(f"{label}: reveal boundary keeps the viewport", state["viewport"] == viewport)
        if state["fullscreen"]:
            check(f"{label}: title and address bar share the reveal boundary",
                  state["top_bar_visible"] == visible, f"{state}")

    # 16% of the WINDOW height is still inside the band, because the band starts below the title
    # bar: a boundary measured from y=0 would already have ended here.
    move_client(hwnd, width * 0.5, height * 0.16)
    check(f"{label}: reveal still on at 16% of the window height",
          pump(player, 2.0, lambda: bool(player.browser_state()["addr_visible"])),
          f"{player.browser_state()}")


def top_bar_right_button_center(hwnd, index_from_right):
    """Right-side top-bar buttons, counted from the window edge: 0 close, 1 fullscreen,
    2 maximize/restore, 3 minimize (32px buttons, 2px gaps, 2px edge inset -- build_top_bar)."""
    s = user32.GetDpiForWindow(hwnd) / 96.0
    _, _, cw, _ = client_rect(hwnd)
    return cw - s * (2.0 + 16.0 + index_from_right * 34.0), s * 18.0


def top_bar_icon_center(hwnd, index):
    """Left-side top-bar icon buttons, counted from the left edge: 0 settings / 1 open /
    2 URL / 3 web stream / 4 export (same 32px button beat as the right side)."""
    s = user32.GetDpiForWindow(hwnd) / 96.0
    return s * (2.0 + 16.0 + index * 34.0), s * 18.0


def window_rect(hwnd):
    rc = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    return rc.left, rc.top, rc.right, rc.bottom


def drag(player, hwnd, x0, y0, dx, dy, steps=12):
    """Real edge drag: press on the border band, move in steps while the UI keeps ticking, then
    release. Everything runs on this thread -- the Player drives the drag per tick (no modal
    loop), so the window follows the cursor while the harness is inside pump()/sleep()."""
    ox, oy, _, _ = client_rect(hwnd)
    user32.SetCursorPos(int(ox + x0), int(oy + y0))
    pump(player, 0.15)
    user32.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN on the border band
    pump(player, 0.15)
    for i in range(1, steps + 1):
        user32.SetCursorPos(int(ox + x0 + dx * i / steps), int(oy + y0 + dy * i / steps))
        pump(player, 0.06)
    pump(player, 0.1)
    user32.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
    pump(player, 0.3)


def main():
    os.environ.pop("SUMU_WEBVIEW2_OFF", None)
    server = _Server(("127.0.0.1", 0), _Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    page_url = f"{base}/page"
    page2_url = f"{base}/page2"

    player = sumu_core.Player(1280, 760, False)
    i18n.apply_to_player(player)
    pump(player, 0.5)
    player.request_open_url_popup()
    pump(player, 1.5)
    flow0 = {"opened": False}
    nav_intents = submit_url(player, page_url)
    ok = pump(player, 20.0, lambda: player.browser_state()["web_visible"])
    check("a submitted address starts no native open",
          not nav_intents.get("open_path"), repr(nav_intents.get("open_path")))
    check("setup: page view up", ok, f"{player.browser_state()}")
    if not ok:
        player.close()
        server.shutdown()
        server.server_close()
        return 1

    hwnd = find_window()
    check("own window found", bool(hwnd))
    if not hwnd:
        player.close()
        server.shutdown()
        server.server_close()
        return 1
    user32.SetWindowPos(hwnd, -1, 60, 40, 0, 0, 0x1)  # topmost, keep size
    check("window is foreground for the real-input phases", ensure_foreground(hwnd, player))
    pump(player, 0.4)

    # ---- 1. keyboard focus hand-back ----------------------------------------------------
    _, _, cw, ch = client_rect(hwnd)
    scale = user32.GetDpiForWindow(hwnd) / 96.0
    addr_y = scale * (36.0 + 18.0)  # middle of the address bar
    page_y = scale * (36.0 + 36.0) + 0.5 * (ch - scale * 72.0)

    click_client(hwnd, cw * 0.5, page_y)  # click INTO the page view: it takes the Win32 focus
    # Chromium grabs the focus asynchronously after its own click handling; the very first click
    # that activates the window can be swallowed, so keep clicking until the focus really moved.
    for _ in range(4):
        if user32.GetFocus() != hwnd:
            break
        click_client(hwnd, cw * 0.5, page_y)
        pump(player, 2.0, lambda: user32.GetFocus() != hwnd)
    check("page view owns the Win32 focus after clicking it", user32.GetFocus() != hwnd,
          f"{user32.GetFocus()}")

    # programmatic focus path (re-entering the page while it is open puts the caret in the bar)
    player.request_open_url_popup()
    pump(player, 0.4)
    check("programmatic address focus takes the keyboard back", user32.GetFocus() == hwnd,
          f"{user32.GetFocus()}")

    # ...and the real one: click the address bar after the page view had the focus
    click_client(hwnd, cw * 0.5, page_y)
    pump(player, 0.3)
    click_client(hwnd, cw * 0.45, addr_y)
    pump(player, 0.3)
    check("clicking the address bar takes the keyboard back", user32.GetFocus() == hwnd,
          f"{user32.GetFocus()}")
    key(0x11)  # Ctrl down
    key(0x41)  # A
    key(0x41, up=True)
    key(0x11, up=True)
    pump(player, 0.2)
    type_text(player, page2_url)
    pump(player, 0.2)
    state = player.browser_state()
    check("typed text reaches the address bar", state["address"] == page2_url, state["address"])
    key(0x0D)  # Enter
    key(0x0D, up=True)
    # A typed address is a NAVIGATION: the page view is asked for it and nothing native runs.
    flow = {"opened": False}
    intents = {}
    t0 = time.monotonic()
    saw_open = False
    while time.monotonic() - t0 < 3.0:
        player.pump_messages()
        player.ui_tick()
        got = player.take_ui_intents()
        for k, v in got.items():
            if v:
                intents[k] = v
        saw_open = saw_open or bool(got.get("open_path"))
        time.sleep(0.02)
    ok = pump(player, 20.0, lambda: (player.browser_state()["url"] or "").rstrip("/") == page2_url)
    state = player.browser_state()
    check("Enter starts no native open for a typed address", not saw_open,
          repr(intents.get("open_path")))
    check("Enter really navigates the page", ok, state["url"])
    check("address bar keeps the typed URL", state["address"] == page2_url, state["address"])

    # ---- 1b. history through the REAL address-bar buttons -------------------------------
    # The typing phase above left us on /page2 with /page in history.
    # The leftmost slot is BACK now: the close X is gone, so a click there must navigate, never
    # leave the page.
    ok = click_until(player, hwnd, lambda: addr_button_center(hwnd, 0),  # back
                     lambda: (player.browser_state()["url"] or "").rstrip("/") == page_url,
                     tries=3, settle=8.0)
    check("real back button (leftmost slot) navigates to the previous page", ok,
          player.browser_state()["url"])
    check("the leftmost address-bar slot is not a close button",
          player.browser_state()["page"], f"{player.browser_state()}")
    ok = click_until(player, hwnd, lambda: addr_button_center(hwnd, 1),  # forward
                     lambda: (player.browser_state()["url"] or "").rstrip("/") == page2_url,
                     tries=3, settle=8.0)
    check("real forward button returns to the later page", ok,
          player.browser_state()["url"])
    hits = _page_hits.get("/page2", 0)
    ok = click_until(player, hwnd, lambda: addr_button_center(hwnd, 2),  # reload
                     lambda: _page_hits.get("/page2", 0) > hits, tries=3, settle=8.0)
    check("real reload button re-fetches the page", ok,
          f"hits {hits} -> {_page_hits.get('/page2', 0)}")
    check("address bar still owns the keyboard after the buttons", user32.GetFocus() == hwnd,
          f"{user32.GetFocus()}")

    # ---- 1c. Native content: the address bar is floating chrome ------------------------
    intents = submit_url(player, f"{base}/noext")
    check("video navigation automatically hands its URL to the native open",
          intents.get("open_path") == f"{base}/noext", repr(intents.get("open_path")))
    opened = bool(intents.get("open_path")) and complete_native_open(player, flow, intents)
    state = player.browser_state()
    check("automatic navigation really opens the media URL", opened, f"{state}")
    check("the automatic open becomes the page's native content", state["native"], f"{state}")
    _, _, cwN, chN = client_rect(hwnd)
    move_client(hwnd, cwN * 0.5, chN * 0.6)
    pump(player, 0.8)
    away = player.browser_state()
    move_client(hwnd, cwN * 0.5, 6)
    pump(player, 0.9)
    top = player.browser_state()
    move_client(hwnd, cwN * 0.5, chN * 0.6)
    pump(player, 0.9)
    back = player.browser_state()
    check("native: address bar hides away from the top strip", not away["addr_visible"],
          f"{away}")
    check("native: address bar appears in the top strip", top["addr_visible"], f"{top}")
    check("native: showing the bar does not move the video viewport",
          away["viewport"] == top["viewport"] == back["viewport"],
          f"{away['viewport']} {top['viewport']} {back['viewport']}")
    check_reveal_boundary(player, hwnd, "windowed")
    # ---- 1d. the same reveal rule in FULLSCREEN (real click on the top bar's button) -----
    # The title bar and the floating address bar share one reveal zone, so entering fullscreen
    # through the user path must hide/show both together and never move the video viewport.
    windowed_rect = window_rect(hwnd)
    check("fullscreen: windowed before the toggle", not player.browser_state()["fullscreen"],
          f"{player.browser_state()}")
    click_client(hwnd, *top_bar_right_button_center(hwnd, 1))  # fullscreen
    ok = pump(player, 3.0, lambda: player.browser_state()["fullscreen"])
    check("fullscreen entered through the real top bar button", ok,
          f"{player.browser_state()}")
    if ok:
        _, _, fw, fh = client_rect(hwnd)
        move_client(hwnd, fw * 0.5, fh * 0.55)
        pump(player, 0.9)
        away = player.browser_state()
        move_client(hwnd, fw * 0.5, 8)
        pump(player, 0.9)
        top = player.browser_state()
        move_client(hwnd, fw * 0.5, fh * 0.55)
        pump(player, 0.9)
        back = player.browser_state()
        check("fullscreen: title bar hidden away from the top",
              not away["top_bar_visible"], f"{away}")
        check("fullscreen: address bar hidden away from the top",
              not away["addr_visible"], f"{away}")
        check("fullscreen: both bars revealed in the top strip",
              top["top_bar_visible"] and top["addr_visible"], f"{top}")
        check("fullscreen: both bars hide again when the cursor leaves",
              not back["top_bar_visible"] and not back["addr_visible"], f"{back}")
        check("fullscreen: revealing the bars does not move the video viewport",
              away["viewport"] == top["viewport"] == back["viewport"],
              f"{away['viewport']} {top['viewport']} {back['viewport']}")
        check_reveal_boundary(player, hwnd, "fullscreen")
        # Leave fullscreen through the same button: reveal the bar first (it auto-hides).
        move_client(hwnd, fw * 0.5, 8)
        pump(player, 0.8)
        click_client(hwnd, *top_bar_right_button_center(hwnd, 1))
        ok = pump(player, 3.0, lambda: not player.browser_state()["fullscreen"])
        check("fullscreen left through the same button", ok, f"{player.browser_state()}")
        pump(player, 0.5)
        restored = window_rect(hwnd)
        check("fullscreen exit restores the windowed rect",
              all(abs(a - b) <= 4 for a, b in zip(windowed_rect, restored)),
              f"{windowed_rect} -> {restored}")
        check("windowed playback still shows the native content",
              player.browser_state()["native"], f"{player.browser_state()}")

    # ---- 1e. the manual native entry with a URL that is not a web page at all ----------
    # A .flv with a query token, TYPED into the bar (no Enter: the address itself is the input)
    # and then handed to the decoder by a real click on the play button. Nothing here consults a
    # web-page MIME type or a filename list.
    flv_url = f"{base}/movie.flv?token=%E4%B8%AD%E6%96%87&t=typed"
    if not ensure_flv():
        check("FLV fixture available for the typed manual entry", False, FLV_VIDEO)
    else:
        _, _, cwN, chN = client_rect(hwnd)
        move_client(hwnd, cwN * 0.5, 8)      # reveal the floating bar over the native video
        pump(player, 0.8)
        click_client(hwnd, cwN * 0.45, addr_y)  # into the URL field
        pump(player, 0.4)
        key(0x11); key(0x41); key(0x41, up=True); key(0x11, up=True)  # Ctrl+A
        pump(player, 0.2)
        type_text(player, flv_url)
        pump(player, 0.3)
        typed = player.browser_state()
        check("the .flv URL reaches the address bar", typed["address"] == flv_url,
              typed["address"])
        intents, opened = play_button_click(player, hwnd, flow)
        state = player.browser_state()
        check("the play button hands the typed URL to the native open unchanged",
              intents.get("open_path") == flv_url, repr(intents.get("open_path")))
        check("the typed .flv URL opens natively (no container/extension gate)", opened,
              f"opened={opened} path={intents.get('open_path')!r}")
        check("the typed-URL handover replays the page as Referer",
              "Referer: " in (intents.get("open_headers") or ""),
              repr(intents.get("open_headers")))
        check("the typed .flv session becomes the page's native content", state["native"],
              f"{state}")
        if opened:
            player.play()
            ok = pump(player, 3.0, lambda: player.current_frame() > 0)
            player.pause()
            check("the typed .flv really decodes and plays", ok,
                  f"frames={player.frame_count()} frame={player.current_frame()}")
        # Leave the field (click the video area) so the next phase starts clean.
        click_client(hwnd, cwN * 0.5, chN * 0.5)
        pump(player, 0.5)

    # Now a web page again (page view visible) so the drag tests below have the child window up.
    submit_url(player, page_url)
    ok = pump(player, 20.0, lambda: player.browser_state()["web_visible"])
    check("web page restored for the drag tests", ok, f"{player.browser_state()}")

    # ---- 1f. favorites dropdown + the popup layer --------------------------------------
    # The page view is a real child HWND: everything ImGui draws in the main swapchain is covered
    # by it. The address bar's favorites dropdown and its button tooltips are therefore lifted
    # into their own owned top-level windows (native/src/popup_layer.cpp) while the live page view
    # is up. Everything here is a REAL mouse hover/click, and the "is it really above the page?"
    # evidence is a screen pixel that is not the page's own background color.
    page_title = "sumu input one"
    other_title = "sumu input two"
    state = player.browser_state()
    check("favorites: the committed page URL is the one on screen",
          (state["url"] or "").rstrip("/") == page_url, state["url"])
    check("favorites: nothing is favorited to start with", not state["favorited"],
          f"{state['favorites']}")

    # The dropdown will open over the page area: prove the page view really covers that spot
    # first (same pixel, page background), otherwise the popup check proves nothing.
    _, _, cw, ch = client_rect(hwnd)
    ox, oy = client_rect(hwnd)[0], client_rect(hwnd)[1]
    probe_y = (36.0 + 36.0 + 20.0) * scale
    coverage_before = screen_pixel(ox + cw * 0.5, oy + probe_y)
    check("favorites: the page view covers the area the dropdown will occupy",
          coverage_before == PAGE_BG, f"{coverage_before}")

    intents = click_right_button(player, hwnd, STAR_FROM_RIGHT,
                                 ("favorite_add_url", "favorite_remove_url"))
    check("star click records favorite_add_url for the committed page URL",
          intents.get("favorite_add_url") == page_url, repr(intents.get("favorite_add_url")))
    check("star click carries the page's document title",
          intents.get("favorite_add_title") == page_title,
          repr(intents.get("favorite_add_title")))
    check("the star fills in the click's own frame (optimistic local update)",
          player.browser_state()["favorited"],
          f"{player.browser_state()['favorites']}")
    # The Python side owns the list (settings store) and echoes it back, exactly like app.py.
    player.set_browser_favorites([
        {"url": page_url, "title": page_title},
        {"url": page2_url, "title": other_title},
    ])
    pump(player, 0.3)
    state = player.browser_state()
    check("set_browser_favorites publishes the list in display order",
          list(state["favorites"]) == [page_url, page2_url], f"{state['favorites']}")

    # Click into the URL field: the dropdown opens under it.
    click_client(hwnd, cw * 0.45, addr_y)
    ok = pump(player, 3.0, lambda: player.browser_state()["popups"]["favorites"]["visible"])
    state = player.browser_state()
    pop = state["popups"]["favorites"]
    check("clicking the address field opens the favorites dropdown",
          ok and state["favorites_open"], f"open={state['favorites_open']} {pop}")
    check("the dropdown is a real top-level window with a live swapchain",
          pop["hwnd"] and pop["visible"] and pop["frames"] > 0, f"{pop}")

    field = state["addr_field_rect"]
    pop_client = pop["client_rect"]
    check("the dropdown is as wide as the field and starts at its left edge",
          abs(pop_client[0] - field[0]) <= 1 and abs(pop_client[2] - field[2]) <= 1,
          f"field={field} popup={pop_client}")
    check("the dropdown hangs below the address field",
          pop_client[1] >= field[1] + field[3], f"field={field} popup={pop_client}")
    bounds = state["bounds"]
    web_rect = (ox + bounds[0], oy + bounds[1], bounds[2], bounds[3])
    pop_screen = pop["screen_rect"]
    check("the dropdown overlaps the web view rect",
          pop_screen[0] < web_rect[0] + web_rect[2] and
          pop_screen[0] + pop_screen[2] > web_rect[0] and
          pop_screen[1] < web_rect[1] + web_rect[3] and
          pop_screen[1] + pop_screen[3] > web_rect[1],
          f"popup={pop_screen} web={web_rect}")
    # A row's own pixels are NOT the page background: the dropdown really is composited above
    # the page view rather than hidden behind it.
    row_h = 32.0 * scale
    row0_y = pop_screen[1] + 4.0 * scale + row_h * 0.5
    row_pixels = [screen_pixel(pop_screen[0] + pop_screen[2] * f, row0_y)
                  for f in (0.03, 0.4, 0.85)]
    check("a screen pixel inside a dropdown row is not the page background",
          all(px != PAGE_BG for px in row_pixels), f"{row_pixels} page_bg={PAGE_BG}")

    # Hovering a row only recolors it (same geometry): the popup must still re-render, otherwise
    # the hover wash never reaches the screen.
    hover_cx = pop_client[0] + pop_client[2] * 0.6
    hover_sx = pop_screen[0] + pop_screen[2] * 0.6
    move_client(hwnd, hover_cx, pop_client[1] + 4.0 * scale + row_h * 0.5)
    pump(player, 0.6)
    hovered_px = screen_pixel(hover_sx, row0_y)
    move_client(hwnd, hover_cx, pop_client[1] + 4.0 * scale + row_h * 1.5)
    pump(player, 0.6)
    unhovered_px = screen_pixel(hover_sx, row0_y)
    check("hovering a dropdown row shows its hover wash on screen",
          hovered_px != unhovered_px, f"hovered={hovered_px} after_leave={unhovered_px}")

    # Real click on the SECOND row -> that favorite's URL is put in the field and navigated.
    click_client(hwnd, pop_client[0] + pop_client[2] * 0.3,
                 pop_client[1] + 4.0 * scale + row_h * 1.5)
    ok = pump(player, 20.0, lambda: (player.browser_state()["url"] or "").rstrip("/") == page2_url)
    state = player.browser_state()
    check("a real click on a dropdown row navigates to that favorite",
          ok, f"{state['url']}")
    check("the row's URL is what the address bar now shows", state["address"] == page2_url,
          state["address"])
    check("the dropdown closes after a row click", not state["favorites_open"],
          f"{state['favorites_open']}")

    # Reopen and press a row's X: that entry is removed and the list stays open.
    _, _, cw, ch = client_rect(hwnd)
    click_client(hwnd, cw * 0.45, addr_y)
    pump(player, 3.0, lambda: player.browser_state()["popups"]["favorites"]["visible"])
    state = player.browser_state()
    pop_client = state["popups"]["favorites"]["client_rect"]
    check("the dropdown reopens on a second field click", state["favorites_open"], f"{state}")
    click_client(hwnd, pop_client[0] + pop_client[2] - row_h * 0.5,
                 pop_client[1] + 4.0 * scale + row_h * 0.5)
    intents = pump_intents(player, 2.0, lambda i: i.get("favorite_remove_url"))
    state = player.browser_state()
    check("a real click on a row's X records favorite_remove_url",
          intents.get("favorite_remove_url") == page_url,
          repr(intents.get("favorite_remove_url")))
    check("the row's X removes that favorite locally", page_url not in list(state["favorites"]),
          f"{state['favorites']}")
    check("the dropdown stays open after a removal", state["favorites_open"], f"{state}")
    player.set_browser_favorites([{"url": page2_url, "title": other_title}])  # app.py's echo
    pump(player, 0.3)

    # Esc closes the dropdown instead of quitting/parking the player.
    check("the address field still owns the keyboard", user32.GetFocus() == hwnd,
          f"{user32.GetFocus()}")
    key(0x1B)  # ESC
    key(0x1B, up=True)
    ok = pump(player, 2.0, lambda: not player.browser_state()["favorites_open"])
    check("Esc closes the dropdown", ok, f"{player.browser_state()['favorites_open']}")
    check("Esc with the dropdown open does not quit the player", not player.should_quit(),
          f"{player.should_quit()}")

    # A real hover on the play button: its tooltip must also rise above the page view.
    move_client(hwnd, *addr_right_button_center(hwnd, PLAY_FROM_RIGHT))
    ok = pump(player, 4.0, lambda: player.browser_state()["popups"]["tooltip"]["visible"])
    state = player.browser_state()
    tip = state["popups"]["tooltip"]
    check("hovering the play button shows its tooltip above the page view",
          ok and tip["hwnd"], f"{tip}")
    if ok:
        tip_rect = tip["screen_rect"]
        bounds = state["bounds"]
        web_rect = (ox + bounds[0], oy + bounds[1], bounds[2], bounds[3])
        check("the tooltip overlaps the web view rect",
              tip_rect[0] < web_rect[0] + web_rect[2] and
              tip_rect[0] + tip_rect[2] > web_rect[0] and
              tip_rect[1] < web_rect[1] + web_rect[3] and
              tip_rect[1] + tip_rect[3] > web_rect[1],
              f"tooltip={tip_rect} web={web_rect}")
        tip_pixels = [screen_pixel(tip_rect[0] + tip_rect[2] * f, tip_rect[1] + tip_rect[3] * 0.5)
                      for f in (0.06, 0.5, 0.94)]
        check("a screen pixel inside the tooltip is not the page background",
              all(px != PAGE_BG for px in tip_pixels), f"{tip_pixels} page_bg={PAGE_BG}")


    # ---- 1g. settings panel + the ImGui popup it opens, over the LIVE page ----------------
    # Both are separate ImGui root windows, so each has its own popup layer
    # (native/src/popup_layer.cpp): the panel floats above the page view (which keeps running,
    # clicking and sounding) and the combo list / context menu that the panel or the address bar
    # opens gets its own window on top of everything. Typing into a routed number box needs the
    # Win32 keyboard focus back from the page view -- the layer hands it over on the click itself.
    state = player.browser_state()
    check("settings: the page view is up before the panel test", state["web_visible"], f"{state}")

    def settings_row_y(panel, row):
        """Client y of settings row `row`'s control row. The panel insets its content by
        kPaddingContainer (10px top), every row block is a kFontSizeSm label + kSpaceM gap over a
        kControlHeight row + kSpaceM gap (see build_settings_panel / ui::SliderInt)."""
        return panel[1] + (34.0 + 68.0 * row) * scale

    # Click INTO the page first: WebView2's child window takes the Win32 focus.
    click_client(hwnd, cw * 0.6, ch * 0.62)
    pump(player, 0.6)
    if user32.GetFocus() == hwnd:  # the first click can be eaten by the activation itself
        click_client(hwnd, cw * 0.6, ch * 0.62)
        pump(player, 0.6)
    check("settings: the page view owns the keyboard before the panel is used",
          user32.GetFocus() != hwnd, f"{user32.GetFocus()}")

    ok = click_until(player, hwnd, lambda: top_bar_icon_center(hwnd, 0),
                     lambda: player.browser_state()["popups"]["settings"]["visible"],
                     tries=4, settle=3.0)
    state = player.browser_state()
    panel = state["popups"]["settings"]
    check("settings: a real gear click lifts the panel into its own window above the page",
          ok and state["settings_open"] and panel["hwnd"] and panel["frames"] > 0, f"{panel}")
    check("settings: the live page view stays up behind the panel",
          state["web_visible"] and not state["muted"], f"{state}")
    bounds = state["bounds"]
    web_rect = (ox + bounds[0], oy + bounds[1], bounds[2], bounds[3])
    panel_rect = panel["screen_rect"]
    check("settings: the panel window overlaps the web view rect",
          panel_rect[0] < web_rect[0] + web_rect[2] and
          panel_rect[0] + panel_rect[2] > web_rect[0] and
          panel_rect[1] < web_rect[1] + web_rect[3] and
          panel_rect[1] + panel_rect[3] > web_rect[1],
          f"panel={panel_rect} web={web_rect}")
    panel_pixels = [screen_pixel(panel_rect[0] + panel_rect[2] * fx,
                                 panel_rect[1] + panel_rect[3] * fy)
                    for fx, fy in ((0.5, 0.15), (0.92, 0.5), (0.5, 0.92))]
    check("settings: screen pixels inside the panel are not the page background",
          all(px != PAGE_BG for px in panel_pixels), f"{panel_pixels} page_bg={PAGE_BG}")

    # A real click on the "lead" number box: the panel is a non-activating window, so the layer
    # must hand the keyboard focus back to the Player window for the typed digits to arrive.
    panel_c = panel["client_rect"]
    lead_box = (panel_c[0] + 48.0 * scale, settings_row_y(panel_c, 0) + 16.0 * scale)
    got_focus = False
    for _ in range(3):
        click_client(hwnd, *lead_box)
        if pump(player, 1.5, lambda: user32.GetFocus() == hwnd):
            got_focus = True
            break
    check("settings: clicking a panel number box takes the keyboard back from the page",
          got_focus, f"focus={user32.GetFocus()}")
    key(0x11)  # Ctrl down
    key(0x41)  # A -- select the whole value
    key(0x41, up=True)
    key(0x11, up=True)
    pump(player, 0.2)
    type_text(player, "42")
    pump(player, 0.3)
    key(0x0D)  # Enter commits the edit
    key(0x0D, up=True)
    intents = pump_intents(player, 2.0, lambda i: i.get("lead"))
    check("settings: a typed number records the lead intent", intents.get("lead") == 42,
          repr(intents.get("lead")))

    # The target-fps combo: its list is a BeginPopup window -- a separate ImGui root window, hence
    # a separate layer -- and must show above the page just like the panel.
    panel_c = player.browser_state()["popups"]["settings"]["client_rect"]
    combo = (panel_c[0] + panel_c[2] * 0.5, settings_row_y(panel_c, 4) + 16.0 * scale)
    ok = click_until(player, hwnd, lambda: combo,
                     lambda: player.browser_state()["popups"]["menu"]["visible"],
                     tries=3, settle=2.0)
    state = player.browser_state()
    menu = state["popups"]["menu"]
    check("settings: the open combo list is its own window above the page",
          ok and menu["hwnd"] and menu["frames"] > 0, f"{menu}")
    menu_rect = menu["screen_rect"]
    # The list hangs below the panel, so its lower part sits over bare page pixels: a pixel there
    # proves the menu layer really composites above the WebView2 child window.
    menu_pixel = screen_pixel(menu_rect[0] + menu_rect[2] * 0.5,
                              menu_rect[1] + menu_rect[3] - 10)
    check("settings: the combo list is composited above the page", menu_pixel != PAGE_BG,
          f"{menu_pixel} page_bg={PAGE_BG} menu={menu_rect}")
    menu_c = menu["client_rect"]
    row = (menu_c[0] + menu_c[2] * 0.4, menu_c[1] + 4.0 * scale + 32.0 * scale * 1.5)
    intents = {}
    for _ in range(3):
        click_client(hwnd, *row)
        intents = pump_intents(player, 2.0, lambda i: i.get("target_fps") is not None)
        if intents.get("target_fps") is not None:
            break
    check("settings: a real click on a combo row records the target_fps intent",
          intents.get("target_fps") == 30, repr(intents.get("target_fps")))
    check("settings: the combo list closes after the pick",
          not player.browser_state()["popups"]["menu"]["visible"],
          f"{player.browser_state()['popups']['menu']}")

    # The address field's right-click menu is a BeginPopupContextItem popup: another root window,
    # routed by the same menu layer.
    right_click_client(hwnd, cw * 0.5, addr_y)
    ok = pump(player, 3.0, lambda: player.browser_state()["popups"]["menu"]["visible"])
    state = player.browser_state()
    ctx = state["popups"]["menu"]
    check("the address field's right-click menu rises above the page view",
          ok and ctx["hwnd"] and ctx["frames"] > 0, f"{ctx}")
    if ok:
        ctx_rect = ctx["screen_rect"]
        ctx_pixel = screen_pixel(ctx_rect[0] + ctx_rect[2] * 0.3,
                                 ctx_rect[1] + ctx_rect[3] - 8)
        check("the address field's menu is composited above the page",
              ctx_pixel != PAGE_BG, f"{ctx_pixel} page_bg={PAGE_BG} menu={ctx_rect}")
    # Close it by clicking the address field (a click outside closes an ImGui popup; the page area
    # is covered by the WebView2 child window, which would swallow the click). NOT Esc
    # unconditionally: with no dropdown open, ESC is the player's own quit/park shortcut -- so it
    # is only pressed when the field click really opened the favorites dropdown.
    click_client(hwnd, cw * 0.45, addr_y)
    pump(player, 0.8)
    check("clicking the address field closes the context menu",
          not player.browser_state()["popups"]["menu"]["visible"],
          f"{player.browser_state()['popups']['menu']}")
    if player.browser_state()["favorites_open"]:
        key(0x1B)  # ESC closes the dropdown instead of quitting/parking the player
        key(0x1B, up=True)
        pump(player, 0.8)

    # Hovering a settings row shows its tooltip -- through the tooltip layer, over the panel that
    # is itself floating above the page.
    panel_c = player.browser_state()["popups"]["settings"]["client_rect"]
    move_client(hwnd, panel_c[0] + panel_c[2] * 0.72,
                settings_row_y(panel_c, 1) + 16.0 * scale)
    ok = pump(player, 5.0, lambda: player.browser_state()["popups"]["tooltip"]["visible"])
    check("hovering a routed settings row shows its tooltip",
          ok, f"{player.browser_state()['popups']['tooltip']}")

    # Toggle the panel closed with the same real button.
    ok = click_until(player, hwnd, lambda: top_bar_icon_center(hwnd, 0),
                     lambda: not player.browser_state()["settings_open"], tries=4, settle=2.0)
    state = player.browser_state()
    check("settings: the same gear click closes the panel", ok, f"{state}")
    check("settings: closing the panel hides its popup window",
          not state["popups"]["settings"]["visible"], f"{state}")
    check("settings: the page is left exactly as it was",
          state["page"] and state["web_visible"] and not state["muted"] and
          (state["url"] or "").rstrip("/") == page2_url, f"{state}")

    # ---- 2a. negative: a press INSIDE the page dragged to the edge must not resize -------
    # (arming requires the button-down to start in the resize band)
    _, _, cwN, chN = client_rect(hwnd)
    width_before = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(width_before))
    bounds_before = player.browser_state()["bounds"]
    drag(player, hwnd, cwN * 0.5, chN * 0.5, (cwN * 0.5) - 8, 0)  # page centre -> right edge
    after = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(after))
    width_after = after.right - after.left
    check("press inside the page + drag to the edge does not resize",
          abs(width_after - (width_before.right - width_before.left)) <= 2,
          f"{width_before.right - width_before.left} -> {width_after}")
    check("page view unchanged after the negative drag",
          tuple(player.browser_state()["bounds"]) == tuple(bounds_before),
          f"{bounds_before} -> {player.browser_state()['bounds']}")

    # ---- 2b. real edge drag: right edge then bottom edge ---------------------------------
    if user32.IsZoomed(hwnd):
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
        pump(player, 0.5)
    check("window is not maximized for the drag test", not user32.IsZoomed(hwnd))

    ox, oy, cw0, ch0 = client_rect(hwnd)
    bounds0 = player.browser_state()["bounds"]
    drag(player, hwnd, cw0 - 2, ch0 * 0.5, -140, 0)
    _, _, cw1, ch1 = client_rect(hwnd)
    bounds1 = player.browser_state()["bounds"]
    check("right-edge drag shrinks the window", cw1 < cw0 - 80, f"{cw0} -> {cw1}")
    check("page view follows the right edge", bounds1[2] < bounds0[2] - 80,
          f"{bounds0} -> {bounds1}")
    check("page view still spans the client width", abs(bounds1[2] - cw1) <= 3,
          f"bounds={bounds1} client={cw1}")
    check("page view stays visible after the drag", player.browser_state()["web_visible"],
          f"{player.browser_state()}")

    ox, oy, cw2, ch2 = client_rect(hwnd)
    bounds2 = player.browser_state()["bounds"]
    drag(player, hwnd, cw2 * 0.5, ch2 - 2, 0, 120)
    _, _, cw3, ch3 = client_rect(hwnd)
    bounds3 = player.browser_state()["bounds"]
    check("bottom-edge drag grows the window", ch3 > ch2 + 60, f"{ch2} -> {ch3}")
    check("page view follows the bottom edge", bounds3[3] > bounds2[3] + 60,
          f"{bounds2} -> {bounds3}")
    check("page view still spans the client height",
          abs(bounds3[3] - (ch3 - bounds3[1])) <= 3,
          f"bounds={bounds3} client_h={ch3}")

    # ---- 2c. left edge: position AND size must change, page view follows ----------------
    rectL = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rectL))
    boundsL = player.browser_state()["bounds"]
    ox, oy, cwL, chL = client_rect(hwnd)
    drag(player, hwnd, 2, chL * 0.5, 120, 0)  # press the left band, drag right
    rectL2 = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rectL2))
    boundsL2 = player.browser_state()["bounds"]
    check("left-edge drag moves the window's left edge right",
          rectL2.left > rectL.left + 60, f"x {rectL.left} -> {rectL2.left}")
    check("left-edge drag shrinks the width",
          (rectL2.right - rectL2.left) < (rectL.right - rectL.left) - 60,
          f"w {rectL.right - rectL.left} -> {rectL2.right - rectL2.left}")
    check("page view follows the left edge",
          boundsL2[2] < boundsL[2] - 60 and abs(boundsL2[2] - client_rect(hwnd)[2]) <= 3,
          f"{boundsL} -> {boundsL2}")

    player.close()
    server.shutdown()
    server.server_close()

    if _failures:
        print(f"\n{len(_failures)} check(s) FAILED: {_failures}")
        return 1
    print("\nall browser input/window checks passed")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(line_buffering=True)  # type: ignore[attr-defined]
    sys.exit(main())
