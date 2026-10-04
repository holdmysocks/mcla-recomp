#!/usr/bin/env bash
# Build symcheck.elf: a print-only payload that asks the console which of
# another payload's imports it can actually resolve.
#
# The payload SDK's startup code resolves every import before any of the
# payload's own code runs, and on a miss it aborts with a message that goes
# only to the kernel log. The SDK's stub libraries are generated lists, not the
# console's real exports, so a clean link does not prove the payload can start.
#
# Usage: build.sh <payload.elf>   -> /root/ps5vk/mcla-arena/symcheck.elf
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
target=${1:?payload ELF to check}
sdk=${PS5_PAYLOAD_SDK:-/root/ps5vk/PS5_Vulkan/.deps/native/ps5-payload-sdk}
work=/root/ps5vk/mcla-arena
mkdir -p "$work"
readelf --dyn-syms -W "$target" | awk '$7=="UND" && $8!=""{printf "  {\"%s\", %d},\n", $8, ($4=="OBJECT")}' > "$work/symcheck_names.inc"
echo "names: $(wc -l < "$work/symcheck_names.inc")"
tr -d '\r' < "$here/main.c" > "$work/symcheck.c"
"$sdk/bin/prospero-clang" -O1 -I"$work" -o "$work/symcheck.elf" "$work/symcheck.c"
ls -la "$work/symcheck.elf"
