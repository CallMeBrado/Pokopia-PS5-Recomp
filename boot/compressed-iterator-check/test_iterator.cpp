#include "nvn_guest_layout.h"
#include "nvn_format.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace nxrt::nvn;

struct Mip {
    uint32_t edge, gob_height, offset;
};

constexpr std::array<Mip, 7> cube_mips{{
    {256, 8, 0}, {128, 4, 0x10000}, {64, 2, 0x14000}, {32, 1, 0x15000},
    {16, 1, 0x15400}, {8, 1, 0x15600}, {4, 1, 0x15800},
}};

static uint8_t pattern(uint32_t mip, uint32_t layer, size_t index)
{
    return static_cast<uint8_t>(29 * layer + 31 * mip + 17 * index + (index >> 8));
}

static size_t tiled_offset(size_t x, size_t y, size_t width_bytes, size_t gob_height)
{
    const size_t gobs_per_row = (width_bytes + 63) / 64;
    const size_t gob_y = y / 8;
    const size_t group = (gob_y / gob_height * gobs_per_row + x / 64) * gob_height;
    const size_t gob = group + gob_y % gob_height;
    const size_t swizzled = ((x & 32) << 3) | ((y & 4) << 5) |
        ((x & 16) << 2) | ((y & 3) << 4) | (x & 15);
    return gob * 512 + swizzled;
}

struct Trace {
    const Mip *mips;
    size_t count;
    uint32_t layers, mip = 0, first = 0, calls = 0, fail_call = 0;
    size_t peak = 0;
};

static VkResult verify(void *opaque, VkExtent2D extent, uint32_t mip, uint32_t first,
    uint32_t layers, const uint8_t *linear, size_t bytes)
{
    auto &trace = *static_cast<Trace *>(opaque);
    assert(mip == trace.mip && mip < trace.count && first == trace.first && layers);
    const auto edge = trace.mips[mip].edge;
    assert(extent.width == edge && extent.height == edge);
    const auto rows = (edge + 3) / 4;
    const size_t layer_bytes = static_cast<size_t>(rows) * rows * 16;
    assert(bytes == layers * layer_bytes && first + layers <= trace.layers);
    assert(bytes <= 2 * 1024 * 1024 || layers == 1);
    for (uint32_t l = 0; l < layers; ++l)
        for (size_t i = 0; i < layer_bytes; ++i)
            assert(linear[l * layer_bytes + i] == pattern(mip, first + l, i));
    trace.peak = std::max(trace.peak, bytes);
    trace.first += layers;
    if (trace.first == trace.layers) {
        trace.first = 0;
        ++trace.mip;
    }
    ++trace.calls;
    return trace.calls == trace.fail_call ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

static void verify_layout(uint32_t target, uint32_t depth, uint32_t layers, uint32_t format,
    const Mip *mips, size_t mip_count, size_t stride, uint32_t expected_calls, size_t expected_peak)
{
    NvnFrontTextureConfig config{};
    config.target = target;
    config.width = config.height = mips[0].edge;
    config.depth = depth;
    config.levels = static_cast<uint32_t>(mip_count);
    config.format = format;
    uint64_t guest_bytes = 0, alignment = 0;
    assert(nvn_guest_texture_layout(&config, &guest_bytes, &alignment) && guest_bytes == stride * layers);
    std::vector<uint8_t> tiled(static_cast<size_t>(guest_bytes), 0xff);
    for (uint32_t l = 0; l < layers; ++l) {
        for (uint32_t m = 0; m < mip_count; ++m) {
            const auto &level = mips[m];
            const size_t rows = (level.edge + 3) / 4;
            const size_t row_bytes = rows * 16;
            for (size_t y = 0; y < rows; ++y)
                for (size_t x = 0; x < row_bytes; ++x)
                    tiled[l * stride + level.offset + tiled_offset(x, y, row_bytes, level.gob_height)] =
                        pattern(m, l, y * row_bytes + x);
        }
    }
    Trace trace{mips, mip_count, layers};
    assert(nvn_guest_untile_compressed(&config, tiled.data(), tiled.size() - 1, verify, &trace) ==
        VK_ERROR_INITIALIZATION_FAILED);
    assert(nvn_guest_untile_compressed(&config, tiled.data(), tiled.size() + 1, verify, &trace) ==
        VK_ERROR_INITIALIZATION_FAILED);
    assert(trace.calls == 0);
    assert(nvn_guest_untile_compressed(&config, tiled.data(), tiled.size(), verify, &trace) == VK_SUCCESS);
    assert(trace.mip == mip_count && trace.first == 0 && trace.calls == expected_calls && trace.peak == expected_peak);
    if (expected_calls > 2) {
        Trace failure{mips, mip_count, layers};
        failure.fail_call = 2;
        assert(nvn_guest_untile_compressed(&config, tiled.data(), tiled.size(), verify, &failure) ==
            VK_ERROR_DEVICE_LOST);
        assert(failure.calls == 2);
    }
    config.samples = 2;
    assert(nvn_guest_untile_compressed(&config, tiled.data(), tiled.size(), verify, &trace) ==
        VK_ERROR_FORMAT_NOT_SUPPORTED);
    config.samples = 0;
    config.format = NVN_FORMAT_RGBA8;
    assert(nvn_guest_untile_compressed(&config, tiled.data(), tiled.size(), verify, &trace) ==
        VK_ERROR_FORMAT_NOT_SUPPORTED);
    std::printf("PASS %ux%ux%u levels=%zu guest=%#llx callbacks=%u peak=%zu\n",
        config.width, config.height, layers, mip_count, static_cast<unsigned long long>(guest_bytes),
        expected_calls, expected_peak);
}

int main()
{
    verify_layout(NVN_TEXTURE_TARGET_2D_ARRAY, 372, 372, NVN_FORMAT_BPTC_UFLOAT, cube_mips.data(),
        cube_mips.size(), 0x16000, 20, 2 * 1024 * 1024);
    verify_layout(NVN_TEXTURE_TARGET_CUBEMAP, 1, 6, NVN_FORMAT_BPTC_UFLOAT, cube_mips.data(),
        cube_mips.size(), 0x16000, 7, 6 * 65536);
    constexpr Mip large{2048, 16, 0};
    verify_layout(NVN_TEXTURE_TARGET_2D, 1, 1, NVN_FORMAT_BPTC_UNORM, &large, 1,
        4 * 1024 * 1024, 1, 4 * 1024 * 1024);
    std::puts("PASS exact Switch 2 order, padded layer stride, complete mip coverage and early callback failure");
}
