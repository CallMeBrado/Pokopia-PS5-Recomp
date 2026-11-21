/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_shared.h"

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
enum { GUEST_PAGE = 0x1000, NATIVE_PAGE = 0x1000, NATIVE_MAP_ALIGN = 0x10000 };
#else
#include <orbis/libkernel.h>
enum { GUEST_PAGE = 0x1000, NATIVE_PAGE = 0x4000, NATIVE_MAP_ALIGN = NATIVE_PAGE };
#endif

static int32_t unmap_view(void *view, size_t bytes)
{
#ifdef _WIN32
    (void)bytes;
    return UnmapViewOfFile(view) ? 0 : (int32_t)GetLastError();
#else
    return sceKernelMunmap(view, bytes);
#endif
}

static int32_t release_backing(uint64_t backing, size_t bytes)
{
#ifdef _WIN32
    (void)bytes;
    return CloseHandle((HANDLE)(uintptr_t)backing) ? 0 : (int32_t)GetLastError();
#else
    return sceKernelReleaseDirectMemory((off_t)backing, bytes);
#endif
}

static bool release_failed(HznLiveShared *shared, int32_t error)
{
    shared->last_host_error = error;
    shared->unsafe = true;
    return false;
}

bool hzn_live_shared_unmap(HznLiveShared *shared)
{
    if (!shared)
        return false;
    shared->address = 0;
    shared->size = 0;
    shared->writer = NULL;

    if (shared->guest_view) {
        const int32_t result = unmap_view(shared->guest_view, shared->mapped_bytes);
        if (result)
            return release_failed(shared, result);
        shared->guest_view = NULL;
    }
    if (shared->writer_view) {
        const int32_t result = unmap_view(shared->writer_view, shared->mapped_bytes);
        if (result)
            return release_failed(shared, result);
        shared->writer_view = NULL;
    }
    if (shared->mapped_bytes) {
        const int32_t result = release_backing(shared->backing, shared->mapped_bytes);
        if (result)
            return release_failed(shared, result);
    }
    *shared = (HznLiveShared){0};
    return true;
}

static bool map_failed(HznLiveShared *shared, int32_t error)
{
    if (hzn_live_shared_unmap(shared))
        shared->last_host_error = error;
    return false;
}

bool hzn_live_shared_map(HznLiveShared *shared, uint64_t address, size_t size)
{
    if (!shared || shared->unsafe || shared->mapped_bytes)
        return false;
    shared->last_host_error = 0;
    const uint64_t base = address & ~(uint64_t)(NATIVE_MAP_ALIGN - 1);
    const size_t offset = (size_t)(address - base);
    if (!base || !size || address % GUEST_PAGE || size % GUEST_PAGE ||
        size > SIZE_MAX - offset - (NATIVE_PAGE - 1))
        return false;
    const size_t bytes = (offset + size + NATIVE_PAGE - 1) & ~(size_t)(NATIVE_PAGE - 1);
    if (base > UINTPTR_MAX - bytes)
        return false;
    void *const wanted = (void *)(uintptr_t)base;

#ifdef _WIN32
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    if (system.dwPageSize != NATIVE_PAGE || system.dwAllocationGranularity != NATIVE_MAP_ALIGN)
        return map_failed(shared, ERROR_NOT_SUPPORTED);
    const HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        (DWORD)((uint64_t)bytes >> 32), (DWORD)bytes, NULL);
    if (!section)
        return map_failed(shared, (int32_t)GetLastError());
    shared->backing = (uintptr_t)section;
    shared->mapped_bytes = bytes;
    shared->writer_view = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, bytes);
    if (!shared->writer_view)
        return map_failed(shared, (int32_t)GetLastError());
    shared->guest_view = MapViewOfFileEx(section, FILE_MAP_READ, 0, 0, bytes, wanted);
    if (!shared->guest_view)
        return map_failed(shared, (int32_t)GetLastError());
    if (shared->guest_view != wanted)
        return map_failed(shared, ERROR_INVALID_ADDRESS);
    if (offset) {
        DWORD old_protection;
        if (!VirtualProtect(shared->guest_view, offset, PAGE_NOACCESS, &old_protection))
            return map_failed(shared, (int32_t)GetLastError());
    }
#else
    const size_t available = sceKernelGetDirectMemorySize();
    if (bytes > available || available > INT64_MAX)
        return map_failed(shared, (int32_t)ORBIS_KERNEL_ERROR_ENOMEM);
    off_t physical;
    int32_t result = sceKernelAllocateDirectMemory(0, (off_t)available, bytes,
        NATIVE_PAGE, ORBIS_KERNEL_WB_ONION, &physical);
    if (result)
        return map_failed(shared, result);
    shared->backing = (uint64_t)physical;
    shared->mapped_bytes = bytes;

    void *writer = NULL;
    result = sceKernelMapDirectMemory(&writer, bytes, ORBIS_KERNEL_PROT_CPU_RW, 0, physical, NATIVE_PAGE);
    if (result)
        return map_failed(shared, result);
    if (!writer)
        return map_failed(shared, (int32_t)ORBIS_KERNEL_ERROR_EFAULT);
    shared->writer_view = writer;
    /* Direct physical memory can come from a previous allocation. */
    memset(writer, 0, bytes);

    /* A non-fixed hint must not replace another mapping. Free only the
     * address actually returned by the kernel if the hint is refused. */
    void *guest = wanted;
    result = sceKernelMapDirectMemory(&guest, bytes, ORBIS_KERNEL_PROT_CPU_READ, 0, physical, NATIVE_PAGE);
    if (result)
        return map_failed(shared, result);
    if (guest == writer)
        return map_failed(shared, (int32_t)ORBIS_KERNEL_ERROR_EFAULT);
    shared->guest_view = guest;
    if (guest != wanted)
        return map_failed(shared, (int32_t)ORBIS_KERNEL_ERROR_EFAULT);
#endif
    shared->address = address;
    shared->size = size;
    shared->writer = (uint8_t *)shared->writer_view + offset;
    return true;
}
