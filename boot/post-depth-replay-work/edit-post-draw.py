exec(open('edit-depth.py').read().split("edit('vk_adapter.h'", 1)[0])
edit('live_draw.c', [
    ('X(CmdPipelineBarrier) X(CmdBeginRendering)', 'X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdBeginRendering)'),
    ('X(CmdBindIndexBuffer) X(CmdDrawIndexed)', 'X(CmdBindIndexBuffer) X(CmdDrawIndexed) X(CmdDraw)'),
    ('    VkSampler sampler;', '    VkSampler samplers[POKOPIA_LIVE_DRAW_SAMPLE_COUNT];\n    PokopiaVkTexture feedback;'),
    ('''static bool supported_sample(const PokopiaLiveDraw *draw)
{
    const PokopiaLiveDrawSample *sample = &draw->fragment_sample;''','''static bool supported_sample(const PokopiaLiveDrawSample *sample, VkFormat format, bool linear)
{'''),
    ('''           texture->backing.image != draw->target->backing.image &&
           texture->format == VK_FORMAT_R32G32B32A32_SFLOAT &&''','''           texture->array_layers <= 1 && texture->format == format &&'''),
    ('''           sampler->min_filter == 3 && sampler->mag_filter == 1 &&''','''           sampler->min_filter == (linear ? 3u : 2u) && sampler->mag_filter == (linear ? 1u : 0u) &&'''),
    ('''        strcmp(draw->vertex_shader.hash, "821538a4398f85a7") != 0 ||
        !draw->fragment_shader.module''','''        !draw->fragment_shader.module'''),
])
p=Path('live_draw.c')
s=p.read_text()
a=s.index('    const bool sampled =')
b=s.index('    const VkVertexInputAttributeDescription expected[] =', a)
s=s[:a]+'''    const bool indexed = draw->index_count != 0;
    const char *vertex = draw->vertex_shader.hash, *fragment = draw->fragment_shader.hash;
    if (strcmp(vertex, indexed ? "821538a4398f85a7" : "70b2ae4bef0dfe46"))
        return false;
    const PokopiaLiveDrawSample *samples = draw->fragment_samples;
    const bool depth = draw->depth_target != NULL;
    const bool needs_fragment_uniform = !strcmp(fragment, "7587f31f1945afaf") || !strcmp(fragment, "ff03fa7b63a79526");
    const bool tonemap = !strcmp(fragment, "ff03fa7b63a79526");
    const VkColorComponentFlags rgb = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    const VkColorComponentFlags rgba = rgb | VK_COLOR_COMPONENT_A_BIT;
    if ((depth && !supported_depth(draw)) ||
        (draw->target_layer && (!depth || draw->target_layer >= target->array_layers)) ||
        (samples[1].texture && !tonemap))
        return false;
    if (!indexed) {
        if (strcmp(fragment, "1c913e7b97e7e999") || !depth || draw->depth_write || draw->target_layer ||
            target->array_layers != 1 || target->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
            samples[0].texture != target || !supported_sample(&samples[0], target->format, true) ||
            draw->color_write_mask != rgb || draw->vertex_count != 6 || draw->first_vertex)
            return false;
    } else if (tonemap) {
        if (!depth || target->format != VK_FORMAT_R8G8B8A8_SRGB || target->array_layers != 1 ||
            draw->color_write_mask != rgba || !supported_sample(&samples[0], VK_FORMAT_R32G32B32A32_SFLOAT, false) ||
            !supported_sample(&samples[1], VK_FORMAT_B10G11R11_UFLOAT_PACK32, true))
            return false;
    } else if (!strcmp(fragment, "c4664b38f6a87835")) {
        if (depth || target->format != VK_FORMAT_R8G8B8A8_SRGB || target->array_layers != 1 ||
            draw->color_write_mask != rgba || !supported_sample(&samples[0], target->format, true))
            return false;
    } else if (!strcmp(fragment, "9f3f4772c2ad7edc")) {
        if (depth || target->format != VK_FORMAT_R32G32B32A32_SFLOAT || draw->color_write_mask != rgba ||
            !supported_sample(&samples[0], target->format, true))
            return false;
    } else if (!strcmp(fragment, "42c917e6eb2c6411")) {
        if (!depth || target->format != VK_FORMAT_R8G8B8A8_UNORM || draw->target_layer >= target->array_layers ||
            target->extent.width != draw->extent.width || target->extent.height != draw->extent.height ||
            samples[0].texture || draw->color_write_mask != rgb)
            return false;
    } else if (strcmp(fragment, "7587f31f1945afaf") || depth || samples[0].texture ||
               target->format != VK_FORMAT_R16G16_SFLOAT || !target->render_serial ||
               (target->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                target->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ||
               draw->depth_write || draw->color_write_mask != rgb) {
        return false;
    }
    if (indexed)
        for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i)
            if (samples[i].texture && samples[i].texture->backing.image == target->backing.image)
                return false;

    if (draw->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || draw->polygon_mode != VK_POLYGON_MODE_FILL ||
        draw->cull_mode != VK_CULL_MODE_NONE || draw->blend_enable || draw->depth_test || draw->stencil_test ||
        draw->instance_count != 1 || draw->first_instance)
        return false;
    if (indexed && (draw->vertex_stride != 24 || draw->index_type != VK_INDEX_TYPE_UINT16 || draw->vertex_count))
        return false;
'''+s[b:]
s=s.replace('for (size_t i = 0; i < sizeof expected / sizeof expected[0]; ++i) {',
            'for (size_t i = 0; indexed && i < sizeof expected / sizeof expected[0]; ++i) {')
a=s.index('    if (!draw->vertex_uniform.data ||')
b=s.index('    const uint8_t *index_bytes', a)
s=s[:a]+'''    if ((indexed ? (!draw->vertex_uniform.data || draw->vertex_uniform.bytes < 256 ||
                    draw->vertex_uniform.bytes > uniform_limit) : draw->vertex_uniform.bytes != 0) ||
        (needs_fragment_uniform ? (!draw->fragment_uniform.data || draw->fragment_uniform.bytes < 256 ||
                                   draw->fragment_uniform.bytes > uniform_limit) : draw->fragment_uniform.bytes != 0))
        return false;
    if (!indexed) {
        *vertex_bytes = 0;
        return true;
    }
    if (!draw->vertices.data || draw->vertices.bytes < draw->vertex_stride || !draw->indices.data ||
        draw->index_count % 3 || (uint64_t)draw->first_index + draw->index_count > draw->indices.bytes / sizeof(uint16_t))
        return false;

'''+s[b:]
a=s.index('    const VkDescriptorSetLayoutBinding bindings[]')
b=s.index('    for (uint32_t i = 0; i < layout_count;', a)
s=s[:a]+'''    VkDescriptorSetLayoutBinding bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    const unsigned span_slots[] = {SUPPORT_UNIFORM, VERTEX_UNIFORM, FRAGMENT_UNIFORM};
    const VkDescriptorBufferInfo ranges[] = {
        {work->buffer, work->offsets[SUPPORT_UNIFORM], SUPPORT_BUFFER_BYTES},
        {work->buffer, work->offsets[VERTEX_UNIFORM], draw->vertex_uniform.bytes},
        {work->buffer, work->offsets[FRAGMENT_UNIFORM], draw->fragment_uniform.bytes},
    };
    unsigned range_slots[3] = {0};
    uint32_t uniform_count = 1;
    if (draw->vertex_uniform.bytes) {
        range_slots[uniform_count] = span_slots[1];
        bindings[uniform_count++] =
            (VkDescriptorSetLayoutBinding){4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
    }
    if (draw->fragment_uniform.bytes) {
        range_slots[uniform_count] = span_slots[2];
        bindings[uniform_count++] =
            (VkDescriptorSetLayoutBinding){22, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    }
    const VkDescriptorSetLayoutBinding sample_bindings[] = {
        {128, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {129, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    const uint32_t sample_count = draw->fragment_samples[1].texture ? 2u : !!draw->fragment_samples[0].texture;
    const VkDescriptorSetLayoutCreateInfo set_layouts[] = {
        {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = uniform_count, .pBindings = bindings},
        {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO},
        {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = sample_count, .pBindings = sample_bindings},
    };
    const uint32_t layout_count = sample_count ? 3 : 1;
'''+s[b:]
s=s.replace('{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},','{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sample_count},')
s=s.replace('const uint32_t set_count = texture ? 2 : 1;', 'const uint32_t set_count = sample_count ? 2 : 1;')
a=s.index('    const VkDescriptorBufferInfo ranges[]', s.index('fn->AllocateDescriptorSets'))
b=s.index('    fn->UpdateDescriptorSets', a)
s=s[:a]+'''    VkWriteDescriptorSet writes[3 + POKOPIA_LIVE_DRAW_SAMPLE_COUNT];
    for (uint32_t i = 0; i < uniform_count; ++i) {
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = work->sets[0],
            .dstBinding = bindings[i].binding, .descriptorCount = 1, .descriptorType = bindings[i].descriptorType,
            .pBufferInfo = &ranges[range_slots[i]],
        };
    }
    uint32_t write_count = uniform_count;
    VkDescriptorImageInfo images[POKOPIA_LIVE_DRAW_SAMPLE_COUNT];
    for (unsigned i = 0; i < sample_count; ++i) {
        const PokopiaLiveDrawSample *sample = &draw->fragment_samples[i];
        const PokopiaVkTexture *texture = sample->texture == draw->target ? &work->feedback : sample->texture;
        const VkSamplerCreateInfo sampler = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = sample->sampler.mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .minFilter = sample->sampler.min_filter == 3 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        };
        result = fn->CreateSampler(device, &sampler, NULL, &work->samplers[i]);
        if (result != VK_SUCCESS)
            return result;
        images[i] = (VkDescriptorImageInfo){
            .sampler = work->samplers[i], .imageView = texture->sample_view ? texture->sample_view : texture->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        writes[write_count++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = work->sets[1],
            .dstBinding = sample_bindings[i].binding, .descriptorCount = 1,
            .descriptorType = sample_bindings[i].descriptorType, .pImageInfo = &images[i],
        };
    }
'''+s[b:]
s=s.replace('    const unsigned span_slots[] = {SUPPORT_UNIFORM, VERTEX_UNIFORM, FRAGMENT_UNIFORM};\n','')
s=s.replace('range_slots[uniform_count] = span_slots[1];','range_slots[uniform_count] = VERTEX_UNIFORM;')
s=s.replace('range_slots[uniform_count] = span_slots[2];','range_slots[uniform_count] = FRAGMENT_UNIFORM;')
p.write_text(s)
edit('live_draw.c', [
    ('''        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = draw->attributes,''','''        .vertexBindingDescriptionCount = draw->index_count ? 1 : 0, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = draw->index_count ? 3 : 0, .pVertexAttributeDescriptions = draw->attributes,'''),
    ('''static VkResult record(const DrawFunctions *fn,''','''static void transition_sample(const DrawFunctions *fn, VkCommandBuffer command, const PokopiaVkTexture *texture,
                              VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access)
{
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = texture->layout, .newLayout = layout,
        .srcAccessMask = texture->access, .dstAccessMask = access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->backing.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    fn->CmdPipelineBarrier(command, texture->stages, stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void record_feedback_copy(const DrawFunctions *fn, VkCommandBuffer command, const PokopiaLiveDraw *draw,
                                 const DrawWork *work)
{
    const PokopiaVkTexture *target = draw->target;
    PokopiaVkTexture snapshot = work->feedback;
    transition_sample(fn, command, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    transition_sample(fn, command, &snapshot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkImageSubresourceLayers color = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    const VkImageCopy region = {
        .srcSubresource = color, .dstSubresource = color, .extent = {draw->extent.width, draw->extent.height, 1},
    };
    fn->CmdCopyImage(command, target->backing.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     snapshot.backing.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    snapshot.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    snapshot.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    snapshot.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    transition_sample(fn, command, &snapshot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

static VkResult record(const DrawFunctions *fn,'''),
    ('''    const PokopiaVkTexture *texture = draw->target;
    const VkImageMemoryBarrier transition = {''','''    const PokopiaVkTexture *texture = draw->target;
    const bool feedback = work->feedback.backing.image != VK_NULL_HANDLE;
    if (feedback)
        record_feedback_copy(fn, command, draw, work);
    const VkImageMemoryBarrier transition = {'''),
    ('''        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = texture->layout,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcAccessMask = texture->access,''','''        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = feedback ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : texture->layout,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcAccessMask = feedback ? VK_ACCESS_TRANSFER_READ_BIT : texture->access,'''),
])
p=Path('live_draw.c')
s=p.read_text()
a=s.index('    fn->CmdPipelineBarrier(command, texture->stages, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,')
b=s.index('    VkRenderingAttachmentInfo depth', a)
s=s[:a]+'''    fn->CmdPipelineBarrier(command, feedback ? VK_PIPELINE_STAGE_TRANSFER_BIT : texture->stages,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &transition);
    for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i) {
        const PokopiaVkTexture *source = draw->fragment_samples[i].texture;
        if (source && source != texture)
            transition_sample(fn, command, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
'''+s[b:]
a=s.index('    if (source)\n        fn->CmdBindDescriptorSets')
b=s.index('    fn->CmdEndRendering', a)
s=s[:a]+'''    if (draw->fragment_samples[0].texture)
        fn->CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, work->pipeline_layout,
                                  2, 1, &work->sets[1], 0, NULL);
    if (draw->index_count) {
        fn->CmdBindVertexBuffers(command, 0, 1, &work->buffer, &work->offsets[VERTICES]);
        fn->CmdBindIndexBuffer(command, work->buffer, work->offsets[INDICES], draw->index_type);
        fn->CmdDrawIndexed(command, draw->index_count, draw->instance_count, draw->first_index,
                           draw->base_vertex, draw->first_instance);
    } else {
        fn->CmdDraw(command, draw->vertex_count, draw->instance_count, draw->first_vertex, draw->first_instance);
    }
'''+s[b:]
a=s.index('    const PokopiaVkTexture *source = draw->fragment_sample.texture;')
b=s.index('    const VkDevice device = adapter->device;', a)
s=s[:a]+'''    for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i) {
        const PokopiaLiveDrawSample *sample = &draw->fragment_samples[i];
        if (!sample->texture)
            continue;
        VkFormatProperties props;
        if (!adapter->format_properties)
            return VK_ERROR_INITIALIZATION_FAILED;
        adapter->format_properties(adapter->physical, sample->texture->format, &props);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            (sample->sampler.mag_filter ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0);
        if ((props.optimalTilingFeatures & required) != required)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

'''+s[b:]
p.write_text(s)
edit('live_draw.c', [
    ('''static void release(PokopiaVkAdapter *adapter, const DrawFunctions *fn, const DrawWork *work)''','''static void release(PokopiaVkAdapter *adapter, const DrawFunctions *fn, DrawWork *work)'''),
    ('''    if (work->sampler)
        fn->DestroySampler(device, work->sampler, NULL);''','''    for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i)
        if (work->samplers[i])
            fn->DestroySampler(device, work->samplers[i], NULL);
    if (work->feedback.backing.image)
        pokopia_vk_texture_close(adapter, &work->feedback);'''),
    ('pokopia_live_draw_indexed','pokopia_live_draw'),
    ('''    VkResult result = open_target_view(device, &fn, draw, &work);
    if (result == VK_SUCCESS)
        result = upload''','''    VkResult result = open_target_view(device, &fn, draw, &work);
    if (result == VK_SUCCESS && draw->fragment_samples[0].texture == draw->target) {
        /* The captured R11G11B10F pass reads its previous contents. Keep a
         * matching one-mip snapshot alive through completion of the draw. */
        const NvnFrontTextureConfig config = {
            .target = 1, .format = 0x3f, .width = draw->extent.width, .height = draw->extent.height,
            .depth = 1, .levels = 1, .swizzle = {2, 3, 4, 5},
        };
        result = pokopia_vk_texture_open(adapter, &config, &work.feedback);
    }
    if (result == VK_SUCCESS)
        result = upload'''),
    ('''        PokopiaVkTexture *sample = draw->fragment_sample.texture;
        if (sample) {
            sample->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            sample->stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            sample->access = VK_ACCESS_SHADER_READ_BIT;
        }''','''        for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i) {
            PokopiaVkTexture *sample = draw->fragment_samples[i].texture;
            if (sample && sample != draw->target) {
                sample->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                sample->stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                sample->access = VK_ACCESS_SHADER_READ_BIT;
            }
        }'''),
])
