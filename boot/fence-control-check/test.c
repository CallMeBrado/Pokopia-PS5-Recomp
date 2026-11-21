
#define main original_front_test
#include "../../gfx-bridge/test_nvn_front.c"
#undef main
#include "nvn_replay.h"

static bool copy_unused(void *a, const void *b, void *c, size_t d)
{ (void)a; (void)b; (void)c; (void)d; return false; }
static bool clear_unused(void *a, void *b, VkRect2D c, const float d[4], uint32_t e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return false; }
static bool draw_unused(void *a, const PokopiaLiveDraw *b)
{ (void)a; (void)b; return false; }
static bool finish_unused(void *a)
{ (void)a; return false; }
static unsigned fences;
static bool sync_execute(void *opaque, void *sync, bool wait)
{
    Probe *probe = opaque;
    assert(sync == &probe->sync[0] && !wait);
    ++fences;
    return true;
}

int main(void)
{
    assert(original_front_test() == 0);
    static Probe probe;
    const NvnFrontConfig config = {
        .guest_opaque = &probe, .backend_opaque = &probe, .read_guest = read_guest, .write_guest = guest,
        .device_create = create_device, .device_destroy = destroy_device,
        .command_buffer_create = create_command_buffer, .command_buffer_destroy = destroy_command_buffer,
        .sync_create = create_sync, .sync_destroy = destroy_sync,
        .queue_create = create_queue, .queue_destroy = destroy_queue,
    };
    NvnFront *front = nvn_front_create(&config);
    assert(front);
    const char name[] = "nvnDeviceGetProcAddress";
    memcpy(guest(&probe, NAME, sizeof name), name, sizeof name);
    const uint64_t loader = invoke(front, NVN_FRONT_SDK_BOOTSTRAP, NAME, 0, 0);
    const uint64_t device_init = get_proc(front, &probe, loader, 0, "nvnDeviceInitialize");
    invoke(front, 0x75005d64f0ULL, DEVICE_BUILDER, 0, 0);
    invoke(front, 0x75005d6500ULL, DEVICE_BUILDER, 0x200, 0);
    assert(invoke(front, device_init, DEVICE, DEVICE_BUILDER, 0) == 1);
    const uint64_t sync_init = get_proc(front, &probe, loader, DEVICE, "nvnSyncInitialize");
    assert(invoke(front, sync_init, SYNC, DEVICE, 0) == 1);
    assert(invoke(front, 0x75005c48e0ULL, COMMAND_BUFFER, DEVICE, 0) == 1);
    const uint64_t callback = UINT64_C(0x7101144990), data = COMMAND_BUFFER - 0x10;
    invoke(front, 0x75005c4970ULL, COMMAND_BUFFER, callback, 0);
    invoke(front, 0x75005c4980ULL, COMMAND_BUFFER, data, 0);

    invoke(front, 0x75005e2360ULL, POOL_BUILDER, 0, 0);
    invoke(front, 0x75005e2350ULL, POOL_BUILDER, DEVICE, 0);
    invoke(front, 0x75005e2450ULL, POOL_BUILDER, 0x22, 0);
    invoke(front, 0x75005e2380ULL, POOL_BUILDER, QUEUE_MEMORY, 0x1000);
    assert(invoke(front, 0x75005e24a0ULL, POOL, POOL_BUILDER, 0) == 1);
    uint64_t x[32] = { COMMAND_BUFFER, POOL, 0x100, 0x100 };
    assert(nvn_front_dispatch(front, 0x75005c49c0ULL, x).kind == NVN_FRONT_HANDLED);
    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 64);

    const NvnReplayConfig replay_config = {
        .front = front, .opaque = &probe, .read_guest = read_guest, .write_guest = guest,
        .copy = copy_unused, .clear = clear_unused, .draw = draw_unused, .sync = sync_execute,
        .finish = finish_unused,
    };
    NvnReplay *replay = nvn_replay_create(&replay_config);
    assert(replay);
    x[0] = COMMAND_BUFFER;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBeginRecording", x, NULL) == NVN_FRONT_HANDLED);
    x[1] = 0;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBarrier", x, NULL) == NVN_FRONT_HANDLED);
    x[1] = SYNC;
    x[2] = x[3] = 0;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferFenceSync", x, NULL) == NVN_FRONT_BOUNDARY);
    NvnFrontCommandBufferInfo info;
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.command_used == 20 && info.control_used == 48 && info.control_revision == 1);
    NvnReplayControlRequest request;
    assert(nvn_replay_take_control_request(replay, &request));
    assert(request.encoder == COMMAND_BUFFER && request.function == callback && request.data == data &&
           request.minimum_bytes == 256 && request.control_revision == 1 && request.native == info.native);
    assert(!nvn_replay_take_control_request(replay, &request));
    assert(!fences);
    /* The ring may hand back the same address after retiring its old block. */
    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 256);
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferFenceSync", x, NULL) == NVN_FRONT_HANDLED);
    assert(!fences);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.command_used == 20 && info.control_used == 16 && info.control_revision == 2);
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferEndRecording", x, NULL) == NVN_FRONT_HANDLED);
    const uint64_t handle = x[0];
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info) && info.control_used == 24);

    const uint64_t queue_init = get_proc(front, &probe, loader, DEVICE, "nvnQueueInitialize");
    invoke(front, 0x75005e82d0ULL, QUEUE_BUILDER, DEVICE, 0);
    invoke(front, 0x75005e82e0ULL, QUEUE_BUILDER, 0, 0);
    invoke(front, 0x75005e8310ULL, QUEUE_BUILDER, 4, 0);
    invoke(front, 0x75005e8320ULL, QUEUE_BUILDER, 0x10000, 0);
    invoke(front, 0x75005e8330ULL, QUEUE_BUILDER, 0, 0);
    invoke(front, 0x75005e8340ULL, QUEUE_BUILDER, 0x10000, 0);
    invoke(front, 0x75005e8360ULL, QUEUE_BUILDER, 0x8000, 0);
    invoke(front, 0x75005e8370ULL, QUEUE_BUILDER, QUEUE_MEMORY, 0x1000);
    assert(invoke(front, queue_init, QUEUE, QUEUE_BUILDER, 0) == 1);
    memcpy(guest(&probe, OUTPUT, sizeof handle), &handle, sizeof handle);
    x[0] = QUEUE; x[1] = 1; x[2] = OUTPUT;
    assert(nvn_replay_dispatch(replay, "nvnQueueSubmitCommands", x, NULL) == NVN_FRONT_HANDLED);
    assert(fences == 1);
    nvn_replay_destroy(replay);
    nvn_front_destroy(front);
    puts("PASS fence reserve, old-segment charge, same-address refill, and one deferred fence execution");
}
