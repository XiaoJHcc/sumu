# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Drag-reorder verification for the export queue: opens a bare Player on the export screen
# with 3 PENDING synthetic items, then performs a REAL mouse drag (SetCursorPos +
# mouse_event) of the 2nd card's left drag strip upward past the 1st card, screenshotting
# before / mid-drag / after-drop for eyeballing.
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe scripts/shot_export_drag.py

import os
import sys
import time
import ctypes
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))
sys.path.insert(0, os.path.join(ROOT, "python", "sumu"))

import sumu_core  # noqa: E402
from sumu import i18n  # noqa: E402

TRACE = os.path.join(ROOT, "native", "trace")


def pump(p, seconds):
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < seconds:
        p.pump_messages()
        p.ui_tick()
        time.sleep(0.016)


def main():
    p = sumu_core.Player(1300, 760, False)
    i18n.apply_to_player(p)
    p.set_export_mode(True)
    mk = lambda i, name: {"id": i, "source": f"E:\\v\\{name}.mp4",
                          "out_path": f"E:\\v\\{name}_out.mp4", "out_mode": "auto",
                          "preset_idx": 0, "status": "pending", "progress": None}
    p.set_export_snapshot({
        "engine_ready": True, "running": False, "clip_length": 120, "global_dir": "E:\\SUMU",
        "default_preset_idx": 0,
        "presets": [{"name": "默认", "codec": "hevc", "preset": "p7", "cq_enabled": True,
                     "cq": 33, "bitrate_enabled": False, "bitrate": 0, "maxrate_enabled": False,
                     "maxrate": 0, "audio_copy": True, "audio_bitrate": 256, "subtitle": True,
                     "suffix": "_Decensored"}],
        "items": [mk(1, "aaa"), mk(2, "bbb"), mk(3, "ccc")],
    })
    pump(p, 1.5)

    user32 = ctypes.windll.user32
    hwnd = user32.FindWindowW(None, "sumu")
    if not hwnd:
        print("window not found", file=sys.stderr)
        p.close()
        sys.exit(1)
    cur_tid = ctypes.windll.kernel32.GetCurrentThreadId()
    for _ in range(20):
        fg = user32.GetForegroundWindow()
        ft = user32.GetWindowThreadProcessId(fg, None)
        user32.AttachThreadInput(cur_tid, ft, True)
        user32.ShowWindow(hwnd, 9)
        user32.SetForegroundWindow(hwnd)
        user32.BringWindowToTop(hwnd)
        user32.AttachThreadInput(cur_tid, ft, False)
        if user32.GetForegroundWindow() == hwnd:
            break
        time.sleep(0.25)
    pump(p, 0.3)
    rc = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    print(f"rect=({rc.left},{rc.top},{rc.right},{rc.bottom}) fg={user32.GetForegroundWindow() == hwnd}",
          file=sys.stderr)

    from PIL import ImageGrab

    def shot(name):
        pump(p, 0.15)
        ImageGrab.grab(bbox=(rc.left, rc.top, rc.right, rc.bottom)).save(os.path.join(TRACE, name))

    shot("drag_before.png")

    # Locate the item cards from the screenshot instead of hardcoding coordinates
    # (window size varies per run). Item cards are slightly lighter than the queue
    # card interior; scan a column through the text area for runs of card pixels.
    from PIL import Image
    img = Image.open(os.path.join(TRACE, "drag_before.png")).convert("RGB")
    W, H = img.size
    col = int(W * 0.53)
    bg = img.getpixel((col, H - 60))  # empty queue-card area below the last item

    def is_card(px):
        return sum(abs(a - b) for a, b in zip(px, bg)) > 18

    runs, y = [], 40
    while y < H - 80:
        if is_card(img.getpixel((col, y))):
            y0 = y
            while y < H and is_card(img.getpixel((col, y))):
                y += 1
            if y - y0 > 30:
                runs.append((y0, y))
        y += 1
    assert len(runs) >= 3, f"expected >=3 item cards, found {runs}"
    # left card edge: scan a card's midline rightward until item-card-colored pixels begin
    mid0 = (runs[0][0] + runs[0][1]) // 2
    c1 = img.getpixel((col, mid0))

    def is_item(px):
        return sum(abs(a - b) for a, b in zip(px, c1)) <= 12

    # left card edge: scan left from the text column along a text-free y near the
    # card's top padding (midline crosses text glyphs, which break the color match)
    edge_y = runs[0][0] + 8
    x = col
    while x > 0 and is_item(img.getpixel((x - 1, edge_y))):
        x -= 1
    grip_x = rc.left + x + 15

    if len(sys.argv) > 1 and sys.argv[1] == "end":
        gx, gy2 = grip_x, rc.top + (runs[0][0] + runs[0][1]) // 2  # card 1 grip
        gy1 = rc.top + runs[-1][1] + 40                             # below last card
    else:
        gx, gy2 = grip_x, rc.top + (runs[1][0] + runs[1][1]) // 2  # card 2 grip
        gy1 = rc.top + runs[0][0] - 15                             # above card 1
    print(f"gx={gx} gy2={gy2} gy1={gy1} runs={runs}", file=sys.stderr)
    user32.SetCursorPos(gx, gy2)
    pump(p, 0.2)
    user32.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
    pump(p, 0.2)
    shot("drag_pressed.png")
    for i in range(1, 9):  # smooth move upward
        user32.SetCursorPos(gx, gy2 - (gy2 - gy1) * i // 8)
        pump(p, 0.08)
    shot("drag_during.png")
    user32.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
    pump(p, 0.4)
    shot("drag_after.png")
    intents = p.take_ui_intents()
    print(f"intents: export_move_id={intents.get('export_move_id')} "
          f"export_move_to={intents.get('export_move_to')}", file=sys.stderr)
    print("done", file=sys.stderr)
    p.close()


if __name__ == "__main__":
    main()
