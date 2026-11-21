#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

#include "heap_watch.h"

enum { HOST_PAGE = 4096, ORBIS_PAGE = 16384, TOP_RANGE = 128 * 1024 * 1024 };

unsigned heap_watch_sample(const HznHeap *heap, HeapWatchSample *sample)
{
    if (!heap || !sample)
        return ERROR_INVALID_PARAMETER;
    *sample = (HeapWatchSample){0};
    if (!heap->size)
        return ERROR_SUCCESS;
    if (!heap->mapping || heap->size % ORBIS_PAGE)
        return ERROR_INVALID_PARAMETER;

    ULONG_PTR count = heap->size / HOST_PAGE;
    const size_t max_count = count;
    void **pages = malloc(max_count * sizeof *pages);
    uint8_t *orbis_pages = calloc((heap->size / ORBIS_PAGE + 7) / 8, 1);
    if (!pages || !orbis_pages) {
        free(orbis_pages);
        free(pages);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    DWORD granularity = 0;
    unsigned result = GetWriteWatch(0, heap->mapping, (size_t)heap->size, pages, &count, &granularity);
    if (!result && (granularity != HOST_PAGE || count > max_count))
        result = ERROR_INVALID_DATA;
    if (!result) {
        const uintptr_t base = (uintptr_t)heap->mapping;
        const uint64_t top = heap->size > TOP_RANGE ? heap->size - TOP_RANGE : 0;
        sample->pages_4k = count;
        for (ULONG_PTR i = 0; i < count; ++i) {
            const uint64_t offset = (uintptr_t)pages[i] - base;
            if (offset >= heap->size || offset % HOST_PAGE) {
                *sample = (HeapWatchSample){0};
                result = ERROR_INVALID_DATA;
                break;
            }
            if (offset + HOST_PAGE > sample->highest_page_end)
                sample->highest_page_end = offset + HOST_PAGE;
            if (offset >= top)
                ++sample->pages_4k_in_last_128mib;
            const uint64_t page = offset / ORBIS_PAGE;
            const uint8_t mask = (uint8_t)(1u << (page % 8));
            if (!(orbis_pages[page / 8] & mask)) {
                orbis_pages[page / 8] |= mask;
                ++sample->pages_16k;
            }
        }
    }
    free(orbis_pages);
    free(pages);
    return result;
}
