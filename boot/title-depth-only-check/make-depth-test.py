from pathlib import Path

text = Path('../title-reference-check/test-composite-route.c').read_text()
text = text.replace('#include "vk_program.c"', '#include "vk_program.c"\n#include "live_draw.c"')
text = text.replace('static unsigned callbacks, modules_created, modules_destroyed;',
                    'static unsigned callbacks, modules_created, modules_destroyed, pipelines;\n'
                    'static const uint16_t indices[] = {0, 1, 2, 0, 2, 3};\n'
                    'static const uint8_t vertices[4][24] = {0};')
text = text.replace('WIDTH = 1920, HEIGHT = 1080,','WIDTH = 80, HEIGHT = 45, VERTICES_ADDRESS = 10, INDICES_ADDRESS = 11,')
text = text.replace('''static PokopiaVkTexture depth = {.format = VK_FORMAT_D24_UNORM_S8_UINT,
    .aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};''', '''static PokopiaVkTexture depth = {.format = VK_FORMAT_D24_UNORM_S8_UINT,
    .aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
    .backing.image = (VkImage)(uintptr_t)2, .view = (VkImageView)(uintptr_t)2,
    .extent = {WIDTH, HEIGHT}, .array_layers = 1,
    .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    .stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT};''')
text = text.replace('return size == 512 && (address == VUBO || address == FUBO);',
                    'return (size == 256 && address == VUBO) || (size == sizeof vertices && address == VERTICES_ADDRESS) ||\n'
                    '        (size == sizeof indices && address == INDICES_ADDRESS);')
text = text.replace('    assert(size == 512 && (address == VUBO || address == FUBO));\n'
                    '    return address == VUBO ? vs_composite : fs_composite;',
                    '    if (address == VERTICES_ADDRESS) return vertices;\n'
                    '    if (address == INDICES_ADDRESS) return indices;\n'
                    '    assert(address == VUBO && size == 256);\n'
                    '    return vs_composite;')
text = text.replace('info->codeSize == (modules_created == 0 ? 3052u : 24172u)', 'info->codeSize == 2624u')
i, j = text.index('static bool fixture_draw'), text.index('int main')
text = text[:i] + '''static VKAPI_ATTR VkResult VKAPI_CALL fixture_pipeline(VkDevice device, VkPipelineCache cache, uint32_t count,
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

''' + text[j:]
i, j = text.index('    uint8_t code[768]'), text.index('    NvnReplay replay =')
text = text[:i] + '''    PokopiaVkProgram program = {.create = fixture_module, .destroy = fixture_module_destroy};
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

''' + text[j:]
i, j = text.index('    Recording recording'), text.index('    free(recording.operations);')
text = text[:i] + '''    Recording recording = {
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
''' + text[j:]
text = text.replace('PASS NVN composite record and execute through authentic packed shader data, vertexless C1 and both CB5 ranges',
                    'PASS depth-only NVN indexed record and execute with one vertex stage, zero color attachments and D24S8 compare ALWAYS')
text = text.replace('VkFormat nvn_guest_texture_vk_format(uint32_t format) { (void)format; return VK_FORMAT_UNDEFINED; }',
                    '''VkFormat nvn_guest_texture_vk_format(uint32_t format) {
    switch (format) {
    case 34: return VK_FORMAT_R32G32B32_SFLOAT;
    case 37: return VK_FORMAT_R8G8B8A8_UNORM;
    case 22: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}
uint32_t nvn_guest_buffer_texel_bytes(uint32_t format) { (void)format; return 0; }''')
text = text.replace('#include "composite-data.h"', '#include "../title-reference-check/composite-data.h"')
Path('test-depth-only-route.c').write_text(text)
