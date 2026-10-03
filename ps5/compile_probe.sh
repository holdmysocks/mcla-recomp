#!/usr/bin/env bash
# PS5 go/no-go probe 3: do the ReXGlue runtime and the generated game code
# compile with the PS5 payload SDK's compiler?
#
# Syntax-only: nothing is linked or sent to a console. Run inside the Linux
# host (WSL). Expects the SDK source and the generated code to be reachable
# through /mnt/d; copies them to the Linux filesystem for speed.
#
# Usage: compile_probe.sh [generated|runtime|all] [jobs]
set -u
MODE="${1:-all}"
JOBS="${2:-4}"
SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
SRC=/mnt/d/PS5/MCLA
WORK=/root/mcla
CXX="$SDK/bin/prospero-clang++"

mkdir -p "$WORK"
rsync -a --delete --exclude out --exclude .git "$SRC/third_party/rexglue-sdk/" "$WORK/rexglue-sdk/"
rsync -a --delete "$SRC/generated/" "$WORK/generated/"
cd "$WORK"

R=rexglue-sdk
T=$R/thirdparty
INC="-I $R/include -I . -I generated/default"
for d in fmt/include spdlog/include simde tomlplusplus/include tomlplusplus xxHash utfcpp/source \
         cli11/include imgui disruptorplus/include o1heap/o1heap vulkan-headers/include \
         vulkan-memory-allocator/include sdl3/include stb libmspack FFmpeg; do
    [ -d "$T/$d" ] && INC="$INC -I $T/$d"
done
FLAGS="-std=c++23 -fsyntax-only -march=znver2 -DSPDLOG_FMT_EXTERNAL -DSPDLOG_COMPILED_LIB -DREX_HAS_VULKAN=1 -w"

check() {  # check <label> <files...>
    local label="$1"; shift
    local total=$# out="$WORK/probe-$label.log"
    : > "$out"
    printf '%s\n' "$@" | xargs -P "$JOBS" -I{} sh -c \
        "if $CXX $FLAGS $INC {} >/tmp/probe.\$\$.log 2>&1; then echo OK {}; else echo FAIL {}; grep -m3 -E 'error' /tmp/probe.\$\$.log | sed 's/^/    /'; fi; rm -f /tmp/probe.\$\$.log" >> "$out"
    local ok; ok=$(grep -c '^OK ' "$out")
    echo "== $label: $ok of $total files pass"
    grep -E '^    ' "$out" | sed -E 's/^.*(fatal error|error): //' | sed -E "s/'[^']*'/'X'/g" | sort | uniq -c | sort -rn | head -25
}

if [ "$MODE" = generated ] || [ "$MODE" = all ]; then
    check generated $(ls generated/default/*.cpp)
fi
if [ "$MODE" = runtime ] || [ "$MODE" = all ]; then
    # Portable and POSIX sources only; Windows, macOS and D3D12 files are excluded.
    check runtime $(find $R/src -name '*.cpp' \
        ! -name '*_win.cpp' ! -name '*_mac.cpp' ! -name '*_mac.mm' ! -path '*/d3d12/*' ! -path '*/metal/*' \
        ! -path '*/rexglue/*' ! -path '*/codegen/*' | sort)
fi
