/* SPDX-License-Identifier: GPL-3.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "heap_sparse.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

enum {
    CHUNK_BYTES = 64 * 1024,
    CHUNK_EMPTY = 0,
    CHUNK_COMMITTING = 1,
    CHUNK_COMMITTED = 2,
};

typedef struct {
    HznHeap *heap;
    volatile LONG *chunks;
    size_t count;
    PVOID handler;
} SparseHeap;

static SparseHeap sparse;

static LONG chunk_state(size_t index)
{
    return InterlockedCompareExchange(&sparse.chunks[index], CHUNK_EMPTY, CHUNK_EMPTY);
}

static bool commit_chunk(size_t index)
{
    LONG state = InterlockedCompareExchange(&sparse.chunks[index], CHUNK_COMMITTING, CHUNK_EMPTY);
    if (state == CHUNK_EMPTY) {
        void *const base = (void *)(uintptr_t)(sparse.heap->base + index * CHUNK_BYTES);
        const bool committed = VirtualAlloc(base, CHUNK_BYTES, MEM_COMMIT, PAGE_READWRITE) == base;
        InterlockedExchange(&sparse.chunks[index], committed ? CHUNK_COMMITTED : CHUNK_EMPTY);
        return committed;
    }
    while (state == CHUNK_COMMITTING) {
        YieldProcessor();
        state = chunk_state(index);
    }
    return state == CHUNK_COMMITTED;
}

static bool writable_range(uintptr_t address, uintptr_t end)
{
    while (address < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((const void *)address, &info, sizeof info) ||
            info.AllocationBase != sparse.heap->mapping || info.State != MEM_COMMIT ||
            info.Protect != PAGE_READWRITE)
            return false;
        address = (uintptr_t)info.BaseAddress + info.RegionSize;
    }
    return true;
}

static LONG CALLBACK first_touch(EXCEPTION_POINTERS *exception)
{
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] > 1 || !sparse.heap)
        return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t address = record->ExceptionInformation[1];
    const uint64_t base = sparse.heap->base;
    if (address < base || address - base >= sparse.heap->size)
        return EXCEPTION_CONTINUE_SEARCH;

    const size_t index = (address - base) / CHUNK_BYTES;
    /* A racing fault can arrive after a chunk is committed. A stack alias
     * may also have made one of its pages inaccessible in the meantime. */
    if (commit_chunk(index) && writable_range(address, address + 1))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

bool hzn_heap_sparse_reserve(HznHeap *heap, size_t size)
{
    if (sparse.heap || !size || size % CHUNK_BYTES) {
        heap->last_host_error = ERROR_INVALID_PARAMETER;
        return false;
    }

    LONG *chunks = calloc(size / CHUNK_BYTES, sizeof *chunks);
    if (!chunks) {
        heap->last_host_error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }

    void *const requested = (void *)(uintptr_t)heap->base;
    void *mapping = VirtualAlloc(requested, size, MEM_RESERVE, PAGE_READWRITE);
    if (!mapping) {
        heap->last_host_error = (int32_t)GetLastError();
        free(chunks);
        return false;
    }

    sparse = (SparseHeap){.heap = heap, .chunks = chunks, .count = size / CHUNK_BYTES};
    heap->mapping = mapping;
    heap->backing_size = size;
    int32_t error = ERROR_INVALID_ADDRESS;
    if (mapping == requested) {
        sparse.handler = AddVectoredExceptionHandler(1, first_touch);
        if (sparse.handler) {
            heap->size = size;
            return true;
        }
        error = (int32_t)GetLastError();
        if (!error)
            error = ERROR_NOT_ENOUGH_MEMORY;
    }

    if (hzn_heap_sparse_release(heap))
        heap->last_host_error = error;
    return false;
}

bool hzn_heap_sparse_release(HznHeap *heap)
{
    if (heap != sparse.heap) {
        heap->last_host_error = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (sparse.handler) {
        if (!RemoveVectoredExceptionHandler(sparse.handler)) {
            heap->last_host_error = (int32_t)GetLastError();
            if (!heap->last_host_error)
                heap->last_host_error = ERROR_INVALID_HANDLE;
            return false;
        }
        sparse.handler = NULL;
    }
    if (!VirtualFree(heap->mapping, 0, MEM_RELEASE)) {
        heap->last_host_error = (int32_t)GetLastError();
        return false;
    }
    free((void *)sparse.chunks);
    sparse = (SparseHeap){0};
    heap->mapping = NULL;
    heap->backing_size = 0;
    heap->size = 0;
    return true;
}

bool hzn_heap_sparse_prepare_io(void *destination, size_t bytes)
{
    if (!sparse.heap || !bytes)
        return true;
    const uintptr_t address = (uintptr_t)destination;
    if (bytes > UINTPTR_MAX - address)
        return false;
    const uintptr_t end = address + bytes;
    const uint64_t heap_base = sparse.heap->base, heap_end = heap_base + sparse.heap->size;
    if (end <= heap_base || address >= heap_end)
        return true;
    if (address < heap_base || end > heap_end)
        return false;
    const size_t last = (end - 1 - heap_base) / CHUNK_BYTES;
    for (size_t index = (address - heap_base) / CHUNK_BYTES; index <= last; ++index)
        if (!commit_chunk(index))
            return false;
    return writable_range(address, end);
}

void hzn_heap_sparse_report(FILE *log, const char *phase)
{
    if (!sparse.heap)
        return;
    size_t committed = 0;
    for (size_t i = 0; i < sparse.count; ++i)
        committed += chunk_state(i) == CHUNK_COMMITTED;
    fprintf(log, "Heap sparse phase=%s chunk=%u committed=%zu reserved=%" PRIu64 "\n",
            phase, (unsigned)CHUNK_BYTES, committed * CHUNK_BYTES, sparse.heap->size);
    if (strcmp(phase, "guest-stop") == 0) {
        for (size_t i = 0; i < sparse.count;) {
            if (chunk_state(i) != CHUNK_COMMITTED) {
                ++i;
                continue;
            }
            const size_t first = i++;
            while (i < sparse.count && chunk_state(i) == CHUNK_COMMITTED)
                ++i;
            fprintf(log, "Heap sparse backed from=%#" PRIx64 " to=%#" PRIx64 "\n",
                    sparse.heap->base + first * CHUNK_BYTES, sparse.heap->base + i * CHUNK_BYTES);
        }
    }
    fflush(log);
}
