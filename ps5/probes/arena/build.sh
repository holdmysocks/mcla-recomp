#!/usr/bin/env bash
# Build the mcla-arena step payloads: the PS5 build of the ReXGlue runtime
# linked into plain payload ELFs, one per step (see main.cpp).
#
# Run on the Arch host after the runtime has been built for PS5 (see
# docs/ps5-port-plan.md). Output: /root/ps5vk/mcla-arena/arena-step<N>.elf.
# Touches no console; sending a payload is a separate, deliberate action.
#
# Usage: build.sh [steps...]   (default: 1 2 3 4 5 6 7)
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
driver=${PS5_VULKAN:-/root/ps5vk/PS5_Vulkan}
sdk="$driver/.deps/native/ps5-payload-sdk"
runtime_src=${REX_SRC:-/root/mcla/rexglue-sdk}
runtime_build=${REX_BUILD:-/root/mcla/build-ps5}
libs="$runtime_src/out/ps5-amd64"
work=/root/ps5vk/mcla-arena
mkdir -p "$work"
tr -d '\r' < "$here/main.cpp" > "$work/main.cpp"

# Compile with exactly the flags the runtime's own sources were built with.
ninja -C "$runtime_build" -t commands rexruntime > "$work/commands.txt"
command=$(grep 'xmemory\.cpp\.o ' "$work/commands.txt" | head -1)
flags=$(printf '%s' "$command" | sed -E 's/^.*prospero-clang\+\+ //; s/ -o [^ ]+\.o -c [^ ]+$//; s/ -MD -MT [^ ]+ -MF [^ ]+//')

# The runtime is built with the large code model, so nearly all of its code and
# data is in .ltext/.lrodata/.ldata/.lbss sections. The SDK's linker script
# does not name those, and as orphans the code lands in the non-executable data
# segment after .dynamic: the payload then dies on its first call, before it
# can print anything. Derive a script that places them with their normal
# counterparts.
script="$work/payload.ld"
# libc++ puts its replaceable operator new in __lcxx_override; same problem.
sed -E 's/\*\(\.text \.text\.\*\)/*(.text .text.* .ltext .ltext.* __lcxx_override)/;
        s/\*\(\.rodata \.rodata\.\*\)/*(.rodata .rodata.* .lrodata .lrodata.*)/;
        s/\*\(\.data \.data\.\*\)/*(.data .data.* .ldata .ldata.*)/;
        s/\*\(\.bss \.bss\.\*\);/*(.bss .bss.* .lbss .lbss.*);/' \
    "$sdk/target/lib/main.script" > "$script"
[ "$(grep -c -E '\.ltext|\.lrodata|\.ldata|\.lbss' "$script")" -eq 4 ] || { echo "linker script patterns did not match" >&2; exit 1; }

steps=("$@"); [ ${#steps[@]} -gt 0 ] || steps=(1 2 3 4 5 6 7)
for step in "${steps[@]}"; do
    ( cd "$runtime_build" && eval "\"$sdk/bin/prospero-clang++\" $flags -DMCLA_STEP=$step -o \"$work/step$step.o\" -c \"$work/main.cpp\"" )
    # A plain payload link: the SDK's own startup code and libraries, and the
    # runtime's static libraries as one group because they reference each other.
    # nodynamic-undefined-weak: the C++ runtime weakly references
    # __cxa_thread_atexit_impl, which no console library exports. Left as a
    # dynamic import it makes the ELF loader reject the payload before any code
    # runs (it binds every import up front); resolved to null at link time, the
    # runtime uses its own fallback.
    "$sdk/bin/prospero-clang++" -Wl,-z,nodynamic-undefined-weak -Wl,-T,"$script" \
        -o "$work/arena-step$step.elf" "$work/step$step.o" \
        -Wl,--start-group $(ls "$libs"/*.a | tr '\n' ' ') "$sdk/target/lib/libc++experimental.a" -Wl,--end-group
    ls -la "$work/arena-step$step.elf"
    # Only .text is mapped executable. Any other executable section means code
    # the payload cannot run.
    stray=$(readelf -SW "$work/arena-step$step.elf" | sed 's/^ *\[ *[0-9]*\] *//' | awk '$7 ~ /X/ && $1 != ".text" {print $1}')
    [ -z "$stray" ] || { echo "executable sections outside .text: $stray" >&2; exit 1; }
done
