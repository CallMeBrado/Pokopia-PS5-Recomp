/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_LIVE_SHARED_H
#define POKOPIA_HORIZON_LIVE_SHARED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t address;
    size_t size;
    void *writer;
    int32_t last_host_error;
    bool unsafe;

    /* Ownership of the two native views and their common backing. */
    void *guest_view, *writer_view;
    size_t mapped_bytes;
    uint64_t backing;
} HznLiveShared;

/* Start with a zero-initialized object. Exclude guest access and host writers during map
 * and unmap. Publish the returned address and writer only after success. */
bool hzn_live_shared_map(HznLiveShared *shared, uint64_t address, size_t size);

/* Release only owned mappings. After a cleanup error, retain ownership for
 * retry and stop guest access while unsafe is set. */
bool hzn_live_shared_unmap(HznLiveShared *shared);

#endif
