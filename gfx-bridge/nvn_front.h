/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_NVN_FRONT_H
#define POKOPIA_NVN_FRONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "shader_cache.h"

#define NVN_FRONT_DEFAULT_THUNK_BASE UINT64_C(0x7f00000000)
#define NVN_FRONT_SDK_BOOTSTRAP UINT64_C(0x7500634240)
#define NVN_FRONT_SDK_BOOTSTRAP_INTERNAL UINT64_C(0x75006342a0)

typedef struct NvnFront NvnFront;

typedef enum {
    NVN_FRONT_WINDOW_ORIGIN_LOWER_LEFT = 0,
    NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT = 1,
} NvnFrontWindowOrigin;

typedef enum {
    NVN_FRONT_DEPTH_NEAR_IS_MINUS_W = 0,
    NVN_FRONT_DEPTH_NEAR_IS_ZERO = 1,
} NvnFrontDepthMode;

typedef struct {
    NvnFrontWindowOrigin window_origin;
    NvnFrontDepthMode depth_mode;
} NvnFrontDeviceModes;

enum {
    NVN_FRONT_QUEUE_COMMAND_MEMORY = 1u << 0,
    NVN_FRONT_QUEUE_COMPUTE_MEMORY = 1u << 1,
    NVN_FRONT_QUEUE_CONTROL_MEMORY = 1u << 2,
    NVN_FRONT_QUEUE_RAY_MEMORY = 1u << 3,
    NVN_FRONT_QUEUE_FLUSH_THRESHOLD = 1u << 4,
};

typedef struct {
    void *primary_queue;
    uint64_t queue_memory;
    uint32_t flags, overrides;
    uint32_t command_bytes, compute_bytes, control_bytes, ray_bytes;
    uint32_t queue_memory_bytes, flush_threshold, priority;
} NvnFrontQueueConfig;

typedef struct {
    uint64_t device;
    void *native;
} NvnFrontQueueInfo;

typedef struct {
    uint64_t device, storage, size;
    uint32_t flags;
} NvnFrontPoolConfig;

typedef struct {
    uint64_t object, offset;
    NvnFrontPoolConfig config;
} NvnFrontPoolHit;

typedef struct {
    uint64_t device, pool, offset, packaged_data;
    uint32_t flags, target, width, height, depth, levels, format, samples;
    uint32_t stride, swizzle[4], depth_stencil_mode, gl_texture_name;
    uint16_t packaged_layout;
} NvnFrontTextureConfig;

typedef struct {
    NvnFrontTextureConfig config;
    uint64_t storage_address, storage_size, storage_alignment;
} NvnFrontTextureCreateInfo;

typedef struct {
    NvnFrontTextureCreateInfo create_info;
    void *native;
} NvnFrontTextureInfo;

typedef struct {
    uint64_t device, memory_pool, offset, storage;
    uint32_t count;
} NvnFrontDescriptorPoolInfo;

typedef struct {
    uint64_t address;
    NvnFrontTextureInfo texture;
    uint8_t view[16];
    bool has_view, image;
} NvnFrontTextureDescriptorInfo;

typedef struct {
    uint32_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_anisotropy, lod_snap;
    uint32_t border_color_bits[4];
} NvnFrontSamplerInfo;

enum { NVN_FRONT_MAX_VERTEX_ATTRIBS = 32, NVN_FRONT_MAX_VERTEX_STREAMS = 32 };

typedef struct {
    uint32_t format, offset, stream;
    bool enabled;
} NvnFrontVertexAttribInfo;

typedef struct {
    uint32_t stride, divisor;
} NvnFrontVertexStreamInfo;

typedef struct {
    uint32_t blend_mask, logic_op, alpha_func;
} NvnFrontColorInfo;

typedef struct {
    uint32_t cull_face, front_face, mode, offset_enables;
} NvnFrontPolygonInfo;

typedef struct {
    uint32_t func, fail, depth_fail, depth_pass;
} NvnFrontStencilInfo;

typedef struct {
    bool depth_test, depth_write, stencil_test;
    uint32_t depth_func;
    NvnFrontStencilInfo front, back;
} NvnFrontDepthStencilInfo;

typedef struct {
    uint32_t target, src_rgb, dst_rgb, src_alpha, dst_alpha, rgb_equation, alpha_equation, advanced_mode;
} NvnFrontBlendInfo;

typedef struct {
    uint32_t samples, raster_samples;
    bool enabled, alpha_to_coverage, alpha_to_coverage_dither;
} NvnFrontMultisampleInfo;

typedef enum {
    NVN_FRONT_TEXTURE_UNSUPPORTED,
    NVN_FRONT_TEXTURE_CREATED,
    NVN_FRONT_TEXTURE_FAILED,
} NvnFrontTextureCreateResult;

typedef struct {
    void *native;
    uint64_t device, memory_callback, memory_callback_data;
    uint64_t command_pool, command_offset, command_address, command_size, command_revision;
    uint64_t control_address, control_size, control_revision;
    uint64_t command_used, control_used;
    uint32_t recording_mode;
} NvnFrontCommandBufferInfo;

typedef struct {
    uint64_t device;
    void *native;
    bool linked, has_unavailable_shader;
} NvnFrontProgramInfo;

typedef struct {
    uint64_t code_address, control_address;
    const void *code, *control;
    NvnShaderControl parsed;
} NvnFrontShaderData;

typedef enum {
    NVN_FRONT_PROGRAM_UNSUPPORTED,
    NVN_FRONT_PROGRAM_LINKED,
    NVN_FRONT_PROGRAM_FAILED,
    NVN_FRONT_PROGRAM_CACHE_MISS,
} NvnFrontProgramLinkResult;

typedef struct {
    void *guest_opaque, *backend_opaque;
    /* The caller reserves this dispatch-only address range. Zero selects the default. */
    uint64_t thunk_base;
    const void *(*read_guest)(void *opaque, uint64_t address, size_t size);
    void *(*write_guest)(void *opaque, uint64_t address, size_t size);
    /* A nonnull object means the backend successfully initialized it. */
    void *(*device_create)(void *opaque, uint32_t flags);
    void (*device_destroy)(void *opaque, void *device);
    void *(*queue_create)(void *opaque, void *device, const NvnFrontQueueConfig *config);
    void (*queue_destroy)(void *opaque, void *queue);
    bool (*get_integer)(void *opaque, void *device_or_null, uint32_t query, int32_t *value);
    /* Storage fields describe the validated guest pool. A created texture needs a
     * nonnull native handle. The frontend owns any returned handle. */
    NvnFrontTextureCreateResult (*texture_create)(void *opaque, void *device,
        const NvnFrontTextureCreateInfo *info, void **texture_out);
    void (*texture_destroy)(void *opaque, void *texture);
    /* Creates an unsignaled native sync. NULL reports an allocation failure. */
    void *(*sync_create)(void *opaque, void *device);
    void (*sync_destroy)(void *opaque, void *sync);
    /* Allocates an initial, unrecorded native command buffer. */
    void *(*command_buffer_create)(void *opaque, void *device);
    void (*command_buffer_destroy)(void *opaque, void *command_buffer);
    void *(*program_create)(void *opaque, void *device);
    void (*program_destroy)(void *opaque, void *program);
    /* Shader pointers are borrowed for the call. LINKED replaces stages, FAILED
     * clears them, and UNSUPPORTED preserves the previous program. */
    NvnFrontProgramLinkResult (*program_link)(void *opaque, void *program,
        const NvnFrontShaderData *shaders, size_t count);
} NvnFrontConfig;

typedef enum {
    NVN_FRONT_NOT_OURS,
    NVN_FRONT_HANDLED,
    NVN_FRONT_BOUNDARY,
    NVN_FRONT_INVALID_GUEST,
} NvnFrontReplyKind;

typedef struct {
    NvnFrontReplyKind kind;
    char name[96];
    const char *key; /* R78: the proc table's static name (NULL when none) */
} NvnFrontReply;

/* Caller serializes dispatch. Guest access callbacks must validate the whole
 * range and keep the returned host pointer live for the duration of the call. */
NvnFront *nvn_front_create(const NvnFrontConfig *config);
NvnFrontReply nvn_front_dispatch(NvnFront *front, uint64_t pc, uint64_t x[32]);
/* Float arguments are the low 32 bits of guest V0 and V1. */
NvnFrontReply nvn_front_dispatch_with_floats(NvnFront *front, uint64_t pc, uint64_t x[32], const uint32_t scalar[2]);
/* Lock-free prefilter: false means dispatch would certainly return NOT_OURS.
 * Reads only configuration fixed at init. */
bool nvn_front_may_own(const NvnFront *front, uint64_t pc);
/* R60: true for builder/state entry points that touch only the guest structure
 * passed in, which the loader may dispatch without its global lock. */
bool nvn_front_is_pure_call(const NvnFront *front, uint64_t pc);
/* R79: the static name of the NVN entry point at pc, or NULL. */
const char *nvn_front_call_name(const NvnFront *front, uint64_t pc);
/* R79: calls that only read front objects (or write a descriptor slot the
 * game owns), safe beside the replay worker under the recording lock. */
bool nvn_front_is_shared_call(const NvnFront *front, uint64_t pc);
/* R80: set by the loader while a call runs without the graphics lock. */
extern _Thread_local bool nvn_front_shared;
/* Replay reads the current modes of a live guest device under the dispatch lock. */
bool nvn_front_get_device_modes(NvnFront *front, uint64_t device_address, NvnFrontDeviceModes *modes);
/* The native handle is borrowed while the caller holds the dispatch lock. */
bool nvn_front_get_queue(NvnFront *front, uint64_t address, NvnFrontQueueInfo *info);
/* Read the guest's pool request under the dispatch lock. Storage is a guest address. */
bool nvn_front_read_pool_builder(const NvnFront *front, uint64_t builder_address, NvnFrontPoolConfig *config);
/* Read live pool state under the dispatch lock. Ordinary pool GPU tokens use the guest storage base. */
bool nvn_front_get_pool(NvnFront *front, uint64_t pool_address, NvnFrontPoolConfig *config);
/* Resolve a readable range in exactly one GPU-accessible pool on the given device. */
bool nvn_front_resolve_pool(const NvnFront *front, uint64_t device_address, uint64_t buffer_address,
    size_t size, NvnFrontPoolHit *hit);
/* Read the verified NVN2 builder fields for layout queries and texture creation. */
bool nvn_front_read_texture_builder(const NvnFront *front, uint64_t builder_address,
    NvnFrontTextureConfig *config);
/* The native handle is borrowed while the caller holds the dispatch lock. */
bool nvn_front_get_texture(NvnFront *front, uint64_t address, NvnFrontTextureInfo *info);
/* Read a live guest descriptor pool under the dispatch lock. */
bool nvn_front_get_descriptor_pool(NvnFront *front, uint64_t address, bool sampler, NvnFrontDescriptorPoolInfo *info);
/* Replay borrows the registered native texture under the dispatch lock. */
bool nvn_front_get_texture_descriptor(NvnFront *front, uint64_t pool, uint32_t index,
    NvnFrontTextureDescriptorInfo *info);
bool nvn_front_get_sampler_descriptor(NvnFront *front, uint64_t pool, uint32_t index, NvnFrontSamplerInfo *info);
/* Snapshot consecutive NVN2 state objects for command recording. Stream stride
 * is the stored SDK value. The NVN2 bind packet uses its low 12 bits. */
bool nvn_front_read_vertex_attribs(const NvnFront *front, uint64_t address, uint32_t count,
    NvnFrontVertexAttribInfo *out);
bool nvn_front_read_vertex_streams(const NvnFront *front, uint64_t address, uint32_t count,
    NvnFrontVertexStreamInfo *out);
/* Snapshot packed SDK state under the dispatch lock. Channel masks are four
 * RGBA bits per color attachment, starting from attachment zero. */
bool nvn_front_read_color(const NvnFront *front, uint64_t address, NvnFrontColorInfo *out);
bool nvn_front_read_channel_masks(const NvnFront *front, uint64_t address, uint32_t *out);
bool nvn_front_read_polygon(const NvnFront *front, uint64_t address, NvnFrontPolygonInfo *out);
bool nvn_front_read_depth_stencil(const NvnFront *front, uint64_t address, NvnFrontDepthStencilInfo *out);
bool nvn_front_read_blend(const NvnFront *front, uint64_t address, NvnFrontBlendInfo *out);
bool nvn_front_read_multisample(const NvnFront *front, uint64_t address, NvnFrontMultisampleInfo *out);
/* The native handle is borrowed while the caller holds the dispatch lock. */
bool nvn_front_get_sync(NvnFront *front, uint64_t address, void **native);
/* Current guest memory bindings and borrowed native command buffer under the dispatch lock. */
bool nvn_front_get_command_buffer(NvnFront *front, uint64_t address, NvnFrontCommandBufferInfo *info);
/* Charge bytes emitted by the replay recorder. Both counters and guest
 * cursors stay unchanged if either span cannot accommodate the charge. */
bool nvn_front_charge_command_memory(NvnFront *front, uint64_t encoder,
    size_t command_bytes, size_t control_bytes);
/* The native handle is borrowed while the caller holds the dispatch lock. */
bool nvn_front_get_program(NvnFront *front, uint64_t address, NvnFrontProgramInfo *info);
void nvn_front_destroy(NvnFront *front);

#ifdef __cplusplus
}
#endif
#endif
