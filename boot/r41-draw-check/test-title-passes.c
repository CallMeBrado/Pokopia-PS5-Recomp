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
enum { WIDTH = 80, HEIGHT = 45, PIXELS = WIDTH * HEIGHT };

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

static void readback(PokopiaVkAdapter *adapter, PokopiaVkTexture *color, PokopiaVkTexture *depth,
                     uint32_t expected_depth, uint8_t expected_stencil)
{
    FN(CreateBuffer); FN(DestroyBuffer); FN(GetBufferMemoryRequirements); FN(AllocateMemory); FN(FreeMemory);
    FN(BindBufferMemory); FN(MapMemory); FN(UnmapMemory); FN(CmdCopyImageToBuffer); FN(CmdPipelineBarrier);

    const VkDevice device = adapter->device;
    const size_t colors = color ? PIXELS * 4 : 0;
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = colors + PIXELS * 5,
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
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {WIDTH, HEIGHT, 1},
        };
        CmdCopyImageToBuffer(command, color->backing.image, color->layout, buffer, 1, &region);
    }
    transition(adapter, command, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferImageCopy regions[] = {
        {.bufferOffset = colors, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {WIDTH, HEIGHT, 1}},
        {.bufferOffset = colors + PIXELS * 4, .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1}, .imageExtent = {WIDTH, HEIGHT, 1}},
    };
    CmdCopyImageToBuffer(command, depth->backing.image, depth->layout, buffer, 2, regions);
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
        printf("Readback RGBA [%u,%u,%u,%u]\n", bytes[0], bytes[1], bytes[2], bytes[3]);
        for (size_t i = 0; i < PIXELS; ++i) {
            const uint8_t *pixel = bytes + i * 4;
            assert(abs((int)pixel[0] - 64) <= 1 && abs((int)pixel[1] - 128) <= 1 &&
                   abs((int)pixel[2] - 191) <= 1 && pixel[3] == 255);
        }
    }
    const uint32_t *depth_words = (const uint32_t *)(bytes + colors);
    const uint8_t *stencil = bytes + colors + PIXELS * 4;
    printf("Readback depth=%08x stencil=%02x format=%u\n", depth_words[0], stencil[0], depth->format);
    for (size_t i = 0; i < PIXELS; ++i) {
        assert((depth_words[i] & (depth->format == VK_FORMAT_D24_UNORM_S8_UINT ? 0x00ffffffu : 0xffffffffu)) ==
               expected_depth);
        assert(stencil[i] == expected_stencil);
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

static void open_texture(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture, uint32_t format,
                         uint32_t width, uint32_t height)
{
    const NvnFrontTextureConfig config = {
        .target = 1, .format = format, .width = width, .height = height,
        .depth = 1, .levels = 1, .swizzle = {2, 3, 4, 5},
    };
    CHECK(pokopia_vk_texture_open(adapter, &config, texture));
}

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
