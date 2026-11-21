#!/usr/bin/env bash
# Sum host-code samples by function.
#   host-profile.sh <fatal_signal runtime addr> <samples file: "tid pc count" per line>
set -eu
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
elf="$BUILD/link/pokopia.elf"
anchor_runtime=$(( $1 ))
anchor_link=$(( 0x$("$SDK/bin/llvm-nm" "$elf" | awk '$3=="fatal_signal"{print $1; exit}') ))
slide=$(( anchor_runtime - anchor_link ))
work=$(mktemp -d)
# One address per sample line; system-library PCs (>= 0x800000000) get a placeholder.
while read -r tid pc count; do
    if (( pc >= 0x800000000 )); then
        printf '0x0\n' >> "$work/addrs"
    else
        printf '%#x\n' $(( pc - slide )) >> "$work/addrs"
    fi
    printf '%s %s %s\n' "$tid" "$pc" "$count" >> "$work/rows"
done < "$2"
llvm-symbolizer --obj="$elf" --functions=linkage --demangle --output-style=GNU < "$work/addrs" |
    awk 'NR % 2 == 1' > "$work/names"
paste -d'\t' "$work/rows" "$work/names" |
    awk -F'\t' '{split($1, r, " "); name = r[2] + 0 >= 0 && r[2] ~ /^0x8/ && length(r[2]) > 10 ? "(system library)" : $2;
                 sum[r[1] "\t" name] += r[3]} END {for (k in sum) print sum[k] "\t" k}' | sort -rn | sed -n '1,40p'
rm -rf "$work"
