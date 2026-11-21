#define nvn_front_get_command_buffer test_command_buffer
#define nvn_front_charge_command_memory test_charge
#include "nvn_replay.c"

#include <assert.h>
#include <stdio.h>

enum { ENCODER = 0x10000 };
static uint32_t mode;
static size_t used, capacity, control_used, control_capacity;

bool nvn_guest_texture_layout(const NvnFrontTextureConfig *config, uint64_t *size, uint64_t *alignment)
{
    (void)config; (void)size; (void)alignment;
    return false;
}

VkFormat nvn_guest_texture_vk_format(uint32_t format)
{
    (void)format;
    return VK_FORMAT_UNDEFINED;
}

bool test_command_buffer(NvnFront *front, uint64_t address, NvnFrontCommandBufferInfo *info)
{
    (void)front;
    if (address != ENCODER)
        return false;
    *info = (NvnFrontCommandBufferInfo){ .recording_mode = mode };
    return true;
}

bool test_charge(NvnFront *front, uint64_t address, size_t command, size_t control)
{
    (void)front;
    assert(address == ENCODER && used <= capacity && control_used <= control_capacity);
    if (command > capacity - used || control > control_capacity - control_used)
        return false;
    used += command;
    control_used += control;
    return true;
}

int main(void)
{
    Recording recording = { .encoder = ENCODER };
    NvnReplay replay = { .recordings = &recording };
    const struct BarrierCase {
        uint32_t mode, flags;
        size_t charge;
        NvnFrontReplyKind reply;
    } cases[] = {
        {0, 0x21, 28, NVN_FRONT_HANDLED},
        {1, 0x21,  0, NVN_FRONT_BOUNDARY},
        {2, 0x21,  0, NVN_FRONT_HANDLED},
        {3, 0x21, 28, NVN_FRONT_HANDLED},
        {4, 0x21,  0, NVN_FRONT_BOUNDARY},
        {5, 0x21, 20, NVN_FRONT_HANDLED},
        {0, 0x03, 24, NVN_FRONT_HANDLED},
        {0, 0x10, 24, NVN_FRONT_HANDLED},
        {5, 0x03, 16, NVN_FRONT_HANDLED},
        {5, 0x10, 16, NVN_FRONT_HANDLED},
        {0, 0x22,  0, NVN_FRONT_BOUNDARY},
        {0, 0x61,  0, NVN_FRONT_BOUNDARY},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const struct BarrierCase test = cases[i];
        const uint64_t x[32] = { ENCODER, test.flags };
        mode = test.mode;
        used = 0;
        capacity = test.charge;
        recording.command_segment = false;
        assert(record_barrier(&replay, x) == test.reply && used == test.charge);
        assert(recording.command_segment == (test.charge != 0));
        assert(!recording.count && !recording.operations);
    }
    const uint64_t x[32] = { ENCODER, 0x21 };
    used = 0;
    capacity = 24;
    mode = 0;
    assert(record_barrier(&replay, x) == NVN_FRONT_BOUNDARY && used == 0);

    const uint64_t indirect[32] = { ENCODER, 0x8 };
    for (mode = 0; mode <= 5; ++mode) {
        const bool valid = mode != 1 && mode != 4;
        const size_t expected = mode == 0 || mode == 3 ? 24 : mode == 5 ? 16 : 0;
        capacity = expected;
        control_capacity = expected ? 72 : 0;
        used = control_used = 0;
        recording.command_segment = false;
        assert(record_barrier(&replay, indirect) == (valid ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY));
        assert(used == expected && control_used == (expected ? 72 : 0));
        assert(recording.command_segment == (expected != 0));
        assert(!recording.count && !recording.operations);
    }
    mode = 0;
    capacity = 24;
    control_capacity = 71;
    used = control_used = 0;
    assert(recording.command_segment);
    assert(record_barrier(&replay, indirect) == NVN_FRONT_BOUNDARY);
    assert(recording.command_segment && !used && !control_used);

    replay.recordings = NULL;
    assert(record_barrier(&replay, x) == NVN_FRONT_INVALID_GUEST);
    puts("PASS NVN barrier flags, exact guest byte accounting and command budget rejection");
}
