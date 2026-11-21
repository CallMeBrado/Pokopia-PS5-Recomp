/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "event.h"

#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

enum {
    OUT_OF_RESOURCE = 0xce01,
    OUT_OF_HANDLES = 0xd201,
    INVALID_HANDLE = 0xe401,
    INVALID_POINTER = 0xe601,
    TIMED_OUT = 0xea01,
    CANCELLED = 0xec01,
    OUT_OF_RANGE = 0xee01,
    INVALID_STATE = 0xfa01,
    MAX_WAIT_HANDLES = 64,
    MAX_EVENT_HANDLES = HZN_EVENT_HANDLE_END - HZN_EVENT_HANDLE_BASE,
};

typedef struct Event {
    struct Event *next_key;
    uint64_t key;
    size_t refs;
    uint32_t thread_handle;
    bool signaled, thread_open;
} Event;

typedef struct {
    Event *event;
    bool writable;
} EventHandle;

typedef struct Wait {
    struct Wait *next;
    Event *objects[MAX_WAIT_HANDLES];
    uint64_t waiter_id;
    uint32_t count;
    int32_t index;
    bool cancelled;
} Wait;

typedef struct Cancel {
    struct Cancel *next;
    uint64_t waiter_id;
} Cancel;

struct HznEvents {
    HznEventConfig config;
    Event *keyed, *threads;
    EventHandle *handles;
    Wait *waiters;
    Cancel *pending_cancels;
    size_t handle_count, handle_capacity;
    bool closing;
#ifdef _WIN32
    SRWLOCK lock;
    CONDITION_VARIABLE changed;
#else
    pthread_mutex_t lock;
    pthread_cond_t changed;
#endif
};

static void lock(HznEvents *events)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&events->lock);
#else
    pthread_mutex_lock(&events->lock);
#endif
}

static void unlock(HznEvents *events)
{
#ifdef _WIN32
    ReleaseSRWLockExclusive(&events->lock);
#else
    pthread_mutex_unlock(&events->lock);
#endif
}

static void wake_all(HznEvents *events)
{
#ifdef _WIN32
    WakeAllConditionVariable(&events->changed);
#else
    pthread_cond_broadcast(&events->changed);
#endif
}

static uint32_t reserve_handles(HznEvents *events, size_t count)
{
    size_t needed = events->handle_count + count;
    if (needed > MAX_EVENT_HANDLES)
        return OUT_OF_HANDLES;
    if (needed <= events->handle_capacity)
        return 0;
    size_t capacity = events->handle_capacity ? 2 * events->handle_capacity : 16;
    EventHandle *handles = realloc(events->handles, capacity * sizeof *handles);
    if (!handles)
        return OUT_OF_RESOURCE;
    events->handles = handles;
    events->handle_capacity = capacity;
    return 0;
}

static uint32_t add_handle(HznEvents *events, Event *event, bool writable)
{
    size_t index = events->handle_count++;
    events->handles[index] = (EventHandle){ event, writable };
    ++event->refs;
    return HZN_EVENT_HANDLE_BASE + (uint32_t)index;
}

static bool owns_handle(uint32_t handle)
{
    return handle >= HZN_EVENT_HANDLE_BASE && handle < HZN_EVENT_HANDLE_END;
}

static EventHandle *find_handle(HznEvents *events, uint32_t handle)
{
    size_t index = handle - HZN_EVENT_HANDLE_BASE;
    if (!owns_handle(handle) || index >= events->handle_count || !events->handles[index].event)
        return NULL;
    return &events->handles[index];
}

static void release_event(Event *event)
{
    if (--event->refs == 0)
        free(event);
}

static Event *keyed_event(HznEvents *events, uint64_t key)
{
    for (Event *event = events->keyed; event; event = event->next_key)
        if (event->key == key)
            return event;
    Event *event = calloc(1, sizeof *event);
    if (event) {
        event->key = key;
        event->refs = 1;
        event->next_key = events->keyed;
        events->keyed = event;
    }
    return event;
}

static void set_signaled(HznEvents *events, Event *event, bool signaled)
{
    if (event->signaled == signaled)
        return;
    event->signaled = signaled;
    if (!signaled)
        return;

    /* Record the completion now so Clear/Reset cannot steal the signal from
     * already registered waiters. An existing signal never consumes itself. */
    bool wake = false;
    for (Wait *wait = events->waiters; wait; wait = wait->next) {
        if (wait->index >= 0 || wait->cancelled)
            continue;
        for (uint32_t i = 0; i < wait->count; ++i) {
            if (wait->objects[i] == event) {
                wait->index = (int32_t)i;
                wake = true;
                break;
            }
        }
    }
    if (wake)
        wake_all(events);
}

/* Threads retain their own reference through native completion. The same
 * event lock protects open-handle lookup, waiter registration and signaling. */
static Event **find_thread(HznEvents *events, uint32_t handle)
{
    Event **link = &events->threads;
    while (*link && (*link)->thread_handle != handle)
        link = &(*link)->next_key;
    return link;
}

bool hzn_events_add_thread(HznEvents *events, uint32_t handle, uint64_t id)
{
    if (!events || !handle || !id || owns_handle(handle) || handle >= UINT32_C(0xffff8000))
        return false;
    lock(events);
    Event **link = find_thread(events, handle);
    Event *event = !*link && !events->closing ? calloc(1, sizeof *event) : NULL;
    if (event) {
        *event = (Event){.key = id, .refs = 1, .thread_handle = handle, .thread_open = true};
        *link = event;
    }
    unlock(events);
    return event != NULL;
}

void hzn_events_close_thread(HznEvents *events, uint32_t handle)
{
    if (!events)
        return;
    lock(events);
    Event *event = *find_thread(events, handle);
    if (event)
        event->thread_open = false;
    unlock(events);
}

void hzn_events_exit_thread(HznEvents *events, uint32_t handle)
{
    if (!events)
        return;
    lock(events);
    Event *event = *find_thread(events, handle);
    if (event)
        set_signaled(events, event, true);
    unlock(events);
}

void hzn_events_remove_thread(HznEvents *events, uint32_t handle)
{
    if (!events)
        return;
    lock(events);
    Event **link = find_thread(events, handle);
    if (*link) {
        Event *event = *link;
        *link = event->next_key;
        release_event(event);
    }
    unlock(events);
}

static Event *find_waitable(HznEvents *events, uint32_t handle, uint64_t waiter_id)
{
    if (handle == UINT32_C(0xffff8000)) {
        for (Event *event = events->threads; event; event = event->next_key)
            if (event->key == waiter_id)
                return event;
        return NULL;
    }
    if (owns_handle(handle)) {
        EventHandle *slot = find_handle(events, handle);
        return slot && !slot->writable ? slot->event : NULL;
    }
    Event *event = *find_thread(events, handle);
    return event && event->thread_open ? event : NULL;
}

static uint32_t create_pair(HznEvents *events, uint64_t x[32])
{
    if (events->closing)
        return CANCELLED;
    uint32_t result = reserve_handles(events, 2);
    if (result)
        return result;
    Event *event = calloc(1, sizeof *event);
    if (!event)
        return OUT_OF_RESOURCE;
    x[1] = add_handle(events, event, true);
    x[2] = add_handle(events, event, false);
    return 0;
}

static uint32_t update_event(HznEvents *events, uint32_t number, uint32_t handle)
{
    EventHandle *slot = find_handle(events, handle);
    if (!slot)
        return INVALID_HANDLE;
    switch (number) {
    case 0x11: /* SignalEvent, writable end only. */
        if (!slot->writable)
            return INVALID_HANDLE;
        set_signaled(events, slot->event, true);
        return 0;
    case 0x12: /* ClearEvent, either end. */
        set_signaled(events, slot->event, false);
        return 0;
    case 0x17: /* ResetSignal, a signaled readable end only. */
        if (slot->writable)
            return INVALID_HANDLE;
        if (!slot->event->signaled)
            return INVALID_STATE;
        set_signaled(events, slot->event, false);
        return 0;
    default: /* CloseHandle. */
        release_event(slot->event);
        *slot = (EventHandle){0};
        return 0;
    }
}

static uint32_t wait_pending(HznEvents *events, Wait *wait, int64_t ns)
{
    if (ns < 0) {
        while (wait->index < 0 && !wait->cancelled && !events->closing) {
#ifdef _WIN32
            if (!SleepConditionVariableSRW(&events->changed, &events->lock, INFINITE, 0))
#else
            if (pthread_cond_wait(&events->changed, &events->lock))
#endif
                return INVALID_STATE;
        }
    } else {
#ifdef _WIN32
        uint64_t ms = (uint64_t)ns / 1000000 + (ns % 1000000 != 0);
        uint64_t deadline = GetTickCount64() + ms;
        while (wait->index < 0 && !wait->cancelled && !events->closing && ms) {
            DWORD timeout = ms < INFINITE ? (DWORD)ms : INFINITE - 1;
            if (!SleepConditionVariableSRW(&events->changed, &events->lock, timeout, 0) &&
                GetLastError() != ERROR_TIMEOUT)
                return INVALID_STATE;
            uint64_t now = GetTickCount64();
            ms = deadline > now ? deadline - now : 0;
        }
#else
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline))
            return INVALID_STATE;
        deadline.tv_sec += ns / 1000000000;
        deadline.tv_nsec += ns % 1000000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_nsec -= 1000000000;
            ++deadline.tv_sec;
        }
        while (wait->index < 0 && !wait->cancelled && !events->closing &&
               pthread_cond_timedwait(&events->changed, &events->lock, &deadline) == 0) {}
#endif
    }
    if (wait->index >= 0)
        return 0;
    return wait->cancelled || events->closing ? CANCELLED : TIMED_OUT;
}

static Cancel **find_cancel(HznEvents *events, uint64_t waiter_id)
{
    Cancel **link = &events->pending_cancels;
    while (*link && (*link)->waiter_id != waiter_id)
        link = &(*link)->next;
    return link;
}

static uint32_t wait_locked(HznEvents *events, const uint32_t *handles, Wait *wait, int64_t ns)
{
    if (events->closing)
        return CANCELLED;
    for (uint32_t i = 0; i < wait->count; ++i) {
        Event *object = find_waitable(events, handles[i], wait->waiter_id);
        if (!object)
            return INVALID_HANDLE;
        wait->objects[i] = object;
    }
    for (uint32_t i = 0; i < wait->count; ++i) {
        if (wait->objects[i]->signaled) {
            wait->index = (int32_t)i;
            return 0;
        }
    }
    if (!ns)
        return TIMED_OUT;

    Cancel **pending = find_cancel(events, wait->waiter_id);
    if (*pending) {
        Cancel *cancel = *pending;
        *pending = cancel->next;
        free(cancel);
        return CANCELLED;
    }

    for (uint32_t i = 0; i < wait->count; ++i)
        ++wait->objects[i]->refs;
    wait->next = events->waiters;
    events->waiters = wait;
    uint32_t result = wait_pending(events, wait, ns);
    Wait **link = &events->waiters;
    while (*link != wait)
        link = &(*link)->next;
    *link = wait->next;
    for (uint32_t i = 0; i < wait->count; ++i)
        release_event(wait->objects[i]);
    return result;
}

static uint32_t wait_synchronization(HznEvents *events, uint64_t x[32], uint64_t waiter_id)
{
    uint32_t count = (uint32_t)x[2];
    uint32_t handles[MAX_WAIT_HANDLES];
    if (count > MAX_WAIT_HANDLES)
        return OUT_OF_RANGE;
    if (!waiter_id)
        return INVALID_HANDLE;
    if (count && !events->config.copy_handles(events->config.opaque, x[1], handles, count))
        return INVALID_POINTER;
    Wait wait = { .waiter_id = waiter_id, .count = count, .index = -1 };
    lock(events);
    uint32_t result = wait_locked(events, handles, &wait, (int64_t)x[3]);
    unlock(events);
    x[1] = (uint32_t)wait.index;
    return result;
}

HznEvents *hzn_events_create(const HznEventConfig *config)
{
    if (!config || !config->copy_handles)
        return NULL;
    HznEvents *events = calloc(1, sizeof *events);
    if (!events)
        return NULL;
#ifdef _WIN32
    InitializeSRWLock(&events->lock);
    InitializeConditionVariable(&events->changed);
#else
    if (pthread_mutex_init(&events->lock, NULL)) {
        free(events);
        return NULL;
    }
    if (pthread_cond_init(&events->changed, NULL)) {
        pthread_mutex_destroy(&events->lock);
        free(events);
        return NULL;
    }
#endif
    events->config = *config;
    return events;
}

bool hzn_event_svc(HznEvents *events, uint32_t number, uint64_t x[32], uint64_t waiter_id)
{
    if (!events)
        return false;
    uint32_t handle = (uint32_t)x[0];
    switch (number) {
    case 0x18: /* WaitSynchronization. */
        x[0] = wait_synchronization(events, x, waiter_id);
        return true;
    case 0x45: /* CreateEvent. */
        lock(events);
        x[0] = create_pair(events, x);
        unlock(events);
        return true;
    case 0x16: /* CloseHandle. */
        if (!owns_handle(handle))
            return false;
        /* Fall through. */
    case 0x11:
    case 0x12:
    case 0x17:
        lock(events);
        x[0] = update_event(events, number, handle);
        unlock(events);
        return true;
    default:
        return false;
    }
}

uint32_t hzn_events_copy_read(HznEvents *events, uint64_t key)
{
    uint32_t handle = 0;
    lock(events);
    if (!events->closing && !reserve_handles(events, 1)) {
        Event *event = keyed_event(events, key);
        if (event)
            handle = add_handle(events, event, false);
    }
    unlock(events);
    return handle;
}

bool hzn_events_set_signaled(HznEvents *events, uint64_t key, bool signaled)
{
    lock(events);
    Event *event = events->closing ? NULL : keyed_event(events, key);
    if (event)
        set_signaled(events, event, signaled);
    unlock(events);
    return event != NULL;
}

static uint32_t cancel_wait(HznEvents *events, uint64_t waiter_id)
{
    if (events->closing)
        return CANCELLED;
    for (Wait *wait = events->waiters; wait; wait = wait->next) {
        if (wait->waiter_id == waiter_id && wait->index < 0 && !wait->cancelled) {
            wait->cancelled = true;
            wake_all(events);
            return 0;
        }
    }
    Cancel **pending = find_cancel(events, waiter_id);
    if (*pending)
        return 0;
    Cancel *cancel = malloc(sizeof *cancel);
    if (!cancel)
        return OUT_OF_RESOURCE;
    *cancel = (Cancel){ .next = NULL, .waiter_id = waiter_id };
    *pending = cancel;
    return 0;
}

uint32_t hzn_events_cancel_wait(HznEvents *events, uint64_t waiter_id)
{
    if (!events || !waiter_id)
        return INVALID_HANDLE;
    lock(events);
    uint32_t result = cancel_wait(events, waiter_id);
    unlock(events);
    return result;
}

void hzn_events_cancel_all(HznEvents *events)
{
    if (!events)
        return;
    lock(events);
    events->closing = true;
    for (Wait *wait = events->waiters; wait; wait = wait->next)
        if (wait->index < 0)
            wait->cancelled = true;
    wake_all(events);
    unlock(events);
}

void hzn_events_destroy(HznEvents *events)
{
    if (!events)
        return;
    for (size_t i = 0; i < events->handle_count; ++i)
        if (events->handles[i].event)
            release_event(events->handles[i].event);
    free(events->handles);
    while (events->keyed) {
        Event *event = events->keyed;
        events->keyed = event->next_key;
        release_event(event);
    }
    while (events->threads) {
        Event *event = events->threads;
        events->threads = event->next_key;
        release_event(event);
    }
    while (events->pending_cancels) {
        Cancel *cancel = events->pending_cancels;
        events->pending_cancels = cancel->next;
        free(cancel);
    }
#ifndef _WIN32
    pthread_cond_destroy(&events->changed);
    pthread_mutex_destroy(&events->lock);
#endif
    free(events);
}
