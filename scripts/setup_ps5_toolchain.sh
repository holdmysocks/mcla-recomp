#!/usr/bin/env bash
# Install the PS5 payload SDK and its compiler into a Linux host (tested on
# Ubuntu 24.04 under WSL2). Run as root.
set -euo pipefail

SDK_VERSION="${SDK_VERSION:-v0.43}"
PREFIX="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq clang-18 lld-18 llvm-18 unzip curl socat netcat-openbsd >/dev/null

if [ ! -f "$PREFIX/toolchain/prospero.mk" ]; then
    tmp="$(mktemp -d)"
    curl -sSL --fail -o "$tmp/sdk.zip" \
        "https://github.com/ps5-payload-dev/sdk/releases/download/${SDK_VERSION}/ps5-payload-sdk.zip"
    mkdir -p "$(dirname "$PREFIX")"
    unzip -q "$tmp/sdk.zip" -d "$(dirname "$PREFIX")"
    rm -rf "$tmp"
fi

echo "SDK: $PREFIX"
ls "$PREFIX"
"$PREFIX/bin/prospero-clang" --version | head -2
