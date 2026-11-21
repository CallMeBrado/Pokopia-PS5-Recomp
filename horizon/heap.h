/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_HEAP_H
#define POKOPIA_HORIZON_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HZN_HEAP_GRANULE = 0x200000,
    HZN_HEAP_OK = 0,
    HZN_HEAP_INVALID_SIZE = 0xca01,
    HZN_HEAP_OUT_OF_MEMORY = 0xd001,
    HZN_HEAP_INVALID_MEMORY_STATE = 0xd401
};

typedef struct {
    uint64_t base, budget, size;
    int32_t last_host_error;

    /* Private backing ownership. A failed cleanup retains these for retry. */
    void *mapping;
    size_t backing_size;
    int64_t direct_offset;
} HznHeap;

/* Configure fresh storage. The caller serializes operations and stops all
 * heap users before release. Budget is physical heap space after reserving
 * the host and graphics share. Initialization allocates no host memory. */
bool hzn_heap_init(HznHeap *heap, uint64_t base, uint64_t budget);

/* Horizon SetHeapSize result. The first nonzero size commits at base. Once
 * committed, repeating the size succeeds and any resize is rejected without
 * touching the mapping. out_base is zero on error. */
uint32_t hzn_heap_set_size(HznHeap *heap, uint64_t size, uint64_t *out_base);

/* Drop owned backing, retaining the configuration for reuse. A false result
 * leaves ownership recorded so teardown can be retried. */
bool hzn_heap_release(HznHeap *heap);

#ifdef __cplusplus
}
#endif
#endif
