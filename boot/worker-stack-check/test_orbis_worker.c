#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>

static int mock_sigaltstack(const stack_t *, stack_t *);
#define sigaltstack mock_sigaltstack
#include "../../horizon/thread.c"
#undef sigaltstack

enum { MIB = 1024 * 1024, FLEX_CAPACITY = 55 * MIB, WORKERS = 63, CAPACITY = 128 };
typedef struct {
    void *address;
    off_t physical;
} Mapping;

static Mapping mappings[CAPACITY];
static off_t physical[CAPACITY], next_physical;
static size_t flexible_used, direct_used;
static unsigned flexible_maps, direct_maps, creates, frees, direct_releases, failures;
static bool fail_flex, fail_direct_map, fail_protect, fail_create, fail_alt, fail_unmap;
static _Thread_local stack_t alt_bounds;
static _Thread_local volatile sig_atomic_t on_alt;

int32_t sceKernelAvailableFlexibleMemorySize(size_t *bytes)
{
    *bytes = FLEX_CAPACITY - flexible_used;
    return 0;
}

size_t sceKernelGetDirectMemorySize(void)
{
    return 4096ull * MIB;
}

int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t bytes, size_t alignment,
                                      int32_t type, off_t *address)
{
    assert(!start && end == (off_t)sceKernelGetDirectMemorySize() &&
           bytes == HOST_STACK_MAPPING_BYTES && alignment == HOST_STACK_GUARD && type == ORBIS_KERNEL_WB_ONION);
    for (size_t i = 0; i < CAPACITY; ++i) {
        if (!physical[i]) {
            next_physical += (off_t)bytes;
            *address = physical[i] = next_physical;
            direct_used += bytes;
            return 0;
        }
    }
    return ENOMEM;
}

int32_t sceKernelReleaseDirectMemory(off_t address, size_t bytes)
{
    assert(address && bytes == HOST_STACK_MAPPING_BYTES);
    for (size_t i = 0; i < CAPACITY; ++i)
        assert(mappings[i].physical != address);
    for (size_t i = 0; i < CAPACITY; ++i) {
        if (physical[i] == address) {
            physical[i] = 0;
            direct_used -= bytes;
            ++direct_releases;
            return 0;
        }
    }
    return EINVAL;
}

static int map_stack(void **address, size_t bytes, off_t direct)
{
    assert(!*address && bytes == HOST_STACK_MAPPING_BYTES);
    for (size_t i = 0; i < CAPACITY; ++i) {
        if (!mappings[i].address) {
            uint8_t *raw = mmap(NULL, bytes + HOST_STACK_GUARD, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            assert(raw != MAP_FAILED);
            size_t head = (-(uintptr_t)raw) & (HOST_STACK_GUARD - 1);
            size_t tail = HOST_STACK_GUARD - head;
            if (head)
                assert(!munmap(raw, head));
            if (tail)
                assert(!munmap(raw + head + bytes, tail));
            *address = raw + head;
            mappings[i] = (Mapping){*address, direct};
            return 0;
        }
    }
    return ENOMEM;
}

int32_t sceKernelMapFlexibleMemory(void **address, size_t bytes, int32_t protection, int flags)
{
    assert(protection == ORBIS_KERNEL_PROT_CPU_RW && !flags);
    assert(FLEX_CAPACITY - flexible_used >= HOST_FLEXIBLE_RESERVE + bytes);
    if (fail_flex)
        return ENOMEM;
    int result = map_stack(address, bytes, 0);
    if (!result) {
        flexible_used += bytes;
        ++flexible_maps;
    }
    return result;
}

int32_t sceKernelMapDirectMemory(void **address, size_t bytes, int32_t protection, int32_t flags,
                                off_t offset, size_t alignment)
{
    assert(protection == ORBIS_KERNEL_PROT_CPU_RW && !flags && offset && alignment == HOST_STACK_GUARD);
    if (fail_direct_map)
        return ENOMEM;
    int result = map_stack(address, bytes, offset);
    if (!result)
        ++direct_maps;
    return result;
}

int32_t sceKernelMunmap(void *address, size_t bytes)
{
    assert(bytes == HOST_STACK_MAPPING_BYTES);
    if (fail_unmap) {
        fail_unmap = false;
        return EIO;
    }
    for (size_t i = 0; i < CAPACITY; ++i) {
        if (mappings[i].address == address) {
            if (!mappings[i].physical)
                flexible_used -= bytes;
            mappings[i] = (Mapping){0};
            ++frees;
            return munmap(address, bytes);
        }
    }
    return EINVAL;
}

int32_t sceKernelMprotect(const void *address, size_t bytes, int protection)
{
    assert(!(HOST_STACK_GUARD - 1 & (uintptr_t)address) && bytes == HOST_STACK_GUARD && !protection);
    if (fail_protect)
        return EPERM;
    return mprotect((void *)address, bytes, PROT_NONE);
}

int32_t scePthreadCreate(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *),
                        void *argument, const char *name)
{
    assert(!strcmp(name, "pokopia-worker"));
    void *address;
    size_t bytes, guard;
    assert(!pthread_attr_getstack(attr, &address, &bytes) && bytes == MIB);
    assert(!pthread_attr_getguardsize(attr, &guard) && !guard);
    assert(address == (uint8_t *)((HznWorker *)argument)->host_stack + HOST_WORKER_STACK_OFFSET);
    if (fail_create)
        return EAGAIN;
    ++creates;
    return pthread_create(thread, attr, start, argument);
}

static int mock_sigaltstack(const stack_t *in, stack_t *out)
{
    if (in && fail_alt) {
        errno = EINVAL;
        return -1;
    }
    return sigaltstack(in, out);
}

static void test_signal(int sig)
{
    (void)sig;
    char local;
    uintptr_t base = (uintptr_t)alt_bounds.ss_sp;
    uintptr_t address = (uintptr_t)&local;
    on_alt = address >= base && address - base < alt_bounds.ss_size;
}

typedef struct {
    Ctx main;
    HznThreads *threads;
    atomic_uint dispatched;
    unsigned tls_allocated, tls_released;
} Probe;

static void dispatch(void *opaque, Ctx *ctx, uint64_t entry)
{
    Probe *probe = opaque;
    assert(entry == 0x8000 && ctx->x[31] == 0x10000);
    pthread_attr_t attr;
    assert(!pthread_getattr_np(pthread_self(), &attr));
    void *base;
    size_t bytes;
    assert(!pthread_attr_getstack(&attr, &base, &bytes) && bytes == MIB);
    assert(!pthread_attr_destroy(&attr));
    assert(!sigaltstack(NULL, &alt_bounds) && alt_bounds.ss_size == HOST_SIGNAL_STACK_BYTES);
    assert((uint8_t *)alt_bounds.ss_sp + HOST_SIGNAL_STACK_BYTES + HOST_STACK_GUARD == base);
    assert(!raise(SIGUSR1) && on_alt);
    atomic_fetch_add_explicit(&probe->dispatched, 1, memory_order_release);
}

static bool allocate_tls(void *opaque, uint64_t *tls, uint64_t *tcb)
{
    Probe *probe = opaque;
    uint8_t *p = calloc(1, 0x2000);
    assert(p);
    *tls = (uintptr_t)p;
    *tcb = (uintptr_t)(p + 0x1000);
    ++probe->tls_allocated;
    return true;
}

static void release_tls(void *opaque, uint64_t tls, uint64_t tcb)
{
    Probe *probe = opaque;
    assert(tcb == tls + 0x1000);
    free((void *)(uintptr_t)tls);
    ++probe->tls_released;
}

static uint32_t validate(void *opaque, uint64_t entry, uint64_t stack)
{
    (void)opaque;
    return entry == 0x8000 && stack == 0x10000 ? 0 : INVALID_ADDRESS;
}

static void report_error(void *opaque, uint32_t handle, const char *operation, int32_t error)
{
    (void)opaque;
    assert(handle >= 0x20000 && operation && error);
    ++failures;
}

static void initialize(Probe *probe)
{
    HznThreadConfig config = {
        .opaque = probe, .main_context = &probe->main, .main_handle = 0x10000,
        .worker_handle_base = 0x20000, .main_id = 1, .main_priority = 44,
        .validate_start = validate, .allocate_tls = allocate_tls, .release_tls = release_tls,
        .dispatch = dispatch, .direct_worker_stacks = true, .worker_error = report_error,
    };
    probe->threads = hzn_threads_create(&config);
    assert(probe->threads);
}

static uint32_t create_worker_handle(Probe *probe)
{
    probe->main.x[1] = 0x8000;
    probe->main.x[2] = 0;
    probe->main.x[3] = 0x10000;
    probe->main.x[4] = 48;
    probe->main.x[5] = 0;
    assert(hzn_thread_svc(probe->threads, &probe->main, 8) && !probe->main.x[0]);
    return (uint32_t)probe->main.x[1];
}

static void start(Probe *probe, uint32_t handle, uint32_t result)
{
    probe->main.x[0] = handle;
    assert(hzn_thread_svc(probe->threads, &probe->main, 9) && probe->main.x[0] == result);
}

static void wait_dispatched(Probe *probe, unsigned count)
{
    while (atomic_load_explicit(&probe->dispatched, memory_order_acquire) != count)
        sched_yield();
}

int main(void)
{
    struct sigaction action = {.sa_handler = test_signal, .sa_flags = SA_ONSTACK};
    assert(!sigemptyset(&action.sa_mask) && !sigaction(SIGUSR1, &action, NULL));

    Probe pool = {0};
    initialize(&pool);
    for (unsigned i = 0; i < WORKERS; ++i)
        start(&pool, create_worker_handle(&pool), 0);
    wait_dispatched(&pool, WORKERS);
    assert(flexible_maps == 35 && direct_maps == 28 && creates == WORKERS);
    assert(flexible_used == 35 * HOST_STACK_MAPPING_BYTES && direct_used == 28 * HOST_STACK_MAPPING_BYTES);
    assert(FLEX_CAPACITY - flexible_used >= HOST_FLEXIBLE_RESERVE);
    printf("55 MiB pool: %u flexible, %u direct, %zu bytes flexible remained, %u alternate stack canaries\n",
           flexible_maps, direct_maps, FLEX_CAPACITY - flexible_used, creates);
    assert(hzn_threads_destroy(pool.threads));
    assert(!flexible_used && !direct_used && frees == WORKERS && direct_releases == 28);
    assert(pool.tls_allocated == WORKERS && pool.tls_released == WORKERS);

    Probe retry = {0};
    initialize(&retry);
    uint32_t handle = create_worker_handle(&retry);
    fail_flex = fail_direct_map = true;
    start(&retry, handle, OUT_OF_RESOURCE);
    assert(!flexible_used && !direct_used);
    fail_flex = fail_direct_map = false;
    fail_protect = fail_unmap = true;
    start(&retry, handle, OUT_OF_RESOURCE);
    assert(flexible_used == HOST_STACK_MAPPING_BYTES && !direct_used);
    fail_protect = false;
    fail_create = true;
    start(&retry, handle, OUT_OF_RESOURCE);
    assert(!flexible_used && !direct_used);
    fail_create = false;
    fail_alt = true;
    start(&retry, handle, OUT_OF_RESOURCE);
    assert(!flexible_used && !direct_used);
    fail_alt = false;
    fail_flex = true;
    start(&retry, handle, 0);
    wait_dispatched(&retry, 1);
    assert(!flexible_used && direct_used == HOST_STACK_MAPPING_BYTES);
    assert(hzn_threads_destroy(retry.threads));
    assert(!flexible_used && !direct_used && retry.tls_allocated == retry.tls_released && failures == 5);
    puts("Map fallback, map/guard/create/alt failures, failed unmap ownership, retry and teardown passed.");
}
