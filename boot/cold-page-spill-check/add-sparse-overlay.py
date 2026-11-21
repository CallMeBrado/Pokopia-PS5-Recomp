from pathlib import Path

p = Path('cold_heap_windows.c')
s = p.read_text()
s = s.replace('#include <stdlib.h>', '#include <inttypes.h>\n#include <stdlib.h>')
s = s.replace('    size_t count, released;', '    size_t count, released, parks, restores;')
s = s.replace('    cold.chunks[index].state = CHUNK_RESTORED;', '    cold.chunks[index].state = CHUNK_RESTORED;\n    ++cold.restores;')
s = s.replace('bool hzn_cold_heap_reserve(HznHeap *heap, size_t bytes, const char *file)',
              'static bool reserve_with_store(HznHeap *heap, size_t bytes, HznColdStore *store)')
s = s.replace('''    HznColdStore store = {0};
    if (!chunks || !hzn_cold_store_open(&store, file, count)) {
        free(chunks);
        return false;
    }''', '''    if (!chunks)
        return false;''')
s = s.replace('        hzn_cold_store_close(&store);\n', '')
s = s.replace('''    cold = (ColdHeap){.heap = heap, .store = store, .chunks = chunks, .count = count};''',
              '''    cold = (ColdHeap){.heap = heap, .store = *store, .chunks = chunks, .count = count};
    *store = (HznColdStore){0};''')
s = s.replace('''bool hzn_cold_heap_release(HznHeap *heap)''', '''bool hzn_cold_heap_reserve(HznHeap *heap, size_t bytes, const char *file)
{
    HznColdStore store = {0};
    if (!hzn_cold_store_open(&store, file, bytes / HZN_COLD_PAGE_BYTES))
        return false;
    const bool reserved = reserve_with_store(heap, bytes, &store);
    if (!reserved)
        hzn_cold_store_close(&store);
    return reserved;
}

bool hzn_cold_heap_release(HznHeap *heap)''')
s = s.replace('    bool spilled = chunk->state == CHUNK_SPILLED;', '    bool spilled = false;')
s = s.replace('''                chunk->state = CHUNK_SPILLED;
            } else {''', '''                chunk->state = CHUNK_SPILLED;
                ++cold.parks;
            } else {''')
s += '''
/* Keep the original Windows sparse behavior when spill is disabled. */
#define hzn_heap_sparse_reserve plain_sparse_reserve
#define hzn_heap_sparse_release plain_sparse_release
#define hzn_heap_sparse_prepare_io plain_sparse_prepare_io
#define hzn_heap_sparse_report plain_sparse_report
#define first_touch plain_first_touch
#include "../../horizon/heap_sparse_windows.c"
#undef first_touch
#undef hzn_heap_sparse_report
#undef hzn_heap_sparse_prepare_io
#undef hzn_heap_sparse_release
#undef hzn_heap_sparse_reserve

static HznColdStore pending_store;

bool hzn_heap_sparse_cold_open(const char *path)
{
    if (cold.heap || sparse.heap)
        return false;
    return hzn_cold_store_open(&pending_store, path, INT64_MAX / HZN_COLD_PAGE_BYTES);
}

bool hzn_heap_sparse_reserve(HznHeap *heap, size_t bytes)
{
    if (!pending_store.slots)
        return plain_sparse_reserve(heap, bytes);
    const bool reserved = reserve_with_store(heap, bytes, &pending_store);
    if (!reserved) {
        heap->last_host_error = (int32_t)GetLastError();
        hzn_cold_store_close(&pending_store);
    }
    return reserved;
}

bool hzn_heap_sparse_release(HznHeap *heap)
{
    return cold.heap == heap ? hzn_cold_heap_release(heap) : plain_sparse_release(heap);
}

bool hzn_heap_sparse_prepare_io(void *buffer, size_t bytes)
{
    return cold.handler ? hzn_cold_heap_prepare_io(buffer, bytes) : plain_sparse_prepare_io(buffer, bytes);
}

size_t hzn_heap_sparse_cold_park(void *address, size_t bytes)
{
    if (!cold.handler || !bytes)
        return 0;
    const uintptr_t start = (uintptr_t)address, base = (uintptr_t)cold.heap->base;
    const uintptr_t limit = base + cold.heap->size;
    if (start < base || start >= limit || bytes > limit - start)
        return 0;
    const uintptr_t end = (start + bytes) & ~(uintptr_t)(HZN_COLD_PAGE_BYTES - 1);
    size_t parked = 0;
    for (uintptr_t page = (start + HZN_COLD_PAGE_BYTES - 1) & ~(uintptr_t)(HZN_COLD_PAGE_BYTES - 1);
         page < end; page += HZN_COLD_PAGE_BYTES) {
        if (hzn_cold_heap_spill((void *)page))
            parked += HZN_COLD_PAGE_BYTES;
    }
    return parked;
}

void hzn_heap_sparse_report(FILE *log, const char *phase)
{
    if (!cold.handler) {
        plain_sparse_report(log, phase);
        return;
    }
    size_t resident = 0, parked = 0;
    lock_heap();
    for (size_t i = 0; i < cold.count; ++i) {
        const ChunkState state = cold.chunks[i].state;
        resident += state == CHUNK_PRIVATE || state == CHUNK_RESTORED;
        parked += state == CHUNK_SPILLED;
    }
    const size_t parks = cold.parks, restores = cold.restores;
    unlock_heap();
    fprintf(log, "Heap sparse phase=%s chunk=%u committed=%zu reserved=%" PRIu64
                 " cold_parked=%zu cold_park_events=%zu cold_restore_events=%zu\\n",
            phase, (unsigned)HZN_COLD_PAGE_BYTES, resident * HZN_COLD_PAGE_BYTES, cold.heap->size,
            parked * HZN_COLD_PAGE_BYTES, parks, restores);
    fflush(log);
}
'''
p.write_text(s)

h = Path('cold_heap_windows.h')
s = h.read_text()
s = s.replace('''#endif''','''/* Call before the sparse heap is reserved. */
bool hzn_heap_sparse_cold_open(const char *path);
/* Count newly parked complete chunks inside this explicit guest range. */
size_t hzn_heap_sparse_cold_park(void *address, size_t bytes);

#endif''')
h.write_text(s)
