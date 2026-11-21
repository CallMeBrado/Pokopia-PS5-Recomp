#!/usr/bin/env bash
# Map runtime addresses from a "Fatal signal" log back to symbols in link/pokopia.elf.
#   symbolize.sh <fatal_signal runtime addr> <addr> [addr...]
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
elf="$BUILD/link/pokopia.elf"
anchor_runtime=$(( $1 )); shift
anchor_link=$(( 0x$("$SDK/bin/llvm-nm" "$elf" | awk '$3=="fatal_signal"{print $1; exit}') ))
slide=$(( anchor_runtime - anchor_link ))
printf 'slide=%#x\n' "$slide"
for addr in "$@"; do
   link=$(( addr - slide ))
   printf '%#x -> %#x  ' "$addr" "$link"
   llvm-symbolizer --obj="$elf" --functions=linkage --demangle "$(printf '%#x' "$link")" | head -2 | paste -sd' '
done
