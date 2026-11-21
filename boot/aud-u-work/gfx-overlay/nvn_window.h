/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_NVN_WINDOW_H
#define POKOPIA_NVN_WINDOW_H

#include "nvn_front.h"
#include "vi_adapter.h"

typedef struct NvnWindowBridge NvnWindowBridge;

typedef struct {
    void *opaque;
    const void *(*read_guest)(void *opaque, uint64_t address, size_t size);
    void *(*write_guest)(void *opaque, uint64_t address, size_t size);
    bool (*get_output)(void *opaque, uint64_t native_window, ViAdapterOutput *output);
    bool (*get_texture)(void *opaque, uint64_t address, NvnFrontTextureInfo *texture);
} NvnWindowConfig;

typedef struct {
    int32_t x, y, width, height;
} NvnWindowCrop;

typedef struct {
    uint64_t device, native_window, textures[4];
    uint32_t texture_count, active_texture_count, present_interval, width, height, format;
    NvnWindowCrop crop;
} NvnWindowInfo;

/* Serialize calls with VI and texture lifetime changes. The bridge borrows
 * the display and textures and leaves their ownership with their adapters. */
NvnWindowBridge *nvn_window_create(const NvnWindowConfig *config);
/* Call with the exact procedure name from an NVN frontend boundary. */
NvnFrontReplyKind nvn_window_dispatch(NvnWindowBridge *bridge, const char *name, uint64_t x[32]);
/* Revalidates the display and texture backing before returning guest indices. */
bool nvn_window_get(const NvnWindowBridge *bridge, uint64_t address, NvnWindowInfo *out);
/* Track acquisition of guest images independently of the host swapchain. */
bool nvn_window_acquire(NvnWindowBridge *bridge, uint64_t address, uint32_t *index);
bool nvn_window_get_acquired(NvnWindowBridge *bridge, uint64_t address, uint32_t index, NvnWindowInfo *out);
void nvn_window_release(NvnWindowBridge *bridge, uint64_t address, uint32_t index);
void nvn_window_destroy(NvnWindowBridge *bridge);

#endif
