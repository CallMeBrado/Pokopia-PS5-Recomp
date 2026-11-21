/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "key_sync.h"

#undef NDEBUG
#include <assert.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum {
    MUTEX = 0x1000, KEY = 0x1004, OTHER_MUTEX = 0x1008,
    ARBITER = 0x1010, WIDE = 0x1018,
    HANDLE_BASE = 0x10000, MAIN = 1, FIRST = 2, SECOND = 3, THIRD = 4,
    INVALID_ADDRESS = 0xcc01, INVALID_CURRENT_MEMORY = 0xd401, INVALID_HANDLE = 0xe401,
    TIMED_OUT = 0xea01, TERMINATION_REQUESTED = 0x7601, INVALID_ENUM = 0xf001,
    INVALID_STATE = 0xfa01,
};
static const uint32_t wait_mask = HZN_KEY_SYNC_WAIT_MASK;
static uint32_t words[3];
static uint32_t arbiter_word;
static uint64_t wide_word;
static HznKeySync *sync;
static HANDLE lookup_seen;

typedef struct {
    uint64_t id;
    int32_t priority;
    uint32_t svc, result;
    uint64_t arg0, arg1, arg2, arg3;
} Worker;

static uint32_t tag(uint32_t id)
{
    return HANDLE_BASE + id;
}

static uint32_t *word(void *opaque, uint64_t address)
{
    (void)opaque;
    if (address == ARBITER)
        return &arbiter_word;
    if (address == WIDE || address == WIDE + 4)
        return (uint32_t *)(void *)((unsigned char *)&wide_word + (address - WIDE));
    if (address < MUTEX || address > OTHER_MUTEX || address % 4)
        return NULL;
    return &words[(address - MUTEX) / 4];
}

static uint64_t thread_id(void *opaque, uint32_t handle)
{
    (void)opaque;
    if (handle < tag(MAIN) || handle > tag(THIRD))
        return 0;
    assert(SetEvent(lookup_seen));
    return handle - HANDLE_BASE;
}

static uint32_t load(uint64_t address)
{
    return __atomic_load_n(word(NULL, address), __ATOMIC_SEQ_CST);
}

static void store(uint64_t address, uint32_t value)
{
    __atomic_store_n(word(NULL, address), value, __ATOMIC_SEQ_CST);
}

static uint32_t call(uint64_t id, int32_t priority, uint32_t number,
                     uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    uint64_t x[32] = {a, b, c, d};
    assert(hzn_key_sync_svc(sync, number, x, id, priority));
    return (uint32_t)x[0];
}

static DWORD WINAPI run(LPVOID opaque)
{
    Worker *worker = opaque;
    worker->result = call(worker->id, worker->priority, worker->svc,
                          worker->arg0, worker->arg1, worker->arg2, worker->arg3);
    return 0;
}

static HANDLE start(Worker *worker)
{
    HANDLE thread = CreateThread(NULL, 0, run, worker, 0, NULL);
    assert(thread);
    return thread;
}

static void join(HANDLE thread, const Worker *worker, uint32_t expected)
{
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    assert(worker->result == expected);
    assert(CloseHandle(thread));
}

static void wait_for(uint64_t address, uint32_t expected)
{
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (load(address) != expected) {
        assert(GetTickCount64() < deadline);
        Sleep(1);
    }
}

static void unlock_by(uint64_t id, uint64_t address)
{
    assert(call(id, 0, 0x1b, address, 0, 0, 0) == 0);
}

static void test_mutex_priority(void)
{
    Worker low = {FIRST, 30, 0x1a, 0, tag(MAIN), MUTEX, tag(FIRST), 0};
    Worker high_a = {SECOND, 10, 0x1a, 0, tag(MAIN), MUTEX, tag(SECOND), 0};
    Worker high_b = {THIRD, 10, 0x1a, 0, tag(MAIN), MUTEX, tag(THIRD), 0};
    store(MUTEX, tag(MAIN) | wait_mask);

    HANDLE a = start(&low);
    assert(WaitForSingleObject(lookup_seen, 5000) == WAIT_OBJECT_0);
    HANDLE b = start(&high_a);
    assert(WaitForSingleObject(lookup_seen, 5000) == WAIT_OBJECT_0);
    HANDLE c = start(&high_b);
    assert(WaitForSingleObject(lookup_seen, 5000) == WAIT_OBJECT_0);
    unlock_by(MAIN, MUTEX);
    join(b, &high_a, 0);
    assert(load(MUTEX) == (tag(SECOND) | wait_mask));
    assert(WaitForSingleObject(a, 0) == WAIT_TIMEOUT && WaitForSingleObject(c, 0) == WAIT_TIMEOUT);
    unlock_by(SECOND, MUTEX);
    join(c, &high_b, 0);
    assert(load(MUTEX) == (tag(THIRD) | wait_mask));
    unlock_by(THIRD, MUTEX);
    join(a, &low, 0);
    assert(load(MUTEX) == tag(FIRST));
    unlock_by(FIRST, MUTEX);
    assert(load(MUTEX) == 0);
}

static void test_signal_reacquires(void)
{
    Worker waiter = {FIRST, 12, 0x1c, 0, MUTEX, KEY + 1, tag(FIRST), UINT64_MAX};
    store(MUTEX, tag(FIRST));
    HANDLE thread = start(&waiter);
    wait_for(MUTEX, 0);
    assert(load(KEY) == 1);
    store(MUTEX, tag(MAIN));
    assert(call(MAIN, 0, 0x1d, KEY + 2, 1, 0, 0) == 0);
    assert(load(KEY) == 0 && load(MUTEX) == (tag(MAIN) | wait_mask));
    assert(WaitForSingleObject(thread, 0) == WAIT_TIMEOUT);
    unlock_by(MAIN, MUTEX);
    join(thread, &waiter, 0);
    assert(load(MUTEX) == tag(FIRST));
    unlock_by(FIRST, MUTEX);
}

static void test_broadcast(void)
{
    Worker low = {FIRST, 30, 0x1c, 0, MUTEX, KEY, tag(FIRST), UINT64_MAX};
    Worker high = {SECOND, 10, 0x1c, 0, MUTEX, KEY, tag(SECOND), UINT64_MAX};
    store(MUTEX, tag(FIRST));
    HANDLE a = start(&low);
    wait_for(MUTEX, 0);
    store(MUTEX, tag(SECOND));
    HANDLE b = start(&high);
    wait_for(MUTEX, 0);
    assert(call(MAIN, 0, 0x1d, KEY, 0, 0, 0) == 0);
    join(b, &high, 0);
    assert(load(KEY) == 0 && load(MUTEX) == (tag(SECOND) | wait_mask));
    assert(WaitForSingleObject(a, 0) == WAIT_TIMEOUT);
    unlock_by(SECOND, MUTEX);
    join(a, &low, 0);
    assert(load(MUTEX) == tag(FIRST));
    unlock_by(FIRST, MUTEX);
}

static void test_timeout(void)
{
    store(MUTEX, tag(MAIN));
    assert(call(MAIN, 0, 0x1c, MUTEX, KEY, tag(MAIN), 0) == TIMED_OUT);
    assert(load(MUTEX) == 0 && load(KEY) == 1);
    assert(call(MAIN, 0, 0x1d, KEY, 1, 0, 0) == 0 && load(KEY) == 0);

    Worker waiter = {FIRST, 12, 0x1c, 0, MUTEX, KEY, tag(FIRST), UINT64_C(250000000)};
    store(MUTEX, tag(FIRST));
    HANDLE thread = start(&waiter);
    wait_for(MUTEX, 0);
    store(MUTEX, tag(MAIN));
    assert(call(MAIN, 0, 0x1d, KEY, 1, 0, 0) == 0);
    assert(load(MUTEX) == (tag(MAIN) | wait_mask));
    join(thread, &waiter, TIMED_OUT);
    assert(load(MUTEX) == (tag(MAIN) | wait_mask));
    unlock_by(MAIN, MUTEX);
    assert(load(MUTEX) == 0);
}

static void test_address_conditions(void)
{
    assert(call(MAIN, 0, 0x34, ARBITER + 1, 2, 1, 0) == INVALID_ADDRESS);
    assert(call(MAIN, 0, 0x34, ARBITER, 4, 1, 0) == INVALID_ENUM);
    assert(call(MAIN, 0, 0x34, ARBITER + 4, 2, 1, 0) == INVALID_CURRENT_MEMORY);
    assert(call(MAIN, 0, 0x35, ARBITER + 4, 0, 0, 1) == 0);
    assert(call(MAIN, 0, 0x35, ARBITER + 4, 1, 0, 1) == INVALID_CURRENT_MEMORY);
    assert(call(MAIN, 0, 0x35, ARBITER, 3, 0, 1) == INVALID_ENUM);

    store(ARBITER, UINT32_MAX);
    assert(call(MAIN, 0, 0x34, ARBITER, 0, 0, 0) == TIMED_OUT);
    assert(load(ARBITER) == UINT32_MAX);
    assert(call(MAIN, 0, 0x34, ARBITER, 1, UINT32_MAX, 0) == INVALID_STATE);
    assert(call(MAIN, 0, 0x34, ARBITER, 1, 0, 0) == TIMED_OUT);
    assert(load(ARBITER) == UINT32_MAX - 1);
    assert(call(MAIN, 0, 0x34, ARBITER, 2, 1, 0) == INVALID_STATE);
    assert(call(MAIN, 0, 0x34, ARBITER, 2, UINT32_MAX - 1, 1000000) == TIMED_OUT);
    assert(call(MAIN, 0, 0x35, ARBITER, 2, UINT32_MAX - 1, -1) == 0);
    assert(load(ARBITER) == UINT32_MAX); /* Empty address queue. */
    assert(call(MAIN, 0, 0x35, ARBITER, 1, UINT32_MAX, 1) == 0);
    assert(load(ARBITER) == 0); /* Increment wraps as a 32-bit word. */

    const uint64_t wide_value = UINT64_C(0x1122334455667788);
    __atomic_store_n(&wide_word, wide_value, __ATOMIC_SEQ_CST);
    assert(call(MAIN, 0, 0x34, WIDE, 3, (uint32_t)wide_value, 0) == INVALID_STATE);
    assert(call(MAIN, 0, 0x34, WIDE, 3, wide_value, 0) == TIMED_OUT);
    assert(call(MAIN, 0, 0x34, WIDE + 4, 3, wide_value, 0) == INVALID_ADDRESS);
    assert(call(MAIN, 0, 0x34, ARBITER, 3, wide_value, 0) == INVALID_CURRENT_MEMORY);
}

static void test_address_signals(void)
{
    Worker low = {FIRST, 30, 0x34, 0, ARBITER, 1, 5, UINT64_MAX};
    Worker high = {SECOND, 10, 0x34, 0, ARBITER, 1, 5, UINT64_MAX};
    Worker next = {THIRD, 10, 0x34, 0, ARBITER, 1, 5, UINT64_MAX};
    store(ARBITER, 4);
    HANDLE a = start(&low);
    wait_for(ARBITER, 3);
    HANDLE b = start(&high);
    wait_for(ARBITER, 2);
    HANDLE c = start(&next);
    wait_for(ARBITER, 1);
    assert(call(MAIN, 0, 0x35, ARBITER, 2, 0, 1) == INVALID_STATE);
    assert(call(MAIN, 0, 0x35, KEY, 0, 0, -1) == 0);
    assert(WaitForSingleObject(a, 0) == WAIT_TIMEOUT && WaitForSingleObject(b, 0) == WAIT_TIMEOUT &&
           WaitForSingleObject(c, 0) == WAIT_TIMEOUT);
    assert(call(MAIN, 0, 0x35, ARBITER, 2, 1, 1) == 0);
    join(b, &high, 0);
    assert(load(ARBITER) == 1); /* Still has two waiters. */
    assert(call(MAIN, 0, 0x35, ARBITER, 0, 0, 1) == 0);
    join(c, &next, 0);
    assert(call(MAIN, 0, 0x35, ARBITER, 2, 1, 1) == 0);
    join(a, &low, 0);
    assert(load(ARBITER) == 0); /* Positive count clears the final waiter. */

    store(ARBITER, 4);
    a = start(&low);
    wait_for(ARBITER, 3);
    b = start(&high);
    wait_for(ARBITER, 2);
    assert(call(MAIN, 0, 0x35, ARBITER, 2, 2, 0) == 0);
    join(a, &low, 0);
    join(b, &high, 0);
    assert(load(ARBITER) == 1); /* Horizon 15+ broadcasts subtract one. */

    a = start(&low);
    wait_for(ARBITER, 0);
    assert(call(MAIN, 0, 0x35, ARBITER, 1, 0, -1) == 0);
    join(a, &low, 0);
    assert(load(ARBITER) == 1);
}

static void test_light_event_race(void)
{
    Worker waiter = {FIRST, 12, 0x34, 0, ARBITER, 2, 1, UINT64_MAX};
    store(ARBITER, 1);
    HANDLE thread = start(&waiter);
    assert(call(MAIN, 0, 0x35, ARBITER, 2, 1, 1) == 0);
    const uint32_t state = load(ARBITER);
    /* The exact auto-clear SDK pair handles either ordering. With an empty
     * queue the signal leaves 2 for the SDK fast path and the SVC retries. */
    assert(state == 0 || state == 2);
    join(thread, &waiter, state == 0 ? 0 : INVALID_STATE);
}

static void test_shutdown(void)
{
    Worker key_wait = {FIRST, 12, 0x1c, 0, MUTEX, KEY, tag(FIRST), UINT64_MAX};
    Worker mutex_wait = {SECOND, 12, 0x1a, 0, tag(MAIN), OTHER_MUTEX, tag(SECOND), 0};
    Worker address_wait = {THIRD, 12, 0x34, 0, ARBITER, 1, 2, UINT64_MAX};
    store(MUTEX, tag(FIRST));
    HANDLE a = start(&key_wait);
    wait_for(MUTEX, 0);
    store(OTHER_MUTEX, tag(MAIN) | wait_mask);
    assert(ResetEvent(lookup_seen));
    HANDLE b = start(&mutex_wait);
    assert(WaitForSingleObject(lookup_seen, 5000) == WAIT_OBJECT_0);
    store(ARBITER, 1);
    HANDLE c = start(&address_wait);
    wait_for(ARBITER, 0);
    hzn_key_sync_cancel_all(sync);
    join(a, &key_wait, TERMINATION_REQUESTED);
    join(b, &mutex_wait, TERMINATION_REQUESTED);
    join(c, &address_wait, TERMINATION_REQUESTED);
    assert(call(MAIN, 0, 0x1c, MUTEX, KEY, tag(MAIN), UINT64_MAX) == TERMINATION_REQUESTED);
    assert(call(MAIN, 0, 0x34, ARBITER, 2, 0, UINT64_MAX) == TERMINATION_REQUESTED);
}

int main(void)
{
    lookup_seen = CreateEventW(NULL, FALSE, FALSE, NULL);
    assert(lookup_seen);
    const HznKeySyncConfig config = {.word = word, .thread_id = thread_id};
    sync = hzn_key_sync_create(&config);
    assert(sync);
    uint64_t x[32] = {0};
    assert(!hzn_key_sync_svc(sync, 0x19, x, MAIN, 0));
    assert(call(MAIN, 0, 0x1a, tag(FIRST), MUTEX + 1, tag(MAIN), 0) == INVALID_ADDRESS);
    assert(call(MAIN, 0, 0x1a, tag(FIRST), 0x100c, tag(MAIN), 0) == INVALID_CURRENT_MEMORY);
    assert(call(MAIN, 0, 0x1a, 0, MUTEX, tag(MAIN), 0) == 0);
    store(MUTEX, wait_mask);
    assert(call(MAIN, 0, 0x1a, 0, MUTEX, tag(MAIN), 0) == INVALID_HANDLE);
    store(MUTEX, 0);
    test_mutex_priority();
    test_signal_reacquires();
    test_broadcast();
    test_timeout();
    test_address_conditions();
    test_address_signals();
    test_light_event_race();
    test_shutdown();
    hzn_key_sync_destroy(sync);
    assert(CloseHandle(lookup_seen));
    puts("PASS Horizon key sync and address arbitration, priority/FIFO, LightEvent, timeouts and shutdown");
    return 0;
}
