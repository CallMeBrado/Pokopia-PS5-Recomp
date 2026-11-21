from pathlib import Path

p = Path('live_draw.h')
s = p.read_text()
s = s.replace('    PokopiaLiveDrawSpan vertices, indices, vertex_uniform, fragment_uniform;', '''    PokopiaLiveDrawSpan vertices, indices, vertex_uniform, fragment_uniform;
    PokopiaLiveDrawSpan vertex_constants;''')
p.write_text(s)

p = Path('live_draw.c')
s = p.read_text()
def edit(old, new):
    global s
    assert s.count(old) == 1, (old, s.count(old))
    s = s.replace(old, new)

edit('enum { SUPPORT_UNIFORM, VERTEX_UNIFORM, FRAGMENT_UNIFORM, VERTICES, INDICES, SPAN_COUNT };',
     'enum { SUPPORT_UNIFORM, VERTEX_CONSTANTS, VERTEX_UNIFORM, FRAGMENT_UNIFORM, VERTICES, INDICES, SPAN_COUNT };')
edit('static bool supported(const PokopiaVkAdapter *adapter', '''static bool is_scene_composite(const PokopiaLiveDraw *draw)
{
    return !strcmp(draw->vertex_shader.hash, "caa3cba58abf8cc9") &&
           !strcmp(draw->fragment_shader.hash, "8526214507533768");
}

static bool supported(const PokopiaVkAdapter *adapter''')
edit('    const bool indexed = draw->index_count != 0;',
     '    const bool indexed = draw->index_count != 0;\n    const bool composite = is_scene_composite(draw);')
edit('    const char *expected_vertex = "70b2ae4bef0dfe46";',
     '    const char *expected_vertex = composite ? "caa3cba58abf8cc9" : "70b2ae4bef0dfe46";')
edit('    if (!indexed) {\n        if (strcmp(fragment, "1c913e7b97e7e999")',
'''    if (composite) {
        if (indexed || !depth || draw->depth_write || draw->stencil_test || draw->target_layer ||
            target->array_layers != 1 || target->format != VK_FORMAT_R8G8B8A8_SRGB ||
            !supported_sample(&samples[0], VK_FORMAT_B10G11R11_UFLOAT_PACK32, true) ||
            samples[0].texture->backing.image == target->backing.image ||
            samples[0].texture->extent.width != draw->extent.width ||
            samples[0].texture->extent.height != draw->extent.height ||
            draw->color_write_mask != rgba || draw->vertex_count != 3 || draw->first_vertex ||
            draw->vertex_stride || draw->vertices.data || draw->vertices.bytes)
            return false;
    } else if (!indexed) {
        if (strcmp(fragment, "1c913e7b97e7e999")''')
edit('    const size_t attribute_count = loading ? 4 : indexed ? 3 : 1;',
     '    const size_t attribute_count = composite ? 0 : loading ? 4 : indexed ? 3 : 1;')
edit('    const uint32_t vertex_uniform_min = loading ? 3328 : 256;',
'''    if (composite) {
        if (!draw->vertex_constants.data || draw->vertex_constants.bytes != 0x30 ||
            !draw->vertex_uniform.data || draw->vertex_uniform.bytes != 512 ||
            !draw->fragment_uniform.data || draw->fragment_uniform.bytes != 512 ||
            uniform_limit < 512)
            return false;
        *vertex_bytes = 0;
        return true;
    }
    if (draw->vertex_constants.bytes)
        return false;
    const uint32_t vertex_uniform_min = loading ? 3328 : 256;''')
edit('''    const PokopiaLiveDrawSpan spans[SPAN_COUNT] = {
        {support, sizeof support}, draw->vertex_uniform, draw->fragment_uniform,''',
'''    uint8_t vertex_constants[256] = {0};
    if (draw->vertex_constants.bytes)
        memcpy(vertex_constants, draw->vertex_constants.data, draw->vertex_constants.bytes);
    const PokopiaLiveDrawSpan spans[SPAN_COUNT] = {
        {support, sizeof support},
        {vertex_constants, draw->vertex_constants.bytes ? sizeof vertex_constants : 0},
        draw->vertex_uniform, draw->fragment_uniform,''')
edit('    VkDescriptorSetLayoutBinding bindings[3] = {',
     '    const bool composite = is_scene_composite(draw);\n    VkDescriptorSetLayoutBinding bindings[4] = {')
edit('        {work->buffer, work->offsets[VERTEX_UNIFORM], draw->vertex_uniform.bytes},',
'''        {work->buffer, work->offsets[VERTEX_CONSTANTS], 256},
        {work->buffer, work->offsets[VERTEX_UNIFORM], draw->vertex_uniform.bytes},''')
edit('''    unsigned range_slots[3] = {0};
    uint32_t uniform_count = 1;''',
'''    unsigned range_slots[4] = {0};
    uint32_t uniform_count = 1;
    if (composite) {
        range_slots[uniform_count] = VERTEX_CONSTANTS;
        bindings[uniform_count++] =
            (VkDescriptorSetLayoutBinding){2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
    }''')
edit('(VkDescriptorSetLayoutBinding){4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};',
     '(VkDescriptorSetLayoutBinding){composite ? 6u : 4u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};')
edit('(VkDescriptorSetLayoutBinding){22, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};',
     '(VkDescriptorSetLayoutBinding){composite ? 24u : 22u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};')
edit('    VkWriteDescriptorSet writes[3 + POKOPIA_LIVE_DRAW_SAMPLE_COUNT];',
     '    VkWriteDescriptorSet writes[4 + POKOPIA_LIVE_DRAW_SAMPLE_COUNT];')
edit('    const VkVertexInputBindingDescription binding = {0, draw->vertex_stride, VK_VERTEX_INPUT_RATE_VERTEX};',
     '    const bool composite = is_scene_composite(draw);\n    const VkVertexInputBindingDescription binding = {0, draw->vertex_stride, VK_VERTEX_INPUT_RATE_VERTEX};')
edit('''        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = draw->index_count ? (draw->vertex_stride == 44 ? 4u : 3u) : 1,
        .pVertexAttributeDescriptions = draw->attributes,''',
'''        .vertexBindingDescriptionCount = composite ? 0u : 1u,
        .pVertexBindingDescriptions = composite ? NULL : &binding,
        .vertexAttributeDescriptionCount = composite ? 0 : draw->index_count ? (draw->vertex_stride == 44 ? 4u : 3u) : 1,
        .pVertexAttributeDescriptions = composite ? NULL : draw->attributes,''')
edit('    fn->CmdBindVertexBuffers(command, 0, 1, &work->buffer, &work->offsets[VERTICES]);',
     '    if (!is_scene_composite(draw))\n        fn->CmdBindVertexBuffers(command, 0, 1, &work->buffer, &work->offsets[VERTICES]);')
p.write_text(s)
