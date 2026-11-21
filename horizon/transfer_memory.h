/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_TRANSFER_MEMORY_H
#define POKOPIA_HORIZON_TRANSFER_MEMORY_H

#include <stdbool.h>
#include <stdint.h>

enum {
    HZN_TRANSFER_MEMORY_HANDLE_BASE = 0x50000,
    HZN_TRANSFER_MEMORY_HANDLE_END = 0x60000,
    HZN_TRANSFER_MEMORY_LIMIT = 128,
};

typedef struct {
    uint64_t address, size;
    uint32_t permission, handle;
} HznTransferMemory;

/* Use zero-initialized storage. The caller holds the kernel lock while
 * creating, looking up or closing a handle. */
typedef struct {
    HznTransferMemory objects[HZN_TRANSFER_MEMORY_LIMIT];
    uint32_t next_handle;
} HznTransferMemoryTable;

uint32_t hzn_transfer_memory_create(HznTransferMemoryTable *table, uint64_t address,
    uint64_t size, uint32_t permission, bool (*owner_range)(uint64_t, uint64_t), uint32_t *handle);

const HznTransferMemory *hzn_transfer_memory_lookup(const HznTransferMemoryTable *table,
    uint32_t handle);

/* Return false for another kernel object's handle namespace. */
bool hzn_transfer_memory_close(HznTransferMemoryTable *table, uint32_t handle, uint32_t *result);

#endif
