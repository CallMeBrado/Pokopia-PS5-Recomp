#include "live_draw.h"
#include "nvn_guest_layout.h"
#include "texture_upload.h"

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

static void check_color(PokopiaVkAdapter *adapter, PokopiaVkTexture *color, const uint8_t expected[4])
{
    FN(CreateBuffer); FN(DestroyBuffer); FN(GetBufferMemoryRequirements); FN(AllocateMemory); FN(FreeMemory);
    FN(BindBufferMemory); FN(MapMemory); FN(UnmapMemory); FN(CmdCopyImageToBuffer); FN(CmdPipelineBarrier);

    const VkDevice device = adapter->device;
    const size_t count = (size_t)color->extent.width * color->extent.height;
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = count * 4,
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
    transition(adapter, command, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferImageCopy region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {color->extent.width, color->extent.height, 1},
    };
    CmdCopyImageToBuffer(command, color->backing.image, color->layout, buffer, 1, &region);
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
    printf("GPU readback %ux%u [%u,%u,%u,%u]\n", color->extent.width, color->extent.height,
           bytes[0], bytes[1], bytes[2], bytes[3]);
    for (size_t i = 0; i < count; ++i)
        assert(!memcmp(bytes + i * 4, expected, 4));
    UnmapMemory(device, memory);
    DestroyBuffer(device, buffer, NULL);
    FreeMemory(device, memory, NULL);
}

static void upload_exposure(PokopiaVkAdapter *adapter, PokopiaVkTexture *exposure)
{
    size_t bytes;
    void *source = read_file("../../frame-reference-655360/loading-replay/texture-trace/t377.bin", &bytes);
    assert(bytes == 32);
    NvnTextureUpload *upload = NULL;
    CHECK(nvn_texture_upload_open(&upload, adapter->device, adapter->queue, adapter->family, &adapter->memory,
                                  adapter->get_device_proc, bytes));
    const NvnTextureUploadRegion region = {
        .image = exposure->backing.image, .format = exposure->format, .mip_extent = exposure->extent,
        .extent = exposure->extent,
        .before = {exposure->layout, exposure->stages, exposure->access},
        .after = {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_SHADER_READ_BIT},
    };
    CHECK(nvn_texture_upload_submit(upload, &region, source, bytes, 0));
    CHECK(nvn_texture_upload_wait(upload, UINT64_MAX));
    CHECK(nvn_texture_upload_close(&upload));
    exposure->layout = region.after.layout;
    exposure->stages = region.after.stages;
    exposure->access = region.after.access;
    free(source);
}

static void open_texture(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture, uint32_t format,
                         uint32_t width, uint32_t height)
{
    const NvnFrontTextureConfig config = {
        .target = 1, .format = format, .width = width, .height = height,
        .depth = 1, .levels = 1, .swizzle = {2, 3, 4, 5},
    };
    CHECK(pokopia_vk_texture_open(adapter, &config, texture));
}

static PFN_vkGetDeviceProcAddr original_proc;
static unsigned draws, copies;
static VKAPI_ATTR void VKAPI_CALL checked_draw(VkCommandBuffer command, uint32_t vertex_count,
                                               uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
{
    assert(vertex_count == 6 && instance_count == 1 && !first_vertex && !first_instance);
    extern VkDevice check_device;
    PFN_vkCmdDraw draw = (PFN_vkCmdDraw)original_proc(check_device, "vkCmdDraw");
    ++draws;
    draw(command, vertex_count, instance_count, first_vertex, first_instance);
}

static VKAPI_ATTR void VKAPI_CALL checked_copy(VkCommandBuffer command, VkImage source, VkImageLayout src_layout,
                                               VkImage destination, VkImageLayout dst_layout, uint32_t count,
                                               const VkImageCopy *regions)
{
    assert(source != destination && src_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
           dst_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && count == 1 &&
           regions[0].extent.width == WIDTH && regions[0].extent.height == HEIGHT);
    extern VkDevice check_device;
    PFN_vkCmdCopyImage copy = (PFN_vkCmdCopyImage)original_proc(check_device, "vkCmdCopyImage");
    ++copies;
    copy(command, source, src_layout, destination, dst_layout, count, regions);
}

VkDevice check_device;
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL checked_proc(VkDevice device, const char *name)
{
    if (!strcmp(name, "vkCmdDraw"))
        return (PFN_vkVoidFunction)checked_draw;
    if (!strcmp(name, "vkCmdCopyImage"))
        return (PFN_vkVoidFunction)checked_copy;
    return original_proc(device, name);
}

typedef struct {
    float x, y, z;
    uint32_t color;
    float u, v;
} Vertex;

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    PokopiaVkAdapter context = {0};
    PokopiaVkAdapter *adapter = &context;
    assert(pokopia_vk_open(adapter, stdout));
    original_proc = adapter->get_device_proc;
    check_device = adapter->device;
    adapter->get_device_proc = checked_proc;
    const char *vs_feedback = "70b2ae4bef0dfe46", *fs_feedback = "1c913e7b97e7e999",
               *vs_quad = "821538a4398f85a7", *fs_tonemap = "ff03fa7b63a79526", *fs_downsample = "c4664b38f6a87835";
    const VkShaderModule v2 = load(adapter, vs_feedback), f2 = load(adapter, fs_feedback), v3 = load(adapter, vs_quad),
                         f3 = load(adapter, fs_tonemap), f4 = load(adapter, fs_downsample);
    PokopiaVkTexture hdr = {0}, exposure = {0}, srgb = {0}, depth = {0}, downsample = {0};
    open_texture(adapter, &hdr, 0x3f, WIDTH, HEIGHT);
    open_texture(adapter, &srgb, 0x38, WIDTH, HEIGHT);
    open_texture(adapter, &depth, 0x35, WIDTH, HEIGHT);
    open_texture(adapter, &exposure, 0x2e, 2, 1);
    open_texture(adapter, &downsample, 0x38, 30, 16);
    upload_exposure(adapter, &exposure);

    const VkRect2D full = {{0, 0}, {WIDTH, HEIGHT}};
    const float black[4] = {0, 0, 0, 1};
    const NvnFrontSamplerInfo linear = {.min_filter = 3, .mag_filter = 1, .wrap = {7, 7, 7}, .max_anisotropy = 1};
    const NvnFrontSamplerInfo nearest = {.min_filter = 2, .wrap = {7, 7, 7}, .max_anisotropy = 1};
    CHECK(pokopia_vk_clear_color(adapter, &hdr, black));
    CHECK(pokopia_vk_clear_depth_stencil(adapter, &depth, full, depth.aspects, (VkClearDepthStencilValue){0, 0}));
    PokopiaLiveDraw draw = {
        .target = &hdr, .depth_target = &depth, .extent = {WIDTH, HEIGHT},
        .vertex_shader = {v2, "main", vs_feedback}, .fragment_shader = {f2, "main", fs_feedback},
        .fragment_samples = {{&hdr, linear}},
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .polygon_mode = VK_POLYGON_MODE_FILL,
        .viewport = {0, HEIGHT, WIDTH, -HEIGHT, 0, 1}, .scissor = full,
        .color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT,
        .vertex_count = 6, .instance_count = 1,
    };
    uint64_t serial = 0;
    CHECK(pokopia_live_draw(adapter, &draw, &serial));
    assert(serial == 2 && draws == 1 && copies == 1 && hdr.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    check_color(adapter, &hdr, (const uint8_t[4]){0, 0, 0, 0});
    puts("PASS exact unindexed feedback draw used one snapshot");

    const Vertex vertices[] = {{0, 0, 0, 0x01000000, 0, 0}, {0, HEIGHT, 0, 0x01000000, 0, 1},
                               {WIDTH, HEIGHT, 0, 0x01000000, 1, 1}, {WIDTH, 0, 0, 0x01000000, 1, 0}};
    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    const uint32_t vs_uniform[64] = {0x3a888889, 0xbaf2b9d6, 0xbf800000, 0x3f800000};
    const uint32_t fs_uniform[64] = {0x3e800000, 0, 0, 0, 0x3f800000, 0x3e4ccccd};
    draw.target = &srgb;
    draw.vertex_shader = (PokopiaLiveDrawShader){v3, "main", vs_quad};
    draw.fragment_shader = (PokopiaLiveDrawShader){f3, "main", fs_tonemap};
    draw.fragment_samples[0] = (PokopiaLiveDrawSample){&exposure, nearest};
    draw.fragment_samples[1] = (PokopiaLiveDrawSample){&hdr, linear};
    draw.vertex_stride = sizeof(Vertex);
    draw.vertex_count = 0;
    draw.index_count = 6;
    draw.index_type = VK_INDEX_TYPE_UINT16;
    draw.color_write_mask |= VK_COLOR_COMPONENT_A_BIT;
    draw.depth_write = true;
    const VkVertexInputAttributeDescription attrib[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12}, {2, 0, VK_FORMAT_R32G32_SFLOAT, 16},
    };
    memcpy(draw.attributes, attrib, sizeof attrib);
    draw.vertices = (PokopiaLiveDrawSpan){vertices, sizeof vertices};
    draw.indices = (PokopiaLiveDrawSpan){indices, sizeof indices};
    draw.vertex_uniform = (PokopiaLiveDrawSpan){vs_uniform, sizeof vs_uniform};
    draw.fragment_uniform = (PokopiaLiveDrawSpan){fs_uniform, sizeof fs_uniform};
    CHECK(pokopia_live_draw(adapter, &draw, &serial));
    assert(serial == 3 && draws == 1 && copies == 1);
    check_color(adapter, &srgb, (const uint8_t[4]){3, 3, 3, 255});
    puts("PASS exact tonemap shader and two captured sampler bindings match desktop snapshot");

    CHECK(pokopia_vk_clear_color(adapter, &srgb, black));
    const Vertex small_vertices[] = {{0, 0, 0, 0xffffffff, 0.15f, 0.15f}, {0, 16, 0, 0xffffffff, 0.15f, 0.85f},
                                     {30, 16, 0, 0xffffffff, 0.85f, 0.85f}, {30, 0, 0, 0xffffffff, 0.85f, 0.15f}};
    const uint32_t small_uniform[64] = {0x3d888889, 0xbe000000, 0xbf800000, 0x3f800000};
    draw.target = &downsample;
    draw.depth_target = NULL;
    draw.extent = downsample.extent;
    draw.scissor = (VkRect2D){{0, 0}, downsample.extent};
    draw.viewport = (VkViewport){0, 16, 30, -16, 0, 1};
    draw.fragment_shader = (PokopiaLiveDrawShader){f4, "main", fs_downsample};
    draw.fragment_samples[0] = (PokopiaLiveDrawSample){&srgb, linear};
    draw.fragment_samples[1] = (PokopiaLiveDrawSample){0};
    draw.vertices = (PokopiaLiveDrawSpan){small_vertices, sizeof small_vertices};
    draw.vertex_uniform = (PokopiaLiveDrawSpan){small_uniform, sizeof small_uniform};
    draw.fragment_uniform = (PokopiaLiveDrawSpan){0};
    CHECK(pokopia_live_draw(adapter, &draw, &serial));
    assert(serial == 5 && draws == 1 && copies == 1);
    check_color(adapter, &downsample, (const uint8_t[4]){0, 0, 0, 255});
    puts("PASS exact downsample matches desktop 30x16 snapshot");

    FN(DestroyShaderModule);
    const VkShaderModule shaders[] = {v2, f2, v3, f3, f4};
    for (unsigned i = 0; i < sizeof shaders / sizeof shaders[0]; ++i)
        DestroyShaderModule(adapter->device, shaders[i], NULL);
    pokopia_vk_texture_close(adapter, &hdr);
    pokopia_vk_texture_close(adapter, &srgb);
    pokopia_vk_texture_close(adapter, &depth);
    pokopia_vk_texture_close(adapter, &exposure);
    pokopia_vk_texture_close(adapter, &downsample);
    pokopia_vk_close(adapter);
    return 0;
}

VkFormat nvn_guest_texture_vk_format(uint32_t format)
{
    switch (format) {
    case 0x2e: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x35: return VK_FORMAT_D24_UNORM_S8_UINT;
    case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x3f: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    default: abort();
    }
}
uint32_t nvn_guest_buffer_texel_bytes(uint32_t format) { (void)format; abort(); }


