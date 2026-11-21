/* SPDX-License-Identifier: GPL-3.0-or-later */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>

#include "cold_heap_windows.h"
#include "cold_store.h"

#include <inttypes.h>
#include <stdlib.h>

typedef enum { COLD_EMPTY, COLD_PRIVATE, COLD_RESTORED, COLD_SPILLED } ChunkState;

typedef struct {
    ChunkState state;
    bool restore_protection;
} ColdChunk;

typedef struct {
    HznHeap *heap;
    HznColdStore store;
    ColdChunk *chunks;
    size_t count, released, parks, restores;
    PVOID handler;
} ColdHeap;

static ColdHeap cold;
static volatile LONG heap_lock;

static void lock_heap(void)
{
    while (InterlockedCompareExchange(&heap_lock, 1, 0))
        YieldProcessor();
}

static void unlock_heap(void)
{
    InterlockedExchange(&heap_lock, 0);
}

static void *chunk_address(size_t index)
{
    return (void *)(uintptr_t)(cold.heap->base + index * HZN_COLD_PAGE_BYTES);
}

static bool writable(uintptr_t start, uintptr_t end)
{
    while (start < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((const void *)start, &info, sizeof info) ||
            info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE)
            return false;
        start = (uintptr_t)info.BaseAddress + info.RegionSize;
    }
    return true;
}

static bool restore_page(size_t index)
{
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, HZN_COLD_PAGE_BYTES, NULL);
    if (!section)
        return false;
    void *alias = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, HZN_COLD_PAGE_BYTES);
    bool ready = alias && hzn_cold_store_load(&cold.store, index, alias);
    if (alias && !UnmapViewOfFile(alias))
        ready = false;

    /* Only publish the prepared section when all bytes were restored. */
    void *target = chunk_address(index);
    void *mapped = ready ? MapViewOfFile3(section, GetCurrentProcess(), target, 0, HZN_COLD_PAGE_BYTES,
                                          MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0) : NULL;
    CloseHandle(section);
    if (mapped != target)
        return false;
    cold.chunks[index].state = COLD_RESTORED;
    ++cold.restores;
    return true;
}

static bool materialize(size_t index)
{
    ColdChunk *chunk = &cold.chunks[index];
    void *target = chunk_address(index);
    if (chunk->restore_protection) {
        DWORD previous;
        if (!VirtualProtect(target, HZN_COLD_PAGE_BYTES, PAGE_READWRITE, &previous))
            return false;
        chunk->restore_protection = false;
    }
    if (chunk->state == COLD_SPILLED)
        return restore_page(index);
    if (chunk->state == COLD_EMPTY) {
        if (VirtualAlloc2(GetCurrentProcess(), target, HZN_COLD_PAGE_BYTES,
                          MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                          PAGE_READWRITE, NULL, 0) != target)
            return false;
        chunk->state = COLD_PRIVATE;
    }
    return true;
}

static LONG CALLBACK first_touch(EXCEPTION_POINTERS *exception)
{
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;
    if (!cold.handler || record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->NumberParameters < 2 || record->ExceptionInformation[0] > 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t address = record->ExceptionInformation[1];
    const uint64_t base = cold.heap->base;
    if (address < base || address - base >= cold.heap->size)
        return EXCEPTION_CONTINUE_SEARCH;
    const size_t index = (address - base) / HZN_COLD_PAGE_BYTES;
    lock_heap();
    const bool ready = materialize(index) && writable(address, address + 1);
    unlock_heap();
    return ready ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

static bool reserve_with_store(HznHeap *heap, size_t bytes, HznColdStore *store)
{
    if (cold.heap || !heap || heap->mapping || !bytes || !heap->base ||
        heap->base % HZN_COLD_PAGE_BYTES || bytes % HZN_COLD_PAGE_BYTES ||
        bytes > UINTPTR_MAX - heap->base)
        return false;

    const size_t count = bytes / HZN_COLD_PAGE_BYTES;
    ColdChunk *chunks = calloc(count, sizeof *chunks);
    if (!chunks)
        return false;

    void *target = (void *)(uintptr_t)heap->base;
    uint8_t *mapping = VirtualAlloc2(GetCurrentProcess(), target, bytes,
                                    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (mapping != target) {
        if (mapping)
            VirtualFree(mapping, 0, MEM_RELEASE);
        free(chunks);
        return false;
    }
    size_t split = 0;
    for (; split + 1 < count; ++split)
        if (!VirtualFree(mapping + split * HZN_COLD_PAGE_BYTES, HZN_COLD_PAGE_BYTES,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            break;
    if (split + 1 != count) {
        for (size_t i = 0; i <= split; ++i)
            VirtualFree(mapping + i * HZN_COLD_PAGE_BYTES, 0, MEM_RELEASE);
        free(chunks);
        return false;
    }

    cold = (ColdHeap){.heap = heap, .store = *store, .chunks = chunks, .count = count};
    *store = (HznColdStore){0};
    cold.handler = AddVectoredExceptionHandler(1, first_touch);
    if (!cold.handler) {
        hzn_cold_heap_release(heap);
        return false;
    }
    heap->mapping = mapping;
    heap->backing_size = heap->size = bytes;
    return true;
}

bool hzn_cold_heap_reserve(HznHeap *heap, size_t bytes, const char *file)
{
    HznColdStore store = {0};
    if (!hzn_cold_store_open(&store, file, bytes / HZN_COLD_PAGE_BYTES))
        return false;
    const bool reserved = reserve_with_store(heap, bytes, &store);
    if (!reserved)
        hzn_cold_store_close(&store);
    return reserved;
}

bool hzn_cold_heap_release(HznHeap *heap)
{
    if (!cold.heap || heap != cold.heap)
        return false;
    if (cold.handler) {
        if (!RemoveVectoredExceptionHandler(cold.handler))
            return false;
        cold.handler = NULL;
    }
    while (cold.released < cold.count) {
        const size_t index = cold.released;
        void *address = chunk_address(index);
        if (!(cold.chunks[index].state == COLD_RESTORED ?
              UnmapViewOfFile(address) : VirtualFree(address, 0, MEM_RELEASE)))
            return false;
        ++cold.released;
    }
    if (!hzn_cold_store_close(&cold.store))
        return false;
    free(cold.chunks);
    cold = (ColdHeap){0};
    heap->mapping = NULL;
    heap->size = heap->backing_size = 0;
    return true;
}

bool hzn_cold_heap_spill(void *address)
{
    const uintptr_t start = (uintptr_t)address;
    if (!cold.handler || start < cold.heap->base || start - cold.heap->base >= cold.heap->size ||
        start % HZN_COLD_PAGE_BYTES)
        return false;
    const size_t index = (start - cold.heap->base) / HZN_COLD_PAGE_BYTES;
    lock_heap();
    ColdChunk *chunk = &cold.chunks[index];
    bool spilled = false;
    if (chunk->state == COLD_PRIVATE || chunk->state == COLD_RESTORED) {
        DWORD previous;
        if (materialize(index) && writable(start, start + HZN_COLD_PAGE_BYTES) &&
            VirtualProtect(address, HZN_COLD_PAGE_BYTES, PAGE_READONLY, &previous)) {
            if (hzn_cold_store_save(&cold.store, index, address))
                spilled = chunk->state == COLD_PRIVATE ?
                    VirtualFree(address, HZN_COLD_PAGE_BYTES, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) :
                    UnmapViewOfFileEx(address, MEM_PRESERVE_PLACEHOLDER);
            if (spilled) {
                chunk->state = COLD_SPILLED;
                ++cold.parks;
            } else {
                DWORD ignored;
                chunk->restore_protection = !VirtualProtect(address, HZN_COLD_PAGE_BYTES, previous, &ignored);
            }
        }
    }
    unlock_heap();
    return spilled;
}

bool hzn_cold_heap_prepare_io(void *buffer, size_t bytes)
{
    if (!cold.handler || !bytes)
        return true;
    const uintptr_t start = (uintptr_t)buffer;
    if (bytes > UINTPTR_MAX - start)
        return false;
    const uintptr_t end = start + bytes, base = (uintptr_t)cold.heap->base, limit = base + cold.heap->size;
    if (end <= base || start >= limit)
        return true;
    if (start < base || end > limit)
        return false;

    lock_heap();
    bool ready = true;
    for (size_t index = (start - base) / HZN_COLD_PAGE_BYTES; index <= (end - 1 - base) / HZN_COLD_PAGE_BYTES; ++index) {
        if (!materialize(index)) {
            ready = false;
            break;
        }
    }
    ready = ready && writable(start, end);
    unlock_heap();
    return ready;
}

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

bool hzn_heap_sparse_cold_contains(const void *address, size_t bytes)
{
    if (!cold.handler || !bytes)
        return false;
    const uintptr_t start = (uintptr_t)address, base = (uintptr_t)cold.heap->base;
    return start >= base && start - base < cold.heap->size && bytes <= cold.heap->size - (start - base);
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
        resident += state == COLD_PRIVATE || state == COLD_RESTORED;
        parked += state == COLD_SPILLED;
    }
    const size_t parks = cold.parks, restores = cold.restores;
    unlock_heap();
    fprintf(log, "Heap sparse phase=%s chunk=%u committed=%zu reserved=%" PRIu64
                 " cold_parked=%zu cold_park_events=%zu cold_restore_events=%zu\n",
            phase, (unsigned)HZN_COLD_PAGE_BYTES, resident * HZN_COLD_PAGE_BYTES, cold.heap->size,
            parked * HZN_COLD_PAGE_BYTES, parks, restores);
    fflush(log);
}


