#ifndef POKOPIA_HEAP_WATCH_H
#define POKOPIA_HEAP_WATCH_H

#include "heap.h"

typedef struct {
    uint64_t pages_4k;
    uint64_t pages_16k;
    uint64_t highest_page_end;
    uint64_t pages_4k_in_last_128mib;
} HeapWatchSample;

unsigned heap_watch_sample(const HznHeap *heap, HeapWatchSample *sample);

#endif
