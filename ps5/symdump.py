#!/usr/bin/env python3
"""Symbolise the thread dumps in a title log against the linked ELF.

symdump.py <log> <elf> [load-base, default 0x400000]
Runs on the Arch host (needs nm).
"""
import bisect
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
    text = symbols[i][1]
    text = re.sub(r'std::__1::', 'std::', text)
    if len(text) > 110:
        text = text[:110] + '...'
    return '%s+0x%x' % (text, offset - symbols[i][0])


for line in open(log, errors='replace'):
    line = line.rstrip('\n')
    m = re.match(r'\s+(pc|stack) 0x([0-9a-f]+)', line)
    if m:
        address = int(m.group(2), 16)
        resolved = name(address)
        if m.group(1) == 'pc':
            print('   pc 0x%x %s' % (address, resolved or '(system library)'))
        elif resolved:
            print('      %s' % resolved)
    elif line.startswith(' thread') or line.startswith('THREAD DUMP') or line.startswith('alive'):
        print(line)
