#!/usr/bin/env bash
# Link the Pokopia PS5 title and lay it out as a PPSA directory app.
# Run compile-runtime.sh and compile-game.sh first.
#
#   wsl -d PS5Build -u root -- bash /mnt/e/Pokopia-PS4/ps5/build/link-title.sh
#
# Output: /mnt/e/Pokopia-PS4/ps5/dist/PPSA94010/ (copy to /data/homebrew/ with ps5/scripts/deploy.py)
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"

param="$POKOPIA/ps5/title/param.json"
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
out="$POKOPIA/ps5/dist/$title_id"
work="$BUILD/link"
native="$VK/tooling/native"
tool="$VK/build/host/ps5-native-tool"
mkdir -p "$work/obj" "$work/stubs"

# --- Opus (static, PS5) ------------------------------------------------------
opus_build="$BUILD/opus"
if [[ ! -f $opus_build/libopus.a ]]; then
   echo "==> building opus"
   rm -rf "$opus_build"
   PS5_PAYLOAD_SDK="$SDK" "$SDK/bin/prospero-cmake" -S "$OPUS_SRC" -B "$opus_build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_C_FLAGS=-march=znver2 \
      -DOPUS_DISABLE_INTRINSICS=ON -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF \
      -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF > "$opus_build.log" 2>&1 ||
      { tail -20 "$opus_build.log"; exit 1; }
   ninja -C "$opus_build" >> "$opus_build.log" 2>&1 || { tail -20 "$opus_build.log"; exit 1; }
fi

# --- App runtime objects and AGC stubs ---------------------------------------
ps5_cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
   -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
ps5_cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
   -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"
stub() {
   ps5_cc -std=c11 -O2 -fPIC -c "$VK/$2" -o "$work/obj/$1_stub.o"
   "$SDK/bin/prospero-lld" --shared -soname "$1.prx" -o "$work/stubs/$1.so" "$work/obj/$1_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

game_libs=("$BUILD/libpokopia-sdk-ps5.a" "$BUILD/libpokopia-main-ps5.a" "$BUILD/libpokopia-subsdk0-ps5.a")
for lib in "${game_libs[@]}" "$opus_build/libopus.a" "$RADV_ARCHIVE"; do
   [[ -f $lib ]] || { echo "missing $lib" >&2; exit 2; }
done

# --- Link ----------------------------------------------------------------------
source "$VK/tools/radv-link.sh"
radv_link_recipe "$VK" "$SDK" "$RADV_ARCHIVE" || exit 2
echo "==> linking"
"$SDK/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
   --version-script "$native/app-symbols.map" --exclude-libs=ALL \
   -e _start -o "$work/pokopia.elf" \
   "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$BUILD"/obj/runtime/*.o \
   "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
   --start-group "${game_libs[@]}" "$opus_build/libopus.a" --end-group \
   "${radv_link_inputs[@]}" \
   --as-needed "$SDK"/target/lib/*.so 2> "$work/link.err" ||
   { grep -E 'error|undefined' "$work/link.err" | sort | uniq -c | sort -rn | head -40; exit 1; }
"$tool" link --in "$work/pokopia.elf" --out "$work/eboot.elf" \
   --stub-dir "$SDK/target/lib" --stub "$work/stubs/libSceAgc.so" \
   --stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
   --companion-sdk 0x08050001 --file-name eboot.elf

# --- Title folder --------------------------------------------------------------
rm -rf -- "$out"
mkdir -p "$out/sce_sys" "$out/sce_module" "$out/modules" "$out/shaders"
"$tool" self --sign --in "$work/eboot.elf" --out "$out/eboot.bin" --magic 0x1D3D154F
cp "$param" "$out/sce_sys/param.json"
cp "$POKOPIA/assets/icon0.png" "$out/sce_sys/icon0.png"
cp "$VK/runtime/libc.prx" "$out/sce_module/libc.prx"
for module in main sdk subsdk0 rtld; do cp "$POKOPIA/loader-probe/generated/$module.bin" "$out/modules/"; done
cp "$POKOPIA/boot/shader-cache-655360/"* "$out/shaders/"
cp "$OPUS_SRC/COPYING" "$out/opus-license.txt"
"$tool" self --inspect --file "$out/eboot.bin" > /dev/null
echo "==> title: $out (eboot $(stat -c %s "$out/eboot.bin") bytes, total $(du -sh "$out" | cut -f1))"
