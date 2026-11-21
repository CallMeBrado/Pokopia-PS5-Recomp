/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "cold_store.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

bool hzn_cold_store_open(HznColdStore *store, const char *path, size_t slots)
{
    if (!store || store->slots || !path || !slots || slots > INT64_MAX / HZN_COLD_PAGE_BYTES)
        return false;
#ifdef _WIN32
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD returned;
    if (!DeviceIoControl(file, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL)) {
        CloseHandle(file);
        return false;
    }
#else
    const int file = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (file < 0)
        return false;
    if (unlink(path) != 0) {
        close(file);
        return false;
    }
#endif
    *store = (HznColdStore){.file = (intptr_t)file, .slots = slots};
    return true;
}

static bool transfer(const HznColdStore *store, size_t slot, void *output, const void *input)
{
    if (!store || slot >= store->slots || (!output && !input))
        return false;
    const int64_t offset = (int64_t)slot * HZN_COLD_PAGE_BYTES;
#ifdef _WIN32
    HANDLE file = (HANDLE)store->file;
    const LARGE_INTEGER position = {.QuadPart = offset};
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN))
        return false;
#endif
    size_t done = 0;
    while (done < HZN_COLD_PAGE_BYTES) {
        const size_t left = HZN_COLD_PAGE_BYTES - done;
#ifdef _WIN32
        DWORD bytes;
        const BOOL result = input ?
            WriteFile(file, (const uint8_t *)input + done, (DWORD)left, &bytes, NULL) :
            ReadFile(file, (uint8_t *)output + done, (DWORD)left, &bytes, NULL);
        if (!result)
            return false;
#else
        const int file = (int)store->file;
        const off_t position = (off_t)(offset + (int64_t)done);
        const ssize_t bytes = input ? pwrite(file, (const uint8_t *)input + done, left, position) :
                                      pread(file, (uint8_t *)output + done, left, position);
        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
#endif
        if (!bytes || (size_t)bytes > left)
            return false;
        done += (size_t)bytes;
    }
    return true;
}

bool hzn_cold_store_save(const HznColdStore *store, size_t slot, const void *page)
{
    return transfer(store, slot, NULL, page);
}

bool hzn_cold_store_load(const HznColdStore *store, size_t slot, void *page)
{
    return transfer(store, slot, page, NULL);
}

bool hzn_cold_store_close(HznColdStore *store)
{
    if (!store || !store->slots)
        return true;
#ifdef _WIN32
    if (!CloseHandle((HANDLE)store->file))
        return false;
    *store = (HznColdStore){0};
    return true;
#else
    const int file = (int)store->file;
    *store = (HznColdStore){0};
    /* An interrupted close does not give this owner permission to reuse fd. */
    return close(file) == 0;
#endif
}
