/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nvn_front.h"
#include "nvn_guest_layout.h"

#include <stdlib.h>
#include <string.h>

enum {
    BOOTSTRAP = 0x634240, BOOTSTRAP_INTERNAL = 0x6342a0,
    INSTALL_GLOBAL_DEBUG_CALLBACK = 0x5d63a0,
    DEVICE_BUILDER_GET_FLAGS = 0x5d64e0, DEVICE_BUILDER_DEFAULTS = 0x5d64f0,
    DEVICE_BUILDER_SET_FLAGS = 0x5d6500, DEVICE_FINALIZE = 0x5d6510,
    DEVICE_GET_INTEGER = 0x5d6840, DEVICE_GET_PROC = 0x5d6880,
    DEVICE_INITIALIZE = 0x5d6900, DEVICE_GET_PROC_INTERNAL = 0x5dbd00,
    DEVICE_SET_DEPTH_MODE = 0x5d69f0, DEVICE_SET_WINDOW_ORIGIN = 0x5d6a10,
    DEVICE_GET_IMAGE_HANDLE = 0x5d6830, DEVICE_GET_SEPARATE_SAMPLER_HANDLE = 0x5d6890,
    DEVICE_GET_SEPARATE_TEXTURE_HANDLE = 0x5d68a0, DEVICE_GET_TEXEL_FETCH_HANDLE = 0x5d68b0,
    DEVICE_GET_TEXTURE_HANDLE = 0x5d68c0,
    POOL_BUILDER_SET_DEVICE = 0x5e2350, POOL_BUILDER_DEFAULTS = 0x5e2360,
    POOL_BUILDER_SET_STORAGE = 0x5e2380, POOL_BUILDER_SET_FLAGS = 0x5e2450,
    POOL_BUILDER_GET_DEVICE = 0x5e2460, POOL_BUILDER_GET_MEMORY = 0x5e2470,
    POOL_BUILDER_GET_SIZE = 0x5e2480, POOL_BUILDER_GET_FLAGS = 0x5e2490,
    POOL_INITIALIZE = 0x5e24a0, POOL_FINALIZE = 0x5e2680, POOL_MAP = 0x5e2740,
    POOL_FLUSH = 0x5e2760, POOL_INVALIDATE = 0x5e2770,
    POOL_GET_BUFFER_ADDRESS = 0x5e2780, POOL_GET_SIZE = 0x5e27f0, POOL_GET_FLAGS = 0x5e2800,
    BUFFER_BUILDER_SET_DEVICE = 0x5c34d0, BUFFER_BUILDER_DEFAULTS = 0x5c34e0,
    BUFFER_BUILDER_SET_STORAGE = 0x5c34f0, BUFFER_BUILDER_GET_DEVICE = 0x5c3500,
    BUFFER_BUILDER_GET_POOL = 0x5c3510, BUFFER_BUILDER_GET_OFFSET = 0x5c3520,
    BUFFER_BUILDER_GET_SIZE = 0x5c3530, BUFFER_INITIALIZE = 0x5c3540,
    BUFFER_FINALIZE = 0x5c3590, BUFFER_MAP = 0x5c35d0, BUFFER_GET_ADDRESS = 0x5c3600,
    BUFFER_FLUSH = 0x5c3610, BUFFER_INVALIDATE = 0x5c3620,
    BUFFER_GET_POOL = 0x5c3630, BUFFER_GET_OFFSET = 0x5c3640, BUFFER_GET_SIZE = 0x5c3650,
    PROGRAM_INITIALIZE = 0x5e4a70, PROGRAM_FINALIZE = 0x5e4b10, PROGRAM_SET_SHADERS = 0x5e4e90,
    TEXTURE_BUILDER_SET_DEVICE = 0x5ee150, TEXTURE_BUILDER_DEFAULTS = 0x5ee160,
    TEXTURE_BUILDER_SET_FLAGS = 0x5ee1a0, TEXTURE_BUILDER_SET_TARGET = 0x5ee1b0,
    TEXTURE_BUILDER_SET_WIDTH = 0x5ee1c0, TEXTURE_BUILDER_SET_HEIGHT = 0x5ee1d0,
    TEXTURE_BUILDER_SET_DEPTH = 0x5ee1e0, TEXTURE_BUILDER_SET_SIZE_1D = 0x5ee1f0,
    TEXTURE_BUILDER_SET_SIZE_2D = 0x5ee200, TEXTURE_BUILDER_SET_SIZE_3D = 0x5ee210,
    TEXTURE_BUILDER_SET_LEVELS = 0x5ee220, TEXTURE_BUILDER_SET_FORMAT = 0x5ee230,
    TEXTURE_BUILDER_SET_SAMPLES = 0x5ee240, TEXTURE_BUILDER_SET_SWIZZLE = 0x5ee250,
    TEXTURE_BUILDER_SET_DEPTH_STENCIL = 0x5ee260, TEXTURE_BUILDER_SET_STORAGE = 0x5ee270,
    TEXTURE_BUILDER_SET_GL_NAME = 0x5ee280, TEXTURE_BUILDER_SET_STRIDE = 0x5ee290,
    TEXTURE_BUILDER_GET_WIDTH = 0x5ee2d0, TEXTURE_BUILDER_GET_HEIGHT = 0x5ee2e0,
    TEXTURE_BUILDER_GET_ALIGNMENT = 0x5ee410, TEXTURE_BUILDER_GET_SIZE = 0x5ee7d0,
    TEXTURE_BUILDER_SET_PACKAGED_DATA = 0x5ee990, TEXTURE_BUILDER_SET_PACKAGED_LAYOUT = 0x5eea00,
    TEXTURE_INITIALIZE = 0x5ef720, TEXTURE_FINALIZE = 0x5efe70,
    TEXTURE_GET_FLAGS = 0x5f0060, TEXTURE_GET_TARGET = 0x5f0080, TEXTURE_GET_WIDTH = 0x5f00b0,
    TEXTURE_GET_HEIGHT = 0x5f00c0, TEXTURE_GET_DEPTH = 0x5f00d0, TEXTURE_GET_LEVELS = 0x5f00e0,
    TEXTURE_GET_FORMAT = 0x5f0100, TEXTURE_GET_SAMPLES = 0x5f0130, TEXTURE_GET_STORAGE_SIZE = 0x5f0e70,
    TEXTURE_POOL_INITIALIZE = 0x5f01e0, TEXTURE_POOL_FINALIZE = 0x5f0270,
    TEXTURE_POOL_REGISTER_TEXTURE = 0x5f0280, TEXTURE_POOL_REGISTER_IMAGE = 0x5f0410,
    TEXTURE_POOL_GET_MEMORY = 0x5f05a0, TEXTURE_POOL_GET_OFFSET = 0x5f05b0, TEXTURE_POOL_GET_SIZE = 0x5f05c0,
    SAMPLER_POOL_INITIALIZE = 0x5ebed0, SAMPLER_POOL_FINALIZE = 0x5ebfe0,
    SAMPLER_POOL_REGISTER = 0x5ebfd0, SAMPLER_POOL_REGISTER_BUILDER = 0x5ebff0,
    SAMPLER_POOL_GET_MEMORY = 0x5ec0e0, SAMPLER_POOL_GET_OFFSET = 0x5ec0f0, SAMPLER_POOL_GET_SIZE = 0x5ec100,
    SAMPLER_BUILDER_SET_DEVICE = 0x5eba50, SAMPLER_BUILDER_DEFAULTS = 0x5eba60,
    SAMPLER_BUILDER_SET_FILTER = 0x5eba90, SAMPLER_BUILDER_SET_WRAP = 0x5ebab0,
    SAMPLER_BUILDER_SET_LOD_CLAMP = 0x5ebad0, SAMPLER_BUILDER_SET_LOD_BIAS = 0x5ebae0,
    SAMPLER_BUILDER_SET_COMPARE = 0x5ebaf0, SAMPLER_BUILDER_SET_BORDER = 0x5ebb10,
    SAMPLER_BUILDER_SET_BORDER_I = 0x5ebb40, SAMPLER_BUILDER_SET_BORDER_UI = 0x5ebb70,
    SAMPLER_BUILDER_SET_ANISOTROPY = 0x5ebba0, SAMPLER_BUILDER_SET_REDUCTION = 0x5ebbb0,
    SAMPLER_BUILDER_SET_LOD_SNAP = 0x5ebbc0, SAMPLER_INITIALIZE = 0x5ebd40,
    SAMPLER_FINALIZE = 0x5ebd70,
    VERTEX_ATTRIB_DEFAULTS = 0x5ec500, VERTEX_ATTRIB_SET_FORMAT = 0x5ec510,
    VERTEX_ATTRIB_SET_STREAM = 0x5ec530, VERTEX_ATTRIB_GET_FORMAT = 0x5ec550,
    VERTEX_ATTRIB_GET_STREAM = 0x5ec570, VERTEX_STREAM_DEFAULTS = 0x5ec580,
    VERTEX_STREAM_SET_STRIDE = 0x5ec590, VERTEX_STREAM_SET_DIVISOR = 0x5ec5a0,
    VERTEX_STREAM_GET_STRIDE = 0x5ec5b0, VERTEX_STREAM_GET_DIVISOR = 0x5ec5c0,
    BLEND_DEFAULTS = 0x5ec1d0, BLEND_SET_TARGET = 0x5ec1f0, BLEND_SET_FUNC = 0x5ec210,
    BLEND_SET_EQUATION = 0x5ec250, BLEND_GET_TARGET = 0x5ec300, BLEND_GET_FUNC = 0x5ec310,
    BLEND_GET_EQUATION = 0x5ec350,
    MASK_DEFAULTS = 0x5ec3b0, MASK_SET = 0x5ec3c0, MASK_GET = 0x5ec410,
    COLOR_DEFAULTS = 0x5ec440, COLOR_SET_BLEND = 0x5ec450, COLOR_SET_LOGIC = 0x5ec490,
    COLOR_SET_ALPHA = 0x5ec4b0, COLOR_GET_BLEND = 0x5ec4d0, COLOR_GET_LOGIC = 0x5ec4e0,
    COLOR_GET_ALPHA = 0x5ec4f0,
    DEPTH_STENCIL_DEFAULTS = 0x5ec5d0, DEPTH_SET_TEST = 0x5ec5f0, DEPTH_SET_WRITE = 0x5ec610,
    DEPTH_SET_FUNC = 0x5ec630, STENCIL_SET_TEST = 0x5ec650, STENCIL_SET_FUNC = 0x5ec670,
    STENCIL_SET_OP = 0x5ec6b0, DEPTH_GET_TEST = 0x5ec700, DEPTH_GET_WRITE = 0x5ec710,
    DEPTH_GET_FUNC = 0x5ec720, STENCIL_GET_TEST = 0x5ec730, STENCIL_GET_FUNC = 0x5ec740,
    STENCIL_GET_OP = 0x5ec760,
    POLYGON_DEFAULTS = 0x5ec7b0, POLYGON_SET_CULL = 0x5ec7c0, POLYGON_SET_FRONT = 0x5ec7e0,
    POLYGON_SET_MODE = 0x5ec800, POLYGON_SET_OFFSET = 0x5ec820,
    POLYGON_GET_CULL = 0x5ec840, POLYGON_GET_FRONT = 0x5ec850, POLYGON_GET_MODE = 0x5ec860,
    POLYGON_GET_OFFSET = 0x5ec870,
    MULTISAMPLE_DEFAULTS = 0x5ec880, MULTISAMPLE_SET_ENABLE = 0x5ec8a0, MULTISAMPLE_SET_ALPHA = 0x5ec8c0,
    MULTISAMPLE_SET_DITHER = 0x5ec8e0, MULTISAMPLE_SET_SAMPLES = 0x5ec900, MULTISAMPLE_GET_ENABLE = 0x5ec920,
    MULTISAMPLE_GET_SAMPLES = 0x5ec930, MULTISAMPLE_GET_ALPHA = 0x5ec940, MULTISAMPLE_GET_DITHER = 0x5ec950,
    MULTISAMPLE_SET_RASTER_SAMPLES = 0x5ec960, MULTISAMPLE_GET_RASTER_SAMPLES = 0x5ec980,
    TEXTURE_VIEW_SET_DEFAULTS = 0x5eea60, TEXTURE_VIEW_SET_LEVELS = 0x5eea70,
    TEXTURE_VIEW_SET_LAYERS = 0x5eeaa0, TEXTURE_VIEW_SET_FORMAT = 0x5eeac0,
    TEXTURE_VIEW_SET_SWIZZLE = 0x5eeae0, TEXTURE_VIEW_SET_DEPTH_STENCIL = 0x5eeb00,
    TEXTURE_VIEW_SET_TARGET = 0x5ee7b0,
    SYNC_INITIALIZE = 0x5ed110, SYNC_FINALIZE = 0x5ed170,
    COMMAND_BUFFER_INITIALIZE = 0x5c48e0, COMMAND_BUFFER_FINALIZE = 0x5c4930,
    COMMAND_BUFFER_SET_CALLBACK = 0x5c4970, COMMAND_BUFFER_SET_CALLBACK_DATA = 0x5c4980,
    COMMAND_BUFFER_ADD_COMMAND = 0x5c49c0, COMMAND_BUFFER_ADD_CONTROL = 0x5c4bb0,
    COMMAND_BUFFER_GET_COMMAND_SIZE = 0x5c4ca0, COMMAND_BUFFER_GET_COMMAND_USED = 0x5c4cb0,
    COMMAND_BUFFER_GET_COMMAND_FREE = 0x5c4cc0, COMMAND_BUFFER_GET_CONTROL_SIZE = 0x5c4cd0,
    COMMAND_BUFFER_GET_CONTROL_USED = 0x5c4cf0, COMMAND_BUFFER_GET_CONTROL_FREE = 0x5c4d00,
    COMMAND_BUFFER_SET_RECORDING_MODE = 0x5c4d10,
    QUEUE_BUILDER_SET_DEVICE = 0x5e82d0, QUEUE_BUILDER_DEFAULTS = 0x5e82e0,
    QUEUE_BUILDER_SET_FLAGS = 0x5e8310, QUEUE_BUILDER_SET_COMMAND = 0x5e8320,
    QUEUE_BUILDER_SET_COMPUTE = 0x5e8330, QUEUE_BUILDER_SET_CONTROL = 0x5e8340,
    QUEUE_BUILDER_SET_RAY = 0x5e8350, QUEUE_BUILDER_SET_FLUSH = 0x5e8360,
    QUEUE_BUILDER_SET_MEMORY = 0x5e8370, QUEUE_BUILDER_SET_PRIMARY = 0x5e8700,
    QUEUE_INITIALIZE = 0x5e98c0, QUEUE_FINALIZE = 0x5e9ad0,
    DEVICE_BYTES = 0x3000, DEVICE_BUILDER_BYTES = 0x18,
    POOL_BYTES = 0x100, POOL_BUILDER_BYTES = 0x40,
    BUFFER_BYTES = 0x30, BUFFER_BUILDER_FIELDS_BYTES = 0x28,
    PROGRAM_BYTES = 0xa0, PROGRAM_DEVICE = 0x08, SHADER_ENTRY_BYTES = 0x10, PROGRAM_MAX_STAGES = 6,
    TEXTURE_BYTES = 0x80, TEXTURE_BUILDER_BYTES = 0x80, TEXTURE_VIEW_BYTES = 0x10,
    DESCRIPTOR_POOL_BYTES = 0x20, DESCRIPTOR_BYTES = 0x20,
    TEXTURE_POOL_MAX_ENTRIES = 0x100000, SAMPLER_POOL_MAX_ENTRIES = 0x1000,
    SAMPLER_BYTES = 0x40, SAMPLER_BUILDER_FIELDS_BYTES = 0x30, SAMPLER_FIELDS_OFFSET = 0x08,
    VERTEX_ATTRIB_BYTES = 4, VERTEX_STREAM_BYTES = 8,
    COLOR_ATTACHMENTS = 8, MULTISAMPLE_BYTES = 0x18,
    SYNC_BYTES = 0x40, SYNC_INITIALIZED_BYTES = 0x20,
    COMMAND_BUFFER_BYTES = 0xc0, COMMAND_BUFFER_INITIALIZED_BYTES = 0xb0,
    QUEUE_BYTES = 0x3000, QUEUE_BUILDER_BYTES = 0x48,
};

/* NVN2 texture builder fields, including the packed layout nibbles at +0x54. */
enum {
    TEX_DEVICE = 0x08, TEX_POOL = 0x10, TEX_OFFSET = 0x18,
    TEX_FLAGS = 0x20, TEX_TARGET = 0x24, TEX_WIDTH = 0x28, TEX_HEIGHT = 0x2c,
    TEX_DEPTH = 0x30, TEX_LEVELS = 0x34, TEX_FORMAT = 0x38, TEX_SAMPLES = 0x3c,
    TEX_SWIZZLE = 0x40, TEX_DEPTH_STENCIL = 0x50, TEX_PACKAGED_LAYOUT = 0x54,
    TEX_STRIDE = 0x58, TEX_GL_NAME = 0x5c, TEX_PACKAGED_DATA = 0x60,
};

/* NVN2 SDK memory pool builder layout. */
enum {
    POOL_FLAGS = 0x04, POOL_DEVICE = 0x08, POOL_STORAGE = 0x10, POOL_SIZE = 0x18,
    POOL_STORAGE_HANDLE = 0x20, POOL_EXTERNAL_STORAGE = 0x28,
    POOL_DEFAULT_FLAGS = 0x21, /* CPU_NO_ACCESS | GPU_CACHED */
};

/* The pinned NVN2 SDK uses the same four fields in the builder and buffer. */
enum {
    BUFFER_DEVICE = 0x08, BUFFER_POOL = 0x10, BUFFER_OFFSET = 0x18, BUFFER_SIZE = 0x20,
    /* R74: the buffer's address, cached in the object's unused tail so
     * nvnBufferGetAddress needs neither the buffer list nor the global lock. */
    BUFFER_ADDRESS_CACHE = 0x28,
};

enum {
    POOL_CPU_NO_ACCESS = 0x01, POOL_CPU_UNCACHED = 0x02, POOL_CPU_CACHED = 0x04,
    POOL_GPU_NO_ACCESS = 0x08, POOL_GPU_UNCACHED = 0x10, POOL_GPU_CACHED = 0x20,
    POOL_SHADER_CODE = 0x40, POOL_COMPRESSIBLE = 0x80,
    POOL_CPU_MASK = 0x07, POOL_GPU_MASK = 0x38,
    POOL_GUEST_ALIGNMENT = 0x1000,
};

static const uint64_t sdk_base = UINT64_C(0x7500000000);

typedef struct {
    const char *name;
    uint32_t sdk_offset;
} Proc;

static const Proc procs[] = {
    { "nvnBootstrapLoader", BOOTSTRAP },
    { "nvnBootstrapLoaderInternal", BOOTSTRAP_INTERNAL },
    { "nvnInstallGlobalDebugCallback", INSTALL_GLOBAL_DEBUG_CALLBACK },
#define NVN2_ENTRY(name, ret, args, offset, flags) { name, offset },
#include "../upstream/switch2-recomp-0.2.0/runtime/src/gfx/nvn/nvn2_table.inc"
#undef NVN2_ENTRY
};

enum { PROC_COUNT = sizeof procs / sizeof procs[0] };

typedef struct Object {
    struct Object *next;
    struct Object *device;
    uint64_t address;
    void *native;
    NvnFrontDeviceModes modes;
} Object;

typedef struct Pool {
    struct Pool *next;
    uint64_t address;
    NvnFrontPoolConfig config;
    size_t texture_uses;
} Pool;

typedef struct {
    const Pool *pool;
    uint64_t max_end; /* Largest pool end up to this entry on the same device. */
} PoolIndexEntry;

typedef struct Buffer {
    struct Buffer *next;
    const Pool *pool;
    uint64_t address, offset, size;
} Buffer;

typedef struct Texture {
    struct Texture *next;
    /* R80: O(1) unlink and a hash index (lookups and inits walked ~6k textures). */
    struct Texture **pprev, *hash_next, *pending_next;
    Pool *pool;
    size_t references;
    uint64_t address;
    NvnFrontTextureInfo info;
} Texture;

typedef struct {
    Texture *texture;
    uint8_t view[TEXTURE_VIEW_BYTES];
    bool has_view, image;
} TextureDescriptor;

typedef struct Sampler {
    struct Sampler *next;
    uint64_t address, device;
} Sampler;

typedef struct {
    NvnFrontSamplerInfo info;
    bool registered;
} SamplerDescriptor;

typedef struct DescriptorPool {
    struct DescriptorPool *next;
    uint64_t address;
    NvnFrontDescriptorPoolInfo info;
    TextureDescriptor *texture_slots;
    SamplerDescriptor *sampler_slots;
    bool sampler;
} DescriptorPool;

typedef struct CommandBuffer {
    struct CommandBuffer *next;
    uint64_t address;
    NvnFrontCommandBufferInfo info;
} CommandBuffer;

typedef struct Program {
    struct Program *next;
    struct Program *bucket_next; /* R94: chain in NvnFront.program_buckets */
    uint64_t address;
    NvnFrontProgramInfo info;
} Program;

struct NvnFront {
    NvnFrontConfig config;
    Object *devices, *queues, *syncs;
    Pool *pools;
    PoolIndexEntry *pool_index;
    size_t pool_index_count, pool_index_capacity;
    bool pool_index_disabled;
    Buffer *buffers;
    Texture *textures;
    Sampler *samplers;
    DescriptorPool *descriptor_pools;
    CommandBuffer *command_buffers;
    Program *programs;
    /* R69: address-indexed caches in front of the texture and program lists.
     * Lookups walked lists of thousands of textures; a node leaves its slot
     * when it is unlinked. */
    Texture *texture_cache[8192];
    Program *program_cache[16384];
    /* R94: every program by address hash. A cache miss walked the whole
     * program list: ~14% of the render thread's host time while turning. */
    Program *program_buckets[4096];
    uint64_t program_misses[512]; /* R76: addresses known not to be programs (+1) */
    Buffer *buffer_cache[8192]; /* R74: same for buffers */
    Texture *texture_buckets[16384]; /* R80 */
    Texture *pending_destroy; /* R80: released during a shared call; destroyed later */
};

_Thread_local bool nvn_front_shared;

/* R74: set (by the loader) while a call runs without the global lock. */
_Thread_local bool nvn_front_lock_free;

static NvnFrontReply reply(NvnFrontReplyKind kind, const char *name)
{
    NvnFrontReply out = { .kind = kind, .key = name };
    if (name) {
        size_t bytes = strlen(name);
        if (bytes >= sizeof out.name)
            bytes = sizeof out.name - 1;
        memcpy(out.name, name, bytes);
    }
    return out;
}

static const Proc *proc_at(const NvnFront *front, uint64_t pc)
{
    uint64_t delta = pc - front->config.thunk_base;
    if (delta < PROC_COUNT * 4u && !(delta & 3))
        return &procs[delta / 4];
    if (pc == sdk_base + DEVICE_GET_PROC_INTERNAL) {
        static const Proc internal = { "nvnDeviceGetProcAddress", DEVICE_GET_PROC };
        return &internal;
    }
    if (pc < sdk_base + 0x5c0000 || pc >= sdk_base + 0x640000)
        return NULL;
    for (size_t i = 0; i < PROC_COUNT; ++i)
        if (procs[i].sdk_offset && pc == sdk_base + procs[i].sdk_offset)
            return &procs[i];
    return NULL;
}

static Object **find_object(Object **list, uint64_t address)
{
    while (*list && (*list)->address != address)
        list = &(*list)->next;
    return list;
}

static Pool **find_pool(Pool **list, uint64_t address)
{
    while (*list && (*list)->address != address)
        list = &(*list)->next;
    return list;
}

static Buffer **find_buffer(Buffer **list, uint64_t address)
{
    while (*list && (*list)->address != address)
        list = &(*list)->next;
    return list;
}

static __attribute__((unused)) Texture **find_texture(Texture **list, uint64_t address)
{
    while (*list && (*list)->address != address)
        list = &(*list)->next;
    return list;
}

static Sampler **find_sampler(Sampler **list, uint64_t address)
{
    while (*list && (*list)->address != address)
        list = &(*list)->next;
    return list;
}

static DescriptorPool **find_descriptor_pool(DescriptorPool **list, uint64_t address, bool sampler)
{
    while (*list && ((*list)->address != address || (*list)->sampler != sampler))
        list = &(*list)->next;
    return list;
}

static CommandBuffer **find_command_buffer(CommandBuffer **list, uint64_t address)
{
    while (*list && (*list)->address != address)
        list = &(*list)->next;
    return list;
}

static Program **find_program(Program **list, uint64_t address)
{
    while (*list && (*list)->address != address)
        list = &(*list)->next;
    return list;
}

static size_t cache_slot(uint64_t address, size_t slots)
{
    return (size_t)(((address >> 3) * UINT64_C(0x9e3779b97f4a7c15)) >> 40) & (slots - 1);
}

static Buffer *lookup_buffer(NvnFront *front, uint64_t address)
{
    Buffer **slot = &front->buffer_cache[cache_slot(address, sizeof front->buffer_cache / sizeof *slot)];
    /* R79: recording threads and the replay worker look up concurrently; the
     * slot is read once so a racing fill cannot swap it after the check. */
    Buffer *hit = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if (hit && hit->address == address)
        return hit;
    Buffer *buffer = *find_buffer(&front->buffers, address);
    if (buffer)
        *slot = buffer;
    return buffer;
}

static void forget_buffer(NvnFront *front, uint64_t address)
{
    Buffer **slot = &front->buffer_cache[cache_slot(address, sizeof front->buffer_cache / sizeof *slot)];
    if (*slot && (*slot)->address == address)
        *slot = NULL;
}

static Texture *lookup_texture(NvnFront *front, uint64_t address)
{
    Texture **slot = &front->texture_cache[cache_slot(address, sizeof front->texture_cache / sizeof *slot)];
    Texture *hit = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if (hit && hit->address == address)
        return hit;
    Texture *texture = front->texture_buckets[cache_slot(address, 16384)];
    while (texture && texture->address != address)
        texture = texture->hash_next;
    if (texture)
        *slot = texture;
    return texture;
}

static void link_texture(NvnFront *front, Texture *texture)
{
    texture->next = front->textures;
    if (texture->next)
        texture->next->pprev = &texture->next;
    texture->pprev = &front->textures;
    front->textures = texture;
    Texture **bucket = &front->texture_buckets[cache_slot(texture->address, 16384)];
    texture->hash_next = *bucket;
    *bucket = texture;
}

static void unlink_texture(NvnFront *front, Texture *texture)
{
    *texture->pprev = texture->next;
    if (texture->next)
        texture->next->pprev = texture->pprev;
    Texture **bucket = &front->texture_buckets[cache_slot(texture->address, 16384)];
    while (*bucket && *bucket != texture)
        bucket = &(*bucket)->hash_next;
    if (*bucket)
        *bucket = texture->hash_next;
}

static void forget_texture(NvnFront *front, const Texture *texture)
{
    Texture **slot = &front->texture_cache[cache_slot(texture->address, sizeof front->texture_cache / sizeof *slot)];
    if (*slot == texture)
        *slot = NULL;
}

static Program *lookup_program(NvnFront *front, uint64_t address)
{
    Program **slot = &front->program_cache[cache_slot(address, sizeof front->program_cache / sizeof *slot)];
    Program *hit = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if (hit && hit->address == address)
        return hit;
    uint64_t *miss = &front->program_misses[cache_slot(address, sizeof front->program_misses / sizeof *miss)];
    if (*miss == address + 1)
        return NULL;
    Program *program = front->program_buckets[cache_slot(address, 4096)];
    while (program && program->address != address)
        program = program->bucket_next;
    if (program)
        *slot = program;
    else
        *miss = address + 1; /* R76: failed lookups walked the whole program list each time */
    return program;
}

static void forget_program(NvnFront *front, const Program *program)
{
    Program **slot = &front->program_cache[cache_slot(program->address, sizeof front->program_cache / sizeof *slot)];
    if (*slot == program)
        *slot = NULL;
}

static const uint8_t *read_guest(const NvnFront *front, uint64_t address, size_t bytes)
{
    if (!address || !bytes || bytes > UINT64_MAX - address)
        return NULL;
    return front->config.read_guest(front->config.guest_opaque, address, bytes);
}

static uint8_t *write_guest(const NvnFront *front, uint64_t address, size_t bytes)
{
    if (!address || !bytes || bytes > UINT64_MAX - address)
        return NULL;
    return front->config.write_guest(front->config.guest_opaque, address, bytes);
}

static uint32_t read32(const uint8_t *address)
{
    uint32_t value;
    memcpy(&value, address, sizeof value);
    return value;
}

static uint64_t read64(const uint8_t *address)
{
    uint64_t value;
    memcpy(&value, address, sizeof value);
    return value;
}

static float read_float(const uint8_t *address)
{
    float value;
    memcpy(&value, address, sizeof value);
    return value;
}

static void write32(uint8_t *address, uint32_t value)
{
    memcpy(address, &value, sizeof value);
}

static void write64(uint8_t *address, uint64_t value)
{
    memcpy(address, &value, sizeof value);
}

static NvnFrontReply lookup(const NvnFront *front, const Proc *proc, uint64_t x[32])
{
    uint64_t address = x[proc->sdk_offset == DEVICE_GET_PROC ? 1 : 0];
    char name[sizeof ((NvnFrontReply *)0)->name];
    for (size_t i = 0; i < sizeof name; ++i) {
        if (i > UINT64_MAX - address)
            break;
        const uint8_t *p = read_guest(front, address + i, 1);
        if (!p)
            break;
        name[i] = (char)*p;
        if (name[i])
            continue;
        for (size_t j = 0; j < PROC_COUNT; ++j) {
            if (!strcmp(name, procs[j].name)) {
                x[0] = front->config.thunk_base + j * 4u;
                return reply(NVN_FRONT_HANDLED, proc->name);
            }
        }
        return reply(NVN_FRONT_BOUNDARY, name[0] ? name : proc->name);
    }
    return reply(NVN_FRONT_INVALID_GUEST, proc->name);
}

static NvnFrontReplyKind device_descriptor_handle(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    if (!*find_object(&front->devices, x[0]))
        return NVN_FRONT_INVALID_GUEST;
    const uint64_t id = (uint64_t)(int64_t)(int32_t)x[1];
    switch (offset) {
    case DEVICE_GET_SEPARATE_SAMPLER_HANDLE: x[0] = id << 20; break;
    case DEVICE_GET_TEXTURE_HANDLE: x[0] = id | ((uint64_t)(int64_t)(int32_t)x[2] << 20); break;
    default: x[0] = id; break;
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind device_integer(NvnFront *front, uint64_t x[32])
{
    Object *device = *find_object(&front->devices, x[0]);
    if (x[0] && !device)
        return NVN_FRONT_INVALID_GUEST;
    uint8_t *output = write_guest(front, x[2], 4);
    if (!output)
        return NVN_FRONT_INVALID_GUEST;
    int32_t value;
    switch ((uint32_t)x[1]) {
    case 0: value = 231; break; /* The guest ABI, independent of backend capabilities. */
    case 1: value = 30; break;
    /* Guest layout constants from the pinned SDK's GetInteger table. */
    case 0x04: value = 0x100; break; /* Uniform buffer alignment. */
    case 0x0b:                      /* Transform feedback buffer alignment. */
    case 0x0c: value = 4; break;     /* Transform feedback control alignment. */
    case 0x0f:                      /* Texture descriptor bytes. */
    case 0x10: value = 0x20; break;  /* Sampler descriptor bytes. */
    case 0x11:                      /* Reserved texture descriptors. */
    case 0x12: value = 0x100; break; /* Reserved sampler descriptors. */
    case 0x13: value = 4; break;     /* Command buffer command alignment. */
    case 0x14: value = 8; break;     /* Command buffer control alignment. */
    case 0x15: value = 512; break;   /* Minimum command memory. */
    case 0x16: value = 256; break;   /* Minimum control memory. */
    case 0x24: value = TEXTURE_POOL_MAX_ENTRIES; break;
    case 0x25: value = SAMPLER_POOL_MAX_ENTRIES; break;
    case 0x5a: value = 0; break; /* Release frontend has no NVN debug layer. */
    default:
        if (!front->config.get_integer || !front->config.get_integer(front->config.backend_opaque,
                device ? device->native : NULL, (uint32_t)x[1], &value))
            return NVN_FRONT_BOUNDARY;
        break;
    }
    write32(output, (uint32_t)value);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind device_builder(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    if (offset == DEVICE_BUILDER_GET_FLAGS) {
        const uint8_t *builder = read_guest(front, x[0], DEVICE_BUILDER_BYTES);
        if (!builder)
            return NVN_FRONT_INVALID_GUEST;
        x[0] = read32(builder + 4);
        return NVN_FRONT_HANDLED;
    }
    uint8_t *builder = write_guest(front, x[0], DEVICE_BUILDER_BYTES);
    if (!builder)
        return NVN_FRONT_INVALID_GUEST;
    if (offset == DEVICE_BUILDER_DEFAULTS)
        memset(builder, 0, DEVICE_BUILDER_BYTES);
    else
        write32(builder + 4, (uint32_t)x[1]);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind set_device_mode(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    Object *device = *find_object(&front->devices, x[0]);
    /* The guest passes both enums in W1. Each accepts values 0 and 1. */
    const uint32_t mode = (uint32_t)x[1];
    if (!device || mode > 1)
        return NVN_FRONT_INVALID_GUEST;

    if (offset == DEVICE_SET_WINDOW_ORIGIN)
        device->modes.window_origin = (NvnFrontWindowOrigin)mode;
    else
        device->modes.depth_mode = (NvnFrontDepthMode)mode;
    return NVN_FRONT_HANDLED;
}

static NvnFrontPoolConfig read_pool_config(const uint8_t *builder)
{
    return (NvnFrontPoolConfig){
        .device = read64(builder + POOL_DEVICE),
        .storage = read64(builder + POOL_STORAGE),
        .size = read64(builder + POOL_SIZE),
        .flags = read32(builder + POOL_FLAGS),
    };
}

bool nvn_front_read_pool_builder(const NvnFront *front, uint64_t builder_address, NvnFrontPoolConfig *config)
{
    if (!front || !config)
        return false;
    const uint8_t *builder = read_guest(front, builder_address, POOL_BUILDER_BYTES);
    if (!builder)
        return false;
    *config = read_pool_config(builder);
    return true;
}

static NvnFrontReplyKind get_pool_builder(const NvnFront *front, uint32_t offset, uint64_t x[32])
{
    NvnFrontPoolConfig config;
    if (!nvn_front_read_pool_builder(front, x[0], &config))
        return NVN_FRONT_INVALID_GUEST;
    switch (offset) {
    case POOL_BUILDER_GET_DEVICE: x[0] = config.device; break;
    case POOL_BUILDER_GET_MEMORY: x[0] = config.storage; break;
    case POOL_BUILDER_GET_SIZE: x[0] = config.size; break;
    case POOL_BUILDER_GET_FLAGS: x[0] = config.flags; break;
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind set_pool_builder(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    uint8_t *builder = write_guest(front, x[0], POOL_BUILDER_BYTES);
    if (!builder)
        return NVN_FRONT_INVALID_GUEST;
    switch (offset) {
    case POOL_BUILDER_SET_DEVICE: write64(builder + POOL_DEVICE, x[1]); break;
    case POOL_BUILDER_SET_FLAGS: write32(builder + POOL_FLAGS, (uint32_t)x[1]); break;
    case POOL_BUILDER_DEFAULTS:
        /* The SDK preserves the device, both padding regions, and the end of the builder. */
        write32(builder + POOL_FLAGS, POOL_DEFAULT_FLAGS);
        memset(builder + POOL_STORAGE, 0, 20);
        memset(builder + POOL_EXTERNAL_STORAGE, 0, 16);
        break;
    case POOL_BUILDER_SET_STORAGE:
        write64(builder + POOL_STORAGE, x[1]);
        write64(builder + POOL_SIZE, x[2]);
        write32(builder + POOL_STORAGE_HANDLE, 0);
        break;
    }
    return NVN_FRONT_HANDLED;
}

static uint32_t normalize_pool_flags(uint32_t flags)
{
    uint32_t cpu = POOL_CPU_UNCACHED, gpu = POOL_GPU_CACHED;
    if (!(flags & POOL_CPU_UNCACHED)) {
        if (flags & POOL_CPU_CACHED)
            cpu = POOL_CPU_CACHED;
        else if (flags & POOL_CPU_NO_ACCESS)
            cpu = POOL_CPU_NO_ACCESS;
    }
    if (!(flags & POOL_GPU_CACHED)) {
        if (flags & POOL_GPU_UNCACHED)
            gpu = POOL_GPU_UNCACHED;
        else if (flags & POOL_GPU_NO_ACCESS)
            gpu = POOL_GPU_NO_ACCESS;
    }
    return (flags & ~(uint32_t)(POOL_CPU_MASK | POOL_GPU_MASK)) | cpu | gpu;
}

/* Pool registration is infrequent relative to draw replay. Keep GPU-visible
 * pools sorted by (device, storage) so reads can skip unrelated pools. */
static size_t pool_upper_bound(const NvnFront *front, uint64_t device, uint64_t address)
{
    size_t lo = 0, hi = front->pool_index_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const NvnFrontPoolConfig *config = &front->pool_index[mid].pool->config;
        if (config->device < device || (config->device == device && config->storage <= address))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void refresh_pool_ends(NvnFront *front, size_t start)
{
    PoolIndexEntry *entries = front->pool_index;
    for (size_t i = start; i < front->pool_index_count; ++i) {
        const NvnFrontPoolConfig *config = &entries[i].pool->config;
        const uint64_t end = config->storage + config->size;
        const uint64_t previous_end = i && entries[i - 1].pool->config.device == config->device ?
            entries[i - 1].max_end : 0;
        entries[i].max_end = end > previous_end ? end : previous_end;
    }
}

static void index_pool(NvnFront *front, const Pool *pool)
{
    if (front->pool_index_disabled || (pool->config.flags & POOL_GPU_NO_ACCESS))
        return;
    if (front->pool_index_count == front->pool_index_capacity) {
        const size_t capacity = front->pool_index_capacity ? front->pool_index_capacity * 2 : 16;
        PoolIndexEntry *entries = realloc(front->pool_index, capacity * sizeof *entries);
        if (!entries) {
            /* Registration remains usable if the optional index cannot grow. */
            free(front->pool_index);
            front->pool_index = NULL;
            front->pool_index_disabled = true;
            return;
        }
        front->pool_index = entries;
        front->pool_index_capacity = capacity;
    }
    const size_t at = pool_upper_bound(front, pool->config.device, pool->config.storage);
    PoolIndexEntry *entries = front->pool_index;
    memmove(entries + at + 1, entries + at, (front->pool_index_count - at) * sizeof *entries);
    entries[at].pool = pool;
    ++front->pool_index_count;
    refresh_pool_ends(front, at);
}

static void unindex_pool(NvnFront *front, const Pool *pool)
{
    if (front->pool_index_disabled || (pool->config.flags & POOL_GPU_NO_ACCESS))
        return;
    PoolIndexEntry *entries = front->pool_index;
    for (size_t at = 0; at < front->pool_index_count; ++at) {
        if (entries[at].pool == pool) {
            --front->pool_index_count;
            memmove(entries + at, entries + at + 1, (front->pool_index_count - at) * sizeof *entries);
            refresh_pool_ends(front, at);
            return;
        }
    }
}

static NvnFrontReplyKind initialize_pool(NvnFront *front, uint64_t x[32])
{
    const uint8_t *builder = read_guest(front, x[1], POOL_BUILDER_BYTES);
    uint8_t *object = write_guest(front, x[0], POOL_BYTES);
    if (!builder || !object || *find_pool(&front->pools, x[0]))
        return NVN_FRONT_INVALID_GUEST;
    NvnFrontPoolConfig config = read_pool_config(builder);
    if (!*find_object(&front->devices, config.device))
        return NVN_FRONT_INVALID_GUEST;

    const uint32_t supported_flags = POOL_CPU_MASK | POOL_GPU_MASK | POOL_SHADER_CODE | POOL_COMPRESSIBLE;
    if ((config.flags & ~supported_flags) || read32(builder + POOL_STORAGE_HANDLE))
        return NVN_FRONT_BOUNDARY;

    Pool *pool = NULL;
    if (config.size <= (UINT64_C(1) << 40) && config.size <= SIZE_MAX &&
        !((config.storage | config.size) & (POOL_GUEST_ALIGNMENT - 1)) &&
        read_guest(front, config.storage, (size_t)config.size) &&
        write_guest(front, config.storage, (size_t)config.size) &&
        (x[0] >= config.storage + config.size || config.storage >= x[0] + POOL_BYTES))
        pool = malloc(sizeof *pool);
    if (!pool) {
        x[0] = 0;
        return NVN_FRONT_HANDLED;
    }

    /* Guest bytes remain the canonical CPU backing. Replay resolves the guest
     * buffer address to the pool and stages only ranges that it consumes. */
    config.flags = normalize_pool_flags(config.flags);
    *pool = (Pool){ .next = front->pools, .address = x[0], .config = config };
    memset(object, 0, POOL_BYTES);
    front->pools = pool;
    index_pool(front, pool);
    x[0] = 1;
    return NVN_FRONT_HANDLED;
}

static void remove_pool(Pool **link)
{
    Pool *pool = *link;
    *link = pool->next;
    free(pool);
}

static NvnFrontReplyKind sync_pool_range(const NvnFront *front, const NvnFrontPoolConfig *pool,
    uint64_t offset, uint64_t size)
{
    if (offset > pool->size || size > pool->size - offset || (pool->flags & POOL_CPU_NO_ACCESS))
        return NVN_FRONT_INVALID_GUEST;
    if (size && !read_guest(front, pool->storage + offset, (size_t)size))
        return NVN_FRONT_INVALID_GUEST;
    /* Replay reads canonical CPU bytes and synchronously writes back completed
     * GPU copies before returning. There is no separate mapping to reconcile. */
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind pool_operation(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    Pool **link = find_pool(&front->pools, x[0]);
    const Pool *pool = *link;
    if (!pool)
        return NVN_FRONT_INVALID_GUEST;
    switch (offset) {
    case POOL_FINALIZE:
        if (pool->texture_uses)
            return NVN_FRONT_INVALID_GUEST;
        for (const DescriptorPool *descriptors = front->descriptor_pools; descriptors; descriptors = descriptors->next)
            if (descriptors->info.memory_pool == x[0])
                return NVN_FRONT_INVALID_GUEST;
        for (const Buffer *buffer = front->buffers; buffer; buffer = buffer->next)
            if (buffer->pool == pool)
                return NVN_FRONT_INVALID_GUEST;
        for (const CommandBuffer *command = front->command_buffers; command; command = command->next)
            if (command->info.command_pool == x[0])
                return NVN_FRONT_INVALID_GUEST;
        unindex_pool(front, pool);
        remove_pool(link);
        break;
    case POOL_MAP:
        x[0] = (pool->config.flags & POOL_CPU_NO_ACCESS) ? 0 : pool->config.storage;
        break;
    case POOL_FLUSH:
    case POOL_INVALIDATE:
        return sync_pool_range(front, &pool->config, x[1], x[2]);
    case POOL_GET_BUFFER_ADDRESS: x[0] = pool->config.storage; break;
    case POOL_GET_SIZE: x[0] = pool->config.size; break;
    case POOL_GET_FLAGS: x[0] = pool->config.flags; break;
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind set_buffer_builder(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    uint8_t *builder = write_guest(front, x[0], BUFFER_BUILDER_FIELDS_BYTES);
    if (!builder)
        return NVN_FRONT_INVALID_GUEST;
    switch (offset) {
    case BUFFER_BUILDER_SET_DEVICE:
        write64(builder + BUFFER_DEVICE, x[1]);
        break;
    case BUFFER_BUILDER_DEFAULTS:
        memset(builder + BUFFER_POOL, 0, BUFFER_BUILDER_FIELDS_BYTES - BUFFER_POOL);
        break;
    case BUFFER_BUILDER_SET_STORAGE:
        write64(builder + BUFFER_POOL, x[1]);
        write64(builder + BUFFER_OFFSET, x[2]);
        write64(builder + BUFFER_SIZE, x[3]);
        break;
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind get_buffer_builder(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    const uint8_t *builder = read_guest(front, x[0], BUFFER_BUILDER_FIELDS_BYTES);
    if (!builder)
        return NVN_FRONT_INVALID_GUEST;
    switch (offset) {
    case BUFFER_BUILDER_GET_DEVICE: x[0] = read64(builder + BUFFER_DEVICE); break;
    case BUFFER_BUILDER_GET_POOL: x[0] = read64(builder + BUFFER_POOL); break;
    case BUFFER_BUILDER_GET_OFFSET: x[0] = read64(builder + BUFFER_OFFSET); break;
    case BUFFER_BUILDER_GET_SIZE: x[0] = read64(builder + BUFFER_SIZE); break;
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind initialize_buffer(NvnFront *front, uint64_t x[32])
{
    const uint8_t *builder = read_guest(front, x[1], BUFFER_BUILDER_FIELDS_BYTES);
    uint8_t *object = write_guest(front, x[0], BUFFER_BYTES);
    if (!builder || !object)
        return NVN_FRONT_INVALID_GUEST;
    const uint64_t device = read64(builder + BUFFER_DEVICE);
    const uint64_t pool_address = read64(builder + BUFFER_POOL);
    const uint64_t offset = read64(builder + BUFFER_OFFSET);
    const uint64_t size = read64(builder + BUFFER_SIZE);
    const Pool *pool = *find_pool(&front->pools, pool_address);
    if (!pool || device != pool->config.device)
        return NVN_FRONT_INVALID_GUEST;
    if (offset > pool->config.size || size > pool->config.size - offset) {
        x[0] = 0;
        return NVN_FRONT_HANDLED;
    }
    const uint64_t storage = pool->config.storage + offset;
    if (size && x[0] < storage + size && storage < x[0] + BUFFER_BYTES)
        return NVN_FRONT_INVALID_GUEST;

    Buffer **link = find_buffer(&front->buffers, x[0]);
    Buffer *buffer = *link;
    if (!buffer) {
        buffer = calloc(1, sizeof *buffer);
        if (!buffer) {
            x[0] = 0;
            return NVN_FRONT_HANDLED;
        }
        *link = buffer;
    }
    buffer->address = x[0];
    buffer->pool = pool;
    buffer->offset = offset;
    buffer->size = size;
    memset(object, 0, BUFFER_BYTES);
    write64(object + BUFFER_DEVICE, device);
    write64(object + BUFFER_POOL, pool_address);
    write64(object + BUFFER_OFFSET, offset);
    write64(object + BUFFER_SIZE, size);
    write64(object + BUFFER_ADDRESS_CACHE, storage);
    x[0] = 1;
    return NVN_FRONT_HANDLED;
}

static void remove_buffer(Buffer **link)
{
    Buffer *buffer = *link;
    *link = buffer->next;
    free(buffer);
}

static NvnFrontReplyKind buffer_operation(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    if (offset == BUFFER_GET_ADDRESS) {
        /* R74: answered from the object without the lock when cached. */
        const uint8_t *object = read_guest(front, x[0], BUFFER_BYTES);
        const uint64_t cached = object ? read64(object + BUFFER_ADDRESS_CACHE) : 0;
        if (cached) {
            x[0] = cached;
            return NVN_FRONT_HANDLED;
        }
    }
    if (nvn_front_lock_free)
        return NVN_FRONT_BOUNDARY; /* redone under the lock */
    if (offset == BUFFER_FINALIZE) {
        Buffer **link = find_buffer(&front->buffers, x[0]);
        if (!*link)
            return NVN_FRONT_INVALID_GUEST;
        forget_buffer(front, x[0]);
        remove_buffer(link);
        uint8_t *object = write_guest(front, x[0], BUFFER_BYTES);
        if (object)
            write64(object + BUFFER_ADDRESS_CACHE, 0);
        return NVN_FRONT_HANDLED;
    }
    const Buffer *buffer = lookup_buffer(front, x[0]);
    if (!buffer)
        return NVN_FRONT_INVALID_GUEST;
    const NvnFrontPoolConfig *pool = &buffer->pool->config;
    switch (offset) {
    case BUFFER_MAP: x[0] = (pool->flags & POOL_CPU_NO_ACCESS) ? 0 : pool->storage + buffer->offset; break;
    case BUFFER_FLUSH:
    case BUFFER_INVALIDATE:
        if (x[1] > buffer->size || x[2] > buffer->size - x[1])
            return NVN_FRONT_INVALID_GUEST;
        return sync_pool_range(front, pool, buffer->offset + x[1], x[2]);
    case BUFFER_GET_ADDRESS: x[0] = pool->storage + buffer->offset; break;
    case BUFFER_GET_POOL: x[0] = buffer->pool->address; break;
    case BUFFER_GET_OFFSET: x[0] = buffer->offset; break;
    case BUFFER_GET_SIZE: x[0] = buffer->size; break;
    }
    return NVN_FRONT_HANDLED;
}

bool nvn_front_read_texture_builder(const NvnFront *front, uint64_t builder_address,
    NvnFrontTextureConfig *config)
{
    if (!front || !config)
        return false;
    const uint8_t *builder = read_guest(front, builder_address, TEXTURE_BUILDER_BYTES);
    if (!builder)
        return false;
    *config = (NvnFrontTextureConfig){
        .device = read64(builder + TEX_DEVICE), .pool = read64(builder + TEX_POOL),
        .offset = read64(builder + TEX_OFFSET), .packaged_data = read64(builder + TEX_PACKAGED_DATA),
        .flags = read32(builder + TEX_FLAGS), .target = read32(builder + TEX_TARGET),
        .width = read32(builder + TEX_WIDTH), .height = read32(builder + TEX_HEIGHT),
        .depth = read32(builder + TEX_DEPTH), .levels = read32(builder + TEX_LEVELS),
        .format = read32(builder + TEX_FORMAT), .samples = read32(builder + TEX_SAMPLES),
        .stride = read32(builder + TEX_STRIDE), .gl_texture_name = read32(builder + TEX_GL_NAME),
        .depth_stencil_mode = read32(builder + TEX_DEPTH_STENCIL),
    };
    memcpy(config->swizzle, builder + TEX_SWIZZLE, sizeof config->swizzle);
    memcpy(&config->packaged_layout, builder + TEX_PACKAGED_LAYOUT, sizeof config->packaged_layout);
    return true;
}

static NvnFrontReplyKind set_texture_builder(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    uint8_t *builder = write_guest(front, x[0], TEXTURE_BUILDER_BYTES);
    if (!builder)
        return NVN_FRONT_INVALID_GUEST;
    const uint32_t value = (uint32_t)x[1];
    switch (offset) {
    case TEXTURE_BUILDER_SET_DEVICE: write64(builder + TEX_DEVICE, x[1]); break;
    case TEXTURE_BUILDER_DEFAULTS: {
        /* The SDK preserves the device and padding. 2D, one mip, RGBA swizzle. */
        static const uint32_t defaults[] = { 1, 0, 0, 0, 1, 0, 0, 2, 3, 4, 5, 0 };
        memset(builder + TEX_POOL, 0, 20);
        memcpy(builder + TEX_TARGET, defaults, sizeof defaults);
        memset(builder + TEX_PACKAGED_LAYOUT, 0, 2);
        memset(builder + TEX_STRIDE, 0, 16);
        break;
    }
    case TEXTURE_BUILDER_SET_FLAGS: write32(builder + TEX_FLAGS, value); break;
    case TEXTURE_BUILDER_SET_TARGET: write32(builder + TEX_TARGET, value); break;
    case TEXTURE_BUILDER_SET_WIDTH: write32(builder + TEX_WIDTH, value); break;
    case TEXTURE_BUILDER_SET_HEIGHT: write32(builder + TEX_HEIGHT, value); break;
    case TEXTURE_BUILDER_SET_DEPTH: write32(builder + TEX_DEPTH, value); break;
    case TEXTURE_BUILDER_SET_SIZE_1D:
    case TEXTURE_BUILDER_SET_SIZE_2D:
    case TEXTURE_BUILDER_SET_SIZE_3D:
        write32(builder + TEX_WIDTH, value);
        write32(builder + TEX_HEIGHT, offset == TEXTURE_BUILDER_SET_SIZE_1D ? 1u : (uint32_t)x[2]);
        write32(builder + TEX_DEPTH, offset == TEXTURE_BUILDER_SET_SIZE_3D ? (uint32_t)x[3] : 1u);
        break;
    case TEXTURE_BUILDER_SET_LEVELS: write32(builder + TEX_LEVELS, value); break;
    case TEXTURE_BUILDER_SET_FORMAT: write32(builder + TEX_FORMAT, value); break;
    case TEXTURE_BUILDER_SET_SAMPLES: write32(builder + TEX_SAMPLES, value); break;
    case TEXTURE_BUILDER_SET_SWIZZLE:
        for (unsigned i = 0; i < 4; ++i)
            write32(builder + TEX_SWIZZLE + i * 4u, (uint32_t)x[1 + i]);
        break;
    case TEXTURE_BUILDER_SET_DEPTH_STENCIL: write32(builder + TEX_DEPTH_STENCIL, value); break;
    case TEXTURE_BUILDER_SET_STORAGE:
        write64(builder + TEX_POOL, x[1]);
        write64(builder + TEX_OFFSET, x[2]);
        break;
    case TEXTURE_BUILDER_SET_GL_NAME: write32(builder + TEX_GL_NAME, value); break;
    case TEXTURE_BUILDER_SET_STRIDE: write32(builder + TEX_STRIDE, value); break;
    case TEXTURE_BUILDER_SET_PACKAGED_DATA: write64(builder + TEX_PACKAGED_DATA, x[1]); break;
    case TEXTURE_BUILDER_SET_PACKAGED_LAYOUT: {
        uint16_t layout;
        const uint8_t *input = read_guest(front, x[1], sizeof layout);
        if (!input)
            return NVN_FRONT_INVALID_GUEST;
        memcpy(&layout, input, sizeof layout);
        layout = (uint16_t)((layout << 4) | 1u);
        memcpy(builder + TEX_PACKAGED_LAYOUT, &layout, sizeof layout);
        break;
    }
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind texture_query(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    NvnFrontTextureConfig config;
    if (!nvn_front_read_texture_builder(front, x[0], &config))
        return NVN_FRONT_INVALID_GUEST;
    if (offset == TEXTURE_BUILDER_GET_WIDTH || offset == TEXTURE_BUILDER_GET_HEIGHT) {
        x[0] = offset == TEXTURE_BUILDER_GET_WIDTH ? config.width : config.height;
        return NVN_FRONT_HANDLED;
    }
    if (!*find_object(&front->devices, config.device))
        return NVN_FRONT_INVALID_GUEST;
    /* This describes the guest pool range. Vulkan images and scanout have their own layouts. */
    uint64_t size, alignment;
    if (!nvn_guest_texture_layout(&config, &size, &alignment))
        return NVN_FRONT_BOUNDARY;
    x[0] = offset == TEXTURE_BUILDER_GET_ALIGNMENT ? alignment : size;
    return NVN_FRONT_HANDLED;
}

static void release_texture(NvnFront *front, Texture *texture)
{
    if (--texture->references)
        return;
    if (nvn_front_shared) {
        /* R80: shared calls run beside the replay worker; destroying the host
         * texture touches its batch, so it waits for the next locked call. */
        texture->pending_next = front->pending_destroy;
        front->pending_destroy = texture;
        return;
    }
    --texture->pool->texture_uses;
    front->config.texture_destroy(front->config.backend_opaque, texture->info.native);
    free(texture);
}

static void destroy_pending_textures(NvnFront *front)
{
    while (front->pending_destroy) {
        Texture *texture = front->pending_destroy;
        front->pending_destroy = texture->pending_next;
        --texture->pool->texture_uses;
        front->config.texture_destroy(front->config.backend_opaque, texture->info.native);
        free(texture);
    }
}

static NvnFrontReplyKind initialize_texture(NvnFront *front, uint64_t x[32])
{
    NvnFrontTextureInfo info = {0};
    NvnFrontTextureCreateInfo *create_info = &info.create_info;
    NvnFrontTextureConfig *config = &create_info->config;
    uint8_t *object = write_guest(front, x[0], TEXTURE_BYTES);
    if (!object || !nvn_front_read_texture_builder(front, x[1], config))
        return NVN_FRONT_INVALID_GUEST;
    const Object *device = *find_object(&front->devices, config->device);
    if (!device)
        return NVN_FRONT_INVALID_GUEST;
    if (config->gl_texture_name)
        return NVN_FRONT_BOUNDARY;

    Pool *pool = *find_pool(&front->pools, config->pool);
    if (!pool || pool->config.device != config->device ||
        (pool->config.flags & POOL_GPU_NO_ACCESS) || config->offset >= pool->config.size)
        return NVN_FRONT_INVALID_GUEST;

    if (!nvn_guest_texture_layout(config, &create_info->storage_size, &create_info->storage_alignment))
        return NVN_FRONT_BOUNDARY;
    const uint64_t storage = pool->config.storage + config->offset;
    const uint64_t size = create_info->storage_size;
    if (size > pool->config.size - config->offset || size > SIZE_MAX ||
        !read_guest(front, storage, (size_t)size) ||
        (x[0] < storage + size && storage < x[0] + TEXTURE_BYTES))
        return NVN_FRONT_INVALID_GUEST;
    if (!front->config.texture_create)
        return NVN_FRONT_BOUNDARY;
    create_info->storage_address = storage;

    Texture *old = lookup_texture(front, x[0]);
    Texture *texture = malloc(sizeof *texture);
    if (!texture) {
        x[0] = 0;
        return NVN_FRONT_HANDLED;
    }
    const NvnFrontTextureCreateResult result = front->config.texture_create(front->config.backend_opaque,
        device->native, create_info, &info.native);
    if (result != NVN_FRONT_TEXTURE_CREATED || !info.native) {
        if (info.native)
            front->config.texture_destroy(front->config.backend_opaque, info.native);
        free(texture);
        if (result == NVN_FRONT_TEXTURE_UNSUPPORTED)
            return NVN_FRONT_BOUNDARY;
        x[0] = 0;
        return NVN_FRONT_HANDLED;
    }

    /* Reinitialization replaces the live object only after the new backing is accepted. */
    ++pool->texture_uses;
    *texture = (Texture){ .pool = pool, .references = 1, .address = x[0], .info = info };
    if (old) {
        unlink_texture(front, old);
        forget_texture(front, old);
        release_texture(front, old);
    }
    link_texture(front, texture);
    memset(object, 0, TEXTURE_BYTES);
    x[0] = 1;
    return NVN_FRONT_HANDLED;
}

static void remove_texture(NvnFront *front, Texture *texture)
{
    unlink_texture(front, texture);
    forget_texture(front, texture);
    release_texture(front, texture);
}

static NvnFrontReplyKind finalize_texture(NvnFront *front, uint64_t address)
{
    Texture *texture = lookup_texture(front, address);
    if (!texture)
        return NVN_FRONT_INVALID_GUEST;
    remove_texture(front, texture);
    return NVN_FRONT_HANDLED;
}

static void reset_descriptor_pool(NvnFront *front, DescriptorPool *pool)
{
    if (pool->texture_slots) {
        for (uint32_t i = 0; i < pool->info.count; ++i) {
            Texture *texture = pool->texture_slots[i].texture;
            if (texture)
                release_texture(front, texture);
        }
        free(pool->texture_slots);
        pool->texture_slots = NULL;
    }
    free(pool->sampler_slots);
    pool->sampler_slots = NULL;
}

static void remove_descriptor_pool(NvnFront *front, DescriptorPool **link)
{
    DescriptorPool *pool = *link;
    *link = pool->next;
    reset_descriptor_pool(front, pool);
    free(pool);
}

static NvnFrontReplyKind initialize_descriptor_pool(NvnFront *front, bool sampler, uint64_t x[32])
{
    /* Exact first slots from the pinned SDK. The sampler defaults are the
     * SDK encoder's output with zero device LOD bias and control flags. */
    static const uint32_t texture_null_slot[8] = { 0x24900, 0, 0x600000 };
    static const uint32_t sampler_default_slots[3][8] = {
        { 0x00026092, 0x262, 0xf00000 },
        { 0x00126092, 0x262, 0xf00000 },
        { 0x00126092, 0x251, 0xf00000 },
    };
    const Pool *memory = *find_pool(&front->pools, x[1]);
    uint8_t *object = write_guest(front, x[0], DESCRIPTOR_POOL_BYTES);
    if (!memory || !object || *find_descriptor_pool(&front->descriptor_pools, x[0], !sampler))
        return NVN_FRONT_INVALID_GUEST;
    const uint64_t offset = x[2];
    const uint32_t count = (uint32_t)x[3];
    const uint64_t bytes = (uint64_t)count * DESCRIPTOR_BYTES;
    const uint32_t maximum = sampler ? SAMPLER_POOL_MAX_ENTRIES : TEXTURE_POOL_MAX_ENTRIES;
    if (count < (sampler ? 3u : 1u) || count > maximum || (offset & (DESCRIPTOR_BYTES - 1)) ||
        offset > memory->config.size || bytes > memory->config.size - offset ||
        (memory->config.flags & (POOL_CPU_NO_ACCESS | POOL_GPU_NO_ACCESS))) {
        x[0] = 0;
        return NVN_FRONT_HANDLED;
    }
    const uint64_t storage = memory->config.storage + offset;
    uint8_t *data = write_guest(front, storage, (size_t)bytes);
    if (!data || (x[0] < storage + bytes && storage < x[0] + DESCRIPTOR_POOL_BYTES))
        return NVN_FRONT_INVALID_GUEST;
    if (sampler) {
        const uint8_t *device_control = read_guest(front, memory->config.device + 0x1cc0, 8);
        if (!device_control)
            return NVN_FRONT_INVALID_GUEST;
        if (read32(device_control) || (read32(device_control + 4) & 0x100u))
            return NVN_FRONT_BOUNDARY;
    }

    DescriptorPool **link = find_descriptor_pool(&front->descriptor_pools, x[0], sampler);
    DescriptorPool *pool = *link;
    if (!pool) {
        pool = calloc(1, sizeof *pool);
        if (!pool) {
            x[0] = 0;
            return NVN_FRONT_HANDLED;
        }
        *link = pool;
    }
    reset_descriptor_pool(front, pool);
    pool->address = x[0];
    pool->sampler = sampler;
    pool->info = (NvnFrontDescriptorPoolInfo){
        .device = memory->config.device, .memory_pool = x[1], .offset = offset, .storage = storage, .count = count,
    };
    write64(object, x[1]);
    write64(object + 8, offset);
    write32(object + 16, count);
    write64(object + 24, 0);

    if (sampler)
        memcpy(data, sampler_default_slots, sizeof sampler_default_slots);
    else
        memcpy(data, texture_null_slot, sizeof texture_null_slot);
    x[0] = 1;
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind descriptor_pool_operation(NvnFront *front, uint32_t offset, bool sampler, uint64_t x[32])
{
    DescriptorPool **link = find_descriptor_pool(&front->descriptor_pools, x[0], sampler);
    const DescriptorPool *pool = *link;
    if (!pool)
        return NVN_FRONT_INVALID_GUEST;
    switch (offset) {
    case TEXTURE_POOL_FINALIZE:
    case SAMPLER_POOL_FINALIZE: remove_descriptor_pool(front, link); break;
    case TEXTURE_POOL_GET_MEMORY:
    case SAMPLER_POOL_GET_MEMORY: x[0] = pool->info.memory_pool; break;
    case TEXTURE_POOL_GET_OFFSET:
    case SAMPLER_POOL_GET_OFFSET: x[0] = pool->info.offset; break;
    case TEXTURE_POOL_GET_SIZE:
    case SAMPLER_POOL_GET_SIZE: x[0] = pool->info.count; break;
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind register_texture_descriptor(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    DescriptorPool *pool = *find_descriptor_pool(&front->descriptor_pools, x[0], false);
    Texture *texture = lookup_texture(front, x[2]);
    const uint32_t index = (uint32_t)x[1];
    if (!pool || !texture || index >= pool->info.count ||
        pool->info.device != texture->info.create_info.config.device)
        return NVN_FRONT_INVALID_GUEST;

    TextureDescriptor descriptor = { .texture = texture, .has_view = x[3] != 0,
        .image = offset == TEXTURE_POOL_REGISTER_IMAGE };
    if (descriptor.has_view) {
        const uint8_t *view = read_guest(front, x[3], sizeof descriptor.view);
        if (!view)
            return NVN_FRONT_INVALID_GUEST;
        memcpy(descriptor.view, view, sizeof descriptor.view);
    }
    if (!pool->texture_slots) {
        pool->texture_slots = calloc(pool->info.count, sizeof *pool->texture_slots);
        if (!pool->texture_slots)
            return NVN_FRONT_BOUNDARY;
    }
    /* Replay resolves this association to a host view when the guest binds the
     * descriptor. The Nintendo GPU packet format is separate from that view. */
    TextureDescriptor *slot = &pool->texture_slots[index];
    Texture *old = slot->texture;
    ++texture->references;
    *slot = descriptor;
    if (old)
        release_texture(front, old);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind set_sampler_builder(NvnFront *front, uint32_t offset, const uint64_t x[32],
    const uint32_t scalar[2])
{
    uint8_t *builder = write_guest(front, x[0], SAMPLER_BUILDER_FIELDS_BYTES);
    if (!builder)
        return NVN_FRONT_INVALID_GUEST;
    switch (offset) {
    case SAMPLER_BUILDER_SET_DEVICE:
        write64(builder, x[1]);
        break;
    case SAMPLER_BUILDER_DEFAULTS:
        /* The pinned SDK copies {0, 1000, 0, 1} from rodata +0xae9b40. */
        memset(builder + 8, 0, SAMPLER_BUILDER_FIELDS_BYTES - 8);
        write32(builder + 12, 0x447a0000u);
        write32(builder + 20, 0x3f800000u);
        write32(builder + 44, 0x08110111u);
        break;
    case SAMPLER_BUILDER_SET_FILTER:
        builder[46] = (uint8_t)((x[1] & 0xfu) | (x[2] << 4));
        break;
    case SAMPLER_BUILDER_SET_WRAP:
    case SAMPLER_BUILDER_SET_COMPARE:
    case SAMPLER_BUILDER_SET_REDUCTION: {
        if (!read_guest(front, x[0] + 44, 4))
            return NVN_FRONT_INVALID_GUEST;
        uint32_t packed = read32(builder + 44);
        if (offset == SAMPLER_BUILDER_SET_WRAP)
            packed = (packed & 0xfffff000u) | (uint32_t)((x[1] & 0xfu) | ((x[2] & 0xfu) << 4) | ((x[3] & 0xfu) << 8));
        else if (offset == SAMPLER_BUILDER_SET_COMPARE)
            packed = (packed & 0x00ffffffu) | ((uint32_t)(x[1] & 3u) << 24) | ((uint32_t)x[2] << 26);
        else
            packed = (packed & 0xffff0fffu) | (uint32_t)((x[1] & 0xfu) << 12);
        write32(builder + 44, packed);
        break;
    }
    case SAMPLER_BUILDER_SET_LOD_CLAMP:
    case SAMPLER_BUILDER_SET_LOD_BIAS:
    case SAMPLER_BUILDER_SET_ANISOTROPY:
    case SAMPLER_BUILDER_SET_LOD_SNAP:
        if (!scalar)
            return NVN_FRONT_BOUNDARY;
        if (offset == SAMPLER_BUILDER_SET_LOD_CLAMP) {
            write32(builder + 8, scalar[0]);
            write32(builder + 12, scalar[1]);
        } else if (offset == SAMPLER_BUILDER_SET_LOD_BIAS) {
            write32(builder + 16, scalar[0]);
        } else if (offset == SAMPLER_BUILDER_SET_ANISOTROPY) {
            write32(builder + 20, scalar[0]);
        } else {
            write32(builder + 24, scalar[0]);
        }
        break;
    case SAMPLER_BUILDER_SET_BORDER:
    case SAMPLER_BUILDER_SET_BORDER_I:
    case SAMPLER_BUILDER_SET_BORDER_UI: {
        const uint8_t *color = read_guest(front, x[1], 16);
        if (!color)
            return NVN_FRONT_INVALID_GUEST;
        memmove(builder + 28, color, 16);
        break;
    }
    }
    return NVN_FRONT_HANDLED;
}

static void remove_sampler(Sampler **link)
{
    Sampler *sampler = *link;
    *link = sampler->next;
    free(sampler);
}

static NvnFrontReplyKind initialize_sampler(NvnFront *front, uint64_t x[32])
{
    const uint8_t *builder = read_guest(front, x[1], SAMPLER_BUILDER_FIELDS_BYTES);
    uint8_t *object = write_guest(front, x[0], SAMPLER_BYTES);
    if (!builder || !object)
        return NVN_FRONT_INVALID_GUEST;
    const uint64_t device = read64(builder);
    if (!*find_object(&front->devices, device))
        return NVN_FRONT_INVALID_GUEST;
    Sampler **link = find_sampler(&front->samplers, x[0]);
    if (!*link) {
        *link = calloc(1, sizeof **link);
        if (!*link) {
            x[0] = 0;
            return NVN_FRONT_HANDLED;
        }
    }
    (*link)->address = x[0];
    (*link)->device = device;
    memmove(object + SAMPLER_FIELDS_OFFSET, builder, SAMPLER_BUILDER_FIELDS_BYTES);
    memset(object, 0, SAMPLER_FIELDS_OFFSET);
    memset(object + SAMPLER_FIELDS_OFFSET + SAMPLER_BUILDER_FIELDS_BYTES, 0,
        SAMPLER_BYTES - SAMPLER_FIELDS_OFFSET - SAMPLER_BUILDER_FIELDS_BYTES);
    x[0] = 1;
    return NVN_FRONT_HANDLED;
}

static NvnFrontSamplerInfo decode_sampler(const uint8_t *fields)
{
    const uint32_t packed = read32(fields + 44);
    NvnFrontSamplerInfo info = {
        .min_filter = (packed >> 16) & 0xfu, .mag_filter = (packed >> 20) & 0xfu,
        .wrap = { packed & 0xfu, (packed >> 4) & 0xfu, (packed >> 8) & 0xfu },
        .compare_mode = (packed >> 24) & 3u, .compare_func = packed >> 26,
        .reduction = (packed >> 12) & 0xfu,
        .lod_min = read_float(fields + 8), .lod_max = read_float(fields + 12),
        .lod_bias = read_float(fields + 16), .max_anisotropy = read_float(fields + 20),
        .lod_snap = read_float(fields + 24),
    };
    memcpy(info.border_color_bits, fields + 28, sizeof info.border_color_bits);
    return info;
}

static NvnFrontReplyKind register_sampler_descriptor(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    DescriptorPool *pool = *find_descriptor_pool(&front->descriptor_pools, x[0], true);
    const uint32_t index = (uint32_t)x[1];
    if (!pool || index >= pool->info.count)
        return NVN_FRONT_INVALID_GUEST;
    const uint8_t *fields;
    if (offset == SAMPLER_POOL_REGISTER) {
        const Sampler *sampler = *find_sampler(&front->samplers, x[2]);
        if (!sampler || sampler->device != pool->info.device)
            return NVN_FRONT_INVALID_GUEST;
        const uint8_t *object = read_guest(front, x[2], SAMPLER_BYTES);
        fields = object ? object + SAMPLER_FIELDS_OFFSET : NULL;
    } else {
        fields = read_guest(front, x[2], SAMPLER_BUILDER_FIELDS_BYTES);
    }
    if (!fields || read64(fields) != pool->info.device)
        return NVN_FRONT_INVALID_GUEST;
    if (!pool->sampler_slots) {
        pool->sampler_slots = calloc(pool->info.count, sizeof *pool->sampler_slots);
        if (!pool->sampler_slots)
            return NVN_FRONT_BOUNDARY;
    }
    pool->sampler_slots[index] = (SamplerDescriptor){ .info = decode_sampler(fields), .registered = true };
    return NVN_FRONT_HANDLED;
}

static NvnFrontVertexAttribInfo decode_vertex_attrib(uint32_t packed)
{
    return (NvnFrontVertexAttribInfo){ .format = (packed >> 21) & 0x3ffu,
        .offset = (packed >> 7) & 0x3fffu, .stream = packed & 0x1fu, .enabled = (packed & 0x40u) != 0 };
}

static NvnFrontReplyKind vertex_attrib_state(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    if (offset == VERTEX_ATTRIB_GET_FORMAT || offset == VERTEX_ATTRIB_GET_STREAM) {
        const uint8_t *object = read_guest(front, x[0], VERTEX_ATTRIB_BYTES);
        if (!object)
            return NVN_FRONT_INVALID_GUEST;
        const NvnFrontVertexAttribInfo info = decode_vertex_attrib(read32(object));
        if (offset == VERTEX_ATTRIB_GET_STREAM) {
            x[0] = info.stream;
        } else {
            uint8_t *format_out = write_guest(front, x[1], 4);
            uint8_t *offset_out = write_guest(front, x[2], 8);
            if (!format_out || !offset_out)
                return NVN_FRONT_INVALID_GUEST;
            write32(format_out, info.format);
            write64(offset_out, info.offset);
        }
        return NVN_FRONT_HANDLED;
    }

    uint8_t *object = write_guest(front, x[0], VERTEX_ATTRIB_BYTES);
    if (!object)
        return NVN_FRONT_INVALID_GUEST;
    uint32_t packed = 0;
    if (offset != VERTEX_ATTRIB_DEFAULTS) {
        const uint8_t *previous = read_guest(front, x[0], VERTEX_ATTRIB_BYTES);
        if (!previous)
            return NVN_FRONT_INVALID_GUEST;
        packed = read32(previous);
        if (offset == VERTEX_ATTRIB_SET_FORMAT)
            packed = (packed & 0x8000003fu) | 0x40u | ((uint32_t)x[1] << 21) | ((uint32_t)x[2] << 7);
        else
            packed = (packed & 0xffffffe0u) | (uint32_t)x[1];
    }
    write32(object, packed);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind vertex_stream_state(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    const bool divisor = offset == VERTEX_STREAM_SET_DIVISOR || offset == VERTEX_STREAM_GET_DIVISOR;
    const uint64_t field = x[0] + (divisor ? 4u : 0u);
    if (!x[0] || field < x[0])
        return NVN_FRONT_INVALID_GUEST;
    if (offset == VERTEX_STREAM_GET_STRIDE || offset == VERTEX_STREAM_GET_DIVISOR) {
        const uint8_t *object = read_guest(front, field, 4);
        if (!object)
            return NVN_FRONT_INVALID_GUEST;
        x[0] = read32(object);
        return NVN_FRONT_HANDLED;
    }
    const bool defaults = offset == VERTEX_STREAM_DEFAULTS;
    uint8_t *object = write_guest(front, field, defaults ? VERTEX_STREAM_BYTES : 4);
    if (!object)
        return NVN_FRONT_INVALID_GUEST;
    if (defaults)
        write64(object, 0);
    else
        write32(object, (uint32_t)x[1]);
    return NVN_FRONT_HANDLED;
}

static uint32_t replace_bits(uint32_t packed, uint32_t mask, uint32_t value)
{
    return (packed & ~mask) | value;
}

static NvnFrontReplyKind color_state(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    if ((offset == COLOR_GET_BLEND || offset == COLOR_SET_BLEND) && (uint32_t)x[1] >= COLOR_ATTACHMENTS)
        return NVN_FRONT_INVALID_GUEST;
    NvnFrontColorInfo info;
    if (offset == COLOR_GET_BLEND || offset == COLOR_GET_LOGIC || offset == COLOR_GET_ALPHA) {
        if (!nvn_front_read_color(front, x[0], &info))
            return NVN_FRONT_INVALID_GUEST;
        if (offset == COLOR_GET_BLEND)
            x[0] = (info.blend_mask >> (uint32_t)x[1]) & 1u;
        else
            x[0] = offset == COLOR_GET_LOGIC ? info.logic_op : info.alpha_func;
        return NVN_FRONT_HANDLED;
    }
    uint8_t *object = write_guest(front, x[0], 4);
    if (!object)
        return NVN_FRONT_INVALID_GUEST;
    uint32_t packed = 0x08030000u;
    if (offset != COLOR_DEFAULTS) {
        const uint8_t *previous = read_guest(front, x[0], 4);
        if (!previous)
            return NVN_FRONT_INVALID_GUEST;
        packed = read32(previous);
        if (offset == COLOR_SET_BLEND) {
            const uint32_t bit = 1u << (8u + (uint32_t)x[1]);
            packed = replace_bits(packed, bit, (uint8_t)x[2] ? bit : 0);
        } else if (offset == COLOR_SET_LOGIC) {
            packed = replace_bits(packed, 0xff0000u, (uint32_t)x[1] << 16);
        } else {
            packed = replace_bits(packed, 0xf000000u, (uint32_t)x[1] << 24);
        }
    }
    write32(object, packed);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind channel_mask_state(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    if (offset != MASK_DEFAULTS && (uint32_t)x[1] >= COLOR_ATTACHMENTS)
        return NVN_FRONT_INVALID_GUEST;
    const uint32_t shift = ((uint32_t)x[1] & 7u) * 4u;
    uint32_t packed = UINT32_MAX;
    if (offset != MASK_DEFAULTS && !nvn_front_read_channel_masks(front, x[0], &packed))
        return NVN_FRONT_INVALID_GUEST;
    if (offset == MASK_GET) {
        uint8_t *channels[4];
        for (unsigned i = 0; i < 4; ++i) {
            channels[i] = write_guest(front, x[i + 2], 1);
            if (!channels[i])
                return NVN_FRONT_INVALID_GUEST;
        }
        for (unsigned i = 0; i < 4; ++i)
            *channels[i] = (uint8_t)((packed >> (shift + i)) & 1u);
        return NVN_FRONT_HANDLED;
    }
    uint8_t *object = write_guest(front, x[0], 4);
    if (!object)
        return NVN_FRONT_INVALID_GUEST;
    if (offset == MASK_SET) {
        uint32_t rgba = 0;
        for (unsigned i = 0; i < 4; ++i)
            rgba |= (uint32_t)((uint8_t)x[i + 2] != 0) << i;
        packed = replace_bits(packed, 0xfu << shift, rgba << shift);
    }
    write32(object, packed);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind polygon_state(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    uint32_t shift, mask;
    switch (offset) {
    case POLYGON_SET_CULL:
    case POLYGON_GET_CULL: shift = 0; mask = 3; break;
    case POLYGON_SET_FRONT:
    case POLYGON_GET_FRONT: shift = 2; mask = 1; break;
    case POLYGON_SET_MODE:
    case POLYGON_GET_MODE: shift = 3; mask = 3; break;
    case POLYGON_SET_OFFSET:
    case POLYGON_GET_OFFSET: shift = 15; mask = 0xf; break;
    default: shift = mask = 0; break;
    }
    uint32_t packed = 0x14u;
    if (offset != POLYGON_DEFAULTS) {
        const uint8_t *previous = read_guest(front, x[0], 4);
        if (!previous)
            return NVN_FRONT_INVALID_GUEST;
        packed = read32(previous);
        if (offset >= POLYGON_GET_CULL) {
            x[0] = (packed >> shift) & mask;
            return NVN_FRONT_HANDLED;
        }
        packed = replace_bits(packed, mask << shift, (uint32_t)x[1] << shift);
    }
    uint8_t *object = write_guest(front, x[0], 4);
    if (!object)
        return NVN_FRONT_INVALID_GUEST;
    write32(object, packed);
    return NVN_FRONT_HANDLED;
}

static NvnFrontStencilInfo decode_stencil(uint32_t packed)
{
    return (NvnFrontStencilInfo){ .func = packed & 0xfu, .fail = (packed >> 4) & 0xfu,
        .depth_fail = (packed >> 12) & 0xfu, .depth_pass = (packed >> 8) & 0xfu };
}

static NvnFrontReplyKind depth_stencil_state(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    const bool faces = offset == STENCIL_SET_FUNC || offset == STENCIL_SET_OP ||
        offset == STENCIL_GET_FUNC || offset == STENCIL_GET_OP;
    if (offset == DEPTH_STENCIL_DEFAULTS) {
        uint8_t *object = write_guest(front, x[0], 8);
        if (!object)
            return NVN_FRONT_INVALID_GUEST;
        write64(object, UINT64_C(0x1118111800000022));
        return NVN_FRONT_HANDLED;
    }
    const uint64_t field = x[0] + (faces ? 4u : 0u);
    if (!x[0] || field < x[0])
        return NVN_FRONT_INVALID_GUEST;
    if ((offset == STENCIL_SET_FUNC || offset == STENCIL_SET_OP) && !(x[1] & 3u))
        return NVN_FRONT_HANDLED;
    const uint8_t *previous = read_guest(front, field, 4);
    if (!previous)
        return NVN_FRONT_INVALID_GUEST;
    uint32_t packed = read32(previous);
    if (offset >= DEPTH_GET_TEST) {
        const NvnFrontStencilInfo face = decode_stencil(packed >> ((uint32_t)x[1] == 2 ? 16 : 0));
        switch (offset) {
        case DEPTH_GET_TEST: x[0] = packed & 1u; break;
        case DEPTH_GET_WRITE: x[0] = (packed >> 1) & 1u; break;
        case DEPTH_GET_FUNC: x[0] = (packed >> 4) & 0xfu; break;
        case STENCIL_GET_TEST: x[0] = (packed >> 2) & 1u; break;
        case STENCIL_GET_FUNC: x[0] = face.func; break;
        case STENCIL_GET_OP: {
            uint8_t *fail = write_guest(front, x[2], 4);
            uint8_t *depth_fail = write_guest(front, x[3], 4);
            uint8_t *depth_pass = write_guest(front, x[4], 4);
            if (!fail || !depth_fail || !depth_pass)
                return NVN_FRONT_INVALID_GUEST;
            write32(fail, face.fail);
            write32(depth_fail, face.depth_fail);
            write32(depth_pass, face.depth_pass);
            break;
        }
        }
        return NVN_FRONT_HANDLED;
    }

    switch (offset) {
    case DEPTH_SET_TEST: packed = replace_bits(packed, 1u, (uint8_t)x[1] != 0); break;
    case DEPTH_SET_WRITE: packed = replace_bits(packed, 2u, (uint8_t)x[1] ? 2u : 0); break;
    case DEPTH_SET_FUNC: packed = replace_bits(packed, 0xf0u, (uint32_t)x[1] << 4); break;
    case STENCIL_SET_TEST: packed = replace_bits(packed, 4u, (uint8_t)x[1] ? 4u : 0); break;
    case STENCIL_SET_FUNC:
        if (x[1] & 1u)
            packed = replace_bits(packed, 0xfu, (uint32_t)x[2]);
        if (x[1] & 2u)
            packed = replace_bits(packed, 0xf0000u, (uint32_t)x[2] << 16);
        break;
    case STENCIL_SET_OP:
        if (x[1] & 1u) {
            const uint32_t front_ops = (((uint32_t)x[2] << 4) & 0xffff00ffu) |
                ((uint32_t)x[3] << 12) | ((uint32_t)x[4] << 8);
            packed = replace_bits(packed, 0xfff0u, front_ops);
        }
        if (x[1] & 2u) {
            const uint32_t back_ops = ((uint32_t)(x[2] & 0xfu) << 20) |
                ((uint32_t)x[3] << 28) | ((uint32_t)x[4] << 24);
            packed = replace_bits(packed, 0xfff00000u, back_ops);
        }
        break;
    }
    uint8_t *object = write_guest(front, field, 4);
    if (!object)
        return NVN_FRONT_INVALID_GUEST;
    write32(object, packed);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind blend_state(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    if (offset == BLEND_DEFAULTS) {
        uint8_t *object = write_guest(front, x[0], 8);
        if (!object)
            return NVN_FRONT_INVALID_GUEST;
        write64(object, UINT64_C(0x01020102110c0000));
        return NVN_FRONT_HANDLED;
    }
    if (offset >= BLEND_GET_TARGET) {
        NvnFrontBlendInfo info;
        if (!nvn_front_read_blend(front, x[0], &info))
            return NVN_FRONT_INVALID_GUEST;
        if (offset == BLEND_GET_TARGET) {
            x[0] = info.target;
            return NVN_FRONT_HANDLED;
        }
        const uint32_t factors[] = { info.src_rgb, info.dst_rgb, info.src_alpha, info.dst_alpha };
        const uint32_t equations[] = { info.rgb_equation, info.alpha_equation };
        const uint32_t *values = offset == BLEND_GET_FUNC ? factors : equations;
        const unsigned count = offset == BLEND_GET_FUNC ? 4 : 2;
        uint8_t *outputs[4];
        for (unsigned i = 0; i < count; ++i) {
            outputs[i] = write_guest(front, x[i + 1], 4);
            if (!outputs[i])
                return NVN_FRONT_INVALID_GUEST;
        }
        for (unsigned i = 0; i < count; ++i)
            write32(outputs[i], values[i]);
        return NVN_FRONT_HANDLED;
    }
    const uint64_t field = x[0] + (offset == BLEND_SET_FUNC ? 4u : 0u);
    if (!x[0] || field < x[0])
        return NVN_FRONT_INVALID_GUEST;
    const uint8_t *previous = read_guest(front, field, 4);
    uint8_t *object = write_guest(front, field, 4);
    if (!previous || !object)
        return NVN_FRONT_INVALID_GUEST;
    uint32_t packed = read32(previous);
    if (offset == BLEND_SET_TARGET) {
        packed = replace_bits(packed, 7u, (uint32_t)x[1]);
    } else if (offset == BLEND_SET_EQUATION) {
        const uint32_t equations = (((uint32_t)x[1] << 24) & 0x8fffffffu) | ((uint32_t)x[2] << 28);
        packed = replace_bits(packed, 0x77000000u, equations);
    } else {
        const uint32_t factors = ((uint32_t)x[1] & 0x8080807fu) | (((uint32_t)x[2] << 8) & 0x8080ff00u) |
            (((uint32_t)x[3] << 16) & 0x80ffffffu) | ((uint32_t)x[4] << 24);
        packed = replace_bits(packed, 0x7f7f7f7fu, factors);
    }
    write32(object, packed);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind multisample_state(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    if (offset == MULTISAMPLE_DEFAULTS) {
        uint8_t *object = write_guest(front, x[0], MULTISAMPLE_BYTES);
        if (!object)
            return NVN_FRONT_INVALID_GUEST;
        write32(object, 5);
        memset(object + 4, 0x88, 16);
        write32(object + 20, 0);
        return NVN_FRONT_HANDLED;
    }
    const uint8_t *previous = read_guest(front, x[0], 4);
    if (!previous)
        return NVN_FRONT_INVALID_GUEST;
    uint32_t packed = read32(previous);
    switch (offset) {
    case MULTISAMPLE_GET_ENABLE: x[0] = packed & 1u; return NVN_FRONT_HANDLED;
    case MULTISAMPLE_GET_ALPHA: x[0] = (packed >> 1) & 1u; return NVN_FRONT_HANDLED;
    case MULTISAMPLE_GET_DITHER: x[0] = (packed >> 2) & 1u; return NVN_FRONT_HANDLED;
    case MULTISAMPLE_GET_SAMPLES: x[0] = (packed >> 8) & 0x1fu; return NVN_FRONT_HANDLED;
    case MULTISAMPLE_GET_RASTER_SAMPLES: x[0] = (packed >> 15) & 0x1fu; return NVN_FRONT_HANDLED;
    case MULTISAMPLE_SET_ENABLE: packed = replace_bits(packed, 1u, (uint8_t)x[1] != 0); break;
    case MULTISAMPLE_SET_ALPHA: packed = replace_bits(packed, 2u, (uint8_t)x[1] ? 2u : 0); break;
    case MULTISAMPLE_SET_DITHER: packed = replace_bits(packed, 4u, (uint8_t)x[1] ? 4u : 0); break;
    case MULTISAMPLE_SET_SAMPLES: packed = replace_bits(packed, 0x1f00u, (uint32_t)x[1] << 8); break;
    case MULTISAMPLE_SET_RASTER_SAMPLES: packed = replace_bits(packed, 0xf8000u, (uint32_t)x[1] << 15); break;
    }
    uint8_t *object = write_guest(front, x[0], 4);
    if (!object)
        return NVN_FRONT_INVALID_GUEST;
    write32(object, packed);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind set_texture_view(NvnFront *front, uint32_t offset, const uint64_t x[32])
{
    uint8_t *view = write_guest(front, x[0], TEXTURE_VIEW_BYTES);
    if (!view)
        return NVN_FRONT_INVALID_GUEST;
    if (offset == TEXTURE_VIEW_SET_DEFAULTS) {
        memset(view, 0, TEXTURE_VIEW_BYTES);
        return NVN_FRONT_HANDLED;
    }
    if (!read_guest(front, x[0], TEXTURE_VIEW_BYTES))
        return NVN_FRONT_INVALID_GUEST;

    /* NVN2 packs the selection-presence bits in byte 7. */
    switch (offset) {
    case TEXTURE_VIEW_SET_LEVELS:
        view[4] = (uint8_t)x[1];
        view[5] = (uint8_t)x[2];
        view[7] |= 1u << 0;
        break;
    case TEXTURE_VIEW_SET_LAYERS:
        write32(view, (uint16_t)x[1] | ((uint32_t)(uint16_t)x[2] << 16));
        view[7] |= 1u << 1;
        break;
    case TEXTURE_VIEW_SET_FORMAT:
        write32(view + 8, (uint32_t)x[1]);
        view[7] |= 1u << 2;
        break;
    case TEXTURE_VIEW_SET_SWIZZLE:
        for (unsigned i = 0; i < 4; ++i)
            view[12 + i] = (uint8_t)x[1 + i];
        view[7] |= 1u << 3;
        break;
    case TEXTURE_VIEW_SET_DEPTH_STENCIL:
        view[7] = (uint8_t)((view[7] & 0x3fu) | ((x[1] & 3u) << 6) | (1u << 4));
        break;
    case TEXTURE_VIEW_SET_TARGET:
        view[6] = (uint8_t)x[1];
        view[7] |= 1u << 5;
        break;
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind queue_builder(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    uint8_t *builder = write_guest(front, x[0], QUEUE_BUILDER_BYTES);
    if (!builder)
        return NVN_FRONT_INVALID_GUEST;
    switch (offset) {
    case QUEUE_BUILDER_DEFAULTS:
        /* Native defaults preserve the device pointer at +8. */
        memset(builder + 16, 0, 25);
        memset(builder + 44, 0, 24);
        write32(builder + 68, 100);
        break;
    case QUEUE_BUILDER_SET_DEVICE: write64(builder + 8, x[1]); break;
    case QUEUE_BUILDER_SET_PRIMARY: write64(builder + 24, x[1]); break;
    case QUEUE_BUILDER_SET_FLAGS: write32(builder + 32, (uint32_t)x[1]); break;
    case QUEUE_BUILDER_SET_MEMORY:
        write64(builder + 16, x[1]);
        write32(builder + 60, (uint32_t)x[2]);
        break;
    default: {
        size_t index = (offset - QUEUE_BUILDER_SET_COMMAND) / 16u;
        size_t field = offset == QUEUE_BUILDER_SET_FLUSH ? 64 : 44 + 4 * index;
        write32(builder + field, (uint32_t)x[1]);
        builder[36 + index] = 1;
        break;
    }
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind initialize_device(NvnFront *front, uint64_t x[32])
{
    if (!front->config.device_create)
        return NVN_FRONT_BOUNDARY;
    const uint8_t *builder = read_guest(front, x[1], DEVICE_BUILDER_BYTES);
    uint8_t *object = write_guest(front, x[0], DEVICE_BYTES);
    if (!object || !builder || *find_object(&front->devices, x[0]))
        return NVN_FRONT_INVALID_GUEST;

    Object *dev = calloc(1, sizeof *dev);
    if (dev) {
        dev->native = front->config.device_create(front->config.backend_opaque, read32(builder + 4));
        if (dev->native) {
            memset(object, 0, DEVICE_BYTES);
            dev->address = x[0];
            dev->modes = (NvnFrontDeviceModes){
                .window_origin = NVN_FRONT_WINDOW_ORIGIN_LOWER_LEFT,
                .depth_mode = NVN_FRONT_DEPTH_NEAR_IS_MINUS_W,
            };
            dev->next = front->devices;
            front->devices = dev;
            x[0] = 1;
            return NVN_FRONT_HANDLED;
        }
        free(dev);
    }
    x[0] = 0;
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind initialize_queue(NvnFront *front, uint64_t x[32])
{
    if (!front->config.queue_create)
        return NVN_FRONT_BOUNDARY;
    const uint8_t *builder = read_guest(front, x[1], QUEUE_BUILDER_BYTES);
    uint8_t *object = write_guest(front, x[0], QUEUE_BYTES);
    if (!object || !builder || *find_object(&front->queues, x[0]))
        return NVN_FRONT_INVALID_GUEST;

    Object *device = *find_object(&front->devices, read64(builder + 8));
    uint64_t primary_address = read64(builder + 24);
    Object *primary = *find_object(&front->queues, primary_address);
    if (!device || (primary_address && (!primary || primary->device != device)))
        return NVN_FRONT_INVALID_GUEST;

    NvnFrontQueueConfig config = {
        .primary_queue = primary ? primary->native : NULL,
        .queue_memory = read64(builder + 16), .flags = read32(builder + 32),
        .command_bytes = read32(builder + 44), .compute_bytes = read32(builder + 48),
        .control_bytes = read32(builder + 52), .ray_bytes = read32(builder + 56),
        .queue_memory_bytes = read32(builder + 60), .flush_threshold = read32(builder + 64),
        .priority = read32(builder + 68),
    };
    for (size_t i = 0; i < 5; ++i)
        if (builder[36 + i])
            config.overrides |= 1u << i;
    if (config.queue_memory_bytes && !write_guest(front, config.queue_memory, config.queue_memory_bytes))
        return NVN_FRONT_INVALID_GUEST;

    Object *queue = calloc(1, sizeof *queue);
    if (queue) {
        queue->native = front->config.queue_create(front->config.backend_opaque, device->native, &config);
        if (queue->native) {
            memset(object, 0, QUEUE_BYTES);
            queue->address = x[0];
            queue->device = device;
            queue->next = front->queues;
            front->queues = queue;
            x[0] = 1;
            return NVN_FRONT_HANDLED;
        }
        free(queue);
    }
    x[0] = 0;
    return NVN_FRONT_HANDLED;
}

static void finalize(NvnFront *front, Object **link, void (*destroy)(void *, void *))
{
    Object *object = *link;
    *link = object->next;
    destroy(front->config.backend_opaque, object->native);
    free(object);
}

static NvnFrontReplyKind initialize_sync(NvnFront *front, uint64_t x[32])
{
    uint8_t *object = write_guest(front, x[0], SYNC_BYTES);
    Object *device = *find_object(&front->devices, x[1]);
    if (!object || !device)
        return NVN_FRONT_INVALID_GUEST;
    if (!front->config.sync_create)
        return NVN_FRONT_BOUNDARY;

    Object **link = find_object(&front->syncs, x[0]);
    Object *old = *link;
    Object *sync = old ? old : malloc(sizeof *sync);
    void *native = sync ? front->config.sync_create(front->config.backend_opaque, device->native) : NULL;
    if (!native) {
        if (!old)
            free(sync);
        x[0] = 0;
        return NVN_FRONT_HANDLED;
    }

    if (old)
        front->config.sync_destroy(front->config.backend_opaque, old->native);
    *sync = (Object){ .next = old ? old->next : NULL, .device = device, .address = x[0], .native = native };
    *link = sync;
    /* The SDK initializes its ordinary sync fields and leaves the reserved tail. */
    write64(object, x[1]);
    memset(object + 8, 0, SYNC_INITIALIZED_BYTES - 8);
    x[0] = 1;
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind initialize_command_buffer(NvnFront *front, uint64_t x[32])
{
    uint8_t *object = write_guest(front, x[0], COMMAND_BUFFER_BYTES);
    const Object *device = *find_object(&front->devices, x[1]);
    const uint8_t *guest_device = read_guest(front, x[1], DEVICE_BYTES);
    if (!object || !device || !guest_device)
        return NVN_FRONT_INVALID_GUEST;
    if (!front->config.command_buffer_create)
        return NVN_FRONT_BOUNDARY;

    CommandBuffer **link = find_command_buffer(&front->command_buffers, x[0]);
    CommandBuffer *old = *link;
    CommandBuffer *command = old ? old : malloc(sizeof *command);
    void *native = command ? front->config.command_buffer_create(front->config.backend_opaque, device->native) : NULL;
    if (!native) {
        if (!old)
            free(command);
        x[0] = 0;
        return NVN_FRONT_HANDLED;
    }

    if (old)
        front->config.command_buffer_destroy(front->config.backend_opaque, old->info.native);
    *command = (CommandBuffer){
        .next = old ? old->next : NULL,
        .address = x[0],
        .info = { .native = native, .device = x[1] },
    };
    *link = command;

    /* NVN2 initializes 0xb0 bytes of the 0xc0-byte object. */
    const uint8_t device_command_flag = (uint8_t)((guest_device[0x1c48] & 1u) << 3);
    const uint8_t device_flag = (uint8_t)((read32(guest_device + 8) >> 18) & 1u);
    memset(object, 0, COMMAND_BUFFER_INITIALIZED_BYTES);
    object[0x13] = device_command_flag;
    object[0x14] = 1;
    object[0x15] = 1;
    object[0xa8] = device_flag;
    write64(object + 0x70, x[1]);
    x[0] = 1;
    return NVN_FRONT_HANDLED;
}

static void remove_command_buffer(NvnFront *front, CommandBuffer **link)
{
    CommandBuffer *command = *link;
    *link = command->next;
    front->config.command_buffer_destroy(front->config.backend_opaque, command->info.native);
    free(command);
}

static NvnFrontReplyKind configure_command_buffer(NvnFront *front, uint32_t offset, const uint64_t x[32])
{
    CommandBuffer *command = *find_command_buffer(&front->command_buffers, x[0]);
    uint8_t *object = write_guest(front, x[0], COMMAND_BUFFER_BYTES);
    if (!command || !object)
        return NVN_FRONT_INVALID_GUEST;
    NvnFrontCommandBufferInfo *info = &command->info;
    switch (offset) {
    case COMMAND_BUFFER_SET_CALLBACK:
        info->memory_callback = x[1];
        write64(object + 0x78, x[1]);
        break;
    case COMMAND_BUFFER_SET_CALLBACK_DATA:
        info->memory_callback_data = x[1];
        write64(object + 0x80, x[1]);
        break;
    case COMMAND_BUFFER_SET_RECORDING_MODE:
        info->recording_mode = (uint32_t)x[1];
        write32(object + 0x18, info->recording_mode);
        break;
    case COMMAND_BUFFER_ADD_COMMAND: {
        const uint64_t pool_offset = x[2], size = x[3];
        NvnFrontPoolConfig pool;
        if (!nvn_front_get_pool(front, x[1], &pool) || pool.device != info->device ||
            (pool.flags & (POOL_CPU_NO_ACCESS | POOL_GPU_NO_ACCESS)) ||
            pool_offset >= pool.size || size > pool.size - pool_offset || size > SIZE_MAX)
            return NVN_FRONT_INVALID_GUEST;
        const uint64_t address = pool.storage + pool_offset;
        if ((address & 3) || !write_guest(front, address, (size_t)size))
            return NVN_FRONT_INVALID_GUEST;

        info->command_pool = x[1];
        info->command_offset = pool_offset;
        info->command_address = address;
        info->command_size = size;
        info->command_used = 0;
        ++info->command_revision;
        /* The unrecorded NVN2 command span has no pending packets or footer. */
        const uint64_t stream_size = size < 0x7ffffc ? size : 0x7ffffc;
        write64(object + 0x00, address);
        write64(object + 0x08, address + stream_size);
        write64(object + 0x20, address + size);
        write64(object + 0x38, address);
        write64(object + 0x48, address);
        write64(object + 0x60, address);
        break;
    }
    case COMMAND_BUFFER_ADD_CONTROL: {
        const uint64_t address = x[1], size = x[2];
        if ((address & 7) || size < 16 || size > SIZE_MAX || !write_guest(front, address, (size_t)size))
            return NVN_FRONT_INVALID_GUEST;
        info->control_address = address;
        info->control_size = size;
        info->control_used = 0;
        ++info->control_revision;
        write64(object + 0x28, address);
        write64(object + 0x30, address + size - 16);
        write64(object + 0x40, address);
        write64(object + 0x50, address);
        write64(object + 0x58, 0);
        break;
    }
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind get_command_memory(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    const CommandBuffer *command = *find_command_buffer(&front->command_buffers, x[0]);
    if (!command)
        return NVN_FRONT_INVALID_GUEST;
    const NvnFrontCommandBufferInfo *info = &command->info;
    /* The SDK adds its 16-byte control footer reserve back when computing
     * size/free, including before AddControlMemory initializes the span. */
    const uint64_t control_size = info->control_size ? info->control_size : 16;
    switch (offset) {
    case COMMAND_BUFFER_GET_COMMAND_SIZE: x[0] = info->command_size; break;
    case COMMAND_BUFFER_GET_COMMAND_USED: x[0] = info->command_used; break;
    case COMMAND_BUFFER_GET_COMMAND_FREE: x[0] = info->command_size - info->command_used; break;
    case COMMAND_BUFFER_GET_CONTROL_SIZE: x[0] = control_size; break;
    case COMMAND_BUFFER_GET_CONTROL_USED: x[0] = info->control_used; break;
    case COMMAND_BUFFER_GET_CONTROL_FREE: x[0] = control_size - info->control_used; break;
    }
    return NVN_FRONT_HANDLED;
}

static void remove_program(NvnFront *front, Program **link)
{
    Program *program = *link;
    *link = program->next;
    for (Program **bucket = &front->program_buckets[cache_slot(program->address, 4096)]; *bucket;
         bucket = &(*bucket)->bucket_next)
        if (*bucket == program) {
            *bucket = program->bucket_next;
            break;
        }
    forget_program(front, program);
    front->config.program_destroy(front->config.backend_opaque, program->info.native);
    free(program);
}

static NvnFrontReplyKind initialize_program(NvnFront *front, uint64_t x[32])
{
    const Object *device = *find_object(&front->devices, x[1]);
    uint8_t *object = write_guest(front, x[0], PROGRAM_BYTES);
    if (!device || !object)
        return NVN_FRONT_INVALID_GUEST;
    if (!front->config.program_create)
        return NVN_FRONT_BOUNDARY;
    Program *program = calloc(1, sizeof *program);
    if (program)
        program->info.native = front->config.program_create(front->config.backend_opaque, device->native);
    if (!program || !program->info.native) {
        free(program);
        x[0] = 0;
        return NVN_FRONT_HANDLED;
    }

    Program **link = find_program(&front->programs, x[0]);
    if (*link)
        remove_program(front, link);
    program->address = x[0];
    program->info.device = x[1];
    program->next = *link;
    *link = program;
    Program **bucket = &front->program_buckets[cache_slot(program->address, 4096)];
    program->bucket_next = *bucket;
    *bucket = program;
    memset(front->program_misses, 0, sizeof front->program_misses);
    /* The backend tracks the device's program list independently of the SDK. */
    memset(object, 0, PROGRAM_BYTES);
    write64(object + PROGRAM_DEVICE, x[1]);
    x[0] = 1;
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind program_operation(NvnFront *front, uint32_t offset, uint64_t x[32])
{
    Program **link = find_program(&front->programs, x[0]);
    Program *program = *link;
    if (!program)
        return NVN_FRONT_INVALID_GUEST;
    if (offset == PROGRAM_FINALIZE) {
        remove_program(front, link);
        return NVN_FRONT_HANDLED;
    }
    if (!front->config.program_link)
        return NVN_FRONT_BOUNDARY;

    const uint32_t count = (uint32_t)x[1];
    if (count > INT32_MAX)
        return NVN_FRONT_INVALID_GUEST;
    if (count > PROGRAM_MAX_STAGES)
        return NVN_FRONT_BOUNDARY;
    const uint8_t *entries = read_guest(front, x[2], count * SHADER_ENTRY_BYTES);
    if (count && !entries)
        return NVN_FRONT_INVALID_GUEST;

    NvnFrontShaderData shaders[PROGRAM_MAX_STAGES] = {0};
    for (uint32_t i = 0; i < count; ++i) {
        NvnFrontShaderData *shader = &shaders[i];
        shader->code_address = read64(entries + i * SHADER_ENTRY_BYTES);
        shader->control_address = read64(entries + i * SHADER_ENTRY_BYTES + 8);
        shader->control = read_guest(front, shader->control_address, NVN_SHADER_CONTROL_PREFIX_BYTES);
        if (!shader->control || !shader->code_address)
            return NVN_FRONT_INVALID_GUEST;
        if (!nvn_shader_control_info(shader->control, NVN_SHADER_CONTROL_PREFIX_BYTES, &shader->parsed))
            return NVN_FRONT_BOUNDARY;

        NvnFrontPoolHit hit;
        if (!nvn_front_resolve_pool(front, program->info.device, shader->code_address, shader->parsed.code_bytes, &hit))
            return NVN_FRONT_INVALID_GUEST;
        shader->code = read_guest(front, shader->code_address, shader->parsed.code_bytes);
        if (!shader->code)
            return NVN_FRONT_INVALID_GUEST;
    }

    const NvnFrontProgramLinkResult result = front->config.program_link(front->config.backend_opaque,
        program->info.native, shaders, count);
    if (result != NVN_FRONT_PROGRAM_LINKED && result != NVN_FRONT_PROGRAM_FAILED &&
        result != NVN_FRONT_PROGRAM_CACHE_MISS)
        return NVN_FRONT_BOUNDARY;
    program->info.linked = result == NVN_FRONT_PROGRAM_LINKED && count;
    program->info.has_unavailable_shader = result == NVN_FRONT_PROGRAM_CACHE_MISS;
    /* A verified cache miss does not invalidate the guest's shader data. */
    x[0] = result != NVN_FRONT_PROGRAM_FAILED;
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind dispatch(NvnFront *front, uint32_t offset, uint64_t x[32], const uint32_t scalar[2])
{
    switch (offset) {
    case INSTALL_GLOBAL_DEBUG_CALLBACK:
        /* The pinned release SDK's exported function returns immediately. */
        return NVN_FRONT_HANDLED;
    case DEVICE_GET_INTEGER: return device_integer(front, x);
    case DEVICE_GET_IMAGE_HANDLE:
    case DEVICE_GET_SEPARATE_SAMPLER_HANDLE:
    case DEVICE_GET_SEPARATE_TEXTURE_HANDLE:
    case DEVICE_GET_TEXEL_FETCH_HANDLE:
    case DEVICE_GET_TEXTURE_HANDLE:
        return device_descriptor_handle(front, offset, x);
    case DEVICE_BUILDER_DEFAULTS:
    case DEVICE_BUILDER_GET_FLAGS:
    case DEVICE_BUILDER_SET_FLAGS:
        return device_builder(front, offset, x);
    case DEVICE_INITIALIZE: return initialize_device(front, x);
    case DEVICE_SET_DEPTH_MODE:
    case DEVICE_SET_WINDOW_ORIGIN:
        return set_device_mode(front, offset, x);
    case POOL_BUILDER_GET_DEVICE:
    case POOL_BUILDER_GET_MEMORY:
    case POOL_BUILDER_GET_SIZE:
    case POOL_BUILDER_GET_FLAGS:
        return get_pool_builder(front, offset, x);
    case POOL_BUILDER_SET_DEVICE:
    case POOL_BUILDER_SET_FLAGS:
    case POOL_BUILDER_DEFAULTS:
    case POOL_BUILDER_SET_STORAGE:
        return set_pool_builder(front, offset, x);
    case POOL_INITIALIZE: return initialize_pool(front, x);
    case POOL_FINALIZE:
    case POOL_MAP:
    case POOL_FLUSH:
    case POOL_INVALIDATE:
    case POOL_GET_BUFFER_ADDRESS:
    case POOL_GET_SIZE:
    case POOL_GET_FLAGS:
        return pool_operation(front, offset, x);
    case BUFFER_BUILDER_SET_DEVICE:
    case BUFFER_BUILDER_DEFAULTS:
    case BUFFER_BUILDER_SET_STORAGE:
        return set_buffer_builder(front, offset, x);
    case BUFFER_BUILDER_GET_DEVICE:
    case BUFFER_BUILDER_GET_POOL:
    case BUFFER_BUILDER_GET_OFFSET:
    case BUFFER_BUILDER_GET_SIZE:
        return get_buffer_builder(front, offset, x);
    case BUFFER_INITIALIZE: return initialize_buffer(front, x);
    case BUFFER_FINALIZE:
    case BUFFER_MAP:
    case BUFFER_FLUSH:
    case BUFFER_INVALIDATE:
    case BUFFER_GET_ADDRESS:
    case BUFFER_GET_POOL:
    case BUFFER_GET_OFFSET:
    case BUFFER_GET_SIZE:
        return buffer_operation(front, offset, x);
    case PROGRAM_INITIALIZE: return initialize_program(front, x);
    case PROGRAM_FINALIZE:
    case PROGRAM_SET_SHADERS:
        return program_operation(front, offset, x);
    case TEXTURE_BUILDER_SET_DEVICE:
    case TEXTURE_BUILDER_DEFAULTS:
    case TEXTURE_BUILDER_SET_FLAGS:
    case TEXTURE_BUILDER_SET_TARGET:
    case TEXTURE_BUILDER_SET_WIDTH:
    case TEXTURE_BUILDER_SET_HEIGHT:
    case TEXTURE_BUILDER_SET_DEPTH:
    case TEXTURE_BUILDER_SET_SIZE_1D:
    case TEXTURE_BUILDER_SET_SIZE_2D:
    case TEXTURE_BUILDER_SET_SIZE_3D:
    case TEXTURE_BUILDER_SET_LEVELS:
    case TEXTURE_BUILDER_SET_FORMAT:
    case TEXTURE_BUILDER_SET_SAMPLES:
    case TEXTURE_BUILDER_SET_SWIZZLE:
    case TEXTURE_BUILDER_SET_DEPTH_STENCIL:
    case TEXTURE_BUILDER_SET_STORAGE:
    case TEXTURE_BUILDER_SET_GL_NAME:
    case TEXTURE_BUILDER_SET_STRIDE:
    case TEXTURE_BUILDER_SET_PACKAGED_DATA:
    case TEXTURE_BUILDER_SET_PACKAGED_LAYOUT:
        return set_texture_builder(front, offset, x);
    case TEXTURE_BUILDER_GET_WIDTH:
    case TEXTURE_BUILDER_GET_HEIGHT:
    case TEXTURE_BUILDER_GET_ALIGNMENT:
    case TEXTURE_BUILDER_GET_SIZE:
        return texture_query(front, offset, x);
    case TEXTURE_INITIALIZE: return initialize_texture(front, x);
    case TEXTURE_FINALIZE: return finalize_texture(front, x[0]);
    case TEXTURE_POOL_INITIALIZE: return initialize_descriptor_pool(front, false, x);
    case SAMPLER_POOL_INITIALIZE: return initialize_descriptor_pool(front, true, x);
    case TEXTURE_POOL_REGISTER_TEXTURE:
    case TEXTURE_POOL_REGISTER_IMAGE:
        return register_texture_descriptor(front, offset, x);
    case SAMPLER_POOL_REGISTER:
    case SAMPLER_POOL_REGISTER_BUILDER:
        return register_sampler_descriptor(front, offset, x);
    case SAMPLER_BUILDER_SET_DEVICE:
    case SAMPLER_BUILDER_DEFAULTS:
    case SAMPLER_BUILDER_SET_FILTER:
    case SAMPLER_BUILDER_SET_WRAP:
    case SAMPLER_BUILDER_SET_LOD_CLAMP:
    case SAMPLER_BUILDER_SET_LOD_BIAS:
    case SAMPLER_BUILDER_SET_COMPARE:
    case SAMPLER_BUILDER_SET_BORDER:
    case SAMPLER_BUILDER_SET_BORDER_I:
    case SAMPLER_BUILDER_SET_BORDER_UI:
    case SAMPLER_BUILDER_SET_ANISOTROPY:
    case SAMPLER_BUILDER_SET_REDUCTION:
    case SAMPLER_BUILDER_SET_LOD_SNAP:
        return set_sampler_builder(front, offset, x, scalar);
    case SAMPLER_INITIALIZE: return initialize_sampler(front, x);
    case VERTEX_ATTRIB_DEFAULTS:
    case VERTEX_ATTRIB_SET_FORMAT:
    case VERTEX_ATTRIB_SET_STREAM:
    case VERTEX_ATTRIB_GET_FORMAT:
    case VERTEX_ATTRIB_GET_STREAM:
        return vertex_attrib_state(front, offset, x);
    case VERTEX_STREAM_DEFAULTS:
    case VERTEX_STREAM_SET_STRIDE:
    case VERTEX_STREAM_SET_DIVISOR:
    case VERTEX_STREAM_GET_STRIDE:
    case VERTEX_STREAM_GET_DIVISOR:
        return vertex_stream_state(front, offset, x);
    case COLOR_DEFAULTS:
    case COLOR_SET_BLEND:
    case COLOR_SET_LOGIC:
    case COLOR_SET_ALPHA:
    case COLOR_GET_BLEND:
    case COLOR_GET_LOGIC:
    case COLOR_GET_ALPHA:
        return color_state(front, offset, x);
    case MASK_DEFAULTS:
    case MASK_SET:
    case MASK_GET:
        return channel_mask_state(front, offset, x);
    case POLYGON_DEFAULTS:
    case POLYGON_SET_CULL:
    case POLYGON_SET_FRONT:
    case POLYGON_SET_MODE:
    case POLYGON_SET_OFFSET:
    case POLYGON_GET_CULL:
    case POLYGON_GET_FRONT:
    case POLYGON_GET_MODE:
    case POLYGON_GET_OFFSET:
        return polygon_state(front, offset, x);
    case DEPTH_STENCIL_DEFAULTS:
    case DEPTH_SET_TEST:
    case DEPTH_SET_WRITE:
    case DEPTH_SET_FUNC:
    case STENCIL_SET_TEST:
    case STENCIL_SET_FUNC:
    case STENCIL_SET_OP:
    case DEPTH_GET_TEST:
    case DEPTH_GET_WRITE:
    case DEPTH_GET_FUNC:
    case STENCIL_GET_TEST:
    case STENCIL_GET_FUNC:
    case STENCIL_GET_OP:
        return depth_stencil_state(front, offset, x);
    case BLEND_DEFAULTS:
    case BLEND_SET_TARGET:
    case BLEND_SET_FUNC:
    case BLEND_SET_EQUATION:
    case BLEND_GET_TARGET:
    case BLEND_GET_FUNC:
    case BLEND_GET_EQUATION:
        return blend_state(front, offset, x);
    case MULTISAMPLE_DEFAULTS:
    case MULTISAMPLE_SET_ENABLE:
    case MULTISAMPLE_SET_ALPHA:
    case MULTISAMPLE_SET_DITHER:
    case MULTISAMPLE_SET_SAMPLES:
    case MULTISAMPLE_SET_RASTER_SAMPLES:
    case MULTISAMPLE_GET_ENABLE:
    case MULTISAMPLE_GET_ALPHA:
    case MULTISAMPLE_GET_DITHER:
    case MULTISAMPLE_GET_SAMPLES:
    case MULTISAMPLE_GET_RASTER_SAMPLES:
        return multisample_state(front, offset, x);
    case SAMPLER_FINALIZE: {
        Sampler **sampler = find_sampler(&front->samplers, x[0]);
        if (!*sampler)
            return NVN_FRONT_INVALID_GUEST;
        remove_sampler(sampler);
        return NVN_FRONT_HANDLED;
    }
    case TEXTURE_POOL_FINALIZE:
    case TEXTURE_POOL_GET_MEMORY:
    case TEXTURE_POOL_GET_OFFSET:
    case TEXTURE_POOL_GET_SIZE:
        return descriptor_pool_operation(front, offset, false, x);
    case SAMPLER_POOL_FINALIZE:
    case SAMPLER_POOL_GET_MEMORY:
    case SAMPLER_POOL_GET_OFFSET:
    case SAMPLER_POOL_GET_SIZE:
        return descriptor_pool_operation(front, offset, true, x);
    case TEXTURE_GET_FLAGS:
    case TEXTURE_GET_TARGET:
    case TEXTURE_GET_WIDTH:
    case TEXTURE_GET_HEIGHT:
    case TEXTURE_GET_DEPTH:
    case TEXTURE_GET_LEVELS:
    case TEXTURE_GET_FORMAT:
    case TEXTURE_GET_SAMPLES:
    case TEXTURE_GET_STORAGE_SIZE: {
        const Texture *texture = lookup_texture(front, x[0]);
        if (!texture)
            return NVN_FRONT_INVALID_GUEST;
        const NvnFrontTextureConfig *config = &texture->info.create_info.config;
        switch (offset) {
        case TEXTURE_GET_FLAGS: x[0] = config->flags & 0x7fffu; break;
        case TEXTURE_GET_TARGET: x[0] = config->target; break;
        case TEXTURE_GET_WIDTH: x[0] = config->width; break;
        case TEXTURE_GET_HEIGHT: x[0] = config->height; break;
        case TEXTURE_GET_DEPTH: x[0] = config->depth; break;
        case TEXTURE_GET_LEVELS: x[0] = config->levels; break;
        case TEXTURE_GET_FORMAT: x[0] = config->format; break;
        case TEXTURE_GET_SAMPLES: x[0] = config->samples; break;
        default: x[0] = (uint32_t)texture->info.create_info.storage_size; break;
        }
        return NVN_FRONT_HANDLED;
    }
    case TEXTURE_VIEW_SET_DEFAULTS:
    case TEXTURE_VIEW_SET_LEVELS:
    case TEXTURE_VIEW_SET_LAYERS:
    case TEXTURE_VIEW_SET_FORMAT:
    case TEXTURE_VIEW_SET_SWIZZLE:
    case TEXTURE_VIEW_SET_DEPTH_STENCIL:
    case TEXTURE_VIEW_SET_TARGET:
        return set_texture_view(front, offset, x);
    case SYNC_INITIALIZE: return initialize_sync(front, x);
    case SYNC_FINALIZE: {
        Object **sync = find_object(&front->syncs, x[0]);
        if (!*sync)
            return NVN_FRONT_INVALID_GUEST;
        finalize(front, sync, front->config.sync_destroy);
        return NVN_FRONT_HANDLED;
    }
    case COMMAND_BUFFER_INITIALIZE: return initialize_command_buffer(front, x);
    case COMMAND_BUFFER_FINALIZE: {
        CommandBuffer **command = find_command_buffer(&front->command_buffers, x[0]);
        if (!*command)
            return NVN_FRONT_INVALID_GUEST;
        remove_command_buffer(front, command);
        return NVN_FRONT_HANDLED;
    }
    case COMMAND_BUFFER_SET_CALLBACK:
    case COMMAND_BUFFER_SET_CALLBACK_DATA:
    case COMMAND_BUFFER_SET_RECORDING_MODE:
    case COMMAND_BUFFER_ADD_COMMAND:
    case COMMAND_BUFFER_ADD_CONTROL:
        return configure_command_buffer(front, offset, x);
    case COMMAND_BUFFER_GET_COMMAND_SIZE:
    case COMMAND_BUFFER_GET_COMMAND_USED:
    case COMMAND_BUFFER_GET_COMMAND_FREE:
    case COMMAND_BUFFER_GET_CONTROL_SIZE:
    case COMMAND_BUFFER_GET_CONTROL_USED:
    case COMMAND_BUFFER_GET_CONTROL_FREE:
        return get_command_memory(front, offset, x);
    case QUEUE_BUILDER_DEFAULTS:
    case QUEUE_BUILDER_SET_DEVICE:
    case QUEUE_BUILDER_SET_FLAGS:
    case QUEUE_BUILDER_SET_COMMAND:
    case QUEUE_BUILDER_SET_COMPUTE:
    case QUEUE_BUILDER_SET_CONTROL:
    case QUEUE_BUILDER_SET_RAY:
    case QUEUE_BUILDER_SET_FLUSH:
    case QUEUE_BUILDER_SET_MEMORY:
    case QUEUE_BUILDER_SET_PRIMARY:
        return queue_builder(front, offset, x);
    case QUEUE_INITIALIZE: return initialize_queue(front, x);
    case QUEUE_FINALIZE: {
        Object **queue = find_object(&front->queues, x[0]);
        if (!*queue)
            return NVN_FRONT_INVALID_GUEST;
        finalize(front, queue, front->config.queue_destroy);
        return NVN_FRONT_HANDLED;
    }
    case DEVICE_FINALIZE: {
        Object **device = find_object(&front->devices, x[0]);
        if (!*device)
            return NVN_FRONT_INVALID_GUEST;
        for (const Object *queue = front->queues; queue; queue = queue->next)
            if (queue->device == *device)
                return NVN_FRONT_INVALID_GUEST;
        for (const Object *sync = front->syncs; sync; sync = sync->next)
            if (sync->device == *device)
                return NVN_FRONT_INVALID_GUEST;
        for (const CommandBuffer *command = front->command_buffers; command; command = command->next)
            if (command->info.device == x[0])
                return NVN_FRONT_INVALID_GUEST;
        for (const Program *program = front->programs; program; program = program->next)
            if (program->info.device == x[0])
                return NVN_FRONT_INVALID_GUEST;
        for (const Sampler *sampler = front->samplers; sampler; sampler = sampler->next)
            if (sampler->device == x[0])
                return NVN_FRONT_INVALID_GUEST;
        for (const Pool *pool = front->pools; pool; pool = pool->next)
            if (pool->config.device == x[0])
                return NVN_FRONT_INVALID_GUEST;
        finalize(front, device, front->config.device_destroy);
        return NVN_FRONT_HANDLED;
    }
    default:
        return NVN_FRONT_BOUNDARY;
    }
}

NvnFront *nvn_front_create(const NvnFrontConfig *config)
{
    if (!config || !config->read_guest || !config->write_guest ||
        !!config->device_create != !!config->device_destroy ||
        !!config->queue_create != !!config->queue_destroy ||
        !!config->texture_create != !!config->texture_destroy ||
        !!config->sync_create != !!config->sync_destroy ||
        !!config->command_buffer_create != !!config->command_buffer_destroy ||
        !!config->program_create != !!config->program_destroy ||
        (config->program_link && !config->program_create))
        return NULL;
    uint64_t base = config->thunk_base ? config->thunk_base : NVN_FRONT_DEFAULT_THUNK_BASE;
    if ((base & 3) || base > UINT64_C(0x8000000000) - PROC_COUNT * 4u ||
        (base < sdk_base + 0x640000 && base + PROC_COUNT * 4u > sdk_base))
        return NULL;
    NvnFront *front = calloc(1, sizeof *front);
    if (front) {
        front->config = *config;
        front->config.thunk_base = base;
    }
    return front;
}

bool nvn_front_may_own(const NvnFront *front, uint64_t pc)
{
    if (!front)
        return false;
    /* Mirrors proc_at(): the thunk table, then the SDK's NVN code range
     * (which contains DEVICE_GET_PROC_INTERNAL). */
    if (pc - front->config.thunk_base < PROC_COUNT * 4u)
        return true;
    return pc >= sdk_base + 0x5c0000 && pc < sdk_base + 0x640000;
}

/* R60: entry points whose handlers only read and write the guest structure
 * passed in (builders and state objects). They never touch the front's object
 * lists, so the loader may run them without its global lock. Audited handlers:
 * device_builder, set/get_pool_builder, set/get_buffer_builder,
 * set_texture_builder, set_sampler_builder, vertex_attrib_state,
 * vertex_stream_state, color_state, channel_mask_state, polygon_state,
 * depth_stencil_state, blend_state, multisample_state. texture_query is
 * excluded (find_object). Anything not listed keeps the locked path. */
static bool pure_offset(uint32_t offset)
{
    switch (offset) {
    case DEVICE_BUILDER_DEFAULTS: case DEVICE_BUILDER_GET_FLAGS: case DEVICE_BUILDER_SET_FLAGS:
    case POOL_BUILDER_GET_DEVICE: case POOL_BUILDER_GET_MEMORY: case POOL_BUILDER_GET_SIZE:
    case POOL_BUILDER_GET_FLAGS: case POOL_BUILDER_SET_DEVICE: case POOL_BUILDER_SET_FLAGS:
    case POOL_BUILDER_DEFAULTS: case POOL_BUILDER_SET_STORAGE:
    case BUFFER_BUILDER_SET_DEVICE: case BUFFER_BUILDER_DEFAULTS: case BUFFER_BUILDER_SET_STORAGE:
    case BUFFER_BUILDER_GET_DEVICE: case BUFFER_BUILDER_GET_POOL: case BUFFER_BUILDER_GET_OFFSET:
    case BUFFER_BUILDER_GET_SIZE: case BUFFER_GET_ADDRESS:
    case TEXTURE_BUILDER_SET_DEVICE: case TEXTURE_BUILDER_DEFAULTS: case TEXTURE_BUILDER_SET_FLAGS:
    case TEXTURE_BUILDER_SET_TARGET: case TEXTURE_BUILDER_SET_WIDTH: case TEXTURE_BUILDER_SET_HEIGHT:
    case TEXTURE_BUILDER_SET_DEPTH: case TEXTURE_BUILDER_SET_SIZE_1D: case TEXTURE_BUILDER_SET_SIZE_2D:
    case TEXTURE_BUILDER_SET_SIZE_3D: case TEXTURE_BUILDER_SET_LEVELS: case TEXTURE_BUILDER_SET_FORMAT:
    case TEXTURE_BUILDER_SET_SAMPLES: case TEXTURE_BUILDER_SET_SWIZZLE: case TEXTURE_BUILDER_SET_DEPTH_STENCIL:
    case TEXTURE_BUILDER_SET_STORAGE: case TEXTURE_BUILDER_SET_GL_NAME: case TEXTURE_BUILDER_SET_STRIDE:
    case TEXTURE_BUILDER_SET_PACKAGED_DATA: case TEXTURE_BUILDER_SET_PACKAGED_LAYOUT:
    case SAMPLER_BUILDER_SET_DEVICE: case SAMPLER_BUILDER_DEFAULTS: case SAMPLER_BUILDER_SET_FILTER:
    case SAMPLER_BUILDER_SET_WRAP: case SAMPLER_BUILDER_SET_LOD_CLAMP: case SAMPLER_BUILDER_SET_LOD_BIAS:
    case SAMPLER_BUILDER_SET_COMPARE: case SAMPLER_BUILDER_SET_BORDER: case SAMPLER_BUILDER_SET_BORDER_I:
    case SAMPLER_BUILDER_SET_BORDER_UI: case SAMPLER_BUILDER_SET_ANISOTROPY: case SAMPLER_BUILDER_SET_REDUCTION:
    case SAMPLER_BUILDER_SET_LOD_SNAP:
    case VERTEX_ATTRIB_DEFAULTS: case VERTEX_ATTRIB_SET_FORMAT: case VERTEX_ATTRIB_SET_STREAM:
    case VERTEX_ATTRIB_GET_FORMAT: case VERTEX_ATTRIB_GET_STREAM:
    case VERTEX_STREAM_DEFAULTS: case VERTEX_STREAM_SET_STRIDE: case VERTEX_STREAM_SET_DIVISOR:
    case VERTEX_STREAM_GET_STRIDE: case VERTEX_STREAM_GET_DIVISOR:
    case COLOR_DEFAULTS: case COLOR_SET_BLEND: case COLOR_SET_LOGIC: case COLOR_SET_ALPHA:
    case COLOR_GET_BLEND: case COLOR_GET_LOGIC: case COLOR_GET_ALPHA:
    case MASK_DEFAULTS: case MASK_SET: case MASK_GET:
    case POLYGON_DEFAULTS: case POLYGON_SET_CULL: case POLYGON_SET_FRONT: case POLYGON_SET_MODE:
    case POLYGON_SET_OFFSET: case POLYGON_GET_CULL: case POLYGON_GET_FRONT: case POLYGON_GET_MODE:
    case POLYGON_GET_OFFSET:
    case DEPTH_STENCIL_DEFAULTS: case DEPTH_SET_TEST: case DEPTH_SET_WRITE: case DEPTH_SET_FUNC:
    case STENCIL_SET_TEST: case STENCIL_SET_FUNC: case STENCIL_SET_OP: case DEPTH_GET_TEST:
    case DEPTH_GET_WRITE: case DEPTH_GET_FUNC: case STENCIL_GET_TEST: case STENCIL_GET_FUNC: case STENCIL_GET_OP:
    case BLEND_DEFAULTS: case BLEND_SET_TARGET: case BLEND_SET_FUNC: case BLEND_SET_EQUATION:
    case BLEND_GET_TARGET: case BLEND_GET_FUNC: case BLEND_GET_EQUATION:
    case MULTISAMPLE_DEFAULTS: case MULTISAMPLE_SET_ENABLE: case MULTISAMPLE_SET_ALPHA:
    case MULTISAMPLE_SET_DITHER: case MULTISAMPLE_SET_SAMPLES: case MULTISAMPLE_SET_RASTER_SAMPLES:
    case MULTISAMPLE_GET_ENABLE: case MULTISAMPLE_GET_ALPHA: case MULTISAMPLE_GET_DITHER:
    case MULTISAMPLE_GET_SAMPLES: case MULTISAMPLE_GET_RASTER_SAMPLES:
        return true;
    default:
        return false;
    }
}

bool nvn_front_is_shared_call(const NvnFront *front, uint64_t pc)
{
    const Proc *proc = front ? proc_at(front, pc) : NULL;
    if (!proc)
        return false;
    switch (proc->sdk_offset) {
    case DEVICE_GET_IMAGE_HANDLE: case DEVICE_GET_SEPARATE_SAMPLER_HANDLE: case DEVICE_GET_SEPARATE_TEXTURE_HANDLE:
    case DEVICE_GET_TEXEL_FETCH_HANDLE: case DEVICE_GET_TEXTURE_HANDLE:
    case POOL_MAP: case POOL_FLUSH: case POOL_INVALIDATE: case POOL_GET_BUFFER_ADDRESS: case POOL_GET_SIZE:
    case POOL_GET_FLAGS:
    case BUFFER_MAP: case BUFFER_GET_ADDRESS: case BUFFER_FLUSH: case BUFFER_INVALIDATE: case BUFFER_GET_POOL:
    case BUFFER_GET_OFFSET: case BUFFER_GET_SIZE:
    case TEXTURE_GET_FLAGS: case TEXTURE_GET_TARGET: case TEXTURE_GET_WIDTH: case TEXTURE_GET_HEIGHT:
    case TEXTURE_GET_DEPTH: case TEXTURE_GET_LEVELS: case TEXTURE_GET_FORMAT: case TEXTURE_GET_SAMPLES:
    case TEXTURE_GET_STORAGE_SIZE:
    /* Descriptor slots: NVN forbids rewriting one that queued work still uses,
     * and queued work is what the replay worker runs. */
    case TEXTURE_POOL_REGISTER_TEXTURE: case TEXTURE_POOL_REGISTER_IMAGE:
    case SAMPLER_POOL_REGISTER: case SAMPLER_POOL_REGISTER_BUILDER:
        return true;
    default:
        return false;
    }
}

const char *nvn_front_call_name(const NvnFront *front, uint64_t pc)
{
    const Proc *proc = front ? proc_at(front, pc) : NULL;
    return proc ? proc->name : NULL;
}

bool nvn_front_is_pure_call(const NvnFront *front, uint64_t pc)
{
    const Proc *proc = front ? proc_at(front, pc) : NULL;
    return proc && pure_offset(proc->sdk_offset);
}

NvnFrontReply nvn_front_dispatch_with_floats(NvnFront *front, uint64_t pc, uint64_t x[32], const uint32_t scalar[2])
{
    const Proc *proc = front ? proc_at(front, pc) : NULL;
    if (!proc)
        return reply(NVN_FRONT_NOT_OURS, NULL);
    if (proc->sdk_offset == BOOTSTRAP || proc->sdk_offset == BOOTSTRAP_INTERNAL ||
        proc->sdk_offset == DEVICE_GET_PROC)
        return lookup(front, proc, x);
    if (front->pending_destroy && !nvn_front_shared && !nvn_front_lock_free)
        destroy_pending_textures(front);
    return reply(dispatch(front, proc->sdk_offset, x, scalar), proc->name);
}

NvnFrontReply nvn_front_dispatch(NvnFront *front, uint64_t pc, uint64_t x[32])
{
    return nvn_front_dispatch_with_floats(front, pc, x, NULL);
}

bool nvn_front_get_device_modes(NvnFront *front, uint64_t device_address, NvnFrontDeviceModes *modes)
{
    if (!front || !modes)
        return false;
    const Object *device = *find_object(&front->devices, device_address);
    if (!device)
        return false;
    *modes = device->modes;
    return true;
}

bool nvn_front_get_queue(NvnFront *front, uint64_t address, NvnFrontQueueInfo *info)
{
    if (!front || !info)
        return false;
    const Object *queue = *find_object(&front->queues, address);
    if (!queue)
        return false;
    *info = (NvnFrontQueueInfo){ .device = queue->device->address, .native = queue->native };
    return true;
}

bool nvn_front_get_pool(NvnFront *front, uint64_t pool_address, NvnFrontPoolConfig *config)
{
    if (!front || !config)
        return false;
    const Pool *pool = *find_pool(&front->pools, pool_address);
    if (!pool)
        return false;
    *config = pool->config;
    return true;
}

bool nvn_front_resolve_pool(const NvnFront *front, uint64_t device_address, uint64_t buffer_address,
    size_t size, NvnFrontPoolHit *hit)
{
    if (!front || !hit || !buffer_address || !size || size > UINT64_MAX - buffer_address)
        return false;
    const uint64_t end = buffer_address + size;
    const Pool *match = NULL;
    if (front->pool_index_disabled) {
        for (const Pool *pool = front->pools; pool; pool = pool->next) {
            const NvnFrontPoolConfig *config = &pool->config;
            if (config->device != device_address || (config->flags & POOL_GPU_NO_ACCESS) ||
                config->storage > buffer_address || config->storage + config->size < end)
                continue;
            if (match)
                return false;
            match = pool;
        }
    } else {
        size_t i = pool_upper_bound(front, device_address, buffer_address);
        while (i) {
            const PoolIndexEntry *entry = &front->pool_index[--i];
            const NvnFrontPoolConfig *config = &entry->pool->config;
            if (config->device != device_address || entry->max_end < end)
                break;
            if (config->storage + config->size < end)
                continue;
            if (match)
                return false;
            match = entry->pool;
        }
    }
    if (!match || !read_guest(front, buffer_address, size))
        return false;
    *hit = (NvnFrontPoolHit){
        .object = match->address,
        .offset = buffer_address - match->config.storage,
        .config = match->config,
    };
    return true;
}

bool nvn_front_get_texture(NvnFront *front, uint64_t address, NvnFrontTextureInfo *info)
{
    if (!front || !info)
        return false;
    const Texture *texture = lookup_texture(front, address);
    if (!texture)
        return false;
    *info = texture->info;
    return true;
}

bool nvn_front_get_descriptor_pool(NvnFront *front, uint64_t address, bool sampler, NvnFrontDescriptorPoolInfo *info)
{
    if (!front || !info)
        return false;
    const DescriptorPool *pool = *find_descriptor_pool(&front->descriptor_pools, address, sampler);
    if (!pool)
        return false;
    *info = pool->info;
    return true;
}

bool nvn_front_get_texture_descriptor(NvnFront *front, uint64_t address, uint32_t index,
    NvnFrontTextureDescriptorInfo *info)
{
    if (!front || !info)
        return false;
    const DescriptorPool *pool = *find_descriptor_pool(&front->descriptor_pools, address, false);
    if (!pool || index >= pool->info.count || !pool->texture_slots)
        return false;
    const TextureDescriptor *slot = &pool->texture_slots[index];
    if (!slot->texture)
        return false;
    *info = (NvnFrontTextureDescriptorInfo){ .address = slot->texture->address, .texture = slot->texture->info,
        .has_view = slot->has_view, .image = slot->image };
    memcpy(info->view, slot->view, sizeof info->view);
    return true;
}

bool nvn_front_get_sampler_descriptor(NvnFront *front, uint64_t address, uint32_t index, NvnFrontSamplerInfo *info)
{
    if (!front || !info)
        return false;
    const DescriptorPool *pool = *find_descriptor_pool(&front->descriptor_pools, address, true);
    if (!pool || index >= pool->info.count)
        return false;
    if (pool->sampler_slots && pool->sampler_slots[index].registered) {
        *info = pool->sampler_slots[index].info;
        return true;
    }
    if (index >= 3)
        return false;
    /* These settings produce the three descriptors seeded at initialization. */
    *info = (NvnFrontSamplerInfo){ .min_filter = index != 2, .mag_filter = index != 2,
        .wrap = { 7, 7, 7 }, .compare_func = 2, .lod_max = 1000.0f,
        .max_anisotropy = index ? 2.0f : 1.0f };
    return true;
}

bool nvn_front_read_vertex_attribs(const NvnFront *front, uint64_t address, uint32_t count,
    NvnFrontVertexAttribInfo *out)
{
    if (!front || count > NVN_FRONT_MAX_VERTEX_ATTRIBS)
        return false;
    if (!count)
        return true;
    const uint8_t *states = read_guest(front, address, count * VERTEX_ATTRIB_BYTES);
    if (!states || !out)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = decode_vertex_attrib(read32(states + i * VERTEX_ATTRIB_BYTES));
    return true;
}

bool nvn_front_read_vertex_streams(const NvnFront *front, uint64_t address, uint32_t count,
    NvnFrontVertexStreamInfo *out)
{
    if (!front || count > NVN_FRONT_MAX_VERTEX_STREAMS)
        return false;
    if (!count)
        return true;
    const uint8_t *states = read_guest(front, address, count * VERTEX_STREAM_BYTES);
    if (!states || !out)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *state = states + i * VERTEX_STREAM_BYTES;
        out[i] = (NvnFrontVertexStreamInfo){ .stride = read32(state), .divisor = read32(state + 4) };
    }
    return true;
}

static bool read_state_word(const NvnFront *front, uint64_t address, uint32_t *word)
{
    if (!front || !word)
        return false;
    const uint8_t *object = read_guest(front, address, 4);
    if (!object)
        return false;
    *word = read32(object);
    return true;
}

bool nvn_front_read_color(const NvnFront *front, uint64_t address, NvnFrontColorInfo *out)
{
    uint32_t word;
    if (!out || !read_state_word(front, address, &word))
        return false;
    *out = (NvnFrontColorInfo){ .blend_mask = (word >> 8) & 0xffu,
        .logic_op = (word >> 16) & 0xffu, .alpha_func = (word >> 24) & 0xfu };
    return true;
}

bool nvn_front_read_channel_masks(const NvnFront *front, uint64_t address, uint32_t *out)
{
    return read_state_word(front, address, out);
}

bool nvn_front_read_polygon(const NvnFront *front, uint64_t address, NvnFrontPolygonInfo *out)
{
    uint32_t word;
    if (!out || !read_state_word(front, address, &word))
        return false;
    *out = (NvnFrontPolygonInfo){ .cull_face = word & 3u, .front_face = (word >> 2) & 1u,
        .mode = (word >> 3) & 3u, .offset_enables = (word >> 15) & 0xfu };
    return true;
}

bool nvn_front_read_depth_stencil(const NvnFront *front, uint64_t address, NvnFrontDepthStencilInfo *out)
{
    if (!front || !out)
        return false;
    const uint8_t *object = read_guest(front, address, 8);
    if (!object)
        return false;
    const uint32_t depth = read32(object);
    const uint32_t stencil = read32(object + 4);
    *out = (NvnFrontDepthStencilInfo){ .depth_test = (depth & 1u) != 0, .depth_write = (depth & 2u) != 0,
        .stencil_test = (depth & 4u) != 0, .depth_func = (depth >> 4) & 0xfu,
        .front = decode_stencil(stencil), .back = decode_stencil(stencil >> 16) };
    return true;
}

bool nvn_front_read_blend(const NvnFront *front, uint64_t address, NvnFrontBlendInfo *out)
{
    if (!front || !out)
        return false;
    const uint8_t *object = read_guest(front, address, 8);
    if (!object)
        return false;
    const uint32_t state = read32(object);
    const uint32_t factors = read32(object + 4);
    *out = (NvnFrontBlendInfo){ .target = state & 7u, .src_rgb = factors & 0x7fu,
        .dst_rgb = (factors >> 8) & 0x7fu, .src_alpha = (factors >> 16) & 0x7fu,
        .dst_alpha = factors >> 24 & 0x7fu, .rgb_equation = (state >> 24) & 7u,
        .alpha_equation = (state >> 28) & 7u, .advanced_mode = (state >> 8) & 0x3fu };
    return true;
}

bool nvn_front_read_multisample(const NvnFront *front, uint64_t address, NvnFrontMultisampleInfo *out)
{
    uint32_t word;
    if (!out || !read_state_word(front, address, &word))
        return false;
    *out = (NvnFrontMultisampleInfo){ .samples = (word >> 8) & 0x1fu, .raster_samples = (word >> 15) & 0x1fu,
        .enabled = (word & 1u) != 0, .alpha_to_coverage = (word & 2u) != 0,
        .alpha_to_coverage_dither = (word & 4u) != 0 };
    return true;
}

bool nvn_front_get_sync(NvnFront *front, uint64_t address, void **native)
{
    if (!front || !native)
        return false;
    const Object *sync = *find_object(&front->syncs, address);
    if (!sync)
        return false;
    *native = sync->native;
    return true;
}

bool nvn_front_get_command_buffer(NvnFront *front, uint64_t address, NvnFrontCommandBufferInfo *info)
{
    if (!front || !info)
        return false;
    const CommandBuffer *command = *find_command_buffer(&front->command_buffers, address);
    if (!command)
        return false;
    *info = command->info;
    return true;
}

bool nvn_front_charge_command_memory(NvnFront *front, uint64_t encoder, size_t command_bytes, size_t control_bytes)
{
    if (!front || (command_bytes & 3u) || (control_bytes & 7u))
        return false;
    CommandBuffer *command = *find_command_buffer(&front->command_buffers, encoder);
    if (!command)
        return false;
    NvnFrontCommandBufferInfo *info = &command->info;
    if (command_bytes > info->command_size - info->command_used ||
        control_bytes > info->control_size - info->control_used)
        return false;
    if (!command_bytes && !control_bytes)
        return true;
    uint8_t *object = write_guest(front, encoder, COMMAND_BUFFER_BYTES);
    if (!object)
        return false;
    if (command_bytes) {
        info->command_used += command_bytes;
        write64(object, info->command_address + info->command_used);
    }
    if (control_bytes) {
        info->control_used += control_bytes;
        write64(object + 0x28, info->control_address + info->control_used);
    }
    return true;
}

bool nvn_front_get_program(NvnFront *front, uint64_t address, NvnFrontProgramInfo *info)
{
    if (!front || !info)
        return false;
    const Program *program = lookup_program(front, address);
    if (!program)
        return false;
    *info = program->info;
    return true;
}

void nvn_front_destroy(NvnFront *front)
{
    if (!front)
        return;
    while (front->command_buffers)
        remove_command_buffer(front, &front->command_buffers);
    while (front->programs)
        remove_program(front, &front->programs);
    while (front->textures)
        remove_texture(front, front->textures);
    while (front->queues)
        finalize(front, &front->queues, front->config.queue_destroy);
    while (front->syncs)
        finalize(front, &front->syncs, front->config.sync_destroy);
    while (front->samplers)
        remove_sampler(&front->samplers);
    while (front->buffers)
        remove_buffer(&front->buffers);
    while (front->descriptor_pools)
        remove_descriptor_pool(front, &front->descriptor_pools);
    free(front->pool_index);
    while (front->pools)
        remove_pool(&front->pools);
    while (front->devices)
        finalize(front, &front->devices, front->config.device_destroy);
    free(front);
}
