/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_STACK_SOURCE_H
#define POKOPIA_HORIZON_STACK_SOURCE_H

#include "heap_stack_alias.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HZN_STACK_SOURCE_ALIGNMENT = HZN_HEAP_STACK_ALIAS_ISOLATED_ALIGNMENT,
    HZN_STACK_SOURCE_SLOT_ALIGNMENT = 0x10000,
    HZN_STACK_SOURCE_LIMIT = HZN_HEAP_STACK_ALIAS_LIMIT,
};

typedef struct {
    uint64_t thread_object, original, address, size;
} HznStackSourceRange;

typedef struct {
    HznStackSourceRange range;
    HznStackBacking backing;
} HznStackSourceSlot;

typedef struct {
    size_t count;
    int32_t last_host_error;
    bool unsafe;

    /* Private arena geometry and ownership. Only occupied slots are mapped. */
    uint64_t base, slot_size;
    HznStackSourceSlot slots[HZN_STACK_SOURCE_LIMIT];
} HznStackSources;

/* Configure an empty isolated guest address range. Base and slot size must
 * be 64 KiB aligned. No host reservation is made until prepare. */
bool hzn_stack_sources_init(HznStackSources *sources, uint64_t base, uint64_t slot_size);

/* Copy a caller-owned stack into an exact host-page-aligned source. The
 * logical stack is 4 KiB aligned and the private reservation rounds up to
 * whole host pages. Validate a writable original and keep it live until
 * release. Serialize all operations against guest stack users and teardown.
 * Pass the returned address to the SDK before it records the stack source.
 * On failure, the returned address is zero and original data is unchanged. */
uint32_t hzn_stack_source_prepare(HznStackSources *sources, uint64_t thread_object,
                                 uint64_t original, uint64_t size, uint64_t *address);

/* Iterate slots in [0, HZN_STACK_SOURCE_LIMIT). Empty slots return NULL.
 * Ranges are borrowed until the next mutation. Use heap alias state to
 * determine if an owned source is currently inaccessible. */
const HznStackSourceRange *hzn_stack_source_at(const HznStackSources *sources, size_t slot);

/* After SDK DestroyThread has stopped the worker and unmapped its alias,
 * copy back the source and free it. Also safe after a failed CreateThread.
 * An active alias prevents release, including across speculative map/unmap
 * retries. Releasing an unrecorded thread is a successful no-op. */
uint32_t hzn_stack_source_release(HznStackSources *sources, const HznHeapStackAliases *aliases,
                                 uint64_t thread_object);

/* Process teardown follows alias teardown, before the original heap is
 * released. Failure retains ownership for retry. */
bool hzn_stack_sources_release(HznStackSources *sources, const HznHeapStackAliases *aliases);

#ifdef __cplusplus
}
#endif
#endif
