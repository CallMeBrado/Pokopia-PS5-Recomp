#define nvn_front_get_descriptor_pool mock_nvn_front_get_descriptor_pool
#define nvn_front_get_texture_descriptor mock_nvn_front_get_texture_descriptor
#define nvn_front_get_sampler_descriptor mock_nvn_front_get_sampler_descriptor
#include "nvn_replay.c"
#include <assert.h>

static PokopiaVkTexture source = {
    .format = VK_FORMAT_BC7_UNORM_BLOCK,
    .sample_alias_format = VK_FORMAT_BC7_SRGB_BLOCK,
    .sample_alias_view = (VkImageView)(uintptr_t)1,
};
static uint32_t view_format = 0x4e;

bool nvn_front_get_descriptor_pool(NvnFront *front, uint64_t address, bool sampler, NvnFrontDescriptorPoolInfo *info)
{
    (void)front;
    if (address != (sampler ? 300 : 200))
        return false;
    *info = (NvnFrontDescriptorPoolInfo){.device = 400, .count = 20};
    return true;
}

bool nvn_front_get_texture_descriptor(NvnFront *front, uint64_t address, uint32_t index,
                                     NvnFrontTextureDescriptorInfo *info)
{
    (void)front;
    assert(address == 200 && index == 13);
    *info = (NvnFrontTextureDescriptorInfo){
        .texture = {.native = &source, .create_info.config = {
            .device = 400, .target = 1, .depth = 1, .levels = 1, .format = 0x4d,
        }},
        .has_view = true,
        .view = {0, 0, 1, 0, 0, 1, 1, 47, 0, 0, 0, 0, 2, 3, 4, 5},
    };
    memcpy(info->view + 8, &view_format, sizeof view_format);
    return true;
}

bool nvn_front_get_sampler_descriptor(NvnFront *front, uint64_t address, uint32_t index, NvnFrontSamplerInfo *info)
{
    (void)front;
    assert(address == 300 && index == 5);
    *info = (NvnFrontSamplerInfo){.min_filter = 3, .mag_filter = 1};
    return true;
}

VkFormat nvn_guest_texture_vk_format(uint32_t format)
{
    return format == 0x4e ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_UNDEFINED;
}

int main(void)
{
    DrawAbi abi;
    assert(draw_abi("cb3=s0b4", NVN_SHADER_VERTEX, &abi) && abi.uniform0 &&
           abi.fragment_slots[0] == -1 && abi.fragment_slots[1] == -1);
    assert(draw_abi("cb3=s0b22 texTexture2D[c-1:8]=s2b128", NVN_SHADER_FRAGMENT, &abi) && abi.uniform0 &&
           abi.fragment_slots[0] == 0 && abi.fragment_slots[1] == -1);
    assert(draw_abi("cb3=s0b22 texTexture2D[c-1:10]=s2b128 texTexture2D[c-1:8]=s2b129",
                    NVN_SHADER_FRAGMENT, &abi) && abi.uniform0 &&
           abi.fragment_slots[0] == 1 && abi.fragment_slots[1] == 0);

    const Draw draw = {.texture_pool = 200, .sampler_pool = 300,
                       .fragment_textures = {(UINT64_C(5) << 20) | 13}};
    PokopiaLiveDrawSample sample = {0};
    assert(resolve_fragment_sample(NULL, 400, &draw, 0, &sample));
    assert(sample.view_format == VK_FORMAT_BC7_SRGB_BLOCK && sample.texture == &source && sample.sampler.mag_filter == 1);

    view_format = 0x38;
    assert(!resolve_fragment_sample(NULL, 400, &draw, 0, &sample));
    view_format = 0x4e;
    source.sample_alias_view = VK_NULL_HANDLE;
    assert(!resolve_fragment_sample(NULL, 400, &draw, 0, &sample));
    puts("PASS loading ABI and guest BC7 sRGB alias resolution, including invalid alias rejection");
}
bool nvn_guest_texture_layout(const NvnFrontTextureConfig *config, uint64_t *size, uint64_t *alignment)
{
    (void)config; (void)size; (void)alignment;
    return false;
}
