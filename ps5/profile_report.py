#!/usr/bin/env python3
"""Turn the PS5 host's sampling profile into per-thread function counts.

The host (ps5/game/main_ps5.cpp, built with -DMCLA_PROFILE_AT=<second>) signals
every runtime thread 25 times a second for 20 s; each sample is one line in the
title log:

    S <thread id> <pc> <code address found on the stack> ...

Addresses are symbolised against the linked ELF. For each thread this prints
how many samples it had, the functions it was most often *in* (the first
address that falls inside the executable: the pc, or when the pc is in a
system library, the caller found on the stack), and the functions most often
*on the stack* (each counted once per sample).

    profile_report.py <title log> <linked elf> [load base, default 0x400000]

Runs on the build host (needs nm and readelf).
"""
import bisect
import collections
import re
import subprocess
import sys

log, elf = sys.argv[1], sys.argv[2]
base = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x400000

symbols = []
text_end = 0
for line in subprocess.run(['nm', '-n', '-C', elf], capture_output=True, text=True).stdout.splitlines():
    parts = line.split(' ', 2)
    if len(parts) == 3 and parts[0] and parts[1] in 'TtWw':
        symbols.append((int(parts[0], 16), parts[2]))
for line in subprocess.run(['readelf', '-SW', elf], capture_output=True, text=True).stdout.splitlines():
    m = re.search(r'\] \.text\s+PROGBITS\s+([0-9a-f]+) [0-9a-f]+ ([0-9a-f]+)', line)
    if m:
        text_end = int(m.group(1), 16) + int(m.group(2), 16)
addresses = [a for a, _ in symbols]


def name(address):
    offset = address - base
    if offset < 0 or offset >= text_end:
        return None
    i = bisect.bisect_right(addresses, offset) - 1
    if i < 0:
        return None
    text = re.sub(r'std::__1::', 'std::', symbols[i][1])
    text = re.sub(r'\(.*', '', text) if len(text) > 90 else text
    return text[:110]


samples = collections.defaultdict(int)
leaf = collections.defaultdict(collections.Counter)
inclusive = collections.defaultdict(collections.Counter)
in_system = collections.defaultdict(int)
for line in open(log, errors='replace'):
    if not line.startswith('S '):
        continue
    fields = line.split()
    if len(fields) < 3:
        continue
    try:
        thread = fields[1]
        frames = [int(x, 16) for x in fields[2:]]
    except ValueError:
        continue
    samples[thread] += 1
    names = [name(a) for a in frames]
    if names[0] is None:
        in_system[thread] += 1
    seen = set()
    first = None
    for n in names:
        if n is None:
            continue
        if first is None:
            first = n
        if n not in seen:
            seen.add(n)
            inclusive[thread][n] += 1
    leaf[thread][first or '(no executable address in the sample)'] += 1

total = sum(samples.values())
print('samples: %d over %d threads' % (total, len(samples)))
for thread in sorted(samples, key=lambda t: -samples[t]):
    count = samples[thread]
    top, top_count = leaf[thread].most_common(1)[0]
    print()
    print('thread %s: %d samples, %d%% with the pc in a system library' % (
        thread, count, 100 * in_system[thread] // count))
    print('  most often in:')
    for function, n in leaf[thread].most_common(10):
        print('    %5.1f%%  %s' % (100.0 * n / count, function))
    print('  most often on the stack:')
    for function, n in inclusive[thread].most_common(10):
        print('    %5.1f%%  %s' % (100.0 * n / count, function))
