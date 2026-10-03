#!/usr/bin/env bash
# Build mihawk-99's PS5 Vulkan driver (Mesa RADV port) and its smoke-test title.
#
# Host: Arch Linux (the driver's supported host), run as root in WSL.
# The driver project documents the individual steps but not their order or its
# sibling repositories; this script records the order that works:
#
#   siblings   PS5_PayloadSDK (SDK fork), PS5_Mesa (RADV port), ps5-opengl at
#              tag v0.3.0 (shader compiler sources and PS5 makefiles)
#   1. make deps                          payload SDK fork at its pinned revision
#   2. ps5-opengl: fetch-sources.py       pinned compiler sources
#   3. ps5-opengl: build-opengnm-psbc.sh  generates headers the next step needs
#   4. build-psbc-ps5.sh                  PS5 shader compiler (bootstrap from the checkout)
#   5. adapt-opengl-sdk.sh                reconstructs the SDK tree the tools read
#   6. fetch-mesa.sh, build-vulkan-runtime.sh
#   7. build-radv.sh release              RADV for PS5
#   8. make build                         host packaging tool and base app
#   9. build-radv-title.sh                smoke-test title (dist/PPSA99014)
#
# Nothing here touches a console. Log: /root/ps5vk-arch.log
set -u
export GIT_TERMINAL_PROMPT=0
exec > /root/ps5vk-arch.log 2>&1
step() { echo; echo "######## $* ($(date +%H:%M:%S))"; }
fail() { echo "STEP FAILED: $*"; exit 1; }

step "host packages"
pacman -Syu --noconfirm --needed base-devel clang llvm lld make python python-pip git curl wget unzip tar \
    pkgconf ccache cmake glslang python-mako python-markupsafe python-yaml python-packaging meson ninja rsync \
    spirv-llvm-translator spirv-tools libclc libelf zlib zstd expat bison flex libdrm wayland wayland-protocols \
    libx11 compiler-rt 2>&1 | tail -4
clang --version | head -1; ld.lld --version; meson --version

mkdir -p /root/ps5vk && cd /root/ps5vk
step "clone repositories"
[ -d PS5_Vulkan ] || git clone https://github.com/mihawk-99/PS5_Vulkan.git || fail clone
[ -d PS5_PayloadSDK ] || git clone https://github.com/mihawk-99/PS5_PayloadSDK.git || fail clone
[ -d ps5-opengl ] || git clone https://github.com/mihawk-99/ps5-opengl.git || fail clone
[ -d PS5_Mesa ] || git clone --filter=blob:none https://github.com/mihawk-99/PS5_Mesa.git || fail clone
git -C ps5-opengl checkout -q v0.3.0 || fail "ps5-opengl v0.3.0"
git -C PS5_Vulkan log -1 --format='PS5_Vulkan %h %ad %s' --date=short

cd /root/ps5vk/PS5_Vulkan
step "1 make deps"
nice -n 15 make deps || fail "make deps"

cd /root/ps5vk/ps5-opengl
step "2 ps5-opengl fetch-sources"
nice -n 15 python3 tools/fetch-sources.py || fail fetch-sources
step "3 ps5-opengl build-opengnm-psbc (generated headers)"
nice -n 15 bash toolchain/build-opengnm-psbc.sh > /root/psbc-host-build.log 2>&1 || { tail -8 /root/psbc-host-build.log; echo "note: continuing if the generated headers exist"; }
ls third_party/opengnm-psbc/src/util/format/u_format_gen.h || fail "generated headers"

cd /root/ps5vk/PS5_Vulkan
step "4 build-psbc-ps5"
PS5_OPENGL_SDK=/root/ps5vk/ps5-opengl nice -n 15 bash tools/build-psbc-ps5.sh || { grep -m5 -n 'error' .deps/work/psbc-ps5/build.log | cut -c1-200; fail build-psbc-ps5; }
step "5 adapt-opengl-sdk"
nice -n 15 bash tools/adapt-opengl-sdk.sh ../ps5-opengl || fail adapt-opengl-sdk
step "6 fetch-mesa and build-vulkan-runtime"
nice -n 15 bash tools/fetch-mesa.sh || fail fetch-mesa
nice -n 15 bash tools/build-vulkan-runtime.sh || fail build-vulkan-runtime

step "7 build-radv release"
# zlib.net was unreachable from this network; Meson checks the archive against
# the hash in the wrap file, so a copy from zlib's own release is equivalent.
nice -n 15 bash tools/build-radv.sh release
if [ $? -ne 0 ]; then
  W=.deps/work/radv-src/subprojects
  if [ -f $W/zlib.wrap ] && [ ! -f $W/packagecache/zlib-1.3.1.tar.gz ]; then
    echo "retrying with zlib pre-seeded"
    mkdir -p $W/packagecache
    curl -sSL --fail -m 120 -o $W/packagecache/zlib-1.3.1.tar.gz https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    want=$(grep '^source_hash' $W/zlib.wrap | sed 's/.*= *//'); got=$(sha256sum $W/packagecache/zlib-1.3.1.tar.gz | cut -d' ' -f1)
    [ "$want" = "$got" ] || fail "zlib checksum mismatch"
    rm -rf .deps/work/radv-build-ps5-release
    nice -n 15 bash tools/build-radv.sh release || { grep -n "ERROR" .deps/work/radv-*/meson-logs/meson-log.txt | tail -4 | cut -c1-220; fail build-radv; }
  else
    grep -n "ERROR" .deps/work/radv-*/meson-logs/meson-log.txt | tail -4 | cut -c1-220; fail build-radv
  fi
fi

step "8 make build (host tool and base app)"
nice -n 15 make build 2>&1 | tail -12
ls -la build/host/ps5-native-tool || fail "host tool"

step "9 build-radv-title"
RADV_ARCHIVE=$PWD/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a nice -n 15 bash tools/build-radv-title.sh 2>&1 | tail -30
[ ${PIPESTATUS[0]} -eq 0 ] || fail build-radv-title

step "outputs"
find dist -maxdepth 3 | head -40
du -sh dist/*
echo "ALL STEPS DONE"
