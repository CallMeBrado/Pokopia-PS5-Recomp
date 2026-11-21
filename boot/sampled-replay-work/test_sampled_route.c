#define nvn_front_charge_command_memory mock_nvn_front_charge_command_memory
#define nvn_front_get_descriptor_pool mock_nvn_front_get_descriptor_pool
#define nvn_front_get_texture_descriptor mock_nvn_front_get_texture_descriptor
#define nvn_front_get_sampler_descriptor mock_nvn_front_get_sampler_descriptor
#include "nvn_replay.c"
#include <assert.h>

static uint8_t encoder[0xa9];
static size_t metered_commands, metered_control;
static uint32_t seen_texture, seen_sampler;
static uint8_t test_view[16];
static PokopiaVkTexture source;

static const void *read_test(void *opaque, uint64_t address, size_t bytes)
{
    (void)opaque;
    assert(address == 100 && bytes == sizeof encoder);
    return encoder;
}

bool nvn_front_charge_command_memory(NvnFront *front, uint64_t address, size_t cmd, size_t ctl)
{
    (void)front;
    assert(address == 100);
    metered_commands += cmd;
    metered_control += ctl;
    return true;
}

bool nvn_front_get_descriptor_pool(NvnFront *front, uint64_t address, bool sampler,
                                   NvnFrontDescriptorPoolInfo *info)
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
    assert(address == 200);
    seen_texture = index;
    *info = (NvnFrontTextureDescriptorInfo){
        .texture = {.native = &source, .create_info.config = {.device = 400, .target = 1, .depth = 1, .levels = 1}},
        .has_view = true,
    };
    memcpy(info->view, test_view, sizeof test_view);
    return true;
}

bool nvn_front_get_sampler_descriptor(NvnFront *front, uint64_t address, uint32_t index, NvnFrontSamplerInfo *info)
{
    (void)front;
    assert(address == 300);
    seen_sampler = index;
    *info = (NvnFrontSamplerInfo){.min_filter = 3, .mag_filter = 1};
    return true;
}

int main(void)
{
    DrawAbi abi;
    assert(draw_abi("cb3=s0b4", NVN_SHADER_VERTEX, &abi) && abi.uniform0 && abi.fragment_texture == -1);
    assert(draw_abi("cb3=s0b22", NVN_SHADER_FRAGMENT, &abi) && abi.uniform0 && abi.fragment_texture == -1);
    assert(draw_abi("texTexture2D[c-1:8]=s2b128", NVN_SHADER_FRAGMENT, &abi) &&
           !abi.uniform0 && abi.fragment_texture == 0);
    assert(draw_abi("texTexture2D[c-1:10]=s2b128", NVN_SHADER_FRAGMENT, &abi) &&
           !abi.uniform0 && abi.fragment_texture == 1);
    assert(!draw_abi("texTexture2D[c-1:9]=s2b128", NVN_SHADER_FRAGMENT, &abi));
    assert(!draw_abi("texTexture2D[c-1:8]=s2b129", NVN_SHADER_FRAGMENT, &abi));
    assert(!draw_abi("texTexture2D[c-1:8]=s2b128", NVN_SHADER_VERTEX, &abi));
    assert(!draw_abi("texTexture2D[c-1:8x2]=s2b128", NVN_SHADER_FRAGMENT, &abi));

    Recording rec = {.encoder = 100, .texture_pool = 200, .sampler_pool = 300};
    NvnReplay replay = {.config.read_guest = read_test, .recordings = &rec};
    uint64_t args[32] = {100, NVN_SHADER_FRAGMENT, 1, (UINT64_C(5) << 20) | 13};
    assert(bind_texture(&replay, args) == NVN_FRONT_HANDLED);
    assert(metered_commands == 16 && metered_control == 0);
    IndexedDraw draw = {.texture_pool = rec.texture_pool, .sampler_pool = rec.sampler_pool};
    memcpy(draw.fragment_textures, rec.textures[NVN_SHADER_FRAGMENT], sizeof draw.fragment_textures);
    encoder[0x13] = 8;
    args[3] = 17;
    assert(bind_texture(&replay, args) == NVN_FRONT_HANDLED);
    assert(metered_commands == 36 && metered_control == 0);

    PokopiaLiveDrawSample sample;
    assert(resolve_fragment_sample(NULL, 400, &draw, 1, &sample));
    assert(seen_texture == 13 && seen_sampler == 5 && sample.texture == &source &&
           sample.sampler.min_filter == 3);
    assert(!resolve_fragment_sample(NULL, 401, &draw, 1, &sample));
    test_view[7] = 8;
    assert(!resolve_fragment_sample(NULL, 400, &draw, 1, &sample));
    puts("PASS sampled replay parses cached ABI, meters BindTexture, and resolves the recorded guest handle");
    return 0;
}

VkFormat nvn_guest_texture_vk_format(uint32_t format) { (void)format; abort(); }
bool nvn_guest_texture_layout(const NvnFrontTextureConfig *c, uint64_t *size, uint64_t *alignment)
{ (void)c; (void)size; (void)alignment; abort(); }
