/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <nxrecomp/ctx.h>

#include <setjmp.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

extern void f_7700000000(Ctx *);
extern void f_2000000000(Ctx *);

static const unsigned char rtld_image[] = {
#include "generated/rtld_image.inc"
};

static const struct {
    uint32_t offset;
    uint64_t addend;
} rtld_relr[] = {
#include "generated/rtld_relr.inc"
};

enum { RTLD_MAP_SIZE = 0x20000, SVC_QUERY_MEMORY = 6 };
static jmp_buf boundary;
static uintptr_t guest_base;
static const char *stop_reason;
static uint64_t stop_detail;

static void *map_rtld(uintptr_t base)
{
    void *wanted = (void *)base;
#ifdef _WIN32
    return VirtualAlloc(wanted, RTLD_MAP_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    /* Orbis mmap uses FreeBSD MAP_ANON, 0x1000. OpenOrbis's musl header
     * defines the Linux value. Use a hint so we cannot replace a mapping. */
    void *mapped = mmap(wanted, RTLD_MAP_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | 0x1000, -1, 0);
    if (mapped == MAP_FAILED)
        return NULL;
    if (mapped != wanted) {
        munmap(mapped, RTLD_MAP_SIZE);
        return NULL;
    }
    return mapped;
#endif
}

static void unmap_rtld(void *mapping)
{
#ifdef _WIN32
    VirtualFree(mapping, 0, MEM_RELEASE);
#else
    munmap(mapping, RTLD_MAP_SIZE);
#endif
}

static void stop_rtld(const char *reason, uint64_t detail, int status)
{
    stop_reason = reason;
    stop_detail = detail;
    longjmp(boundary, status);
}

void guest_svc(Ctx *cpu, uint32_t number)
{
    if (number == SVC_QUERY_MEMORY && cpu->x[30] == guest_base + 0x5cc) {
        /* At this call the guest has cleared its BSS, relocated RTLD and
         * started its Horizon memory scan. Stop before answering the SVC. */
        for (size_t i = 0; i < sizeof rtld_relr / sizeof rtld_relr[0]; ++i) {
            uintptr_t slot = guest_base + rtld_relr[i].offset;
            if (rd64(slot) != guest_base + rtld_relr[i].addend)
                stop_rtld("incorrect RTLD self-relocation", slot, 2);
        }
        stop_rtld("first Horizon QueryMemory", cpu->x[30], 1);
    }
    stop_rtld("unexpected SVC", number, 2);
}

void call_indirect(Ctx *cpu, uint64_t address)
{
    (void)cpu;
    stop_rtld("unexpected indirect call", address, 2);
}

void host_import(Ctx *cpu, uint32_t import_id)
{
    (void)cpu;
    stop_rtld("unexpected module import", import_id, 2);
}

void guest_trap(const char *what, uint64_t address)
{
    stop_rtld(what, address, 2);
}

static int call_rtld(void (*entry)(Ctx *))
{
    Ctx cpu = {0};
    _Alignas(16) unsigned char stack[0x2000];
    cpu.x[31] = (uintptr_t)(stack + sizeof stack);
    cpu.x[1] = 1;
    cpu.ex_addr = UINT64_MAX;
    switch (setjmp(boundary)) {
    case 0:
        entry(&cpu);
        stop_rtld("RTLD entry returned", cpu.fn, 2);
        return 2;
    case 1:
        return 1;
    default:
        return 2;
    }
}

static int try_rtld(FILE *log, uintptr_t base, void (*entry)(Ctx *))
{
    void *mapping = map_rtld(base);
    if (!mapping) {
        fprintf(log, "RTLD address 0x%llx unavailable\n", (unsigned long long)base);
        return -1;
    }
    memcpy(mapping, rtld_image, sizeof rtld_image);
    guest_base = base;
    fprintf(log, "Entering translated rtld at guest base 0x%llx\n", (unsigned long long)base);
    fflush(log);
    int status = call_rtld(entry);
    unmap_rtld(mapping);
    fprintf(log, "%s rtld: %s (0x%llx)\n", status == 1 ? "PASS" : "FAIL",
            stop_reason, (unsigned long long)stop_detail);
    return status == 1 ? 0 : 1;
}

int pokopia_rtld_boot_probe(FILE *log)
{
    int result = try_rtld(log, 0x7700000000, f_7700000000);
    if (result != -1)
        return result;
    result = try_rtld(log, 0x2000000000, f_2000000000);
    if (result == -1) {
        fprintf(log, "FAIL rtld: neither build-time address could be mapped\n");
        return 1;
    }
    return result;
}

