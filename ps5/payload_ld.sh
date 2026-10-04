#!/usr/bin/env bash
# Write the linker script for a payload that links the runtime, and check a
# linked payload against it.
#
# The runtime (and the game code) is built with the large code model, so nearly
# all code and data is in .ltext/.lrodata/.ldata/.lbss sections. The payload
# SDK's linker script does not name those, and as orphans the code lands in the
# non-executable data segment after .dynamic: the payload then dies on its
# first call, before it can print anything. libc++ puts its replaceable
# operator new in __lcxx_override, with the same result. The script written
# here places all of them with their normal counterparts.
#
#   payload_ld.sh write <sdk> <out.ld>
#   payload_ld.sh check <payload.elf>
set -euo pipefail
case "${1:-}" in
write)
    sdk=$2; out=$3
    sed -E 's/\*\(\.text \.text\.\*\)/*(.text .text.* .ltext .ltext.* __lcxx_override)/;
            s/\*\(\.rodata \.rodata\.\*\)/*(.rodata .rodata.* .lrodata .lrodata.*)/;
            s/\*\(\.data \.data\.\*\)/*(.data .data.* .ldata .ldata.*)/;
            s/\*\(\.bss \.bss\.\*\);/*(.bss .bss.* .lbss .lbss.*);/' \
        "$sdk/target/lib/main.script" > "$out"
    [ "$(grep -c -E '\.ltext|\.lrodata|\.ldata|\.lbss' "$out")" -eq 4 ] || { echo "linker script patterns did not match" >&2; exit 1; }
    ;;
check)
    # Only .text is mapped executable. Any other executable section is code the
    # payload cannot run.
    stray=$(readelf -SW "$2" | sed 's/^ *\[ *[0-9]*\] *//' | awk '$7 ~ /X/ && $1 != ".text" {print $1}')
    [ -z "$stray" ] || { echo "executable sections outside .text: $stray" >&2; exit 1; }
    ;;
*)
    echo "usage: payload_ld.sh write <sdk> <out.ld> | check <payload.elf>" >&2; exit 2 ;;
esac
