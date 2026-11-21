#include "live_draw.h"
#include "nvn_guest_layout.h"
#include "texture_upload.h"
#include "live_texture_upload.h"
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

static void save_color(PokopiaVkAdapter *adapter, PokopiaVkTexture *color, const char *path)
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
    FILE *output = fopen(path, "wb");
    assert(output && fwrite(bytes, 4, count, output) == count && fclose(output) == 0);
    UnmapMemory(device, memory);
    DestroyBuffer(device, buffer, NULL);
    FreeMemory(device, memory, NULL);
}

static const char *fixture_directory = "../../frame-reference-655360/loading-replay";

static void *fixture(const char *name, size_t *bytes)
{
    char path[128];
    const int length = snprintf(path, sizeof path, "%s/%s", fixture_directory, name);
    assert(length > 0 && (size_t)length < sizeof path);
    return read_file(path, bytes);
}

static PokopiaLiveDrawSpan fixture_span(const char *name)
{
    size_t bytes;
    const void *data = fixture(name, &bytes);
    return (PokopiaLiveDrawSpan){data, bytes};
}

enum { FIXTURE_TEXTURE = 0x1000, FIXTURE_STORAGE = 0x2000, FIXTURE_SOURCE = 0x3000, FIXTURE_REGION = 0x4000 };
typedef struct {
    NvnFrontTextureInfo info;
    void *tiled, *packed;
    size_t bytes;
    uint32_t region[6];
} DeferredBc7;

bool nvn_front_get_texture(NvnFront *front, uint64_t address, NvnFrontTextureInfo *info)
{
    if (address != FIXTURE_TEXTURE)
        return false;
    *info = ((DeferredBc7 *)front)->info;
    return true;
}

bool nvn_front_get_pool(NvnFront *front, uint64_t address, NvnFrontPoolConfig *config)
{
    (void)front;
    (void)address;
    (void)config;
    return false;
}

static void *map_fixture(void *opaque, uint64_t address, size_t bytes)
{
    DeferredBc7 *image = opaque;
    if (address == FIXTURE_STORAGE && bytes == image->info.create_info.storage_size)
        return image->tiled;
    if (address == FIXTURE_SOURCE && bytes == image->bytes)
        return image->packed;
    if (address == FIXTURE_REGION && bytes == sizeof image->region)
        return image->region;
    return NULL;
}

static const void *read_fixture(void *opaque, uint64_t address, size_t bytes)
{
    return map_fixture(opaque, address, bytes);
}

static void prepare_bc7(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                        uint32_t width, uint32_t height, const char *name)
{
    DeferredBc7 image = {.info = {.native = texture, .create_info = {
        .config = {.target = 1, .format = 0x4d, .width = width, .height = height, .depth = 1,
            .levels = 1, .swizzle = {2, 3, 4, 5}}, .storage_address = FIXTURE_STORAGE,
    }}, .region = {0, 0, 0, width, height, 1}};
    NvnFrontTextureCreateInfo *info = &image.info.create_info;
    assert(nvn_guest_texture_layout(&info->config, &info->storage_size, &info->storage_alignment));
    image.tiled = calloc(1, (size_t)info->storage_size);
    assert(image.tiled);
    image.packed = fixture(name, &image.bytes);
    PokopiaLiveTextureUpload upload = {.front = (NvnFront *)&image, .adapter = adapter,
        .guest_opaque = &image, .read_guest = read_fixture, .write_guest = map_fixture, .log = stdout};
    CHECK(pokopia_vk_texture_open_lazy(adapter, &info->config, texture));
    assert(texture->deferred && !texture->backing.image && !texture->sample_alias_view);
    const uint64_t args[32] = {FIXTURE_TEXTURE, 0, FIXTURE_REGION, FIXTURE_SOURCE};
    assert(pokopia_live_texture_upload_dispatch(&upload, "nvnTextureWriteTexels", args) == NVN_FRONT_HANDLED);
    assert(texture->deferred && !texture->backing.image && !upload.staging);
    CHECK(pokopia_live_texture_prepare_sample(&upload, &image.info));
    assert(!texture->deferred && texture->backing.image && texture->sample_alias_view);
    CHECK(pokopia_live_texture_upload_close(&upload));
    free(image.packed);
    free(image.tiled);
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

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    PokopiaVkAdapter context = {0};
    PokopiaVkAdapter *adapter = &context;
    assert(pokopia_vk_open(adapter, stdout));
    const char *vs_key = "a26763a4bdd7b361", *fs_key = "2db3327d9fbc5bb3";
    const VkShaderModule vs = load(adapter, vs_key), fs = load(adapter, fs_key);
    PokopiaVkTexture srgb = {0}, depth = {0}, bc7[2] = {0};
    open_texture(adapter, &srgb, 0x38, WIDTH, HEIGHT);
    open_texture(adapter, &depth, 0x35, WIDTH, HEIGHT);

    const VkRect2D full = {{0, 0}, {WIDTH, HEIGHT}};
    const float black[4] = {0, 0, 0, 1};
    CHECK(pokopia_vk_clear_color(adapter, &srgb, black));
    CHECK(pokopia_vk_clear_depth_stencil(adapter, &depth, full, depth.aspects, (VkClearDepthStencilValue){0, 0}));

    PokopiaLiveDraw draw = {
        .target = &srgb, .depth_target = &depth, .extent = {WIDTH, HEIGHT},
        .vertex_shader = {vs, "main", vs_key}, .fragment_shader = {fs, "main", fs_key},
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .polygon_mode = VK_POLYGON_MODE_FILL,
        .viewport = {0, HEIGHT, WIDTH, -HEIGHT, 0, 1}, .scissor = full,
        .color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .vertex_stride = 44, .index_type = VK_INDEX_TYPE_UINT16, .instance_count = 1, .blend_enable = true,
        .attributes = {{0, 0, VK_FORMAT_R32_SFLOAT, 40}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
                       {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 36}, {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}},
        .blend = {.src_rgb = 5, .dst_rgb = 6, .src_alpha = 5, .dst_alpha = 6, .rgb_equation = 1, .alpha_equation = 1},
    };
    draw.vertices = fixture_span("vertices.bin");
    const char *index_files[] = {"draw3-indices.bin", "draw4-indices.bin"};
    const char *vertex_files[] = {"draw3-vs-ubo.bin", "draw4-vs-ubo.bin"};
    const char *fragment_files[] = {"draw3-fs-ubo.bin", "draw4-fs-ubo.bin"};
    const char *outputs[] = {"after-draw3.rgba", "after-draw4.rgba"};
    uint64_t serial = 0;
    for (unsigned i = 0; i < 2; ++i) {
        prepare_bc7(adapter, &bc7[i], i ? 1024 : 4, i ? 2048 : 16, i ? "draw4-bc7.bin" : "draw3-bc7.bin");
        draw.indices = fixture_span(index_files[i]);
        draw.index_count = (uint32_t)(draw.indices.bytes / 2);
        draw.vertex_uniform = fixture_span(vertex_files[i]);
        draw.fragment_uniform = fixture_span(fragment_files[i]);
        draw.fragment_samples[0] = (PokopiaLiveDrawSample){
            .texture = &bc7[i], .view_format = VK_FORMAT_BC7_SRGB_BLOCK,
            .sampler = {.min_filter = 2 + i, .mag_filter = i, .wrap = {7, 7, 7}, .max_anisotropy = 1},
        };
        if (!i) {
            draw.fragment_samples[0].view_format = VK_FORMAT_UNDEFINED;
            assert(pokopia_live_draw(adapter, &draw, &serial) == VK_ERROR_FORMAT_NOT_SUPPORTED);
            draw.fragment_samples[0].view_format = VK_FORMAT_BC7_SRGB_BLOCK;
        }
        CHECK(pokopia_live_draw(adapter, &draw, &serial));
        assert(serial == 2 + i);
        save_color(adapter, &srgb, outputs[i]);
        free((void *)draw.indices.data);
        free((void *)draw.vertex_uniform.data);
        free((void *)draw.fragment_uniform.data);
    }
    puts("PASS actual cached loading shaders rendered both captured indexed draws");
    free((void *)draw.vertices.data);

    FN(DestroyShaderModule);
    DestroyShaderModule(adapter->device, vs, NULL);
    DestroyShaderModule(adapter->device, fs, NULL);
    pokopia_vk_texture_close(adapter, &srgb);
    pokopia_vk_texture_close(adapter, &depth);
    pokopia_vk_texture_close(adapter, &bc7[0]);
    pokopia_vk_texture_close(adapter, &bc7[1]);
    pokopia_vk_close(adapter);
    return 0;
}

