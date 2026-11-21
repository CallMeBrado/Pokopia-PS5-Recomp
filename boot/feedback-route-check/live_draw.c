/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_draw.h"
#include "vk_ledger.h"

#include <string.h>

#define DRAW_FUNCTIONS(X) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
    X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CreateSampler) X(DestroySampler) X(CreateImageView) X(DestroyImageView) \
    X(CreateGraphicsPipelines) X(DestroyPipeline) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdBeginRendering) X(CmdEndRendering) X(CmdBindPipeline) \
    X(CmdBindDescriptorSets) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) X(CmdDrawIndexed) X(CmdDraw)

typedef struct {
#define DECLARE(name) PFN_vk##name name;
    DRAW_FUNCTIONS(DECLARE)
#undef DECLARE
} DrawFunctions;

enum { SUPPORT_UNIFORM, VERTEX_UNIFORM, FRAGMENT_UNIFORM, VERTICES, INDICES, SPAN_COUNT };
enum { SUPPORT_BUFFER_BYTES = 0x570 };

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDescriptorSetLayout layouts[3];
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet sets[2];
    VkSampler samplers[POKOPIA_LIVE_DRAW_SAMPLE_COUNT];
    PokopiaVkTexture feedback;
    VkImageView target_view;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkCommandBuffer command;
    VkDeviceSize offsets[SPAN_COUNT];
} DrawWork;

static VkFormat sampled_format(const PokopiaLiveDrawSample *sample)
{
    return sample->view_format == VK_FORMAT_UNDEFINED ? sample->texture->format : sample->view_format;
}

static VkImageView sampled_view(const PokopiaVkTexture *texture, VkFormat view_format)
{
    if (view_format != VK_FORMAT_UNDEFINED && view_format != texture->format)
        return view_format == texture->sample_alias_format ? texture->sample_alias_view : VK_NULL_HANDLE;
    return texture->sample_view ? texture->sample_view : texture->view;
}

static bool supported_sample(const PokopiaLiveDrawSample *sample, VkFormat format, bool linear)
{
    const PokopiaVkTexture *texture = sample->texture;
    const NvnFrontSamplerInfo *sampler = &sample->sampler;
    return texture && texture->backing.image && sampled_view(texture, sample->view_format) &&
           texture->array_layers <= 1 && sampled_format(sample) == format &&
           texture->aspects == VK_IMAGE_ASPECT_COLOR_BIT &&
           texture->layout != VK_IMAGE_LAYOUT_UNDEFINED && texture->stages &&
           sampler->min_filter == (linear ? 3u : 2u) && sampler->mag_filter == (linear ? 1u : 0u) &&
           sampler->wrap[0] == 7 && sampler->wrap[1] == 7 && sampler->wrap[2] == 7 &&
           !sampler->compare_mode && !sampler->reduction && !sampler->lod_snap &&
           sampler->lod_min == 0 && sampler->lod_max == 0 && sampler->lod_bias == 0 &&
           sampler->max_anisotropy == 1;
}

static bool supported_depth(const PokopiaLiveDraw *draw)
{
    const PokopiaVkTexture *depth = draw->depth_target;
    return depth && depth->backing.image && depth->view && depth->stages &&
           depth->backing.image != draw->target->backing.image &&
           depth->aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) &&
           depth->array_layers == 1 && depth->extent.width == draw->extent.width &&
           depth->extent.height == draw->extent.height;
}

static bool supported_loading_blend(const NvnFrontBlendInfo *blend)
{
    enum { SRC_ALPHA = 5, ONE_MINUS_SRC_ALPHA = 6, ADD = 1 };
    return !blend->target && !blend->advanced_mode &&
           blend->src_rgb == SRC_ALPHA && blend->dst_rgb == ONE_MINUS_SRC_ALPHA && blend->rgb_equation == ADD &&
           blend->src_alpha == SRC_ALPHA && blend->dst_alpha == ONE_MINUS_SRC_ALPHA && blend->alpha_equation == ADD;
}

static bool supported(const PokopiaVkAdapter *adapter, const PokopiaLiveDraw *draw, size_t *vertex_bytes)
{
    const PokopiaVkTexture *target = draw->target;
    if (!target || !target->backing.image || !target->view || target->texel.buffer ||
        target->aspects != VK_IMAGE_ASPECT_COLOR_BIT || !target->stages ||
        target->render_serial > adapter->render_serial ||
        !draw->extent.width || !draw->extent.height ||
        draw->extent.width > adapter->properties.limits.maxFramebufferWidth ||
        draw->extent.height > adapter->properties.limits.maxFramebufferHeight ||
        (target->extent.width && target->extent.width != draw->extent.width) ||
        (target->extent.height && target->extent.height != draw->extent.height))
        return false;

    if (!draw->vertex_shader.module || !draw->vertex_shader.entry || !draw->vertex_shader.hash ||
        !draw->fragment_shader.module || !draw->fragment_shader.entry || !draw->fragment_shader.hash ||
        !draw->vertex_shader.entry[0] || !draw->fragment_shader.entry[0])
        return false;

    const bool indexed = draw->index_count != 0;
    const char *vertex = draw->vertex_shader.hash, *fragment = draw->fragment_shader.hash;
    const bool loading = !strcmp(fragment, "2db3327d9fbc5bb3");
    const char *expected_vertex = "70b2ae4bef0dfe46";
    if (indexed)
        expected_vertex = loading ? "a26763a4bdd7b361" : "821538a4398f85a7";
    if (strcmp(vertex, expected_vertex))
        return false;
    const PokopiaLiveDrawSample *samples = draw->fragment_samples;
    const bool depth = draw->depth_target != NULL;
    const bool tonemap = !strcmp(fragment, "ff03fa7b63a79526");
    const bool needs_fragment_uniform = loading || tonemap || !strcmp(fragment, "7587f31f1945afaf");
    const VkColorComponentFlags rgb = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    const VkColorComponentFlags rgba = rgb | VK_COLOR_COMPONENT_A_BIT;
    if ((depth && !supported_depth(draw)) ||
        (draw->target_layer && (!depth || draw->target_layer >= target->array_layers)) ||
        (samples[1].texture && !tonemap))
        return false;
    if (!indexed) {
        if (strcmp(fragment, "1c913e7b97e7e999") || !depth || draw->depth_write || draw->target_layer ||
            target->array_layers != 1 || target->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
            samples[0].texture != target || !supported_sample(&samples[0], target->format, true) ||
            draw->color_write_mask != rgb || draw->vertex_count != 6 || draw->first_vertex)
            return false;
    } else if (loading) {
        if (!depth || draw->depth_write || !draw->blend_enable || !supported_loading_blend(&draw->blend) ||
            target->format != VK_FORMAT_R8G8B8A8_SRGB || target->array_layers != 1 ||
            target->layout == VK_IMAGE_LAYOUT_UNDEFINED || draw->color_write_mask != rgba ||
            !supported_sample(&samples[0], VK_FORMAT_BC7_SRGB_BLOCK, samples[0].sampler.mag_filter != 0))
            return false;
    } else if (tonemap) {
        if (!depth || target->format != VK_FORMAT_R8G8B8A8_SRGB || target->array_layers != 1 ||
            draw->color_write_mask != rgba || !supported_sample(&samples[0], VK_FORMAT_R32G32B32A32_SFLOAT, false) ||
            !supported_sample(&samples[1], VK_FORMAT_B10G11R11_UFLOAT_PACK32, true))
            return false;
    } else if (!strcmp(fragment, "c4664b38f6a87835")) {
        if (depth || target->format != VK_FORMAT_R8G8B8A8_SRGB || target->array_layers != 1 ||
            draw->color_write_mask != rgba || !supported_sample(&samples[0], target->format, true))
            return false;
    } else if (!strcmp(fragment, "9f3f4772c2ad7edc")) {
        if (depth || target->format != VK_FORMAT_R32G32B32A32_SFLOAT || draw->color_write_mask != rgba ||
            !supported_sample(&samples[0], target->format, true))
            return false;
    } else if (!strcmp(fragment, "42c917e6eb2c6411")) {
        if (!depth || target->format != VK_FORMAT_R8G8B8A8_UNORM || draw->target_layer >= target->array_layers ||
            samples[0].texture || draw->color_write_mask != rgb)
            return false;
    } else if (strcmp(fragment, "7587f31f1945afaf") || depth || samples[0].texture ||
               target->format != VK_FORMAT_R16G16_SFLOAT || !target->render_serial ||
               (target->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                target->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) ||
               draw->depth_write || draw->color_write_mask != rgb) {
        return false;
    }
    if (indexed)
        for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i)
            if (samples[i].texture && samples[i].texture->backing.image == target->backing.image)
                return false;

    if (draw->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || draw->polygon_mode != VK_POLYGON_MODE_FILL ||
        draw->cull_mode != VK_CULL_MODE_NONE || (draw->blend_enable && !loading) ||
        draw->depth_test || (draw->stencil_test && !depth) ||
        draw->instance_count != 1 || draw->first_instance)
        return false;
    if (indexed && (draw->vertex_stride != (loading ? 44u : 24u) || draw->index_type != VK_INDEX_TYPE_UINT16 ||
                    draw->vertex_count || (!loading && draw->attributes[3].format != VK_FORMAT_UNDEFINED)))
        return false;
    const VkVertexInputAttributeDescription quad_attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 16},
    };
    const VkVertexInputAttributeDescription loading_attributes[] = {
        {0, 0, VK_FORMAT_R32_SFLOAT, 40},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 36},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
    };
    const VkVertexInputAttributeDescription *expected = loading ? loading_attributes : quad_attributes;
    for (size_t i = 0; indexed && i < (loading ? 4u : 3u); ++i) {
        const VkVertexInputAttributeDescription *actual = &draw->attributes[i];
        if (actual->location != expected[i].location || actual->binding != expected[i].binding ||
            actual->format != expected[i].format || actual->offset != expected[i].offset)
            return false;
    }

    /* The shader pair was verified with the viewport transform enabled, an
     * upper-left origin and unit depth range. The negative Vulkan height
     * carries the orientation through to rasterization. */
    const VkViewport viewport = draw->viewport;
    const VkRect2D scissor = draw->scissor;
    if (!(viewport.x >= 0 && viewport.width > 0 && viewport.x + viewport.width <= draw->extent.width &&
          viewport.y <= draw->extent.height && viewport.height < 0 && viewport.y + viewport.height >= 0 &&
          viewport.minDepth == 0 && viewport.maxDepth == 1) ||
        scissor.offset.x < 0 || scissor.offset.y < 0 || !scissor.extent.width || !scissor.extent.height ||
        (uint64_t)scissor.offset.x + scissor.extent.width > draw->extent.width ||
        (uint64_t)scissor.offset.y + scissor.extent.height > draw->extent.height)
        return false;

    const uint32_t uniform_limit = adapter->properties.limits.maxUniformBufferRange;
    const uint32_t vertex_uniform_min = loading ? 3328 : 256;
    if ((indexed ? (!draw->vertex_uniform.data || draw->vertex_uniform.bytes < vertex_uniform_min ||
                    draw->vertex_uniform.bytes > uniform_limit) : draw->vertex_uniform.bytes != 0) ||
        (needs_fragment_uniform ? (!draw->fragment_uniform.data || draw->fragment_uniform.bytes < 256 ||
                                   draw->fragment_uniform.bytes > uniform_limit) : draw->fragment_uniform.bytes != 0))
        return false;
    if (!indexed) {
        *vertex_bytes = 0;
        return true;
    }
    if (!draw->vertices.data || draw->vertices.bytes < draw->vertex_stride || !draw->indices.data ||
        draw->index_count % 3 || (uint64_t)draw->first_index + draw->index_count > draw->indices.bytes / sizeof(uint16_t))
        return false;

    const uint8_t *index_bytes = draw->indices.data;
    size_t highest_vertex = 0;
    for (uint32_t i = 0; i < draw->index_count; ++i) {
        uint16_t index;
        memcpy(&index, index_bytes + ((size_t)draw->first_index + i) * sizeof index, sizeof index);
        const int64_t vertex = (int64_t)index + draw->base_vertex;
        if (vertex < 0 || (uint64_t)vertex >= draw->vertices.bytes / draw->vertex_stride)
            return false;
        if ((size_t)vertex > highest_vertex)
            highest_vertex = (size_t)vertex;
    }
    *vertex_bytes = (highest_vertex + 1) * draw->vertex_stride;
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

static VkResult upload(const PokopiaVkAdapter *adapter, const DrawFunctions *fn, const PokopiaLiveDraw *draw,
                       size_t vertex_bytes, DrawWork *work)
{
    /* Pinned nvn_shaders.cpp support_buffer_defaults for unscaled attachments
     * and textures, without BGRA, Y negate or transform feedback. */
    uint8_t support[SUPPORT_BUFFER_BYTES] = {0};
    const float unit_scale = 1.0f;
    for (size_t i = 0; i < 73; ++i)
        memcpy(support + 0xc0 + i * 16, &unit_scale, sizeof unit_scale);

    const PokopiaLiveDrawSpan spans[SPAN_COUNT] = {
        {support, sizeof support}, draw->vertex_uniform, draw->fragment_uniform,
        {draw->vertices.data, vertex_bytes},
        {draw->indices.data, ((size_t)draw->first_index + draw->index_count) * sizeof(uint16_t)},
    };
    const VkDeviceSize uniform_align = adapter->properties.limits.minUniformBufferOffsetAlignment;
    if (!uniform_align || uniform_align > SIZE_MAX)
        return VK_ERROR_INITIALIZATION_FAILED;
    size_t bytes = 0;
    for (size_t i = 0; i < SPAN_COUNT; ++i) {
        const size_t alignment = i < VERTICES ? (size_t)uniform_align : 4;
        const size_t remainder = bytes % alignment;
        const size_t padding = remainder ? alignment - remainder : 0;
        if (padding > SIZE_MAX - bytes || spans[i].bytes > SIZE_MAX - bytes - padding)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        bytes += padding;
        work->offsets[i] = bytes;
        bytes += spans[i].bytes;
    }

    const VkDevice device = adapter->device;
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
    };
    VkResult result = fn->CreateBuffer(device, &buffer_info, NULL, &work->buffer);
    if (result != VK_SUCCESS)
        return result;
    VkMemoryRequirements req;
    fn->GetBufferMemoryRequirements(device, work->buffer, &req);
    bool coherent;
    const uint32_t index = memory_type(adapter, req.memoryTypeBits, &coherent);
    if (index == UINT32_MAX)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkMemoryAllocateInfo memory_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = index,
    };
    result = pokopia_vk_ledger_allocate(fn->AllocateMemory, device, &memory_info, &work->memory, VK_LEDGER_DRAW);
    if (result != VK_SUCCESS)
        return result;
    result = fn->BindBufferMemory(device, work->buffer, work->memory, 0);
    if (result != VK_SUCCESS)
        return result;
    void *mapped = NULL;
    result = fn->MapMemory(device, work->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS)
        return result;
    for (size_t i = 0; i < SPAN_COUNT; ++i)
        if (spans[i].bytes)
            memcpy((uint8_t *)mapped + (size_t)work->offsets[i], spans[i].data, spans[i].bytes);
    if (!coherent) {
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = work->memory, .size = VK_WHOLE_SIZE,
        };
        result = fn->FlushMappedMemoryRanges(device, 1, &range);
    }
    fn->UnmapMemory(device, work->memory);
    return result;
}

static VkResult descriptors(VkDevice device, const DrawFunctions *fn, const PokopiaLiveDraw *draw, DrawWork *work)
{
    VkDescriptorSetLayoutBinding bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    const VkDescriptorBufferInfo ranges[] = {
        {work->buffer, work->offsets[SUPPORT_UNIFORM], SUPPORT_BUFFER_BYTES},
        {work->buffer, work->offsets[VERTEX_UNIFORM], draw->vertex_uniform.bytes},
        {work->buffer, work->offsets[FRAGMENT_UNIFORM], draw->fragment_uniform.bytes},
    };
    unsigned range_slots[3] = {0};
    uint32_t uniform_count = 1;
    if (draw->vertex_uniform.bytes) {
        range_slots[uniform_count] = VERTEX_UNIFORM;
        bindings[uniform_count++] =
            (VkDescriptorSetLayoutBinding){4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
    }
    if (draw->fragment_uniform.bytes) {
        range_slots[uniform_count] = FRAGMENT_UNIFORM;
        bindings[uniform_count++] =
            (VkDescriptorSetLayoutBinding){22, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    }
    const VkDescriptorSetLayoutBinding sample_bindings[] = {
        {128, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {129, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    const uint32_t sample_count = draw->fragment_samples[1].texture ? 2u : !!draw->fragment_samples[0].texture;
    const VkDescriptorSetLayoutCreateInfo set_layouts[] = {
        {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = uniform_count, .pBindings = bindings},
        {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO},
        {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = sample_count, .pBindings = sample_bindings},
    };
    const uint32_t layout_count = sample_count ? 3 : 1;
    for (uint32_t i = 0; i < layout_count; ++i) {
        VkResult result = fn->CreateDescriptorSetLayout(device, &set_layouts[i], NULL, &work->layouts[i]);
        if (result != VK_SUCCESS)
            return result;
    }

    const VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = layout_count, .pSetLayouts = work->layouts,
    };
    VkResult result = fn->CreatePipelineLayout(device, &layout_info, NULL, &work->pipeline_layout);
    if (result != VK_SUCCESS)
        return result;
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniform_count},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sample_count},
    };
    const uint32_t set_count = sample_count ? 2 : 1;
    const VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = set_count,
        .poolSizeCount = set_count, .pPoolSizes = pool_sizes,
    };
    result = fn->CreateDescriptorPool(device, &pool_info, NULL, &work->descriptor_pool);
    if (result != VK_SUCCESS)
        return result;
    const VkDescriptorSetLayout allocate_layouts[] = {work->layouts[0], work->layouts[2]};
    const VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = work->descriptor_pool,
        .descriptorSetCount = set_count, .pSetLayouts = allocate_layouts,
    };
    result = fn->AllocateDescriptorSets(device, &set_info, work->sets);
    if (result != VK_SUCCESS)
        return result;

    VkWriteDescriptorSet writes[3 + POKOPIA_LIVE_DRAW_SAMPLE_COUNT];
    for (uint32_t i = 0; i < uniform_count; ++i) {
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = work->sets[0],
            .dstBinding = bindings[i].binding, .descriptorCount = 1, .descriptorType = bindings[i].descriptorType,
            .pBufferInfo = &ranges[range_slots[i]],
        };
    }
    uint32_t write_count = uniform_count;
    VkDescriptorImageInfo images[POKOPIA_LIVE_DRAW_SAMPLE_COUNT];
    for (unsigned i = 0; i < sample_count; ++i) {
        const PokopiaLiveDrawSample *sample = &draw->fragment_samples[i];
        const PokopiaVkTexture *texture = sample->texture == draw->target ? &work->feedback : sample->texture;
        const VkSamplerCreateInfo sampler = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = sample->sampler.mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .minFilter = sample->sampler.min_filter == 3 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        };
        result = fn->CreateSampler(device, &sampler, NULL, &work->samplers[i]);
        if (result != VK_SUCCESS)
            return result;
        images[i] = (VkDescriptorImageInfo){
            .sampler = work->samplers[i], .imageView = sampled_view(texture, sample->view_format),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        writes[write_count++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = work->sets[1],
            .dstBinding = sample_bindings[i].binding, .descriptorCount = 1,
            .descriptorType = sample_bindings[i].descriptorType, .pImageInfo = &images[i],
        };
    }
    fn->UpdateDescriptorSets(device, write_count, writes, 0, NULL);
    return VK_SUCCESS;
}

static VkResult make_pipeline(VkDevice device, const DrawFunctions *fn, const PokopiaLiveDraw *draw, DrawWork *work)
{
    const VkPipelineShaderStageCreateInfo shaders[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = draw->vertex_shader.module, .pName = draw->vertex_shader.entry},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = draw->fragment_shader.module, .pName = draw->fragment_shader.entry},
    };
    const VkVertexInputBindingDescription binding = {0, draw->vertex_stride, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = draw->index_count ? 1 : 0, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = draw->index_count ? (draw->vertex_stride == 44 ? 4u : 3u) : 0,
        .pVertexAttributeDescriptions = draw->attributes,
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = draw->topology,
    };
    const VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &draw->viewport, .scissorCount = 1, .pScissors = &draw->scissor,
    };
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = draw->polygon_mode, .cullMode = draw->cull_mode,
        .frontFace = draw->front_face, .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState attachment = {
        .blendEnable = draw->blend_enable, .colorWriteMask = draw->color_write_mask,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD, .alphaBlendOp = VK_BLEND_OP_ADD,
    };
    const VkPipelineColorBlendStateCreateInfo color = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &attachment,
    };
    const VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = draw->depth_test, .depthWriteEnable = draw->depth_write,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS, .stencilTestEnable = draw->stencil_test,
        .front = draw->stencil[0], .back = draw->stencil[1],
    };
    const VkFormat depth_format = draw->depth_target ? draw->depth_target->format : VK_FORMAT_UNDEFINED;
    const VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &draw->target->format,
        .depthAttachmentFormat = depth_format, .stencilAttachmentFormat = depth_format,
    };
    const VkGraphicsPipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
        .stageCount = 2, .pStages = shaders, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport, .pRasterizationState = &raster, .pMultisampleState = &samples,
        .pColorBlendState = &color, .layout = work->pipeline_layout,
        .pDepthStencilState = draw->depth_target ? &depth : NULL,
    };
    return fn->CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline, NULL, &work->pipeline);
}

static VkResult open_target_view(VkDevice device, const DrawFunctions *fn,
                                 const PokopiaLiveDraw *draw, DrawWork *work)
{
    if (!draw->target_layer)
        return VK_SUCCESS;
    const VkImageViewUsageCreateInfo usage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    };
    const VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &usage,
        .image = draw->target->backing.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = draw->target->format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, draw->target_layer, 1},
    };
    return fn->CreateImageView(device, &info, NULL, &work->target_view);
}

static const VkPipelineStageFlags depth_stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
static const VkAccessFlags depth_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

static void transition_sample(const DrawFunctions *fn, VkCommandBuffer command, const PokopiaVkTexture *texture,
                              VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access)
{
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = texture->layout, .newLayout = layout,
        .srcAccessMask = texture->access, .dstAccessMask = access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->backing.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    fn->CmdPipelineBarrier(command, texture->stages, stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void record_feedback_copy(const DrawFunctions *fn, VkCommandBuffer command, const PokopiaLiveDraw *draw,
                                 const DrawWork *work)
{
    const PokopiaVkTexture *target = draw->target;
    PokopiaVkTexture snapshot = work->feedback;
    transition_sample(fn, command, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    transition_sample(fn, command, &snapshot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkImageSubresourceLayers color = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    const VkImageCopy region = {
        .srcSubresource = color, .dstSubresource = color, .extent = {draw->extent.width, draw->extent.height, 1},
    };
    fn->CmdCopyImage(command, target->backing.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     snapshot.backing.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    snapshot.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    snapshot.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    snapshot.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    transition_sample(fn, command, &snapshot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

static VkResult record(const DrawFunctions *fn, const PokopiaLiveDraw *draw, const DrawWork *work)
{
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    const VkCommandBuffer command = work->command;
    VkResult result = fn->BeginCommandBuffer(command, &begin);
    if (result != VK_SUCCESS)
        return result;

    const PokopiaVkTexture *texture = draw->target;
    const bool feedback = work->feedback.backing.image != VK_NULL_HANDLE;
    if (feedback)
        record_feedback_copy(fn, command, draw, work);
    const VkImageMemoryBarrier transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = feedback ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : texture->layout,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcAccessMask = feedback ? VK_ACCESS_TRANSFER_READ_BIT : texture->access,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->backing.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, draw->depth_target ? texture->array_layers : 1},
    };
    fn->CmdPipelineBarrier(command, feedback ? VK_PIPELINE_STAGE_TRANSFER_BIT : texture->stages,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &transition);
    for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i) {
        const PokopiaVkTexture *source = draw->fragment_samples[i].texture;
        if (source && source != texture)
            transition_sample(fn, command, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    VkRenderingAttachmentInfo depth = {0};
    const PokopiaVkTexture *depth_target = draw->depth_target;
    if (depth_target) {
        const VkImageMemoryBarrier depth_transition = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = depth_target->layout,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .srcAccessMask = depth_target->access,
            .dstAccessMask = depth_access,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = depth_target->backing.image, .subresourceRange = {depth_target->aspects, 0, 1, 0, 1},
        };
        fn->CmdPipelineBarrier(command, depth_target->stages, depth_stages, 0, 0, NULL, 0, NULL, 1,
                               &depth_transition);
        depth = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = depth_target->view,
            .imageLayout = depth_transition.newLayout,
            .loadOp = depth_target->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE :
                                                                        VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
    }
    const VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = work->target_view ? work->target_view : texture->view,
        .imageLayout = transition.newLayout,
        .loadOp = texture->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, draw->extent},
        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &color,
        .pDepthAttachment = depth_target ? &depth : NULL, .pStencilAttachment = depth_target ? &depth : NULL,
    };
    fn->CmdBeginRendering(command, &rendering);
    fn->CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, work->pipeline);
    fn->CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, work->pipeline_layout,
                              0, 1, &work->sets[0], 0, NULL);
    if (draw->fragment_samples[0].texture)
        fn->CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, work->pipeline_layout,
                                  2, 1, &work->sets[1], 0, NULL);
    if (draw->index_count) {
        fn->CmdBindVertexBuffers(command, 0, 1, &work->buffer, &work->offsets[VERTICES]);
        fn->CmdBindIndexBuffer(command, work->buffer, work->offsets[INDICES], draw->index_type);
        fn->CmdDrawIndexed(command, draw->index_count, draw->instance_count, draw->first_index,
                           draw->base_vertex, draw->first_instance);
    } else {
        fn->CmdDraw(command, draw->vertex_count, draw->instance_count, draw->first_vertex, draw->first_instance);
    }
    fn->CmdEndRendering(command);
    return fn->EndCommandBuffer(command);
}

static void release(PokopiaVkAdapter *adapter, const DrawFunctions *fn, DrawWork *work)
{
    const VkDevice device = adapter->device;
    if (work->command)
        pokopia_vk_command_buffer_close(adapter, work->command);
    if (work->target_view)
        fn->DestroyImageView(device, work->target_view, NULL);
    if (work->pipeline)
        fn->DestroyPipeline(device, work->pipeline, NULL);
    if (work->descriptor_pool)
        fn->DestroyDescriptorPool(device, work->descriptor_pool, NULL);
    if (work->pipeline_layout)
        fn->DestroyPipelineLayout(device, work->pipeline_layout, NULL);
    for (size_t i = 0; i < sizeof work->layouts / sizeof work->layouts[0]; ++i)
        if (work->layouts[i])
            fn->DestroyDescriptorSetLayout(device, work->layouts[i], NULL);
    for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i)
        if (work->samplers[i])
            fn->DestroySampler(device, work->samplers[i], NULL);
    if (work->feedback.backing.image)
        pokopia_vk_texture_close(adapter, &work->feedback);
    if (work->buffer)
        fn->DestroyBuffer(device, work->buffer, NULL);
    if (work->memory)
        pokopia_vk_ledger_free(fn->FreeMemory, device, work->memory);
}

VkResult pokopia_live_draw(PokopiaVkAdapter *adapter, const PokopiaLiveDraw *draw,
                                  uint64_t *completed_render_serial)
{
    if (!adapter || !adapter->device || !adapter->queue || !adapter->get_device_proc || !adapter->wait_idle ||
        !completed_render_serial || adapter->render_serial == UINT64_MAX)
        return VK_ERROR_INITIALIZATION_FAILED;
    size_t vertex_bytes;
    if (draw && draw->fragment_shader.hash && !strcmp(draw->fragment_shader.hash, "1c913e7b97e7e999")) {
        const PokopiaVkTexture *t = draw->target;
        const PokopiaVkTexture *d = draw->depth_target;
        const PokopiaLiveDrawSample *s = &draw->fragment_samples[0];
        const PokopiaVkTexture *st = s->texture;
        const NvnFrontSamplerInfo *sp = &s->sampler;
        VkFormatProperties props = {0};
        adapter->format_properties(adapter->physical, t->format, &props);
        fprintf(stderr, "Feedback target=%p format=%u layers=%u layout=%u access=%x serial=%llu adapter=%llu depth=%p fmt=%u aspects=%x layout=%u supports_depth=%u layer=%u\n",
                (void *)t, t->format, t->array_layers, t->layout, t->access,
                (unsigned long long)t->render_serial, (unsigned long long)adapter->render_serial,
                (void *)d, d ? d->format : 0, d ? d->aspects : 0, d ? d->layout : 0,
                supported_depth(draw), draw->target_layer);
        fprintf(stderr, "Feedback sample=%p guest=%llx format=%u view_format=%u aspects=%x layers=%u layout=%u stages=%x support=%u features=%x sample1=%p min=%u mag=%u wrap=%u,%u,%u compare=%u reduction=%u lod=%g,%g,%g,%g aniso=%g\n",
                (void *)st, (unsigned long long)s->guest_texture, st ? st->format : 0, s->view_format,
                st ? st->aspects : 0, st ? st->array_layers : 0, st ? st->layout : 0, st ? st->stages : 0,
                supported_sample(s, t->format, true), props.optimalTilingFeatures, (void *)draw->fragment_samples[1].texture,
                sp->min_filter, sp->mag_filter, sp->wrap[0], sp->wrap[1], sp->wrap[2],
                sp->compare_mode, sp->reduction, sp->lod_min, sp->lod_max, sp->lod_bias, sp->lod_snap, sp->max_anisotropy);
        fprintf(stderr, "Feedback topology=%u polygon=%u cull=%u blend=%u depthwrite=%u depthtest=%u stencil=%u mask=%x count=%u first=%u instances=%u firstinst=%u vubo=%zu fubo=%zu viewport=%g,%g,%g,%g,%g,%g scissor=%d,%d,%u,%u\n",
                draw->topology, draw->polygon_mode, draw->cull_mode, draw->blend_enable, draw->depth_write, draw->depth_test,
                draw->stencil_test, draw->color_write_mask, draw->vertex_count, draw->first_vertex, draw->instance_count,
                draw->first_instance, draw->vertex_uniform.bytes, draw->fragment_uniform.bytes,
                draw->viewport.x, draw->viewport.y, draw->viewport.width, draw->viewport.height, draw->viewport.minDepth, draw->viewport.maxDepth,
                draw->scissor.offset.x, draw->scissor.offset.y, draw->scissor.extent.width, draw->scissor.extent.height);
        for (unsigned face = 0; face < 2; ++face) {
            const VkStencilOpState *stencil = &draw->stencil[face];
            fprintf(stderr, "Feedback stencil face=%u cmp=%u fail=%u pass=%u depthfail=%u compare_mask=%x write_mask=%x ref=%u winding=%u\n",
                    face, stencil->compareOp, stencil->failOp, stencil->passOp, stencil->depthFailOp,
                    stencil->compareMask, stencil->writeMask, stencil->reference, draw->front_face);
        }
        fflush(stderr);
    }
    if (!draw || !supported(adapter, draw, &vertex_bytes))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (!adapter->features.shaderFloat64 || !adapter->features.shaderImageGatherExtended)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i) {
        const PokopiaLiveDrawSample *sample = &draw->fragment_samples[i];
        if (!sample->texture)
            continue;
        VkFormatProperties props;
        if (!adapter->format_properties)
            return VK_ERROR_INITIALIZATION_FAILED;
        adapter->format_properties(adapter->physical, sampled_format(sample), &props);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            (sample->sampler.mag_filter ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0);
        if ((props.optimalTilingFeatures & required) != required)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    const VkDevice device = adapter->device;
    DrawFunctions fn;
#define LOAD(name) \
    fn.name = (PFN_vk##name)adapter->get_device_proc(device, "vk" #name); \
    if (!fn.name) \
        return VK_ERROR_FEATURE_NOT_PRESENT;
    DRAW_FUNCTIONS(LOAD)
#undef LOAD

    const bool trace_result = !strcmp(draw->fragment_shader.hash, "1c913e7b97e7e999");
    if (trace_result)
        fprintf(stderr, "Feedback shader entry vertex=%s fragment=%s\n", draw->vertex_shader.entry, draw->fragment_shader.entry);
    DrawWork work = {0};
    VkResult result = open_target_view(device, &fn, draw, &work);
    if (trace_result) { fprintf(stderr, "Feedback open target view %d\n", (int)result); fflush(stderr); }
    if (result == VK_SUCCESS && draw->fragment_samples[0].texture == draw->target) {
        /* The captured R11G11B10F pass reads its previous contents. Keep a
         * matching one-mip snapshot alive through completion of the draw. */
        const NvnFrontTextureConfig config = {
            .target = 1, .format = 0x3f, .width = draw->extent.width, .height = draw->extent.height,
            .depth = 1, .levels = 1, .swizzle = {2, 3, 4, 5},
        };
        result = pokopia_vk_texture_open(adapter, &config, &work.feedback);
        if (trace_result) { fprintf(stderr, "Feedback open feedback image %d\n", (int)result); fflush(stderr); }
    }
    if (result == VK_SUCCESS)
        result = upload(adapter, &fn, draw, vertex_bytes, &work);
    if (trace_result) { fprintf(stderr, "Feedback upload %d\n", (int)result); fflush(stderr); }
    if (result == VK_SUCCESS)
        result = descriptors(device, &fn, draw, &work);
    if (trace_result) { fprintf(stderr, "Feedback descriptors %d\n", (int)result); fflush(stderr); }
    if (result == VK_SUCCESS)
        result = make_pipeline(device, &fn, draw, &work);
    if (trace_result) { fprintf(stderr, "Feedback pipeline %d\n", (int)result); fflush(stderr); }
    if (result == VK_SUCCESS)
        result = pokopia_vk_command_buffer_open(adapter, &work.command);
    if (trace_result) { fprintf(stderr, "Feedback command open %d\n", (int)result); fflush(stderr); }
    if (result == VK_SUCCESS)
        result = record(&fn, draw, &work);
    if (trace_result) { fprintf(stderr, "Feedback record %d\n", (int)result); fflush(stderr); }
    bool retired = true;
    if (result == VK_SUCCESS)
        result = pokopia_vk_submit_wait(adapter, work.command, &retired);
    if (trace_result) { fprintf(stderr, "Feedback submit %d\n", (int)result); fflush(stderr); }
    /* The adapter's submit helper has already attempted both queue and device
     * waits. An unretired error is terminal; keep in-flight resources alive
     * for device teardown. */
    if (retired)
        release(adapter, &fn, &work);
    if (result == VK_SUCCESS) {
        for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i) {
            PokopiaVkTexture *sample = draw->fragment_samples[i].texture;
            if (sample && sample != draw->target) {
                sample->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                sample->stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                sample->access = VK_ACCESS_SHADER_READ_BIT;
            }
        }
        if (draw->depth_target) {
            draw->depth_target->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            draw->depth_target->stages = depth_stages;
            draw->depth_target->access = depth_access;
        }
        draw->target->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        draw->target->stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        draw->target->access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        draw->target->render_serial = ++adapter->render_serial;
        *completed_render_serial = draw->target->render_serial;
    }
    return result;
}
