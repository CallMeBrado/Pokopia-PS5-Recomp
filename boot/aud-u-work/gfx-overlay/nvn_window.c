/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nvn_window.h"

#include <stdlib.h>
#include <string.h>

/* Verified against NVN2 SDK+0x5f11d0 through +0x5f1660. */
enum {
    BUILDER_DEVICE = 0x08, BUILDER_COUNT = 0x10, BUILDER_ACTIVE = 0x14,
    BUILDER_NATIVE = 0x18, BUILDER_INTERVAL = 0x20, BUILDER_TEXTURES = 0x28,
    BUILDER_HDR = 0x30, BUILDER_SIZE = 0x60,
    WINDOW_DEVICE = 0x00, WINDOW_NATIVE = 0x08, WINDOW_CROP = 0x10,
    WINDOW_COUNT = 0x20, WINDOW_ACTIVE = 0x24, WINDOW_INTERVAL = 0x28,
    WINDOW_HDR = 0x38, WINDOW_TEXTURES = 0x68, WINDOW_SIZE = 0x218,
    TEXTURE_2D = 1, TEXTURE_RGBA8_SRGB = 0x38, TEXTURE_DISPLAY = 1,
    TEXTURE_IMAGE = 4, TEXTURE_COMPRESSIBLE = 8, MAX_TEXTURES = 4,
};

/* The twelve floats copied by nvnWindowBuilderSetDefaults. */
static const uint32_t default_hdr[] = {
    0x3f23d70a, 0x3ea8f5c3, 0x3e99999a, 0x3f19999a,
    0x3e19999a, 0x3d75c28f, 0x3ea01a37, 0x3ea872b0,
    0x3cf5c28f, 0x447a0000, 0x43480000, 0x42700000,
};

_Static_assert(sizeof(NvnWindowCrop) == 16, "NVNrectangle layout");

typedef struct {
    uintptr_t native;
    uint64_t start, end;
} TextureIdentity;

typedef struct Window {
    struct Window *next;
    uint64_t address;
    NvnWindowInfo info;
    TextureIdentity backing[MAX_TEXTURES];
    uint32_t next_acquire, acquired;
} Window;

struct NvnWindowBridge {
    NvnWindowConfig config;
    Window *windows;
};

static uint32_t load32(const uint8_t *address)
{
    uint32_t value;
    memcpy(&value, address, sizeof value);
    return value;
}

static uint64_t load64(const uint8_t *address)
{
    uint64_t value;
    memcpy(&value, address, sizeof value);
    return value;
}

static void store32(uint8_t *address, uint32_t value)
{
    memcpy(address, &value, sizeof value);
}

static void store64(uint8_t *address, uint64_t value)
{
    memcpy(address, &value, sizeof value);
}

NvnWindowBridge *nvn_window_create(const NvnWindowConfig *config)
{
    if (!config || !config->read_guest || !config->write_guest ||
        !config->get_output || !config->get_texture)
        return NULL;
    NvnWindowBridge *bridge = calloc(1, sizeof *bridge);
    if (bridge)
        bridge->config = *config;
    return bridge;
}

static Window **find_window(Window **link, uint64_t address)
{
    while (*link && (*link)->address != address)
        link = &(*link)->next;
    return link;
}

static bool get_backing(const NvnWindowBridge *bridge, const NvnWindowInfo *window,
    uint64_t address, TextureIdentity *identity)
{
    NvnFrontTextureInfo info;
    if (!address || !bridge->config.get_texture(bridge->config.opaque, address, &info) || !info.native)
        return false;

    const NvnFrontTextureCreateInfo *backing = &info.create_info;
    const NvnFrontTextureConfig *texture = &backing->config;
    const uint32_t allowed = TEXTURE_DISPLAY | TEXTURE_IMAGE | TEXTURE_COMPRESSIBLE;
    if (texture->device != window->device || !(texture->flags & TEXTURE_DISPLAY) ||
        (texture->flags & ~allowed) || texture->target != TEXTURE_2D ||
        texture->width != window->width || texture->height != window->height ||
        texture->depth != 1 || texture->levels != 1 || texture->samples > 1 ||
        texture->format != window->format || texture->stride || texture->packaged_data ||
        texture->packaged_layout || texture->gl_texture_name ||
        !backing->storage_address || !backing->storage_size ||
        backing->storage_address > UINT64_MAX - backing->storage_size)
        return false;

    *identity = (TextureIdentity){.native = (uintptr_t)info.native, .start = backing->storage_address,
        .end = backing->storage_address + backing->storage_size};
    return true;
}

static NvnFrontReplyKind initialize(NvnWindowBridge *bridge, uint64_t x[32])
{
    const NvnWindowConfig *config = &bridge->config;
    Window **link = find_window(&bridge->windows, x[0]);
    if (!x[0] || (x[0] & 7) || !x[1] || (x[1] & 7) || *link)
        return NVN_FRONT_INVALID_GUEST;
    uint8_t *object = config->write_guest(config->opaque, x[0], WINDOW_SIZE);
    const uint8_t *builder = config->read_guest(config->opaque, x[1], BUILDER_SIZE);
    if (!object || !builder)
        return NVN_FRONT_INVALID_GUEST;

    NvnWindowInfo info = {
        .device = load64(builder + BUILDER_DEVICE), .native_window = load64(builder + BUILDER_NATIVE),
        .texture_count = load32(builder + BUILDER_COUNT), .active_texture_count = load32(builder + BUILDER_ACTIVE),
        .present_interval = load32(builder + BUILDER_INTERVAL) + 1u, .format = TEXTURE_RGBA8_SRGB,
    };
    if (!info.device || !info.texture_count || info.texture_count > MAX_TEXTURES ||
        info.active_texture_count > info.texture_count)
        return NVN_FRONT_INVALID_GUEST;
    if (!info.active_texture_count)
        info.active_texture_count = info.texture_count;

    ViAdapterOutput output;
    if (info.present_interval != 1 || !info.native_window ||
        !config->get_output(config->opaque, info.native_window, &output) || !output.width || !output.height ||
        output.width > INT32_MAX || output.height > INT32_MAX)
        return NVN_FRONT_BOUNDARY;
    info.width = output.width;
    info.height = output.height;
    info.crop = (NvnWindowCrop){.width = (int32_t)info.width, .height = (int32_t)info.height};

    const uint64_t array_address = load64(builder + BUILDER_TEXTURES);
    const size_t array_size = info.texture_count * sizeof info.textures[0];
    if (!array_address || (array_address & 7))
        return NVN_FRONT_INVALID_GUEST;
    const void *array = config->read_guest(config->opaque, array_address, array_size);
    if (!array)
        return NVN_FRONT_INVALID_GUEST;
    memcpy(info.textures, array, array_size);

    TextureIdentity backing[MAX_TEXTURES];
    for (uint32_t i = 0; i < info.texture_count; ++i) {
        if (!get_backing(bridge, &info, info.textures[i], &backing[i]))
            return NVN_FRONT_BOUNDARY;
        for (uint32_t j = 0; j < i; ++j) {
            if (info.textures[i] == info.textures[j] || backing[i].native == backing[j].native ||
                (backing[i].start < backing[j].end && backing[j].start < backing[i].end))
                return NVN_FRONT_INVALID_GUEST;
        }
    }

    Window *window = calloc(1, sizeof *window);
    if (!window) {
        x[0] = 0;
        return NVN_FRONT_HANDLED;
    }
    window->address = x[0];
    window->info = info;
    memcpy(window->backing, backing, info.texture_count * sizeof backing[0]);

    uint8_t fields[WINDOW_SIZE] = {0};
    store64(fields + WINDOW_DEVICE, info.device);
    store64(fields + WINDOW_NATIVE, info.native_window);
    memcpy(fields + WINDOW_CROP, &info.crop, sizeof info.crop);
    store32(fields + WINDOW_COUNT, info.texture_count);
    store32(fields + WINDOW_ACTIVE, info.active_texture_count);
    store32(fields + WINDOW_INTERVAL, info.present_interval);
    memcpy(fields + WINDOW_HDR, builder + BUILDER_HDR, sizeof default_hdr);
    memcpy(fields + WINDOW_TEXTURES, info.textures, array_size);
    memcpy(object, fields, sizeof fields);
    *link = window;
    x[0] = 1;
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind finalize(NvnWindowBridge *bridge, uint64_t address)
{
    Window **link = find_window(&bridge->windows, address);
    if (!*link)
        return NVN_FRONT_INVALID_GUEST;
    void *object = bridge->config.write_guest(bridge->config.opaque, address, WINDOW_SIZE);
    if (!object)
        return NVN_FRONT_INVALID_GUEST;
    Window *window = *link;
    *link = window->next;
    free(window);
    memset(object, 0, WINDOW_SIZE);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind crop_window(NvnWindowBridge *bridge, bool set, const uint64_t x[32])
{
    Window *window = *find_window(&bridge->windows, x[0]);
    if (!window)
        return NVN_FRONT_INVALID_GUEST;
    NvnWindowInfo *info = &window->info;
    const NvnWindowConfig *config = &bridge->config;
    if (!set) {
        void *out = config->write_guest(config->opaque, x[1], sizeof info->crop);
        if (!x[1] || !out)
            return NVN_FRONT_INVALID_GUEST;
        memcpy(out, &info->crop, sizeof info->crop);
        return NVN_FRONT_HANDLED;
    }

    NvnWindowCrop crop = {(int32_t)x[1], (int32_t)x[2], (int32_t)x[3], (int32_t)x[4]};
    if (!crop.width || !crop.height)
        crop = (NvnWindowCrop){.width = (int32_t)info->width, .height = (int32_t)info->height};
    if (crop.x < 0 || crop.y < 0 || crop.width < 0 || crop.height < 0 ||
        (uint64_t)crop.x + (uint64_t)crop.width > info->width ||
        (uint64_t)crop.y + (uint64_t)crop.height > info->height)
        return NVN_FRONT_INVALID_GUEST;

    uint8_t *object = config->write_guest(config->opaque, x[0], WINDOW_SIZE);
    if (!object)
        return NVN_FRONT_INVALID_GUEST;
    memcpy(object + WINDOW_CROP, &crop, sizeof crop);
    info->crop = crop;
    return NVN_FRONT_HANDLED;
}

NvnFrontReplyKind nvn_window_dispatch(NvnWindowBridge *bridge, const char *name, uint64_t x[32])
{
    if (!bridge || !name || !x)
        return NVN_FRONT_NOT_OURS;
    if (strcmp(name, "nvnWindowInitialize") == 0)
        return initialize(bridge, x);
    if (strcmp(name, "nvnWindowFinalize") == 0)
        return finalize(bridge, x[0]);
    if (strcmp(name, "nvnWindowSetCrop") == 0)
        return crop_window(bridge, true, x);
    if (strcmp(name, "nvnWindowGetCrop") == 0)
        return crop_window(bridge, false, x);
    if (strcmp(name, "nvnWindowGetPresentInterval") == 0) {
        Window *window = *find_window(&bridge->windows, x[0]);
        if (!window)
            return NVN_FRONT_INVALID_GUEST;
        x[0] = window->info.present_interval;
        return NVN_FRONT_HANDLED;
    }

    enum { SET_DEVICE, SET_DEFAULTS, SET_NATIVE, SET_TEXTURES, SET_INTERVAL, SET_ACTIVE, BUILDER_CALLS };
    static const char *const builder_calls[BUILDER_CALLS] = {
        [SET_DEVICE] = "nvnWindowBuilderSetDevice", [SET_DEFAULTS] = "nvnWindowBuilderSetDefaults",
        [SET_NATIVE] = "nvnWindowBuilderSetNativeWindow", [SET_TEXTURES] = "nvnWindowBuilderSetTextures",
        [SET_INTERVAL] = "nvnWindowBuilderSetPresentInterval", [SET_ACTIVE] = "nvnWindowBuilderSetNumActiveTextures",
    };
    size_t call = 0;
    while (call < BUILDER_CALLS && strcmp(name, builder_calls[call]) != 0)
        ++call;
    if (call == BUILDER_CALLS)
        return NVN_FRONT_NOT_OURS;
    if (!x[0] || (x[0] & 7))
        return NVN_FRONT_INVALID_GUEST;
    uint8_t *builder = bridge->config.write_guest(bridge->config.opaque, x[0], BUILDER_SIZE);
    if (!builder)
        return NVN_FRONT_INVALID_GUEST;

    switch (call) {
    case SET_DEVICE: store64(builder + BUILDER_DEVICE, x[1]); break;
    case SET_DEFAULTS:
        memset(builder + BUILDER_COUNT, 0, BUILDER_HDR - BUILDER_COUNT);
        memcpy(builder + BUILDER_HDR, default_hdr, sizeof default_hdr);
        break;
    case SET_NATIVE: store64(builder + BUILDER_NATIVE, x[1]); break;
    case SET_TEXTURES:
        store32(builder + BUILDER_COUNT, (uint32_t)x[1]);
        store64(builder + BUILDER_TEXTURES, x[2]);
        break;
    case SET_INTERVAL: store32(builder + BUILDER_INTERVAL, (uint32_t)x[1] - 1u); break;
    case SET_ACTIVE: store32(builder + BUILDER_ACTIVE, (uint32_t)x[1]); break;
    }
    return NVN_FRONT_HANDLED;
}

bool nvn_window_get(const NvnWindowBridge *bridge, uint64_t address, NvnWindowInfo *out)
{
    if (!bridge || !out)
        return false;
    const Window *window = bridge->windows;
    while (window && window->address != address)
        window = window->next;
    if (!window)
        return false;

    const NvnWindowInfo *info = &window->info;
    ViAdapterOutput output;
    if (!bridge->config.get_output(bridge->config.opaque, info->native_window, &output) ||
        output.width != info->width || output.height != info->height)
        return false;
    for (uint32_t i = 0; i < info->texture_count; ++i) {
        TextureIdentity backing;
        const TextureIdentity *registered = &window->backing[i];
        if (!get_backing(bridge, info, info->textures[i], &backing) || backing.native != registered->native ||
            backing.start != registered->start || backing.end != registered->end)
            return false;
    }
    *out = *info;
    return true;
}

bool nvn_window_acquire(NvnWindowBridge *bridge, uint64_t address, uint32_t *index)
{
    NvnWindowInfo info;
    if (!index || !nvn_window_get(bridge, address, &info))
        return false;
    Window *window = *find_window(&bridge->windows, address);
    for (uint32_t i = 0; i < info.active_texture_count; ++i) {
        uint32_t candidate = (window->next_acquire + i) % info.active_texture_count;
        uint32_t bit = UINT32_C(1) << candidate;
        if (window->acquired & bit)
            continue;
        window->acquired |= bit;
        window->next_acquire = (candidate + 1) % info.active_texture_count;
        *index = candidate;
        return true;
    }
    return false;
}

bool nvn_window_get_acquired(NvnWindowBridge *bridge, uint64_t address, uint32_t index, NvnWindowInfo *out)
{
    NvnWindowInfo info;
    if (!out || !nvn_window_get(bridge, address, &info) || index >= info.texture_count)
        return false;
    Window *window = *find_window(&bridge->windows, address);
    if (!(window->acquired & (UINT32_C(1) << index)))
        return false;
    *out = info;
    return true;
}

void nvn_window_release(NvnWindowBridge *bridge, uint64_t address, uint32_t index)
{
    if (!bridge || index >= MAX_TEXTURES)
        return;
    Window *window = *find_window(&bridge->windows, address);
    if (window)
        window->acquired &= ~(UINT32_C(1) << index);
}

void nvn_window_destroy(NvnWindowBridge *bridge)
{
    if (!bridge)
        return;
    while (bridge->windows) {
        Window *window = bridge->windows;
        bridge->windows = window->next;
        free(window);
    }
    free(bridge);
}
