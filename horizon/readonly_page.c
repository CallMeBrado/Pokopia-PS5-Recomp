/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "readonly_page.h"

#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
enum { RESERVATION_BYTES = 0x10000 };
#else
#include <errno.h>
#include <orbis/libkernel.h>
#include <sys/mman.h>
enum { RESERVATION_BYTES = 0x4000, ORBIS_MAP_ANONYMOUS = 0x1000 };
#endif

static int32_t release_host(void *reservation, size_t size)
{
#ifdef _WIN32
    (void)size;
    return VirtualFree(reservation, 0, MEM_RELEASE) ? 0 : (int32_t)GetLastError();
#else
    return sceKernelMunmap(reservation, size);
#endif
}

bool hzn_readonly_page_unmap(HznReadonlyPage *page)
{
    if (!page)
        return false;
    if (page->reservation) {
        const int32_t result = release_host(page->reservation, page->reservation_bytes);
        if (result != 0) {
            page->last_host_error = result;
            page->unsafe = true;
            return false;
        }
    }
    *page = (HznReadonlyPage){0};
    return true;
}

bool hzn_readonly_page_map(HznReadonlyPage *page, uint64_t address, const void *initial_data)
{
    return initial_data && hzn_readonly_span_map(page, address, HZN_READONLY_PAGE_BYTES, initial_data);
}

bool hzn_readonly_span_map(HznReadonlyPage *page, uint64_t address, size_t size, const void *initial_data)
{
    if (!page || page->unsafe || page->reservation)
        return false;
    page->last_host_error = 0;

    const uint64_t base = address & ~(uint64_t)(RESERVATION_BYTES - 1);
    const size_t offset = (size_t)(address - base);
    if (!size || size % HZN_READONLY_PAGE_BYTES || address % HZN_READONLY_PAGE_BYTES ||
        !base || size > SIZE_MAX - offset - (RESERVATION_BYTES - 1))
        return false;
    const size_t reservation_bytes = (offset + size + RESERVATION_BYTES - 1) & ~(size_t)(RESERVATION_BYTES - 1);
    if (base > UINTPTR_MAX - reservation_bytes)
        return false;

    void *const wanted = (void *)(uintptr_t)base;
    void *const target = (void *)(uintptr_t)address;
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (info.dwPageSize != HZN_READONLY_PAGE_BYTES || info.dwAllocationGranularity != RESERVATION_BYTES) {
        page->last_host_error = ERROR_NOT_SUPPORTED;
        return false;
    }
    void *memory = VirtualAlloc(wanted, reservation_bytes, MEM_RESERVE, PAGE_NOACCESS);
    if (!memory) {
        page->last_host_error = (int32_t)GetLastError();
        return false;
    }
    const int32_t wrong_address = ERROR_INVALID_ADDRESS;
#else
    /* Request a non-replacing hint. Orbis uses FreeBSD's MAP_ANON value. */
    void *memory = mmap(wanted, reservation_bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | ORBIS_MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        page->last_host_error = errno;
        return false;
    }
    const int32_t wrong_address = (int32_t)ORBIS_KERNEL_ERROR_EFAULT;
#endif
    page->reservation = memory;
    page->reservation_bytes = reservation_bytes;
    int32_t result = memory == wanted ? 0 : wrong_address;
#ifdef _WIN32
    if (result == 0) {
        void *committed = VirtualAlloc(target, size, MEM_COMMIT, PAGE_READWRITE);
        if (committed != target)
            result = committed ? wrong_address : (int32_t)GetLastError();
    }
#endif
    if (result == 0) {
        if (initial_data)
            memcpy(target, initial_data, size);
#ifdef _WIN32
        DWORD old_protection;
        if (!VirtualProtect(target, size, PAGE_READONLY, &old_protection))
            result = (int32_t)GetLastError();
#else
        result = sceKernelMprotect(memory, reservation_bytes, PROT_READ);
#endif
    }
    if (result != 0) {
        if (hzn_readonly_page_unmap(page))
            page->last_host_error = result;
        return false;
    }

    page->address = address;
    return true;
}
