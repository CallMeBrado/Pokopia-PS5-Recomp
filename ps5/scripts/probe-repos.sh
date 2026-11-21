#!/usr/bin/env bash
# Inspect the PS5 Vulkan driver repos and how the Burnout 3 recomp consumes them.
api() { curl -s "https://api.github.com/repos/$1"; }
raw() { curl -sL "https://raw.githubusercontent.com/$1"; }

echo "=== PS5_Vulkan tree (top)"
api mihawk-99/PS5_Vulkan/contents | grep -E '"(name|type)"' | paste - - | head -40
echo "=== PS5_Vulkan README build section"
raw mihawk-99/PS5_Vulkan/HEAD/README.md | grep -niE 'build|archive|\.a\b|install|release|prebuilt|winsys|link' | head -60
echo "=== PS5_Burnout3 tree"
api griches/PS5_Burnout3/contents | grep -E '"name"' | head -40
echo "=== PS5_Burnout3 README vulkan/build lines"
raw griches/PS5_Burnout3/HEAD/README.md | grep -niE 'vulkan|radv|mesa|sdk|build|llvm|ppsa|shadowmount|install' | head -60
