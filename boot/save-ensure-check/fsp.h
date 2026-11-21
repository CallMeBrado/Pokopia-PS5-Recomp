/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_GFX_BRIDGE_FSP_H
#define POKOPIA_GFX_BRIDGE_FSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HznFsp HznFsp;
typedef void *(*HznFspWriteGuest)(void *opaque, uint64_t address, size_t size);
typedef uint32_t (*HznFspClone)(void *opaque, HznFsp *same_domain);

struct HznFsp {
    const char *romfs_root;
    const char *save_root;
    HznFspWriteGuest write_guest;
    HznFspClone clone;
    void *opaque;
    FILE *read_log;
    uint32_t storage_objects[16];
    uint32_t save_objects[8];
    uint32_t next_object_id;
    bool domain, root_open, process_set, romfs_verified, first_asset_read;
};

typedef enum { HZN_FSP_REPLY, HZN_FSP_BOUNDARY } HznFspReplyKind;

typedef struct {
    HznFspReplyKind kind;
    uint32_t result, command, object_id;
    const char *object_type;
} HznFspReply;

/* Keep the root string and state alive for the original and all clone handles.
 * Dispatch is serialized. write_guest validates the complete writable span. */
void hzn_fsp_init(HznFsp *fsp, const char *romfs_root, HznFspWriteGuest write_guest,
                  HznFspClone clone, void *opaque);
/* Optional borrowed sink for mount verification failures and the first
 * successful payload read. Successful mounting and synthetic metadata
 * reads do not emit payload diagnostics. */
void hzn_fsp_set_read_log(HznFsp *fsp, FILE *log);
void hzn_fsp_set_save_root(HznFsp *fsp, const char *save_root);
HznFspReply hzn_fsp_send_sync(HznFsp *fsp, void *tls, size_t tls_size);

#ifdef __cplusplus
}
#endif
#endif
