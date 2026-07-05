# SPDX-FileCopyrightText: Lada Authors, sumu Authors
# SPDX-License-Identifier: AGPL-3.0
"""Present-cadence distribution analysis for sumu spikes.

Ported from lada-realtime's scripts/analyze_present.py (cold-start vs steady-state
windowing methodology). Adapted to be target-fps aware (4K60 budget = 16.67ms, not
33ms) and to accept two input formats so the SAME analysis applies to the cross-app
baseline (PresentMon) and to our own native present loop:

  * PresentMon CSV  (--format presentmon): reads `msBetweenPresents` + `TimeInSeconds`.
    This is the ground-truth on-screen cadence for ANY process = our pass/fail ruler.
  * raw ns column   (--format ns): one QueryPerformanceCounter-derived nanosecond
    timestamp per present, per line (optional header). Intervals are derived here.

"Even spacing == smooth playback." Steady-state should be single-peaked, median glued
to the frame budget, with %>2x-budget in the low single digits (matching mpv baseline).

Usage:
    python scripts/analyze_present.py <csv> [--format presentmon|ns] [--fps 60]
    python scripts/analyze_present.py <csv> --fps 60 --cold 0 6 --steady 10 end
"""
import argparse
import csv as _csv
import statistics


def _load_presentmon(path):
    """Return list of (time_s, interval_ms). Uses PresentMon msBetweenPresents."""
    rows = []
    with open(path, encoding="utf-8", newline="") as f:
        reader = _csv.DictReader(f)
        # PresentMon column names vary slightly across versions; probe.
        cols = {c.lstrip("﻿").lower(): c for c in (reader.fieldnames or [])}
        # PresentMon 1.x used TimeInSeconds; PresentMon 2.x (Intel.PresentMon.Console)
        # renamed it to TimeInMs (milliseconds) and CPUStartTime to CPUStartTimeInMs.
        t_col = cols.get("timeinseconds") or cols.get("cpustarttime")
        t_col_ms = cols.get("timeinms") or cols.get("cpustarttimeinms")
        iv_col = cols.get("msbetweenpresents") or cols.get("msbetweendisplaychange")
        drop_col = cols.get("dropped") or cols.get("allowstearing")
        if not (t_col or t_col_ms) or not iv_col:
            raise SystemExit(
                f"PresentMon CSV missing expected columns; saw {reader.fieldnames}")
        for r in reader:
            try:
                if drop_col and str(r.get(drop_col, "0")).strip() in ("1", "True", "true"):
                    continue
                t = float(r[t_col]) if t_col else float(r[t_col_ms]) / 1000.0
                iv = float(r[iv_col])
            except (ValueError, KeyError):
                continue
            if iv > 0:
                rows.append((t, iv))
    if rows:  # normalise time origin to 0
        t0 = rows[0][0]
        rows = [(t - t0, iv) for t, iv in rows]
    return rows


def _load_ns(path):
    """Return list of (time_s, interval_ms) from one ns-timestamp per line."""
    ts = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            tok = line.split(",")[0]
            try:
                ts.append(int(tok))
            except ValueError:
                continue  # header line
    if len(ts) < 2:
        return []
    t0 = ts[0]
    out = []
    for i in range(1, len(ts)):
        out.append(((ts[i] - t0) / 1e9, (ts[i] - ts[i - 1]) / 1e6))
    return out


def _window(rows, start_s, end_s):
    hi = float("inf") if end_s is None else end_s
    return [iv for (t, iv) in rows if start_s <= t < hi]


def _pctile(s, q):
    if not s:
        return 0.0
    pos = q * (len(s) - 1)
    lo = int(pos); hi = min(lo + 1, len(s) - 1); frac = pos - lo
    return s[lo] * (1 - frac) + s[hi] * frac


def _report(ivs, budget_ms, label):
    s = sorted(ivs)
    n = len(s)
    print(f"\n  {label}")
    if n == 0:
        print("    (no intervals in window)")
        return
    b15 = 1.5 * budget_ms
    b20 = 2.0 * budget_ms
    late15 = 100.0 * sum(1 for v in s if v > b15) / n
    late20 = 100.0 * sum(1 for v in s if v > b20) / n
    big = sum(1 for v in s if v > 50)
    print(f"    n={n} mean={statistics.fmean(s):.2f} median={statistics.median(s):.2f} "
          f"stddev={statistics.pstdev(s):.2f}  (budget={budget_ms:.2f}ms)")
    print(f"    p50={_pctile(s,.50):.2f} p90={_pctile(s,.90):.2f} p95={_pctile(s,.95):.2f} "
          f"p99={_pctile(s,.99):.2f} max={s[-1]:.1f}")
    print(f"    %>1.5x({b15:.1f}ms)={late15:.1f}  %>2x({b20:.1f}ms)={late20:.1f}  gaps>50ms={big}")
    # histogram centred on the budget
    lo = max(0, int((budget_ms - 8) // 2) * 2)
    hi = int((budget_ms * 2 + 12) // 2) * 2
    bins = {}
    for v in s:
        bb = int(v // 1) if budget_ms < 20 else int(v // 2) * 2
        bins[bb] = bins.get(bb, 0) + 1
    step = 1 if budget_ms < 20 else 2
    for bb in sorted(bins):
        if lo <= bb <= hi:
            bar = "#" * max(1, int(60 * bins[bb] / n))
            print(f"      {bb:>3}-{bb+step:<3}ms {bins[bb]:>5} {100*bins[bb]/n:4.1f}% {bar}")


def _parse_win(vals):
    if not vals:
        return None
    a = float(vals[0])
    b = None if len(vals) < 2 or vals[1] == "end" else float(vals[1])
    return (a, b)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--format", choices=["presentmon", "ns"], default="presentmon")
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--cold", nargs="+", default=["0", "6"])
    ap.add_argument("--steady", nargs="+", default=["10", "end"])
    args = ap.parse_args()

    budget = 1000.0 / args.fps
    rows = _load_presentmon(args.csv) if args.format == "presentmon" else _load_ns(args.csv)
    if not rows:
        raise SystemExit(f"no present intervals parsed from {args.csv}")
    total_s = rows[-1][0]
    print(f"\n########## {args.csv}  ({args.format}, {args.fps:.0f}fps, {total_s:.1f}s, "
          f"{len(rows)} presents) ##########")

    cold = _parse_win(args.cold)
    steady = _parse_win(args.steady)
    if cold:
        _report(_window(rows, cold[0], cold[1]),
                budget, f"COLD-START [{cold[0]:.0f}s-{'end' if cold[1] is None else f'{cold[1]:.0f}s'}]")
    if steady:
        _report(_window(rows, steady[0], steady[1]),
                budget, f"STEADY-STATE [{steady[0]:.0f}s-{'end' if steady[1] is None else f'{steady[1]:.0f}s'}]")
