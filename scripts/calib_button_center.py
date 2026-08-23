# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Pixel-level calibration for ui::Button label centering. Renders the export page
# (same setup as scripts/shot_buttons.py), screenshots it, and MEASURES -- no guessing:
#
#   1. 开始导出 (Primary chip, solid #3965A8 fill, white CJK text, NO icon):
#      locates the blue chip rect and the white ink bbox inside it, reports the
#      top/bottom/left/right gaps. Vertical gap asymmetry == the centering error in px.
#   2. 新建预设 (Secondary + lucide plus icon): locates its neutral fill region inside
#      the preset card and the bright content block; reports horizontal block centering.
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe scripts/calib_button_center.py
import os, sys, time, ctypes
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))
sys.path.insert(0, os.path.join(ROOT, "python", "sumu"))

import sumu_core
from sumu import i18n

OUTDIR = os.path.join(ROOT, "native", "trace")
SHOT = os.path.join(OUTDIR, "calib_buttons.png")


def make_preset(name, cq):
    return {"name": name, "codec": "hevc", "preset": "p7",
            "cq_enabled": cq is not None, "cq": cq or 0,
            "vbr_enabled": False, "bitrate": 2000, "maxrate": 2500,
            "audio_copy": True, "audio_bitrate": 256,
            "subtitle": True, "suffix": "_Decensored"}


def run(p, seconds):
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < seconds:
        p.pump_messages(); p.ui_tick(); time.sleep(0.02)


def grab_window():
    user32 = ctypes.windll.user32
    hwnd = user32.FindWindowW(None, "sumu")
    if not hwnd:
        sys.exit("sumu window not found")
    rc = wintypes.RECT()
    user32.SetWindowPos(hwnd, -1, 60, 10, 0, 0, 0x1)  # TOPMOST, move to known pos
    run(p, 0.3)
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    # park cursor on a neutral spot so hover fills don't pollute the measurement
    user32.SetCursorPos(rc.left + (rc.right - rc.left) // 2, rc.top + 5)
    run(p, 0.4)
    from PIL import ImageGrab
    img = ImageGrab.grab(bbox=(rc.left, rc.top, rc.right, rc.bottom)).convert("RGB")
    img.save(SHOT)
    return img


def near(px, ref, tol):
    return all(abs(a - b) <= tol for a, b in zip(px, ref))


def main():
    global p
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
    img = grab_window()
    p.close()

    px = img.load()
    W, H = img.size

    # ---- measurements are QUADRANT-LIMITED: a whole-window color scan picks up other
    # UI elements sharing the accent/fill colors and poisons the bbox.
    def scan(x0, y0, x1, y1, pred):
        xs, ys = [], []
        px = img.load()
        for y in range(y0, y1):
            for x in range(x0, x1):
                if pred(px[x, y]):
                    xs.append(x); ys.append(y)
        return (min(xs), min(ys), max(xs), max(ys)) if xs else None

    # ---- 1. 开始导出 Primary chip: solid accent #3965A8, bottom-right footer -------
    ACCENT = (57, 101, 168)
    chip = scan(W // 2, H - 260, W, H, lambda c: near(c, ACCENT, 24))
    if not chip:
        sys.exit("no accent chip found")
    chip_l, chip_top, chip_r, chip_bot = chip
    ch, cw = chip_bot - chip_top + 1, chip_r - chip_l + 1

    # white ink of 开始导出 inside the chip
    ink = scan(chip_l + 2, chip_top + 2, chip_r - 1, chip_bot - 1,
               lambda c: min(c) > 120)
    if not ink:
        sys.exit("no ink found inside chip")
    t, b = ink[1] - chip_top, chip_bot - ink[3]
    l, r = ink[0] - chip_l, chip_r - ink[2]
    print(f"chip   rect {cw}x{ch} @({chip_l},{chip_top})")
    print(f"ink(开始导出) top_gap={t} bottom_gap={b} -> VERTICAL ERROR={b - t:+d}px"
          f"   (positive = text sits HIGH)")
    print(f"       left_gap={l} right_gap={r} -> HORIZONTAL ERROR={l - r:+d}px")

    # ---- 2. Secondary+icon buttons (新建预设 / 添加文件): neutral fill over panel ---
    # button fill ~= white 8% over card panel (#262628) -> ~(56,56,58); panel ~(38,38,40).
    # Both buttons sit on one footer row, so split the fill runs along the row center
    # line and measure each button's own ink block separately.
    mid_y = None
    rows = [y for y in range(H - 200, H)
            if sum(1 for x in range(W) if near(px[x, y], (56, 56, 58), 3)) > 600]
    if rows:
        mid_y = (min(rows) + max(rows)) // 2
    if mid_y is None:
        sys.exit("no secondary button row found")
    # Button extents come from COLUMN fill counts across the whole button band (glyph
    # strokes only interrupt a few rows per column, while inter-button gaps have none),
    # so text on the scan line cannot split one button into two runs.
    top, bot = min(rows), max(rows)
    cols = [x for x in range(W)
            if sum(1 for y in range(top, bot + 1) if near(px[x, y], (56, 56, 58), 3)) > 20]
    merged, s, prev = [], cols[0], cols[0]
    for x in cols[1:]:
        if x - prev > 30:
            merged.append((s, prev)); s = x
        prev = x
    merged.append((s, prev))
    for a, b in merged:
        blk = scan(a + 2, top + 2, b - 1, bot - 1, lambda c: min(c) > 140)
        if not blk:
            print(f"btn @ {a}..{b}: no ink?")
            continue
        il, it, ir, ib = blk
        print(f"btn @ {a}..{b} w={b-a+1}  block h-center offset={((il+ir)/2-(a+b)/2):+.1f}px"
              f"  v-mid offset={((it+ib)/2-(top+bot)/2):+.1f}px")


if __name__ == "__main__":
    main()
