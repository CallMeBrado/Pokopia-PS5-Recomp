/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_STACK_BACKING_H
#define POKOPIA_HORIZON_STACK_BACKING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *address;
    uint64_t physical;
    bool direct_owned;
} HznStackBacking;

static inline bool hzn_stack_backing_owned(const HznStackBacking *backing)
{
    return backing->address || backing->direct_owned;
}

#ifndef _WIN32
#include <errno.h>
#include <orbis/libkernel.h>
#include <string.h>
#include <sys/mman.h>

/* The SDK header declares the output size by value. */
extern int32_t hzn_stack_flexible_available(size_t *bytes) __asm__("sceKernelAvailableFlexibleMemorySize");

static inline int32_t hzn_orbis_stack_backing_open(HznStackBacking *backing, void *requested, size_t bytes)
{
    enum {
        ANONYMOUS = 0x1000, NO_OVERWRITE = 0x80, PAGE_BYTES = 0x4000,
        FLEXIBLE_RESERVE = 16 * 1024 * 1024,
    };
    size_t available = 0;
    if (hzn_stack_flexible_available(&available) ||
        (available >= bytes && available - bytes >= FLEXIBLE_RESERVE)) {
        void *mapped = mmap(requested, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            backing->address = mapped;
            return mapped == requested ? 0 : (int32_t)ORBIS_KERNEL_ERROR_EFAULT;
        }
        const int error = errno;
        /* At the flexible ceiling libkernel also returns EINVAL for this tuple. */
        if (error && error != ENOMEM && error != EAGAIN && error != EINVAL)
            return error;
    }

    off_t physical;
    int32_t result = sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), bytes,
        PAGE_BYTES, ORBIS_KERNEL_WB_ONION, &physical);
    if (result)
        return result;
    backing->physical = (uint64_t)physical;
    backing->direct_owned = true;

    /* Never replace a host mapping that the guest address-space model missed. */
    void *mapped = requested;
    result = sceKernelMapDirectMemory(&mapped, bytes, ORBIS_KERNEL_PROT_CPU_RW,
        MAP_FIXED | NO_OVERWRITE, physical, PAGE_BYTES);
    if (result)
        return result;
    backing->address = mapped;
    if (mapped != requested)
        return (int32_t)ORBIS_KERNEL_ERROR_EFAULT;
    memset(mapped, 0, bytes);
    return 0;
}

static inline int32_t hzn_orbis_stack_backing_close(HznStackBacking *backing, size_t bytes)
{
    if (backing->address) {
        const int32_t result = sceKernelMunmap(backing->address, bytes);
        if (result)
            return result;
        backing->address = NULL;
    }
    if (backing->direct_owned) {
        const int32_t result = sceKernelReleaseDirectMemory((off_t)backing->physical, bytes);
        if (result)
            return result;
        backing->direct_owned = false;
    }
    return 0;
}
#endif

#endif
