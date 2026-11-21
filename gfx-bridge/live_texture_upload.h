/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_LIVE_TEXTURE_UPLOAD_H
#define POKOPIA_LIVE_TEXTURE_UPLOAD_H

#include "texture_upload.h"
#include "vk_adapter.h"

typedef struct {
    NvnFront *front;
    PokopiaVkAdapter *adapter;
    void *guest_opaque;
    const void *(*read_guest)(void *opaque, uint64_t address, size_t size);
    void *(*write_guest)(void *opaque, uint64_t address, size_t size);
    FILE *log;

    NvnTextureUpload *staging;
    size_t capacity;
    unsigned logged;
} PokopiaLiveTextureUpload;

/* Handles tight CPU texel reads/writes, flushes and invalidation for single-level
 * 2D images and arrays, depth-one volumes, and cubemaps and cube arrays
 * with supported color formats.
 * The caller serializes dispatch with guest memory, NVN objects and queue use.
 * A handled write commits the guest pool and updates existing sampled backing. */
NvnFrontReplyKind pokopia_live_texture_upload_dispatch(PokopiaLiveTextureUpload *upload,
                                                       const char *name, const uint64_t x[32]);

/* Initialize deferred backing or refresh a CPU-coherent image at queue execution. */
VkResult pokopia_live_texture_prepare_sample(PokopiaLiveTextureUpload *upload, const NvnFrontTextureInfo *info);

/* Retire and release staging before closing NVN textures and their VkDevice. */
VkResult pokopia_live_texture_upload_close(PokopiaLiveTextureUpload *upload);

#endif
