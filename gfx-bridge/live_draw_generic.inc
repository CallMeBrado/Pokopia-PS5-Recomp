/* SPDX-License-Identifier: GPL-3.0-or-later */
/* R61 general draw, included at the end of live_draw.c so it shares that
 * file's helpers (DrawFunctions, memory_type, sampled_view, copy_text, the
 * depth stage masks and draw_now_ns).
 *
 * The pinned path recognises a handful of loading-screen shapes. This one takes
 * any translated vertex/fragment pair and builds descriptor layouts, samplers,
 * vertex input and pipeline state from the draw itself, then retires it with the
 * same submit-and-wait as the pinned path. */

#if defined(__x86_64__) && defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

/* R75: open-addressed pipeline cache (power of two). */
enum { GENERIC_SUPPORT_BINDING = 0, GENERIC_CACHE_SLOTS = 4096, GENERIC_MAX_UNIFORM = 65536 };

uint64_t pokopia_generic_draw_calls, pokopia_generic_draw_ns, pokopia_generic_pipeline_misses;
/* R73: stand-in auto-exposure until the exposure compute passes run; the
 * loader reads it from debug/exposure. */
float pokopia_exposure_fallback = 0.0f; /* off: the exposure compute pass runs now */
/* R67: general draw time by phase: feedback copies, upload, descriptors,
 * pipeline, record (views, command buffer), submit+wait, release. */
uint64_t pokopia_generic_phase_ns[7];
/* R74: bytes the upload phase copies: uniforms, vertex streams, indices. */
uint64_t pokopia_generic_upload_bytes[3];
/* R66: why the last general draw returned VK_ERROR_FORMAT_NOT_SUPPORTED. */
char pokopia_generic_reject[160];
#define GENERIC_REJECT(...) do { snprintf(pokopia_generic_reject, sizeof pokopia_generic_reject, __VA_ARGS__); \
                                 return VK_ERROR_FORMAT_NOT_SUPPORTED; } while (0)

static VkSamplerAddressMode generic_wrap(uint32_t wrap)
{
    /* NVN WrapMode: CLAMP, REPEAT, MIRROR_CLAMP, MIRROR_CLAMP_TO_EDGE,
     * MIRROR_CLAMP_TO_BORDER, CLAMP_TO_BORDER, MIRRORED_REPEAT, CLAMP_TO_EDGE. */
    switch (wrap) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: case 3: case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static bool generic_sampler(const NvnFrontSamplerInfo *s, bool shadow, VkSamplerCreateInfo *out)
{
    /* R73: depth comparison applies only to shadow bindings; other bindings
     * ignore the sampler's compare mode, as the shader does no comparison. */
    if (s->reduction || s->min_filter > 5)
        return false;
    /* NVN MinFilter: NEAREST, LINEAR, then {NEAREST,LINEAR}_MIPMAP_NEAREST and _MIPMAP_LINEAR. */
    const bool mipmapped = s->min_filter >= 2;
    const bool min_linear = s->min_filter == 1 || s->min_filter == 3 || s->min_filter == 5;
    float max_lod = s->lod_max > 1000.0f ? 1000.0f : s->lod_max;
    float min_lod = s->lod_min < 0.0f ? 0.0f : s->lod_min;
    if (!mipmapped)
        min_lod = max_lod = 0.0f;
    if (max_lod < min_lod)
        max_lod = min_lod;
    *out = (VkSamplerCreateInfo){
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = s->mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = s->min_filter >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = generic_wrap(s->wrap[0]), .addressModeV = generic_wrap(s->wrap[1]),
        .addressModeW = generic_wrap(s->wrap[2]), .mipLodBias = mipmapped ? s->lod_bias : 0.0f,
        .minLod = min_lod, .maxLod = max_lod, .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .compareEnable = shadow,
        /* NVN CompareFunc NEVER 1 ... ALWAYS 8; Vulkan counts from 0. */
        .compareOp = shadow && s->compare_func >= 1 && s->compare_func <= 8 ? (VkCompareOp)(s->compare_func - 1) :
                                                                             VK_COMPARE_OP_LESS_OR_EQUAL,
    };
    /* Shadow maps are clamped with a white border: outside the map is lit. */
    if (shadow && out->addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)
        out->borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    return true;
}

typedef struct {
    char vertex_hash[24], fragment_hash[24], vertex_entry[16], fragment_entry[16];
    uint32_t binding_count, attribute_count, uniform_count, texture_count;
    VkVertexInputBindingDescription bindings[POKOPIA_GENERIC_STREAMS];
    VkVertexInputAttributeDescription attributes[POKOPIA_GENERIC_ATTRIBUTES];
    uint32_t uniform_bindings[POKOPIA_GENERIC_UNIFORMS], texture_bindings[POKOPIA_GENERIC_TEXTURES];
    VkPrimitiveTopology topology;
    VkViewport viewport;
    VkRect2D scissor;
    VkCullModeFlags cull_mode;
    VkFrontFace front_face;
    VkColorComponentFlags color_write_mask;
    uint32_t blend_enable, depth_test, depth_write, stencil_test, has_target, has_depth;
    uint32_t depth_bias; /* R73 */
    float depth_bias_factor, depth_bias_units;
    VkBlendFactor factors[4];
    VkBlendOp ops[2];
    VkCompareOp depth_compare;
    VkStencilOpState stencil[2];
    VkFormat color_format, depth_format;
    /* R63: multiple render targets */
    uint32_t color_count, blend_bits;
    VkFormat color_formats[8];
    VkColorComponentFlags color_masks[8];
} GenericKey;

/* R63: the colour attachments in slot order; NULL marks an unused slot. */
static uint32_t generic_colors(const PokopiaGenericDraw *draw, PokopiaVkTexture *colors[8])
{
    if (!draw->target)
        return 0;
    uint32_t count = draw->color_count > 1 ? draw->color_count : 1;
    if (count > 8)
        count = 8;
    colors[0] = draw->target;
    for (uint32_t i = 1; i < count; ++i)
        colors[i] = draw->more_targets[i - 1];
    return count;
}

static VkColorComponentFlags generic_color_mask(const PokopiaGenericDraw *draw, uint32_t i)
{
    return draw->color_count > 1 ? (VkColorComponentFlags)((draw->color_write_masks >> (4 * i)) & 15u) :
                                   draw->color_write_mask;
}

static bool generic_blend_on(const PokopiaGenericDraw *draw, uint32_t i)
{
    return draw->color_count > 1 ? draw->blend_enable && ((draw->blend_mask >> i) & 1u) : draw->blend_enable;
}

typedef struct {
    uint64_t hash;
    GenericKey key;
    VkPipeline pipeline;
} GenericEntry;

static GenericEntry generic_cache[GENERIC_CACHE_SLOTS];
static size_t generic_cache_count;
static VkDevice generic_cache_device;
static const GenericEntry *generic_last_pipeline;

typedef struct {
    VkFormat format;
    bool valid, sampled;
} GenericFormat;

static bool generic_format_sampled(const PokopiaVkAdapter *adapter, VkFormat format, bool cache)
{
    static VkDevice device;
    static GenericFormat formats[64];
    GenericFormat *entry = NULL;
    if (cache) {
        if (device != adapter->device) {
            memset(formats, 0, sizeof formats);
            device = adapter->device;
        }
        entry = &formats[((uint32_t)format * UINT32_C(0x9e3779b9)) >> 26];
        if (entry->valid && entry->format == format)
            return entry->sampled;
    }
    VkFormatProperties properties;
    adapter->format_properties(adapter->physical, format, &properties);
    const bool sampled = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    if (entry)
        *entry = (GenericFormat){.format = format, .valid = true, .sampled = sampled};
    return sampled;
}

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDescriptorSetLayout layouts[3];
    VkPipelineLayout pipeline_layout;
    VkDescriptorPool pool;
    VkDescriptorSet sets[2];
    VkSampler samplers[POKOPIA_GENERIC_TEXTURES];
    VkPipeline pipeline;
    bool pipeline_cached;
    VkCommandBuffer command;
    VkDeviceSize support_offset, uniform_offsets[POKOPIA_GENERIC_UNIFORMS];
    VkDeviceSize stream_offsets[POKOPIA_GENERIC_STREAMS], index_offset;
    uint32_t uniform_bytes[POKOPIA_GENERIC_UNIFORMS];
    VkImageView layer_views[8]; /* R64: views for array layers above 0; R78: owned by layer_view_cache */
    VkImageView texture_views[POKOPIA_GENERIC_TEXTURES]; /* R66: sampled views, owned by the textures */
    /* Read eligible inputs directly from the imported, coherent guest heap. */
    VkBuffer uniform_buffers[POKOPIA_GENERIC_UNIFORMS];
    VkBuffer stream_buffers[POKOPIA_GENERIC_STREAMS], index_buffer;
    bool shared_buffer; /* R67: buffer and memory belong to generic_shared */
    bool layouts_cached, samplers_cached, pool_shared; /* R74: owned by the caches below */
} GenericWork;

/* R74: set layouts and the pipeline layout depend only on the binding
 * numbers and descriptor types, samplers only on their create info; both were
 * created and destroyed for every draw. One descriptor pool serves every draw
 * and is reset once the draw retires. */
typedef struct {
    uint32_t uniform_count, texture_count;
    uint32_t uniforms[POKOPIA_GENERIC_UNIFORMS], textures[POKOPIA_GENERIC_TEXTURES]; /* texel bit 31 */
} GenericLayoutKey;

static struct {
    GenericLayoutKey key;
    VkDescriptorSetLayout layouts[3];
    VkPipelineLayout pipeline_layout;
    bool used;
} generic_layout_cache[512];

typedef struct {
    VkFilter mag, min;
    VkSamplerMipmapMode mipmap;
    VkSamplerAddressMode u, v, w;
    float bias, min_lod, max_lod;
    VkBool32 compare;
    VkCompareOp compare_op;
    VkBorderColor border;
} GenericSamplerKey;

static struct {
    GenericSamplerKey key;
    VkSampler sampler;
    bool used;
} generic_sampler_cache[1024];


static uint64_t generic_hash(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    size_t i = 0;
#if defined(__x86_64__) && defined(__SSE4_2__)
    /* The cache still compares the full keys. CRC32C avoids a serial
     * multiply for every byte of every sampler or layout key. */
    uint64_t hash = UINT32_MAX;
    for (; i + sizeof(uint64_t) <= bytes; i += sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, p + i, sizeof word);
        hash = _mm_crc32_u64(hash, word);
    }
    for (; i < bytes; ++i)
        hash = _mm_crc32_u8((uint32_t)hash, p[i]);
    return hash ^ (hash >> 16);
#else
    uint64_t hash = UINT64_C(1469598103934665603);
    for (; i + sizeof(uint64_t) <= bytes; i += sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, p + i, sizeof word);
        hash = (hash ^ word) * UINT64_C(1099511628211);
        hash ^= hash >> 29;
    }
    for (; i < bytes; ++i)
        hash = (hash ^ p[i]) * UINT64_C(1099511628211);
    return hash;
#endif
}

static VkSampler generic_cached_sampler(VkDevice device, const DrawFunctions *fn, const VkSamplerCreateInfo *info)
{
    GenericSamplerKey key;
    memset(&key, 0, sizeof key);
    key.mag = info->magFilter;
    key.min = info->minFilter;
    key.mipmap = info->mipmapMode;
    key.u = info->addressModeU;
    key.v = info->addressModeV;
    key.w = info->addressModeW;
    key.bias = info->mipLodBias;
    key.min_lod = info->minLod;
    key.max_lod = info->maxLod;
    key.compare = info->compareEnable;
    key.compare_op = info->compareOp;
    key.border = info->borderColor;
    const size_t slots = sizeof generic_sampler_cache / sizeof generic_sampler_cache[0];
    size_t slot = (size_t)generic_hash(&key, sizeof key) & (slots - 1);
    for (size_t probe = 0; probe < slots; ++probe, slot = (slot + 1) & (slots - 1)) {
        if (!generic_sampler_cache[slot].used) {
            VkSampler sampler = VK_NULL_HANDLE;
            if (fn->CreateSampler(device, info, NULL, &sampler) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            generic_sampler_cache[slot].key = key;
            generic_sampler_cache[slot].sampler = sampler;
            generic_sampler_cache[slot].used = true;
            return sampler;
        }
        if (!memcmp(&generic_sampler_cache[slot].key, &key, sizeof key))
            return generic_sampler_cache[slot].sampler;
    }
    return VK_NULL_HANDLE;
}

/* R67: general draws retire one at a time, so one persistently mapped buffer
 * serves them all. A draw that did not retire keeps it busy, and later draws
 * fall back to their own allocation. */
static struct {
    VkDevice device;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    VkDeviceSize size;
    bool coherent, busy;
} generic_shared;

static void generic_release(PokopiaVkAdapter *adapter, const DrawFunctions *fn, GenericWork *work)
{
    const VkDevice device = adapter->device;
    if (work->shared_buffer) {
        work->buffer = VK_NULL_HANDLE;
        work->memory = VK_NULL_HANDLE;
    }
    if (work->command)
        pokopia_vk_command_buffer_close(adapter, work->command);
    if (work->pipeline && !work->pipeline_cached)
        fn->DestroyPipeline(device, work->pipeline, NULL);
    if (work->pool && work->pool_shared) {
        /* R75: reset once per batch in generic_batch_flush() */
    } else if (work->pool) {
        fn->DestroyDescriptorPool(device, work->pool, NULL);
    }
    if (!work->layouts_cached) {
        if (work->pipeline_layout)
            fn->DestroyPipelineLayout(device, work->pipeline_layout, NULL);
        for (size_t i = 0; i < 3; ++i)
            if (work->layouts[i])
                fn->DestroyDescriptorSetLayout(device, work->layouts[i], NULL);
    }
    if (!work->samplers_cached)
        for (size_t i = 0; i < POKOPIA_GENERIC_TEXTURES; ++i)
            if (work->samplers[i])
                fn->DestroySampler(device, work->samplers[i], NULL);
    if (work->buffer)
        fn->DestroyBuffer(device, work->buffer, NULL);
    if (work->memory)
        pokopia_vk_ledger_free(fn->FreeMemory, device, work->memory);
    for (size_t i = 0; i < 8; ++i)
        (void)work->layer_views[i]; /* R78: cached in layer_view_cache */
}

static DrawFunctions generic_functions;
static VkDevice generic_functions_device;

static bool generic_functions_load(PokopiaVkAdapter *adapter)
{
    const VkDevice device = adapter->device;
    if (generic_functions_device == device)
        return true;
    DrawFunctions fn;
#define LOAD(name) \
    fn.name = (PFN_vk##name)adapter->get_device_proc(device, "vk" #name); \
    if (!fn.name) \
        return false;
    DRAW_FUNCTIONS(LOAD)
#undef LOAD
    generic_functions = fn;
    generic_functions_device = device;
    return true;
}

/* R75: general draws are recorded into one command buffer and submitted
 * together; each was its own submit and wait (~1.9 s of every 5 s in the
 * world). Draw data goes to a linear arena in the shared buffer, descriptor
 * sets to one pool sized for a whole batch. The batch is flushed when full,
 * before any other GPU submission or resource release (vk_flush.h), at the end
 * of every guest queue submit, and before present. */
enum { GENERIC_BATCH = 512, GENERIC_ARENA_BYTES = 64u << 20, GENERIC_SLOTS = 2 }; /* R89: 6 slots measured slower per draw */
/* R77: two batch slots. A full batch is submitted with a fence and the next one
 * is recorded in the other slot while the GPU runs it (every flush waited for
 * the GPU: ~0.6 s per 5 s). A slot's arena half, descriptor pool, per-draw
 * objects and command buffer are reused once its fence has signaled. */
typedef struct {
    VkCommandBuffer command; /* in flight while pending, else reset and ready */
    VkFence fence;
    VkDescriptorPool pool;
    GenericWork works[GENERIC_BATCH];
    uint32_t retire_count;
    uint64_t id;
    bool pending;
    /* R78: image-only compute dispatches recorded into the batch */
    VkDescriptorPool compute_pool;
    uint32_t compute_count;
    bool timed; /* R89: GPU timestamps at the batch's start and end */
} GenericSlot;
static GenericSlot generic_slots[GENERIC_SLOTS];

/* R89: GPU time spent on draw batches (the union of their start-end spans). */
static VkQueryPool generic_queries;
static PFN_vkCmdWriteTimestamp generic_write_timestamp;
static PFN_vkCmdResetQueryPool generic_reset_queries;
static PFN_vkGetQueryPoolResults generic_query_results;
static uint64_t generic_gpu_last_end;
static PokopiaVkAdapter *generic_query_adapter;
uint64_t pokopia_gpu_batch_ns, pokopia_gpu_batches;
uint64_t pokopia_generic_submit_ns, pokopia_generic_submits; /* R90: vkQueueSubmit of draw batches */

static bool generic_queries_ready(PokopiaVkAdapter *adapter)
{
    static bool tried;
    if (generic_queries)
        return true;
    if (tried || !adapter || !adapter->device)
        return false;
    tried = true;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    PFN_vkCreateQueryPool create = (PFN_vkCreateQueryPool)proc(adapter->device, "vkCreateQueryPool");
    generic_write_timestamp = (PFN_vkCmdWriteTimestamp)proc(adapter->device, "vkCmdWriteTimestamp");
    generic_reset_queries = (PFN_vkCmdResetQueryPool)proc(adapter->device, "vkCmdResetQueryPool");
    generic_query_results = (PFN_vkGetQueryPoolResults)proc(adapter->device, "vkGetQueryPoolResults");
    if (!create || !generic_write_timestamp || !generic_reset_queries || !generic_query_results ||
        !(adapter->properties.limits.timestampPeriod > 0.0f))
        return false;
    const VkQueryPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = 2 * GENERIC_SLOTS,
    };
    VkQueryPool pool;
    if (create(adapter->device, &info, NULL, &pool) != VK_SUCCESS)
        return false;
    generic_queries = pool;
    generic_query_adapter = adapter;
    return true;
}

static void generic_slot_timing(GenericSlot *slot)
{
    if (!slot->timed)
        return;
    slot->timed = false;
    PokopiaVkAdapter *adapter = generic_query_adapter;
    const uint32_t first = 2 * (uint32_t)(slot - generic_slots);
    uint64_t ticks[2];
    if (generic_query_results(adapter->device, generic_queries, first, 2, sizeof ticks, ticks, sizeof ticks[0],
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || ticks[1] < ticks[0])
        return;
    if (ticks[1] > generic_gpu_last_end) {
        const uint64_t start = ticks[0] > generic_gpu_last_end ? ticks[0] : generic_gpu_last_end;
        pokopia_gpu_batch_ns += (uint64_t)((double)(ticks[1] - start) * adapter->properties.limits.timestampPeriod);
        generic_gpu_last_end = ticks[1];
    }
    ++pokopia_gpu_batches;
}
static unsigned generic_slot;
#define generic_pool (generic_slots[generic_slot].pool)
static struct {
    PokopiaVkAdapter *adapter;
    VkCommandBuffer command;
    DrawFunctions fn;
    uint32_t count;
    VkDeviceSize arena_used;
    bool flushing;
    /* R76: dynamic rendering left open across draws with the same attachments
     * and no barrier between them (each draw began and ended its own). */
    bool rendering, rendering_stencil;
    uint32_t rendering_colors;
    VkImageView rendering_views[8], rendering_depth;
    VkExtent2D rendering_extent;
    /* R90: graphics state last bound in this command buffer (skipped when a
     * draw binds the same again); forgotten whenever the buffer is begun or
     * lent to other code (generic_batch_command_hook). */
    struct {
        bool valid;
        VkPipeline pipeline;
        VkPipelineLayout layout;
        VkDescriptorSet sets[2];
        VkViewport viewport;
        VkRect2D scissor;
    } bound;
} generic_batch;
uint64_t pokopia_generic_binds_skipped;

static void generic_end_rendering(void)
{
    if (generic_batch.rendering && generic_batch.command)
        generic_batch.fn.CmdEndRendering(generic_batch.command);
    generic_batch.rendering = false;
}
uint64_t pokopia_generic_batches, pokopia_generic_merged, pokopia_generic_compute_batched;

static PFN_vkQueueSubmit generic_queue_submit;
static PFN_vkCreateFence generic_create_fence;
static PFN_vkResetFences generic_reset_fences;
static PFN_vkWaitForFences generic_wait_fences;
static PFN_vkResetDescriptorPool generic_reset_pool;

static bool generic_sync_procs(PokopiaVkAdapter *adapter)
{
    if (!generic_queue_submit) {
        const VkDevice device = adapter->device;
        generic_queue_submit = (PFN_vkQueueSubmit)adapter->get_device_proc(device, "vkQueueSubmit");
        generic_create_fence = (PFN_vkCreateFence)adapter->get_device_proc(device, "vkCreateFence");
        generic_reset_fences = (PFN_vkResetFences)adapter->get_device_proc(device, "vkResetFences");
        generic_wait_fences = (PFN_vkWaitForFences)adapter->get_device_proc(device, "vkWaitForFences");
        generic_reset_pool = (PFN_vkResetDescriptorPool)adapter->get_device_proc(device, "vkResetDescriptorPool");
    }
    return generic_queue_submit && generic_create_fence && generic_reset_fences && generic_wait_fences &&
           generic_reset_pool;
}

/* Releases a slot's objects. Only for a slot the GPU is done with (or never saw). */
static void generic_slot_release(GenericSlot *slot)
{
    PokopiaVkAdapter *adapter = generic_batch.adapter;
    const DrawFunctions *fn = &generic_batch.fn;
    for (uint32_t i = 0; i < slot->retire_count; ++i)
        generic_release(adapter, fn, &slot->works[i]);
    slot->retire_count = 0;
    if (slot->pool && generic_reset_pool)
        generic_reset_pool(adapter->device, slot->pool, 0);
    if (slot->compute_pool && generic_reset_pool)
        generic_reset_pool(adapter->device, slot->compute_pool, 0);
    slot->compute_count = 0;
    if (slot->command && fn->ResetCommandBuffer(slot->command, 0) != VK_SUCCESS) {
        pokopia_vk_command_buffer_close(adapter, slot->command);
        slot->command = VK_NULL_HANDLE;
    }
}

static void generic_slot_retire(GenericSlot *slot)
{
    if (!slot->pending)
        return;
    PokopiaVkAdapter *adapter = generic_batch.adapter;
    const uint64_t start = draw_now_ns();
    const VkResult waited = generic_wait_fences(adapter->device, 1, &slot->fence, VK_TRUE,
                                                UINT64_C(10000000000));
    pokopia_generic_phase_ns[5] += draw_now_ns() - start;
    slot->pending = false;
    if (waited != VK_SUCCESS) {
        /* Lost or hung: keep the objects rather than free ones still in use. */
        slot->retire_count = 0;
        slot->command = VK_NULL_HANDLE;
        return;
    }
    generic_slot_timing(slot);
    generic_slot_release(slot);
    if (slot->id > pokopia_vk_batch_retired)
        pokopia_vk_batch_retired = slot->id;
}

static VkResult generic_batch_submit(bool wait)
{
    if (!generic_batch.adapter || generic_batch.flushing)
        return VK_SUCCESS;
    generic_batch.flushing = true;
    PokopiaVkAdapter *adapter = generic_batch.adapter;
    const DrawFunctions *fn = &generic_batch.fn;
    VkResult result = VK_SUCCESS;
    GenericSlot *slot = &generic_slots[generic_slot];
    if (generic_batch.command || generic_batch.count) {
        bool submitted = false;
        if (generic_batch.command) {
            generic_end_rendering();
            if (pokopia_vk_before_batch_end_hook)
                pokopia_vk_before_batch_end_hook(generic_batch.command);
            if (slot->timed)
                generic_write_timestamp(generic_batch.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, generic_queries,
                                        2 * (uint32_t)(slot - generic_slots) + 1);
            result = fn->EndCommandBuffer(generic_batch.command);
            if (result == VK_SUCCESS && !generic_sync_procs(adapter))
                result = VK_ERROR_INITIALIZATION_FAILED;
            if (result == VK_SUCCESS && !slot->fence) {
                const VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                result = generic_create_fence(adapter->device, &info, NULL, &slot->fence);
            }
            if (result == VK_SUCCESS)
                result = generic_reset_fences(adapter->device, 1, &slot->fence);
            if (result == VK_SUCCESS) {
                const VkSubmitInfo submit = {
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                    .pCommandBuffers = &generic_batch.command,
                };
                const uint64_t submit_start = draw_now_ns();
                result = generic_queue_submit(adapter->queue, 1, &submit, slot->fence);
                pokopia_generic_submit_ns += draw_now_ns() - submit_start; /* R90 */
                ++pokopia_generic_submits;
                submitted = result == VK_SUCCESS;
            }
        }
        slot->command = generic_batch.command;
        slot->id = pokopia_vk_batch_id;
        slot->pending = submitted;
        if (submitted && pokopia_vk_after_submit_hook)
            pokopia_vk_after_submit_hook();
        if (!submitted)
            generic_slot_release(slot);
        ++pokopia_generic_batches;
        ++pokopia_vk_batch_id;
        generic_batch.command = VK_NULL_HANDLE;
        generic_batch.rendering = false;
        generic_batch.count = 0;
        generic_batch.arena_used = 0;
        generic_slot = (generic_slot + 1) % GENERIC_SLOTS;
        generic_slot_retire(&generic_slots[generic_slot]);
    }
    if (wait)
        for (unsigned i = 0; i < GENERIC_SLOTS; ++i)
            generic_slot_retire(&generic_slots[i]);
    generic_batch.flushing = false;
    return result;
}

bool pokopia_live_generic_wait_batch(uint64_t id)
{
    if (pokopia_vk_batch_retired >= id)
        return true;
    if (generic_batch.flushing || !generic_batch.adapter)
        return false;
    for (unsigned i = 0; i < GENERIC_SLOTS; ++i)
        if (generic_slots[i].pending && generic_slots[i].id <= id)
            generic_slot_retire(&generic_slots[i]);
    return pokopia_vk_batch_retired >= id;
}

static VkResult generic_batch_flush(void)
{
    return generic_batch_submit(false);
}

static VkResult generic_batch_open(PokopiaVkAdapter *adapter, const DrawFunctions *fn);
static VkCommandBuffer generic_batch_command_hook(void)
{
    if (!generic_batch.adapter || generic_batch.flushing ||
        generic_batch_open(generic_batch.adapter, &generic_batch.fn) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    generic_end_rendering();
    generic_batch.bound.valid = false; /* R90: other code may bind state */
    return generic_batch.command;
}

/* R90: draws recorded into the open batch. */
uint32_t pokopia_live_generic_batch_count(void)
{
    return generic_batch.command && !generic_batch.flushing ? generic_batch.count : 0;
}

bool pokopia_live_generic_batch_open(void)
{
    return generic_batch.command != VK_NULL_HANDLE && !generic_batch.flushing;
}

static bool generic_flush_hook(bool wait)
{
    if (generic_batch.flushing)
        return false;
    bool pending = generic_batch.command != VK_NULL_HANDLE;
    for (unsigned i = 0; wait && i < GENERIC_SLOTS; ++i)
        pending |= generic_slots[i].pending;
    (void)generic_batch_submit(wait);
    return pending;
}

static VkResult generic_batch_open(PokopiaVkAdapter *adapter, const DrawFunctions *fn)
{
    generic_batch.adapter = adapter;
    generic_batch.fn = *fn;
    pokopia_vk_flush_hook = generic_flush_hook;
    pokopia_vk_batch_command_hook = generic_batch_command_hook;
    if (generic_batch.command)
        return VK_SUCCESS;
    GenericSlot *slot = &generic_slots[generic_slot];
    generic_slot_retire(slot);
    VkResult result = VK_SUCCESS;
    if (slot->command) {
        generic_batch.command = slot->command;
        slot->command = VK_NULL_HANDLE;
    } else {
        result = pokopia_vk_command_buffer_open(adapter, &generic_batch.command);
    }
    if (result != VK_SUCCESS) {
        generic_batch.command = VK_NULL_HANDLE;
        return result;
    }
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = fn->BeginCommandBuffer(generic_batch.command, &begin);
    generic_batch.bound.valid = false; /* R90 */
    if (result != VK_SUCCESS) {
        pokopia_vk_command_buffer_close(adapter, generic_batch.command);
        generic_batch.command = VK_NULL_HANDLE;
    } else if (generic_queries_ready(adapter)) {
        const uint32_t first = 2 * (uint32_t)(slot - generic_slots);
        generic_reset_queries(generic_batch.command, generic_queries, first, 2);
        generic_write_timestamp(generic_batch.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, generic_queries, first);
        slot->timed = true;
    }
    return result;
}

/* R75: within one batch (and between guest buffer copies) a guest range reads
 * the same bytes for every draw, as on the console's GPU, so a vertex or index
 * range already copied to the arena is reused instead of copied again. */
typedef struct {
    const void *data;
    size_t bytes;
    VkDeviceSize offset;
    uint64_t batch, epoch;
} GenericReuse;
static GenericReuse generic_reuse[4096];
uint64_t pokopia_generic_reused_bytes;

static GenericReuse *generic_reuse_slot(const void *data, size_t bytes)
{
    uint64_t h = (uint64_t)(uintptr_t)data * UINT64_C(0x9e3779b97f4a7c15) ^ (uint64_t)bytes;
    h ^= h >> 31;
    return &generic_reuse[h & (sizeof generic_reuse / sizeof generic_reuse[0] - 1)];
}

/* Copies one range to the arena unless this batch already holds it; returns
 * the offset the draw binds. */
static VkDeviceSize generic_place(uint8_t *mapped, VkDeviceSize offset, const void *data, size_t bytes)
{
    if (!bytes)
        return offset;
    GenericReuse *slot = generic_reuse_slot(data, bytes);
    if (bytes >= 256 && slot->data == data && slot->bytes == bytes && slot->batch == pokopia_vk_batch_id &&
        slot->epoch == pokopia_vk_guest_write_epoch) {
        pokopia_generic_reused_bytes += bytes;
        return slot->offset;
    }
    memcpy(mapped + offset, data, bytes);
    *slot = (GenericReuse){data, bytes, offset, pokopia_vk_batch_id, pokopia_vk_guest_write_epoch};
    return offset;
}

/* R64: colour attachments on an array layer above 0 need their own view; the
 * texture's own attachment view is level 0, layer 0. */
/* R78: attachment views for a layer or mip level, kept per image (a view was
 * created and destroyed for every such draw, and its new handle kept the draws
 * from sharing a render pass). Dropped when the image is destroyed. */
enum { LAYER_VIEWS = 1024 };
static struct {
    VkImage image;
    VkFormat format;
    uint32_t level, layer;
    VkImageView view;
} layer_view_cache[LAYER_VIEWS];
static VkDevice layer_view_device;
static PFN_vkDestroyImageView layer_view_destroy;

static void layer_view_forget(VkImage image)
{
    for (unsigned i = 0; i < LAYER_VIEWS; ++i)
        if (layer_view_cache[i].image == image && layer_view_cache[i].view) {
            layer_view_destroy(layer_view_device, layer_view_cache[i].view, NULL);
            layer_view_cache[i] = (__typeof__(layer_view_cache[0])){0};
        }
}

static VkResult generic_layer_views(VkDevice device, const DrawFunctions *fn, const PokopiaGenericDraw *draw,
                                    GenericWork *work)
{
    if (layer_view_device != device) {
        layer_view_device = device;
        layer_view_destroy = fn->DestroyImageView;
        pokopia_vk_image_forget_hook = layer_view_forget;
    }
    PokopiaVkTexture *colors[8] = {0};
    const uint32_t count = generic_colors(draw, colors);
    for (uint32_t i = 0; i < count; ++i) {
        if (!colors[i] || (!draw->color_layers[i] && !draw->color_levels[i]))
            continue;
        const VkImageViewUsageCreateInfo usage = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        };
        const VkImageViewCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &usage,
            .image = colors[i]->backing.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = colors[i]->format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, draw->color_levels[i], 1, draw->color_layers[i], 1},
        };
        const uint64_t key = (uint64_t)(uintptr_t)colors[i]->backing.image * UINT64_C(0x9e3779b97f4a7c15) ^
                             ((uint64_t)draw->color_levels[i] << 32 | draw->color_layers[i]) ^ (uint64_t)colors[i]->format;
        unsigned slot = (unsigned)(key ^ key >> 29) & (LAYER_VIEWS - 1), empty = LAYER_VIEWS;
        for (unsigned probe = 0; probe < 32; ++probe, slot = (slot + 1) & (LAYER_VIEWS - 1)) {
            if (!layer_view_cache[slot].view) {
                if (empty == LAYER_VIEWS)
                    empty = slot;
                continue;
            }
            if (layer_view_cache[slot].image == colors[i]->backing.image &&
                layer_view_cache[slot].format == colors[i]->format &&
                layer_view_cache[slot].level == draw->color_levels[i] &&
                layer_view_cache[slot].layer == draw->color_layers[i]) {
                work->layer_views[i] = layer_view_cache[slot].view;
                break;
            }
        }
        if (work->layer_views[i])
            continue;
        if (empty == LAYER_VIEWS)
            return VK_ERROR_TOO_MANY_OBJECTS;
        const VkResult result = fn->CreateImageView(device, &info, NULL, &work->layer_views[i]);
        if (result != VK_SUCCESS)
            return result;
        layer_view_cache[empty] = (__typeof__(layer_view_cache[0])){colors[i]->backing.image, colors[i]->format,
                                                                     draw->color_levels[i], draw->color_layers[i],
                                                                     work->layer_views[i]};
    }
    return VK_SUCCESS;
}

static size_t generic_align(size_t value, size_t alignment)
{
    const size_t remainder = value % alignment;
    return remainder ? value + alignment - remainder : value;
}

/* R78: the guest heap (direct memory on the coherent bus) imported with
 * VK_EXT_external_memory_host as 2 GiB buffers starting every 1 GiB, so any
 * range up to 1 GiB lies inside one. Vertex and index data are read where the
 * game wrote them instead of being copied (~8 GB per 5 s). */
uint64_t pokopia_guest_import_base, pokopia_guest_import_size, pokopia_generic_direct_bytes;
enum { GUEST_WINDOW_BYTES = 1u << 31, GUEST_WINDOW_STEP = 1u << 30, GUEST_WINDOWS = 16 };
static struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint64_t base, size;
    bool host_coherent;
} guest_windows[GUEST_WINDOWS];
static unsigned guest_window_count;
static bool guest_import_tried;
FILE *pokopia_generic_log;

static void generic_guest_import(const PokopiaVkAdapter *adapter, const DrawFunctions *fn)
{
    guest_import_tried = true;
    const uint64_t base = pokopia_guest_import_base, size = pokopia_guest_import_size;
    if (!base || !size || (base | size) & 0x3fff)
        return;
    const VkDevice device = adapter->device;
    const PFN_vkGetMemoryHostPointerPropertiesEXT properties = (PFN_vkGetMemoryHostPointerPropertiesEXT)
        adapter->get_device_proc(device, "vkGetMemoryHostPointerPropertiesEXT");
    if (!properties) {
        if (pokopia_generic_log)
            fprintf(pokopia_generic_log, "Guest heap import unavailable (no VK_EXT_external_memory_host)\n");
        return;
    }
    for (uint64_t at = base; at < base + size && guest_window_count < GUEST_WINDOWS; at += GUEST_WINDOW_STEP) {
        const uint64_t bytes = base + size - at < GUEST_WINDOW_BYTES ? base + size - at : GUEST_WINDOW_BYTES;
        VkMemoryHostPointerPropertiesEXT host = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        VkResult result = properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                     (void *)(uintptr_t)at, &host);
        const VkExternalMemoryBufferCreateInfo external = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        };
        const VkBufferCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external, .size = bytes,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (result == VK_SUCCESS)
            result = fn->CreateBuffer(device, &info, NULL, &buffer);
        uint32_t type = UINT32_MAX;
        if (result == VK_SUCCESS) {
            VkMemoryRequirements requirements;
            fn->GetBufferMemoryRequirements(device, buffer, &requirements);
            const uint32_t bits = requirements.memoryTypeBits & host.memoryTypeBits;
            for (uint32_t i = 0; i < 32 && type == UINT32_MAX; ++i)
                if (bits & (UINT32_C(1) << i))
                    type = i;
            if (type == UINT32_MAX)
                result = VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (result == VK_SUCCESS) {
            const VkImportMemoryHostPointerInfoEXT import = {
                .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                .pHostPointer = (void *)(uintptr_t)at,
            };
            const VkMemoryAllocateInfo allocate = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import, .allocationSize = bytes,
                .memoryTypeIndex = type,
            };
            result = fn->AllocateMemory(device, &allocate, NULL, &memory);
        }
        if (result == VK_SUCCESS)
            result = fn->BindBufferMemory(device, buffer, memory, 0);
        if (pokopia_generic_log) {
            fprintf(pokopia_generic_log, "Guest heap import %#" PRIx64 "+%#" PRIx64 " result=%d type=%u\n", at, bytes,
                    (int)result, type);
            fflush(pokopia_generic_log);
        }
        if (result != VK_SUCCESS) {
            if (buffer)
                fn->DestroyBuffer(device, buffer, NULL);
            if (memory)
                fn->FreeMemory(device, memory, NULL);
            return;
        }
        const bool coherent = (adapter->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        guest_windows[guest_window_count++] = (__typeof__(guest_windows[0])){buffer, memory, at, bytes, coherent};
    }
}

static bool generic_guest_bind(const void *data, size_t bytes, size_t align, bool require_coherent,
                               VkBuffer *buffer, VkDeviceSize *offset)
{
    const uint64_t address = (uint64_t)(uintptr_t)data;
    if (!guest_window_count || !bytes || address < pokopia_guest_import_base || address % align)
        return false;
    const uint64_t index = (address - pokopia_guest_import_base) / GUEST_WINDOW_STEP;
    if (index >= guest_window_count || (require_coherent && !guest_windows[index].host_coherent))
        return false;
    if (address + bytes > guest_windows[index].base + guest_windows[index].size)
        return false;
    *buffer = guest_windows[index].buffer;
    *offset = address - guest_windows[index].base;
    pokopia_generic_direct_bytes += bytes;
    return true;
}

/* R80: the stream will be bound in place (no copy), so the replay need not
 * find the vertex range a draw reads. */
bool pokopia_live_generic_direct(const void *data, size_t bytes)
{
    const uint64_t address = (uint64_t)(uintptr_t)data;
    if (!guest_window_count || !bytes || address < pokopia_guest_import_base || address % 4)
        return false;
    const uint64_t index = (address - pokopia_guest_import_base) / GUEST_WINDOW_STEP;
    return index < guest_window_count && address + bytes <= guest_windows[index].base + guest_windows[index].size;
}

static VkResult generic_upload(const PokopiaVkAdapter *adapter, const DrawFunctions *fn,
                               const PokopiaGenericDraw *draw, GenericWork *work)
{
    if (!guest_import_tried)
        generic_guest_import(adapter, fn);
    const size_t uniform_align = (size_t)adapter->properties.limits.minUniformBufferOffsetAlignment;
    const size_t uniform_limit = adapter->properties.limits.maxUniformBufferRange < GENERIC_MAX_UNIFORM ?
        (size_t)adapter->properties.limits.maxUniformBufferRange : GENERIC_MAX_UNIFORM;
    if (!uniform_align)
        return VK_ERROR_INITIALIZATION_FAILED;
    size_t bytes = 0;
    const bool direct_uniforms = (pokopia_debug_flags & (1u << 27)) != 0;
    for (uint32_t i = 0; i < draw->uniform_count; ++i) {
        const PokopiaLiveDrawSpan *data = &draw->uniforms[i].data;
        const uint32_t count = (uint32_t)(data->bytes < uniform_limit ? data->bytes : uniform_limit);
        work->uniform_bytes[i] = count;
        if (direct_uniforms && generic_guest_bind(data->data, count, uniform_align, true, &work->uniform_buffers[i],
                                                   &work->uniform_offsets[i]))
            continue;
        bytes = generic_align(bytes, uniform_align);
        work->uniform_offsets[i] = bytes;
        bytes += count ? count : 16;
        pokopia_generic_upload_bytes[0] += count;
    }
    for (uint32_t i = 0; i < draw->binding_count; ++i) {
        work->stream_buffers[i] = VK_NULL_HANDLE;
        if (generic_guest_bind(draw->streams[i].data, draw->streams[i].bytes, 4, false, &work->stream_buffers[i],
                               &work->stream_offsets[i]))
            continue;
        bytes = generic_align(bytes, 16);
        work->stream_offsets[i] = bytes;
        bytes += draw->streams[i].bytes;
        pokopia_generic_upload_bytes[1] += draw->streams[i].bytes;
    }
    work->index_buffer = VK_NULL_HANDLE;
    if (!generic_guest_bind(draw->indices.data, draw->indices.bytes, draw->index_type == VK_INDEX_TYPE_UINT32 ? 4 : 2, false,
                            &work->index_buffer, &work->index_offset)) {
        bytes = generic_align(bytes, 16);
        work->index_offset = bytes;
        bytes += draw->indices.bytes;
        pokopia_generic_upload_bytes[2] += draw->indices.bytes;
    }
    const VkDevice device = adapter->device;
    /* Each slot reserves an immutable support uniform before its draw data. */
    const size_t arena_align = uniform_align > 256 ? uniform_align : 256;
    const size_t first = generic_align(SUPPORT_BUFFER_BYTES, arena_align);
    if (generic_shared.buffer && generic_shared.device == device &&
        (generic_batch.command || generic_batch.count) &&
        generic_align((size_t)generic_batch.arena_used, arena_align) + first + bytes >
            generic_shared.size / GENERIC_SLOTS)
        generic_batch_submit(false);
    if (generic_shared.buffer &&
        (generic_shared.device != device || first + bytes > generic_shared.size / GENERIC_SLOTS)) {
        /* Grow the shared buffer once nothing uses it. */
        generic_batch_submit(true);
        fn->DestroyBuffer(generic_shared.device, generic_shared.buffer, NULL);
        if (generic_shared.memory)
            pokopia_vk_ledger_free(fn->FreeMemory, generic_shared.device, generic_shared.memory);
        generic_shared = (__typeof__(generic_shared)){0};
    }
    const bool share = true;
    VkDeviceSize create_bytes = bytes;
    if (!generic_shared.buffer) {
        create_bytes = GENERIC_ARENA_BYTES;
        while (create_bytes / GENERIC_SLOTS < first + bytes)
            create_bytes <<= 1;
    }
    const VkDeviceSize half = (generic_shared.buffer ? generic_shared.size : create_bytes) / GENERIC_SLOTS;
    const VkDeviceSize base = generic_align((size_t)generic_batch.arena_used, arena_align);
    work->support_offset = half * generic_slot;
    const VkDeviceSize place = work->support_offset + first + base;
    for (uint32_t i = 0; i < draw->uniform_count; ++i)
        if (!work->uniform_buffers[i])
            work->uniform_offsets[i] += place;
    for (uint32_t i = 0; i < draw->binding_count; ++i)
        if (!work->stream_buffers[i])
            work->stream_offsets[i] += place;
    if (!work->index_buffer)
        work->index_offset += place;
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = create_bytes, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
    };
    VkResult result = VK_SUCCESS;
    if (generic_shared.buffer) {
        work->buffer = generic_shared.buffer;
        work->memory = generic_shared.memory;
        work->shared_buffer = true;
        generic_batch.arena_used = base + bytes;
        uint8_t *base = generic_shared.mapped;
        /* R74: no memset: every range a descriptor or binding reads is written below
         * (an empty uniform reads 16 bytes, zeroed here). */
        for (uint32_t i = 0; i < draw->uniform_count; ++i)
            if (!work->uniform_bytes[i])
                memset(base + work->uniform_offsets[i], 0, 16);
        for (uint32_t i = 0; i < draw->uniform_count; ++i)
            if (work->uniform_bytes[i] && !work->uniform_buffers[i])
                memcpy(base + work->uniform_offsets[i], draw->uniforms[i].data.data, work->uniform_bytes[i]);
        for (uint32_t i = 0; i < draw->binding_count; ++i)
            if (!work->stream_buffers[i])
                work->stream_offsets[i] = generic_place(base, work->stream_offsets[i], draw->streams[i].data,
                                                        draw->streams[i].bytes);
        if (!work->index_buffer)
            work->index_offset = generic_place(base, work->index_offset, draw->indices.data, draw->indices.bytes);
        if (!generic_shared.coherent) {
            const VkMappedMemoryRange range = {
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = generic_shared.memory, .size = VK_WHOLE_SIZE,
            };
            result = fn->FlushMappedMemoryRanges(device, 1, &range);
        }
        return result;
    }
    result = fn->CreateBuffer(device, &buffer_info, NULL, &work->buffer);
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
    if (result == VK_SUCCESS)
        result = fn->BindBufferMemory(device, work->buffer, work->memory, 0);
    void *mapped = NULL;
    if (result == VK_SUCCESS)
        result = fn->MapMemory(device, work->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (result != VK_SUCCESS)
        return result;
    if (share) {
        /* Keep it mapped and reuse it from the next draw on. */
        generic_shared = (__typeof__(generic_shared)){device, work->buffer, work->memory, mapped, create_bytes,
                                                       coherent, false};
        work->shared_buffer = true;
        generic_batch.arena_used = base + bytes;
    }
    uint8_t *out = mapped;
    const float unit_scale = 1.0f;
    for (unsigned slot = 0; slot < GENERIC_SLOTS; ++slot) {
        uint8_t *support = out + half * slot;
        memset(support, 0, SUPPORT_BUFFER_BYTES);
        for (unsigned i = 0; i < 73; ++i)
            memcpy(support + 0xc0 + i * 16, &unit_scale, sizeof unit_scale);
    }
    memset(out + place, 0, bytes);
    for (uint32_t i = 0; i < draw->uniform_count; ++i)
        if (work->uniform_bytes[i] && !work->uniform_buffers[i])
            memcpy(out + work->uniform_offsets[i], draw->uniforms[i].data.data, work->uniform_bytes[i]);
    for (uint32_t i = 0; i < draw->binding_count; ++i)
        if (draw->streams[i].bytes && !work->stream_buffers[i])
            memcpy(out + work->stream_offsets[i], draw->streams[i].data, draw->streams[i].bytes);
    if (draw->indices.bytes && !work->index_buffer)
        memcpy(out + work->index_offset, draw->indices.data, draw->indices.bytes);
    if (!coherent) {
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = work->memory, .size = VK_WHOLE_SIZE,
        };
        result = fn->FlushMappedMemoryRanges(device, 1, &range);
    }
    if (!share)
        fn->UnmapMemory(device, work->memory);
    return result;
}

typedef struct {
    VkImageView image;
    VkBufferView texel;
    NvnFrontSamplerInfo sampler;
    bool shadow;
} GenericTextureBinding;

/* Only reuse immutable sets from the same live pool generation. The pool
 * reset and all of its descriptor sets retire together at the batch fence. */
static struct {
    uint64_t batch;
    VkDescriptorPool pool;
    VkDescriptorSetLayout layout;
    VkDescriptorSet set;
    GenericTextureBinding bindings[POKOPIA_GENERIC_TEXTURES];
} generic_texture_set;

static bool generic_texture_set_matches(const PokopiaGenericDraw *draw, const GenericWork *work)
{
    if (generic_texture_set.batch != pokopia_vk_batch_id || generic_texture_set.pool != work->pool ||
        generic_texture_set.layout != work->layouts[2])
        return false;
    for (uint32_t i = 0; i < draw->texture_count; ++i) {
        const PokopiaGenericTexture *texture = &draw->textures[i];
        const GenericTextureBinding *binding = &generic_texture_set.bindings[i];
        if (texture->texel) {
            if (binding->texel != texture->texel->view)
                return false;
        } else if (binding->image != work->texture_views[i] || binding->shadow != texture->shadow ||
                   memcmp(&binding->sampler, &texture->sampler, sizeof binding->sampler)) {
            return false;
        }
    }
    return true;
}

static void generic_remember_texture_set(const PokopiaGenericDraw *draw, const GenericWork *work)
{
    generic_texture_set.batch = pokopia_vk_batch_id;
    generic_texture_set.pool = work->pool;
    generic_texture_set.layout = work->layouts[2];
    generic_texture_set.set = work->sets[1];
    for (uint32_t i = 0; i < draw->texture_count; ++i) {
        const PokopiaGenericTexture *texture = &draw->textures[i];
        GenericTextureBinding *binding = &generic_texture_set.bindings[i];
        if (texture->texel) {
            binding->texel = texture->texel->view;
        } else {
            binding->image = work->texture_views[i];
            binding->sampler = texture->sampler;
            binding->shadow = texture->shadow;
        }
    }
}

static VkResult generic_descriptors(VkDevice device, const DrawFunctions *fn, const PokopiaGenericDraw *draw,
                                    GenericWork *work)
{
    GenericLayoutKey layout_key;
    memset(&layout_key, 0, sizeof layout_key);
    layout_key.uniform_count = draw->uniform_count;
    layout_key.texture_count = draw->texture_count;
    for (uint32_t i = 0; i < draw->uniform_count; ++i)
        layout_key.uniforms[i] = draw->uniforms[i].binding;
    for (uint32_t i = 0; i < draw->texture_count; ++i)
        layout_key.textures[i] = draw->textures[i].binding | (draw->textures[i].texel ? 0x80000000u : 0);
    const size_t layout_slots = sizeof generic_layout_cache / sizeof generic_layout_cache[0];
    size_t layout_slot = (size_t)generic_hash(&layout_key, sizeof layout_key) & (layout_slots - 1);
    size_t probe = 0;
    for (; probe < layout_slots && generic_layout_cache[layout_slot].used &&
           memcmp(&generic_layout_cache[layout_slot].key, &layout_key, sizeof layout_key);
         ++probe, layout_slot = (layout_slot + 1) & (layout_slots - 1))
        ;
    VkResult result = VK_SUCCESS;
    if (probe < layout_slots && generic_layout_cache[layout_slot].used) {
        memcpy(work->layouts, generic_layout_cache[layout_slot].layouts, sizeof work->layouts);
        work->pipeline_layout = generic_layout_cache[layout_slot].pipeline_layout;
        work->layouts_cached = true;
    } else {
        const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding uniform_bindings[POKOPIA_GENERIC_UNIFORMS + 1];
        uniform_bindings[0] = (VkDescriptorSetLayoutBinding){GENERIC_SUPPORT_BINDING,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, NULL};
        for (uint32_t i = 0; i < draw->uniform_count; ++i)
            uniform_bindings[i + 1] = (VkDescriptorSetLayoutBinding){draw->uniforms[i].binding,
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, NULL};
        VkDescriptorSetLayoutBinding texture_bindings[POKOPIA_GENERIC_TEXTURES];
        for (uint32_t i = 0; i < draw->texture_count; ++i) {
            texture_bindings[i] = (VkDescriptorSetLayoutBinding){draw->textures[i].binding,
                draw->textures[i].texel ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                1, stages, NULL};
        }
        const VkDescriptorSetLayoutCreateInfo infos[3] = {
            {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
             .bindingCount = draw->uniform_count + 1, .pBindings = uniform_bindings},
            {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO},
            {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
             .bindingCount = draw->texture_count, .pBindings = texture_bindings},
        };
        for (uint32_t i = 0; i < 3; ++i) {
            result = fn->CreateDescriptorSetLayout(device, &infos[i], NULL, &work->layouts[i]);
            if (result != VK_SUCCESS)
                return result;
        }
        const VkPipelineLayoutCreateInfo layout_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3, .pSetLayouts = work->layouts,
        };
        result = fn->CreatePipelineLayout(device, &layout_info, NULL, &work->pipeline_layout);
        if (result != VK_SUCCESS)
            return result;
        if (probe < layout_slots) {
            generic_layout_cache[layout_slot].key = layout_key;
            memcpy(generic_layout_cache[layout_slot].layouts, work->layouts, sizeof work->layouts);
            generic_layout_cache[layout_slot].pipeline_layout = work->pipeline_layout;
            generic_layout_cache[layout_slot].used = true;
            work->layouts_cached = true;
        }
    }
    if (!generic_pool) {
        const VkDescriptorPoolSize shared_sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (POKOPIA_GENERIC_UNIFORMS + 1) * GENERIC_BATCH},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, POKOPIA_GENERIC_TEXTURES * GENERIC_BATCH},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, POKOPIA_GENERIC_TEXTURES * GENERIC_BATCH},
        };
        const VkDescriptorPoolCreateInfo shared_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2 * GENERIC_BATCH, .poolSizeCount = 3,
            .pPoolSizes = shared_sizes,
        };
        if (fn->CreateDescriptorPool(device, &shared_info, NULL, &generic_pool) != VK_SUCCESS)
            generic_pool = VK_NULL_HANDLE;
    }
    if (generic_pool) {
        work->pool = generic_pool; /* R75: one slot of a batch; reset per batch */
        work->pool_shared = true;
    } else {
        uint32_t texel_count = 0;
        for (uint32_t i = 0; i < draw->texture_count; ++i)
            texel_count += draw->textures[i].texel != NULL;
        const uint32_t image_count = draw->texture_count - texel_count;
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, draw->uniform_count + 1},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, image_count ? image_count : 1},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, texel_count ? texel_count : 1},
        };
        const VkDescriptorPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 3, .pPoolSizes = sizes,
        };
        result = fn->CreateDescriptorPool(device, &pool_info, NULL, &work->pool);
        if (result != VK_SUCCESS)
            return result;
    }
    const bool cache_textures = (pokopia_debug_flags & (1u << 24)) && draw->texture_count &&
        work->pool_shared && work->layouts_cached;
    const bool reuse_textures = cache_textures && generic_texture_set_matches(draw, work);
    const VkDescriptorSetLayout set_layouts[] = {work->layouts[0], work->layouts[2]};
    const VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = work->pool,
        .descriptorSetCount = draw->texture_count && !reuse_textures ? 2u : 1u, .pSetLayouts = set_layouts,
    };
    result = fn->AllocateDescriptorSets(device, &set_info, work->sets);
    if (result != VK_SUCCESS)
        return result;
    if (reuse_textures)
        work->sets[1] = generic_texture_set.set;

    VkDescriptorBufferInfo buffers[POKOPIA_GENERIC_UNIFORMS + 1];
    VkDescriptorImageInfo images[POKOPIA_GENERIC_TEXTURES];
    VkWriteDescriptorSet writes[POKOPIA_GENERIC_UNIFORMS + 1 + POKOPIA_GENERIC_TEXTURES];
    uint32_t count = 0;
    buffers[0] = (VkDescriptorBufferInfo){work->buffer, work->support_offset, SUPPORT_BUFFER_BYTES};
    for (uint32_t i = 0; i < draw->uniform_count; ++i) {
        const VkBuffer buffer = work->uniform_buffers[i] ? work->uniform_buffers[i] : work->buffer;
        buffers[i + 1] = (VkDescriptorBufferInfo){buffer, work->uniform_offsets[i],
                                                  work->uniform_bytes[i] ? work->uniform_bytes[i] : 16};
    }
    for (uint32_t i = 0; i <= draw->uniform_count; ++i)
        writes[count++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = work->sets[0],
            .dstBinding = i ? draw->uniforms[i - 1].binding : GENERIC_SUPPORT_BINDING, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &buffers[i],
        };
    for (uint32_t i = 0; !reuse_textures && i < draw->texture_count; ++i) {
        const PokopiaGenericTexture *t = &draw->textures[i];
        if (t->texel) {
            writes[count++] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = work->sets[1],
                .dstBinding = t->binding, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &t->texel->view,
            };
            continue;
        }
        VkSamplerCreateInfo sampler;
        if (!generic_sampler(&t->sampler, t->shadow, &sampler))
            GENERIC_REJECT("texture %u sampler cmp=%u reduction=%u min=%u", i, (unsigned)t->sampler.compare_mode,
                           (unsigned)t->sampler.reduction, (unsigned)t->sampler.min_filter);
        work->samplers[i] = generic_cached_sampler(device, fn, &sampler);
        if (!work->samplers[i])
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        work->samplers_cached = true;
        images[i] = (VkDescriptorImageInfo){work->samplers[i], work->texture_views[i],
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[count++] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = work->sets[1],
            .dstBinding = t->binding, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &images[i],
        };
    }
    fn->UpdateDescriptorSets(device, count, writes, 0, NULL);
    if (cache_textures && !reuse_textures)
        generic_remember_texture_set(draw, work);
    return VK_SUCCESS;
}

static void generic_key(const PokopiaGenericDraw *draw, GenericKey *key)
{
    memset(key, 0, sizeof *key); /* padding must compare equal */
    copy_text(key->vertex_hash, sizeof key->vertex_hash, draw->vertex_shader.hash);
    copy_text(key->fragment_hash, sizeof key->fragment_hash, draw->fragment_shader.hash);
    copy_text(key->vertex_entry, sizeof key->vertex_entry, draw->vertex_shader.entry);
    copy_text(key->fragment_entry, sizeof key->fragment_entry, draw->fragment_shader.entry);
    key->binding_count = draw->binding_count;
    key->attribute_count = draw->attribute_count;
    memcpy(key->bindings, draw->bindings, draw->binding_count * sizeof draw->bindings[0]);
    memcpy(key->attributes, draw->attributes, draw->attribute_count * sizeof draw->attributes[0]);
    key->uniform_count = draw->uniform_count;
    for (uint32_t i = 0; i < draw->uniform_count; ++i)
        key->uniform_bindings[i] = draw->uniforms[i].binding;
    key->texture_count = draw->texture_count;
    for (uint32_t i = 0; i < draw->texture_count; ++i)
        key->texture_bindings[i] = draw->textures[i].binding;
    key->topology = draw->topology;
    key->cull_mode = draw->cull_mode;
    key->front_face = draw->front_face;
    key->color_write_mask = draw->color_write_mask;
    key->blend_enable = draw->blend_enable;
    if (draw->blend_enable) {
        key->factors[0] = draw->src_rgb;
        key->factors[1] = draw->dst_rgb;
        key->factors[2] = draw->src_alpha;
        key->factors[3] = draw->dst_alpha;
        key->ops[0] = draw->rgb_op;
        key->ops[1] = draw->alpha_op;
    }
    key->has_target = draw->target != NULL;
    key->color_format = draw->target ? draw->target->format : VK_FORMAT_UNDEFINED;
    PokopiaVkTexture *colors[8] = {0};
    key->color_count = generic_colors(draw, colors);
    for (uint32_t i = 0; i < key->color_count; ++i) {
        key->color_formats[i] = colors[i] ? colors[i]->format : VK_FORMAT_UNDEFINED;
        key->color_masks[i] = colors[i] ? generic_color_mask(draw, i) : 0;
        key->blend_bits |= (colors[i] && generic_blend_on(draw, i)) ? 1u << i : 0u;
    }
    key->has_depth = draw->depth_target != NULL;
    key->depth_bias = draw->depth_bias;
    if (draw->depth_bias) {
        key->depth_bias_factor = draw->depth_bias_factor;
        key->depth_bias_units = draw->depth_bias_units;
    }
    key->depth_compare = VK_COMPARE_OP_ALWAYS;
    if (draw->depth_target) {
        key->depth_format = draw->depth_target->format;
        key->depth_test = draw->depth_test;
        key->depth_write = draw->depth_write;
        if (draw->depth_test)
            key->depth_compare = draw->depth_compare;
        key->stencil_test = draw->stencil_test;
        if (draw->stencil_test) {
            key->stencil[0] = draw->stencil[0];
            key->stencil[1] = draw->stencil[1];
        }
    }
}

static VkResult generic_pipeline(VkDevice device, const DrawFunctions *fn, const PokopiaGenericDraw *draw,
                                 GenericWork *work)
{
    if (generic_cache_device != device) {
        memset(generic_cache, 0, sizeof generic_cache);
        generic_cache_count = 0;
        generic_cache_device = device;
        generic_last_pipeline = NULL;
    }
    GenericKey key;
    generic_key(draw, &key);
    const bool cache = (pokopia_debug_flags & (1u << 22)) != 0;
    if (cache && generic_last_pipeline && !memcmp(&generic_last_pipeline->key, &key, sizeof key)) {
        work->pipeline = generic_last_pipeline->pipeline;
        work->pipeline_cached = true;
        return VK_SUCCESS;
    }
    uint64_t hash = generic_hash(&key, sizeof key);
    if (!hash)
        hash = 1; /* 0 marks an empty slot */
    size_t slot = (size_t)hash & (GENERIC_CACHE_SLOTS - 1);
    size_t probes = 0;
    for (; probes < GENERIC_CACHE_SLOTS && generic_cache[slot].hash; ++probes, slot = (slot + 1) & (GENERIC_CACHE_SLOTS - 1))
        if (generic_cache[slot].hash == hash && !memcmp(&generic_cache[slot].key, &key, sizeof key)) {
            if (cache)
                generic_last_pipeline = &generic_cache[slot];
            work->pipeline = generic_cache[slot].pipeline;
            work->pipeline_cached = true;
            return VK_SUCCESS;
        }
    ++pokopia_generic_pipeline_misses;
    const bool has_fragment = draw->target != NULL;
    const VkPipelineShaderStageCreateInfo shaders[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = draw->vertex_shader.module, .pName = draw->vertex_shader.entry},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = draw->fragment_shader.module, .pName = draw->fragment_shader.entry},
    };
    const VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = draw->binding_count, .pVertexBindingDescriptions = draw->bindings,
        .vertexAttributeDescriptionCount = draw->attribute_count, .pVertexAttributeDescriptions = draw->attributes,
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = draw->topology,
    };
    const VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1,
    };
    /* R75: viewport and scissor are set per draw in the command buffer. */
    static const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = draw->cull_mode, .frontFace = draw->front_face,
        .depthBiasEnable = draw->depth_bias, .depthBiasConstantFactor = draw->depth_bias ? draw->depth_bias_units : 0.0f,
        .depthBiasSlopeFactor = draw->depth_bias ? draw->depth_bias_factor : 0.0f,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState attachments[8];
    for (uint32_t i = 0; i < key.color_count; ++i)
        attachments[i] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = (key.blend_bits >> i) & 1u, .colorWriteMask = key.color_masks[i],
            .srcColorBlendFactor = key.factors[0], .dstColorBlendFactor = key.factors[1],
            .srcAlphaBlendFactor = key.factors[2], .dstAlphaBlendFactor = key.factors[3],
            .colorBlendOp = key.ops[0], .alphaBlendOp = key.ops[1],
        };
    const VkPipelineColorBlendStateCreateInfo color = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = has_fragment ? key.color_count : 0u, .pAttachments = has_fragment ? attachments : NULL,
    };
    const VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = key.depth_test, .depthWriteEnable = key.depth_write, .depthCompareOp = key.depth_compare,
        .stencilTestEnable = key.stencil_test, .front = key.stencil[0], .back = key.stencil[1],
    };
    const VkFormat depth_format = draw->depth_target ? draw->depth_target->format : VK_FORMAT_UNDEFINED;
    const bool has_stencil = draw->depth_target && (draw->depth_target->aspects & VK_IMAGE_ASPECT_STENCIL_BIT);
    const VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = has_fragment ? key.color_count : 0u,
        .pColorAttachmentFormats = has_fragment ? key.color_formats : NULL,
        .depthAttachmentFormat = depth_format,
        .stencilAttachmentFormat = has_stencil ? depth_format : VK_FORMAT_UNDEFINED,
    };
    const VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
        .stageCount = has_fragment ? 2u : 1u, .pStages = shaders,
        .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport, .pRasterizationState = &raster, .pMultisampleState = &samples,
        .pColorBlendState = &color, .layout = work->pipeline_layout,
        .pDepthStencilState = draw->depth_target ? &depth : NULL, .pDynamicState = &dynamic,
    };
    VkResult result = fn->CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &work->pipeline);
    if (result == VK_SUCCESS && probes < GENERIC_CACHE_SLOTS / 2 && generic_cache_count < GENERIC_CACHE_SLOTS * 3 / 4) {
        generic_cache[slot] = (GenericEntry){hash, key, work->pipeline};
        if (cache)
            generic_last_pipeline = &generic_cache[slot];
        ++generic_cache_count;
        work->pipeline_cached = true;
    }
    return result;
}

static bool generic_barrier_needed(const PokopiaVkTexture *texture, VkImageLayout layout,
                                   VkPipelineStageFlags stage, VkAccessFlags access)
{
    const VkAccessFlags writes = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT |
        VK_ACCESS_MEMORY_WRITE_BIT;
    return !(texture->layout == layout && !(texture->access & writes) && !(access & writes) &&
             (texture->stages & stage) == stage);
}

static void generic_barrier(const DrawFunctions *fn, VkCommandBuffer command, const PokopiaVkTexture *texture,
                            VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access)
{
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = texture->layout, .newLayout = layout,
        .srcAccessMask = texture->access, .dstAccessMask = access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->backing.image,
        .subresourceRange = {texture->aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
    };
    /* R75: read after read in the same layout needs no barrier (every sampled
     * texture of every draw had one). */
    const VkAccessFlags writes = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT |
        VK_ACCESS_MEMORY_WRITE_BIT;
    if (texture->layout == layout && !(texture->access & writes) && !(access & writes) &&
        (texture->stages & stage) == stage)
        return;
    fn->CmdPipelineBarrier(command, texture->stages ? texture->stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static VkResult generic_record(const DrawFunctions *fn, const PokopiaGenericDraw *draw, const GenericWork *work,
                               VkCommandBuffer command)
{
    (void)work->command;
    PokopiaVkTexture *targets[8] = {0};
    const uint32_t target_count = generic_colors(draw, targets);
    VkImageView target_views[8] = {0};
    for (uint32_t i = 0; i < target_count; ++i)
        target_views[i] = targets[i] ? (work->layer_views[i] ? work->layer_views[i] : targets[i]->view) : VK_NULL_HANDLE;
    const VkImageView depth_view = draw->depth_target ? draw->depth_target->view : VK_NULL_HANDLE;
    const bool stencil = draw->depth_target && (draw->depth_target->aspects & VK_IMAGE_ASPECT_STENCIL_BIT);
    /* R76: stay in the open rendering when the attachments match and no
     * sampled texture needs a transition, clear or barrier. */
    bool merge = generic_batch.rendering && generic_batch.rendering_colors == target_count &&
        !memcmp(generic_batch.rendering_views, target_views, sizeof target_views) &&
        generic_batch.rendering_depth == depth_view && generic_batch.rendering_stencil == stencil &&
        generic_batch.rendering_extent.width == draw->extent.width &&
        generic_batch.rendering_extent.height == draw->extent.height;
    for (uint32_t i = 0; merge && i < draw->texture_count; ++i) {
        const PokopiaVkTexture *texture = draw->textures[i].texture;
        if (draw->textures[i].texel)
            continue;
        merge = texture->layout != VK_IMAGE_LAYOUT_UNDEFINED && pokopia_exposure_fallback <= 0.0f &&
            !generic_barrier_needed(texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                    VK_ACCESS_SHADER_READ_BIT);
    }
    if (merge) {
        ++pokopia_generic_merged;
        goto draw_only;
    }
    generic_end_rendering();
    for (uint32_t i = 0; i < draw->texture_count; ++i) {
        const PokopiaVkTexture *texture = draw->textures[i].texture;
        if (draw->textures[i].texel)
            continue; /* R73: texel buffers are host-written; the submit makes them visible */
        bool seen = false;
        for (uint32_t j = 0; j < i && !seen; ++j)
            seen = draw->textures[j].texture == texture;
        if (seen)
            continue;
        PokopiaVkTexture state = *texture;
        /* R73: a tiny single-channel float storage image that no host GPU work
         * has written is the auto-exposure value, which the skipped compute
         * passes (histogram 62619a57, exposure 7dbc445a) would produce. It is
         * refilled with pokopia_exposure_fallback before every draw; zero made
         * the bloom and tonemap passes output black. */
        const bool exposure = pokopia_exposure_fallback > 0.0f && state.storage_view && /* debug override only */
            (state.format == VK_FORMAT_R32_SFLOAT || state.format == VK_FORMAT_R16_SFLOAT) &&
            state.extent.width * state.extent.height <= 4;
        if ((state.layout == VK_IMAGE_LAYOUT_UNDEFINED || exposure) && state.aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
            /* R62: nothing has written this texture yet (its producer, often a
             * compute pass, is still skipped). Give it a neutral value instead of
             * stale memory: 1.0 for single-channel float images (exposure,
             * luminance, occlusion), 0 for everything else. R61's final tonemap
             * pass read an unwritten R32_SFLOAT exposure texture. */
            generic_barrier(fn, command, &state, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            const bool unit = state.format == VK_FORMAT_R32_SFLOAT || state.format == VK_FORMAT_R16_SFLOAT;
            const float fill = exposure ? pokopia_exposure_fallback : unit ? 1.0f : 0.0f;
            const VkClearColorValue value = {.float32 = {fill, fill, fill, fill}};
            const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS,
                                                   0, VK_REMAINING_ARRAY_LAYERS};
            fn->CmdClearColorImage(command, state.backing.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   &value, 1, &range);
            state.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            state.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
            state.access = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        generic_barrier(fn, command, &state, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT);
    }
    VkRenderingAttachmentInfo color[8] = {0}, depth = {0};
    PokopiaVkTexture *colors[8] = {0};
    const uint32_t color_count = generic_colors(draw, colors);
    for (uint32_t i = 0; i < color_count; ++i) {
        color[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!colors[i])
            continue; /* unused slot: VK_NULL_HANDLE view */
        bool seen = false;
        for (uint32_t j = 0; j < i && !seen; ++j)
            seen = colors[j] == colors[i];
        if (!seen)
            generic_barrier(fn, command, colors[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        color[i] = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = work->layer_views[i] ? work->layer_views[i] : colors[i]->view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = colors[i]->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE :
                                                                      VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
    }
    const bool has_stencil = draw->depth_target && (draw->depth_target->aspects & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (draw->depth_target) {
        generic_barrier(fn, command, draw->depth_target, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        depth_stages, depth_access);
        depth = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = draw->depth_target->view,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .loadOp = draw->depth_target->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_DONT_CARE :
                                                                               VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
    }
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, draw->extent}, .layerCount = 1,
        .colorAttachmentCount = color_count, .pColorAttachments = color_count ? color : NULL,
        .pDepthAttachment = draw->depth_target ? &depth : NULL, .pStencilAttachment = has_stencil ? &depth : NULL,
    };
    fn->CmdBeginRendering(command, &rendering);
    generic_batch.rendering = true;
    generic_batch.rendering_colors = target_count;
    memcpy(generic_batch.rendering_views, target_views, sizeof target_views);
    generic_batch.rendering_depth = depth_view;
    generic_batch.rendering_stencil = stencil;
    generic_batch.rendering_extent = draw->extent;
draw_only:;
    /* R90: bind only what differs from the previous draw in this buffer. */
    __typeof__(generic_batch.bound) *bound = &generic_batch.bound;
    if (command != generic_batch.command || (pokopia_debug_flags & 1))
        bound->valid = false;
    if (!bound->valid || bound->pipeline != work->pipeline)
        fn->CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, work->pipeline);
    else
        ++pokopia_generic_binds_skipped;
    if (!bound->valid || memcmp(&bound->viewport, &draw->viewport, sizeof draw->viewport))
        fn->CmdSetViewport(command, 0, 1, &draw->viewport);
    if (!bound->valid || memcmp(&bound->scissor, &draw->scissor, sizeof draw->scissor))
        fn->CmdSetScissor(command, 0, 1, &draw->scissor);
    const bool same_layout = bound->valid && bound->layout == work->pipeline_layout;
    if (!same_layout || bound->sets[0] != work->sets[0])
        fn->CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, work->pipeline_layout, 0, 1,
                                  &work->sets[0], 0, NULL);
    if (draw->texture_count && (!same_layout || bound->sets[1] != work->sets[1]))
        fn->CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, work->pipeline_layout, 2, 1,
                                  &work->sets[1], 0, NULL);
    if (command == generic_batch.command && !(pokopia_debug_flags & 1)) {
        *bound = (__typeof__(*bound)){true, work->pipeline, work->pipeline_layout,
                                      {work->sets[0], draw->texture_count ? work->sets[1] : VK_NULL_HANDLE},
                                      draw->viewport, draw->scissor};
    }
    for (uint32_t i = 0; i < draw->binding_count; ++i)
        fn->CmdBindVertexBuffers(command, draw->bindings[i].binding, 1,
                                 work->stream_buffers[i] ? &work->stream_buffers[i] : &work->buffer,
                                 &work->stream_offsets[i]);
    if (draw->index_count) {
        fn->CmdBindIndexBuffer(command, work->index_buffer ? work->index_buffer : work->buffer, work->index_offset,
                               draw->index_type);
        fn->CmdDrawIndexed(command, draw->index_count, draw->instance_count ? draw->instance_count : 1,
                           draw->first_index, draw->base_vertex, draw->first_instance);
    } else {
        fn->CmdDraw(command, draw->vertex_count, draw->instance_count ? draw->instance_count : 1,
                    draw->first_vertex, draw->first_instance);
    }
    return VK_SUCCESS; /* R76: rendering stays open; generic_end_rendering() closes it */
}

VkResult pokopia_live_draw_generic(PokopiaVkAdapter *adapter, const PokopiaGenericDraw *draw,
                                   uint64_t *completed_render_serial)
{
    if (!adapter || !adapter->device || !adapter->queue || !adapter->get_device_proc ||
        !completed_render_serial || adapter->render_serial == UINT64_MAX)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!draw || !draw->extent.width || !draw->extent.height || (!draw->target && !draw->depth_target) ||
        !draw->vertex_shader.module || !draw->vertex_shader.entry ||
        (draw->target && (!draw->fragment_shader.module || !draw->fragment_shader.entry ||
                          !draw->target->backing.image || !draw->target->view ||
                          draw->target->aspects != VK_IMAGE_ASPECT_COLOR_BIT)) ||
        (draw->depth_target && (!draw->depth_target->backing.image || !draw->depth_target->view)) ||
        draw->uniform_count > POKOPIA_GENERIC_UNIFORMS || draw->texture_count > POKOPIA_GENERIC_TEXTURES ||
        draw->binding_count > POKOPIA_GENERIC_STREAMS || draw->attribute_count > POKOPIA_GENERIC_ATTRIBUTES)
        GENERIC_REJECT("draw state (target=%d depth=%d)", draw->target ? (int)draw->target->format : -1,
                       draw->depth_target ? (int)draw->depth_target->format : -1);
    pokopia_generic_reject[0] = 0;
    PokopiaVkTexture *colors[8] = {0};
    const uint32_t color_count = generic_colors(draw, colors);
    for (uint32_t i = 1; i < color_count; ++i)
        if (colors[i] && (!colors[i]->backing.image || !colors[i]->view ||
                          colors[i]->aspects != VK_IMAGE_ASPECT_COLOR_BIT ||
                          ((colors[i]->extent.width >> draw->color_levels[i]) ?
                               colors[i]->extent.width >> draw->color_levels[i] : 1) < draw->extent.width ||
                          ((colors[i]->extent.height >> draw->color_levels[i]) ?
                               colors[i]->extent.height >> draw->color_levels[i] : 1) < draw->extent.height))
            GENERIC_REJECT("MRT color%u unusable", i);
    for (uint32_t i = 0; i < draw->texture_count; ++i) {
        const PokopiaGenericTexture *t = &draw->textures[i];
        if (t->texel) {
            if (!t->texel->view)
                GENERIC_REJECT("texture %u texel buffer has no view", i);
            continue;
        }
        bool is_target = t->texture == draw->depth_target;
        for (uint32_t j = 0; j < color_count; ++j)
            is_target |= colors[j] && t->texture == colors[j];
        if (!t->texture || !t->texture->backing.image)
            GENERIC_REJECT("texture %u has no image", i);
        if (is_target && !t->guest_info)
            GENERIC_REJECT("texture %u is also the render target (%ux%u fmt=%d layers=%u)", i,
                           t->texture->extent.width, t->texture->extent.height, (int)t->texture->format,
                           t->texture->array_layers);
        if (!sampled_view(t->texture, t->view_format))
            GENERIC_REJECT("texture %u no view for format %d (texture %d)", i, (int)t->view_format,
                           (int)t->texture->format);
        if (adapter->format_properties) {
            const VkFormat format = t->view_format == VK_FORMAT_UNDEFINED ? t->texture->format : t->view_format;
            if (!generic_format_sampled(adapter, format, (pokopia_debug_flags & (1u << 22)) != 0))
                GENERIC_REJECT("texture %u format %d not sampleable", i, (int)format);
        }
    }

    const unsigned timing_weight = (pokopia_debug_flags & POKOPIA_DEBUG_SAMPLE_DRAWS) ? 16u : 1u;
    const bool timed = timing_weight == 1 || pokopia_timing_sample(pokopia_generic_draw_calls);
    const uint64_t start = timed ? draw_now_ns() : 0;
    const VkDevice device = adapter->device;
    /* R76: looked up once per device; ~40 vkGetDeviceProcAddr string lookups
     * per draw were ~3% of the main thread. */
    if (!generic_functions_load(adapter))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const DrawFunctions fn = generic_functions;

    if (generic_batch.count >= GENERIC_BATCH)
        generic_batch_flush();

    /* R66: a draw that samples its own colour or depth target samples a copy
     * taken just before the draw. */
    PokopiaGenericDraw patched;
    bool patching = false;
    for (uint32_t i = 0; i < draw->texture_count; ++i) {
        PokopiaVkTexture *texture = draw->textures[i].texture;
        if (draw->textures[i].texel)
            continue;
        bool is_target = texture == draw->depth_target;
        for (uint32_t j = 0; j < color_count; ++j)
            is_target |= colors[j] && texture == colors[j];
        if (!is_target || (patching && patched.textures[i].texture != texture))
            continue;
        if (!patching) {
            patched = *draw;
            patching = true;
        }
        PokopiaVkTexture *copy = NULL;
        const VkResult opened = generic_batch_open(adapter, &fn);
        generic_end_rendering();
        const VkResult copied = opened != VK_SUCCESS ? opened :
            pokopia_vk_texture_snapshot_into(adapter, texture, &draw->textures[i].guest_info->create_info.config,
                                             &copy, &generic_batch.command);
        if (copied != VK_SUCCESS) {
            snprintf(pokopia_generic_reject, sizeof pokopia_generic_reject,
                     "texture %u feedback copy failed result=%d (%ux%u fmt=%d)", i, (int)copied,
                     texture->extent.width, texture->extent.height, (int)texture->format);
            return copied == VK_ERROR_DEVICE_LOST ? copied : VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        for (uint32_t j = i; j < draw->texture_count; ++j)
            if (draw->textures[j].texture == texture)
                patched.textures[j].texture = copy;
    }
    if (patching)
        draw = &patched;
    uint64_t phase_start = timed ? draw_now_ns() : 0;
#define GENERIC_PHASE(i) do { if (timed) { const uint64_t now_ = draw_now_ns(); \
    pokopia_generic_phase_ns[i] += (now_ - phase_start) * timing_weight; phase_start = now_; } } while (0)
    GENERIC_PHASE(0);

    GenericWork work = {0};
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < draw->texture_count && result == VK_SUCCESS; ++i) {
        const PokopiaGenericTexture *t = &draw->textures[i];
        if (t->texel)
            continue;
        const bool swizzled = t->swizzle[0] | t->swizzle[1] | t->swizzle[2] | t->swizzle[3];
        static const uint8_t identity[4] = {2, 3, 4, 5}; /* NVN TextureSwizzle R, G, B, A */
        work.texture_views[i] = t->as_array ?
            pokopia_vk_texture_typed_view(adapter, t->texture, t->view_format, swizzled ? t->swizzle : identity,
                                          VK_IMAGE_VIEW_TYPE_2D_ARRAY) :
            swizzled ? pokopia_vk_texture_swizzle_view(adapter, t->texture, t->view_format, t->swizzle) :
            sampled_view(t->texture, t->view_format);
        if (!work.texture_views[i]) {
            snprintf(pokopia_generic_reject, sizeof pokopia_generic_reject,
                     "texture %u swizzled view failed fmt=%d swz=%u%u%u%u", i, (int)t->texture->format,
                     t->swizzle[0], t->swizzle[1], t->swizzle[2], t->swizzle[3]);
            result = VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
    }
    if (result != VK_SUCCESS)
        return result;
    result = generic_upload(adapter, &fn, draw, &work);
    GENERIC_PHASE(1);
    if (result == VK_SUCCESS)
        result = generic_descriptors(device, &fn, draw, &work);
    GENERIC_PHASE(2);
    if (result == VK_SUCCESS)
        result = generic_pipeline(device, &fn, draw, &work);
    GENERIC_PHASE(3);
    if (result == VK_SUCCESS)
        result = generic_layer_views(device, &fn, draw, &work);
    if (result == VK_SUCCESS)
        result = generic_batch_open(adapter, &fn);
    if (result == VK_SUCCESS)
        result = generic_record(&fn, draw, &work, generic_batch.command);
    GENERIC_PHASE(4);
    /* R75: the work's objects (and its descriptor sets in the shared pool) stay
     * until the batch retires. */
    if (!generic_batch.command)
        (void)generic_batch_open(adapter, &fn); /* a failed draw may hold pool sets */
    if (generic_batch.command && generic_batch.count < GENERIC_BATCH) {
        ++generic_batch.count;
        /* Shared buffers and pools and cached objects are kept alive by the
         * slot or their cache. Only per-draw allocations need a retirement
         * record. Preserve cleanup for partially initialized draws. */
        if ((pokopia_debug_flags & 32) || result != VK_SUCCESS || work.command ||
            !work.shared_buffer || !work.pool_shared || !work.layouts_cached || !work.pipeline_cached) {
            GenericSlot *slot = &generic_slots[generic_slot];
            slot->works[slot->retire_count++] = work;
        }
        for (uint32_t i = 0; i < draw->texture_count; ++i)
            if (draw->textures[i].texel)
                draw->textures[i].texel->batch_stamp = pokopia_vk_batch_id;
    }
    else
        generic_release(adapter, &fn, &work);
    GENERIC_PHASE(6);
#undef GENERIC_PHASE
    if (result == VK_SUCCESS) {
        for (uint32_t i = 0; i < draw->texture_count; ++i) {
            PokopiaVkTexture *texture = draw->textures[i].texture;
            if (draw->textures[i].texel)
                continue;
            texture->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            texture->stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            texture->access = VK_ACCESS_SHADER_READ_BIT;
        }
        if (draw->depth_target) {
            draw->depth_target->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            draw->depth_target->stages = depth_stages;
            draw->depth_target->access = depth_access;
        }
        const uint64_t serial = ++adapter->render_serial;
        for (uint32_t i = 0; i < color_count; ++i) {
            if (!colors[i])
                continue;
            colors[i]->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colors[i]->stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            colors[i]->access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            colors[i]->render_serial = serial;
        }
        if (draw->depth_target)
            draw->depth_target->render_serial = serial;
        *completed_render_serial = serial;
    }
    ++pokopia_generic_draw_calls;
    if (timed)
        pokopia_generic_draw_ns += (draw_now_ns() - start) * timing_weight;
    return result;
}

/* R78: a whole-image colour clear recorded into the draw batch (each was its
 * own submit and wait: ~0.8 ms, 3 a frame). Level 0, layer 0, like
 * pokopia_vk_clear_color. */
VkResult pokopia_live_generic_clear_color(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture, const float rgba[4])
{
    if (!adapter || !texture || !texture->backing.image || texture->aspects != VK_IMAGE_ASPECT_COLOR_BIT ||
        adapter->render_serial == UINT64_MAX || !generic_functions_load(adapter))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const DrawFunctions *fn = &generic_functions;
    VkResult result = generic_batch_open(adapter, fn);
    if (result != VK_SUCCESS)
        return result;
    generic_end_rendering();
    const VkCommandBuffer command = generic_batch.command;
    const VkImageMemoryBarrier target = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = texture->access, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = texture->layout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->backing.image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    fn->CmdPipelineBarrier(command, texture->stages ? texture->stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &target);
    VkClearColorValue value;
    memcpy(value.float32, rgba, sizeof value.float32);
    fn->CmdClearColorImage(command, texture->backing.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                           &target.subresourceRange);
    texture->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    texture->stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    texture->access = VK_ACCESS_TRANSFER_WRITE_BIT;
    texture->render_serial = ++adapter->render_serial;
    return VK_SUCCESS;
}

/* R78: a whole-image depth/stencil clear recorded into the draw batch (a
 * submit and wait each, ~1 ms once the queue holds batched work). */
VkResult pokopia_live_generic_clear_depth(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                         VkImageAspectFlags aspects, VkClearDepthStencilValue value)
{
    if (!adapter || !texture || !texture->backing.image || !aspects || (aspects & ~texture->aspects) ||
        (texture->aspects & VK_IMAGE_ASPECT_COLOR_BIT) || adapter->render_serial == UINT64_MAX ||
        !generic_functions_load(adapter))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const DrawFunctions *fn = &generic_functions;
    const PFN_vkCmdClearDepthStencilImage clear =
        (PFN_vkCmdClearDepthStencilImage)adapter->get_device_proc(adapter->device, "vkCmdClearDepthStencilImage");
    if (!clear)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult result = generic_batch_open(adapter, fn);
    if (result != VK_SUCCESS)
        return result;
    generic_end_rendering();
    const VkCommandBuffer command = generic_batch.command;
    const VkImageSubresourceRange range = {texture->aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    const VkImageMemoryBarrier target = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = texture->access, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = texture->layout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->backing.image, .subresourceRange = range,
    };
    fn->CmdPipelineBarrier(command, texture->stages ? texture->stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &target);
    const VkImageSubresourceRange cleared = {aspects, 0, 1, 0, 1};
    clear(command, texture->backing.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &cleared);
    texture->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    texture->stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    texture->access = VK_ACCESS_TRANSFER_WRITE_BIT;
    texture->render_serial = ++adapter->render_serial;
    return VK_SUCCESS;
}

/* R73: general compute. Any translated compute shader, described by its own
 * resource bindings (Ryujinx's Vulkan scheme): set 0 uniform buffers with the
 * support buffer at binding 0, set 1 storage buffers, set 2 sampled textures and
 * texel buffers, set 3 storage images. Storage buffers are copied from guest
 * memory before the dispatch and back after it; storage images stay on the GPU
 * and are left in GENERAL layout. One dispatch is submitted and retired at a
 * time, like general draws. */
uint64_t pokopia_generic_compute_calls, pokopia_generic_compute_ns;

VkResult pokopia_live_compute_generic(PokopiaVkAdapter *adapter, const PokopiaGenericCompute *c)
{
    if (!adapter || !adapter->device || !adapter->queue || !c || !c->shader.module || !c->shader.entry ||
        c->uniform_count > POKOPIA_COMPUTE_UNIFORMS || c->storage_count > POKOPIA_COMPUTE_STORAGE ||
        c->texture_count > POKOPIA_COMPUTE_TEXTURES || c->image_count > POKOPIA_COMPUTE_IMAGES)
        GENERIC_REJECT("compute state");
    const VkPhysicalDeviceLimits *limits = &adapter->properties.limits;
    for (unsigned i = 0; i < 3; ++i)
        if (!c->groups[i] || c->groups[i] > limits->maxComputeWorkGroupCount[i])
            GENERIC_REJECT("compute groups %u", c->groups[i]);
    for (uint32_t i = 0; i < c->texture_count; ++i) {
        const PokopiaGenericTexture *t = &c->textures[i];
        if (t->texel ? !t->texel->view : (!t->texture || !t->texture->backing.image ||
                                          !sampled_view(t->texture, t->view_format)))
            GENERIC_REJECT("compute texture %u unusable", i);
    }
    for (uint32_t i = 0; i < c->image_count; ++i)
        if (!c->images[i].texture || !c->images[i].texture->backing.image || !c->images[i].texture->storage_view)
            GENERIC_REJECT("compute image %u has no storage view", i);

    const uint64_t start = draw_now_ns();
    const VkDevice device = adapter->device;
    /* R76: looked up once per device; ~40 vkGetDeviceProcAddr string lookups
     * per draw were ~3% of the main thread. */
    if (!generic_functions_load(adapter))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const DrawFunctions fn = generic_functions;
    const PFN_vkCreateComputePipelines CreateComputePipelines =
        (PFN_vkCreateComputePipelines)adapter->get_device_proc(device, "vkCreateComputePipelines");
    const PFN_vkCmdDispatch CmdDispatch = (PFN_vkCmdDispatch)adapter->get_device_proc(device, "vkCmdDispatch");
    if (!CreateComputePipelines || !CmdDispatch)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    /* Arena: support buffer, uniforms, storage buffers. */
    enum { RANGES = 1 + POKOPIA_COMPUTE_UNIFORMS + POKOPIA_COMPUTE_STORAGE };
    VkDeviceSize offsets[RANGES], sizes[RANGES], bytes = 0;
    uint32_t range_count = 0;
    sizes[range_count++] = SUPPORT_BUFFER_BYTES;
    for (uint32_t i = 0; i < c->uniform_count; ++i)
        sizes[range_count++] = c->uniforms[i].data.bytes > limits->maxUniformBufferRange ?
            limits->maxUniformBufferRange : (c->uniforms[i].data.bytes ? c->uniforms[i].data.bytes : 16);
    for (uint32_t i = 0; i < c->storage_count; ++i)
        sizes[range_count++] = c->storage[i].bytes ? c->storage[i].bytes : 16;
    for (uint32_t i = 0; i < range_count; ++i) {
        const VkDeviceSize align = i <= c->uniform_count ? limits->minUniformBufferOffsetAlignment :
                                                           limits->minStorageBufferOffsetAlignment;
        bytes = generic_align(bytes, align ? align : 256);
        offsets[i] = bytes;
        bytes += sizes[i];
    }

    /* R78: a dispatch without storage buffers writes only images, so nothing is
     * copied back to guest memory: record it into the draw batch with its
     * uniforms in the batch arena and its sets in the slot's compute pool
     * (each dispatch was a submit and wait, ~1 ms). */
    enum { COMPUTE_PER_BATCH = 64 };
    /* R78: storage buffers in the guest heap are bound where they are (the GPU
     * writes guest memory directly; nothing is copied back), so such
     * dispatches go into the batch too. */
    VkBuffer direct_buffers[POKOPIA_COMPUTE_STORAGE];
    VkDeviceSize direct_offsets[POKOPIA_COMPUTE_STORAGE];
    bool storage_direct = c->storage_count > 0;
    const size_t storage_align = limits->minStorageBufferOffsetAlignment ? (size_t)limits->minStorageBufferOffsetAlignment : 16;
    for (uint32_t i = 0; i < c->storage_count && storage_direct; ++i)
        storage_direct = c->storage[i].bytes && c->storage[i].bytes <= limits->maxStorageBufferRange &&
                         generic_guest_bind(c->storage[i].data, c->storage[i].bytes, storage_align, false, &direct_buffers[i],
                                            &direct_offsets[i]);
    bool slot_resources = c->storage_count == 0 || storage_direct;
    if (slot_resources) {
        const VkDeviceSize half = generic_shared.size / GENERIC_SLOTS;
        const size_t uniform_align = (size_t)limits->minUniformBufferOffsetAlignment;
        const size_t arena_align = uniform_align > 256 ? uniform_align : 256;
        const VkDeviceSize first = generic_align(SUPPORT_BUFFER_BYTES, arena_align);
        const VkDeviceSize at = generic_align((size_t)generic_batch.arena_used, arena_align);
        if (!generic_shared.buffer || generic_shared.device != device || first + at + bytes > half ||
            generic_slots[generic_slot].compute_count >= COMPUTE_PER_BATCH || !generic_sync_procs(adapter)) {
            slot_resources = false;
        } else {
            for (uint32_t i = 0; i < range_count; ++i)
                offsets[i] += half * generic_slot + first + at;
            generic_batch.arena_used = at + bytes;
        }
    }
    const unsigned compute_slot = generic_slot;
    bool in_batch = false;

    VkResult result;
    VkBuffer arena = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t *mapped = NULL;
    VkDescriptorSetLayout layouts[4] = {0};
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[4] = {0};
    VkSampler samplers[POKOPIA_COMPUTE_TEXTURES] = {0};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    bool coherent = false, retired = true;

    /* R78: one arena kept across dispatches (each dispatch completes before
     * the next), and pipelines and layouts cached per shader and bindings;
     * every dispatch created and destroyed them (~1 ms each). */
    static struct {
        VkDevice device;
        VkBuffer buffer;
        VkDeviceMemory memory;
        uint8_t *mapped;
        VkDeviceSize size;
    } compute_arena;
    if (compute_arena.buffer && (compute_arena.device != device || compute_arena.size < bytes)) {
        fn.UnmapMemory(compute_arena.device, compute_arena.memory);
        fn.DestroyBuffer(compute_arena.device, compute_arena.buffer, NULL);
        pokopia_vk_ledger_free(fn.FreeMemory, compute_arena.device, compute_arena.memory);
        memset(&compute_arena, 0, sizeof compute_arena);
    }
    VkDeviceSize arena_bytes = bytes;
    if (!compute_arena.buffer) {
        arena_bytes = 1u << 20;
        while (arena_bytes < bytes)
            arena_bytes <<= 1;
    }
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = arena_bytes,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    };
    if (slot_resources) {
        arena = generic_shared.buffer;
        memory = generic_shared.memory;
        mapped = generic_shared.mapped;
        result = VK_SUCCESS;
    } else if (compute_arena.buffer) {
        arena = compute_arena.buffer;
        memory = compute_arena.memory;
        mapped = compute_arena.mapped;
        result = VK_SUCCESS;
    } else {
        result = fn.CreateBuffer(device, &buffer_info, NULL, &arena);
    }
    if (result == VK_SUCCESS && !mapped) {
        VkMemoryRequirements needs;
        fn.GetBufferMemoryRequirements(device, arena, &needs);
        const uint32_t type = memory_type(adapter, needs.memoryTypeBits, &coherent);
        const VkMemoryAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = needs.size, .memoryTypeIndex = type,
        };
        result = type == UINT32_MAX || !coherent ? VK_ERROR_FEATURE_NOT_PRESENT :
            pokopia_vk_ledger_allocate(fn.AllocateMemory, device, &alloc, &memory, VK_LEDGER_COMPUTE);
    }
    if (result == VK_SUCCESS && !slot_resources && !compute_arena.buffer) {
        result = fn.BindBufferMemory(device, arena, memory, 0);
        if (result == VK_SUCCESS)
            result = fn.MapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped);
        if (result == VK_SUCCESS)
            compute_arena = (__typeof__(compute_arena)){device, arena, memory, mapped, arena_bytes};
    }
    if (result == VK_SUCCESS) {
        uint8_t *support = mapped + offsets[0];
        memset(support, 0, SUPPORT_BUFFER_BYTES);
        const float unit_scale = 1.0f;
        for (unsigned i = 0; i < 73; ++i)
            memcpy(support + 0xc0 + i * 16, &unit_scale, sizeof unit_scale);
        for (uint32_t i = 0; i < c->uniform_count; ++i) {
            memset(mapped + offsets[1 + i], 0, sizes[1 + i]);
            if (c->uniforms[i].data.data)
                memcpy(mapped + offsets[1 + i], c->uniforms[i].data.data,
                       c->uniforms[i].data.bytes < sizes[1 + i] ? c->uniforms[i].data.bytes : sizes[1 + i]);
        }
        for (uint32_t i = 0; i < c->storage_count && !storage_direct; ++i)
            if (c->storage[i].bytes)
                memcpy(mapped + offsets[1 + c->uniform_count + i], c->storage[i].data, c->storage[i].bytes);
    }

    /* Descriptor layouts. */
    const VkShaderStageFlags stage = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding uniform_bindings[1 + POKOPIA_COMPUTE_UNIFORMS], storage_bindings[POKOPIA_COMPUTE_STORAGE],
        texture_bindings[POKOPIA_COMPUTE_TEXTURES], image_bindings[POKOPIA_COMPUTE_IMAGES];
    uniform_bindings[0] = (VkDescriptorSetLayoutBinding){GENERIC_SUPPORT_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stage, NULL};
    for (uint32_t i = 0; i < c->uniform_count; ++i)
        uniform_bindings[1 + i] = (VkDescriptorSetLayoutBinding){c->uniforms[i].binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stage, NULL};
    for (uint32_t i = 0; i < c->storage_count; ++i)
        storage_bindings[i] = (VkDescriptorSetLayoutBinding){c->storage[i].binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stage, NULL};
    uint32_t texel_count = 0;
    for (uint32_t i = 0; i < c->texture_count; ++i) {
        texel_count += c->textures[i].texel != NULL;
        texture_bindings[i] = (VkDescriptorSetLayoutBinding){c->textures[i].binding,
            c->textures[i].texel ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            1, stage, NULL};
    }
    for (uint32_t i = 0; i < c->image_count; ++i)
        image_bindings[i] = (VkDescriptorSetLayoutBinding){c->images[i].binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stage, NULL};
    typedef struct {
        VkShaderModule module;
        uint32_t counts[4];
        uint32_t bindings[1 + POKOPIA_COMPUTE_UNIFORMS + POKOPIA_COMPUTE_STORAGE + POKOPIA_COMPUTE_TEXTURES +
                          POKOPIA_COMPUTE_IMAGES];
    } ComputeKey;
    static struct {
        ComputeKey key;
        VkDescriptorSetLayout layouts[4];
        VkPipelineLayout pipeline_layout;
        VkPipeline pipeline;
    } compute_cache[128];
    static unsigned compute_cache_count;
    static VkDevice compute_cache_device;
    if (compute_cache_device != device) {
        compute_cache_count = 0;
        compute_cache_device = device;
    }
    ComputeKey compute_key;
    memset(&compute_key, 0, sizeof compute_key);
    compute_key.module = c->shader.module;
    compute_key.counts[0] = c->uniform_count;
    compute_key.counts[1] = c->storage_count;
    compute_key.counts[2] = c->texture_count;
    compute_key.counts[3] = c->image_count;
    {
        uint32_t k = 0;
        for (uint32_t i = 0; i < c->uniform_count; ++i)
            compute_key.bindings[k++] = c->uniforms[i].binding;
        for (uint32_t i = 0; i < c->storage_count; ++i)
            compute_key.bindings[k++] = c->storage[i].binding;
        for (uint32_t i = 0; i < c->texture_count; ++i)
            compute_key.bindings[k++] = c->textures[i].binding | (c->textures[i].texel ? 0x80000000u : 0);
        for (uint32_t i = 0; i < c->image_count; ++i)
            compute_key.bindings[k++] = c->images[i].binding;
    }
    int compute_hit = -1;
    for (unsigned i = 0; i < compute_cache_count && compute_hit < 0; ++i)
        if (!memcmp(&compute_cache[i].key, &compute_key, sizeof compute_key))
            compute_hit = (int)i;
    if (compute_hit >= 0) {
        memcpy(layouts, compute_cache[compute_hit].layouts, sizeof layouts);
        pipeline_layout = compute_cache[compute_hit].pipeline_layout;
        pipeline = compute_cache[compute_hit].pipeline;
    }
    const VkDescriptorSetLayoutCreateInfo layout_infos[4] = {
        {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1 + c->uniform_count, .pBindings = uniform_bindings},
        {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = c->storage_count, .pBindings = storage_bindings},
        {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = c->texture_count, .pBindings = texture_bindings},
        {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = c->image_count, .pBindings = image_bindings},
    };
    for (unsigned i = 0; i < 4 && result == VK_SUCCESS && compute_hit < 0; ++i)
        result = fn.CreateDescriptorSetLayout(device, &layout_infos[i], NULL, &layouts[i]);
    if (result == VK_SUCCESS && compute_hit < 0) {
        const VkPipelineLayoutCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 4, .pSetLayouts = layouts,
        };
        result = fn.CreatePipelineLayout(device, &info, NULL, &pipeline_layout);
    }
    if (result == VK_SUCCESS) {
        const uint32_t images_sampled = c->texture_count - texel_count;
        const VkDescriptorPoolSize pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 + c->uniform_count},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, c->storage_count ? c->storage_count : 1},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images_sampled ? images_sampled : 1},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, texel_count ? texel_count : 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, c->image_count ? c->image_count : 1},
        };
        const VkDescriptorPoolCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4, .poolSizeCount = 5, .pPoolSizes = pool_sizes,
        };
        GenericSlot *slot = &generic_slots[compute_slot];
        if (slot_resources && !slot->compute_pool) {
            const VkDescriptorPoolSize slot_sizes[] = {
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (1 + POKOPIA_COMPUTE_UNIFORMS) * COMPUTE_PER_BATCH},
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, COMPUTE_PER_BATCH},
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, POKOPIA_COMPUTE_TEXTURES * COMPUTE_PER_BATCH},
                {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, POKOPIA_COMPUTE_TEXTURES * COMPUTE_PER_BATCH},
                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, POKOPIA_COMPUTE_IMAGES * COMPUTE_PER_BATCH},
            };
            const VkDescriptorPoolCreateInfo slot_info = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4 * COMPUTE_PER_BATCH,
                .poolSizeCount = 5, .pPoolSizes = slot_sizes,
            };
            if (fn.CreateDescriptorPool(device, &slot_info, NULL, &slot->compute_pool) != VK_SUCCESS)
                slot->compute_pool = VK_NULL_HANDLE;
        }
        if (slot_resources && slot->compute_pool)
            pool = slot->compute_pool;
        else if (slot_resources)
            result = VK_ERROR_OUT_OF_POOL_MEMORY;
        else
            result = fn.CreateDescriptorPool(device, &info, NULL, &pool);
    }
    if (result == VK_SUCCESS) {
        const VkDescriptorSetAllocateInfo info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
            .descriptorSetCount = 4, .pSetLayouts = layouts,
        };
        result = fn.AllocateDescriptorSets(device, &info, sets);
    }
    if (result == VK_SUCCESS) {
        VkDescriptorBufferInfo buffers[RANGES];
        VkDescriptorImageInfo images[POKOPIA_COMPUTE_TEXTURES + POKOPIA_COMPUTE_IMAGES];
        VkWriteDescriptorSet writes[RANGES + POKOPIA_COMPUTE_TEXTURES + POKOPIA_COMPUTE_IMAGES];
        uint32_t count = 0;
        for (uint32_t i = 0; i < range_count; ++i)
            buffers[i] = (VkDescriptorBufferInfo){arena, offsets[i], sizes[i]};
        for (uint32_t i = 0; i < c->storage_count && storage_direct; ++i)
            buffers[1 + c->uniform_count + i] = (VkDescriptorBufferInfo){direct_buffers[i], direct_offsets[i],
                                                                        c->storage[i].bytes};
        for (uint32_t i = 0; i <= c->uniform_count; ++i)
            writes[count++] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0], .dstBinding = uniform_bindings[i].binding,
                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &buffers[i],
            };
        for (uint32_t i = 0; i < c->storage_count; ++i)
            writes[count++] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[1], .dstBinding = c->storage[i].binding,
                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &buffers[1 + c->uniform_count + i],
            };
        for (uint32_t i = 0; i < c->texture_count && result == VK_SUCCESS; ++i) {
            const PokopiaGenericTexture *t = &c->textures[i];
            if (t->texel) {
                writes[count++] = (VkWriteDescriptorSet){
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[2], .dstBinding = t->binding,
                    .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                    .pTexelBufferView = &t->texel->view,
                };
                continue;
            }
            VkSamplerCreateInfo sampler;
            if (!generic_sampler(&t->sampler, t->shadow, &sampler)) {
                snprintf(pokopia_generic_reject, sizeof pokopia_generic_reject, "compute texture %u sampler", i);
                result = VK_ERROR_FORMAT_NOT_SUPPORTED;
                break;
            }
            if (slot_resources) {
                samplers[i] = generic_cached_sampler(device, &fn, &sampler);
                result = samplers[i] ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
            } else {
                result = fn.CreateSampler(device, &sampler, NULL, &samplers[i]);
            }
            bool written = false;
            for (uint32_t j = 0; j < c->image_count; ++j)
                written |= c->images[j].texture == t->texture;
            images[i] = (VkDescriptorImageInfo){samplers[i], sampled_view(t->texture, t->view_format),
                written ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[count++] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[2], .dstBinding = t->binding,
                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &images[i],
            };
        }
        for (uint32_t i = 0; i < c->image_count; ++i) {
            images[POKOPIA_COMPUTE_TEXTURES + i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, c->images[i].texture->storage_view,
                                                                         VK_IMAGE_LAYOUT_GENERAL};
            writes[count++] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[3], .dstBinding = c->images[i].binding,
                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &images[POKOPIA_COMPUTE_TEXTURES + i],
            };
        }
        if (result == VK_SUCCESS)
            fn.UpdateDescriptorSets(device, count, writes, 0, NULL);
    }
    if (result == VK_SUCCESS && compute_hit < 0) {
        const VkComputePipelineCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = c->shader.module, .pName = c->shader.entry},
            .layout = pipeline_layout,
        };
        result = CreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline);
        if (result == VK_SUCCESS && compute_cache_count < sizeof compute_cache / sizeof compute_cache[0]) {
            compute_hit = (int)compute_cache_count++;
            compute_cache[compute_hit].key = compute_key;
            memcpy(compute_cache[compute_hit].layouts, layouts, sizeof layouts);
            compute_cache[compute_hit].pipeline_layout = pipeline_layout;
            compute_cache[compute_hit].pipeline = pipeline;
        }
    }
    if (result == VK_SUCCESS && slot_resources && compute_hit >= 0 && generic_slot == compute_slot &&
        generic_batch_open(adapter, &fn) == VK_SUCCESS && generic_slot == compute_slot) {
        generic_end_rendering();
        command = generic_batch.command;
        in_batch = true;
        ++generic_slots[compute_slot].compute_count;
        ++pokopia_generic_compute_batched;
    } else if (result == VK_SUCCESS) {
        result = pokopia_vk_command_buffer_open(adapter, &command);
        if (result == VK_SUCCESS) {
            const VkCommandBufferBeginInfo begin = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            result = fn.BeginCommandBuffer(command, &begin);
        }
    }
    if (result == VK_SUCCESS) {
        const VkPipelineStageFlags compute_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        /* Images first (GENERAL), then sampled textures not also written. */
        for (uint32_t i = 0; i < c->image_count; ++i) {
            PokopiaVkTexture *texture = c->images[i].texture;
            bool seen = false;
            for (uint32_t j = 0; j < i; ++j)
                seen |= c->images[j].texture == texture;
            if (!seen)
                generic_barrier(&fn, command, texture, VK_IMAGE_LAYOUT_GENERAL, compute_stage,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        for (uint32_t i = 0; i < c->texture_count; ++i) {
            PokopiaVkTexture *texture = c->textures[i].texture;
            if (c->textures[i].texel)
                continue;
            bool seen = false;
            for (uint32_t j = 0; j < c->image_count; ++j)
                seen |= c->images[j].texture == texture;
            for (uint32_t j = 0; j < i; ++j)
                seen |= c->textures[j].texture == texture;
            if (!seen)
                generic_barrier(&fn, command, texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, compute_stage,
                                VK_ACCESS_SHADER_READ_BIT);
        }
        const VkMemoryBarrier host = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        fn.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, compute_stage, 0, 1, &host, 0, NULL, 0, NULL);
        fn.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        fn.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 4, sets, 0, NULL);
        CmdDispatch(command, c->groups[0], c->groups[1], c->groups[2]);
        const VkMemoryBarrier back = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        fn.CmdPipelineBarrier(command, compute_stage, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &back, 0, NULL, 0, NULL);
        if (!in_batch)
            result = fn.EndCommandBuffer(command);
    }
    if (result == VK_SUCCESS && !in_batch)
        result = pokopia_vk_submit_wait(adapter, command, &retired);
    if (result == VK_SUCCESS) {
        for (uint32_t i = 0; i < c->storage_count; ++i)
            if (c->storage[i].bytes)
                if (!storage_direct)
                    memcpy(c->storage[i].data, mapped + offsets[1 + c->uniform_count + i], c->storage[i].bytes);
        if (storage_direct)
            ++pokopia_vk_guest_write_epoch; /* the GPU writes these ranges */
        const uint64_t serial = ++adapter->render_serial;
        for (uint32_t i = 0; i < c->texture_count; ++i) {
            PokopiaVkTexture *texture = c->textures[i].texture;
            if (c->textures[i].texel)
                continue;
            texture->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            texture->stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            texture->access = VK_ACCESS_SHADER_READ_BIT;
        }
        for (uint32_t i = 0; i < c->image_count; ++i) {
            PokopiaVkTexture *texture = c->images[i].texture;
            texture->layout = VK_IMAGE_LAYOUT_GENERAL;
            texture->stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            texture->access = VK_ACCESS_SHADER_WRITE_BIT;
            texture->render_serial = serial;
        }
    }
    if (retired) {
        if (command && !in_batch)
            pokopia_vk_command_buffer_close(adapter, command);
        for (uint32_t i = 0; i < c->texture_count && !slot_resources; ++i)
            if (samplers[i])
                fn.DestroySampler(device, samplers[i], NULL);
        if (pool && !slot_resources)
            fn.DestroyDescriptorPool(device, pool, NULL);
        if (compute_hit < 0) {
            if (pipeline)
                fn.DestroyPipeline(device, pipeline, NULL);
            if (pipeline_layout)
                fn.DestroyPipelineLayout(device, pipeline_layout, NULL);
            for (unsigned i = 0; i < 4; ++i)
                if (layouts[i])
                    fn.DestroyDescriptorSetLayout(device, layouts[i], NULL);
        }
        if (!compute_arena.buffer && !slot_resources) {
            if (mapped)
                fn.UnmapMemory(device, memory);
            if (arena)
                fn.DestroyBuffer(device, arena, NULL);
            if (memory)
                pokopia_vk_ledger_free(fn.FreeMemory, device, memory);
        }
    }
    ++pokopia_generic_compute_calls;
    pokopia_generic_compute_ns += draw_now_ns() - start;
    return result;
}
