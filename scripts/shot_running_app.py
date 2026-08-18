# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Screenshot/click helper for an ALREADY-RUNNING sumu window (finds it by title, raises it,
# optionally clicks client-relative scaled coordinates, grabs the window rect to a PNG).
# Used together with scripts/shot_export_ui.py for real-app UI verification.
#
# Usage:
#   python scripts/shot_running_app.py out.png [click_x_frac click_y_frac]
# Fractions are relative to the grabbed window image (0..1), so coordinates can be read
# straight off a previous screenshot.

import os
import sys
import time
import ctypes
from ctypes import wintypes

user32 = ctypes.windll.user32

hwnd = user32.FindWindowW(None, "sumu")
if not hwnd:
    print("window not found", file=sys.stderr)
    sys.exit(1)

fg = user32.GetForegroundWindow()
cur_tid = ctypes.windll.kernel32.GetCurrentThreadId()
# SetForegroundWindow often fails silently for background processes; retry the
# attach/restore/raise dance until the window actually owns the foreground.
for _ in range(20):
    fg = user32.GetForegroundWindow()
    fg_tid = user32.GetWindowThreadProcessId(fg, None)
    user32.AttachThreadInput(cur_tid, fg_tid, True)
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    user32.BringWindowToTop(hwnd)
    user32.AttachThreadInput(cur_tid, fg_tid, False)
    if user32.GetForegroundWindow() == hwnd:
        break
    time.sleep(0.25)
print(f"foreground={'OK' if user32.GetForegroundWindow() == hwnd else 'FAILED'}", file=sys.stderr)
user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x2 | 0x1)  # TOPMOST, NOMOVE|NOSIZE
time.sleep(0.6)

rc = wintypes.RECT()
user32.GetWindowRect(hwnd, ctypes.byref(rc))
w, h = rc.right - rc.left, rc.bottom - rc.top

if len(sys.argv) >= 4:
    # Post input messages straight to the window queue (client coords): mouse_event after
    # SetCursorPos leaves ImGui's win32 backend with a stale mouse position (it only
    # updates on WM_MOUSEMOVE), so the click lands wherever the cursor last was.
    cx = int(float(sys.argv[2]) * w)  # borderless custom chrome: image coords ~= client
    cy = int(float(sys.argv[3]) * h)
    lp = (cy << 16) | (cx & 0xFFFF)
    user32.PostMessageW(hwnd, 0x0200, 0, lp)          # WM_MOUSEMOVE
    time.sleep(0.15)
    user32.PostMessageW(hwnd, 0x0201, 1, lp)          # WM_LBUTTONDOWN (MK_LBUTTON)
    time.sleep(0.08)
    user32.PostMessageW(hwnd, 0x0202, 0, lp)          # WM_LBUTTONUP
    time.sleep(0.8)

from PIL import ImageGrab
out = sys.argv[1]
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
ImageGrab.grab(bbox=(rc.left, rc.top, rc.right, rc.bottom)).save(out)
print(f"wrote {out} ({w}x{h})", file=sys.stderr)
