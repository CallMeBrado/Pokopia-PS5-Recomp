/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "thread.h"
#include "event.h"

#include <assert.h>
#include <nxrecomp/ctx.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { MAIN_HANDLE = 0x10000, WORKER_BASE = 0x20000, ENTRY = 0x8000, STACK_TOP = 0x10000 };
static const uint32_t self_handle = 0xffff8000u;

typedef struct {
    HznThreads *threads;
    HznEvents *events;
    Ctx main;
    atomic_uint stage;
    atomic_bool let_exit;
    unsigned allocations, releases;
    bool deny_tls;
} Probe;

static bool copy_wait(void *opaque, uint64_t address, uint32_t *out, size_t count)
{
    (void)opaque;
    if (!address || count > 64)
        return false;
    memcpy(out, (const void *)(uintptr_t)address, count * sizeof *out);
    return true;
}

static void wait_guest(Probe *probe, uint64_t id, const uint32_t *handles, uint32_t count,
                       int64_t timeout, uint32_t result, uint32_t index)
{
    uint64_t x[32] = {0, (uintptr_t)handles, count, (uint64_t)timeout};
    assert(hzn_event_svc(probe->events, 0x18, x, id));
    assert(x[0] == result && x[1] == index);
}

static uint32_t validate(void *opaque, uint64_t entry, uint64_t stack)
{
    (void)opaque;
    return entry == ENTRY && stack == STACK_TOP ? 0 : 0xcc01;
}

static bool allocate_tls(void *opaque, uint64_t *tls, uint64_t *tcb)
{
    Probe *probe = opaque;
    if (probe->deny_tls)
        return false;
    uint8_t *pages = calloc(1, 0x2000);
    assert(pages);
    pages[0x1010] = 0xa5;
    *tls = (uintptr_t)pages;
    *tcb = (uintptr_t)(pages + 0x1000);
    ++probe->allocations;
    return true;
}

static void release_tls(void *opaque, uint64_t tls, uint64_t tcb)
{
    Probe *probe = opaque;
    assert(tcb == tls + 0x1000);
    free((void *)(uintptr_t)tls);
    ++probe->releases;
}

static void sleep_ns(Probe *probe, Ctx *ctx, uint64_t ns)
{
    ctx->x[0] = ns;
    assert(hzn_thread_svc(probe->threads, ctx, 0x0b));
    assert(ctx->x[0] == ns);
}

static void get_affinity(Probe *probe, Ctx *ctx, uint32_t handle, int32_t ideal, uint64_t mask, uint32_t result)
{
    ctx->x[2] = handle;
    assert(hzn_thread_svc(probe->threads, ctx, 0x0e));
    assert(ctx->x[0] == result && ctx->x[1] == (uint32_t)ideal && ctx->x[2] == mask);
}

static void set_affinity(Probe *probe, uint32_t handle, int32_t ideal, uint64_t mask, uint32_t result)
{
    Ctx *ctx = &probe->main;
    ctx->x[0] = handle;
    ctx->x[1] = (uint32_t)ideal;
    ctx->x[2] = mask;
    assert(hzn_thread_svc(probe->threads, ctx, 0x0f) && ctx->x[0] == result);
}

static void dispatch(void *opaque, Ctx *ctx, uint64_t entry)
{
    Probe *probe = opaque;
    uint64_t id = ctx->x[0];
    assert(ctx != &probe->main && entry == ENTRY && ctx->x[31] == STACK_TOP);
    assert(ctx->ex_addr == UINT64_MAX && ctx->tpidr != ctx->tpidrro);
    uint64_t linked_tcb;
    memcpy(&linked_tcb, (void *)(uintptr_t)(ctx->tpidrro + 0x1f0), sizeof linked_tcb);
    assert(linked_tcb == ctx->tpidr);
    assert(*(const uint8_t *)(uintptr_t)(ctx->tpidr + 0x10) == 0xa5);

    ctx->x[1] = self_handle;
    assert(hzn_thread_svc(probe->threads, ctx, 0x25) && ctx->x[0] == 0 && ctx->x[1] == id);
    if (id == 2) {
        atomic_store(&probe->stage, 1);
        while (!atomic_load(&probe->let_exit))
            sleep_ns(probe, ctx, UINT64_MAX);
        ctx->x[1] = self_handle;
        assert(hzn_thread_svc(probe->threads, ctx, 0x25) && ctx->x[0] == 0 && ctx->x[1] == id);
        get_affinity(probe, ctx, self_handle, 5, 0x30, 0);
        wait_guest(probe, id, &self_handle, 1, 0, 0xea01, UINT32_MAX);
        atomic_store(&probe->stage, 2);
        hzn_thread_svc(probe->threads, ctx, 0x0a);
    } else if (id == 3) {
        atomic_store(&probe->stage, 3);
        sleep_ns(probe, ctx, UINT64_C(60000000000));
    } else if (id == 4) {
        atomic_store(&probe->stage, 4);
        return;
    }
    abort();
}

static void wait_stage(Probe *probe, unsigned stage)
{
    for (unsigned i = 0; i < 1000 && atomic_load(&probe->stage) < stage; ++i)
        sleep_ns(probe, &probe->main, 1000000);
    assert(atomic_load(&probe->stage) == stage);
}

static uint32_t create(Probe *probe, uint64_t entry, uint64_t stack, uint32_t priority, uint32_t core, uint32_t result)
{
    Ctx *ctx = &probe->main;
    ctx->x[1] = entry;
    ctx->x[2] = probe->allocations + 2;
    ctx->x[3] = stack;
    ctx->x[4] = priority;
    ctx->x[5] = core;
    assert(hzn_thread_svc(probe->threads, ctx, 8) && ctx->x[0] == result);
    return (uint32_t)ctx->x[1];
}

static void handle_call(Probe *probe, uint32_t number, uint32_t handle, uint32_t result)
{
    Ctx *ctx = &probe->main;
    ctx->x[0] = handle;
    assert(hzn_thread_svc(probe->threads, ctx, number) && ctx->x[0] == result);
}

int main(void)
{
    Probe probe = {0};
    const HznEventConfig event_config = {.copy_handles = copy_wait};
    probe.events = hzn_events_create(&event_config);
    assert(probe.events);
    HznThreadConfig config = {
        .opaque = &probe, .main_context = &probe.main, .main_handle = MAIN_HANDLE,
        .worker_handle_base = WORKER_BASE, .main_id = 1, .main_priority = 44,
        .worker_handle_limit = WORKER_BASE + 3,
        .process_core_mask = 0x3f, .main_core = 2,
        .process_priority_mask = UINT64_C(0xffffffff0000000),
        .validate_start = validate, .allocate_tls = allocate_tls, .release_tls = release_tls,
        .dispatch = dispatch, .events = probe.events,
    };
    probe.threads = hzn_threads_create(&config);
    assert(probe.threads);
    assert(!hzn_threads_create(&config));
    const uint32_t main_handle = MAIN_HANDLE;
    wait_guest(&probe, 1, &main_handle, 1, 0, 0xea01, UINT32_MAX);
    create(&probe, ENTRY, STACK_TOP, 27, 0, 0xe001);
    create(&probe, ENTRY, STACK_TOP, 60, 0, 0xe001);
    create(&probe, ENTRY, STACK_TOP, UINT32_MAX, 0, 0xe001);
    create(&probe, ENTRY, STACK_TOP, 64, 0, 0xe001);
    create(&probe, ENTRY, STACK_TOP, 48, 6, 0xe201);
    create(&probe, ENTRY, STACK_TOP, 48, 8, 0xe201);
    create(&probe, ENTRY, STACK_TOP - 1, 48, 0, 0xcc01);
    create(&probe, ENTRY + 4, STACK_TOP, 48, 0, 0xcc01);
    probe.deny_tls = true;
    create(&probe, ENTRY, STACK_TOP, 48, 0, 0xce01);
    probe.deny_tls = false;
    assert(probe.allocations == 0);

    uint32_t first = create(&probe, ENTRY, STACK_TOP, 48, 0, 0);
    assert(first == WORKER_BASE && atomic_load(&probe.stage) == 0);
    wait_guest(&probe, 1, &first, 1, 0, 0xea01, UINT32_MAX);
    wait_guest(&probe, 1, &first, 1, 1000000, 0xea01, UINT32_MAX);
    uint32_t reader = hzn_events_copy_read(probe.events, 0);
    const uint32_t mixed[2] = {first, reader};
    assert(reader && hzn_events_set_signaled(probe.events, 0, true));
    wait_guest(&probe, 1, mixed, 2, 0, 0, 1);
    assert(hzn_events_set_signaled(probe.events, 0, false));
    assert(!hzn_events_cancel_wait(probe.events, 1));
    wait_guest(&probe, 1, mixed, 2, -1, 0xec01, UINT32_MAX);
    get_affinity(&probe, &probe.main, self_handle, 2, 4, 0);
    get_affinity(&probe, &probe.main, first, 0, 1, 0);
    set_affinity(&probe, first, -1, 0x30, 0);
    get_affinity(&probe, &probe.main, first, -1, 0x30, 0);
    set_affinity(&probe, first, -3, 0x30, 0xe801);
    set_affinity(&probe, first, -2, UINT64_MAX, 0);
    get_affinity(&probe, &probe.main, first, 2, 4, 0);
    set_affinity(&probe, first, 5, 0x20, 0);
    set_affinity(&probe, first, -3, 0x30, 0);
    set_affinity(&probe, first, -3, 0x10, 0xe801);
    set_affinity(&probe, first, 5, 0, 0xe801);
    set_affinity(&probe, first, 5, 0x10, 0xe801);
    set_affinity(&probe, first, 5, 0xa0, 0xe201);
    set_affinity(&probe, first, 8, 0x20, 0xe201);
    set_affinity(&probe, first, -4, 0x20, 0xe201);
    get_affinity(&probe, &probe.main, first, 5, 0x30, 0);
    probe.main.x[1] = first;
    assert(hzn_thread_svc(probe.threads, &probe.main, 0x0c) && probe.main.x[1] == 48);
    handle_call(&probe, 9, first, 0);
    wait_stage(&probe, 1);
    handle_call(&probe, 9, first, 0xfa01);
    wait_guest(&probe, 1, &first, 1, 0, 0xea01, UINT32_MAX);
    handle_call(&probe, 0x16, first, 0);
    handle_call(&probe, 0x16, first, 0xe401);
    assert(probe.releases == 0);
    wait_guest(&probe, 1, &first, 1, 0, 0xe401, UINT32_MAX);
    get_affinity(&probe, &probe.main, first, 0, 0, 0xe401);
    set_affinity(&probe, first, -2, 0, 0xe401);
    probe.main.x[1] = first;
    assert(hzn_thread_svc(probe.threads, &probe.main, 0x25));
    assert(probe.main.x[0] == 0xe401 && probe.main.x[1] == 0);
    atomic_store(&probe.let_exit, true);
    wait_stage(&probe, 2);

    uint32_t second = create(&probe, ENTRY, STACK_TOP, 48, 0xfffffffeu, 0);
    assert(second == first + 1);
    get_affinity(&probe, &probe.main, second, 2, 4, 0);
    handle_call(&probe, 9, second, 0);
    wait_stage(&probe, 3);
    uint32_t last = create(&probe, ENTRY, STACK_TOP, 48, 0, 0);
    uint64_t thread_wait[32] = {last};
    assert(hzn_event_svc(probe.events, 0x11, thread_wait, 0) && thread_wait[0] == 0xe401);
    handle_call(&probe, 9, last, 0);
    const uint32_t mixed_final[] = {reader, last};
    wait_guest(&probe, 1, mixed_final, 2, 5000000000, 0, 1);
    assert(atomic_load(&probe.stage) == 4);
    wait_guest(&probe, 1, &last, 1, 0, 0, 0);
    thread_wait[0] = last;
    assert(hzn_event_svc(probe.events, 0x17, thread_wait, 0) && thread_wait[0] == 0xe401);
    wait_guest(&probe, 1, &last, 1, 0, 0, 0);
    handle_call(&probe, 0x16, last, 0);
    handle_call(&probe, 9, last, 0xe401);
    wait_guest(&probe, 1, &last, 1, 0, 0xe401, UINT32_MAX);
    create(&probe, ENTRY, STACK_TOP, 48, 0, 0xce01);
    probe.main.x[0] = 0x30000;
    assert(!hzn_thread_svc(probe.threads, &probe.main, 0x16));
    handle_call(&probe, 0x16, MAIN_HANDLE, 0);
    probe.main.x[1] = self_handle;
    assert(hzn_thread_svc(probe.threads, &probe.main, 0x25));
    assert(probe.main.x[0] == 0 && probe.main.x[1] == 1);
    wait_guest(&probe, 1, &main_handle, 1, 0, 0xe401, UINT32_MAX);
    wait_guest(&probe, 1, &self_handle, 1, 0, 0xea01, UINT32_MAX);
    assert(hzn_threads_destroy(probe.threads));
    assert(probe.allocations == 3 && probe.releases == 3);

    config.main_priority = 27;
    assert(!hzn_threads_create(&config));
    config.process_priority_mask = 0;
    config.process_core_mask = 0;
    probe.threads = hzn_threads_create(&config);
    assert(probe.threads);
    uint32_t default_priority = create(&probe, ENTRY, STACK_TOP, 63, 0, 0);
    handle_call(&probe, 0x16, default_priority, 0);
    set_affinity(&probe, self_handle, -1, 8, 0xe201);
    assert(hzn_threads_destroy(probe.threads));
    config.process_core_mask = 1;
    assert(!hzn_threads_create(&config));

    hzn_events_destroy(probe.events);
    puts("PASS Horizon threads: lifecycle, mixed event waits, persistent exit, cancellation and timeout");
    return 0;
}
