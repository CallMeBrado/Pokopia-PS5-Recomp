from pathlib import Path
from difflib import unified_diff

p = Path('live_draw.c')
s = p.read_text()
def edit(old, new):
    global s
    assert s.count(old) == 1, (old, s.count(old))
    s = s.replace(old, new)

edit('''static bool supported_loading_blend(const NvnFrontBlendInfo *blend)
{
    enum { SRC_ALPHA = 5, ONE_MINUS_SRC_ALPHA = 6, ADD = 1 };
    return !blend->target && !blend->advanced_mode &&
           blend->src_rgb == SRC_ALPHA && blend->dst_rgb == ONE_MINUS_SRC_ALPHA && blend->rgb_equation == ADD &&
           blend->src_alpha == SRC_ALPHA && blend->dst_alpha == ONE_MINUS_SRC_ALPHA && blend->alpha_equation == ADD;
}''',
'''enum { BLEND_ONE = 2, BLEND_SRC_ALPHA = 5, BLEND_ONE_MINUS_SRC_ALPHA = 6, BLEND_ADD = 1 };

static bool supported_alpha_blend(const NvnFrontBlendInfo *blend, uint32_t destination)
{
    return !blend->target && !blend->advanced_mode &&
           blend->src_rgb == BLEND_SRC_ALPHA && blend->dst_rgb == destination && blend->rgb_equation == BLEND_ADD &&
           blend->src_alpha == BLEND_SRC_ALPHA && blend->dst_alpha == destination && blend->alpha_equation == BLEND_ADD;
}''')
edit('''    const bool loading = !strcmp(fragment, "2db3327d9fbc5bb3");''',
'''    const bool loading = !strcmp(fragment, "2db3327d9fbc5bb3");
    const bool color_quad = !strcmp(fragment, "42c917e6eb2c6411");
    const bool additive = color_quad && target->format == VK_FORMAT_R16G16B16A16_UNORM;''')
edit('!supported_loading_blend(&draw->blend)', '!supported_alpha_blend(&draw->blend, BLEND_ONE_MINUS_SRC_ALPHA)')
edit('''    } else if (!strcmp(fragment, "42c917e6eb2c6411")) {
        if (!depth || target->format != VK_FORMAT_R8G8B8A8_UNORM || draw->target_layer >= target->array_layers ||
            samples[0].texture || draw->color_write_mask != rgb)
            return false;''',
'''    } else if (color_quad) {
        if (!depth || samples[0].texture || draw->color_write_mask != rgb ||
            draw->target_layer >= target->array_layers)
            return false;
        if (additive) {
            if (target->array_layers != 1 || draw->depth_write ||
                target->layout == VK_IMAGE_LAYOUT_UNDEFINED || !draw->blend_enable ||
                !supported_alpha_blend(&draw->blend, BLEND_ONE))
                return false;
        } else if (target->format != VK_FORMAT_R8G8B8A8_UNORM) {
            return false;
        }''')
edit('''        draw->cull_mode != VK_CULL_MODE_NONE || (draw->blend_enable && !loading) ||''',
'''        draw->cull_mode != VK_CULL_MODE_NONE || (draw->blend_enable && !loading && !additive) ||''')
edit('''    const VkPipelineColorBlendAttachmentState attachment = {
        .blendEnable = draw->blend_enable, .colorWriteMask = draw->color_write_mask,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,''',
'''    const VkBlendFactor destination = draw->blend.dst_rgb == BLEND_ONE ? VK_BLEND_FACTOR_ONE :
                                                                         VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    const VkPipelineColorBlendAttachmentState attachment = {
        .blendEnable = draw->blend_enable, .colorWriteMask = draw->color_write_mask,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = destination,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstAlphaBlendFactor = destination,''')
edit('''    if (!adapter->features.shaderFloat64 || !adapter->features.shaderImageGatherExtended)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i) {''',
'''    if (!adapter->features.shaderFloat64 || !adapter->features.shaderImageGatherExtended)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    if (draw->blend_enable && draw->target->format == VK_FORMAT_R16G16B16A16_UNORM) {
        VkFormatProperties props;
        if (!adapter->format_properties)
            return VK_ERROR_INITIALIZATION_FAILED;
        adapter->format_properties(adapter->physical, draw->target->format, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i) {''')
p.write_text(s)
old, new = 'a/gfx-bridge/live_draw.c', 'b/gfx-bridge/live_draw.c'
with Path('additive-rgba16.patch').open('w', encoding='utf-8', newline='\n') as output:
    output.writelines([f'diff --git {old} {new}\n', *unified_diff(
        Path('live_draw-base.c').read_text().splitlines(keepends=True), s.splitlines(keepends=True), old, new)])
