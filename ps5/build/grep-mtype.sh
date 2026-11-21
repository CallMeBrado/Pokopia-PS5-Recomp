#!/usr/bin/env bash
cd ~/ps5/PS5_Vulkan/docs
sed -n '475,495p' M5_PHASE_B.md
echo ====
grep -n -iE "sceKernelAllocateDirectMemory" *.md | head -20
echo ====
sed -n '3691,3720p' HARDWARE_FINDINGS.md
