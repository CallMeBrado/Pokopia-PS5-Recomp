/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "orbis_memory.h"
#include "../horizon/heap.h"

#include <inttypes.h>
#include <orbis/libkernel.h>

/* The bundled OpenOrbis header declares these output arguments by value.
 * Bind to the same exports using the pointer ABI from orbis-compat. */
extern int32_t orbis_direct_available(off_t start, off_t end, size_t alignment,
    off_t *offset, size_t *size) __asm__("sceKernelAvailableDirectMemorySize");
extern int32_t orbis_flexible_available(size_t *size)
    __asm__("sceKernelAvailableFlexibleMemorySize");

void pokopia_orbis_memory_census(FILE *log, const char *phase)
{
    const size_t direct_total = sceKernelGetDirectMemorySize();
    off_t direct_offset = 0;
    size_t direct_span = 0;
    size_t flexible_available = 0;

    /* The guest heap and RADV arena both require 2 MiB-aligned direct memory. */
    const int32_t direct_result = orbis_direct_available(0, (off_t)direct_total,
        HZN_HEAP_GRANULE, &direct_offset, &direct_span);
    const int32_t flexible_result = orbis_flexible_available(&flexible_available);

    fprintf(log, "Orbis memory [%s] direct total=%zu bytes alignment=0x%x "
        "query=0x%08" PRIx32, phase, direct_total, HZN_HEAP_GRANULE,
        (uint32_t)direct_result);
    if (direct_result == 0)
        fprintf(log, " span=%zu bytes offset=0x%" PRIx64, direct_span, (uint64_t)direct_offset);
    fprintf(log, "; flexible query=0x%08" PRIx32, (uint32_t)flexible_result);
    if (flexible_result == 0)
        fprintf(log, " available=%zu bytes", flexible_available);
    fputc('\n', log);
    fflush(log);
}
