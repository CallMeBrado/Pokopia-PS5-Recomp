/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "event.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

enum { GUEST_HANDLES = 0x1000, INVALID_HANDLE = 0xe401, TIMED_OUT = 0xea01, CANCELLED = 0xec01 };

typedef struct {
    HznEvents *events;
    uint32_t handles[64];
    bool signal_in_copy;
} Probe;

static bool copy_handles(void *opaque, uint64_t guest_addr, uint32_t *out, size_t count)
{
    Probe *probe = opaque;
    if (guest_addr != GUEST_HANDLES || count > 64)
        return false;
    if (probe->signal_in_copy)
        assert(hzn_events_set_signaled(probe->events, 0, true));
    memcpy(out, probe->handles, count * sizeof *out);
    return true;
}

static void svc(Probe *probe, uint32_t number, uint32_t handle, uint32_t result)
{
    uint64_t x[32] = { handle };
    assert(hzn_event_svc(probe->events, number, x, 0));
    assert(x[0] == result);
}

static void wait(Probe *probe, uint64_t id, uint64_t guest_addr, uint32_t count,
                 int64_t ns, uint32_t result, uint64_t index)
{
    uint64_t x[32] = { 0, guest_addr, count, (uint64_t)ns };
    assert(hzn_event_svc(probe->events, 0x18, x, id));
    assert(x[0] == result && x[1] == index);
}

typedef struct {
    Probe *probe;
    uint64_t id;
    uint32_t count, result, index;
#ifdef _WIN32
    HANDLE host;
#else
    pthread_t host;
#endif
} Worker;

#ifdef _WIN32
static unsigned __stdcall worker_main(void *opaque)
#else
static void *worker_main(void *opaque)
#endif
{
    Worker *worker = opaque;
    wait(worker->probe, worker->id, GUEST_HANDLES, worker->count, -1, worker->result, worker->index);
    return 0;
}

static void start(Worker *worker)
{
#ifdef _WIN32
    worker->host = (HANDLE)_beginthreadex(NULL, 0, worker_main, worker, 0, NULL);
    assert(worker->host);
#else
    assert(pthread_create(&worker->host, NULL, worker_main, worker) == 0);
#endif
}

static void join(Worker *worker)
{
#ifdef _WIN32
    assert(WaitForSingleObject(worker->host, 5000) == WAIT_OBJECT_0);
    assert(CloseHandle(worker->host));
#else
    assert(pthread_join(worker->host, NULL) == 0);
#endif
}

int main(void)
{
    Probe probe = {0};
    HznEventConfig config = { .opaque = &probe, .copy_handles = copy_handles };
    probe.events = hzn_events_create(&config);
    assert(probe.events);

    uint64_t x[32] = {0};
    assert(hzn_event_svc(probe.events, 0x45, x, 0) && !x[0]);
    uint32_t writer = (uint32_t)x[1], reader = (uint32_t)x[2];
    assert(writer == HZN_EVENT_HANDLE_BASE && reader == writer + 1);
    svc(&probe, 0x11, reader, INVALID_HANDLE);
    svc(&probe, 0x17, writer, INVALID_HANDLE);
    svc(&probe, 0x17, reader, 0xfa01);
    probe.handles[0] = writer;
    wait(&probe, 1, GUEST_HANDLES, 1, 0, INVALID_HANDLE, UINT32_MAX);
    probe.handles[0] = reader;
    wait(&probe, 1, GUEST_HANDLES, 1, 1000000, TIMED_OUT, UINT32_MAX);
    wait(&probe, 1, GUEST_HANDLES, 0, 0, TIMED_OUT, UINT32_MAX);
    wait(&probe, 1, GUEST_HANDLES + 4, 1, 0, 0xe601, GUEST_HANDLES + 4);
    wait(&probe, 1, GUEST_HANDLES, 65, 0, 0xee01, GUEST_HANDLES);

    assert(hzn_events_set_signaled(probe.events, 0, true));
    uint32_t remote = hzn_events_copy_read(probe.events, 0);
    uint32_t duplicate = hzn_events_copy_read(probe.events, 0);
    assert(remote != duplicate && remote && duplicate);
    probe.handles[1] = duplicate;
    wait(&probe, 1, GUEST_HANDLES, 2, 0, 0, 1);
    wait(&probe, 1, GUEST_HANDLES, 2, 0, 0, 1);
    svc(&probe, 0x11, writer, 0);
    wait(&probe, 1, GUEST_HANDLES, 2, 0, 0, 0);
    svc(&probe, 0x12, reader, 0);
    svc(&probe, 0x17, remote, 0);
    wait(&probe, 1, GUEST_HANDLES, 2, 0, TIMED_OUT, UINT32_MAX);
    svc(&probe, 0x16, writer, 0);
    svc(&probe, 0x16, writer, INVALID_HANDLE);
    svc(&probe, 0x16, HZN_EVENT_HANDLE_END - 1, INVALID_HANDLE);
    x[0] = HZN_EVENT_HANDLE_END;
    assert(!hzn_event_svc(probe.events, 0x16, x, 0) && x[0] == HZN_EVENT_HANDLE_END);

    probe.signal_in_copy = true;
    wait(&probe, 1, GUEST_HANDLES, 2, 0, 0, 1);
    probe.signal_in_copy = false;
    assert(hzn_events_cancel_wait(probe.events, 0) == INVALID_HANDLE);
    assert(hzn_events_cancel_wait(probe.events, 1) == 0);
    wait(&probe, 1, GUEST_HANDLES, 2, -1, 0, 1);
    svc(&probe, 0x12, duplicate, 0);
    wait(&probe, 1, GUEST_HANDLES, 2, 0, TIMED_OUT, UINT32_MAX);
    wait(&probe, 1, GUEST_HANDLES, 2, -1, CANCELLED, UINT32_MAX);
    wait(&probe, 1, GUEST_HANDLES, 2, 1000000, TIMED_OUT, UINT32_MAX);

    Worker target = { .probe = &probe, .id = 2, .result = CANCELLED, .index = UINT32_MAX };
    Worker other = { .probe = &probe, .id = 3, .count = 2, .index = 1 };
    start(&target);
    start(&other);
    assert(hzn_events_cancel_wait(probe.events, target.id) == 0);
    join(&target);
    assert(hzn_events_set_signaled(probe.events, 0, true));
    join(&other);

    assert(hzn_events_set_signaled(probe.events, 0, false));
    other.result = CANCELLED;
    other.index = UINT32_MAX;
    start(&other);
    hzn_events_cancel_all(probe.events);
    join(&other);
    wait(&probe, 1, GUEST_HANDLES, 0, -1, CANCELLED, UINT32_MAX);
    assert(hzn_events_copy_read(probe.events, 0) == 0);
    hzn_events_destroy(probe.events);
    puts("PASS Horizon events: access rights, shared applet copies, wait, timeout and cancellation");
    return 0;
}
