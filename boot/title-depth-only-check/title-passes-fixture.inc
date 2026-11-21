int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    PokopiaVkAdapter context = {0};
    PokopiaVkAdapter *adapter = &context;
    assert(pokopia_vk_open(adapter, stdout));
    const char *vs = "821538a4398f85a7", *fs = "c4664b38f6a87835";
    const VkShaderModule vertex = load(adapter, vs), fragment = load(adapter, fs);

    PokopiaVkTexture source = {0}, color = {0}, depth = {0};
    open_texture(adapter, &source, 0x38, 1920, 1080);
    open_texture(adapter, &color, 0x25, WIDTH, HEIGHT);
    open_texture(adapter, &depth, 0x35, WIDTH, HEIGHT);
    const VkRect2D full = {{0, 0}, {WIDTH, HEIGHT}};
    CHECK(pokopia_vk_clear_color(adapter, &source, (const float[4]){0.25f, 0.5f, 0.75f, 1}));
    CHECK(pokopia_vk_clear_color(adapter, &color, (const float[4]){1, 0, 1, 1}));
    CHECK(pokopia_vk_clear_depth_stencil(adapter, &depth, full, depth.aspects, (VkClearDepthStencilValue){1, 0x5a}));

    const Vertex vertices[] = {{0, 0, 0, 0xffffffff, 0, 0}, {0, HEIGHT, 0, 0xffffffff, 0, 1},
                               {WIDTH, HEIGHT, 0, 0xffffffff, 1, 1}, {WIDTH, 0, 0, 0xffffffff, 1, 0}};
    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    const float vs_uniform[64] = {2.0f / WIDTH, -2.0f / HEIGHT, -1.0f, 1.0f};
    const NvnFrontSamplerInfo nearest = {.min_filter = 2, .wrap = {7, 7, 7}, .max_anisotropy = 1};
    PokopiaLiveDraw draw = {
        .target = &color, .depth_target = &depth, .extent = {WIDTH, HEIGHT},
        .vertex_shader = {vertex, "main", vs}, .fragment_shader = {fragment, "main", fs},
        .vertices = {vertices, sizeof vertices}, .indices = {indices, sizeof indices},
        .vertex_uniform = {vs_uniform, sizeof vs_uniform},
        .fragment_samples = {{.texture = &source, .sampler = nearest}},
        .vertex_stride = sizeof vertices[0], .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .index_type = VK_INDEX_TYPE_UINT16, .viewport = {0, HEIGHT, WIDTH, -HEIGHT, 0, 1}, .scissor = full,
        .polygon_mode = VK_POLYGON_MODE_FILL, .color_write_mask = 15, .index_count = 6, .instance_count = 1,
        .attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12},
                       {2, 0, VK_FORMAT_R32G32_SFLOAT, 16}},
    };
    uint64_t serial = 0;
    CHECK(pokopia_live_draw(adapter, &draw, &serial));
    assert(serial == color.render_serial && serial > 0);
    readback(adapter, &color, &depth, 0xffffff, 0x5a);
    puts("PASS 80x45 nearest downsample produced linear RGBA8 with depth and stencil preserved");

    draw.target = NULL;
    draw.fragment_shader = (PokopiaLiveDrawShader){0};
    draw.fragment_samples[0] = (PokopiaLiveDrawSample){0};
    draw.depth_test = true;
    draw.depth_write = true;
    const uint64_t prev = serial;
    CHECK(pokopia_live_draw(adapter, &draw, &serial));
    assert(serial == depth.render_serial && serial == prev + 1);
    readback(adapter, NULL, &depth, 0, 0x5a);
    puts("PASS 80x45 vertex-only depth draw wrote all D24S8 depth values and preserved stencil");

    FN(DestroyShaderModule);
    DestroyShaderModule(adapter->device, vertex, NULL);
    DestroyShaderModule(adapter->device, fragment, NULL);
    pokopia_vk_texture_close(adapter, &source);
    pokopia_vk_texture_close(adapter, &color);
    pokopia_vk_texture_close(adapter, &depth);
    pokopia_vk_close(adapter);
    return 0;
}

VkFormat nvn_guest_texture_vk_format(uint32_t format)
{
    switch (format) {
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x35: return VK_FORMAT_D24_UNORM_S8_UINT;
    case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
    default: abort();
    }
}
uint32_t nvn_guest_buffer_texel_bytes(uint32_t format) { (void)format; abort(); }
