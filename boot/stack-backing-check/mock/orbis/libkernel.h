
#ifndef TEST_ORBIS_LIBKERNEL_H
#define TEST_ORBIS_LIBKERNEL_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#define ORBIS_KERNEL_ERROR_EFAULT 0x8002000eU
#define ORBIS_KERNEL_WB_ONION 0
#define ORBIS_KERNEL_PROT_CPU_RW 3
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelMprotect(void *address, size_t bytes, int protection);
int32_t sceKernelMunmap(void *address, size_t bytes);
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t bytes, size_t align, int32_t type, off_t *physical);
int32_t sceKernelMapDirectMemory(void **mapping, size_t bytes, int32_t protection, int32_t flags, off_t physical, size_t align);
int32_t sceKernelReleaseDirectMemory(off_t physical, size_t bytes);
#endif
