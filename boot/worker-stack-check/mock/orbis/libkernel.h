#ifndef POKOPIA_TEST_ORBIS_KERNEL_H
#define POKOPIA_TEST_ORBIS_KERNEL_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum { ORBIS_KERNEL_WB_ONION = 0, ORBIS_KERNEL_PROT_CPU_RW = 3 };

size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(off_t, off_t, size_t, size_t, int32_t, off_t *);
int32_t sceKernelReleaseDirectMemory(off_t, size_t);
int32_t sceKernelMapDirectMemory(void **, size_t, int32_t, int32_t, off_t, size_t);
int32_t sceKernelMapFlexibleMemory(void **, size_t, int32_t, int);
int32_t sceKernelMunmap(void *, size_t);
int32_t sceKernelMprotect(const void *, size_t, int);
int32_t scePthreadCreate(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *, const char *);

#endif
