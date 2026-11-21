/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_GFX_BRIDGE_ROMFS_H
#define POKOPIA_GFX_BRIDGE_ROMFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const char *path;
    uint64_t file_offset;
    size_t bytes;
} HznRomfsPayload;

uint64_t hzn_romfs_size(void);
bool hzn_romfs_verify(const char *root);
bool hzn_romfs_verify_logged(const char *root, FILE *log);
bool hzn_romfs_read(const char *root, uint64_t offset, void *destination, size_t bytes);

/* On success, describe the first loose-file span copied by the read, if any.
 * Header, metadata and padding do not produce a payload record. */
bool hzn_romfs_read_payload(const char *root, uint64_t offset, void *destination, size_t bytes,
                            HznRomfsPayload *first);

#endif
