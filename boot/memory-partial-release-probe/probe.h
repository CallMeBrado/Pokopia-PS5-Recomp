#ifndef POKOPIA_DIRECT_MEMORY_PROBE_H
#define POKOPIA_DIRECT_MEMORY_PROBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* The installed Orbis SDK declares off_t as signed 64-bit on PS4. */
typedef int64_t DirectOffset;
enum { PROBE_PAGE = 0x4000, PROBE_SLICE = 0x10000, PROBE_BYTES = 3 * PROBE_SLICE };

size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(DirectOffset start, DirectOffset end, size_t bytes,
                                     size_t alignment, int32_t type, DirectOffset *offset);
int32_t sceKernelAvailableDirectMemorySize(DirectOffset start, DirectOffset end, size_t alignment,
                                          DirectOffset *offset, size_t *bytes);
int32_t sceKernelReleaseDirectMemory(DirectOffset offset, size_t bytes);
int32_t sceKernelMapDirectMemory(void **address, size_t bytes, int32_t protection, int32_t flags,
                                DirectOffset offset, size_t alignment);
int32_t sceKernelMunmap(void *address, size_t bytes);
int32_t sceKernelQueryMemoryProtection(void *address, void **start, void **end, int32_t *protection);

bool run_direct_memory_proof(FILE *log);

#endif
