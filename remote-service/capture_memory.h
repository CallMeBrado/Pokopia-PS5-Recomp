/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS4_REMOTE_CAPTURE_MEMORY_H
#define PS4_REMOTE_CAPTURE_MEMORY_H

#include <errno.h>
#include <stdlib.h>

#if defined(__FreeBSD__)
#include <orbis/libkernel.h>
#include <sys/mman.h>
#endif

static inline void *capture_allocate(size_t bytes, int *error)
{
#if defined(__FreeBSD__)
    /* The compositor's libc allocator rejected the frame buffers. Give the
       capture worker its own zero-filled mapping, like the detour SDK does. */
    enum { ORBIS_MAP_PRIVATE_ANONYMOUS = 0x1002 };
    void *memory = NULL;
    *error = sceKernelMmap(NULL, bytes, PROT_READ | PROT_WRITE,
                          ORBIS_MAP_PRIVATE_ANONYMOUS, -1, 0, &memory);
    return *error == 0 ? memory : NULL;
#else
    void *memory = calloc(1, bytes);
    *error = memory ? 0 : errno;
    return memory;
#endif
}

static inline void capture_release(void *memory, size_t bytes)
{
#if defined(__FreeBSD__)
    if (memory)
        sceKernelMunmap(memory, bytes);
#else
    (void)bytes;
    free(memory);
#endif
}

#endif
