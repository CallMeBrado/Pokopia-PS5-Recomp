#include "live_draw.h"
#include "nvn_guest_layout.h"
#include "composite-data.h"

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

static void check_color(PokopiaVkAdapter *adapter, PokopiaVkTexture *color, uint8_t sample[4])
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
    memcpy(sample, bytes + (count / 2 + color->extent.width / 2) * 4, 4);
    size_t matching = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *pixel = bytes + i * 4;
        assert(memcmp(pixel, (const uint8_t[4]){255, 0, 255, 255}, 4));
        assert(pixel[3] == 255);
        matching += !memcmp(pixel, sample, 4);
    }
    printf("center [%u,%u,%u,%u], matching pixels %zu/%zu\n",
           sample[0], sample[1], sample[2], sample[3], matching, count);
    assert(matching > count / 100);
    UnmapMemory(device, memory);
    DestroyBuffer(device, buffer, NULL);
    FreeMemory(device, memory, NULL);
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
static VkDevice check_device;
static unsigned pipelines, draws, binds, copies;

static VKAPI_ATTR VkResult VKAPI_CALL check_pipeline(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *out)
{
    assert(count == 1 && infos->pVertexInputState->vertexBindingDescriptionCount == 0 &&
           infos->pVertexInputState->vertexAttributeDescriptionCount == 0);
    assert(infos->pDepthStencilState && !infos->pDepthStencilState->depthTestEnable &&
           !infos->pDepthStencilState->depthWriteEnable);
    assert(!infos->pColorBlendState->pAttachments[0].blendEnable);
    ++pipelines;
    PFN_vkCreateGraphicsPipelines real = (PFN_vkCreateGraphicsPipelines)original_proc(device, "vkCreateGraphicsPipelines");
    return real(device, cache, count, infos, allocator, out);
}

static VKAPI_ATTR void VKAPI_CALL check_bind(VkCommandBuffer command, uint32_t first, uint32_t count,
    const VkBuffer *buffers, const VkDeviceSize *offsets)
{
    ++binds;
    PFN_vkCmdBindVertexBuffers real = (PFN_vkCmdBindVertexBuffers)original_proc(check_device, "vkCmdBindVertexBuffers");
    real(command, first, count, buffers, offsets);
}

static VKAPI_ATTR void VKAPI_CALL check_copy(VkCommandBuffer command, VkImage src, VkImageLayout src_layout,
    VkImage dst, VkImageLayout dst_layout, uint32_t count, const VkImageCopy *regions)
{
    ++copies;
    PFN_vkCmdCopyImage real = (PFN_vkCmdCopyImage)original_proc(check_device, "vkCmdCopyImage");
    real(command, src, src_layout, dst, dst_layout, count, regions);
}

static VKAPI_ATTR void VKAPI_CALL check_draw(VkCommandBuffer command, uint32_t vertex_count,
    uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
{
    assert(vertex_count == 3 && instance_count == 1 && !first_vertex && !first_instance);
    ++draws;
    PFN_vkCmdDraw real = (PFN_vkCmdDraw)original_proc(check_device, "vkCmdDraw");
    real(command, vertex_count, instance_count, first_vertex, first_instance);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL check_proc(VkDevice device, const char *name)
{
    if (!strcmp(name, "vkCreateGraphicsPipelines"))
        return (PFN_vkVoidFunction)check_pipeline;
    if (!strcmp(name, "vkCmdBindVertexBuffers"))
        return (PFN_vkVoidFunction)check_bind;
    if (!strcmp(name, "vkCmdCopyImage"))
        return (PFN_vkVoidFunction)check_copy;
    if (!strcmp(name, "vkCmdDraw"))
        return (PFN_vkVoidFunction)check_draw;
    return original_proc(device, name);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    PokopiaVkAdapter context = {0};
    PokopiaVkAdapter *adapter = &context;
    assert(pokopia_vk_open(adapter, stdout));
    original_proc = adapter->get_device_proc;
    check_device = adapter->device;
    adapter->get_device_proc = check_proc;

    const char *vs = "caa3cba58abf8cc9", *fs = "8526214507533768";
    const VkShaderModule vertex = load(adapter, vs), fragment = load(adapter, fs);
    PokopiaVkTexture hdr = {0}, srgb = {0}, depth = {0};
    open_texture(adapter, &hdr, 0x3f, WIDTH, HEIGHT);
    open_texture(adapter, &srgb, 0x38, WIDTH, HEIGHT);
    open_texture(adapter, &depth, 0x35, WIDTH, HEIGHT);

    const VkRect2D full = {{0, 0}, {WIDTH, HEIGHT}};
    const float sentinel[4] = {1, 0, 1, 1};
    CHECK(pokopia_vk_clear_color(adapter, &srgb, sentinel));
    CHECK(pokopia_vk_clear_color(adapter, &hdr, (const float[4]){0.125f, 0.25f, 0.5f, 1}));
    CHECK(pokopia_vk_clear_depth_stencil(adapter, &depth, full, depth.aspects, (VkClearDepthStencilValue){0, 0}));
    const NvnFrontSamplerInfo linear = {.min_filter = 3, .mag_filter = 1, .wrap = {7, 7, 7}, .max_anisotropy = 1};
    PokopiaLiveDraw draw = {
        .target = &srgb, .depth_target = &depth, .extent = {WIDTH, HEIGHT},
        .vertex_shader = {vertex, "main", vs}, .fragment_shader = {fragment, "main", fs},
        .vertex_uniform = {vs_composite, sizeof vs_composite},
        .fragment_uniform = {fs_composite, sizeof fs_composite},
        .fragment_samples = {{.texture = &hdr, .sampler = linear}},
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .polygon_mode = VK_POLYGON_MODE_FILL,
        .viewport = {0, HEIGHT, WIDTH, -HEIGHT, 0, 1}, .scissor = full,
        .color_write_mask = 15, .vertex_count = 3, .instance_count = 1,
    };
    uint64_t serial = 0;
    assert(pokopia_live_draw(adapter, &draw, &serial) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    draw.vertex_constants = (PokopiaLiveDrawSpan){c1_composite, sizeof c1_composite};
    assert(pokopia_live_draw(adapter, &draw, &serial) == VK_SUCCESS);
    assert(serial == srgb.render_serial && serial != 0 && pipelines == 1 && draws == 1 && !binds && !copies);
    assert(hdr.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    uint8_t sample1[4], sample2[4];
    check_color(adapter, &srgb, sample1);

    CHECK(pokopia_vk_clear_color(adapter, &srgb, sentinel));
    CHECK(pokopia_vk_clear_color(adapter, &hdr, (const float[4]){0.75f, 0.5f, 0.25f, 1}));
    CHECK(pokopia_live_draw(adapter, &draw, &serial));
    assert(pipelines == 2 && draws == 2 && !binds && !copies);
    check_color(adapter, &srgb, sample2);
    assert(memcmp(sample1, sample2, sizeof sample1));
    puts("PASS real composite SPIR-V, authentic c1/c5, vertexless scene sample and full GPU pixel coverage");
    FN(DestroyShaderModule);
    DestroyShaderModule(adapter->device, vertex, NULL);
    DestroyShaderModule(adapter->device, fragment, NULL);
    pokopia_vk_texture_close(adapter, &hdr);
    pokopia_vk_texture_close(adapter, &srgb);
    pokopia_vk_texture_close(adapter, &depth);
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


