#!/usr/bin/env bash
# Build the Pokopia PS5 probe as a PPSA directory title linked with RADV.
# Mirrors PS5_Vulkan's tools/build-radv-title.sh, with our own source.
#
#   wsl -d PS5Build -u root -- bash /mnt/e/Pokopia-PS4/ps5/probe/build.sh
#
# Output: /mnt/e/Pokopia-PS4/ps5/dist/PPSA94003/  (copy to /data/homebrew/ on the console)
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
vk=${PS5_VULKAN_DIR:-$HOME/ps5/PS5_Vulkan}
sdk_root="$vk/.deps/native/ps5-payload-sdk"
archive=${RADV_ARCHIVE:-$vk/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
vk_include="$vk/.deps/native/radv-release/include"
native="$vk/tooling/native"
tool="$vk/build/host/ps5-native-tool"
work=$HOME/ps5/build/pokopia-probe
param="$here/param.json"
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
out="$here/../dist/$title_id"

module_sdk=0x02000009
companion_sdk=0x08050001
fself_magic=0x1D3D154F

mkdir -p "$work"
if [[ ! -x $tool ]]; then
   echo "==> building ps5-native-tool"
   mkdir -p "$(dirname "$tool")"
   clang++ -std=c++20 -O2 -Wall \
      "$native/native_app_builder.cpp" "$native/self_container.cpp" \
      "$native/elf_object.cpp" "$native/sce_module_writer.cpp" -lz -o "$tool"
fi
for file in "$archive" "$tool" "$sdk_root/bin/prospero-lld" "$native/app_crt.cpp"; do
   [[ -e $file ]] || { echo "missing $file" >&2; exit 2; }
done

mkdir -p "$work/obj" "$work/stubs"
cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$vk/tooling/prospero-clang18" "$@"; }

cc -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
   -I "$vk_include" -c "$here/probe.c" -o "$work/obj/probe.o"
cc -std=c++20 -O2 -Wall -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
   -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -Wall -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
   -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"

stub() {
   local library=$1 source=$2
   cc -std=c11 -O2 -fPIC -c "$vk/$source" -o "$work/obj/${library}_stub.o"
   "$sdk_root/bin/prospero-lld" --shared -soname "${library}.prx" \
      -o "$work/stubs/${library}.so" "$work/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

source "$vk/tools/radv-link.sh"
radv_link_recipe "$vk" "$sdk_root" "$archive" || exit 2
"$sdk_root/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
   --version-script "$native/app-symbols.map" --exclude-libs=ALL \
   -e _start -o "$work/llvm-pie.elf" \
   "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$work/obj/probe.o" \
   "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
   "${radv_link_inputs[@]}" \
   --as-needed "$sdk_root"/target/lib/*.so
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
   --stub-dir "$sdk_root/target/lib" --stub "$work/stubs/libSceAgc.so" \
   --stub "$work/stubs/libSceAgcDriver.so" --module-sdk "$module_sdk" \
   --companion-sdk "$companion_sdk" --file-name eboot.elf

rm -rf -- "$out"
mkdir -p "$out/sce_sys" "$out/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$out/eboot.bin" --magic "$fself_magic"
cp "$param" "$out/sce_sys/param.json"
[[ -f $here/../../assets/icon0.png ]] && cp "$here/../../assets/icon0.png" "$out/sce_sys/icon0.png"
[[ -f $vk/runtime/libc.prx ]] || bash "$vk/tools/rebuild-libc.sh"
(cd "$vk/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vk/runtime/libc.prx" "$out/sce_module/libc.prx"
"$tool" self --inspect --file "$out/eboot.bin" > /dev/null
echo "probe title: $out ($(stat -c %s "$out/eboot.bin") bytes)"
