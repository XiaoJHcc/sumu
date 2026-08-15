# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Final integration test: real StreamingServer + real TranscodeEngine serving a decensored HLS
# stream over HTTP. Extracts a short clip, starts the server, and verifies the full path:
# directory index -> player page -> HLS playlist (with ?token= injection) -> a real TS segment,
# plus that the AI actually restored >= 1 clip (i.e. decensor ran).
from __future__ import annotations

import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "python"))
sys.path.insert(0, os.path.join(_REPO, "python", "sumu"))

import torch  # noqa: E402


def fetch(base, path, retries=1):
    last = None
    for _ in range(retries + 1):
        try:
            with urllib.request.urlopen(base + path, timeout=15) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            last = (e.code, e.read())
            if e.code == 404:
                time.sleep(1.0)
                continue
            return e.code, e.read()
        except Exception as e:  # noqa: BLE001
            last = (0, str(e).encode())
            time.sleep(1.0)
    return last


def main():
    src = os.path.join(_REPO, "scripts", "trace", "e2e_src")
    cache = os.path.join(_REPO, "scripts", "trace", "e2e_cache")
    os.makedirs(src, exist_ok=True)
    clip = os.path.join(src, "clip.mp4")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i",
         os.path.join(_REPO, "test_video.mp4"), "-t", "6", "-c", "copy", clip],
        check=True)

    from sumu.pipeline import build_models  # noqa: E402
    from sumu.scheduler import SchedulerConfig  # noqa: E402
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    det_model, res_model, pad_mode = build_models(device, device.type == "cuda",
                                                  allow_trt_compile=False)
    cfg = SchedulerConfig(clip_length=30, max_regions_per_frame=1)

    from sumu.webstream import TranscodeEngine, StreamingServer  # noqa: E402
    engine = TranscodeEngine(det_model, res_model, pad_mode, cfg)
    srv = StreamingServer(src, 0, engine, host="127.0.0.1", token="tok", cache_dir=cache,
                          passthrough=False)
    srv.start()
    port = srv.httpd.server_address[1]
    base = f"http://127.0.0.1:{port}"

    ok = True
    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and cond
        print(f"  [{'OK' if cond else 'FAIL'}] {name} {detail}", file=sys.stderr)

    code, body = fetch(base, "/?token=tok")
    check("index", code == 200 and b"clip.mp4" in body, f"code={code}")

    code, body = fetch(base, "/stream/clip.mp4/index.m3u8?token=tok")
    playlist = body.decode("utf-8", "replace")
    check("playlist", code == 200 and "index0.ts" in playlist, f"code={code}")
    check("token injected", "index0.ts?token=tok" in playlist)

    # first .ts line in the playlist (token is injected as `indexN.ts?token=...`)
    seg = None
    for line in playlist.splitlines():
        name = line.strip().split("?")[0]
        if name.endswith(".ts"):
            seg = name
            break
    code, segbody = fetch(base, f"/stream/clip.mp4/{seg}?token=tok", retries=5)
    check("segment served", code == 200 and len(segbody) > 1000,
          f"code={code} size={len(segbody) if isinstance(segbody, bytes) else '-'}")

    # decensor actually ran (wait for the transcode to finish -- last_stats is set at the end)
    deadline = time.time() + 90.0
    while engine.last_stats is None and time.time() < deadline:
        time.sleep(0.5)
    st = engine.last_stats or {}
    check("decensor ran", st.get("clips_restored", 0) > 0, f"clips={st.get('clips_restored')}")

    srv.stop()
    print(f"== RESULT == {'PASS' if ok else 'FAIL'}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
