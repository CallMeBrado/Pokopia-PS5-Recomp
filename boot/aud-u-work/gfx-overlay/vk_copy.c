/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vk_adapter.h"

#include <string.h>

VkResult pokopia_vk_copy_guest(PokopiaVkAdapter *adapter, const void *source, void *destination, size_t bytes)
{
    if (!adapter->device || !adapter->queue || !source || !destination || !bytes || bytes > (SIZE_MAX - 6) / 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkDevice device = adapter->device;
    const PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
#define LOAD(name) PFN_vk##name name = (PFN_vk##name)proc(device, "vk" #name)
    LOAD(CreateBuffer); LOAD(DestroyBuffer); LOAD(GetBufferMemoryRequirements);
    LOAD(AllocateMemory); LOAD(FreeMemory); LOAD(BindBufferMemory); LOAD(MapMemory); LOAD(UnmapMemory);
    LOAD(FlushMappedMemoryRanges); LOAD(InvalidateMappedMemoryRanges);
    LOAD(BeginCommandBuffer); LOAD(EndCommandBuffer); LOAD(CmdCopyBuffer); LOAD(CmdPipelineBarrier);
#undef LOAD
    if (!CreateBuffer || !DestroyBuffer || !GetBufferMemoryRequirements || !AllocateMemory || !FreeMemory ||
        !BindBufferMemory || !MapMemory || !UnmapMemory || !FlushMappedMemoryRanges ||
        !InvalidateMappedMemoryRanges || !BeginCommandBuffer || !EndCommandBuffer ||
        !CmdCopyBuffer || !CmdPipelineBarrier)
        return VK_ERROR_INITIALIZATION_FAILED;

    const size_t stride = (bytes + 3) & ~(size_t)3;
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = stride * 2,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer;
    VkResult result = CreateBuffer(device, &buffer_info, NULL, &buffer);
    if (result != VK_SUCCESS)
        return result;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    void *mapped = NULL;
    VkMemoryRequirements requirements;
    GetBufferMemoryRequirements(device, buffer, &requirements);
    uint32_t type = UINT32_MAX;
    bool coherent = false;
    for (uint32_t i = 0; i < adapter->memory.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags flags = adapter->memory.memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (UINT32_C(1) << i)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        type = i;
        coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (coherent)
            break;
    }
    if (type == UINT32_MAX) {
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto done;
    }
    const VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = buffer,
    };
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &dedicated,
        .allocationSize = requirements.size, .memoryTypeIndex = type,
    };
    VkDeviceMemory allocated;
    result = AllocateMemory(device, &allocation, NULL, &allocated);
    if (result != VK_SUCCESS)
        goto done;
    memory = allocated;
    result = BindBufferMemory(device, buffer, memory, 0);
    if (result != VK_SUCCESS)
        goto done;
    void *mapping;
    result = MapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapping);
    if (result != VK_SUCCESS)
        goto done;
    mapped = mapping;
    memcpy(mapped, source, bytes);
    memset((uint8_t *)mapped + bytes, 0, stride - bytes);
    const VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = memory, .size = VK_WHOLE_SIZE,
    };
    if (!coherent) {
        result = FlushMappedMemoryRanges(device, 1, &range);
        if (result != VK_SUCCESS)
            goto done;
    }

    result = pokopia_vk_command_buffer_open(adapter, &command);
    if (result != VK_SUCCESS)
        goto done;
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = BeginCommandBuffer(command, &begin);
    if (result != VK_SUCCESS)
        goto done;
    const VkBufferCopy copy = { .dstOffset = stride, .size = stride };
    CmdCopyBuffer(command, buffer, buffer, 1, &copy);
    const VkBufferMemoryBarrier ready = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = buffer, .offset = stride, .size = stride,
    };
    CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &ready, 0, NULL);
    result = EndCommandBuffer(command);
    if (result != VK_SUCCESS)
        goto done;
    bool retired;
    result = pokopia_vk_submit_wait(adapter, command, &retired);
    if (!retired)
        return result;
    if (result != VK_SUCCESS)
        goto done;
    if (!coherent) {
        result = InvalidateMappedMemoryRanges(device, 1, &range);
        if (result != VK_SUCCESS)
            goto done;
    }
    memcpy(destination, (const uint8_t *)mapped + stride, bytes);

done:
    if (command)
        pokopia_vk_command_buffer_close(adapter, command);
    if (mapped)
        UnmapMemory(device, memory);
    DestroyBuffer(device, buffer, NULL);
    if (memory)
        FreeMemory(device, memory, NULL);
    return result;
}
