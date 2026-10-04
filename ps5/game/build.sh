#!/usr/bin/env bash
# Build the game for PS5 as a payload ELF: the recompiled code, the host
# sources that the recompiled code calls into (hooks), the PS5 host
# (main_ps5.cpp) and the runtime's static libraries. Milestone P4: no graphics.
#
# Run on the Arch host after the runtime has been built for PS5 (see
# docs/ps5-port-plan.md) and after codegen has produced generated/default on
# the PC side. Touches no console.
#
# Usage: build.sh [stage ...]     (default: 4)  -> $work/mcla-stage<N>.elf
# Environment: MCLA_RUN_SECONDS, MCLA_LOG_LEVEL, JOBS
#   TITLE=<TITLEID>  build an installable title instead (ps5/title_build.sh):
#                    <driver>/dist/<TITLEID>, one stage at a time
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$here/../.." && pwd)
driver=${PS5_VULKAN:-/root/ps5vk/PS5_Vulkan}
sdk="$driver/.deps/native/ps5-payload-sdk"
runtime_src=${REX_SRC:-/root/mcla/rexglue-sdk}
runtime_build=${REX_BUILD:-/root/mcla/build-ps5}
libs="$runtime_src/out/ps5-amd64"
work=${MCLA_PS5_WORK:-/root/mcla/game-ps5}
jobs=${JOBS:-$(nproc)}
cxx="$sdk/bin/prospero-clang++"
mkdir -p "$work/obj" "$work/src/generated" "$work/src/src" "$work/src/ps5"

# A local copy of the sources: the repository is on a Windows drive here, which
# is slow to read 120 MB from for every compile and has CRLF line endings.
rsync -a --delete "$repo/generated/default/" "$work/src/generated/default/"
rsync -a --delete --include='*.cpp' --include='*.h' --exclude='*' "$repo/src/" "$work/src/src/"
tr -d '\r' < "$here/main_ps5.cpp" > "$work/src/ps5/main_ps5.cpp.new"
cmp -s "$work/src/ps5/main_ps5.cpp.new" "$work/src/ps5/main_ps5.cpp" 2>/dev/null \
    && rm "$work/src/ps5/main_ps5.cpp.new" \
    || mv "$work/src/ps5/main_ps5.cpp.new" "$work/src/ps5/main_ps5.cpp"
for header in title_log.h log_fd_sink.h; do
    tr -d '\r' < "$here/../$header" > "$work/src/ps5/$header"
done

# The runtime's own defines and include paths, taken from its build; its
# precompiled header and float model are not wanted for the game code.
ninja -C "$runtime_build" -t commands rexruntime > "$work/commands.txt"
command=$(grep 'xmemory\.cpp\.o ' "$work/commands.txt" | head -1)
inherited=$(printf '%s' "$command" | tr ' ' '\n' | grep -E '^(--sysroot=|-D|-I)' | grep -v -E '^-DNDEBUG$' | tr '\n' ' ')
inherited_system=$(printf '%s' "$command" | grep -o -E -- '-isystem [^ ]+' | tr '\n' ' ')
# -ffp-contract=off: the desktop build targets SSE4.1 and so never fuses a
# multiply and an add; with znver2 the compiler would, and guest floating
# point results would differ between the two builds.
flags="$inherited $inherited_system -march=znver2 -fexperimental-library -O3 -DNDEBUG -std=c++23 -fPIC \
  -mcmodel=large -fno-strict-aliasing -fno-char8_t -ffp-contract=off -g0 -w \
  -I$work/src -I$work/src/src -I$work/src/generated/default"
# Sources include "generated/default/mcla_init.h" relative to the repository root.
[ -e "$work/src/generated/default/mcla_init.h" ] || { echo "generated/default is missing; run codegen on the PC" >&2; exit 1; }

printf '%s' "$flags" > "$work/flags.new"
if ! cmp -s "$work/flags.new" "$work/flags.txt" 2>/dev/null; then
    rm -f "$work"/obj/*.o "$work"/obj/*.pch
    mv "$work/flags.new" "$work/flags.txt"
fi

pch="$work/obj/mcla_pch.h.pch"
if [ ! -e "$pch" ] || [ -n "$(find "$work/src/generated/default" -name '*.h' -newer "$pch" -print -quit)" ]; then
    echo "precompiling mcla_pch.h"
    ( cd "$runtime_build" && eval "\"$cxx\" $flags -x c++-header -o \"$pch\" -c \"$work/src/generated/default/mcla_pch.h\"" )
    rm -f "$work"/obj/gen_*.o
fi

# One compile per line, run in parallel; only what is out of date.
: > "$work/todo.txt"
for source in "$work"/src/generated/default/*.cpp; do
    object="$work/obj/gen_$(basename "$source" .cpp).o"
    [ "$object" -nt "$source" ] || printf '%s\t%s\t%s\n' "$source" "$object" "-include-pch $pch" >> "$work/todo.txt"
done
# The desktop-only sources are left out: mcla_app.cpp and main.cpp are the
# windowed host, and the code-pointer scan, crash trace and sampling profiler
# are development tools tied to it.
for name in audio_fallback frame_timing render_perf button_prompts settings_menu; do
    source="$work/src/src/$name.cpp"
    object="$work/obj/host_$name.o"
    [ "$object" -nt "$source" ] && [ "$object" -nt "$work/src/src/mcla_app.h" ] || printf '%s\t%s\t%s\n' "$source" "$object" "" >> "$work/todo.txt"
done
count=$(wc -l < "$work/todo.txt")
echo "compiling $count sources with $jobs jobs"
if [ "$count" -gt 0 ]; then
    export cxx flags runtime_build
    compile_one() {
        IFS=$'\t' read -r source object extra <<< "$1"
        ( cd "$runtime_build" && eval "\"$cxx\" $flags $extra -o \"$object\" -c \"$source\"" ) 2> "$object.log" \
            || { echo "FAILED $(basename "$source")"; head -20 "$object.log"; rm -f "$object"; return 1; }
        rm -f "$object.log"
    }
    export -f compile_one
    nice -n 10 xargs -a "$work/todo.txt" -d '\n' -P "$jobs" -I{} bash -c 'compile_one "$1"' _ {} || { echo "compile failed" >&2; exit 1; }
fi

bash "$here/../payload_ld.sh" write "$sdk" "$work/payload.ld"
stages=("$@"); [ ${#stages[@]} -gt 0 ] || stages=(4)
for stage in "${stages[@]}"; do
    extra="-DMCLA_STAGE=$stage"
    [ -z "${MCLA_RUN_SECONDS:-}" ] || extra="$extra -DMCLA_RUN_SECONDS=$MCLA_RUN_SECONDS"
    [ -z "${MCLA_LOG_LEVEL:-}" ] || extra="$extra -DMCLA_LOG_LEVEL=\\\"$MCLA_LOG_LEVEL\\\""
    if [ -n "${TITLE:-}" ]; then
        # As an installable title, linked with the Vulkan driver, log over TCP.
        ( cd "$runtime_build" && eval "\"$cxx\" $flags $extra -DMCLA_TITLE -I\"$work/src/ps5\" -o \"$work/obj/title_stage$stage.o\" -c \"$work/src/ps5/main_ps5.cpp\"" )
        bash "$here/../title_build.sh" "$TITLE" "MCLA Stage $stage" \
            "$work/obj/title_stage$stage.o" "$work"/obj/gen_*.o "$work"/obj/host_*.o \
            --start-group $(ls "$libs"/*.a | tr '\n' ' ') "$sdk/target/lib/libc++experimental.a" --end-group
        continue
    fi
    ( cd "$runtime_build" && eval "\"$cxx\" $flags $extra -I\"$work/src/ps5\" -o \"$work/obj/main_stage$stage.o\" -c \"$work/src/ps5/main_ps5.cpp\"" )
    # The runtime's libraries as one group because they reference each other.
    # nodynamic-undefined-weak: see ps5/probes/arena/build.sh.
    "$cxx" -Wl,-z,nodynamic-undefined-weak -Wl,-T,"$work/payload.ld" \
        -o "$work/mcla-stage$stage.elf" "$work/obj/main_stage$stage.o" "$work"/obj/gen_*.o "$work"/obj/host_*.o \
        -Wl,--start-group $(ls "$libs"/*.a | tr '\n' ' ') "$sdk/target/lib/libc++experimental.a" -Wl,--end-group
    bash "$here/../payload_ld.sh" check "$work/mcla-stage$stage.elf"
    ls -la "$work/mcla-stage$stage.elf"
done
