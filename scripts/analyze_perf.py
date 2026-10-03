#!/usr/bin/env python3
"""Summarise a per-frame counter CSV written with --mcla_perf_csv.

Usage: analyze_perf.py <perf.csv> [slow_frame_ms]

Prints overall percentiles, then compares slow frames against normal ones so
the counters that move with slowdowns stand out, and lists slow stretches.
"""
import csv
import statistics
import sys


def pct(values, p):
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p))] if s else 0


def main():
    path = sys.argv[1]
    slow_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0
    with open(path, newline="") as f:
        rows = [{k: float(v) for k, v in r.items()} for r in csv.DictReader(f)]
    if not rows:
        raise SystemExit("no frames")
    cols = list(rows[0].keys())
    ft = [r["frame_time_us"] / 1000.0 for r in rows]
    total_s = sum(ft) / 1000.0
    print(f"{len(rows)} frames, {total_s / 60:.1f} min of frame time")
    print(f"frame time ms: median {pct(ft, .5):.1f}  p95 {pct(ft, .95):.1f}  p99 {pct(ft, .99):.1f}  max {max(ft):.1f}")

    slow = [r for r in rows if r["frame_time_us"] / 1000.0 > slow_ms]
    fast = [r for r in rows if r["frame_time_us"] / 1000.0 <= slow_ms]
    slow_s = sum(r["frame_time_us"] for r in slow) / 1e6
    print(f"frames over {slow_ms:.0f} ms: {len(slow)} ({100 * len(slow) / len(rows):.1f}%), "
          f"{slow_s:.0f} s ({100 * slow_s / total_s:.0f}% of time)")
    if slow and fast:
        print(f"\n{'counter':30} {'normal median':>14} {'slow median':>12}")
        for c in cols:
            a = statistics.median(r[c] for r in fast)
            b = statistics.median(r[c] for r in slow)
            flag = "  <--" if (a or b) and (b > 1.5 * a + 1 or a > 1.5 * b + 1) else ""
            print(f"{c:30} {a:14.0f} {b:12.0f}{flag}")

    # Slow stretches: runs of slow frames, merged across gaps under one second.
    print("\nslow stretches (start time, duration, frames, worst ms, median interrupts, median pipeline misses):")
    t = 0.0
    stretches = []
    cur = None
    for r in rows:
        ms = r["frame_time_us"] / 1000.0
        if ms > slow_ms:
            if cur and t - cur["end"] < 1.0:
                cur["rows"].append(r)
            else:
                cur = {"start": t, "rows": [r]}
                stretches.append(cur)
            cur["end"] = t + ms / 1000.0
        t += ms / 1000.0
    stretches.sort(key=lambda s: s["start"] - s["end"])
    for s in stretches[:15]:
        rs = s["rows"]
        print(f"  {s['start'] / 60:6.2f} min  {s['end'] - s['start']:6.1f} s  {len(rs):5d} frames  "
              f"worst {max(x['frame_time_us'] for x in rs) / 1000:7.1f}  "
              f"irq {statistics.median(x.get('interrupt_dispatches', 0) for x in rs):7.0f}  "
              f"pipe {statistics.median(x.get('pipeline_cache_misses', 0) for x in rs):4.0f}")


if __name__ == "__main__":
    main()
