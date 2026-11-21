#include "vk_adapter.h"
#include "nvn_guest_layout.h"

#include <assert.h>
#include <inttypes.h>
#include <string.h>

static PFN_vkGetDeviceProcAddr original_proc;
static unsigned image_calls;
static unsigned view_calls;
static unsigned storage_view_calls;

static VKAPI_ATTR VkResult VKAPI_CALL inspect_create_image(VkDevice device,
    const VkImageCreateInfo *info, const VkAllocationCallbacks *allocation, VkImage *image)
{
    ++image_calls;
    assert(info->imageType == VK_IMAGE_TYPE_2D);
    assert(info->format == VK_FORMAT_R32_SFLOAT);
    assert(info->extent.width == 1 && info->extent.height == 1 && info->extent.depth == 1);
    assert(info->mipLevels == 1 && info->arrayLayers == 1);
    assert(info->samples == VK_SAMPLE_COUNT_1_BIT);
    assert(info->tiling == VK_IMAGE_TILING_OPTIMAL);
    const VkImageUsageFlags expected_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(info->usage == expected_usage);
    PFN_vkCreateImage create = (PFN_vkCreateImage)original_proc(device, "vkCreateImage");
    return create(device, info, allocation, image);
}

static VKAPI_ATTR VkResult VKAPI_CALL inspect_create_view(VkDevice device,
    const VkImageViewCreateInfo *info, const VkAllocationCallbacks *allocation, VkImageView *view)
{
    ++view_calls;
    assert(info->viewType == VK_IMAGE_VIEW_TYPE_2D);
    assert(info->format == VK_FORMAT_R32_SFLOAT);
    assert(info->subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
    assert(info->subresourceRange.baseMipLevel == 0 && info->subresourceRange.levelCount == 1);
    const VkImageViewUsageCreateInfo *usage = info->pNext;
    assert(usage && usage->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO);
    if (usage->usage & VK_IMAGE_USAGE_STORAGE_BIT)
        ++storage_view_calls;
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
        .target = 1, .width = 1, .height = 1, .depth = 1, .levels = 1,
        .format = 0xa, .flags = 0x4, .swizzle = {2, 3, 4, 5},
    };
    uint64_t size = 0, alignment = 0;
    assert(nvn_guest_texture_layout(&config, &size, &alignment));
    assert(nvn_guest_texture_vk_format(config.format) == VK_FORMAT_R32_SFLOAT);
    printf("Guest R32F storage size=%" PRIu64 ", alignment=%" PRIu64 "\n", size, alignment);
    PokopiaVkTexture texture = {0};
    VkResult result = pokopia_vk_texture_open(&adapter, &config, &texture);
    printf("1x1 R32F with 1 mip: VkResult=%d, Vulkan bytes=%" PRIu64 "\n",
        result, texture.backing.bytes);
    assert(result == VK_SUCCESS && texture.backing.image && texture.backing.memory);
    assert(texture.view && texture.storage_view);
    assert(image_calls == 1 && view_calls == 2 && storage_view_calls == 1);
    pokopia_vk_texture_close(&adapter, &texture);
    assert(!texture.backing.image && !texture.view && !texture.storage_view);

    config.levels = 2;
    result = pokopia_vk_texture_open(&adapter, &config, &texture);
    printf("1x1 R32F with 2 mips: VkResult=%d, image creation calls=%u\n", result, image_calls);
    assert(result == VK_ERROR_FORMAT_NOT_SUPPORTED && !texture.backing.image && image_calls == 1);

    pokopia_vk_close(&adapter);
    puts("PASS: exact R32F image and both views allocated, and two mips rejected before Vulkan creation.");
    return 0;
}

