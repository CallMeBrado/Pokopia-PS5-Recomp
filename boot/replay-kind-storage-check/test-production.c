
#define main original_front_test
#include "../../gfx-bridge/test_nvn_front.c"
#undef main
#include <stdlib.h>

static bool fail_list_alloc;
static void *last_list_allocation;
static size_t last_list_size;

static void *fixture_malloc(size_t bytes)
{
    if (fail_list_alloc)
        return NULL;
    last_list_size = bytes;
    last_list_allocation = malloc(bytes);
    return last_list_allocation;
}

static void fixture_free(void *address)
{
    if (address == last_list_allocation)
        last_list_allocation = NULL;
    free(address);
}

#define malloc fixture_malloc
#define free fixture_free
#include "nvn_replay.c"
#undef free
#undef malloc

static bool copy_unused(void *a, const void *b, void *c, size_t d)
{ (void)a; (void)b; (void)c; (void)d; return false; }
static bool clear_unused(void *a, void *b, VkRect2D c, const float d[4], uint32_t e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return false; }
static bool draw_unused(void *a, const PokopiaLiveDraw *b)
{ (void)a; (void)b; return false; }
static bool finish_unused(void *a)
{ (void)a; return false; }
static unsigned fences, timestamps;
static uint64_t sample_timestamp(void)
{
    assert(fences == 4 + 2 * timestamps);
    return 1000 + ++timestamps;
}
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
        .finish = finish_unused, .timestamp_ns = sample_timestamp,
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
    NvnReplayMemoryRequest request;
    assert(nvn_replay_take_memory_request(replay, &request));
    assert(request.encoder == COMMAND_BUFFER && request.function == callback && request.data == data &&
           request.minimum_bytes == 256 && request.event == NVN_REPLAY_MEMORY_CONTROL && request.revision == 1 && request.native == info.native);
    assert(!nvn_replay_take_memory_request(replay, &request));
    assert(!fences);
    /* The ring may hand back the same address after retiring its old block. */
    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 256);
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferFenceSync", x, NULL) == NVN_FRONT_HANDLED);
    assert(!fences);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.command_used == 20 && info.control_used == 16 && info.control_revision == 2);
    Recording *active = *find_recording(replay, COMMAND_BUFFER);
    assert(active && active->count == 1 && active->capacity == 8);
    const uint64_t first_handle = replay->next_handle;
    fail_list_alloc = true;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferEndRecording", x, NULL) == NVN_FRONT_BOUNDARY);
    assert(x[0] == COMMAND_BUFFER && replay->next_handle == first_handle && !replay->lists);
    assert(*find_recording(replay, COMMAND_BUFFER) == active && active->count == 1);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info) && info.control_used == 16);
    assert(!nvn_replay_take_memory_request(replay, &request));
    fail_list_alloc = false;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferEndRecording", x, NULL) == NVN_FRONT_HANDLED);
    const uint64_t handle = x[0];
    assert(handle == first_handle && !*find_recording(replay, COMMAND_BUFFER));
    const RecordedList *ended = *find_list(replay, handle);
    assert(ended && ended->count == 1 && ended->operations[0] == OP_SYNC);
    assert(last_list_size == sizeof(RecordedList) + 1 + sizeof(Sync));
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
    assert(nvn_replay_dispatch(replay, "nvnQueueSubmitCommands", x, NULL) == NVN_FRONT_HANDLED);
    assert(fences == 2);
    const uint64_t invalid_batch[2] = { handle, handle + 8 };
    memcpy(guest(&probe, OUTPUT, sizeof invalid_batch), invalid_batch, sizeof invalid_batch);
    x[1] = 2;
    assert(nvn_replay_dispatch(replay, "nvnQueueSubmitCommands", x, NULL) == NVN_FRONT_INVALID_GUEST);
    assert(fences == 2);
    x[0] = COMMAND_BUFFER; x[1] = POOL; x[2] = 0x100; x[3] = 24;
    assert(nvn_front_dispatch(front, 0x75005c49c0ULL, x).kind == NVN_FRONT_HANDLED);
    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 48);
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBeginRecording", x, NULL) == NVN_FRONT_HANDLED);
    x[1] = 0;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBarrier", x, NULL) == NVN_FRONT_HANDLED);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    const uint64_t command_revision = info.command_revision, control_revision = info.control_revision;
    const uint64_t command_address = info.command_address;
    assert(info.command_used == 20 && info.control_used == 16);

    x[1] = 0x8;
    uint8_t *command_enabled = guest(&probe, COMMAND_BUFFER + 0x14, 1);
    *command_enabled = 0;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBarrier", x, NULL) == NVN_FRONT_BOUNDARY);
    assert(!nvn_replay_take_memory_request(replay, &request));
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.command_used == 20 && info.control_used == 16);
    *command_enabled = 1;

    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBarrier", x, NULL) == NVN_FRONT_BOUNDARY);
    assert(nvn_replay_take_memory_request(replay, &request));
    assert(request.event == NVN_REPLAY_MEMORY_CONTROL && request.minimum_bytes == 256 &&
           request.revision == control_revision && request.native == info.native);
    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 256);
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBarrier", x, NULL) == NVN_FRONT_BOUNDARY);
    assert(nvn_replay_take_memory_request(replay, &request));
    assert(request.event == NVN_REPLAY_MEMORY_COMMAND && request.minimum_bytes == 512 &&
           request.revision == command_revision && request.encoder == COMMAND_BUFFER &&
           request.function == callback && request.data == data && request.native == info.native);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.command_used == 20 && info.control_used == 32 && info.control_revision == control_revision + 1);

    uint64_t add[32] = {COMMAND_BUFFER, POOL, 0x100, 512};
    assert(nvn_front_dispatch(front, 0x75005c49c0ULL, add).kind == NVN_FRONT_HANDLED);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.command_address == command_address && info.command_revision == command_revision + 1);
    assert(info.command_used == 0 && info.control_used == 32);
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBarrier", x, NULL) == NVN_FRONT_HANDLED);
    assert(!nvn_replay_take_memory_request(replay, &request));
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.command_used == 24 && info.control_used == 104);
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferEndRecording", x, NULL) == NVN_FRONT_HANDLED);
    assert(nvn_front_get_command_buffer(front, COMMAND_BUFFER, &info));
    assert(info.command_used == 24 && info.control_used == 144);
    puts("PASS disabled command callback, dual memory exhaustion, exact callback minimums, same-address refill and single segment accounting");

    /* Compact list allocation is rolled back when the footer needs a refill. */
    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 32);
    x[0] = COMMAND_BUFFER;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBeginRecording", x, NULL) == NVN_FRONT_HANDLED);
    active = *find_recording(replay, COMMAND_BUFFER);
    const uint64_t empty_handle = replay->next_handle;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferEndRecording", x, NULL) == NVN_FRONT_BOUNDARY);
    assert(!last_list_allocation && replay->next_handle == empty_handle);
    assert(*find_recording(replay, COMMAND_BUFFER) == active && !active->count);
    assert(nvn_replay_take_memory_request(replay, &request));
    assert(request.event == NVN_REPLAY_MEMORY_CONTROL && request.minimum_bytes == 256);
    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 256);
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferEndRecording", x, NULL) == NVN_FRONT_HANDLED);
    assert(x[0] == empty_handle && last_list_size == sizeof(RecordedList));

    /* Same-address command refill did not retire an earlier handle. */
    const uint64_t live_batch[2] = { empty_handle, handle };
    memcpy(guest(&probe, OUTPUT, sizeof live_batch), live_batch, sizeof live_batch);
    x[0] = QUEUE; x[1] = 2; x[2] = OUTPUT;
    assert(nvn_replay_dispatch(replay, "nvnQueueSubmitCommands", x, NULL) == NVN_FRONT_HANDLED);
    assert(fences == 3);
    x[0] = DEVICE + 1; x[1] = handle;
    assert(nvn_replay_dispatch(replay, "nvnDeviceFinalizeCommandHandle", x, NULL) == NVN_FRONT_INVALID_GUEST);
    assert(*find_list(replay, handle) == ended);
    x[0] = DEVICE;
    assert(nvn_replay_dispatch(replay, "nvnDeviceFinalizeCommandHandle", x, NULL) == NVN_FRONT_HANDLED);
    assert(!*find_list(replay, handle));
    x[0] = QUEUE; x[1] = 2; x[2] = OUTPUT;
    assert(nvn_replay_dispatch(replay, "nvnQueueSubmitCommands", x, NULL) == NVN_FRONT_INVALID_GUEST);
    assert(fences == 3);
    puts("PASS compact empty and one-operation lists, reusable handles, validation and explicit retirement");
    puts("PASS list allocation failure and footer callback retries preserve active recording and handle sequence");
    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 256);
    x[0] = COMMAND_BUFFER;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBeginRecording", x, NULL) == NVN_FRONT_HANDLED);
    active = *find_recording(replay, COMMAND_BUFFER);
    Operation operations[OP_TIMESTAMP + 1] = {0};
    size_t exact = sizeof(RecordedList);
    for (unsigned kind = OP_COPY; kind <= OP_TIMESTAMP; ++kind) {
        Operation *operation = &operations[kind];
        operation->kind = (OperationKind)kind;
        const size_t bytes = operation_payload_size(operation->kind);
        memset(&operation->data, (int)(1 + kind), bytes);
        assert(append_operation(active, *operation));
        exact += 1 + bytes;
    }
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferEndRecording", x, NULL) == NVN_FRONT_HANDLED);
    const RecordedList *mixed = *find_list(replay, x[0]);
    assert(mixed && mixed->count == OP_TIMESTAMP + 1 && last_list_size == exact);
    const uint8_t *cursor = mixed->operations;
    printf("recording=%zu finalized_header=%zu working_operation=%zu packed_operations=",
           sizeof(Recording), sizeof(RecordedList), sizeof(Operation));
    for (unsigned kind = OP_COPY; kind <= OP_TIMESTAMP; ++kind) {
        const size_t bytes = operation_payload_size((OperationKind)kind);
        assert(*cursor++ == kind && !memcmp(cursor, &operations[kind].data, bytes));
        cursor += bytes;
        printf("%s%zu", kind ? "," : "", bytes + 1);
    }
    assert(cursor == (const uint8_t *)mixed + exact);
    puts("");
    puts("PASS all seven operation kinds survive unaligned mixed-list serialization");

    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 256);
    x[0] = COMMAND_BUFFER;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBeginRecording", x, NULL) == NVN_FRONT_HANDLED);
    active = *find_recording(replay, COMMAND_BUFFER);
    const uint64_t timestamp_address = QUEUE_MEMORY + 0xc00;
    const Operation sync = {.kind = OP_SYNC, .data.sync = {SYNC, &probe.sync[0], false}};
    const Operation timestamp = {.kind = OP_TIMESTAMP, .data.timestamp_address = timestamp_address};
    assert(append_operation(active, sync) && append_operation(active, timestamp) && append_operation(active, sync));
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferEndRecording", x, NULL) == NVN_FRONT_HANDLED);
    memcpy(guest(&probe, OUTPUT, sizeof x[0]), x, sizeof x[0]);
    x[0] = QUEUE; x[1] = 1; x[2] = OUTPUT;
    assert(nvn_replay_dispatch(replay, "nvnQueueSubmitCommands", x, NULL) == NVN_FRONT_HANDLED);
    assert(fences == 5 && timestamps == 1);
    assert(nvn_replay_dispatch(replay, "nvnQueueSubmitCommands", x, NULL) == NVN_FRONT_HANDLED);
    assert(fences == 7 && timestamps == 2);
    CounterReport report;
    memcpy(&report, guest(&probe, timestamp_address, sizeof report), sizeof report);
    assert(report.counter == 0 && report.timestamp == 1002);
    puts("PASS repeated mixed submissions executed fence, timestamp, fence in order");
    nvn_replay_destroy(replay);
    assert(!last_list_allocation);
    nvn_front_destroy(front);
    puts("PASS fence reserve, old-segment charge, same-address refill, and deferred fence execution");
}

