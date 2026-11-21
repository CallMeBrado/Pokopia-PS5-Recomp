#!/usr/bin/env bash
# Show which POKOPIA_* / platform macros a runtime TU actually sees.
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
echo "PS5_DEFS: ${PS5_DEFS[*]}"
ps5_cc -std=gnu11 "${PS5_ARCH[@]}" "${PS5_DEFS[@]}" "${PS5_INCS[@]}" -E -dM "$POKOPIA/${1:-toolchain/main.c}" |
   grep -E 'POKOPIA|__ORBIS__|__PROSPERO__|_XOPEN' | sort
