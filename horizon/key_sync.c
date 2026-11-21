/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "key_sync.h"

#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#endif

enum {
    TERMINATION_REQUESTED = 0x7601,
    INVALID_ADDRESS = 0xcc01,
    INVALID_CURRENT_MEMORY = 0xd401,
    INVALID_HANDLE = 0xe401,
    TIMED_OUT = 0xea01,
    INVALID_ENUM = 0xf001,
    INVALID_STATE = 0xfa01,
};

enum {
    WAIT_IF_LESS = 0, DECREMENT_AND_WAIT_IF_LESS = 1, WAIT_IF_EQUAL = 2, WAIT_IF_EQUAL_64 = 3,
    SIGNAL = 0, SIGNAL_AND_INCREMENT = 1, SIGNAL_AND_MODIFY_BY_WAITERS = 2,
};

typedef struct KeyWait {
    struct KeyWait *next;
    uint32_t *mutex;
    uint64_t mutex_address, wait_address, thread_id, owner_id;
    uint32_t tag, result;
    int32_t priority;
    bool done;
    uint64_t completed_ns; /* R71: when complete() ran, for wake latency */
#ifndef _WIN32
    /* R72: each waiter sleeps on its own condition variable, so completing
     * one wait wakes one thread. The shared one woke every blocked guest
     * thread (~45 per completion) and made each hand-off take ~0.7 ms. */
    pthread_cond_t cond;
    bool has_cond;
#endif
} KeyWait;

/* R71: wake latency (complete() to the waiter running again) and how many
 * times blocked threads woke without being the one completed:
 * [0 wakes, 1 latency sum ns, 2 max ns, 3 wakes > 1 ms, 4 spurious wakeups, 5 broadcasts]. */
uint64_t hzn_wake_stats[6];
static uint64_t wake_clock_ns(void)
{
#ifdef _WIN32
    return 0;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
#endif
}

struct HznKeySync {
    HznKeySyncConfig config;
    KeyWait *keys, *mutexes, *addresses;
    bool closing;
#ifdef _WIN32
    SRWLOCK lock;
    CONDITION_VARIABLE changed;
#else
    pthread_mutex_t lock;
    pthread_cond_t changed;
#endif
};

static void lock(HznKeySync *sync)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&sync->lock);
#else
    pthread_mutex_lock(&sync->lock);
#endif
}

static void unlock(HznKeySync *sync)
{
#ifdef _WIN32
    ReleaseSRWLockExclusive(&sync->lock);
#else
    pthread_mutex_unlock(&sync->lock);
#endif
}

static void notify(HznKeySync *sync)
{
#ifdef _WIN32
    WakeAllConditionVariable(&sync->changed);
#else
    pthread_cond_broadcast(&sync->changed);
#endif
}

HznKeySync *hzn_key_sync_create(const HznKeySyncConfig *config)
{
    if (!config || !config->word || !config->thread_id)
        return NULL;
    HznKeySync *sync = calloc(1, sizeof *sync);
    if (!sync)
        return NULL;

    sync->config = *config;
#ifdef _WIN32
    InitializeSRWLock(&sync->lock);
    InitializeConditionVariable(&sync->changed);
#else
    if (pthread_mutex_init(&sync->lock, NULL)) {
        free(sync);
        return NULL;
    }
    if (pthread_cond_init(&sync->changed, NULL)) {
        pthread_mutex_destroy(&sync->lock);
        free(sync);
        return NULL;
    }
#endif
    return sync;
}

void hzn_key_sync_cancel_all(HznKeySync *sync)
{
    if (!sync)
        return;
    lock(sync);
    sync->closing = true;
#ifndef _WIN32
    KeyWait *const queues[] = {sync->keys, sync->mutexes, sync->addresses};
    for (size_t i = 0; i < sizeof queues / sizeof queues[0]; ++i)
        for (KeyWait *wait = queues[i]; wait; wait = wait->next)
            if (wait->has_cond)
                pthread_cond_signal(&wait->cond);
#endif
    notify(sync);
    unlock(sync);
}

void hzn_key_sync_destroy(HznKeySync *sync)
{
    if (!sync)
        return;
#ifndef _WIN32
    pthread_cond_destroy(&sync->changed);
    pthread_mutex_destroy(&sync->lock);
#endif
    free(sync);
}

/* Guest fast paths access these words with atomics outside the host lock. */
static uint32_t load_word(const uint32_t *word)
{
    return __atomic_load_n(word, __ATOMIC_SEQ_CST);
}

static void store_word(uint32_t *word, uint32_t value)
{
    __atomic_store_n(word, value, __ATOMIC_SEQ_CST);
}

static bool compare_word(uint32_t *word, uint32_t expected, uint32_t value)
{
    return __atomic_compare_exchange_n(word, &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static uint32_t *resolve_word(HznKeySync *sync, uint64_t address)
{
    uint32_t *word = sync->config.word(sync->config.opaque, address);
    return (uintptr_t)word % sizeof(uint32_t) == 0 ? word : NULL;
}

static uint64_t thread_id(HznKeySync *sync, uint32_t handle)
{
    /* The kernel resolves real handles here, without pseudo-handles. */
    if (!handle || handle >= UINT32_C(0x40000000))
        return 0;
    return sync->config.thread_id(sync->config.opaque, handle);
}

static void enqueue(KeyWait **queue, KeyWait *wait)
{
    while (*queue)
        queue = &(*queue)->next;
    wait->next = NULL;
    *queue = wait;
}

static KeyWait *detach(KeyWait **link)
{
    KeyWait *wait = *link;
    *link = wait->next;
    wait->next = NULL;
    return wait;
}

static void remove_wait(KeyWait **queue, KeyWait *wait)
{
    for (; *queue; queue = &(*queue)->next) {
        if (*queue == wait) {
            detach(queue);
            return;
        }
    }
}

static bool matches(const KeyWait *wait, uint64_t address, uint64_t owner)
{
    return owner ? wait->owner_id == owner && wait->mutex_address == address
                 : wait->wait_address == address;
}

static KeyWait **best_wait(KeyWait **queue, uint64_t address, uint64_t owner)
{
    KeyWait **best = NULL;
    for (; *queue; queue = &(*queue)->next) {
        if (matches(*queue, address, owner) && (!best || (*queue)->priority < (*best)->priority))
            best = queue;
    }
    return best;
}

static void complete(HznKeySync *sync, KeyWait *wait, uint32_t result)
{
    wait->result = result;
    wait->done = true;
    wait->completed_ns = wake_clock_ns();
    ++hzn_wake_stats[5];
#ifndef _WIN32
    if (wait->has_cond) {
        pthread_cond_signal(&wait->cond);
        return;
    }
#endif
    notify(sync);
}

static void release_mutex(HznKeySync *sync, uint32_t *mutex, uint64_t address, uint64_t owner)
{
    KeyWait **best = best_wait(&sync->mutexes, address, owner);
    if (!best) {
        store_word(mutex, 0);
        return;
    }

    KeyWait *next = detach(best);
    uint32_t tag = next->tag;
    /* The remaining waiters now wait for the new owner of the mutex. */
    for (KeyWait *wait = sync->mutexes; wait; wait = wait->next) {
        if (matches(wait, address, owner)) {
            wait->owner_id = next->thread_id;
            tag |= HZN_KEY_SYNC_WAIT_MASK;
        }
    }
    store_word(mutex, tag);
    complete(sync, next, 0);
}

static uint32_t block(HznKeySync *sync, KeyWait *wait, int64_t ns)
{
    uint32_t result = TIMED_OUT;
#ifdef _WIN32
    const uint64_t duration_ms = ns > 0 ? (uint64_t)ns / 1000000 + (ns % 1000000 != 0) : 0;
    const uint64_t start_ms = GetTickCount64();
    while (!wait->done && !sync->closing) {
        DWORD timeout = INFINITE;
        if (ns >= 0) {
            const uint64_t elapsed = GetTickCount64() - start_ms;
            if (elapsed >= duration_ms)
                break;
            const uint64_t remaining = duration_ms - elapsed;
            timeout = remaining < INFINITE ? (DWORD)remaining : INFINITE - 1;
        }
        if (!SleepConditionVariableSRW(&sync->changed, &sync->lock, timeout, 0) &&
            GetLastError() != ERROR_TIMEOUT) {
            result = INVALID_STATE;
            break;
        }
    }
#else
    struct timespec deadline;
    if (ns > 0) {
        if (clock_gettime(CLOCK_REALTIME, &deadline))
            return INVALID_STATE;
        deadline.tv_sec += ns / 1000000000;
        deadline.tv_nsec += ns % 1000000000;
        if (deadline.tv_nsec >= 1000000000) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000;
        }
    }
    while (!wait->done && !sync->closing && ns != 0) {
        pthread_cond_t *const cond = wait->has_cond ? &wait->cond : &sync->changed;
        const int error = ns < 0 ? pthread_cond_wait(cond, &sync->lock)
                                 : pthread_cond_timedwait(cond, &sync->lock, &deadline);
        if (error) {
            if (error != ETIMEDOUT)
                result = INVALID_STATE;
            break;
        }
        if (!wait->done)
            ++hzn_wake_stats[4];
    }
#endif
    if (wait->done && wait->completed_ns) {
        const uint64_t latency = wake_clock_ns() - wait->completed_ns;
        ++hzn_wake_stats[0];
        hzn_wake_stats[1] += latency;
        if (latency > hzn_wake_stats[2])
            hzn_wake_stats[2] = latency;
        hzn_wake_stats[3] += latency > 1000000;
    }
    if (wait->done)
        return wait->result;
    return sync->closing ? TERMINATION_REQUESTED : result;
}

static uint32_t arbitrate_lock(HznKeySync *sync, KeyWait *wait, uint32_t owner)
{
    if (load_word(wait->mutex) != (owner | HZN_KEY_SYNC_WAIT_MASK))
        return 0; /* The owner released it before entry. The SDK retries. */
    wait->owner_id = thread_id(sync, owner);
    if (!wait->owner_id)
        return INVALID_HANDLE;

    enqueue(&sync->mutexes, wait);
    const uint32_t result = block(sync, wait, -1);
    if (!wait->done)
        remove_wait(&sync->mutexes, wait);
    return result;
}

/* R71: how condition-variable (0x1c) and address (0x34) waits end, per kind:
 * [kind][0 signaled, 1 timed out, 2 infinite waits, 3 sum of finite timeouts ns,
 * 4 finite waits]. Updated under the sync lock. */
uint64_t hzn_wait_stats[2][5];
/* R71: timed condvar waits by thread and timeout: {thread, timeout ns, waits, timeouts}. */
uint64_t hzn_timed_waits[24][4];
static void count_wait(unsigned kind, int64_t ns, uint32_t result, uint64_t thread)
{
    if (kind == 0 && ns > 0) {
        for (size_t i = 0; i < sizeof hzn_timed_waits / sizeof hzn_timed_waits[0]; ++i) {
            uint64_t *slot = hzn_timed_waits[i];
            if (!slot[2] || (slot[0] == thread && slot[1] / 1000000 == (uint64_t)ns / 1000000)) {
                slot[0] = thread;
                slot[1] = (uint64_t)ns;
                ++slot[2];
                slot[3] += result == TIMED_OUT;
                break;
            }
        }
    }
    uint64_t *stats = hzn_wait_stats[kind];
    if (ns < 0)
        ++stats[2];
    else if (ns > 0) {
        ++stats[4];
        stats[3] += (uint64_t)ns;
    }
    ++stats[result == TIMED_OUT ? 1 : 0];
}

static uint32_t wait_key(HznKeySync *sync, KeyWait *wait, uint32_t *key, int64_t ns)
{
    if (ns != 0)
        enqueue(&sync->keys, wait);

    /* The SDK reads this flag without entering the kernel. It must be visible
     * before another thread can take the released mutex and signal the key. */
    store_word(key, 1);
    release_mutex(sync, wait->mutex, wait->mutex_address, wait->thread_id);
    if (ns == 0)
        return TIMED_OUT;

    const uint32_t result = block(sync, wait, ns);
    count_wait(0, ns, result, wait->thread_id);
    if (!wait->done) {
        /* A signaled waiter can time out while waiting to regain the mutex.
         * The SDK relocks it after timeout; Signal clears the key's hint. */
        remove_wait(&sync->keys, wait);
        remove_wait(&sync->mutexes, wait);
    }
    return result;
}

static void reacquire_mutex(HznKeySync *sync, KeyWait *wait)
{
    for (;;) {
        const uint32_t owner = load_word(wait->mutex);
        if (!owner) {
            if (!compare_word(wait->mutex, 0, wait->tag))
                continue;
            complete(sync, wait, 0);
            return;
        }

        /* Set the wait mask atomically so the guest's fast unlock cannot
         * clear a mutex while this waiter is being registered. */
        if (!(owner & HZN_KEY_SYNC_WAIT_MASK) &&
            !compare_word(wait->mutex, owner, owner | HZN_KEY_SYNC_WAIT_MASK))
            continue;
        wait->owner_id = thread_id(sync, owner & ~HZN_KEY_SYNC_WAIT_MASK);
        if (wait->owner_id)
            enqueue(&sync->mutexes, wait);
        else
            complete(sync, wait, INVALID_STATE);
        return;
    }
}

static void signal_key(HznKeySync *sync, uint32_t *key, uint64_t address, int32_t count)
{
    while (count != 0) {
        KeyWait **best = best_wait(&sync->keys, address, 0);
        if (!best)
            break;
        reacquire_mutex(sync, detach(best));
        if (count > 0)
            --count;
    }
    if (!best_wait(&sync->keys, address, 0))
        store_word(key, 0);
}

static uint32_t wait_for_address(HznKeySync *sync, uint32_t *word, KeyWait *wait,
                                 uint32_t type, uint64_t value, int64_t ns)
{
    if (type == WAIT_IF_EQUAL_64) {
        if (__atomic_load_n((const uint64_t *)word, __ATOMIC_SEQ_CST) != value)
            return INVALID_STATE;
    } else if (type == WAIT_IF_EQUAL) {
        if (load_word(word) != (uint32_t)value)
            return INVALID_STATE;
    } else {
        uint32_t current = load_word(word);
        for (;;) {
            if ((int32_t)current >= (int32_t)value)
                return INVALID_STATE;
            /* WaitIfLessThan reads the word. The decrementing variant changes
             * it in the same atomic operation as the successful comparison. */
            if (type == WAIT_IF_LESS || compare_word(word, current, current - 1))
                break;
            current = load_word(word);
        }
    }
    if (ns == 0)
        return TIMED_OUT;

    enqueue(&sync->addresses, wait);
    const uint32_t result = block(sync, wait, ns);
    count_wait(1, ns, result, 0);
    if (!wait->done)
        remove_wait(&sync->addresses, wait);
    return result;
}

static uint32_t signal_to_address(HznKeySync *sync, uint32_t *word, uint64_t address,
                                  uint32_t type, uint32_t value, int32_t count)
{
    if (type != SIGNAL) {
        uint32_t next_value = value + 1;
        if (type == SIGNAL_AND_MODIFY_BY_WAITERS) {
            size_t waiting = 0;
            for (KeyWait *wait = sync->addresses; wait; wait = wait->next)
                if (wait->wait_address == address)
                    ++waiting;
            /* Horizon 15 and later subtract one when the signal empties the queue. */
            if (waiting)
                next_value = count <= 0 || waiting <= (uint32_t)count ? value - 1 : value;
        }
        if (next_value == value ? load_word(word) != value : !compare_word(word, value, next_value))
            return INVALID_STATE;
    }

    while (true) {
        KeyWait **best = best_wait(&sync->addresses, address, 0);
        if (!best)
            break;
        complete(sync, detach(best), 0);
        if (count > 0 && --count == 0)
            break;
    }
    return 0;
}

static uint32_t address_svc(HznKeySync *sync, uint32_t number, const uint64_t x[32],
                            uint64_t current_thread_id, int32_t current_priority)
{
    const uint64_t address = x[0];
    const uint32_t type = (uint32_t)x[1];
    const bool signal = number == 0x35;
    const bool wide = !signal && type == WAIT_IF_EQUAL_64;
    if (address % (wide ? sizeof(uint64_t) : sizeof(uint32_t)))
        return INVALID_ADDRESS;
    if (type > (signal ? SIGNAL_AND_MODIFY_BY_WAITERS : WAIT_IF_EQUAL_64))
        return INVALID_ENUM;
    uint32_t *word = NULL;
    if (!signal || type != SIGNAL) {
        word = resolve_word(sync, address);
        if (!word)
            return INVALID_CURRENT_MEMORY;
        if (wide && ((uintptr_t)word % sizeof(uint64_t) ||
                     (uintptr_t)resolve_word(sync, address + sizeof(uint32_t)) !=
                         (uintptr_t)word + sizeof(uint32_t)))
            return INVALID_CURRENT_MEMORY;
    }

    KeyWait wait = {.wait_address = address, .priority = current_priority};
#ifndef _WIN32
    wait.has_cond = !signal && pthread_cond_init(&wait.cond, NULL) == 0;
#endif
    uint32_t result;
    lock(sync);
    if (sync->closing)
        result = TERMINATION_REQUESTED;
    else if (signal)
        result = signal_to_address(sync, word, address, type, (uint32_t)x[2], (int32_t)x[3]);
    else if (!current_thread_id)
        result = INVALID_HANDLE;
    else
        result = wait_for_address(sync, word, &wait, type, x[2], (int64_t)x[3]);
    unlock(sync);
#ifndef _WIN32
    if (wait.has_cond)
        pthread_cond_destroy(&wait.cond);
#endif
    return result;
}

bool hzn_key_sync_svc(HznKeySync *sync, uint32_t number, uint64_t x[32],
                     uint64_t current_thread_id, int32_t current_priority)
{
    if (!sync)
        return false;
    if (number == 0x34 || number == 0x35) {
        x[0] = address_svc(sync, number, x, current_thread_id, current_priority);
        return true;
    }
    if (number < 0x1a || number > 0x1d)
        return false;

    const uint64_t mutex_address = number == 0x1a ? x[1] : x[0];
    const uint64_t key_address = (number == 0x1c ? x[1] : x[0]) & ~UINT64_C(3);
    uint32_t result = 0;
    uint32_t *mutex = NULL, *key = NULL;
    if (number != 0x1d) {
        if (mutex_address % sizeof(uint32_t))
            result = INVALID_ADDRESS;
        else if (!(mutex = resolve_word(sync, mutex_address)))
            result = INVALID_CURRENT_MEMORY;
    }
    if (!result && (number == 0x1c || number == 0x1d)) {
        if (!(key = resolve_word(sync, key_address)) || key == mutex)
            result = INVALID_CURRENT_MEMORY;
    }
    if (result) {
        x[0] = result;
        return true;
    }

    KeyWait wait = {
        .mutex = mutex, .mutex_address = mutex_address, .wait_address = key_address,
        .thread_id = current_thread_id, .tag = (uint32_t)x[2], .priority = current_priority,
    };
#ifndef _WIN32
    wait.has_cond = (number == 0x1a || number == 0x1c) && pthread_cond_init(&wait.cond, NULL) == 0;
#endif
    lock(sync);
    if (sync->closing) {
        result = TERMINATION_REQUESTED;
    } else if (!current_thread_id && number != 0x1d) {
        result = INVALID_HANDLE;
    } else {
        switch (number) {
        case 0x1a:
            result = arbitrate_lock(sync, &wait, (uint32_t)x[0]);
            break;
        case 0x1b:
            release_mutex(sync, mutex, mutex_address, current_thread_id);
            break;
        case 0x1c:
            result = wait_key(sync, &wait, key, (int64_t)x[3]);
            break;
        case 0x1d:
            /* Horizon uses a nonpositive count to signal all waiters. */
            signal_key(sync, key, key_address, (int32_t)x[1] <= 0 ? -1 : (int32_t)x[1]);
            break;
        }
    }
    unlock(sync);
#ifndef _WIN32
    if (wait.has_cond)
        pthread_cond_destroy(&wait.cond);
#endif
    x[0] = result;
    return true;
}

