typedef struct {
    float x, y, z;
    uint8_t rgba[4];
    float u, v;
} Vertex;

static const char *vs_key = "821538a4398f85a7", *fs_key = "42c917e6eb2c6411";

static const Vertex vertices[] = {
    {0, 0, 0, {64, 128, 192, 128}, 0, 0}, {0, HEIGHT, 0, {64, 128, 192, 128}, 0, 1},
    {WIDTH, HEIGHT, 0, {64, 128, 192, 128}, 1, 1}, {WIDTH, 0, 0, {64, 128, 192, 128}, 1, 0},
};
static const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
static const float ubo[64] = {2.0f / WIDTH, -2.0f / HEIGHT, -1, 1};
static const VkRect2D full = {{0, 0}, {WIDTH, HEIGHT}};

static PokopiaLiveDraw draw_state(PokopiaVkTexture *target, PokopiaVkTexture *depth, VkShaderModule vs, VkShaderModule fs)
{
    return (PokopiaLiveDraw){
        .target = target, .depth_target = depth, .extent = {WIDTH, HEIGHT},
        .vertex_shader = {vs, "main", vs_key}, .fragment_shader = {fs, "main", fs_key},
        .vertices = {vertices, sizeof vertices}, .indices = {indices, sizeof indices},
        .vertex_uniform = {ubo, sizeof ubo}, .vertex_stride = sizeof(Vertex),
        .attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12},
                       {2, 0, VK_FORMAT_R32G32_SFLOAT, 16}},
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .index_type = VK_INDEX_TYPE_UINT16,
        .viewport = {0, HEIGHT, WIDTH, -HEIGHT, 0, 1}, .scissor = full,
        .color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT,
        .polygon_mode = VK_POLYGON_MODE_FILL, .index_count = 6, .instance_count = 1,
        .blend_enable = true,
        .blend = {.src_rgb = 5, .dst_rgb = 2, .rgb_equation = 1,
                  .src_alpha = 5, .dst_alpha = 2, .alpha_equation = 1},
    };
}

static VKAPI_ATTR VkResult VKAPI_CALL check_pipeline(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *pipeline)
{
    (void)device;
    (void)cache;
    (void)allocator;
    (void)pipeline;
    assert(count == 1 && infos->stageCount == 2);
    const VkPipelineColorBlendAttachmentState *blend = infos->pColorBlendState->pAttachments;
    assert(blend->blendEnable && blend->colorWriteMask == 7);
    assert(blend->srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA && blend->dstColorBlendFactor == VK_BLEND_FACTOR_ONE &&
           blend->srcAlphaBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA && blend->dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
           blend->colorBlendOp == VK_BLEND_OP_ADD && blend->alphaBlendOp == VK_BLEND_OP_ADD);
    const VkPipelineRenderingCreateInfo *rendering = infos->pNext;
    assert(rendering->colorAttachmentCount == 1 &&
           rendering->pColorAttachmentFormats[0] == VK_FORMAT_R16G16B16A16_UNORM);
    return VK_SUCCESS;
}

static void validate(void)
{
    const PokopiaVkAdapter adapter = {.properties.limits = {
        .maxFramebufferWidth = WIDTH, .maxFramebufferHeight = HEIGHT, .maxUniformBufferRange = 65536,
    }};
    PokopiaVkTexture target = {
        .format = VK_FORMAT_R16G16B16A16_UNORM, .extent = {WIDTH, HEIGHT}, .array_layers = 1,
        .aspects = VK_IMAGE_ASPECT_COLOR_BIT, .stages = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .backing.image = (VkImage)(uintptr_t)1, .view = (VkImageView)(uintptr_t)1,
    };
    PokopiaVkTexture depth = {
        .format = VK_FORMAT_D24_UNORM_S8_UINT, .extent = {WIDTH, HEIGHT}, .array_layers = 1,
        .aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
        .stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .backing.image = (VkImage)(uintptr_t)2, .view = (VkImageView)(uintptr_t)2,
    };
    PokopiaLiveDraw draw = draw_state(&target, &depth, (VkShaderModule)(uintptr_t)1, (VkShaderModule)(uintptr_t)2);
    size_t used = 0;
    assert(supported(&adapter, &draw, &used) && used == sizeof vertices);
    DrawFunctions functions = {.CreateGraphicsPipelines = check_pipeline};
    DrawWork work = {0};
    CHECK(make_pipeline(VK_NULL_HANDLE, &functions, &draw, &work));

    draw.blend.dst_rgb = draw.blend.dst_alpha = 6;
    assert(!supported(&adapter, &draw, &used));
    draw.blend.dst_rgb = draw.blend.dst_alpha = 2;
    draw.depth_write = true;
    assert(!supported(&adapter, &draw, &used));
    draw.depth_write = false;
    draw.color_write_mask = 15;
    assert(!supported(&adapter, &draw, &used));
    draw.color_write_mask = 7;
    target.format = VK_FORMAT_R8G8B8A8_UNORM;
    assert(!supported(&adapter, &draw, &used));
    draw.blend_enable = false;
    assert(supported(&adapter, &draw, &used));
    puts("PASS exact RGBA16 additive state and prior nonblended RGBA8, with wrong blend/depth/mask rejected");
}

int main(int argc, char **argv)
{
    validate();
    if (argc > 1 && !strcmp(argv[1], "--validate"))
        return 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    PokopiaVkAdapter context = {0};
    PokopiaVkAdapter *adapter = &context;
    assert(pokopia_vk_open(adapter, stdout));
    const VkShaderModule vs = load(adapter, vs_key), fs = load(adapter, fs_key);
    PokopiaVkTexture target = {0}, depth = {0};
    open_texture(adapter, &target, 0x2a, WIDTH, HEIGHT);
    open_texture(adapter, &depth, 0x35, WIDTH, HEIGHT);
    const float background[] = {0.125f, 0.25f, 0.375f, 0.625f};
    CHECK(pokopia_vk_clear_color(adapter, &target, background));
    CHECK(pokopia_vk_clear_depth_stencil(adapter, &depth, full, depth.aspects, (VkClearDepthStencilValue){0, 0}));
    const PokopiaLiveDraw draw = draw_state(&target, &depth, vs, fs);
    uint64_t serial = 0;
    CHECK(pokopia_live_draw(adapter, &draw, &serial));
    assert(serial == target.render_serial && serial);
    uint16_t expected[4];
    for (unsigned c = 0; c < 4; ++c) {
        const float source = (vertices[0].rgba[c] / 255.0f) * (vertices[0].rgba[3] / 255.0f);
        expected[c] = (uint16_t)lroundf(65535 * (background[c] + (c < 3 ? source : 0)));
    }
    check_color(adapter, &target, expected);
    puts("PASS exact cached shaders blended every RGBA16 pixel and preserved destination alpha");
    FN(DestroyShaderModule);
    DestroyShaderModule(adapter->device, vs, NULL);
    DestroyShaderModule(adapter->device, fs, NULL);
    pokopia_vk_texture_close(adapter, &target);
    pokopia_vk_texture_close(adapter, &depth);
    pokopia_vk_close(adapter);
    return 0;
}

VkFormat nvn_guest_texture_vk_format(uint32_t format)
{
    switch (format) {
    case 0x2a: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x35: return VK_FORMAT_D24_UNORM_S8_UINT;
    default: abort();
    }
}

uint32_t nvn_guest_buffer_texel_bytes(uint32_t format) { (void)format; abort(); }
