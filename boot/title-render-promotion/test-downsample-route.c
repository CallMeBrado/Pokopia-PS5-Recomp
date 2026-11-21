#include "live_draw.c"
#include <assert.h>

static PokopiaVkTexture texture(VkFormat format, uint32_t width, uint32_t height, unsigned handle)
{
    return (PokopiaVkTexture){
        .format = format, .extent = {width, height},
        .aspects = VK_IMAGE_ASPECT_COLOR_BIT, .array_layers = 1,
        .backing.image = (VkImage)(uintptr_t)handle, .view = (VkImageView)(uintptr_t)handle,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
}

int main(void)
{
    const PokopiaVkAdapter adapter = {.properties.limits = {
        .maxFramebufferWidth = 1920, .maxFramebufferHeight = 1080, .maxUniformBufferRange = 65536,
    }};
    PokopiaVkTexture source = texture(VK_FORMAT_R8G8B8A8_SRGB, 1920, 1080, 1);
    PokopiaVkTexture target = texture(VK_FORMAT_R8G8B8A8_UNORM, 80, 45, 2);
    PokopiaVkTexture depth = texture(VK_FORMAT_D24_UNORM_S8_UINT, 80, 45, 3);
    depth.aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    const uint8_t vertices[4][24] = {0};
    const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    const uint8_t vs[256] = {0};
    PokopiaLiveDraw draw = {
        .target = &target, .depth_target = &depth, .extent = {80, 45},
        .vertex_shader = {(VkShaderModule)(uintptr_t)1, "main", "821538a4398f85a7"},
        .fragment_shader = {(VkShaderModule)(uintptr_t)2, "main", "c4664b38f6a87835"},
        .fragment_samples = {{.texture = &source, .sampler = {.min_filter = 2, .wrap = {7, 7, 7}, .max_anisotropy = 1}}},
        .vertices = {vertices, sizeof vertices}, .indices = {indices, sizeof indices}, .vertex_uniform = {vs, sizeof vs},
        .vertex_stride = 24, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .index_type = VK_INDEX_TYPE_UINT16,
        .attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12},
                       {2, 0, VK_FORMAT_R32G32_SFLOAT, 16}},
        .viewport = {0, 45, 80, -45, 0, 1}, .scissor = {{0, 0}, {80, 45}}, .color_write_mask = 15,
        .polygon_mode = VK_POLYGON_MODE_FILL, .index_count = 6, .instance_count = 1,
    };
    size_t used = 0;
    assert(supported(&adapter, &draw, &used) && used == sizeof vertices);
    draw.depth_write = true;
    assert(!supported(&adapter, &draw, &used));
    draw.depth_write = false;
    draw.fragment_samples[0].sampler.min_filter = 3;
    draw.fragment_samples[0].sampler.mag_filter = 1;
    assert(!supported(&adapter, &draw, &used));

    draw.depth_target = NULL;
    draw.depth_write = true; /* Captured, with no bound depth image. */
    target.format = VK_FORMAT_R8G8B8A8_SRGB;
    assert(supported(&adapter, &draw, &used));
    target.format = source.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    draw.extent = target.extent = (VkExtent2D){960, 540};
    draw.viewport = (VkViewport){0, 540, 960, -540, 0, 1};
    draw.scissor.extent = draw.extent;
    assert(supported(&adapter, &draw, &used));
    draw.fragment_samples[0].texture = &target;
    assert(!supported(&adapter, &draw, &used));
    puts("PASS captured c466 nearest sRGB-to-RGBA8 with D24S8, prior sRGB path, and half-resolution R11G11B10F");
    return 0;
}
VkFormat nvn_guest_texture_vk_format(uint32_t format) { (void)format; return VK_FORMAT_UNDEFINED; }
uint32_t nvn_guest_buffer_texel_bytes(uint32_t format) { (void)format; return 0; }
