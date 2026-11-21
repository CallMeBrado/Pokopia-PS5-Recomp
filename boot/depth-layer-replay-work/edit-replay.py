exec(open('edit-depth.py').read().split("edit('vk_adapter.h'", 1)[0])
edit('nvn_replay.c', [
    ('enum { TEXTURE_SLOTS = 32, COMPUTE_STORAGE_SLOTS = 2 };','''typedef struct {
    uint64_t address;
    void *native;
    VkRect2D area;
    VkImageAspectFlags aspects;
    VkClearDepthStencilValue value;
} DepthClear;

enum { TEXTURE_SLOTS = 32, COMPUTE_STORAGE_SLOTS = 2 };'''),
    ('    uint64_t target, program[2], texture_pool, sampler_pool;','    uint64_t target, depth_target, program[2], texture_pool, sampler_pool;'),
    ('OP_COPY, OP_CLEAR, OP_DRAW', 'OP_COPY, OP_CLEAR, OP_DEPTH_CLEAR, OP_DRAW'),
    ('        Clear clear;','        Clear clear;\n        DepthClear depth_clear;'),
    ('    uint64_t targets[COLOR_TARGETS];','    uint64_t targets[COLOR_TARGETS];\n    uint32_t target_layers[COLOR_TARGETS];'),
    ('''static bool get_target(NvnReplay *replay, uint64_t device, uint64_t address, NvnFrontTextureInfo *target)
{
    return nvn_front_get_texture(replay->config.front, address, target) && target->native &&
           target->create_info.config.device == device && target->create_info.config.target == 1 &&
           target->create_info.config.levels == 1 && target->create_info.config.depth == 1;
}''','''enum { TARGET_2D = 1, TARGET_2D_ARRAY = 4, FORMAT_RGBA8 = 0x25, FORMAT_D24S8 = 0x35 };

static bool get_attachment(NvnReplay *replay, uint64_t device, uint64_t address, NvnFrontTextureInfo *target)
{
    return nvn_front_get_texture(replay->config.front, address, target) && target->native &&
           target->create_info.config.device == device && target->create_info.config.levels == 1;
}

static bool get_color_target(NvnReplay *replay, uint64_t device, uint64_t address, NvnFrontTextureInfo *target)
{
    if (!get_attachment(replay, device, address, target) ||
        ((const PokopiaVkTexture *)target->native)->aspects != VK_IMAGE_ASPECT_COLOR_BIT)
        return false;
    const NvnFrontTextureConfig *config = &target->create_info.config;
    return (config->target == TARGET_2D && config->depth == 1) ||
           (config->target == TARGET_2D_ARRAY && config->format == FORMAT_RGBA8 && config->depth > 1);
}

static bool get_depth_target(NvnReplay *replay, uint64_t device, uint64_t address, NvnFrontTextureInfo *target)
{
    if (!get_attachment(replay, device, address, target))
        return false;
    const NvnFrontTextureConfig *config = &target->create_info.config;
    return config->target == TARGET_2D && config->depth == 1 && config->format == FORMAT_D24S8;
}'''),
    ('''static bool matches_target_view(NvnReplay *replay, uint64_t address, const NvnFrontTextureInfo *target)
{
    if (!address)
        return true;''','''static bool select_target_layer(NvnReplay *replay, uint64_t address, const NvnFrontTextureInfo *target,
                                uint32_t *selected_layer)
{
    const NvnFrontTextureConfig *config = &target->create_info.config;
    *selected_layer = 0;
    if (!address)
        return config->depth == 1;'''),
    ('''    return (!(fields & 1u) || (!view[4] && view[5] == 1)) &&
           (!(fields & 2u) || (!layer && layers == 1)) &&
           (!(fields & 4u) || format == target->create_info.config.format) &&
           (!(fields & 32u) || view[6] == 1);''','''    if (((fields & 1u) && (view[4] || view[5] != 1)) ||
        ((fields & 4u) && format != config->format) ||
        ((fields & 32u) && view[6] != TARGET_2D && view[6] != config->target))
        return false;
    if (!(fields & 2u))
        return config->depth == 1;
    if (layers != 1 || layer >= config->depth)
        return false;
    *selected_layer = layer;
    return true;'''),
    ('    uint64_t targets[COLOR_TARGETS] = {0}, views[COLOR_TARGETS] = {0};','''    uint64_t targets[COLOR_TARGETS] = {0}, views[COLOR_TARGETS] = {0};
    uint32_t layers[COLOR_TARGETS] = {0};'''),
    ('''            if (!get_target(replay, recording->device, targets[i], &target) ||
                !matches_target_view(replay, views[i], &target))''','''            if (!get_color_target(replay, recording->device, targets[i], &target) ||
                !select_target_layer(replay, views[i], &target, &layers[i]))'''),
    ('''    if (x[4])
        return NVN_FRONT_BOUNDARY;
    if (!charge_recording(replay, recording, 76 + 32 * present, 0))
        return NVN_FRONT_BOUNDARY;
    memcpy(recording->targets, targets, sizeof targets);''','''    if (x[4]) {
        uint32_t depth_layer;
        if (!get_depth_target(replay, recording->device, x[4], &target) ||
            !select_target_layer(replay, x[5], &target, &depth_layer))
            return NVN_FRONT_BOUNDARY;
    }
    /* The standard NVN2 D24S8 attachment path replaces 16 bytes of
     * disable commands with 124 bytes, then records its 16-byte control entry. */
    if (!charge_recording(replay, recording, 76 + 32 * present + (x[4] ? 108 : 0), 0) ||
        (x[4] && !charge_recording(replay, recording, 0, 16)))
        return NVN_FRONT_BOUNDARY;
    memcpy(recording->targets, targets, sizeof targets);
    memcpy(recording->target_layers, layers, sizeof layers);'''),
    ('''    /* Both supported graphics pairs bind the pinned SDK's vertex and
       fragment program packets. No compute packet is emitted here. */''','''    /* Supported graphics pairs bind the pinned SDK's vertex and
       fragment program packets. No compute packet is emitted here. */'''),
    ('''            strcmp(info.hash, "9f3f4772c2ad7edc"))
            return NVN_FRONT_BOUNDARY;''','''            strcmp(info.hash, "9f3f4772c2ad7edc") && strcmp(info.hash, "42c917e6eb2c6411"))
            return NVN_FRONT_BOUNDARY;'''),
    ('''    if (!get_target(replay, recording->device, recording->targets[index], &texture))
        return NVN_FRONT_INVALID_GUEST;''','''    if (!get_color_target(replay, recording->device, recording->targets[index], &texture))
        return NVN_FRONT_INVALID_GUEST;
    if (texture.create_info.config.depth != 1)
        return NVN_FRONT_BOUNDARY;'''),
    ('static NvnFrontReplyKind record_indexed_draw(', '''static VkRect2D depth_clear_area(const Recording *recording, const NvnFrontTextureConfig *config)
{
    if (!recording->scissor_set)
        return (VkRect2D){{0, 0}, {config->width, config->height}};
    const int32_t *scissor = recording->scissor;
    const int32_t x = scissor[0] < 0 ? 0 : scissor[0];
    const int32_t y = scissor[1] < 0 ? 0 : scissor[1];
    int64_t end_x = (int64_t)scissor[0] + scissor[2];
    int64_t end_y = (int64_t)scissor[1] + scissor[3];
    if (end_x > config->width)
        end_x = config->width;
    if (end_y > config->height)
        end_y = config->height;
    return (VkRect2D){{x, y}, {end_x > x ? (uint32_t)(end_x - x) : 0, end_y > y ? (uint32_t)(end_y - y) : 0}};
}

static NvnFrontReplyKind record_depth_clear(NvnReplay *replay, const uint64_t x[32], const uint32_t *float_args)
{
    Recording *recording = *find_recording(replay, 0, x[0]);
    if (!recording || !float_args)
        return NVN_FRONT_INVALID_GUEST;
    NvnFrontTextureInfo texture;
    NvnFrontDeviceModes modes;
    if (!replay->config.clear_depth_stencil ||
        !get_depth_target(replay, recording->device, recording->depth_target, &texture) ||
        !nvn_front_get_device_modes(replay->config.front, recording->device, &modes) ||
        modes.window_origin != NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT)
        return NVN_FRONT_BOUNDARY;

    const uint32_t stencil_mask = (uint32_t)x[3] & 0xff;
    float depth;
    memcpy(&depth, float_args, sizeof depth);
    if ((stencil_mask && stencil_mask != 0xff) || ((uint8_t)x[1] && !(depth >= 0 && depth <= 1)))
        return NVN_FRONT_BOUNDARY;
    const VkImageAspectFlags aspects = ((uint8_t)x[1] ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                      (stencil_mask ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    const VkRect2D area = depth_clear_area(recording, &texture.create_info.config);
    if (!aspects || !area.extent.width || !area.extent.height)
        return charge_recording(replay, recording, 0, 24) ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
    const Operation operation = {.kind = OP_DEPTH_CLEAR, .data.depth_clear = {
        .address = recording->depth_target, .native = texture.native,
        .area = area, .aspects = aspects, .value = {depth, (uint32_t)x[2] & 0xff},
    }};
    return append_metered(replay, recording, operation, 0, 24);
}

static NvnFrontReplyKind record_indexed_draw('''),
    ('''    NvnFrontTextureInfo target;
    if (!nvn_front_get_device_modes''','''    NvnFrontTextureInfo target, depth_target = {0};
    if (!nvn_front_get_device_modes'''),
    ('''        r->target_count != 1 || r->depth_target || !get_target(replay, r->device, r->targets[0], &target) ||''','''        r->target_count != 1 || !get_color_target(replay, r->device, r->targets[0], &target) ||
        (r->depth_target && !get_depth_target(replay, r->device, r->depth_target, &depth_target)) ||
        (!r->depth_target && target.create_info.config.depth != 1) ||'''),
    ('''        .target = r->targets[0], .program = {r->program[0], r->program[1]},''','''        .target = r->targets[0], .depth_target = r->depth_target,
        .program = {r->program[0], r->program[1]},'''),
    ('''            .target = target.native,
            .extent =''','''            .target = target.native, .depth_target = depth_target.native, .target_layer = r->target_layers[0],
            .extent ='''),
    ('''    if (!get_target(replay, device, record->target, &target) || target.native != record->state.target)
        return NVN_FRONT_INVALID_GUEST;''','''    if (!get_color_target(replay, device, record->target, &target) || target.native != record->state.target)
        return NVN_FRONT_INVALID_GUEST;
    if (record->depth_target &&
        (!get_depth_target(replay, device, record->depth_target, &target) || target.native != record->state.depth_target))
        return NVN_FRONT_INVALID_GUEST;'''),
    ('''    const Clear *clear = &operation->data.clear;''','''    if (operation->kind == OP_DEPTH_CLEAR) {
        const DepthClear *clear = &operation->data.depth_clear;
        NvnFrontTextureInfo texture;
        if (!get_depth_target(replay, device, clear->address, &texture) || texture.native != clear->native)
            return NVN_FRONT_INVALID_GUEST;
        return !execute || config->clear_depth_stencil(config->opaque, texture.native, clear->area,
                                                        clear->aspects, clear->value) ?
            NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
    }
    const Clear *clear = &operation->data.clear;'''),
    ('''    if (!get_target(replay, device, clear->address, &texture) || texture.native != clear->native)''','''    if (!get_color_target(replay, device, clear->address, &texture) || texture.native != clear->native)'''),
    ('''    if (!strcmp(name, "nvnCommandBufferClearColor"))
        return record_clear(replay, x);''','''    if (!strcmp(name, "nvnCommandBufferClearColor"))
        return record_clear(replay, x);
    if (!strcmp(name, "nvnCommandBufferClearDepthStencil"))
        return record_depth_clear(replay, x, float_args);'''),
])
