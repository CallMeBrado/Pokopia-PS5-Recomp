#ifndef NVN_STAGING_H
#define NVN_STAGING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct NvnStaging NvnStaging;

typedef struct {
    NvnStaging *owner;
    uint8_t *guest_data;
    uint64_t guest_address;
    size_t size;
    uint64_t generation;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *mapped;
} NvnStagedPool;

typedef struct {
    NvnStagedPool *source;
    NvnStagedPool *destination;
    uint64_t source_generation;
    uint64_t destination_generation;
    size_t source_offset;
    size_t destination_offset;
    size_t size;
} NvnStagedCopy;

struct NvnStaging {
    VkDevice device;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkFence fence;
    uint64_t next_generation;
    size_t max_pool_size;
    size_t open_pools;
    NvnStagedCopy pending;
    bool ready;
};

/* Context and pool objects are zero-initialized by their owners. Operations are serialized by
   the replay thread. The context handles one copy at a time; VkDevice and VkQueue are borrowed. */
VkResult nvn_staging_open(NvnStaging *staging, VkPhysicalDevice physical, VkDevice device,
                          VkQueue queue, uint32_t queue_family, size_t max_pool_size);
VkResult nvn_staging_close(NvnStaging *staging);
VkResult nvn_staging_pool_open(NvnStaging *staging, NvnStagedPool *pool, uint64_t guest_address,
                               void *guest_data, size_t size);
VkResult nvn_staging_pool_close(NvnStaging *staging, NvnStagedPool *pool);

/* Recording stores validated ranges and pool generations. Submit uploads the source's current
   bytes, not a snapshot made while recording. The guest destination is visible after invalidate. */
VkResult nvn_staging_record_copy(NvnStagedCopy *copy, NvnStagedPool *source, uint64_t source_address,
                                 NvnStagedPool *destination, uint64_t destination_address, size_t size);
VkResult nvn_staging_submit(NvnStaging *staging, const NvnStagedCopy *copy);
VkResult nvn_staging_wait(NvnStaging *staging, uint64_t timeout_ns);
VkResult nvn_staging_invalidate(NvnStaging *staging, NvnStagedPool *destination,
                                uint64_t guest_address, size_t size);

#endif
