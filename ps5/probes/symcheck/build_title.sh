#!/usr/bin/env bash
# Build the title form of symcheck (title_main.cpp): a print-only title that
# reports which of another ELF's imports are null inside a title.
#
# Usage: build_title.sh <TITLEID> <elf whose imports to check> [more ELFs...]
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
title_id=${1:?title id}; shift
[ $# -gt 0 ] || { echo "no ELF given" >&2; exit 2; }
driver=${PS5_VULKAN:-/root/ps5vk/PS5_Vulkan}
sdk="$driver/.deps/native/ps5-payload-sdk"
work=/root/ps5vk/mcla-symcheck-title
mkdir -p "$work"
for elf in "$@"; do
    readelf --dyn-syms -W "$elf" | awk '$7=="UND" && $8!=""{print $8}'
done | sed 's/@.*//' | sort -u | awk '{printf "X(%d, %s)\n", NR, $1}' > "$work/title_names.inc"
echo "names: $(wc -l < "$work/title_names.inc")"
tr -d '\r' < "$here/title_main.cpp" > "$work/title_main.cpp"
tr -d '\r' < "$here/../../title_log.h" > "$work/title_log.h"
"$sdk/bin/prospero-clang++" -std=c++20 -O1 -fPIC -DMCLA_TITLE -I"$work" -c "$work/title_main.cpp" -o "$work/title_main.o"
bash "$here/../../title_build.sh" "$title_id" "MCLA Import Check" "$work/title_main.o"
