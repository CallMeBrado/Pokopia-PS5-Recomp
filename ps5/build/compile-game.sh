#!/usr/bin/env bash
# Compile the recompiled game modules (lifted C++) for the PS5 and archive them.
# Uses the existing lift output from the PS4 build; the lifted code is
# platform-neutral, only the compiler target and -march change.
#
#   main     game-bringup/lift/main      (STOCK arenas: no ps4-memory.sites)
#   subsdk0  game-bringup/lift/subsdk0
#   sdk      sdk-bringup/lift-graphics   (graphics boot sites)
#
#   wsl -d PS5Build -u root -- bash /mnt/e/Pokopia-PS4/ps5/build/compile-game.sh [module...]
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"

declare -A lift=(
   [main]="$POKOPIA/game-bringup/lift/main"
   [subsdk0]="$POKOPIA/game-bringup/lift/subsdk0"
   [sdk]="$POKOPIA/sdk-bringup/lift-graphics"
)
modules=("$@")
(( ${#modules[@]} )) || modules=(subsdk0 sdk main)
jobs=${JOBS:-$(nproc)}
flags=(-std=c++20 -march=znver2 -mcx16 -O1 -fPIC -fno-exceptions -fno-rtti -ffp-contract=off
   -ffunction-sections -fdata-sections -I "$RECOMP/include")

export SDK VK
export -f ps5_cc
for name in "${modules[@]}"; do
   src=${lift[$name]}
   out="$BUILD/obj/game-$name"
   mkdir -p "$out"
   # Copy the lift to local disk once: compiling from /mnt/e is several times slower.
   local_src="$BUILD/lift/$name"
   mkdir -p "$local_src"
   rsync -a --delete "$src/" "$local_src/"
   echo "==> $name: $(ls "$local_src"/*.cpp | wc -l) units, $jobs jobs"
   start=$SECONDS
   ls "$local_src"/*.cpp | xargs -P "$jobs" -I{} bash -c '
      unit={}; obj="'"$out"'/$(basename "${unit%.cpp}").o"
      [[ -f $obj && $obj -nt $unit ]] && exit 0
      ps5_cc "$@" -c "$unit" -o "$obj" 2> "$obj.err" || { echo "FAILED $unit"; head -20 "$obj.err"; exit 255; }
   ' _ "${flags[@]}"
   "$SDK/bin/prospero-ar" rcs "$BUILD/libpokopia-$name-ps5.a" "$out"/*.o
   echo "==> $name done in $((SECONDS - start)) s: $(du -h "$BUILD/libpokopia-$name-ps5.a" | cut -f1)"
done
