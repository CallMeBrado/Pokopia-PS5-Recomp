/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "host.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static DWORD thread_key;
static SRWLOCK kernel_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE checkpoint = CONDITION_VARIABLE_INIT;

bool pokopia_host_init(void)
{
    thread_key = TlsAlloc();
    return thread_key != TLS_OUT_OF_INDEXES;
}

void pokopia_host_destroy(void) { TlsFree(thread_key); }
bool pokopia_host_set_thread(void *value) { return TlsSetValue(thread_key, value) != 0; }
void *pokopia_host_thread(void) { return TlsGetValue(thread_key); }
void pokopia_host_lock(void) { AcquireSRWLockExclusive(&kernel_lock); }
void pokopia_host_unlock(void) { ReleaseSRWLockExclusive(&kernel_lock); }
bool pokopia_host_trylock(void) { return TryAcquireSRWLockExclusive(&kernel_lock) != 0; }
void pokopia_host_notify(void) { WakeAllConditionVariable(&checkpoint); }
size_t pokopia_host_page_size(void)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
}

bool pokopia_host_protect(uint64_t address, uint64_t size, uint32_t permission)
{
    DWORD previous;
    DWORD protection = permission == 3 ? PAGE_READWRITE : permission == 1 ? PAGE_READONLY : PAGE_NOACCESS;
    return VirtualProtect((void *)(uintptr_t)address, (SIZE_T)size, protection, &previous) != 0;
}

bool pokopia_host_wait(const bool *ready, const bool *failed, uint32_t timeout_ms)
{
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (!*ready && !*failed) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline || !SleepConditionVariableSRW(&checkpoint, &kernel_lock,
                                                         (DWORD)(deadline - now), 0))
            break;
    }
    return *ready && !*failed;
}
#else
#include <orbis/libkernel.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

static pthread_key_t thread_key;
static pthread_mutex_t kernel_lock;
static pthread_cond_t checkpoint;

bool pokopia_host_init(void)
{
    if (pthread_mutex_init(&kernel_lock, NULL))
        return false;
    if (!pthread_cond_init(&checkpoint, NULL)) {
        if (!pthread_key_create(&thread_key, NULL))
            return true;
        pthread_cond_destroy(&checkpoint);
    }
    pthread_mutex_destroy(&kernel_lock);
    return false;
}

void pokopia_host_destroy(void)
{
    pthread_key_delete(thread_key);
    pthread_cond_destroy(&checkpoint);
    pthread_mutex_destroy(&kernel_lock);
}

bool pokopia_host_set_thread(void *value) { return pthread_setspecific(thread_key, value) == 0; }
void *pokopia_host_thread(void) { return pthread_getspecific(thread_key); }
void pokopia_host_lock(void) { pthread_mutex_lock(&kernel_lock); }
void pokopia_host_unlock(void) { pthread_mutex_unlock(&kernel_lock); }
bool pokopia_host_trylock(void) { return pthread_mutex_trylock(&kernel_lock) == 0; }
void pokopia_host_notify(void) { pthread_cond_broadcast(&checkpoint); }
size_t pokopia_host_page_size(void) { return 0x4000; }

bool pokopia_host_protect(uint64_t address, uint64_t size, uint32_t permission)
{
    int protection = permission == 3 ? PROT_READ | PROT_WRITE : permission == 1 ? PROT_READ : PROT_NONE;
    return sceKernelMprotect((void *)(uintptr_t)address, size, protection) == 0;
}

bool pokopia_host_wait(const bool *ready, const bool *failed, uint32_t timeout_ms)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline))
        return false;
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    while (!*ready && !*failed)
        if (pthread_cond_timedwait(&checkpoint, &kernel_lock, &deadline))
            break;
    return *ready && !*failed;
}
#endif
