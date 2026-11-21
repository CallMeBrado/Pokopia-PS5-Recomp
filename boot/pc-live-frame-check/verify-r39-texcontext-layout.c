#include "nvn_guest_layout.h"
#include <inttypes.h>
#include <stdio.h>

int main(void)
{
    const NvnFrontTextureConfig builder = {
        .flags = 4, .target = 4, .width = 1024, .height = 1024, .depth = 2,
        .levels = 9, .format = 0x4d,
    };
    uint64_t size, alignment;
    if (!nvn_guest_texture_layout(&builder, &size, &alignment))
        return 1;
    printf("Production guest layout request = %#" PRIx64 " (%" PRIu64 ") bytes, alignment %#" PRIx64 "\n", size, size, alignment);
    return size == UINT64_C(0x2ac000) ? 0 : 2;
}
