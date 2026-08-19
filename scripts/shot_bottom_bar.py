# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# UI verification helper: opens a bare sumu_core.Player window (no video), parks the mouse
# in the bottom reveal zone so build_bottom_bar() stays visible, renders ~2s of frames,
# then grabs the screen to a PNG for eyeballing the bottom-bar layout (icon sizes/margins).
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe scripts/shot_bottom_bar.py [out.png]

import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))        # for `sumu.i18n`
sys.path.insert(0, os.path.join(ROOT, "python", "sumu"))  # for `sumu_core`

import sumu_core  # noqa: E402
from sumu import i18n  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "native", "trace", "bottom_bar_shot.png")


def main():
    win_w = int(os.environ.get("SHOT_W", "1300"))
    win_h = int(os.environ.get("SHOT_H", "760"))
    p = sumu_core.Player(win_w, win_h, False)
    i18n.apply_to_player(p)
    # Bottom bar only renders with an active session; open the synthetic clip (SHOT_VIDEO
    # override allowed). test_clip.mp4 has an audio track so the volume controls show too.
    video = os.environ.get("SHOT_VIDEO", os.path.join(ROOT, "native", "trace", "test_clip.mp4"))
    if os.path.exists(video):
        p.open(video)
        p.set_volume(1.0)  # full volume: exercises the volume knob's right-edge clearance
    else:
        print(f"warning: {video} missing, bar will not render", file=sys.stderr)

    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 1.0:
        p.pump_messages()
        p.ui_tick()
        time.sleep(0.02)

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
    fg = user32.GetForegroundWindow()
    cur_tid = ctypes.windll.kernel32.GetCurrentThreadId()
    fg_tid = user32.GetWindowThreadProcessId(fg, None)
    user32.AttachThreadInput(cur_tid, fg_tid, True)
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    user32.BringWindowToTop(hwnd)
    user32.AttachThreadInput(cur_tid, fg_tid, False)
    user32.SetWindowPos(hwnd, -1, 60, 10, 0, 0, 0x1)  # HWND_TOPMOST, NOSIZE
    user32.GetWindowRect(hwnd, ctypes.byref(rc))

    # Park the cursor inside the bottom 15% reveal zone so the bar doesn't auto-hide.
    user32.SetCursorPos((rc.left + rc.right) // 2, rc.bottom - 20)
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 1.0:
        p.pump_messages()
        p.ui_tick()
        time.sleep(0.02)

    from PIL import ImageGrab
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    ImageGrab.grab(bbox=(rc.left, rc.top, rc.right, rc.bottom)).save(OUT)
    print(f"wrote {OUT}", file=sys.stderr)
    p.close()


if __name__ == "__main__":
    main()
