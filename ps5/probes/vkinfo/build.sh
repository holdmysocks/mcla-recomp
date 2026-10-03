#!/usr/bin/env bash
# Build the mcla-vkinfo title against the PS5 Vulkan driver.
#
# Run on the Arch host after ps5/build_ps5_vulkan_driver.sh has completed.
# Reuses the driver project's own title build (tools/build-radv-title.sh) with
# this probe's source, title id and work directory substituted, so it is
# linked exactly as the driver's smoke test is. Output: dist/PPSA99777 in the
# driver checkout. Touches no console.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
driver=${PS5_VULKAN:-/root/ps5vk/PS5_Vulkan}
work=/root/ps5vk/mcla-vkinfo
mkdir -p "$work"
tr -d '\r' < "$here/main.c" > "$work/main.c"

# Same metadata as the smoke test, under its own title id and name.
python3 - "$driver/sce_sys/param-radv.json" "$work/param.json" <<'PY'
import json, sys
param = json.load(open(sys.argv[1]))
param["titleId"] = "PPSA99777"
param["conceptId"] = "99777"
param["contentId"] = "UP9000-PPSA99777_00-MCLAVKINFO000001"
param["localizedParameters"]["en-US"]["titleName"] = "MCLA Vulkan Info"
json.dump(param, open(sys.argv[2], "w"), indent=2)
PY

sed -e "s#^param=.*#param=\"$work/param.json\"#" \
    -e "s#^work=.*#work=\"\$root/build/mcla-vkinfo\"#" \
    -e "s#\"\$root/radv/radv_smoke.c\"#\"$work/main.c\"#" \
    "$driver/tools/build-radv-title.sh" > "$driver/tools/build-mcla-vkinfo.sh"
grep -n "^param=\|^work=\|main.c" "$driver/tools/build-mcla-vkinfo.sh"

cd "$driver"
RADV_ARCHIVE="$driver/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" bash tools/build-mcla-vkinfo.sh
find dist/PPSA99777 -type f -exec ls -la {} \;
