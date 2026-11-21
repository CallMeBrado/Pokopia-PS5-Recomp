/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <orbis/libkernel.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

enum {
    PAGE_BYTES = 0x4000, SLICE_BYTES = 0x10000, BLOCK_BYTES = 3 * SLICE_BYTES,
    NO_OVERWRITE = 0x80, ORBIS_SIGBUS = 10,
    LEFT = 0x41, ORIGINAL = 0x62, RIGHT = 0x83,
    FILE_FIRST = 0x17, FILE_MIDDLE = ORIGINAL, PRIVATE = 0xa4, DIRECT_WRITE = 0xd5,
};
#define BASE_VA UINT64_C(0x7800400000)
#define MIDDLE_VA (BASE_VA + SLICE_BYTES)

/* OpenOrbis declares these output parameters by value. */
extern int32_t direct_available(off_t, off_t, size_t, off_t *, size_t *)
    __asm__("sceKernelAvailableDirectMemorySize");
extern int32_t flexible_available(size_t *) __asm__("sceKernelAvailableFlexibleMemorySize");

static uint8_t file_buffer[SLICE_BYTES];
static sigjmp_buf cow_return;
static volatile sig_atomic_t cow_active, cow_signal;

static void report(FILE *log, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(log, format, args);
    va_end(args);
    fputc('\n', log);
    fflush(log);
}

static uint8_t pattern(size_t index, uint8_t seed)
{
    return (uint8_t)(seed ^ index ^ (index >> 6) ^ (index >> 12));
}

static void fill(volatile uint8_t *address, uint8_t seed)
{
    for (size_t i = 0; i < SLICE_BYTES; ++i)
        address[i] = pattern(i, seed);
}

static bool verify_range(FILE *log, const char *label, const volatile uint8_t *address,
    size_t start, size_t bytes, uint8_t seed)
{
    for (size_t i = start; i < start + bytes; ++i) {
        const uint8_t actual = address[i], expected = pattern(i, seed);
        if (actual != expected) {
            report(log, "%s mismatch byte=%#zx actual=%#x expected=%#x", label, i, actual, expected);
            return false;
        }
    }
    report(log, "%s verified %zu bytes offset=%#zx seed=%#x", label, bytes, start, seed);
    return true;
}

static bool verify_bytes(FILE *log, const char *label, const volatile uint8_t *address, uint8_t seed)
{
    return verify_range(log, label, address, 0, SLICE_BYTES, seed);
}

static void census(FILE *log, const char *phase, off_t limit)
{
    off_t offset = -1;
    size_t span = 0, flex = 0;
    const int32_t direct_rc = direct_available(0, limit, PAGE_BYTES, &offset, &span);
    const int32_t flex_rc = flexible_available(&flex);
    report(log, "memory %s direct-capacity=%#" PRIx64 " direct-largest=%#zx at=%#" PRIx64
                " direct-rc=%#" PRIx32 " flexible-free=%#zx flexible-rc=%#" PRIx32,
        phase, (uint64_t)limit, span, (uint64_t)offset, (uint32_t)direct_rc, flex, (uint32_t)flex_rc);
}

static bool exact_span(FILE *log, const char *phase, off_t physical, bool expected_free)
{
    off_t offset = -1;
    size_t span = 0;
    const int32_t rc = direct_available(physical, physical + SLICE_BYTES, PAGE_BYTES, &offset, &span);
    report(log, "physical %s expected=%s start=%#" PRIx64 " rc=%#" PRIx32
                " reported=%#" PRIx64 " bytes=%#zx",
        phase, expected_free ? "free" : "occupied", (uint64_t)physical, (uint32_t)rc, (uint64_t)offset, span);
    return expected_free ? !rc && offset == physical && span == SLICE_BYTES :
        (uint32_t)rc == ORBIS_KERNEL_ERROR_ENOMEM;
}

static bool check_protection(FILE *log, const char *phase, void *address, int required, bool direct)
{
    void *start = NULL, *end = NULL;
    int32_t protection = 0;
    OrbisKernelVirtualQueryInfo region = {0};
    const int32_t rc = sceKernelQueryMemoryProtection(address, &start, &end, &protection);
    const int32_t vq = sceKernelVirtualQuery(address, 0, &region, sizeof region);
    report(log, "protection %s addr=%p rc=%#" PRIx32 " prot=%#x %p..%p vq=%#" PRIx32
                " direct=%u flexible=%u committed=%u offset=%#" PRIx64,
        phase, address, (uint32_t)rc, (unsigned)protection, start, end, (uint32_t)vq,
        region.isDirectMemory, region.isFlexibleMemory, region.isCommitted, (uint64_t)region.offset);
    const uintptr_t base = (uintptr_t)address, limit = base + SLICE_BYTES;
    return !rc && (protection & (PROT_READ | PROT_WRITE | PROT_EXEC)) == required &&
        (uintptr_t)start <= base && (uintptr_t)end >= limit &&
        !vq && (uintptr_t)region.unk01 <= base && (uintptr_t)region.unk02 >= limit &&
        region.isDirectMemory == direct;
}

static bool check_guards(FILE *log, const char *phase)
{
    uint8_t *const left = (void *)(uintptr_t)BASE_VA;
    uint8_t *const right = left + 2 * SLICE_BYTES;
    report(log, "guards %s", phase);
    const bool left_ok = check_protection(log, "left", left, PROT_READ | PROT_WRITE, true) &&
        verify_bytes(log, "left", left, LEFT);
    const bool right_ok = check_protection(log, "right", right, PROT_READ | PROT_WRITE, true) &&
        verify_bytes(log, "right", right, RIGHT);
    return left_ok && right_ok;
}

static bool check_hole(FILE *log)
{
    for (uintptr_t address = MIDDLE_VA; address < MIDDLE_VA + SLICE_BYTES; address += PAGE_BYTES) {
        void *start = NULL, *end = NULL;
        int32_t protection = 0;
        if (!sceKernelQueryMemoryProtection((void *)address, &start, &end, &protection)) {
            report(log, "FAIL middle VA page %#" PRIxPTR " is occupied", address);
            return false;
        }
    }
    return true;
}

static bool unmap(FILE *log, void *address, size_t bytes)
{
    const int32_t rc = sceKernelMunmap(address, bytes);
    report(log, "unmap %p bytes=%#zx rc=%#" PRIx32, address, bytes, (uint32_t)rc);
    return !rc;
}

static bool release(FILE *log, off_t offset, size_t bytes)
{
    const int32_t rc = sceKernelReleaseDirectMemory(offset, bytes);
    report(log, "release %#" PRIx64 " bytes=%#zx rc=%#" PRIx32, (uint64_t)offset, bytes, (uint32_t)rc);
    return !rc;
}

static bool file_transfer(int fd, uint8_t *data, size_t size, off_t offset, bool writing)
{
    size_t copied = 0;
    while (copied < size) {
        const off_t at = offset + (off_t)copied;
        const size_t bytes = size - copied;
        const ssize_t result = writing ? pwrite(fd, data + copied, bytes, at) : pread(fd, data + copied, bytes, at);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return false;
        copied += (size_t)result;
    }
    return true;
}

static bool map_file(FILE *log, void **mapping, int fd, int mode)
{
    void *const middle = (void *)(uintptr_t)MIDDLE_VA;
    if (!check_hole(log))
        return false;
    const int flags = mode | MAP_FIXED;
    const int32_t rc = sceKernelMmap(middle, SLICE_BYTES, PROT_READ, flags, fd, SLICE_BYTES, mapping);
    report(log, "fixed file map want=%p result=%p flags=%#x offset=%#x rc=%#" PRIx32,
        middle, *mapping, flags, SLICE_BYTES, (uint32_t)rc);
    if (rc) {
        *mapping = NULL;
        return false;
    }
    return *mapping == middle &&
        check_protection(log, "fixed file RO", middle, PROT_READ, false) &&
        verify_bytes(log, "fixed file RO", middle, FILE_MIDDLE);
}

static void cow_fault(int signal, siginfo_t *info, void *context)
{
    (void)context;
    if (cow_active && (uintptr_t)info >= PAGE_BYTES &&
        (uintptr_t)info->si_addr >= MIDDLE_VA && (uintptr_t)info->si_addr < MIDDLE_VA + SLICE_BYTES) {
        cow_signal = signal;
        siglongjmp(cow_return, 1);
    }
    _Exit(128 + signal);
}

static bool try_cow(FILE *log, uint8_t *middle, int fd, bool *kernel_ok)
{
    enum { HALF = SLICE_BYTES / 2 };
    const int32_t rc = sceKernelMprotect(middle, SLICE_BYTES, PROT_READ | PROT_WRITE);
    report(log, "private mprotect RW rc=%#" PRIx32, (uint32_t)rc);
    if (rc || !check_protection(log, "private file RW", middle, PROT_READ | PROT_WRITE, false))
        return false;

    struct sigaction action = { .sa_sigaction = cow_fault, .sa_flags = SA_SIGINFO };
    struct sigaction old_segv, old_bus;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, &old_segv))
        return false;
    if (sigaction(ORBIS_SIGBUS, &action, &old_bus)) {
        (void)sigaction(SIGSEGV, &old_segv, NULL);
        return false;
    }
    cow_signal = 0;
    if (sigsetjmp(cow_return, 1) == 0) {
        cow_active = 1;
        volatile uint8_t *destination = middle;
        for (size_t i = 0; i < HALF; ++i)
            destination[i] = pattern(i, PRIVATE);
    }
    cow_active = 0;
    const int restore_bus = sigaction(ORBIS_SIGBUS, &old_bus, NULL);
    const int restore_segv = sigaction(SIGSEGV, &old_segv, NULL);
    report(log, "private stores signal=%d restore-bus=%d restore-segv=%d",
        (int)cow_signal, restore_bus, restore_segv);
    if (cow_signal || restore_bus || restore_segv ||
        !verify_range(log, "private CPU stores", middle, 0, HALF, PRIVATE))
        return false;

    /* These two pages still share the vnode. Force COW through kernel copyout
     * independently of the preceding CPU stores. */
    const bool copied = file_transfer(fd, middle + HALF, HALF, HALF, false);
    report(log, "private pread bytes=%u offset=%#x success=%u errno=%d",
        HALF, HALF, (unsigned)copied, copied ? 0 : errno);
    const bool kernel_bytes = copied && verify_range(log, "private kernel stores", middle, HALF, HALF, FILE_FIRST);
    const int32_t ro = sceKernelMprotect(middle, SLICE_BYTES, PROT_READ);
    report(log, "private mprotect RO rc=%#" PRIx32, (uint32_t)ro);
    const bool protected = !ro && check_protection(log, "private COW RO", middle, PROT_READ, false);
    *kernel_ok = protected && kernel_bytes &&
        verify_range(log, "private kernel stores reprotected", middle, HALF, HALF, FILE_FIRST);
    return protected && verify_range(log, "private CPU stores reprotected", middle, 0, HALF, PRIVATE);
}

static void run(FILE *log, int fd)
{
    const size_t capacity = sceKernelGetDirectMemorySize();
    if (capacity < BLOCK_BYTES || capacity > INT64_MAX) {
        report(log, "FAIL invalid direct memory capacity=%#zx", capacity);
        return;
    }
    const off_t limit = (off_t)capacity;
    uint8_t *const base = (void *)(uintptr_t)BASE_VA;
    uint8_t *const middle = base + SLICE_BYTES;
    off_t physical = -1, reacquired = -1;
    void *original = NULL, *view = NULL;
    bool block_owned = false, hole = false, split = false, middle_owned = false;
    bool shared_ok = false, private_ok = false, cow_ok = false, cow_kernel_ok = false, remat_ok = false;

    census(log, "before allocate", limit);
    int32_t rc = sceKernelAllocateDirectMemory(0, limit, BLOCK_BYTES, SLICE_BYTES,
        ORBIS_KERNEL_WB_ONION, &physical);
    report(log, "allocate direct bytes=%#x rc=%#" PRIx32 " physical=%#" PRIx64,
        BLOCK_BYTES, (uint32_t)rc, (uint64_t)physical);
    if (rc || physical < 0 || physical > INT64_MAX - BLOCK_BYTES || physical % SLICE_BYTES)
        goto cleanup;
    block_owned = true;
    if (physical > limit - BLOCK_BYTES) {
        report(log, "FAIL foreground direct coordinates do not match");
        goto cleanup;
    }
    const off_t mid_physical = physical + SLICE_BYTES;
    const off_t right_physical = mid_physical + SLICE_BYTES;
    original = base;
    rc = sceKernelMapDirectMemory(&original, BLOCK_BYTES, ORBIS_KERNEL_PROT_CPU_RW,
        MAP_FIXED | NO_OVERWRITE, physical, SLICE_BYTES);
    report(log, "map original want=%p actual=%p rc=%#" PRIx32, (void *)base, original, (uint32_t)rc);
    if (rc)
        original = NULL;
    if (original != base || !check_protection(log, "original middle", middle, PROT_READ | PROT_WRITE, true))
        goto cleanup;
    fill(base, LEFT);
    fill(middle, ORIGINAL);
    fill(base + 2 * SLICE_BYTES, RIGHT);
    if (!check_guards(log, "original") ||
        !exact_span(log, "original middle", mid_physical, false) ||
        !unmap(log, middle, SLICE_BYTES))
        goto cleanup;
    hole = true;
    if (!check_hole(log) || !check_guards(log, "after middle unmap") || !release(log, mid_physical, SLICE_BYTES))
        goto cleanup;
    split = true;
    census(log, "middle released", limit);
    if (!check_guards(log, "after middle release") || !exact_span(log, "released middle", mid_physical, true) ||
        !exact_span(log, "retained left", physical, false) ||
        !exact_span(log, "retained right", right_physical, false))
        goto cleanup;

    shared_ok = map_file(log, &view, fd, MAP_SHARED);
    if (shared_ok) {
        census(log, "shared fixed file touched", limit);
        shared_ok = check_guards(log, "with shared file") &&
            exact_span(log, "shared file middle", mid_physical, true);
    }
    if (view && !unmap(log, view, SLICE_BYTES))
        goto cleanup;
    view = NULL;

    private_ok = map_file(log, &view, fd, MAP_PRIVATE);
    if (private_ok) {
        census(log, "private fixed file touched", limit);
        private_ok = check_guards(log, "with private file") &&
            exact_span(log, "private file middle", mid_physical, true);
    }
    if (private_ok) {
        bool kernel_wrote = false;
        const bool stored = try_cow(log, middle, fd, &kernel_wrote);
        census(log, "after attempted private writes", limit);
        const bool guards = check_guards(log, "after private write attempt");
        const bool source_unchanged = file_transfer(fd, file_buffer, SLICE_BYTES, 0, false) &&
            verify_bytes(log, "first file slice after private stores", file_buffer, FILE_FIRST);
        const bool backing_unchanged = file_transfer(fd, file_buffer, SLICE_BYTES, SLICE_BYTES, false) &&
            verify_bytes(log, "mapped file slice after private stores", file_buffer, FILE_MIDDLE);
        const bool file_unchanged = source_unchanged && backing_unchanged;
        const bool still_free = exact_span(log, "after COW attempt", mid_physical, true);
        cow_ok = stored && guards && file_unchanged && still_free;
        cow_kernel_ok = kernel_wrote && guards && file_unchanged && still_free;
        if (!guards || !file_unchanged)
            goto cleanup;
    }
    if (view && !unmap(log, view, SLICE_BYTES))
        goto cleanup;
    view = NULL;
    if ((!shared_ok && !private_ok) || !check_hole(log) || !check_guards(log, "after file unmap"))
        goto cleanup;

    rc = sceKernelAllocateDirectMemory(mid_physical, right_physical, SLICE_BYTES, SLICE_BYTES,
        ORBIS_KERNEL_WB_ONION, &reacquired);
    report(log, "reacquire middle want=%#" PRIx64 " result=%#" PRIx64 " rc=%#" PRIx32,
        (uint64_t)mid_physical, (uint64_t)reacquired, (uint32_t)rc);
    if (rc || reacquired != mid_physical) {
        if (!rc && reacquired >= 0 && reacquired <= INT64_MAX - SLICE_BYTES &&
            (reacquired + SLICE_BYTES <= physical || reacquired >= physical + BLOCK_BYTES))
            (void)release(log, reacquired, SLICE_BYTES);
        goto cleanup;
    }
    middle_owned = true;
    view = middle;
    rc = sceKernelMapDirectMemory(&view, SLICE_BYTES, ORBIS_KERNEL_PROT_CPU_RW,
        MAP_FIXED | NO_OVERWRITE, reacquired, SLICE_BYTES);
    report(log, "direct rematerialize want=%p actual=%p rc=%#" PRIx32, (void *)middle, view, (uint32_t)rc);
    if (rc)
        view = NULL;
    if (view != middle || !check_protection(log, "direct restored", middle, PROT_READ | PROT_WRITE, true) ||
        !file_transfer(fd, middle, SLICE_BYTES, SLICE_BYTES, false) ||
        !verify_bytes(log, "direct restored from file", middle, FILE_MIDDLE))
        goto cleanup;
    fill(middle, DIRECT_WRITE);
    remat_ok = verify_bytes(log, "direct restored writes", middle, DIRECT_WRITE) &&
        check_guards(log, "after direct restore") &&
        exact_span(log, "reacquired direct middle", mid_physical, false);
    census(log, "direct rematerialized", limit);

cleanup:;
    const bool view_clean = !view || unmap(log, view, SLICE_BYTES);
    const bool middle_clean = !middle_owned || (view_clean && release(log, reacquired, SLICE_BYTES));
    bool left_clean = true, right_clean = true;
    if (original && hole) {
        left_clean = unmap(log, base, SLICE_BYTES);
        right_clean = unmap(log, base + 2 * SLICE_BYTES, SLICE_BYTES);
    } else if (original) {
        left_clean = right_clean = unmap(log, original, BLOCK_BYTES);
    }
    if (split) {
        left_clean = left_clean && release(log, physical, SLICE_BYTES);
        right_clean = right_clean && release(log, physical + 2 * SLICE_BYTES, SLICE_BYTES);
    } else if (block_owned) {
        left_clean = left_clean && right_clean && release(log, physical, BLOCK_BYTES);
    }
    census(log, "after cleanup", limit);
    report(log, "RESULT fixed-shared=%s fixed-private=%s CPU-COW=%s kernel-COW=%s direct-rematerialization=%s cleanup=%s",
        shared_ok ? "PASS" : "FAIL", private_ok ? "PASS" : "FAIL", cow_ok ? "PASS" : "UNPROVEN",
        cow_kernel_ok ? "PASS" : "UNPROVEN",
        remat_ok ? "PASS" : "FAIL", view_clean && middle_clean && left_clean && right_clean ? "PASS" : "FAIL");
}

int main(void)
{
    FILE *log = fopen("/data/pokopia-ps4-memory-proof-r4.txt", "w");
    if (log) {
        char path[100];
        snprintf(path, sizeof path, "/data/pokopia-ps4-memory-proof-r4-%d.tmp", (int)getpid());
        report(log, "Pokopia memory proof R4 foreground PID %d", (int)getpid());
        int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) {
            fill(file_buffer, FILE_FIRST);
            const bool first = file_transfer(fd, file_buffer, SLICE_BYTES, 0, true);
            fill(file_buffer, FILE_MIDDLE);
            const bool prepared = first && file_transfer(fd, file_buffer, SLICE_BYTES, SLICE_BYTES, true) && !fsync(fd);
            const int closed = close(fd);
            fd = prepared && !closed ? open(path, O_RDONLY) : -1;
            if (fd >= 0) {
                run(log, fd);
                if (close(fd))
                    report(log, "FAIL close fixture errno=%d", errno);
            } else {
                report(log, "FAIL could not reopen prepared fixture errno=%d", errno);
            }
            if (unlink(path))
                report(log, "FAIL remove fixture errno=%d", errno);
        } else {
            report(log, "FAIL could not create fixture errno=%d", errno);
        }
        fclose(log);
    }
    for (;;)
        sceKernelUsleep(1000000);
}
