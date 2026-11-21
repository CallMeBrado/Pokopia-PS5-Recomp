/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <nxrecomp/ctx.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu-probe/generated/lift-7700000000/funcs.h"
#ifdef POKOPIA_BOOT_PROBE
#include "../boot/boot.h"
#include "../boot/host.h"
#include "../horizon/sm.h"
#include "../horizon/lm.h"
#include "../horizon/cmif.h"
#include "../horizon/applet.h"
#include "../horizon/aoc.h"
#include "../horizon/pctl.h"
#include "../horizon/time_service.h"
#ifdef POKOPIA_HWOPUS
#include "../horizon/hwopus.h"
#endif
#include "../horizon/readonly_page.h"
#include "../horizon/live_shared.h"
#include "../horizon/get_info.h"
#include "../horizon/event.h"
#include "../horizon/stack_map.h"
#include "../horizon/heap_stack_alias.h"
#include "../horizon/key_sync.h"
#include "../horizon/thread.h"
#include "../horizon/transfer_memory.h"
#endif
#ifdef POKOPIA_GFX
#include "../horizon/account_identity.h"
#include "../horizon/bsd_ssl.h"
#include "../horizon/hid.h"
#include "../horizon/offline_app.h"
#include "../horizon/save_store.h"
#include "../horizon/stpl.h"
#include "../gfx-bridge/account.h"
#include "../gfx-bridge/fsp.h"
#include "../gfx-bridge/live_compute.h"
#include "../gfx-bridge/live_texture_upload.h"
#ifdef POKOPIA_AUDIO_OUT
#include "../horizon/audio_device.h"
#include "../horizon/audio_out.h"
#endif
#include "../gfx-bridge/nvn_front.h"
#include "../gfx-bridge/nvn_guest_layout.h"
#include "../gfx-bridge/nvn_replay.h"
#include "../gfx-bridge/nvn_window.h"
#ifndef _WIN32
#include "../gfx-bridge/live_present.h"
#include "../gfx-bridge/orbis_memory.h"
#endif
#include "../gfx-bridge/vi_adapter.h"
#include "../gfx-bridge/vk_adapter.h"
#include "../gfx-bridge/vk_ledger.h"
#include "../gfx-bridge/vk_program.h"
#include "../gfx-bridge/sdk-graphics.inc"
#include "../horizon/stack_source.h"
#endif

#ifndef POKOPIA_LOADER_ASSET_DIR
#ifdef _WIN32
#define POKOPIA_LOADER_ASSET_DIR "generated"
#else
#define POKOPIA_LOADER_ASSET_DIR "/app0/modules"
#endif
#endif

typedef struct {
    uint64_t start, end;
    uint32_t state, permission;
} Segment;

typedef struct {
    const char *name;
    uint64_t base;
    size_t image_size, memory_size;
    Segment segments[3];
} Module;

typedef struct {
    uint64_t base, size;
    uint32_t type, attribute, permission, ipc_refcount, device_refcount, padding;
} MemoryInfo;

_Static_assert(sizeof(MemoryInfo) == 0x28, "Horizon MemoryInfo ABI");
static const Module modules[] = {
#include "generated/layout.inc"
};

static const struct {
    uint64_t address;
    void (*function)(Ctx *);
} functions[] = {
#include "generated/rtld_functions.inc"
};

static const uint64_t sdk_start =
#include "generated/sdk_start.inc"
;
static const uint64_t guest_limit = 1ULL << 39;
static const uint64_t rtld_base = 0x7700000000ULL;
#ifndef POKOPIA_BOOT_PROBE
static jmp_buf boundary;
#endif
static Ctx cpu;
static FILE *probe_log;
static unsigned query_count;

#ifdef POKOPIA_SDK_PROBE
extern void f_7500118e90(Ctx *);
#endif
#if defined(POKOPIA_SDK_PROBE) || defined(POKOPIA_BOOT_PROBE)
static const uint64_t sdk_base = 0x7500000000;
#endif
#ifdef POKOPIA_BOOT_PROBE
typedef struct {
    jmp_buf jump;
    Ctx *context;
    bool kernel_locked;
#ifdef POKOPIA_GFX
    uint64_t bypass_entry;
    bool nvn_memory_callback;
#endif
} GuestBoundary;

static HznStackMap stack_map;
static HznHeapStackAliases heap_aliases;
static HznThreads *threads;
static HznKeySync *key_sync;
static const uint64_t exception_context_address = UINT64_C(0x7e02000000);
static void *exception_context;
static HznEvents *events;
static HznTransferMemoryTable transfer_memory;
static bool worker_stack_seeded, worker_started, worker_waiting, worker_failed, applet_focus_pending;
static uint32_t gpu_error_event;
static uint64_t system_worker_id;
/* Audio startup creates another worker while the first 32 are live. */
static bool worker_tls_used[128];
_Static_assert(HZN_HEAP_STACK_ALIAS_LIMIT >= sizeof worker_tls_used / sizeof worker_tls_used[0],
               "Worker stacks must have an alias slot for each TLS slot");
_Static_assert(POKOPIA_BOOT_WORKER_TLS + sizeof worker_tls_used / sizeof worker_tls_used[0] *
               POKOPIA_BOOT_WORKER_TLS_STRIDE <= UINT64_C(0x7e01000000),
               "Worker TLS must stay below the graphics driver mapping");
typedef struct {
    uint64_t start, end;
    uint32_t permission, attribute;
} CodeProtection;
static CodeProtection protected_code[16];
enum { SHARED_READ_HANDLE_COUNT = 16, AOC_LIST_EVENT = 0x100 };
typedef struct {
    HznReadonlyPage mapping;
    HznLiveShared *live;
    const void *data;
    size_t size, next_handle;
    uint32_t handle_base;
    bool handles[SHARED_READ_HANDLE_COUNT];
} SharedRead;
static unsigned char clock_page[HZN_READONLY_PAGE_BYTES];
static SharedRead shared_clock = {
    .data = clock_page, .size = sizeof clock_page, .handle_base = HZN_EVENT_HANDLE_END,
};
#ifdef POKOPIA_GFX
static SharedRead shared_streamplay = {
    .size = HZN_STPL_HID_BYTES, .handle_base = HZN_EVENT_HANDLE_END + SHARED_READ_HANDLE_COUNT,
};
static HznLiveShared mapped_hid;
static SharedRead shared_hid = {
    .live = &mapped_hid, .size = HZN_HID_MEMORY_BYTES,
    .handle_base = HZN_EVENT_HANDLE_END + 2 * SHARED_READ_HANDLE_COUNT,
};
static HznHidState hid_state;
#ifdef _WIN32
static char account_save_root[HZN_SAVE_PATH_MAX];
#else
static const char account_save_root[] = "/data/pokopia/save";
static int native_pad = -1;
#endif
#endif
static HznTimeConfig clock_config;
static bool clock_initialized;
static HznSm service_manager;
static const uint64_t guest_pid = 0x51;
#ifdef POKOPIA_GFX
static const uint64_t gfx_driver_address = UINT64_C(0x7e01000000);
static void *gfx_driver;
static PokopiaVkAdapter graphics;
static NvnFront *nvn;
static NvnWindowBridge *windows;
static NvnReplay *replay;
static PokopiaLiveTextureUpload texture_upload;
static NvnShaderCache *shaders;
#ifndef _WIN32
static PokopiaLivePresent live_output;
static PokopiaGameCapture live_capture;
static VkCommandBuffer live_present_command;
#endif
static void update_guest_pad(void)
{
    if (!mapped_hid.writer)
        return;
    HznHidPad pad = {0};
#ifndef _WIN32
    if (native_pad >= 0)
        hzn_hid_ps4_read(native_pad, &pad);
#endif
    hzn_hid_publish(mapped_hid.writer, &hid_state, &pad);
}
static ViAdapter vi;
static HznStackSources stack_sources;
typedef struct {
    union {
        HznFsp fsp;
        HznAccount account;
        HznOfflineApp offline_app;
    } service;
    unsigned refs;
#ifdef _WIN32
    char *root;
#endif
} SharedSession;
#endif
typedef enum { SERVICE_LM, SERVICE_APPLET, SERVICE_AOC, SERVICE_PCTL, SERVICE_TIME, SERVICE_ECTX,
               SERVICE_FSP, SERVICE_ACCOUNT, SERVICE_AUDIO_USER, SERVICE_AUDIO_RENDERER_MANAGER,
               SERVICE_AUDIO_DEVICE, SERVICE_AUDIO_OUTPUT_MANAGER, SERVICE_AUDIO_OUT, SERVICE_HWOPUS,
               SERVICE_BSD_SSL, SERVICE_STPL, SERVICE_STPL_USER, SERVICE_OFFLINE_APP,
               SERVICE_HID, SERVICE_HID_RESOURCE, SERVICE_HID_VIBRATION_LIST } ServiceType;
typedef struct {
    uint32_t handle;
    ServiceType type;
    union {
        HznLm lm;
        HznApplet applet;
        HznAoc aoc;
        HznPctl pctl;
        HznTime time;
        bool ectx_domain;
#ifdef POKOPIA_GFX
        SharedSession *shared;
        HznBsdSsl bsd_ssl;
#endif
#ifdef POKOPIA_AUDIO_OUT
        PokopiaAudioOut *audio_out;
#endif
#ifdef POKOPIA_HWOPUS
        HznHwopus hwopus;
#endif
    } state;
} ServiceSession;
static ServiceSession service_sessions[128];
static void kernel_lock(void)
{
    GuestBoundary *scope = pokopia_host_thread();
    pokopia_host_lock();
    if (scope)
        scope->kernel_locked = true;
}

static void kernel_unlock(void)
{
    GuestBoundary *scope = pokopia_host_thread();
    if (scope)
        scope->kernel_locked = false;
    pokopia_host_unlock();
}

static _Noreturn void finish_guest(int status)
{
    GuestBoundary *scope = pokopia_host_thread();
    if (!scope)
        abort();
    if (scope->kernel_locked)
        kernel_unlock();
    longjmp(scope->jump, status);
}
#endif

static void unmap_module(void *address, size_t size)
{
#ifdef _WIN32
    (void)size;
    VirtualFree(address, 0, MEM_RELEASE);
#else
    munmap(address, size);
#endif
}

static void *map_module(uint64_t base, size_t size)
{
    void *wanted = (void *)(uintptr_t)base;
#ifdef _WIN32
    void *mapping = VirtualAlloc(wanted, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mapping)
        return NULL;
#else
    /* Orbis expects FreeBSD's MAP_ANON. A hint avoids replacing an existing mapping. */
    void *mapping = mmap(wanted, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | 0x1000, -1, 0);
    if (mapping == MAP_FAILED)
        return NULL;
#endif
    if (mapping != wanted) {
        unmap_module(mapping, size);
        return NULL;
    }
    return mapping;
}

static _Noreturn void stop(const char *reason, uint64_t detail)
{
#ifdef POKOPIA_BOOT_PROBE
    GuestBoundary *scope = pokopia_host_thread();
    const Ctx *context = scope ? scope->context : &cpu;
#else
    const Ctx *context = &cpu;
#endif
    fprintf(probe_log, "STOP %s detail=%#" PRIx64 " guest-function=%#" PRIx64 " lr=%#" PRIx64 "\n",
            reason, detail, context->fn, context->x[30]);
#ifdef POKOPIA_GFX
    pokopia_vk_ledger_report(probe_log, reason);
#ifdef POKOPIA_SPARSE_HEAP
    pokopia_boot_report_heap(probe_log, "guest-stop");
#endif
#endif
    fflush(probe_log);
#if defined(POKOPIA_GFX) && !defined(_WIN32) && defined(POKOPIA_ORBIS_MEMORY_TRACE)
    pokopia_orbis_memory_census(probe_log, "guest boundary");
#endif
#ifdef POKOPIA_BOOT_PROBE
    finish_guest(2);
#else
    longjmp(boundary, 2);
#endif
}

static bool scan_segment(uint64_t address, Segment region, uint64_t *before, uint64_t *after,
                         MemoryInfo *result)
{
    if (address < region.start) {
        if (region.start < *after)
            *after = region.start;
    } else if (address >= region.end) {
        if (region.end > *before)
            *before = region.end;
    } else {
#ifdef POKOPIA_BOOT_PROBE
        if (region.state == 4) {
            for (size_t i = 0; i < sizeof protected_code / sizeof protected_code[0]; ++i) {
                const CodeProtection *protection = &protected_code[i];
                if (!protection->start || protection->start < region.start || protection->end > region.end)
                    continue;
                if (address < protection->start) {
                    region.end = protection->start;
                } else if (address >= protection->end) {
                    region.start = protection->end;
                } else {
                    *result = (MemoryInfo){ .base = protection->start, .size = protection->end - protection->start,
                                           .type = region.state, .permission = protection->permission,
                                           .attribute = protection->attribute };
                    return true;
                }
            }
        }
#endif
        *result = (MemoryInfo){ .base = region.start, .size = region.end - region.start,
                               .type = region.state, .permission = region.permission };
        return true;
    }
    return false;
}

static MemoryInfo query_memory(uint64_t address)
{
    /* The upper inaccessible range wraps to zero and ends the loader's scan. */
    if (address >= guest_limit)
        return (MemoryInfo){ .base = guest_limit, .size = 0 - guest_limit, .type = 0x10 };
    uint64_t before = 0, after = guest_limit;
    MemoryInfo result;
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; ++i) {
        const Module *module = &modules[i];
        for (size_t j = 0; j < 3; ++j) {
            Segment region = module->segments[j];
            region.start += module->base;
            region.end += module->base;
            if (scan_segment(address, region, &before, &after, &result))
                return result;
        }
    }
#ifdef POKOPIA_BOOT_PROBE
    const uint64_t heap_size = pokopia_boot_heap_size();
    if (heap_size && address >= POKOPIA_BOOT_HEAP_BASE && address - POKOPIA_BOOT_HEAP_BASE < heap_size) {
        before = POKOPIA_BOOT_HEAP_BASE;
        after = before + heap_size;
        for (size_t i = 0; i < heap_aliases.count; ++i) {
            const HznHeapStackAliasRange *alias = hzn_heap_stack_alias_at(&heap_aliases, i);
            Segment source = { alias->source, alias->source + alias->size, 0x05, 0 };
            if (scan_segment(address, source, &before, &after, &result))
                return result;
        }
        return (MemoryInfo){ .base = before, .size = after - before, .type = 0x05, .permission = 3 };
    }
    const Segment process_ranges[] = {
        { POKOPIA_BOOT_STACK_BASE, POKOPIA_BOOT_STACK_BASE + POKOPIA_BOOT_STACK_SIZE, 0x0b, 3 },
        { POKOPIA_BOOT_TLS_BASE, POKOPIA_BOOT_TCB_BASE + POKOPIA_BOOT_TLS_SIZE, 0x0c, 3 },
        { stack_map.source, stack_map.source + HZN_STACK_ALIAS_BYTES, 0x05, stack_map.alias ? 0 : 3 },
        { stack_map.alias, stack_map.alias + HZN_STACK_ALIAS_BYTES, 0x0b, 3 },
        { shared_clock.mapping.address, shared_clock.mapping.address + shared_clock.size, 0x06, 1 },
#ifdef POKOPIA_GFX
        { shared_streamplay.mapping.address, shared_streamplay.mapping.address + shared_streamplay.size, 0x06, 1 },
        { mapped_hid.address, mapped_hid.address + mapped_hid.size, 0x06, 1 },
#endif
        { heap_size ? POKOPIA_BOOT_HEAP_BASE : 0, POKOPIA_BOOT_HEAP_BASE + heap_size, 0x05, 3 },
        { exception_context ? exception_context_address : 0, exception_context_address + 0x1000, 0x05, 3 },
#ifdef POKOPIA_GFX
        { gfx_driver ? gfx_driver_address : 0, gfx_driver_address + POKOPIA_BOOT_TLS_SIZE, 0x05, 3 },
#endif
    };
    for (size_t i = 0; i < sizeof process_ranges / sizeof process_ranges[0]; ++i)
        if (process_ranges[i].start && scan_segment(address, process_ranges[i], &before, &after, &result))
            return result;
    for (size_t i = 0; i < heap_aliases.count; ++i) {
        const HznHeapStackAliasRange *alias = hzn_heap_stack_alias_at(&heap_aliases, i);
        Segment destination = { alias->destination, alias->destination + alias->size, 0x0b, 3 };
        if (scan_segment(address, destination, &before, &after, &result))
            return result;
    }
#ifdef POKOPIA_GFX
    for (size_t i = 0; i < HZN_STACK_SOURCE_LIMIT && stack_sources.count; ++i) {
        const HznStackSourceRange *stack = hzn_stack_source_at(&stack_sources, i);
        if (!stack)
            continue;
        uint32_t permission = 3;
        for (size_t j = 0; j < heap_aliases.count; ++j) {
            if (hzn_heap_stack_alias_at(&heap_aliases, j)->source == stack->address) {
                permission = 0;
                break;
            }
        }
        Segment backing = { stack->address, stack->address + stack->size, 0x05, permission };
        if (scan_segment(address, backing, &before, &after, &result))
            return result;
    }
#endif
    for (size_t i = 0; i < sizeof worker_tls_used / sizeof worker_tls_used[0]; ++i) {
        if (worker_tls_used[i]) {
            uint64_t start = POKOPIA_BOOT_WORKER_TLS + i * POKOPIA_BOOT_WORKER_TLS_STRIDE;
            Segment region = { start, start + 2 * POKOPIA_BOOT_TLS_SIZE, 0x0c, 3 };
            if (scan_segment(address, region, &before, &after, &result))
                return result;
        }
    }
#endif
    return (MemoryInfo){ .base = before, .size = after - before };
}

#ifdef POKOPIA_BOOT_PROBE
static uint32_t validate_thread(void *opaque, uint64_t entry, uint64_t stack_top)
{
    (void)opaque;
    if (!pokopia_boot_lookup(entry) || !stack_top || (stack_top & 15))
        return HZN_STACK_MAP_INVALID_ADDRESS;
    kernel_lock();
    MemoryInfo stack = query_memory(stack_top - 1);
    kernel_unlock();
    return stack.type == 0x0b && stack.permission == 3 ? 0 : HZN_STACK_MAP_INVALID_MEMORY_STATE;
}

static bool allocate_thread_tls(void *opaque, uint64_t *tls, uint64_t *tcb)
{
    (void)opaque;
    kernel_lock();
    for (size_t i = 0; i < sizeof worker_tls_used / sizeof worker_tls_used[0]; ++i) {
        if (worker_tls_used[i])
            continue;
        uint64_t address = POKOPIA_BOOT_WORKER_TLS + i * POKOPIA_BOOT_WORKER_TLS_STRIDE;
        if (!map_module(address, 2 * POKOPIA_BOOT_TLS_SIZE))
            continue;
        worker_tls_used[i] = true;
        *tls = address;
        *tcb = address + POKOPIA_BOOT_TLS_SIZE;
        kernel_unlock();
        return true;
    }
    kernel_unlock();
    return false;
}

static void release_thread_tls(void *opaque, uint64_t tls, uint64_t tcb)
{
    (void)opaque;
    (void)tcb;
    kernel_lock();
    unmap_module((void *)(uintptr_t)tls, 2 * POKOPIA_BOOT_TLS_SIZE);
    worker_tls_used[(tls - POKOPIA_BOOT_WORKER_TLS) / POKOPIA_BOOT_WORKER_TLS_STRIDE] = false;
    kernel_unlock();
}

void nx_site_inline(Ctx *context, uint64_t address)
{
    if (address != sdk_base + 0x14bf84)
        return;
    const uint64_t thread = sdk_base + 0xe13000;
    const uint64_t source = sdk_base + 0xe14000;
    if (context->x[0] != thread || context->x[2] != thread || context->x[3] != source ||
        context->x[1] != sdk_base + 0x14c000 || context->x[4] != HZN_STACK_ALIAS_BYTES)
        stop("unexpected SDK worker stack construction", address);
    kernel_lock();
    /* This SDK's 4 KiB static stack shares an Orbis page with SDK globals.
     * Relocate the stack argument before the SDK records it for map/unmap. */
    if (!worker_stack_seeded) {
        memcpy((void *)(uintptr_t)stack_map.source, (const void *)(uintptr_t)source, HZN_STACK_ALIAS_BYTES);
        worker_stack_seeded = true;
    }
    context->x[3] = stack_map.source;
    kernel_unlock();
}

static const void *read_guest(void *opaque, uint64_t address, size_t size)
{
    (void)opaque;
    MemoryInfo region = query_memory(address);
    if (!(region.permission & 1) || size > region.base + region.size - address)
        return NULL;
    return (const void *)(uintptr_t)address;
}

static void log_guest_frames(const Ctx *context)
{
    uint64_t frame = context->x[29];
    for (unsigned i = 0; i < 12; ++i) {
        MemoryInfo region = query_memory(frame);
        if (region.type != 0x0b || !(region.permission & 1) || (frame & 7) ||
            16 > region.base + region.size - frame)
            break;
        uint64_t caller = rd64(frame), lr = rd64(frame + 8);
        fprintf(probe_log, "guest frame %u fp=%#" PRIx64 " lr=%#" PRIx64 "\n", i, frame, lr);
        if (caller <= frame)
            break;
        frame = caller;
    }
}

static void *write_guest(void *opaque, uint64_t address, size_t size)
{
    (void)opaque;
    MemoryInfo region = query_memory(address);
    if (!(region.permission & 2) || size > region.base + region.size - address)
        return NULL;
    return (void *)(uintptr_t)address;
}

static uint32_t *resolve_sync_word(void *opaque, uint64_t address)
{
    (void)opaque;
    if (address & 3)
        return NULL;
    kernel_lock();
    uint32_t *word = write_guest(NULL, address, sizeof *word);
    kernel_unlock();
    return word;
}

static uint64_t resolve_sync_thread(void *opaque, uint32_t handle)
{
    (void)opaque;
    GuestBoundary *scope = pokopia_host_thread();
    return hzn_thread_id(threads, scope ? scope->context : NULL, handle);
}

#ifdef POKOPIA_GFX
/* The verified 655360 lift exposes the KTGL sites from games/pkp/main.sites. */
static void copy_render_view_sizes(uint64_t address)
{
    const uint64_t view_config = UINT64_C(0x7103561880);
    enum { VIEW_STRIDE = 28, MAX_SIZE = 24, CURRENT_SIZE = 32,
           MAX_LEVEL = 41, CURRENT_LEVEL = 42, SPLIT_VIEW = 562 };
    const uint64_t view = rd64(view_config);
    const uint8_t *config = read_guest(NULL, view, SPLIT_VIEW + 1);
    const size_t count = config && config[SPLIT_VIEW] ? 2 : 1;
    uint8_t *manager = write_guest(NULL, address, (count - 1) * VIEW_STRIDE + CURRENT_LEVEL + 1);
    if (!manager)
        return;
    for (size_t i = 0; i < count; ++i) {
        uint8_t *current = manager + i * VIEW_STRIDE;
        memcpy(current + CURRENT_SIZE, current + MAX_SIZE, 2 * sizeof(uint32_t));
        current[CURRENT_LEVEL] = current[MAX_LEVEL];
    }
}

static uint32_t render_level_size(uint32_t level, uint32_t extent, uint32_t native)
{
    if (level >= 30)
        return extent;
    const uint32_t size = native / 2 + level * (native / 60);
    return ((size * extent + native / 2) / native) & ~1u;
}

static bool render_entry_hook(Ctx *context, uint64_t address)
{
    uint32_t width, height;
    if (!pokopia_boot_render_override(&width, &height))
        return false;
    const uint32_t level = (uint8_t)context->x[0];
    switch (address) {
    case UINT64_C(0x7101129980): context->x[0] = width; return true;
    case UINT64_C(0x71011299c0): context->x[0] = height; return true;
    case UINT64_C(0x7101129990):
    case UINT64_C(0x71011299d0): {
        /* Levels 31–33 name separate stock resolutions. */
        if (level >= 31 && level <= 33)
            return false;
        const bool horizontal = address == UINT64_C(0x7101129990);
        context->x[0] = horizontal ? render_level_size(level, width, POKOPIA_RENDER_NATIVE_WIDTH) :
                                     render_level_size(level, height, POKOPIA_RENDER_NATIVE_HEIGHT);
        return true;
    }
    case UINT64_C(0x710022d1e0):
        kernel_lock();
        copy_render_view_sizes(context->x[0]);
        kernel_unlock();
        return false;
    default: return false;
    }
}

static bool authored_full_extent(double width, double height, int32_t target_width,
                                 int32_t target_height, uint32_t render_width, uint32_t render_height)
{
    if (target_width <= 0 || target_height <= 0 || (width == target_width && height == target_height))
        return false;
    const double dw = width - (double)target_width * POKOPIA_RENDER_NATIVE_WIDTH / render_width;
    const double dh = height - (double)target_height * POKOPIA_RENDER_NATIVE_HEIGHT / render_height;
    return dw >= -1.0 && dw <= 1.0 && dh >= -1.0 && dh <= 1.0;
}

static void fix_command_viewports(const Ctx *context, uint64_t rect_address, uint32_t count,
                                 uint32_t width, uint32_t height)
{
    const uint64_t command_context = context->x[0];
    if (!count || count > 16 || !read_guest(NULL, command_context, 0x28))
        return;
    const int32_t index = (int32_t)rd32(command_context + 0x24);
    if (index < 0)
        return;
    const uint64_t table = rd64(command_context + 0x18);
    const uint64_t slot = 616 * (uint64_t)index + 0x50;
    if (slot > UINT64_MAX - table || !read_guest(NULL, table + slot, sizeof(uint64_t)))
        return;
    const uint64_t target = rd64(table + slot);
    if (!read_guest(NULL, target, 0x60))
        return;
    const int32_t target_width = (int32_t)rd32(target + 0x58);
    const int32_t target_height = (int32_t)rd32(target + 0x5c);
    enum { VIEWPORT_BYTES = 24 };
    uint8_t *rects = write_guest(NULL, rect_address, count * VIEWPORT_BYTES);
    if (!rects)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *viewport = rects + i * VIEWPORT_BYTES;
        int32_t rect[4]; /* width, height, x, y */
        memcpy(rect, viewport, sizeof rect);
        if (!rect[2] && !rect[3] && authored_full_extent(rect[0], rect[1], target_width, target_height, width, height)) {
            rect[0] = target_width;
            rect[1] = target_height;
            memcpy(viewport, rect, 2 * sizeof(int32_t));
        }
    }
}

static void fix_replay_viewports(const Ctx *context, uint32_t width, uint32_t height)
{
    const uint32_t count = (uint32_t)context->x[23];
    const uint64_t stack = context->x[31];
    if (!count || count > 16 || stack > UINT64_MAX - 216)
        return;
    float (*viewports)[4] = write_guest(NULL, stack + 216, count * sizeof *viewports);
    const int32_t (*scissors)[4] = read_guest(NULL, stack + 24, count * sizeof *scissors);
    if (!viewports || !scissors)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        float *viewport = viewports[i];
        const int32_t *scissor = scissors[i];
        if (!viewport[0] && !viewport[1] && !scissor[0] && !scissor[1] &&
            authored_full_extent(viewport[2], viewport[3], scissor[2], scissor[3], width, height)) {
            viewport[2] = (float)scissor[2];
            viewport[3] = (float)scissor[3];
        }
    }
}

static void fix_terrain_copy(const Ctx *context, uint32_t width, uint32_t height)
{
    const uint64_t source = context->x[3];
    int32_t *rect = write_guest(NULL, context->x[5], 4 * sizeof *rect);
    if (!rect || !read_guest(NULL, source, 52))
        return;
    const int32_t source_width = rd16(source + 48), source_height = rd16(source + 50);
    /* The 80x45 destination keeps its size. Correct only the full source rectangle. */
    if (!rect[0] && !rect[1] && authored_full_extent(rect[2], rect[3], source_width, source_height, width, height)) {
        rect[2] = source_width;
        rect[3] = source_height;
    }
}

int nx_site_midhook(Ctx *context, uint64_t address)
{
    if (address != UINT64_C(0x71017b50ac) && address != UINT64_C(0x71017b5254) &&
        address != UINT64_C(0x71017b6c98) && address != UINT64_C(0x710073d824))
        return 0;
    uint32_t width, height;
    if (!pokopia_boot_render_override(&width, &height))
        return 0;
    kernel_lock();
    switch (address) {
    case UINT64_C(0x71017b50ac): fix_command_viewports(context, context->x[1], 1, width, height); break;
    case UINT64_C(0x71017b5254):
        fix_command_viewports(context, context->x[2], (uint32_t)context->x[1], width, height); break;
    case UINT64_C(0x71017b6c98): fix_replay_viewports(context, width, height); break;
    case UINT64_C(0x710073d824): fix_terrain_copy(context, width, height); break;
    }
    kernel_unlock();
    return 0;
}

static bool gfx_ready(void *opaque)
{
    const PokopiaVkAdapter *adapter = opaque;
    return adapter->device && adapter->queue;
}

static bool vi_output(void *opaque, ViAdapterOutput *output)
{
    VkExtent2D extent;
    if (!pokopia_vk_output(opaque, &extent))
        return false;
    *output = (ViAdapterOutput){ .width = extent.width, .height = extent.height };
    return true;
}

static bool nvn_window_output(void *opaque, uint64_t native_window, ViAdapterOutput *output)
{
    (void)opaque;
    return vi_adapter_get_output(&vi, native_window, output);
}

static bool nvn_window_texture(void *opaque, uint64_t address, NvnFrontTextureInfo *info)
{
    return nvn_front_get_texture(opaque, address, info);
}

static bool heap_range(uint64_t address, uint64_t size)
{
    uint64_t heap_size = pokopia_boot_heap_size();
    return address >= POKOPIA_BOOT_HEAP_BASE && address - POKOPIA_BOOT_HEAP_BASE < heap_size &&
           size <= heap_size - (address - POKOPIA_BOOT_HEAP_BASE);
}

static bool isolated_stack_range(uint64_t address, uint64_t size)
{
    for (size_t i = 0; i < HZN_STACK_SOURCE_LIMIT && stack_sources.count; ++i) {
        const HznStackSourceRange *stack = hzn_stack_source_at(&stack_sources, i);
        if (stack && stack->address == address && stack->size == size)
            return true;
    }
    return false;
}

static bool stack_entry_hook(Ctx *context, uint64_t address)
{
    if (address != POKOPIA_THREAD_CREATE && address != POKOPIA_THREAD_DESTROY)
        return false;
    GuestBoundary *scope = pokopia_host_thread();
    if (!scope)
        return false;
    if (scope->bypass_entry == address) {
        scope->bypass_entry = 0;
        return false;
    }
    uint64_t thread = context->x[1];
    bool creating = address == POKOPIA_THREAD_CREATE;
    kernel_lock();
    if (creating) {
        uint64_t original = context->x[4], size = context->x[5], aligned;
        /* The game embeds live ThreadType objects next to 4 KiB stack ranges.
         * Isolate any partial host page before the SDK records its source. */
        if (!((original | size) & (HZN_STACK_SOURCE_ALIGNMENT - 1)) || !heap_range(original, size)) {
            kernel_unlock();
            return false;
        }
        if (!write_guest(NULL, original, size) || !write_guest(NULL, thread, 0x60))
            stop("invalid SDK thread stack storage", original);
        uint32_t result = hzn_stack_source_prepare(&stack_sources, thread, original, size, &aligned);
        if (result) {
            fprintf(probe_log, "Game worker stack source=%#" PRIx64 " size=%#" PRIx64
                               " slot=%#" PRIx64 " thread=%#" PRIx64 "\n",
                    original, size, stack_sources.slot_size, thread);
            stop("could not isolate game worker stack", result);
        }
        context->x[4] = aligned;
        fprintf(probe_log, "Isolated game worker stack %#" PRIx64 " -> %#" PRIx64 " size=%#" PRIx64 "\n",
                original, aligned, size);
        fflush(probe_log);
    } else {
        bool recorded = false;
        for (size_t i = 0; i < HZN_STACK_SOURCE_LIMIT && stack_sources.count; ++i) {
            const HznStackSourceRange *stack = hzn_stack_source_at(&stack_sources, i);
            if (stack && stack->thread_object == thread) {
                recorded = true;
                break;
            }
        }
        if (!recorded) {
            kernel_unlock();
            return false;
        }
    }
    kernel_unlock();
    PokopiaBootEntry original_entry = pokopia_boot_lookup(address);
    if (!original_entry)
        stop("missing SDK thread entry", address);
    scope->bypass_entry = address;
    original_entry(context);
    scope->bypass_entry = 0;
    if (!creating || (uint32_t)context->x[0]) {
        kernel_lock();
        uint32_t result = hzn_stack_source_release(&stack_sources, &heap_aliases, thread);
        if (result)
            stop("could not release game worker stack", result);
        kernel_unlock();
    }
    return true;
}

typedef struct {
    PokopiaVkAdapter *adapter;
    VkCommandPool commands;
} NvnHostQueue;

typedef struct {
    VkFence fence;
    bool pending;
} NvnHostSync;

static void *create_nvn_device(void *opaque, uint32_t flags)
{
    PokopiaVkAdapter *adapter = opaque;
    if (!adapter->device)
        return NULL;
    fprintf(probe_log, "NVN device bound to active Vulkan device flags=%#" PRIx32 "\n", flags);
    fflush(probe_log);
    return adapter;
}

static void destroy_nvn_device(void *opaque, void *device)
{
    /* The adapter owns the VkDevice across all frontend objects. */
    (void)opaque;
    (void)device;
}

static void *create_nvn_queue(void *opaque, void *device, const NvnFrontQueueConfig *config)
{
    PokopiaVkAdapter *adapter = opaque;
    if (device != adapter || !adapter->queue)
        return NULL;
    NvnHostQueue *queue = calloc(1, sizeof *queue);
    if (!queue)
        return NULL;
    const VkCommandPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = adapter->family,
    };
    VkResult result = adapter->create_command_pool(adapter->device, &info, NULL, &queue->commands);
    if (result != VK_SUCCESS) {
        free(queue);
        fprintf(probe_log, "Vulkan queue command pool failed (%d)\n", result);
        return NULL;
    }
    queue->adapter = adapter;
    fprintf(probe_log, "NVN queue bound to Vulkan family=%u flags=%#" PRIx32 "\n", adapter->family, config->flags);
    fflush(probe_log);
    return queue;
}

static void destroy_nvn_queue(void *opaque, void *object)
{
    (void)opaque;
    NvnHostQueue *queue = object;
    queue->adapter->destroy_command_pool(queue->adapter->device, queue->commands, NULL);
    free(queue);
}

static VkResult open_nvn_texel(PokopiaVkAdapter *adapter, const NvnFrontTextureCreateInfo *info,
                               PokopiaVkTexture *texture)
{
    const NvnFrontTextureConfig *config = &info->config;
    VkFormat format = nvn_guest_texture_vk_format(config->format);
    uint32_t texel_bytes = nvn_guest_buffer_texel_bytes(config->format);
    if (!adapter->format_properties || format == VK_FORMAT_UNDEFINED || config->flags ||
        config->levels != 1 || config->samples > 1 || config->stride || config->gl_texture_name ||
        config->packaged_layout || config->packaged_data || !texel_bytes || !config->width ||
        (uint64_t)config->width * texel_bytes != info->storage_size || info->storage_size > SIZE_MAX)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (!read_guest(NULL, info->storage_address, (size_t)info->storage_size))
        return VK_ERROR_INITIALIZATION_FAILED;
    VkFormatProperties format_properties;
    adapter->format_properties(adapter->physical, format, &format_properties);
    VkResult result = pokopia_vk_texel_configure(&texture->texel, adapter->device, adapter->get_device_proc,
        &adapter->memory, format_properties.bufferFeatures, format, config->width, texel_bytes,
        adapter->properties.limits.maxTexelBufferElements);
    if (result != VK_SUCCESS)
        return result;
    texture->format = format;
    return VK_SUCCESS;
}

static NvnFrontTextureCreateResult create_nvn_texture(void *opaque, void *device,
    const NvnFrontTextureCreateInfo *info, void **object)
{
    PokopiaVkAdapter *adapter = opaque;
    if (device != adapter)
        return NVN_FRONT_TEXTURE_FAILED;
    PokopiaVkTexture *texture = calloc(1, sizeof *texture);
    if (!texture)
        return NVN_FRONT_TEXTURE_FAILED;
    const bool is_texel = info->config.target == 0xa;
    VkResult result = is_texel ? open_nvn_texel(adapter, info, texture) :
        pokopia_vk_texture_open_lazy(adapter, &info->config, texture);
    if (result != VK_SUCCESS) {
        free(texture);
        fprintf(probe_log, "Vulkan texture creation result=%d NVN format=%#x flags=%#x "
                "%ux%ux%u target=%#x mips=%u guest=%#" PRIx64 " guest-bytes=%#" PRIx64 "\n",
                result, info->config.format, info->config.flags, info->config.width,
                info->config.height, info->config.depth, info->config.target, info->config.levels,
                info->storage_address, info->storage_size);
#if !defined(_WIN32) && defined(POKOPIA_ORBIS_MEMORY_TRACE)
        static bool reported_device_oom;
        if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !reported_device_oom) {
            reported_device_oom = true;
            pokopia_vk_ledger_report(probe_log, "first texture device allocation failure");
            pokopia_orbis_memory_census(probe_log, "first texture device allocation failure");
        }
#endif
        fflush(probe_log);
        if (result == VK_ERROR_FORMAT_NOT_SUPPORTED || result == VK_ERROR_FEATURE_NOT_PRESENT)
            return NVN_FRONT_TEXTURE_UNSUPPORTED;
        return NVN_FRONT_TEXTURE_FAILED;
    }
    *object = texture;
    if (is_texel) {
        fprintf(probe_log, "NVN buffer texture registered for Vulkan backing format=%u guest=%#" PRIx64
                " bytes=%#" PRIx64 "\n", (unsigned)texture->format, info->storage_address, info->storage_size);
        fflush(probe_log);
        return NVN_FRONT_TEXTURE_CREATED;
    }
    fprintf(probe_log, "NVN texture %s %ux%ux%u target=%#x format=%u mips=%u guest=%#" PRIx64
            " guest-bytes=%#" PRIx64 " Vulkan-bytes=%#" PRIx64 " storage-view=%u\n",
            texture->deferred ? "deferred Vulkan backing for" : "backed by Vulkan",
            info->config.width, info->config.height, info->config.depth, info->config.target,
            (unsigned)texture->format, info->config.levels, info->storage_address,
            info->storage_size, (uint64_t)texture->backing.bytes, texture->storage_view != VK_NULL_HANDLE);
    fflush(probe_log);
    return NVN_FRONT_TEXTURE_CREATED;
}

static void destroy_nvn_texture(void *opaque, void *object)
{
    PokopiaVkTexture *texture = object;
    pokopia_vk_texture_close(opaque, texture);
    free(texture);
}

static void *create_nvn_sync(void *opaque, void *device)
{
    PokopiaVkAdapter *adapter = opaque;
    if (device != adapter)
        return NULL;
    PFN_vkCreateFence create = (PFN_vkCreateFence)adapter->get_device_proc(adapter->device, "vkCreateFence");
    if (!create || !adapter->get_device_proc(adapter->device, "vkDestroyFence") ||
        !adapter->get_device_proc(adapter->device, "vkWaitForFences"))
        return NULL;
    NvnHostSync *sync = calloc(1, sizeof *sync);
    if (!sync)
        return NULL;
    const VkFenceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkResult result = create(adapter->device, &info, NULL, &sync->fence);
    if (result != VK_SUCCESS) {
        fprintf(probe_log, "Vulkan sync fence creation failed (%d)\n", result);
        fflush(probe_log);
        free(sync);
        return NULL;
    }
    fputs("NVN sync backed by an unsignaled Vulkan fence\n", probe_log);
    fflush(probe_log);
    return sync;
}

static VkResult wait_nvn_sync(PokopiaVkAdapter *adapter, NvnHostSync *sync, uint64_t timeout)
{
    if (!sync->pending)
        return VK_SUCCESS;
    PFN_vkWaitForFences wait = (PFN_vkWaitForFences)adapter->get_device_proc(adapter->device, "vkWaitForFences");
    VkResult result = wait(adapter->device, 1, &sync->fence, VK_TRUE, timeout);
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
        sync->pending = false;
    return result;
}

static void destroy_nvn_sync(void *opaque, void *object)
{
    PokopiaVkAdapter *adapter = opaque;
    NvnHostSync *sync = object;
    VkResult result = wait_nvn_sync(adapter, sync, UINT64_MAX);
    if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
        result = adapter->wait_idle(adapter->device);
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
            fprintf(probe_log, "Could not retire Vulkan sync fence (%d)\n", result);
            fflush(probe_log);
            free(sync);
            return;
        }
    }
    PFN_vkDestroyFence destroy = (PFN_vkDestroyFence)adapter->get_device_proc(adapter->device, "vkDestroyFence");
    destroy(adapter->device, sync->fence, NULL);
    free(sync);
}

static void *create_nvn_command_buffer(void *opaque, void *device)
{
    PokopiaVkAdapter *adapter = opaque;
    if (device != adapter)
        return NULL;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkResult result = pokopia_vk_command_buffer_open(adapter, &command);
    if (result != VK_SUCCESS) {
        fprintf(probe_log, "Vulkan command buffer allocation failed (%d)\n", result);
        fflush(probe_log);
        return NULL;
    }
    fputs("NVN command buffer backed by a Vulkan primary command buffer\n", probe_log);
    fflush(probe_log);
    return command;
}

static void destroy_nvn_command_buffer(void *opaque, void *object)
{
    pokopia_vk_command_buffer_close(opaque, object);
}

static bool execute_nvn_copy(void *opaque, const void *source, void *destination, size_t bytes)
{
    VkResult result = pokopia_vk_copy_guest(opaque, source, destination, bytes);
    fprintf(probe_log, "NVN Vulkan buffer copy bytes=%zu result=%d\n", bytes, result);
    fflush(probe_log);
    return result == VK_SUCCESS;
}

static bool execute_nvn_clear(void *opaque, void *object, VkRect2D area, const float rgba[4], uint32_t mask)
{
    PokopiaVkTexture *texture = object;
    uint32_t channels = texture->format == VK_FORMAT_R16G16_SFLOAT ? 3u :
                        texture->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ? 7u : 15u;
    if ((mask & channels) != channels)
        return false;
    VkResult result = pokopia_vk_clear_color_region(opaque, texture, area, rgba);
    if (result != VK_SUCCESS || texture->render_serial <= 8) {
        fprintf(probe_log, "NVN Vulkan color clear serial=%" PRIu64 " result=%d\n", texture->render_serial, result);
        fflush(probe_log);
    }
    return result == VK_SUCCESS;
}

static bool execute_nvn_clear_depth_stencil(void *opaque, void *texture, VkRect2D area,
                                            VkImageAspectFlags aspects, VkClearDepthStencilValue value)
{
    const VkResult result = pokopia_vk_clear_depth_stencil(opaque, texture, area, aspects, value);
    if (result != VK_SUCCESS) {
        fprintf(probe_log, "NVN Vulkan depth/stencil clear aspects=%#x result=%d\n", aspects, result);
        fflush(probe_log);
    }
    return result == VK_SUCCESS;
}

static bool execute_nvn_draw(void *opaque, const PokopiaLiveDraw *draw)
{
    uint64_t serial = 0;
    VkResult result = VK_SUCCESS;
    for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT && result == VK_SUCCESS; ++i) {
        const PokopiaLiveDrawSample *sample = &draw->fragment_samples[i];
        if (sample->texture && sample->texture != draw->target)
            result = pokopia_live_texture_prepare_sample(&texture_upload, sample->guest_info);
    }
    if (result == VK_SUCCESS)
        result = pokopia_live_draw(opaque, draw, &serial);
    static unsigned loading_draws_logged;
    const bool log_loading_draw = loading_draws_logged < 4 &&
        draw->vertex_shader.hash && draw->fragment_shader.hash &&
        !strcmp(draw->vertex_shader.hash, "a26763a4bdd7b361") &&
        !strcmp(draw->fragment_shader.hash, "2db3327d9fbc5bb3");
    if (log_loading_draw)
        ++loading_draws_logged;
    if (result != VK_SUCCESS || serial <= 16 || log_loading_draw) {
        fprintf(probe_log, "NVN Vulkan draw vs=%s fs=%s %ux%u indices=%" PRIu32
                           " vertices=%" PRIu32 " serial=%" PRIu64 " result=%d\n",
                draw->vertex_shader.hash, draw->fragment_shader.hash ? draw->fragment_shader.hash : "-",
                draw->extent.width, draw->extent.height,
                draw->index_count, draw->vertex_count, serial, result);
        fflush(probe_log);
    }
#if defined(POKOPIA_SPARSE_HEAP) && defined(POKOPIA_VK_MEMORY_TRACE)
    if (log_loading_draw && loading_draws_logged == 2 && result == VK_SUCCESS) {
        pokopia_boot_report_heap(probe_log, "loading-draw");
        pokopia_vk_ledger_report(probe_log, "loading draw");
#if !defined(_WIN32) && defined(POKOPIA_ORBIS_MEMORY_TRACE)
        pokopia_orbis_memory_census(probe_log, "loading draw");
#endif
        fflush(probe_log);
    }
#endif
    return result == VK_SUCCESS;
}

static bool execute_nvn_compute(void *opaque, const PokopiaLiveCompute *dispatch)
{
    const VkResult result = pokopia_live_compute(opaque, dispatch);
    static unsigned logged;
    if (result != VK_SUCCESS || logged < 4) {
        fprintf(probe_log, "NVN Vulkan compute %s groups=%ux%ux%u result=%d\n", dispatch->hash,
                dispatch->groups[0], dispatch->groups[1], dispatch->groups[2], result);
        fflush(probe_log);
        ++logged;
    }
    return result == VK_SUCCESS;
}

static bool finish_nvn_queue(void *opaque)
{
    PokopiaVkAdapter *adapter = opaque;
    PFN_vkQueueWaitIdle wait = (PFN_vkQueueWaitIdle)adapter->get_device_proc(adapter->device, "vkQueueWaitIdle");
    return wait && wait(adapter->queue) == VK_SUCCESS;
}

static bool execute_nvn_sync(void *opaque, void *object, bool wait)
{
    PokopiaVkAdapter *adapter = opaque;
    NvnHostSync *sync = object;
    if (wait_nvn_sync(adapter, sync, UINT64_MAX) != VK_SUCCESS)
        return false;
    if (wait)
        return true;
    VkDevice device = adapter->device;
    PFN_vkResetFences reset = (PFN_vkResetFences)adapter->get_device_proc(device, "vkResetFences");
    PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)adapter->get_device_proc(device, "vkQueueSubmit");
    if (!reset || !submit || reset(device, 1, &sync->fence) != VK_SUCCESS ||
        submit(adapter->queue, 0, NULL, sync->fence) != VK_SUCCESS)
        return false;
    sync->pending = true;
    return true;
}

static void *create_nvn_program(void *opaque, void *device)
{
    return opaque == device ? pokopia_vk_program_create(opaque) : NULL;
}

static void destroy_nvn_program(void *opaque, void *program)
{
    (void)opaque;
    pokopia_vk_program_destroy(program);
}

static NvnFrontProgramLinkResult link_nvn_program(void *opaque, void *program,
    const NvnFrontShaderData *code, size_t count)
{
    (void)opaque;
    if (count && !shaders) {
        NvnShaderCacheStatus status = NVN_SHADER_CACHE_MISSING;
#ifdef _WIN32
        char *path = NULL;
        size_t length;
        if (!_dupenv_s(&path, &length, "POKOPIA_SHADER_CACHE") && path && *path)
            shaders = nvn_shader_cache_open(path, &status);
        free(path);
#else
        shaders = nvn_shader_cache_open("/app0/shaders", &status);
        if (!shaders && status == NVN_SHADER_CACHE_MISSING)
            shaders = nvn_shader_cache_open("/data/pokopia/shaders", &status);
#endif
        if (!shaders) {
            fprintf(probe_log, "NVN shader cache could not open (%d)\n", status);
            fflush(probe_log);
        }
    }
    return pokopia_vk_program_link(program, shaders, code, count, probe_log);
}

static bool nvn_device_integer(void *opaque, void *device, uint32_t query, int32_t *value)
{
    PokopiaVkAdapter *adapter = opaque;
    if (!adapter->device || (device && device != adapter))
        return false;
    const VkPhysicalDeviceLimits *limits = &adapter->properties.limits;
    switch (query) {
    case 0x06: *value = (int32_t)limits->maxVertexInputBindings; return true;
    case 0x0e: *value = (int32_t)limits->maxVertexInputAttributes; return true;
    case 0x2a: *value = adapter->features.textureCompressionASTC_LDR != 0; return true;
    case 0x38: *value = NVN_GUEST_LINEAR_STRIDE_ALIGNMENT; return true;
    default: return false;
    }
}

static NvnHostSync *find_nvn_sync(uint64_t address, uint64_t device)
{
    void *sync;
    if (!nvn_front_get_sync(nvn, address, &sync))
        return NULL;
    if (device) {
        const void *field = read_guest(NULL, address, sizeof device);
        uint64_t owner;
        if (!field)
            return NULL;
        memcpy(&owner, field, sizeof owner);
        if (owner != device)
            return NULL;
    }
    return sync;
}

static NvnFrontReplyKind nvn_acquire_texture(uint64_t window, uint64_t sync_address, uint64_t device,
                                            uint64_t output, uint64_t *result)
{
    NvnWindowInfo info;
    uint32_t *index = write_guest(NULL, output, sizeof *index);
    if (!index || !nvn_window_get(windows, window, &info) || (device && device != info.device))
        return NVN_FRONT_INVALID_GUEST;
    if (sync_address) {
        NvnHostSync *sync = find_nvn_sync(sync_address, info.device);
        if (!sync)
            return NVN_FRONT_INVALID_GUEST;
        if (wait_nvn_sync(&graphics, sync, UINT64_MAX) != VK_SUCCESS)
            return NVN_FRONT_BOUNDARY;
    }
    uint32_t acquired;
    if (nvn_window_acquire(windows, window, &acquired)) {
        memcpy(index, &acquired, sizeof acquired);
        *result = 0;
        fprintf(probe_log, "NVN acquired game image %u\n", acquired);
        fflush(probe_log);
    } else {
        *result = 1;
    }
    return NVN_FRONT_HANDLED;
}

#ifndef _WIN32
static bool open_live_output(void)
{
    if (live_present_command)
        return true;
    VkResult result = pokopia_vk_command_buffer_open(&graphics, &live_present_command);
    if (result != VK_SUCCESS)
        return false;
    PokopiaGameCapture *capture = NULL;
    if (pokopia_game_capture_init(&live_capture, POKOPIA_DEBUG_DIRECTORY))
        capture = &live_capture;
    else
        fputs("Live PS4 screenshot mailbox unavailable\n", probe_log);
    result = pokopia_live_present_init(&live_output, graphics.physical, &graphics.present,
                                      live_present_command, capture);
    return result == VK_SUCCESS;
}
#endif

#if !defined(_WIN32) || defined(POKOPIA_HOST_HEADLESS_PRESENT)
static NvnFrontReplyKind present_nvn_frame(const uint64_t x[32])
{
    update_guest_pad();
    NvnFrontQueueInfo queue;
    NvnWindowInfo window;
    NvnFrontTextureInfo image;
    const uint32_t index = (uint32_t)x[2];
    if (!nvn_front_get_queue(nvn, x[0], &queue) || !nvn_window_get_acquired(windows, x[1], index, &window) ||
        window.device != queue.device || !nvn_front_get_texture(nvn, window.textures[index], &image))
        return NVN_FRONT_INVALID_GUEST;
    PokopiaVkTexture *texture = image.native;
    if (!texture || !texture->backing.image || !texture->render_serial)
        return NVN_FRONT_BOUNDARY;
#ifdef _WIN32
    /* The headless test consumes completed images and releases the same NVN window slots. */
    if (texture->render_serial > graphics.render_serial)
        return NVN_FRONT_BOUNDARY;
    nvn_window_release(windows, x[1], index);
    if (texture->render_serial <= 8) {
        fprintf(probe_log, "Host headless present render serial=%" PRIu64 " image=%u\n", texture->render_serial, index);
#ifdef POKOPIA_SPARSE_HEAP
        pokopia_boot_report_heap(probe_log, "headless-present");
#endif
        if (texture->render_serial == 1 || texture->render_serial == 4)
            pokopia_vk_ledger_report(probe_log, "rendered present");
        fflush(probe_log);
    }
#else
    const PokopiaLiveFrame frame = {
        .image = texture->backing.image, .format = texture->format,
        .extent = { image.create_info.config.width, image.create_info.config.height },
        .layout = texture->layout, .stages = texture->stages, .access = texture->access,
        .crop = {{window.crop.x, window.crop.y}, {(uint32_t)window.crop.width, (uint32_t)window.crop.height}},
        .render_serial = texture->render_serial, .completed_render_serial = graphics.render_serial,
    };
    VkResult shot = VK_NOT_READY;
    VkResult result = pokopia_live_present_frame(&live_output, &frame, UINT64_C(5000000000), &shot);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        fprintf(probe_log, "Live PS4 VideoOut present failed (%d)\n", result);
        fflush(probe_log);
        return NVN_FRONT_BOUNDARY;
    }
    nvn_window_release(windows, x[1], index);
#ifdef POKOPIA_ORBIS_MEMORY_TRACE
    static uint64_t live_presents;
    if (++live_presents % 256 == 0) {
        fprintf(probe_log, "Live PS4 memory after %" PRIu64 " presents\n", live_presents);
#ifdef POKOPIA_SPARSE_HEAP
        pokopia_boot_report_heap(probe_log, "videoout-budget");
#endif
        pokopia_vk_ledger_report(probe_log, "videoout-budget");
        pokopia_orbis_memory_census(probe_log, "videoout-budget");
    } else {
        pokopia_orbis_memory_trace_flush(probe_log);
    }
#endif
    if (frame.render_serial <= 8 || shot == VK_SUCCESS) {
        fprintf(probe_log, "Live PS4 VideoOut submitted render serial=%" PRIu64 " image=%u\n", frame.render_serial, index);
        if (shot == VK_SUCCESS)
            fprintf(probe_log, "Captured live frame serial=%" PRIu64 " request=%016" PRIx64 "\n",
                    live_capture.captured_render_serial, live_capture.captured_request_seq);
#ifdef POKOPIA_SPARSE_HEAP
        pokopia_boot_report_heap(probe_log, "videoout-present");
#endif
        if (frame.render_serial == 1 || frame.render_serial == 4 || shot == VK_SUCCESS)
            pokopia_vk_ledger_report(probe_log, "rendered present");
        fflush(probe_log);
    }
#endif
    return NVN_FRONT_HANDLED;
}
#endif

static NvnFrontReplyKind nvn_frame_control(const char *name, uint64_t x[32])
{
#if !defined(_WIN32) || defined(POKOPIA_HOST_HEADLESS_PRESENT)
    if (!strcmp(name, "nvnQueuePresentTexture"))
        return present_nvn_frame(x);
#endif
    if (!strcmp(name, "nvnWindowAcquireTexture"))
        return nvn_acquire_texture(x[0], x[1], 0, x[2], &x[0]);
    if (!strcmp(name, "nvnSyncWait")) {
        NvnHostSync *sync = find_nvn_sync(x[0], 0);
        if (!sync)
            return NVN_FRONT_INVALID_GUEST;
        VkResult result = wait_nvn_sync(&graphics, sync, 0);
        if (result == VK_SUCCESS) {
            x[0] = 0;
        } else if (result == VK_TIMEOUT && x[1]) {
            result = wait_nvn_sync(&graphics, sync, x[1]);
            x[0] = result == VK_SUCCESS ? 1 : result == VK_TIMEOUT ? 2 : 3;
        } else {
            x[0] = result == VK_TIMEOUT ? 2 : 3;
        }
        return NVN_FRONT_HANDLED;
    }
    bool acquire = !strcmp(name, "nvnQueueAcquireTexture");
    bool fence = !strcmp(name, "nvnQueueFenceSync");
    if (!acquire && !fence && strcmp(name, "nvnQueueWaitSync"))
        return NVN_FRONT_NOT_OURS;
    NvnFrontQueueInfo queue;
    if (!nvn_front_get_queue(nvn, x[0], &queue))
        return NVN_FRONT_INVALID_GUEST;
    if (acquire)
        return nvn_acquire_texture(x[1], 0, queue.device, x[2], &x[0]);
    NvnHostSync *sync = find_nvn_sync(x[1], queue.device);
    if (!sync)
        return NVN_FRONT_INVALID_GUEST;
    if (!fence) {
        /* The guest queues submit to one ordered VkQueue. */
        x[0] = 1;
        return NVN_FRONT_HANDLED;
    }
    return !(uint32_t)x[2] && (uint32_t)x[3] <= 1 && execute_nvn_sync(&graphics, sync, false) ?
           NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}

static NvnFrontReplyKind dispatch_nvn_replay(Ctx *context, const char *name, const uint32_t scalar[3])
{
    NvnFrontReplyKind reply = nvn_replay_dispatch(replay, name, context->x, scalar);
    NvnReplayMemoryRequest request;
    unsigned callbacks = 0;
    while (reply == NVN_FRONT_BOUNDARY && nvn_replay_take_memory_request(replay, &request)) {
        GuestBoundary *scope = pokopia_host_thread();
        const unsigned event_bit = 1u << request.event;
        if (!scope || scope->nvn_memory_callback || (callbacks & event_bit))
            break;
        callbacks |= event_bit;

        const bool control = request.event == NVN_REPLAY_MEMORY_CONTROL;
        const Ctx saved = *context;
        context->x[0] = request.encoder;
        context->x[1] = request.event;
        context->x[2] = request.minimum_bytes;
        context->x[3] = request.data;
        scope->nvn_memory_callback = true;
        fprintf(probe_log, "NVN %s memory callback encoder=%#" PRIx64 " function=%#" PRIx64
                           " minimum=%" PRIu64 " caller=%#" PRIx64 "\n",
                control ? "control" : "command", request.encoder, request.function, request.minimum_bytes, saved.x[30]);
        fflush(probe_log);
        kernel_unlock();
        call_indirect(context, request.function);
        kernel_lock();
        scope->nvn_memory_callback = false;
        *context = saved;

        NvnFrontCommandBufferInfo command;
        if (!nvn_front_get_command_buffer(nvn, request.encoder, &command) || command.native != request.native)
            break;
        if ((control ? command.control_revision : command.command_revision) == request.revision)
            break;
        reply = nvn_replay_dispatch(replay, name, context->x, scalar);
    }
    return reply;
}

static bool dispatch_graphics(Ctx *context, uint64_t address)
{
    if (address == POKOPIA_NV_INITIALIZE || address == POKOPIA_NV_INITIALIZE_DEFAULT ||
        address == POKOPIA_NV_INITIALIZE_DEVTOOLS) {
        kernel_lock();
        void *initialized = write_guest(NULL, POKOPIA_GS_FLAG, sizeof(uint32_t));
        void *driver = write_guest(NULL, POKOPIA_NV_DRIVER_SLOT, sizeof(uint64_t));
        if (!initialized || !driver)
            stop("SDK graphics state is outside writable memory", address);
        if (!gfx_driver)
            gfx_driver = map_module(gfx_driver_address, POKOPIA_BOOT_TLS_SIZE);
#ifndef _WIN32
        /* Keep R27's arena fixed so memory traces can distinguish allocation
         * savings from a smaller Vulkan reservation. */
        if (setenv("ORBIS_ARENA_MIB", "768", 1) != 0)
            stop("could not configure the Vulkan arena", address);
        pokopia_orbis_memory_census(probe_log, "before Vulkan");
#endif
        bool ready = gfx_driver && pokopia_vk_open(&graphics, probe_log);
#ifndef _WIN32
        ready = ready && open_live_output();
        pokopia_orbis_memory_census(probe_log, "after Vulkan");
#endif
        if (!ready)
            stop("could not initialize graphics backend", address);
        pokopia_vk_ledger_report(probe_log, "after Vulkan");
        /* The exact SDK checks this pointer for presence and this flag for
         * successful boot. Host NVN calls are dispatched through nvn_front. */
        wr64(POKOPIA_NV_DRIVER_SLOT, gfx_driver_address);
        wr32(POKOPIA_GS_FLAG, 1);
        fprintf(probe_log, "Pokopia nv::InitializeGraphics bound to active Vulkan backend at %#" PRIx64 "\n", address);
        fflush(probe_log);
        kernel_unlock();
        return true;
    }
    if (!nvn)
        return false;
    kernel_lock();
    ViAdapterReply video = vi_adapter_dispatch(&vi, address, context->x);
    if (video.kind != VI_ADAPTER_NOT_OURS) {
        if (video.kind != VI_ADAPTER_HANDLED) {
            fprintf(probe_log, "VI %s %s\n",
                    video.kind == VI_ADAPTER_BOUNDARY ? "boundary" : "invalid guest argument", video.name);
            stop("VI graphics call", address);
        }
        fprintf(probe_log, "VI %s handled by the Vulkan output\n", video.name);
        fflush(probe_log);
        kernel_unlock();
        return true;
    }
    const uint32_t scalar[] = { context->v[0].w[0], context->v[1].w[0], context->v[2].w[0] };
    NvnFrontReply reply = nvn_front_dispatch_with_floats(nvn, address, context->x, scalar);
    if (reply.kind == NVN_FRONT_BOUNDARY) {
        uint64_t object = context->x[0];
        NvnFrontReplyKind window_reply = nvn_window_dispatch(windows, reply.name, context->x);
        if (window_reply != NVN_FRONT_NOT_OURS)
            reply.kind = window_reply;
        else {
            NvnFrontReplyKind frame_reply = nvn_frame_control(reply.name, context->x);
            if (frame_reply != NVN_FRONT_NOT_OURS)
                reply.kind = frame_reply;
            else {
                NvnFrontReplyKind replay_reply = dispatch_nvn_replay(context, reply.name, scalar);
                if (replay_reply != NVN_FRONT_NOT_OURS)
                    reply.kind = replay_reply;
                else {
                    NvnFrontReplyKind upload_reply =
                        pokopia_live_texture_upload_dispatch(&texture_upload, reply.name, context->x);
                    if (upload_reply != NVN_FRONT_NOT_OURS)
                        reply.kind = upload_reply;
                }
            }
        }
        NvnWindowInfo window;
        if (window_reply == NVN_FRONT_HANDLED && context->x[0] && !strcmp(reply.name, "nvnWindowInitialize") &&
            nvn_window_get(windows, object, &window)) {
            fprintf(probe_log, "NVN window render extent %ux%u textures=%u interval=%u\n",
                    window.width, window.height, window.texture_count, window.present_interval);
            fflush(probe_log);
        }
    }
    if (reply.kind == NVN_FRONT_BOUNDARY || reply.kind == NVN_FRONT_INVALID_GUEST) {
        fprintf(probe_log, "NVN %s %s x0=%#" PRIx64 " x1=%#" PRIx64 "\n",
                reply.kind == NVN_FRONT_BOUNDARY ? "boundary" : "invalid guest argument",
                reply.name, context->x[0], context->x[1]);
        if (!strncmp(reply.name, "nvnCommandBuffer", sizeof "nvnCommandBuffer" - 1))
            fprintf(probe_log, "NVN command arguments x2=%#" PRIx64 " x3=%#" PRIx64
                               " x4=%#" PRIx64 " x5=%#" PRIx64 "\n",
                    context->x[2], context->x[3], context->x[4], context->x[5]);
        if (!strncmp(reply.name, "nvnTextureWriteTexels", sizeof "nvnTextureWriteTexels" - 1))
            fprintf(probe_log, "NVN texel write region=%#" PRIx64 " source=%#" PRIx64 "\n",
                    context->x[2], context->x[3]);
        NvnFrontPoolConfig pool;
        if (reply.kind == NVN_FRONT_BOUNDARY && strcmp(reply.name, "nvnMemoryPoolInitialize") == 0 &&
            nvn_front_read_pool_builder(nvn, context->x[1], &pool)) {
            fprintf(probe_log, "NVN memory pool device=%#" PRIx64 " storage=%#" PRIx64
                    " size=%#" PRIx64 " flags=%#" PRIx32 "\n",
                    pool.device, pool.storage, pool.size, pool.flags);
        }
        if (reply.kind == NVN_FRONT_BOUNDARY) {
            uint64_t builder = 0;
            if (!strcmp(reply.name, "nvnTextureBuilderGetStorageAlignment") ||
                !strcmp(reply.name, "nvnTextureBuilderGetStorageSize"))
                builder = context->x[0];
            else if (!strcmp(reply.name, "nvnTextureInitialize"))
                builder = context->x[1];
            NvnFrontTextureConfig texture;
            if (builder && nvn_front_read_texture_builder(nvn, builder, &texture)) {
                fprintf(probe_log, "NVN texture %ux%ux%u target=%#x format=%#x flags=%#x levels=%u samples=%u"
                        " stride=%u pool=%#" PRIx64 " offset=%#" PRIx64 "\n",
                        texture.width, texture.height, texture.depth, texture.target, texture.format,
                        texture.flags, texture.levels, texture.samples, texture.stride, texture.pool, texture.offset);
            }
        }
        stop("NVN graphics call", address);
    }
    kernel_unlock();
    return reply.kind == NVN_FRONT_HANDLED;
}
#endif

static uint32_t copy_aoc_event(void *opaque)
{
    (void)opaque;
    return hzn_events_copy_read(events, AOC_LIST_EVENT);
}

static uint32_t copy_shared_read_handle(void *opaque)
{
    SharedRead *shared = opaque;
    if (shared->next_handle == SHARED_READ_HANDLE_COUNT)
        return 0;
    shared->handles[shared->next_handle] = true;
    return shared->handle_base + (uint32_t)shared->next_handle++;
}

static bool init_clock_service(HznTime *time)
{
    if (!clock_initialized) {
        uint64_t source[2];
        if (!hzn_get_info_entropy_seed(&source[0]) || !hzn_get_info_entropy_seed(&source[1]) ||
            !pokopia_boot_sample_clocks(&clock_config.tick_ns, &clock_config.utc_ns))
            return false;
        memcpy(clock_config.source_id, source, sizeof source);
        clock_config.opaque = &shared_clock;
        clock_config.copy_shared_read = copy_shared_read_handle;
        clock_config.write_guest = write_guest;
        if (!hzn_time_init_page(clock_page, sizeof clock_page, &clock_config))
            return false;
        clock_initialized = true;
    }
    return hzn_time_init(time, &clock_config);
}

static bool map_shared_read(SharedRead *shared, uint64_t address)
{
    if (!shared->live)
        return hzn_readonly_span_map(&shared->mapping, address, shared->size, shared->data);
    if (!hzn_live_shared_map(shared->live, address, shared->size))
        return false;
#ifdef POKOPIA_GFX
    if (shared == &shared_hid) {
        if (!hzn_hid_memory_init(shared->live->writer, shared->size)) {
            hzn_live_shared_unmap(shared->live);
            return false;
        }
#ifndef _WIN32
        if (native_pad < 0) {
            native_pad = hzn_hid_ps4_open();
            fprintf(probe_log, "PS4 pad open result=%#x\n", (uint32_t)native_pad);
        }
#endif
        update_guest_pad();
    }
#endif
    return true;
}

static bool shared_memory_svc(Ctx *context, uint32_t number)
{
    if (number != 0x13 && number != 0x14 && number != 0x16)
        return false;
    const uint32_t handle = (uint32_t)context->x[0];
    SharedRead *const ranges[] = { &shared_clock,
#ifdef POKOPIA_GFX
        &shared_streamplay, &shared_hid,
#endif
    };
    SharedRead *shared = NULL;
    for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
        if (handle >= ranges[i]->handle_base && handle - ranges[i]->handle_base < SHARED_READ_HANDLE_COUNT) {
            shared = ranges[i];
            break;
        }
    }
    if (!shared) {
        if (number == 0x16)
            return false;
        context->x[0] = 0xe401;
        return true;
    }
    const size_t index = handle - shared->handle_base;
    const bool open = shared->handles[index];
    if (number == 0x16) {
        context->x[0] = open ? 0 : 0xe401;
        shared->handles[index] = false;
        return true;
    }
    uint64_t address = context->x[1], size = context->x[2];
    const uint64_t mapped_address = shared->live ? shared->live->address : shared->mapping.address;
    uint32_t result = 0;
    if (!open)
        result = 0xe401; /* InvalidHandle */
    else if ((address & (HZN_READONLY_PAGE_BYTES - 1)) || address < 0x8000000 ||
             address > guest_limit - shared->size)
        result = HZN_STACK_MAP_INVALID_ADDRESS;
    else if (size != shared->size)
        result = HZN_STACK_MAP_INVALID_SIZE;
    else if (number == 0x14) {
        if (mapped_address != address || (shared->live ? !hzn_live_shared_unmap(shared->live) :
            !hzn_readonly_page_unmap(&shared->mapping)))
            result = HZN_STACK_MAP_INVALID_MEMORY_STATE;
    } else if ((uint32_t)context->x[3] != 1) {
        result = 0xd801; /* InvalidNewMemoryPermission */
    } else {
        const MemoryInfo memory = query_memory(address);
        if (memory.type || size > memory.base + memory.size - address || mapped_address ||
            !map_shared_read(shared, address))
            result = HZN_STACK_MAP_INVALID_MEMORY_STATE;
    }
    fprintf(probe_log, "%s read-only shared memory handle=%#" PRIx32 " address=%#" PRIx64 " size=%#" PRIx64
            " result=%#" PRIx32 " host=%#" PRIx32 "\n", number == 0x13 ? "MapSharedMemory" : "UnmapSharedMemory",
            handle, address, size, result,
            (uint32_t)(shared->live ? shared->live->last_host_error : shared->mapping.last_host_error));
    fflush(probe_log);
    if (shared->live ? shared->live->unsafe : shared->mapping.unsafe)
        stop("could not roll back read-only shared memory mapping", address);
    context->x[0] = result;
    return true;
}

static bool transfer_memory_owner(uint64_t address, uint64_t size)
{
    const MemoryInfo memory = query_memory(address);
    return (memory.type == 4 || memory.type == 5) && memory.permission == 3 && !memory.attribute &&
           size <= memory.base + memory.size - address;
}

static bool transfer_memory_svc(Ctx *context, uint32_t number)
{
    uint32_t result;
    if (number == 0x16) {
        if (!hzn_transfer_memory_close(&transfer_memory, (uint32_t)context->x[0], &result))
            return false;
        context->x[0] = result;
        return true;
    }
    if (number != 0x15)
        return false;

    const uint64_t address = context->x[1], size = context->x[2];
    const uint32_t permission = (uint32_t)context->x[3];
    uint32_t handle;
    result = hzn_transfer_memory_create(&transfer_memory, address, size, permission,
                                        transfer_memory_owner, &handle);
    context->x[0] = result;
    context->x[1] = handle;
    static unsigned logged;
    if (result || logged < 4) {
        fprintf(probe_log, "CreateTransferMemory owner=%#" PRIx64 " size=%#" PRIx64
                " permission=%#" PRIx32 " result=%#" PRIx32 " handle=%#" PRIx32 "\n",
                address, size, permission, result, handle);
        if (logged < 4) {
            ++logged;
            log_guest_frames(context);
        }
        fflush(probe_log);
    }
    return true;
}

static uint32_t protect_module_data(uint32_t number, uint64_t address, uint64_t size,
                                    uint32_t value, uint32_t extra)
{
    if (address & 0xfff)
        return HZN_STACK_MAP_INVALID_ADDRESS;
    if (!size || (size & 0xfff))
        return HZN_STACK_MAP_INVALID_SIZE;
    MemoryInfo region = query_memory(address);
    if (region.type != 4 || size > region.base + region.size - address || region.attribute)
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;

    CodeProtection *slot = NULL;
    uint64_t end = address + size;
    for (size_t i = 0; i < sizeof protected_code / sizeof protected_code[0]; ++i) {
        CodeProtection *candidate = &protected_code[i];
        if (candidate->start == address && candidate->end == end) {
            slot = candidate;
            break;
        }
        if (candidate->start && candidate->start < end && address < candidate->end)
            return HZN_STACK_MAP_INVALID_MEMORY_STATE;
        if (!candidate->start && !slot)
            slot = candidate;
    }
    if (!slot)
        return HZN_STACK_MAP_OUT_OF_MEMORY;

    if (number == 3) {
        if (value != 0x10 || extra != 0x10)
            return 0xe801; /* InvalidCombination */
        *slot = (CodeProtection){ address, end, region.permission, 0x10 };
        return 0;
    }
    if (value != 0 && value != 1 && value != 3)
        return 0xd801; /* InvalidNewMemoryPermission */
    uint64_t mask = pokopia_host_page_size() - 1;
    uint64_t host_start = address & ~mask, host_end = (end + mask) & ~mask;
    /* The neighbouring guest pages on a larger host page must already
     * have the same access or the native protection would change them. */
    if (host_start != address) {
        MemoryInfo first = query_memory(host_start);
        if (first.permission != value || first.base + first.size < address)
            return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    }
    if (host_end != end) {
        MemoryInfo last = query_memory(end);
        if (last.permission != value || last.base + last.size < host_end)
            return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    }
    if (!pokopia_host_protect(host_start, host_end - host_start, value))
        return HZN_STACK_MAP_INVALID_MEMORY_STATE;
    *slot = (CodeProtection){ address, end, value, 0 };
    return 0;
}

static ServiceSession *service_session(uint32_t handle)
{
    for (size_t i = 0; i < sizeof service_sessions / sizeof service_sessions[0]; ++i)
        if (service_sessions[i].handle == handle)
            return &service_sessions[i];
    return NULL;
}

static void close_service(ServiceSession *service)
{
#ifdef POKOPIA_HWOPUS
    if (service->type == SERVICE_HWOPUS)
        hzn_hwopus_close(&service->state.hwopus);
#endif
#ifdef POKOPIA_AUDIO_OUT
    if (service->type == SERVICE_AUDIO_OUT)
        pokopia_audio_out_close(service->state.audio_out);
#endif
#ifdef POKOPIA_GFX
    if (service->type == SERVICE_BSD_SSL)
        hzn_bsd_ssl_close(&service->state.bsd_ssl);
    if ((service->type == SERVICE_FSP || service->type == SERVICE_ACCOUNT || service->type == SERVICE_OFFLINE_APP) &&
        service->state.shared && --service->state.shared->refs == 0) {
        if (service->type == SERVICE_FSP)
            hzn_fsp_close(&service->state.shared->service.fsp);
#ifdef _WIN32
        free(service->state.shared->root);
#endif
        free(service->state.shared);
    }
#endif
    *service = (ServiceSession){0};
}

#ifdef POKOPIA_GFX
static uint32_t clone_bsd_ssl_session(void *opaque, const HznBsdSsl *source)
{
    (void)opaque;
    ServiceSession *slot = service_session(0);
    const uint32_t handle = slot ? hzn_sm_allocate_handle(&service_manager) : 0;
    if (handle) {
        *slot = (ServiceSession){ .handle = handle, .type = SERVICE_BSD_SSL };
        hzn_bsd_ssl_clone(&slot->state.bsd_ssl, source);
    }
    return handle;
}

static uint32_t open_stpl_user(void *opaque)
{
    (void)opaque;
    ServiceSession *slot = service_session(0);
    const uint32_t handle = slot ? hzn_sm_allocate_handle(&service_manager) : 0;
    if (handle)
        *slot = (ServiceSession){ .handle = handle, .type = SERVICE_STPL_USER };
    return handle;
}

static uint32_t open_hid_object(ServiceType type)
{
    ServiceSession *slot = service_session(0);
    const uint32_t handle = slot ? hzn_sm_allocate_handle(&service_manager) : 0;
    if (handle)
        *slot = (ServiceSession){ .handle = handle, .type = type };
    return handle;
}

static uint32_t open_hid_resource(void *opaque, uint64_t aruid)
{
    (void)opaque;
    return aruid == hid_state.aruid ? open_hid_object(SERVICE_HID_RESOURCE) : 0;
}

static uint32_t open_hid_vibration_list(void *opaque)
{
    (void)opaque;
    return open_hid_object(SERVICE_HID_VIBRATION_LIST);
}

#ifndef _WIN32
static bool vibrate_hid_pad(void *opaque, float amplitude_low, float amplitude_high)
{
    (void)opaque;
    return hzn_hid_ps4_vibrate(native_pad, amplitude_low, amplitude_high);
}
#endif

static uint32_t copy_input_memory(void *opaque, size_t bytes)
{
    SharedRead *shared = opaque;
    return bytes == shared->size ? copy_shared_read_handle(shared) : 0;
}
#endif

#ifdef POKOPIA_HWOPUS
static uint32_t clone_hwopus_session(void *opaque, const HznHwopus *source)
{
    (void)opaque;
    ServiceSession *slot = service_session(0);
    const uint32_t handle = slot ? hzn_sm_allocate_handle(&service_manager) : 0;
    if (handle) {
        *slot = (ServiceSession){ .handle = handle, .type = SERVICE_HWOPUS };
        hzn_hwopus_clone(&slot->state.hwopus, source);
    }
    return handle;
}
#endif

#ifdef POKOPIA_AUDIO_OUT
static void send_audio(ServiceSession *service, uint8_t *tls, uint32_t request_type)
{
    const bool user = service->type == SERVICE_AUDIO_USER;
    const bool renderer_manager = service->type == SERVICE_AUDIO_RENDERER_MANAGER;
    const bool device = service->type == SERVICE_AUDIO_DEVICE;
    const bool out_manager = service->type == SERVICE_AUDIO_OUTPUT_MANAGER;
    const bool out = service->type == SERVICE_AUDIO_OUT;
    if ((request_type == 2 || request_type == 15) && !hzn_cmif_read32(tls + 4)) {
        memset(tls, 0, 8);
        return;
    }

    HznCmifRequest request;
    const bool parsed = hzn_cmif_parse(tls, 0x100, &request) || ((device || out) &&
        (hzn_cmif_parse_with_output(tls, 0x100, &request) || hzn_cmif_parse_with_auto_output(tls, 0x100, &request))) ||
        (out_manager && hzn_cmif_parse_with_input_output(tls, 0x100, &request));
    const bool cmif = parsed && request.raw_size >= 16 && hzn_cmif_read32(request.raw) == HZN_SFCI;
    const uint32_t command = cmif ? hzn_cmif_read32(request.raw + 8) : UINT32_MAX;
    const bool simple = cmif && !request.has_pid && !request.has_copy_handle && !request.has_buffer_descriptors;
    if (out && simple && (request.type == 5 || request.type == 7) && command == 3) {
        const uint32_t pointer_buffer_size = 0x8000;
        if (!hzn_cmif_reply(tls, 0x100, false, 0, &pointer_buffer_size))
            stop("audio pointer buffer reply failed", command);
        return;
    }
    const bool valid = cmif && (request.type == 4 || request.type == 6);
    const PokopiaAudioGuest audio_guest = { .read = read_guest, .write = write_guest };
    if (out && valid && pokopia_audio_out_request(service->state.audio_out, command, &request,
                                                tls, 0x100, &audio_guest))
        return;
    if (out_manager && valid && command == 1) {
        ServiceSession *child = service_session(0);
        const uint32_t handle = child ? hzn_sm_allocate_handle(&service_manager) : 0;
        PokopiaAudioOut *stream;
        if (pokopia_audio_out_open(&request, tls, 0x100, &audio_guest, events, handle, &stream)) {
            if (stream) {
                *child = (ServiceSession){ .type = SERVICE_AUDIO_OUT, .handle = handle, .state.audio_out = stream };
                fprintf(probe_log, "Opened IAudioOut stereo PCM session %#" PRIx32 "\n", handle);
                fflush(probe_log);
            }
            return;
        }
    }
    if (device && valid && pokopia_audio_device_request(command, &request, tls, 0x100, events, write_guest))
        return;

    const bool open_manager = user && (command == 1 || command == 2);
    const bool open_device = renderer_manager && (command == 2 || (command == 4 && request.raw_size >= 32));
    if (valid && simple && (open_manager || open_device)) {
        ServiceSession *child = service_session(0);
        const uint32_t handle = child ? hzn_sm_allocate_handle(&service_manager) : 0;
        if (!hzn_cmif_reply_move_handle(tls, 0x100, false, handle ? 0 : 0xce01, handle))
            stop("audio service reply failed", command);
        if (handle)
            *child = (ServiceSession){
                .type = open_device ? SERVICE_AUDIO_DEVICE :
                    command == 1 ? SERVICE_AUDIO_OUTPUT_MANAGER : SERVICE_AUDIO_RENDERER_MANAGER,
                .handle = handle,
            };
        return;
    }
    fprintf(probe_log, "Audio session type=%d command=%#" PRIx32 " request=%#" PRIx32 "\n",
            service->type, command, request_type);
    stop("unsupported audio request", command);
}
#endif

#ifdef POKOPIA_GFX
static uint32_t clone_shared(SharedSession *shared, ServiceType type)
{
    ServiceSession *slot = service_session(0);
    if (!slot)
        return 0;
    uint32_t handle = hzn_sm_allocate_handle(&service_manager);
    if (!handle)
        return 0;
    slot->handle = handle;
    slot->type = type;
    slot->state.shared = shared;
    ++shared->refs;
    return handle;
}

static uint32_t clone_fsp(void *opaque, HznFsp *source)
{
    SharedSession *shared = opaque;
    return source == &shared->service.fsp ? clone_shared(shared, SERVICE_FSP) : 0;
}

static uint32_t clone_account(void *opaque, HznAccount *source)
{
    SharedSession *shared = opaque;
    return source == &shared->service.account ? clone_shared(shared, SERVICE_ACCOUNT) : 0;
}

static uint32_t clone_offline_app(void *opaque, HznOfflineApp *source)
{
    SharedSession *shared = opaque;
    return source == &shared->service.offline_app ? clone_shared(shared, SERVICE_OFFLINE_APP) : 0;
}

static bool init_offline_app(ServiceSession *slot, HznOfflineAppKind kind)
{
    SharedSession *shared = calloc(1, sizeof *shared);
    if (!shared)
        return false;
    shared->refs = 1;
    hzn_offline_app_init(&shared->service.offline_app, kind, clone_offline_app, shared);
    slot->state.shared = shared;
    return true;
}

static bool init_fsp(ServiceSession *slot)
{
    SharedSession *shared = calloc(1, sizeof *shared);
    if (!shared)
        return false;
#ifdef _WIN32
    size_t root_size;
    if (_dupenv_s(&shared->root, &root_size, "POKOPIA_ROMFS_ROOT") != 0) {
        free(shared);
        return false;
    }
    const char *root = shared->root;
#else
    const char *root = "/data/pokopia/romfs";
#endif
    hzn_fsp_init(&shared->service.fsp, root, write_guest, clone_fsp, shared);
    hzn_fsp_set_read_log(&shared->service.fsp, probe_log);
    hzn_fsp_set_save_root(&shared->service.fsp, account_save_root, read_guest);
    shared->refs = 1;
    slot->state.shared = shared;
    return true;
}

static uint32_t ensure_applet_save(void *opaque, uint64_t uid_hi, uint64_t uid_lo)
{
    (void)opaque;
    if (uid_hi != HZN_OFFLINE_UID_WORD0 || uid_lo != HZN_OFFLINE_UID_WORD1)
        return 0x7d402; /* Save target not found for this user. */
    const uint32_t result = hzn_save_ensure_account(account_save_root, uid_hi, uid_lo);
    fprintf(probe_log, "Pokopia account save ensure result=%#" PRIx32 "\n", result);
    fflush(probe_log);
    return result;
}

static uint32_t get_applet_save_size(void *opaque, uint64_t uid_hi, uint64_t uid_lo,
                                     int64_t *normal, int64_t *journal)
{
    (void)opaque;
    if (uid_hi != HZN_OFFLINE_UID_WORD0 || uid_lo != HZN_OFFLINE_UID_WORD1)
        return 0x7d402;
    return hzn_save_account_size(account_save_root, uid_hi, uid_lo, normal, journal);
}

static bool init_account(ServiceSession *slot)
{
    SharedSession *shared = calloc(1, sizeof *shared);
    if (!shared)
        return false;
    HznAccount *account = &shared->service.account;
    hzn_account_init(account);
    account->clone = clone_account;
    account->clone_opaque = shared;
    shared->refs = 1;
    slot->state.shared = shared;
    return true;
}
#endif

static uint32_t copy_applet_event(void *opaque, HznAppletEvent event)
{
    (void)opaque;
    uint32_t handle = hzn_events_copy_read(events, event);
    if (handle && event == HZN_APPLET_GPU_ERROR)
        gpu_error_event = handle;
    return handle;
}

static uint32_t receive_applet_message(void *opaque, uint32_t *message)
{
    (void)opaque;
    if (!applet_focus_pending)
        return HZN_APPLET_NO_MESSAGES;
    if (!hzn_events_set_signaled(events, HZN_APPLET_MESSAGE, false))
        stop("could not clear the applet message event", HZN_APPLET_MESSAGE);
    applet_focus_pending = false;
    *message = 15; /* FocusStateChanged for the initial foreground application. */
    return 0;
}

static bool copy_wait_handles(void *opaque, uint64_t address, uint32_t *out, size_t count)
{
    (void)opaque;
    if (address % sizeof *out)
        return false;
    kernel_lock();
    const void *handles = read_guest(NULL, address, count * sizeof *out);
    if (handles)
        memcpy(out, handles, count * sizeof *out);
    kernel_unlock();
    return handles != NULL;
}

#ifdef POKOPIA_GFX
static void *get_hid_transfer_memory(void *opaque, uint32_t handle, uint64_t size)
{
    (void)opaque;
    const HznTransferMemory *memory = hzn_transfer_memory_lookup(&transfer_memory, handle);
    if (!memory || memory->permission != 1 || size > memory->size || size > SIZE_MAX)
        return NULL;
    return write_guest(NULL, memory->address, (size_t)size);
}
#endif

static bool service_manager_svc(Ctx *context, uint32_t number)
{
    if (number == 0x1f) {
        uint64_t address = context->x[1];
        MemoryInfo region = query_memory(address);
        if (!(region.permission & 1))
            stop("invalid named-port address", address);
        uint64_t remaining = region.base + region.size - address;
        size_t bytes = remaining < 12 ? (size_t)remaining : 12;
        uint32_t handle;
        const char *port = (const char *)(uintptr_t)address;
        context->x[0] = hzn_sm_connect(&service_manager, port, bytes, &handle);
        context->x[1] = handle;
        fprintf(probe_log, "ConnectToNamedPort(%.*s) result=%#" PRIx64 " handle=%#" PRIx32 "\n",
                (int)bytes, port, context->x[0], handle);
        fflush(probe_log);
        return true;
    }
    uint32_t handle = (uint32_t)context->x[0];
    if ((number != 0x16 && number != 0x21) ||
        !hzn_sm_owns_handle(&service_manager, handle))
        return false;
    ServiceSession *service = service_session(handle);
    if (number == 0x16) {
        if (service) {
            close_service(service);
            context->x[0] = 0;
        } else {
            context->x[0] = hzn_sm_close_handle(&service_manager, handle);
        }
        return true;
    }

    MemoryInfo tls_region = query_memory(context->tpidrro);
    if (tls_region.type != 0x0c || tls_region.base != context->tpidrro)
        stop("unexpected IPC TLS address", context->tpidrro);
    unsigned char *tls = (unsigned char *)(uintptr_t)context->tpidrro;
    uint32_t request_type = rd32(context->tpidrro) & 0xffffu;
#ifdef POKOPIA_HWOPUS
    if (service && service->type == SERVICE_HWOPUS) {
        const HznHwopusReply reply = hzn_hwopus_send_sync(&service->state.hwopus, tls, 0x100);
        if (!reply.handled)
            stop("unsupported Opus command", reply.command);
        context->x[0] = reply.result;
        return true;
    }
#endif
#ifdef POKOPIA_AUDIO_OUT
    if (service && service->type >= SERVICE_AUDIO_USER && service->type <= SERVICE_AUDIO_OUT) {
        send_audio(service, tls, request_type);
        context->x[0] = 0;
        return true;
    }
#endif
#ifdef POKOPIA_GFX
    if (service && service->type == SERVICE_BSD_SSL) {
        const HznBsdSslReply reply = hzn_bsd_ssl_send_sync(&service->state.bsd_ssl, tls, 0x100);
        if (!reply.handled)
            stop("unsupported socket or TLS service command", reply.command);
        context->x[0] = reply.result;
        return true;
    }
    if (service && service->type == SERVICE_OFFLINE_APP) {
        const HznOfflineAppReply reply = hzn_offline_app_send(&service->state.shared->service.offline_app, tls, 0x100);
        if (reply.boundary_type) {
            fprintf(probe_log, "%s unsupported command %#" PRIx32 " object=%#" PRIx32 "\n",
                    reply.boundary_type, reply.command, reply.object_id);
            stop("unsupported offline application service command", reply.command);
        }
        context->x[0] = reply.result;
        return true;
    }
    if (service && (service->type == SERVICE_STPL || service->type == SERVICE_STPL_USER)) {
        const HznStplConfig config = { .opaque = &shared_streamplay, .open_user = open_stpl_user,
                                      .copy_hid_memory = copy_input_memory };
        const HznStplReply reply = hzn_stpl_send_sync(service->type == SERVICE_STPL_USER, &config, tls, 0x100);
        if (!reply.handled)
            stop("unsupported streamplay command", reply.command);
        context->x[0] = reply.result;
        return true;
    }
    if (service && service->type >= SERVICE_HID && service->type <= SERVICE_HID_VIBRATION_LIST) {
        const HznHidConfig config = { .opaque = &shared_hid, .read_guest = read_guest,
            .open_applet_resource = open_hid_resource, .copy_hid_memory = copy_input_memory,
            .open_vibration_list = open_hid_vibration_list, .get_transfer_memory = get_hid_transfer_memory,
#ifndef _WIN32
            .vibrate = vibrate_hid_pad,
#endif
        };
        const HznHidInterface endpoint = service->type == SERVICE_HID ? HZN_HID_SERVER :
            service->type == SERVICE_HID_RESOURCE ? HZN_HID_APPLET_RESOURCE : HZN_HID_VIBRATION_LIST;
        const HznHidReply reply = hzn_hid_send_sync(&hid_state, endpoint, &config, tls, 0x100);
        if (!reply.handled)
            stop("unsupported HID command", reply.command);
        update_guest_pad();
        context->x[0] = reply.result;
        return true;
    }
    if (service && service->type == SERVICE_FSP) {
        HznFsp *fsp = &service->state.shared->service.fsp;
        bool was_verified = fsp->romfs_verified;
        HznFspReply reply = hzn_fsp_send_sync(fsp, tls, 0x100);
        if (reply.kind == HZN_FSP_BOUNDARY) {
            fprintf(probe_log, "fsp-srv unsupported %s command %#" PRIx32 " object=%#" PRIx32 "\n",
                    reply.object_type, reply.command, reply.object_id);
            stop("unsupported filesystem command", reply.command);
        }
        if (!was_verified && fsp->romfs_verified) {
            fputs("Pokopia RomFS opened from verified local game data\n", probe_log);
            fflush(probe_log);
        }
        context->x[0] = reply.result;
        return true;
    }
    if (service && service->type == SERVICE_ACCOUNT) {
        HznAccountReply reply = hzn_account_send_sync(&service->state.shared->service.account, tls, 0x100);
        if (reply.kind == HZN_ACCOUNT_BOUNDARY) {
            fprintf(probe_log, "acc:u0 unsupported %s command %#" PRIx32 " object=%#" PRIx32 "\n",
                    reply.object_type, reply.command, reply.object_id);
            stop("unsupported account command", reply.command);
        }
        context->x[0] = reply.result;
        return true;
    }
#endif
    if (service && service->type == SERVICE_APPLET) {
        HznApplet *applet = &service->state.applet;
        const uint32_t previous_terminate_result = applet->terminate_result;
        HznAppletReply reply = hzn_applet_send_sync(applet, tls, 0x100);
        if (reply.kind == HZN_APPLET_BOUNDARY) {
            /* The worker probe measures both threads through this point in
             * InitializeApplet and leaves the following IPC request untouched. */
            if (context == &cpu && reply.command == 2400 && reply.object_type &&
                strcmp(reply.object_type, "ISelfController") == 0) {
                fputs("SDK main reached ISelfController command 2400 after foreground setup\n", probe_log);
                finish_guest(1);
            }
            fprintf(probe_log, "appletOE: unsupported command %#" PRIx32 " on %s %#" PRIx32 "\n",
                    reply.command, reply.object_type, reply.object_id);
            stop("unsupported applet-service request", reply.command);
        }
        context->x[0] = reply.result;
        if (applet->terminate_result != previous_terminate_result)
            fprintf(probe_log, "appletOE: application terminate result=%#" PRIx32 "\n", applet->terminate_result);
        fprintf(probe_log, "appletOE: HIPC request %#" PRIx32 " SVC result=%#" PRIx32 "\n",
                request_type, reply.result);
        fflush(probe_log);
        return true;
    }
    if (service && service->type == SERVICE_AOC) {
        HznAocReply reply = hzn_aoc_send_sync(&service->state.aoc, tls, 0x100);
        if (reply.kind == HZN_AOC_BOUNDARY)
            stop("unsupported add-on content command", reply.command);
        context->x[0] = reply.result;
        fprintf(probe_log, "aoc:u: HIPC request %#" PRIx32 " SVC result=%#" PRIx32 "\n",
                request_type, reply.result);
        fflush(probe_log);
        return true;
    }
    if (service && service->type == SERVICE_PCTL) {
        HznPctlReply reply = hzn_pctl_send_sync(&service->state.pctl, tls, 0x100);
        if (reply.kind == HZN_PCTL_BOUNDARY)
            stop("unsupported parental control command", reply.command);
        context->x[0] = reply.result;
        fprintf(probe_log, "pctl: HIPC request %#" PRIx32 " SVC result=%#" PRIx32 "\n",
                request_type, reply.result);
        fflush(probe_log);
        return true;
    }
    if (service && service->type == SERVICE_TIME) {
        HznTimeReply reply = hzn_time_send_sync(&service->state.time, tls, 0x100);
        if (reply.kind == HZN_TIME_BOUNDARY)
            stop("unsupported clock service command", reply.command);
        context->x[0] = reply.result;
        fprintf(probe_log, "time:u: HIPC request %#" PRIx32 " SVC result=%#" PRIx32 "\n",
                request_type, reply.result);
        fflush(probe_log);
        return true;
    }
    if (service && service->type == SERVICE_ECTX) {
        HznCmifRequest request;
        uint32_t command = UINT32_MAX;
        bool domain = service->state.ectx_domain && (request_type == 4 || request_type == 6);
        size_t offset = domain ? 16 : 0;
        if (hzn_cmif_parse(tls, 0x100, &request) && request.raw_size >= offset + 16 &&
            hzn_cmif_read32(request.raw + offset) == HZN_SFCI)
            command = hzn_cmif_read32(request.raw + offset + 8);
        if (request_type == 7 && (command == 0 || command == 3) &&
            !request.has_pid && !request.has_copy_handle && !request.has_buffer_descriptors) {
            uint32_t data = command == 0 ? 1 : 0x8000;
            if (command == 0 && service->state.ectx_domain)
                stop("service session already converted to domain", handle);
            if (!hzn_cmif_reply(tls, 0x100, false, 0, &data))
                stop("service control reply failed", command);
            if (command == 0)
                service->state.ectx_domain = true;
            context->x[0] = 0;
            return true;
        }
        fprintf(probe_log, "ectx:aw: reached HIPC type=%#" PRIx32 " command=%#" PRIx32 " domain=%d\n",
                request_type, command, domain);
        stop("service IPC requires implementation", command);
    }
    if (service) {
        HznLmReply reply = hzn_lm_send_sync(&service->state.lm, tls, 0x100, read_guest, NULL, probe_log);
        if (reply.kind == HZN_LM_BOUNDARY) {
            fprintf(probe_log, "lm: unsupported command %#" PRIx32 " on object %#" PRIx32 "\n",
                    reply.command, reply.object_id);
            stop("unsupported logging-service request", reply.command);
        }
        context->x[0] = reply.result;
        fprintf(probe_log, "lm: HIPC request %#" PRIx32 " SVC result=%#" PRIx32 "\n",
                request_type, reply.result);
        fflush(probe_log);
        return true;
    }
    HznSmReply reply = hzn_sm_send_sync(&service_manager, handle, tls, 0x100);
    if (reply.kind == HZN_SM_SERVICE_BOUNDARY) {
        ServiceType type;
#ifdef POKOPIA_GFX
        const bool ssl = strcmp(reply.service, "ssl:s") == 0;
        const bool bcat = strcmp(reply.service, "bcat:u") == 0;
#endif
        if (strcmp(reply.service, "lm") == 0) {
            type = SERVICE_LM;
        } else if (strcmp(reply.service, "appletOE") == 0) {
            type = SERVICE_APPLET;
        } else if (strcmp(reply.service, "aoc:u") == 0) {
            type = SERVICE_AOC;
        } else if (strcmp(reply.service, "pctl") == 0 || strcmp(reply.service, "pctl:s") == 0 ||
                   strcmp(reply.service, "pctl:a") == 0 || strcmp(reply.service, "pctl:r") == 0) {
            type = SERVICE_PCTL;
        } else if (strcmp(reply.service, "time:u") == 0) {
            type = SERVICE_TIME;
        } else if (strcmp(reply.service, "ectx:aw") == 0) {
            type = SERVICE_ECTX;
#ifdef POKOPIA_AUDIO_OUT
        } else if (strcmp(reply.service, "aud:u") == 0) {
            type = SERVICE_AUDIO_USER;
#endif
#ifdef POKOPIA_HWOPUS
        } else if (strcmp(reply.service, "hwopus") == 0) {
            type = SERVICE_HWOPUS;
#endif
#ifdef POKOPIA_GFX
        } else if (ssl || strcmp(reply.service, "bsd:s") == 0) {
            type = SERVICE_BSD_SSL;
        } else if (bcat || strcmp(reply.service, "nifm:u") == 0) {
            type = SERVICE_OFFLINE_APP;
        } else if (strcmp(reply.service, "stpl:u") == 0) {
            type = SERVICE_STPL;
        } else if (strcmp(reply.service, "hid") == 0) {
            type = SERVICE_HID;
        } else if (strcmp(reply.service, "fsp-srv") == 0) {
            type = SERVICE_FSP;
        } else if (strcmp(reply.service, "acc:u0") == 0) {
            type = SERVICE_ACCOUNT;
#endif
        } else {
            fprintf(probe_log, "SDK requested unimplemented service %s\n", reply.service);
            stop("unimplemented service", handle);
        }
        ServiceSession *slot = service_session(0);
        if (!slot)
            stop("service session table full", handle);
        fprintf(probe_log, "sm: opening %s\n", reply.service);
        reply = hzn_sm_move_service(&service_manager, tls, 0x100, &slot->handle);
        if (slot->handle) {
            slot->type = type;
            if (type == SERVICE_LM) {
                hzn_lm_init(&slot->state.lm, guest_pid);
            } else if (type == SERVICE_APPLET) {
                hzn_applet_init(&slot->state.applet, guest_pid);
                slot->state.applet.copy_event = copy_applet_event;
                slot->state.applet.receive_message = receive_applet_message;
                slot->state.applet.write_guest = write_guest;
#ifdef POKOPIA_GFX
                slot->state.applet.ensure_save = ensure_applet_save;
                slot->state.applet.get_save_size = get_applet_save_size;
                slot->state.applet.save_data_limit = HZN_SAVE_ACCOUNT_DATA_LIMIT;
                slot->state.applet.save_journal_limit = HZN_SAVE_ACCOUNT_JOURNAL_LIMIT;
#endif
            } else if (type == SERVICE_AOC) {
                if (!hzn_aoc_init(&slot->state.aoc, NULL, 0))
                    stop("could not initialize add-on content service", slot->handle);
                slot->state.aoc.write_guest = write_guest;
                slot->state.aoc.copy_event = copy_aoc_event;
            } else if (type == SERVICE_PCTL) {
                hzn_pctl_init(&slot->state.pctl);
            } else if (type == SERVICE_TIME && !init_clock_service(&slot->state.time)) {
                stop("could not initialize the clock service", slot->handle);
            }
#ifdef POKOPIA_HWOPUS
            if (type == SERVICE_HWOPUS) {
                const HznHwopusConfig config = { .read_guest = read_guest, .clone_session = clone_hwopus_session };
                hzn_hwopus_init(&slot->state.hwopus, &config);
            }
#endif
#ifdef POKOPIA_GFX
            if (type == SERVICE_BSD_SSL) {
                const HznBsdSslConfig config = { .guest_pid = guest_pid, .clone_session = clone_bsd_ssl_session };
                if (!hzn_bsd_ssl_init(&slot->state.bsd_ssl, ssl ? HZN_NET_BOOT_SSL : HZN_NET_BOOT_BSD, &config))
                    stop("could not initialize socket or TLS service", slot->handle);
            }
            if (type == SERVICE_OFFLINE_APP &&
                !init_offline_app(slot, bcat ? HZN_OFFLINE_BCAT_CREATOR : HZN_OFFLINE_NIFM_STATIC))
                stop("could not initialize offline application service", slot->handle);
            if (type == SERVICE_FSP && !init_fsp(slot))
                stop("could not initialize the filesystem service", slot->handle);
            if (type == SERVICE_ACCOUNT && !init_account(slot))
                stop("could not initialize the account service", slot->handle);
#endif
        }
    }
    if (reply.kind == HZN_SM_COMMAND_BOUNDARY)
        stop("unsupported service-manager command", reply.command);
    context->x[0] = reply.result;
    fprintf(probe_log, "sm: TIPC request %#" PRIx32 " SVC result=%#" PRIx32 "\n",
            request_type, reply.result);
    fflush(probe_log);
    return true;
}
#endif

void guest_svc(Ctx *context, uint32_t number)
{
#ifdef POKOPIA_BOOT_PROBE
    bool system_worker_create = context == &cpu && number == 0x08 && context->x[1] == sdk_base + 0x1507c0 &&
                                context->x[2] == sdk_base + 0xe13000;
    if (hzn_thread_svc(threads, context, number)) {
        if (system_worker_create && context->x[0] == 0)
            system_worker_id = hzn_thread_id(threads, context, (uint32_t)context->x[1]);
        if (number == 0x09 && context == &cpu && context->x[0] == 0)
            worker_started = true;
        if (number == 0x08 || number == 0x09 || number == 0x0f) {
            fprintf(probe_log, "Thread SVC %#" PRIx32 " result=%#" PRIx64 " x1=%#" PRIx64 "\n",
                    number, context->x[0], context->x[1]);
            fflush(probe_log);
        }
        return;
    }
    uint64_t waiter_id = 0;
    if (number == 0x19) {
        uint32_t handle = (uint32_t)context->x[0];
        context->x[0] = hzn_events_cancel_wait(events, hzn_thread_id(threads, context, handle));
        fprintf(probe_log, "CancelSynchronization handle=%#" PRIx32 " result=%#" PRIx64 "\n",
                handle, context->x[0]);
        fflush(probe_log);
        return;
    }
    if (number == 0x18) {
        waiter_id = hzn_thread_id(threads, context, 0xffff8000u);
        if (waiter_id && waiter_id == system_worker_id) {
            kernel_lock();
            if (rd64(context->tpidrro + 0x1f8) != sdk_base + 0xe13000 ||
                context->tpidr != context->tpidrro + POKOPIA_BOOT_TLS_SIZE)
                stop("SDK worker failed to establish thread-local state", context->tpidrro);
            const uint32_t count = (uint32_t)context->x[2];
            fprintf(probe_log, "SDK worker WaitSynchronization count=%" PRIu32 " timeout=%" PRId64
                    " lr=%#" PRIx64 "\n", count, (int64_t)context->x[3], context->x[30]);
            fflush(probe_log);
            /* A zero-handle wait can occur while startup is attaching the GPU
             * error event. The persistent service wait includes that handle. */
            const unsigned char *handles = count <= 64
                ? read_guest(NULL, context->x[1], count * sizeof(uint32_t)) : NULL;
            if (gpu_error_event && handles && (int64_t)context->x[3] < 0) {
                for (size_t i = 0; i < count; ++i) {
                    uint32_t handle;
                    memcpy(&handle, handles + i * sizeof handle, sizeof handle);
                    if (handle == gpu_error_event) {
                        worker_waiting = true;
                        pokopia_host_notify();
                        break;
                    }
                }
            }
            kernel_unlock();
        }
    }
    if (hzn_event_svc(events, number, context->x, waiter_id))
        return;
    if ((number >= 0x1a && number <= 0x1d) || number == 0x34 || number == 0x35) {
        uint64_t id = hzn_thread_id(threads, context, 0xffff8000u);
        int32_t priority = hzn_thread_priority(threads, context);
        if (hzn_key_sync_svc(key_sync, number, context->x, id, priority))
            return;
    }
    kernel_lock();
    if (number == 7) {
        log_guest_frames(context);
        stop("guest requested ExitProcess", context->x[0]);
    }
    if (number == 0x29 && (uint32_t)context->x[1] == 20) {
        uint32_t result = 0;
        if (context->x[3])
            result = 0xe801;
        else if ((uint32_t)context->x[2] != 0xffff8001u)
            result = 0xe401;
        else if (!exception_context) {
            exception_context = map_module(exception_context_address, POKOPIA_BOOT_TLS_SIZE);
            if (!exception_context)
                result = HZN_STACK_MAP_OUT_OF_MEMORY;
        }
        context->x[0] = result;
        context->x[1] = result ? 0 : exception_context_address;
        fprintf(probe_log, "GetInfo user exception context=%#" PRIx64 " result=%#" PRIx32 "\n",
                context->x[1], result);
        fflush(probe_log);
        kernel_unlock();
        return;
    }
    if (number == 4 || number == 5) {
        const uint64_t dst = context->x[0], src = context->x[1], bytes = context->x[2];
        const MemoryInfo target = query_memory(dst);
        const bool isolated = src == stack_map.source;
        int32_t host_error = 0;
        uint32_t result;
        if ((dst | src) & 0xfff)
            result = HZN_STACK_MAP_INVALID_ADDRESS;
        else if (!bytes || (bytes & 0xfff))
            result = HZN_STACK_MAP_INVALID_SIZE;
        else if (number == 4 && (target.type || bytes > target.base + target.size - dst))
            result = HZN_STACK_MAP_INVALID_MEMORY_STATE;
        else if (isolated) {
            result = number == 4 ? hzn_stack_map_map(&stack_map, dst, src, bytes) :
                                  hzn_stack_map_unmap(&stack_map, dst, src, bytes);
            host_error = stack_map.last_host_error;
        } else if (number == 5) {
            result = hzn_heap_stack_alias_unmap(&heap_aliases, dst, src, bytes);
            host_error = heap_aliases.last_host_error;
        } else {
            const uint64_t heap_size = pokopia_boot_heap_size();
            const MemoryInfo source = query_memory(src);
            bool valid_source = src >= POKOPIA_BOOT_HEAP_BASE && src - POKOPIA_BOOT_HEAP_BASE < heap_size &&
                                bytes <= heap_size - (src - POKOPIA_BOOT_HEAP_BASE);
            bool isolated_source = false;
#ifdef POKOPIA_GFX
            isolated_source = isolated_stack_range(src, bytes);
            valid_source = valid_source || isolated_source;
#endif
            if (!valid_source ||
                source.permission != 3 || bytes > source.base + source.size - src ||
                dst < POKOPIA_BOOT_STACK_REGION || dst - POKOPIA_BOOT_STACK_REGION >= POKOPIA_BOOT_STACK_REGION_SIZE ||
                bytes > POKOPIA_BOOT_STACK_REGION_SIZE - (dst - POKOPIA_BOOT_STACK_REGION)) {
                result = HZN_STACK_MAP_INVALID_MEMORY_STATE;
            } else {
                result = isolated_source ? hzn_heap_stack_alias_map_isolated(&heap_aliases, dst, src, bytes) :
                                           hzn_heap_stack_alias_map(&heap_aliases, dst, src, bytes);
                host_error = heap_aliases.last_host_error;
            }
        }
        fprintf(probe_log, "%s dst=%#" PRIx64 " src=%#" PRIx64 " size=%#" PRIx64
                " result=%#" PRIx32 " host=%#" PRIx32 "\n", number == 4 ? "MapMemory" : "UnmapMemory",
                dst, src, bytes, result, (uint32_t)host_error);
#ifdef POKOPIA_GFX
        if (!isolated && number == 4 && !result && heap_aliases.count == 1)
            log_guest_frames(context);
#endif
        fflush(probe_log);
        if (stack_map.unsafe || heap_aliases.unsafe)
            stop("stack mapping rollback failed", dst);
        context->x[0] = result;
        kernel_unlock();
        return;
    }
    if (pokopia_boot_svc(context, number, probe_log) || shared_memory_svc(context, number) ||
        transfer_memory_svc(context, number) || service_manager_svc(context, number)) {
        kernel_unlock();
        return;
    }
    if (number == 2 || number == 3) {
        uint32_t result = protect_module_data(number, context->x[0], context->x[1],
                                              (uint32_t)context->x[2], (uint32_t)context->x[3]);
        fprintf(probe_log, "Memory SVC %u address=%#" PRIx64 " size=%#" PRIx64
                " value=%#" PRIx64 " extra=%#" PRIx64 " result=%#" PRIx32 "\n",
                number, context->x[0], context->x[1], context->x[2], context->x[3], result);
        fflush(probe_log);
        context->x[0] = result;
        kernel_unlock();
        return;
    }
#endif
#ifdef POKOPIA_SDK_PROBE
    if (number == 0x29 && context->x[30] == sdk_base + 0x152074 &&
        context->x[1] == 11 && context->x[2] == 0 && context->x[3] == 2) {
        if (rd64(sdk_base + 0xe15640) != 1)
            stop("SDK boot parameter did not retain the main thread handle", context->x[30]);
        fprintf(probe_log, "PASS translated SDK nn::init::Start reached GetInfo(11, 0, 2) at %#" PRIx64 "\n",
                sdk_base + 0x180d78);
        longjmp(boundary, 1);
    }
#endif
    if (number != 0x06)
        stop("unimplemented Horizon SVC", number);

    uint64_t address = context->x[2];
    MemoryInfo result = query_memory(address);
#ifdef POKOPIA_GFX
    const unsigned max_queries = 2048;
#else
    const unsigned max_queries = 128;
#endif
    if (++query_count > max_queries)
        stop("QueryMemory iteration limit", address);
    fprintf(probe_log, "QueryMemory #%u %#" PRIx64 " -> [%#" PRIx64 ", %#" PRIx64 ") type=%u perm=%u\n",
            query_count, address, result.base, result.base + result.size,
            result.type, result.permission);
    fflush(probe_log);

#ifdef POKOPIA_BOOT_PROBE
    MemoryInfo output_region = query_memory(context->x[0]);
    if (!(output_region.permission & 2) ||
        sizeof result > output_region.base + output_region.size - context->x[0])
        stop("invalid QueryMemory output address", context->x[0]);
#endif
    memcpy((void *)(uintptr_t)context->x[0], &result, sizeof result);
    context->x[0] = 0;
    context->x[1] = 0;
#ifdef POKOPIA_BOOT_PROBE
    kernel_unlock();
#endif
}

void call_indirect(Ctx *context, uint64_t address)
{
#ifdef POKOPIA_BOOT_PROBE
#ifdef POKOPIA_GFX
    if (dispatch_graphics(context, address))
        return;
#endif
#ifdef POKOPIA_BOOT_GAME
    if (address == UINT64_C(0x71000a4630)) {
        fputs("Reached Pokopia nnMain at 0x71000a4630\n", probe_log);
        fflush(probe_log);
    }
#endif
    if (pokopia_boot_dispatch(context, address))
        return;
#endif
    for (size_t i = 0; i < sizeof functions / sizeof functions[0]; ++i) {
        if (functions[i].address == address) {
            functions[i].function(context);
            return;
        }
    }
    stop("unimplemented guest call", address);
}

#ifdef POKOPIA_BOOT_PROBE
int nx_hook_entry(Ctx *context, uint64_t address)
{
#ifdef POKOPIA_GFX
    return render_entry_hook(context, address) || stack_entry_hook(context, address) || dispatch_graphics(context, address);
#else
    (void)context;
    (void)address;
    return 0;
#endif
}
#endif

void host_import(Ctx *context, uint32_t id)
{
    if (id != NX_IMPORT_ID(3, 1) || context->x[30] != rtld_base + 0x8c) {
#ifdef POKOPIA_BOOT_PROBE
        const char *name;
        uint64_t address = pokopia_boot_import(id, &name);
        if (address) {
            call_indirect(context, address);
            return;
        }
        if (name)
            fprintf(probe_log, "Unresolved guest import %s\n", name);
#endif
        stop("unexpected module import", id);
    }
#ifdef POKOPIA_BOOT_PROBE
    const uint32_t thread_handle = POKOPIA_BOOT_THREAD_HANDLE;
#else
    const uint32_t thread_handle = 1;
#endif
    if (context->x[0] != thread_handle || context->x[1] != rtld_base + 0x20000 ||
        context->x[2] != rtld_base + 0x1a78 || context->x[3] != rtld_base + 0x16a0 ||
        context->x[4] != rtld_base + 0x1720)
        stop("unexpected nn::init::Start ABI", id);

    uint64_t got_entry = rd64(rtld_base + 0x42e8);
    if (got_entry != sdk_start)
        stop("RTLD did not resolve the SDK nn::init::Start export", got_entry);
    fprintf(probe_log, "PASS actual translated RTLD reached SDK nn::init::Start at %#" PRIx64
            " after %u QueryMemory calls\n", got_entry, query_count);
#if defined(POKOPIA_SDK_PROBE) || defined(POKOPIA_BOOT_PROBE)
    if (rd64(sdk_base + 0xde1c58) != sdk_base + 0xe15640 ||
        rd64(sdk_base + 0xde1bf0) != sdk_base + 0xe15018)
        stop("RTLD did not resolve the SDK startup globals", sdk_base);
    fflush(probe_log);
#ifdef POKOPIA_BOOT_PROBE
    call_indirect(context, sdk_start);
#else
    f_7500118e90(context);
#endif
    stop("SDK nn::init::Start returned", sdk_start);
#else
    longjmp(boundary, 1);
#endif
}

void guest_trap(const char *what, uint64_t address)
{
    stop(what, address);
}

static int load_module(const Module *module, void *mapping)
{
    char path[256];
    int length = snprintf(path, sizeof path, "%s/%s.bin", POKOPIA_LOADER_ASSET_DIR, module->name);
    if (length < 0 || (size_t)length >= sizeof path)
        return 0;

#ifdef _WIN32
    FILE *input = NULL;
    fopen_s(&input, path, "rb");
#else
    FILE *input = fopen(path, "rb");
#endif
    if (!input)
        return 0;
    int complete = fread(mapping, 1, module->image_size, input) == module->image_size &&
                   fgetc(input) == EOF && !ferror(input);
    fclose(input);
    return complete;
}

#ifdef POKOPIA_BOOT_PROBE
static int enter_guest(Ctx *context, uint64_t entry)
{
    GuestBoundary scope = { .context = context };
    if (!pokopia_host_set_thread(&scope))
        return 1;
    int status = 1;
    switch (setjmp(scope.jump)) {
    case 0:
        call_indirect(context, entry);
        if (context == &cpu)
            stop("RTLD returned", context->fn);
        status = 0;
        break;
    case 1:
        status = 0;
        break;
    }
    if (!pokopia_host_set_thread(NULL))
        return 1;
    return status;
}

static void dispatch_thread(void *opaque, Ctx *context, uint64_t entry)
{
    (void)opaque;
    fprintf(probe_log, "Dispatching guest worker entry=%#" PRIx64 " sp=%#" PRIx64 " tls=%#" PRIx64 "\n",
            entry, context->x[31], context->tpidrro);
    fflush(probe_log);
    if (enter_guest(context, entry)) {
        kernel_lock();
        worker_failed = true;
        pokopia_host_notify();
        kernel_unlock();
    }
}

#if defined(POKOPIA_GFX) && !defined(_WIN32)
static void report_worker_error(void *opaque, uint32_t handle, const char *operation, int32_t error)
{
    (void)opaque;
    fprintf(probe_log, "Orbis worker %#" PRIx32 " %s failed, host=%#" PRIx32 "\n",
            handle, operation, (uint32_t)error);
    fflush(probe_log);
    pokopia_orbis_memory_census(probe_log, "worker start or teardown failure");
    pokopia_vk_ledger_report(probe_log, "worker start or teardown failure");
}
#endif
#else
static int enter_rtld(void)
{
    switch (setjmp(boundary)) {
    case 0:
        f_7700000000(&cpu);
        stop("RTLD returned", cpu.fn);
        return 1;
    case 1:
        return 0;
    default:
        return 1;
    }
}
#endif

int pokopia_guest_probe(FILE *log)
{
    probe_log = log ? log : stdout;
#if defined(POKOPIA_GFX) && !defined(_WIN32) && defined(POKOPIA_ORBIS_MEMORY_TRACE)
    pokopia_orbis_memory_trace_start();
#endif
    memset(&cpu, 0, sizeof cpu);
    query_count = 0;

    size_t mapped_count = 0;
    int status = 1;
    enum { STACK_SIZE = 0x100000 };
#ifdef POKOPIA_BOOT_PROBE
    _Static_assert(STACK_SIZE == POKOPIA_BOOT_STACK_SIZE, "guest stack map size");
    bool host_ready = false;
    worker_stack_seeded = worker_started = worker_waiting = worker_failed = false;
    applet_focus_pending = true;
    gpu_error_event = 0;
    system_worker_id = 0;
    clock_initialized = false;
    shared_clock.next_handle = 0;
    memset(shared_clock.handles, 0, sizeof shared_clock.handles);
#ifdef POKOPIA_GFX
    shared_streamplay.next_handle = 0;
    memset(shared_streamplay.handles, 0, sizeof shared_streamplay.handles);
    shared_hid.next_handle = 0;
    memset(shared_hid.handles, 0, sizeof shared_hid.handles);
    hzn_hid_init(&hid_state, hzn_applet_resource_user_id);
#ifdef _WIN32
    size_t save_root_bytes;
    if (getenv_s(&save_root_bytes, account_save_root, sizeof account_save_root, "POKOPIA_SAVE_ROOT"))
        return 1;
#endif
#endif
    memset(worker_tls_used, 0, sizeof worker_tls_used);
    memset(protected_code, 0, sizeof protected_code);
    uint64_t mapped_bytes = STACK_SIZE + 2 * POKOPIA_BOOT_TLS_SIZE + HZN_STACK_ALIAS_BYTES;
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; ++i)
        mapped_bytes += modules[i].memory_size;
    if (!pokopia_boot_init(probe_log, mapped_bytes))
        return 1;
    memset(service_sessions, 0, sizeof service_sessions);
    if (!hzn_sm_init(&service_manager, guest_pid, POKOPIA_BOOT_THREAD_HANDLE + 1,
                     POKOPIA_BOOT_WORKER_HANDLE))
        return 1;
    unsigned char *stack = map_module(POKOPIA_BOOT_STACK_BASE, STACK_SIZE);
#else
    unsigned char *stack = malloc(STACK_SIZE);
#endif
    if (!stack) {
        fprintf(probe_log, "FAIL could not allocate guest stack\n");
        return 1;
    }
    cpu.x[31] = (uintptr_t)(stack + STACK_SIZE) & ~(uintptr_t)15;
#ifdef POKOPIA_BOOT_PROBE
    void *tls = map_module(POKOPIA_BOOT_TLS_BASE, 2 * POKOPIA_BOOT_TLS_SIZE);
    if (!tls) {
        fputs("FAIL could not map guest TLS\n", probe_log);
        unmap_module(stack, STACK_SIZE);
        return 1;
    }
    cpu.tpidrro = POKOPIA_BOOT_TLS_BASE;
    cpu.tpidr = POKOPIA_BOOT_TCB_BASE;
    wr64(cpu.tpidrro + 0x1f0, cpu.tpidr);
    cpu.x[1] = POKOPIA_BOOT_THREAD_HANDLE;
#else
    cpu.x[1] = 1;
#endif
    cpu.ex_addr = UINT64_MAX;

    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; ++i) {
        const Module *module = &modules[i];
        void *mapping = map_module(module->base, module->memory_size);
        if (!mapping) {
            fprintf(probe_log, "FAIL could not map %s at %#" PRIx64 "\n", module->name, module->base);
            goto done;
        }
        ++mapped_count;
        if (!load_module(module, mapping)) {
            fprintf(probe_log, "FAIL could not read complete %s image\n", module->name);
            goto done;
        }
        fprintf(probe_log, "Mapped %s [%#" PRIx64 ", %#" PRIx64 ")\n",
                module->name, module->base, module->base + module->memory_size);
        fflush(probe_log);
    }

#ifdef POKOPIA_BOOT_PROBE
    if (!pokopia_host_init()) {
        fputs("FAIL could not initialize host thread state\n", probe_log);
        goto done;
    }
    host_ready = true;
    if (!hzn_stack_map_init(&stack_map, POKOPIA_BOOT_WORKER_STACK,
                            POKOPIA_BOOT_STACK_REGION, POKOPIA_BOOT_STACK_REGION_SIZE)) {
        fprintf(probe_log, "FAIL could not isolate SDK worker stack, host=%#" PRIx32 "\n",
                (uint32_t)stack_map.last_host_error);
        goto done;
    }
    const HznEventConfig event_config = { .copy_handles = copy_wait_handles };
    events = hzn_events_create(&event_config);
    if (!events || !hzn_events_set_signaled(events, HZN_APPLET_MESSAGE, true)) {
        fputs("FAIL could not initialize guest events\n", probe_log);
        goto done;
    }
    const HznThreadConfig config = {
        .events = events,
        .main_context = &cpu,
        .main_handle = POKOPIA_BOOT_THREAD_HANDLE,
        .worker_handle_base = POKOPIA_BOOT_WORKER_HANDLE,
        .worker_handle_limit = HZN_EVENT_HANDLE_BASE,
        .main_id = 1,
        .main_priority = POKOPIA_MAIN_PRIORITY,
        .main_core = POKOPIA_MAIN_CORE,
        .process_core_mask = POKOPIA_CORE_MASK,
        .process_priority_mask = POKOPIA_PRIORITY_MASK,
        .validate_start = validate_thread,
        .allocate_tls = allocate_thread_tls,
        .release_tls = release_thread_tls,
        .dispatch = dispatch_thread,
#if defined(POKOPIA_GFX) && !defined(_WIN32)
        .direct_worker_stacks = true,
        .worker_error = report_worker_error,
#endif
    };
    const HznKeySyncConfig sync_config = { .word = resolve_sync_word, .thread_id = resolve_sync_thread };
    key_sync = hzn_key_sync_create(&sync_config);
    if (!key_sync) {
        fputs("FAIL could not initialize guest key synchronization\n", probe_log);
        goto done;
    }
    threads = hzn_threads_create(&config);
    if (!threads) {
        fputs("FAIL could not initialize guest thread table\n", probe_log);
        goto done;
    }
#ifdef POKOPIA_GFX
    const ViAdapterConfig vi_config = {
        .sdk_base = sdk_base, .guest_object_base = gfx_driver_address + 0x100,
        .opaque = &graphics, .graphics_ready = gfx_ready, .query_output = vi_output, .write_guest = write_guest,
    };
    if (!hzn_stack_sources_init(&stack_sources, UINT64_C(0x7e10000000), 0x400000) ||
        !vi_adapter_init(&vi, &vi_config)) {
        fputs("FAIL could not initialize game graphics resources\n", probe_log);
        hzn_threads_destroy(threads);
        threads = NULL;
        goto done;
    }
    const NvnFrontConfig nvn_config = {
        .backend_opaque = &graphics,
        .read_guest = read_guest,
        .write_guest = write_guest,
        .device_create = create_nvn_device,
        .device_destroy = destroy_nvn_device,
        .queue_create = create_nvn_queue,
        .queue_destroy = destroy_nvn_queue,
        .texture_create = create_nvn_texture,
        .texture_destroy = destroy_nvn_texture,
        .sync_create = create_nvn_sync,
        .sync_destroy = destroy_nvn_sync,
        .command_buffer_create = create_nvn_command_buffer,
        .command_buffer_destroy = destroy_nvn_command_buffer,
        .program_create = create_nvn_program,
        .program_destroy = destroy_nvn_program,
        .program_link = link_nvn_program,
        .get_integer = nvn_device_integer,
    };
    nvn = nvn_front_create(&nvn_config);
    texture_upload = (PokopiaLiveTextureUpload){
        .front = nvn, .adapter = &graphics, .read_guest = read_guest, .write_guest = write_guest,
        .log = probe_log,
    };
    const NvnWindowConfig window_config = {
        .opaque = nvn, .read_guest = read_guest, .write_guest = write_guest,
        .get_output = nvn_window_output, .get_texture = nvn_window_texture,
    };
    if (nvn)
        windows = nvn_window_create(&window_config);
    const NvnReplayConfig replay_config = {
        .front = nvn, .opaque = &graphics, .read_guest = read_guest, .write_guest = write_guest,
        .copy = execute_nvn_copy, .clear = execute_nvn_clear,
        .clear_depth_stencil = execute_nvn_clear_depth_stencil, .draw = execute_nvn_draw,
        .compute = execute_nvn_compute, .sync = execute_nvn_sync, .finish = finish_nvn_queue,
        .timestamp_ns = pokopia_boot_monotonic_ns,
    };
    if (nvn)
        replay = nvn_replay_create(&replay_config);
    if (!nvn || !windows || !replay) {
        fputs("FAIL could not initialize NVN dispatch\n", probe_log);
        hzn_threads_destroy(threads);
        threads = NULL;
        goto done;
    }
#endif
    status = enter_guest(&cpu, rtld_base);
    if (worker_started) {
        kernel_lock();
        if (!pokopia_host_wait(&worker_waiting, &worker_failed, 2000))
            status = 1;
        kernel_unlock();
    }
    hzn_threads_request_exit(threads);
    hzn_events_cancel_all(events);
    hzn_key_sync_cancel_all(key_sync);
    if (!hzn_threads_destroy(threads)) {
        fputs("FAIL could not join all guest threads\n", probe_log);
        fflush(probe_log);
        return 1;
    }
    threads = NULL;
    if (!worker_waiting || worker_failed) {
        fputs("FAIL SDK worker did not complete its startup checkpoint\n", probe_log);
        status = 1;
    }
    if (status == 0)
        fputs("PASS translated SDK worker executed with its own stack and TLS\n", probe_log);
    if (status == 0 && !pokopia_boot_heap_size() && !pokopia_boot_check_heap(probe_log))
        status = 1;
#else
    status = enter_rtld();
#endif

done:
#ifdef POKOPIA_BOOT_PROBE
    if (exception_context) {
        unmap_module(exception_context, POKOPIA_BOOT_TLS_SIZE);
        exception_context = NULL;
    }
#ifdef POKOPIA_GFX
    for (size_t i = 0; i < sizeof service_sessions / sizeof service_sessions[0]; ++i)
        if (service_sessions[i].handle)
            close_service(&service_sessions[i]);
    nvn_window_destroy(windows);
    windows = NULL;
    nvn_replay_destroy(replay);
    replay = NULL;
    VkResult upload_close = pokopia_live_texture_upload_close(&texture_upload);
    if (upload_close != VK_SUCCESS) {
        fprintf(probe_log, "FAIL could not release texture staging (%d)\n", (int)upload_close);
        status = 1;
    }
#ifndef _WIN32
    if (live_present_command) {
        graphics.wait_idle(graphics.device);
        pokopia_live_present_close(&live_output);
        pokopia_game_capture_close(&live_capture);
        pokopia_vk_command_buffer_close(&graphics, live_present_command);
        live_present_command = VK_NULL_HANDLE;
    }
#endif
    if (nvn) {
        nvn_front_destroy(nvn);
        nvn = NULL;
    }
    nvn_shader_cache_close(shaders);
    shaders = NULL;
    pokopia_vk_close(&graphics);
    pokopia_vk_ledger_report(probe_log, "after teardown");
    if (gfx_driver) {
        unmap_module(gfx_driver, POKOPIA_BOOT_TLS_SIZE);
        gfx_driver = NULL;
    }
#endif
    if (!hzn_heap_stack_alias_release(&heap_aliases)) {
        fprintf(probe_log, "FAIL could not restore heap stack aliases, host=%#" PRIx32 "\n",
                (uint32_t)heap_aliases.last_host_error);
        status = 1;
    }
#ifdef POKOPIA_GFX
    else if (!hzn_stack_sources_release(&stack_sources, &heap_aliases)) {
        fprintf(probe_log, "FAIL could not release isolated game stacks, host=%#" PRIx32 "\n",
                (uint32_t)stack_sources.last_host_error);
        status = 1;
    }
#endif
    else if (!pokopia_boot_release_heap(probe_log))
        status = 1;
    if (host_ready) {
        if (!hzn_readonly_page_unmap(&shared_clock.mapping)) {
            fprintf(probe_log, "FAIL could not release clock page, host=%#" PRIx32 "\n",
                    (uint32_t)shared_clock.mapping.last_host_error);
            status = 1;
        }
#ifdef POKOPIA_GFX
        if (!hzn_live_shared_unmap(&mapped_hid)) {
            fprintf(probe_log, "FAIL could not release HID memory, host=%#" PRIx32 "\n",
                    (uint32_t)mapped_hid.last_host_error);
            status = 1;
        }
#ifndef _WIN32
        hzn_hid_ps4_close(native_pad);
        native_pad = -1;
#endif
        if (!hzn_readonly_page_unmap(&shared_streamplay.mapping)) {
            fprintf(probe_log, "FAIL could not release streamplay memory, host=%#" PRIx32 "\n",
                    (uint32_t)shared_streamplay.mapping.last_host_error);
            status = 1;
        }
#endif
        if (events) {
            hzn_events_destroy(events);
            events = NULL;
        }
        if (key_sync) {
            hzn_key_sync_destroy(key_sync);
            key_sync = NULL;
        }
        if (!hzn_stack_map_release(&stack_map)) {
            fprintf(probe_log, "FAIL could not release SDK stack mapping, host=%#" PRIx32 "\n",
                    (uint32_t)stack_map.last_host_error);
            status = 1;
        }
        pokopia_host_destroy();
    }
#endif
    while (mapped_count > 0) {
        const Module *module = &modules[--mapped_count];
        unmap_module((void *)(uintptr_t)module->base, module->memory_size);
    }
#ifdef POKOPIA_BOOT_PROBE
    unmap_module(tls, 2 * POKOPIA_BOOT_TLS_SIZE);
    unmap_module(stack, STACK_SIZE);
#else
    free(stack);
#endif
#if defined(POKOPIA_GFX) && !defined(_WIN32) && defined(POKOPIA_ORBIS_MEMORY_TRACE)
    pokopia_orbis_memory_census(probe_log, "probe cleanup");
#endif
    fflush(probe_log);
    return status;
}

#ifdef _WIN32
#ifdef POKOPIA_BOOT_PROBE
static LONG CALLBACK report_host_fault(EXCEPTION_POINTERS *exception)
{
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const GuestBoundary *scope = pokopia_host_thread();
    if (!scope)
        return EXCEPTION_CONTINUE_SEARCH;
    const Ctx *context = scope->context;
    char line[384];
    int length = snprintf(line, sizeof line, "Host access violation address=%#" PRIxPTR
                          " operation=%" PRIuPTR " guest-function=%#" PRIx64 " lr=%#" PRIx64
                          " x0=%#" PRIx64 " x19=%#" PRIx64 " fp=%#" PRIx64 " sp=%#" PRIx64 "\n",
                          (uintptr_t)record->ExceptionInformation[1], (uintptr_t)record->ExceptionInformation[0],
                          context->fn, context->x[30], context->x[0], context->x[19],
                          context->x[29], context->x[31]);
    HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written;
    if (length > 0 && (size_t)length < sizeof line) {
        WriteFile(output, line, (DWORD)length, &written, NULL);
    }
    uint64_t frame = context->x[29];
    for (unsigned i = 0; i < 12; ++i) {
        uint64_t saved[2];
        if ((frame & 7) || frame < POKOPIA_BOOT_STACK_REGION ||
            frame - POKOPIA_BOOT_STACK_REGION > POKOPIA_BOOT_STACK_REGION_SIZE - sizeof saved ||
            !ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)frame, saved, sizeof saved, NULL))
            break;
        length = snprintf(line, sizeof line, "fault guest frame %u fp=%#" PRIx64 " lr=%#" PRIx64 "\n",
                          i, frame, saved[1]);
        if (length > 0 && (size_t)length < sizeof line)
            WriteFile(output, line, (DWORD)length, &written, NULL);
        if (saved[0] <= frame)
            break;
        frame = saved[0];
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(void)
{
#ifdef POKOPIA_BOOT_PROBE
    PVOID handler = AddVectoredExceptionHandler(1, report_host_fault);
    int result = pokopia_guest_probe(stdout);
    if (handler)
        RemoveVectoredExceptionHandler(handler);
    return result;
#else
    return pokopia_guest_probe(stdout);
#endif
}
#endif


