#include "live_draw.h"
#include "nvn_guest_layout.h"

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static void check(VkResult result, const char *where)
{
    if (result == VK_SUCCESS)
        return;
    fprintf(stderr, "%s failed with VkResult %d\n", where, (int)result);
    exit(EXIT_FAILURE);
}
#define CHECK(call) check((call), #call)

static void *read_file(const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    assert(f && fseek(f, 0, SEEK_END) == 0);
    const long length = ftell(f);
    assert(length > 0 && fseek(f, 0, SEEK_SET) == 0);
    void *data = malloc((size_t)length);
    assert(data && fread(data, 1, (size_t)length, f) == (size_t)length);
    fclose(f);
    *bytes = (size_t)length;
    return data;
}

static VkShaderModule load(PokopiaVkAdapter *adapter, const char *key)
{
    char path[128];
    int len = snprintf(path, sizeof path, "../../frame-reference-655360/shader_cache/spirv/%s.spv", key);
    assert(len > 0 && (size_t)len < sizeof path);
    size_t bytes;
    void *code = read_file(path, &bytes);
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes, .pCode = code,
    };
    PFN_vkCreateShaderModule create = (PFN_vkCreateShaderModule)
        adapter->get_device_proc(adapter->device, "vkCreateShaderModule");
    VkShaderModule module;
    CHECK(create(adapter->device, &info, NULL, &module));
    free(code);
    return module;
}

#define FN(name) PFN_vk##name name = (PFN_vk##name)adapter->get_device_proc(adapter->device, "vk" #name)
enum { WIDTH = 1920, HEIGHT = 1080, PIXELS = WIDTH * HEIGHT, LAYERS = 5 };

static VkCommandBuffer begin(PokopiaVkAdapter *adapter)
{
    FN(BeginCommandBuffer);
    VkCommandBuffer command;
    CHECK(pokopia_vk_command_buffer_open(adapter, &command));
    const VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(BeginCommandBuffer(command, &info));
    return command;
}

static void finish(PokopiaVkAdapter *adapter, VkCommandBuffer command)
{
    FN(EndCommandBuffer);
    CHECK(EndCommandBuffer(command));
    bool retired = false;
    CHECK(pokopia_vk_submit_wait(adapter, command, &retired));
    assert(retired);
    pokopia_vk_command_buffer_close(adapter, command);
}

static void transition(PokopiaVkAdapter *adapter, VkCommandBuffer command, PokopiaVkTexture *texture,
                       VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access)
{
    FN(CmdPipelineBarrier);
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = texture->layout, .newLayout = layout,
        .srcAccessMask = texture->access, .dstAccessMask = access, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = texture->backing.image,
        .subresourceRange = {texture->aspects, 0, 1, 0, texture->array_layers},
    };
    CmdPipelineBarrier(command, texture->stages, stage, 0, 0, NULL, 0, NULL, 1, &barrier);
    texture->layout = layout;
    texture->stages = stage;
    texture->access = access;
}

static void seed_color(PokopiaVkAdapter *adapter, PokopiaVkTexture *color)
{
    FN(CmdClearColorImage);
    VkCommandBuffer command = begin(adapter);
    transition(adapter, command, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    for (uint32_t layer = 0; layer < LAYERS; ++layer) {
        const VkClearColorValue value = {.float32 = {1.0f, layer / 5.0f, 1.0f, 1.0f}};
        const VkImageSubresourceRange range = {color->aspects, 0, 1, layer, 1};
        CmdClearColorImage(command, color->backing.image, color->layout, &value, 1, &range);
    }
    finish(adapter, command);
}

static void readback(PokopiaVkAdapter *adapter, PokopiaVkTexture *color, PokopiaVkTexture *depth,
                     uint32_t expected_depth, uint8_t expected_stencil, const VkRect2D *patch)
{
    FN(CreateBuffer); FN(DestroyBuffer); FN(GetBufferMemoryRequirements); FN(AllocateMemory); FN(FreeMemory);
    FN(BindBufferMemory); FN(MapMemory); FN(UnmapMemory); FN(CmdCopyImageToBuffer); FN(CmdPipelineBarrier);

    const VkDevice device = adapter->device;
    const size_t colors = color ? PIXELS * 4 * LAYERS : 0;
    const bool depth16 = depth->format == VK_FORMAT_D16_UNORM;
    const uint32_t width = depth->extent.width, height = depth->extent.height;
    const size_t pixels = (size_t)width * height;
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = colors + pixels * (depth16 ? 2 : 5),
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer;
    CHECK(CreateBuffer(device, &info, NULL, &buffer));
    VkMemoryRequirements requirements;
    GetBufferMemoryRequirements(device, buffer, &requirements);
    uint32_t type;
    const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (type = 0; type < adapter->memory.memoryTypeCount; ++type)
        if ((requirements.memoryTypeBits & (1u << type)) &&
            (adapter->memory.memoryTypes[type].propertyFlags & flags) == flags)
            break;
    assert(type < adapter->memory.memoryTypeCount);
    const VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type,
    };
    VkDeviceMemory memory;
    CHECK(AllocateMemory(device, &allocate, NULL, &memory));
    CHECK(BindBufferMemory(device, buffer, memory, 0));

    VkCommandBuffer command = begin(adapter);
    if (color) {
        transition(adapter, command, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferImageCopy region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS},
            .imageExtent = {WIDTH, HEIGHT, 1},
        };
        CmdCopyImageToBuffer(command, color->backing.image, color->layout, buffer, 1, &region);
    }
    transition(adapter, command, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferImageCopy regions[] = {
        {.bufferOffset = colors, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {width, height, 1}},
        {.bufferOffset = colors + PIXELS * 4, .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1}, .imageExtent = {WIDTH, HEIGHT, 1}},
    };
    CmdCopyImageToBuffer(command, depth->backing.image, depth->layout, buffer, depth16 ? 1 : 2, regions);
    const VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = buffer, .size = VK_WHOLE_SIZE,
    };
    CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL,
                       1, &barrier, 0, NULL);
    finish(adapter, command);
    void *mapped;
    CHECK(MapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    const uint8_t *bytes = mapped;
    if (color) {
        for (unsigned layer = 0; layer < LAYERS; ++layer) {
            const uint8_t expected_g[] = {0, 51, 102, 153, 204};
            const uint8_t expected[] = {layer == 3 ? 0 : 255, layer == 3 ? 0 : expected_g[layer],
                                        layer == 3 ? 0 : 255, 255};
            size_t mismatches = 0;
            for (size_t i = 0; i < PIXELS; ++i) {
                const uint8_t *pixel = bytes + (layer * PIXELS + i) * 4;
                if (memcmp(pixel, expected, sizeof expected) && ++mismatches == 1)
                    printf("layer %u pixel %zu=%u,%u,%u,%u expected %u,%u,%u,%u\n", layer, i,
                           pixel[0], pixel[1], pixel[2], pixel[3], expected[0], expected[1], expected[2], expected[3]);
            }
            assert(!mismatches);
        }
    }
    if (depth16) {
        const uint16_t *depth_words = (const uint16_t *)(bytes + colors);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const bool cleared = patch &&
                    x >= (uint32_t)patch->offset.x && x - (uint32_t)patch->offset.x < patch->extent.width &&
                    y >= (uint32_t)patch->offset.y && y - (uint32_t)patch->offset.y < patch->extent.height;
                assert(depth_words[(size_t)y * width + x] == (cleared ? UINT16_MAX : expected_depth));
            }
        }
    } else {
        const uint32_t *depth_words = (const uint32_t *)(bytes + colors);
        const uint8_t *stencil = bytes + colors + pixels * 4;
        printf("Readback depth=%08x stencil=%02x format=%u\n", depth_words[0], stencil[0], depth->format);
        for (size_t i = 0; i < pixels; ++i) {
            assert((depth_words[i] & (depth->format == VK_FORMAT_D24_UNORM_S8_UINT ? 0x00ffffffu : 0xffffffffu)) ==
                   expected_depth);
            assert(stencil[i] == expected_stencil);
        }
    }
    UnmapMemory(device, memory);
    DestroyBuffer(device, buffer, NULL);
    FreeMemory(device, memory, NULL);
}

typedef struct {
    float x, y, z;
    uint32_t color;
    float u, v;
} Vertex;

static VkFormat physical_depth_format = VK_FORMAT_D24_UNORM_S8_UINT;

static void test_depth16(PokopiaVkAdapter *adapter)
{
    const NvnFrontTextureConfig config = {
        .target = 1, .width = 2048, .height = 2048, .depth = 1, .levels = 1,
        .format = 0x32, .flags = 0xc, .swizzle = {2, 3, 4, 5},
    };
    PokopiaVkTexture depth = {0};
    CHECK(pokopia_vk_texture_open(adapter, &config, &depth));
    assert(depth.format == VK_FORMAT_D16_UNORM && depth.aspects == VK_IMAGE_ASPECT_DEPTH_BIT);
    const VkRect2D full = {{0, 0}, {2048, 2048}}, patch = {{37, 129}, {47, 59}};
    CHECK(pokopia_vk_clear_depth_stencil(adapter, &depth, full, VK_IMAGE_ASPECT_DEPTH_BIT,
                                       (VkClearDepthStencilValue){0, 0}));
    CHECK(pokopia_vk_clear_depth_stencil(adapter, &depth, patch, VK_IMAGE_ASPECT_DEPTH_BIT,
                                       (VkClearDepthStencilValue){1, 0}));
    assert(pokopia_vk_clear_depth_stencil(adapter, &depth, full, VK_IMAGE_ASPECT_STENCIL_BIT,
                                        (VkClearDepthStencilValue){0, 0}) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    readback(adapter, NULL, &depth, 0, 0, &patch);
    pokopia_vk_texture_close(adapter, &depth);
    puts("PASS real 2048x2048 D16 clear and clipped depth readback");
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc == 2)
        physical_depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    setvbuf(stdout, NULL, _IONBF, 0);
    PokopiaVkAdapter context = {0};
    PokopiaVkAdapter *adapter = &context;
    assert(pokopia_vk_open(adapter, stdout));
    test_depth16(adapter);
    const char *vs_key = "821538a4398f85a7", *fs_key = "42c917e6eb2c6411";
    const VkShaderModule vs = load(adapter, vs_key), fs = load(adapter, fs_key);
    PokopiaVkTexture color = {0}, depth = {0};
    NvnFrontTextureConfig cfg = {.target = 4, .width = WIDTH, .height = HEIGHT, .depth = LAYERS, .levels = 1, .format = 0x25, .swizzle = {2, 3, 4, 5}};
    CHECK(pokopia_vk_texture_open(adapter, &cfg, &color));
    cfg.target = 1;
    cfg.depth = 1;
    cfg.format = 0x35;
    CHECK(pokopia_vk_texture_open(adapter, &cfg, &depth));

    const VkRect2D full = {{0, 0}, {WIDTH, HEIGHT}};
    const VkImageAspectFlags depth_stencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    CHECK(pokopia_vk_clear_depth_stencil(adapter, &depth, full, depth_stencil, (VkClearDepthStencilValue){0.25f, 0x5a}));
    CHECK(pokopia_vk_clear_depth_stencil(adapter, &depth, full, VK_IMAGE_ASPECT_DEPTH_BIT, (VkClearDepthStencilValue){1, 0}));
    readback(adapter, NULL, &depth, depth.format == VK_FORMAT_D24_UNORM_S8_UINT ? 0xffffff : 0x3f800000, 0x5a, NULL);
    CHECK(pokopia_vk_clear_depth_stencil(adapter, &depth, full, depth_stencil, (VkClearDepthStencilValue){0, 0}));

    seed_color(adapter, &color);
    const Vertex vertices[] = {{0, 0, 0, 0, 0, 0}, {0, HEIGHT, 0, 0, 0, 1},
                               {WIDTH, HEIGHT, 0, 0, 1, 1}, {WIDTH, 0, 0, 0, 1, 0}};
    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    const uint32_t vs_uniform[64] = {0x3a888889, 0xbaf2b9d6, 0xbf800000, 0x3f800000};
    PokopiaLiveDraw draw = {
        .target = &color, .depth_target = &depth, .target_layer = 3, .extent = {WIDTH, HEIGHT},
        .vertex_shader = {vs, "main", vs_key}, .fragment_shader = {fs, "main", fs_key},
        .vertices = {vertices, sizeof vertices}, .indices = {indices, sizeof indices},
        .vertex_uniform = {vs_uniform, sizeof vs_uniform}, .vertex_stride = sizeof(Vertex),
        .attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                       {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12}, {2, 0, VK_FORMAT_R32G32_SFLOAT, 16}},
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .index_type = VK_INDEX_TYPE_UINT16,
        .viewport = {0, HEIGHT, WIDTH, -HEIGHT, 0, 1}, .scissor = full,
        .color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT,
        .polygon_mode = VK_POLYGON_MODE_FILL, .index_count = 6, .instance_count = 1, .depth_write = true,
    };
    uint64_t serial = 0;
    CHECK(pokopia_live_draw(adapter, &draw, &serial));
    assert(serial == 1 && color.render_serial == serial);
    readback(adapter, &color, &depth, 0, 0, NULL);
    puts("PASS exact VS/FS wrote black to layer 3 only, preserving alpha and cleared D/S");
    FN(DestroyShaderModule);
    DestroyShaderModule(adapter->device, vs, NULL);
    DestroyShaderModule(adapter->device, fs, NULL);
    pokopia_vk_texture_close(adapter, &color);
    pokopia_vk_texture_close(adapter, &depth);
    pokopia_vk_close(adapter);
    return 0;
}

VkFormat nvn_guest_texture_vk_format(uint32_t format)
{
    switch (format) {
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x32: return VK_FORMAT_D16_UNORM;
    case 0x35: return physical_depth_format;
    default: abort();
    }
}
uint32_t nvn_guest_buffer_texel_bytes(uint32_t format) { (void)format; abort(); }




