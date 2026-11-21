/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "transfer_memory.h"

#include <stddef.h>

enum {
    PAGE_MASK = 0xfff,
    INVALID_SIZE = 0xca01,
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_CURRENT_MEMORY = 0xd401,
    INVALID_NEW_MEMORY_PERMISSION = 0xd801,
    INVALID_HANDLE = 0xe401,
};

uint32_t hzn_transfer_memory_create(HznTransferMemoryTable *table, uint64_t address,
    uint64_t size, uint32_t permission, bool (*owner_range)(uint64_t, uint64_t), uint32_t *handle)
{
    *handle = 0;
    if (address & PAGE_MASK)
        return INVALID_ADDRESS;
    if (!size || (size & PAGE_MASK))
        return INVALID_SIZE;
    if (address > UINT64_MAX - size)
        return INVALID_CURRENT_MEMORY;
    if (permission != 0 && permission != 1 && permission != 3)
        return INVALID_NEW_MEMORY_PERMISSION;
    if (!owner_range(address, size))
        return INVALID_CURRENT_MEMORY;

    HznTransferMemory *available = NULL;
    for (size_t i = 0; i < HZN_TRANSFER_MEMORY_LIMIT; ++i) {
        HznTransferMemory *object = &table->objects[i];
        if (!object->handle) {
            if (!available)
                available = object;
        } else if (address < object->address + object->size && object->address < address + size) {
            return INVALID_CURRENT_MEMORY;
        }
    }

    /* A transfer handle records the original guest backing; creating one
     * does not allocate a second range or materialize sparse heap pages. */
    const uint32_t next = table->next_handle ? table->next_handle : HZN_TRANSFER_MEMORY_HANDLE_BASE;
    if (!available || next >= HZN_TRANSFER_MEMORY_HANDLE_END)
        return OUT_OF_RESOURCE;
    *available = (HznTransferMemory){address, size, permission, next};
    table->next_handle = next + 1;
    *handle = next;
    return 0;
}

const HznTransferMemory *hzn_transfer_memory_lookup(const HznTransferMemoryTable *table,
    uint32_t handle)
{
    if (handle < HZN_TRANSFER_MEMORY_HANDLE_BASE || handle >= HZN_TRANSFER_MEMORY_HANDLE_END)
        return NULL;
    for (size_t i = 0; i < HZN_TRANSFER_MEMORY_LIMIT; ++i)
        if (table->objects[i].handle == handle)
            return &table->objects[i];
    return NULL;
}

bool hzn_transfer_memory_close(HznTransferMemoryTable *table, uint32_t handle, uint32_t *result)
{
    if (handle < HZN_TRANSFER_MEMORY_HANDLE_BASE || handle >= HZN_TRANSFER_MEMORY_HANDLE_END)
        return false;
    for (size_t i = 0; i < HZN_TRANSFER_MEMORY_LIMIT; ++i) {
        if (table->objects[i].handle == handle) {
            table->objects[i] = (HznTransferMemory){0};
            *result = 0;
            return true;
        }
    }
    *result = INVALID_HANDLE;
    return true;
}
