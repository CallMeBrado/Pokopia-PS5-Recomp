/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_HEAP_STACK_ALIAS_H
#define POKOPIA_HORIZON_HEAP_STACK_ALIAS_H

#include "stack_map.h"
#include "stack_backing.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HZN_HEAP_STACK_ALIAS_LIMIT = 128,
    HZN_HEAP_STACK_ALIAS_ISOLATED_ALIGNMENT = 0x4000,
#ifdef _WIN32
    HZN_HEAP_STACK_ALIAS_SOURCE_ALIGNMENT = 0x1000
#else
    HZN_HEAP_STACK_ALIAS_SOURCE_ALIGNMENT = 0x4000
#endif
};

typedef struct {
    uint64_t source, destination, size;
} HznHeapStackAliasRange;

typedef struct {
    HznHeapStackAliasRange range;
    HznStackBacking backing;
    size_t reservation_size, source_protection_size;
    bool active, source_protected;
} HznHeapStackAliasSlot;

typedef struct {
    size_t count;
    int32_t last_host_error;
    bool unsafe;

    /* Private ownership. Only the destination reservations are owned. */
    HznHeapStackAliasSlot slots[HZN_HEAP_STACK_ALIAS_LIMIT];
} HznHeapStackAliases;

/* Start with zeroed storage. The caller validates a dedicated, committed
 * read/write heap source and a free destination in guest stack space, and
 * serializes access against stack users and heap teardown. Map copies the
 * range to an exact host destination and makes the heap source inaccessible.
 * Orbis requires source and size to align to its 16 KiB host pages. */
uint32_t hzn_heap_stack_alias_map(HznHeapStackAliases *aliases, uint64_t dst, uint64_t src, uint64_t size);

/* The source owns its complete 16 KiB pages, including any padding after the
 * guest's 4 KiB-aligned span. Only the guest span is copied and exposed. */
uint32_t hzn_heap_stack_alias_map_isolated(HznHeapStackAliases *aliases,
                                         uint64_t dst, uint64_t src, uint64_t size);

/* Require the exact mapped tuple, restore source access, and copy back.
 * An unsafe result means the caller must stop guest execution. */
uint32_t hzn_heap_stack_alias_unmap(HznHeapStackAliases *aliases, uint64_t dst, uint64_t src, uint64_t size);

/* Enumerate indices below count while serialized. The pointer is borrowed
 * until the next mutation. The range is the guest span without host padding. */
const HznHeapStackAliasRange *hzn_heap_stack_alias_at(const HznHeapStackAliases *aliases, size_t index);

/* Restore and release every alias after its users have stopped. On failure
 * keep ownership for a retry. The caller still owns the source heap. */
bool hzn_heap_stack_alias_release(HznHeapStackAliases *aliases);

#ifdef __cplusplus
}
#endif
#endif
