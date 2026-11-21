#include "vk_adapter.h"
#include "nvn_guest_layout.h"

#include <assert.h>
#include <inttypes.h>
#include <string.h>

static PFN_vkGetDeviceProcAddr original_proc;
static PFN_vkGetPhysicalDeviceImageFormatProperties2 original_query;
static unsigned query_calls, image_calls, view_calls, sampled_views, storage_views;
static uint32_t edge = 1;
static uint32_t levels = 1;
static bool limit_layers;

static VKAPI_ATTR VkResult VKAPI_CALL inspect_query(VkPhysicalDevice device,
    const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *properties)
{
    ++query_calls;
    assert(info->type == VK_IMAGE_TYPE_2D && info->tiling == VK_IMAGE_TILING_OPTIMAL);
    assert(info->format == VK_FORMAT_R8G8B8A8_UNORM);
    assert(info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    assert(info->usage & VK_IMAGE_USAGE_STORAGE_BIT);
    VkResult result = original_query(device, info, properties);
    if (result == VK_SUCCESS) {
        assert(properties->imageFormatProperties.maxArrayLayers >= 6);
        if (limit_layers)
            properties->imageFormatProperties.maxArrayLayers = 5;
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL inspect_create_image(VkDevice device,
    const VkImageCreateInfo *info, const VkAllocationCallbacks *allocation, VkImage *image)
{
    ++image_calls;
    assert(info->imageType == VK_IMAGE_TYPE_2D && info->format == VK_FORMAT_R8G8B8A8_UNORM);
    assert(info->extent.width == edge && info->extent.height == edge && info->extent.depth == 1);
    assert(info->mipLevels == levels && info->arrayLayers == 6);
    assert(info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    assert(info->samples == VK_SAMPLE_COUNT_1_BIT && info->tiling == VK_IMAGE_TILING_OPTIMAL);
    const VkImageUsageFlags expected = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(info->usage == expected);
    PFN_vkCreateImage create = (PFN_vkCreateImage)original_proc(device, "vkCreateImage");
    return create(device, info, allocation, image);
}

static VKAPI_ATTR VkResult VKAPI_CALL inspect_create_view(VkDevice device,
    const VkImageViewCreateInfo *info, const VkAllocationCallbacks *allocation, VkImageView *view)
{
    ++view_calls;
    const VkImageSubresourceRange *range = &info->subresourceRange;
    assert(info->format == VK_FORMAT_R8G8B8A8_UNORM && range->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
    assert(range->baseMipLevel == 0 && range->baseArrayLayer == 0);
    const VkImageViewUsageCreateInfo *usage = info->pNext;
    assert(usage && usage->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO);
    if (usage->usage == VK_IMAGE_USAGE_SAMPLED_BIT) {
        ++sampled_views;
        assert(info->viewType == VK_IMAGE_VIEW_TYPE_CUBE && range->layerCount == 6 && range->levelCount == levels);
    } else if (usage->usage == VK_IMAGE_USAGE_STORAGE_BIT) {
        ++storage_views;
        assert(info->viewType == VK_IMAGE_VIEW_TYPE_CUBE && range->layerCount == 6 && range->levelCount == 1);
    } else {
        assert(usage->usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        assert(info->viewType == VK_IMAGE_VIEW_TYPE_2D && range->layerCount == 1 && range->levelCount == 1);
    }
    PFN_vkCreateImageView create = (PFN_vkCreateImageView)original_proc(device, "vkCreateImageView");
    return create(device, info, allocation, view);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL inspect_proc(VkDevice device, const char *name)
{
    if (!strcmp(name, "vkCreateImage"))
        return (PFN_vkVoidFunction)inspect_create_image;
    if (!strcmp(name, "vkCreateImageView"))
        return (PFN_vkVoidFunction)inspect_create_view;
    return original_proc(device, name);
}

int main(void)
{
    PokopiaVkAdapter adapter = {0};
    assert(pokopia_vk_open(&adapter, stdout));
    original_proc = adapter.get_device_proc;
    original_query = adapter.image_format_properties;
    adapter.get_device_proc = inspect_proc;
    adapter.image_format_properties = inspect_query;

    NvnFrontTextureConfig config = {
        .target = 8, .width = 1, .height = 1, .depth = 6, .levels = 1,
        .format = 0x25, .flags = 0x4, .swizzle = {2, 3, 4, 5},
    };
    uint64_t size = 0, alignment = 0;
    assert(nvn_guest_texture_layout(&config, &size, &alignment));
    assert(nvn_guest_texture_vk_format(config.format) == VK_FORMAT_R8G8B8A8_UNORM);
    printf("Guest cube storage size=%" PRIu64 ", alignment=%" PRIu64 "\n", size, alignment);
    PokopiaVkTexture texture = {0};
    VkResult result = pokopia_vk_texture_open(&adapter, &config, &texture);
    printf("1x1 RGBA8 storage cube: VkResult=%d, Vulkan bytes=%" PRIu64 "\n", result, texture.backing.bytes);
    assert(result == VK_SUCCESS && texture.backing.image && texture.backing.memory);
    assert(texture.view && texture.sample_view && texture.storage_view);
    assert(query_calls == 1 && image_calls == 1 && view_calls == 3 && sampled_views == 1 && storage_views == 1);
    pokopia_vk_texture_close(&adapter, &texture);
    assert(!texture.backing.image && !texture.view && !texture.sample_view && !texture.storage_view);

    edge = 8;
    levels = 4;
    config.width = config.height = edge;
    config.depth = 1;
    config.levels = levels;
    result = pokopia_vk_texture_open(&adapter, &config, &texture);
    printf("8x8 RGBA8 storage cube with 4 mips and SetSize2D depth: VkResult=%d\n", result);
    assert(result == VK_SUCCESS && query_calls == 2 && image_calls == 2 && view_calls == 6);
    pokopia_vk_texture_close(&adapter, &texture);

    limit_layers = true;
    result = pokopia_vk_texture_open(&adapter, &config, &texture);
    assert(result == VK_ERROR_FORMAT_NOT_SUPPORTED && !texture.backing.image && query_calls == 3 && image_calls == 2);
    config.height = 4;
    result = pokopia_vk_texture_open(&adapter, &config, &texture);
    assert(result == VK_ERROR_FORMAT_NOT_SUPPORTED && !texture.backing.image && query_calls == 3 && image_calls == 2);

    pokopia_vk_close(&adapter);
    puts("PASS cube image/view shapes, six-layer capability check, and rectangular cube rejection");
    return 0;
}
