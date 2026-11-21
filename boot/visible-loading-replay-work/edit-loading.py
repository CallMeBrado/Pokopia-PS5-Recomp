exec(open('edit-depth.py').read().split("edit('vk_adapter.h'", 1)[0])
edit('live_draw.h', [
    ('    NvnFrontSamplerInfo sampler;\n', '    NvnFrontSamplerInfo sampler;\n    VkFormat view_format;\n'),
    ('    VkVertexInputAttributeDescription attributes[3];','    VkVertexInputAttributeDescription attributes[4];'),
    ('    bool blend_enable, depth_test, depth_write, stencil_test;',
     '    NvnFrontBlendInfo blend;\n    bool blend_enable, depth_test, depth_write, stencil_test;'),
])
edit('nvn_replay.c', [
    ('''            strcmp(info.hash, "70b2ae4bef0dfe46"))''','''            strcmp(info.hash, "70b2ae4bef0dfe46") && strcmp(info.hash, "a26763a4bdd7b361"))'''),
    ('''            strcmp(info.hash, "c4664b38f6a87835"))''','''            strcmp(info.hash, "c4664b38f6a87835") && strcmp(info.hash, "2db3327d9fbc5bb3"))'''),
    ('''        r->blend = state;''','''        if (!state.target)
            r->blend = state;'''),
    ('''            .cull_mode = VK_CULL_MODE_NONE, .blend_enable = r->blend_enable,''',
     '''            .cull_mode = VK_CULL_MODE_NONE, .blend_enable = r->blend_enable, .blend = r->blend,'''),
    ('''        if (i >= 3) {
            if (attribute->enabled)
                return NVN_FRONT_BOUNDARY;
        } else {
            if (!attribute->enabled || attribute->stream)
                return NVN_FRONT_BOUNDARY;
            operation.data.draw.state.attributes[i] = (VkVertexInputAttributeDescription){
                i, 0, nvn_guest_texture_vk_format(attribute->format), attribute->offset,
            };
        }''','''        if (i >= 4 || !attribute->enabled) {
            if (attribute->enabled || i < 3)
                return NVN_FRONT_BOUNDARY;
            continue;
        }
        if (attribute->stream)
            return NVN_FRONT_BOUNDARY;
        operation.data.draw.state.attributes[i] = (VkVertexInputAttributeDescription){
            i, 0, nvn_guest_texture_vk_format(attribute->format), attribute->offset,
        };'''),
    ('''static bool matches_sampled_view(const NvnFrontTextureDescriptorInfo *descriptor)''','''static bool resolve_sampled_view(const NvnFrontTextureDescriptorInfo *descriptor, PokopiaLiveDrawSample *sample)'''),
    ('''    return (!(fields & 1u) || (!view[4] && view[5] == 1)) &&
           (!(fields & 2u) || (!layer && layers == 1)) &&
           (!(fields & 4u) || format == config->format) &&
           (!(fields & 8u) || !memcmp(view + 12, identity, sizeof identity)) &&
           (!(fields & 32u) || view[6] == config->target);''',
     '''    if (((fields & 1u) && (view[4] || view[5] != 1)) ||
        ((fields & 2u) && (layer || layers != 1)) ||
        ((fields & 8u) && memcmp(view + 12, identity, sizeof identity)) ||
        ((fields & 32u) && view[6] != config->target))
        return false;
    if ((fields & 4u) && format != config->format) {
        const PokopiaVkTexture *texture = descriptor->texture.native;
        const VkFormat view_format = nvn_guest_texture_vk_format(format);
        if (!texture->sample_alias_view || view_format != texture->sample_alias_format)
            return false;
        sample->view_format = view_format;
    }
    return true;'''),
    ('''    if (!matches_sampled_view(&desc))''','''    if (!resolve_sampled_view(&desc, sample))'''),
])
edit('live_draw.c', [
    ('''static bool supported_sample(const PokopiaLiveDrawSample *sample, VkFormat format, bool linear)''','''static VkFormat sampled_format(const PokopiaLiveDrawSample *sample)
{
    return sample->view_format == VK_FORMAT_UNDEFINED ? sample->texture->format : sample->view_format;
}

static VkImageView sampled_view(const PokopiaVkTexture *texture, VkFormat view_format)
{
    if (view_format != VK_FORMAT_UNDEFINED && view_format != texture->format)
        return view_format == texture->sample_alias_format ? texture->sample_alias_view : VK_NULL_HANDLE;
    return texture->sample_view ? texture->sample_view : texture->view;
}

static bool supported_sample(const PokopiaLiveDrawSample *sample, VkFormat format, bool linear)'''),
    ('''    return texture && texture->backing.image && (texture->sample_view || texture->view) &&
           texture->array_layers <= 1 && texture->format == format &&''',
     '''    return texture && texture->backing.image && sampled_view(texture, sample->view_format) &&
           texture->array_layers <= 1 && sampled_format(sample) == format &&'''),
    ('''static bool supported(const PokopiaVkAdapter *adapter,''','''static bool supported_loading_blend(const NvnFrontBlendInfo *blend)
{
    enum { SRC_ALPHA = 5, ONE_MINUS_SRC_ALPHA = 6, ADD = 1 };
    return !blend->target && !blend->advanced_mode &&
           blend->src_rgb == SRC_ALPHA && blend->dst_rgb == ONE_MINUS_SRC_ALPHA && blend->rgb_equation == ADD &&
           blend->src_alpha == SRC_ALPHA && blend->dst_alpha == ONE_MINUS_SRC_ALPHA && blend->alpha_equation == ADD;
}

static bool supported(const PokopiaVkAdapter *adapter,'''),
    ('''    if (strcmp(vertex, indexed ? "821538a4398f85a7" : "70b2ae4bef0dfe46"))
        return false;''',
     '''    const bool loading = !strcmp(fragment, "2db3327d9fbc5bb3");
    const char *expected_vertex = "70b2ae4bef0dfe46";
    if (indexed)
        expected_vertex = loading ? "a26763a4bdd7b361" : "821538a4398f85a7";
    if (strcmp(vertex, expected_vertex))
        return false;'''),
    ('''    const bool needs_fragment_uniform = tonemap || !strcmp(fragment, "7587f31f1945afaf");''',
     '''    const bool needs_fragment_uniform = loading || tonemap || !strcmp(fragment, "7587f31f1945afaf");'''),
    ('''    } else if (tonemap) {
        if (!depth''',
     '''    } else if (loading) {
        if (!depth || draw->depth_write || !draw->blend_enable || !supported_loading_blend(&draw->blend) ||
            target->format != VK_FORMAT_R8G8B8A8_SRGB || target->array_layers != 1 ||
            target->layout == VK_IMAGE_LAYOUT_UNDEFINED || draw->color_write_mask != rgba ||
            !supported_sample(&samples[0], VK_FORMAT_BC7_SRGB_BLOCK, samples[0].sampler.mag_filter != 0))
            return false;
    } else if (tonemap) {
        if (!depth'''),
    ('''        draw->cull_mode != VK_CULL_MODE_NONE || draw->blend_enable || draw->depth_test || draw->stencil_test ||''',
     '''        draw->cull_mode != VK_CULL_MODE_NONE || (draw->blend_enable && !loading) ||
        draw->depth_test || draw->stencil_test ||'''),
    ('''    if (indexed && (draw->vertex_stride != 24 || draw->index_type != VK_INDEX_TYPE_UINT16 || draw->vertex_count))
        return false;
    const VkVertexInputAttributeDescription expected[] = {''',
     '''    if (indexed && (draw->vertex_stride != (loading ? 44u : 24u) || draw->index_type != VK_INDEX_TYPE_UINT16 ||
                    draw->vertex_count || (!loading && draw->attributes[3].format != VK_FORMAT_UNDEFINED)))
        return false;
    const VkVertexInputAttributeDescription quad_attributes[] = {'''),
    ('''    for (size_t i = 0; indexed && i < sizeof expected / sizeof expected[0]; ++i) {
        const VkVertexInputAttributeDescription *actual = &draw->attributes[i];''',
     '''    const VkVertexInputAttributeDescription loading_attributes[] = {
        {0, 0, VK_FORMAT_R32_SFLOAT, 40},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 36},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
    };
    const VkVertexInputAttributeDescription *expected = loading ? loading_attributes : quad_attributes;
    for (size_t i = 0; indexed && i < (loading ? 4u : 3u); ++i) {
        const VkVertexInputAttributeDescription *actual = &draw->attributes[i];'''),
    ('''    if ((indexed ? (!draw->vertex_uniform.data || draw->vertex_uniform.bytes < 256 ||''',
     '''    const uint32_t vertex_uniform_min = loading ? 3328 : 256;
    if ((indexed ? (!draw->vertex_uniform.data || draw->vertex_uniform.bytes < vertex_uniform_min ||'''),
    ('''            .sampler = work->samplers[i], .imageView = texture->sample_view ? texture->sample_view : texture->view,''',
     '''            .sampler = work->samplers[i], .imageView = sampled_view(texture, sample->view_format),'''),
    ('''        .vertexAttributeDescriptionCount = draw->index_count ? 3 : 0, .pVertexAttributeDescriptions = draw->attributes,''',
     '''        .vertexAttributeDescriptionCount = draw->index_count ? (draw->vertex_stride == 44 ? 4u : 3u) : 0,
        .pVertexAttributeDescriptions = draw->attributes,'''),
    ('''    const VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = draw->color_write_mask};''',
     '''    const VkPipelineColorBlendAttachmentState attachment = {
        .blendEnable = draw->blend_enable, .colorWriteMask = draw->color_write_mask,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD, .alphaBlendOp = VK_BLEND_OP_ADD,
    };'''),
    ('''        adapter->format_properties(adapter->physical, sample->texture->format, &props);''',
     '''        adapter->format_properties(adapter->physical, sampled_format(sample), &props);'''),
])
