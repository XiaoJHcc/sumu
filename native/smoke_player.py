# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Verification driver for native/src/player.cpp's Player class (module sumu_core), covering
# the three scenarios required before promoting spike2's kernel into sumu proper:
#
#   1. smoothness  -- 50s playback of test_video_4k.mp4, reproducing spike0/2's single-peaked
#                      ~16.68ms present cadence (checked afterwards via scripts/analyze_present.py).
#   2. seek        -- 20+ seeks on test_video_long.mp4 (2.1GB, ~2h45m) mixing shallow/deep/
#                      near-tail jumps (10%/90%/50%/99%/30% of total frames, repeated),
#                      measuring seek() call latency, confirming the present-thread heartbeat
#                      (present_count) keeps advancing through every seek (never freezes), and
#                      taking a best-effort full-desktop-screenshot mean-luma sample right
#                      before/after each seek as a black-screen tripwire (see screen_mean_luma()
#                      docstring for this technique's real limitations -- it is NOT a substitute
#                      for an actual visual check, only a coarse automated smoke signal).
#   3. pause       -- play() -> pause() -> verify current_frame() freezes exactly -> play()
#                      again -> verify it resumes advancing.
#
# Usage:
#   d:/Git/sumu/.venv/Scripts/python.exe native/smoke_player.py smoothness --seconds 50
#   d:/Git/sumu/.venv/Scripts/python.exe native/smoke_player.py seek --rounds 4
#   d:/Git/sumu/.venv/Scripts/python.exe native/smoke_player.py pause
#   d:/Git/sumu/.venv/Scripts/python.exe native/smoke_player.py all

import argparse
import ctypes
import json
import os
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python", "sumu"))
import sumu_core  # noqa: E402

TRACE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "trace")
os.makedirs(TRACE_DIR, exist_ok=True)

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


def screen_mean_luma():
    """Coarse black-screen tripwire: BitBlt the WHOLE desktop (not just our window -- Player
    doesn't expose its HWND over pybind11, and this needs no new native surface to add that
    risk) into a 32bpp DIB and return the mean byte value. Meaningful only when our window
    covers most of the screen (smoothness/seek runs below open it maximized) -- taskbar/other
    chrome contributes a small, roughly constant bias. A near-zero mean is strong evidence of
    an actual black screen; a healthy mean is NOT proof the video specifically is showing
    (could be desktop chrome), so this is a tripwire to catch gross failures, not a
    replacement for an eyes-on check.
    """
    hdesktop = user32.GetDesktopWindow()
    w = user32.GetSystemMetrics(0)
    h = user32.GetSystemMetrics(1)
    hwin_dc = user32.GetWindowDC(hdesktop)
    hmem_dc = gdi32.CreateCompatibleDC(hwin_dc)
    hbmp = gdi32.CreateCompatibleBitmap(hwin_dc, w, h)
    gdi32.SelectObject(hmem_dc, hbmp)
    SRCCOPY = 0x00CC0020
    gdi32.BitBlt(hmem_dc, 0, 0, w, h, hwin_dc, 0, 0, SRCCOPY)

    bmi = _BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
    bmi.biWidth = w
    bmi.biHeight = -h  # negative = top-down DIB
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0  # BI_RGB
    buf = (ctypes.c_ubyte * (w * h * 4))()
    gdi32.GetDIBits(hmem_dc, hbmp, 0, h, buf, ctypes.byref(bmi), 0)

    gdi32.DeleteObject(hbmp)
    gdi32.DeleteDC(hmem_dc)
    user32.ReleaseDC(hdesktop, hwin_dc)

    total = 0
    n = len(buf)
    step = 997 * 4  # sparse-sample every ~997th pixel (4 bytes/px) -- full sum over a 4K/8K
                     # desktop in pure python ctypes is too slow to call every seek; this is
                     # plenty of samples (tens of thousands) for a mean-luma tripwire.
    count = 0
    for i in range(0, n, step):
        total += buf[i]
        count += 1
    return total / count if count else 0.0


def run_smoothness(video, seconds, maximized=True, width=3840, height=2160):
    trace_path = os.path.join(TRACE_DIR, "present_smoothness.csv")
    p = sumu_core.Player(width, height, maximized)
    p.open(video)
    print(f"[smoothness] opened {video} fps={p.fps():.4f} frames={p.frame_count()} dims={p.dims()}",
          file=sys.stderr)
    p.play()
    t0 = time.perf_counter()
    quit_early = False
    while time.perf_counter() - t0 < seconds:
        p.pump_messages()
        if p.should_quit():
            quit_early = True
            break
        time.sleep(0.02)
    p.pump_messages()
    stats = p.stats()
    print(f"[smoothness] stats={json.dumps(stats)}", file=sys.stderr)
    p.dump_present_trace(trace_path)
    p.close()
    return {"quit_early": quit_early, "stats": stats, "trace_path": trace_path}


def run_pause_play(video, width=1280, height=720):
    p = sumu_core.Player(width, height, False)
    p.open(video)
    p.play()
    time.sleep(2.0)
    p.pump_messages()
    f_before_pause = p.current_frame()

    p.pause()
    playing_after_pause = p.is_playing()
    time.sleep(1.0)
    p.pump_messages()
    f_during_pause = p.current_frame()
    frozen_ok = (f_during_pause == f_before_pause) and (not playing_after_pause)
    pc_before_resume = p.stats()["present_count"]

    p.play()
    playing_after_resume = p.is_playing()
    time.sleep(1.0)
    p.pump_messages()
    f_after_resume = p.current_frame()
    pc_after_resume = p.stats()["present_count"]
    resumed_ok = playing_after_resume and (f_after_resume > f_during_pause) and (pc_after_resume > pc_before_resume)

    result = {
        "f_before_pause": f_before_pause,
        "f_during_pause": f_during_pause,
        "frozen_ok": frozen_ok,
        "f_after_resume": f_after_resume,
        "resumed_ok": resumed_ok,
        "quit_early": p.should_quit(),
    }
    print(f"[pause] {json.dumps(result)}", file=sys.stderr)
    p.close()
    return result


def run_seek_stress(video, rounds, width=1920, height=1080):
    trace_path = os.path.join(TRACE_DIR, "present_seek_stress.csv")
    p = sumu_core.Player(width, height, True)
    p.open(video)
    fc = p.frame_count()
    fps = p.fps()
    print(f"[seek] opened {video} fps={fps:.4f} frames={fc} dims={p.dims()}", file=sys.stderr)
    p.play()
    time.sleep(0.5)
    p.pump_messages()

    fractions = [0.10, 0.90, 0.50, 0.99, 0.30]
    results = []
    crashed = False
    frozen = False
    quit_early = False

    for round_i in range(rounds):
        for frac in fractions:
            target = int(fc * frac)
            luma_before = screen_mean_luma()
            pc_before = p.stats()["present_count"]
            t0 = time.perf_counter()
            actual = None
            err = None
            try:
                actual = p.seek(target)
            except Exception as e:  # noqa: BLE001
                err = repr(e)
                crashed = True
            t1 = time.perf_counter()

            time.sleep(0.15)  # let a handful of present ticks land post-seek
            p.pump_messages()
            pc_after = p.stats()["present_count"]
            luma_after = screen_mean_luma()
            cur = p.current_frame()
            ticks_during = pc_after - pc_before
            if ticks_during <= 0 and err is None:
                frozen = True

            rec = {
                "round": round_i, "frac": frac, "target": target, "actual": actual, "err": err,
                "latency_ms": (t1 - t0) * 1000.0, "ticks_during": ticks_during,
                "luma_before": luma_before, "luma_after": luma_after,
                "current_frame_after": cur,
            }
            results.append(rec)
            print(f"[seek] round={round_i} frac={frac:.2f} target={target} actual={actual} "
                  f"latency_ms={rec['latency_ms']:.2f} ticks_during={ticks_during} "
                  f"luma_before={luma_before:.1f} luma_after={luma_after:.1f} err={err}",
                  file=sys.stderr)

            if p.should_quit():
                quit_early = True
                break
            if err is not None:
                break
        if quit_early or crashed:
            break

    p.pump_messages()
    stats = p.stats()
    try:
        p.dump_present_trace(trace_path)
    except Exception as e:  # noqa: BLE001
        print(f"[seek] dump_present_trace failed: {e!r}", file=sys.stderr)
    p.close()

    summary = {
        "n_seeks": len(results), "crashed": crashed, "frozen": frozen, "quit_early": quit_early,
        "stats": stats, "trace_path": trace_path, "seeks": results,
    }
    print(f"[seek] SUMMARY crashed={crashed} frozen={frozen} quit_early={quit_early} "
          f"n_seeks={len(results)}", file=sys.stderr)
    return summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario", choices=["smoothness", "seek", "pause", "all"])
    ap.add_argument("--video4k", default="d:/Git/sumu/test_video_4k.mp4")
    ap.add_argument("--videolong", default="d:/Git/sumu/test_video_long.mp4")
    ap.add_argument("--seconds", type=float, default=50.0)
    ap.add_argument("--rounds", type=int, default=4)
    ap.add_argument("--out", default=os.path.join(TRACE_DIR, "smoke_result.json"))
    args = ap.parse_args()

    out = {}
    if args.scenario in ("smoothness", "all"):
        out["smoothness"] = run_smoothness(args.video4k, args.seconds)
    if args.scenario in ("pause", "all"):
        out["pause"] = run_pause_play(args.video4k)
    if args.scenario in ("seek", "all"):
        out["seek"] = run_seek_stress(args.videolong, args.rounds)

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)
    print(f"wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
