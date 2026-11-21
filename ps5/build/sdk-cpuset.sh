#!/usr/bin/env bash
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
grep -rn "define\s*CPU_SETSIZE\|define\s*CPU_MAXSIZE\|define\s*_CPU_SETSIZE" "$SDK"/target/include 2>/dev/null | head
grep -rln "scePthreadSetaffinity\|sceKernelSetThreadAffinityMask\|scePthreadSetprio" "$SDK"/target/include 2>/dev/null | head
