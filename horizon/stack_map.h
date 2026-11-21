/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_STACK_MAP_H
#define POKOPIA_HORIZON_STACK_MAP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HZN_STACK_ALIAS_BYTES = 0x1000,
    HZN_STACK_MAP_OK = 0,
    HZN_STACK_MAP_INVALID_SIZE = 0xca01,
    HZN_STACK_MAP_INVALID_ADDRESS = 0xcc01,
    HZN_STACK_MAP_OUT_OF_MEMORY = 0xd001,
    HZN_STACK_MAP_INVALID_MEMORY_STATE = 0xd401
};

/* Owns one isolated source for the SDK system worker and one active alias.
 * Only the first 4 KiB of source and the 4 KiB at alias are guest pages. */
typedef struct {
    uint64_t source, alias;
    int32_t last_host_error;
    bool unsafe;

    /* Private reservation ownership and guest stack bounds. */
    uint64_t stack_base, stack_size;
    void *source_host, *alias_host;
} HznStackMap;

/* Allocate an exact, zeroed, isolated source for a fresh object. The source
 * must be 64 KiB aligned so Windows and Orbis can both reserve it. Caller
 * serializes operations against guest access, including guest thread teardown.
 * If cleanup fails, release may be called on the remaining owned reservations. */
bool hzn_stack_map_init(HznStackMap *map, uint64_t source, uint64_t stack_base, uint64_t stack_size);

/* Copy the source page to a free stack address and make the whole isolated
 * source host page inaccessible. Refuse addresses we do not own. */
uint32_t hzn_stack_map_map(HznStackMap *map, uint64_t dst, uint64_t src, uint64_t size);

/* Require the exact mapping, restore source access and copy the data back.
 * If unsafe is set, host rollback failed and guest execution must stop. */
uint32_t hzn_stack_map_unmap(HznStackMap *map, uint64_t dst, uint64_t src, uint64_t size);

/* Tear down owned reservations only, once all guest access has stopped. */
bool hzn_stack_map_release(HznStackMap *map);

#ifdef __cplusplus
}
#endif
#endif
