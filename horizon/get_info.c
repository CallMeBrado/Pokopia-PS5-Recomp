/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "get_info.h"

#include <stddef.h>

enum {
    HZN_INFO_CORE_MASK = 0,
    HZN_INFO_PRIORITY_MASK = 1,
    HZN_INFO_ALIAS_REGION_ADDRESS = 2,
    HZN_INFO_ALIAS_REGION_SIZE = 3,
    HZN_INFO_HEAP_REGION_ADDRESS = 4,
    HZN_INFO_HEAP_REGION_SIZE = 5,
    HZN_INFO_TOTAL_MEMORY_SIZE = 6,
    HZN_INFO_USED_MEMORY_SIZE = 7,
    HZN_INFO_RANDOM_ENTROPY = 11,
    HZN_INFO_ASLR_REGION_ADDRESS = 12,
    HZN_INFO_ASLR_REGION_SIZE = 13,
    HZN_INFO_STACK_REGION_ADDRESS = 14,
    HZN_INFO_STACK_REGION_SIZE = 15,
    HZN_INFO_SYSTEM_RESOURCE_SIZE = 16,
    HZN_INFO_SYSTEM_RESOURCE_USED = 17,
    HZN_INFO_TOTAL_NONSYSTEM_MEMORY_SIZE = 21,
    HZN_INFO_USED_NONSYSTEM_MEMORY_SIZE = 22,
    HZN_INFO_ALIAS_REGION_EXTRA_SIZE = 28,
    HZN_INFO_REMOTE_REGION_ADDRESS = 29,
    HZN_INFO_REMOTE_REGION_SIZE = 30,
    HZN_INFO_PROCESS_PAGE_SIZE = 33,
    HZN_INFO_ADDRESS_SPACE_SIZE = 36,

    HZN_INVALID_HANDLE = 0xe401,
    HZN_INVALID_COMBINATION = 0xe801,
};

static const uint32_t current_process = 0xffff8001u;
static const uint64_t remote_alignment = 0x10000;

static bool power_of_two(uint64_t value)
{
    return value && !(value & (value - 1));
}

static bool valid_range(HznVirtualRange range, uint64_t limit, uint64_t align)
{
    return range.size && range.base < limit && range.size <= limit - range.base &&
           !((range.base | range.size) & (align - 1));
}

static bool overlaps(HznVirtualRange left, HznVirtualRange right)
{
    return left.base < right.base + right.size && right.base < left.base + left.size;
}

bool hzn_get_info_init(HznGetInfoProcess *process, const HznGetInfoLayout *layout,
                       uint64_t entropy_seed)
{
    if (!process || !layout || !power_of_two(layout->address_space_size) ||
        !power_of_two(layout->page_size) || layout->page_size > layout->address_space_size)
        return false;

    uint64_t limit = layout->address_space_size;
    uint64_t page = layout->page_size;
    const HznVirtualRange regions[] = { layout->alias, layout->heap, layout->stack, layout->remote };
    if (!valid_range(layout->aslr, limit, page) ||
        !valid_range(layout->remote, limit, remote_alignment))
        return false;
    for (size_t i = 0; i < sizeof regions / sizeof regions[0]; ++i) {
        if (!valid_range(regions[i], limit, page))
            return false;
        for (size_t j = 0; j < i; ++j)
            if (overlaps(regions[i], regions[j]))
                return false;
    }
    *process = (HznGetInfoProcess){ .layout = *layout, .entropy_seed = entropy_seed };
    return true;
}

bool hzn_get_info_memory(HznGetInfoProcess *process, uint64_t total, uint64_t used)
{
    if (!total || used > total)
        return false;
    process->total_memory = total;
    process->used_memory = used;
    return true;
}

bool hzn_get_info(const HznGetInfoProcess *process, uint64_t regs[32])
{
    const HznGetInfoLayout *layout = &process->layout;
    const uint32_t type = (uint32_t)regs[1];
    const uint32_t handle = (uint32_t)regs[2];
    const uint64_t subtype = regs[3];
    const bool entropy = type == HZN_INFO_RANDOM_ENTROPY;
    uint64_t value;
    switch (type) {
    case HZN_INFO_CORE_MASK:
        if (!layout->core_mask) return false;
        value = layout->core_mask;
        break;
    case HZN_INFO_PRIORITY_MASK:
        if (!layout->priority_mask) return false;
        value = layout->priority_mask;
        break;
    case HZN_INFO_ALIAS_REGION_ADDRESS:  value = layout->alias.base; break;
    case HZN_INFO_ALIAS_REGION_SIZE:     value = layout->alias.size; break;
    case HZN_INFO_HEAP_REGION_ADDRESS:   value = layout->heap.base; break;
    case HZN_INFO_HEAP_REGION_SIZE:      value = layout->heap.size; break;
    case HZN_INFO_ASLR_REGION_ADDRESS:   value = layout->aslr.base; break;
    case HZN_INFO_ASLR_REGION_SIZE:      value = layout->aslr.size; break;
    case HZN_INFO_STACK_REGION_ADDRESS:  value = layout->stack.base; break;
    case HZN_INFO_STACK_REGION_SIZE:     value = layout->stack.size; break;
    case HZN_INFO_SYSTEM_RESOURCE_SIZE: value = layout->system_resource_size; break;
    case HZN_INFO_SYSTEM_RESOURCE_USED: value = 0; break;
    case HZN_INFO_ALIAS_REGION_EXTRA_SIZE: value = 0; break; /* This layout has no extra alias region. */
    case HZN_INFO_REMOTE_REGION_ADDRESS: value = layout->remote.base; break;
    case HZN_INFO_REMOTE_REGION_SIZE:    value = layout->remote.size; break;
    case HZN_INFO_PROCESS_PAGE_SIZE:     value = layout->page_size; break;
    case HZN_INFO_ADDRESS_SPACE_SIZE:    value = layout->address_space_size; break;
    case HZN_INFO_TOTAL_MEMORY_SIZE:
    case HZN_INFO_TOTAL_NONSYSTEM_MEMORY_SIZE:
        if (!process->total_memory)
            return false;
        value = process->total_memory;
        break;
    case HZN_INFO_USED_MEMORY_SIZE:
    case HZN_INFO_USED_NONSYSTEM_MEMORY_SIZE:
        if (!process->total_memory)
            return false;
        value = process->used_memory;
        break;
    case HZN_INFO_RANDOM_ENTROPY:
        /* Match the upstream runtime's stable, distinct values for Switch 2's
         * eight subtypes. The seed is sampled once at process startup. */
        value = process->entropy_seed ^ (subtype * UINT64_C(0x9e3779b97f4a7c15));
        break;
    default:
        return false;
    }

    uint32_t result = 0;
    if (subtype > (entropy ? 7u : 0u))
        result = HZN_INVALID_COMBINATION;
    else if (handle != (entropy ? 0u : current_process))
        result = HZN_INVALID_HANDLE;

    regs[0] = result;
    regs[1] = result ? 0 : value;
    return true;
}

