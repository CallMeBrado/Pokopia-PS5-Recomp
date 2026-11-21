/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "heap.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HznHeap *watched_heap;
static volatile LONG *chunks;
static PVOID fault_handler;
static size_t chunk_size, chunk_count;

static LONG CALLBACK materialize_heap_chunk(EXCEPTION_POINTERS *exception)
{
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] > 1 || !watched_heap)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t address = record->ExceptionInformation[1];
    if (address < watched_heap->base || address - watched_heap->base >= watched_heap->size)
        return EXCEPTION_CONTINUE_SEARCH;
    const size_t index = (address - watched_heap->base) / chunk_size;
    const LONG state = InterlockedCompareExchange(&chunks[index], 1, 0);
    if (state == 0) {
        void *base = (void *)(uintptr_t)(watched_heap->base + index * chunk_size);
        void *mapped = VirtualAlloc(base, chunk_size, MEM_COMMIT, PAGE_READWRITE);
        InterlockedExchange(&chunks[index], mapped == base ? 2 : 0);
        return mapped == base ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }
    if (state == 1)
        while (InterlockedCompareExchange(&chunks[index], 0, 0) == 1)
            YieldProcessor();
    /* A racing first touch can arrive after another thread mapped the chunk.
       Heap stack aliases also use NOACCESS, so never retry a protected page. */
    MEMORY_BASIC_INFORMATION mapping;
    if (VirtualQuery((const void *)address, &mapping, sizeof mapping) &&
        mapping.State == MEM_COMMIT && mapping.Protect == PAGE_READWRITE)
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

bool sparse_heap_prepare_io(void *destination, size_t bytes)
{
    if (!watched_heap || !bytes)
        return true;
    const uintptr_t address = (uintptr_t)destination;
    if (address < watched_heap->base || address - watched_heap->base >= watched_heap->size)
        return true;
    const size_t offset = address - watched_heap->base;
    if (bytes > watched_heap->size - offset)
        return false;
    const size_t last = offset + bytes - 1;
    for (size_t i = offset / chunk_size; i <= last / chunk_size; ++i) {
        const uintptr_t page = watched_heap->base + i * chunk_size;
        EXCEPTION_RECORD record = {
            .ExceptionCode = EXCEPTION_ACCESS_VIOLATION, .NumberParameters = 2,
            .ExceptionInformation = {1, page < address ? address : page}
        };
        EXCEPTION_POINTERS exception = { .ExceptionRecord = &record };
        if (materialize_heap_chunk(&exception) != EXCEPTION_CONTINUE_EXECUTION)
            return false;
    }
    return true;
}
void sparse_heap_report(FILE *log, const char *phase)
{
    if (!watched_heap)
        return;
    size_t live = 0;
    for (size_t i = 0; i < chunk_count; ++i)
        live += InterlockedCompareExchange(&chunks[i], 0, 0) == 2;
    fprintf(log, "Heap sparse phase=%s chunk=%zu committed=%zu reserved=%" PRIu64 "\n",
            phase, chunk_size, live * chunk_size, watched_heap->size);
    if (strcmp(phase, "guest-stop") == 0) {
        for (size_t i = 0; i < chunk_count;) {
            if (InterlockedCompareExchange(&chunks[i], 0, 0) != 2) {
                ++i;
                continue;
            }
            const size_t first = i++;
            while (i < chunk_count && InterlockedCompareExchange(&chunks[i], 0, 0) == 2)
                ++i;
            fprintf(log, "Heap sparse backed from=%#" PRIx64 " to=%#" PRIx64 "\n",
                    watched_heap->base + first * chunk_size, watched_heap->base + i * chunk_size);
        }
    }
}
#else
#include <orbis/libkernel.h>
#include <string.h>
#endif

bool hzn_heap_init(HznHeap *heap, uint64_t base, uint64_t budget)
{
    if (!heap || !base || base % HZN_HEAP_GRANULE || base > UINTPTR_MAX ||
        budget < HZN_HEAP_GRANULE || budget > SIZE_MAX || budget > UINTPTR_MAX - base)
        return false;
    *heap = (HznHeap){ .base = base, .budget = budget };
    return true;
}

bool hzn_heap_release(HznHeap *heap)
{
    if (!heap)
        return false;
    heap->last_host_error = 0;
    if (!heap->backing_size)
        return true;

#ifdef _WIN32
    if (!VirtualFree(heap->mapping, 0, MEM_RELEASE)) {
        heap->last_host_error = (int32_t)GetLastError();
        return false;
    }
    heap->mapping = NULL;
    heap->size = 0;
    if (watched_heap == heap) {
        RemoveVectoredExceptionHandler(fault_handler);
        watched_heap = NULL;
        fault_handler = NULL;
        free((void *)chunks);
        chunks = NULL;
    }
#else
    if (heap->mapping) {
        int32_t result = sceKernelMunmap(heap->mapping, heap->backing_size);
        if (result != 0) {
            heap->last_host_error = result;
            return false;
        }
        heap->mapping = NULL;
        heap->size = 0;
    }
    int32_t result = sceKernelReleaseDirectMemory((off_t)heap->direct_offset, heap->backing_size);
    if (result != 0) {
        heap->last_host_error = result;
        return false;
    }
    heap->direct_offset = 0;
#endif
    heap->backing_size = 0;
    return true;
}

static bool commit_heap(HznHeap *heap, size_t size)
{
    void *const requested = (void *)(uintptr_t)heap->base;
#ifdef _WIN32
    void *mapped = VirtualAlloc(requested, size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!mapped) {
        heap->last_host_error = (int32_t)GetLastError();
        return false;
    }
    heap->mapping = mapped;
    heap->backing_size = size;
    const int32_t wrong_address = ERROR_INVALID_ADDRESS;
#else
    const size_t available = sceKernelGetDirectMemorySize();
    if (size > available || available > INT64_MAX) {
        heap->last_host_error = (int32_t)ORBIS_KERNEL_ERROR_ENOMEM;
        return false;
    }
    off_t offset;
    int32_t result = sceKernelAllocateDirectMemory(0, (off_t)available, size,
        HZN_HEAP_GRANULE, ORBIS_KERNEL_WB_ONION, &offset);
    if (result != 0) {
        heap->last_host_error = result;
        return false;
    }
    heap->direct_offset = offset;
    heap->backing_size = size;

    /* A non-fixed hint cannot overwrite another host mapping. Keep and free
     * only the address actually returned by the kernel. */
    void *mapped = requested;
    result = sceKernelMapDirectMemory(&mapped, size, ORBIS_KERNEL_PROT_CPU_RW, 0,
        offset, HZN_HEAP_GRANULE);
    if (result != 0) {
        if (hzn_heap_release(heap))
            heap->last_host_error = result;
        return false;
    }
    heap->mapping = mapped;
    const int32_t wrong_address = (int32_t)ORBIS_KERNEL_ERROR_EFAULT;
#endif
    if (mapped != requested) {
        if (hzn_heap_release(heap))
            heap->last_host_error = wrong_address;
        return false;
    }
#ifdef _WIN32
    char chunk_kib[8];
    DWORD len = GetEnvironmentVariableA("POKOPIA_SPARSE_CHUNK_KIB", chunk_kib, sizeof chunk_kib);
    unsigned long kib = len && len < sizeof chunk_kib ? strtoul(chunk_kib, NULL, 10) : 256;
    if (watched_heap || kib < 16 || kib > 2048 || (kib & (kib - 1))) {
        hzn_heap_release(heap);
        heap->last_host_error = ERROR_INVALID_PARAMETER;
        return false;
    }
    chunk_size = kib * 1024;
    chunk_count = size / chunk_size;
    chunks = calloc(chunk_count, sizeof *chunks);
    fault_handler = chunks ? AddVectoredExceptionHandler(1, materialize_heap_chunk) : NULL;
    if (!fault_handler) {
        free((void *)chunks);
        chunks = NULL;
        hzn_heap_release(heap);
        heap->last_host_error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    watched_heap = heap;
#else
    /* Horizon exposes zeroed heap pages. Do not rely on recycled direct
     * physical memory being cleared by the host allocator. */
    memset(mapped, 0, size);
#endif
    heap->size = size;
    return true;
}

uint32_t hzn_heap_set_size(HznHeap *heap, uint64_t size, uint64_t *out_base)
{
    if (out_base)
        *out_base = 0;
    if (!heap || !heap->base || !out_base)
        return HZN_HEAP_INVALID_MEMORY_STATE;
    heap->last_host_error = 0;
    if (size % HZN_HEAP_GRANULE)
        return HZN_HEAP_INVALID_SIZE;
    if (size > heap->budget)
        return HZN_HEAP_OUT_OF_MEMORY;
    if (heap->size && size != heap->size)
        return HZN_HEAP_INVALID_MEMORY_STATE;
    if (heap->backing_size && !heap->size && !hzn_heap_release(heap))
        return HZN_HEAP_OUT_OF_MEMORY;
    if (size && !heap->size && !commit_heap(heap, (size_t)size))
        return HZN_HEAP_OUT_OF_MEMORY;
    *out_base = heap->base;
    return HZN_HEAP_OK;
}
