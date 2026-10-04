#!/usr/bin/env bash
# Link objects into an installable PS5 title (a /data/homebrew/<TITLEID>
# folder) together with the RADV Vulkan driver.
#
# The driver (mihawk-99/PS5_Vulkan) presents through the console's own display
# and GPU libraries, which only an installed title can use, and it has to be
# linked into the title. This follows that project's tools/build-radv-title.sh
# (GPL-3.0-or-later, Copyright (C) 2026 Mihawk-99): its startup code, its link
# recipe (tools/radv-link.sh), its ELF converter and signer.
#
# One difference, and it matters. The runtime and the game code are built with
# the large code model, so their code is in .ltext.* sections, which that
# project's linker script (tooling/native/ps5-pie.ld) does not name. Left as
# orphans they are placed in the read/write segment, which is not executable.
# The first title built this way (PPSA99778) was linked like that, and it took
# the console down on launch. The script used here is derived from theirs with
# the large-model sections placed beside their normal counterparts, and the
# build fails if any executable section is outside .text.
#
# Usage: title_build.sh <TITLEID> <title name> <link inputs...>
#   link inputs: object files, then anything else lld takes (archives,
#   --start-group ... --end-group). Output: <driver>/dist/<TITLEID>.
set -euo pipefail
title_id=${1:?title id, e.g. PPSA99779}; title_name=${2:?title name}; shift 2
[ $# -gt 0 ] || { echo "no link inputs" >&2; exit 2; }

root=${PS5_VULKAN:-/root/ps5vk/PS5_Vulkan}
archive=${RADV_ARCHIVE:-$root/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
sdk_root="$root/.deps/native/ps5-payload-sdk"
native="$root/tooling/native"
tool="$root/build/host/ps5-native-tool"
work="$root/build/mcla-$title_id"
module_sdk=0x02000009
companion_sdk=0x08050001
fself_magic=0x1D3D154F

for file in "$archive" "$tool" "$sdk_root/bin/prospero-lld" "$root/sce_sys/param-radv.json"; do
    [[ -e $file ]] || { echo "missing $file" >&2; exit 2; }
done
mkdir -p "$work/obj" "$work/stubs" "$work/ld"
cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" "$@"; }

python3 - "$root/sce_sys/param-radv.json" "$work/param.json" "$title_id" "$title_name" <<'PY'
import json, sys
param = json.load(open(sys.argv[1]))
title_id, name = sys.argv[3], sys.argv[4]
param["titleId"] = title_id
param["conceptId"] = title_id[4:]
param["contentId"] = "UP9000-%s_00-MCLA%012d" % (title_id, int(title_id[4:]))
param["localizedParameters"]["en-US"]["titleName"] = name
json.dump(param, open(sys.argv[2], "w"), indent=2)
PY

# The linker script: theirs, plus the large-model sections.
sed -E 's/\*\(\.text \.text\.\*\)/*(.text .text.* .ltext .ltext.* __lcxx_override)/;
        s/\*\(\.rodata \.rodata\.\*\)/*(.rodata .rodata.* .lrodata .lrodata.*)/;
        s/\*\(\.data \.data\.\*\)/*(.data .data.* .ldata .ldata.*)/;
        s/\*\(\.bss \.bss\.\*\)/*(.bss .bss.* .lbss .lbss.*)/' \
    "$native/ps5-pie.ld" > "$work/ld/ps5-pie.ld"
[ "$(grep -c -E '\.ltext|\.lrodata|\.ldata|\.lbss' "$work/ld/ps5-pie.ld")" -eq 4 ] || { echo "linker script patterns did not match" >&2; exit 1; }
cp "$root/tooling/psbc/ps5-pie-unwind.ld" "$work/ld/ps5-pie-unwind.ld"

cc -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"

# AGC comes from system modules; these host-link stubs only name the imports.
stub() {
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$root/$source" -o "$work/obj/${library}_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$work/stubs/${library}.so" "$work/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# shellcheck disable=SC1091
source "$root/tools/radv-link.sh"
radv_link_recipe "$root" "$sdk_root" "$archive" || exit 2
"$sdk_root/bin/prospero-lld" -T "$work/ld/ps5-pie-unwind.ld" -L "$work/ld" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$@" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so

# Only .text is mapped executable.
stray=$(readelf -SW "$work/llvm-pie.elf" | sed 's/^ *\[ *[0-9]*\] *//' | awk '$7 ~ /X/ && $1 != ".text" {print $1}' | head -5)
[ -z "$stray" ] || { echo "executable sections outside .text: $stray" >&2; exit 1; }
main_address=$(nm "$work/llvm-pie.elf" | awk '$3=="main"{print $1}')
echo "main at 0x$main_address; segments:"
readelf -lW "$work/llvm-pie.elf" | grep -E '^ +(LOAD|DYNAMIC)'

"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --stub "$work/stubs/libSceAgc.so" \
    --stub "$work/stubs/libSceAgcDriver.so" --module-sdk "$module_sdk" \
    --companion-sdk "$companion_sdk" --file-name eboot.elf

app="$root/dist/$title_id"
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic "$fself_magic"
cp "$work/param.json" "$app/sce_sys/param.json"
for asset in icon0.png pic0.dds pic1.dds snd0.at9; do
    [[ -f $root/sce_sys/$asset ]] && cp "$root/sce_sys/$asset" "$app/sce_sys/$asset"
done
[[ -f $root/runtime/libc.prx ]] || bash "$root/tools/rebuild-libc.sh"
(cd "$root/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$root/runtime/libc.prx" "$app/sce_module/libc.prx"
"$tool" self --inspect --file "$app/sce_module/libc.prx" > /dev/null
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
echo "title: $app ($(stat -c %s "$app/eboot.bin") bytes)"
