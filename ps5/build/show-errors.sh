#!/usr/bin/env bash
# Print the first errors of every failed runtime object.
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
for err in "$BUILD"/obj/*/*.err; do
   [[ -s $err ]] && grep -q 'error:' "$err" || continue
   echo "=== $(basename "${err%.o.err}")"
   grep -A3 'error:' "$err" | head -${1:-24}
done
