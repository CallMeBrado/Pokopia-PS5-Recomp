from pathlib import Path

def edit(filename, replacements):
    path = Path(filename)
    text = path.read_text()
    for old, new in replacements:
        assert text.count(old) == 1, (filename, old, text.count(old))
        text = text.replace(old, new)
    path.write_text(text)

edit('nvn_replay.c', [
    ('''    uint32_t patch_size;
    NvnFrontBlendInfo blend;''',
     '''    uint32_t patch_size, depth_func;
    NvnFrontBlendInfo blend;'''),
    ('''        r->depth_test = state.depth_test;
        r->depth_write = state.depth_write;''',
     '''        r->depth_test = state.depth_test;
        r->depth_write = state.depth_write;
        r->depth_func = state.depth_func;'''),
    ('''    const unsigned uniform_slot = composite ? 2 : 0;
    NvnFrontDeviceModes modes;
    NvnFrontTextureInfo target, depth_target = {0};''',
     '''    const unsigned uniform_slot = composite ? 2 : 0;
    const bool depth_only = r->target_count <= 1 && !r->targets[0] && r->depth_target &&
        stages[0].hash && !strcmp(stages[0].hash, "821538a4398f85a7") &&
        !r->program[NVN_SHADER_FRAGMENT] && indexed && r->depth_test && r->depth_write && r->depth_func == 8;
    NvnFrontDeviceModes modes;
    NvnFrontTextureInfo target = {0}, depth_target = {0};'''),
    ('''        r->target_count != 1 || !get_color_target(replay, r->device, r->targets[0], &target) ||''',
     '''        (!depth_only && (r->target_count != 1 || !get_color_target(replay, r->device, r->targets[0], &target))) ||'''),
    ('''    const PokopiaVkTexture *depth_texture = depth_target.native;''',
     '''    const NvnFrontTextureConfig *attachment = depth_only ? &depth_target.create_info.config : &target.create_info.config;
    const PokopiaVkTexture *depth_texture = depth_target.native;'''),
    ('''            .extent = {target.create_info.config.width, target.create_info.config.height},''',
     '''            .extent = {attachment->width, attachment->height},'''),
    ('''    if (!get_color_target(replay, device, record->target, &target) || target.native != record->state.target)
        return NVN_FRONT_INVALID_GUEST;''',
     '''    if (record->target &&
        (!get_color_target(replay, device, record->target, &target) || target.native != record->state.target))
        return NVN_FRONT_INVALID_GUEST;'''),
    ('''    const PokopiaVkProgram *programs[2];
    PokopiaVkProgramStage stages[2];
    for (unsigned stage = 0; stage < 2; ++stage) {''',
     '''    const unsigned stage_count = record->target ? 2 : 1;
    const PokopiaVkProgram *programs[2] = {0};
    PokopiaVkProgramStage stages[2] = {0};
    for (unsigned stage = 0; stage < stage_count; ++stage) {'''),
    ('''    const bool composite = scene_composite(stages);
    for (unsigned stage = 0; stage < 2; ++stage) {''',
     '''    const bool composite = scene_composite(stages);
    for (unsigned stage = 0; stage < stage_count; ++stage) {'''),
    ('''    NvnFrontReplyKind result = NVN_FRONT_BOUNDARY;
    for (unsigned stage = 0; stage < 2; ++stage) {''',
     '''    NvnFrontReplyKind result = NVN_FRONT_BOUNDARY;
    for (unsigned stage = 0; stage < stage_count; ++stage) {'''),
    ('''    for (unsigned stage = 0; stage < 2; ++stage)
        pokopia_vk_program_release_module(programs[stage], &modules[stage]);''',
     '''    for (unsigned stage = 0; stage < stage_count; ++stage)
        pokopia_vk_program_release_module(programs[stage], &modules[stage]);''')
])

edit('live_draw.c', [
    ('''           depth->backing.image != draw->target->backing.image &&''',
     '''           (!draw->target || depth->backing.image != draw->target->backing.image) &&'''),
    ('''    return !strcmp(draw->vertex_shader.hash, "caa3cba58abf8cc9") &&
           !strcmp(draw->fragment_shader.hash, "8526214507533768");''',
     '''    return draw->fragment_shader.hash && !strcmp(draw->vertex_shader.hash, "caa3cba58abf8cc9") &&
           !strcmp(draw->fragment_shader.hash, "8526214507533768");'''),
    ('''    if (!target || !target->backing.image || !target->view || target->texel.buffer ||
        target->aspects != VK_IMAGE_ASPECT_COLOR_BIT || !target->stages ||
        target->render_serial > adapter->render_serial ||
        !draw->extent.width || !draw->extent.height ||
        draw->extent.width > adapter->properties.limits.maxFramebufferWidth ||
        draw->extent.height > adapter->properties.limits.maxFramebufferHeight ||
        (target->extent.width && target->extent.width != draw->extent.width) ||
        (target->extent.height && target->extent.height != draw->extent.height))
        return false;

    if (!draw->vertex_shader.module || !draw->vertex_shader.entry || !draw->vertex_shader.hash ||
        !draw->fragment_shader.module || !draw->fragment_shader.entry || !draw->fragment_shader.hash ||
        !draw->vertex_shader.entry[0] || !draw->fragment_shader.entry[0])
        return false;''',
     '''    const bool depth_only = !target;
    if (!draw->extent.width || !draw->extent.height ||
        draw->extent.width > adapter->properties.limits.maxFramebufferWidth ||
        draw->extent.height > adapter->properties.limits.maxFramebufferHeight ||
        (!depth_only && (!target->backing.image || !target->view || target->texel.buffer ||
         target->aspects != VK_IMAGE_ASPECT_COLOR_BIT || !target->stages ||
         target->render_serial > adapter->render_serial ||
         (target->extent.width && target->extent.width != draw->extent.width) ||
         (target->extent.height && target->extent.height != draw->extent.height))))
        return false;

    if (!draw->vertex_shader.module || !draw->vertex_shader.entry || !draw->vertex_shader.hash ||
        !draw->vertex_shader.entry[0] ||
        (!depth_only && (!draw->fragment_shader.module || !draw->fragment_shader.entry ||
                         !draw->fragment_shader.hash || !draw->fragment_shader.entry[0])))
        return false;'''),
    ('''    const char *vertex = draw->vertex_shader.hash, *fragment = draw->fragment_shader.hash;''',
     '''    const char *vertex = draw->vertex_shader.hash, *fragment = depth_only ? "" : draw->fragment_shader.hash;'''),
    ('''        (draw->target_layer && (!depth || draw->target_layer >= target->array_layers)) ||''',
     '''        (draw->target_layer && (depth_only || !depth || draw->target_layer >= target->array_layers)) ||'''),
    ('''    if (composite) {
        if (indexed || !depth''',
     '''    if (depth_only) {
        if (!indexed || !depth || !draw->depth_test || !draw->depth_write || draw->fragment_shader.module ||
            draw->fragment_shader.entry || draw->fragment_shader.hash || samples[0].texture || samples[1].texture ||
            draw->depth_target->render_serial > adapter->render_serial)
            return false;
    } else if (composite) {
        if (indexed || !depth'''),
    ('''    if (indexed)
        for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i)''',
     '''    if (indexed && !depth_only)
        for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i)'''),
    ('''        draw->depth_test || (draw->stencil_test && !depth) ||''',
     '''        (draw->depth_test && !depth_only) || (draw->stencil_test && !depth) ||'''),
    ('''    const VkPipelineColorBlendStateCreateInfo color = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &attachment,
    };''',
     '''    const VkPipelineColorBlendStateCreateInfo color = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = draw->target ? 1u : 0u, .pAttachments = draw->target ? &attachment : NULL,
    };'''),
    ('''        .colorAttachmentCount = 1, .pColorAttachmentFormats = &draw->target->format,''',
     '''        .colorAttachmentCount = draw->target ? 1u : 0u,
        .pColorAttachmentFormats = draw->target ? &draw->target->format : NULL,'''),
    ('''        .stageCount = 2, .pStages = shaders, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,''',
     '''        .stageCount = draw->target ? 2u : 1u, .pStages = shaders,
        .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,'''),
    ('''    const VkImageMemoryBarrier transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = feedback ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : texture->layout,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcAccessMask = feedback ? VK_ACCESS_TRANSFER_READ_BIT : texture->access,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->backing.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, draw->depth_target ? texture->array_layers : 1},
    };
    fn->CmdPipelineBarrier(command, feedback ? VK_PIPELINE_STAGE_TRANSFER_BIT : texture->stages,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &transition);''',
     '''    VkRenderingAttachmentInfo color = {0};
    if (texture) {
        const VkImageMemoryBarrier transition = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = feedback ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : texture->layout,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcAccessMask = feedback ? VK_ACCESS_TRANSFER_READ_BIT : texture->access,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture->backing.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, draw->depth_target ? texture->array_layers : 1},
        };
        fn->CmdPipelineBarrier(command, feedback ? VK_PIPELINE_STAGE_TRANSFER_BIT : texture->stages,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &transition);
        color = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = work->target_view ? work->target_view : texture->view,
            .imageLayout = transition.newLayout,
            .loadOp = texture->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE :
                                                                    VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
    }'''),
    ('''    const VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = work->target_view ? work->target_view : texture->view,
        .imageLayout = transition.newLayout,
        .loadOp = texture->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
''', ''),
    ('''        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &color,''',
     '''        .layerCount = 1, .colorAttachmentCount = texture ? 1u : 0u, .pColorAttachments = texture ? &color : NULL,'''),
    ('''    if (result == VK_SUCCESS && draw->fragment_samples[0].texture == draw->target) {''',
     '''    if (result == VK_SUCCESS && draw->target && draw->fragment_samples[0].texture == draw->target) {'''),
    ('''        draw->target->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        draw->target->stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        draw->target->access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        draw->target->render_serial = ++adapter->render_serial;
        *completed_render_serial = draw->target->render_serial;''',
     '''        if (draw->target) {
            draw->target->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            draw->target->stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            draw->target->access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        }
        PokopiaVkTexture *written = draw->target ? draw->target : draw->depth_target;
        written->render_serial = ++adapter->render_serial;
        *completed_render_serial = written->render_serial;''')
])
