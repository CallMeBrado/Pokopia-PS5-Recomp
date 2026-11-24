/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <nxrecomp/ctx.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#if defined(__PROSPERO__)
#include <pthread_np.h>
#include <sys/resource.h>
#endif
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
#endif

#include <inttypes.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../cpu-probe/generated/lift-7700000000/funcs.h"
#ifdef POKOPIA_BOOT_PROBE
#include "../../boot/boot.h"
#include "../../boot/host.h"
#include "../../horizon/sm.h"
#include "../../horizon/lm.h"
#include "../../horizon/cmif.h"
#include "../../horizon/applet.h"
#include "../../horizon/aoc.h"
#include "../../horizon/pctl.h"
#include "../../horizon/time_service.h"
#ifdef POKOPIA_HWOPUS
#include "../../horizon/hwopus.h"
#endif
#include "../../horizon/readonly_page.h"
#include "../../horizon/live_shared.h"
#include "../../horizon/get_info.h"
#include "../../horizon/event.h"
#include "../../horizon/stack_map.h"
#include "../../horizon/heap_stack_alias.h"
#include "../../horizon/key_sync.h"
#include "../../horizon/thread.h"
#include "../../horizon/transfer_memory.h"
#endif
#ifdef POKOPIA_GFX
#include "../../horizon/account_identity.h"
#include "../../horizon/bsd_ssl.h"
#include "../../horizon/hid.h"
#include "../../horizon/offline_app.h"
#include "../../horizon/save_store.h"
#include "../../gfx-bridge/vk_flush.h"
#include "../../gfx-bridge/osk.h"
#include "../../horizon/stpl.h"
#include "../../gfx-bridge/account.h"
#include "../../gfx-bridge/fsp.h"
#include "../../gfx-bridge/live_compute.h"
#include "../../gfx-bridge/live_texture_upload.h"
#ifdef POKOPIA_AUDIO_OUT
#include "../../horizon/audio_device.h"
#include "../../horizon/audio_out.h"
#endif
#include "../../gfx-bridge/nvn_front.h"
#include "../../gfx-bridge/nvn_guest_layout.h"
#include "../../gfx-bridge/nvn_replay.h"
#include "../../gfx-bridge/nvn_window.h"
#ifndef _WIN32
#include "../../gfx-bridge/live_present.h"
#include "../../gfx-bridge/orbis_memory.h"
#endif
#include "../../gfx-bridge/vi_adapter.h"
#include "../../gfx-bridge/vk_adapter.h"
#include "../../gfx-bridge/vk_ledger.h"
#include "../../gfx-bridge/vk_program.h"
#include "../../gfx-bridge/sdk-graphics.inc"
#include "../../horizon/stack_source.h"
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
#include "../../loader-probe/generated/layout.inc"
};

static const struct {
    uint64_t address;
    void (*function)(Ctx *);
} functions[] = {
#include "../../loader-probe/generated/rtld_functions.inc"
};

static const uint64_t sdk_start =
#include "../../loader-probe/generated/sdk_start.inc"
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
static const char account_save_root[] = "/data/pokopia/save-r40";
static int native_pad = -1;
/* R96: debug/framecap (30, 60 or 0), -1 when the port menu's setting applies. */
static volatile int pokopia_frame_cap_override = -1;
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

static unsigned active_frame_cap(void)
{
    const int override = pokopia_frame_cap_override;
    return override >= 0 ? (unsigned)override : pokopia_settings.frame_cap;
}

#if defined(__PROSPERO__)
/* R86: while the on-screen keyboard is up the guest sees an idle pad and the
 * keyboard gets the real one; the last presented game image is re-shown behind
 * it when the game stops presenting. */
static _Atomic bool osk_active;
static HznHidPad osk_pad;
static pthread_mutex_t pad_mutex = PTHREAD_MUTEX_INITIALIZER;
/* R92: the port settings menu. Holding the touch pad (Minus) for a second
 * opens it; the game sees a neutral pad while it is open and until the
 * buttons that closed it are released. Its state belongs to pad_mutex; the
 * replay worker draws it at present when menu_dirty is set. */
#define PORT_SETTINGS_PATH "/data/pokopia/port-settings.ini"
static PokopiaMenu port_menu;
static bool menu_open, menu_release_wait;
static uint64_t menu_touch_since_ms, menu_previous;
static atomic_bool menu_dirty;
static atomic_flag port_settings_loaded = ATOMIC_FLAG_INIT;

static void apply_port_settings(const PokopiaSettings *values)
{
    const bool glyphs = values->prompts != pokopia_settings.prompts || values->confirm != pokopia_settings.confirm;
    pokopia_settings = *values;
    hzn_hid_ps4_swap_ab = values->confirm == POKOPIA_CONFIRM_CROSS;
    if (glyphs)
        atomic_fetch_add_explicit(&pokopia_glyph_generation, 1, memory_order_relaxed);
}

static void port_settings_once(void)
{
    if (atomic_flag_test_and_set_explicit(&port_settings_loaded, memory_order_acq_rel))
        return;
    PokopiaSettings loaded;
    pokopia_settings_load(&loaded, PORT_SETTINGS_PATH);
    pokopia_settings = loaded;
    hzn_hid_ps4_swap_ab = loaded.confirm == POKOPIA_CONFIRM_CROSS;
}

/* R96: the game's frame limiter (0x710089df20) targets 60.0f / 1.0f frames a
 * second; ps5/build/patch-lift.py makes it call this for the numerator every
 * frame instead. Its delta time stays measured, so the game's speed is right
 * at any rate. Uncapped is 1000 (a 1 ms period: no wait); the display's
 * refresh (FIFO) is then the limit. debug/framecap overrides the menu. */
uint32_t pokopia_frame_limit_bits(void)
{
    const unsigned cap = active_frame_cap();
    const float fps = cap == 30 ? 30.0f : cap == 0 ? 1000.0f : 60.0f;
    uint32_t bits;
    memcpy(&bits, &fps, sizeof bits);
    return bits;
}

static uint64_t stat_now_ns(void);
/* Called with pad_mutex held, on the pad as the guest would see it. */
static void port_menu_pad(HznHidPad *pad)
{
    const uint64_t now_ms = stat_now_ns() / 1000000;
    if (!menu_open && !atomic_load_explicit(&osk_active, memory_order_acquire)) {
        if (!menu_release_wait && (pad->buttons & POKOPIA_OSK_MINUS)) {
            if (!menu_touch_since_ms) {
                menu_touch_since_ms = now_ms ? now_ms : 1;
            } else if (now_ms - menu_touch_since_ms >= 1000) {
                pokopia_menu_init(&port_menu, &pokopia_settings);
                menu_open = true;
                menu_previous = pad->buttons; /* the held touch pad does not count */
                menu_touch_since_ms = 0;
                atomic_store_explicit(&menu_dirty, true, memory_order_release);
                fprintf(probe_log, "Port menu opened\n");
            }
        } else {
            menu_touch_since_ms = 0;
        }
    }
    if (menu_open) {
        bool changed = false;
        const PokopiaMenuState state =
            pokopia_menu_input(&port_menu, pad->buttons, menu_previous, pad->lx, pad->ly, now_ms, &changed);
        menu_previous = pad->buttons;
        if (changed) {
            apply_port_settings(&port_menu.values);
            atomic_store_explicit(&menu_dirty, true, memory_order_release);
        }
        if (state == POKOPIA_MENU_CLOSED) {
            menu_open = false;
            menu_release_wait = true;
            const bool saved = pokopia_settings_save(&pokopia_settings, PORT_SETTINGS_PATH);
            fprintf(probe_log, "Port menu closed; settings %s\n", saved ? "saved" : "NOT saved");
            atomic_store_explicit(&menu_dirty, true, memory_order_release);
        }
    }
    if (menu_release_wait && !menu_open && !pad->buttons)
        menu_release_wait = false;
    if (menu_open || menu_release_wait) {
        pad->buttons = 0;
        pad->lx = pad->ly = pad->rx = pad->ry = 0;
    }
}
static struct {
    PokopiaVkTexture *texture;
    VkExtent2D extent;
    VkRect2D crop;
    uint64_t presented_ns;
} last_present;
#endif
static PokopiaGameCapture live_capture;
static VkCommandBuffer live_present_command;
static _Atomic(const char *) overlay_stop;
static bool overlay_disabled;
#if defined(__PROSPERO__)
static int overlay_gpu_draw_percent = -1;
#endif
#endif
#if defined(__PROSPERO__)
/* R70: virtual controller from /data/pokopia/debug/pad.req (see
 * ps5/scripts/ps5-debug.py), merged over the physical pad. */
static HznDebugPad debug_pad;
static atomic_flag debug_pad_polling = ATOMIC_FLAG_INIT;
static uint64_t debug_pad_next_poll_ms;
#endif

static void update_guest_pad(void)
{
    if (!mapped_hid.writer)
        return;
#if defined(__PROSPERO__)
    pthread_mutex_lock(&pad_mutex); /* R86: the keyboard loop polls from its own thread */
#endif
    HznHidPad pad = {0};
#ifndef _WIN32
    if (native_pad >= 0)
        hzn_hid_ps4_read(native_pad, &pad);
#endif
#if defined(__PROSPERO__)
    if (!atomic_flag_test_and_set_explicit(&debug_pad_polling, memory_order_acquire)) {
        const uint64_t now = pokopia_boot_monotonic_ns() / 1000000;
        if (live_capture.mailbox.directory && now >= debug_pad_next_poll_ms) {
            debug_pad_next_poll_ms = now + 20;
            /* R73: trace.req in the mailbox traces the next 30 submits. */
            extern FILE *nvn_replay_trace_log;
            extern volatile int nvn_replay_trace_submits;
            static uint64_t trace_next_poll_ms;
            if (now >= trace_next_poll_ms) {
                trace_next_poll_ms = now + 500;
                char trace_request[256];
                snprintf(trace_request, sizeof trace_request, "%s/trace.req", live_capture.mailbox.directory);
                if (!access(trace_request, F_OK)) {
                    unlink(trace_request);
                    if (!nvn_replay_trace_log)
                        nvn_replay_trace_log = fopen("/data/pokopia/ps5-trace.txt", "w");
                    if (nvn_replay_trace_log) {
                        fprintf(nvn_replay_trace_log, "#### trace requested t=%.1fs\n", now / 1000.0);
                        nvn_replay_trace_submits = 30;
                    }
                    fprintf(probe_log, "Frame trace requested\n");
                }
                /* R96: debug/framecap overrides the port menu's frame cap (30, 60, 0). */
                snprintf(trace_request, sizeof trace_request, "%s/framecap", live_capture.mailbox.directory);
                FILE *cap_file = fopen(trace_request, "r");
                if (cap_file) {
                    int cap;
                    if (fscanf(cap_file, "%d", &cap) == 1 && cap != pokopia_frame_cap_override) {
                        pokopia_frame_cap_override = cap;
                        fprintf(probe_log, "Frame cap override %d (display %u mHz)\n", cap, pokopia_present_refresh_mhz);
                    }
                    fclose(cap_file);
                }
                /* R96: debug/tickscale (percent) speeds up or slows the guest's system tick. */
                snprintf(trace_request, sizeof trace_request, "%s/tickscale", live_capture.mailbox.directory);
                FILE *tick_file = fopen(trace_request, "r");
                if (tick_file) {
                    extern volatile unsigned pokopia_tick_scale_pct;
                    unsigned pct;
                    if (fscanf(tick_file, "%u", &pct) == 1 && pct >= 10 && pct <= 400 && pct != pokopia_tick_scale_pct) {
                        pokopia_tick_scale_pct = pct;
                        fprintf(probe_log, "Guest tick scale %u%%\n", pct);
                    }
                    fclose(tick_file);
                }
                /* R96: debug/tickcallers: 1 counts system-tick reads by caller, 2 logs and stops. */
                snprintf(trace_request, sizeof trace_request, "%s/tickcallers", live_capture.mailbox.directory);
                FILE *callers_file = fopen(trace_request, "r");
                if (callers_file) {
                    extern volatile int pokopia_tick_callers_on;
                    extern struct PokopiaTickCaller { uint64_t lr, fn, count; } pokopia_tick_callers[64];
                    int mode = 0;
                    const int fields = fscanf(callers_file, "%d", &mode);
                    fclose(callers_file);
                    unlink(trace_request);
                    if (fields == 1 && mode == 1) {
                        memset(pokopia_tick_callers, 0, sizeof pokopia_tick_callers);
                        pokopia_tick_callers_on = 1;
                    } else if (fields == 1 && mode == 2) {
                        pokopia_tick_callers_on = 0;
                        for (unsigned i = 0; i < 64 && pokopia_tick_callers[i].lr; ++i)
                            fprintf(probe_log, "Tick caller lr=%#" PRIx64 " fn=%#" PRIx64 " reads=%" PRIu64 "\n",
                                    pokopia_tick_callers[i].lr, pokopia_tick_callers[i].fn, pokopia_tick_callers[i].count);
                        fflush(probe_log);
                    }
                }
                /* R73: debug/exposure overrides the stand-in auto-exposure. */
                extern float pokopia_exposure_fallback;
                snprintf(trace_request, sizeof trace_request, "%s/exposure", live_capture.mailbox.directory);
                FILE *exposure_file = fopen(trace_request, "r");
                if (exposure_file) {
                    float value;
                    if (fscanf(exposure_file, "%f", &value) == 1 && value != pokopia_exposure_fallback) {
                        pokopia_exposure_fallback = value;
                        fprintf(probe_log, "Exposure stand-in set to %g\n", (double)value);
                    }
                    fclose(exposure_file);
                }
                /* R90: debug/flags holds A/B switches (see vk_flush.h). */
                snprintf(trace_request, sizeof trace_request, "%s/flags", live_capture.mailbox.directory);
                FILE *flags_file = fopen(trace_request, "r");
                unsigned flags = POKOPIA_DEBUG_DEFAULTS; /* R94: no file, the tuned set */
                if (flags_file) {
                    if (fscanf(flags_file, "%u", &flags) != 1)
                        flags = POKOPIA_DEBUG_DEFAULTS;
                    fclose(flags_file);
                }
                if (flags != pokopia_debug_flags) {
                    pokopia_debug_flags = flags;
                    fprintf(probe_log, "Debug flags set to %u\n", flags);
                }
                /* R95: debug/skip holds a shader hash whose draws are skipped (empty or absent: none). */
                {
                    extern char nvn_replay_skip_hash[24];
                    snprintf(trace_request, sizeof trace_request, "%s/skip", live_capture.mailbox.directory);
                    FILE *skip_file = fopen(trace_request, "r");
                    char skip[24] = {0};
                    if (skip_file) {
                        if (fscanf(skip_file, "%23s", skip) != 1)
                            skip[0] = 0;
                        fclose(skip_file);
                    }
                    if (strcmp(skip, nvn_replay_skip_hash)) {
                        memcpy(nvn_replay_skip_hash, skip, sizeof skip);
                        fprintf(probe_log, "Skipping draws of shader %s\n", skip[0] ? skip : "(none)");
                    }
                }
                /* R73: dump.req holds a shader hash; dump that draw's textures. */
                extern char nvn_replay_dump_hash[24];
                extern volatile int nvn_replay_dump_pending;
                snprintf(trace_request, sizeof trace_request, "%s/dump.req", live_capture.mailbox.directory);
                FILE *dump_request = fopen(trace_request, "r");
                if (dump_request) {
                    char hash[24] = {0};
                    unsigned width = 0, height = 0, skip = 0;
                    if (fscanf(dump_request, "%23s %u %u %u", hash, &width, &height, &skip) >= 1) {
                        extern unsigned nvn_replay_dump_width, nvn_replay_dump_height, nvn_replay_dump_skip,
                            nvn_replay_dump_seen_count;
                        memcpy(nvn_replay_dump_hash, hash, sizeof hash);
                        nvn_replay_dump_width = width;
                        nvn_replay_dump_height = height;
                        nvn_replay_dump_skip = skip;
                        nvn_replay_dump_seen_count = 0;
                        nvn_replay_dump_pending = 1;
                    }
                    fclose(dump_request);
                    unlink(trace_request);
                    if (!nvn_replay_trace_log)
                        nvn_replay_trace_log = fopen("/data/pokopia/ps5-trace.txt", "w");
                    fprintf(probe_log, "Texture dump requested after %s\n", hash);
                }
            }
            HznDebugPadCommand request;
            if (pokopia_debug_pad(&live_capture.mailbox, &request) &&
                hzn_debug_pad_apply(&debug_pad, &request, now) == HZN_DEBUG_PAD_APPLIED) {
                pokopia_debug_ack_pad(&live_capture.mailbox, request.seq);
                fprintf(probe_log, "Virtual controller seq=%" PRIu64 " buttons=%#" PRIx64 " hold=%ums\n",
                        request.seq, request.buttons, request.hold_ms);
            }
        }
        const HznDebugPadInput physical = {
            pad.buttons, (int16_t)pad.lx, (int16_t)pad.ly, (int16_t)pad.rx, (int16_t)pad.ry,
            pad.connected ? HZN_NPAD_CONNECTED : 0,
        };
        const HznDebugPadInput input = hzn_debug_pad_sample(&debug_pad, &physical, now);
        pad.buttons = input.buttons;
        pad.lx = input.lx;
        pad.ly = input.ly;
        pad.rx = input.rx;
        pad.ry = input.ry;
        pad.connected = (input.attributes & HZN_NPAD_CONNECTED) != 0;
        atomic_flag_clear_explicit(&debug_pad_polling, memory_order_release);
    }
#endif
#if defined(__PROSPERO__)
    port_settings_once();
    port_menu_pad(&pad);
    if (atomic_load_explicit(&osk_active, memory_order_acquire)) {
        osk_pad = pad;
        pad.buttons = 0;
        pad.lx = pad.ly = pad.rx = pad.ry = 0;
    }
#endif
    hzn_hid_publish(mapped_hid.writer, &hid_state, &pad);
#if defined(__PROSPERO__)
    pthread_mutex_unlock(&pad_mutex);
#endif
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
               SERVICE_HID, SERVICE_HID_RESOURCE, SERVICE_HID_VIBRATION_LIST, SERVICE_PREPO } ServiceType;
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
#ifndef _WIN32
/* R44 load stats: where loading time goes, one line every 5 s from the
 * present hook. Counters are relaxed atomics; the report swaps them to 0. */
/* R94: statistics only: a plain load and store, no locked add. The locked adds
 * on these shared lines ran several times per graphics call on every thread. */
#define STAT_ADD(counter, value)     atomic_store_explicit(&(counter), atomic_load_explicit(&(counter), memory_order_relaxed) + (value),                           memory_order_relaxed)
#define STAT_TAKE(counter) atomic_exchange_explicit(&(counter), 0, memory_order_relaxed)
enum { STAT_SVCS = 0x80 };
static atomic_uint_fast64_t stat_lock_calls, stat_lock_contended, stat_lock_wait_ns, stat_lock_hold_ns,
    stat_lock_hold_max_ns, stat_pager_hold_ns, stat_fsp_calls, stat_fsp_ns, stat_nvn_calls, stat_nvn_ns,
    stat_sync_fast, stat_sync_slow, stat_pure_fast, stat_pure_locked;
static atomic_uint_fast64_t stat_svc_calls[STAT_SVCS], stat_svc_ns[STAT_SVCS];
static _Thread_local uint64_t stat_lock_acquired;
extern uint64_t hzn_fsp_romfs_bytes;

static uint64_t stat_clock_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

#if defined(__PROSPERO__)
/* Every guest indirect call reads this clock (dispatch_graphics). On the PS5
 * clock_gettime may enter the kernel, so count invariant-TSC ticks instead,
 * calibrated once against CLOCK_MONOTONIC. Only this file compares these
 * values, all from this one clock. */
static pthread_once_t stat_tsc_once = PTHREAD_ONCE_INIT;
static uint64_t stat_tsc_base, stat_tsc_base_ns;
static double stat_ns_per_tick;

static void stat_tsc_calibrate(void)
{
    const uint64_t ns0 = stat_clock_ns(), tsc0 = __builtin_ia32_rdtsc();
    nanosleep(&(struct timespec){.tv_nsec = 20000000L}, NULL);
    const uint64_t ns1 = stat_clock_ns(), tsc1 = __builtin_ia32_rdtsc();
    stat_ns_per_tick = (double)(ns1 - ns0) / (double)(tsc1 - tsc0);
    stat_tsc_base = tsc1;
    stat_tsc_base_ns = ns1;

    enum { CALLS = 100000 };
    const uint64_t c0 = __builtin_ia32_rdtsc();
    uint64_t sink = 0;
    for (int i = 0; i < CALLS; ++i)
        sink += stat_clock_ns();
    const uint64_t c1 = __builtin_ia32_rdtsc();
    for (int i = 0; i < CALLS; ++i)
        sink += __builtin_ia32_rdtsc();
    const uint64_t c2 = __builtin_ia32_rdtsc();
    FILE *log = fopen("/data/pokopia/ps5-mem.txt", "a");
    if (log) {
        fprintf(log, "tsc %.3f MHz; clock_gettime %.1f ns/call, rdtsc %.1f ns/call (sink %llu)\n",
                1e3 / stat_ns_per_tick, (double)(c1 - c0) * stat_ns_per_tick / CALLS,
                (double)(c2 - c1) * stat_ns_per_tick / CALLS, (unsigned long long)(sink & 1));
        fclose(log);
    }
}

static uint64_t stat_now_ns(void)
{
    /* R94: pthread_once on every read showed in the dispatcher's profile. */
    static atomic_bool calibrated;
    if (__builtin_expect(!atomic_load_explicit(&calibrated, memory_order_acquire), 0)) {
        pthread_once(&stat_tsc_once, stat_tsc_calibrate);
        atomic_store_explicit(&calibrated, true, memory_order_release);
    }
    return stat_tsc_base_ns + (uint64_t)((double)(__builtin_ia32_rdtsc() - stat_tsc_base) * stat_ns_per_tick);
}

#if defined(POKOPIA_GFX)
static bool use_precise_main_sleep(void)
{
    return (pokopia_debug_flags & (1u << 25)) != 0;
}

#endif
#else
#define stat_now_ns stat_clock_ns
#endif

/* R60 loading-screen frame cap. Every rendered frame costs global-lock time
 * the loading threads need; while the Ditto loading screen is up, present at
 * most 10 times a second. Frames without the loading draw (menus, gameplay)
 * are never paced. The sleep itself happens in dispatch_graphics() after the
 * lock is released. */
enum { LOADING_FRAME_NS = 100 * 1000 * 1000 };
static atomic_bool frame_has_loading_draw;
static _Thread_local uint64_t present_pace_until;
static uint64_t last_loading_present_ns;
static atomic_uint_fast64_t stat_paced_ns, stat_paced_frames;
static uint64_t generic_draw_ok, generic_draw_failed; /* R61 general draw path, under the kernel lock */

static void pace_loading_present(void)
{
    const uint64_t now = stat_now_ns();
    if (!atomic_exchange_explicit(&frame_has_loading_draw, false, memory_order_relaxed)) {
        last_loading_present_ns = 0;
        return;
    }
    const uint64_t due = last_loading_present_ns ? last_loading_present_ns + LOADING_FRAME_NS : now;
    if (due > now) {
        present_pace_until = due;
        STAT_ADD(stat_paced_frames, 1);
    }
    last_loading_present_ns = due > now ? due : now;
}

/* R46: graphics time by NVN entry point. The dispatcher names the call;
 * the timing wrapper charges it. A short spinlock guards the small table. */
enum { STAT_NAMES = 128, STAT_NAME_BYTES = 48 };
typedef struct { char name[STAT_NAME_BYTES]; uint64_t calls, ns, max_ns; const char *key; } StatName;
static StatName stat_names[STAT_NAMES];
static atomic_flag stat_names_lock = ATOMIC_FLAG_INIT;
/* R78: the call name is a static string (the NVN proc table); it is kept as a
 * pointer and the counters are found by that pointer. A string copy and a
 * linear strcmp search on every graphics call cost ~1 us each. */
static _Thread_local const char *stat_call_name;
/* R89: depth of game command-memory callbacks on this thread; graphics calls
 * made inside one that take over 2 ms are logged. */
static _Thread_local unsigned memory_callback_depth;
extern uint64_t pokopia_live_draw_calls, pokopia_live_draw_ns, pokopia_live_pipeline_ns,
    pokopia_live_submit_ns, pokopia_live_setup_ns, pokopia_live_pipeline_hits, pokopia_live_pipeline_misses;

static void stat_name_call(const char *name)
{
    stat_call_name = name;
}

static void stat_charge_name(uint64_t ns, unsigned weight)
{
    const char *name = stat_call_name ? stat_call_name : "(unnamed)";
    while (atomic_flag_test_and_set_explicit(&stat_names_lock, memory_order_acquire))
        __builtin_ia32_pause();
    StatName *slot = NULL;
    size_t at = (size_t)(((uintptr_t)name >> 3) * UINT64_C(0x9e3779b97f4a7c15) >> 57) & (STAT_NAMES - 1);
    for (size_t probe = 0; probe < STAT_NAMES; ++probe, at = (at + 1) & (STAT_NAMES - 1)) {
        if (stat_names[at].key == name || !stat_names[at].name[0]) {
            slot = &stat_names[at];
            break;
        }
    }
    if (!slot)
        slot = &stat_names[STAT_NAMES - 1];
    if (!slot->name[0]) {
        size_t bytes = strlen(name);
        if (bytes >= STAT_NAME_BYTES)
            bytes = STAT_NAME_BYTES - 1;
        memcpy(slot->name, name, bytes);
        slot->name[bytes] = 0;
        slot->key = name;
    }
    slot->calls += weight;
    slot->ns += ns * weight;
    if (ns > slot->max_ns)
        slot->max_ns = ns;
    atomic_flag_clear_explicit(&stat_names_lock, memory_order_release);
}

/* R48: IPC requests by service, and per-thread busy time. "Busy" is wall
 * time between leaving one SVC and entering the next: guest code plus HLE
 * hooks such as NVN. A thread near the window length is the critical path. */
enum { STAT_IPC = 32, STAT_THREADS = 64 };
static atomic_uint_fast64_t stat_ipc[STAT_IPC];
static const char *const stat_ipc_names[STAT_IPC] = {
    "lm", "applet", "aoc", "pctl", "time", "ectx", "fsp", "acc", "audout-user", "audren-mgr",
    "auddev", "audout-mgr", "audout", "hwopus", "ssl", "stpl", "stpl-user", "offline-app",
    "hid", "hid-resource", "hid-vibration", "prepo", [STAT_IPC - 1] = "non-session",
};
typedef struct {
    atomic_uint_fast64_t id, busy_ns, svc_ns, svcs;
    atomic_uint_fast64_t svc_by_ns[STAT_SVCS]; /* R68: time per SVC number */
#if defined(__PROSPERO__)
    _Atomic(Ctx *) ctx;
    atomic_int in_svc; /* SVC number + 1 while inside one */
    /* R68: host-code sampling while the thread runs a graphics call */
    pthread_t host;
    atomic_int in_host, host_pc_ready;
    atomic_uint_fast64_t host_pc, host_caller, host_caller2, host_caller3; /* R76: first eboot return address when pc is in a system library */
    atomic_uint_fast64_t gfx_wait_ns, rec_wait_ns, gfx_hold_ns; /* R79: lock time by thread */
    /* R96: hang watchdog. The guest graphics call in progress, the thread's
     * stack bounds, and a deep scan of its stack for return addresses. */
    atomic_uint_fast64_t op_addr;
    uint64_t stack_lo, stack_hi;
    uint64_t hang_rip, hang_stack[48];
    atomic_int hang_n;
#endif
} StatThread;
static StatThread stat_threads[STAT_THREADS];
static atomic_uint stat_thread_count;
static _Thread_local StatThread *stat_thread;
/* R96: which thread (StatThread id; 1 when it has none) holds each host lock. */
static atomic_uint_fast64_t gfx_owner_id, rec_owner_id, kernel_owner_id;
static atomic_int prof_hang_deep;
static _Thread_local StatThread *lock_stats; /* R79: the replay worker's entry */
static uint64_t lock_owner_tag(void)
{
    StatThread *entry = lock_stats ? lock_stats : stat_thread;
    return entry ? (uint64_t)atomic_load_explicit(&entry->id, memory_order_relaxed) : 1;
}
static _Thread_local uint64_t stat_thread_left_svc;

#if defined(__PROSPERO__)
/* PS5 sampling profiler. A loading thread ran 173 s of "busy" time on the
 * PS5 against 75 s on a PS4 with no clue where. Every millisecond, read the
 * guest function each thread entered last (Ctx.fn) and the host import it is
 * in, for threads outside an SVC; every 10 s write the top entries to
 * /data/pokopia/ps5-prof.txt. Reads race the owning thread on purpose. */
enum { PROF_SLOTS = 8192, PROF_TOP = 24, PROF_PERIOD_US = 1000, PROF_REPORT_SAMPLES = 10000 };
typedef struct {
    uint64_t tid, fn;
    const char *import;
    uint32_t count;
} ProfSlot;
static ProfSlot prof_slots[PROF_SLOTS];
static pthread_once_t prof_once = PTHREAD_ONCE_INIT;
#if defined(POKOPIA_BOOT_PROBE)
static void prof_report_fast(FILE *log);
#endif

static void fatal_signal(int signal, siginfo_t *info, void *context);
/* R96: the memory scanner reads guest ranges that may not be backed; a
 * fault on its thread jumps back into it instead of ending the process. */
static _Thread_local volatile int memscan_guarded;
static _Thread_local jmp_buf memscan_jump; /* setjmp: sigsetjmp is not in the PS5 libc */
static _Atomic(StatThread *) prof_host_target;
static atomic_bool prof_raw_pc_only;


/* R68: runs on the sampled thread; records where its host code is. */
static void prof_host_signal(int signal, siginfo_t *info, void *context)
{
    (void)signal;
    (void)info;
    StatThread *thread = atomic_load_explicit(&prof_host_target, memory_order_acquire);
    /* A signal delayed beyond its request must not populate another thread's slot. */
    if (!thread || pthread_self() != thread->host)
        return;
    const ucontext_t *uc = context;
    const uint64_t rip = (uint64_t)uc->uc_mcontext.mc_rip;
    uint64_t caller = 0;
    if (atomic_load_explicit(&prof_hang_deep, memory_order_relaxed)) {
        /* R96: every word on the stack that is a return address into this
         * module (the byte before it ends a call instruction). */
        const uint64_t *stack = (const uint64_t *)uc->uc_mcontext.mc_rsp;
        const uint64_t rsp = (uint64_t)uc->uc_mcontext.mc_rsp;
        /* Only inside the thread's own recorded stack, and at most 64 KiB of it:
         * an unbounded scan killed the process during boot loading stalls. */
        uint64_t limit = 0;
        if (thread->stack_lo <= rsp && rsp < thread->stack_hi)
            limit = thread->stack_hi - rsp > 0x10000 ? rsp + 0x10000 : thread->stack_hi;
        int n = 0;
        for (const uint64_t *word = stack; (uint64_t)(word + 1) <= limit && n < 48 &&
                                           word < stack + 0x40000 / sizeof *word; ++word) {
            const uint64_t value = *word;
            if (value < UINT64_C(0x400010) || value >= UINT64_C(0x11e00000))
                continue;
            const uint8_t *p = (const uint8_t *)value;
            if (p[-5] == 0xe8 || (p[-2] == 0xff && ((p[-1] >> 3) & 7) == 2) ||
                (p[-3] == 0xff && ((p[-2] >> 3) & 7) == 2) || (p[-6] == 0xff && ((p[-5] >> 3) & 7) == 2) ||
                (p[-7] == 0xff && ((p[-6] >> 3) & 7) == 2))
                thread->hang_stack[n++] = value;
        }
        thread->hang_rip = rip;
        atomic_store_explicit(&thread->hang_n, n, memory_order_relaxed);
    }
    if (rip >= UINT64_C(0x800000000) && !atomic_load_explicit(&prof_raw_pc_only, memory_order_relaxed)) {
        /* R76: in a system library (kernel call stubs): find who called it. */
        const uint64_t *stack = (const uint64_t *)uc->uc_mcontext.mc_rsp;
        uint64_t caller2 = 0, caller3 = 0;
        for (int i = 0; i < 240 && !caller3; ++i)
            if (stack[i] >= UINT64_C(0x400000) && stack[i] < UINT64_C(0x20000000)) {
                if (!caller)
                    caller = stack[i];
                else if (!caller2 && stack[i] != caller)
                    caller2 = stack[i];
                else if (caller2 && stack[i] != caller2 && stack[i] != caller)
                    caller3 = stack[i];
            }
        atomic_store_explicit(&thread->host_caller2, caller2, memory_order_relaxed);
        atomic_store_explicit(&thread->host_caller3, caller3, memory_order_relaxed);
    }
    atomic_store_explicit(&thread->host_caller, caller, memory_order_relaxed);
    atomic_store_explicit(&thread->host_pc, rip, memory_order_relaxed);
    atomic_store_explicit(&thread->host_pc_ready, 1, memory_order_release);
}

static void prof_record(uint64_t tid, uint64_t fn, const char *import)
{
    uint64_t hash = (tid * 0x9e3779b97f4a7c15ull) ^ fn ^ (uint64_t)(uintptr_t)import;
    hash ^= hash >> 29;
    for (unsigned probe = 0; probe < PROF_SLOTS; ++probe) {
        ProfSlot *slot = &prof_slots[(hash + probe) & (PROF_SLOTS - 1)];
        if (!slot->count) {
            *slot = (ProfSlot){.tid = tid, .fn = fn, .import = import, .count = 1};
            return;
        }
        if (slot->tid == tid && slot->fn == fn && slot->import == import) {
            ++slot->count;
            return;
        }
    }
}

uint64_t pokopia_trace_guest_tid(void)
{
    return stat_thread ? (uint64_t)atomic_load(&stat_thread->id) : 0;
}

uint64_t pokopia_trace_guest_lr(void)
{
    Ctx *ctx = stat_thread ? atomic_load_explicit(&stat_thread->ctx, memory_order_acquire) : NULL;
    return ctx ? ctx->x[30] : 0;
}

#if defined(POKOPIA_GFX)
/* Capture a short cross-thread timeline in memory. Writing the trace after
 * capture avoids using time in the middle of the frames being measured. */
enum { POKOPIA_DEBUG_FRAME_TRACE = 1u << 28, FRAME_TRACE_ROWS = 32768 };
typedef struct {
    uint64_t ns, tid, first, second;
    const char *kind;
} FrameTraceRow;

enum { TRACE_FIRST = 1, TRACE_SECOND, TRACE_WRITING, TRACE_WAIT_OFF, TRACE_FINISHED };
static atomic_uint trace_phase = TRACE_FIRST;
static atomic_uint_fast64_t trace_render_wait_key;

static unsigned frame_trace_active(void)
{
    unsigned phase = atomic_load_explicit(&trace_phase, memory_order_acquire);
    if (!(pokopia_debug_flags & POKOPIA_DEBUG_FRAME_TRACE)) {
        if (phase == TRACE_WAIT_OFF)
            atomic_compare_exchange_strong(&trace_phase, &phase, TRACE_SECOND);
        return 0;
    }
    return phase == TRACE_FIRST || phase == TRACE_SECOND ? phase : 0;
}

static void frame_trace(const char *kind, uint64_t ns, uint64_t first, uint64_t second)
{
    static FrameTraceRow rows[FRAME_TRACE_ROWS];
    static unsigned count;
    static atomic_flag lock = ATOMIC_FLAG_INIT;
    const unsigned phase = frame_trace_active();
    if (!phase)
        return;
    while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire))
        __builtin_ia32_pause();
    bool write = false;
    if (phase == atomic_load_explicit(&trace_phase, memory_order_acquire)) {
        rows[count++] = (FrameTraceRow){ns ? ns : stat_now_ns(), pokopia_trace_guest_tid(), first, second, kind};
        if (count == FRAME_TRACE_ROWS) {
            atomic_store_explicit(&trace_phase, TRACE_WRITING, memory_order_release);
            count = 0;
            write = true;
        }
    }
    atomic_flag_clear_explicit(&lock, memory_order_release);
    if (!write)
        return;
    const char *tmp = "/data/pokopia/ps5-codex-sleep-timing.tmp";
    const char *path = phase == TRACE_FIRST ? "/data/pokopia/ps5-codex-sleep-timing-1.txt" :
                                             "/data/pokopia/ps5-codex-sleep-timing-2.txt";
    FILE *trace = fopen(tmp, "w");
    bool saved = false;
    if (trace) {
        for (unsigned i = 0; i < FRAME_TRACE_ROWS; ++i) {
            const FrameTraceRow *row = &rows[i];
            fprintf(trace, "%" PRIu64 " t%" PRIu64 " %s %" PRIu64 " %" PRIu64 "\n",
                    row->ns, row->tid, row->kind, row->first, row->second);
        }
        const bool complete = !ferror(trace);
        saved = fclose(trace) == 0 && complete && rename(tmp, path) == 0;
    }
    atomic_store_explicit(&trace_phase, saved && phase == TRACE_FIRST ? TRACE_WAIT_OFF : TRACE_FINISHED,
                          memory_order_release);
}

/* Follow SDK wrappers on the calling thread's permanently mapped main stack.
 * An unrecognized stack or incomplete chain leaves the original LR intact. */
static uint64_t trace_sleep_caller(const Ctx *context)
{
    const uint64_t raw = context->x[30];
#if defined(POKOPIA_BOOT_PROBE)
    const uint64_t sp = context->x[31];
    const uint64_t end = POKOPIA_BOOT_STACK_BASE + POKOPIA_BOOT_STACK_SIZE;
    if (context == &cpu && sp >= POKOPIA_BOOT_STACK_BASE && sp < end) {
        uint64_t lr = raw, fp = context->x[29];
        for (unsigned i = 0; i < 8 && (lr >> 32) == 0x75; ++i) {
            if (fp < sp || fp > end - 16 || (fp & 7))
                break;
            const uint64_t next = rd64(fp);
            lr = rd64(fp + 8);
            if ((lr >> 32) == 0x71)
                return lr;
            if (next <= fp)
                break;
            fp = next;
        }
    }
#endif
    return raw;
}

void pokopia_trace_producer(const char *kind, uint64_t ns, uint64_t first, uint64_t second)
{
    frame_trace(kind, ns, first, second);
}
#endif

/* R70: once a second, launch-relative progress: frames, RomFS reads, and
 * SleepThread accuracy. Covers the boot before the first present too. */
static atomic_uint_fast64_t timeline_presents;
static void timeline_tick(void)
{
    extern uint64_t hzn_fsp_romfs_reads, hzn_fsp_romfs_bytes, hzn_sleep_stats[5][3];
    extern uint64_t pokopia_boot_monotonic_ns(void);
    static FILE *log;
    static uint64_t last_presents, last_reads, last_bytes, last_sleep[5][3];
    if (!log)
        log = fopen("/data/pokopia/ps5-timeline.txt", "w");
    if (!log)
        return;
    const uint64_t presents = atomic_load_explicit(&timeline_presents, memory_order_relaxed);
    const uint64_t reads = hzn_fsp_romfs_reads, bytes = hzn_fsp_romfs_bytes;
    fprintf(log, "%.1f presents=%" PRIu64 " reads=%" PRIu64 " KiB=%.0f sleeps", pokopia_boot_monotonic_ns() * 1e-9,
            presents - last_presents, reads - last_reads, (bytes - last_bytes) / 1024.0);
    static const char *const names[5] = {"yield", "<=1ms", "<=4ms", "<=17ms", "long"};
    for (unsigned b = 0; b < 5; ++b) {
        const uint64_t n = hzn_sleep_stats[b][0] - last_sleep[b][0];
        if (n)
            fprintf(log, " %s=%" PRIu64 " req=%.2fms got=%.2fms", names[b], n,
                    (hzn_sleep_stats[b][1] - last_sleep[b][1]) * 1e-6 / n,
                    (hzn_sleep_stats[b][2] - last_sleep[b][2]) * 1e-6 / n);
        for (unsigned k = 0; k < 3; ++k)
            last_sleep[b][k] = hzn_sleep_stats[b][k];
    }
    extern uint64_t hzn_wait_stats[2][5];
    static uint64_t last_wait[2][5];
    for (unsigned kind = 0; kind < 2; ++kind) {
        uint64_t d[5];
        for (unsigned i = 0; i < 5; ++i) {
            d[i] = hzn_wait_stats[kind][i] - last_wait[kind][i];
            last_wait[kind][i] = hzn_wait_stats[kind][i];
        }
        if (d[0] + d[1])
            fprintf(log, " %s[signaled=%" PRIu64 " timeout=%" PRIu64 " infinite=%" PRIu64 " finite=%" PRIu64
                    " avg_timeout=%.2fms]", kind ? "addr" : "condvar", d[0], d[1], d[2], d[4],
                    d[4] ? d[3] * 1e-6 / d[4] : 0.0);
    }
    extern uint64_t hzn_wake_stats[6];
    static uint64_t last_wake[6];
    {
        uint64_t d[6];
        for (unsigned i = 0; i < 6; ++i)
            d[i] = i == 2 ? hzn_wake_stats[2] : hzn_wake_stats[i] - last_wake[i];
        for (unsigned i = 0; i < 6; ++i)
            last_wake[i] = hzn_wake_stats[i];
        hzn_wake_stats[2] = 0;
        if (d[0])
            fprintf(log, " wake[n=%" PRIu64 " avg=%.3fms max=%.2fms over1ms=%" PRIu64 " spurious=%" PRIu64
                    " completions=%" PRIu64 "]", d[0], d[1] * 1e-6 / d[0], d[2] * 1e-6, d[3], d[4], d[5]);
    }
    extern uint64_t hzn_timed_waits[24][4];
    static uint64_t last_timed[24][2];
    for (unsigned i = 0; i < 24 && hzn_timed_waits[i][2]; ++i) {
        const uint64_t waits = hzn_timed_waits[i][2] - last_timed[i][0];
        const uint64_t timeouts = hzn_timed_waits[i][3] - last_timed[i][1];
        if (waits)
            fprintf(log, " tw[t%" PRIu64 " %.1fms %" PRIu64 "/%" PRIu64 "]", hzn_timed_waits[i][0],
                    hzn_timed_waits[i][1] * 1e-6, timeouts, waits);
        last_timed[i][0] = hzn_timed_waits[i][2];
        last_timed[i][1] = hzn_timed_waits[i][3];
    }
    static uint64_t last_long[16];
    for (unsigned i = 0; i < 16 && hzn_long_sleeps[i].count; ++i) {
        const uint64_t n = hzn_long_sleeps[i].count - last_long[i];
        if (n)
            fprintf(log, " long[t%" PRIu64 " lr=%#" PRIx64 " %.0fms x%" PRIu64 "]", hzn_long_sleeps[i].id,
                    hzn_long_sleeps[i].lr, hzn_long_sleeps[i].ns * 1e-6, n);
        last_long[i] = hzn_long_sleeps[i].count;
    }
    fputc('\n', log);
    fflush(log);
    last_presents = presents;
    last_reads = reads;
    last_bytes = bytes;
}

static void prof_report(unsigned ticks, unsigned samples)
{
    FILE *log = fopen("/data/pokopia/ps5-prof.txt", "a");
    if (log) {
        fprintf(log, "--- %u ticks, %u busy samples (fatal_signal at %p for symbolize.sh)\n", ticks, samples,
                (void *)fatal_signal);
#if defined(POKOPIA_BOOT_PROBE)
        prof_report_fast(log);
#endif
        /* R68: wait samples only for threads that also ran (>= 3% of ticks),
         * so idle workers do not crowd the list. */
        enum { BUSY_TIDS = 32 };
        uint64_t tids[BUSY_TIDS];
        unsigned busy[BUSY_TIDS], tid_count = 0;
        for (unsigned i = 0; i < PROF_SLOTS; ++i) {
            const ProfSlot *slot = &prof_slots[i];
            if (!slot->count || (slot->import && !strncmp(slot->import, "svc-", 4)))
                continue;
            unsigned t = 0;
            while (t < tid_count && tids[t] != slot->tid)
                ++t;
            if (t == tid_count) {
                if (tid_count == BUSY_TIDS)
                    continue;
                tids[tid_count] = slot->tid;
                busy[tid_count++] = 0;
            }
            busy[t] += slot->count;
        }
        for (unsigned i = 0; i < PROF_SLOTS; ++i) {
            ProfSlot *slot = &prof_slots[i];
            if (!slot->count || !slot->import || strncmp(slot->import, "svc-", 4))
                continue;
            bool keep = false;
            for (unsigned t = 0; t < tid_count; ++t)
                keep |= tids[t] == slot->tid && busy[t] * 200u >= ticks;
            if (!keep)
                slot->count = 0;
        }
        /* R78: per-thread split of the samples: guest code, host code (graphics
         * calls), SVC waits, and native imports. */
        {
            enum { SPLIT_TIDS = 64 };
            uint64_t split_tid[SPLIT_TIDS];
            unsigned split[SPLIT_TIDS][4], split_count = 0;
            for (unsigned i = 0; i < PROF_SLOTS; ++i) {
                const ProfSlot *slot = &prof_slots[i];
                if (!slot->count)
                    continue;
                unsigned t = 0;
                while (t < split_count && split_tid[t] != slot->tid)
                    ++t;
                if (t == split_count) {
                    if (split_count == SPLIT_TIDS)
                        continue;
                    split_tid[split_count] = slot->tid;
                    memset(split[split_count++], 0, sizeof split[0]);
                }
                const char *import = slot->import;
                const unsigned kind = !import || !strcmp(import, "-") ? 0 :
                    !strncmp(import, "host", 4) ? (strncmp(import, "host-up", 7) ? 1 : 4) :
                    !strncmp(import, "svc-", 4) ? 2 : 3;
                if (kind < 4)
                    split[t][kind] += slot->count;
            }
            for (unsigned t = 0; t < split_count; ++t)
                if (split[t][0] + split[t][1] + split[t][3] > ticks / 50)
                    fprintf(log, "T t%" PRIu64 " guest=%u host=%u svc=%u import=%u\n", split_tid[t], split[t][0],
                            split[t][1], split[t][2], split[t][3]);
        }
        /* R76: every host sample with its exact pc, for per-function profiles
         * (ps5/scripts/prof-host.py symbolizes and sums them). */
        for (unsigned i = 0; i < PROF_SLOTS; ++i) {
            const ProfSlot *slot = &prof_slots[i];
            if (slot->count && slot->import && !strncmp(slot->import, "host", 4))
                fprintf(log, "H%s t%" PRIu64 " %#" PRIx64 " %u\n", !strcmp(slot->import, "host-up") ? "U" : !strcmp(slot->import, "host-up3") ? "V" :
                        slot->import[4] ? "S" : "", slot->tid, slot->fn,
                        slot->count);
        }
        for (int rank = 0; rank < PROF_TOP * 2; ++rank) {
            ProfSlot *best = NULL;
            for (unsigned i = 0; i < PROF_SLOTS; ++i)
                if (prof_slots[i].count && (!best || prof_slots[i].count > best->count))
                    best = &prof_slots[i];
            if (!best)
                break;
            fprintf(log, "t%" PRIu64 " fn=%#" PRIx64 " import=%s samples=%u\n", best->tid, best->fn,
                    best->import ? best->import : "-", best->count);
            best->count = 0;
        }
        fclose(log);
    }
    memset(prof_slots, 0, sizeof prof_slots);
}

/* Only prof_main issues requests, one at a time. */
static bool prof_request_pc(StatThread *thread)
{
    atomic_store_explicit(&thread->host_pc_ready, 0, memory_order_relaxed);
    atomic_store_explicit(&prof_host_target, thread, memory_order_release);
    if (!pthread_kill(thread->host, SIGUSR2)) {
        const uint64_t give_up = stat_clock_ns() + 500000;
        while (!atomic_load_explicit(&thread->host_pc_ready, memory_order_acquire) &&
               stat_clock_ns() < give_up) {}
    }
    atomic_store_explicit(&prof_host_target, NULL, memory_order_release);
    return atomic_load_explicit(&thread->host_pc_ready, memory_order_acquire) != 0;
}

/* R96: the host stack bounds of the calling thread, for the deep scan. */
static void stat_thread_stack_bounds(StatThread *thread)
{
    pthread_attr_t attr;
    void *base = NULL;
    size_t size = 0;
    if (pthread_attr_init(&attr))
        return;
    if (!pthread_attr_get_np(pthread_self(), &attr) && !pthread_attr_getstack(&attr, &base, &size) && base) {
        thread->stack_lo = (uint64_t)(uintptr_t)base;
        thread->stack_hi = (uint64_t)(uintptr_t)base + size;
    }
    pthread_attr_destroy(&attr);
}

/* R96: frames stopped. Write every thread's state, the owner of each host
 * lock and, for threads in host code, their stacks to ps5-hang.txt. Uses its
 * own file: a thread stuck inside probe_log's lock must not stop the dump. */
static void hang_dump(double stalled_s)
{
    FILE *out = fopen("/data/pokopia/ps5-hang.txt", "a");
    if (!out)
        return;
    extern uint64_t pokopia_boot_monotonic_ns(void);
    fprintf(out, "=== hang t=%.1fs no present for %.1fs anchor fatal_signal=%#" PRIx64 "\n",
            pokopia_boot_monotonic_ns() * 1e-9, stalled_s, (uint64_t)(uintptr_t)fatal_signal);
    fprintf(out, "owners gfx=%" PRIu64 " rec=%" PRIu64 " kernel=%" PRIu64 "\n",
            (uint64_t)atomic_load(&gfx_owner_id), (uint64_t)atomic_load(&rec_owner_id),
            (uint64_t)atomic_load(&kernel_owner_id));
    const unsigned count = atomic_load_explicit(&stat_thread_count, memory_order_acquire);
    for (unsigned i = 0; i < count && i < STAT_THREADS; ++i) {
        StatThread *thread = &stat_threads[i];
        const Ctx *ctx = atomic_load_explicit(&thread->ctx, memory_order_acquire);
        const uint64_t op = atomic_load_explicit(&thread->op_addr, memory_order_relaxed);
        const int in_host = atomic_load_explicit(&thread->in_host, memory_order_relaxed);
        const int in_svc = atomic_load_explicit(&thread->in_svc, memory_order_relaxed);
        const char *name = op && nvn ? nvn_front_call_name(nvn, op) : NULL;
        fprintf(out, "t%" PRIu64 " host=%d svc=%#x op=%#" PRIx64 " %s fn=%#" PRIx64 " lr=%#" PRIx64,
                (uint64_t)atomic_load(&thread->id), in_host, in_svc ? in_svc - 1 : 0, op, name ? name : "-",
                ctx ? ctx->fn : 0, ctx ? ctx->x[30] : 0);
        /* Only threads in host code, as the profiler samples them every
         * millisecond: signalling threads blocked in system calls killed the
         * process during a boot loading stall. */
        if (ctx && in_host && thread->stack_hi) {
            atomic_store_explicit(&thread->hang_n, -1, memory_order_relaxed);
            atomic_store_explicit(&prof_hang_deep, 1, memory_order_relaxed);
            const bool captured = prof_request_pc(thread);
            atomic_store_explicit(&prof_hang_deep, 0, memory_order_relaxed);
            const int n = atomic_load_explicit(&thread->hang_n, memory_order_relaxed);
            if (captured && n >= 0) {
                fprintf(out, " rip=%#" PRIx64 " stack", thread->hang_rip);
                for (int k = 0; k < n; ++k)
                    fprintf(out, " %#" PRIx64, thread->hang_stack[k]);
            }
        }
        fputc('\n', out);
    }
    fclose(out);
}

#if defined(POKOPIA_GFX)
/* The SVC owner publishes a unique token only inside its matching sleep.
 * All counters, PC slots and the selection cursor are owned by prof_main. */
static atomic_uint_fast64_t wait_work_gate;
static atomic_bool wait_work_finished;
enum { WAIT_WORK_TICKS = 5000, WAIT_WORK_PC_SLOTS = 8192 };
enum WaitWorkState { WAIT_GUEST, WAIT_HOST, WAIT_SVC, WAIT_IMPORT, WAIT_IDLE, WAIT_STATES };
typedef struct {
    uint64_t tid;
    unsigned states[WAIT_STATES], attempted, sampled;
} WaitWorkThread;
typedef struct {
    uint64_t tid, pc;
    unsigned count;
} WaitWorkPc;
static WaitWorkThread wait_work_threads[STAT_THREADS];
static WaitWorkPc wait_work_pcs[WAIT_WORK_PC_SLOTS];

static enum WaitWorkState wait_work_state(StatThread *thread, const Ctx *ctx, uint64_t tid)
{
    if (atomic_load_explicit(&thread->in_svc, memory_order_relaxed))
        return WAIT_SVC;
    if (atomic_load_explicit(&thread->in_host, memory_order_relaxed))
        return WAIT_HOST;
    if (tid == 999)
        return WAIT_IDLE;
    const char *import = *(const char *volatile *)&ctx->in_import;
    return import && strcmp(import, "-") ? WAIT_IMPORT : WAIT_GUEST;
}

static void wait_work_record(uint64_t tid, uint64_t pc)
{
    const uint64_t hash = (pc ^ (tid * UINT64_C(0x9e3779b97f4a7c15))) * UINT64_C(0xbf58476d1ce4e5b9);
    for (unsigned probe = 0; probe < WAIT_WORK_PC_SLOTS; ++probe) {
        WaitWorkPc *slot = &wait_work_pcs[((hash >> 32) + probe) % WAIT_WORK_PC_SLOTS];
        if (!slot->count || (slot->pc == pc && slot->tid == tid)) {
            slot->pc = pc;
            slot->tid = tid;
            ++slot->count;
            return;
        }
    }
}

static bool wait_work_save(unsigned ticks, uint64_t first, uint64_t last)
{
    const char *tmp = "/data/pokopia/ps5-codex-wait-work.tmp";
    FILE *file = fopen(tmp, "w");
    if (!file)
        return false;
    fprintf(file, "WAIT_WORK_V1 fatal_signal=%#" PRIxPTR " gate_ticks=%u started_ns=%" PRIu64
                  " ended_ns=%" PRIu64 "\n", (uintptr_t)fatal_signal, ticks, first, last);
    for (unsigned i = 0; i < STAT_THREADS; ++i) {
        const WaitWorkThread *thread = &wait_work_threads[i];
        if (thread->tid)
            fprintf(file, "T t%" PRIu64 " guest=%u host=%u svc=%u import=%u idle=%u attempted=%u sampled=%u\n",
                    thread->tid, thread->states[WAIT_GUEST], thread->states[WAIT_HOST], thread->states[WAIT_SVC],
                    thread->states[WAIT_IMPORT], thread->states[WAIT_IDLE], thread->attempted, thread->sampled);
    }
    for (unsigned i = 0; i < WAIT_WORK_PC_SLOTS; ++i) {
        const WaitWorkPc *slot = &wait_work_pcs[i];
        if (slot->count)
            fprintf(file, "P t%" PRIu64 " %#" PRIx64 " %u\n", slot->tid, slot->pc, slot->count);
    }
    const bool complete = !ferror(file);
    return fclose(file) == 0 && complete && rename(tmp, "/data/pokopia/ps5-codex-wait-work.txt") == 0;
}

static void wait_work_tick(unsigned count)
{
    static unsigned ticks, cursor;
    static uint64_t first, retry_at;
    if (!(pokopia_debug_flags & POKOPIA_DEBUG_FRAME_TRACE) ||
        atomic_load_explicit(&wait_work_finished, memory_order_relaxed))
        return;
    const uint64_t gate = atomic_load_explicit(&wait_work_gate, memory_order_acquire);
    if (!first && !gate)
        return;
    const uint64_t now = stat_now_ns();
    if (!first)
        first = now;
    if (ticks == WAIT_WORK_TICKS || now - first >= UINT64_C(60000000000)) {
        if (now >= retry_at) {
            if (wait_work_save(ticks, first, now))
                atomic_store_explicit(&wait_work_finished, true, memory_order_release);
            retry_at = now + UINT64_C(1000000000);
        }
        return;
    }
    if (!gate || !count)
        return;
    ++ticks;
    if (count > STAT_THREADS)
        count = STAT_THREADS;
    int selected = -1;
    for (unsigned offset = 0; offset < count; ++offset) {
        const unsigned i = (cursor + offset) % count;
        StatThread *thread = &stat_threads[i];
        const Ctx *ctx = atomic_load_explicit(&thread->ctx, memory_order_acquire);
        if (!ctx)
            continue;
        const uint64_t tid = atomic_load_explicit(&thread->id, memory_order_relaxed);
        const enum WaitWorkState state = wait_work_state(thread, ctx, tid);
        wait_work_threads[i].tid = tid;
        ++wait_work_threads[i].states[state];
        if (selected < 0 && state == WAIT_GUEST && tid && tid != 1 && tid != 24 && tid != 999)
            selected = (int)i;
    }
    if (selected < 0 || gate != atomic_load_explicit(&wait_work_gate, memory_order_acquire))
        return;
    cursor = ((unsigned)selected + 1) % count;
    StatThread *thread = &stat_threads[selected];
    WaitWorkThread *worker = &wait_work_threads[selected];
    ++worker->attempted;
    atomic_store_explicit(&prof_raw_pc_only, true, memory_order_relaxed);
    const bool captured = prof_request_pc(thread);
    atomic_store_explicit(&prof_raw_pc_only, false, memory_order_relaxed);
    const Ctx *ctx = atomic_load_explicit(&thread->ctx, memory_order_acquire);
    if (captured && ctx && gate == atomic_load_explicit(&wait_work_gate, memory_order_acquire) &&
        wait_work_state(thread, ctx, worker->tid) == WAIT_GUEST) {
        const uint64_t pc = atomic_load_explicit(&thread->host_pc, memory_order_relaxed);
        wait_work_record(worker->tid, pc);
        ++worker->sampled;
    }
}
#endif

static void memscan_poll(void);
static bool memscan_copy(void *destination, uint64_t address, size_t size);
static void *prof_main(void *unused)
{
    (void)unused;
    unsigned ticks = 0, samples = 0;
    for (;;) {
        nanosleep(&(struct timespec){.tv_nsec = PROF_PERIOD_US * 1000L}, NULL);
        ++ticks;
        if (ticks % 16 == 0)
            hzn_hid_ps4_refresh(); /* R96: keep rumble alive (the PS5 drops unrepeated values) */
        if (native_pad >= 0) { /* every 1 ms tick: a name stays in the slot only ~10 ms */
            /* R96: the game's text slots at 0x71036bdff0 (formatted on-screen
             * text) and 0x71036bbff0 (message templates); the light bar takes
             * the colour of a Pokemon name in either. Diagnostics log every
             * change (first 100). */
            static const uint64_t slots[2] = {UINT64_C(0x71036bdff0), UINT64_C(0x71036bbff0)};
            static char last[2][48];
            static unsigned changes;
            for (int k = 0; k < 2; ++k) {
                char label[48];
                if (!memscan_copy(label, slots[k], sizeof label - 1))
                    continue;
                label[sizeof label - 1] = 0;
                if (!strcmp(label, last[k]))
                    continue;
                memcpy(last[k], label, sizeof label);
                hzn_hid_ps4_lightbar_label(native_pad, label);
                if (changes < 100) {
                    ++changes;
                    for (size_t i = 0; i < sizeof label && label[i]; ++i)
                        if (label[i] == 10 || label[i] == 13)
                            label[i] = 32;
                    fprintf(probe_log, "Label slot%d t=%.2fs [%s]%c", k, pokopia_boot_monotonic_ns() * 1e-9, label, 10);
                    fflush(probe_log);
                }
            }
        }
        if (ticks % 1000 == 0) {
            timeline_tick();
            memscan_poll(); /* R96 */
            /* R96: rumble experiment. debug/rumble holds "kind mode" (see
             * hzn_hid_ps4_rumble_mode); switch the pad's vibration mode, then
             * pulse both motors at full strength for a second. */
            FILE *rumble = fopen("/data/pokopia/debug/rumble", "r");
            if (rumble) {
                int kind = 0, mode = 0;
                const int fields = fscanf(rumble, "%d %d", &kind, &mode);
                fclose(rumble);
                unlink("/data/pokopia/debug/rumble");
                extern int hzn_hid_ps4_rumble_mode(int handle, int kind, int mode);
                extern int hzn_hid_ps4_pulse(int handle, uint8_t strength);
                const int set = fields >= 1 ? hzn_hid_ps4_rumble_mode(native_pad, kind, mode) : -1;
                const int on = hzn_hid_ps4_pulse(native_pad, 255);
                nanosleep(&(struct timespec){.tv_sec = 1}, NULL);
                const int off = hzn_hid_ps4_pulse(native_pad, 0);
                fprintf(probe_log, "Rumble test kind=%d mode=%d set=%#x pulse=%#x off=%#x pad=%d\n", kind, mode,
                        (uint32_t)set, (uint32_t)on, (uint32_t)off, native_pad);
                fflush(probe_log);
            }
            /* R96: hang watchdog. Once frames have started, four seconds with
             * no present writes ps5-hang.txt (then again every 60 s, 5 times). */
            static uint64_t seen_presents, stalled_ticks, dumps;
            const uint64_t now_presents = atomic_load_explicit(&timeline_presents, memory_order_relaxed);
            if (now_presents != seen_presents || !now_presents) {
                seen_presents = now_presents;
                stalled_ticks = 0;
            } else if (++stalled_ticks == 4 || (stalled_ticks > 4 && stalled_ticks % 60 == 4 && dumps < 5)) {
                ++dumps;
                hang_dump(stalled_ticks * (PROF_PERIOD_US * 1000.0) * 1e-6);
            }
        }
        /* R94: debug flags bit 16 pauses sampling (a SIGUSR2 to a thread in
         * host code every millisecond, the replay worker nearly always). */
        if (pokopia_debug_flags & (1u << 16)) {
            if (ticks >= PROF_REPORT_SAMPLES)
                ticks = samples = 0;
            continue;
        }
        const unsigned count = atomic_load_explicit(&stat_thread_count, memory_order_acquire);
#if defined(POKOPIA_GFX)
        wait_work_tick(count);
#endif
        for (unsigned i = 0; i < count && i < STAT_THREADS; ++i) {
            StatThread *thread = &stat_threads[i];
            Ctx *ctx = atomic_load_explicit(&thread->ctx, memory_order_acquire);
            if (!ctx)
                continue;
            if (atomic_load_explicit(&thread->in_host, memory_order_relaxed)) {
                if (prof_request_pc(thread)) {
                    const uint64_t caller = atomic_load_explicit(&thread->host_caller, memory_order_relaxed);
                    prof_record(atomic_load_explicit(&thread->id, memory_order_relaxed),
                                caller ? caller : atomic_load_explicit(&thread->host_pc, memory_order_relaxed),
                                caller ? "host-sys" : "host");
                    /* R76: the caller's caller, recorded separately as "host-up" */
                    const uint64_t caller2 = caller ?
                        atomic_load_explicit(&thread->host_caller2, memory_order_relaxed) : 0;
                    if (caller2)
                        prof_record(atomic_load_explicit(&thread->id, memory_order_relaxed), caller2, "host-up");
                    const uint64_t caller3 = caller ?
                        atomic_load_explicit(&thread->host_caller3, memory_order_relaxed) : 0;
                    if (caller3)
                        prof_record(atomic_load_explicit(&thread->id, memory_order_relaxed), caller3, "host-up3");
                    ++samples;
                    continue;
                }
            }
            const int in_svc = atomic_load_explicit(&thread->in_svc, memory_order_relaxed);
            if (in_svc) {
                /* R68: where blocked threads wait: SVC number and the guest return address. */
                static const char *const svc_labels[] = {
                    [0x0b] = "svc-sleep", [0x18] = "svc-wait-sync", [0x1a] = "svc-arbitrate-lock",
                    [0x1b] = "svc-arbitrate-unlock", [0x1c] = "svc-wait-condvar", [0x1d] = "svc-signal-condvar",
                    [0x34] = "svc-wait-for-address", [0x35] = "svc-signal-to-address",
                };
                const unsigned number = (unsigned)in_svc - 1;
                const char *label = number < sizeof svc_labels / sizeof svc_labels[0] && svc_labels[number] ?
                    svc_labels[number] : "svc-other";
                /* R71: walk up out of the SDK wrappers (0x75...) to the game's caller.
                 * Frames must sit within 1 MiB above the thread's stack pointer. */
                uint64_t lr = *(volatile uint64_t *)&ctx->x[30], fp = *(volatile uint64_t *)&ctx->x[29];
                const uint64_t sp = *(volatile uint64_t *)&ctx->x[31];
                for (int depth = 0; depth < 8 && (lr >> 32) == 0x75; ++depth) {
                    if (fp < sp || fp - sp > (UINT64_C(1) << 20) || (fp & 7))
                        break;
                    const uint64_t next_fp = rd64(fp);
                    lr = rd64(fp + 8);
                    if (next_fp <= fp)
                        break;
                    fp = next_fp;
                }
                prof_record(atomic_load_explicit(&thread->id, memory_order_relaxed), lr, label);
                continue;
            }
            const uint64_t fn = *(volatile uint64_t *)&ctx->fn;
            const char *import = *(const char *volatile *)&ctx->in_import;
            prof_record(atomic_load_explicit(&thread->id, memory_order_relaxed), fn, import);
            ++samples;
        }
        if (ticks >= PROF_REPORT_SAMPLES) {
            prof_report(ticks, samples);
            ticks = samples = 0;
        }
    }
    return NULL;
}

static void prof_start(void)
{
    struct sigaction action = {0};
    action.sa_sigaction = (void (*)(int, struct __siginfo *, void *))prof_host_signal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR2, &action, NULL);
    pthread_t thread;
    if (!pthread_create(&thread, NULL, prof_main, NULL))
        pthread_detach(thread);
}
#endif

static void report_thread_stats(FILE *log)
{
    fputs("IPC by service", log);
    for (unsigned i = 0; i < STAT_IPC; ++i) {
        const uint64_t calls = STAT_TAKE(stat_ipc[i]);
        if (calls)
            fprintf(log, " %s=%" PRIu64, stat_ipc_names[i] ? stat_ipc_names[i] : "?", calls);
    }
    fprintf(log, " | sync-word cache fast=%" PRIu64 " locked=%" PRIu64 " | nvn builder calls lock-free=%" PRIu64
            " locked=%" PRIu64, (uint64_t)STAT_TAKE(stat_sync_fast), (uint64_t)STAT_TAKE(stat_sync_slow),
            (uint64_t)STAT_TAKE(stat_pure_fast), (uint64_t)STAT_TAKE(stat_pure_locked));
    fprintf(log, " | general draws ok=%" PRIu64 " failed=%" PRIu64, generic_draw_ok, generic_draw_failed);
    generic_draw_ok = generic_draw_failed = 0;
    fprintf(log, " | loading frame cap paced=%" PRIu64 " slept_ms=%.0f", (uint64_t)STAT_TAKE(stat_paced_frames),
            STAT_TAKE(stat_paced_ns) * 1e-6);
    fputs(" | Thread busy_ms/svc_ms/svcs", log);
    const unsigned count = atomic_load_explicit(&stat_thread_count, memory_order_acquire);
    for (unsigned i = 0; i < count && i < STAT_THREADS; ++i) {
        StatThread *thread = &stat_threads[i];
        const uint64_t busy = STAT_TAKE(thread->busy_ns), svc = STAT_TAKE(thread->svc_ns), n = STAT_TAKE(thread->svcs);
        if (busy >= UINT64_C(20000000) || n >= 1000)
            fprintf(log, " t%" PRIu64 "=%.0f/%.0f/%" PRIu64, (uint64_t)atomic_load(&thread->id), busy * 1e-6, svc * 1e-6, n);
    }
    fputc('\n', log);
    /* R68: for threads that did real work, the SVCs their wait time went to. */
    fputs("Thread waits", log);
    for (unsigned i = 0; i < count && i < STAT_THREADS; ++i) {
        StatThread *thread = &stat_threads[i];
        uint64_t by[STAT_SVCS];
        for (unsigned s = 0; s < STAT_SVCS; ++s)
            by[s] = STAT_TAKE(thread->svc_by_ns[s]);
        uint64_t total = 0;
        for (unsigned s = 0; s < STAT_SVCS; ++s)
            total += by[s];
        if (total < UINT64_C(100000000))
            continue;
        fprintf(log, " t%" PRIu64 "[", (uint64_t)atomic_load(&thread->id));
        for (unsigned rank = 0; rank < 3; ++rank) {
            unsigned best = STAT_SVCS;
            for (unsigned s = 0; s < STAT_SVCS; ++s)
                if (by[s] && (best == STAT_SVCS || by[s] > by[best]))
                    best = s;
            if (best == STAT_SVCS)
                break;
            fprintf(log, "%s%#x=%.0f", rank ? " " : "", best, by[best] * 1e-6);
            by[best] = 0;
        }
        fputc(']', log);
    }
    fputc('\n', log);
}

static void report_stat_names(FILE *log, uint64_t window_ns)
{
#if !defined(__PROSPERO__) || !defined(POKOPIA_GFX)
    (void)window_ns;
#endif
    StatName copy[STAT_NAMES];
    while (atomic_flag_test_and_set_explicit(&stat_names_lock, memory_order_acquire))
        __builtin_ia32_pause();
    memcpy(copy, stat_names, sizeof copy);
    memset(stat_names, 0, sizeof stat_names);
    atomic_flag_clear_explicit(&stat_names_lock, memory_order_release);
    fputs("Graphics stats top", log);
    for (int rank = 0; rank < 14; ++rank) {
        StatName *best = NULL;
        for (size_t i = 0; i < STAT_NAMES; ++i)
            if (copy[i].calls && (!best || copy[i].ns > best->ns))
                best = &copy[i];
        if (!best)
            break;
        fprintf(log, " %s=%" PRIu64 "/%.1fms/max%.2fms", best->name, best->calls, best->ns * 1e-6, best->max_ns * 1e-6);
        best->calls = 0;
    }
    fprintf(log, " | live_draw calls=%" PRIu64 " ms=%.1f setup_ms=%.1f pipeline_ms=%.1f submit_wait_ms=%.1f"
        " pipeline_cache hits=%" PRIu64 " misses=%" PRIu64 "\n",
        pokopia_live_draw_calls, pokopia_live_draw_ns * 1e-6, pokopia_live_setup_ns * 1e-6,
        pokopia_live_pipeline_ns * 1e-6, pokopia_live_submit_ns * 1e-6,
        pokopia_live_pipeline_hits, pokopia_live_pipeline_misses);
    {
        extern uint64_t pokopia_generic_draw_calls, pokopia_generic_draw_ns, pokopia_generic_phase_ns[7];
        extern uint64_t generic_prepare_ns, generic_exec_ns;
        fprintf(log, "General draw timing calls=%" PRIu64 " prepare_ms=%.1f exec_ms=%.1f inner_ms=%.1f"
                " feedback=%.1f upload=%.1f descriptors=%.1f pipeline=%.1f record=%.1f submit_wait=%.1f release=%.1f\n",
                pokopia_generic_draw_calls, generic_prepare_ns * 1e-6, generic_exec_ns * 1e-6,
                pokopia_generic_draw_ns * 1e-6, pokopia_generic_phase_ns[0] * 1e-6, pokopia_generic_phase_ns[1] * 1e-6,
                pokopia_generic_phase_ns[2] * 1e-6, pokopia_generic_phase_ns[3] * 1e-6,
                pokopia_generic_phase_ns[4] * 1e-6, pokopia_generic_phase_ns[5] * 1e-6,
                pokopia_generic_phase_ns[6] * 1e-6);
        extern uint64_t pokopia_generic_upload_bytes[3], pokopia_generic_compute_calls, pokopia_generic_compute_ns;
        extern uint64_t pokopia_generic_batches, pokopia_generic_reused_bytes, pokopia_generic_merged,
            pokopia_generic_direct_bytes;
        extern uint64_t pokopia_generic_compute_batched;
        fprintf(log, "General draw direct guest reads MiB=%.1f | compute batched=%" PRIu64 "\n",
                pokopia_generic_direct_bytes / 1048576.0, pokopia_generic_compute_batched);
        pokopia_generic_direct_bytes = pokopia_generic_compute_batched = 0;
        fprintf(log, "General draw upload MiB uniforms=%.1f vertices=%.1f indices=%.1f | general compute calls=%" PRIu64
                " ms=%.1f | draw batches=%" PRIu64 " reused MiB=%.1f merged draws=%" PRIu64 "\n",
                pokopia_generic_upload_bytes[0] / 1048576.0,
                pokopia_generic_upload_bytes[1] / 1048576.0, pokopia_generic_upload_bytes[2] / 1048576.0,
                pokopia_generic_compute_calls, pokopia_generic_compute_ns * 1e-6, pokopia_generic_batches,
                pokopia_generic_reused_bytes / 1048576.0, pokopia_generic_merged);
        pokopia_generic_batches = pokopia_generic_reused_bytes = pokopia_generic_merged = 0;
        {
            extern uint64_t gfx_wait_ns, gfx_wait_count, gfx_yields, replay_worker_waits, replay_worker_wait_ns;
            extern uint64_t guest_slow_reads, guest_slow_bytes;
            fprintf(log, "Replay worker waits=%" PRIu64 " ms=%.1f | worker locked guest reads=%" PRIu64 " KiB=%.0f\n",
                    replay_worker_waits, replay_worker_wait_ns * 1e-6, guest_slow_reads, guest_slow_bytes / 1024.0);
            replay_worker_waits = replay_worker_wait_ns = guest_slow_reads = guest_slow_bytes = 0;
            extern uint64_t pokopia_present_ring_waits, pokopia_present_ring_wait_ns, pokopia_present_acquire_ns;
            extern uint64_t pokopia_readback_fast, pokopia_readback_slow, pokopia_gpu_batch_ns, pokopia_gpu_batches;
            extern uint64_t pokopia_linear_sync_ns, pokopia_linear_sync_calls, pokopia_linear_sync_bytes;
            extern uint64_t pokopia_prepare_skips;
            extern uint64_t pokopia_generic_submit_ns, pokopia_generic_submits;
            fprintf(log, "Batch vkQueueSubmit calls=%" PRIu64 " ms=%.1f\n", pokopia_generic_submits,
                    pokopia_generic_submit_ns * 1e-6);
            pokopia_generic_submits = pokopia_generic_submit_ns = 0;
            extern uint64_t pokopia_generic_binds_skipped;
            fprintf(log, "Pipeline binds skipped=%" PRIu64 "\n", pokopia_generic_binds_skipped);
            pokopia_generic_binds_skipped = 0;
            extern uint64_t pokopia_inline_uploads, pokopia_inline_bytes;
            fprintf(log, "Texture prepare skipped (prepared this submit)=%" PRIu64 " | inline uploads=%" PRIu64
                         " KiB=%.0f\n", pokopia_prepare_skips, pokopia_inline_uploads, pokopia_inline_bytes / 1024.0);
            pokopia_prepare_skips = pokopia_inline_uploads = pokopia_inline_bytes = 0;
            fprintf(log, "GPU draw batches=%" PRIu64 " busy_ms=%.1f | linear sync ms=%.1f compares=%" PRIu64
                         " MiB=%.0f\n", pokopia_gpu_batches, pokopia_gpu_batch_ns * 1e-6, pokopia_linear_sync_ns * 1e-6,
                    pokopia_linear_sync_calls, pokopia_linear_sync_bytes / 1048576.0);
#if defined(__PROSPERO__) && defined(POKOPIA_GFX)
            /* Share the existing timestamp window with the HUD. These spans cover draw batches. */
            overlay_gpu_draw_percent = -1;
            if (pokopia_gpu_batches && window_ns) {
                const double usage = 100.0 * (double)pokopia_gpu_batch_ns / (double)window_ns;
                overlay_gpu_draw_percent = (int)(usage >= 100.0 ? 100.0 : usage + 0.5);
            }
#endif
            pokopia_gpu_batches = pokopia_gpu_batch_ns = 0;
            pokopia_linear_sync_ns = pokopia_linear_sync_calls = pokopia_linear_sync_bytes = 0;
            fprintf(log, "Present GPU-behind waits=%" PRIu64 " ms=%.1f | acquire ms=%.1f | readbacks shadow=%" PRIu64
                         " waited=%" PRIu64 "\n", pokopia_present_ring_waits,
                    pokopia_present_ring_wait_ns * 1e-6, pokopia_present_acquire_ns * 1e-6, pokopia_readback_fast,
                    pokopia_readback_slow);
            pokopia_present_ring_waits = pokopia_present_ring_wait_ns = pokopia_present_acquire_ns = 0;
            pokopia_readback_fast = pokopia_readback_slow = 0;
            extern uint64_t rec_wait_ns, rec_wait_count;
            fprintf(log, "Graphics lock waits=%" PRIu64 " ms=%.1f yields=%" PRIu64 " | recording lock waits=%" PRIu64
                         " ms=%.1f | by thread gfx_wait/rec_wait/gfx_hold ms",
                    gfx_wait_count, gfx_wait_ns * 1e-6, gfx_yields, rec_wait_count, rec_wait_ns * 1e-6);
            gfx_wait_ns = gfx_wait_count = gfx_yields = rec_wait_ns = rec_wait_count = 0;
#if defined(__PROSPERO__)
            const unsigned threads = atomic_load_explicit(&stat_thread_count, memory_order_acquire);
            for (unsigned i = 0; i < threads && i < STAT_THREADS; ++i) {
                StatThread *thread = &stat_threads[i];
                const uint64_t gw = STAT_TAKE(thread->gfx_wait_ns), rw = STAT_TAKE(thread->rec_wait_ns),
                               gh = STAT_TAKE(thread->gfx_hold_ns);
                if (gw + rw + gh >= UINT64_C(5000000))
                    fprintf(log, " t%" PRIu64 "=%.0f/%.0f/%.0f", (uint64_t)atomic_load(&thread->id), gw * 1e-6,
                            rw * 1e-6, gh * 1e-6);
            }
#endif
            fputc('\n', log);
#if defined(__PROSPERO__)
            extern void gfx_sites_report(FILE *log);
            gfx_sites_report(log);
#endif
        }
        fprintf(log, "Draw batch flushes upload=%" PRIu64 " close_image=%" PRIu64 " texture_close=%" PRIu64
                " submit_wait=%" PRIu64 " texel_close=%" PRIu64 " texel_refresh=%" PRIu64 " finish=%" PRIu64
                " sync=%" PRIu64 " present=%" PRIu64 " queue_submit=%" PRIu64 "\n",
                pokopia_vk_flush_counts[0], pokopia_vk_flush_counts[1], pokopia_vk_flush_counts[2],
                pokopia_vk_flush_counts[3], pokopia_vk_flush_counts[4], pokopia_vk_flush_counts[5],
                pokopia_vk_flush_counts[6], pokopia_vk_flush_counts[7], pokopia_vk_flush_counts[8],
                pokopia_vk_flush_counts[9]);
        memset(pokopia_vk_flush_counts, 0, sizeof pokopia_vk_flush_counts);
        fputs("Draw batch flush sites", log);
        for (size_t i = 0; i < 16 && pokopia_vk_flush_sites[i].site; ++i) {
            const char *site = strrchr(pokopia_vk_flush_sites[i].site, '/');
            fprintf(log, " %s=%" PRIu64, site ? site + 1 : pokopia_vk_flush_sites[i].site,
                    pokopia_vk_flush_sites[i].count);
            pokopia_vk_flush_sites[i].count = 0;
        }
        fputc('\n', log);
        memset(pokopia_generic_upload_bytes, 0, sizeof pokopia_generic_upload_bytes);
        pokopia_generic_compute_calls = pokopia_generic_compute_ns = 0;
        pokopia_generic_draw_calls = pokopia_generic_draw_ns = generic_prepare_ns = generic_exec_ns = 0;
        extern uint64_t nvn_replay_op_ns[8][2], nvn_replay_op_calls[8][2], nvn_replay_submit_ns;
        static const char *const op_names[8] = {"copy", "clear", "depth_clear", "draw", "dispatch", "sync",
                                                "timestamp", "copy_texture"};
        fprintf(log, "Replay timing submit_ms=%.1f", nvn_replay_submit_ns * 1e-6);
        for (int pass = 0; pass < 2; ++pass)
            for (int op = 0; op < 8; ++op)
                if (nvn_replay_op_calls[op][pass])
                    fprintf(log, " %s%s=%" PRIu64 "/%.1fms", pass ? "" : "check:", op_names[op],
                            nvn_replay_op_calls[op][pass], nvn_replay_op_ns[op][pass] * 1e-6);
        fputc('\n', log);
        memset(nvn_replay_op_ns, 0, sizeof nvn_replay_op_ns);
        memset(nvn_replay_op_calls, 0, sizeof nvn_replay_op_calls);
        nvn_replay_submit_ns = 0;
        memset(pokopia_generic_phase_ns, 0, sizeof pokopia_generic_phase_ns);
    }
    pokopia_live_pipeline_hits = pokopia_live_pipeline_misses = 0;
    pokopia_live_draw_calls = pokopia_live_draw_ns = pokopia_live_pipeline_ns = 0;
    pokopia_live_submit_ns = pokopia_live_setup_ns = 0;
}
#endif

static void kernel_lock(void)
{
    GuestBoundary *scope = pokopia_host_thread();
#ifndef _WIN32
    const uint64_t start = stat_now_ns();
    bool contended = !pokopia_host_trylock();
    if (contended)
        pokopia_host_lock();
    stat_lock_acquired = stat_now_ns();
    STAT_ADD(stat_lock_calls, 1);
    if (contended) {
        STAT_ADD(stat_lock_contended, 1);
        STAT_ADD(stat_lock_wait_ns, stat_lock_acquired - start);
    }
#else
    pokopia_host_lock();
#endif
    if (scope)
        scope->kernel_locked = true;
    atomic_store_explicit(&kernel_owner_id, lock_owner_tag(), memory_order_relaxed);
}

static void kernel_unlock(void)
{
    GuestBoundary *scope = pokopia_host_thread();
    if (scope)
        scope->kernel_locked = false;
    atomic_store_explicit(&kernel_owner_id, 0, memory_order_relaxed);
#ifndef _WIN32
    const uint64_t held = stat_now_ns() - stat_lock_acquired;
    STAT_ADD(stat_lock_hold_ns, held);
    uint_fast64_t peak = atomic_load_explicit(&stat_lock_hold_max_ns, memory_order_relaxed);
    while (held > peak && !atomic_compare_exchange_weak_explicit(&stat_lock_hold_max_ns, &peak, held,
                                                                 memory_order_relaxed, memory_order_relaxed)) {}
#endif
    pokopia_host_unlock();
}

static unsigned gfx_release_all(void);
static void gfx_restore(unsigned depth);
static void rec_lock(void);
static void rec_unlock(void);
static unsigned rec_release_all(void);
static void rec_restore(unsigned depth);
static void gfx_lock_late(void);
static void gfx_unlock(void);
static bool gfx_held_now(void);
static _Noreturn void finish_guest(int status)
{
    GuestBoundary *scope = pokopia_host_thread();
    if (!scope)
        abort();
    if (scope->kernel_locked)
        kernel_unlock();
    gfx_release_all();
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
#if defined(POKOPIA_GFX) && !defined(_WIN32)
    const char *unset = NULL;
    atomic_compare_exchange_strong_explicit(&overlay_stop, &unset, reason,
        memory_order_relaxed, memory_order_relaxed);
#endif
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

/* R56: guest mutexes, condition variables and address waits resolve their
 * sync word on every call. That lookup took the global lock, so ordinary thread
 * synchronization queued behind graphics calls holding it for up to ~28 ms.
 * Cache 4 KiB pages already proven writable, keyed by a layout generation.
 * Heap and module answers depend only on the heap size and the heap stack
 * aliases (module segments are fixed after load), so every alias or stack
 * source change bumps the generation and the heap size is checked directly.
 * Each entry packs the page number (guest addresses < 2^39) with the
 * generation into one atomic word.
 * R60: read_guest/write_guest share the cache. Only a thread holding the
 * global lock (which every layout change also holds) fills it, and NVN
 * builder/state calls dispatched without the lock may only touch cached pages. */
/* R78: 64Ki slots (was 1Ki: 4 MiB of an 8 GiB heap, so most bridge reads missed
 * and took the global lock and a memory query). */
enum { SYNC_PAGE_SHIFT = 12, SYNC_CACHE_SLOTS = 65536, SYNC_PAGE_BITS = 27, GUEST_FILL_PAGES = 32 };
static _Atomic uint64_t sync_page_cache[SYNC_CACHE_SLOTS];
static _Atomic uint64_t sync_layout_generation = 1;
static _Atomic uint64_t sync_cache_heap_size;
static _Thread_local bool guest_lockfree; /* set around lock-free NVN dispatch */
/* R68: set while a submit executes without the global lock (graphics lock held). */
static _Thread_local bool guest_kernel_released;

static void sync_layout_changed(void)
{
    atomic_fetch_add_explicit(&sync_layout_generation, 1, memory_order_release);
}

static uint64_t sync_cache_generation(void)
{
    const uint64_t heap_size = pokopia_boot_heap_size();
    if (atomic_load_explicit(&sync_cache_heap_size, memory_order_relaxed) != heap_size) {
        atomic_store_explicit(&sync_cache_heap_size, heap_size, memory_order_relaxed);
        sync_layout_changed();
    }
    return atomic_load_explicit(&sync_layout_generation, memory_order_acquire);
}

static uint64_t sync_page_tag(uint64_t generation, uint64_t page)
{
    return generation << SYNC_PAGE_BITS | (page & ((UINT64_C(1) << SYNC_PAGE_BITS) - 1));
}

static bool guest_page_cached(uint64_t generation, uint64_t page)
{
    return page >> SYNC_PAGE_BITS == 0 &&
           atomic_load_explicit(&sync_page_cache[page % SYNC_CACHE_SLOTS], memory_order_acquire) ==
           sync_page_tag(generation, page);
}

static bool guest_range_cached(uint64_t address, size_t size)
{
    if (!size || size > UINT64_MAX - address)
        return false;
    const uint64_t generation = sync_cache_generation();
    for (uint64_t page = address >> SYNC_PAGE_SHIFT; page <= (address + size - 1) >> SYNC_PAGE_SHIFT; ++page)
        if (!guest_page_cached(generation, page))
            return false;
    return true;
}

/* Cache only heap and module pages whose whole 4 KiB is one writable region.
 * The caller holds the global lock. */
static void guest_cache_fill(uint64_t generation, uint64_t address)
{
    const uint64_t page = address >> SYNC_PAGE_SHIFT;
    if (page >> SYNC_PAGE_BITS || guest_page_cached(generation, page))
        return;
    const MemoryInfo region = query_memory(address);
    const uint64_t start = page << SYNC_PAGE_SHIFT, end = start + (UINT64_C(1) << SYNC_PAGE_SHIFT);
    const uint64_t heap_size = pokopia_boot_heap_size();
    const bool in_heap = heap_size && address >= POKOPIA_BOOT_HEAP_BASE &&
                         address - POKOPIA_BOOT_HEAP_BASE < heap_size;
    bool in_module = false;
    for (size_t i = 0; i < sizeof modules / sizeof modules[0] && !in_module; ++i)
        for (size_t j = 0; j < 3; ++j)
            if (address >= modules[i].base + modules[i].segments[j].start &&
                address < modules[i].base + modules[i].segments[j].end)
                in_module = true;
    /* R78: guest thread stacks too (type 0x0b); stack maps change the layout
     * generation like every other map. */
    if ((in_heap || in_module || region.type == 0x0b) && (region.permission & 2) && region.base <= start &&
        region.base + region.size >= end &&
        atomic_load_explicit(&sync_layout_generation, memory_order_acquire) == generation)
        atomic_store_explicit(&sync_page_cache[page % SYNC_CACHE_SLOTS], sync_page_tag(generation, page),
                              memory_order_release);
}

static void guest_cache_fill_range(uint64_t address, size_t size)
{
    GuestBoundary *scope = pokopia_host_thread();
    if (!scope || !scope->kernel_locked || !size || size > UINT64_MAX - address)
        return;
    const uint64_t generation = sync_cache_generation();
    const uint64_t first = address >> SYNC_PAGE_SHIFT, last = (address + size - 1) >> SYNC_PAGE_SHIFT;
    for (uint64_t page = first; page <= last && page - first < GUEST_FILL_PAGES; ++page)
        guest_cache_fill(generation, page == first ? address : page << SYNC_PAGE_SHIFT);
}

static void kernel_lock(void);
static void kernel_unlock(void);

/* R79: the heap stays mapped read/write for the whole run; only stack aliases
 * carve holes in it (query_memory's heap case). Reads and writes inside it skip
 * the memory query, and the replay worker skips the global lock: its misses
 * (ranges over GUEST_FILL_PAGES never cache) waited behind the recording
 * threads, ~20% of the worker. The alias table is a fixed array. */
uint64_t guest_slow_reads, guest_slow_bytes;
/* R80: alias sources as compact snapshots (built under the global lock around
 * every MapMemory/UnmapMemory and published by index; four rotate so a reader
 * never sees one being rebuilt), so the check is a short inline loop. A range
 * about to be mapped is published before the map protects it. */
enum { ALIAS_SNAPSHOTS = 4 };
#if defined(__PROSPERO__) && defined(POKOPIA_GFX) && defined(POKOPIA_BOOT_PROBE)
#define POKOPIA_ALIAS_BIN_INDEX
enum { ALIAS_BIN_SHIFT = 20, ALIAS_BIN_WORDS = 128, POKOPIA_DEBUG_ALIAS_INDEX = 1u << 30 };
#define ALIAS_INDEX_BYTES ((uint64_t)ALIAS_BIN_WORDS * 64 << ALIAS_BIN_SHIFT)
#endif
typedef struct {
    unsigned count;
    uint64_t lo[HZN_HEAP_STACK_ALIAS_LIMIT + 1], hi[HZN_HEAP_STACK_ALIAS_LIMIT + 1];
#ifdef POKOPIA_ALIAS_BIN_INDEX
    uint64_t occupied[ALIAS_BIN_WORDS];
#endif
} HeapAliasSnapshot;
static HeapAliasSnapshot alias_snapshots[ALIAS_SNAPSHOTS];

#ifdef POKOPIA_ALIAS_BIN_INDEX
/* A bit covers a 1 MiB heap interval. A set bit sends the query through the
 * original precise intersection check, including aliases pending a map. */
static uint64_t alias_bin_mask(unsigned word, unsigned first, unsigned last)
{
    const uint64_t low = word == first / 64 ? UINT64_MAX << (first % 64) : UINT64_MAX;
    const uint64_t high = word == last / 64 ? UINT64_MAX >> (63 - last % 64) : UINT64_MAX;
    return low & high;
}

static void index_heap_aliases(HeapAliasSnapshot *snapshot)
{
    const uint64_t base = POKOPIA_BOOT_HEAP_BASE, limit = base + ALIAS_INDEX_BYTES;
    memset(snapshot->occupied, 0, sizeof snapshot->occupied);
    for (unsigned i = 0; i < snapshot->count; ++i) {
        const uint64_t lo = snapshot->lo[i], hi = snapshot->hi[i];
        if (hi <= lo) {
            /* Defer any malformed or wrapped interval to the exact scan. */
            memset(snapshot->occupied, 0xff, sizeof snapshot->occupied);
            return;
        }
        if (hi <= base || lo >= limit)
            continue;
        const unsigned first = lo <= base ? 0 : (unsigned)((lo - base) >> ALIAS_BIN_SHIFT);
        const unsigned last = (unsigned)(((hi < limit ? hi : limit) - 1 - base) >> ALIAS_BIN_SHIFT);
        for (unsigned w = first / 64; w <= last / 64; ++w)
            snapshot->occupied[w] |= alias_bin_mask(w, first, last);
    }
}

static bool heap_alias_bins_clear(const HeapAliasSnapshot *snapshot, uint64_t offset, size_t size)
{
    if (!size || offset >= ALIAS_INDEX_BYTES || size > ALIAS_INDEX_BYTES - offset)
        return false;
    const unsigned first = (unsigned)(offset >> ALIAS_BIN_SHIFT);
    const unsigned last = (unsigned)((offset + size - 1) >> ALIAS_BIN_SHIFT);
    if (first == last)
        return !(snapshot->occupied[first / 64] & (UINT64_C(1) << (first % 64)));
    for (unsigned w = first / 64; w <= last / 64; ++w)
        if (snapshot->occupied[w] & alias_bin_mask(w, first, last))
            return false;
    return true;
}
#endif
static _Atomic unsigned alias_published;
/* R94: 64 KiB heap blocks known to hold no alias, tagged with the alias
 * generation they were checked against. The scan over every alias range ran
 * on each guest access: ~12% of the render thread's host time. */
static _Atomic uint32_t alias_generation = 1;
static _Atomic uint64_t plain_blocks[4096];
static void heap_alias_snapshot(uint64_t pending, uint64_t pending_size)
{
#ifdef POKOPIA_BOOT_PROBE
    const unsigned next = (atomic_load_explicit(&alias_published, memory_order_relaxed) + 1) % ALIAS_SNAPSHOTS;
    unsigned count = 0;
    for (size_t i = 0; i < heap_aliases.count && i < HZN_HEAP_STACK_ALIAS_LIMIT; ++i) {
        const HznHeapStackAliasRange *alias = hzn_heap_stack_alias_at(&heap_aliases, i);
        if (!alias)
            continue;
        alias_snapshots[next].lo[count] = alias->source;
        alias_snapshots[next].hi[count] = alias->source + alias->size;
        ++count;
    }
    if (pending_size) {
        alias_snapshots[next].lo[count] = pending;
        alias_snapshots[next].hi[count] = pending + pending_size;
        ++count;
    }
    alias_snapshots[next].count = count;
#ifdef POKOPIA_ALIAS_BIN_INDEX
    index_heap_aliases(&alias_snapshots[next]);
#endif
    atomic_store_explicit(&alias_published, next, memory_order_release);
    atomic_fetch_add_explicit(&alias_generation, 1, memory_order_acq_rel);
#else
    (void)pending;
    (void)pending_size;
#endif
}
static bool guest_heap_plain(uint64_t address, size_t size)
{
#ifdef POKOPIA_BOOT_PROBE
    const uint64_t heap_size = pokopia_boot_heap_size();
    if (!size || !heap_size || address < POKOPIA_BOOT_HEAP_BASE || size > heap_size ||
        address - POKOPIA_BOOT_HEAP_BASE > heap_size - size)
        return false;
    const uint64_t block = address >> 16;
    const bool one_block = block == (address + size - 1) >> 16;
    const uint32_t generation = atomic_load_explicit(&alias_generation, memory_order_acquire);
    const uint64_t tag = (uint64_t)generation << 32 | (uint32_t)block;
    _Atomic uint64_t *cached = &plain_blocks[(block ^ (block >> 12)) & 4095];
    if (one_block && atomic_load_explicit(cached, memory_order_relaxed) == tag)
        return true;
    const unsigned at = atomic_load_explicit(&alias_published, memory_order_acquire);
    if (one_block) {
        const uint64_t lo = block << 16, hi = lo + 0x10000;
        const unsigned count = alias_snapshots[at].count;
        unsigned i = 0;
        while (i < count && !(lo < alias_snapshots[at].hi[i] && alias_snapshots[at].lo[i] < hi))
            ++i;
        if (i == count) {
            atomic_store_explicit(cached, tag, memory_order_relaxed);
            return true;
        }
    }
#ifdef POKOPIA_ALIAS_BIN_INDEX
    if ((pokopia_debug_flags & POKOPIA_DEBUG_ALIAS_INDEX) &&
        heap_alias_bins_clear(&alias_snapshots[at], address - POKOPIA_BOOT_HEAP_BASE, size))
        return true;
#endif
    const unsigned count = alias_snapshots[at].count;
    const uint64_t end = address + size;
    for (unsigned i = 0; i < count; ++i)
        if (address < alias_snapshots[at].hi[i] && alias_snapshots[at].lo[i] < end)
            return false;
    return true;
#else
    (void)address;
    (void)size;
    return false;
#endif
}

static const void *read_guest(void *opaque, uint64_t address, size_t size)
{
    (void)opaque;
    /* R90: the heap check is lock-free (an atomic alias snapshot), so lock-free
     * calls use it too: nvnBufferGetAddress on heap objects (~140k calls a
     * second outdoors) always fell back to the locked path. */
    if (guest_heap_plain(address, size))
        return (const void *)(uintptr_t)address;
    if (guest_lockfree)
        return guest_range_cached(address, size) ? (const void *)(uintptr_t)address : NULL;
    if (guest_kernel_released) {
        /* R68: the memory map belongs to the global lock; take it only on a cache miss. */
        if (guest_range_cached(address, size))
            return (const void *)(uintptr_t)address;
        ++guest_slow_reads;
        guest_slow_bytes += size;
        guest_kernel_released = false;
        kernel_lock();
        const void *result = read_guest(opaque, address, size);
        kernel_unlock();
        guest_kernel_released = true;
        return result;
    }
    MemoryInfo region = query_memory(address);
    if (!(region.permission & 1) || size > region.base + region.size - address)
        return NULL;
    guest_cache_fill_range(address, size);
    return (const void *)(uintptr_t)address;
}

/* R96: debug memory tools, polled once a second by the profiler thread.
 *   debug/memfind  "u16 TEXT" | "u8 TEXT" | "hex AABBCC..."  -> debug/memfind.txt
 *                  (every readable guest region; up to 400 hits with context)
 *   debug/memread  "ADDRESS LENGTH" (hex; length <= 16 MiB) -> debug/memread.bin
 * Requests end with a newline. Unbacked pages are skipped through the fault
 * guard in fatal_signal. */
static bool memscan_copy(void *destination, uint64_t address, size_t size)
{
    if (setjmp(memscan_jump))
        return false;
    memscan_guarded = 1;
    memcpy(destination, (const void *)(uintptr_t)address, size);
    memscan_guarded = 0;
    return true;
}

static size_t memscan_pattern(const char *line, uint8_t *pattern, size_t capacity)
{
    char kind[8] = {0};
    int skip = 0;
    if (sscanf(line, "%7s %n", kind, &skip) != 1)
        return 0;
    const char *text = line + skip;
    const size_t length = strcspn(text, "\r\n");
    size_t n = 0;
    if (!strcmp(kind, "u16")) {
        for (size_t i = 0; i < length && n + 2 <= capacity; ++i) {
            pattern[n++] = (uint8_t)text[i];
            pattern[n++] = 0;
        }
    } else if (!strcmp(kind, "u8")) {
        for (size_t i = 0; i < length && n < capacity; ++i)
            pattern[n++] = (uint8_t)text[i];
    } else if (!strcmp(kind, "hex")) {
        for (size_t i = 0; i + 1 < length && n < capacity; i += 2) {
            unsigned byte;
            if (sscanf(text + i, "%2x", &byte) != 1)
                break;
            pattern[n++] = (uint8_t)byte;
        }
    }
    return n;
}

static void memscan_find(const char *line)
{
    uint8_t pattern[128];
    const size_t length = memscan_pattern(line, pattern, sizeof pattern);
    FILE *out = fopen("/data/pokopia/debug/memfind.txt.tmp", "w");
    if (!out)
        return;
    fprintf(out, "memfind %s", line);
    enum { CHUNK = 65536, MAX_HITS = 400 };
    static uint8_t buffer[CHUNK + 128];
    unsigned hits = 0, regions = 0;
    uint64_t scanned = 0, faulted = 0;
    const uint64_t start_ns = stat_now_ns();
    for (uint64_t address = 0; length && hits < MAX_HITS;) {
        kernel_lock();
        const MemoryInfo region = query_memory(address);
        kernel_unlock();
        if (!region.size || region.base + region.size <= address)
            break;
        const uint64_t end = region.base + region.size;
        if ((region.permission & 1) && region.type != 0 && region.type != 0x10) {
            ++regions;
            for (uint64_t at = region.base; at < end && hits < MAX_HITS; at += CHUNK) {
                size_t size = end - at < CHUNK + length - 1 ? (size_t)(end - at) : CHUNK + length - 1;
                if (!memscan_copy(buffer, at, size)) {
                    /* the overlap may run into an unbacked chunk */
                    size = end - at < CHUNK ? (size_t)(end - at) : CHUNK;
                    if (!memscan_copy(buffer, at, size)) {
                        faulted += size;
                        continue;
                    }
                }
                scanned += size;
                for (size_t i = 0; i + length <= size && hits < MAX_HITS; ++i) {
                    if (buffer[i] != pattern[0] || memcmp(buffer + i, pattern, length))
                        continue;
                    const uint64_t hit = at + i;
                    uint8_t context[160];
                    const uint64_t from = hit >= 32 ? hit - 32 : 0;
                    const bool ok = memscan_copy(context, from, sizeof context);
                    fprintf(out, "hit %#" PRIx64 " region=%#" PRIx64 "+%#" PRIx64 " type=%#x perm=%u ctx@%#" PRIx64 " ",
                            hit, region.base, region.size, region.type, region.permission, from);
                    for (size_t k = 0; k < sizeof context; ++k)
                        fprintf(out, ok ? "%02x" : "??", context[k]);
                    fputc('\n', out);
                    ++hits;
                }
            }
        }
        address = end;
    }
    fprintf(out, "done hits=%u regions=%u scanned_mib=%.1f faulted_mib=%.1f ms=%.0f\n", hits, regions,
            scanned / 1048576.0, faulted / 1048576.0, (stat_now_ns() - start_ns) * 1e-6);
    fclose(out);
    rename("/data/pokopia/debug/memfind.txt.tmp", "/data/pokopia/debug/memfind.txt");
}

static void memscan_read(const char *line)
{
    unsigned long long address = 0, length = 0;
    if (sscanf(line, "%llx %llx", &address, &length) != 2 || !length || length > (16u << 20))
        return;
    FILE *out = fopen("/data/pokopia/debug/memread.bin.tmp", "wb");
    if (!out)
        return;
    static uint8_t chunk[4096];
    for (uint64_t at = address; at < address + length; at += sizeof chunk) {
        const size_t size = address + length - at < sizeof chunk ? (size_t)(address + length - at) : sizeof chunk;
        if (!memscan_copy(chunk, at, size))
            memset(chunk, 0xee, size);
        fwrite(chunk, 1, size, out);
    }
    fclose(out);
    rename("/data/pokopia/debug/memread.bin.tmp", "/data/pokopia/debug/memread.bin");
}

static void memscan_poll(void)
{
    static const char *const requests[2] = {"/data/pokopia/debug/memfind", "/data/pokopia/debug/memread"};
    for (int r = 0; r < 2; ++r) {
        FILE *file = fopen(requests[r], "r");
        if (!file)
            continue;
        char line[512] = {0};
        const bool got = fgets(line, sizeof line, file) != NULL;
        fclose(file);
        if (!got || !strchr(line, '\n'))
            continue; /* still being written */
        unlink(requests[r]);
        if (r == 0)
            memscan_find(line);
        else
            memscan_read(line);
    }
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
    if (guest_heap_plain(address, size)) /* R90: lock-free, as in read_guest */
        return (void *)(uintptr_t)address;
    if (guest_lockfree)
        return guest_range_cached(address, size) ? (void *)(uintptr_t)address : NULL;
    if (guest_kernel_released) {
        if (guest_range_cached(address, size))
            return (void *)(uintptr_t)address;
        ++guest_slow_reads;
        guest_slow_bytes += size;
        guest_kernel_released = false;
        kernel_lock();
        void *result = write_guest(opaque, address, size);
        kernel_unlock();
        guest_kernel_released = true;
        return result;
    }
    MemoryInfo region = query_memory(address);
    if (!(region.permission & 2) || size > region.base + region.size - address)
        return NULL;
    guest_cache_fill_range(address, size);
    return (void *)(uintptr_t)address;
}

static uint32_t *resolve_sync_word(void *opaque, uint64_t address)
{
    (void)opaque;
    if (address & 3)
        return NULL;
    const uint64_t generation = sync_cache_generation();
    if (guest_page_cached(generation, address >> SYNC_PAGE_SHIFT)) {
#ifndef _WIN32
        STAT_ADD(stat_sync_fast, 1);
#endif
        return (uint32_t *)(uintptr_t)address;
    }
#ifndef _WIN32
    STAT_ADD(stat_sync_slow, 1);
#endif
    kernel_lock();
    uint32_t *word = write_guest(NULL, address, sizeof *word); /* fills the cache under the lock */
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
        sync_layout_changed();
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
        sync_layout_changed();
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
    /* R77: replay worker item after which the fence has been submitted. */
    uint64_t signal_seq;
    /* R78: the fence is submitted right after the open draw batch. */
    bool deferred;
    /* R78: each signal takes the next fence of a ring, so re-signalling does
     * not wait for the GPU to finish the previous one (~18% of the worker). */
    VkFence ring[4];
    bool ring_submitted[4];
    unsigned ring_at;
    PFN_vkWaitForFences wait_fences;
    PFN_vkGetFenceStatus fence_status;
} NvnHostSync;

enum {
    POKOPIA_DEBUG_WAIT_SITES = 64, POKOPIA_DEBUG_FENCE_STATUS = 128, POKOPIA_DEBUG_FENCE_SPIN = 256,
    POKOPIA_DEBUG_NONBLOCKING_POLL = 512, POKOPIA_DEBUG_AFFINITY = 1u << 26,
};

/* R78: fence signals waiting for the open draw batch to be submitted. A fence
 * sync submitted the partly recorded batch each time (2,000 a 5 s window,
 * ~0.3 ms each, and the batches stayed small). */
static NvnHostSync *deferred_syncs[256];
static unsigned deferred_sync_count;
static PokopiaVkAdapter *deferred_sync_adapter;

static void submit_deferred_syncs(void)
{
    if (!deferred_sync_count)
        return;
    PokopiaVkAdapter *adapter = deferred_sync_adapter;
    VkDevice device = adapter->device;
    PFN_vkResetFences reset = (PFN_vkResetFences)adapter->get_device_proc(device, "vkResetFences");
    PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)adapter->get_device_proc(device, "vkQueueSubmit");
    for (unsigned i = 0; i < deferred_sync_count; ++i) {
        NvnHostSync *sync = deferred_syncs[i];
        sync->deferred = false;
        (void)reset;
        if (submit && submit(adapter->queue, 0, NULL, sync->fence) == VK_SUCCESS) {
            sync->pending = true;
            sync->ring_submitted[sync->ring_at] = true;
        }
    }
    deferred_sync_count = 0;
}

/* R77: replay worker state used before its definition (see replay_enqueue_item). */
static _Thread_local bool replay_worker_thread;
static bool replay_worker_running;
static uint64_t replay_enqueue_item(int kind, void *object, const uint64_t *args);

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

/* Age and throughput are explicit so the budget trace distinguishes
 * successful page outs from queued texture ranges. */
bool hzn_heap_sparse_cold_open(const char *path);
int32_t hzn_heap_sparse_cold_error(void);
size_t hzn_heap_sparse_cold_park(void *address, size_t bytes);
#ifndef _WIN32
size_t hzn_heap_sparse_cold_flush(void);
#endif

enum {
    COLD_PAGE = 64 * 1024,
    COLD_GRACE = 30,
    COLD_QUEUE = 4096,
    COLD_BYTES_PER_PRESENT = 8 * 1024 * 1024,
    /* PS4: the pager thread parks at most this much per host-lock hold, so
     * FSP reads and presents never wait behind more than ~1 MiB of disk I/O. */
    COLD_BYTES_PER_SLICE = 1024 * 1024,
    COLD_PAGER_IDLE_US = 16000,
    COLD_PAGER_YIELD_US = 2000,
};

typedef struct {
    uint64_t first, end, ready_present;
} ColdTextureRange;

static ColdTextureRange cold_textures[COLD_QUEUE];
static size_t cold_head, cold_count;
static uint64_t cold_present, cold_seen, cold_queued, cold_parked, cold_dropped;
static bool cold_enabled;

/* Park ready ranges up to `budget` bytes. Caller holds the host lock, which
 * serializes the queue with register/retire and parking with FSP I/O. */
static size_t park_ready_cold_textures(size_t budget)
{
    size_t remaining = budget;
    while (cold_count && remaining) {
        ColdTextureRange *range = &cold_textures[cold_head];
        if (range->ready_present > cold_present)
            break;
        const uint64_t available = range->end - range->first;
        const size_t count = available < remaining ? (size_t)available : remaining;
        cold_parked += hzn_heap_sparse_cold_park((void *)(uintptr_t)range->first, count);
        if (hzn_heap_sparse_cold_error())
            return budget - remaining;
        remaining -= count;
        range->first += count;
        if (range->first == range->end) {
            cold_head = (cold_head + 1) % COLD_QUEUE;
            --cold_count;
        }
    }
    return budget - remaining;
}

#ifndef _WIN32
static bool cold_pager_started;

/* Disk writes used to run inside present_nvn_frame under the host lock, up to
 * 8 MiB a frame, stalling every guest thread's HLE calls (FSP reads included)
 * for 100+ ms. This thread does the same work in 1 MiB slices. A rollback
 * error is left for retire_cold_textures to stop() on a guest thread. */
static void *cold_pager_main(void *unused)
{
    (void)unused;
    for (;;) {
        pokopia_host_lock();
        const uint64_t locked_at = stat_now_ns();
        const size_t parked = cold_enabled && !hzn_heap_sparse_cold_error()
            ? park_ready_cold_textures(COLD_BYTES_PER_SLICE) : 0;
        STAT_ADD(stat_pager_hold_ns, stat_now_ns() - locked_at);
        pokopia_host_unlock();
        /* R57: the disk write happens here, with no lock held. */
        if (parked)
            hzn_heap_sparse_cold_flush();
        const long pause_us = parked ? COLD_PAGER_YIELD_US : COLD_PAGER_IDLE_US;
        const struct timespec pause = { .tv_sec = 0, .tv_nsec = pause_us * 1000L };
        nanosleep(&pause, NULL);
    }
    return NULL;
}
#endif

static void cold_initialize(void)
{
    static bool attempted;
    if (attempted)
        return;
    attempted = true;
    cold_enabled = hzn_heap_sparse_cold_open("/data/pokopia-ps4-texture-pages.tmp");
#ifndef _WIN32
    if (cold_enabled) {
        pthread_t pager;
        pthread_attr_t attr;
        cold_pager_started = !pthread_attr_init(&attr) &&
            !pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) &&
            !pthread_create(&pager, &attr, cold_pager_main, NULL);
        pthread_attr_destroy(&attr);
    }
    fprintf(probe_log, "Cold texture backing open=%u pager=%u\n",
            (unsigned)cold_enabled, (unsigned)cold_pager_started);
#else
    fprintf(probe_log, "Cold texture backing open=%u\n", (unsigned)cold_enabled);
#endif
    fflush(probe_log);
}

static void register_cold_texture(const NvnFrontTextureCreateInfo *texture)
{
    cold_initialize();
    const uint64_t address = texture->storage_address, size = texture->storage_size;
    const uint64_t mask = COLD_PAGE - 1;
    if (!cold_enabled || address > UINT64_MAX - mask || size > UINT64_MAX - address)
        return;
    const uint64_t first = (address + mask) & ~mask, end = (address + size) & ~mask;
    if (first >= end)
        return;
    ++cold_seen;
    if (cold_count == COLD_QUEUE) {
        cold_dropped += end - first;
        return;
    }
    ColdTextureRange *range = &cold_textures[(cold_head + cold_count) % COLD_QUEUE];
    *range = (ColdTextureRange){first, end, cold_present + COLD_GRACE};
    cold_queued += end - first;
    ++cold_count;
}

#ifndef _WIN32
static void report_load_stats(void)
{
    static uint64_t window_start, fsp_bytes_seen, presents;
    const uint64_t now = stat_now_ns();
    ++presents;
#if defined(__PROSPERO__)
    atomic_fetch_add_explicit(&timeline_presents, 1, memory_order_relaxed);
#endif
    if (!window_start)
        window_start = now;
    if (now - window_start < UINT64_C(5000000000))
        return;
    static uint64_t report_ns; /* R94: how long the previous report took on this thread */
    const double ms = 1e-6;
    const uint64_t window = now - window_start, fsp_bytes = hzn_fsp_romfs_bytes - fsp_bytes_seen;
    window_start = now;
    fsp_bytes_seen = hzn_fsp_romfs_bytes;
    fprintf(probe_log, "Load stats t=%.1fs window_ms=%.0f presents=%" PRIu64 " lock calls=%" PRIu64 " contended=%" PRIu64
        " wait_ms=%.0f hold_ms=%.0f hold_max_ms=%.1f pager_hold_ms=%.0f fsp calls=%" PRIu64 " ms=%.0f MiB=%.2f"
        " nvn calls=%" PRIu64 " ms=%.0f svc",
        pokopia_boot_monotonic_ns() * 1e-9, window * ms, presents, (uint64_t)STAT_TAKE(stat_lock_calls), (uint64_t)STAT_TAKE(stat_lock_contended),
        STAT_TAKE(stat_lock_wait_ns) * ms, STAT_TAKE(stat_lock_hold_ns) * ms, STAT_TAKE(stat_lock_hold_max_ns) * ms,
        STAT_TAKE(stat_pager_hold_ns) * ms, (uint64_t)STAT_TAKE(stat_fsp_calls), STAT_TAKE(stat_fsp_ns) * ms,
        fsp_bytes / 1048576.0, (uint64_t)STAT_TAKE(stat_nvn_calls), STAT_TAKE(stat_nvn_ns) * ms);
    presents = 0;
    /* Each SVC with its call count and total time inside it (summed over threads). */
    for (unsigned number = 0; number < STAT_SVCS; ++number) {
        const uint64_t calls = STAT_TAKE(stat_svc_calls[number]), ns = STAT_TAKE(stat_svc_ns[number]);
        if (calls)
            fprintf(probe_log, " %#x=%" PRIu64 "/%.0fms", number, calls, ns * ms);
    }
    fputc('\n', probe_log);
    report_stat_names(probe_log, window);
    report_thread_stats(probe_log);
    nvn_replay_report_skips(probe_log);
    fprintf(probe_log, "Stats report previous ms=%.2f tid=%" PRIu64 "\n", report_ns * ms, pokopia_trace_guest_tid());
    fflush(probe_log);
    report_ns = stat_now_ns() - now;
}
#endif

static void retire_cold_textures(void)
{
#ifndef _WIN32
    report_load_stats();
#endif
    ++cold_present;
    if (!cold_enabled)
        return;
#ifndef _WIN32
    /* The pager thread parks; fall back to inline parking only if it never started. */
    if (!cold_pager_started && park_ready_cold_textures(COLD_BYTES_PER_PRESENT))
        hzn_heap_sparse_cold_flush();
#else
    park_ready_cold_textures(COLD_BYTES_PER_PRESENT);
#endif
    const int32_t error = hzn_heap_sparse_cold_error();
    if (error) {
        fprintf(probe_log, "Cold texture rollback host error=%#" PRIx32 "\n", (uint32_t)error);
        stop("could not restore texture backing", cold_count ? cold_textures[cold_head].first : 0);
    }
    if (cold_present % 128 == 0) {
        fprintf(probe_log, "Cold textures present=%" PRIu64 " seen=%" PRIu64 " waiting=%zu"
            " scheduled=%" PRIu64 " parked=%" PRIu64 " dropped=%" PRIu64 "\n",
            cold_present, cold_seen, cold_count, cold_queued, cold_parked, cold_dropped);
        fflush(probe_log);
    }
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
#ifndef _WIN32
#endif
    if (!is_texel && texture->deferred)
        register_cold_texture(info);
#if defined(__PROSPERO__)
    /* Keep startup diagnostics without flushing the log under the graphics
     * lock for every successful texture allocation. Errors still log above. */
    static unsigned texture_creates_logged;
    if (texture_creates_logged >= 16)
        return NVN_FRONT_TEXTURE_CREATED;
    ++texture_creates_logged;
#endif
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
    PFN_vkWaitForFences wait_fences = (PFN_vkWaitForFences)
        adapter->get_device_proc(adapter->device, "vkWaitForFences");
    if (!create || !adapter->get_device_proc(adapter->device, "vkDestroyFence") || !wait_fences)
        return NULL;
    NvnHostSync *sync = calloc(1, sizeof *sync);
    if (!sync)
        return NULL;
    sync->wait_fences = wait_fences;
    sync->fence_status = (PFN_vkGetFenceStatus)adapter->get_device_proc(adapter->device, "vkGetFenceStatus");
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

static void replay_wait_for(uint64_t target);
static uint64_t replay_completed_now(void);

typedef struct {
    uint64_t replay_ns, flush_ns, fence_ns, pending_polls;
} NvnWaitBreakdown;

static VkResult wait_host_fence(PFN_vkWaitForFences wait, PFN_vkGetFenceStatus get_status,
                               VkDevice device, VkFence fence, uint64_t timeout, bool can_spin)
{
    const unsigned flags = pokopia_debug_flags;
    if (!timeout && (flags & POKOPIA_DEBUG_FENCE_STATUS) && get_status) {
        const VkResult status = get_status(device, fence);
        return status == VK_NOT_READY ? VK_TIMEOUT : status;
    }
#if defined(__PROSPERO__)
    if (can_spin && timeout && (flags & POKOPIA_DEBUG_FENCE_SPIN) && get_status) {
        const uint64_t start = stat_now_ns();
        const uint64_t limit = timeout < UINT64_C(150000) ? timeout : UINT64_C(150000);
        uint64_t elapsed;
        do {
            const VkResult status = get_status(device, fence);
            if (status != VK_NOT_READY)
                return status;
            const uint64_t pause_until = stat_now_ns() + UINT64_C(10000);
            while (stat_now_ns() < pause_until)
                __builtin_ia32_pause();
            elapsed = stat_now_ns() - start;
        } while (elapsed < limit);
        if (timeout != UINT64_MAX)
            timeout = elapsed >= timeout ? 0 : timeout - elapsed;
    }
#else
    (void)can_spin;
#endif
    return wait(device, 1, &fence, VK_TRUE, timeout);
}

static VkResult wait_nvn_sync_traced(PokopiaVkAdapter *adapter, NvnHostSync *sync, uint64_t timeout,
                                     NvnWaitBreakdown *trace)
{
    if (sync->signal_seq) {
        /* The guest asks for readiness with timeout zero. Since the poll now
         * runs under the recording lock, it need not stall behind replay. */
        if (!timeout && (pokopia_debug_flags & POKOPIA_DEBUG_NONBLOCKING_POLL) &&
            replay_completed_now() < sync->signal_seq) {
            if (trace)
                ++trace->pending_polls;
            return VK_TIMEOUT;
        }
        const uint64_t start = trace ? stat_now_ns() : 0;
        replay_wait_for(sync->signal_seq);
        if (trace)
            trace->replay_ns += stat_now_ns() - start;
    }
    if (sync->deferred) { /* R78: its batch has not been submitted yet */
        const uint64_t start = trace ? stat_now_ns() : 0;
        gfx_lock_late();
        if (sync->deferred)
            pokopia_vk_flush_pending(POKOPIA_FLUSH_SYNC);
        if (sync->deferred)
            submit_deferred_syncs(); /* the batch was already gone */
        gfx_unlock();
        if (trace)
            trace->flush_ns += stat_now_ns() - start;
    }
    if (!sync->pending)
        return VK_SUCCESS;
    const PFN_vkWaitForFences wait = sync->wait_fences;
    const PFN_vkGetFenceStatus get_status = sync->fence_status;
    if (replay_worker_running && !replay_worker_thread && timeout && !gfx_held_now()) {
        /* R79: wait for the GPU with every lock released: the worker could not
         * replay (graphics lock) while a game thread sat here. The fence is the
         * sync's current ring entry; the worker re-signals into the next one
         * and only reuses this one after waiting for it, so it stays valid. */
        const VkFence fence = sync->fence;
        GuestBoundary *scope = pokopia_host_thread();
        const bool kernel = scope && scope->kernel_locked;
        if (kernel)
            kernel_unlock();
        const unsigned rec_held = rec_release_all();
        const unsigned gfx_held = gfx_release_all();
        const uint64_t start = trace ? stat_now_ns() : 0;
        const VkResult result = wait_host_fence(wait, get_status, adapter->device, fence, timeout, true);
        if (trace)
            trace->fence_ns += stat_now_ns() - start;
        gfx_restore(gfx_held);
        rec_restore(rec_held);
        if (kernel)
            kernel_lock();
        return result; /* pending stays set; the next wait finds the fence signaled */
    }
    const uint64_t start = trace ? stat_now_ns() : 0;
    const VkResult result = wait_host_fence(wait, get_status, adapter->device, sync->fence, timeout, false);
    if (trace)
        trace->fence_ns += stat_now_ns() - start;
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
        sync->pending = false;
    return result;
}

static VkResult wait_nvn_sync(PokopiaVkAdapter *adapter, NvnHostSync *sync, uint64_t timeout)
{
    return wait_nvn_sync_traced(adapter, sync, timeout, NULL);
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
    PFN_vkWaitForFences fences_wait = (PFN_vkWaitForFences)adapter->get_device_proc(adapter->device, "vkWaitForFences");
    if (sync->ring[0]) {
        for (unsigned i = 0; i < 4; ++i)
            if (sync->ring[i]) {
                if (sync->ring_submitted[i])
                    fences_wait(adapter->device, 1, &sync->ring[i], VK_TRUE, UINT64_MAX);
                destroy(adapter->device, sync->ring[i], NULL);
            }
    } else {
        destroy(adapter->device, sync->fence, NULL);
    }
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

/* nvnCommandBufferCopyTextureToTexture. A block-compressed source may still
 * be deferred: upload it from guest memory first, as sampling does. */
static bool execute_nvn_copy_texture(void *opaque, const NvnReplayTextureCopy *copy)
{
    static unsigned logged;
    PokopiaVkTexture *source = copy->source.texture.native, *destination = copy->destination.texture.native;
    VkResult result = VK_SUCCESS;
    if (source->deferred || source->layout == VK_IMAGE_LAYOUT_UNDEFINED)
        result = pokopia_live_texture_prepare_sample(&texture_upload, &copy->source.texture);
    const NvnReplayTextureCopySide *from = &copy->source, *to = &copy->destination;
    const PokopiaVkCopyRegion src = {
        .level = from->level, .layer = from->layer, .layers = from->layers, .volume = from->volume,
        .offset = {from->x, from->y, from->z},
        .extent = {(uint32_t)from->width, (uint32_t)from->height, (uint32_t)from->depth},
    };
    const PokopiaVkCopyRegion dst = {
        .level = to->level, .layer = to->layer, .layers = to->layers, .volume = to->volume,
        .offset = {to->x, to->y, to->z},
        .extent = {(uint32_t)to->width, (uint32_t)to->height, (uint32_t)to->depth},
    };
    if (result == VK_SUCCESS)
        result = pokopia_vk_copy_texture(opaque, source, &src, destination, &dst, copy->linear);
    if (result != VK_SUCCESS || logged < 8) {
        ++logged;
        fprintf(probe_log, "NVN texture copy %ux%ux%u fmt=%d L%u/%u+%u -> %ux%ux%u fmt=%d L%u/%u+%u linear=%d result=%d\n",
                src.extent.width, src.extent.height, src.extent.depth, (int)source->format, src.level, src.layer,
                src.layers, dst.extent.width, dst.extent.height, dst.extent.depth, (int)destination->format, dst.level,
                dst.layer, dst.layers, copy->linear, (int)result);
        fflush(probe_log);
    }
    return result == VK_SUCCESS;
}

/* R96: nvnCommandBufferCopyTextureToBuffer. Saving a Pokemon reads a
 * 1920x1080 frame back into a buffer (its save thumbnail). */
static bool execute_nvn_copy_to_buffer(void *opaque, uint64_t device, const NvnReplayTextureCopySide *side,
                                       uint64_t buffer, uint64_t row_stride, uint64_t image_stride)
{
    static unsigned logged;
    PokopiaVkTexture *texture = side->texture.native;
    const uint32_t texel = pokopia_vk_texture_texel_bytes(texture);
    VkResult result = VK_ERROR_FORMAT_NOT_SUPPORTED;
    uint64_t bytes = 0;
    if (texel && !side->volume && side->width > 0 && side->height > 0) {
        const uint64_t packed_row = (uint64_t)side->width * texel;
        const uint64_t row = row_stride ? row_stride : packed_row;
        const uint64_t layer_bytes = row * ((uint64_t)side->height - 1) + packed_row;
        const uint64_t image = image_stride ? image_stride : row * (uint64_t)side->height;
        bytes = image * ((uint64_t)side->layers - 1) + layer_bytes;
        NvnFrontPoolHit hit;
        uint8_t *destination = row >= packed_row && bytes <= (UINT64_C(1) << 30) &&
                               nvn_front_resolve_pool(nvn, device, buffer, (size_t)bytes, &hit) ?
                               write_guest(NULL, buffer, (size_t)bytes) : NULL;
        if (destination) {
            if (texture->deferred || texture->layout == VK_IMAGE_LAYOUT_UNDEFINED)
                pokopia_live_texture_prepare_sample(&texture_upload, &side->texture);
            result = pokopia_vk_texture_read_region(opaque, texture, side->level, side->layer, side->layers,
                                                    (VkOffset2D){side->x, side->y},
                                                    (VkExtent2D){(uint32_t)side->width, (uint32_t)side->height},
                                                    destination, (size_t)row, (size_t)image);
        }
    }
    if (result != VK_SUCCESS || logged < 8) {
        ++logged;
        fprintf(probe_log, "NVN texture to buffer copy %dx%d+%d,%d fmt=%d L%u/%u+%u -> %#" PRIx64
                           " row=%" PRIu64 " image=%" PRIu64 " bytes=%" PRIu64 " result=%d\n",
                side->width, side->height, side->x, side->y, texture ? (int)texture->format : -1, side->level,
                side->layer, side->layers, buffer, row_stride, image_stride, bytes, (int)result);
        fflush(probe_log);
    }
    return result == VK_SUCCESS;
}

static bool execute_nvn_clear(void *opaque, void *object, VkRect2D area, const float rgba[4], uint32_t mask)
{
    PokopiaVkTexture *texture = object;
    uint32_t channels = texture->format == VK_FORMAT_R16G16_SFLOAT ? 3u :
                        texture->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ? 7u : 15u;
    if ((mask & channels) != channels)
        return false;
    extern VkResult pokopia_live_generic_clear_color(PokopiaVkAdapter *, PokopiaVkTexture *, const float[4]);
    const bool whole = area.offset.x == 0 && area.offset.y == 0 && area.extent.width == texture->extent.width &&
                       area.extent.height == texture->extent.height;
    VkResult result = whole ? pokopia_live_generic_clear_color(opaque, texture, rgba) : VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (result != VK_SUCCESS)
        result = pokopia_vk_clear_color_region(opaque, texture, area, rgba);
    if (result != VK_SUCCESS || texture->render_serial <= 8) {
        fprintf(probe_log, "NVN Vulkan color clear serial=%" PRIu64 " result=%d\n", texture->render_serial, result);
        fflush(probe_log);
    }
    return result == VK_SUCCESS;
}

static bool execute_nvn_clear_depth_stencil(void *opaque, void *texture, VkRect2D area,
                                            VkImageAspectFlags aspects, VkClearDepthStencilValue value)
{
    extern VkResult pokopia_live_generic_clear_depth(PokopiaVkAdapter *, PokopiaVkTexture *, VkImageAspectFlags,
                                                     VkClearDepthStencilValue);
    const PokopiaVkTexture *target = texture;
    const bool whole = area.offset.x == 0 && area.offset.y == 0 && area.extent.width == target->extent.width &&
                       area.extent.height == target->extent.height && target->array_layers == 1;
    VkResult result = whole ? pokopia_live_generic_clear_depth(opaque, texture, aspects, value) :
                              VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (result != VK_SUCCESS)
        result = pokopia_vk_clear_depth_stencil(opaque, texture, area, aspects, value);
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
    const bool check_loading_draw = loading_draws_logged < 4;
    const bool loading_draw = check_loading_draw && draw->vertex_shader.hash && draw->fragment_shader.hash &&
        !strcmp(draw->vertex_shader.hash, "a26763a4bdd7b361") &&
        !strcmp(draw->fragment_shader.hash, "2db3327d9fbc5bb3");
#ifndef _WIN32
    /* R60: mark frames that draw the Ditto loading screen for the frame cap. */
    if (draw->fragment_shader.hash && !strcmp(draw->fragment_shader.hash, "2db3327d9fbc5bb3"))
        atomic_store_explicit(&frame_has_loading_draw, true, memory_order_relaxed);
#endif
    const bool log_loading_draw = loading_draws_logged < 4 && loading_draw;
    if (log_loading_draw)
        ++loading_draws_logged;
    static unsigned failed_draws_logged; /* R68: these repeat every frame */
    /* R73: a declined draw has serial 0, so "serial <= 16" logged (and flushed)
     * every one of them, ~10k lines a minute in the intro. Declines fall back
     * to the general path, so only the first 64 are worth keeping. */
    if ((result != VK_SUCCESS && failed_draws_logged++ < 64) ||
        (result == VK_SUCCESS && serial <= 16) || log_loading_draw) {
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
    /* R51: VK_ERROR_FORMAT_NOT_SUPPORTED means this draw's formats or shape
     * are outside what live_draw translates; the draw changed nothing. Skip
     * it like an unsupported shader rather than stopping the game. */
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
        nvn_replay_draw_unsupported = true; /* R62: replay retries it on the general path */
        /* Log the first three skips of each fragment shader (R52), so one
         * frequent draw cannot hide the details of the others. */
        static struct { char hash[20]; unsigned count; } seen[48];
        static unsigned seen_count, format_skips;
        const char *key = draw->fragment_shader.hash ? draw->fragment_shader.hash : "-";
        unsigned *count = NULL;
        for (unsigned i = 0; i < seen_count && !count; ++i)
            if (!strncmp(seen[i].hash, key, sizeof seen[i].hash - 1))
                count = &seen[i].count;
        if (!count && seen_count < sizeof seen / sizeof seen[0]) {
            snprintf(seen[seen_count].hash, sizeof seen[seen_count].hash, "%s", key);
            count = &seen[seen_count++].count;
        }
        ++format_skips;
        if (count && (*count)++ < 3) {
            fprintf(probe_log, "NVN draw skipped (unsupported format/shape) #%u vs=%s fs=%s target_format=%d"
                    " depth_format=%d layer=%u extent=%ux%u stride=%u samples=%d/%d sample_views=%d/%d"
                    " depth_test=%d stencil=%d blend=%d\n", format_skips,
                    draw->vertex_shader.hash ? draw->vertex_shader.hash : "-",
                    draw->fragment_shader.hash ? draw->fragment_shader.hash : "-",
                    draw->target ? (int)draw->target->format : -1,
                    draw->depth_target ? (int)draw->depth_target->format : -1, draw->target_layer,
                    draw->extent.width, draw->extent.height, draw->vertex_stride,
                    draw->fragment_samples[0].texture ? (int)draw->fragment_samples[0].texture->format : -1,
                    draw->fragment_samples[1].texture ? (int)draw->fragment_samples[1].texture->format : -1,
                    (int)draw->fragment_samples[0].view_format, (int)draw->fragment_samples[1].view_format,
                    draw->depth_test, draw->stencil_test, draw->blend_enable);
            /* R53: which live_draw.c check rejected it, and the sampler state
             * that supported_sample() compares exactly. */
            extern int pokopia_live_draw_reject_line;
            fprintf(probe_log, "  rejected at live_draw.c:%d write_mask=%#x depth_write=%d", pokopia_live_draw_reject_line,
                    (unsigned)draw->color_write_mask, draw->depth_write);
            for (unsigned i = 0; i < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++i) {
                const PokopiaLiveDrawSample *sample = &draw->fragment_samples[i];
                if (!sample->texture)
                    continue;
                const NvnFrontSamplerInfo *s = &sample->sampler;
                fprintf(probe_log, " s%u[min=%u mag=%u wrap=%u/%u/%u cmp=%u red=%u snap=%u lod=%g..%g bias=%g aniso=%g"
                        " layers=%u layout=%d]", i, (unsigned)s->min_filter, (unsigned)s->mag_filter,
                        (unsigned)s->wrap[0], (unsigned)s->wrap[1], (unsigned)s->wrap[2], (unsigned)s->compare_mode,
                        (unsigned)s->reduction, (unsigned)s->lod_snap, (double)s->lod_min, (double)s->lod_max,
                        (double)s->lod_bias, (double)s->max_anisotropy, (unsigned)sample->texture->array_layers,
                        (int)sample->texture->layout);
            }
            fputc('\n', probe_log);
            fflush(probe_log);
        }
        return true;
    }
    return result == VK_SUCCESS;
}

/* R61: general draw path. Prepares every sampled texture (uploading guest data
 * on first use, as the pinned path does), then draws. A draw the GPU path
 * cannot run is logged and skipped; only a lost device stops the guest. */
uint64_t generic_prepare_ns, generic_exec_ns; /* R67 */
/* R73: general compute; failures are logged (capped) with the executor's reason. */
static bool execute_nvn_compute_generic(void *opaque, const PokopiaGenericCompute *compute)
{
    VkResult result = VK_SUCCESS;
    for (uint32_t i = 0; i < compute->texture_count && result == VK_SUCCESS; ++i)
        if (compute->textures[i].guest_info)
            result = pokopia_live_texture_prepare_sample(&texture_upload, compute->textures[i].guest_info);
    if (result == VK_SUCCESS)
        result = pokopia_live_compute_generic(opaque, compute);
    /* Log the first dispatch of each shader, and failures (capped). */
    static char seen[64][20];
    static unsigned seen_count, failures;
    bool first = seen_count < 64;
    for (unsigned i = 0; i < seen_count && first; ++i)
        first = strcmp(seen[i], compute->shader.hash ? compute->shader.hash : "-") != 0;
    if (first)
        snprintf(seen[seen_count++], sizeof seen[0], "%s", compute->shader.hash ? compute->shader.hash : "-");
    if (first || (result != VK_SUCCESS && failures++ < 64)) {
        extern char pokopia_generic_reject[160];
        fprintf(probe_log, "NVN general compute %s %ux%ux%u ubo=%u ssbo=%u tex=%u img=%u result=%d %s\n",
                compute->shader.hash ? compute->shader.hash : "-", compute->groups[0], compute->groups[1],
                compute->groups[2], compute->uniform_count, compute->storage_count, compute->texture_count,
                compute->image_count, (int)result, result == VK_SUCCESS ? "" : pokopia_generic_reject);
        fflush(probe_log);
    }
    return result == VK_SUCCESS;
}

static bool execute_nvn_draw_generic(void *opaque, const PokopiaGenericDraw *draw)
{
    VkResult result = VK_SUCCESS;
    const unsigned weight = (pokopia_debug_flags & POKOPIA_DEBUG_SAMPLE_DRAWS) ? 16u : 1u;
    const bool timed = weight == 1 || pokopia_timing_sample(generic_draw_ok + generic_draw_failed);
    const uint64_t prepare_start = timed ? stat_now_ns() : 0;
    for (uint32_t i = 0; i < draw->texture_count && result == VK_SUCCESS; ++i)
        if (draw->textures[i].guest_info)
            result = pokopia_live_texture_prepare_sample(&texture_upload, draw->textures[i].guest_info);
    const uint64_t prepare_end = timed ? stat_now_ns() : 0;
    generic_prepare_ns += (prepare_end - prepare_start) * weight;
    uint64_t serial = 0;
    if (result == VK_SUCCESS)
        result = pokopia_live_draw_generic(opaque, draw, &serial);
    if (timed)
        generic_exec_ns += (stat_now_ns() - prepare_end) * weight;
    if (result == VK_SUCCESS)
        ++generic_draw_ok;
    else
        ++generic_draw_failed;
    static struct { char key[36]; unsigned count; } seen[96];
    static unsigned seen_count;
    /* R78: once the table is full, successful draws skip the key search (a
     * snprintf and up to 96 strcmp on every draw). */
    static unsigned searched;
    if (result == VK_SUCCESS && (seen_count == sizeof seen / sizeof seen[0] || ++searched > 20000))
        return true;
    char key[36];
    snprintf(key, sizeof key, "%.16s/%.16s", draw->vertex_shader.hash ? draw->vertex_shader.hash : "-",
             draw->fragment_shader.hash ? draw->fragment_shader.hash : "-");
    unsigned *count = NULL;
    for (unsigned i = 0; i < seen_count && !count; ++i)
        if (!strcmp(seen[i].key, key))
            count = &seen[i].count;
    if (!count && seen_count < sizeof seen / sizeof seen[0]) {
        memcpy(seen[seen_count].key, key, sizeof key);
        count = &seen[seen_count++].count;
    }
    if (count && (*count)++ < 2) {
        extern char pokopia_generic_reject[160];
        fprintf(probe_log, "NVN general draw %s %ux%u uniforms=%u textures=%u streams=%u attributes=%u"
                " indices=%u vertices=%u result=%d", key, draw->extent.width, draw->extent.height,
                draw->uniform_count, draw->texture_count, draw->binding_count, draw->attribute_count,
                draw->index_count, draw->vertex_count, result);
        if (result != VK_SUCCESS)
            fprintf(probe_log, " why=[%s]", pokopia_generic_reject);
        fputc('\n', probe_log);
        fflush(probe_log);
    }
    return result != VK_ERROR_DEVICE_LOST;
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

static void replay_drain(void);
static bool finish_nvn_queue(void *opaque)
{
    replay_drain();
    pokopia_vk_flush_pending(POKOPIA_FLUSH_FINISH);
    PokopiaVkAdapter *adapter = opaque;
    PFN_vkQueueWaitIdle wait = (PFN_vkQueueWaitIdle)adapter->get_device_proc(adapter->device, "vkQueueWaitIdle");
    return wait && wait(adapter->queue) == VK_SUCCESS;
}

/* Moves the sync to a fresh, reset fence of its ring. */
static bool sync_next_fence(PokopiaVkAdapter *adapter, NvnHostSync *sync)
{
    VkDevice device = adapter->device;
    PFN_vkCreateFence create = (PFN_vkCreateFence)adapter->get_device_proc(device, "vkCreateFence");
    PFN_vkResetFences reset = (PFN_vkResetFences)adapter->get_device_proc(device, "vkResetFences");
    PFN_vkWaitForFences wait = (PFN_vkWaitForFences)adapter->get_device_proc(device, "vkWaitForFences");
    if (!create || !reset || !wait)
        return false;
    if (!sync->ring[0]) {
        sync->ring[0] = sync->fence;
        sync->ring_submitted[0] = sync->pending;
        sync->ring_at = 0;
    }
    const unsigned next = (sync->ring_at + 1) % 4;
    if (!sync->ring[next]) {
        const VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (create(device, &info, NULL, &sync->ring[next]) != VK_SUCCESS) {
            sync->ring[next] = VK_NULL_HANDLE;
            return false;
        }
    } else if (sync->ring_submitted[next]) {
        if (wait(device, 1, &sync->ring[next], VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            return false;
    }
    sync->ring_submitted[next] = false;
    if (reset(device, 1, &sync->ring[next]) != VK_SUCCESS)
        return false;
    sync->ring_at = next;
    sync->fence = sync->ring[next];
    sync->pending = false;
    return true;
}

static bool execute_nvn_sync(void *opaque, void *object, bool wait)
{
    PokopiaVkAdapter *adapter = opaque;
    NvnHostSync *sync = object;
    /* R78: a command-buffer WaitSync waits for a fence signaled earlier on the
     * same ordered VkQueue (every guest queue submits to it), so it orders
     * nothing; it flushed the batch and waited for the GPU (~0.3 ms each). */
    if (wait)
        return true;
    if (sync->deferred)
        return true; /* already signaled after the open batch */
    /* R90: a batch holding real work is submitted at the fence, so the GPU runs
     * it while the game goes on; deferring it to the game's wait (or to the
     * present) made every sync wait include the whole batch (~1 s of
     * nvnSyncWait per 5 s outdoors once nothing else submitted mid-frame). */
    if (!(pokopia_debug_flags & 2) && pokopia_live_generic_batch_count() >= ((pokopia_debug_flags & 4) ? 1u : 32u))
        pokopia_vk_flush_pending(POKOPIA_FLUSH_SYNC);
    if (!wait && pokopia_live_generic_batch_open() && deferred_sync_count < 256) {
        if (!sync_next_fence(adapter, sync))
            return false;
        deferred_sync_adapter = adapter;
        pokopia_vk_after_submit_hook = submit_deferred_syncs;
        sync->deferred = true;
        deferred_syncs[deferred_sync_count++] = sync;
        return true;
    }
    pokopia_vk_flush_pending(POKOPIA_FLUSH_SYNC);
    if (wait)
        return wait_nvn_sync(adapter, sync, UINT64_MAX) == VK_SUCCESS;
    VkDevice device = adapter->device;
    PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)adapter->get_device_proc(device, "vkQueueSubmit");
    if (!submit || !sync_next_fence(adapter, sync) || submit(adapter->queue, 0, NULL, sync->fence) != VK_SUCCESS)
        return false;
    sync->pending = true;
    sync->ring_submitted[sync->ring_at] = true;
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
    const uint64_t acquire_start = stat_now_ns();
    uint64_t worker_ns = 0, fence_ns = 0, drain_ns = 0;
    bool was_deferred = false, was_pending = false;
    if (sync_address) {
        NvnHostSync *sync = find_nvn_sync(sync_address, info.device);
        if (!sync)
            return NVN_FRONT_INVALID_GUEST;
        /* R93: wait with every lock released, as nvnSyncWait does (R79). This
         * call holds the graphics lock (and the global one), so wait_nvn_sync
         * spun in vkWaitForFences with both held: ~0.7 s stalls every ~1.3 s
         * while the replay worker sat behind the graphics lock. */
        GuestBoundary *scope = pokopia_host_thread();
        const bool kernel = scope && scope->kernel_locked;
        if (kernel)
            kernel_unlock();
        const unsigned rec_held = rec_release_all();
        const unsigned gfx_held = gfx_release_all();
        if (sync->signal_seq)
            replay_wait_for(sync->signal_seq);
        const uint64_t fence_start = stat_now_ns();
        worker_ns = fence_start - acquire_start;
        was_deferred = sync->deferred;
        was_pending = sync->pending;
        const VkResult waited = wait_nvn_sync(&graphics, sync, UINT64_MAX);
        fence_ns = stat_now_ns() - fence_start;
        gfx_restore(gfx_held);
        rec_restore(rec_held);
        if (kernel)
            kernel_lock();
        if (waited != VK_SUCCESS)
            return NVN_FRONT_BOUNDARY;
    }
    uint32_t acquired;
    bool got = nvn_window_acquire(windows, window, &acquired);
    if (!got && replay_worker_running) {
        const uint64_t drain_start = stat_now_ns();
        replay_drain(); /* R78: images are released as the worker presents */
        drain_ns = stat_now_ns() - drain_start;
        got = nvn_window_acquire(windows, window, &acquired);
    }
    /* R93: any acquire over 50 ms, by where the time went. */
    const uint64_t acquire_ns = stat_now_ns() - acquire_start;
    if (acquire_ns > UINT64_C(50000000)) {
        static unsigned logged;
        if (logged++ < 400)
            fprintf(probe_log, "Acquire stall ms=%.1f worker=%.1f fence=%.1f (deferred=%d pending=%d) drain=%.1f t=%.1fs\n",
                acquire_ns * 1e-6, worker_ns * 1e-6, fence_ns * 1e-6, was_deferred, was_pending, drain_ns * 1e-6,
                pokopia_boot_monotonic_ns() * 1e-9);
    }
    if (got) {
#if defined(__PROSPERO__)
        frame_trace("acquire", 0, acquired, acquire_ns);
#endif
        memcpy(index, &acquired, sizeof acquired);
        *result = 0;
        /* Keep the startup diagnostic. Per-frame log writes can block the
         * game thread and hold the recording lock across filesystem I/O. */
        static unsigned logged_acquires;
        if (logged_acquires < 12 || !(pokopia_debug_flags & (1u << 20))) {
            if (logged_acquires < 12)
                ++logged_acquires;
            fprintf(probe_log, "NVN acquired game image %u\n", acquired);
            fflush(probe_log);
        }
    } else {
        *result = 1;
    }
    return NVN_FRONT_HANDLED;
}

#ifndef _WIN32
static uint64_t overlay_clock_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

#if defined(__PROSPERO__)
static bool overlay_process_cpu_us(uint64_t *cpu_us)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        *cpu_us = ((uint64_t)usage.ru_utime.tv_sec + (uint64_t)usage.ru_stime.tv_sec) * UINT64_C(1000000) +
                  (uint64_t)usage.ru_utime.tv_usec + (uint64_t)usage.ru_stime.tv_usec;
        return true;
    }
    struct timespec cpu;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) != 0)
        return false;
    *cpu_us = (uint64_t)cpu.tv_sec * UINT64_C(1000000) + (uint64_t)cpu.tv_nsec / 1000;
    return true;
}

typedef struct {
    int cpu_percent;
    uint64_t rss_bytes, guest_heap_bytes;
} OverlayUsage;

static OverlayUsage overlay_usage(uint64_t now_ms)
{
    static OverlayUsage usage = {.cpu_percent = -1};
    static uint64_t previous_ms, previous_cpu_us;
    static bool previous_cpu_valid;
    static unsigned cpu_count;
    if (!now_ms || (previous_ms && now_ms - previous_ms < 1000))
        return usage;
    if (!cpu_count)
        cpu_count = hzn_thread_process_cpu_count();
    uint64_t cpu_us = 0;
    const bool cpu_valid = cpu_count && overlay_process_cpu_us(&cpu_us);
    const uint64_t elapsed_ms = now_ms - previous_ms;
    usage.cpu_percent = -1;
    if (cpu_valid && previous_cpu_valid && elapsed_ms <= 3000 && cpu_us >= previous_cpu_us) {
        const double share = (double)(cpu_us - previous_cpu_us) / (10.0 * (double)elapsed_ms * cpu_count);
        usage.cpu_percent = (int)(share >= 100.0 ? 100.0 : share + 0.5);
    }
    previous_ms = now_ms;
    previous_cpu_us = cpu_us;
    previous_cpu_valid = cpu_valid;
    usage.rss_bytes = pokopia_orbis_process_rss_bytes();
    usage.guest_heap_bytes = pokopia_boot_heap_size();
    return usage;
}
#endif

static bool refresh_live_overlay(const PokopiaLiveFrame *frame, double fps)
{
    static uint64_t next_update_ms;
    static const char *last_phase;
    if (overlay_disabled)
        return false;

    const char *reason = atomic_load_explicit(&overlay_stop, memory_order_relaxed);
    const char *phase = reason ? reason : frame ? NULL : "STARTING GAME";
    const uint64_t now = overlay_clock_ms();
    if (now && now < next_update_ms && phase == last_phase)
        return true;

#if defined(__PROSPERO__)
    const char *title = "POKOPIA PS5 DEBUG";
#else
    const char *title = "POKOPIA PS4 DEBUG";
#endif
    char timing[35], pacing[35], resolution[35];
    if (phase)
        snprintf(timing, sizeof timing, "%s%.29s", reason ? "STOP " : "", phase);
    else if (fps > 0)
        snprintf(timing, sizeof timing, "%.1f FPS  %.1f MS AVG", fps, 1000.0 / fps);
    else
        snprintf(timing, sizeof timing, "FPS --  -- MS AVG");

    const unsigned cap = active_frame_cap();
    char cap_text[11] = "OFF";
    if (cap)
        snprintf(cap_text, sizeof cap_text, "%u", cap);
    const uint32_t refresh_mhz = pokopia_present_refresh_mhz;
    if (refresh_mhz)
        snprintf(pacing, sizeof pacing, "CAP %s  DISPLAY %.2f HZ", cap_text, refresh_mhz / 1000.0);
    else
        snprintf(pacing, sizeof pacing, "CAP %s  DISPLAY -- HZ", cap_text);

    const VkExtent2D output = live_output.present->extent;
    if (frame) {
        const VkExtent2D crop = frame->crop.extent;
        const VkExtent2D render = crop.width && crop.height ? crop : frame->extent;
        snprintf(resolution, sizeof resolution, "RENDER %uX%u / OUT %uX%u",
                 render.width, render.height, output.width, output.height);
    } else {
        snprintf(resolution, sizeof resolution, "RENDER -- / OUT %uX%u", output.width, output.height);
    }
#if defined(__PROSPERO__)
    char loads[35], memory[35];
    char cpu_text[5] = "--", gpu_text[5] = "--";
    const OverlayUsage usage = frame ? overlay_usage(now) : (OverlayUsage){.cpu_percent = -1};
    if (usage.cpu_percent >= 0)
        snprintf(cpu_text, sizeof cpu_text, "%u%%", (unsigned)usage.cpu_percent);
    if (overlay_gpu_draw_percent >= 0)
        snprintf(gpu_text, sizeof gpu_text, "%u%%", (unsigned)overlay_gpu_draw_percent);
    snprintf(loads, sizeof loads, "CPU %s  GPU DRAW %s", cpu_text, gpu_text);
    const double gib = 1.0 / 1073741824.0;
    if (usage.rss_bytes)
        snprintf(memory, sizeof memory, "RSS %.2f GIB  HEAP %.2f GIB",
                 usage.rss_bytes * gib, usage.guest_heap_bytes * gib);
    else if (usage.guest_heap_bytes)
        snprintf(memory, sizeof memory, "GUEST HEAP %.2f GIB", usage.guest_heap_bytes * gib);
    else
        snprintf(memory, sizeof memory, "MEMORY --");
    const char *const lines[POKOPIA_OVERLAY_LINES] = {title, timing, pacing, resolution, loads, memory};
#else
    const char *const lines[POKOPIA_OVERLAY_LINES] = {title, timing, pacing, resolution};
#endif
    const VkResult result = pokopia_live_present_overlay_update(&live_output, lines);
    if (result != VK_SUCCESS) {
        overlay_disabled = true;
        fprintf(probe_log, "Live debug overlay unavailable (%d)\n", result);
        fflush(probe_log);
        return false;
    }
    last_phase = phase;
    next_update_ms = now + 250;
    return true;
}

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
    if (result != VK_SUCCESS)
        return false;
    if (refresh_live_overlay(NULL, 0)) {
        result = pokopia_live_present_show_overlay(&live_output, UINT64_C(5000000000));
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
            fputs("Live debug overlay startup frame submitted\n", probe_log);
        else
            fprintf(probe_log, "Live debug startup frame failed (%d)\n", result);
        fflush(probe_log);
    }
    return !graphics.present.faulted;
}
#endif

#if !defined(_WIN32) || defined(POKOPIA_HOST_HEADLESS_PRESENT)
static NvnFrontReplyKind present_nvn_frame_execute(const uint64_t x[32]);
/* R78: a present is queued behind the frame's replay jobs and made by the
 * replay worker, so the game starts its next frame meanwhile (it waited ~30 ms
 * a frame for the worker here). Images stay acquired until then; acquiring
 * waits for the worker only when none is free. */
static NvnFrontReplyKind present_nvn_frame(const uint64_t x[32])
{
#if defined(__PROSPERO__)
    frame_trace("present-call", 0, x[2], 0);
#endif
    update_guest_pad();
#if defined(__PROSPERO__)
    if (pokopia_trace_guest_tid() == 1)
        hzn_thread_test_affinity(0, (pokopia_debug_flags & POKOPIA_DEBUG_AFFINITY) != 0);
#endif
    /* R92/R96: the port menu's frame cap. The game thread sleeps after this
     * present until a frame period (a little under, so the present still
     * lands on the vblank it is due for) after the last one, in
     * dispatch_graphics; FIFO presentation then shows each frame for two
     * (60 fps at 120 Hz, 30 at 60 Hz) or four vblanks. Uncapped (0), and a
     * cap at or above the display's refresh, do not sleep: the game then
     * runs up to the display's refresh. debug/framecap overrides the menu. */
    const unsigned frame_cap = active_frame_cap();
    const uint32_t refresh_mhz = pokopia_present_refresh_mhz ? pokopia_present_refresh_mhz : 59940;
    if (frame_cap && frame_cap * 1000u < refresh_mhz - 1000u) {
        static uint64_t cap_due;
        const uint64_t now = stat_now_ns(), frame = frame_cap == 30 ? UINT64_C(33000000) : UINT64_C(16400000);
        if (cap_due > now) {
            present_pace_until = cap_due;
            STAT_ADD(stat_paced_frames, 1);
        }
        cap_due = (cap_due > now ? cap_due : now) + frame;
    }
    if (replay_worker_running) {
        NvnFrontQueueInfo queue;
        NvnWindowInfo window;
        if (!nvn_front_get_queue(nvn, x[0], &queue) || !nvn_window_get_acquired(windows, x[1], (uint32_t)x[2], &window) ||
            window.device != queue.device)
            return NVN_FRONT_INVALID_GUEST;
        const uint64_t args[3] = {x[0], x[1], x[2]};
        if (replay_enqueue_item(2, NULL, args))
            return NVN_FRONT_HANDLED;
    }
    return present_nvn_frame_execute(x);
}

static void release_nvn_image(uint64_t window, uint32_t index)
{
    rec_lock();
    nvn_window_release(windows, window, index);
    rec_unlock();
}

static NvnFrontReplyKind present_nvn_frame_execute(const uint64_t x[32])
{
    NvnFrontQueueInfo queue;
    NvnWindowInfo window;
    NvnFrontTextureInfo image;
    const uint32_t index = (uint32_t)x[2];
    rec_lock();
    const bool valid = nvn_front_get_queue(nvn, x[0], &queue) &&
        nvn_window_get_acquired(windows, x[1], index, &window) && window.device == queue.device &&
        nvn_front_get_texture(nvn, window.textures[index], &image);
    rec_unlock();
    if (!valid)
        return NVN_FRONT_INVALID_GUEST;
    PokopiaVkTexture *texture = image.native;
    if (!texture || !texture->backing.image || !texture->render_serial) {
        if (replay_worker_thread) {
            release_nvn_image(x[1], index); /* R78: nothing drawn yet; drop this frame */
            return NVN_FRONT_HANDLED;
        }
        return NVN_FRONT_BOUNDARY;
    }
#ifdef _WIN32
    /* The headless test consumes completed images and releases the same NVN window slots. */
    if (texture->render_serial > graphics.render_serial)
        return NVN_FRONT_BOUNDARY;
    release_nvn_image(x[1], index);
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
    replay_drain(); /* R77: the frame's queued replay jobs first */
    pokopia_vk_flush_pending(POKOPIA_FLUSH_PRESENT);
    /* Use the same one-second sample for the optional FPS counter and debug panel. */
    static uint64_t fps_window_ns, fps_frames;
    static double fps;
    static bool fps_disabled, fps_shown;
    const uint64_t now_ns = pokopia_boot_monotonic_ns();
    if (!fps_window_ns)
        fps_window_ns = now_ns;
    else
        ++fps_frames;
    const uint64_t span_ns = now_ns - fps_window_ns;
    const bool sampled = span_ns >= UINT64_C(1000000000);
    if (sampled) {
        fps = (double)fps_frames * 1e9 / (double)span_ns;
        fps_window_ns = now_ns;
        fps_frames = 0;
    }
    if (!fps_disabled && (!fps_shown || sampled)) {
        char text[16];
        if (fps > 0)
            snprintf(text, sizeof text, "FPS %.1f", fps);
        else
            snprintf(text, sizeof text, "FPS --");
        const VkResult fps_result = pokopia_live_present_fps_update(&live_output, text);
        if (fps_result == VK_SUCCESS) {
            fps_shown = true;
        } else if (fps_result == VK_ERROR_FEATURE_NOT_PRESENT || fps_result == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
                   fps_result == VK_ERROR_OUT_OF_HOST_MEMORY) {
            fps_disabled = true;
            fprintf(probe_log, "Live FPS counter unavailable (%d)\n", (int)fps_result);
            fflush(probe_log);
        }
    }
    /* R92: port settings: overlays shown or hidden, the menu drawn when it changed. */
    live_output.fps_hidden = !pokopia_settings.fps_counter;
    live_output.overlay_hidden = !pokopia_settings.debug_info;
    if (!live_output.overlay_hidden)
        refresh_live_overlay(&frame, fps);
    if (atomic_exchange_explicit(&menu_dirty, false, memory_order_acq_rel) && live_output.present) {
        pthread_mutex_lock(&pad_mutex);
        const PokopiaMenu menu = port_menu;
        const bool open = menu_open;
        pthread_mutex_unlock(&pad_mutex);
        uint8_t *menu_pixels = NULL;
        bool menu_bgra = false;
        const uint32_t menu_w = live_output.present->extent.width * 1600 / 3840;
        const uint32_t menu_h = menu_w * 1300 / 1600;
        if (open && pokopia_live_present_menu_pixels(&live_output, menu_w, menu_h, &menu_pixels, &menu_bgra) == VK_SUCCESS) {
            pokopia_menu_draw(&menu, menu_pixels, menu_w, menu_h, menu_bgra);
            pokopia_live_present_menu_flush(&live_output);
            live_output.menu_visible = true;
        } else {
            live_output.menu_visible = false;
        }
    }
#if defined(__PROSPERO__)
    {
        /* R96: "PlayStation Edition" under the logo while the title screen's logo
         * texture is being drawn, fading in and out (FSR output path). */
        extern volatile int pokopia_title_drawn;
        const bool title = pokopia_title_drawn != 0;
        pokopia_title_drawn = 0;
        float alpha = live_output.banner_alpha + (title ? 0.1f : -0.2f);
        live_output.banner_alpha = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
        if (live_output.present) {
            live_output.banner_x = (int32_t)((uint64_t)live_output.present->extent.width * 2510 / 3840);
            live_output.banner_y = (int32_t)((uint64_t)live_output.present->extent.height * 1285 / 2160);
        }
    }
#endif
    VkResult result = pokopia_live_present_frame(&live_output, &frame, UINT64_C(5000000000), &shot);
    {
        /* R72: say why a requested screenshot was not saved. */
        static unsigned shot_failures;
        if (shot != VK_SUCCESS && shot != VK_NOT_READY && shot_failures < 16) {
            ++shot_failures;
            fprintf(probe_log, "Live screenshot failed result=%d path=%s\n", (int)shot, live_capture.path);
            fflush(probe_log);
        }
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        fprintf(probe_log, "Live PS4 VideoOut present failed (%d)\n", result);
#if defined(__PROSPERO__)
        {
            VkFormatProperties src, dst;
            vkGetPhysicalDeviceFormatProperties(graphics.physical, frame.format, &src);
            vkGetPhysicalDeviceFormatProperties(graphics.physical, live_output.present->format, &dst);
            fprintf(probe_log,
                    "  frame format=%d %ux%u layout=%d crop=%d,%d %ux%u; scanout format=%d %ux%u; "
                    "src optimal features=0x%x dst optimal features=0x%x\n",
                    (int)frame.format, frame.extent.width, frame.extent.height, (int)frame.layout,
                    frame.crop.offset.x, frame.crop.offset.y, frame.crop.extent.width, frame.crop.extent.height,
                    (int)live_output.present->format, live_output.present->extent.width,
                    live_output.present->extent.height, (unsigned)src.optimalTilingFeatures,
                    (unsigned)dst.optimalTilingFeatures);
        }
#endif
        fflush(probe_log);
        return NVN_FRONT_BOUNDARY;
    }
#if defined(__PROSPERO__)
    last_present.texture = texture;
    last_present.extent = frame.extent;
    last_present.crop = frame.crop;
    last_present.presented_ns = stat_now_ns();
#endif
    /* R61: the R60 loading-screen frame cap is off; it slept 80 s per load
     * without shortening it (loading ended at 364 s either way). */
    retire_cold_textures();
    release_nvn_image(x[1], index);
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

#if defined(__PROSPERO__)
static void report_nvn_wait_site(uint64_t lr, uint64_t request, bool blocked, uint64_t total,
                                 const NvnWaitBreakdown *trace)
{
    enum { SITE_COUNT = 24 };
    typedef struct {
        uint64_t lr, hits, blocked, requested, total;
        NvnWaitBreakdown trace;
    } Site;
    static _Thread_local Site sites[SITE_COUNT];
    static _Thread_local uint64_t report_at;
    const uint64_t now = stat_now_ns();
    if (!report_at)
        report_at = now + UINT64_C(5000000000);
    Site *site = &sites[SITE_COUNT - 1];
    for (unsigned i = 0; i < SITE_COUNT - 1; ++i) {
        if (!sites[i].hits || sites[i].lr == lr) {
            site = &sites[i];
            site->lr = lr;
            break;
        }
    }
    ++site->hits;
    site->blocked += blocked;
    site->requested += blocked && request < UINT64_C(1000000000) ? request : 0;
    site->total += total;
    site->trace.replay_ns += trace->replay_ns;
    site->trace.flush_ns += trace->flush_ns;
    site->trace.fence_ns += trace->fence_ns;
    site->trace.pending_polls += trace->pending_polls;
    if (now < report_at)
        return;
    const uint64_t tid = pokopia_trace_guest_tid();
    for (unsigned i = 0; i < SITE_COUNT; ++i) {
        const Site *s = &sites[i];
        if (s->total >= UINT64_C(1000000))
            fprintf(probe_log, "NVN wait t%" PRIu64 " lr=%#" PRIx64 " count=%" PRIu64 " blocked=%" PRIu64
                " requested_ms=%.1f total_ms=%.1f worker_ms=%.1f flush_ms=%.1f fence_ms=%.1f"
                " pending_polls=%" PRIu64 "\n",
                tid, s->lr, s->hits, s->blocked, s->requested * 1e-6, s->total * 1e-6,
                s->trace.replay_ns * 1e-6, s->trace.flush_ns * 1e-6, s->trace.fence_ns * 1e-6,
                s->trace.pending_polls);
    }
    memset(sites, 0, sizeof sites);
    report_at = now + UINT64_C(5000000000);
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
        NvnWaitBreakdown *trace = NULL;
#if defined(__PROSPERO__)
        NvnWaitBreakdown sample = {0};
        if (pokopia_debug_flags & POKOPIA_DEBUG_WAIT_SITES)
            trace = &sample;
        const uint64_t start = trace ? stat_now_ns() : 0;
#endif
        VkResult result = wait_nvn_sync_traced(&graphics, sync, 0, trace);
        const bool blocked = result == VK_TIMEOUT && x[1];
        if (result == VK_SUCCESS) {
            x[0] = 0;
        } else if (blocked) {
            result = wait_nvn_sync_traced(&graphics, sync, x[1], trace);
            x[0] = result == VK_SUCCESS ? 1 : result == VK_TIMEOUT ? 2 : 3;
        } else {
            x[0] = result == VK_TIMEOUT ? 2 : 3;
        }
#if defined(__PROSPERO__)
        if (trace)
            report_nvn_wait_site(x[30], x[1], blocked, stat_now_ns() - start, trace);
        if (x[0] == 2 && !x[1])
            frame_trace("poll-pending", 0, sync->signal_seq, x[30]);
#endif
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
    if ((uint32_t)x[2] || (uint32_t)x[3] > 1)
        return NVN_FRONT_BOUNDARY;
    if (replay_worker_running && replay_enqueue_item(1, sync, NULL))
        return NVN_FRONT_HANDLED; /* R77: signaled in order by the replay worker */
    return execute_nvn_sync(&graphics, sync, false) ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}

static NvnFrontReplyKind dispatch_nvn_replay(Ctx *context, const char *name, const char *static_key,
                                            const uint32_t scalar[3])
{
#if defined(__PROSPERO__)
    enum { POKOPIA_DEBUG_INTERNED_DISPATCH = 1u << 29 };
    if (!(pokopia_debug_flags & POKOPIA_DEBUG_INTERNED_DISPATCH))
        static_key = NULL;
#else
    static_key = NULL;
#endif
    NvnFrontReplyKind reply = nvn_replay_dispatch_keyed(replay, name, static_key, context->x, scalar);
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
        /* R90: the caller may hold the global lock or run with it released. */
        const bool had_kernel = scope->kernel_locked;
        const bool kernel_released = guest_kernel_released;
        if (had_kernel)
            kernel_unlock();
        guest_kernel_released = false;
        const unsigned gfx_held = gfx_release_all();
        const unsigned rec_held = rec_release_all(); /* R79 */
        /* R89: how long the game's callback takes, and which SVCs it waits in. */
        uint64_t svc_before[STAT_SVCS] = {0};
#if defined(__PROSPERO__)
        if (stat_thread)
            for (unsigned s = 0; s < STAT_SVCS; ++s)
                svc_before[s] = atomic_load_explicit(&stat_thread->svc_by_ns[s], memory_order_relaxed);
#endif
        const uint64_t callback_start = stat_now_ns();
        ++memory_callback_depth;
        call_indirect(context, request.function);
        --memory_callback_depth;
        const uint64_t callback_ns = stat_now_ns() - callback_start;
        if (callback_ns > UINT64_C(2000000)) {
            fprintf(probe_log, "NVN memory callback took %.1f ms:", callback_ns * 1e-6);
#if defined(__PROSPERO__)
            if (stat_thread)
                for (unsigned s = 0; s < STAT_SVCS; ++s) {
                    const uint64_t now = atomic_load_explicit(&stat_thread->svc_by_ns[s], memory_order_relaxed);
                    if (now > svc_before[s] && now - svc_before[s] > UINT64_C(500000))
                        fprintf(probe_log, " svc%#x=%.1fms", s, (now - svc_before[s]) * 1e-6);
                }
#endif
            fputc('\n', probe_log);
            fflush(probe_log);
        }
        gfx_restore(gfx_held);
        rec_restore(rec_held);
        guest_kernel_released = kernel_released;
        if (had_kernel)
            kernel_lock();
        scope->nvn_memory_callback = false;
        *context = saved;

        NvnFrontCommandBufferInfo command;
        if (!nvn_front_get_command_buffer(nvn, request.encoder, &command) || command.native != request.native)
            break;
        if ((control ? command.control_revision : command.command_revision) == request.revision)
            break;
        reply = nvn_replay_dispatch_keyed(replay, name, static_key, context->x, scalar);
    }
    /* R75: a guest queue submit ends the general-draw batch, so host texel
     * buffers and guest copies see every draw of the submit. */
    if (!strcmp(name, "nvnQueueSubmitCommands")) {
#if defined(__PROSPERO__)
        if (pokopia_trace_guest_tid() == 24)
            hzn_thread_test_affinity(2, (pokopia_debug_flags & POKOPIA_DEBUG_AFFINITY) != 0);
#endif
        if (!replay_worker_running)
            pokopia_vk_flush_pending(POKOPIA_FLUSH_QUEUE_SUBMIT);
    }
    return reply;
}

/* R68: graphics state (NVN front, replay, Vulkan queue) has its own lock,
 * taken before the global lock. A submit's execute pass drops the global lock
 * so the game's other threads keep running while the bridge draws. */
#if defined(__PROSPERO__)
static pthread_mutex_t gfx_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned gfx_depth;
/* R76: time threads spend waiting for the graphics lock, by thread role. */
uint64_t gfx_wait_ns, gfx_wait_count, gfx_yields;
static _Atomic unsigned gfx_waiters;
/* R79: per-thread lock accounting; the replay worker has no stat_thread. */
static _Thread_local uint64_t gfx_since;
static StatThread *lock_stat_entry(void)
{
    return lock_stats ? lock_stats : stat_thread;
}
/* R79: graphics-lock waits by the call that waited. */
static _Thread_local const char *gfx_site;
enum { GFX_SITES = 32 };
static struct { const char *site; uint64_t ns, count; } gfx_sites[GFX_SITES];
static atomic_flag gfx_sites_lock = ATOMIC_FLAG_INIT;
static void gfx_site_charge(uint64_t ns)
{
    const char *site = gfx_site ? gfx_site : "(other)";
    while (atomic_flag_test_and_set_explicit(&gfx_sites_lock, memory_order_acquire))
        __builtin_ia32_pause();
    for (unsigned i = 0; i < GFX_SITES; ++i)
        if (gfx_sites[i].site == site || !gfx_sites[i].site) {
            gfx_sites[i].site = site;
            gfx_sites[i].ns += ns;
            ++gfx_sites[i].count;
            break;
        }
    atomic_flag_clear_explicit(&gfx_sites_lock, memory_order_release);
}
void gfx_sites_report(FILE *log)
{
    while (atomic_flag_test_and_set_explicit(&gfx_sites_lock, memory_order_acquire))
        __builtin_ia32_pause();
    fputs("Graphics lock waits by call", log);
    for (unsigned i = 0; i < GFX_SITES && gfx_sites[i].site; ++i)
        if (gfx_sites[i].ns >= 1000000)
            fprintf(log, " %s=%" PRIu64 "/%.0fms", gfx_sites[i].site, gfx_sites[i].count, gfx_sites[i].ns * 1e-6);
    fputc('\n', log);
    memset(gfx_sites, 0, sizeof gfx_sites);
    atomic_flag_clear_explicit(&gfx_sites_lock, memory_order_release);
}
static void gfx_acquire(void)
{
    if (pthread_mutex_trylock(&gfx_mutex) != 0) {
        const uint64_t start = stat_now_ns();
        atomic_fetch_add_explicit(&gfx_waiters, 1, memory_order_relaxed);
        pthread_mutex_lock(&gfx_mutex);
        atomic_fetch_sub_explicit(&gfx_waiters, 1, memory_order_relaxed);
        const uint64_t waited = stat_now_ns() - start;
        __atomic_add_fetch(&gfx_wait_ns, waited, __ATOMIC_RELAXED);
        __atomic_add_fetch(&gfx_wait_count, 1, __ATOMIC_RELAXED);
        StatThread *entry = lock_stat_entry();
        if (entry)
            atomic_fetch_add_explicit(&entry->gfx_wait_ns, waited, memory_order_relaxed);
        gfx_site_charge(waited);
    }
    gfx_since = stat_now_ns();
    atomic_store_explicit(&gfx_owner_id, lock_owner_tag(), memory_order_relaxed);
}
static void gfx_release(void)
{
    StatThread *entry = lock_stat_entry();
    if (entry)
        atomic_fetch_add_explicit(&entry->gfx_hold_ns, stat_now_ns() - gfx_since, memory_order_relaxed);
    atomic_store_explicit(&gfx_owner_id, 0, memory_order_relaxed);
    pthread_mutex_unlock(&gfx_mutex);
}
static void gfx_lock(void)
{
    if (gfx_depth++ == 0)
        gfx_acquire();
}
static void gfx_unlock(void)
{
    if (gfx_depth && --gfx_depth == 0)
        gfx_release();
}
static unsigned gfx_release_all(void)
{
    const unsigned depth = gfx_depth;
    if (depth) {
        gfx_depth = 0;
        gfx_release();
    }
    return depth;
}
static void gfx_restore(unsigned depth)
{
    if (depth) {
        gfx_acquire();
        gfx_depth = depth;
    }
}
/* R79: command-buffer recording has its own lock, taken before the graphics
 * lock. Recording only appends to a guest command buffer's host stream and
 * reads front objects; the replay worker holds the graphics lock for whole
 * jobs, and the game's render thread waited behind it for every recorded call.
 * Front object changes take both locks (graphics first), so recording never
 * sees them half done; the worker never takes this one. */
static pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned rec_depth;
uint64_t rec_wait_ns, rec_wait_count;
static void rec_acquire(void)
{
    if (pthread_mutex_trylock(&rec_mutex) != 0) {
        const uint64_t start = stat_now_ns();
        pthread_mutex_lock(&rec_mutex);
        const uint64_t waited = stat_now_ns() - start;
        __atomic_add_fetch(&rec_wait_ns, waited, __ATOMIC_RELAXED);
        __atomic_add_fetch(&rec_wait_count, 1, __ATOMIC_RELAXED);
        StatThread *entry = lock_stat_entry();
        if (entry)
            atomic_fetch_add_explicit(&entry->rec_wait_ns, waited, memory_order_relaxed);
    }
    atomic_store_explicit(&rec_owner_id, lock_owner_tag(), memory_order_relaxed);
}
static void rec_lock(void)
{
    if (rec_depth++ == 0)
        rec_acquire();
}
static void rec_unlock(void)
{
    if (rec_depth && --rec_depth == 0) {
        atomic_store_explicit(&rec_owner_id, 0, memory_order_relaxed);
        pthread_mutex_unlock(&rec_mutex);
    }
}
static unsigned rec_release_all(void)
{
    const unsigned depth = rec_depth;
    if (depth) {
        rec_depth = 0;
        atomic_store_explicit(&rec_owner_id, 0, memory_order_relaxed);
        pthread_mutex_unlock(&rec_mutex);
    }
    return depth;
}
static void rec_restore(unsigned depth)
{
    if (depth) {
        rec_acquire();
        rec_depth = depth;
    }
}
static bool gfx_held_now(void)
{
    return gfx_depth != 0;
}
/* R79: the graphics lock from a call that holds only the recording (and
 * global) lock; those are dropped first to keep the lock order. */
static void gfx_lock_late(void)
{
    if (gfx_depth) {
        ++gfx_depth;
        return;
    }
    GuestBoundary *scope = pokopia_host_thread();
    const bool kernel = scope && scope->kernel_locked && !replay_worker_thread;
    if (kernel)
        kernel_unlock();
    const unsigned rec_held = rec_release_all();
    gfx_site = "(sync flush)";
    gfx_lock();
    rec_restore(rec_held);
    if (kernel)
        kernel_lock();
}
static void replay_execute_begin(void *opaque)
{
    (void)opaque;
    if (replay_worker_thread)
        return;
    GuestBoundary *scope = pokopia_host_thread();
    if (!gfx_depth || !scope || !scope->kernel_locked || guest_kernel_released)
        return;
    kernel_unlock();
    guest_kernel_released = true;
}
/* R76: between replayed operations, hand the graphics lock to a thread that
 * is waiting for it (the game's main thread records the next frame's commands
 * while the render thread replays this one; it used to wait out whole submits). */
static void replay_execute_yield(void *opaque)
{
    (void)opaque;
    /* R78: every 48 operations at most (16: 23 fps, 48: 24-25, 128: 21); yielding after each one made the
     * lock hand-offs the worker's biggest cost once the game ran alongside. */
    static _Thread_local unsigned since_yield;
    if (++since_yield < 48 || !gfx_depth || !atomic_load_explicit(&gfx_waiters, memory_order_relaxed))
        return;
    since_yield = 0;
    const unsigned before = atomic_load_explicit(&gfx_waiters, memory_order_relaxed);
    const unsigned depth = gfx_release_all();
    const uint64_t give_up = stat_now_ns() + 200000;
    while (atomic_load_explicit(&gfx_waiters, memory_order_relaxed) >= before && stat_now_ns() < give_up)
        sched_yield();
    gfx_restore(depth);
    ++gfx_yields;
}
/* R77: replay worker. nvnQueueSubmitCommands validates on the game's thread and
 * queues the execute pass here, so the game records the next commands while
 * this thread replays (they ran one after the other on the main thread). Fence
 * waits, finish and present wait for the worker to pass the item they need,
 * with the graphics and global locks released. */
typedef struct {
    int kind; /* 0: replay job, 1: queue fence signal, 2: present (R78) */
    void *object;
    uint64_t seq;
    uint64_t args[3];
} ReplayItem;
enum { REPLAY_QUEUE = 256 };
static ReplayItem replay_items[REPLAY_QUEUE];
static unsigned replay_item_head, replay_item_tail;
static pthread_mutex_t replay_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t replay_cond = PTHREAD_COND_INITIALIZER;
static uint64_t replay_submitted;
static _Atomic uint64_t replay_completed;
uint64_t replay_worker_waits, replay_worker_wait_ns;

static uint64_t replay_completed_now(void)
{
    return replay_worker_running ? atomic_load_explicit(&replay_completed, memory_order_acquire) : UINT64_MAX;
}

static void replay_wait_for(uint64_t target)
{
    if (replay_worker_thread || !replay_worker_running ||
        atomic_load_explicit(&replay_completed, memory_order_acquire) >= target)
        return;
    const uint64_t start = stat_now_ns();
    const unsigned depth = gfx_release_all();
    const unsigned rec_held = rec_release_all(); /* R79: others may record meanwhile */
    GuestBoundary *scope = pokopia_host_thread();
    const bool kernel = scope && scope->kernel_locked;
    if (kernel)
        kernel_unlock();
    pthread_mutex_lock(&replay_mutex);
    while (atomic_load_explicit(&replay_completed, memory_order_acquire) < target)
        pthread_cond_wait(&replay_cond, &replay_mutex);
    pthread_mutex_unlock(&replay_mutex);
    gfx_site = "(after worker wait)";
    gfx_restore(depth);
    rec_restore(rec_held);
    if (kernel)
        kernel_lock();
    ++replay_worker_waits;
    replay_worker_wait_ns += stat_now_ns() - start;
}

static void replay_drain(void)
{
    replay_wait_for(replay_submitted);
}

static void replay_execute_end(void *opaque);
static void *replay_worker_main(void *unused);

/* Called with the graphics lock held. */
static uint64_t replay_enqueue_item(int kind, void *object, const uint64_t *args)
{
    if (!replay_worker_running) {
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 4u << 20);
        replay_worker_running = !pthread_create(&thread, &attr, replay_worker_main, NULL);
        pthread_attr_destroy(&attr);
        if (!replay_worker_running)
            return 0;
        pthread_detach(thread);
    }
    while (replay_submitted - atomic_load_explicit(&replay_completed, memory_order_acquire) >= REPLAY_QUEUE - 1)
        replay_wait_for(replay_submitted - (REPLAY_QUEUE - 2));
    pthread_mutex_lock(&replay_mutex);
    const uint64_t seq = ++replay_submitted;
    if (kind == 1)
        ((NvnHostSync *)object)->signal_seq = seq;
    ReplayItem *slot = &replay_items[replay_item_tail++ % REPLAY_QUEUE];
    *slot = (ReplayItem){kind, object, seq, {0}};
    if (args)
        memcpy(slot->args, args, sizeof slot->args);
    if (!kind && (pokopia_debug_flags & POKOPIA_DEBUG_FRAME_TRACE))
        nvn_replay_trace_submission(object, seq);
    const uint64_t queued_at = (pokopia_debug_flags & POKOPIA_DEBUG_FRAME_TRACE) ? stat_now_ns() : 0;
    pthread_cond_broadcast(&replay_cond);
    pthread_mutex_unlock(&replay_mutex);
    if (queued_at)
        frame_trace("enqueue", queued_at, seq, (unsigned)kind);
    return seq;
}

static bool replay_enqueue_job(void *opaque, NvnReplayJob *job)
{
    (void)opaque;
    return replay_enqueue_item(0, job, NULL) != 0;
}

static void replay_sync_queued(void *opaque, void *object)
{
    (void)opaque;
    ((NvnHostSync *)object)->signal_seq = replay_submitted + 1;
}

static void *replay_worker_main(void *unused)
{
    (void)unused;
    replay_worker_thread = true;
    guest_kernel_released = true;
    StatThread *worker_entry = NULL;
    {
        /* R77: sampled by the profiler as t999 (always in host code). */
        static Ctx worker_ctx;
        const unsigned index = atomic_fetch_add_explicit(&stat_thread_count, 1, memory_order_relaxed);
        if (index < STAT_THREADS) {
            StatThread *entry = &stat_threads[index];
            worker_entry = entry;
            lock_stats = entry; /* R79 */
            atomic_store(&entry->id, 999);
            entry->host = pthread_self();
            stat_thread_stack_bounds(entry); /* R96 */
            atomic_store_explicit(&entry->in_host, 1, memory_order_relaxed);
            atomic_store_explicit(&entry->ctx, &worker_ctx, memory_order_release);
        }
    }
    for (;;) {
        pthread_mutex_lock(&replay_mutex);
        if (replay_item_head == replay_item_tail && worker_entry)
            atomic_store_explicit(&worker_entry->in_host, 0, memory_order_relaxed); /* idle: not sampled */
        while (replay_item_head == replay_item_tail)
            pthread_cond_wait(&replay_cond, &replay_mutex);
        if (worker_entry)
            atomic_store_explicit(&worker_entry->in_host, 1, memory_order_relaxed);
        const ReplayItem item = replay_items[replay_item_head % REPLAY_QUEUE];
        pthread_mutex_unlock(&replay_mutex);
        hzn_thread_test_affinity(4, (pokopia_debug_flags & POKOPIA_DEBUG_AFFINITY) != 0);
        gfx_site = "(worker)";
        gfx_lock();
        frame_trace("begin", 0, item.seq, (unsigned)item.kind);
        bool ok;
        if (item.kind == 0) {
            ok = nvn_replay_run_job(replay, item.object) == NVN_FRONT_HANDLED;
            /* R78: batches span guest submits; each VkQueueSubmit costs ~1 ms in
             * the PS5 driver. Flush only when the queue runs dry. */
            pthread_mutex_lock(&replay_mutex);
            const bool idle = replay_item_head + 1 == replay_item_tail;
            pthread_mutex_unlock(&replay_mutex);
            if (idle || ((pokopia_debug_flags & 8) && pokopia_live_generic_batch_count() >= 64))
                pokopia_vk_flush_pending(POKOPIA_FLUSH_QUEUE_SUBMIT);
        } else if (item.kind == 2) {
            uint64_t args[32] = {item.args[0], item.args[1], item.args[2]};
            ok = present_nvn_frame_execute(args) == NVN_FRONT_HANDLED;
        } else {
            ok = execute_nvn_sync(&graphics, item.object, false);
        }
        gfx_unlock();
        if (!ok) {
            static unsigned logged;
            if (logged++ < 32) {
                fprintf(probe_log, "Replay worker item %" PRIu64 " kind=%d failed\n", item.seq, item.kind);
                fflush(probe_log);
            }
        }
        pthread_mutex_lock(&replay_mutex);
        ++replay_item_head;
        atomic_store_explicit(&replay_completed, item.seq, memory_order_release);
        pthread_cond_broadcast(&replay_cond);
        pthread_mutex_unlock(&replay_mutex);
        frame_trace("done", 0, item.seq, (unsigned)item.kind);
    }
    return NULL;
}

static void replay_execute_end(void *opaque)
{
    (void)opaque;
    if (replay_worker_thread || !guest_kernel_released)
        return;
    guest_kernel_released = false;
    kernel_lock();
}
#else
static void gfx_lock(void) {}
static void gfx_unlock(void) {}
static unsigned gfx_release_all(void) { return 0; }
static void gfx_restore(unsigned depth) { (void)depth; }
static void rec_lock(void) {}
static void rec_unlock(void) {}
static void gfx_lock_late(void) {}
static bool gfx_held_now(void) { return false; }
static unsigned rec_release_all(void) { return 0; }
static void rec_restore(unsigned depth) { (void)depth; }
#define replay_execute_begin NULL
#define replay_execute_end NULL
#define replay_execute_yield NULL
#define replay_enqueue_job NULL
#define replay_sync_queued NULL
static void replay_wait_for(uint64_t target) { (void)target; }
static void replay_drain(void) {}
static uint64_t replay_completed_now(void) { return UINT64_MAX; }
static uint64_t replay_enqueue_item(int kind, void *object, const uint64_t *args)
{
    (void)kind;
    (void)object;
    (void)args;
    return 0;
}
#endif

/* R88: blocks of shaders the translated cache lacks, saved in the translation
 * harness's corpus layout (manifest.tsv, code_<hash>.bin, ctrl_<hash>.bin) under
 * debug/missing, once each. */
static void save_missing_shader(const NvnFrontShaderData *shader, const char *hash)
{
    static char saved[64][17];
    static unsigned saved_count;
    for (unsigned i = 0; i < saved_count; ++i)
        if (!strcmp(saved[i], hash))
            return;
    if (saved_count == 64 || !shader->code || !shader->control || strlen(hash) != 16)
        return;
    snprintf(saved[saved_count++], sizeof saved[0], "%s", hash);
    const uint8_t *control = shader->control;
    uint32_t control_bytes, program_bytes;
    memcpy(&control_bytes, control + 0x30, 4);
    memcpy(&program_bytes, control + 0x38, 4);
    if (control_bytes < NVN_SHADER_CONTROL_PREFIX_BYTES || control_bytes > 0x10000)
        return;
    static const char *const stages[] = {"vertex", "fragment", "geometry", "tess_control", "tess_evaluation",
                                         "compute"};
    const unsigned stage = (unsigned)shader->parsed.stage;
    if (stage >= sizeof stages / sizeof stages[0])
        return;
    mkdir("/data/pokopia/debug/missing", 0777);
    char path[256];
    snprintf(path, sizeof path, "/data/pokopia/debug/missing/code_%s.bin", hash);
    FILE *file = fopen(path, "wb");
    if (!file)
        return;
    fwrite(shader->code, 1, shader->parsed.code_bytes, file);
    fclose(file);
    snprintf(path, sizeof path, "/data/pokopia/debug/missing/ctrl_%s.bin", hash);
    file = fopen(path, "wb");
    if (!file)
        return;
    fwrite(control, 1, control_bytes, file);
    fclose(file);
    const bool fresh = access("/data/pokopia/debug/missing/manifest.tsv", F_OK) != 0;
    file = fopen("/data/pokopia/debug/missing/manifest.tsv", "a");
    if (!file)
        return;
    if (fresh)
        fputs("id\tstage\tcode_off\tcode_size\tctrl_off\tctrl_size\tprogram_start\tprogram_bytes\tconst_off\tconst_size\t"
              "code_sha1_16\tcode_file\tctrl_file\n", file);
    fprintf(file, "run_%s\t%s\t0x0\t%#x\t0x0\t%#x\t0x0\t%#x\t%#x\t%#x\t%s\tcode_%s.bin\tctrl_%s.bin\n", hash,
            stages[stage], shader->parsed.code_bytes, control_bytes, program_bytes, shader->parsed.constants_offset,
            shader->parsed.constants_bytes, hash, hash, hash);
    fclose(file);
    fprintf(probe_log, "Saved missing %s shader %s for translation\n", stages[stage], hash);
    fflush(probe_log);
}

/* R87: RGBA8 texel buffers standing in for 2D placeholders bound as buffer
 * textures (one per size, refreshed from the placeholder's bytes per submit). */
static PokopiaVkTexture *texel_placeholder(uint64_t bytes)
{
    static struct {
        uint64_t bytes;
        PokopiaVkTexture *texture;
    } cache[8];
    static unsigned count;
    for (unsigned i = 0; i < count; ++i)
        if (cache[i].bytes == bytes)
            return cache[i].texture;
    if (count == sizeof cache / sizeof cache[0] || bytes % 4 || !graphics.format_properties)
        return NULL;
    PokopiaVkTexture *texture = calloc(1, sizeof *texture);
    if (!texture)
        return NULL;
    VkFormatProperties properties;
    graphics.format_properties(graphics.physical, VK_FORMAT_R8G8B8A8_UNORM, &properties);
    const VkResult result = pokopia_vk_texel_configure(&texture->texel, graphics.device, graphics.get_device_proc,
        &graphics.memory, properties.bufferFeatures, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)(bytes / 4), 4,
        graphics.properties.limits.maxTexelBufferElements);
    fprintf(probe_log, "NVN texel placeholder bytes=%#" PRIx64 " result=%d\n", bytes, (int)result);
    if (result != VK_SUCCESS) {
        free(texture);
        return NULL;
    }
    texture->format = VK_FORMAT_R8G8B8A8_UNORM;
    cache[count++] = (__typeof__(cache[0])){bytes, texture};
    return texture;
}

/* R87: see pokopia_vk_volume_placeholder (called by the replay worker). */
static void *volume_placeholder(const NvnFrontTextureConfig *like, uint32_t rgba8)
{
    return pokopia_vk_volume_placeholder(&graphics, like, rgba8);
}

static bool dispatch_graphics_inner(Ctx *context, uint64_t address);
static bool dispatch_graphics(Ctx *context, uint64_t address)
{
#ifndef _WIN32
    /* R94: every guest indirect call (the game's virtual calls on all its
     * threads) comes here first. Rule out non-graphics targets before the
     * timing and per-thread bookkeeping below (emulated TLS reads, a TSC
     * read, two atomic stores per call). */
    if (nvn && address != POKOPIA_NV_INITIALIZE && address != POKOPIA_NV_INITIALIZE_DEFAULT &&
        address != POKOPIA_NV_INITIALIZE_DEVTOOLS && !vi_adapter_may_own(&vi, address) &&
        !nvn_front_may_own(nvn, address))
        return false;
    /* R94: per-call timing (the "Graphics stats top" line) only when debug
     * flags bit 15 is clear; its emulated-TLS reads and TSC reads were most of
     * the dispatcher's own time on the render thread. */
    const unsigned flags = pokopia_debug_flags;
    const bool timing = !(flags & POKOPIA_DEBUG_NO_NVN_TIMING);
    unsigned weight = 1;
    bool timed = false;
    uint64_t start = 0;
    const char *outer_name = NULL;
    if (timing) {
        static _Thread_local uint64_t graphics_timing_iteration;
        weight = (flags & POKOPIA_DEBUG_SAMPLE_NVNS) && !memory_callback_depth ? 16u : 1u;
        timed = weight == 1 || pokopia_timing_sample(graphics_timing_iteration++);
        start = timed ? stat_now_ns() : 0;
        outer_name = stat_call_name; /* R89: a nested call (memory callback) keeps the outer name */
        stat_call_name = NULL;
    }
#if defined(__PROSPERO__)
    StatThread *const host_stats = stat_thread;
    uint64_t outer_op = 0;
    if (host_stats) {
        outer_op = atomic_load_explicit(&host_stats->op_addr, memory_order_relaxed); /* R96 */
        atomic_store_explicit(&host_stats->op_addr, address, memory_order_relaxed);
        atomic_store_explicit(&host_stats->in_host, 1, memory_order_relaxed);
    }
#endif
    const bool handled = dispatch_graphics_inner(context, address);
#if defined(__PROSPERO__)
    if (host_stats) {
        atomic_store_explicit(&host_stats->in_host, 0, memory_order_relaxed);
        atomic_store_explicit(&host_stats->op_addr, outer_op, memory_order_relaxed);
    }
#endif
    if (handled && timed) {
        const uint64_t ns = stat_now_ns() - start;
        STAT_ADD(stat_nvn_calls, weight);
        STAT_ADD(stat_nvn_ns, ns * weight);
        stat_charge_name(ns, weight);
        if (memory_callback_depth && ns > UINT64_C(2000000)) {
            fprintf(probe_log, "NVN memory callback inner call %s took %.1f ms\n",
                    stat_call_name ? stat_call_name : "(unnamed)", ns * 1e-6);
            fflush(probe_log);
        }
    }
    if (timing)
        stat_call_name = outer_name;
    /* R60: a paced loading-screen present sleeps here, after the global lock
     * is released, so the loading threads get that time instead. */
    if (present_pace_until) {
        const GuestBoundary *scope = pokopia_host_thread();
        const uint64_t now = stat_now_ns();
        if ((!scope || !scope->kernel_locked) && present_pace_until > now) {
            const uint64_t wait = present_pace_until - now;
            const struct timespec pause = { .tv_sec = (time_t)(wait / UINT64_C(1000000000)),
                                            .tv_nsec = (long)(wait % UINT64_C(1000000000)) };
            nanosleep(&pause, NULL);
            STAT_ADD(stat_paced_ns, wait);
        }
        present_pace_until = 0;
    }
    return handled;
#else
    return dispatch_graphics_inner(context, address);
#endif
}

static bool dispatch_graphics_inner(Ctx *context, uint64_t address)
{
    if (address == POKOPIA_NV_INITIALIZE || address == POKOPIA_NV_INITIALIZE_DEFAULT ||
        address == POKOPIA_NV_INITIALIZE_DEVTOOLS) {
        gfx_lock();
        rec_lock();
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
#if defined(__PROSPERO__)
        pokopia_now_ns = stat_now_ns; /* R76: TSC clock for replay and draw timing */
        {
            extern void *(*nvn_replay_volume_placeholder)(const NvnFrontTextureConfig *, uint32_t);
            nvn_replay_volume_placeholder = volume_placeholder;
            port_settings_once(); /* R92 */
            extern PokopiaVkTexture *(*nvn_replay_texel_placeholder)(uint64_t);
            nvn_replay_texel_placeholder = texel_placeholder;
            extern void (*pokopia_vk_program_missing_hook)(const NvnFrontShaderData *, const char *);
            pokopia_vk_program_missing_hook = save_missing_shader;
        }
        {
            extern uint64_t pokopia_guest_import_base, pokopia_guest_import_size;
            extern FILE *pokopia_generic_log;
            pokopia_guest_import_base = POKOPIA_BOOT_HEAP_BASE;
            pokopia_guest_import_size = pokopia_boot_heap_size();
            pokopia_generic_log = probe_log;
        }
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
        rec_unlock();
        gfx_unlock();
        return true;
    }
    if (!nvn)
        return false;
    /* Every guest indirect call passes through here. Rule out non-graphics
     * targets before taking the global lock: R44 measured ~150k lock
     * acquisitions a second from this path alone, queued behind NVN holds. */
    if (!vi_adapter_may_own(&vi, address) && !nvn_front_may_own(nvn, address))
        return false;
#ifndef _WIN32
    /* R60: builder and state calls touch only the guest structure passed in.
     * Run them without the global lock when every page they touch is already
     * cached as writable; otherwise (or on any failure) redo them under the
     * lock, which is safe because they repeat cleanly. */
    if (nvn_front_is_pure_call(nvn, address)) {
        const uint32_t pure_scalar[] = { context->v[0].w[0], context->v[1].w[0], context->v[2].w[0] };
        extern _Thread_local bool nvn_front_lock_free;
        guest_lockfree = nvn_front_lock_free = true;
        const NvnFrontReply pure = nvn_front_dispatch_with_floats(nvn, address, context->x, pure_scalar);
        guest_lockfree = nvn_front_lock_free = false;
        if (pure.kind == NVN_FRONT_HANDLED) {
            STAT_ADD(stat_pure_fast, 1);
            stat_name_call(pure.key ? pure.key : "(pure)");
            return true;
        }
        STAT_ADD(stat_pure_locked, 1);
    }
#endif
    /* R79: command-buffer recording takes only the recording lock (see rec_lock). */
    const char *call_name = nvn_front_call_name(nvn, address);
    /* R79: SDK-internal NVN or VI code reached by an indirect call and handled
     * by neither adapter: nothing to do, so no locks. These took all three
     * locks and waited behind the replay worker (12.8k calls, 6.7 s a window,
     * on two game threads). */
    if (!call_name && !vi_adapter_is_call(&vi, address))
        return false;
    bool record_only = call_name && !strncmp(call_name, "nvnCommandBuffer", 16) &&
                       strcmp(call_name + 16, "Initialize") && strcmp(call_name + 16, "Finalize");
    /* Queue calls that only validate and hand work to the replay worker (or do
     * nothing): the game's submit thread waited ~1.2 s a window for the worker. */
    if (!record_only && call_name && replay_worker_running && !strncmp(call_name, "nvnQueue", 8))
        record_only = !strcmp(call_name + 8, "SubmitCommands") || !strcmp(call_name + 8, "FenceSync") ||
                      !strcmp(call_name + 8, "Flush") || !strcmp(call_name + 8, "WaitSync");
    /* Front getters and descriptor registration; nvnSyncWait takes the graphics
     * lock itself only when it must flush (wait_nvn_sync). */
    if (!record_only && call_name)
        record_only = nvn_front_is_shared_call(nvn, address) ||
                      (replay_worker_running && !strcmp(call_name, "nvnSyncWait")) ||
                      (replay_worker_running && !(pokopia_debug_flags & 16) &&
                       (!strcmp(call_name, "nvnWindowAcquireTexture") ||
                        !strcmp(call_name, "nvnQueueAcquireTexture"))) ||
                      /* R80: window getters (t1 waited ~1.2 s a window behind the worker) */
                      !strcmp(call_name, "nvnWindowGetPresentInterval") || !strcmp(call_name, "nvnWindowGetCrop");
    /* Graphics before recording, so a thread waiting out the worker does not
     * hold up the recording threads meanwhile. */
    if (!record_only) {
        gfx_site = call_name ? call_name : "(vi)";
        gfx_lock();
    }
    rec_lock();
    /* R90: recording-only calls are serialized by the recording lock; the
     * global lock only guards the memory map, which read_guest/write_guest take
     * on a cache miss (as the replay worker does). Taking it for every draw and
     * bind on the render thread cost ~1 us a call and ~100k contended
     * acquisitions per 5 s outdoors. */
    const bool kernel_needed = !record_only || guest_kernel_released;
    if (kernel_needed)
        kernel_lock();
    else
        guest_kernel_released = true;
#define DISPATCH_KERNEL_RELEASE() do { if (kernel_needed) kernel_unlock(); else guest_kernel_released = false; } while (0)
    nvn_front_shared = record_only;
    ViAdapterReply video = vi_adapter_dispatch(&vi, address, context->x);
    if (video.kind != VI_ADAPTER_NOT_OURS) {
        if (video.kind != VI_ADAPTER_HANDLED) {
            fprintf(probe_log, "VI %s %s\n",
                    video.kind == VI_ADAPTER_BOUNDARY ? "boundary" : "invalid guest argument", video.name);
            stop("VI graphics call", address);
        }
#ifndef _WIN32
        stat_name_call(video.name);
#endif
        fprintf(probe_log, "VI %s handled by the Vulkan output\n", video.name);
        fflush(probe_log);
        DISPATCH_KERNEL_RELEASE();
        rec_unlock();
        if (!record_only)
            gfx_unlock();
        return true;
    }
    const uint32_t scalar[] = { context->v[0].w[0], context->v[1].w[0], context->v[2].w[0] };
    NvnFrontReply reply = nvn_front_dispatch_with_floats(nvn, address, context->x, scalar);
#ifndef _WIN32
    stat_name_call(reply.key ? reply.key : "(nvn)");
#endif
    if (reply.kind == NVN_FRONT_BOUNDARY) {
        uint64_t object = context->x[0];
        /* R78: a call the replay handled before goes straight there; the
         * window and frame-control name chains cost ~20 strcmp a call. */
        /* A failed proc lookup can name the requested procedure in the reply.
         * Cache only the static key for the actual dispatched procedure. */
        const char *static_key = reply.key == call_name ? call_name : NULL;
        static const char *replay_names[256];
        const size_t route = (size_t)(((uintptr_t)reply.key >> 3) * UINT64_C(0x9e3779b97f4a7c15) >> 56);
        NvnFrontReplyKind window_reply = NVN_FRONT_NOT_OURS;
        if (reply.key && replay_names[route] == reply.key) {
            reply.kind = dispatch_nvn_replay(context, reply.name, static_key, scalar);
            if (reply.kind == NVN_FRONT_NOT_OURS)
                reply.kind = NVN_FRONT_BOUNDARY;
        } else if ((window_reply = nvn_window_dispatch(windows, reply.name, context->x)) != NVN_FRONT_NOT_OURS)
            reply.kind = window_reply;
        else {
            NvnFrontReplyKind frame_reply = nvn_frame_control(reply.name, context->x);
            if (frame_reply != NVN_FRONT_NOT_OURS)
                reply.kind = frame_reply;
            else {
                NvnFrontReplyKind replay_reply = dispatch_nvn_replay(context, reply.name, static_key, scalar);
                if (replay_reply != NVN_FRONT_NOT_OURS) {
                    reply.kind = replay_reply;
                    if (reply.key && !strncmp(reply.key, "nvnCommandBuffer", 16))
                        replay_names[route] = reply.key;
                } else {
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
        if (nvn_replay_boundary_reason)
            fprintf(probe_log, "NVN boundary reason: %s\n", nvn_replay_boundary_reason);
        nvn_replay_report_skips(probe_log);
        stop("NVN graphics call", address);
    }
    DISPATCH_KERNEL_RELEASE();
#undef DISPATCH_KERNEL_RELEASE
    rec_unlock();
    if (!record_only)
        gfx_unlock();
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
    const bool ok = hzn_hid_ps4_vibrate(native_pad, amplitude_low, amplitude_high);
    /* R96: rumble diagnostics (the first 40 changes, then every failure up to 40 more). */
    extern int hzn_hid_ps4_vibration_result;
    static unsigned logged, failures, nonzero;
    const bool felt = amplitude_low >= 0.005f || amplitude_high >= 0.005f;
    if (logged < 10 || (felt && nonzero++ < 20) || (!ok && failures < 40)) {
        ++logged;
        failures += !ok;
        fprintf(probe_log, "Rumble low=%.3f high=%.3f pad=%d result=%#x\n", amplitude_low, amplitude_high, native_pad,
                (uint32_t)hzn_hid_ps4_vibration_result);
        fflush(probe_log);
    }
    return ok;
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
    /* R96: a self-contained package (the Windows builder's folder, exFAT or
     * .ffpkg export) carries the game data at /app0/romfs; a title copied
     * without it reads the shared copy in /data/pokopia/romfs. */
    const char *root = access("/app0/romfs/LI_C0_R.IDX", R_OK) == 0 ? "/app0/romfs" : "/data/pokopia/romfs";
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

#if defined(__PROSPERO__)
/* R86: the software keyboard applet, shown as the port's own Switch-style
 * keyboard (gfx-bridge/osk.c). The game's thread waits here while the player
 * types; the global lock is released meanwhile. */
static void swkbd_title(const HznSwkbdRequest *request, char *out, size_t capacity)
{
    size_t n = 0;
    bool printable = request->header[0] != 0;
    for (size_t i = 0; request->header[i] && n + 1 < capacity; ++i) {
        uint16_t unit = request->header[i];
        /* The keyboard font is ASCII: typographic quotes become plain ones. */
        if (unit == 0x2018 || unit == 0x2019)
            unit = '\'';
        else if (unit == 0x201c || unit == 0x201d)
            unit = '"';
        else if (unit == 0x2026)
            unit = '.';
        if (unit < 32 || unit > 126) {
            printable = false;
            break;
        }
        out[n++] = (char)unit;
    }
    out[n] = 0;
    if (printable && n >= 3)
        return;
    /* The Switch 2 config layout differs: take its first readable UTF-16 text. */
    for (uint64_t at = 0; request->config && at + 12 <= request->config_size; at += 2) {
        size_t run = 0;
        while (at + (run + 1) * 2 <= request->config_size && run + 1 < capacity) {
            uint16_t unit;
            memcpy(&unit, request->config + at + run * 2, 2);
            if (unit < 32 || unit > 126)
                break;
            out[run++] = (char)unit;
        }
        if (run >= 6) {
            out[run] = 0;
            return;
        }
    }
    snprintf(out, capacity, "Enter text");
}

static void osk_present_behind(void)
{
    PokopiaVkTexture *texture = last_present.texture;
    if (!texture || !texture->backing.image || !texture->render_serial)
        return;
    const PokopiaLiveFrame frame = {
        .image = texture->backing.image, .format = texture->format, .extent = last_present.extent,
        .layout = texture->layout, .stages = texture->stages, .access = texture->access, .crop = last_present.crop,
        .render_serial = texture->render_serial, .completed_render_serial = graphics.render_serial,
    };
    VkResult shot = VK_NOT_READY;
    pokopia_live_present_frame(&live_output, &frame, UINT64_C(5000000000), &shot);
}

static bool run_applet_keyboard(void *opaque, const HznSwkbdRequest *request, uint16_t *text, size_t capacity)
{
    (void)opaque;
    char title[POKOPIA_OSK_TITLE + 1];
    swkbd_title(request, title, sizeof title);
    unsigned max = request->max_length && request->max_length <= 64 ? request->max_length : 16;
    if (max >= capacity)
        max = (unsigned)capacity - 1;
    const unsigned min = request->min_length <= max ? request->min_length : 0;
    fprintf(probe_log, "Software keyboard: \"%s\" max=%u min=%u config=%" PRIu64 " bytes\n", title, max, min,
            request->config_size);
    fflush(probe_log);
    if (request->config) {
        FILE *dump = fopen("/data/pokopia/debug/swkbd-config.bin", "wb");
        if (dump) {
            fwrite(request->config, 1, (size_t)request->config_size, dump);
            fclose(dump);
        }
    }

    PokopiaOsk osk;
    pokopia_osk_init(&osk, title, max, min);
    GuestBoundary *scope = pokopia_host_thread();
    const bool kernel = scope && scope->kernel_locked;
    if (kernel)
        kernel_unlock();

    uint8_t *pixels = NULL;
    uint32_t width = 0;
    bool bgra = false;
    const uint32_t height = live_output.present ? live_output.present->extent.height : 0;
    const uint32_t top = height * 360 / 2160, bottom = height * 1100 / 2160;
    gfx_lock();
    VkResult ready = height ? pokopia_live_present_keyboard_pixels(&live_output, top, bottom, &pixels, &width, &bgra)
                            : VK_ERROR_INITIALIZATION_FAILED;
    uint64_t now_ms = stat_now_ns() / 1000000;
    if (ready == VK_SUCCESS) {
        pokopia_osk_draw(&osk, pixels, width, top, bottom, bgra, now_ms);
        pokopia_live_present_keyboard_flush(&live_output);
        live_output.keyboard_visible = true;
    }
    gfx_unlock();

    PokopiaOskState state = POKOPIA_OSK_OPEN;
    if (ready != VK_SUCCESS) {
        fprintf(probe_log, "Software keyboard: panels unavailable (%d); entering \"Player\"\n", (int)ready);
        snprintf(osk.text, sizeof osk.text, "Player");
        osk.length = 6;
        state = POKOPIA_OSK_DONE;
    }
    atomic_store_explicit(&osk_active, true, memory_order_release);
    update_guest_pad();
    pthread_mutex_lock(&pad_mutex);
    uint64_t previous = osk_pad.buttons; /* buttons already down do not count */
    pthread_mutex_unlock(&pad_mutex);
    uint64_t blink = now_ms / 500, own_present_ns = 0;
    const struct timespec frame_time = {0, 16000000};
    while (state == POKOPIA_OSK_OPEN) {
        update_guest_pad();
        pthread_mutex_lock(&pad_mutex);
        const HznHidPad pad = osk_pad;
        pthread_mutex_unlock(&pad_mutex);
        now_ms = stat_now_ns() / 1000000;
        bool changed = false;
        state = pokopia_osk_input(&osk, pad.buttons, previous, pad.lx, pad.ly, now_ms, &changed);
        previous = pad.buttons;
        if (now_ms / 500 != blink) {
            blink = now_ms / 500;
            changed = true;
        }
        const uint64_t now_ns = stat_now_ns();
        const bool game_idle = now_ns - last_present.presented_ns > UINT64_C(60000000);
        if (changed || (game_idle && now_ns - own_present_ns > UINT64_C(33000000))) {
            gfx_lock();
            if (changed) {
                pokopia_osk_draw(&osk, pixels, width, top, bottom, bgra, now_ms);
                pokopia_live_present_keyboard_flush(&live_output);
            }
            if (game_idle) {
                osk_present_behind();
                own_present_ns = now_ns;
            }
            gfx_unlock();
        }
        nanosleep(&frame_time, NULL);
    }
    gfx_lock();
    live_output.keyboard_visible = false;
    if (stat_now_ns() - last_present.presented_ns > UINT64_C(60000000))
        osk_present_behind();
    gfx_unlock();
    /* The guest gets the pad back once the buttons that closed the keyboard are up. */
    for (unsigned i = 0; i < 120; ++i) {
        update_guest_pad();
        pthread_mutex_lock(&pad_mutex);
        const uint64_t held = osk_pad.buttons;
        pthread_mutex_unlock(&pad_mutex);
        if (!held)
            break;
        nanosleep(&frame_time, NULL);
    }
    atomic_store_explicit(&osk_active, false, memory_order_release);
    if (kernel)
        kernel_lock();

    size_t i = 0;
    for (; osk.text[i] && i + 1 < capacity; ++i)
        text[i] = (uint8_t)osk.text[i];
    text[i] = 0;
    fprintf(probe_log, "Software keyboard: %s \"%s\"\n", state == POKOPIA_OSK_DONE ? "entered" : "cancelled", osk.text);
    fflush(probe_log);
    return state == POKOPIA_OSK_DONE;
}
#endif

static bool signal_applet_event(void *opaque, uint32_t event, bool signaled)
{
    (void)opaque;
    return hzn_events_set_signaled(events, event, signaled);
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
#ifndef _WIN32
    STAT_ADD(stat_ipc[service && (unsigned)service->type < STAT_IPC - 1 ? (unsigned)service->type : STAT_IPC - 1], 1);
#endif

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
        if (reply.command == 214) { /* R96: SendVibrationValues2 diagnostics */
            extern unsigned hzn_hid_vibration_sends;
            extern uint32_t hzn_hid_vibration_last_handle;
            extern float hzn_hid_vibration_last_amplitude;
            static unsigned logged;
            if (logged < 30 || (logged < 60 && hzn_hid_vibration_last_amplitude > 0)) {
                ++logged;
                fprintf(probe_log, "Rumble send #%u handle=%#" PRIx32 " amplitude=%.3f devices=%zu",
                        hzn_hid_vibration_sends, hzn_hid_vibration_last_handle, hzn_hid_vibration_last_amplitude,
                        hid_state.vibration_device_count);
                for (size_t i = 0; i < hid_state.vibration_device_count; ++i)
                    fprintf(probe_log, " %#" PRIx32, hid_state.vibration_devices[i].handle);
                fputc('\n', probe_log);
                fflush(probe_log);
            }
        }
        update_guest_pad();
        context->x[0] = reply.result;
        return true;
    }
    if (service && service->type == SERVICE_FSP) {
        HznFsp *fsp = &service->state.shared->service.fsp;
        bool was_verified = fsp->romfs_verified;
#ifndef _WIN32
        const uint64_t fsp_start = stat_now_ns();
#endif
        HznFspReply reply = hzn_fsp_send_sync(fsp, tls, 0x100);
#ifndef _WIN32
        STAT_ADD(stat_fsp_calls, 1);
        STAT_ADD(stat_fsp_ns, stat_now_ns() - fsp_start);
#endif
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
        if (applet->noted_type) {
            fprintf(probe_log, "appletOE: answered unknown command %" PRIu32 " on %s with success\n",
                    applet->noted_command, applet->noted_type);
            applet->noted_type = NULL;
        }
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
            { /* R85: the request words, for working out a new command */
                uint32_t words[24];
                memcpy(words, tls, sizeof words);
                fputs("appletOE: request", probe_log);
                for (unsigned i = 0; i < 24; ++i)
                    fprintf(probe_log, " %08" PRIx32, words[i]);
                fputc('\n', probe_log);
            }
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
        if (reply.kind == HZN_TIME_BOUNDARY) {
            fprintf(probe_log, "time:u unsupported %s command %" PRIu32 " object=%#" PRIx32 "\n",
                    reply.object_type ? reply.object_type : "?", reply.command, reply.object_id);
            stop("unsupported clock service command", reply.command);
        }
        context->x[0] = reply.result;
        fprintf(probe_log, "time:u: HIPC request %#" PRIx32 " SVC result=%#" PRIx32 "\n",
                request_type, reply.result);
        fflush(probe_log);
        return true;
    }
    if (service && service->type == SERVICE_PREPO) {
        /* R74: prepo:u (play reports) is accepted and every report dropped.
         * The game opens it after character creation. */
        HznCmifRequest request;
        uint32_t command = UINT32_MAX;
        const bool domain = service->state.ectx_domain && (request_type == 4 || request_type == 6);
        const size_t offset = domain ? 16 : 0;
        const bool parsed = hzn_cmif_parse(tls, 0x100, &request);
        if (parsed && request.raw_size >= offset + 16 && hzn_cmif_read32(request.raw + offset) == HZN_SFCI)
            command = hzn_cmif_read32(request.raw + offset + 8);
        if (request_type == 5 || request_type == 7) {
            if (command != 0 && command != 3)
                stop("unsupported play-report control request", command);
            uint32_t data = command == 0 ? 1 : 0x8000;
            if (!hzn_cmif_reply(tls, 0x100, false, 0, &data))
                stop("service control reply failed", command);
            if (command == 0)
                service->state.ectx_domain = true;
            context->x[0] = 0;
            return true;
        }
        if (!parsed || !hzn_cmif_reply(tls, 0x100, domain, 0, NULL))
            stop("play-report reply failed", command);
        static uint32_t seen[16];
        static unsigned seen_count;
        bool first = seen_count < 16;
        for (unsigned i = 0; i < seen_count && first; ++i)
            first = seen[i] != command;
        if (first) {
            seen[seen_count++] = command;
            fprintf(probe_log, "prepo:u: command %u accepted and dropped\n", command);
            fflush(probe_log);
        }
        context->x[0] = 0;
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
        } else if (strcmp(reply.service, "prepo:u") == 0) {
            type = SERVICE_PREPO;
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
                slot->state.applet.signal_event = signal_applet_event;
#if defined(__PROSPERO__)
                slot->state.applet.keyboard = run_applet_keyboard;
#endif
                slot->state.applet.receive_message = receive_applet_message;
                slot->state.applet.write_guest = write_guest;
                slot->state.applet.read_guest = read_guest;
#ifdef POKOPIA_GFX
                slot->state.applet.ensure_save = ensure_applet_save;
                slot->state.applet.get_save_size = get_applet_save_size;
                /* R49: GetSaveDataSizeMax (IApplicationFunctions 28) reports the
                 * same quotas the account save is created with. R48 stopped
                 * here right after loading finished. */
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

static void guest_svc_inner(Ctx *context, uint32_t number);

void guest_svc(Ctx *context, uint32_t number)
{
#ifndef _WIN32
    /* Time includes blocking waits, so WaitSynchronization / ArbitrateLock
     * show how long guest threads sit idle versus work inside HLE. */
    const uint64_t start = stat_now_ns();
    if (!stat_thread) {
        const unsigned index = atomic_fetch_add_explicit(&stat_thread_count, 1, memory_order_relaxed);
        if (index < STAT_THREADS) {
            stat_thread = &stat_threads[index];
            atomic_store(&stat_thread->id, hzn_thread_id(threads, context, 0xffff8000u));
#if defined(__PROSPERO__)
            stat_thread->host = pthread_self();
            stat_thread_stack_bounds(stat_thread); /* R96 */
            atomic_store_explicit(&stat_thread->ctx, context, memory_order_release);
            pthread_once(&prof_once, prof_start);
#endif
        }
    } else if (stat_thread_left_svc) {
        STAT_ADD(stat_thread->busy_ns, start - stat_thread_left_svc);
    }
#if defined(__PROSPERO__)
    if (stat_thread)
        atomic_store_explicit(&stat_thread->in_svc, (int)(number < STAT_SVCS ? number : STAT_SVCS - 1) + 1,
                              memory_order_relaxed);
#endif
#if defined(__PROSPERO__) && defined(POKOPIA_GFX)
    const unsigned phase = number == 0x0b || number == 0x1c || number == 0x1d ? frame_trace_active() : 0;
    const uint64_t trace_tid = phase ? pokopia_trace_guest_tid() : 0;
    const bool trace_sleep = number == 0x0b && trace_tid == 1;
    const bool trace_render_wait = number == 0x1c && trace_tid == 24;
    const bool trace_worker_wait = number == 0x1c && trace_tid >= 4 && trace_tid <= 8;
    const uint64_t sleep_request = trace_sleep ? context->x[0] : 0;
    /* Keep the worker capture active after the short frame-trace buffer fills. */
    const bool profile_sleep = number == 0x0b && context->x[0] == UINT64_C(1000000) &&
        (pokopia_debug_flags & POKOPIA_DEBUG_FRAME_TRACE) &&
        !atomic_load_explicit(&wait_work_finished, memory_order_relaxed) && pokopia_trace_guest_tid() == 1;
    const uint64_t sleep_caller = trace_sleep || profile_sleep ? trace_sleep_caller(context) : 0;
    const bool profile_gate = profile_sleep && sleep_caller == UINT64_C(0x71008832b0);
    const uint64_t wait_key = trace_render_wait || trace_worker_wait ? context->x[1] : 0;
    if (trace_render_wait)
        atomic_store_explicit(&trace_render_wait_key, wait_key, memory_order_release);
    const uint64_t signal_key = phase && number == 0x1d && context->x[0] &&
        context->x[0] == atomic_load_explicit(&trace_render_wait_key, memory_order_acquire) ? context->x[0] : 0;
    const uint64_t caller = trace_sleep || trace_render_wait || signal_key ? context->x[30] : 0;
    const uint64_t sleep_fn = trace_sleep ? context->fn : 0;
    if (profile_gate)
        atomic_store_explicit(&wait_work_gate, start, memory_order_release);
#endif
    guest_svc_inner(context, number);
#if defined(__PROSPERO__) && defined(POKOPIA_GFX)
    if (profile_gate)
        atomic_store_explicit(&wait_work_gate, 0, memory_order_release);
#endif
#if defined(__PROSPERO__)
    if (stat_thread)
        atomic_store_explicit(&stat_thread->in_svc, 0, memory_order_relaxed);
#endif
    const uint64_t end = stat_now_ns();
#if defined(__PROSPERO__) && defined(POKOPIA_GFX)
    if (trace_render_wait)
        atomic_store_explicit(&trace_render_wait_key, 0, memory_order_release);
    if (phase && phase == frame_trace_active()) {
        if (trace_sleep) {
            frame_trace("sleep", start, sleep_request, end - start);
            frame_trace("sleep-site", start, sleep_caller, sleep_request);
            if ((sleep_caller >> 32) != 0x71)
                frame_trace("sleep-raw", start, caller, sleep_fn);
        }
        if (trace_render_wait || (trace_worker_wait && end - start >= UINT64_C(300000)))
            frame_trace("cond-wait", start, wait_key, end - start);
        if (trace_render_wait)
            frame_trace("cond-site", start, wait_key, caller);
        if (signal_key && context->x[0] == 0)
            frame_trace("cond-signal", start, signal_key, caller);
    }
#endif
    const unsigned slot = number < STAT_SVCS ? number : STAT_SVCS - 1;
    STAT_ADD(stat_svc_calls[slot], 1);
    STAT_ADD(stat_svc_ns[slot], end - start);
    if (stat_thread) {
        STAT_ADD(stat_thread->svc_by_ns[slot], end - start);
        STAT_ADD(stat_thread->svc_ns, end - start);
        STAT_ADD(stat_thread->svcs, 1);
        stat_thread_left_svc = end;
    }
#else
    guest_svc_inner(context, number);
#endif
}

static void guest_svc_inner(Ctx *context, uint32_t number)
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
                heap_alias_snapshot(src, bytes); /* R80: before the source is protected */
                result = isolated_source ? hzn_heap_stack_alias_map_isolated(&heap_aliases, dst, src, bytes) :
                                           hzn_heap_stack_alias_map(&heap_aliases, dst, src, bytes);
                host_error = heap_aliases.last_host_error;
            }
        }
        sync_layout_changed();
        heap_alias_snapshot(0, 0);
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

#if defined(__PROSPERO__) && defined(POKOPIA_BOOT_PROBE)
/* Native libc for hot imports. The PS5 profile put 43% of a 174 s loading
 * thread in the SDK's lifted strlen/strncmp, called through the main
 * module's PLT. Guest pointers are host pointers, so the host routines can
 * run on the AArch64 argument registers directly. The cache also skips the
 * per-call name lookup for every other import. */
enum {
    FAST_RESOLVED = 1, FAST_STRLEN, FAST_STRNLEN, FAST_STRCMP, FAST_STRNCMP, FAST_MEMCMP,
    FAST_MEMCPY, FAST_MEMMOVE, FAST_MEMSET, FAST_STRCHR, FAST_KINDS
};
enum { IMPORT_CACHE_SLOTS = 1 << 15 };
static const char *const fast_names[FAST_KINDS] = {
    [FAST_STRLEN] = "strlen", [FAST_STRNLEN] = "strnlen", [FAST_STRCMP] = "strcmp",
    [FAST_STRNCMP] = "strncmp", [FAST_MEMCMP] = "memcmp", [FAST_MEMCPY] = "memcpy",
    [FAST_MEMMOVE] = "memmove", [FAST_MEMSET] = "memset", [FAST_STRCHR] = "strchr",
};
typedef struct {
    atomic_uint_fast32_t key; /* import id + 1; 0 = empty */
    atomic_uint_fast64_t value; /* guest address | kind << 56; 0 = being filled */
} ImportCacheSlot;
static ImportCacheSlot import_cache[IMPORT_CACHE_SLOTS];
static atomic_uint_fast64_t fast_calls[FAST_KINDS];

static uint64_t import_cache_get(uint32_t id)
{
    const uint32_t key = id + 1;
    for (uint32_t probe = 0, slot = (key * 2654435761u) & (IMPORT_CACHE_SLOTS - 1); probe < IMPORT_CACHE_SLOTS;
         ++probe, slot = (slot + 1) & (IMPORT_CACHE_SLOTS - 1)) {
        const uint32_t seen = (uint32_t)atomic_load_explicit(&import_cache[slot].key, memory_order_acquire);
        if (seen == key)
            return atomic_load_explicit(&import_cache[slot].value, memory_order_acquire);
        if (!seen)
            return 0;
    }
    return 0;
}

static void import_cache_put(uint32_t id, uint64_t value)
{
    const uint32_t key = id + 1;
    for (uint32_t probe = 0, slot = (key * 2654435761u) & (IMPORT_CACHE_SLOTS - 1); probe < IMPORT_CACHE_SLOTS;
         ++probe, slot = (slot + 1) & (IMPORT_CACHE_SLOTS - 1)) {
        uint_fast32_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(&import_cache[slot].key, &expected, key, memory_order_acq_rel,
                                                    memory_order_acquire) || expected == key) {
            atomic_store_explicit(&import_cache[slot].value, value, memory_order_release);
            return;
        }
    }
}

static uint64_t import_classify(uint64_t address, const char *name)
{
    unsigned kind = FAST_RESOLVED;
    for (unsigned k = FAST_STRLEN; k < FAST_KINDS; ++k)
        if (name && !strcmp(name, fast_names[k]))
            kind = k;
    return address | ((uint64_t)kind << 56);
}

static bool import_fast(Ctx *context, unsigned kind)
{
    uint64_t *x = context->x;
    void *a = (void *)(uintptr_t)x[0];
    const void *b = (const void *)(uintptr_t)x[1];
    switch (kind) {
    case FAST_STRLEN: x[0] = strlen(a); break;
    case FAST_STRNLEN: x[0] = strnlen(a, (size_t)x[1]); break;
    case FAST_STRCMP: x[0] = (uint64_t)(int64_t)strcmp(a, b); break;
    case FAST_STRNCMP: x[0] = (uint64_t)(int64_t)strncmp(a, b, (size_t)x[2]); break;
    case FAST_MEMCMP: x[0] = (uint64_t)(int64_t)memcmp(a, b, (size_t)x[2]); break;
    case FAST_MEMCPY: /* AArch64 memcpy tolerates overlap in practice; keep that. */
    case FAST_MEMMOVE: memmove(a, b, (size_t)x[2]); break;
    case FAST_MEMSET: memset(a, (int)(uint8_t)x[1], (size_t)x[2]); break;
    case FAST_STRCHR: {
        const char *hit = strchr(a, (int)(char)x[1]);
        x[0] = (uint64_t)(uintptr_t)hit;
        break;
    }
    default: return false;
    }
    atomic_fetch_add_explicit(&fast_calls[kind], 1, memory_order_relaxed);
    return true;
}

static void prof_report_fast(FILE *log)
{
    fputs("fast imports", log);
    for (unsigned k = FAST_STRLEN; k < FAST_KINDS; ++k)
        fprintf(log, " %s=%" PRIu64, fast_names[k],
                (uint64_t)atomic_exchange_explicit(&fast_calls[k], 0, memory_order_relaxed));
    fputc('\n', log);
}
#endif

void host_import(Ctx *context, uint32_t id)
{
#if defined(__PROSPERO__) && defined(POKOPIA_BOOT_PROBE)
    const uint64_t cached = id != NX_IMPORT_ID(3, 1) ? import_cache_get(id) : 0;
    if (cached) {
        const unsigned kind = (unsigned)(cached >> 56);
        if (kind > FAST_RESOLVED && import_fast(context, kind))
            return;
        call_indirect(context, cached & ((UINT64_C(1) << 56) - 1));
        return;
    }
#endif
    if (id != NX_IMPORT_ID(3, 1) || context->x[30] != rtld_base + 0x8c) {
#ifdef POKOPIA_BOOT_PROBE
        const char *name;
        uint64_t address = pokopia_boot_import(id, &name);
        if (address) {
#if defined(__PROSPERO__)
            const uint64_t value = import_classify(address, name);
            if (id != NX_IMPORT_ID(3, 1))
                import_cache_put(id, value);
            if ((unsigned)(value >> 56) > FAST_RESOLVED && import_fast(context, (unsigned)(value >> 56)))
                return;
#endif
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

#ifndef _WIN32
/* R49: closing the game used to leave the PS4 waiting on an error. A fatal
 * signal with the default action makes the system write a crash dump of a
 * multi-GiB process. Log the signal instead and exit at once. The sparse
 * heap installs its fault handler later and chains here for faults it
 * cannot recover, so its page-in path is unchanged. */
static void fatal_signal(int signal, siginfo_t *info, void *context)
{
    if (memscan_guarded && (signal == SIGSEGV || signal == SIGBUS)) {
        memscan_guarded = 0;
        longjmp(memscan_jump, 1); /* SA_NODEFER: SIGSEGV stays unblocked */
    }
    (void)context;
    char line[160];
    const bool close_request = signal == SIGTERM || signal == SIGINT || signal == SIGHUP;
    const int bytes = snprintf(line, sizeof line, "%s signal=%d code=%d address=%#" PRIx64 " - exiting without a crash dump\n",
        close_request ? "Close requested" : "Fatal", signal, info ? info->si_code : 0,
        info ? (uint64_t)(uintptr_t)info->si_addr : 0);
    if (probe_log && bytes > 0) {
        const int fd = fileno(probe_log);
        if (fd >= 0) {
            (void)!write(fd, line, (size_t)bytes < sizeof line ? (size_t)bytes : sizeof line - 1);
#if defined(__PROSPERO__)
            /* Host registers plus return-address candidates from the stack.
             * fatal_signal's runtime address gives the image slide, so these
             * map to symbols with llvm-symbolizer on link/pokopia.elf. */
            if (!close_request && context) {
                const ucontext_t *uc = context;
                const uint64_t rip = (uint64_t)uc->uc_mcontext.mc_rip;
                const uint64_t rsp = (uint64_t)uc->uc_mcontext.mc_rsp;
                const uint64_t anchor = (uint64_t)(uintptr_t)&fatal_signal;
                char detail[256];
                int n = snprintf(detail, sizeof detail,
                                 "  host rip=%#" PRIx64 " rsp=%#" PRIx64 " rbp=%#" PRIx64 " rdi=%#" PRIx64
                                 " rsi=%#" PRIx64 " fatal_signal=%#" PRIx64 "\n",
                                 rip, rsp, (uint64_t)uc->uc_mcontext.mc_rbp, (uint64_t)uc->uc_mcontext.mc_rdi,
                                 (uint64_t)uc->uc_mcontext.mc_rsi, anchor);
                if (n > 0)
                    (void)!write(fd, detail, (size_t)n < sizeof detail ? (size_t)n : sizeof detail - 1);
                /* The eboot is ~300 MiB; treat anything within 512 MiB of the
                 * anchor as a code address. */
                const uint64_t lo = anchor > (UINT64_C(512) << 20) ? anchor - (UINT64_C(512) << 20) : 0;
                const uint64_t hi = anchor + (UINT64_C(512) << 20);
                const uint64_t *stack = (const uint64_t *)(uintptr_t)rsp;
                int found = 0;
                for (int i = 0; rsp && i < 256 && found < 24; ++i) {
                    const uint64_t word = stack[i];
                    if (word < lo || word >= hi)
                        continue;
                    n = snprintf(detail, sizeof detail, "  stack[%d]=%#" PRIx64 "\n", i, word);
                    if (n > 0)
                        (void)!write(fd, detail, (size_t)n);
                    ++found;
                }
            }
#endif
            fsync(fd);
        }
    }
    _Exit(close_request ? 0 : 128 + signal);
}

static void install_fatal_handlers(void)
{
    struct sigaction action = {0};
#if defined(__PROSPERO__)
    action.sa_sigaction = (void (*)(int, struct __siginfo *, void *))fatal_signal;
#else
    /* OpenOrbis' sa_sigaction macro names a member its union lacks. */
    action.__sa_handler.__sa_sigaction = (void (*)(int, struct __siginfo *, void *))fatal_signal;
#endif
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER; /* R96: lets the memory scanner longjmp out */
    sigemptyset(&action.sa_mask);
    static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTERM, SIGINT, SIGHUP };
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; ++i)
        sigaction(signals[i], &action, NULL);
}
#endif

#if defined(POKOPIA_GFX) && !defined(_WIN32) && !defined(__PROSPERO__)
/* R51: the PS4 suspends an app before closing it, and a GPU process must
 * have called sceGnmSubmitDone() for that suspend to complete. Mesa never
 * calls it, so every close failed with CPU_FAULT_SUBMITDONE_TIMEOUT_IN_
 * SUSPEND_ASYNC (CE-34878-0, kernel log "submitDone() is not called").
 * A heartbeat covers frames, the long loading stalls and the idle loop after
 * a STOP. The host lock keeps it off Mesa's submission path. */
extern int32_t sceGnmSubmitDone(void);
static void *submit_done_main(void *unused)
{
    (void)unused;
    static const struct timespec pause = { .tv_sec = 0, .tv_nsec = 100L * 1000L * 1000L };
    for (;;) {
        nanosleep(&pause, NULL);
        if (!nvn)
            continue; /* the GPU is not open yet */
        pokopia_host_lock();
        sceGnmSubmitDone();
        pokopia_host_unlock();
    }
    return NULL;
}

static void start_submit_done_heartbeat(void)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, submit_done_main, NULL) == 0)
        pthread_detach(thread);
    else
        fputs("Could not start the sceGnmSubmitDone heartbeat; closing will report CE-34878-0\n", probe_log);
}
#endif

int pokopia_guest_probe(FILE *log)
{
    probe_log = log ? log : stdout;
#ifndef _WIN32
    install_fatal_handlers();
#endif
#if defined(POKOPIA_GFX) && !defined(_WIN32) && !defined(__PROSPERO__)
    /* PS4 only: there is no Gnm on the PS5; RADV submits through AGC. */
    start_submit_done_heartbeat();
#endif
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
#if defined(__PROSPERO__) && defined(POKOPIA_GFX)
        .use_precise_main_sleep = use_precise_main_sleep,
        .precise_sleep_clock = stat_now_ns,
#endif
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
    {
        extern FILE *hzn_fsp_error_log, *nvn_replay_error_log;
        hzn_fsp_error_log = probe_log;
        nvn_replay_error_log = probe_log;
    }
    const NvnReplayConfig replay_config = {
        .front = nvn, .opaque = &graphics, .read_guest = read_guest, .write_guest = write_guest,
        .copy = execute_nvn_copy, .clear = execute_nvn_clear,
        .clear_depth_stencil = execute_nvn_clear_depth_stencil, .draw = execute_nvn_draw,
        .draw_generic = execute_nvn_draw_generic, .compute_generic = execute_nvn_compute_generic,
        .compute = execute_nvn_compute, .sync = execute_nvn_sync, .finish = finish_nvn_queue,
        .copy_texture = execute_nvn_copy_texture, .copy_texture_to_buffer = execute_nvn_copy_to_buffer,
        .timestamp_ns = pokopia_boot_monotonic_ns,
        .execute_begin = replay_execute_begin, .execute_end = replay_execute_end, .execute_yield = replay_execute_yield,
        .enqueue = replay_enqueue_job, .sync_queued = replay_sync_queued,
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


