# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# End-to-end verification of the embedded browser page (native/src/browser_view.cpp +
# Player's browser-page state machine). No mocks: a real WebView2 loads a real page over
# HTTP and the captured request context is then used to really open the video.
#
#   1. A local Range-capable server hosts
#        /page              HTML <video muted autoplay src="/media/movie.mp4?t=e2e">, replies
#                           with Set-Cookie: sumu_e2e=1 (the cookie the native open must replay)
#        /media/movie.mp4   test_video.mp4, but answers 403 unless the request carries a
#                           Referer pointing at /page AND the sumu_e2e=1 cookie
#   2. Negative control: player.open(media_url) with no request context is rejected -- so the
#      later success really proves the headers/cookies were carried.
#   3. request_open_url_popup() enters the page (browser_enter intent).
#   4. browser_navigate(media_url_with_query) is treated as a direct media link: the intent
#      carries the URL with NO headers/cookies (query string does not hide the extension).
#   5. browser_navigate(page_url) really loads the page; the page's own media request is
#      captured as the candidate, with Referer/User-Agent.
#   6. browser_play_native() -> CookieManager lookup -> UiIntents carries the media URL plus
#      the page's Referer/User-Agent and the cookie jar.
#   7. player.open(url, headers=..., cookies=...) really opens it and playback advances.
#   8. notify_open_url_finished(True) closes the page (browser_exit intent).
#   R1. Cookie lookup issued, then a direct media link submitted WITHOUT pumping in between:
#       only the direct link's open_path may appear (no headers/cookies), and the retired
#       lookup's late answer must not produce a second handover.
#   R2. Cookie lookup issued, then the page navigates away: the stale answer must not hand the
#       old media URL over; once the new page's candidate appears, native play must work again
#       (the pending state may never stick).
#   R3. Page view audio follows visibility: muted=false while the page is shown, muted=true
#       after leaving it, muted=false again when the page is restored.
#   9. With the page view forced unavailable (SUMU_WEBVIEW2_OFF, the same dev-hook spirit as
#      SUMU_UI_SCALE): the page degrades to browser_unavailable, navigation is inert, and a
#      plain local open still plays -- "no WebView2" must never affect anything else.
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe scripts/verify_browser.py

import http.server
import io
import os
import re
import sys
import threading
import time
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))
sys.path.insert(0, os.path.join(ROOT, "python", "sumu"))

import sumu_core  # noqa: E402
from sumu import i18n  # noqa: E402

TEST_VIDEO = os.path.join(ROOT, "test_video.mp4")

_failures = []
_requests = []  # (path, referer, cookie) of every /media request the server saw


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}{(' -- ' + detail) if detail else ''}")
    if not cond:
        _failures.append(name)


PAGE_HTML = """<!doctype html>
<html><head><meta charset="utf-8"><title>sumu e2e __TAG__</title></head>
<body style="margin:0;background:#101012;color:#ddd;font:16px sans-serif">
<h3 style="padding:12px">sumu browser e2e __TAG__</h3>
<video src="/media/movie.mp4?t=__TAG__" muted autoplay playsinline width="640"></video>
</body></html>
"""

# Two pages, two different media URLs: the second one is what the stale-cookie regression needs
# (a handover naming the FIRST page's URL would be wrong, and is what we assert never happens).
PAGES = {
    "/page": PAGE_HTML.replace("__TAG__", "e2e"),
    "/page2": PAGE_HTML.replace("__TAG__", "e2e2"),
}


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
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):  # noqa: N802 -- BaseHTTPRequestHandler's API
        path = urllib.parse.urlparse(self.path).path
        if path in PAGES:
            body = PAGES[path].encode("utf-8")
            self._plain(200, body, (("Content-Type", "text/html; charset=utf-8"),
                                    ("Set-Cookie", "sumu_e2e=1; Path=/")))
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

    def _serve_file(self, filename):
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


def main():
    check("test video exists", os.path.isfile(TEST_VIDEO), TEST_VIDEO)
    if not os.path.isfile(TEST_VIDEO):
        return 1

    server = _Server(("127.0.0.1", 0), _Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    page_url = f"{base}/page"
    page2_url = f"{base}/page2"
    media_url = f"{base}/media/movie.mp4"
    media_with_query = f"{media_url}?t=e2e"
    media2_with_query = f"{media_url}?t=e2e2"

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
    check("browser page state reports the page", bool(player.browser_state()["page"]))
    ready = pump(player, 15.0, lambda: player.browser_state()["available"]
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
    del ready

    # ---- 4. direct media link typed into the address bar --------------------------------
    player.browser_navigate(media_with_query)
    intents, ok = drain_until(player, lambda i: i.get("open_path"), 5.0)
    check("address bar media link goes straight to the native open", ok,
          repr(intents.get("open_path")))
    check("query string does not hide the extension", intents.get("open_path") == media_with_query,
          repr(intents.get("open_path")))
    check("direct link carries no page context",
          not intents.get("open_headers") and not intents.get("open_cookies"),
          f"headers={intents.get('open_headers')!r} cookies={intents.get('open_cookies')!r}")
    player.notify_open_url_finished(False)  # simulate the failure path, back to the page
    pump(player, 0.3)

    # ---- 5. navigate a real page and capture its media candidate ------------------------
    player.browser_navigate(page_url)
    ok = pump(player, 20.0, lambda: bool(player.browser_state()["candidate"]))
    state = player.browser_state()
    check("page really loaded in WebView2", state["url"].startswith(page_url), state["url"])
    check("candidate captured", ok, state["candidate"])
    check("candidate is the page's media URL", state["candidate"] == media_with_query,
          state["candidate"])
    check("candidate carries the page referer", "/page" in (state.get("referer") or ""),
          repr(state.get("referer")))

    # ---- R1. a direct media link submitted while a cookie lookup is in flight ----------
    # No ui_tick between the two calls: the CookieManager answer cannot be consumed in between,
    # so this is exactly the "user clicks native play, then pastes a direct link" race. The
    # second action must win, and the retired lookup must never hand anything over afterwards.
    player.browser_play_native()
    check("cookie lookup is in flight before the direct link",
          player.browser_state()["cookie_pending"], f"{player.browser_state()}")
    player.browser_navigate(media_with_query)
    records = collect_intents(player, 3.0)
    records += collect_intents(player, 2.0)  # give the retired answer time to arrive late
    opens = [r for r in records if r.get("open_path")]
    check("direct link wins over the pending cookie lookup",
          len(opens) == 1 and opens[0].get("open_path") == media_with_query,
          f"{[r.get('open_path') for r in opens]}")
    if opens:
        check("winning handover carries no page context",
              not opens[0].get("open_headers") and not opens[0].get("open_cookies"),
              f"headers={opens[0].get('open_headers')!r} cookies={opens[0].get('open_cookies')!r}")
    check("retired cookie answer never hands over",
          not any(r.get("open_headers") for r in records),
          f"{[(r.get('open_path'), r.get('open_headers')) for r in records if r.get('open_path')]}")
    check("no cookie wait left after the direct link",
          not player.browser_state()["cookie_pending"], f"{player.browser_state()}")
    player.notify_open_url_finished(False)  # failure path -> back to the page
    pump(player, 0.5)

    # ---- R2. a cookie lookup overtaken by a navigation ---------------------------------
    ok = pump(player, 5.0, lambda: player.browser_state()["candidate"] == media_with_query)
    check("first page candidate still available", ok, player.browser_state()["candidate"])
    player.browser_play_native()          # lookup for page 1's media
    check("cookie lookup is in flight before the navigation",
          player.browser_state()["cookie_pending"], f"{player.browser_state()}")
    player.browser_navigate(page2_url)    # ...immediately replaced by a different page
    records = collect_intents(player, 3.0)
    records += collect_intents(player, 2.0)  # ...and a tail window for a late answer
    check("stale cookie answer after navigation does not hand over",
          not any(r.get("open_path") for r in records),
          f"{[r.get('open_path') for r in records if r.get('open_path')]}")
    check("no cookie wait left after the navigation",
          not player.browser_state()["cookie_pending"], f"{player.browser_state()}")
    ok = pump(player, 20.0,
              lambda: player.browser_state()["candidate"] == media2_with_query)
    check("second page candidate captured", ok, player.browser_state()["candidate"])

    # ---- 6. native play on the new page: cookie lookup -> UiIntents ---------------------
    player.browser_play_native()
    intents, ok = drain_until(player, lambda i: i.get("open_path"), 10.0)
    check("native play works again after the aborted lookup", ok,
          repr(intents.get("open_path")))
    check("handover URL is the second page's media URL",
          intents.get("open_path") == media2_with_query, repr(intents.get("open_path")))
    check("native play hands over the page context",
          bool(intents.get("open_headers")) and bool(intents.get("open_cookies")),
          f"headers={intents.get('open_headers')!r} cookies={intents.get('open_cookies')!r}")
    headers = intents.get("open_headers") or ""
    cookies = intents.get("open_cookies") or ""
    check("handover headers carry Referer", "Referer: " + page2_url in headers, repr(headers))
    check("handover headers carry User-Agent", "User-Agent: " in headers, repr(headers))
    check("handover cookies carry the page cookie", "sumu_e2e=1" in cookies, repr(cookies))
    check("no cookie wait left after the handover",
          not player.browser_state()["cookie_pending"], f"{player.browser_state()}")

    if not ok:
        player.close()
        server.shutdown()
        server.server_close()
        return 1

    # ---- 7. the real open with that context --------------------------------------------
    try:
        player.open(intents["open_path"], headers=headers, cookies=cookies)
        opened = True
    except Exception as e:  # noqa: BLE001
        opened = False
        print(f"  open error: {e!r}")
    check("native open with page context succeeded", opened)
    if opened:
        check("session is network", player.is_network())
        check("has frames", player.frame_count() > 0, f"frames={player.frame_count()}")
        player.play()
        pump(player, 1.0)
        check("playback advances", player.current_frame() > 0, f"frame={player.current_frame()}")
        player.pause()
        check("server saw an authorised media request",
              any("/page" in ref and "sumu_e2e=1" in ck for _, ref, ck in _requests))
        player.notify_open_url_finished(True)
        intents, ok = drain_until(player, lambda i: i.get("browser_exit"), 5.0)
        check("page closed after the successful open (browser_exit)", ok)
        check("page state cleared", not player.browser_state()["page"])

    player.close()

    # ---- 9. no WebView2: the page degrades, everything else still works ------------------
    # SUMU_WEBVIEW2_OFF makes the page view report itself unavailable (dev hook, same spirit as
    # SUMU_UI_SCALE), so the degradation path is exercised on a machine that HAS the runtime.
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
        # ...and the plain native open path is completely unaffected.
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
    # A closed/loading page must not keep playing its own audio over the video; showing the page
    # again unmutes it. Its own Player keeps this independent of the playback above.
    mp = sumu_core.Player(1024, 640, False)
    i18n.apply_to_player(mp)
    pump(mp, 0.3)
    try:
        mp.request_open_url_popup()
        pump(mp, 1.0)
        check("page starts muted while hidden (first-screen form)", mp.browser_state()["muted"],
              f"{mp.browser_state()}")
        mp.browser_navigate(page_url)
        ok = pump(mp, 20.0, lambda: mp.browser_state()["url"].startswith(page_url)
                  and not mp.browser_state()["muted"])
        check("page visible -> unmuted", ok, f"{mp.browser_state()}")
        mp.exit_browser_page()
        pump(mp, 0.5)
        check("page closed -> muted", mp.browser_state()["muted"], f"{mp.browser_state()}")
        mp.request_open_url_popup()  # continuity: the loaded page comes back
        pump(mp, 1.0)
        state = mp.browser_state()
        check("page restored -> unmuted", state["web"] and not state["muted"], f"{state}")
    finally:
        mp.close()

    server.shutdown()
    server.server_close()

    if _failures:
        print(f"\n{len(_failures)} check(s) FAILED: {_failures}")
        return 1
    print("\nall browser page checks passed")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
    sys.exit(main())
