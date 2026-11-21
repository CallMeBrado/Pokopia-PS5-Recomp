/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "boot.h"
#include "../../horizon/get_info.h"
#include "../../horizon/heap.h"
#ifdef POKOPIA_SPARSE_HEAP
#include "../../horizon/heap_sparse.h"
#endif

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <orbis/libkernel.h>
#ifdef POKOPIA_GFX
#include "../../gfx-bridge/orbis_memory.h"
#endif
#endif

extern const NxModule nxmod_sdk;
extern const uint32_t nxmod_sdk_abi;
#ifdef POKOPIA_BOOT_GAME
extern const NxModule nxmod_main, nxmod_subsdk0;
extern const uint32_t nxmod_main_abi, nxmod_subsdk0_abi;
#endif

typedef struct {
    const NxModule *module;
    const uint32_t *abi;
} BootModule;

static const BootModule modules[] = {
#ifdef POKOPIA_BOOT_GAME
    { &nxmod_main, &nxmod_main_abi },
#endif
    { &nxmod_sdk, &nxmod_sdk_abi },
#ifdef POKOPIA_BOOT_GAME
    { &nxmod_subsdk0, &nxmod_subsdk0_abi },
#endif
};
typedef struct {
    const DynSym *symbol;
    size_t order;
} BootExport;

static BootExport *exports;
static size_t export_count;
static HznGetInfoProcess guest;
static HznHeap guest_heap;
static const uint64_t guest_tick_hz = 31250000;
static uint64_t host_tick_hz;

#ifdef POKOPIA_GFX
static uint32_t render_height = POKOPIA_RENDER_NATIVE_HEIGHT;

static uint32_t parse_render_height(const char *setting)
{
    const char whitespace[] = " \t\r\n";
    setting += strspn(setting, whitespace);
    const size_t size = strcspn(setting, whitespace);
    if (setting[size + strspn(setting + size, whitespace)])
        return 0;
    if (size == 3 && memcmp(setting, "720", 3) == 0)
        return 720;
    if (size == 3 && memcmp(setting, "900", 3) == 0)
        return 900;
    if (size == 4 && memcmp(setting, "1080", 4) == 0)
        return POKOPIA_RENDER_NATIVE_HEIGHT;
    return 0;
}

static void configure_render_height(FILE *log)
{
    const char *setting = NULL;
    char input[32];
#ifdef _WIN32
    DWORD length = GetEnvironmentVariableA("POKOPIA_RENDER_RESOLUTION", input, sizeof input);
    if (length)
        setting = length < sizeof input ? input : "";
#else
    FILE *file = fopen("/data/pokopia/render-resolution.txt", "rb");
    if (file) {
        size_t bytes = fread(input, 1, sizeof input - 1, file);
        bool complete = feof(file) && !ferror(file) && !memchr(input, 0, bytes);
        fclose(file);
        input[bytes] = 0;
        setting = complete ? input : "";
    }
#endif
    uint32_t height = setting ? parse_render_height(setting) : POKOPIA_RENDER_NATIVE_HEIGHT;
    if (!height) {
        fputs("Invalid render resolution setting. Using 1080.\n", log);
        height = POKOPIA_RENDER_NATIVE_HEIGHT;
    }
    render_height = height;
    fprintf(log, "Pokopia internal render extent %ux%u\n", height * 16 / 9, height);
}

bool pokopia_boot_render_override(uint32_t *width, uint32_t *height)
{
    *height = render_height;
    *width = render_height * 16 / 9;
    return render_height != POKOPIA_RENDER_NATIVE_HEIGHT;
}
#endif

static int compare_exports(const void *a, const void *b)
{
    const BootExport *left = a, *right = b;
    const int name = strcmp(left->symbol->name, right->symbol->name);
    if (name)
        return name;
    return (left->order > right->order) - (left->order < right->order);
}

static bool index_exports(void)
{
    size_t capacity = 0;
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; ++i) {
        size_t count = (size_t)modules[i].module->nsyms;
        if (count > SIZE_MAX / sizeof *exports - capacity)
            return false;
        capacity += count;
    }
    BootExport *index = malloc(capacity * sizeof *index);
    if (!index)
        return false;
    size_t count = 0;
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; ++i) {
        const NxModule *module = modules[i].module;
        for (int j = 0; j < module->nsyms; ++j) {
            const DynSym *symbol = &module->syms[j];
            if (symbol->defined && symbol->value) {
                index[count] = (BootExport){symbol, count};
                ++count;
            }
        }
    }
    /* Preserve the original module and symbol order for duplicate names. */
    qsort(index, count, sizeof *index, compare_exports);
    exports = index;
    export_count = count;
    return true;
}

bool pokopia_boot_init(FILE *log, uint64_t mapped_bytes)
{
    uint64_t seed;
    const HznGetInfoLayout layout = {
        .alias = { UINT64_C(0x6000000000), UINT64_C(0x200000000) },
        .heap = { POKOPIA_BOOT_HEAP_BASE, UINT64_C(0x300000000) },
        .aslr = { UINT64_C(0x8000000), UINT64_C(0x7ff8000000) },
        .stack = { POKOPIA_BOOT_STACK_REGION, POKOPIA_BOOT_STACK_REGION_SIZE },
        .remote = { UINT64_C(0x7200000000), UINT64_C(0x100000000) },
        .address_space_size = UINT64_C(1) << 39,
        .page_size = 0x1000,
        .core_mask = POKOPIA_CORE_MASK,
        .priority_mask = POKOPIA_PRIORITY_MASK,
        .system_resource_size = POKOPIA_SYSTEM_RESOURCE_SIZE,
    };
    if (!hzn_get_info_entropy_seed(&seed)) {
        fputs("FAIL could not acquire an OS entropy seed\n", log);
        return false;
    }
    if (!hzn_get_info_init(&guest, &layout, seed)) {
        fputs("FAIL invalid Horizon virtual address layout\n", log);
        return false;
    }

#ifdef _WIN32
    /* Keep the desktop boot test within the direct memory capacity reported by the PS4 probe. */
    uint64_t guest_budget = UINT64_C(4831838208);
#else
    uint64_t guest_budget = sceKernelGetDirectMemorySize();
#endif
#ifdef POKOPIA_GFX
#if defined(__PROSPERO__)
    /* PS5: 12288 MiB direct, ~11 GiB mappable (phase-0 probe). Give RADV and
     * the host copies of guest textures 4 GiB and the guest 8 GiB, the size
     * the probe mapped at the heap base. Stock (unpatched) game arenas. */
    const uint64_t graphics_reserve = UINT64_C(4096) * 1024 * 1024;
#else
    /* With 4608 MiB direct, expose 3306 MiB to Horizon. The fixed startup
     * arenas pass at 3290 MiB on the desktop profile, leaving 16 MiB here. */
    const uint64_t graphics_reserve = UINT64_C(1302) * 1024 * 1024;
#endif
    if (guest_budget <= graphics_reserve)
        return false;
    guest_budget -= graphics_reserve;
    fprintf(log, "Graphics startup reserved %" PRIu64 " MiB outside the guest budget\n",
            graphics_reserve / (1024 * 1024));
#if defined(POKOPIA_SPARSE_HEAP) && !defined(_WIN32)
    /* The guest reserves its texture arena before any texture uses it.
     * The sparse backend allocates direct memory only when pages are touched. */
    const uint64_t texture_virtual_allowance = UINT64_C(1918) * 1024 * 1024;
    guest_budget += texture_virtual_allowance;
    fprintf(log, "Sparse guest texture reservation added %" PRIu64 " MiB of virtual space\n",
            texture_virtual_allowance / (1024 * 1024));
#endif
#ifdef _WIN32
    char desktop_memory[16];
    DWORD length = GetEnvironmentVariableA("POKOPIA_DESKTOP_MEMORY_MIB", desktop_memory, sizeof desktop_memory);
    if (length) {
        char *end;
        if (length >= sizeof desktop_memory)
            return false;
        uint64_t mib = strtoull(desktop_memory, &end, 10);
        MEMORYSTATUSEX host_memory = { .dwLength = sizeof host_memory };
        if (*end || mib < 2048 || mib > 12288 || !GlobalMemoryStatusEx(&host_memory) ||
            host_memory.ullAvailPageFile < (mib + 1024) * 1024 * 1024) {
            fputs("FAIL desktop diagnostic memory budget is invalid or unavailable\n", log);
            return false;
        }
        guest_budget = mib * 1024 * 1024;
        fprintf(log, "Desktop-only graphics diagnostic budget %" PRIu64 " MiB\n", mib);
    }
#endif
#endif
    if (!hzn_get_info_memory(&guest, guest_budget, mapped_bytes) ||
        !hzn_heap_init(&guest_heap, POKOPIA_BOOT_HEAP_BASE, guest_budget - mapped_bytes)) {
        fputs("FAIL guest mappings exceed the measured memory budget\n", log);
        return false;
    }

#ifdef _WIN32
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency))
        return false;
    host_tick_hz = (uint64_t)frequency.QuadPart;
#else
    host_tick_hz = sceKernelGetProcessTimeCounterFrequency();
#endif
    if (!host_tick_hz) {
        fputs("FAIL host monotonic clock frequency is unavailable\n", log);
        return false;
    }

    uint64_t last = 0;
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; ++i) {
        const NxModule *module = modules[i].module;
        if (*modules[i].abi != NXRECOMP_ABI_VERSION || module->base < last ||
            module->text_end <= module->base || module->nfns <= 0 || module->nsyms <= 0) {
            fprintf(log, "FAIL invalid translated module descriptor: %s\n", module->name);
            return false;
        }
        last = module->text_end;
        fprintf(log, "Registered %s ABI %u with %d translated functions\n",
                module->name, *modules[i].abi, module->nfns);
    }
    if (index_exports())
        fprintf(log, "Indexed %zu translated exports\n", export_count);
#ifdef POKOPIA_GFX
    configure_render_height(log);
#endif
    return true;
}

bool pokopia_boot_check_heap(FILE *log)
{
    HznHeap heap;
    if (!hzn_heap_init(&heap, POKOPIA_BOOT_HEAP_BASE, HZN_HEAP_GRANULE))
        return false;

    uint64_t address;
    uint32_t result = hzn_heap_set_size(&heap, HZN_HEAP_GRANULE, &address);
    bool verified = false;
    if (result == HZN_HEAP_OK) {
        volatile uint8_t *pages = (volatile uint8_t *)(uintptr_t)address;
        bool zeroed = pages[0] == 0 && pages[HZN_HEAP_GRANULE - 1] == 0;
        pages[0] = 0x53;
        pages[HZN_HEAP_GRANULE - 1] = 0xb6;
        verified = zeroed && pages[0] == 0x53 && pages[HZN_HEAP_GRANULE - 1] == 0xb6;
    } else {
        fprintf(log, "FAIL guest heap map result=%#" PRIx32 " host=%#" PRIx32 "\n",
                result, (uint32_t)heap.last_host_error);
    }

    if (!hzn_heap_release(&heap)) {
        fprintf(log, "FAIL guest heap release host=%#" PRIx32 "\n", (uint32_t)heap.last_host_error);
        return false;
    }
    if (result == HZN_HEAP_OK)
        fprintf(log, "%s 2 MiB guest heap map, write, and release at %#" PRIx64 "\n",
                verified ? "PASS" : "FAIL", address);
    return verified;
}

uint64_t pokopia_boot_heap_size(void)
{
    return guest_heap.size;
}

#ifdef POKOPIA_SPARSE_HEAP
void pokopia_boot_report_heap(FILE *log, const char *phase)
{
    hzn_heap_sparse_report(log, phase);
}
#endif

bool pokopia_boot_release_heap(FILE *log)
{
    free(exports);
    exports = NULL;
    export_count = 0;
    if (hzn_heap_release(&guest_heap))
        return true;
    fprintf(log, "FAIL guest heap release host=%#" PRIx32 "\n", (uint32_t)guest_heap.last_host_error);
    return false;
}

static uint64_t system_tick(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter))
        guest_trap("host monotonic clock read failed", 0);
    uint64_t host_counter = (uint64_t)counter.QuadPart;
#else
    uint64_t host_counter = sceKernelGetProcessTimeCounter();
#endif
    return (host_counter / host_tick_hz) * guest_tick_hz +
           (host_counter % host_tick_hz) * guest_tick_hz / host_tick_hz;
}

uint64_t pokopia_boot_monotonic_ns(void)
{
    return system_tick() * (1000000000 / guest_tick_hz);
}

bool pokopia_boot_sample_clocks(uint64_t *tick_ns, int64_t *utc_ns)
{
    struct timespec now;
#ifdef _WIN32
    if (timespec_get(&now, TIME_UTC) != TIME_UTC)
        return false;
#else
    if (clock_gettime(CLOCK_REALTIME, &now))
        return false;
#endif
    if (now.tv_sec < 0 || (uint64_t)now.tv_sec > (INT64_MAX - UINT64_C(999999999)) / 1000000000)
        return false;
    *utc_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    *tick_ns = pokopia_boot_monotonic_ns();
    return true;
}

bool pokopia_boot_svc(Ctx *cpu, uint32_t number, FILE *log)
{
    switch (number) {
    case 0x01: { /* SetHeapSize */
        uint64_t size = cpu->x[1], address, previous_size = guest_heap.size;
#if defined(POKOPIA_GFX) && !defined(_WIN32)
        pokopia_orbis_memory_census(log, "before guest heap");
#endif
        uint32_t result = hzn_heap_set_size(&guest_heap, size, &address);
        if (!result)
            guest.used_memory = guest.used_memory - previous_size + guest_heap.size;
        fprintf(log, "SetHeapSize size=%#" PRIx64 " base=%#" PRIx64 " result=%#" PRIx32
                " host=%#" PRIx32 "\n", size, address, result, (uint32_t)guest_heap.last_host_error);
        fflush(log);
#ifdef POKOPIA_SPARSE_HEAP
        if (!result && guest_heap.size != previous_size)
            pokopia_boot_report_heap(log, "after guest heap");
#endif
#if defined(POKOPIA_GFX) && !defined(_WIN32)
        pokopia_orbis_memory_census(log, "after guest heap");
#endif
        cpu->x[0] = result;
        cpu->x[1] = address;
        return true;
    }
    case 0x1e: /* GetSystemTick */
        cpu->x[0] = system_tick();
        return true;
    case 0x29: { /* GetInfo */
        uint32_t type = (uint32_t)cpu->x[1];
        uint32_t handle = (uint32_t)cpu->x[2];
        uint64_t subtype = cpu->x[3];
        if (!hzn_get_info(&guest, cpu->x)) {
            fprintf(log, "Unsupported GetInfo type=%u handle=%#" PRIx32 " subtype=%#" PRIx64 "\n",
                    type, handle, subtype);
            return false;
        }
        fprintf(log, "GetInfo type=%u handle=%#" PRIx32 " subtype=%#" PRIx64 " result=%#" PRIx64,
                type, handle, subtype, cpu->x[0]);
        if (type != 11)
            fprintf(log, " value=%#" PRIx64, cpu->x[1]);
        fputc('\n', log);
        fflush(log);
        return true;
    }
    default:
        return false;
    }
}

PokopiaBootEntry pokopia_boot_lookup(uint64_t address)
{
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; ++i) {
        const NxModule *module = modules[i].module;
        if (address < module->base || address >= module->text_end)
            continue;
        int low = 0, high = module->nfns;
        while (low < high) {
            int mid = low + (high - low) / 2;
            const FnEntry *entry = &module->fns[mid];
            if (entry->addr == address)
                return entry->fn;
            if (entry->addr < address)
                low = mid + 1;
            else
                high = mid;
        }
        return NULL;
    }
    return NULL;
}

bool pokopia_boot_dispatch(Ctx *cpu, uint64_t address)
{
    PokopiaBootEntry entry = pokopia_boot_lookup(address);
    if (!entry)
        return false;
    entry(cpu);
    return true;
}

uint64_t pokopia_boot_import(uint32_t id, const char **name)
{
    *name = NULL;
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; ++i) {
        const NxModule *module = modules[i].module;
        if (module->index != NX_IMPORT_MODULE(id) || NX_IMPORT_SYM(id) >= (uint32_t)module->nsyms)
            continue;
        const DynSym *import = &module->syms[NX_IMPORT_SYM(id)];
        *name = import->name;
        if (import->defined)
            return import->value;
        break;
    }
    if (!*name)
        return 0;
    if (exports) {
        size_t low = 0, high = export_count;
        while (low < high) {
            const size_t mid = low + (high - low) / 2;
            if (strcmp(exports[mid].symbol->name, *name) < 0)
                low = mid + 1;
            else
                high = mid;
        }
        if (low < export_count && strcmp(exports[low].symbol->name, *name) == 0)
            return exports[low].symbol->value;
        return 0;
    }
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; ++i) {
        const NxModule *module = modules[i].module;
        for (int symbol = 0; symbol < module->nsyms; ++symbol) {
            const DynSym *export = &module->syms[symbol];
            if (export->defined && export->value && strcmp(export->name, *name) == 0)
                return export->value;
        }
    }
    return 0;
}

uint64_t guest_mrs(Ctx *cpu, uint32_t reg)
{
    (void)cpu;
    switch (reg) {
    case 0xdf00:
        return guest_tick_hz;
    case 0xdf01:
    case 0xdf02:
        return system_tick();
    default:
        guest_trap("unsupported system register read", reg);
        return 0;
    }
}

void guest_msr(Ctx *cpu, uint32_t reg, uint64_t value)
{
    (void)cpu;
    (void)value;
    guest_trap("unsupported system register write", reg);
}

void guest_unimpl(uint64_t pc, uint32_t insn, const char *what)
{
    (void)insn;
    guest_trap(what, pc);
}

void stx_diag(Ctx *cpu, uint64_t address, uint64_t expected, uint64_t seen, uint64_t value, int bits)
{
    (void)address;
    (void)expected;
    (void)seen;
    (void)value;
    (void)bits;
    ++cpu->ex_fail;
}

void nx_catch_call(Ctx *cpu, void (*body)(Ctx *, uint64_t))
{
    /* Begin the protected body at its normal entry. Guest exception
     * unwinding is a separate runtime path. */
    body(cpu, 0);
}

#ifndef POKOPIA_GFX
int nx_site_midhook(Ctx *cpu, uint64_t address) { (void)cpu; (void)address; return 0; }
#endif

