# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
"""Switch-point hitch analysis for spike 2's present trace.

Reads the qpc_ns,source,frame_num CSV written by RealtimePresenter::dump_trace() and, in
addition to the ordinary present-interval distribution (see ../../scripts/analyze_present.py
for the shared version of that analysis), specifically separates present intervals into:

  - "switch" intervals: the present tick where `source` differs from the previous tick's
    source (i.e. AI->passthrough or passthrough->AI, fresh or stale variants all count as
    their AI/passthrough family) -- this is where a hitch would show up if the ready-map/
    ring-buffer handoff weren't actually seamless.
  - "non-switch" intervals: everything else.

If spike 2's mental model holds, these two populations should look statistically the same
(same median, no fat tail of large intervals concentrated at switch points).

Usage:
    python analyze_switches.py trace/present_spike2_mixed.csv --fps 60
"""
import argparse
import csv
import statistics


SOURCE_NAMES = {0: "pt_fresh", 1: "ai_fresh", 2: "pt_stale", 3: "ai_stale"}


def is_ai(source):
    return source in (1, 3)


def load(path):
    rows = []
    with open(path, encoding="utf-8", newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        for r in reader:
            if len(r) < 3:
                continue
            try:
                qpc_ns = int(r[0])
                source = int(r[1])
                frame_num = int(r[2])
            except ValueError:
                continue
            rows.append((qpc_ns, source, frame_num))
    return rows


def pctile(s, q):
    if not s:
        return 0.0
    pos = q * (len(s) - 1)
    lo = int(pos)
    hi = min(lo + 1, len(s) - 1)
    frac = pos - lo
    return s[lo] * (1 - frac) + s[hi] * frac


def report(ivs, budget_ms, label):
    s = sorted(ivs)
    n = len(s)
    print(f"\n  {label}")
    if n == 0:
        print("    (no intervals)")
        return
    b15 = 1.5 * budget_ms
    b20 = 2.0 * budget_ms
    late15 = 100.0 * sum(1 for v in s if v > b15) / n
    late20 = 100.0 * sum(1 for v in s if v > b20) / n
    big = sum(1 for v in s if v > 50)
    print(f"    n={n} mean={statistics.fmean(s):.2f} median={statistics.median(s):.2f} "
          f"stddev={statistics.pstdev(s):.2f} (budget={budget_ms:.2f}ms)")
    print(f"    p50={pctile(s,.50):.2f} p90={pctile(s,.90):.2f} p95={pctile(s,.95):.2f} "
          f"p99={pctile(s,.99):.2f} max={s[-1]:.1f}")
    print(f"    %>1.5x({b15:.1f}ms)={late15:.1f}  %>2x({b20:.1f}ms)={late20:.1f}  gaps>50ms={big}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--steady-start-s", type=float, default=5.0,
                     help="skip this many seconds of cold-start / ramp-up before analysis")
    args = ap.parse_args()

    budget = 1000.0 / args.fps
    rows = load(args.csv)
    if len(rows) < 3:
        raise SystemExit(f"not enough rows parsed from {args.csv}")

    t0_ns = rows[0][0]
    steady_start_ns = t0_ns + int(args.steady_start_s * 1e9)

    switch_ivs = []
    non_switch_ivs = []
    counts = {0: 0, 1: 0, 2: 0, 3: 0}
    n_switches = 0

    for i in range(1, len(rows)):
        qpc_prev, src_prev, _ = rows[i - 1]
        qpc_cur, src_cur, _ = rows[i]
        counts[src_cur] = counts.get(src_cur, 0) + 1
        if qpc_cur < steady_start_ns:
            continue
        iv_ms = (qpc_cur - qpc_prev) / 1e6
        if is_ai(src_prev) != is_ai(src_cur):
            switch_ivs.append(iv_ms)
            n_switches += 1
        else:
            non_switch_ivs.append(iv_ms)

    total = sum(counts.values())
    print(f"\n########## {args.csv} ({args.fps:.0f}fps, {len(rows)} presents) ##########")
    print("\n  source breakdown (whole run):")
    for k in sorted(counts):
        print(f"    {SOURCE_NAMES.get(k, k):10s} n={counts[k]:6d} ({100.0*counts[k]/total:.1f}%)")
    ai_hit_rate = counts.get(1, 0) / total if total else 0.0
    print(f"    ai_hit_rate (fresh AI / total) = {ai_hit_rate:.4f}")
    print(f"    switch points (steady window)  = {n_switches}")

    report(non_switch_ivs, budget, f"NON-SWITCH intervals [{args.steady_start_s:.0f}s-end]")
    report(switch_ivs, budget, f"SWITCH intervals [{args.steady_start_s:.0f}s-end] (AI<->passthrough handoff)")
    report(non_switch_ivs + switch_ivs, budget, f"ALL intervals [{args.steady_start_s:.0f}s-end]")
