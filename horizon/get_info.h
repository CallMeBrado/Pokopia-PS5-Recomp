/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_GET_INFO_H
#define POKOPIA_HORIZON_GET_INFO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t base;
    uint64_t size;
} HznVirtualRange;

/* Guest virtual ranges. Their sizes do not promise physical backing. */
typedef struct {
    HznVirtualRange alias, heap, aslr, stack, remote;
    uint64_t address_space_size, page_size;
    uint64_t core_mask, priority_mask, system_resource_size;
} HznGetInfoLayout;

typedef struct {
    HznGetInfoLayout layout;
    uint64_t entropy_seed;
    uint64_t total_memory, used_memory;
} HznGetInfoProcess;

/* Acquires a fresh process seed from the host OS. Leaves seed untouched on failure. */
bool hzn_get_info_entropy_seed(uint64_t *seed);

/* Validates the guest layout before recording it and the process seed. */
bool hzn_get_info_init(HznGetInfoProcess *process, const HznGetInfoLayout *layout,
                       uint64_t entropy_seed);

/* Record the measured budget and guest committed memory. */
bool hzn_get_info_memory(HznGetInfoProcess *process, uint64_t total, uint64_t used);

/* AArch64 GetInfo ABI: w1 = type, w2 = handle, x3 = subtype.
 * A handled request writes w0 = result and x1 = value (zero on error).
 * An unsupported request returns false and leaves all registers unchanged. */
bool hzn_get_info(const HznGetInfoProcess *process, uint64_t regs[32]);

#ifdef __cplusplus
}
#endif
#endif
