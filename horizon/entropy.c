/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "get_info.h"

#include <stddef.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <stdio.h>
#endif

#if defined(__FreeBSD__) || defined(__ORBIS__) || defined(__PS4__)
/* Exported by libkernel. OpenOrbis's libc headers omit the BSD declaration. */
extern int sysctlbyname(const char *, void *, size_t *, const void *, size_t);
#endif

bool hzn_get_info_entropy_seed(uint64_t *seed)
{
    uint64_t value;
    if (!seed)
        return false;
#ifdef _WIN32
    HMODULE library = LoadLibraryW(L"advapi32.dll");
    if (!library)
        return false;
    typedef BOOLEAN (WINAPI *RandomFn)(void *, ULONG);
    RandomFn generate = (RandomFn)GetProcAddress(library, "SystemFunction036");
    bool success = generate && generate(&value, sizeof value);
    FreeLibrary(library);
#else
    bool success = false;
#if defined(__FreeBSD__) || defined(__ORBIS__) || defined(__PS4__)
    size_t length = sizeof value;
    success = sysctlbyname("kern.arandom", &value, &length, NULL, 0) == 0 && length == sizeof value;
#endif
    if (!success) {
        FILE *random = fopen("/dev/urandom", "rb");
        if (random) {
            success = fread(&value, sizeof value, 1, random) == 1;
            fclose(random);
        }
    }
#endif
    if (success)
        *seed = value;
    return success;
}

