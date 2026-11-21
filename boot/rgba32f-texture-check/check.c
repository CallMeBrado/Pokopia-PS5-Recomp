#include "vk_adapter.h"
#include "nvn_guest_layout.h"

#include <assert.h>
#include <inttypes.h>
#include <string.h>

static PFN_vkGetDeviceProcAddr original_proc;
static unsigned image_calls, view_calls, storage_views;
static uint32_t width = 2;
static bool expect_storage;

static VKAPI_ATTR VkResult VKAPI_CALL inspect_create_image(VkDevice device,
    const VkImageCreateInfo *info, const VkAllocationCallbacks *allocation, VkImage *image)
{
    ++image_calls;
    assert(info->imageType == VK_IMAGE_TYPE_2D && info->format == VK_FORMAT_R32G32B32A32_SFLOAT);
    assert(info->extent.width == width && info->extent.height == 1 && info->extent.depth == 1);
    assert(info->mipLevels == 1 && info->arrayLayers == 1);
    assert(info->samples == VK_SAMPLE_COUNT_1_BIT && info->tiling == VK_IMAGE_TILING_OPTIMAL);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (expect_storage)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    assert(info->usage == usage);
    PFN_vkCreateImage create = (PFN_vkCreateImage)original_proc(device, "vkCreateImage");
    return create(device, info, allocation, image);
}

static VKAPI_ATTR VkResult VKAPI_CALL inspect_create_view(VkDevice device,
    const VkImageViewCreateInfo *info, const VkAllocationCallbacks *allocation, VkImageView *view)
{
    ++view_calls;
    const VkImageSubresourceRange *range = &info->subresourceRange;
    assert(info->viewType == VK_IMAGE_VIEW_TYPE_2D && info->format == VK_FORMAT_R32G32B32A32_SFLOAT);
    assert(range->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && range->baseMipLevel == 0 &&
           range->levelCount == 1 && range->baseArrayLayer == 0 && range->layerCount == 1);
    const VkImageViewUsageCreateInfo *usage = info->pNext;
    assert(usage && usage->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO);
    if (usage->usage == VK_IMAGE_USAGE_STORAGE_BIT)
        ++storage_views;
    else
        assert(usage->usage == (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
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
    adapter.get_device_proc = inspect_proc;

    NvnFrontTextureConfig config = {
        .target = 1, .width = 2, .height = 1, .depth = 1, .levels = 1,
        .format = 0x2e, .swizzle = {2, 3, 4, 5},
    };
    uint64_t size = 0, alignment = 0;
    assert(nvn_guest_texture_layout(&config, &size, &alignment));
    assert(nvn_guest_texture_vk_format(config.format) == VK_FORMAT_R32G32B32A32_SFLOAT);
    assert(nvn_guest_buffer_texel_bytes(config.format) == 16);
    printf("Guest RGBA32F storage size=%" PRIu64 ", alignment=%" PRIu64 "\n", size, alignment);
    PokopiaVkTexture texture = {0};
    VkResult result = pokopia_vk_texture_open(&adapter, &config, &texture);
    printf("2x1 RGBA32F sampled/color attachment: VkResult=%d, Vulkan bytes=%" PRIu64 "\n", result, texture.backing.bytes);
    assert(result == VK_SUCCESS && texture.view && !texture.storage_view);
    assert(image_calls == 1 && view_calls == 1);
    pokopia_vk_texture_close(&adapter, &texture);

    config.flags = 0x4;
    expect_storage = true;
    result = pokopia_vk_texture_open(&adapter, &config, &texture);
    printf("2x1 RGBA32F sampled/color/storage: VkResult=%d, Vulkan bytes=%" PRIu64 "\n", result, texture.backing.bytes);
    if (result == VK_SUCCESS) {
        assert(texture.storage_view && image_calls == 2 && view_calls == 3 && storage_views == 1);
        pokopia_vk_texture_close(&adapter, &texture);
    } else {
        assert(result == VK_ERROR_FORMAT_NOT_SUPPORTED && image_calls == 1 && view_calls == 1);
    }

    width = config.width = 4;
    config.flags = 0x10;
    config.stride = 32;
    expect_storage = false;
    unsigned previous_images = image_calls;
    assert(pokopia_vk_texture_open(&adapter, &config, &texture) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    assert(image_calls == previous_images);
    config.stride = 64;
    assert(pokopia_vk_texture_open(&adapter, &config, &texture) == VK_SUCCESS);
    assert(image_calls == previous_images + 1 && texture.view && !texture.storage_view);
    pokopia_vk_texture_close(&adapter, &texture);

    pokopia_vk_close(&adapter);
    puts("PASS RGBA32F sampled/color allocation, storage capability, and 16-byte texel stride.");
    return 0;
}
