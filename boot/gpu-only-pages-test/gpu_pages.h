#ifndef POKOPIA_GPU_PAGES_TEST_H
#define POKOPIA_GPU_PAGES_TEST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    GPU_PAGES_TEXTURE, GPU_PAGES_BUFFER, GPU_PAGES_TEXTURE_DESC,
    GPU_PAGES_SAMPLER_DESC, GPU_PAGES_COMMAND, GPU_PAGES_CONTROL, GPU_PAGES_SCRATCH,
} GpuPagesKind;

bool gpu_pages_add(FILE *log, GpuPagesKind kind, uint64_t owner, uint64_t base, uint64_t bytes,
                   uint32_t pool_flags, bool shape_eligible);
bool gpu_pages_allow_alias(FILE *log, GpuPagesKind kind, uint64_t base, uint64_t bytes);
bool gpu_pages_remove(FILE *log, GpuPagesKind kind, uint64_t owner);
void gpu_pages_bound(FILE *log, uint64_t owner);
void gpu_pages_cpu_texture(FILE *log, uint64_t owner, const char *call);
bool gpu_pages_written(FILE *log, uint64_t owner, bool full, uint64_t heap_base, uint64_t heap_size,
                       const char *call);
void gpu_pages_report(FILE *log, const char *phase);
void gpu_pages_fault(uintptr_t address, uintptr_t operation, uintptr_t rip);

#endif
