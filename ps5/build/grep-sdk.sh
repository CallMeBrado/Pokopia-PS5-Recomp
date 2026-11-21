#!/usr/bin/env bash
# grep the PS5 payload SDK target headers: grep-sdk.sh <regex> [file-glob]
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
grep -rnE "$1" "$SDK/target/include" --include="${2:-*.h}" | head -${3:-20}
