/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vk_texel.h"
#include "vk_ledger.h"

#include <string.h>

void pokopia_vk_texel_close(PokopiaVkTexel *texel)
{
    if (!texel || !texel->device)
        return;

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

VkResult pokopia_vk_texel_open(PokopiaVkTexel *texel, VkDevice device,
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

#define LOAD(name) PFN_vk##name name = (PFN_vk##name)get_proc(device, "vk" #name)
    LOAD(CreateBuffer);
    LOAD(GetBufferMemoryRequirements);
    LOAD(AllocateMemory);
    LOAD(BindBufferMemory);
    LOAD(MapMemory);
    LOAD(CreateBufferView);
#undef LOAD
    if (!CreateBuffer || !GetBufferMemoryRequirements || !AllocateMemory || !BindBufferMemory ||
        !MapMemory || !CreateBufferView ||
        !get_proc(device, "vkDestroyBufferView") || !get_proc(device, "vkDestroyBuffer") ||
        !get_proc(device, "vkFreeMemory") || !get_proc(device, "vkUnmapMemory") ||
        !get_proc(device, "vkFlushMappedMemoryRanges"))
        return VK_ERROR_INITIALIZATION_FAILED;

    PokopiaVkTexel opened = {.device = device, .get_proc = get_proc, .format = format, .bytes = (size_t)bytes};
    const VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer created = VK_NULL_HANDLE;
    VkResult result = CreateBuffer(device, &buffer, NULL, &created);
    if (result != VK_SUCCESS)
        return result;
    opened.buffer = created;

    VkMemoryRequirements requirements;
    GetBufferMemoryRequirements(device, opened.buffer, &requirements);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = memory->memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (UINT32_C(1) << i)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        type = i;
        if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
            opened.coherent = true;
            break;
        }
    }
    if (type == UINT32_MAX) {
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto fail;
    }

    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory allocated = VK_NULL_HANDLE;
    result = pokopia_vk_ledger_allocate(AllocateMemory, device, &allocation, &allocated, VK_LEDGER_TEXEL);
    if (result != VK_SUCCESS)
        goto fail;
    opened.memory = allocated;
    result = BindBufferMemory(device, opened.buffer, opened.memory, 0);
    if (result != VK_SUCCESS)
        goto fail;
    void *mapped = NULL;
    result = MapMemory(device, opened.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS)
        goto fail;
    opened.mapped = mapped;

    const VkBufferViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = opened.buffer,
        .format = format, .offset = 0, .range = bytes,
    };
    VkBufferView created_view = VK_NULL_HANDLE;
    result = CreateBufferView(device, &view, NULL, &created_view);
    if (result != VK_SUCCESS)
        goto fail;
    opened.view = created_view;
    *texel = opened;
    return VK_SUCCESS;

fail:
    pokopia_vk_texel_close(&opened);
    return result;
}

VkResult pokopia_vk_texel_refresh(PokopiaVkTexel *texel, const void *guest_bytes, size_t byte_count)
{
    if (!texel || !texel->view || !texel->mapped || !guest_bytes || byte_count != texel->bytes)
        return VK_ERROR_INITIALIZATION_FAILED;
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
