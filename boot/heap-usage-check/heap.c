/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "heap.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
    void *mapped = VirtualAlloc(requested, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
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
#ifndef _WIN32
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
