#include "tegra_tiling.h"

#include <cstddef>
#include <cstdint>

extern "C" bool bc7_reference_tile(uint32_t width, uint32_t height, uint32_t gob_blocks_y,
    const uint8_t *linear, size_t linear_size, uint8_t *tiled, size_t tiled_size)
{
    const tegra::TextureDesc desc{
        .width = width, .height = height, .block_w = 4, .block_h = 4, .bytes_per_block = 16,
        .gob_blocks_y = gob_blocks_y, .linear_stride_align = 1, .gob_switch2 = true,
    };
    const tegra::Layout layout = tegra::compute_layout(desc);
    if (!layout.valid || layout.linear_size != linear_size || layout.tiled_size != tiled_size)
        return false;
    tegra::tile_level(desc, layout.levels[0], linear, tiled);
    return true;
}
