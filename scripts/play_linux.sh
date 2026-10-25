#!/bin/sh
# Starts the Linux build. The SDK loads the Xenos GPU plugin from the
# executable's folder only, so the SDK libraries are linked in next to mcla
# on first run. Options are passed on to mcla.
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/out/build/linux-amd64-release"
sdk_install="$root/third_party/rexglue-sdk/out/install/linux-amd64"
# GNUInstallDirs picks lib64 on Fedora/Arch and lib on Debian/Ubuntu.
sdk_lib="$sdk_install/lib64"
if [ ! -f "$sdk_lib/librexruntime.so" ]; then
  sdk_lib="$sdk_install/lib"
fi
if [ ! -f "$sdk_lib/librexruntime.so" ]; then
  echo "error: SDK libraries not found in $sdk_install/lib64 or $sdk_install/lib; build and install the SDK first (README.md, 'Linux')" >&2
  exit 1
fi

if [ ! -x "$build/mcla" ]; then
  echo "error: $build/mcla not found; build it first (README.md, 'Linux')" >&2
  exit 1
fi
for lib in librexruntime.so librexgpu-xenos.so; do
  if [ ! -e "$build/$lib" ]; then
    # -e is false for a dangling symlink, and ln would then fail on it.
    rm -f "$build/$lib"
    ln -s "$sdk_lib/$lib" "$build/$lib"
  fi
done

LD_LIBRARY_PATH="$build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$build/mcla" "$@"
