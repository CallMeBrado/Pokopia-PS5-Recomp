#!/usr/bin/env bash
# Compile the Pokopia runtime (loader, Horizon HLE, NVN->Vulkan bridge) for the PS5.
# Mirrors the source list of boot/r41-cold-check/build-native.ps1 -Graphics -Ps4Memory,
# minus the PS4-only sparse heap / cold-page spill / memory census.
#
#   wsl -d PS5Build -u root -- bash /mnt/e/Pokopia-PS4/ps5/build/compile-runtime.sh [--keep-going]
set -uo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
keep_going=false
[[ ${1:-} == --keep-going ]] && keep_going=true

c_sources=(
   boot/r41-cold-check/loader_native.c cpu-probe/generated/rtld_7700000000.c
   horizon/get_info.c horizon/entropy.c horizon/sm.c horizon/lm.c horizon/cmif.c horizon/applet.c
   horizon/aoc.c horizon/pctl.c horizon/time_service.c horizon/readonly_page.c horizon/live_shared.c
   horizon/heap.c horizon/transfer_memory.c horizon/stack_map.c horizon/heap_stack_alias.c
   horizon/thread.c horizon/event.c horizon/key_sync.c boot/r41-cold-check/boot_budget.c boot/host.c
   gfx-bridge/vk_adapter.c gfx-bridge/vk_copy.c gfx-bridge/vk_program.c gfx-bridge/vk_texel.c
   gfx-bridge/vi_adapter.c gfx-bridge/nvn_front.c gfx-bridge/nvn_window.c gfx-bridge/nvn_replay.c
   gfx-bridge/shader_cache.c gfx-bridge/texture_upload.c gfx-bridge/live_texture_upload.c gfx-bridge/osk.c gfx-bridge/port_settings.c
   gfx-bridge/live_draw.c gfx-bridge/live_compute.c gfx-bridge/live_present.c gfx-bridge/game_capture.c
   gfx-bridge/fsp.c gfx-bridge/romfs.c gfx-bridge/account.c gfx-bridge/orbis_memory.c
   ps5/compat/vkloader/vkloader.c ps5/compat/vkloader/vkthunks.c ps5/compat/cold_disabled.c
   gpu-present/present.c gpu-present/frame_capture.c gpu-present/debug_mailbox.c
   horizon/stack_source.c horizon/audio_device.c horizon/audio_out.c horizon/hwopus.c horizon/bsd_ssl.c
   horizon/stpl.c horizon/offline_app.c horizon/hid.c horizon/hid_ps4.c horizon/debug_pad.c horizon/save_store.c
   toolchain/main.c
)
cpp_sources=(
   gfx-bridge/nvn_guest_layout.cpp horizon/ps4-pcm-stream.cpp horizon/ps4-pcm-native.cpp
   "$PINNED/runtime/src/gfx/nvn/nvn_texture_layout.cpp" "$PINNED/runtime/src/gfx/nvn/nvn_format.cpp"
   "$PINNED/tools/tiling/src/tegra_tiling.cpp"
)
cpp_incs=(-I "$POKOPIA/gfx-bridge/nvn-layout-include" -I "$PINNED/runtime/include"
   -I "$PINNED/runtime/src/gfx/nvn" -I "$PINNED/runtime/src/gfx/nvn/third_party"
   -I "$PINNED/tools/tiling/include")

obj="$BUILD/obj/runtime"
mkdir -p "$obj"
failed=()
compile() {
   local src=$1 out=$2; shift 2
   if ! ps5_cc "$@" -c "$src" -o "$out" 2> "$out.err"; then
      failed+=("$src")
      echo "FAILED $src ($(grep -c 'error:' "$out.err") errors)"
      $keep_going || { head -40 "$out.err"; exit 1; }
   fi
}
for src in "${c_sources[@]}"; do
   [[ $src == /* ]] || src="$POKOPIA/$src"
   compile "$src" "$obj/$(basename "${src%.*}").o" -std=gnu11 "${PS5_ARCH[@]}" "${PS5_DEFS[@]}" "${PS5_INCS[@]}" -I "$POKOPIA/ps5/compat/vkloader"
done
for src in "${cpp_sources[@]}"; do
   [[ $src == /* ]] || src="$POKOPIA/$src"
   compile "$src" "$obj/$(basename "${src%.*}").o" -std=c++20 -fno-exceptions -fno-rtti \
      "${PS5_ARCH[@]}" "${PS5_DEFS[@]}" "${PS5_INCS[@]}" "${cpp_incs[@]}"
done
echo "runtime: ${#failed[@]} of $(( ${#c_sources[@]} + ${#cpp_sources[@]} )) sources failed"
(( ${#failed[@]} == 0 ))
