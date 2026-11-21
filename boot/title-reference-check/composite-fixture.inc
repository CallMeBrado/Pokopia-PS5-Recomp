static PFN_vkGetDeviceProcAddr original_proc;
static VkDevice check_device;
static unsigned pipelines, draws, binds, copies;

static VKAPI_ATTR VkResult VKAPI_CALL check_pipeline(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *out)
{
    assert(count == 1 && infos->pVertexInputState->vertexBindingDescriptionCount == 0 &&
           infos->pVertexInputState->vertexAttributeDescriptionCount == 0);
    assert(infos->pDepthStencilState && !infos->pDepthStencilState->depthTestEnable &&
           !infos->pDepthStencilState->depthWriteEnable);
    assert(!infos->pColorBlendState->pAttachments[0].blendEnable);
    ++pipelines;
    PFN_vkCreateGraphicsPipelines real = (PFN_vkCreateGraphicsPipelines)original_proc(device, "vkCreateGraphicsPipelines");
    return real(device, cache, count, infos, allocator, out);
}

static VKAPI_ATTR void VKAPI_CALL check_bind(VkCommandBuffer command, uint32_t first, uint32_t count,
    const VkBuffer *buffers, const VkDeviceSize *offsets)
{
    ++binds;
    PFN_vkCmdBindVertexBuffers real = (PFN_vkCmdBindVertexBuffers)original_proc(check_device, "vkCmdBindVertexBuffers");
    real(command, first, count, buffers, offsets);
}

static VKAPI_ATTR void VKAPI_CALL check_copy(VkCommandBuffer command, VkImage src, VkImageLayout src_layout,
    VkImage dst, VkImageLayout dst_layout, uint32_t count, const VkImageCopy *regions)
{
    ++copies;
    PFN_vkCmdCopyImage real = (PFN_vkCmdCopyImage)original_proc(check_device, "vkCmdCopyImage");
    real(command, src, src_layout, dst, dst_layout, count, regions);
}

static VKAPI_ATTR void VKAPI_CALL check_draw(VkCommandBuffer command, uint32_t vertex_count,
    uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
{
    assert(vertex_count == 3 && instance_count == 1 && !first_vertex && !first_instance);
    ++draws;
    PFN_vkCmdDraw real = (PFN_vkCmdDraw)original_proc(check_device, "vkCmdDraw");
    real(command, vertex_count, instance_count, first_vertex, first_instance);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL check_proc(VkDevice device, const char *name)
{
    if (!strcmp(name, "vkCreateGraphicsPipelines"))
        return (PFN_vkVoidFunction)check_pipeline;
    if (!strcmp(name, "vkCmdBindVertexBuffers"))
        return (PFN_vkVoidFunction)check_bind;
    if (!strcmp(name, "vkCmdCopyImage"))
        return (PFN_vkVoidFunction)check_copy;
    if (!strcmp(name, "vkCmdDraw"))
        return (PFN_vkVoidFunction)check_draw;
    return original_proc(device, name);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    PokopiaVkAdapter context = {0};
    PokopiaVkAdapter *adapter = &context;
    assert(pokopia_vk_open(adapter, stdout));
    original_proc = adapter->get_device_proc;
    check_device = adapter->device;
    adapter->get_device_proc = check_proc;

    const char *vs = "caa3cba58abf8cc9", *fs = "8526214507533768";
    const VkShaderModule vertex = load(adapter, vs), fragment = load(adapter, fs);
    PokopiaVkTexture hdr = {0}, srgb = {0}, depth = {0};
    open_texture(adapter, &hdr, 0x3f, WIDTH, HEIGHT);
    open_texture(adapter, &srgb, 0x38, WIDTH, HEIGHT);
    open_texture(adapter, &depth, 0x35, WIDTH, HEIGHT);

    const VkRect2D full = {{0, 0}, {WIDTH, HEIGHT}};
    const float sentinel[4] = {1, 0, 1, 1};
    CHECK(pokopia_vk_clear_color(adapter, &srgb, sentinel));
    CHECK(pokopia_vk_clear_color(adapter, &hdr, (const float[4]){0.125f, 0.25f, 0.5f, 1}));
    CHECK(pokopia_vk_clear_depth_stencil(adapter, &depth, full, depth.aspects, (VkClearDepthStencilValue){0, 0}));
    const NvnFrontSamplerInfo linear = {.min_filter = 3, .mag_filter = 1, .wrap = {7, 7, 7}, .max_anisotropy = 1};
    PokopiaLiveDraw draw = {
        .target = &srgb, .depth_target = &depth, .extent = {WIDTH, HEIGHT},
        .vertex_shader = {vertex, "main", vs}, .fragment_shader = {fragment, "main", fs},
        .vertex_uniform = {vs_composite, sizeof vs_composite},
        .fragment_uniform = {fs_composite, sizeof fs_composite},
        .fragment_samples = {{.texture = &hdr, .sampler = linear}},
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .polygon_mode = VK_POLYGON_MODE_FILL,
        .viewport = {0, HEIGHT, WIDTH, -HEIGHT, 0, 1}, .scissor = full,
        .color_write_mask = 15, .vertex_count = 3, .instance_count = 1,
    };
    uint64_t serial = 0;
    assert(pokopia_live_draw(adapter, &draw, &serial) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    draw.vertex_constants = (PokopiaLiveDrawSpan){c1_composite, sizeof c1_composite};
    assert(pokopia_live_draw(adapter, &draw, &serial) == VK_SUCCESS);
    assert(serial == srgb.render_serial && serial != 0 && pipelines == 1 && draws == 1 && !binds && !copies);
    assert(hdr.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    uint8_t sample1[4], sample2[4];
    check_color(adapter, &srgb, sample1);

    CHECK(pokopia_vk_clear_color(adapter, &srgb, sentinel));
    CHECK(pokopia_vk_clear_color(adapter, &hdr, (const float[4]){0.75f, 0.5f, 0.25f, 1}));
    CHECK(pokopia_live_draw(adapter, &draw, &serial));
    assert(pipelines == 2 && draws == 2 && !binds && !copies);
    check_color(adapter, &srgb, sample2);
    assert(memcmp(sample1, sample2, sizeof sample1));
    puts("PASS real composite SPIR-V, authentic c1/c5, vertexless scene sample and full GPU pixel coverage");
    FN(DestroyShaderModule);
    DestroyShaderModule(adapter->device, vertex, NULL);
    DestroyShaderModule(adapter->device, fragment, NULL);
    pokopia_vk_texture_close(adapter, &hdr);
    pokopia_vk_texture_close(adapter, &srgb);
    pokopia_vk_texture_close(adapter, &depth);
    pokopia_vk_close(adapter);
    return 0;
}

