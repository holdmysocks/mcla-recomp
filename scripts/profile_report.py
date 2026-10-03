#!/usr/bin/env python3
"""Summarise a sampling profile written with --mcla_profile.

Usage: profile_report.py <profile.txt> [--bin DIR] [--symbolizer PATH] [--top N] [--threads N]

Samples in recompiled game code are already named by guest function. Samples
in DLLs are module+offset; they are resolved with llvm-symbolizer against the
DLLs (and their PDBs) in --bin.
"""
import argparse
import collections
import os
import subprocess


def symbolize(symbolizer, binary, offsets):
    if not (symbolizer and os.path.exists(binary)):
        return {}
    text = "\n".join(hex(o) for o in offsets) + "\n"
    try:
        out = subprocess.run([symbolizer, f"--obj={binary}", "--relative-address", "--functions=short",
                              "--no-inlines", "--demangle"],
                             input=text, capture_output=True, text=True, timeout=600).stdout
    except (OSError, subprocess.TimeoutExpired):
        return {}
    names = {}
    blocks = out.strip().split("\n\n")
    for offset, block in zip(offsets, blocks):
        first = block.strip().splitlines()[0] if block.strip() else "??"
        names[offset] = first
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profile")
    ap.add_argument("--bin", default=None)
    ap.add_argument("--symbolizer", default=None)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--threads", type=int, default=3)
    args = ap.parse_args()

    per_thread = collections.defaultdict(collections.Counter)
    header = ""
    by_module = collections.defaultdict(set)
    with open(args.profile) as f:
        for line in f:
            if line.startswith("#"):
                header = line.strip()
                continue
            tid, n, where = line.rstrip("\n").split("\t")
            per_thread[int(tid)][where] += int(n)
            if where.startswith("module "):
                _, mod, off = where.split(" ")
                by_module[mod].add(int(off, 16))

    resolved = {}
    for mod, offsets in by_module.items():
        if args.bin:
            names = symbolize(args.symbolizer, os.path.join(args.bin, mod), sorted(offsets))
            for off, name in names.items():
                if name and name != "??":
                    resolved[(mod, off)] = name

    def label(where):
        if where.startswith("module "):
            _, mod, off = where.split(" ")
            name = resolved.get((mod, int(off, 16)))
            return f"{mod}!{name}" if name else f"{mod} (unresolved)"
        return where

    print(header)
    rounds = int(header.split("rounds=")[1].split()[0]) if "rounds=" in header else 0
    # Waiting threads sit in ntdll; rank threads by samples outside it.
    def busy(counter):
        return sum(n for w, n in counter.items() if "ntdll.dll" not in w and "win32u.dll" not in w)

    ranked = sorted(per_thread.items(), key=lambda kv: -busy(kv[1]))
    for tid, counter in ranked[:args.threads]:
        total = sum(counter.values())
        merged = collections.Counter()
        for where, n in counter.items():
            merged[label(where)] += n
        active = busy(counter)
        share = f"{100 * active / rounds:.0f}% of wall time running" if rounds else ""
        print(f"\nthread {tid}: {total} samples, {share}")
        kinds = collections.Counter()
        for where, n in merged.items():
            kinds[where.split("!")[0].split(" ")[0] if not where.startswith("guest") else "guest code"] += n
        print("  by module: " + ", ".join(f"{k} {100 * v / total:.0f}%" for k, v in kinds.most_common(6)))
        for where, n in merged.most_common(args.top):
            print(f"  {100 * n / total:5.1f}%  {where[:110]}")


if __name__ == "__main__":
    main()
