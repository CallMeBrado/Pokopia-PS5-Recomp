/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "get_info.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint64_t process_handle = UINT64_C(0xffff8001);
static const HznGetInfoLayout layout = {
    .alias = { 0x6000000000, 0x200000000 },
    .heap = { 0x7800000000, 0x300000000 },
    .aslr = { 0x0008000000, 0x7ff8000000 },
    .stack = { 0x7c00000000, 0x100000000 },
    .remote = { 0x7200000000, 0x100000000 },
    .address_space_size = UINT64_C(1) << 39,
    .page_size = 0x1000,
    .core_mask = 0x3f, .priority_mask = UINT64_C(0x0ffffffff0000000),
};

static void expect(const HznGetInfoProcess *process, uint64_t type, uint64_t handle,
                   uint64_t subtype, uint64_t result, uint64_t value)
{
    uint64_t actual[32];
    for (size_t i = 0; i < sizeof actual / sizeof actual[0]; ++i)
        actual[i] = UINT64_MAX;
    actual[1] = type;
    actual[2] = handle;
    actual[3] = subtype;

    uint64_t expected[32];
    memcpy(expected, actual, sizeof expected);
    expected[0] = result;
    expected[1] = value;
    assert(hzn_get_info(process, actual));
    assert(!memcmp(actual, expected, sizeof actual));
}

static void test_layout(void)
{
    HznGetInfoProcess process;
    HznGetInfoLayout invalid = layout;
    assert(hzn_get_info_init(&process, &layout, 0));
    invalid.page_size = 0x3000;
    assert(!hzn_get_info_init(&process, &invalid, 0));
    invalid = layout;
    invalid.heap.size = UINT64_MAX;
    assert(!hzn_get_info_init(&process, &invalid, 0));
    invalid = layout;
    invalid.heap = invalid.stack;
    assert(!hzn_get_info_init(&process, &invalid, 0));
    invalid = layout;
    invalid.remote.base += layout.page_size;
    assert(!hzn_get_info_init(&process, &invalid, 0));
}

static void test_get_info(void)
{
    static const struct { uint32_t type; uint64_t value; } regions[] = {
        { 0, 0x3f }, { 1, UINT64_C(0x0ffffffff0000000) },
        { 2, 0x6000000000 }, { 3, 0x200000000 },
        { 4, 0x7800000000 }, { 5, 0x300000000 },
        { 12, 0x0008000000 }, { 13, 0x7ff8000000 },
        { 14, 0x7c00000000 }, { 15, 0x100000000 },
        { 16, 0 }, { 17, 0 },
        { 28, 0 }, { 29, 0x7200000000 }, { 30, 0x100000000 },
        { 33, 0x1000 }, { 36, UINT64_C(1) << 39 },
    };
    uint64_t seed = UINT64_C(0xa572fc021734ad6e);
    HznGetInfoProcess process;
    assert(hzn_get_info_init(&process, &layout, seed));
    for (size_t i = 0; i < sizeof regions / sizeof regions[0]; ++i)
        expect(&process, regions[i].type, process_handle, 0, 0, regions[i].value);

    /* The SDK passes 32-bit type and handle registers, but a 64-bit subtype. */
    expect(&process, UINT64_C(0xf00b00000004), UINT64_C(0xabcdffff8001),
           0, 0, layout.heap.base);
    expect(&process, 4, 0, 0, 0xe401, 0);
    expect(&process, 30, process_handle, UINT64_C(1) << 32, 0xe801, 0);

    for (uint64_t sub = 0; sub < 8; ++sub) {
        uint64_t value = seed ^ (sub * UINT64_C(0x9e3779b97f4a7c15));
        expect(&process, 11, 0, sub, 0, value);
        expect(&process, 11, 0, sub, 0, value);
    }
    expect(&process, 11, 0, 8, 0xe801, 0);
    expect(&process, 11, process_handle, 2, 0xe401, 0);

    uint64_t registers[32] = { 41, 21, process_handle };
    uint64_t before[32];
    memcpy(before, registers, sizeof registers);
    assert(!hzn_get_info(&process, registers));
    assert(!memcmp(before, registers, sizeof registers));

    const uint64_t total = UINT64_C(0x120000000);
    const uint64_t used = UINT64_C(0x4f78000);
    assert(!hzn_get_info_memory(&process, total, total + 1));
    assert(hzn_get_info_memory(&process, total, used));
    expect(&process, 6, process_handle, 0, 0, total);
    expect(&process, 7, process_handle, 0, 0, used);
    expect(&process, 21, process_handle, 0, 0, total);
    expect(&process, 22, process_handle, 0, 0, used);
    expect(&process, 22, 0, 0, 0xe401, 0);
}

static void test_host_entropy(void)
{
    uint64_t seed;
    assert(!hzn_get_info_entropy_seed(NULL));
    assert(hzn_get_info_entropy_seed(&seed));
    HznGetInfoProcess process;
    assert(hzn_get_info_init(&process, &layout, seed));
    expect(&process, 11, 0, 0, 0, seed);
}

int main(void)
{
    test_layout();
    test_get_info();
    test_host_entropy();
    puts("PASS Horizon GetInfo layout, registers, and host entropy");
    return 0;
}

