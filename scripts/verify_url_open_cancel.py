# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Verification for the URL-float loading-state cancel (Cancel -> Player::cancel_open_url
# -> Decoder AVIOInterruptCB) plus a regression check that a normal URL open still works:
#
#   1. stall   -- a TCP server that accepts but never responds: player.open() blocks inside
#                 avformat_open_input; cancel_open_url() must abort it in ~1s, far below the
#                 15s rw_timeout. open_url_cancel_requested() flips True, then
#                 notify_open_url_finished(False) clears it again (full app.py cycle).
#   2. serve   -- a real http.server over test_video.mp4: player.open(url) succeeds, and the
#                 interrupt flag from the previous aborted open does NOT leak into the new
#                 session (Decoder::open clears it on entry).
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe scripts/verify_url_open_cancel.py

import functools
import http.server
import os
import socket
import socketserver
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python", "sumu"))
import sumu_core  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_VIDEO = os.path.join(ROOT, "test_video.mp4")

_failures = []


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}{(' -- ' + detail) if detail else ''}")
    if not cond:
        _failures.append(name)


class _StallServer(socketserver.ThreadingTCPServer):
    """Accepts connections and lets them hang -- avformat_open_input blocks on the read."""
    allow_reuse_address = True
    daemon_threads = True

    class Handler(socketserver.BaseRequestHandler):
        def handle(self):
            time.sleep(60)  # never respond; connection dies with the server


class _RangeHandler(http.server.SimpleHTTPRequestHandler):
    """Minimal Range support -- FFmpeg probes MP4 moov with byte-range reads and bails
    ("Unexpected offset ... got 0" -> moov atom not found) against a server that ignores
    the Range header, which stock SimpleHTTPRequestHandler does."""

    def send_head(self):
        rng = self.headers.get("Range")
        if not rng:
            return super().send_head()
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            return super().send_head()
        try:
            start_s, _, end_s = rng.removeprefix("bytes=").partition("-")
            size = os.path.getsize(path)
            start = int(start_s)
            end = int(end_s) if end_s else size - 1
            end = min(end, size - 1)
        except ValueError:
            return super().send_head()
        length = end - start + 1
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        self.end_headers()
        f = open(path, "rb")
        f.seek(start)
        self._range_left = length
        self._range_file = f
        return f  # do_GET only calls copyfile() on a truthy return

    def copyfile(self, source, outputfile):
        f = getattr(self, "_range_file", None)
        if f is None:
            return super().copyfile(source, outputfile)
        try:
            left = self._range_left
            while left > 0:
                chunk = f.read(min(65536, left))
                if not chunk:
                    break
                outputfile.write(chunk)
                left -= len(chunk)
        except OSError:
            pass  # client went away mid-transfer (FFmpeg closes probe connections)
        finally:
            f.close()
            self._range_file = None

    def log_message(self, *args):
        pass


def _open_in_thread(player, url):
    """Mirror app.py's _open_worker: blocking open off the main thread (GIL released)."""
    box = {}

    def run():
        try:
            player.open(url)
            box["ok"] = True
        except Exception as e:  # noqa: BLE001
            box["ok"] = False
            box["err"] = e

    t = threading.Thread(target=run, name="verify-open", daemon=True)
    t.start()
    return t, box


def main():
    check("test video exists", os.path.isfile(TEST_VIDEO), TEST_VIDEO)

    player = sumu_core.Player()

    # ---- 1. cancel a stalled network open ------------------------------------------
    stall = _StallServer(("127.0.0.1", 0), _StallServer.Handler)
    threading.Thread(target=stall.serve_forever, daemon=True).start()
    stall_url = f"http://127.0.0.1:{stall.server_address[1]}/never.mp4"

    t, box = _open_in_thread(player, stall_url)
    time.sleep(2.0)
    check("open still blocked after 2s", "ok" not in box)
    check("cancel flag initially false", not player.open_url_cancel_requested())

    t0 = time.monotonic()
    player.cancel_open_url()
    check("cancel flag set", player.open_url_cancel_requested())
    t.join(timeout=10.0)
    abort_s = time.monotonic() - t0
    check("worker unwound after cancel", "ok" in box, f"{abort_s:.2f}s")
    check("aborted open raised (failure path)", box.get("ok") is False,
          repr(box.get("err"))[:120])
    check("abort was fast (interrupt, not rw_timeout)", abort_s < 10.0, f"{abort_s:.2f}s")
    check("flag still set until notify", player.open_url_cancel_requested())
    player.notify_open_url_finished(False)  # app.py's _finish_open_cancelled does this
    check("notify clears the cancel flag", not player.open_url_cancel_requested())
    stall.shutdown()
    stall.server_close()

    # Same Player, straight into the next open -- this is the app's retry-from-form flow and
    # doubles as proof that the decoder interrupt flag does not leak across opens.

    # ---- 2. regression: a normal URL open still works -------------------------------
    handler = functools.partial(_RangeHandler, directory=ROOT)
    httpd = socketserver.ThreadingTCPServer(("127.0.0.1", 0), handler)
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{httpd.server_address[1]}/test_video.mp4"

    t, box = _open_in_thread(player, url)
    t.join(timeout=30.0)
    check("normal URL open succeeded", box.get("ok") is True, repr(box.get("err"))[:120])
    if box.get("ok"):
        check("session is network", player.is_network())
        check("has frames", player.frame_count() > 0,
              f"frames={player.frame_count()} fps={player.fps():.3f}")
        player.play()
        time.sleep(1.0)
        check("playback advances", player.current_frame() > 0,
              f"frame={player.current_frame()}")
        player.pause()
    httpd.shutdown()
    httpd.server_close()

    player.close()

    if _failures:
        print(f"\n{len(_failures)} check(s) FAILED: {_failures}")
        sys.exit(1)
    print("\nall URL-open-cancel checks passed")


if __name__ == "__main__":
    main()
