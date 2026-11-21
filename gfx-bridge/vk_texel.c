/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vk_texel.h"
#include "vk_ledger.h"
#include "vk_flush.h"

#include <string.h>

void pokopia_vk_texel_close(PokopiaVkTexel *texel)
{
    if (!texel || !texel->device)
        return;
    pokopia_vk_flush_pending(POKOPIA_FLUSH_TEXEL_CLOSE);

    const VkDevice device = texel->device;
    const PFN_vkGetDeviceProcAddr proc = texel->get_proc;
    if (texel->view) {
        PFN_vkDestroyBufferView destroy = (PFN_vkDestroyBufferView)proc(device, "vkDestroyBufferView");
        destroy(device, texel->view, NULL);
    }
    if (texel->mapped) {
        PFN_vkUnmapMemory unmap = (PFN_vkUnmapMemory)proc(device, "vkUnmapMemory");
        unmap(device, texel->memory);
    }
    if (texel->buffer) {
        PFN_vkDestroyBuffer destroy = (PFN_vkDestroyBuffer)proc(device, "vkDestroyBuffer");
        destroy(device, texel->buffer, NULL);
    }
    if (texel->memory) {
        PFN_vkFreeMemory release = (PFN_vkFreeMemory)proc(device, "vkFreeMemory");
        pokopia_vk_ledger_free(release, device, texel->memory);
    }
    *texel = (PokopiaVkTexel){0};
}

VkResult pokopia_vk_texel_configure(PokopiaVkTexel *texel, VkDevice device,
                                   PFN_vkGetDeviceProcAddr get_proc,
                                   const VkPhysicalDeviceMemoryProperties *memory,
                                   VkFormatFeatureFlags buffer_features, VkFormat format,
                                   uint32_t texel_count, uint32_t texel_bytes, uint32_t max_texels)
{
    if (!texel || texel->device || !device || !get_proc || !memory ||
        memory->memoryTypeCount > VK_MAX_MEMORY_TYPES)
        return VK_ERROR_INITIALIZATION_FAILED;
    const uint64_t bytes = (uint64_t)texel_count * texel_bytes;
    if (format == VK_FORMAT_UNDEFINED || !(buffer_features & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) ||
        !texel_count || !texel_bytes || texel_count > max_texels || bytes > SIZE_MAX)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    static const char *const functions[] = {
        "vkCreateBuffer", "vkGetBufferMemoryRequirements", "vkAllocateMemory", "vkBindBufferMemory",
        "vkMapMemory", "vkCreateBufferView", "vkDestroyBufferView", "vkDestroyBuffer",
        "vkFreeMemory", "vkUnmapMemory", "vkFlushMappedMemoryRanges",
    };
    for (size_t i = 0; i < sizeof functions / sizeof functions[0]; ++i)
        if (!get_proc(device, functions[i]))
            return VK_ERROR_INITIALIZATION_FAILED;

    PokopiaVkTexel configured = {.device = device, .get_proc = get_proc, .format = format, .bytes = (size_t)bytes};
    for (uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = memory->memoryTypes[i].propertyFlags;
        if (!(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        const uint32_t bit = UINT32_C(1) << i;
        configured.visible_types |= bit;
        if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            configured.coherent_types |= bit;
    }
    if (!configured.visible_types)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    *texel = configured;
    return VK_SUCCESS;
}

static VkResult create_backing(PokopiaVkTexel *texel)
{
    const VkDevice device = texel->device;
    const PFN_vkGetDeviceProcAddr get_proc = texel->get_proc;
#define LOAD(name) PFN_vk##name name = (PFN_vk##name)get_proc(device, "vk" #name)
    LOAD(CreateBuffer);
    LOAD(GetBufferMemoryRequirements);
    LOAD(AllocateMemory);
    LOAD(BindBufferMemory);
    LOAD(MapMemory);
    LOAD(CreateBufferView);
#undef LOAD
    const VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = texel->bytes,
        .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer created = VK_NULL_HANDLE;
    VkResult result = CreateBuffer(device, &buffer, NULL, &created);
    if (result != VK_SUCCESS)
        return result;
    texel->buffer = created;

    VkMemoryRequirements requirements;
    GetBufferMemoryRequirements(device, texel->buffer, &requirements);
    uint32_t type = UINT32_MAX;
    const uint32_t compatible = requirements.memoryTypeBits & texel->visible_types;
    for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; ++i) {
        const uint32_t bit = UINT32_C(1) << i;
        if (!(compatible & bit))
            continue;
        type = i;
        if (texel->coherent_types & bit) {
            texel->coherent = true;
            break;
        }
    }
    if (type == UINT32_MAX)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory allocated = VK_NULL_HANDLE;
    result = pokopia_vk_ledger_allocate(AllocateMemory, device, &allocation, &allocated, VK_LEDGER_TEXEL);
    if (result != VK_SUCCESS)
        return result;
    texel->memory = allocated;
    result = BindBufferMemory(device, texel->buffer, texel->memory, 0);
    if (result != VK_SUCCESS)
        return result;
    void *mapped = NULL;
    result = MapMemory(device, texel->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS)
        return result;
    texel->mapped = mapped;

    const VkBufferViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = texel->buffer,
        .format = texel->format, .offset = 0, .range = texel->bytes,
    };
    VkBufferView created_view = VK_NULL_HANDLE;
    result = CreateBufferView(device, &view, NULL, &created_view);
    if (result != VK_SUCCESS)
        return result;
    texel->view = created_view;
    return VK_SUCCESS;
}

VkResult pokopia_vk_texel_open(PokopiaVkTexel *texel, VkDevice device,
                              PFN_vkGetDeviceProcAddr get_proc,
                              const VkPhysicalDeviceMemoryProperties *memory,
                              VkFormatFeatureFlags buffer_features, VkFormat format,
                              uint32_t texel_count, uint32_t texel_bytes, uint32_t max_texels)
{
    VkResult result = pokopia_vk_texel_configure(texel, device, get_proc, memory, buffer_features, format,
                                                texel_count, texel_bytes, max_texels);
    if (result == VK_SUCCESS) {
        result = create_backing(texel);
        if (result != VK_SUCCESS)
            pokopia_vk_texel_close(texel);
    }
    return result;
}

static VkResult refresh_backing(PokopiaVkTexel *texel, const void *guest_bytes, size_t byte_count)
{
    /* R75: only when a pending draw reads this buffer */
    if (texel->batch_stamp > pokopia_vk_batch_retired)
        pokopia_vk_flush_pending(POKOPIA_FLUSH_TEXEL_REFRESH);
    memcpy(texel->mapped, guest_bytes, byte_count);
    if (texel->coherent)
        return VK_SUCCESS;

    const VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = texel->memory,
        .offset = 0, .size = VK_WHOLE_SIZE,
    };
    PFN_vkFlushMappedMemoryRanges flush =
        (PFN_vkFlushMappedMemoryRanges)texel->get_proc(texel->device, "vkFlushMappedMemoryRanges");
    return flush(texel->device, 1, &range);
}

VkResult pokopia_vk_texel_refresh(PokopiaVkTexel *texel, const void *guest_bytes, size_t byte_count)
{
    if (!texel || !texel->device || !texel->get_proc || !texel->visible_types ||
        !guest_bytes || byte_count != texel->bytes)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (texel->view) {
        if (!texel->mapped)
            return VK_ERROR_INITIALIZATION_FAILED;
        return refresh_backing(texel, guest_bytes, byte_count);
    }

    PokopiaVkTexel opened = *texel;
    VkResult result = create_backing(&opened);
    if (result == VK_SUCCESS)
        result = refresh_backing(&opened, guest_bytes, byte_count);
    if (result == VK_SUCCESS)
        *texel = opened;
    else
        pokopia_vk_texel_close(&opened);
    return result;
}
