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
#include <errno.h>
#include <orbis/libkernel.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
#endif
#if defined(__PROSPERO__)
#include <pthread_np.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/cpuset.h>
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
    off_t *host_stack_pieces; /* R58: 16 KiB direct pieces when memory is fragmented */
    size_t host_stack_piece_count;
    atomic_int host_stack_startup;
    bool host_stack_allocated, native_needs_join;
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
enum {
    HOST_STACK_BYTES = 4 * 1024 * 1024,
    HOST_WORKER_STACK_BYTES = 1024 * 1024,
    HOST_SIGNAL_STACK_BYTES = 64 * 1024,
    HOST_STACK_GUARD = 0x4000,
    HOST_WORKER_STACK_OFFSET = 2 * HOST_STACK_GUARD + HOST_SIGNAL_STACK_BYTES,
    HOST_STACK_MAPPING_BYTES = HOST_WORKER_STACK_OFFSET + HOST_WORKER_STACK_BYTES,
    HOST_FLEXIBLE_RESERVE = 16 * 1024 * 1024,
    HOST_STACK_STARTING = -1,
};

/* The SDK declaration passes the output size by value. */
extern int32_t flexible_available(size_t *bytes) __asm__("sceKernelAvailableFlexibleMemorySize");

static bool join_worker(HznWorker *worker);

static void worker_error(HznWorker *worker, const char *operation, int32_t error)
{
    const HznThreadConfig *config = &worker->threads->config;
    if (config->worker_error)
        config->worker_error(config->opaque, worker->handle, operation, error);
}

static bool release_host_stack(HznWorker *worker)
{
    if (worker->host_stack) {
        int32_t result = sceKernelMunmap(worker->host_stack, HOST_STACK_MAPPING_BYTES);
        if (result) {
            worker_error(worker, "unmap worker stack", result);
            return false;
        }
        worker->host_stack = NULL;
    }
    while (worker->host_stack_piece_count) {
        const off_t piece = worker->host_stack_pieces[worker->host_stack_piece_count - 1];
        int32_t result = sceKernelReleaseDirectMemory(piece, HOST_STACK_GUARD);
        if (result) {
            worker_error(worker, "release direct stack piece", result);
            return false;
        }
        --worker->host_stack_piece_count;
    }
    free(worker->host_stack_pieces);
    worker->host_stack_pieces = NULL;
    if (worker->host_stack_allocated) {
        int32_t result = sceKernelReleaseDirectMemory(worker->host_stack_offset, HOST_STACK_MAPPING_BYTES);
        if (result) {
            worker_error(worker, "release direct stack", result);
            return false;
        }
        worker->host_stack_allocated = false;
    }
    return true;
}

/* R58: after the sparse heap has parked ~1.6 GiB of textures, free direct
 * memory is plentiful but scattered in 64 KiB holes, and R57's next thread
 * failed to find one contiguous 1.1 MiB stack (ENOMEM, 0x80020023), so the game
 * aborted. Build the stack from 16 KiB pieces mapped side by side in one
 * reserved range instead. Each piece is tracked for release at join. */
static bool map_piecewise_stack(HznWorker *worker)
{
    enum { MAP_NO_OVERWRITE = 0x80, PIECES = HOST_STACK_MAPPING_BYTES / HOST_STACK_GUARD };
    _Static_assert(HOST_STACK_MAPPING_BYTES % HOST_STACK_GUARD == 0, "stack pieces tile the mapping");
    const char *operation = "reserve stack range";
    void *base = NULL;
    int32_t result = sceKernelReserveVirtualRange(&base, HOST_STACK_MAPPING_BYTES, 0, HOST_STACK_GUARD);
    if (result) {
        worker_error(worker, operation, result);
        return false;
    }
    worker->host_stack = base; /* release_host_stack unmaps the whole range */
    worker->host_stack_pieces = calloc(PIECES, sizeof *worker->host_stack_pieces);
    if (!worker->host_stack_pieces) {
        worker_error(worker, "allocate stack piece table", (int32_t)ORBIS_KERNEL_ERROR_ENOMEM);
        return false;
    }
    for (size_t i = 0; i < PIECES; ++i) {
        uint8_t *const at = (uint8_t *)base + i * HOST_STACK_GUARD;
        off_t piece;
        operation = "allocate direct stack piece";
        result = sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), HOST_STACK_GUARD,
            HOST_STACK_GUARD, ORBIS_KERNEL_WB_ONION, &piece);
        if (result)
            break;
        worker->host_stack_pieces[worker->host_stack_piece_count++] = piece;
        operation = "map direct stack piece";
        result = sceKernelMunmap(at, HOST_STACK_GUARD); /* drop this part of the reservation */
        void *mapping = at;
        if (!result)
            result = sceKernelMapDirectMemory(&mapping, HOST_STACK_GUARD, ORBIS_KERNEL_PROT_CPU_RW,
                MAP_FIXED | MAP_NO_OVERWRITE, piece, HOST_STACK_GUARD);
        if (!result && mapping != at)
            result = (int32_t)ORBIS_KERNEL_ERROR_EFAULT;
        if (result)
            break;
    }
    if (!result)
        return true;
    worker_error(worker, operation, result);
    return false;
}

static bool map_direct_stack(HznWorker *worker)
{
    const char *operation = "allocate direct stack";
    int32_t result = sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(),
        HOST_STACK_MAPPING_BYTES, HOST_STACK_GUARD, ORBIS_KERNEL_WB_ONION, &worker->host_stack_offset);
    if (result == (int32_t)ORBIS_KERNEL_ERROR_ENOMEM || result == (int32_t)ORBIS_KERNEL_ERROR_EAGAIN)
        return map_piecewise_stack(worker); /* fragmented: assemble from pieces */
    if (!result) {
        worker->host_stack_allocated = true;
        operation = "map direct stack";
        void *mapping = NULL;
        result = sceKernelMapDirectMemory(&mapping, HOST_STACK_MAPPING_BYTES, ORBIS_KERNEL_PROT_CPU_RW,
            0, worker->host_stack_offset, HOST_STACK_GUARD);
        if (!result)
            worker->host_stack = mapping;
    }
    if (!result)
        return true;
    worker_error(worker, operation, result);
    return false;
}

static bool prepare_host_stack(HznWorker *worker, pthread_attr_t *attr)
{
    if (!release_host_stack(worker))
        return false;

    size_t available;
    if (!flexible_available(&available) && available >= HOST_FLEXIBLE_RESERVE + HOST_STACK_MAPPING_BYTES) {
        void *mapping = NULL;
        if (!sceKernelMapFlexibleMemory(&mapping, HOST_STACK_MAPPING_BYTES, ORBIS_KERNEL_PROT_CPU_RW, 0))
            worker->host_stack = mapping;
    }
    if (!worker->host_stack && !map_direct_stack(worker)) {
        release_host_stack(worker);
        return false;
    }

    /* Both stacks grow down into a guard, and remain mapped until join. */
    uint8_t *const base = worker->host_stack;
    const char *operation = "protect worker stack guard";
    int32_t result = sceKernelMprotect(base, HOST_STACK_GUARD, 0);
    if (!result)
        result = sceKernelMprotect(base + HOST_WORKER_STACK_OFFSET - HOST_STACK_GUARD, HOST_STACK_GUARD, 0);
    if (!result) {
        operation = "pthread_attr_setguardsize";
        result = pthread_attr_setguardsize(attr, 0);
    }
    if (!result) {
        operation = "pthread_attr_setstack";
        result = pthread_attr_setstack(attr, base + HOST_WORKER_STACK_OFFSET, HOST_WORKER_STACK_BYTES);
    }
    if (!result)
        return true;
    worker_error(worker, operation, result);
    release_host_stack(worker);
    return false;
}

#if defined(__PROSPERO__)
/* PS5: guest priority/affinity are tracked but were never applied to host
 * threads, so workers ran with whatever the platform handed them. The first
 * PS5 boot spent 177 s (PS4: 77 s) in one CPU-bound loading thread. Give
 * every worker the process's full CPU set explicitly and record what the
 * scheduler actually did in /data/pokopia/ps5-sched.txt. */
static pthread_once_t ps5_sched_once = PTHREAD_ONCE_INIT;
static cpuset_t ps5_worker_cpus;
static bool ps5_worker_cpus_valid;
static atomic_int ps5_workers_reported;

static unsigned long long ps5_cpuset_bits(const cpuset_t *set)
{
    unsigned long long bits = 0;
    for (int cpu = 0; cpu < 64 && cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, set))
            bits |= 1ull << cpu;
    return bits;
}

static void ps5_sched_log(const char *format, ...)
{
    FILE *log = fopen("/data/pokopia/ps5-sched.txt", "a");
    if (!log)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(log, format, args);
    va_end(args);
    fclose(log);
}

/* The SDK's cpuset_t is 256 bits, but the PS5 kernel rejects sizes other
 * than its own (ERANGE); Sony's libkernel passes 16 bytes. Probe once. */
static size_t ps5_cpuset_bytes;
static const size_t ps5_cpuset_sizes[] = {16, 8, 32};

static int ps5_get_process_cpus(cpuset_t *set, size_t *used)
{
    int err = ERANGE;
    for (size_t i = 0; i < sizeof ps5_cpuset_sizes / sizeof ps5_cpuset_sizes[0]; ++i) {
        CPU_ZERO(set);
        if (!cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, ps5_cpuset_sizes[i], set)) {
            *used = ps5_cpuset_sizes[i];
            return 0;
        }
        err = errno;
        if (err != ERANGE)
            break;
    }
    return err;
}

static int ps5_get_thread_cpus(cpuset_t *set, size_t *used)
{
    int err = ERANGE;
    for (size_t i = 0; i < sizeof ps5_cpuset_sizes / sizeof ps5_cpuset_sizes[0]; ++i) {
        CPU_ZERO(set);
        err = pthread_getaffinity_np(pthread_self(), ps5_cpuset_sizes[i], set);
        if (!err) {
            *used = ps5_cpuset_sizes[i];
            return 0;
        }
        if (err != ERANGE && err != EINVAL)
            break;
    }
    return err;
}

void hzn_thread_test_affinity(unsigned cpu_rank, bool enabled)
{
    static _Thread_local cpuset_t original;
    static _Thread_local size_t bytes;
    static _Thread_local bool requested, selected;
    if (enabled == requested)
        return;
    requested = enabled;
    if (enabled) {
        if (ps5_get_thread_cpus(&original, &bytes) != 0 || (unsigned)CPU_COUNT(&original) <= cpu_rank)
            return;
        cpuset_t chosen;
        CPU_ZERO(&chosen);
        unsigned rank = 0;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &original) && rank++ == cpu_rank) {
                CPU_SET(cpu, &chosen);
                break;
            }
        }
        const int rc = pthread_setaffinity_np(pthread_self(), bytes, &chosen);
        ps5_sched_log("benchmark rank=%u mask=%#llx set rc=%d\n", cpu_rank, ps5_cpuset_bits(&chosen), rc);
        selected = rc == 0;
    } else if (selected) {
        const int rc = pthread_setaffinity_np(pthread_self(), bytes, &original);
        ps5_sched_log("benchmark rank=%u restored mask=%#llx rc=%d\n", cpu_rank, ps5_cpuset_bits(&original), rc);
        selected = rc != 0;
    }
}

static void ps5_sched_init(void)
{
    cpuset_t process, self;
    size_t process_bytes = 0, self_bytes = 0;
    const int process_err = ps5_get_process_cpus(&process, &process_bytes);
    const int self_err = ps5_get_thread_cpus(&self, &self_bytes);
    int policy = -1;
    struct sched_param param = {0};
    const int sched_rc = pthread_getschedparam(pthread_self(), &policy, &param);
    int min_prio = sched_get_priority_min(policy), max_prio = sched_get_priority_max(policy);
    if (!process_err && CPU_COUNT(&process) > 0) {
        ps5_worker_cpus = process;
        ps5_cpuset_bytes = process_bytes;
        ps5_worker_cpus_valid = true;
    } else if (!self_err && CPU_COUNT(&self) > 0) {
        ps5_worker_cpus = self;
        ps5_cpuset_bytes = self_bytes;
        ps5_worker_cpus_valid = true;
    }
    ps5_sched_log("process cpuset err=%d bytes=%zu mask=%#llx count=%d\n", process_err, process_bytes,
                  ps5_cpuset_bits(&process), CPU_COUNT(&process));
    ps5_sched_log("main thread affinity err=%d bytes=%zu mask=%#llx; sched rc=%d policy=%d priority=%d range=%d..%d\n",
                  self_err, self_bytes, ps5_cpuset_bits(&self), sched_rc, policy, param.sched_priority, min_prio,
                  max_prio);
    ps5_sched_log("worker cpuset %s bytes=%zu mask=%#llx\n", ps5_worker_cpus_valid ? "applied" : "NOT applied",
                  ps5_cpuset_bytes, ps5_cpuset_bits(&ps5_worker_cpus));
}

static void ps5_sched_apply(HznWorker *worker, pthread_attr_t *attr)
{
    pthread_once(&ps5_sched_once, ps5_sched_init);
    if (!ps5_worker_cpus_valid)
        return;
    const int err = pthread_attr_setaffinity_np(attr, ps5_cpuset_bytes, &ps5_worker_cpus);
    if (err)
        ps5_sched_log("worker %#x: pthread_attr_setaffinity_np failed %d\n", (unsigned)worker->handle, err);
}

static void ps5_sched_report(const HznWorker *worker)
{
    if (atomic_fetch_add(&ps5_workers_reported, 1) >= 12)
        return;
    cpuset_t set;
    size_t bytes = 0;
    const int affinity_rc = ps5_get_thread_cpus(&set, &bytes);
    int policy = -1;
    struct sched_param param = {0};
    const int sched_rc = pthread_getschedparam(pthread_self(), &policy, &param);
    ps5_sched_log("worker %#x guest prio=%d ideal=%d: host affinity rc=%d mask=%#llx sched rc=%d policy=%d priority=%d\n",
                  (unsigned)worker->handle, worker->priority, worker->affinity.ideal, affinity_rc,
                  ps5_cpuset_bits(&set), sched_rc, policy, param.sched_priority);
}
#endif

static void *run_on_host_stack(void *opaque)
{
    HznWorker *worker = opaque;
#if defined(__PROSPERO__)
    ps5_sched_report(worker);
#endif
    uint8_t *signal = (uint8_t *)worker->host_stack + HOST_STACK_GUARD;
    for (size_t offset = 0; offset < HOST_SIGNAL_STACK_BYTES; offset += HOST_STACK_GUARD)
        ((volatile uint8_t *)signal)[offset] = 0;
    stack_t alt = {.ss_sp = signal, .ss_size = HOST_SIGNAL_STACK_BYTES, .ss_flags = 0};
    int result = 0;
    if (sigaltstack(&alt, NULL))
        result = errno ? errno : EINVAL;
    /* Publish before run_worker takes the thread-table lock held by StartThread. */
    atomic_store_explicit(&worker->host_stack_startup, result, memory_order_release);
    return result ? NULL : run_worker(worker);
}

static int32_t create_on_host_stack(HznWorker *worker, const pthread_attr_t *attr)
{
    atomic_init(&worker->host_stack_startup, HOST_STACK_STARTING);
    /* The compatibility pthread_create can replace explicit stacks below
     * its 2 MiB floor. This entry point preserves the supplied address. */
    int32_t result = scePthreadCreate(&worker->native, attr, run_on_host_stack, worker, "pokopia-worker");
    if (result) {
        worker_error(worker, "scePthreadCreate", result);
        return result;
    }
    worker->native_needs_join = true;
    while ((result = atomic_load_explicit(&worker->host_stack_startup, memory_order_acquire)) == HOST_STACK_STARTING)
        sched_yield();
    if (result)
        worker_error(worker, "sigaltstack", result);
    return result;
}
#endif

static bool start_worker(HznWorker *worker)
{
#ifdef _WIN32
    worker->native = (HANDLE)_beginthreadex(NULL, 0, run_worker, worker, 0, NULL);
    return worker->native != NULL;
#else
    if (!join_worker(worker))
        return false;
    pthread_attr_t attr;
    int err = pthread_attr_init(&attr);
    if (err) {
        worker_error(worker, "pthread_attr_init", err);
        return false;
    }
#if defined(__PROSPERO__)
    ps5_sched_apply(worker, &attr);
#endif
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
        if (worker->threads->config.direct_worker_stacks) {
            err = create_on_host_stack(worker, &attr);
            if (err && join_worker(worker))
                release_host_stack(worker);
        } else {
            err = pthread_create(&worker->native, &attr, run_worker, worker);
            if (err)
                worker_error(worker, "pthread_create", err);
            worker->native_needs_join = err == 0;
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
    if (worker->native_needs_join) {
        int32_t result = pthread_join(worker->native, NULL);
        if (result) {
            worker_error(worker, "pthread_join", result);
            return false;
        }
        worker->native_needs_join = false;
    }
    return true;
#endif
}

static bool release_worker(HznThreads *threads, HznWorker **link)
{
    HznWorker *worker = *link;
#ifndef _WIN32
    if (!join_worker(worker) || !release_host_stack(worker))
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
/* R70: SleepThread accuracy, by requested length: [bucket][count, requested
 * ns, actual ns]. Buckets: yield, <=1 ms, <=4 ms, <=17 ms, longer. Updated
 * under the threads lock; the loader reads it for the timeline. */
uint64_t hzn_sleep_stats[5][3];
/* R71: who sleeps >= 17 ms: guest thread id and return address, with counts. */
HznSleepSite hzn_long_sleeps[16];
static uint64_t sleep_clock_ns(void)
{
#ifdef _WIN32
    return 0;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
#endif
}

static void sleep_thread_inner(HznThreads *threads, int64_t ns);
static bool precise_main_sleep(HznThreads *threads, bool main, int64_t ns)
{
#if defined(__PROSPERO__)
    const HznThreadConfig *config = &threads->config;
    if (main && ns > 0 && ns <= 1000000 && !threads->closing && config->precise_sleep_clock &&
        config->use_precise_main_sleep && config->use_precise_main_sleep()) {
        const uint64_t deadline = config->precise_sleep_clock() + (uint64_t)ns;
        unlock(threads);
        while (config->precise_sleep_clock() < deadline)
            __builtin_ia32_pause();
        lock(threads);
        return true;
    }
#else
    (void)threads;
    (void)main;
    (void)ns;
#endif
    return false;
}

static void sleep_thread(HznThreads *threads, bool main, int64_t ns)
{
    const uint64_t start = sleep_clock_ns();
    if (!precise_main_sleep(threads, main, ns)) {
#if defined(__PROSPERO__) && defined(POKOPIA_FAST_SHORT_SLEEP)
        /* R71 experiment (off): short polls wake after a quarter of the request.
         * It did not shorten startup (r71a: same read rate, gameplay at 225 s vs
         * 235 s), so the first loading screen is not paced by 1 ms polls. */
        const int64_t slept = ns > 0 && ns <= 1000000 ? (ns / 4 > 50000 ? ns / 4 : 50000) : ns;
        sleep_thread_inner(threads, slept);
#else
        sleep_thread_inner(threads, ns);
#endif
    }
    const unsigned bucket = ns <= 0 ? 0 : ns <= 1000000 ? 1 : ns <= 4000000 ? 2 : ns <= 17000000 ? 3 : 4;
    ++hzn_sleep_stats[bucket][0];
    hzn_sleep_stats[bucket][1] += ns > 0 ? (uint64_t)ns : 0;
    hzn_sleep_stats[bucket][2] += sleep_clock_ns() - start;
}

static void sleep_thread_inner(HznThreads *threads, int64_t ns)
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
        if ((int64_t)ctx->x[0] > 17000000) {
            const uint64_t id = caller ? caller->id : threads->config.main_id, lr = ctx->x[30];
            for (size_t i = 0; i < sizeof hzn_long_sleeps / sizeof hzn_long_sleeps[0]; ++i) {
                HznSleepSite *site = &hzn_long_sleeps[i];
                if (!site->count || (site->id == id && site->lr == lr)) {
                    *site = (HznSleepSite){id, lr, (uint64_t)ctx->x[0], site->count + 1};
                    break;
                }
            }
        }
        sleep_thread(threads, main, (int64_t)ctx->x[0]);
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
