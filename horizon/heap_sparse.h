/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_HEAP_SPARSE_H
#define POKOPIA_HORIZON_HEAP_SPARSE_H

#include "heap.h"
#include <stdio.h>

/* One heap is active at a time. Release requires its users to have stopped. */
bool hzn_heap_sparse_reserve(HznHeap *heap, size_t size);
bool hzn_heap_sparse_release(HznHeap *heap);

/* Kernel file I/O needs writable backing before it receives a guest buffer. */
bool hzn_heap_sparse_prepare_io(void *destination, size_t bytes);
void hzn_heap_sparse_report(FILE *log, const char *phase);

#endif
