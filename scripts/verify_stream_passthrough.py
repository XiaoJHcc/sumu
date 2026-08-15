# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# 原片直出 (passthrough) VOD acceptance test for the web-streaming server, using a DUMMY engine
# (passthrough never touches the AI engine): directory index -> custom player page (seekbar JS) ->
# meta (duration) -> HLS playlist (unique `s<nonce>.NNNNN.ts` names + ?token= injection) -> a real
# h264 segment -> server-side seek (reposition) -> stop (204). This exercises the fixes for the AI
# v1 blockers: fast start, reposition seek, and stop-on-demand.
from __future__ import annotations

import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "python"))
sys.path.insert(0, os.path.join(_REPO, "python", "sumu"))

from sumu.webstream.server import StreamingServer  # noqa: E402


class DummyEngine:
    def cancel(self):
        pass


def _startupinfo():
    if sys.platform != "win32":
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    return si


def fetch(base, path, timeout=30):
    try:
        with urllib.request.urlopen(base + path, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def first_seg(playlist: str):
    for line in playlist.splitlines():
        n = line.strip().split("?")[0]
        if n.endswith(".ts"):
            return n
    return None


def main():
    src_dir = os.path.join(_REPO, "scripts", "trace", "pt_src")
    cache = os.path.join(_REPO, "scripts", "trace", "pt_cache")
    os.makedirs(src_dir, exist_ok=True)
    clip = os.path.join(src_dir, "clip.mp4")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i",
         os.path.join(_REPO, "test_video.mp4"), "-t", "8", "-c", "copy", clip],
        check=True, startupinfo=_startupinfo())

    srv = StreamingServer(src_dir, 0, DummyEngine(), host="127.0.0.1", token="tok",
                          cache_dir=cache, passthrough=True)
    srv.start()
    port = srv.httpd.server_address[1]
    base = f"http://127.0.0.1:{port}"

    results = []
    def check(name, cond, detail=""):
        results.append(cond)
        print(f"  [{'OK' if cond else 'FAIL'}] {name} {detail}", file=sys.stderr)

    code, body = fetch(base, "/play/clip.mp4?token=tok")
    body_s = body.decode("utf-8", "replace")
    check("player page (hls.js + custom seekbar)", code == 200 and 'id="v"' in body_s
          and "hls.min.js" in body_s and 'id="seek"' in body_s, f"code={code}")
    # Regression guard for the no-token seek bug: the m3u8 URL builder must separate params with
    # `&` (the old `AMP`-based builder emitted `?start=N?_=...` when TOKEN was empty, so the server
    # parsed `start` as `"N?_=..."`, float() failed, and the seek was silently dropped -> "从头播").
    check("m3u8Url uses & separator", "'start='+pos+'&_='" in body_s,
          "regression: query params joined by '&'")

    code, jsbody = fetch(base, "/static/hls.min.js?token=tok")
    check("hls.js served", code == 200 and b"!function" in jsbody and b"Hls" in jsbody,
          f"code={code} size={len(jsbody) if isinstance(jsbody, bytes) else '-'}")

    code, body = fetch(base, "/stream/clip.mp4/meta?token=tok")
    meta = json.loads(body.decode("utf-8", "replace"))
    check("meta duration", code == 200 and meta.get("duration", 0) > 0,
          f"code={code} dur={meta.get('duration')}")

    code, body = fetch(base, "/stream/clip.mp4/index.m3u8?token=tok")
    pl = body.decode("utf-8", "replace")
    seg = first_seg(pl)
    check("playlist", code == 200 and seg is not None, f"code={code} seg={seg}")
    check("token injected", ".ts?token=tok" in pl)
    check("EXT-X-START injected", "#EXT-X-START:TIME-OFFSET=0" in pl)
    check("unique segment name", bool(seg) and seg.startswith("s"), f"seg={seg}")

    code, sbody = fetch(base, f"/stream/clip.mp4/{seg}?token=tok")
    check("segment 200 non-empty", code == 200 and len(sbody) > 1000,
          f"code={code} size={len(sbody) if isinstance(sbody, bytes) else '-'}")

    code, body = fetch(base, "/stream/clip.mp4/seek?t=4.0&token=tok")
    sj = json.loads(body.decode("utf-8", "replace"))
    check("seek json position", code == 200 and abs(sj.get("position", 0) - 4.0) < 0.1,
          f"code={code} pos={sj.get('position')}")

    # client path: reposition carried on the m3u8 URL as ?start=<seconds>
    code, body = fetch(base, "/stream/clip.mp4/index.m3u8?start=6.0&token=tok")
    seg2 = first_seg(body.decode("utf-8", "replace"))
    check("?start= reposition (new nonce)", code == 200 and seg2 is not None and seg2 != seg,
          f"code={code} seg2={seg2}")

    # a live HLS re-fetch re-sends the same ?start=; must NOT restart the transcode
    code, body = fetch(base, "/stream/clip.mp4/index.m3u8?start=6.0&token=tok")
    seg3 = first_seg(body.decode("utf-8", "replace"))
    check("same ?start= no restart", code == 200 and seg3 == seg2, f"seg3={seg3}")

    # stale out-of-order reposition guard: an older `_` generation must NOT regress the transcode.
    code, body = fetch(base, "/stream/clip.mp4/index.m3u8?start=5.0&_=5000&token=tok")
    seg_hi = first_seg(body.decode("utf-8", "replace"))
    code, body = fetch(base, "/stream/clip.mp4/index.m3u8?start=0.0&_=1000&token=tok")
    seg_lo = first_seg(body.decode("utf-8", "replace"))
    check("stale ?start= (_ guard)", code == 200 and seg_hi is not None and seg_lo == seg_hi,
          f"hi={seg_hi} lo={seg_lo}")

    try:
        with urllib.request.urlopen(base + "/stream/clip.mp4/stop?token=tok", timeout=10) as r:
            stop_code = r.status
    except urllib.error.HTTPError as e:
        stop_code = e.code
    check("stop 204", stop_code == 204, f"code={stop_code}")

    srv.stop()
    ok = all(results)
    print(f"== RESULT == {'PASS' if ok else 'FAIL'}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
