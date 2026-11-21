#!/bin/bash
source /mnt/e/Pokopia-PS4/ps5/build/env.sh
$SDK/bin/llvm-nm -n --defined-only /root/ps5/build/pokopia/link/pokopia.elf | grep -n -A6 -B2 " dispose_chunk$"
