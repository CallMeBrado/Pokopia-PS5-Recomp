exec(open('edit-depth.py').read().split("edit('vk_adapter.h'", 1)[0])
edit('live_draw.c', [
    ('    X(CreateSampler) X(DestroySampler) \\\n', '    X(CreateSampler) X(DestroySampler) X(CreateImageView) X(DestroyImageView) \\\n'),
    ('    VkSampler sampler;\n','    VkSampler sampler;\n    VkImageView target_view;\n'),
    ('static bool supported(const', '''static bool supported_depth(const PokopiaLiveDraw *draw)
{
    const PokopiaVkTexture *depth = draw->depth_target;
    return depth && depth->backing.image && depth->view && depth->stages &&
           depth->backing.image != draw->target->backing.image &&
           depth->aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) &&
           depth->array_layers == 1 && depth->extent.width == draw->extent.width &&
           depth->extent.height == draw->extent.height;
}

static bool supported(const'''),
    ('    const bool sampled = draw->fragment_sample.texture != NULL;\n', '''    const bool sampled = draw->fragment_sample.texture != NULL;
    const bool depth = draw->depth_target != NULL;
    if (!depth && draw->target_layer)
        return false;
'''),
    ('        if (strcmp(draw->fragment_shader.hash, "9f3f4772c2ad7edc") ||','        if (depth || strcmp(draw->fragment_shader.hash, "9f3f4772c2ad7edc") ||'),
    ('    } else if (strcmp(draw->fragment_shader.hash, "7587f31f1945afaf") ||', '''    } else if (depth) {
        if (strcmp(draw->fragment_shader.hash, "42c917e6eb2c6411") ||
            target->format != VK_FORMAT_R8G8B8A8_UNORM || draw->target_layer >= target->array_layers ||
            target->extent.width != draw->extent.width || target->extent.height != draw->extent.height ||
            draw->fragment_uniform.data || draw->color_write_mask != rgb || !supported_depth(draw))
            return false;
    } else if (strcmp(draw->fragment_shader.hash, "7587f31f1945afaf") ||'''),
    ('        (sampled ? draw->fragment_uniform.bytes != 0 :', '        ((sampled || depth) ? draw->fragment_uniform.bytes != 0 :'),
    ('    const VkPipelineRenderingCreateInfo rendering = {', '''    const VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = draw->depth_test, .depthWriteEnable = draw->depth_write,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS, .stencilTestEnable = draw->stencil_test,
    };
    const VkFormat depth_format = draw->depth_target ? draw->depth_target->format : VK_FORMAT_UNDEFINED;
    const VkPipelineRenderingCreateInfo rendering = {'''),
    ('        .colorAttachmentCount = 1, .pColorAttachmentFormats = &draw->target->format,','''        .colorAttachmentCount = 1, .pColorAttachmentFormats = &draw->target->format,
        .depthAttachmentFormat = depth_format, .stencilAttachmentFormat = depth_format,'''),
    ('        .pColorBlendState = &color, .layout = work->pipeline_layout,','''        .pColorBlendState = &color, .layout = work->pipeline_layout,
        .pDepthStencilState = draw->depth_target ? &depth : NULL,'''),
    ('static VkResult record(', '''static VkResult open_target_view(VkDevice device, const DrawFunctions *fn,
                                 const PokopiaLiveDraw *draw, DrawWork *work)
{
    if (!draw->target_layer)
        return VK_SUCCESS;
    const VkImageViewUsageCreateInfo usage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    };
    const VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &usage,
        .image = draw->target->backing.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = draw->target->format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, draw->target_layer, 1},
    };
    return fn->CreateImageView(device, &info, NULL, &work->target_view);
}

static const VkPipelineStageFlags depth_stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
static const VkAccessFlags depth_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

static VkResult record('''),
    ('        .image = texture->backing.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},','''        .image = texture->backing.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, draw->depth_target ? texture->array_layers : 1},'''),
    ('    const VkRenderingAttachmentInfo color = {\n        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = texture->view,','''    VkRenderingAttachmentInfo depth = {0};
    const PokopiaVkTexture *depth_target = draw->depth_target;
    if (depth_target) {
        const VkImageMemoryBarrier depth_transition = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = depth_target->layout,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .srcAccessMask = depth_target->access,
            .dstAccessMask = depth_access,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = depth_target->backing.image, .subresourceRange = {depth_target->aspects, 0, 1, 0, 1},
        };
        fn->CmdPipelineBarrier(command, depth_target->stages, depth_stages, 0, 0, NULL, 0, NULL, 1,
                               &depth_transition);
        depth = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = depth_target->view,
            .imageLayout = depth_transition.newLayout,
            .loadOp = depth_target->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE :
                                                                        VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
    }
    const VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = work->target_view ? work->target_view : texture->view,'''),
    ('        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &color,','''        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &color,
        .pDepthAttachment = depth_target ? &depth : NULL, .pStencilAttachment = depth_target ? &depth : NULL,'''),
    ('    if (work->pipeline)\n', '''    if (work->target_view)
        fn->DestroyImageView(device, work->target_view, NULL);
    if (work->pipeline)
'''),
    ('    VkResult result = upload(adapter, &fn, draw, vertex_bytes, &work);','''    VkResult result = open_target_view(device, &fn, draw, &work);
    if (result == VK_SUCCESS)
        result = upload(adapter, &fn, draw, vertex_bytes, &work);'''),
    ('        draw->target->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;','''        if (draw->depth_target) {
            draw->depth_target->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            draw->depth_target->stages = depth_stages;
            draw->depth_target->access = depth_access;
        }
        draw->target->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;'''),
])
