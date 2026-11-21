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
#include "live_draw.c"
#include "../title-reference-check/composite-data.h"

#include <assert.h>

enum {
    DEVICE = 1, ENCODER, PROGRAM, TARGET, DEPTH, TEXTURES, SAMPLERS, VUBO, FUBO,
    WIDTH = 80, HEIGHT = 45, VERTICES_ADDRESS = 10, INDICES_ADDRESS = 11,
};

static PokopiaVkProgram *shader_program;
static size_t command_used;
static unsigned callbacks, modules_created, modules_destroyed, pipelines;
static const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
static const uint8_t vertices[4][24] = {0};
static PokopiaVkTexture color = {.format = VK_FORMAT_R8G8B8A8_SRGB, .aspects = VK_IMAGE_ASPECT_COLOR_BIT};
static PokopiaVkTexture depth = {.format = VK_FORMAT_D24_UNORM_S8_UINT,
    .aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
    .backing.image = (VkImage)(uintptr_t)2, .view = (VkImageView)(uintptr_t)2,
    .extent = {WIDTH, HEIGHT}, .array_layers = 1,
    .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    .stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT};
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
    return (size == 256 && address == VUBO) || (size == sizeof vertices && address == VERTICES_ADDRESS) ||
        (size == sizeof indices && address == INDICES_ADDRESS);
}

static const void *fixture_read_guest(void *opaque, uint64_t address, size_t size)
{
    (void)opaque;
    if (address == VERTICES_ADDRESS) return vertices;
    if (address == INDICES_ADDRESS) return indices;
    assert(address == VUBO && size == 256);
    return vs_composite;
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
    assert(info->pCode[0] == 0x07230203 && info->codeSize == 2624u);
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

static VKAPI_ATTR VkResult VKAPI_CALL fixture_pipeline(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *out)
{
    (void)device;
    (void)cache;
    (void)allocator;
    (void)out;
    assert(count == 1 && infos->stageCount == 1 && infos->pStages[0].stage == VK_SHADER_STAGE_VERTEX_BIT);
    assert(!infos->pColorBlendState->attachmentCount && !infos->pColorBlendState->pAttachments);
    const VkPipelineRenderingCreateInfo *rendering = infos->pNext;
    assert(!rendering->colorAttachmentCount && !rendering->pColorAttachmentFormats &&
           rendering->depthAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT &&
           rendering->stencilAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT);
    assert(infos->pDepthStencilState->depthTestEnable && infos->pDepthStencilState->depthWriteEnable &&
           infos->pDepthStencilState->depthCompareOp == VK_COMPARE_OP_ALWAYS);
    ++pipelines;
    return VK_SUCCESS;
}

static bool fixture_draw(void *opaque, const PokopiaLiveDraw *draw)
{
    (void)opaque;
    assert(!draw->target && draw->depth_target == &depth);
    assert(draw->extent.width == WIDTH && draw->extent.height == HEIGHT);
    assert(!draw->vertex_count && draw->index_count == 6 && draw->vertex_stride == 24);
    assert(draw->vertex_uniform.data == vs_composite && draw->vertex_uniform.bytes == 256);
    assert(draw->vertices.data == vertices && draw->indices.data == indices);
    assert(!draw->fragment_uniform.data && !draw->fragment_samples[0].texture);
    assert(draw->vertex_shader.module && !draw->fragment_shader.module && !draw->fragment_shader.hash);
    const PokopiaVkAdapter adapter = {.properties.limits = {
        .maxFramebufferWidth = 1920, .maxFramebufferHeight = 1080, .maxUniformBufferRange = 65536,
    }};
    size_t used = 0;
    assert(supported(&adapter, draw, &used) && used == sizeof vertices);
    DrawFunctions calls = {.CreateGraphicsPipelines = fixture_pipeline};
    DrawWork work = {0};
    assert(make_pipeline(VK_NULL_HANDLE, &calls, draw, &work) == VK_SUCCESS);
    ++callbacks;
    return true;
}

int main(void)
{
    NvnShaderCacheStatus status;
    NvnShaderCache *cache = nvn_shader_cache_open("../shader-cache-655360", &status);
    assert(cache && status == NVN_SHADER_CACHE_OK);
    PokopiaVkProgram program = {.create = fixture_module, .destroy = fixture_module_destroy};
    shader_program = &program;
    NvnCachedShader cached = {0};
    assert(nvn_shader_cache_lookup_key(cache, "821538a4398f85a7", NVN_SHADER_VERTEX, &cached) ==
           NVN_SHADER_CACHE_OK);
    ProgramStage *vertex = &program.stages[NVN_SHADER_VERTEX];
    memcpy(vertex->hash, cached.hash, sizeof vertex->hash);
    vertex->cache = cache;
    vertex->resources = cached.resources;
    cached.resources = NULL;
    nvn_cached_shader_release(&cached);

    NvnReplay replay = {.config = {.read_guest = fixture_read_guest, .draw = fixture_draw}};
    Recording recording = {
        .encoder = ENCODER, .device = DEVICE, .depth_target = DEPTH, .program = {PROGRAM},
        .viewport = {0, 0, WIDTH, HEIGHT}, .scissor = {0, 0, WIDTH, HEIGHT}, .depth_range = {0, 1},
        .viewport_set = true, .scissor_set = true, .raster_bound = 15, .polygon_mode = 2, .color_write_mask = 15,
        .depth_test = true, .depth_write = true, .depth_func = 7,
        .attribute_count = 3, .stream_count = 1, .streams = {{.stride = 24}},
        .attributes = {{.format = 34, .enabled = true}, {.format = 37, .offset = 12, .enabled = true},
                       {.format = 22, .offset = 16, .enabled = true}},
    };
    replay.recordings = &recording;
    recording.uniform[0][0] = (BufferRange){VUBO, 256};
    recording.vertex[0] = (BufferRange){VERTICES_ADDRESS, sizeof vertices};
    uint64_t args[32] = {ENCODER, 4, 1, 6, INDICES_ADDRESS};
    assert(record_draw(&replay, args, true, false, false) == NVN_FRONT_BOUNDARY && !command_used);
    recording.depth_func = 8;
    assert(record_draw(&replay, args, true, false, false) == NVN_FRONT_HANDLED);
    assert(recording.count == 1 && command_used == 24 && recording.command_segment);
    const Draw *draw = &recording.operations[0].data.draw;
    assert(!draw->target && draw->depth_target == DEPTH);
    assert(run_draw(&replay, DEVICE, draw, false) == NVN_FRONT_HANDLED && !modules_created && !callbacks);
    assert(run_draw(&replay, DEVICE, draw, true) == NVN_FRONT_HANDLED);
    assert(modules_created == 1 && modules_destroyed == 1 && callbacks == 1 && pipelines == 1);
    free(recording.operations);
    release_stages(program.stages);
    nvn_shader_cache_close(cache);
    puts("PASS depth-only NVN indexed record and execute with one vertex stage, zero color attachments and D24S8 compare ALWAYS");
    return 0;
}

VkFormat nvn_guest_texture_vk_format(uint32_t format) {
    switch (format) {
    case 34: return VK_FORMAT_R32G32B32_SFLOAT;
    case 37: return VK_FORMAT_R8G8B8A8_UNORM;
    case 22: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}
uint32_t nvn_guest_buffer_texel_bytes(uint32_t format) { (void)format; return 0; }
bool nvn_guest_texture_layout(const NvnFrontTextureConfig *config, uint64_t *size, uint64_t *alignment)
{ (void)config; (void)size; (void)alignment; return false; }


