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
# GAME_OPT overrides the optimization level (R96: -O2 by default, -O1 before);
# GAME_OBJ_TAG keeps its objects apart.
flags=(-std=c++20 -march=znver2 -mcx16 ${GAME_OPT:--O2} -fPIC -fno-exceptions -fno-rtti -ffp-contract=off
   -ffunction-sections -fdata-sections -I "$RECOMP/include")

game_object() {
   local base=${1##*/}
   case "$name/${base%.cpp}" in
      main/c_042|main/c_050|main/c_051|main/c_052|main/c_145)
         printf '%s/obj/%s.o\n' "$packed_build" "${base%.cpp}" ;;
      *) printf '%s/%s.o\n' "$out" "${base%.cpp}" ;;
   esac
}

compile_unit() {
   local unit=$1 obj
   shift
   obj=$(game_object "$unit")
   if [[ -s $obj && $obj -nt $unit && ( ! -f ${unit%/*}/funcs.h || $obj -nt ${unit%/*}/funcs.h ) ]]; then
      return
   fi
   local extra=()
   if [[ -n $packed_build && $obj == "$packed_build/obj/"* ]]; then
      extra=(-DPOKOPIA_PACKED_FLOAT32 -I "$packed_build/include")
   fi
   if ! ps5_cc "${extra[@]}" "$@" -c "$unit" -o "$obj.tmp" 2> "$obj.err"; then
      echo "FAILED $unit"
      head -20 "$obj.err"
      return 255
   fi
   mv -- "$obj.tmp" "$obj"
}

export SDK VK
export -f ps5_cc game_object compile_unit
for name in "${modules[@]}"; do
   src=${lift[$name]}
   out="$BUILD/obj/game-$name${GAME_OBJ_TAG:-}"
   mkdir -p "$out"
   exec {lock_fd}>"$out/.compile.lock"
   flock "$lock_fd"
   packed_build=
   if [[ $name == main ]]; then
      packed_build=$(python3 "$POKOPIA/ps5/build/prepare-packed-floats.py" \
         "$RECOMP/include/nxrecomp" "$out/packed-float32" "${flags[@]}")
      mkdir -p "$packed_build/obj"
   fi
   export name out packed_build
   # Copy the lift to local disk once: compiling from /mnt/e is several times slower.
   local_src="$BUILD/lift/$name"
   mkdir -p "$local_src"
   rsync -a --delete "$src/" "$local_src/"
   # R96: port patches to the local lift (the frame limiter's target rate).
   [[ $name == main ]] && python3 "$POKOPIA/ps5/build/patch-lift.py" "$local_src"
   units=("$local_src"/*.cpp)
   echo "==> $name: ${#units[@]} units, $jobs jobs"
   start=$SECONDS
   printf '%s\0' "${units[@]}" | xargs -0 -P "$jobs" -I{} bash -c 'compile_unit "$@"' _ {} "${flags[@]}"
   objects=()
   for unit in "${units[@]}"; do objects+=("$(game_object "$unit")"); done
   archive="$BUILD/libpokopia-$name-ps5.a"
   temp=$(mktemp -d "$BUILD/.archive-$name-XXXXXXXX")
   trap 'rm -f -- "$temp/library.a"; rmdir -- "$temp"' EXIT
   "$SDK/bin/prospero-ar" rcs "$temp/library.a" "${objects[@]}"
   mv -- "$temp/library.a" "$archive"
   rmdir -- "$temp"
   trap - EXIT
   exec {lock_fd}>&-
   echo "==> $name done in $((SECONDS - start)) s: $(du -h "$BUILD/libpokopia-$name-ps5.a" | cut -f1)"
done
