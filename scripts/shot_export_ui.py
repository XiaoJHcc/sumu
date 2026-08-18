# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# UI verification helper: opens a bare sumu_core.Player window, switches it to the
# offline-export screen with a synthetic snapshot (6 presets, 3 queue items in assorted
# states), renders ~2s of frames, then grabs the screen to a PNG for eyeballing layout
# (centering, scrollbars, margins, alignment). No video decode, no AI engine needed.
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe scripts/shot_export_ui.py [out.png]

import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Optional SHOT_PYD_DIR env: load sumu_core from another directory (e.g. dist/sumu/_internal)
# to screenshot-verify exactly what a packaged/stale build renders.
_pyd_dir = os.environ.get("SHOT_PYD_DIR")
if _pyd_dir:
    sys.path.insert(0, os.path.join(ROOT, _pyd_dir))
sys.path.insert(0, os.path.join(ROOT, "python"))        # for `sumu.i18n`
if not _pyd_dir:
    sys.path.insert(0, os.path.join(ROOT, "python", "sumu"))  # for `sumu_core`

import sumu_core  # noqa: E402
from sumu import i18n  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "native", "trace", "export_ui_shot.png")


def make_preset(name, cq, codec="hevc", preset="p7", bitrate=0, maxrate=0):
    return {
        "name": name, "codec": codec, "preset": preset,
        "cq_enabled": cq is not None, "cq": cq if cq is not None else 0,
        "bitrate_enabled": bitrate > 0, "bitrate": bitrate,
        "maxrate_enabled": maxrate > 0, "maxrate": maxrate,
        "audio_copy": True, "audio_bitrate": 256, "subtitle": True,
        "suffix": "_Decensored",
    }


def main():
    # Default to the user's real-world geometry (~1300x760 logical @150% DPI): narrow enough
    # to expose width/scroll feedback bugs that a big 1800x950 window hides.
    win_w = int(os.environ.get("SHOT_W", "1300"))
    win_h = int(os.environ.get("SHOT_H", "760"))
    p = sumu_core.Player(win_w, win_h, False)
    i18n.apply_to_player(p)
    p.set_export_mode(True)
    p.set_export_snapshot({
        "engine_ready": True,
        "running": False,
        "clip_length": 120,
        "global_dir": "E:\\SUMU",
        "default_preset_idx": 0,
        "presets": [
            make_preset("近无损", 18),
            make_preset("高画质", 30),
            make_preset("均衡", 33),
            make_preset("压缩", 40),
            make_preset("高码率 30M", None, bitrate=30000, maxrate=60000),
            make_preset("中码率 6M", None, bitrate=6000, maxrate=12000),
            make_preset("低码率 1.3M", None, bitrate=1300, maxrate=2600),
            make_preset("默认", 33, bitrate=1400, maxrate=1600),
        ],
        "items": [
            {"id": 1, "source": "E:\\Download\\南日菜乃\\apns-371\\apns-371.mp4",
             "out_path": "E:\\Download\\南日菜乃\\apns-371\\apns-371_Decensored.mp4",
             "out_mode": "auto", "preset_idx": 0, "status": "pending", "progress": None},
            {"id": 2, "source": "E:\\Download\\club-653\\club-653.mp4",
             "out_path": "E:\\SUMU\\club-653_Decensored.mp4",
             "out_mode": "global", "preset_idx": 2, "status": "running", "progress": 0.42},
            {"id": 3, "source": "E:\\Download\\start-190\\start-190.mp4",
             "out_path": "E:\\SUMU\\start-190_Decensored.mp4",
             "out_mode": "global", "preset_idx": 1, "status": "done", "progress": 1.0},
        ],
    })

    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 2.0:
        p.pump_messages()
        p.ui_tick()
        time.sleep(0.02)

    # The freshly opened window is not necessarily the foreground one (a background-launched
    # script has no foreground rights), so pin it TOPMOST and grab exactly its rect.
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    hwnd = user32.FindWindowW(None, "sumu")
    if not hwnd:
        print("window not found", file=sys.stderr)
        p.close()
        sys.exit(1)
    rc = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    print(f"hwnd={hwnd} visible={user32.IsWindowVisible(hwnd)} "
          f"rect=({rc.left},{rc.top},{rc.right},{rc.bottom})", file=sys.stderr)
    # Foreground-rights workaround: attach to the current foreground thread's input state,
    # then restore + raise our window.
    fg = user32.GetForegroundWindow()
    cur_tid = ctypes.windll.kernel32.GetCurrentThreadId()
    fg_tid = user32.GetWindowThreadProcessId(fg, None)
    user32.AttachThreadInput(cur_tid, fg_tid, True)
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    user32.BringWindowToTop(hwnd)
    user32.AttachThreadInput(cur_tid, fg_tid, False)
    # Move fully on-screen (the default position leaves the bottom under the taskbar).
    user32.SetWindowPos(hwnd, -1, 60, 10, 0, 0, 0x1)  # HWND_TOPMOST, NOSIZE
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 0.5:
        p.pump_messages()
        p.ui_tick()
        time.sleep(0.02)
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    print(f"after-raise rect=({rc.left},{rc.top},{rc.right},{rc.bottom}) "
          f"fg={user32.GetForegroundWindow()}", file=sys.stderr)

    from PIL import ImageGrab
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    ImageGrab.grab(bbox=(rc.left, rc.top, rc.right, rc.bottom)).save(OUT)
    print(f"wrote {OUT}", file=sys.stderr)

    # Second shot: click the first preset row's body to open the preset EDITOR modal,
    # verifying its label-column alignment / right margin / title strip in the same run.
    # Row 1 body center ~ (200, 289) client-relative @96DPI; scales with SUMU_UI_SCALE.
    scale = float(os.environ.get("SUMU_UI_SCALE", "1.0"))
    if rc.right - rc.left >= 1400:
        cx = rc.left + 8 + int(200 * scale)
        cy = rc.top + 47 + int(289 * scale)
        user32.SetCursorPos(cx, cy)
        user32.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
        user32.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < 1.0:
            p.pump_messages()
            p.ui_tick()
            time.sleep(0.02)
        out2 = OUT.replace(".png", "_editor.png")
        ImageGrab.grab(bbox=(rc.left, rc.top, rc.right, rc.bottom)).save(out2)
        print(f"wrote {out2}", file=sys.stderr)

    p.close()


if __name__ == "__main__":
    main()
