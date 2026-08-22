# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Modal-padding forensics: opens each ui::BeginModal dialog (open-URL, web-server,
# preset editor), screenshots it, and measures window width + left/right content
# margins in PHYSICAL pixels. Baseline expectation at 150% DPI (s=1.5):
#   pad = kPaddingContainer(12) * 1.5 = 18px on every side
#   URL/web window width = 440*1.5 + 36 = 696; preset editor = 480*1.5 + 36 = 756
#
# The modal is the only UNDIMMED panel-colored region in the central band, so the
# bbox of kPanelBg pixels in cx±700/cy±450 is the modal rect.
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe scripts/shot_modals.py
import os, sys, time, ctypes
from ctypes import wintypes
from PIL import Image, ImageGrab

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))
sys.path.insert(0, os.path.join(ROOT, "python", "sumu"))

import sumu_core
from sumu import i18n

OUTDIR = os.path.join(ROOT, "native", "trace")
SCALE = 1.5

PANEL = (38, 38, 40)      # kPanelBg -- modal body fill
FRAME = (53, 53, 55)      # kControlBg (white 7%) over kPanelBg -- input frames


def make_preset(name, cq):
    return {"name": name, "codec": "hevc", "preset": "p7",
            "cq_enabled": cq is not None, "cq": cq or 0,
            "vbr_enabled": False, "bitrate": 2000, "maxrate": 2500,
            "audio_copy": True, "audio_bitrate": 256, "subtitle": True,
            "suffix": "_Decensored"}


def apply_export(p):
    p.set_export_snapshot({
        "engine_ready": True, "running": False, "clip_length": 120,
        "global_dir": "E:\\SUMU", "default_preset_idx": 0,
        "presets": [make_preset("近无损", 18), make_preset("均衡", 33)],
        "items": [],
    })
    p.set_export_mode(True)


def run(p, seconds):
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < seconds:
        p.pump_messages(); p.ui_tick(); time.sleep(0.02)


def measure(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    cx, cy = w // 2, h // 2

    def is_c(px_, c, tol):
        return all(abs(px_[i] - c[i]) <= tol for i in range(3))

    # modal bbox: panel-colored pixels inside the central band
    x0, x1, y0, y1 = w, 0, h, 0
    for y in range(cy - 450, cy + 450):
        for x in range(cx - 700, cx + 700):
            if is_c(px[x, y], PANEL, 3):
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    if x1 == 0:
        print(f"  {os.path.basename(path)}: NO MODAL FOUND")
        return

    # field edges: runs of frame-bg pixels below the title strip (strip is ~49,49,51,
    # skip 60px); min/max x over all runs >= 60px wide
    fx0, fx1 = w, 0
    for y in range(y0 + 60, y1 + 1):
        run_start = None
        for x in range(x0, x1 + 1):
            if is_c(px[x, y], FRAME, 3):
                if run_start is None: run_start = x
            else:
                if run_start is not None and x - run_start >= 60:
                    fx0 = min(fx0, run_start); fx1 = max(fx1, x - 1)
                run_start = None
        if run_start is not None and x1 + 1 - run_start >= 60:
            fx0 = min(fx0, run_start); fx1 = max(fx1, x1)

    print(f"  {os.path.basename(path)}: win_x=[{x0}..{x1}] win_w={x1 - x0 + 1}px "
          f"field_x=[{fx0}..{fx1}] left={fx0 - x0}px right={x1 - fx1}px")


def main():
    p = sumu_core.Player(1300, 760, False)
    i18n.apply_to_player(p)
    run(p, 1.5)

    user32 = ctypes.windll.user32
    # Find OUR window: exact title + owned by this process (FindWindowW alone can land
    # on a stale window from a previous harness run, or the wrong one entirely).
    my_pid = os.getpid()
    hwnd = 0
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def _enum(h, lp):
        nonlocal hwnd
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, buf, 256)
        if buf.value == "sumu":
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == my_pid:
                hwnd = h
                return False
        return True
    user32.EnumWindows(_enum, 0)
    if not hwnd:
        print("own sumu window not found!", file=sys.stderr)
        return
    rc = wintypes.RECT()
    user32.SetWindowPos(hwnd, -1, 60, 10, 0, 0, 0x1)  # TOPMOST
    user32.SetForegroundWindow(hwnd)
    run(p, 0.3)
    user32.GetWindowRect(hwnd, ctypes.byref(rc))

    def grab(name):
        ImageGrab.grab(bbox=(rc.left, rc.top, rc.right, rc.bottom)).save(
            os.path.join(OUTDIR, name))
        print("wrote", name, file=sys.stderr)

    def click(cx, cy):
        user32.SetCursorPos(cx, cy)
        user32.mouse_event(0x0002, 0, 0, 0, 0)
        user32.mouse_event(0x0004, 0, 0, 0, 0)

    def escape():
        user32.keybd_event(0x1B, 0, 0, 0)
        user32.keybd_event(0x1B, 0, 2, 0)

    def icon_x(idx):
        return rc.left + int((2 + 16) * SCALE + idx * (32 + 2) * SCALE)

    ty = rc.top + int(18 * SCALE)
    park = (rc.left + 900, rc.top + 900)

    # 1. open-URL modal (icon index 2)
    click(icon_x(2), ty)
    run(p, 0.8)
    user32.SetCursorPos(*park); run(p, 0.3)
    grab("modal_url.png")
    escape(); run(p, 0.5)

    # 2. web-server modal (icon index 3)
    click(icon_x(3), ty)
    run(p, 0.8)
    user32.SetCursorPos(*park); run(p, 0.3)
    grab("modal_stream.png")
    escape(); run(p, 0.5)

    # 3. preset editor: enter export page directly, then click the first preset card
    apply_export(p)
    run(p, 1.0)
    shot = ImageGrab.grab(bbox=(rc.left, rc.top, rc.right, rc.bottom)).convert("RGB")
    spx = shot.load()
    sw, sh = shot.size
    card_pt = None
    for y in range(sh - 1, 60, -1):
        run_len = 0
        for x in range(0, int(sw * 0.35)):  # left column only
            if all(abs(spx[x, y][i] - (41, 41, 43)[i]) <= 2 for i in range(3)):
                run_len += 1
                if run_len == 80:
                    card_pt = (x - 40, y)
                    break
            else:
                run_len = 0
        if card_pt: break
    if card_pt:
        click(rc.left + card_pt[0], rc.top + card_pt[1])
    else:
        print("  preset card not found!", file=sys.stderr)
    run(p, 0.8)
    user32.SetCursorPos(*park); run(p, 0.3)
    grab("modal_preset.png")
    escape(); run(p, 0.3)

    p.close()

    print("measurements (physical px, expected pad=18; win_w: url/stream=696, preset=756):")
    for n in ("modal_url.png", "modal_stream.png", "modal_preset.png"):
        measure(os.path.join(OUTDIR, n))


if __name__ == "__main__":
    main()
