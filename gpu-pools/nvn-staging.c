#include "nvn-staging.h"

#include <string.h>

VkResult nvn_staging_open(NvnStaging *staging, VkPhysicalDevice physical, VkDevice device,
                          VkQueue queue, uint32_t queue_family, size_t max_pool_size)
{
    if (staging->device || !max_pool_size)
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family,
    };
    NvnStaging opened = {
        .device = device,
        .queue = queue,
        .next_generation = 1,
        .max_pool_size = max_pool_size,
    };
    vkGetPhysicalDeviceMemoryProperties(physical, &opened.memory_properties);
    VkResult result = vkCreateCommandPool(device, &pool_info, NULL, &opened.command_pool);
    if (result != VK_SUCCESS)
        return result;

    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = opened.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    result = vkAllocateCommandBuffers(device, &command_info, &opened.command);
    if (result == VK_SUCCESS) {
        const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(device, &fence_info, NULL, &opened.fence);
    }
    if (result != VK_SUCCESS) {
        vkDestroyCommandPool(device, opened.command_pool, NULL);
        return result;
    }
    *staging = opened;
    return VK_SUCCESS;
}

VkResult nvn_staging_close(NvnStaging *staging)
{
    if (staging->open_pools)
        return VK_NOT_READY;
    if (staging->device) {
        vkDestroyFence(staging->device, staging->fence, NULL);
        vkDestroyCommandPool(staging->device, staging->command_pool, NULL);
        memset(staging, 0, sizeof(*staging));
    }
    return VK_SUCCESS;
}

static void destroy_pool(VkDevice device, NvnStagedPool *pool)
{
    if (pool->mapped)
        vkUnmapMemory(device, pool->memory);
    if (pool->buffer)
        vkDestroyBuffer(device, pool->buffer, NULL);
    if (pool->memory)
        vkFreeMemory(device, pool->memory, NULL);
    memset(pool, 0, sizeof(*pool));
}

static VkResult allocate_pool(NvnStaging *staging, NvnStagedPool *pool, size_t size)
{
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vkCreateBuffer(staging->device, &info, NULL, &pool->buffer);
    if (result != VK_SUCCESS)
        return result;

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(staging->device, pool->buffer, &requirements);
    const VkMemoryPropertyFlags required =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkPhysicalDeviceMemoryProperties *properties = &staging->memory_properties;
    uint32_t type = 0;
    while (type < properties->memoryTypeCount) {
        if ((requirements.memoryTypeBits & (1u << type)) &&
            (properties->memoryTypes[type].propertyFlags & required) == required)
            break;
        ++type;
    }
    if (type == properties->memoryTypeCount)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    result = vkAllocateMemory(staging->device, &allocation, NULL, &pool->memory);
    if (result != VK_SUCCESS)
        return result;
    result = vkBindBufferMemory(staging->device, pool->buffer, pool->memory, 0);
    if (result != VK_SUCCESS)
        return result;
    void *mapping = NULL;
    result = vkMapMemory(staging->device, pool->memory, 0, VK_WHOLE_SIZE, 0, &mapping);
    if (result == VK_SUCCESS)
        pool->mapped = mapping;
    return result;
}

VkResult nvn_staging_pool_open(NvnStaging *staging, NvnStagedPool *pool, uint64_t guest_address,
                               void *guest_data, size_t size)
{
    if (!staging->device || pool->generation || !guest_data || !size ||
        size > staging->max_pool_size || guest_address > UINT64_MAX - size || !staging->next_generation)
        return VK_ERROR_INITIALIZATION_FAILED;

    NvnStagedPool opened = {
        .owner = staging, .guest_data = guest_data, .guest_address = guest_address, .size = size,
    };
    const VkResult result = allocate_pool(staging, &opened, size);
    if (result != VK_SUCCESS) {
        destroy_pool(staging->device, &opened);
        return result;
    }
    opened.generation = staging->next_generation++;
    ++staging->open_pools;
    *pool = opened;
    return VK_SUCCESS;
}

VkResult nvn_staging_pool_close(NvnStaging *staging, NvnStagedPool *pool)
{
    if (!pool->owner)
        return VK_SUCCESS;
    if (pool->owner != staging)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (staging->pending.destination == pool ||
        (staging->pending.source == pool && !staging->ready))
        return VK_NOT_READY;
    destroy_pool(staging->device, pool);
    --staging->open_pools;
    return VK_SUCCESS;
}

static bool in_range(const NvnStagedPool *pool, size_t offset, size_t size)
{
    return size && offset < pool->size && size <= pool->size - offset;
}

static bool resolve(const NvnStagedPool *pool, uint64_t address, size_t size, size_t *offset)
{
    if (!pool->generation || address < pool->guest_address || !size)
        return false;
    const uint64_t relative = address - pool->guest_address;
    if (relative > SIZE_MAX || !in_range(pool, (size_t)relative, size))
        return false;
    *offset = (size_t)relative;
    return true;
}

VkResult nvn_staging_record_copy(NvnStagedCopy *copy, NvnStagedPool *source, uint64_t source_address,
                                 NvnStagedPool *destination, uint64_t destination_address, size_t size)
{
    NvnStagedCopy recording = {
        .source = source,
        .destination = destination,
        .source_generation = source->generation,
        .destination_generation = destination->generation,
        .size = size,
    };
    /* This backend handles copies between distinct registered pools. */
    if (source == destination || source->owner != destination->owner ||
        !resolve(source, source_address, size, &recording.source_offset) ||
        !resolve(destination, destination_address, size, &recording.destination_offset))
        return VK_ERROR_INITIALIZATION_FAILED;
    *copy = recording;
    return VK_SUCCESS;
}

VkResult nvn_staging_submit(NvnStaging *staging, const NvnStagedCopy *copy)
{
    if (staging->pending.destination)
        return VK_NOT_READY;
    if (!staging->device || !copy->source || !copy->destination || copy->source == copy->destination ||
        copy->source->owner != staging || copy->destination->owner != staging ||
        copy->source->generation != copy->source_generation ||
        copy->destination->generation != copy->destination_generation ||
        !in_range(copy->source, copy->source_offset, copy->size) ||
        !in_range(copy->destination, copy->destination_offset, copy->size))
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result = vkResetCommandBuffer(staging->command, 0);
    if (result != VK_SUCCESS)
        return result;
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = vkBeginCommandBuffer(staging->command, &begin);
    if (result != VK_SUCCESS)
        return result;

    memcpy(copy->source->mapped + copy->source_offset,
           copy->source->guest_data + copy->source_offset, copy->size);
    const VkBufferCopy region = {copy->source_offset, copy->destination_offset, copy->size};
    vkCmdCopyBuffer(staging->command, copy->source->buffer, copy->destination->buffer, 1, &region);
    const VkBufferMemoryBarrier host_read = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy->destination->buffer,
        .offset = copy->destination_offset,
        .size = copy->size,
    };
    vkCmdPipelineBarrier(staging->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_read, 0, NULL);
    result = vkEndCommandBuffer(staging->command);
    if (result != VK_SUCCESS)
        return result;
    result = vkResetFences(staging->device, 1, &staging->fence);
    if (result != VK_SUCCESS)
        return result;
    /* Queue submission makes prior coherent host writes available to the transfer. */
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &staging->command,
    };
    result = vkQueueSubmit(staging->queue, 1, &submit, staging->fence);
    if (result == VK_SUCCESS) {
        staging->pending = *copy;
        staging->ready = false;
    }
    return result;
}

VkResult nvn_staging_wait(NvnStaging *staging, uint64_t timeout_ns)
{
    if (!staging->pending.destination)
        return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = vkWaitForFences(staging->device, 1, &staging->fence, VK_TRUE, timeout_ns);
    if (result == VK_SUCCESS)
        staging->ready = true;
    return result;
}

VkResult nvn_staging_invalidate(NvnStaging *staging, NvnStagedPool *destination,
                                uint64_t guest_address, size_t size)
{
    const NvnStagedCopy *copy = &staging->pending;
    size_t offset;
    if (copy->destination != destination || copy->destination_generation != destination->generation ||
        !resolve(destination, guest_address, size, &offset) ||
        copy->destination_offset != offset || copy->size != size)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!staging->ready)
        return VK_NOT_READY;
    memcpy(destination->guest_data + offset, destination->mapped + offset, size);
    memset(&staging->pending, 0, sizeof(staging->pending));
    staging->ready = false;
    return VK_SUCCESS;
}
