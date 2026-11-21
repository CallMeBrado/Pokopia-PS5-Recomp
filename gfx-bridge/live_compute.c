/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_compute.h"
#include "vk_ledger.h"

#include <string.h>

#define COMPUTE_FUNCTIONS(X) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
    X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
    X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CreateComputePipelines) X(DestroyPipeline) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPipelineBarrier) X(CmdDispatch)

typedef struct {
#define DECLARE(name) PFN_vk##name name;
    COMPUTE_FUNCTIONS(DECLARE)
#undef DECLARE
} ComputeFunctions;

enum { SUPPORT_BYTES = 0x570 };
enum { SUPPORT, UNIFORM, STORAGE0, STORAGE1, RANGES };

typedef struct {
    VkBuffer arena;
    VkDeviceMemory memory;
    uint8_t *mapped;
    bool coherent;
    VkDeviceSize offsets[RANGES], bytes;
    VkDescriptorSetLayout layouts[3];
    VkDescriptorPool descriptors;
    VkDescriptorSet sets[3];
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkCommandBuffer command;
} ComputeWork;

static bool supported(const PokopiaVkAdapter *adapter, const PokopiaLiveCompute *d, bool *clustered)
{
    if (!d->module || !d->entry || !*d->entry || !d->hash || !d->uniform || !d->storage[0].data ||
        d->uniform_bytes > adapter->properties.limits.maxUniformBufferRange ||
        d->storage[0].bytes != 4)
        return false;
    *clustered = strcmp(d->hash, "b81c48ff486acef4") == 0;
    if (!*clustered && strcmp(d->hash, "082274fc8b17c887"))
        return false;
    if (d->uniform_bytes < (*clustered ? 512u : 256u))
        return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!d->groups[i] || d->groups[i] > adapter->properties.limits.maxComputeWorkGroupCount[i])
            return false;
    if (!*clustered)
        return d->groups[0] == 1 && d->groups[1] == 1 && d->groups[2] == 1 &&
               !d->storage[1].data && !d->storage[1].bytes && !d->texels[0] && !d->texels[1];

    const uintptr_t counter = (uintptr_t)d->storage[0].data, output = (uintptr_t)d->storage[1].data;
    const size_t output_bytes = pokopia_cluster_output_bytes(d->groups);
    if (!output || !output_bytes || d->storage[1].bytes != output_bytes ||
        !pokopia_cluster_extent_matches(d->groups, d->uniform) ||
        d->storage[1].bytes > adapter->properties.limits.maxStorageBufferRange ||
        counter > UINTPTR_MAX - 4 || output > UINTPTR_MAX - d->storage[1].bytes ||
        (counter < output + d->storage[1].bytes && output < counter + 4))
        return false;
    const size_t expected[] = {1024 * 16, 64 * 16};
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; ++i) {
        const PokopiaVkTexel *view = d->texels[i];
        if (!view || view->device != adapter->device || !view->view || !view->buffer ||
            view->format != VK_FORMAT_R32G32B32A32_SFLOAT || view->bytes != expected[i])
            return false;
    }
    return true;
}

static uint32_t memory_type(const PokopiaVkAdapter *adapter, uint32_t allowed, bool *coherent)
{
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < adapter->memory.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = adapter->memory.memoryTypes[i].propertyFlags;
        if (!(allowed & (UINT32_C(1) << i)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
            *coherent = true;
            return i;
        }
        fallback = i;
    }
    *coherent = false;
    return fallback;
}

static VkResult stage(PokopiaVkAdapter *adapter, const ComputeFunctions *fn,
                      const PokopiaLiveCompute *d, ComputeWork *work)
{
    const size_t sizes[RANGES] = {SUPPORT_BYTES, d->uniform_bytes, d->storage[0].bytes, d->storage[1].bytes};
    const VkPhysicalDeviceLimits *limits = &adapter->properties.limits;
    VkDeviceSize bytes = 0;
    for (unsigned i = 0; i < RANGES; ++i) {
        const VkDeviceSize align = i < STORAGE0 ? limits->minUniformBufferOffsetAlignment :
                                                  limits->minStorageBufferOffsetAlignment;
        if (!align || align > SIZE_MAX)
            return VK_ERROR_INITIALIZATION_FAILED;
        const VkDeviceSize remainder = bytes % align;
        const VkDeviceSize padding = remainder ? align - remainder : 0;
        if (padding > SIZE_MAX - bytes || sizes[i] > SIZE_MAX - bytes - padding)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        work->offsets[i] = bytes + padding;
        bytes += padding + sizes[i];
    }
    work->bytes = bytes;
    const VkDevice device = adapter->device;
    const VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = fn->CreateBuffer(device, &buffer, NULL, &work->arena);
    if (result != VK_SUCCESS)
        return result;
    VkMemoryRequirements requirements;
    fn->GetBufferMemoryRequirements(device, work->arena, &requirements);
    uint32_t type = memory_type(adapter, requirements.memoryTypeBits, &work->coherent);
    if (type == UINT32_MAX)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type,
    };
    result = pokopia_vk_ledger_allocate(fn->AllocateMemory, device, &allocation, &work->memory, VK_LEDGER_COMPUTE);
    if (result != VK_SUCCESS)
        return result;
    result = fn->BindBufferMemory(device, work->arena, work->memory, 0);
    if (result != VK_SUCCESS)
        return result;
    void *mapping;
    result = fn->MapMemory(device, work->memory, 0, VK_WHOLE_SIZE, 0, &mapping);
    if (result != VK_SUCCESS)
        return result;
    work->mapped = mapping;
    uint8_t *support = work->mapped + work->offsets[SUPPORT];
    memset(support, 0, SUPPORT_BYTES);
    const float unit_scale = 1.0f;
    for (unsigned i = 0; i < 73; ++i)
        memcpy(support + 0xc0 + i * 16, &unit_scale, sizeof unit_scale);
    memcpy(work->mapped + work->offsets[UNIFORM], d->uniform, d->uniform_bytes);
    for (unsigned i = 0; i < 2; ++i)
        if (d->storage[i].bytes)
            memcpy(work->mapped + work->offsets[STORAGE0 + i], d->storage[i].data, d->storage[i].bytes);
    const VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = work->memory, .size = VK_WHOLE_SIZE,
    };
    return work->coherent ? VK_SUCCESS : fn->FlushMappedMemoryRanges(device, 1, &range);
}

static VkResult descriptors(VkDevice device, const ComputeFunctions *fn,
                            const PokopiaLiveCompute *d, bool clustered, ComputeWork *work)
{
    const VkDescriptorSetLayoutBinding bindings[3][2] = {
        {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
         {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}},
        {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}},
        {{64, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
         {65, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}},
    };
    const uint32_t set_count = clustered ? 3 : 2;
    const uint32_t counts[] = {2, clustered ? 2u : 1u, 2};
    for (unsigned set = 0; set < set_count; ++set) {
        const VkDescriptorSetLayoutCreateInfo layout = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = counts[set], .pBindings = bindings[set],
        };
        VkResult result = fn->CreateDescriptorSetLayout(device, &layout, NULL, &work->layouts[set]);
        if (result != VK_SUCCESS)
            return result;
    }
    const VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = set_count, .pSetLayouts = work->layouts,
    };
    VkResult result = fn->CreatePipelineLayout(device, &layout, NULL, &work->pipeline_layout);
    if (result != VK_SUCCESS)
        return result;

    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, counts[0]},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, counts[1]},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, counts[2]},
    };
    const VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = set_count,
        .poolSizeCount = set_count, .pPoolSizes = sizes,
    };
    result = fn->CreateDescriptorPool(device, &pool, NULL, &work->descriptors);
    if (result != VK_SUCCESS)
        return result;
    const VkDescriptorSetAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = work->descriptors,
        .descriptorSetCount = set_count, .pSetLayouts = work->layouts,
    };
    result = fn->AllocateDescriptorSets(device, &alloc, work->sets);
    if (result != VK_SUCCESS)
        return result;

    const VkDescriptorBufferInfo ranges[RANGES] = {
        {work->arena, work->offsets[SUPPORT], SUPPORT_BYTES},
        {work->arena, work->offsets[UNIFORM], d->uniform_bytes},
        {work->arena, work->offsets[STORAGE0], d->storage[0].bytes},
        {work->arena, work->offsets[STORAGE1], d->storage[1].bytes},
    };
    VkWriteDescriptorSet writes[6];
    unsigned count = 0;
    for (unsigned set = 0; set < set_count; ++set)
        for (unsigned i = 0; i < counts[set]; ++i)
            writes[count++] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = work->sets[set],
                .dstBinding = bindings[set][i].binding, .descriptorCount = 1,
                .descriptorType = bindings[set][i].descriptorType,
                .pBufferInfo = set < 2 ? &ranges[set * 2 + i] : NULL,
                .pTexelBufferView = set == 2 ? &d->texels[i]->view : NULL,
            };
    fn->UpdateDescriptorSets(device, count, writes, 0, NULL);
    return VK_SUCCESS;
}

static VkResult record(const ComputeFunctions *fn, const PokopiaLiveCompute *d, bool clustered, const ComputeWork *work)
{
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult result = fn->BeginCommandBuffer(work->command, &begin);
    if (result != VK_SUCCESS)
        return result;

    VkBufferMemoryBarrier access = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = work->arena, .offset = 0, .size = work->bytes,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    fn->CmdPipelineBarrier(work->command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 0, NULL, 1, &access, 0, NULL);
    fn->CmdBindPipeline(work->command, VK_PIPELINE_BIND_POINT_COMPUTE, work->pipeline);
    fn->CmdBindDescriptorSets(work->command, VK_PIPELINE_BIND_POINT_COMPUTE, work->pipeline_layout,
                             0, clustered ? 3 : 2, work->sets, 0, NULL);
    fn->CmdDispatch(work->command, d->groups[0], d->groups[1], d->groups[2]);
    access.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    access.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    fn->CmdPipelineBarrier(work->command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                           0, 0, NULL, 1, &access, 0, NULL);
    return fn->EndCommandBuffer(work->command);
}

static void release(PokopiaVkAdapter *adapter, const ComputeFunctions *fn, const ComputeWork *work)
{
    const VkDevice device = adapter->device;
    if (work->command)
        pokopia_vk_command_buffer_close(adapter, work->command);
    if (work->pipeline)
        fn->DestroyPipeline(device, work->pipeline, NULL);
    if (work->descriptors)
        fn->DestroyDescriptorPool(device, work->descriptors, NULL);
    if (work->pipeline_layout)
        fn->DestroyPipelineLayout(device, work->pipeline_layout, NULL);
    for (unsigned i = 0; i < 3; ++i)
        if (work->layouts[i])
            fn->DestroyDescriptorSetLayout(device, work->layouts[i], NULL);
    if (work->mapped)
        fn->UnmapMemory(device, work->memory);
    if (work->arena)
        fn->DestroyBuffer(device, work->arena, NULL);
    if (work->memory)
        pokopia_vk_ledger_free(fn->FreeMemory, device, work->memory);
}

VkResult pokopia_live_compute(PokopiaVkAdapter *adapter, const PokopiaLiveCompute *dispatch)
{
    if (!adapter || !adapter->device || !adapter->queue || !adapter->get_device_proc || !adapter->wait_idle)
        return VK_ERROR_INITIALIZATION_FAILED;
    bool clustered;
    if (!dispatch || !supported(adapter, dispatch, &clustered))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkDevice device = adapter->device;
    ComputeFunctions fn;
#define LOAD(name) \
    fn.name = (PFN_vk##name)adapter->get_device_proc(device, "vk" #name); \
    if (!fn.name) \
        return VK_ERROR_FEATURE_NOT_PRESENT;
    COMPUTE_FUNCTIONS(LOAD)
#undef LOAD

    ComputeWork work = {0};
    VkResult result = stage(adapter, &fn, dispatch, &work);
    if (result == VK_SUCCESS)
        result = descriptors(device, &fn, dispatch, clustered, &work);
    if (result == VK_SUCCESS) {
        const VkComputePipelineCreateInfo pipeline = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = dispatch->module, .pName = dispatch->entry,
            },
            .layout = work.pipeline_layout,
        };
        result = fn.CreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, NULL, &work.pipeline);
    }
    if (result == VK_SUCCESS)
        result = pokopia_vk_command_buffer_open(adapter, &work.command);
    if (result == VK_SUCCESS)
        result = record(&fn, dispatch, clustered, &work);
    bool retired = true;
    if (result == VK_SUCCESS)
        result = pokopia_vk_submit_wait(adapter, work.command, &retired);
    if (!retired)
        return result;

    if (result == VK_SUCCESS && !work.coherent) {
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = work.memory, .size = VK_WHOLE_SIZE,
        };
        result = fn.InvalidateMappedMemoryRanges(device, 1, &range);
    }
    if (result == VK_SUCCESS)
        for (unsigned i = 0; i < 2; ++i)
            if (dispatch->storage[i].bytes)
                memcpy(dispatch->storage[i].data, work.mapped + work.offsets[STORAGE0 + i], dispatch->storage[i].bytes);
    release(adapter, &fn, &work);
    return result;
}
