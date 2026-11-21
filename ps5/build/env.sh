# Shared settings for the PS5 build scripts (sourced, run inside the PS5Build WSL distro).
POKOPIA=${POKOPIA:-/mnt/e/Pokopia-PS4}
VK=${PS5_VULKAN_DIR:-$HOME/ps5/PS5_Vulkan}
SDK="$VK/.deps/native/ps5-payload-sdk"
RADV_ARCHIVE=${RADV_ARCHIVE:-$VK/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
RADV_INCLUDE="$VK/.deps/native/radv-release/include"
RECOMP=${RECOMP:-/mnt/c/Users/Brad/Downloads/PokopiaRecomp-0.1.1-win64/PokopiaRecomp-0.1.1/tools/build/recomp}
OPUS_SRC="$POKOPIA/upstream/opus-1.6.1"
PINNED="$POKOPIA/upstream/switch2-recomp-0.2.0"
BUILD=${PS5_BUILD:-$HOME/ps5/build/pokopia}

ps5_cc() { PS5_PAYLOAD_SDK="$SDK" sh "$VK/tooling/prospero-clang18" "$@"; }

# Flags shared by every runtime translation unit. __ORBIS__ keeps the shared
# console code paths; __PROSPERO__ marks the PS5-specific branches.
PS5_ARCH=(-march=znver2 -O2 -fPIC -funwind-tables -ffunction-sections -fdata-sections)
PS5_DEFS=(-D__ORBIS__ -D__PROSPERO__ -D_BSD_SOURCE=1 -D_XOPEN_SOURCE=700 -DPOKOPIA_GUEST_PROBE -DPOKOPIA_LOADER_PROBE
   -DPOKOPIA_BOOT_PROBE -DPOKOPIA_BOOT_GAME -DPOKOPIA_GFX -DPOKOPIA_AUDIO_OUT -DPOKOPIA_HWOPUS
   -DPOKOPIA_GAME_BOOT=${GAME_BOOT_REVISION:-65})
PS5_INCS=(-I "$POKOPIA/ps5/compat/include" -I "$RECOMP/include" -I "$POKOPIA/loader-probe"
   -I "$POKOPIA/horizon" -I "$POKOPIA/boot" -I "$OPUS_SRC/include" -isystem "$RADV_INCLUDE")
