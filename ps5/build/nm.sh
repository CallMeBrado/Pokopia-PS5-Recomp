#!/usr/bin/env bash
# nm.sh <object-or-elf relative to $BUILD> [grep pattern]
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
"$SDK/bin/llvm-nm" "$BUILD/$1" 2>&1 | grep -E "${2:-.}" | head -${3:-40}
