/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "thread.h"
#include "event.h"

#include <nxrecomp/ctx.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <orbis/libkernel.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

enum {
    OUT_OF_RESOURCE = 0xce01,
    INVALID_ADDRESS = 0xcc01,
    INVALID_PRIORITY = 0xe001,
    INVALID_CORE_ID = 0xe201,
    INVALID_HANDLE = 0xe401,
    INVALID_COMBINATION = 0xe801,
    INVALID_STATE = 0xfa01,
    CORE_DONT_CARE = -1,
    CORE_PROCESS_DEFAULT = -2,
    CORE_NO_UPDATE = -3,
    GUEST_CORE_COUNT = 8,
};

static const uint32_t current_thread = 0xffff8000u;

typedef enum { CREATED, RUNNING, EXITED } ThreadState;

typedef struct {
    int32_t ideal;
    uint64_t mask;
} ThreadAffinity;

typedef struct HznWorker {
    HznThreads *threads;
    struct HznWorker *next;
    Ctx context;
    uint64_t id, entry, tls, tcb;
    uint32_t handle;
    int32_t priority;
    ThreadAffinity affinity;
    ThreadState state;
    bool open;
#ifdef _WIN32
    HANDLE native;
#else
    pthread_t native;
    void *host_stack;
    off_t host_stack_offset;
    bool host_stack_allocated, native_joined;
#endif
} HznWorker;

struct HznThreads {
    HznThreadConfig config;
    HznWorker *workers;
    uint64_t last_id;
    uint32_t next_handle;
    ThreadAffinity main_affinity;
    bool main_open, closing;
#ifdef _WIN32
    SRWLOCK lock;
    CONDITION_VARIABLE stop;
#else
    pthread_mutex_t lock;
    pthread_cond_t stop;
#endif
};

static void lock(HznThreads *threads)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&threads->lock);
#else
    pthread_mutex_lock(&threads->lock);
#endif
}

static void unlock(HznThreads *threads)
{
#ifdef _WIN32
    ReleaseSRWLockExclusive(&threads->lock);
#else
    pthread_mutex_unlock(&threads->lock);
#endif
}

static void mark_exited(HznWorker *worker)
{
    lock(worker->threads);
    worker->state = EXITED;
    hzn_events_exit_thread(worker->threads->config.events, worker->handle);
    unlock(worker->threads);
}

static _Noreturn void exit_worker(HznWorker *worker)
{
    mark_exited(worker);
#ifdef _WIN32
    _endthreadex(0);
    abort();
#else
    pthread_exit(NULL);
#endif
}

#ifdef _WIN32
static unsigned __stdcall run_worker(void *opaque)
#else
static void *run_worker(void *opaque)
#endif
{
    HznWorker *worker = opaque;
    HznThreads *threads = worker->threads;
    lock(threads);
    bool closing = threads->closing;
    unlock(threads);
    if (!closing)
        threads->config.dispatch(threads->config.opaque, &worker->context, worker->entry);
    mark_exited(worker);
    return 0;
}

#ifndef _WIN32
enum { HOST_STACK_BYTES = 4 * 1024 * 1024, HOST_DIRECT_STACK_BYTES = 2 * 1024 * 1024,
       HOST_STACK_GUARD = 0x4000 };
/* Keep direct-backed workers at the Orbis compatibility floor. The lifted
 * Windows build reaches this stage with a 1 MiB native stack reservation. */
static const size_t host_stack_mapping_bytes = HOST_DIRECT_STACK_BYTES + HOST_STACK_GUARD;

static void worker_error(HznWorker *worker, const char *operation, int32_t error)
{
    const HznThreadConfig *config = &worker->threads->config;
    if (config->worker_error)
        config->worker_error(config->opaque, worker->handle, operation, error);
}

static bool release_host_stack(HznWorker *worker)
{
    if (worker->host_stack) {
        int32_t result = sceKernelMunmap(worker->host_stack, host_stack_mapping_bytes);
        if (result) {
            worker_error(worker, "unmap direct stack", result);
            return false;
        }
        worker->host_stack = NULL;
    }
    if (worker->host_stack_allocated) {
        int32_t result = sceKernelReleaseDirectMemory(worker->host_stack_offset, host_stack_mapping_bytes);
        if (result) {
            worker_error(worker, "release direct stack", result);
            return false;
        }
        worker->host_stack_allocated = false;
    }
    return true;
}

static bool prepare_host_stack(HznWorker *worker, pthread_attr_t *attr)
{
    if (!release_host_stack(worker))
        return false;
    const char *operation = "allocate direct stack";
    int32_t result = sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(),
        host_stack_mapping_bytes, HOST_STACK_GUARD, ORBIS_KERNEL_WB_ONION, &worker->host_stack_offset);
    if (!result) {
        worker->host_stack_allocated = true;
        operation = "map direct stack";
        void *mapping = NULL;
        result = sceKernelMapDirectMemory(&mapping, host_stack_mapping_bytes, ORBIS_KERNEL_PROT_CPU_RW,
            0, worker->host_stack_offset, HOST_STACK_GUARD);
        if (!result) {
            worker->host_stack = mapping;
            operation = "protect direct stack guard";
            result = sceKernelMprotect(mapping, HOST_STACK_GUARD, 0);
        }
        if (!result) {
            operation = "pthread_attr_setstack";
            result = pthread_attr_setstack(attr, (uint8_t *)mapping + HOST_STACK_GUARD, HOST_DIRECT_STACK_BYTES);
        }
    }
    if (!result)
        return true;
    worker_error(worker, operation, result);
    release_host_stack(worker);
    return false;
}
#endif

static bool start_worker(HznWorker *worker)
{
#ifdef _WIN32
    worker->native = (HANDLE)_beginthreadex(NULL, 0, run_worker, worker, 0, NULL);
    return worker->native != NULL;
#else
    pthread_attr_t attr;
    int err = pthread_attr_init(&attr);
    if (err) {
        worker_error(worker, "pthread_attr_init", err);
        return false;
    }
    /* The host stack carries lifted C++ frames independently of the guest SP. */
    bool ready = true;
    if (worker->threads->config.direct_worker_stacks) {
        ready = prepare_host_stack(worker, &attr);
    } else {
        err = pthread_attr_setstacksize(&attr, HOST_STACK_BYTES);
        if (err)
            worker_error(worker, "pthread_attr_setstacksize", err);
        ready = err == 0;
    }
    if (ready) {
        err = pthread_create(&worker->native, &attr, run_worker, worker);
        if (err) {
            worker_error(worker, "pthread_create", err);
            release_host_stack(worker);
        }
    }
    pthread_attr_destroy(&attr);
    return ready && err == 0;
#endif
}

static bool join_worker(HznWorker *worker)
{
#ifdef _WIN32
    return WaitForSingleObject(worker->native, INFINITE) == WAIT_OBJECT_0 && CloseHandle(worker->native);
#else
    if (!worker->native_joined) {
        int32_t result = pthread_join(worker->native, NULL);
        if (result) {
            worker_error(worker, "pthread_join", result);
            return false;
        }
        worker->native_joined = true;
    }
    return true;
#endif
}

static bool release_worker(HznThreads *threads, HznWorker **link)
{
    HznWorker *worker = *link;
#ifndef _WIN32
    if (!release_host_stack(worker))
        return false;
#endif
    *link = worker->next;
    hzn_events_remove_thread(threads->config.events, worker->handle);
    threads->config.release_tls(threads->config.opaque, worker->tls, worker->tcb);
    free(worker);
    return true;
}

/* A running worker retains its object after CloseHandle. Only join workers
 * that have published EXITED, since they no longer need the table lock. */
static void reap_closed(HznThreads *threads)
{
    HznWorker **link = &threads->workers;
    while (*link) {
        HznWorker *worker = *link;
        if (!worker->open && (worker->state == CREATED || (worker->state == EXITED && join_worker(worker))) &&
            release_worker(threads, link))
            continue;
        link = &worker->next;
    }
}

static HznWorker *find_worker(HznThreads *threads, uint32_t handle)
{
    for (HznWorker *worker = threads->workers; worker; worker = worker->next)
        if (worker->open && worker->handle == handle)
            return worker;
    return NULL;
}

static HznWorker *calling_worker(HznThreads *threads, const Ctx *ctx)
{
    for (HznWorker *worker = threads->workers; worker; worker = worker->next)
        if (&worker->context == ctx)
            return worker;
    return NULL;
}

static uint64_t core_bit(int32_t core)
{
    return core >= 0 && core < GUEST_CORE_COUNT ? UINT64_C(1) << core : 0;
}

static bool valid_priority(uint64_t mask, int32_t priority)
{
    return priority >= 0 && priority < 64 && (mask & (UINT64_C(1) << priority));
}

static uint32_t create_worker(HznThreads *threads, Ctx *ctx)
{
    int32_t priority = (int32_t)ctx->x[4], core = (int32_t)ctx->x[5];
    if (!valid_priority(threads->config.process_priority_mask, priority))
        return INVALID_PRIORITY;
    if (core == CORE_PROCESS_DEFAULT)
        core = threads->config.main_core;
    uint64_t mask = core_bit(core);
    if (!(mask & threads->config.process_core_mask))
        return INVALID_CORE_ID;

    uint64_t entry = ctx->x[1], stack = ctx->x[3];
    if (!entry || (entry & 3) || !stack || (stack & 15))
        return INVALID_ADDRESS;
    uint32_t validation = threads->config.validate_start(threads->config.opaque, entry, stack);
    if (validation)
        return validation;

    reap_closed(threads);
    if (threads->next_handle >= threads->config.worker_handle_limit || threads->last_id == UINT64_MAX)
        return OUT_OF_RESOURCE;
    HznWorker *worker = calloc(1, sizeof *worker);
    if (!worker)
        return OUT_OF_RESOURCE;
    if (!threads->config.allocate_tls(threads->config.opaque, &worker->tls, &worker->tcb)) {
        free(worker);
        return OUT_OF_RESOURCE;
    }

    worker->threads = threads;
    worker->id = threads->last_id + 1;
    worker->handle = threads->next_handle;
    if (threads->config.events && !hzn_events_add_thread(threads->config.events, worker->handle, worker->id)) {
        threads->config.release_tls(threads->config.opaque, worker->tls, worker->tcb);
        free(worker);
        return OUT_OF_RESOURCE;
    }
    ++threads->last_id;
    ++threads->next_handle;
    worker->entry = entry;
    worker->priority = priority;
    worker->affinity = (ThreadAffinity){ core, mask };
    worker->open = true;
    worker->context.x[0] = ctx->x[2];
    worker->context.x[31] = stack;
    worker->context.tpidrro = worker->tls;
    worker->context.tpidr = worker->tcb;
    worker->context.ex_addr = UINT64_MAX;
    memcpy((void *)(uintptr_t)(worker->tls + 0x1f0), &worker->tcb, sizeof worker->tcb);
    worker->next = threads->workers;
    threads->workers = worker;
    ctx->x[1] = worker->handle;
    return 0;
}

static uint32_t start_handle(HznThreads *threads, uint32_t handle)
{
    if (handle == threads->config.main_handle && threads->main_open)
        return INVALID_STATE;
    HznWorker *worker = find_worker(threads, handle);
    if (!worker)
        return INVALID_HANDLE;
    if (worker->state != CREATED)
        return INVALID_STATE;
    if (!start_worker(worker))
        return OUT_OF_RESOURCE;
    worker->state = RUNNING;
    return 0;
}

static bool target_is_main(HznThreads *threads, const Ctx *ctx, uint32_t handle)
{
    return handle == current_thread ? ctx == threads->config.main_context :
        handle == threads->config.main_handle && threads->main_open;
}

static void get_thread_info(HznThreads *threads, Ctx *ctx, HznWorker *caller, uint32_t number)
{
    uint32_t handle = (uint32_t)ctx->x[1];
    HznWorker *target = handle == current_thread ? caller : find_worker(threads, handle);
    bool main = target_is_main(threads, ctx, handle);
    ctx->x[0] = main || target ? 0 : INVALID_HANDLE;
    ctx->x[1] = 0;
    if (target)
        ctx->x[1] = number == 0x25 ? target->id : (uint32_t)target->priority;
    else if (main)
        ctx->x[1] = number == 0x25 ? threads->config.main_id : (uint32_t)threads->config.main_priority;
}

static ThreadAffinity *find_affinity(HznThreads *threads, const Ctx *ctx, HznWorker *caller,
                                     uint32_t handle)
{
    if (target_is_main(threads, ctx, handle))
        return &threads->main_affinity;
    HznWorker *target = handle == current_thread ? caller : find_worker(threads, handle);
    return target ? &target->affinity : NULL;
}

static uint32_t set_affinity(HznThreads *threads, Ctx *ctx, HznWorker *caller)
{
    ThreadAffinity *affinity = find_affinity(threads, ctx, caller, (uint32_t)ctx->x[0]);
    if (!affinity)
        return INVALID_HANDLE;

    int32_t core = (int32_t)ctx->x[1];
    uint64_t mask = ctx->x[2];
    if (core == CORE_PROCESS_DEFAULT) {
        core = threads->config.main_core;
        mask = core_bit(core);
    } else {
        if (mask & ~threads->config.process_core_mask)
            return INVALID_CORE_ID;
        if (!mask)
            return INVALID_COMBINATION;
        if (core == CORE_NO_UPDATE) {
            core = affinity->ideal;
            if (!(core_bit(core) & mask))
                return INVALID_COMBINATION;
        } else if (core != CORE_DONT_CARE) {
            uint64_t ideal_bit = core_bit(core);
            if (!ideal_bit)
                return INVALID_CORE_ID;
            if (!(ideal_bit & mask))
                return INVALID_COMBINATION;
        }
    }
    *affinity = (ThreadAffinity){ core, mask };
    return 0;
}

/* Called with the lock held. Timed waits release it while sleeping. */
static void sleep_thread(HznThreads *threads, int64_t ns)
{
    if (ns <= 0) {
        unlock(threads);
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
        lock(threads);
        return;
    }
#ifdef _WIN32
    uint64_t ms = (uint64_t)ns / 1000000 + (ns % 1000000 != 0);
    uint64_t deadline = GetTickCount64() + ms;
    while (!threads->closing && ms) {
        DWORD timeout = ms < INFINITE ? (DWORD)ms : INFINITE - 1;
        if (!SleepConditionVariableSRW(&threads->stop, &threads->lock, timeout, 0) &&
            GetLastError() != ERROR_TIMEOUT)
            break;
        uint64_t now = GetTickCount64();
        ms = deadline > now ? deadline - now : 0;
    }
#else
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline))
        return;
    deadline.tv_sec += ns / 1000000000;
    deadline.tv_nsec += ns % 1000000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_nsec -= 1000000000;
        ++deadline.tv_sec;
    }
    while (!threads->closing && pthread_cond_timedwait(&threads->stop, &threads->lock, &deadline) == 0) {}
#endif
}

HznThreads *hzn_threads_create(const HznThreadConfig *config)
{
    if (!config || !config->main_context || !config->main_handle || !config->main_id ||
        config->worker_handle_base <= config->main_handle || config->worker_handle_base >= current_thread ||
        !config->validate_start || !config->allocate_tls || !config->release_tls || !config->dispatch)
        return NULL;
    uint64_t priority_mask = config->process_priority_mask ? config->process_priority_mask : UINT64_MAX;
    if (!valid_priority(priority_mask, config->main_priority))
        return NULL;
    uint64_t process_mask = config->process_core_mask ? config->process_core_mask : 7;
    uint64_t main_mask = core_bit(config->main_core);
    if ((process_mask >> GUEST_CORE_COUNT) || !(main_mask & process_mask))
        return NULL;
    uint32_t worker_limit = config->worker_handle_limit ? config->worker_handle_limit : current_thread;
    if (worker_limit <= config->worker_handle_base || worker_limit > current_thread)
        return NULL;

    HznThreads *threads = calloc(1, sizeof *threads);
    if (!threads)
        return NULL;
#ifdef _WIN32
    InitializeSRWLock(&threads->lock);
    InitializeConditionVariable(&threads->stop);
#else
    if (pthread_mutex_init(&threads->lock, NULL)) {
        free(threads);
        return NULL;
    }
    if (pthread_cond_init(&threads->stop, NULL)) {
        pthread_mutex_destroy(&threads->lock);
        free(threads);
        return NULL;
    }
#endif
    threads->config = *config;
    threads->config.process_core_mask = process_mask;
    threads->config.process_priority_mask = priority_mask;
    threads->config.worker_handle_limit = worker_limit;
    threads->main_affinity = (ThreadAffinity){ config->main_core, main_mask };
    threads->last_id = config->main_id;
    threads->next_handle = config->worker_handle_base;
    threads->main_open = true;
    if (config->events && !hzn_events_add_thread(config->events, config->main_handle, config->main_id)) {
#ifndef _WIN32
        pthread_cond_destroy(&threads->stop);
        pthread_mutex_destroy(&threads->lock);
#endif
        free(threads);
        return NULL;
    }
    return threads;
}

bool hzn_thread_svc(HznThreads *threads, Ctx *ctx, uint32_t number)
{
    if (!threads)
        return false;
    lock(threads);
    bool main = ctx == threads->config.main_context;
    HznWorker *caller = main ? NULL : calling_worker(threads, ctx);
    if (!main && !caller) {
        unlock(threads);
        return false;
    }
    if (caller && (threads->closing || number == 0x0a)) {
        unlock(threads);
        exit_worker(caller);
    }

    bool handled = true;
    switch (number) {
    case 0x08: /* CreateThread */
        ctx->x[0] = create_worker(threads, ctx);
        break;
    case 0x09: /* StartThread */
        ctx->x[0] = (uint32_t)ctx->x[0] == current_thread ? INVALID_STATE :
            start_handle(threads, (uint32_t)ctx->x[0]);
        break;
    case 0x0b: /* SleepThread */
        sleep_thread(threads, (int64_t)ctx->x[0]);
        if (caller && threads->closing) {
            unlock(threads);
            exit_worker(caller);
        }
        break;
    case 0x0c: /* GetThreadPriority */
    case 0x25: /* GetThreadId */
        get_thread_info(threads, ctx, caller, number);
        break;
    case 0x0e: { /* GetThreadCoreMask */
        ThreadAffinity *affinity = find_affinity(threads, ctx, caller, (uint32_t)ctx->x[2]);
        ctx->x[0] = affinity ? 0 : INVALID_HANDLE;
        ctx->x[1] = affinity ? (uint32_t)affinity->ideal : 0;
        ctx->x[2] = affinity ? affinity->mask : 0;
        break;
    }
    case 0x0f: /* SetThreadCoreMask */
        ctx->x[0] = set_affinity(threads, ctx, caller);
        break;
    case 0x16: { /* CloseHandle */
        uint32_t handle = (uint32_t)ctx->x[0];
        if (handle == threads->config.main_handle) {
            ctx->x[0] = threads->main_open ? 0 : INVALID_HANDLE;
            if (threads->main_open)
                hzn_events_close_thread(threads->config.events, handle);
            threads->main_open = false;
        } else if (handle >= threads->config.worker_handle_base && handle < threads->next_handle) {
            HznWorker *worker = find_worker(threads, handle);
            ctx->x[0] = worker ? 0 : INVALID_HANDLE;
            if (worker) {
                worker->open = false;
                hzn_events_close_thread(threads->config.events, handle);
                reap_closed(threads);
            }
        } else {
            handled = false;
        }
        break;
    }
    default:
        handled = false;
        break;
    }
    unlock(threads);
    return handled;
}

uint64_t hzn_thread_id(HznThreads *threads, const Ctx *caller, uint32_t handle)
{
    if (!threads)
        return 0;
    lock(threads);
    HznWorker *target = handle == current_thread ? calling_worker(threads, caller) : find_worker(threads, handle);
    uint64_t id = target ? target->id : (target_is_main(threads, caller, handle) ? threads->config.main_id : 0);
    unlock(threads);
    return id;
}

int32_t hzn_thread_priority(HznThreads *threads, const Ctx *caller)
{
    lock(threads);
    HznWorker *worker = calling_worker(threads, caller);
    int32_t priority = worker ? worker->priority : threads->config.main_priority;
    unlock(threads);
    return priority;
}

void hzn_threads_request_exit(HznThreads *threads)
{
    if (!threads)
        return;
    lock(threads);
    threads->closing = true;
#ifdef _WIN32
    WakeAllConditionVariable(&threads->stop);
#else
    pthread_cond_broadcast(&threads->stop);
#endif
    unlock(threads);
}

bool hzn_threads_destroy(HznThreads *threads)
{
    if (!threads)
        return true;
    hzn_threads_request_exit(threads);
    while (threads->workers) {
        HznWorker *worker = threads->workers;
        /* Closing prevents any further guest updates to the table. The
         * only worker state transition left is RUNNING to EXITED. */
        lock(threads);
        bool started = worker->state != CREATED;
        unlock(threads);
        if (started && !join_worker(worker))
            return false;
        lock(threads);
        bool released = release_worker(threads, &threads->workers);
        unlock(threads);
        if (!released)
            return false;
    }
#ifndef _WIN32
    pthread_cond_destroy(&threads->stop);
    pthread_mutex_destroy(&threads->lock);
#endif
    hzn_events_remove_thread(threads->config.events, threads->config.main_handle);
    free(threads);
    return true;
}
