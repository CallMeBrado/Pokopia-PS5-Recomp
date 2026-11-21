#define nvn_front_get_device_modes fixture_device_modes
#define nvn_front_get_program fixture_get_program
#define nvn_front_get_texture fixture_get_texture
#define nvn_front_get_descriptor_pool fixture_get_descriptor_pool
#define nvn_front_get_texture_descriptor fixture_get_texture_descriptor
#define nvn_front_get_sampler_descriptor fixture_get_sampler_descriptor
#define nvn_front_resolve_pool fixture_resolve_pool
#define nvn_front_get_command_buffer fixture_get_command_buffer
#define nvn_front_charge_command_memory fixture_charge_command_memory
#include "nvn_replay.c"
#include "vk_program.c"
#include "composite-data.h"

#include <assert.h>

_Static_assert(sizeof(Operation) == 952, "Composite must preserve the compact operation stride");

enum {
    DEVICE = 1, ENCODER, PROGRAM, TARGET, DEPTH, TEXTURES, SAMPLERS, VUBO, FUBO,
    WIDTH = 1920, HEIGHT = 1080,
};

static PokopiaVkProgram *shader_program;
static size_t command_used;
static unsigned callbacks, modules_created, modules_destroyed;
static PokopiaVkTexture color = {.format = VK_FORMAT_R8G8B8A8_SRGB, .aspects = VK_IMAGE_ASPECT_COLOR_BIT};
static PokopiaVkTexture depth = {.format = VK_FORMAT_D24_UNORM_S8_UINT,
    .aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
static PokopiaVkTexture source = {.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32, .aspects = VK_IMAGE_ASPECT_COLOR_BIT};

bool fixture_device_modes(NvnFront *front, uint64_t address, NvnFrontDeviceModes *out)
{
    (void)front;
    assert(address == DEVICE);
    *out = (NvnFrontDeviceModes){NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT, NVN_FRONT_DEPTH_NEAR_IS_ZERO};
    return true;
}

bool fixture_get_program(NvnFront *front, uint64_t address, NvnFrontProgramInfo *out)
{
    (void)front;
    if (address != PROGRAM)
        return false;
    *out = (NvnFrontProgramInfo){.device = DEVICE, .native = shader_program, .linked = true};
    return true;
}

static NvnFrontTextureInfo texture_info(PokopiaVkTexture *texture, uint32_t format)
{
    return (NvnFrontTextureInfo){.native = texture, .create_info.config = {
        .device = DEVICE, .target = TARGET_2D, .depth = 1, .levels = 1, .format = format, .width = WIDTH, .height = HEIGHT,
    }};
}

bool fixture_get_texture(NvnFront *front, uint64_t address, NvnFrontTextureInfo *out)
{
    (void)front;
    if (address == TARGET)
        *out = texture_info(&color, 0x38);
    else if (address == DEPTH)
        *out = texture_info(&depth, FORMAT_D24S8);
    else
        return false;
    return true;
}

bool fixture_get_descriptor_pool(NvnFront *front, uint64_t address, bool sampler, NvnFrontDescriptorPoolInfo *out)
{
    (void)front;
    assert(address == (sampler ? SAMPLERS : TEXTURES));
    *out = (NvnFrontDescriptorPoolInfo){.device = DEVICE};
    return true;
}

bool fixture_get_texture_descriptor(NvnFront *front, uint64_t address, uint32_t index,
                                    NvnFrontTextureDescriptorInfo *out)
{
    (void)front;
    assert(address == TEXTURES && index == 13);
    *out = (NvnFrontTextureDescriptorInfo){.texture = texture_info(&source, 0x3f)};
    return true;
}

bool fixture_get_sampler_descriptor(NvnFront *front, uint64_t address, uint32_t index, NvnFrontSamplerInfo *out)
{
    (void)front;
    assert(address == SAMPLERS && index == 5);
    *out = (NvnFrontSamplerInfo){.min_filter = 3, .mag_filter = 1, .wrap = {7, 7, 7}, .max_anisotropy = 1};
    return true;
}

bool fixture_resolve_pool(const NvnFront *front, uint64_t device, uint64_t address, size_t size, NvnFrontPoolHit *hit)
{
    (void)front;
    (void)hit;
    assert(device == DEVICE);
    return size == 512 && (address == VUBO || address == FUBO);
}

static const void *fixture_read_guest(void *opaque, uint64_t address, size_t size)
{
    (void)opaque;
    assert(size == 512 && (address == VUBO || address == FUBO));
    return address == VUBO ? vs_composite : fs_composite;
}

bool fixture_get_command_buffer(NvnFront *front, uint64_t address, NvnFrontCommandBufferInfo *out)
{
    (void)front;
    assert(address == ENCODER);
    *out = (NvnFrontCommandBufferInfo){.device = DEVICE, .command_size = 128, .command_used = command_used,
                                     .control_size = 128};
    return true;
}

bool fixture_charge_command_memory(NvnFront *front, uint64_t encoder, size_t command, size_t control)
{
    (void)front;
    assert(encoder == ENCODER && !control);
    command_used += command;
    return true;
}

static VKAPI_ATTR VkResult VKAPI_CALL fixture_module(VkDevice device, const VkShaderModuleCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkShaderModule *out)
{
    (void)device;
    (void)allocator;
    assert(info->pCode[0] == 0x07230203 && info->codeSize == (modules_created == 0 ? 3052u : 24172u));
    *out = (VkShaderModule)(uintptr_t)++modules_created;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL fixture_module_destroy(VkDevice device, VkShaderModule module,
    const VkAllocationCallbacks *allocator)
{
    (void)device;
    (void)allocator;
    assert((uintptr_t)module == ++modules_destroyed);
}

static bool fixture_draw(void *opaque, const PokopiaLiveDraw *draw)
{
    (void)opaque;
    assert(draw->target == &color && draw->depth_target == &depth);
    assert(draw->extent.width == WIDTH && draw->extent.height == HEIGHT);
    assert(draw->vertex_count == 3 && !draw->index_count && !draw->vertex_stride);
    assert(draw->vertex_constants.bytes == sizeof c1_composite);
    assert(!memcmp(draw->vertex_constants.data, c1_composite, sizeof c1_composite));
    assert(draw->vertex_uniform.data == vs_composite && draw->vertex_uniform.bytes == sizeof vs_composite);
    assert(draw->fragment_uniform.data == fs_composite && draw->fragment_uniform.bytes == sizeof fs_composite);
    assert(draw->fragment_samples[0].texture == &source && !draw->fragment_samples[1].texture);
    assert(draw->vertex_shader.module && draw->fragment_shader.module);
    assert(!strcmp(draw->vertex_shader.entry, "main") && !strcmp(draw->fragment_shader.entry, "main"));
    ++callbacks;
    return true;
}

static void read_exact(const char *name, void *out, size_t size)
{
    FILE *file = fopen(name, "rb");
    assert(file && fread(out, 1, size, file) == size && fgetc(file) == EOF);
    fclose(file);
}

int main(void)
{
    NvnShaderCacheStatus status;
    NvnShaderCache *cache = nvn_shader_cache_open("../shader-cache-655360", &status);
    assert(cache && status == NVN_SHADER_CACHE_OK);
    uint8_t code[768], control[224];
    read_exact("composite-missing/code_caa3cba58abf8cc9.bin", code, sizeof code);
    read_exact("composite-missing/ctrl_caa3cba58abf8cc9.bin", control, sizeof control);
    NvnFrontShaderData guest_shader = {.code = code, .control = control};
    assert(nvn_shader_control_info(control, sizeof control, &guest_shader.parsed));

    PokopiaVkProgram program = {.create = fixture_module, .destroy = fixture_module_destroy};
    shader_program = &program;
    assert(pokopia_vk_program_link(&program, cache, &guest_shader, 1, stdout) == NVN_FRONT_PROGRAM_LINKED);
    memset(code, 0, sizeof code); /* Retained C1 must survive guest shader storage reuse. */

    NvnCachedShader cached = {0};
    assert(nvn_shader_cache_lookup_key(cache, POKOPIA_COMPOSITE_FRAGMENT, NVN_SHADER_FRAGMENT, &cached) ==
           NVN_SHADER_CACHE_OK);
    ProgramStage *fragment = &program.stages[NVN_SHADER_FRAGMENT];
    memcpy(fragment->hash, cached.hash, sizeof fragment->hash);
    fragment->cache = cache;
    fragment->resources = cached.resources;
    cached.resources = NULL;
    nvn_cached_shader_release(&cached);

    NvnReplay replay = {.config = {.read_guest = fixture_read_guest, .draw = fixture_draw}};
    Recording recording = {
        .encoder = ENCODER, .device = DEVICE, .targets = {TARGET}, .target_count = 1, .depth_target = DEPTH,
        .texture_pool = TEXTURES, .sampler_pool = SAMPLERS, .program = {PROGRAM, PROGRAM},
        .viewport = {0, 0, WIDTH, HEIGHT}, .scissor = {0, 0, WIDTH, HEIGHT}, .depth_range = {0, 1},
        .viewport_set = true, .scissor_set = true, .raster_bound = 15, .polygon_mode = 2, .color_write_mask = 15,
    };
    replay.recordings = &recording;
    recording.uniform[0][0] = (BufferRange){0x111, 256};
    recording.uniform[1][0] = (BufferRange){0x222, 1280};
    recording.uniform[0][2] = (BufferRange){VUBO, sizeof vs_composite};
    recording.uniform[1][2] = (BufferRange){FUBO, sizeof fs_composite};
    recording.textures[NVN_SHADER_FRAGMENT][0] = (UINT64_C(5) << 20) | 13;
    uint64_t args[32] = {ENCODER, 4, 0, 2};
    assert(record_draw(&replay, args, false, false, false) == NVN_FRONT_BOUNDARY && !command_used);
    args[3] = 3;
    assert(record_draw(&replay, args, false, false, false) == NVN_FRONT_HANDLED);
    assert(recording.count == 1 && command_used == 16 && recording.command_segment);
    const Draw *draw = &recording.operations[0].data.draw;
    assert(draw->vertex.address == 0 && draw->uniform[0].address == VUBO && draw->uniform[1].address == FUBO);
    assert(run_draw(&replay, DEVICE, draw, false) == NVN_FRONT_HANDLED && !modules_created && !callbacks);
    assert(run_draw(&replay, DEVICE, draw, true) == NVN_FRONT_HANDLED);
    assert(modules_created == 2 && modules_destroyed == 2 && callbacks == 1);
    free(recording.operations);
    release_stages(program.stages);
    nvn_shader_cache_close(cache);
    puts("PASS NVN composite record and execute through authentic packed shader data, vertexless C1 and both CB5 ranges");
    return 0;
}

VkFormat nvn_guest_texture_vk_format(uint32_t format) { (void)format; return VK_FORMAT_UNDEFINED; }
bool nvn_guest_texture_layout(const NvnFrontTextureConfig *config, uint64_t *size, uint64_t *alignment)
{ (void)config; (void)size; (void)alignment; return false; }



