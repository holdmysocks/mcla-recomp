#!/usr/bin/env python3
"""Steady-state statistics for per-frame CSVs: the last N seconds of each file.

Usage: sweep_stats.py [seconds] <file.csv>...
"""
import csv
import sys


def main():
    args = sys.argv[1:]
    seconds = 15.0
    if args and args[0].replace(".", "").isdigit():
        seconds = float(args.pop(0))
    for path in args:
        with open(path, newline="") as f:
            rows = list(csv.DictReader(f))
        tail, t = [], 0.0
        for r in reversed(rows):
            if t > seconds * 1000:
                break
            tail.append(r)
            t += float(r["frame_time_us"]) / 1000
        n = len(tail)
        if n < 5:
            print(f"{path}: too few frames")
            continue
        ft = sorted(float(r["frame_time_us"]) / 1000 for r in tail)

        def med(key):
            return sorted(float(r[key]) for r in tail)[n // 2]

        print(f"{path}: {n} frames in last {t / 1000:.0f} s, mean {t / n:.2f} ms ({1000 * n / t:.1f} FPS), "
              f"median {ft[n // 2]:.2f}, p5 {ft[int(n * .05)]:.2f}, p95 {ft[int(n * .95)]:.2f}, max {ft[-1]:.1f}")
        print(f"    draws {med('draw_calls'):.0f}  irq {med('interrupt_dispatches'):.0f}  "
              f"contention {med('critical_region_contentions'):.0f}  stalls {med('command_buffer_stalls'):.0f}  "
              f"verts {med('vertices_processed'):.0f}  threads {med('active_threads'):.0f}")


if __name__ == "__main__":
    main()
