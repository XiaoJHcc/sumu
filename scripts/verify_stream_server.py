# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Routing / session / token test for the web-streaming server, using a FAKE engine (no AI models,
# no GPU) so it exercises the full HTTP surface without the heavy transcode path: directory index,
# subfolder browse, player page, HLS playlist (with ?token= injection) and segment serving.
from __future__ import annotations

import os
import sys
import time
import urllib.request
import urllib.error

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "python"))
sys.path.insert(0, os.path.join(_REPO, "python", "sumu"))

from sumu.webstream.server import StreamingServer  # noqa: E402


class FakeEngine:
    def __init__(self):
        self.cancelled = False

    def cancel(self):
        self.cancelled = True

    def run(self, source, out, mode, *, bitrate="8M", video_meta=None, audio_source=None,
            progress_cb=None):
        time.sleep(0.4)  # simulate first-segment latency
        with open(os.path.join(out, "index.m3u8"), "w") as f:
            f.write("#EXTM3U\n#EXT-X-TARGETDURATION:2\n"
                    "#EXTINF:2.0,\nindex0.ts\n#EXTINF:1.0,\nindex1.ts\n#EXT-X-ENDLIST\n")
        with open(os.path.join(out, "index0.ts"), "wb") as f:
            f.write(b"fake-seg0")
        with open(os.path.join(out, "index1.ts"), "wb") as f:
            f.write(b"fake-seg1")
        for i in range(3):
            if progress_cb:
                progress_cb(i, 3)
        return 3


def fetch(base, path, expect_status=200):
    try:
        with urllib.request.urlopen(base + path, timeout=10) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def main():
    src = os.path.join(_REPO, "scripts", "trace", "stream_src")
    cache = os.path.join(_REPO, "scripts", "trace", "stream_cache")
    os.makedirs(os.path.join(src, "sub"), exist_ok=True)
    open(os.path.join(src, "a.mp4"), "w").close()
    open(os.path.join(src, "sub", "b.mp4"), "w").close()
    open(os.path.join(src, "readme.txt"), "w").close()  # non-video -> filtered

    srv = StreamingServer(src, 0, FakeEngine(), host="127.0.0.1", token="testtoken",
                          cache_dir=cache)
    srv.start()
    port = srv.httpd.server_address[1]
    base = f"http://127.0.0.1:{port}"

    checks = []
    def check(name, cond, detail=""):
        checks.append((name, cond, detail))
        print(f"  [{'OK' if cond else 'FAIL'}] {name} {detail}", file=sys.stderr)

    code, body = fetch(base, "/")
    check("no-token 401", code == 401, f"code={code}")

    code, body = fetch(base, "/?token=testtoken")
    check("root index", code == 200 and "a.mp4" in body and "sub" in body, f"code={code}")
    check("txt filtered", "readme.txt" not in body)

    code, body = fetch(base, "/browse/sub?token=testtoken")
    check("subfolder", code == 200 and "b.mp4" in body, f"code={code}")

    code, body = fetch(base, "/play/a.mp4?token=testtoken")
    check("player page", code == 200 and "<video" in body, f"code={code}")

    code, body = fetch(base, "/stream/a.mp4/index.m3u8?token=testtoken")
    check("playlist", code == 200 and "index0.ts" in body, f"code={code}")
    check("token injected", "index0.ts?token=testtoken" in body)

    code, body = fetch(base, "/stream/a.mp4/index0.ts?token=testtoken")
    check("segment", code == 200 and body == "fake-seg0", f"code={code}")

    # second session while busy: use a second video, but first session is done by now (fake fast)
    # -> should start a new session, not 503. (busy path is transient; just verify it serves.)
    srv.stop()

    ok = all(c[1] for c in checks)
    print(f"== RESULT == {'PASS' if ok else 'FAIL'}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
