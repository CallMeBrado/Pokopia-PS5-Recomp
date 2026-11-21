#include "live_texture_upload.h"
#include "nvn_guest_layout.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { LIVE_TEXTURE = 0x1000, LIVE_REGION = 0x2000, LIVE_SOURCE = 0x3000, LIVE_STORAGE = 0x4000, LIVE_OUTPUT = 0x5000 };
static PFN_vkGetDeviceProcAddr original_proc;
static unsigned storage_views;
static uint32_t expected_width = 1;
static bool storage_expected = true;

typedef struct {
    NvnFrontTextureInfo texture;
    uint32_t region[6];
    uint8_t source[8], output[8], storage[512];
} LiveGuest;

bool nvn_front_get_texture(NvnFront *front, uint64_t address, NvnFrontTextureInfo *info)
{
    if (address != LIVE_TEXTURE)
        return false;
    *info = ((LiveGuest *)front)->texture;
    return true;
}

static void *write_guest(void *opaque, uint64_t address, size_t bytes)
{
    LiveGuest *guest = opaque;
    switch (address) {
    case LIVE_REGION: return bytes == sizeof guest->region ? guest->region : NULL;
    case LIVE_SOURCE: return bytes == sizeof guest->source ? guest->source : NULL;
    case LIVE_STORAGE: return bytes == sizeof guest->storage ? guest->storage : NULL;
    case LIVE_OUTPUT: return bytes == sizeof guest->output ? guest->output : NULL;
    default: return NULL;
    }
}

static const void *read_guest(void *opaque, uint64_t address, size_t bytes)
{
    return write_guest(opaque, address, bytes);
}

static VKAPI_ATTR VkResult VKAPI_CALL check_image(VkDevice device, const VkImageCreateInfo *info,
                                                   const VkAllocationCallbacks *allocation, VkImage *image)
{
    assert(info->format == VK_FORMAT_R32G32_SFLOAT && info->imageType == VK_IMAGE_TYPE_2D);
    assert(info->extent.width == expected_width && info->extent.height == 1 && info->extent.depth == 1);
    assert(info->mipLevels == 1 && info->arrayLayers == 1 && info->samples == VK_SAMPLE_COUNT_1_BIT);
    const VkImageUsageFlags required = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(info->usage == (required | (storage_expected ? VK_IMAGE_USAGE_STORAGE_BIT : 0)));
    PFN_vkCreateImage create = (PFN_vkCreateImage)original_proc(device, "vkCreateImage");
    return create(device, info, allocation, image);
}

static VKAPI_ATTR VkResult VKAPI_CALL check_view(VkDevice device, const VkImageViewCreateInfo *info,
                                                  const VkAllocationCallbacks *allocation, VkImageView *view)
{
    assert(info->format == VK_FORMAT_R32G32_SFLOAT && info->viewType == VK_IMAGE_VIEW_TYPE_2D);
    const VkImageSubresourceRange *range = &info->subresourceRange;
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

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL check_proc(VkDevice device, const char *name)
{
    if (!strcmp(name, "vkCreateImage"))
        return (PFN_vkVoidFunction)check_image;
    if (!strcmp(name, "vkCreateImageView"))
        return (PFN_vkVoidFunction)check_view;
    return original_proc(device, name);
}

int main(void)
{
    PokopiaVkAdapter adapter = {0};
    assert(pokopia_vk_open(&adapter, stdout));
    original_proc = adapter.get_device_proc;
    adapter.get_device_proc = check_proc;
    PokopiaVkTexture texture = {0};
    LiveGuest guest = {
        .texture = {.native = &texture, .create_info = {
            .config = {.target = 1, .width = 1, .height = 1, .depth = 1, .levels = 1,
                       .format = 0x16, .flags = 0xc, .swizzle = {2, 3, 4, 5}},
            .storage_address = LIVE_STORAGE,
        }},
        .region = {0, 0, 0, 1, 1, 1},
        .source = {0x00, 0x00, 0xa0, 0x3f, 0x00, 0x00, 0x20, 0xc0},
    };
    NvnFrontTextureCreateInfo *info = &guest.texture.create_info;
    assert(nvn_guest_texture_vk_format(info->config.format) == VK_FORMAT_R32G32_SFLOAT);
    assert(nvn_guest_texture_layout(&info->config, &info->storage_size, &info->storage_alignment));
    assert(info->storage_size == sizeof guest.storage && info->storage_alignment == 512);
    assert(pokopia_vk_texture_open(&adapter, &info->config, &texture) == VK_SUCCESS);
    assert(texture.view && texture.storage_view && storage_views == 1);

    PokopiaLiveTextureUpload live = {
        .front = (NvnFront *)&guest, .adapter = &adapter, .guest_opaque = &guest,
        .read_guest = read_guest, .write_guest = write_guest, .log = stdout,
    };
    uint64_t args[32] = {LIVE_TEXTURE, 0, LIVE_REGION, LIVE_SOURCE};
    memset(guest.storage, 0x5a, sizeof guest.storage);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureWriteTexels", args) == NVN_FRONT_HANDLED);
    assert(texture.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    assert(!memcmp(guest.source, guest.storage, sizeof guest.source));

    memset(guest.storage, 0x5a, sizeof guest.source);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureInvalidateTexels", args) == NVN_FRONT_HANDLED);
    assert(!memcmp(guest.source, guest.storage, sizeof guest.source));

    const uint8_t second[] = {0x00, 0x00, 0x80, 0xc0, 0x00, 0x00, 0x70, 0x40};
    memcpy(guest.storage, second, sizeof second);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureFlushTexels", args) == NVN_FRONT_HANDLED);
    memset(guest.storage, 0x5a, sizeof second);
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureInvalidateTexels", args) == NVN_FRONT_HANDLED);
    assert(!memcmp(second, guest.storage, sizeof second));
    for (size_t i = sizeof second; i < sizeof guest.storage; ++i)
        assert(guest.storage[i] == 0x5a);
    args[3] = LIVE_OUTPUT;
    assert(pokopia_live_texture_upload_dispatch(&live, "nvnTextureReadTexels", args) == NVN_FRONT_HANDLED);
    assert(!memcmp(second, guest.output, sizeof second));
    assert(pokopia_live_texture_upload_close(&live) == VK_SUCCESS);
    pokopia_vk_texture_close(&adapter, &texture);

    NvnTextureFootprint footprint;
    assert(nvn_texture_upload_footprint(VK_FORMAT_R32G32_SFLOAT, (VkExtent2D){3, 2}, 32, &footprint) == VK_SUCCESS);
    assert(footprint.bytes_per_block == 8 && footprint.row_bytes == 24 && footprint.packed_bytes == 48 &&
           footprint.required_source_bytes == 56);
    expected_width = info->config.width = 5;
    storage_expected = false;
    info->config.flags = 0x10;
    info->config.stride = 32;
    assert(pokopia_vk_texture_open(&adapter, &info->config, &texture) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    info->config.stride = 64;
    assert(pokopia_vk_texture_open(&adapter, &info->config, &texture) == VK_SUCCESS && !texture.storage_view);
    pokopia_vk_texture_close(&adapter, &texture);

    pokopia_vk_close(&adapter);
    puts("PASS exact 1x1 RG32F storage image and NVN write/flush/invalidate/read bytes, padded footprint, and 8-byte stride");
}
