#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>

#include "heap_watch.h"

int main(void)
{
    HznHeap heap;
    HeapWatchSample sample;
    uint64_t base;
    assert(hzn_heap_init(&heap, UINT64_C(0x1000000000), HZN_HEAP_GRANULE));
    assert(hzn_heap_set_size(&heap, HZN_HEAP_GRANULE, &base) == HZN_HEAP_OK);
    volatile unsigned char *data = heap.mapping;
    assert(heap_watch_sample(&heap, &sample) == 0 && sample.pages_4k == 0);
    assert(data[0x2000] == 0);
    assert(heap_watch_sample(&heap, &sample) == 0 && sample.pages_4k == 1);
    data[0x10] = 1;
    data[0x3000] = 1;
    data[0x4000] = 1;
    assert(heap_watch_sample(&heap, &sample) == 0);
    assert(sample.pages_4k == 4 && sample.pages_16k == 2 && sample.highest_page_end == 0x5000);

    DWORD protection;
    assert(VirtualProtect((void *)(data + 0x3000), 0x1000, PAGE_NOACCESS, &protection));
    data[HZN_HEAP_GRANULE - 1] = 1;
    assert(heap_watch_sample(&heap, &sample) == 0);
    assert(sample.pages_4k == 5 && sample.pages_16k == 3 && sample.highest_page_end == HZN_HEAP_GRANULE);
    assert(VirtualProtect((void *)(data + 0x3000), 0x1000, protection, &protection));
    assert(heap_watch_sample(&heap, &sample) == 0 && sample.pages_4k == 5);
    assert(ResetWriteWatch((void *)data, HZN_HEAP_GRANULE) == 0);
    assert(data[0x2000] == 0);
    assert(heap_watch_sample(&heap, &sample) == 0 && sample.pages_4k == 0);
    assert(hzn_heap_release(&heap));
    assert(heap_watch_sample(&heap, &sample) == 0 && sample.pages_4k == 0);
    puts("Heap watch passed: cumulative pages, demand-zero first read, 16 KiB grouping, protected dirty pages, highest end.");
}
