/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nvn_front.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
    BASE = 0x10000, DEVICE_BUILDER = BASE + 0x100, QUEUE_BUILDER = BASE + 0x200,
    NAME = BASE + 0x300, OUTPUT = BASE + 0x400, DEVICE = BASE + 0x1000,
    COMMAND_BUFFER = BASE + 0x420, SECOND_COMMAND_BUFFER = BASE + 0x4e0,
    POOL_BUILDER = BASE + 0x600, POOL = BASE + 0x700, SECOND_POOL = BASE + 0x900,
    TEXTURE_BUILDER = BASE + 0xa00, TEXTURE = BASE + 0xb00, PACKAGED_LAYOUT = BASE + 0xc00,
    TEXTURE_VIEW = BASE + 0xc20, SYNC = BASE + 0xc80, SECOND_SYNC = BASE + 0xcc0,
    BUFFER_BUILDER = BASE + 0xd00, BUFFER = BASE + 0xd40,
    TEXTURE_POOL = BASE + 0xd80, SAMPLER_POOL = BASE + 0xda8,
    SAMPLER_BUILDER = BASE + 0x5a0, SAMPLER = BASE + 0x680, BORDER_COLOR = BASE + 0xc40,
    PROGRAM = BASE + 0xf20, SHADER_ENTRIES = BASE + 0x190, SHADER_CONTROL = BASE + 0x250,
    VERTEX_ATTRIBS = BASE + 0x800, VERTEX_STREAMS = BASE + 0x820,
    COLOR_STATE = BASE + 0x850, CHANNEL_MASK = BASE + 0x858, POLYGON_STATE = BASE + 0x860,
    DEPTH_STENCIL_STATE = BASE + 0x868, BLEND_STATE = BASE + 0x878, MULTISAMPLE_STATE = BASE + 0x888,
    CONTROL_MEMORY = BASE + 0xe00,
    QUEUE = BASE + 0x5000, SECOND_QUEUE = BASE + 0x9000, QUEUE_MEMORY = BASE + 0xd000,
    SECOND_DEVICE = BASE + 0xe000,
};

typedef struct {
    uint8_t memory[0x11000];
    uint64_t unreadable_byte;
    unsigned devices, queues, calls, integer_queries;
    unsigned textures, texture_calls, texture_destroys;
    unsigned expected_texture_height;
    unsigned syncs, sync_calls, sync_destroys;
    unsigned commands, command_calls, command_destroys;
    unsigned programs, program_destroys, program_links;
    bool fail_device, fail_sync, fail_command, fail_program;
    int device, second_device, queue[2], texture[2], sync[2], command[2];
    int program[2];
    NvnFrontTextureCreateResult texture_result;
    NvnFrontProgramLinkResult program_result;
} Probe;

static void *guest(void *opaque, uint64_t address, size_t size)
{
    Probe *probe = opaque;
    uint64_t offset = address - BASE;
    if (offset > sizeof probe->memory || size > sizeof probe->memory - offset)
        return NULL;
    return probe->memory + offset;
}

static const void *read_guest(void *opaque, uint64_t address, size_t size)
{
    const Probe *probe = opaque;
    if (address <= probe->unreadable_byte && probe->unreadable_byte - address < size)
        return NULL;
    return guest(opaque, address, size);
}

static void *create_device(void *opaque, uint32_t flags)
{
    Probe *probe = opaque;
    assert(flags == 0x200 && probe->devices < 2);
    ++probe->calls;
    if (probe->fail_device)
        return NULL;
    return probe->devices++ ? &probe->second_device : &probe->device;
}

static void destroy_device(void *opaque, void *device)
{
    Probe *probe = opaque;
    assert(probe->devices && !probe->queues && !probe->textures && !probe->syncs && !probe->commands);
    assert(!probe->programs);
    assert(device == (probe->devices == 2 ? &probe->second_device : &probe->device));
    --probe->devices;
}

static void *create_queue(void *opaque, void *device, const NvnFrontQueueConfig *config)
{
    Probe *probe = opaque;
    assert(device == &probe->device && probe->devices == 1 && probe->queues < 2);
    assert(config->primary_queue == (probe->queues ? &probe->queue[0] : NULL));
    assert(config->flags == 4 && config->priority == 100);
    assert(config->overrides == (NVN_FRONT_QUEUE_COMMAND_MEMORY | NVN_FRONT_QUEUE_COMPUTE_MEMORY |
                                 NVN_FRONT_QUEUE_CONTROL_MEMORY | NVN_FRONT_QUEUE_FLUSH_THRESHOLD));
    assert(config->command_bytes == 0x10000 && config->compute_bytes == 0);
    assert(config->control_bytes == 0x10000 && config->flush_threshold == 0x8000);
    assert(config->queue_memory == QUEUE_MEMORY && config->queue_memory_bytes == 0x1000);
    return &probe->queue[probe->queues++];
}

static void destroy_queue(void *opaque, void *queue)
{
    Probe *probe = opaque;
    assert(probe->queues && queue == &probe->queue[probe->queues - 1]);
    --probe->queues;
}

static bool get_integer(void *opaque, void *device, uint32_t query, int32_t *value)
{
    Probe *probe = opaque;
    assert(device == &probe->device && probe->devices == 1);
    ++probe->integer_queries;
    if (query != 0x2a)
        return false;
    *value = 1;
    return true;
}

static NvnFrontTextureCreateResult create_texture(void *opaque, void *device,
    const NvnFrontTextureCreateInfo *info, void **texture_out)
{
    Probe *probe = opaque;
    ++probe->texture_calls;
    assert(device == &probe->device && probe->devices == 1 && !*texture_out);
    assert(info->config.device == DEVICE && info->config.pool == POOL && info->config.format == 0x38);
    const unsigned height = probe->expected_texture_height ? probe->expected_texture_height : 16;
    assert(info->config.width == 16 && info->config.height == height && info->config.depth == 1);
    assert(info->storage_address == QUEUE_MEMORY + info->config.offset);
    const size_t bytes = height == 8 ? 0x200 : 0x400;
    assert(info->storage_size == bytes && info->storage_alignment == bytes);
    if (probe->texture_result == NVN_FRONT_TEXTURE_CREATED) {
        const unsigned index = (probe->textures & 1u) ? 1u : 0u;
        assert(!(probe->textures & (1u << index)));
        probe->textures |= 1u << index;
        *texture_out = &probe->texture[index];
    }
    return probe->texture_result;
}

static void destroy_texture(void *opaque, void *texture)
{
    Probe *probe = opaque;
    assert(texture == &probe->texture[0] || texture == &probe->texture[1]);
    const unsigned bit = texture == &probe->texture[0] ? 1u : 2u;
    assert(probe->textures & bit);
    probe->textures &= ~bit;
    ++probe->texture_destroys;
}

static void *create_sync(void *opaque, void *device)
{
    Probe *probe = opaque;
    assert(device == &probe->device && probe->devices == 1);
    ++probe->sync_calls;
    if (probe->fail_sync)
        return NULL;
    const unsigned index = (probe->syncs & 1u) ? 1u : 0u;
    assert(!(probe->syncs & (1u << index)));
    probe->syncs |= 1u << index;
    return &probe->sync[index];
}

static void destroy_sync(void *opaque, void *sync)
{
    Probe *probe = opaque;
    assert(sync == &probe->sync[0] || sync == &probe->sync[1]);
    const unsigned bit = sync == &probe->sync[0] ? 1u : 2u;
    assert(probe->syncs & bit);
    probe->syncs &= ~bit;
    ++probe->sync_destroys;
}

static void *create_command_buffer(void *opaque, void *device)
{
    Probe *probe = opaque;
    assert(device == &probe->device && probe->devices == 1);
    ++probe->command_calls;
    if (probe->fail_command)
        return NULL;
    const unsigned index = (probe->commands & 1u) ? 1u : 0u;
    assert(!(probe->commands & (1u << index)));
    probe->commands |= 1u << index;
    return &probe->command[index];
}

static void destroy_command_buffer(void *opaque, void *command)
{
    Probe *probe = opaque;
    assert(command == &probe->command[0] || command == &probe->command[1]);
    const unsigned bit = command == &probe->command[0] ? 1u : 2u;
    assert(probe->commands & bit);
    probe->commands &= ~bit;
    ++probe->command_destroys;
}

static uint64_t invoke(NvnFront *front, uint64_t pc, uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t x[32] = { a, b, c };
    NvnFrontReply reply = nvn_front_dispatch(front, pc, x);
    assert(reply.kind == NVN_FRONT_HANDLED);
    return x[0];
}

static uint64_t get_proc(NvnFront *front, Probe *probe, uint64_t loader, uint64_t device, const char *name)
{
    memcpy(guest(probe, NAME, strlen(name) + 1), name, strlen(name) + 1);
    uint64_t result = invoke(front, loader, device, NAME, 0);
    assert(result >= NVN_FRONT_DEFAULT_THUNK_BASE && !(result & 3));
    return result;
}

static void assert_modes(NvnFront *front, uint64_t device, NvnFrontWindowOrigin origin, NvnFrontDepthMode depth)
{
    NvnFrontDeviceModes modes;
    assert(nvn_front_get_device_modes(front, device, &modes));
    assert(modes.window_origin == origin && modes.depth_mode == depth);
}

static void test_device_modes(NvnFront *front, Probe *probe, uint64_t loader, uint64_t device_init)
{
    const uint64_t set_origin = get_proc(front, probe, loader, DEVICE, "nvnDeviceSetWindowOriginMode");
    const uint64_t set_depth = get_proc(front, probe, loader, DEVICE, "nvnDeviceSetDepthMode");
    assert_modes(front, DEVICE, NVN_FRONT_WINDOW_ORIGIN_LOWER_LEFT, NVN_FRONT_DEPTH_NEAR_IS_MINUS_W);
    assert(invoke(front, set_origin, DEVICE, NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT, 0) == DEVICE);
    assert_modes(front, DEVICE, NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT, NVN_FRONT_DEPTH_NEAR_IS_MINUS_W);
    invoke(front, 0x75005d69f0ULL, DEVICE, NVN_FRONT_DEPTH_NEAR_IS_ZERO, 0);
    assert_modes(front, DEVICE, NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT, NVN_FRONT_DEPTH_NEAR_IS_ZERO);

    uint64_t regs[32] = { DEVICE, 2 };
    NvnFrontReply result = nvn_front_dispatch(front, 0x75005d6a10ULL, regs);
    assert(result.kind == NVN_FRONT_INVALID_GUEST && !strcmp(result.name, "nvnDeviceSetWindowOriginMode"));
    regs[1] = UINT32_MAX;
    result = nvn_front_dispatch(front, set_depth, regs);
    assert(result.kind == NVN_FRONT_INVALID_GUEST && !strcmp(result.name, "nvnDeviceSetDepthMode"));
    assert_modes(front, DEVICE, NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT, NVN_FRONT_DEPTH_NEAR_IS_ZERO);

    NvnFrontDeviceModes modes;
    assert(!nvn_front_get_device_modes(front, SECOND_DEVICE, &modes));
    regs[0] = SECOND_DEVICE;
    regs[1] = NVN_FRONT_DEPTH_NEAR_IS_ZERO;
    assert(nvn_front_dispatch(front, set_depth, regs).kind == NVN_FRONT_INVALID_GUEST);
    assert(invoke(front, device_init, SECOND_DEVICE, DEVICE_BUILDER, 0) == 1);
    assert_modes(front, SECOND_DEVICE, NVN_FRONT_WINDOW_ORIGIN_LOWER_LEFT, NVN_FRONT_DEPTH_NEAR_IS_MINUS_W);
    invoke(front, set_depth, SECOND_DEVICE, UINT64_C(0xdeadbeef00000001), 0);
    assert_modes(front, SECOND_DEVICE, NVN_FRONT_WINDOW_ORIGIN_LOWER_LEFT, NVN_FRONT_DEPTH_NEAR_IS_ZERO);
    invoke(front, 0x75005d6a10ULL, DEVICE, NVN_FRONT_WINDOW_ORIGIN_LOWER_LEFT, 0);
    invoke(front, set_depth, DEVICE, NVN_FRONT_DEPTH_NEAR_IS_MINUS_W, 0);
    assert_modes(front, DEVICE, NVN_FRONT_WINDOW_ORIGIN_LOWER_LEFT, NVN_FRONT_DEPTH_NEAR_IS_MINUS_W);
    assert_modes(front, SECOND_DEVICE, NVN_FRONT_WINDOW_ORIGIN_LOWER_LEFT, NVN_FRONT_DEPTH_NEAR_IS_ZERO);
    invoke(front, 0x75005d6510ULL, SECOND_DEVICE, 0, 0);
    assert(!nvn_front_get_device_modes(front, SECOND_DEVICE, &modes));
}

static void test_texture_builder(NvnFront *front, Probe *probe, uint64_t loader)
{
    const uint64_t set_device = get_proc(front, probe, loader, DEVICE, "nvnTextureBuilderSetDevice");
    const uint64_t get_alignment = get_proc(front, probe, loader, DEVICE, "nvnTextureBuilderGetStorageAlignment");
    const uint64_t initialize = get_proc(front, probe, loader, DEVICE, "nvnTextureInitialize");
    uint8_t *builder = guest(probe, TEXTURE_BUILDER, 0x81);
    memset(builder, 0xa5, 0x81);
    invoke(front, set_device, TEXTURE_BUILDER, DEVICE, 0);
    invoke(front, 0x75005ee160ULL, TEXTURE_BUILDER, 0, 0);

    NvnFrontTextureConfig config;
    assert(nvn_front_read_texture_builder(front, TEXTURE_BUILDER, &config));
    assert(config.device == DEVICE && config.target == 1 && config.levels == 1);
    assert(!config.width && !config.height && !config.depth && !config.format && !config.samples);
    assert(!config.flags && !config.pool && !config.offset && !config.packaged_layout && !config.packaged_data);
    assert(config.swizzle[0] == 2 && config.swizzle[1] == 3 && config.swizzle[2] == 4 && config.swizzle[3] == 5);
    assert(builder[0] == 0xa5 && builder[0x56] == 0xa5 && builder[0x57] == 0xa5 && builder[0x80] == 0xa5);

    invoke(front, 0x75005ee1c0ULL, TEXTURE_BUILDER, 640, 0);
    invoke(front, 0x75005ee1d0ULL, TEXTURE_BUILDER, 360, 0);
    invoke(front, 0x75005ee1e0ULL, TEXTURE_BUILDER, 1, 0);
    invoke(front, 0x75005ee220ULL, TEXTURE_BUILDER, 2, 0);
    invoke(front, 0x75005ee1b0ULL, TEXTURE_BUILDER, 1, 0);
    invoke(front, 0x75005ee230ULL, TEXTURE_BUILDER, 0x38, 0);
    invoke(front, 0x75005ee1a0ULL, TEXTURE_BUILDER, 0xd, 0);
    assert(invoke(front, 0x75005ee2d0ULL, TEXTURE_BUILDER, 0, 0) == 640);
    assert(invoke(front, 0x75005ee2e0ULL, TEXTURE_BUILDER, 0, 0) == 360);

    const uint16_t layout = 0xf321;
    memcpy(guest(probe, PACKAGED_LAYOUT, sizeof layout), &layout, sizeof layout);
    invoke(front, 0x75005eea00ULL, TEXTURE_BUILDER, PACKAGED_LAYOUT, 0);
    assert(nvn_front_read_texture_builder(front, TEXTURE_BUILDER, &config));
    assert(config.packaged_layout == 0x3211 && config.format == 0x38 && config.flags == 0xd && config.levels == 2);

    /* The first descriptor captured from the 655360 desktop run. */
    invoke(front, 0x75005ee160ULL, TEXTURE_BUILDER, 0, 0);
    invoke(front, 0x75005ee200ULL, TEXTURE_BUILDER, 1920, 1080);
    invoke(front, 0x75005ee230ULL, TEXTURE_BUILDER, 0x38, 0);
    invoke(front, 0x75005ee1a0ULL, TEXTURE_BUILDER, 0xd, 0);
    assert(invoke(front, get_alignment, TEXTURE_BUILDER, 0, 0) == 0x2000);
    assert(invoke(front, 0x75005ee7d0ULL, TEXTURE_BUILDER, 0, 0) == 0x870000);

    invoke(front, 0x75005ee1a0ULL, TEXTURE_BUILDER, 0x20, 0);
    uint64_t regs[32] = { TEXTURE_BUILDER };
    NvnFrontReply result = nvn_front_dispatch(front, get_alignment, regs);
    assert(result.kind == NVN_FRONT_BOUNDARY && !strcmp(result.name, "nvnTextureBuilderGetStorageAlignment"));
    assert(regs[0] == TEXTURE_BUILDER);
    invoke(front, 0x75005ee1a0ULL, TEXTURE_BUILDER, 0xd, 0);

    regs[0] = TEXTURE;
    regs[1] = TEXTURE_BUILDER;
    assert(nvn_front_dispatch(front, initialize, regs).kind == NVN_FRONT_INVALID_GUEST);
    invoke(front, 0x75005ee270ULL, TEXTURE_BUILDER, POOL, 0x1000);
    assert(nvn_front_dispatch(front, initialize, regs).kind == NVN_FRONT_INVALID_GUEST);
    invoke(front, 0x75005ee270ULL, TEXTURE_BUILDER, POOL, 0);
    assert(nvn_front_dispatch(front, initialize, regs).kind == NVN_FRONT_INVALID_GUEST);

    invoke(front, 0x75005ee200ULL, TEXTURE_BUILDER, 16, 16);
    invoke(front, 0x75005ee1a0ULL, TEXTURE_BUILDER, 0x8, 0);
    invoke(front, 0x75005ee270ULL, TEXTURE_BUILDER, POOL, 0x400);
    assert(nvn_front_read_texture_builder(front, TEXTURE_BUILDER, &config));
    assert(config.pool == POOL && config.offset == 0x400);
    probe->unreadable_byte = QUEUE_MEMORY + 0x7ff;
    assert(nvn_front_dispatch(front, initialize, regs).kind == NVN_FRONT_INVALID_GUEST);
    probe->unreadable_byte = 0;
    uint8_t *texture = guest(probe, TEXTURE, 0x81);
    memset(texture, 0x5a, 0x81);
    result = nvn_front_dispatch(front, initialize, regs);
    assert(result.kind == NVN_FRONT_BOUNDARY && !strcmp(result.name, "nvnTextureInitialize"));
    assert(regs[0] == TEXTURE && regs[1] == TEXTURE_BUILDER);
    for (size_t i = 0; i < 0x81; ++i)
        assert(texture[i] == 0x5a);
    assert(probe->texture_calls == 1 && !probe->textures);

    probe->texture_result = NVN_FRONT_TEXTURE_FAILED;
    assert(invoke(front, initialize, TEXTURE, TEXTURE_BUILDER, 0) == 0);
    NvnFrontTextureInfo info;
    assert(!nvn_front_get_texture(front, TEXTURE, &info));
    assert(texture[0] == 0x5a && texture[0x80] == 0x5a);

    probe->texture_result = NVN_FRONT_TEXTURE_CREATED;
    assert(invoke(front, initialize, TEXTURE, TEXTURE_BUILDER, 0) == 1);
    assert(!texture[0] && !texture[0x7f] && texture[0x80] == 0x5a);
    assert(nvn_front_get_texture(front, TEXTURE, &info));
    assert(info.native == &probe->texture[0] && info.create_info.config.offset == 0x400);
    assert(info.create_info.storage_address == QUEUE_MEMORY + 0x400 && info.create_info.storage_size == 0x400);
    assert(invoke(front, 0x75005f0e70ULL, TEXTURE, 0, 0) == 0x400);
    const uint64_t get_flags = get_proc(front, probe, loader, DEVICE, "nvnTextureGetFlags");
    assert(invoke(front, get_flags, TEXTURE, 0, 0) == 0x8);
    const uint64_t get_width = get_proc(front, probe, loader, DEVICE, "nvnTextureGetWidth");
    const uint64_t get_height = get_proc(front, probe, loader, DEVICE, "nvnTextureGetHeight");
    assert(get_width != get_height);
    assert(invoke(front, get_width, TEXTURE, UINT64_MAX, 0) == 16);
    assert(invoke(front, get_height, TEXTURE, UINT64_MAX, 0) == 16);
    const uint64_t get_format = get_proc(front, probe, loader, DEVICE, "nvnTextureGetFormat");
    assert(invoke(front, get_format, TEXTURE, UINT64_MAX, 0) == 0x38);
    assert(invoke(front, 0x75005f0100ULL, TEXTURE, 0, 0) == 0x38);

    invoke(front, 0x75005ee270ULL, TEXTURE_BUILDER, POOL, 0x800);
    probe->texture_result = NVN_FRONT_TEXTURE_FAILED;
    assert(invoke(front, initialize, TEXTURE, TEXTURE_BUILDER, 0) == 0);
    assert(nvn_front_get_texture(front, TEXTURE, &info));
    assert(info.native == &probe->texture[0] && info.create_info.config.offset == 0x400);
    assert(!probe->texture_destroys);
    invoke(front, 0x75005ee200ULL, TEXTURE_BUILDER, 16, 8);
    assert(invoke(front, get_height, TEXTURE, 0, 0) == 16);
    probe->expected_texture_height = 8;
    probe->texture_result = NVN_FRONT_TEXTURE_CREATED;
    assert(invoke(front, initialize, TEXTURE, TEXTURE_BUILDER, 0) == 1);
    assert(nvn_front_get_texture(front, TEXTURE, &info));
    assert(info.native == &probe->texture[1] && info.create_info.config.offset == 0x800);
    assert(probe->texture_destroys == 1 && probe->textures == 2);
    assert(invoke(front, get_width, TEXTURE, UINT64_MAX, 0) == 16);
    assert(invoke(front, get_height, TEXTURE, UINT64_MAX, 0) == 8);
    assert(invoke(front, 0x75005f00b0ULL, TEXTURE, 0, 0) == 16);
    assert(invoke(front, 0x75005f00c0ULL, TEXTURE, 0, 0) == 8);
    probe->expected_texture_height = 0;
    invoke(front, 0x75005ee200ULL, TEXTURE_BUILDER, 16, 16);
    assert(invoke(front, get_height, TEXTURE, 0, 0) == 8);

    regs[0] = POOL;
    assert(nvn_front_dispatch(front, 0x75005e2680ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    const uint64_t finalize = get_proc(front, probe, loader, DEVICE, "nvnTextureFinalize");
    invoke(front, finalize, TEXTURE, 0, 0);
    assert(!nvn_front_get_texture(front, TEXTURE, &info));
    assert(probe->texture_destroys == 2 && !probe->textures);
    regs[0] = TEXTURE;
    assert(nvn_front_dispatch(front, 0x75005f0e70ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    assert(nvn_front_dispatch(front, get_format, regs).kind == NVN_FRONT_INVALID_GUEST);
    assert(nvn_front_dispatch(front, get_flags, regs).kind == NVN_FRONT_INVALID_GUEST);
    assert(nvn_front_dispatch(front, get_width, regs).kind == NVN_FRONT_INVALID_GUEST);
    assert(nvn_front_dispatch(front, get_height, regs).kind == NVN_FRONT_INVALID_GUEST);
    assert(nvn_front_dispatch(front, finalize, regs).kind == NVN_FRONT_INVALID_GUEST);
}

static void test_sync_lifecycle(NvnFront *front, Probe *probe, uint64_t loader)
{
    const uint64_t initialize = get_proc(front, probe, loader, DEVICE, "nvnSyncInitialize");
    const uint64_t finalize = get_proc(front, probe, loader, DEVICE, "nvnSyncFinalize");
    uint8_t *object = guest(probe, SYNC, 0x41);
    memset(object, 0x5a, 0x41);
    uint64_t regs[32] = { SYNC, 0 };
    assert(nvn_front_dispatch(front, initialize, regs).kind == NVN_FRONT_INVALID_GUEST);
    regs[0] = BASE + sizeof probe->memory - 0x3f;
    regs[1] = DEVICE;
    assert(nvn_front_dispatch(front, initialize, regs).kind == NVN_FRONT_INVALID_GUEST);
    assert(!probe->sync_calls);

    probe->fail_sync = true;
    assert(invoke(front, initialize, SYNC, DEVICE, 0) == 0);
    void *native = NULL;
    assert(!nvn_front_get_sync(front, SYNC, &native));
    assert(object[0] == 0x5a && object[0x40] == 0x5a);

    probe->fail_sync = false;
    assert(invoke(front, initialize, SYNC, DEVICE, 0) == 1);
    assert(nvn_front_get_sync(front, SYNC, &native) && native == &probe->sync[0]);
    uint64_t guest_device;
    memcpy(&guest_device, object, sizeof guest_device);
    assert(guest_device == DEVICE);
    for (size_t i = 8; i < 0x20; ++i)
        assert(!object[i]);
    for (size_t i = 0x20; i < 0x41; ++i)
        assert(object[i] == 0x5a);

    regs[0] = DEVICE;
    assert(nvn_front_dispatch(front, 0x75005d6510ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    regs[0] = SYNC;
    regs[1] = 0;
    NvnFrontReply result = nvn_front_dispatch(front, 0x75005ed780ULL, regs);
    assert(result.kind == NVN_FRONT_BOUNDARY && !strcmp(result.name, "nvnSyncWait"));
    assert(regs[0] == SYNC && regs[1] == 0);
    result = nvn_front_dispatch(front, 0x75005ed770ULL, regs);
    assert(result.kind == NVN_FRONT_BOUNDARY && !strcmp(result.name, "nvnQueueFenceSync"));

    probe->fail_sync = true;
    assert(invoke(front, initialize, SYNC, DEVICE, 0) == 0);
    assert(nvn_front_get_sync(front, SYNC, &native) && native == &probe->sync[0]);
    assert(!probe->sync_destroys);
    probe->fail_sync = false;
    assert(invoke(front, initialize, SYNC, DEVICE, 0) == 1);
    assert(nvn_front_get_sync(front, SYNC, &native) && native == &probe->sync[1]);
    assert(probe->sync_destroys == 1);
    invoke(front, finalize, SYNC, 0, 0);
    assert(!nvn_front_get_sync(front, SYNC, &native));
    assert(probe->sync_destroys == 2 && !probe->syncs);
    assert(nvn_front_dispatch(front, finalize, regs).kind == NVN_FRONT_INVALID_GUEST);

    assert(invoke(front, 0x75005ed110ULL, SECOND_SYNC, DEVICE, 0) == 1);
}

static void test_texture_view(NvnFront *front, Probe *probe, uint64_t loader)
{
    const uint64_t defaults = get_proc(front, probe, loader, DEVICE, "nvnTextureViewSetDefaults");
    uint8_t *view = guest(probe, TEXTURE_VIEW, 17);
    memset(view, 0x5a, 17);
    invoke(front, defaults, TEXTURE_VIEW, 0, 0);
    for (size_t i = 0; i < 16; ++i)
        assert(!view[i]);
    assert(view[16] == 0x5a);

    view[7] = 0xc0;
    memset(view + 12, 0xa5, 4);
    invoke(front, 0x75005eea70ULL, TEXTURE_VIEW, 0x1ff, 0x103);
    invoke(front, 0x75005eeaa0ULL, TEXTURE_VIEW, 0x11234, 0xffff8002);
    invoke(front, 0x75005eeac0ULL, TEXTURE_VIEW, 0xabcdef38, 0);
    invoke(front, 0x75005ee7b0ULL, TEXTURE_VIEW, 0x101, 0);
    const uint64_t swizzle = get_proc(front, probe, loader, DEVICE, "nvnTextureViewSetSwizzle");
    uint64_t swizzle_regs[32] = { TEXTURE_VIEW, 0x102, 0x103, 0x104, 0x105 };
    assert(nvn_front_dispatch(front, swizzle, swizzle_regs).kind == NVN_FRONT_HANDLED);
    assert(view[7] == 0xef);
    invoke(front, 0x75005eeb00ULL, TEXTURE_VIEW, 0x101, 0);
    const uint8_t expected[] = {
        0x34, 0x12, 0x02, 0x80, 0xff, 0x03, 0x01, 0x7f,
        0x38, 0xef, 0xcd, 0xab, 0x02, 0x03, 0x04, 0x05, 0x5a,
    };
    assert(!memcmp(view, expected, sizeof expected));

    uint64_t regs[32] = { BASE + sizeof probe->memory - 15 };
    assert(nvn_front_dispatch(front, defaults, regs).kind == NVN_FRONT_INVALID_GUEST);
    probe->unreadable_byte = TEXTURE_VIEW + 7;
    regs[0] = TEXTURE_VIEW;
    assert(nvn_front_dispatch(front, 0x75005eea70ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    assert(!memcmp(view, expected, sizeof expected));
    probe->unreadable_byte = 0;
}

static void test_vertex_state(NvnFront *front, Probe *probe, uint64_t loader)
{
    const uint64_t attrib_default = get_proc(front, probe, loader, DEVICE, "nvnVertexAttribStateSetDefaults");
    const uint64_t attrib_format = get_proc(front, probe, loader, DEVICE, "nvnVertexAttribStateSetFormat");
    const uint64_t stream_stride = get_proc(front, probe, loader, DEVICE, "nvnVertexStreamStateSetStride");
    uint8_t *attrib_bytes = guest(probe, VERTEX_ATTRIBS, 17);
    uint8_t *stream_bytes = guest(probe, VERTEX_STREAMS, 17);
    memset(attrib_bytes, 0xa5, 17);
    memset(stream_bytes, 0xa5, 17);
    for (uint32_t i = 0; i < 4; ++i)
        invoke(front, attrib_default, VERTEX_ATTRIBS + i * 4, 0, 0);
    invoke(front, 0x75005ec580ULL, VERTEX_STREAMS, 0, 0);
    invoke(front, 0x75005ec580ULL, VERTEX_STREAMS + 8, 0, 0);
    assert(attrib_bytes[16] == 0xa5 && stream_bytes[16] == 0xa5);

    uint32_t preserved = 0x80000020u;
    memcpy(attrib_bytes, &preserved, sizeof preserved);
    invoke(front, 0x75005ec530ULL, VERTEX_ATTRIBS, 17, 0);
    invoke(front, attrib_format, VERTEX_ATTRIBS, 0x22, 0);
    invoke(front, attrib_format, VERTEX_ATTRIBS + 4, 0x25, 12);
    invoke(front, attrib_format, VERTEX_ATTRIBS + 8, 0x16, 16);
    uint32_t packed;
    memcpy(&packed, attrib_bytes, sizeof packed);
    assert(packed == (preserved | (0x22u << 21) | 0x40u | 17u));
    assert(invoke(front, 0x75005ec570ULL, VERTEX_ATTRIBS, 0, 0) == 17);
    invoke(front, 0x75005ec530ULL, VERTEX_ATTRIBS, 0, 0);

    invoke(front, stream_stride, VERTEX_STREAMS, UINT64_C(0xdeadbeef00000018), 0);
    invoke(front, 0x75005ec5a0ULL, VERTEX_STREAMS + 8, UINT64_C(0xdeadbeef00000002), 0);
    assert(invoke(front, 0x75005ec5b0ULL, VERTEX_STREAMS, 0, 0) == 24);
    assert(invoke(front, 0x75005ec5c0ULL, VERTEX_STREAMS + 8, 0, 0) == 2);

    NvnFrontVertexAttribInfo attribs[4];
    NvnFrontVertexStreamInfo streams[2];
    assert(nvn_front_read_vertex_attribs(front, VERTEX_ATTRIBS, 4, attribs));
    assert(nvn_front_read_vertex_streams(front, VERTEX_STREAMS, 2, streams));
    assert(attribs[0].enabled && attribs[0].format == 0x22 && !attribs[0].offset && !attribs[0].stream);
    assert(attribs[1].enabled && attribs[1].format == 0x25 && attribs[1].offset == 12 && !attribs[1].stream);
    assert(attribs[2].enabled && attribs[2].format == 0x16 && attribs[2].offset == 16 && !attribs[2].stream);
    assert(!attribs[3].enabled);
    assert(streams[0].stride == 24 && !streams[0].divisor && !streams[1].stride && streams[1].divisor == 2);

    uint8_t *output = guest(probe, OUTPUT, 17);
    memset(output, 0xa5, 17);
    invoke(front, 0x75005ec550ULL, VERTEX_ATTRIBS + 4, OUTPUT, OUTPUT + 8);
    uint32_t format;
    uint64_t offset;
    memcpy(&format, output, sizeof format);
    memcpy(&offset, output + 8, sizeof offset);
    assert(format == 0x25 && offset == 12 && output[4] == 0xa5 && output[16] == 0xa5);

    uint64_t invalid[32] = { VERTEX_ATTRIBS + 4, OUTPUT, BASE + sizeof probe->memory - 7 };
    memset(output, 0xa5, 17);
    assert(nvn_front_dispatch(front, 0x75005ec550ULL, invalid).kind == NVN_FRONT_INVALID_GUEST);
    assert(output[0] == 0xa5);
    assert(!nvn_front_read_vertex_attribs(front, VERTEX_ATTRIBS, NVN_FRONT_MAX_VERTEX_ATTRIBS + 1, attribs));
    assert(!nvn_front_read_vertex_streams(front, VERTEX_STREAMS, NVN_FRONT_MAX_VERTEX_STREAMS + 1, streams));
    assert(nvn_front_read_vertex_attribs(front, 0, 0, NULL));
    assert(nvn_front_read_vertex_streams(front, 0, 0, NULL));
    probe->unreadable_byte = VERTEX_ATTRIBS + 15;
    assert(!nvn_front_read_vertex_attribs(front, VERTEX_ATTRIBS, 4, attribs));
    assert(attribs[0].enabled && attribs[0].format == 0x22);
    probe->unreadable_byte = VERTEX_STREAMS + 15;
    assert(!nvn_front_read_vertex_streams(front, VERTEX_STREAMS, 2, streams));
    assert(streams[0].stride == 24);
    probe->unreadable_byte = 0;
}

static void test_raster_state(NvnFront *front, Probe *probe, uint64_t loader)
{
    const uint64_t color_default = get_proc(front, probe, loader, DEVICE, "nvnColorStateSetDefaults");
    memset(guest(probe, COLOR_STATE, 0x51), 0xa5, 0x51);
    invoke(front, color_default, COLOR_STATE, 0, 0);
    invoke(front, 0x75005ec3b0ULL, CHANNEL_MASK, 0, 0);
    invoke(front, 0x75005ec7b0ULL, POLYGON_STATE, 0, 0);
    invoke(front, 0x75005ec5d0ULL, DEPTH_STENCIL_STATE, 0, 0);
    invoke(front, 0x75005ec1d0ULL, BLEND_STATE, 0, 0);
    invoke(front, 0x75005ec880ULL, MULTISAMPLE_STATE, 0, 0);

    NvnFrontColorInfo color;
    uint32_t masks;
    NvnFrontPolygonInfo polygon;
    NvnFrontDepthStencilInfo depth;
    NvnFrontBlendInfo blend;
    NvnFrontMultisampleInfo multisample;
    assert(nvn_front_read_color(front, COLOR_STATE, &color));
    assert(!color.blend_mask && color.logic_op == 3 && color.alpha_func == 8);
    assert(nvn_front_read_channel_masks(front, CHANNEL_MASK, &masks) && masks == UINT32_MAX);
    assert(nvn_front_read_polygon(front, POLYGON_STATE, &polygon));
    assert(!polygon.cull_face && polygon.front_face == 1 && polygon.mode == 2 && !polygon.offset_enables);
    assert(nvn_front_read_depth_stencil(front, DEPTH_STENCIL_STATE, &depth));
    assert(!depth.depth_test && depth.depth_write && !depth.stencil_test && depth.depth_func == 2);
    assert(depth.front.func == 8 && depth.front.fail == 1 && depth.front.depth_fail == 1 && depth.front.depth_pass == 1);
    assert(depth.back.func == 8 && depth.back.fail == 1);
    assert(nvn_front_read_blend(front, BLEND_STATE, &blend));
    assert(!blend.target && blend.src_rgb == 2 && blend.dst_rgb == 1 && blend.rgb_equation == 1 && blend.alpha_equation == 1);
    assert(nvn_front_read_multisample(front, MULTISAMPLE_STATE, &multisample));
    assert(multisample.enabled && !multisample.alpha_to_coverage && multisample.alpha_to_coverage_dither);
    uint8_t *sample_bytes = guest(probe, MULTISAMPLE_STATE, 25);
    for (unsigned i = 4; i < 20; ++i)
        assert(sample_bytes[i] == 0x88);
    assert(sample_bytes[20] == 0 && sample_bytes[24] == 0xa5);

    invoke(front, 0x75005ec450ULL, COLOR_STATE, 0, 1);
    invoke(front, 0x75005ec450ULL, COLOR_STATE, 7, 0x100);
    invoke(front, 0x75005ec490ULL, COLOR_STATE, 6, 0);
    invoke(front, 0x75005ec4b0ULL, COLOR_STATE, 4, 0);
    assert(nvn_front_read_color(front, COLOR_STATE, &color));
    assert(color.blend_mask == 1 && color.logic_op == 6 && color.alpha_func == 4);
    assert(invoke(front, 0x75005ec4d0ULL, COLOR_STATE, 0, 0) == 1);
    assert(invoke(front, 0x75005ec4d0ULL, COLOR_STATE, 7, 0) == 0);

    uint64_t regs[32] = { CHANNEL_MASK, 7, 1, 0x100, 0x102, 0 };
    assert(nvn_front_dispatch(front, 0x75005ec3c0ULL, regs).kind == NVN_FRONT_HANDLED);
    assert(nvn_front_read_channel_masks(front, CHANNEL_MASK, &masks) && masks == 0x5fffffffu);
    uint8_t *channels = guest(probe, OUTPUT, 8);
    memset(channels, 0xa5, 8);
    for (unsigned i = 0; i < 4; ++i)
        regs[i + 2] = OUTPUT + i * 2;
    assert(nvn_front_dispatch(front, 0x75005ec410ULL, regs).kind == NVN_FRONT_HANDLED);
    const uint8_t expected_channels[] = { 1, 0xa5, 0, 0xa5, 1, 0xa5, 0, 0xa5 };
    assert(!memcmp(channels, expected_channels, sizeof expected_channels));

    invoke(front, 0x75005ec7c0ULL, POLYGON_STATE, 2, 0);
    invoke(front, 0x75005ec7e0ULL, POLYGON_STATE, 0, 0);
    invoke(front, 0x75005ec800ULL, POLYGON_STATE, 1, 0);
    invoke(front, 0x75005ec820ULL, POLYGON_STATE, 7, 0);
    assert(nvn_front_read_polygon(front, POLYGON_STATE, &polygon));
    assert(polygon.cull_face == 2 && !polygon.front_face && polygon.mode == 1 && polygon.offset_enables == 7);
    assert(invoke(front, 0x75005ec870ULL, POLYGON_STATE, 0, 0) == 7);

    invoke(front, 0x75005ec5f0ULL, DEPTH_STENCIL_STATE, 1, 0);
    invoke(front, 0x75005ec610ULL, DEPTH_STENCIL_STATE, 0x100, 0);
    invoke(front, 0x75005ec630ULL, DEPTH_STENCIL_STATE, 7, 0);
    invoke(front, 0x75005ec650ULL, DEPTH_STENCIL_STATE, 1, 0);
    invoke(front, 0x75005ec670ULL, DEPTH_STENCIL_STATE, 1, 3);
    regs[0] = DEPTH_STENCIL_STATE;
    regs[1] = 1; regs[2] = 2; regs[3] = 3; regs[4] = 4;
    assert(nvn_front_dispatch(front, 0x75005ec6b0ULL, regs).kind == NVN_FRONT_HANDLED);
    regs[1] = 2; regs[2] = 5; regs[3] = 6; regs[4] = 7;
    assert(nvn_front_dispatch(front, 0x75005ec6b0ULL, regs).kind == NVN_FRONT_HANDLED);
    uint64_t raw_depth;
    memcpy(&raw_depth, guest(probe, DEPTH_STENCIL_STATE, 8), sizeof raw_depth);
    assert(raw_depth == UINT64_C(0x6758342300000075));
    assert(nvn_front_read_depth_stencil(front, DEPTH_STENCIL_STATE, &depth));
    assert(depth.depth_test && !depth.depth_write && depth.stencil_test && depth.depth_func == 7);
    assert(depth.front.func == 3 && depth.front.fail == 2 && depth.front.depth_fail == 3 && depth.front.depth_pass == 4);
    assert(depth.back.func == 8 && depth.back.fail == 5 && depth.back.depth_fail == 6 && depth.back.depth_pass == 7);
    regs[2] = OUTPUT; regs[3] = OUTPUT + 4; regs[4] = OUTPUT + 8;
    assert(nvn_front_dispatch(front, 0x75005ec760ULL, regs).kind == NVN_FRONT_HANDLED);
    uint32_t ops[3];
    memcpy(ops, guest(probe, OUTPUT, sizeof ops), sizeof ops);
    assert(ops[0] == 5 && ops[1] == 6 && ops[2] == 7);

    invoke(front, 0x75005ec1f0ULL, BLEND_STATE, 2, 0);
    invoke(front, 0x75005ec250ULL, BLEND_STATE, 2, 3);
    regs[0] = BLEND_STATE; regs[1] = 3; regs[2] = 4; regs[3] = 5; regs[4] = 6;
    assert(nvn_front_dispatch(front, 0x75005ec210ULL, regs).kind == NVN_FRONT_HANDLED);
    assert(nvn_front_read_blend(front, BLEND_STATE, &blend));
    assert(blend.target == 2 && blend.src_rgb == 3 && blend.dst_rgb == 4 && blend.src_alpha == 5 && blend.dst_alpha == 6);
    assert(blend.rgb_equation == 2 && blend.alpha_equation == 3 && !blend.advanced_mode);

    invoke(front, 0x75005ec8a0ULL, MULTISAMPLE_STATE, 0, 0);
    invoke(front, 0x75005ec8c0ULL, MULTISAMPLE_STATE, 1, 0);
    invoke(front, 0x75005ec8e0ULL, MULTISAMPLE_STATE, 0, 0);
    invoke(front, 0x75005ec900ULL, MULTISAMPLE_STATE, 4, 0);
    invoke(front, 0x75005ec960ULL, MULTISAMPLE_STATE, 8, 0);
    assert(nvn_front_read_multisample(front, MULTISAMPLE_STATE, &multisample));
    assert(!multisample.enabled && multisample.alpha_to_coverage && !multisample.alpha_to_coverage_dither);
    assert(multisample.samples == 4 && multisample.raster_samples == 8);
    probe->unreadable_byte = DEPTH_STENCIL_STATE + 7;
    assert(!nvn_front_read_depth_stencil(front, DEPTH_STENCIL_STATE, &depth));
    assert(depth.depth_func == 7);
    probe->unreadable_byte = 0;
}

static void test_command_buffer(NvnFront *front, Probe *probe, uint64_t loader)
{
    const uint64_t initialize = get_proc(front, probe, loader, DEVICE, "nvnCommandBufferInitialize");
    const uint64_t finalize = get_proc(front, probe, loader, DEVICE, "nvnCommandBufferFinalize");
    const uint64_t add_command = get_proc(front, probe, loader, DEVICE, "nvnCommandBufferAddCommandMemory");
    const uint64_t callback = UINT64_C(0x7101144990);
    const uint64_t callback_data = UINT64_C(0x123456789abcdef0);
    uint8_t *object = guest(probe, COMMAND_BUFFER, 0xc1);
    uint8_t *control = guest(probe, CONTROL_MEMORY, 0x100);
    memset(object, 0x5a, 0xc1);
    memset(control, 0x6b, 0x100);
    uint64_t regs[32] = { COMMAND_BUFFER, 0 };
    assert(nvn_front_dispatch(front, initialize, regs).kind == NVN_FRONT_INVALID_GUEST);
    regs[0] = BASE + sizeof probe->memory - 0xbf;
    regs[1] = DEVICE;
    assert(nvn_front_dispatch(front, initialize, regs).kind == NVN_FRONT_INVALID_GUEST);
    assert(!probe->command_calls);

    probe->fail_command = true;
    assert(invoke(front, initialize, COMMAND_BUFFER, DEVICE, 0) == 0);
    NvnFrontCommandBufferInfo info;
    assert(!nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(object[0] == 0x5a && object[0xc0] == 0x5a);
    probe->fail_command = false;
    assert(invoke(front, initialize, COMMAND_BUFFER, DEVICE, 0) == 1);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.native == &probe->command[0] && info.device == DEVICE && !info.recording_mode);
    assert(invoke(front, 0x75005c4cd0ULL, COMMAND_BUFFER, 0, 0) == 16);
    assert(invoke(front, 0x75005c4d00ULL, COMMAND_BUFFER, 0, 0) == 16);
    assert(nvn_front_charge_command_memory(front, COMMAND_BUFFER, 0, 0));
    assert(!nvn_front_charge_command_memory(front, COMMAND_BUFFER, 0, 16));
    uint64_t guest_device;
    memcpy(&guest_device, object + 0x70, sizeof guest_device);
    assert(guest_device == DEVICE && object[0x14] == 1 && object[0x15] == 1);
    assert(!object[0xaf] && object[0xb0] == 0x5a && object[0xc0] == 0x5a);

    invoke(front, 0x75005c4970ULL, COMMAND_BUFFER, callback, 0);
    invoke(front, 0x75005c4980ULL, COMMAND_BUFFER, callback_data, 0);
    invoke(front, 0x75005c4d10ULL, COMMAND_BUFFER, UINT64_C(0xdeadbeef00000002), 0);
    uint64_t raw_callback[2];
    memcpy(raw_callback, object + 0x78, sizeof raw_callback);
    assert(raw_callback[0] == callback && raw_callback[1] == callback_data && object[0x18] == 2);

    regs[0] = COMMAND_BUFFER;
    regs[1] = POOL;
    regs[2] = UINT64_MAX;
    regs[3] = 0x100;
    assert(nvn_front_dispatch(front, add_command, regs).kind == NVN_FRONT_INVALID_GUEST);
    regs[2] = 0xffc;
    assert(nvn_front_dispatch(front, add_command, regs).kind == NVN_FRONT_INVALID_GUEST);
    regs[2] = 0x100;
    assert(nvn_front_dispatch(front, add_command, regs).kind == NVN_FRONT_HANDLED);
    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 0x100);
    assert(control[0] == 0x6b && control[0xff] == 0x6b);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.memory_callback == callback && info.memory_callback_data == callback_data && info.recording_mode == 2);
    assert(info.command_pool == POOL && info.command_offset == 0x100);
    assert(info.command_address == QUEUE_MEMORY + 0x100 && info.command_size == 0x100);
    assert(info.control_address == CONTROL_MEMORY && info.control_size == 0x100);
    assert(!info.command_used && !info.control_used);
    assert(nvn_front_charge_command_memory(front, COMMAND_BUFFER, 0, 16));
    assert(nvn_front_charge_command_memory(front, COMMAND_BUFFER, 44, 0));
    assert(!nvn_front_charge_command_memory(front, COMMAND_BUFFER, 4, 0xf8));
    assert(!nvn_front_charge_command_memory(front, COMMAND_BUFFER, 0xd8, 8));
    assert(!nvn_front_charge_command_memory(front, COMMAND_BUFFER, 3, 0));
    assert(!nvn_front_charge_command_memory(front, COMMAND_BUFFER, 0, 4));
    assert(invoke(front, 0x75005c4ca0ULL, COMMAND_BUFFER, 0, 0) == 0x100);
    assert(invoke(front, 0x75005c4cb0ULL, COMMAND_BUFFER, 0, 0) == 44);
    assert(invoke(front, 0x75005c4cc0ULL, COMMAND_BUFFER, 0, 0) == 0x100 - 44);
    assert(invoke(front, 0x75005c4cd0ULL, COMMAND_BUFFER, 0, 0) == 0x100);
    assert(invoke(front, 0x75005c4cf0ULL, COMMAND_BUFFER, 0, 0) == 16);
    assert(invoke(front, 0x75005c4d00ULL, COMMAND_BUFFER, 0, 0) == 0xf0);

    uint64_t command_cursor, control_cursor, command_base, control_base;
    memcpy(&command_cursor, object, 8);
    memcpy(&control_cursor, object + 0x28, 8);
    memcpy(&command_base, object + 0x38, 8);
    memcpy(&control_base, object + 0x40, 8);
    assert(command_cursor == QUEUE_MEMORY + 0x100 + 44 && control_cursor == CONTROL_MEMORY + 16);
    assert(command_base == QUEUE_MEMORY + 0x100 && control_base == CONTROL_MEMORY);
    assert(nvn_front_dispatch(front, add_command, regs).kind == NVN_FRONT_HANDLED);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(!info.command_used && info.control_used == 16);
    assert(nvn_front_charge_command_memory(front, COMMAND_BUFFER, 4, 8));
    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 0x100);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.command_used == 4 && !info.control_used);

    regs[1] = CONTROL_MEMORY + 1;
    regs[2] = 0x100;
    assert(nvn_front_dispatch(front, 0x75005c4bb0ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    regs[1] = CONTROL_MEMORY;
    regs[2] = 15;
    assert(nvn_front_dispatch(front, 0x75005c4bb0ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    regs[0] = POOL;
    assert(nvn_front_dispatch(front, 0x75005e2680ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    regs[0] = COMMAND_BUFFER;
    NvnFrontReply result = nvn_front_dispatch(front, 0x75005c4d20ULL, regs);
    assert(result.kind == NVN_FRONT_BOUNDARY && !strcmp(result.name, "nvnCommandBufferBeginRecording"));
    result = nvn_front_dispatch(front, 0x75005c4de0ULL, regs);
    assert(result.kind == NVN_FRONT_BOUNDARY && !strcmp(result.name, "nvnCommandBufferEndRecording"));

    probe->fail_command = true;
    assert(invoke(front, initialize, COMMAND_BUFFER, DEVICE, 0) == 0);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.native == &probe->command[0] && info.command_pool == POOL && info.control_size == 0x100);
    probe->fail_command = false;
    assert(invoke(front, initialize, COMMAND_BUFFER, DEVICE, 0) == 1);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.native == &probe->command[1] && !info.command_pool && !info.control_size && !info.memory_callback);
    assert(!info.command_used && !info.control_used);
    assert(probe->command_destroys == 1);
    invoke(front, finalize, COMMAND_BUFFER, 0, 0);
    assert(!nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(probe->command_destroys == 2 && !probe->commands);
    assert(nvn_front_dispatch(front, finalize, regs).kind == NVN_FRONT_INVALID_GUEST);
}

static void *create_program(void *opaque, void *device)
{
    Probe *probe = opaque;
    assert(device == &probe->device);
    if (probe->fail_program)
        return NULL;
    const unsigned index = (probe->programs & 1u) ? 1u : 0u;
    assert(!(probe->programs & (1u << index)));
    probe->programs |= 1u << index;
    return &probe->program[index];
}

static void destroy_program(void *opaque, void *program)
{
    Probe *probe = opaque;
    assert(program == &probe->program[0] || program == &probe->program[1]);
    const unsigned mask = program == &probe->program[0] ? 1u : 2u;
    assert(probe->programs & mask);
    probe->programs &= ~mask;
    ++probe->program_destroys;
}

static NvnFrontProgramLinkResult link_program(void *opaque, void *program,
    const NvnFrontShaderData *shaders, size_t count)
{
    Probe *probe = opaque;
    assert(program == &probe->program[0] && (probe->programs & 1u));
    ++probe->program_links;
    if (!count)
        return NVN_FRONT_PROGRAM_LINKED;
    assert(count == 1 && shaders[0].code_address == QUEUE_MEMORY + 0x800);
    assert(shaders[0].control_address == SHADER_CONTROL);
    assert(shaders[0].code == guest(probe, QUEUE_MEMORY + 0x800, 0x100));
    assert(shaders[0].control == guest(probe, SHADER_CONTROL, NVN_SHADER_CONTROL_PREFIX_BYTES));
    assert(shaders[0].parsed.stage == NVN_SHADER_COMPUTE && shaders[0].parsed.code_bytes == 0x100);
    return probe->program_result;
}

static void test_program(NvnFront *front, Probe *probe, uint64_t loader)
{
    const uint64_t initialize = get_proc(front, probe, loader, DEVICE, "nvnProgramInitialize");
    const uint64_t set_shaders = get_proc(front, probe, loader, DEVICE, "nvnProgramSetShaders");
    const uint64_t finalize = get_proc(front, probe, loader, DEVICE, "nvnProgramFinalize");
    uint8_t *object = guest(probe, PROGRAM, 0xa1);
    memset(object, 0xa5, 0xa1);
    uint64_t regs[32] = { PROGRAM, SECOND_DEVICE };
    assert(nvn_front_dispatch(front, initialize, regs).kind == NVN_FRONT_INVALID_GUEST);
    probe->fail_program = true;
    assert(invoke(front, initialize, PROGRAM, DEVICE, 0) == 0);
    NvnFrontProgramInfo info;
    assert(!nvn_front_get_program(front, PROGRAM, &info) && object[0] == 0xa5);
    probe->fail_program = false;
    assert(invoke(front, initialize, PROGRAM, DEVICE, 0) == 1);
    assert(nvn_front_get_program(front, PROGRAM, &info));
    assert(info.device == DEVICE && info.native == &probe->program[0] && !info.linked);
    uint64_t device;
    memcpy(&device, object + 8, sizeof device);
    assert(device == DEVICE && !object[0] && !object[0x9f] && object[0xa0] == 0xa5);

    const uint64_t entry[] = { QUEUE_MEMORY + 0x800, SHADER_CONTROL };
    uint32_t control[NVN_SHADER_CONTROL_PREFIX_BYTES / 4] = {0};
    control[0] = 0x98761234;
    control[1] = 201;
    control[0x30 / 4] = NVN_SHADER_CONTROL_PREFIX_BYTES;
    control[0x44 / 4] = 0x100;
    control[0x54 / 4] = NVN_SHADER_COMPUTE << 16;
    memcpy(guest(probe, SHADER_ENTRIES, sizeof entry), entry, sizeof entry);
    memcpy(guest(probe, SHADER_CONTROL, sizeof control), control, sizeof control);
    regs[1] = 7;
    regs[2] = SHADER_ENTRIES;
    assert(nvn_front_dispatch(front, set_shaders, regs).kind == NVN_FRONT_BOUNDARY);
    regs[1] = 1;
    probe->unreadable_byte = QUEUE_MEMORY + 0x8ff;
    assert(nvn_front_dispatch(front, set_shaders, regs).kind == NVN_FRONT_INVALID_GUEST);
    probe->unreadable_byte = 0;
    assert(!probe->program_links);

    probe->program_result = NVN_FRONT_PROGRAM_LINKED;
    assert(invoke(front, set_shaders, PROGRAM, 1, SHADER_ENTRIES) == 1);
    assert(nvn_front_get_program(front, PROGRAM, &info) && info.linked);
    probe->program_result = NVN_FRONT_PROGRAM_UNSUPPORTED;
    assert(nvn_front_dispatch(front, set_shaders, regs).kind == NVN_FRONT_BOUNDARY);
    assert(nvn_front_get_program(front, PROGRAM, &info) && info.linked);
    probe->program_result = NVN_FRONT_PROGRAM_FAILED;
    assert(invoke(front, set_shaders, PROGRAM, 1, SHADER_ENTRIES) == 0);
    assert(nvn_front_get_program(front, PROGRAM, &info) && !info.linked);
    assert(invoke(front, set_shaders, PROGRAM, 0, 0) == 1);
    assert(nvn_front_get_program(front, PROGRAM, &info) && !info.linked && probe->program_links == 4);

    assert(invoke(front, initialize, PROGRAM, DEVICE, 0) == 1);
    assert(nvn_front_get_program(front, PROGRAM, &info) && info.native == &probe->program[1]);
    assert(probe->program_destroys == 1);
    invoke(front, finalize, PROGRAM, 0, 0);
    assert(!nvn_front_get_program(front, PROGRAM, &info) && probe->program_destroys == 2);
    assert(nvn_front_dispatch(front, set_shaders, regs).kind == NVN_FRONT_INVALID_GUEST);
}

static void test_buffer(NvnFront *front, Probe *probe, uint64_t loader)
{
    const uint64_t initialize = get_proc(front, probe, loader, DEVICE, "nvnBufferInitialize");
    const uint64_t set_storage = get_proc(front, probe, loader, DEVICE, "nvnBufferBuilderSetStorage");
    uint8_t *builder = guest(probe, BUFFER_BUILDER, 0x40);
    uint8_t *object = guest(probe, BUFFER, 0x31);
    memset(builder, 0x6b, 0x40);
    memset(object, 0xa5, 0x31);
    invoke(front, 0x75005c34d0ULL, BUFFER_BUILDER, DEVICE, 0);
    invoke(front, 0x75005c34e0ULL, BUFFER_BUILDER, 0, 0);
    assert(invoke(front, 0x75005c3500ULL, BUFFER_BUILDER, 0, 0) == DEVICE);
    for (size_t i = 0x10; i < 0x40; ++i)
        assert(builder[i] == (i < 0x28 ? 0 : 0x6b));

    uint64_t regs[32] = { BUFFER_BUILDER, POOL, UINT64_C(0x100001000), UINT64_C(0x1000000c0) };
    assert(nvn_front_dispatch(front, set_storage, regs).kind == NVN_FRONT_HANDLED);
    assert(invoke(front, 0x75005c3510ULL, BUFFER_BUILDER, 0, 0) == POOL);
    assert(invoke(front, 0x75005c3520ULL, BUFFER_BUILDER, 0, 0) == UINT64_C(0x100001000));
    assert(invoke(front, 0x75005c3530ULL, BUFFER_BUILDER, 0, 0) == UINT64_C(0x1000000c0));
    assert(invoke(front, initialize, BUFFER, BUFFER_BUILDER, 0) == 0);
    assert(object[0] == 0xa5 && object[0x30] == 0xa5);

    regs[2] = 0x200;
    regs[3] = 0x180;
    assert(nvn_front_dispatch(front, set_storage, regs).kind == NVN_FRONT_HANDLED);
    uint64_t invalid[32] = { QUEUE_MEMORY + 0x220, BUFFER_BUILDER };
    assert(nvn_front_dispatch(front, initialize, invalid).kind == NVN_FRONT_INVALID_GUEST);
    assert(invoke(front, initialize, BUFFER, BUFFER_BUILDER, 0) == 1);
    assert(!object[0] && !object[0x2f] && object[0x30] == 0xa5);
    assert(!memcmp(object + 8, builder + 8, 0x20));
    assert(invoke(front, 0x75005c35d0ULL, BUFFER, 0, 0) == QUEUE_MEMORY + 0x200);
    assert(invoke(front, 0x75005c3600ULL, BUFFER, 0, 0) == QUEUE_MEMORY + 0x200);
    assert(invoke(front, 0x75005c3630ULL, BUFFER, 0, 0) == POOL);
    assert(invoke(front, 0x75005c3640ULL, BUFFER, 0, 0) == 0x200);
    assert(invoke(front, 0x75005c3650ULL, BUFFER, 0, 0) == 0x180);
    invoke(front, 0x75005c3610ULL, BUFFER, 0x40, 0x140);
    invoke(front, 0x75005c3620ULL, BUFFER, 0x180, 0);
    invalid[0] = BUFFER;
    invalid[1] = 0x170;
    invalid[2] = 0x11;
    assert(nvn_front_dispatch(front, 0x75005c3610ULL, invalid).kind == NVN_FRONT_INVALID_GUEST);

    regs[2] = 0xf00;
    assert(nvn_front_dispatch(front, set_storage, regs).kind == NVN_FRONT_HANDLED);
    assert(invoke(front, initialize, BUFFER, BUFFER_BUILDER, 0) == 0);
    assert(invoke(front, 0x75005c3640ULL, BUFFER, 0, 0) == 0x200);
    invalid[0] = POOL;
    assert(nvn_front_dispatch(front, 0x75005e2680ULL, invalid).kind == NVN_FRONT_INVALID_GUEST);
    invoke(front, 0x75005c3590ULL, BUFFER, 0, 0);
    invalid[0] = BUFFER;
    assert(nvn_front_dispatch(front, 0x75005c35d0ULL, invalid).kind == NVN_FRONT_INVALID_GUEST);

    regs[2] = 0x1000;
    regs[3] = 0;
    assert(nvn_front_dispatch(front, set_storage, regs).kind == NVN_FRONT_HANDLED);
    assert(invoke(front, initialize, BUFFER, BUFFER_BUILDER, 0) == 1);
    assert(invoke(front, 0x75005c3650ULL, BUFFER, 0, 0) == 0);
    invoke(front, 0x75005c3590ULL, BUFFER, 0, 0);
}

static void test_sampler_descriptors(NvnFront *front, Probe *probe)
{
    NvnFrontSamplerInfo info;
    assert(nvn_front_get_sampler_descriptor(front, SAMPLER_POOL, 0, &info));
    assert(info.min_filter == 1 && info.mag_filter == 1 && info.max_anisotropy == 1.0f);
    assert(nvn_front_get_sampler_descriptor(front, SAMPLER_POOL, 2, &info));
    assert(!info.min_filter && !info.mag_filter && info.max_anisotropy == 2.0f);
    assert(info.lod_max == 1000.0f && info.wrap[0] == 7 && info.wrap[2] == 7);
    assert(!nvn_front_get_sampler_descriptor(front, SAMPLER_POOL, 3, &info));

    uint8_t *builder = guest(probe, SAMPLER_BUILDER, 0x31);
    uint8_t *object = guest(probe, SAMPLER, 0x41);
    memset(builder, 0xa5, 0x31);
    memset(object, 0xa5, 0x41);
    invoke(front, 0x75005eba50ULL, SAMPLER_BUILDER, DEVICE, 0);
    invoke(front, 0x75005eba60ULL, SAMPLER_BUILDER, 0, 0);
    assert(builder[0x30] == 0xa5);
    invoke(front, 0x75005eba90ULL, SAMPLER_BUILDER, 5, 1);
    uint64_t regs[32] = { SAMPLER_BUILDER, 7, 6, 1 };
    assert(nvn_front_dispatch(front, 0x75005ebab0ULL, regs).kind == NVN_FRONT_HANDLED);
    invoke(front, 0x75005ebaf0ULL, SAMPLER_BUILDER, 1, 4);
    invoke(front, 0x75005ebbb0ULL, SAMPLER_BUILDER, 2, 0);

    const uint32_t color[] = { 0x3f800000, 0, 0x3f000000, 0x3f800000 };
    memcpy(guest(probe, BORDER_COLOR, sizeof color), color, sizeof color);
    invoke(front, 0x75005ebb10ULL, SAMPLER_BUILDER, BORDER_COLOR, 0);
    assert(nvn_front_dispatch(front, 0x75005ebad0ULL, regs).kind == NVN_FRONT_BOUNDARY);
    uint32_t scalar[2] = { 0x3f800000, 0x41800000 };
    assert(nvn_front_dispatch_with_floats(front, 0x75005ebad0ULL, regs, scalar).kind == NVN_FRONT_HANDLED);
    scalar[0] = 0xbf000000;
    assert(nvn_front_dispatch_with_floats(front, 0x75005ebae0ULL, regs, scalar).kind == NVN_FRONT_HANDLED);
    scalar[0] = 0x40800000;
    assert(nvn_front_dispatch_with_floats(front, 0x75005ebba0ULL, regs, scalar).kind == NVN_FRONT_HANDLED);
    scalar[0] = 0x3e800000;
    assert(nvn_front_dispatch_with_floats(front, 0x75005ebbc0ULL, regs, scalar).kind == NVN_FRONT_HANDLED);

    assert(invoke(front, 0x75005ebd40ULL, SAMPLER, SAMPLER_BUILDER, 0) == 1);
    assert(!object[0] && !object[0x3f] && object[0x40] == 0xa5);
    assert(!memcmp(object + 8, builder, 0x30));
    invoke(front, 0x75005ebfd0ULL, SAMPLER_POOL, 3, SAMPLER);
    invoke(front, 0x75005ebd70ULL, SAMPLER, 0, 0);
    assert(nvn_front_get_sampler_descriptor(front, SAMPLER_POOL, 3, &info));
    assert(info.min_filter == 5 && info.mag_filter == 1 && info.reduction == 2);
    assert(info.wrap[0] == 7 && info.wrap[1] == 6 && info.wrap[2] == 1);
    assert(info.compare_mode == 1 && info.compare_func == 4);
    assert(info.lod_min == 1.0f && info.lod_max == 16.0f && info.lod_bias == -0.5f);
    assert(info.max_anisotropy == 4.0f && info.lod_snap == 0.25f);
    assert(!memcmp(info.border_color_bits, color, sizeof color));

    regs[0] = SAMPLER_POOL;
    regs[1] = 3;
    regs[2] = SAMPLER;
    assert(nvn_front_dispatch(front, 0x75005ebfd0ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    invoke(front, 0x75005eba60ULL, SAMPLER_BUILDER, 0, 0);
    invoke(front, 0x75005ebff0ULL, SAMPLER_POOL, 3, SAMPLER_BUILDER);
    assert(nvn_front_get_sampler_descriptor(front, SAMPLER_POOL, 3, &info));
    assert(info.min_filter == 1 && info.lod_max == 1000.0f && info.wrap[0] == 1);
}

static void test_texture_descriptors(NvnFront *front, Probe *probe)
{
    const unsigned previous_destroys = probe->texture_destroys;
    assert(!probe->textures);
    assert(invoke(front, 0x75005ef720ULL, TEXTURE, TEXTURE_BUILDER, 0) == 1);
    NvnFrontTextureDescriptorInfo info;
    assert(!nvn_front_get_texture_descriptor(front, TEXTURE_POOL, 1, &info));

    uint8_t *view = guest(probe, TEXTURE_VIEW, 16);
    const uint8_t original[16] = { 1, 0, 2, 0, 0, 1, 1, 0x2f, 0x38, 0, 0, 0, 2, 3, 4, 5 };
    memcpy(view, original, sizeof original);
    uint64_t regs[32] = { TEXTURE_POOL, 1, TEXTURE, TEXTURE_VIEW };
    probe->unreadable_byte = TEXTURE_VIEW + 15;
    assert(nvn_front_dispatch(front, 0x75005f0280ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    probe->unreadable_byte = 0;
    assert(nvn_front_dispatch(front, 0x75005f0280ULL, regs).kind == NVN_FRONT_HANDLED);
    memset(view, 0xa5, 16);
    assert(nvn_front_get_texture_descriptor(front, TEXTURE_POOL, 1, &info));
    assert(info.address == TEXTURE && info.has_view && !info.image);
    assert(info.texture.native == &probe->texture[0] && !memcmp(info.view, original, sizeof original));
    invoke(front, 0x75005f0410ULL, TEXTURE_POOL, 2, TEXTURE);
    assert(nvn_front_get_texture_descriptor(front, TEXTURE_POOL, 2, &info));
    assert(info.image && !info.has_view && info.texture.native == &probe->texture[0]);

    regs[1] = 4;
    regs[3] = 0;
    assert(nvn_front_dispatch(front, 0x75005f0280ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    assert(invoke(front, 0x75005d68c0ULL, DEVICE, 1, 3) == 0x300001);
    assert(invoke(front, 0x75005d6830ULL, DEVICE, 2, 0) == 2);
    assert(invoke(front, 0x75005d6890ULL, DEVICE, 3, 0) == 0x300000);
    assert(invoke(front, 0x75005d68a0ULL, DEVICE, 1, 0) == 1);
    assert(invoke(front, 0x75005d68b0ULL, DEVICE, UINT32_MAX, 0) == UINT64_MAX);

    invoke(front, 0x75005efe70ULL, TEXTURE, 0, 0);
    assert(probe->texture_destroys == previous_destroys);
    assert(invoke(front, 0x75005ef720ULL, TEXTURE, TEXTURE_BUILDER, 0) == 1);
    assert(nvn_front_get_texture_descriptor(front, TEXTURE_POOL, 1, &info));
    assert(info.texture.native == &probe->texture[0]);
    invoke(front, 0x75005f0280ULL, TEXTURE_POOL, 1, TEXTURE);
    assert(nvn_front_get_texture_descriptor(front, TEXTURE_POOL, 1, &info));
    assert(info.texture.native == &probe->texture[1] && !info.has_view);
    assert(nvn_front_get_texture_descriptor(front, TEXTURE_POOL, 2, &info));
    assert(info.texture.native == &probe->texture[0] && info.image);
    invoke(front, 0x75005efe70ULL, TEXTURE, 0, 0);
    assert(probe->texture_destroys == previous_destroys);
}

static void test_descriptor_pools(NvnFront *front, Probe *probe, uint64_t loader)
{
    const uint64_t texture_init = get_proc(front, probe, loader, DEVICE, "nvnTexturePoolInitialize");
    const uint64_t sampler_init = get_proc(front, probe, loader, DEVICE, "nvnSamplerPoolInitialize");
    uint8_t *texture_object = guest(probe, TEXTURE_POOL, 0x21);
    uint8_t *sampler_object = guest(probe, SAMPLER_POOL, 0x21);
    memset(texture_object, 0xa5, 0x21);
    memset(sampler_object, 0xa5, 0x21);
    uint8_t *texture_slots = guest(probe, QUEUE_MEMORY + 0x100, 0x80);
    uint8_t *sampler_slots = guest(probe, QUEUE_MEMORY + 0x200, 0x80);
    memset(texture_slots, 0x6b, 0x80);
    memset(sampler_slots, 0x6b, 0x80);

    uint64_t regs[32] = { TEXTURE_POOL, POOL, 0x100, 4 };
    assert(nvn_front_dispatch(front, texture_init, regs).kind == NVN_FRONT_HANDLED && regs[0] == 1);
    NvnFrontDescriptorPoolInfo info;
    assert(nvn_front_get_descriptor_pool(front, TEXTURE_POOL, false, &info));
    assert(!nvn_front_get_descriptor_pool(front, TEXTURE_POOL, true, &info));
    assert(info.device == DEVICE && info.memory_pool == POOL && info.count == 4);
    assert(info.offset == 0x100 && info.storage == QUEUE_MEMORY + 0x100);
    const uint32_t texture_null[8] = { 0x24900, 0, 0x600000 };
    assert(!memcmp(texture_slots, texture_null, sizeof texture_null));
    assert(texture_slots[0x20] == 0x6b && texture_slots[0x7f] == 0x6b);
    assert(texture_object[0x14] == 0xa5 && texture_object[0x18] == 0 && texture_object[0x20] == 0xa5);
    assert(invoke(front, 0x75005f05a0ULL, TEXTURE_POOL, 0, 0) == POOL);
    assert(invoke(front, 0x75005f05b0ULL, TEXTURE_POOL, 0, 0) == 0x100);
    assert(invoke(front, 0x75005f05c0ULL, TEXTURE_POOL, 0, 0) == 4);

    regs[0] = SAMPLER_POOL;
    regs[2] = 0x200;
    regs[3] = 2;
    assert(nvn_front_dispatch(front, sampler_init, regs).kind == NVN_FRONT_HANDLED && !regs[0]);
    assert(!nvn_front_get_descriptor_pool(front, SAMPLER_POOL, true, &info));
    assert(sampler_object[0] == 0xa5 && sampler_slots[0] == 0x6b);
    regs[0] = SAMPLER_POOL;
    regs[3] = 4;
    uint8_t *bias = guest(probe, DEVICE + 0x1cc0, 1);
    *bias = 1;
    assert(nvn_front_dispatch(front, sampler_init, regs).kind == NVN_FRONT_BOUNDARY);
    *bias = 0;
    assert(nvn_front_dispatch(front, sampler_init, regs).kind == NVN_FRONT_HANDLED && regs[0] == 1);
    const uint32_t sampler_defaults[3][8] = {
        { 0x00026092, 0x262, 0xf00000 }, { 0x00126092, 0x262, 0xf00000 }, { 0x00126092, 0x251, 0xf00000 },
    };
    assert(!memcmp(sampler_slots, sampler_defaults, sizeof sampler_defaults));
    assert(sampler_slots[0x60] == 0x6b && sampler_slots[0x7f] == 0x6b);
    assert(nvn_front_get_descriptor_pool(front, SAMPLER_POOL, true, &info));
    assert(info.device == DEVICE && info.memory_pool == POOL && info.count == 4);
    assert(info.offset == 0x200 && info.storage == QUEUE_MEMORY + 0x200);
    assert(invoke(front, 0x75005ec0e0ULL, SAMPLER_POOL, 0, 0) == POOL);
    assert(invoke(front, 0x75005ec0f0ULL, SAMPLER_POOL, 0, 0) == 0x200);
    assert(invoke(front, 0x75005ec100ULL, SAMPLER_POOL, 0, 0) == 4);

    test_texture_descriptors(front, probe);
    test_sampler_descriptors(front, probe);
    regs[0] = POOL;
    assert(nvn_front_dispatch(front, 0x75005e2680ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    invoke(front, 0x75005f0270ULL, TEXTURE_POOL, 0, 0);
    invoke(front, 0x75005ebfe0ULL, SAMPLER_POOL, 0, 0);
    assert(!nvn_front_get_descriptor_pool(front, TEXTURE_POOL, false, &info));
    assert(!nvn_front_get_descriptor_pool(front, SAMPLER_POOL, true, &info));
    assert(!probe->textures);
}

static void test_memory_pool(NvnFront *front, Probe *probe, uint64_t loader)
{
    const uint64_t set_device = get_proc(front, probe, loader, DEVICE, "nvnMemoryPoolBuilderSetDevice");
    const uint64_t set_storage = get_proc(front, probe, loader, DEVICE, "nvnMemoryPoolBuilderSetStorage");
    const uint64_t initialize = get_proc(front, probe, loader, DEVICE, "nvnMemoryPoolInitialize");
    const uint64_t map = get_proc(front, probe, loader, DEVICE, "nvnMemoryPoolMap");
    const uint64_t finalize_pool = get_proc(front, probe, loader, DEVICE, "nvnMemoryPoolFinalize");
    uint8_t *builder = guest(probe, POOL_BUILDER, 0x41);
    memset(builder, 0xa5, 0x41);
    invoke(front, set_device, POOL_BUILDER, DEVICE, 0);
    invoke(front, 0x75005e2360ULL, POOL_BUILDER, 0, 0);

    NvnFrontPoolConfig config;
    assert(nvn_front_read_pool_builder(front, POOL_BUILDER, &config));
    assert(config.device == DEVICE && config.flags == 0x21 && !config.storage && !config.size);
    for (size_t i = 0; i < 4; ++i)
        assert(builder[i] == 0xa5);
    for (size_t i = 0x20; i < 0x41; ++i)
        assert(builder[i] == (i < 0x24 || (i >= 0x28 && i < 0x38) ? 0 : 0xa5));

    memset(builder + 0x20, 0x5a, 4);
    invoke(front, 0x75005e2450ULL, POOL_BUILDER, 0xa2, 0);
    invoke(front, set_storage, POOL_BUILDER, QUEUE_MEMORY, UINT64_C(0x100001000));
    assert(invoke(front, 0x75005e2460ULL, POOL_BUILDER, 0, 0) == DEVICE);
    assert(invoke(front, 0x75005e2470ULL, POOL_BUILDER, 0, 0) == QUEUE_MEMORY);
    assert(invoke(front, 0x75005e2480ULL, POOL_BUILDER, 0, 0) == UINT64_C(0x100001000));
    assert(invoke(front, 0x75005e2490ULL, POOL_BUILDER, 0, 0) == 0xa2);
    for (size_t i = 0x20; i < 0x24; ++i)
        assert(!builder[i]);

    uint8_t *pool = guest(probe, POOL, 0x101);
    uint8_t *storage = guest(probe, QUEUE_MEMORY, 0x1000);
    memset(pool, 0x5a, 0x101);
    memset(storage, 0x6b, 0x1000);
    assert(invoke(front, initialize, POOL, POOL_BUILDER, 0) == 0);
    invoke(front, set_storage, POOL_BUILDER, QUEUE_MEMORY + 16, 0x1000);
    assert(invoke(front, initialize, POOL, POOL_BUILDER, 0) == 0);
    invoke(front, set_storage, POOL_BUILDER, QUEUE_MEMORY, 0x1000);
    probe->unreadable_byte = QUEUE_MEMORY + 0xfff;
    assert(invoke(front, initialize, POOL, POOL_BUILDER, 0) == 0);
    probe->unreadable_byte = 0;
    uint64_t regs[32] = { POOL, POOL_BUILDER };
    invoke(front, 0x75005e2450ULL, POOL_BUILDER, 0x200, 0);
    NvnFrontReply result = nvn_front_dispatch(front, initialize, regs);
    assert(result.kind == NVN_FRONT_BOUNDARY && !strcmp(result.name, "nvnMemoryPoolInitialize"));
    assert(regs[0] == POOL && regs[1] == POOL_BUILDER);
    for (size_t i = 0; i < 0x100; ++i)
        assert(pool[i] == 0x5a);

    invoke(front, 0x75005e2450ULL, POOL_BUILDER, 0x22, 0);
    assert(invoke(front, initialize, POOL, POOL_BUILDER, 0) == 1);
    assert(!pool[0] && !pool[0xff] && pool[0x100] == 0x5a);
    assert(storage[0] == 0x6b && storage[0xfff] == 0x6b);
    assert(invoke(front, map, POOL, 0, 0) == QUEUE_MEMORY);
    assert(invoke(front, 0x75005e2780ULL, POOL, 0, 0) == QUEUE_MEMORY);
    assert(invoke(front, 0x75005e27f0ULL, POOL, 0, 0) == 0x1000);
    assert(invoke(front, 0x75005e2800ULL, POOL, 0, 0) == 0x22);
    assert(nvn_front_get_pool(front, POOL, &config));
    assert(config.device == DEVICE && config.storage == QUEUE_MEMORY && config.size == 0x1000 && config.flags == 0x22);
    test_texture_builder(front, probe, loader);
    test_command_buffer(front, probe, loader);
    test_buffer(front, probe, loader);
    test_program(front, probe, loader);
    test_descriptor_pools(front, probe, loader);
    assert(nvn_front_dispatch(front, initialize, regs).kind == NVN_FRONT_INVALID_GUEST);
    regs[1] = UINT64_MAX;
    assert(nvn_front_dispatch(front, 0x75005e2760ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    regs[1] = 0xf00;
    regs[2] = 0x100;
    assert(nvn_front_dispatch(front, 0x75005e2760ULL, regs).kind == NVN_FRONT_HANDLED);
    probe->unreadable_byte = QUEUE_MEMORY + 0xfff;
    assert(nvn_front_dispatch(front, 0x75005e2770ULL, regs).kind == NVN_FRONT_INVALID_GUEST);
    probe->unreadable_byte = 0;
    assert(nvn_front_dispatch(front, 0x75005e2770ULL, regs).kind == NVN_FRONT_HANDLED);
    regs[1] = POOL_BUILDER;
    regs[2] = 0;
    regs[0] = DEVICE;
    assert(nvn_front_dispatch(front, 0x75005d6510ULL, regs).kind == NVN_FRONT_INVALID_GUEST);

    NvnFrontPoolHit hit;
    assert(nvn_front_resolve_pool(front, DEVICE, QUEUE_MEMORY + 0xffc, 4, &hit));
    assert(hit.object == POOL && hit.offset == 0xffc && hit.config.storage == QUEUE_MEMORY);
    assert(!nvn_front_resolve_pool(front, DEVICE, QUEUE_MEMORY + 0xffc, 5, &hit));
    assert(!nvn_front_resolve_pool(front, SECOND_DEVICE, QUEUE_MEMORY, 1, &hit));
    probe->unreadable_byte = QUEUE_MEMORY + 0xfff;
    assert(!nvn_front_resolve_pool(front, DEVICE, QUEUE_MEMORY + 0xffc, 4, &hit));
    probe->unreadable_byte = 0;

    invoke(front, 0x75005e2450ULL, POOL_BUILDER, 0x21, 0);
    assert(invoke(front, initialize, SECOND_POOL, POOL_BUILDER, 0) == 1);
    assert(invoke(front, map, SECOND_POOL, 0, 0) == 0);
    assert(invoke(front, 0x75005e2780ULL, SECOND_POOL, 0, 0) == QUEUE_MEMORY);
    assert(!nvn_front_resolve_pool(front, DEVICE, QUEUE_MEMORY, 1, &hit));
    uint64_t buffer_regs[32] = { BUFFER_BUILDER, SECOND_POOL, 0x100, 0x100 };
    assert(nvn_front_dispatch(front, 0x75005c34f0ULL, buffer_regs).kind == NVN_FRONT_HANDLED);
    assert(invoke(front, 0x75005c3540ULL, BUFFER, BUFFER_BUILDER, 0) == 1);
    assert(invoke(front, 0x75005c35d0ULL, BUFFER, 0, 0) == 0);
    assert(invoke(front, 0x75005c3600ULL, BUFFER, 0, 0) == QUEUE_MEMORY + 0x100);
    invoke(front, 0x75005c3590ULL, BUFFER, 0, 0);
    invoke(front, finalize_pool, SECOND_POOL, 0, 0);
    assert(nvn_front_resolve_pool(front, DEVICE, QUEUE_MEMORY, 1, &hit));
    invoke(front, finalize_pool, POOL, 0, 0);
    assert(!nvn_front_get_pool(front, POOL, &config));
    assert(!nvn_front_resolve_pool(front, DEVICE, QUEUE_MEMORY, 1, &hit));
    assert(storage[0] == 0x6b && storage[0xfff] == 0x6b);

    invoke(front, 0x75005e2450ULL, POOL_BUILDER, 0x0a, 0);
    assert(invoke(front, initialize, POOL, POOL_BUILDER, 0) == 1);
    assert(invoke(front, map, POOL, 0, 0) == QUEUE_MEMORY);
    assert(!nvn_front_resolve_pool(front, DEVICE, QUEUE_MEMORY, 1, &hit));
    invoke(front, finalize_pool, POOL, 0, 0);

    invoke(front, set_device, POOL_BUILDER, 0, 0);
    regs[0] = POOL;
    assert(nvn_front_dispatch(front, initialize, regs).kind == NVN_FRONT_INVALID_GUEST);
    regs[0] = BASE + sizeof probe->memory - 0x3f;
    assert(nvn_front_dispatch(front, set_device, regs).kind == NVN_FRONT_INVALID_GUEST);
    assert(!nvn_front_read_pool_builder(front, regs[0], &config));

    invoke(front, set_device, POOL_BUILDER, DEVICE, 0);
    invoke(front, 0x75005e2450ULL, POOL_BUILDER, 0x22, 0);
    assert(invoke(front, initialize, POOL, POOL_BUILDER, 0) == 1);
    assert(invoke(front, 0x75005ef720ULL, TEXTURE, TEXTURE_BUILDER, 0) == 1);
    assert(invoke(front, 0x75005c48e0ULL, SECOND_COMMAND_BUFFER, DEVICE, 0) == 1);
}

int main(void)
{
    static Probe probe;
    NvnFrontConfig config = {
        .guest_opaque = &probe, .backend_opaque = &probe, .read_guest = read_guest, .write_guest = guest,
        .device_create = create_device, .device_destroy = destroy_device, .queue_create = create_queue,
        .queue_destroy = destroy_queue, .get_integer = get_integer,
        .texture_create = create_texture, .texture_destroy = destroy_texture,
        .sync_create = create_sync, .sync_destroy = destroy_sync,
        .command_buffer_create = create_command_buffer, .command_buffer_destroy = destroy_command_buffer,
        .program_create = create_program, .program_destroy = destroy_program, .program_link = link_program,
    };
    NvnFront *front = nvn_front_create(&config);
    assert(front);
    uint64_t regs[32] = { NAME };
    const char get_proc_name[] = "nvnDeviceGetProcAddress";
    memcpy(guest(&probe, NAME, sizeof get_proc_name), get_proc_name, sizeof get_proc_name);
    uint64_t loader = invoke(front, NVN_FRONT_SDK_BOOTSTRAP, NAME, 0, 0);
    assert(invoke(front, NVN_FRONT_SDK_BOOTSTRAP_INTERNAL, NAME, 0, 0) == loader);
    assert(nvn_front_dispatch(front, 0x7500118e90ULL, regs).kind == NVN_FRONT_NOT_OURS);

    /* The guest loads unsupported entries too. Calling one identifies it. */
    uint64_t submit = get_proc(front, &probe, loader, 0, "nvnQueueSubmitCommands");
    NvnFrontReply result = nvn_front_dispatch(front, submit, regs);
    assert(result.kind == NVN_FRONT_BOUNDARY && !strcmp(result.name, "nvnQueueSubmitCommands"));
    memcpy(guest(&probe, NAME, 16), "nvnUnknownEntry", 16);
    result = nvn_front_dispatch(front, NVN_FRONT_SDK_BOOTSTRAP, regs);
    assert(result.kind == NVN_FRONT_BOUNDARY && !strcmp(result.name, "nvnUnknownEntry"));
    regs[0] = BASE + sizeof probe.memory;
    assert(nvn_front_dispatch(front, NVN_FRONT_SDK_BOOTSTRAP, regs).kind == NVN_FRONT_INVALID_GUEST);

    uint64_t device_init = get_proc(front, &probe, loader, 0, "nvnDeviceInitialize");
    uint64_t get_integer_proc = get_proc(front, &probe, loader, 0, "nvnDeviceGetInteger");
    uint32_t *output = guest(&probe, OUTPUT, 4);
    invoke(front, get_integer_proc, 0, 0, OUTPUT);
    assert(*output == 231);
    invoke(front, get_integer_proc, 0, 1, OUTPUT);
    assert(*output == 30);
    invoke(front, 0x75005d64f0ULL, DEVICE_BUILDER, 0, 0);
    invoke(front, 0x75005d6500ULL, DEVICE_BUILDER, 0x200, 0);
    assert(invoke(front, 0x75005d64e0ULL, DEVICE_BUILDER, 0, 0) == 0x200);

    uint8_t *device_data = guest(&probe, DEVICE, 0x3001);
    memset(device_data, 0xa5, 0x3001);
    probe.fail_device = true;
    assert(invoke(front, device_init, DEVICE, DEVICE_BUILDER, 0) == 0);
    assert(!probe.devices && probe.calls == 1 && device_data[0] == 0xa5);
    probe.fail_device = false;
    assert(invoke(front, device_init, DEVICE, DEVICE_BUILDER, 0) == 1);
    assert(probe.devices == 1 && device_data[0x2fff] == 0 && device_data[0x3000] == 0xa5);
    invoke(front, get_integer_proc, DEVICE, 0x2a, OUTPUT);
    assert(*output == 1 && probe.integer_queries == 1);
    invoke(front, get_integer_proc, DEVICE, 0x5a, OUTPUT);
    assert(*output == 0 && probe.integer_queries == 1);
    static const struct { uint32_t query, value; } device_layout[] = {
        {0x04, 0x100}, {0x0b, 4}, {0x0c, 4}, {0x0f, 0x20}, {0x10, 0x20},
        {0x11, 0x100}, {0x12, 0x100}, {0x24, 0x100000}, {0x25, 0x1000},
    };
    for (size_t i = 0; i < sizeof device_layout / sizeof device_layout[0]; ++i) {
        invoke(front, get_integer_proc, DEVICE, device_layout[i].query, OUTPUT);
        assert(*output == device_layout[i].value);
    }
    assert(probe.integer_queries == 1);
    regs[0] = DEVICE;
    regs[1] = 0xff;
    regs[2] = OUTPUT;
    result = nvn_front_dispatch(front, get_integer_proc, regs);
    assert(result.kind == NVN_FRONT_BOUNDARY && !strcmp(result.name, "nvnDeviceGetInteger"));
    test_device_modes(front, &probe, loader, device_init);
    test_sync_lifecycle(front, &probe, loader);
    test_memory_pool(front, &probe, loader);
    test_texture_view(front, &probe, loader);
    test_vertex_state(front, &probe, loader);
    test_raster_state(front, &probe, loader);

    uint64_t queue_init = get_proc(front, &probe, loader, DEVICE, "nvnQueueInitialize");
    invoke(front, 0x75005e82d0ULL, QUEUE_BUILDER, DEVICE, 0);
    invoke(front, 0x75005e82e0ULL, QUEUE_BUILDER, 0, 0);
    invoke(front, 0x75005e8310ULL, QUEUE_BUILDER, 4, 0);
    invoke(front, 0x75005e8320ULL, QUEUE_BUILDER, 0x10000, 0);
    invoke(front, 0x75005e8330ULL, QUEUE_BUILDER, 0, 0);
    invoke(front, 0x75005e8340ULL, QUEUE_BUILDER, 0x10000, 0);
    invoke(front, 0x75005e8360ULL, QUEUE_BUILDER, 0x8000, 0);
    invoke(front, 0x75005e8370ULL, QUEUE_BUILDER, QUEUE_MEMORY, 0x1000);
    NvnFrontQueueInfo queue_info;
    assert(!nvn_front_get_queue(front, QUEUE, &queue_info));
    assert(invoke(front, queue_init, QUEUE, QUEUE_BUILDER, 0) == 1);
    assert(nvn_front_get_queue(front, QUEUE, &queue_info));
    assert(queue_info.device == DEVICE && queue_info.native == &probe.queue[0]);
    invoke(front, 0x75005e8700ULL, QUEUE_BUILDER, QUEUE, 0);
    assert(invoke(front, queue_init, SECOND_QUEUE, QUEUE_BUILDER, 0) == 1);
    assert(probe.queues == 2);
    assert(nvn_front_get_queue(front, SECOND_QUEUE, &queue_info));
    assert(queue_info.device == DEVICE && queue_info.native == &probe.queue[1]);
    invoke(front, 0x75005e9ad0ULL, SECOND_QUEUE, 0, 0);
    assert(!nvn_front_get_queue(front, SECOND_QUEUE, &queue_info));
    nvn_front_destroy(front);
    assert(!probe.devices && !probe.queues && !probe.textures && probe.texture_destroys == 5);
    assert(!probe.syncs && probe.sync_destroys == 3);
    assert(!probe.commands && probe.command_destroys == 3);

    config.device_create = NULL;
    config.device_destroy = NULL;
    front = nvn_front_create(&config);
    assert(front);
    regs[0] = DEVICE;
    regs[1] = DEVICE_BUILDER;
    result = nvn_front_dispatch(front, device_init, regs);
    assert(result.kind == NVN_FRONT_BOUNDARY && regs[0] == DEVICE);
    nvn_front_destroy(front);
    puts("PASS Pokopia NVN2 front: device, queue, pool, buffer, texture, sync, command-buffer and shader-program setup");
    return 0;
}
