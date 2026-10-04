#!/usr/bin/env bash
# One command from your own disc image to the game installed on a PS5.
#
#   sudo bash ps5/make_ps5.sh --iso /path/to/your.iso [--console 192.168.1.50]
#
# Host: Arch Linux, as root (a fresh Arch under WSL2 on Windows is the tested
# setup; docs/ps5-build-guide.md walks through it). Everything is built on
# your machine from your own copy of the game: nothing of the game is in this
# repository, and what this script produces from it is yours to keep, not to
# share.
#
# Every step is skipped when its result is already there, so the script can be
# run again after a failure, or after a change, and continues from that step.
#
#   --iso PATH        your disc image (Midnight Club: Los Angeles Complete
#                     Edition, USA/Europe, Xbox 360). Not needed once game/
#                     has been extracted.
#   --console IP      upload the title and the game data to the console (FTP)
#   --ftp-port N      the console's FTP port (default 2121)
#   --title-id ID     the title's id on the console (default PPSA99779)
#   --tile IMAGE      your own picture for the home-screen tile (any common
#                     format; it is resized to 512x512)
#   --theme FILE.at9  your own home-screen theme (ATRAC9, 48 kHz stereo)
#   --art-dir DIR     your own tile, backgrounds and theme in the console's
#                     formats (icon0.png, pic0.dds, pic1.dds, snd0.at9)
#                     instead of the art made from the disc
#   --test-build      keep the test scaffolding (waits for a log connection)
#   --jobs N          parallel compile jobs (default: all cores)
#
# Locations, changeable through the environment:
#   PS5VK   (/root/ps5vk)   the Vulkan driver project and its toolchain
#   WORK    (/root/mcla)    the SDK checkout and all build trees
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$here/.." && pwd)
PS5VK=${PS5VK:-/root/ps5vk}
WORK=${WORK:-/root/mcla}
driver="$PS5VK/PS5_Vulkan"
sdk="$driver/.deps/native/ps5-payload-sdk"
rex_src="$WORK/rexglue-sdk"
rex_build="$WORK/build-ps5"
rex_host="$WORK/build-host"
rex_tag=v0.10.0
rex_commit=f5337cdc947ff6d4c4196737e2c807a48f2a1fc2

iso= console= ftp_port=2121 title_id=PPSA99779 art_dir= tile= theme= play=1 jobs=$(nproc)
while [ $# -gt 0 ]; do
    case $1 in
        --iso) iso=$2; shift 2 ;;
        --console) console=$2; shift 2 ;;
        --ftp-port) ftp_port=$2; shift 2 ;;
        --title-id) title_id=$2; shift 2 ;;
        --art-dir) art_dir=$2; shift 2 ;;
        --tile) tile=$2; shift 2 ;;
        --theme) theme=$2; shift 2 ;;
        --test-build) play=; shift ;;
        --jobs) jobs=$2; shift 2 ;;
        -h|--help) sed -n '2,34p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done

step() { printf '\n==> %s\n' "$*"; }
fail() { printf '\nFAILED: %s\n' "$*" >&2; exit 1; }
mkdir -p "$WORK"
logs="$WORK/logs"; mkdir -p "$logs"
# Run a long command with its output in a log; on failure show the end of it.
logged() {
    local name=$1; shift
    if ! "$@" > "$logs/$name.log" 2>&1; then
        tail -25 "$logs/$name.log" >&2
        fail "$name (full log: $logs/$name.log)"
    fi
}

[ "$(id -u)" -eq 0 ] || fail "run as root (the driver project's build installs packages and writes under /root)"
command -v pacman > /dev/null || fail "this script needs Arch Linux (pacman); see docs/ps5-build-guide.md"

# ---------------------------------------------------------------------------
step "1/9 host packages"
# The X11 packages are for the recompiler's own build: it is a command-line
# tool, but the SDK configures its window library along with everything else.
logged packages pacman -Sy --noconfirm --needed base-devel clang llvm lld cmake ninja git curl rsync python \
    python-pillow ffmpeg unzip libx11 libxext libxi libxcursor libxrandr libxinerama libxss libxkbcommon \
    libxtst libxfixes libxrender mesa

# ---------------------------------------------------------------------------
step "2/9 PS5 toolchain and Vulkan driver (about 20 minutes the first time)"
if [ -f "$driver/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" ] && [ -x "$driver/build/host/ps5-native-tool" ]; then
    echo "already built: $driver"
else
    [ "$PS5VK" = /root/ps5vk ] || fail "the driver build script works in /root/ps5vk; leave PS5VK unset"
    bash "$here/build_ps5_vulkan_driver.sh" || true
    grep -q "ALL STEPS DONE" /root/ps5vk-arch.log || { tail -25 /root/ps5vk-arch.log >&2; fail "driver build (log: /root/ps5vk-arch.log)"; }
fi
[ -f "$sdk/toolchain/prospero.cmake" ] || fail "the PS5 toolchain is missing under $sdk"

# ---------------------------------------------------------------------------
step "3/9 ReXGlue SDK $rex_tag with this project's patches"
if [ ! -f "$rex_src/.mcla-patched" ]; then
    if [ ! -d "$rex_src/.git" ]; then
        logged sdk-clone git clone --recursive --branch "$rex_tag" https://github.com/rexglue/rexglue-sdk.git "$rex_src"
    fi
    [ "$(git -C "$rex_src" rev-parse HEAD)" = "$rex_commit" ] || fail "the SDK checkout is not $rex_tag ($rex_commit)"
    git -C "$rex_src" apply "$repo/patches/rexglue-v0.10.0-mcla.patch" || fail "SDK patch does not apply (is the checkout clean?)"
    git -C "$rex_src/thirdparty/FFmpeg" apply "$repo/patches/rexglue-ffmpeg-ps5-config.patch" || fail "FFmpeg patch does not apply"
    touch "$rex_src/.mcla-patched"
else
    echo "already patched: $rex_src"
fi

# ---------------------------------------------------------------------------
step "4/9 the recompiler (rexglue), for this machine"
rexglue=$(find "$rex_host" "$rex_src/out" -maxdepth 4 -name rexglue -type f -perm -u+x 2> /dev/null | head -1 || true)
if [ -z "$rexglue" ]; then
    logged host-configure cmake -S "$rex_src" -B "$rex_host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_STANDARD=23 \
        -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
        -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF
    logged host-build ninja -C "$rex_host" -j "$jobs" rexglue
    rexglue=$(find "$rex_host" "$rex_src/out" -maxdepth 4 -name rexglue -type f -perm -u+x | head -1)
fi
[ -x "$rexglue" ] || fail "the recompiler was not built"
echo "recompiler: $rexglue"

# ---------------------------------------------------------------------------
step "5/9 your game: extract from the disc image, then recompile its code"
if [ ! -f "$repo/game/default.xex" ]; then
    [ -n "$iso" ] || fail "game/ is empty: give your disc image with --iso"
    [ -f "$iso" ] || fail "no such file: $iso"
    logged extract python3 "$repo/scripts/extract_game.py" "$iso" "$repo/game"
fi
[ -f "$repo/game/default.xex" ] || fail "game/default.xex is missing after extraction"
# The recompiled code depends on the hook and function lists in config/ and on
# the recompiler: regenerate when any of them is newer than the result.
stamp="$repo/generated/default/.generated"
if [ ! -f "$stamp" ] || [ -n "$(find "$repo/config" "$repo/mcla_manifest.toml" "$rexglue" -newer "$stamp" -print -quit)" ]; then
    ( cd "$repo" && logged codegen "$rexglue" codegen mcla_manifest.toml )
    grep -q 'REX_PLATFORM_PS5' "$repo/generated/default/mcla_pch.h" || fail "the generated header lacks the PS5 rule (unpatched recompiler?)"
    touch "$stamp"
else
    echo "generated code is up to date"
fi
echo "generated sources: $(ls "$repo"/generated/default/*.cpp | wc -l)"

# ---------------------------------------------------------------------------
step "6/9 the runtime, for PS5"
if [ ! -f "$rex_build/build.ninja" ]; then
    # -flto=thin: 5-8% more draws a second on the console (docs/ps5-port-plan.md).
    logged ps5-configure cmake -S "$rex_src" -B "$rex_build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=23 \
        -DCMAKE_C_FLAGS="-march=znver2 -flto=thin" \
        -DCMAKE_CXX_FLAGS="-march=znver2 -fexperimental-library -flto=thin" \
        -DREXGLUE_USE_D3D12=OFF -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF
fi
logged ps5-runtime ninja -C "$rex_build" -j "$jobs" rexruntime rexgpu-xenos

# ---------------------------------------------------------------------------
step "7/9 the title's tile and backgrounds"
if [ -z "$art_dir" ]; then
    art_dir="$WORK/title-art"
    if [ ! -f "$art_dir/icon0.png" ]; then
        # From the dashboard art on your own disc; none is in the repository.
        logged art-extract python3 "$here/make_title_art.py" "$repo/game" "$WORK/title-art-source"
        [ -d "$driver/.deps/native/bc7enc_rdo" ] || ( cd "$driver" && logged art-tools bash tools/setup-asset-dependencies.sh )
        mkdir -p "$art_dir"
        ( cd "$driver" && logged art-convert bash tools/prepare-assets.sh --icon "$WORK/title-art-source/icon.png" \
            --background "$WORK/title-art-source/background.png" --output-directory "$art_dir" )
    else
        echo "already made: $art_dir"
    fi
fi
[ -f "$art_dir/icon0.png" ] || fail "no icon0.png in $art_dir"
# Your own tile or theme over that: kept apart, so the art made from the disc
# is still there for a later build without them.
if [ -n "$tile$theme" ]; then
    [ -z "$tile" ] || [ -f "$tile" ] || fail "no such file: $tile"
    [ -z "$theme" ] || [ -f "$theme" ] || fail "no such file: $theme"
    rm -rf "$WORK/title-art-own"; cp -r "$art_dir" "$WORK/title-art-own"; art_dir="$WORK/title-art-own"
    if [ -n "$tile" ]; then
        python3 - "$tile" "$art_dir/icon0.png" <<'PY' || fail "the tile could not be converted"
import sys
from PIL import Image
Image.open(sys.argv[1]).convert("RGBA").resize((512, 512), Image.LANCZOS).save(sys.argv[2])
PY
    fi
    [ -z "$theme" ] || cp "$theme" "$art_dir/snd0.at9"
    echo "own art: $art_dir"
fi

# ---------------------------------------------------------------------------
step "8/9 the game for PS5 (about 120 large files: 10 to 30 minutes the first time)"
export PS5_VULKAN="$driver" REX_SRC="$rex_src" REX_BUILD="$rex_build" MCLA_PS5_WORK="$WORK/game-ps5" JOBS="$jobs"
export TITLE="$title_id" MCLA_TITLE_NAME="Midnight Club: Los Angeles" ART_DIR="$art_dir"
if [ -n "$play" ]; then
    export MCLA_PLAY=1 MCLA_LOG_LEVEL=warning
else
    export MCLA_RUN_SECONDS=3600
fi
logged game-build bash "$here/game/build.sh" 7
dist="$driver/dist/$title_id"
[ -f "$dist/eboot.bin" ] || fail "no eboot.bin in $dist"
out="$repo/out/ps5-title/$title_id"
rm -rf "$out"; mkdir -p "$(dirname "$out")"; cp -r "$dist" "$out"
echo "title: $out ($(du -sh "$out" | cut -f1))"

# ---------------------------------------------------------------------------
step "9/9 the console"
if [ -z "$console" ]; then
    cat <<EOF
No --console given, so nothing was uploaded. To install by hand, copy
  $out            ->  /data/homebrew/$title_id   on the console
  $repo/game      ->  /data/mcla/game            on the console
and see docs/ps5-build-guide.md, "On the console".
EOF
    exit 0
fi
ftp="ftp://$console:$ftp_port"
curl -s --max-time 15 "$ftp/data/" > /dev/null || fail "no FTP server at $console:$ftp_port (start one on the console first)"
# The size of a file on the console; nothing if it is not there (curl fails
# then, which must not end the script).
remote_size_of() {
    { curl -s --max-time 30 -I "$ftp$1" 2> /dev/null || true; } | tr -d '\r' | awk '/Content-Length/{print $2}'
}
# Upload a file unless one of the same size is already there.
put() {
    local source=$1 target=$2 local_size remote_size
    local_size=$(stat -c %s "$source")
    remote_size=$(remote_size_of "$target")
    if [ "$local_size" = "$remote_size" ] && [ "${3:-}" != always ]; then
        echo "  same size, kept: $target"
        return
    fi
    echo "  uploading $target ($local_size bytes)"
    # Three tries: a console's FTP server now and then drops a long transfer.
    local attempt
    for attempt in 1 2 3; do
        curl -s -S --ftp-create-dirs -T "$source" "$ftp$target" && break
        [ "$attempt" -lt 3 ] || fail "upload of $target (run the same command again to continue; files already there are kept)"
        echo "  retrying $target"
        sleep 10
    done
    remote_size=$(remote_size_of "$target")
    [ "$local_size" = "$remote_size" ] || fail "$target is $remote_size bytes on the console, $local_size here"
}
echo "game data to /data/mcla/game (6 GB the first time)"
( cd "$repo/game" && find . -type f | sed 's#^\./##' | sort ) | while read -r file; do
    put "$repo/game/$file" "/data/mcla/game/$file"
done
echo "title to /data/homebrew/$title_id"
( cd "$out" && find . -type f | sed 's#^\./##' | sort ) | while read -r file; do
    put "$out/$file" "/data/homebrew/$title_id/$file" always
done
cat <<EOF

Done. On the console, "Midnight Club: Los Angeles" appears on the home screen
once your homebrew mounter has picked up /data/homebrew/$title_id (see
docs/ps5-build-guide.md, "On the console").
EOF
