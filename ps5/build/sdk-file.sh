#!/usr/bin/env bash
# Print a range of a PS5 SDK header: sdk-file.sh <relative path> [start] [end]
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
sed -n "${2:-1},${3:-80}p" "$SDK/target/include/$1"
