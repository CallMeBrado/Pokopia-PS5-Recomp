#!/usr/bin/env bash
# Regenerate ps5/compat/vkloader/vkthunks.c from the runtime's own undefined vk* symbols,
# with prototypes from the vulkan_core.h the PS5 RADV archive was built against.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
scan="$BUILD/obj/vkthunk-scan"
rm -rf "$scan" && mkdir -p "$scan"
for obj in "$BUILD"/obj/runtime/*.o; do
   case $(basename "$obj") in vkthunks.o|vkloader.o) continue ;; esac
   ln -s "$obj" "$scan/"
done
python3 "$POKOPIA/ps5/compat/vkloader/gen.py" --header "$RADV_INCLUDE/vulkan/vulkan_core.h" \
   --out "$POKOPIA/ps5/compat/vkloader/vkthunks.c" --objs "$scan"
echo "thunks: $(grep -c '^vk' "$POKOPIA/ps5/compat/vkloader/vkthunks.c")"
