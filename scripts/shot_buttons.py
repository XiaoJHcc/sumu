# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# UI verification helper: export page (framed folder picker), top-bar close hover-red,
# web-server stream modal (framed lucide-folder picker). Real-cursor hover; no clicks
# except opening the stream modal. Shots land in native/trace/btn_*.png.
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe scripts/shot_buttons.py
import os, sys, time, ctypes
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))
sys.path.insert(0, os.path.join(ROOT, "python", "sumu"))

import sumu_core
from sumu import i18n

OUTDIR = os.path.join(ROOT, "native", "trace")


def make_preset(name, cq):
    return {"name": name, "codec": "hevc", "preset": "p7",
            "cq_enabled": cq is not None, "cq": cq or 0,
            "bitrate_enabled": False, "bitrate": 0,
            "maxrate_enabled": False, "maxrate": 0,
            "audio_copy": True, "audio_bitrate": 256, "subtitle": True,
            "suffix": "_Decensored"}


def run(p, seconds):
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < seconds:
        p.pump_messages(); p.ui_tick(); time.sleep(0.02)


def main():
    p = sumu_core.Player(1300, 760, False)
    i18n.apply_to_player(p)
    p.set_export_mode(True)
    p.set_export_snapshot({
        "engine_ready": True, "running": False, "clip_length": 120,
        "global_dir": "E:\\SUMU", "default_preset_idx": 0,
        "presets": [make_preset("近无损", 18), make_preset("均衡", 33)],
        "items": [],
    })
    run(p, 1.5)

    user32 = ctypes.windll.user32
    hwnd = user32.FindWindowW(None, "sumu")
    rc = wintypes.RECT()
    user32.SetWindowPos(hwnd, -1, 60, 10, 0, 0, 0x1)  # TOPMOST
    run(p, 0.3)
    user32.GetWindowRect(hwnd, ctypes.byref(rc))

    from PIL import ImageGrab

    def grab(name):
        ImageGrab.grab(bbox=(rc.left, rc.top, rc.right, rc.bottom)).save(
            os.path.join(OUTDIR, name))
        print("wrote", name, file=sys.stderr)

    # cursor parked over a neutral spot
    user32.SetCursorPos(rc.left + 700, rc.top + 400)
    run(p, 0.3)
    grab("btn_export_page.png")

    # hover the top-bar close X (no click): last 32px button, 2px inset at 96dpi
    scale = 1.5
    bx = rc.right - int((2 + 16) * scale)
    by = rc.top + int(18 * scale)
    user32.SetCursorPos(bx, by)
    run(p, 0.3)
    grab("btn_close_hover.png")

    # stream modal: leave export mode, click the web-server top-bar button (4th icon)
    p.set_export_mode(False)
    run(p, 0.3)
    wx = rc.left + int((2 + 16) * scale + 3 * (32 + 2) * scale)
    wy = rc.top + int(18 * scale)
    user32.SetCursorPos(wx, wy)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    run(p, 0.8)
    # park cursor off the buttons
    user32.SetCursorPos(rc.left + 700, rc.top + 500)
    run(p, 0.3)
    grab("btn_stream_modal.png")

    p.close()


if __name__ == "__main__":
    main()
