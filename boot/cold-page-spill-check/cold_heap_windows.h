/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HZN_COLD_HEAP_WINDOWS_H
#define HZN_COLD_HEAP_WINDOWS_H

#include "../../horizon/heap.h"

/* Isolated Windows reference for the cold chunk lifecycle. */
bool hzn_cold_heap_reserve(HznHeap *heap, size_t bytes, const char *file);
bool hzn_cold_heap_release(HznHeap *heap);
bool hzn_cold_heap_spill(void *address);
bool hzn_cold_heap_prepare_io(void *buffer, size_t bytes);

/* Call before the sparse heap is reserved. */
bool hzn_heap_sparse_cold_open(const char *path);
bool hzn_heap_sparse_cold_contains(const void *address, size_t bytes);
/* Count newly parked complete chunks inside this explicit guest range. */
size_t hzn_heap_sparse_cold_park(void *address, size_t bytes);

#endif
