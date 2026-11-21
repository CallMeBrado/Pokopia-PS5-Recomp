#define main nvn_front_regression_main
#include "../../gfx-bridge/test_nvn_front.c"
#undef main
#include "nvn_replay.h"
#include "nvn_guest_layout.h"

enum { REPORT = QUEUE_MEMORY + 0x200, SECOND_REPORT = REPORT + 16 };
static unsigned samples;

static uint64_t timestamp(void)
{
    return UINT64_C(1234567890) + 32 * ++samples;
}

static bool copy(void *opaque, const void *source, void *destination, size_t size)
{
    (void)opaque;
    assert(samples == 1 && size == 16);
    memcpy(destination, source, size);
    return true;
}

static bool clear(void *opaque, void *texture, VkRect2D area, const float rgba[4], uint32_t mask)
{
    (void)opaque; (void)texture; (void)area; (void)rgba; (void)mask;
    return false;
}

static bool draw(void *opaque, const PokopiaLiveDraw *call)
{
    (void)opaque; (void)call;
    return false;
}

static bool sync(void *opaque, void *native, bool wait)
{
    (void)opaque; (void)native; (void)wait;
    return false;
}

static bool finish(void *opaque)
{
    (void)opaque;
    return true;
}

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

static uint64_t replay_call(NvnReplay *replay, NvnFrontReplyKind expected, const char *name,
                            uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    uint64_t registers[32] = { a, b, c, d };
    assert(nvn_replay_dispatch(replay, name, registers, NULL) == expected);
    return registers[0];
}

int main(void)
{
    static Probe probe;
    const NvnFrontConfig front_config = {
        .guest_opaque = &probe, .backend_opaque = &probe, .read_guest = read_guest, .write_guest = guest,
        .device_create = create_device, .device_destroy = destroy_device,
        .queue_create = create_queue, .queue_destroy = destroy_queue,
        .command_buffer_create = create_command_buffer, .command_buffer_destroy = destroy_command_buffer,
    };
    NvnFront *front = nvn_front_create(&front_config);
    assert(front);
    const char get_proc_name[] = "nvnDeviceGetProcAddress";
    memcpy(guest(&probe, NAME, sizeof get_proc_name), get_proc_name, sizeof get_proc_name);
    const uint64_t loader = invoke(front, NVN_FRONT_SDK_BOOTSTRAP, NAME, 0, 0);
    invoke(front, 0x75005d64f0ULL, DEVICE_BUILDER, 0, 0);
    invoke(front, 0x75005d6500ULL, DEVICE_BUILDER, 0x200, 0);
    assert(invoke(front, get_proc(front, &probe, loader, 0, "nvnDeviceInitialize"), DEVICE, DEVICE_BUILDER, 0) == 1);

    invoke(front, 0x75005e2360ULL, POOL_BUILDER, 0, 0);
    invoke(front, get_proc(front, &probe, loader, DEVICE, "nvnMemoryPoolBuilderSetDevice"), POOL_BUILDER, DEVICE, 0);
    invoke(front, get_proc(front, &probe, loader, DEVICE, "nvnMemoryPoolBuilderSetStorage"), POOL_BUILDER, QUEUE_MEMORY, 0x1000);
    invoke(front, 0x75005e2450ULL, POOL_BUILDER, 0x22, 0);
    assert(invoke(front, get_proc(front, &probe, loader, DEVICE, "nvnMemoryPoolInitialize"), POOL, POOL_BUILDER, 0) == 1);

    invoke(front, 0x75005e82d0ULL, QUEUE_BUILDER, DEVICE, 0);
    invoke(front, 0x75005e82e0ULL, QUEUE_BUILDER, 0, 0);
    invoke(front, 0x75005e8310ULL, QUEUE_BUILDER, 4, 0);
    invoke(front, 0x75005e8320ULL, QUEUE_BUILDER, 0x10000, 0);
    invoke(front, 0x75005e8330ULL, QUEUE_BUILDER, 0, 0);
    invoke(front, 0x75005e8340ULL, QUEUE_BUILDER, 0x10000, 0);
    invoke(front, 0x75005e8360ULL, QUEUE_BUILDER, 0x8000, 0);
    invoke(front, 0x75005e8370ULL, QUEUE_BUILDER, QUEUE_MEMORY, 0x1000);
    assert(invoke(front, get_proc(front, &probe, loader, DEVICE, "nvnQueueInitialize"), QUEUE, QUEUE_BUILDER, 0) == 1);

    assert(invoke(front, get_proc(front, &probe, loader, DEVICE, "nvnCommandBufferInitialize"), COMMAND_BUFFER, DEVICE, 0) == 1);
    const uint64_t add_command = get_proc(front, &probe, loader, DEVICE, "nvnCommandBufferAddCommandMemory");
    uint64_t args[32] = { COMMAND_BUFFER, POOL, 0x100, 0x100 };
    assert(nvn_front_dispatch(front, add_command, args).kind == NVN_FRONT_HANDLED);
    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 0x100);

    const NvnReplayConfig config = {
        .front = front, .opaque = &probe, .read_guest = read_guest, .write_guest = guest,
        .copy = copy, .clear = clear, .draw = draw, .sync = sync, .finish = finish,
        .timestamp_ns = timestamp,
    };
    NvnReplay *replay = nvn_replay_create(&config);
    assert(replay);

    uint8_t *storage = guest(&probe, REPORT - 1, 34);
    memset(storage, 0xa5, 34);
    replay_call(replay, NVN_FRONT_HANDLED, "nvnCommandBufferBeginRecording", COMMAND_BUFFER, 0, 0, 0);
    replay_call(replay, NVN_FRONT_BOUNDARY, "nvnCommandBufferReportCounter", COMMAND_BUFFER, 1, REPORT, 0);
    replay_call(replay, NVN_FRONT_INVALID_GUEST, "nvnCommandBufferReportCounter", COMMAND_BUFFER, 0, REPORT + 1, 0);
    replay_call(replay, NVN_FRONT_INVALID_GUEST, "nvnCommandBufferReportCounter", COMMAND_BUFFER, 0, QUEUE_MEMORY + 0x1000, 0);
    replay_call(replay, NVN_FRONT_HANDLED, "nvnCommandBufferReportCounter", COMMAND_BUFFER, 0, REPORT, 0);
    replay_call(replay, NVN_FRONT_HANDLED, "nvnCommandBufferCopyBufferToBuffer", COMMAND_BUFFER, REPORT, SECOND_REPORT, 16);
    const uint64_t handle = replay_call(replay, NVN_FRONT_HANDLED, "nvnCommandBufferEndRecording", COMMAND_BUFFER, 0, 0, 0);
    memcpy(guest(&probe, OUTPUT, sizeof handle), &handle, sizeof handle);
    assert(samples == 0);

    probe.unreadable_byte = SECOND_REPORT;
    replay_call(replay, NVN_FRONT_INVALID_GUEST, "nvnQueueSubmitCommands", QUEUE, 1, OUTPUT, 0);
    assert(samples == 0);
    for (size_t i = 0; i < 34; ++i)
        assert(storage[i] == 0xa5);
    probe.unreadable_byte = 0;

    replay_call(replay, NVN_FRONT_HANDLED, "nvnQueueSubmitCommands", QUEUE, 1, OUTPUT, 0);
    uint64_t report[4];
    memcpy(report, storage + 1, sizeof report);
    assert(samples == 1 && report[0] == 0 && report[1] == UINT64_C(1234567922));
    assert(report[2] == report[0] && report[3] == report[1]);
    assert(storage[0] == 0xa5 && storage[33] == 0xa5);
    nvn_replay_destroy(replay);
    nvn_front_destroy(front);
    puts("PASS timestamp ABI, submission order, pool/range guards, and atomic preflight");
}
