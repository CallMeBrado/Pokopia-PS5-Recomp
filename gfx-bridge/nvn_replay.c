/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nvn_replay.h"
#include "nvn_guest_layout.h"
#include "vk_program.h"
#include "vk_flush.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

static void note_skip(const char *text);

/* R62: set by the loader's pinned draw callback when it skipped a draw as
 * unsupported (VK_ERROR_FORMAT_NOT_SUPPORTED), so run_draw() can retry it on
 * the general path. Replay runs under the loader's kernel lock. */
bool nvn_replay_draw_unsupported;

typedef struct {
    uint64_t source, destination;
    size_t size;
} Copy;

typedef struct {
    uint64_t address;
    size_t size;
} BufferRange;

typedef struct {
    uint64_t address;
    void *native;
    float rgba[4];
    uint32_t mask;
    VkRect2D area;
} Clear;

typedef struct {
    uint64_t address;
    void *native;
    VkRect2D area;
    VkImageAspectFlags aspects;
    VkClearDepthStencilValue value;
} DepthClear;

/* R75: the world binds compute storage slot 2 and up; NVN allows 16 per stage. */
enum { TEXTURE_SLOTS = 32, COMPUTE_STORAGE_SLOTS = 16 };
enum { STENCIL_VALUE_MASK, STENCIL_WRITE_MASK, STENCIL_REFERENCE, STENCIL_VALUE_COUNT };

/* R61: what the general draw path keeps beyond the pinned path's snapshot. */
enum { GENERIC_UNIFORM_SLOTS = 8, GENERIC_VERTEX_TEXTURES = 16, GENERIC_STREAM_SLOTS = 8, GENERIC_ATTRIBUTE_SLOTS = 16 };

typedef struct {
    uint64_t target, depth_target, program[2], texture_pool, sampler_pool;
    uint64_t fragment_textures[TEXTURE_SLOTS];
    BufferRange vertex, index, uniform[2];
    PokopiaLiveDraw state;
    bool generic;
    uint8_t nvn_topology, index_size, attribute_count;
    uint32_t cull, depth_func;
    uint64_t vertex_textures[GENERIC_VERTEX_TEXTURES];
    BufferRange uniforms[2][GENERIC_UNIFORM_SLOTS], streams[GENERIC_STREAM_SLOTS];
    NvnFrontVertexAttribInfo attributes[GENERIC_ATTRIBUTE_SLOTS];
    NvnFrontVertexStreamInfo stream_info[GENERIC_STREAM_SLOTS];
    /* R63: multiple render targets. target is color0; more_targets[i] is color(i+1).
     * target_count 0 means a single target (the pinned recorder leaves it 0). */
    uint64_t more_targets[7];
    uint8_t target_count;
    uint8_t target_layers[8]; /* R64: array layer per colour target */
    uint8_t target_levels[8]; /* R73: mip level per colour target */
    uint8_t blend_mask;
    bool depth_bias; /* R73: polygon offset on filled polygons */
    float depth_bias_values[2];
    uint32_t color_write_masks;
} Draw;

typedef struct {
    uint64_t program, texture_pool, texture_handles[2];
    BufferRange uniform, storage[2];
    uint32_t groups[3];
    /* R73: general dispatch: every compute binding, in NVN slot order. */
    bool generic;
    uint64_t sampler_pool, textures[8], images[8];
    BufferRange uniforms[8], ssbo[COMPUTE_STORAGE_SLOTS];
} ComputeDispatch;

typedef struct {
    uint64_t address;
    void *native;
    bool wait;
} Sync;

typedef struct {
    uint64_t counter, timestamp;
} CounterReport;

/* nvnCommandBufferCopyTextureToTexture: the guest objects and the natives they
 * had at record time (rechecked at submit), plus the resolved copy. */
typedef struct {
    uint64_t source, destination;
    void *source_native, *destination_native;
    NvnReplayTextureCopy copy;
} TextureCopy;

typedef enum {
    OP_COPY, OP_CLEAR, OP_DEPTH_CLEAR, OP_DRAW, OP_DISPATCH, OP_SYNC, OP_TIMESTAMP, OP_COPY_TEXTURE,
} OperationKind;

typedef struct {
    OperationKind kind;
    union {
        Copy copy;
        Clear clear;
        DepthClear depth_clear;
        Draw draw;
        ComputeDispatch dispatch;
        Sync sync;
        uint64_t timestamp_address;
        TextureCopy copy_texture;
    } data;
} Operation;

enum {
    COLOR_TARGETS = 8, VERTEX_BUFFERS = NVN_FRONT_MAX_VERTEX_STREAMS,
    UNIFORM_BUFFERS = 16, PROGRAM_STAGES = NVN_SHADER_COMPUTE + 1,
};

typedef struct Recording {
    struct Recording *next;
    uint64_t encoder, device;
    uint64_t targets[COLOR_TARGETS];
    uint32_t target_layers[COLOR_TARGETS];
    uint32_t target_levels[COLOR_TARGETS]; /* R73 */
    uint64_t depth_target, texture_pool, sampler_pool;
    BufferRange scratch;
    uint64_t program[PROGRAM_STAGES];
    uint64_t textures[PROGRAM_STAGES][TEXTURE_SLOTS];
    BufferRange vertex[VERTEX_BUFFERS], uniform[PROGRAM_STAGES][UNIFORM_BUFFERS];
    BufferRange compute_storage[COMPUTE_STORAGE_SLOTS];
    uint64_t compute_images[8]; /* R73: BindImage handles for the compute stage */
    NvnFrontVertexAttribInfo attributes[NVN_FRONT_MAX_VERTEX_ATTRIBS];
    NvnFrontVertexStreamInfo streams[NVN_FRONT_MAX_VERTEX_STREAMS];
    uint32_t attribute_count, stream_count;
    float viewport[4], depth_range[2];
    int32_t scissor[4];
    bool viewport_set, scissor_set;
    uint32_t color_write_mask, cull, front_face, polygon_mode;
    uint32_t color_write_masks, blend_mask; /* R63: all eight targets */
    uint32_t raster_bound;
    uint32_t patch_size, depth_func;
    NvnFrontBlendInfo blend;
    float polygon_offset[3];
    NvnFrontStencilInfo stencil_faces[2];
    uint8_t stencil[STENCIL_VALUE_COUNT][2];
    bool blend_enable, depth_test, depth_write, stencil_test;
    bool unsupported_color, unsupported_polygon, unsupported_multisample;
    bool polygon_offset_fill; /* R73 */
    uint32_t target_count;
    /* R65: operations are kept as the same compact byte stream a recorded list
     * uses (kind byte, optional alignment padding, payload) instead of an array of
     * full-size Operation unions (~1.7 KiB each, even for a fence). At
     * EndRecording the stream becomes the list without another copy. R64
     * stopped when one long recording failed to double that array. */
    uint8_t *stream;
    size_t count, used, capacity, last_size;
    bool command_segment;
} Recording;

typedef struct RecordedList {
    struct RecordedList *next;
    uint64_t device, handle;
    size_t count;
    /* R63: bytes held and the last submit, for the bounded list cache. */
    size_t bytes;
    uint64_t last_use;
    /* R65: owned stream handed over from the recording. */
    uint8_t *operations;
    unsigned pinned; /* R76: executing; never evicted or freed meanwhile */
    size_t capacity; /* R78: bytes allocated for operations */
} RecordedList;

/* R78: operation streams are recycled. Each recording grew its stream from
 * 16 KiB by realloc and each evicted list freed it: ~11% of the game thread's
 * host time went to the allocator. */
enum { STREAM_POOL = 64, STREAM_POOL_BYTES = 48u << 20 };
static struct {
    uint8_t *data;
    size_t capacity;
} stream_pool[STREAM_POOL];
static unsigned stream_pool_count;
static size_t stream_pool_bytes;

static void stream_release(uint8_t *data, size_t capacity)
{
    if (!data)
        return;
    if (stream_pool_count < STREAM_POOL && capacity >= 16384 && stream_pool_bytes + capacity <= STREAM_POOL_BYTES) {
        stream_pool[stream_pool_count].data = data;
        stream_pool[stream_pool_count++].capacity = capacity;
        stream_pool_bytes += capacity;
        return;
    }
    free(data);
}

static uint8_t *stream_take(size_t need, size_t *capacity)
{
    unsigned best = stream_pool_count;
    for (unsigned i = 0; i < stream_pool_count; ++i)
        if (stream_pool[i].capacity >= need && (best == stream_pool_count || stream_pool[i].capacity > stream_pool[best].capacity))
            best = i;
    if (best == stream_pool_count)
        return NULL;
    uint8_t *data = stream_pool[best].data;
    *capacity = stream_pool[best].capacity;
    stream_pool_bytes -= *capacity;
    stream_pool[best] = stream_pool[--stream_pool_count];
    return data;
}

static size_t operation_payload_size(OperationKind kind)
{
    static const size_t sizes[] = {
        [OP_COPY] = sizeof(Copy),
        [OP_CLEAR] = sizeof(Clear),
        [OP_DEPTH_CLEAR] = sizeof(DepthClear),
        [OP_DRAW] = sizeof(Draw),
        [OP_DISPATCH] = sizeof(ComputeDispatch),
        [OP_SYNC] = sizeof(Sync),
        [OP_TIMESTAMP] = sizeof(uint64_t),
        [OP_COPY_TEXTURE] = sizeof(TextureCopy),
    };
    return (unsigned)kind < sizeof sizes / sizeof sizes[0] ? sizes[kind] : 0;
}

_Static_assert(OP_COPY_TEXTURE <= UINT8_MAX, "recorded operation kind must fit in one byte");

/* Only draws need aligned payloads. The replay reads those directly and keeps
 * the smaller command types packed. malloc and realloc align the stream base. */
static size_t operation_data_offset(size_t kind_offset, OperationKind kind)
{
    const size_t start = kind_offset + 1;
    const size_t mask = _Alignof(Draw) - 1;
    return kind == OP_DRAW ? (start + mask) & ~mask : start;
}

typedef enum {
    HOT_OTHER,
    HOT_DRAW_BASE_VERTEX,
    HOT_BIND_VERTEX,
    HOT_BIND_UNIFORM,
    HOT_BIND_TEXTURE,
    HOT_BIND_PROGRAM,
    HOT_BIND_ATTRIB_STATE,
    HOT_BIND_STREAM_STATE,
    HOT_DRAW_INSTANCED,
} HotReplayCommand;

typedef struct {
    const char *key;
    HotReplayCommand command;
} ReplayDispatchEntry;

struct NvnReplay {
    ReplayDispatchEntry dispatch_cache[256];
    NvnReplayConfig config;
    Recording *recordings;
    RecordedList *lists;
    NvnReplayMemoryRequest memory_request;
    uint64_t next_handle;
    /* R63: NVN command handles are plain memory the game reuses without ever
     * calling nvnDeviceFinalizeCommandHandle, so every recorded list was kept
     * forever. R62 stopped at EndRecording once that leak (and the bigger R61
     * draw records) exhausted the heap. Lists are now evicted least recently
     * submitted first once they hold more than LIST_CACHE_BYTES. */
    size_t list_bytes, list_count;
    uint64_t use_clock;
};

enum { LIST_CACHE_BYTES = 12u * 1024 * 1024, LIST_CACHE_KEEP = 64 };
const char *nvn_replay_boundary_reason;
uint64_t nvn_replay_lists_evicted, nvn_replay_evicted_submits;
size_t nvn_replay_cached_lists, nvn_replay_cached_list_bytes;

NvnReplay *nvn_replay_create(const NvnReplayConfig *config)
{
    if (!config || !config->front || !config->read_guest || !config->write_guest ||
        !config->copy || !config->clear || !config->draw || !config->sync || !config->finish)
        return NULL;
    NvnReplay *replay = calloc(1, sizeof *replay);
    if (replay) {
        replay->config = *config;
        replay->next_handle = UINT64_C(0x7f10000000);
    }
    return replay;
}

static void remove_recording(Recording **link)
{
    Recording *recording = *link;
    *link = recording->next;
    stream_release(recording->stream, recording->capacity);
    free(recording);
}

static void remove_list(NvnReplay *replay, RecordedList **link)
{
    RecordedList *list = *link;
    *link = list->next;
    replay->list_bytes -= list->bytes;
    --replay->list_count;
    stream_release(list->operations, list->capacity);
    nvn_replay_cached_list_bytes = replay->list_bytes;
    nvn_replay_cached_lists = replay->list_count;
    free(list);
}

/* R65: out of host heap. Drop the least recently submitted half of the lists
 * (keeping the newest LIST_CACHE_KEEP) so the allocation can be retried. */
static void evict_lists_for_heap(NvnReplay *replay)
{
    const size_t target = replay->list_count / 2;
    while (replay->list_count > LIST_CACHE_KEEP && replay->list_count > target) {
        RecordedList **oldest = NULL;
        size_t index = 0;
        for (RecordedList **link = &replay->lists; *link; link = &(*link)->next, ++index)
            if (index >= LIST_CACHE_KEEP && !__atomic_load_n(&(*link)->pinned, __ATOMIC_ACQUIRE) &&
                (!oldest || (*link)->last_use < (*oldest)->last_use))
                oldest = link;
        if (!oldest)
            break;
        remove_list(replay, oldest);
        ++nvn_replay_lists_evicted;
    }
}

/* Evict least recently submitted lists down to three quarters of the budget. */
static void trim_lists(NvnReplay *replay)
{
    if (replay->list_bytes <= LIST_CACHE_BYTES)
        return;
    while (replay->list_bytes > LIST_CACHE_BYTES / 4 * 3 && replay->list_count > LIST_CACHE_KEEP) {
        RecordedList **oldest = NULL;
        /* Skip the newest lists: they were just recorded and are about to be submitted. */
        size_t index = 0;
        for (RecordedList **link = &replay->lists; *link; link = &(*link)->next, ++index)
            if (index >= LIST_CACHE_KEEP && !__atomic_load_n(&(*link)->pinned, __ATOMIC_ACQUIRE) &&
                (!oldest || (*link)->last_use < (*oldest)->last_use))
                oldest = link;
        if (!oldest)
            break;
        remove_list(replay, oldest);
        ++nvn_replay_lists_evicted;
    }
}

void nvn_replay_destroy(NvnReplay *replay)
{
    if (!replay)
        return;
    while (replay->recordings)
        remove_recording(&replay->recordings);
    while (replay->lists)
        remove_list(replay, &replay->lists);
    free(replay);
}

static Recording **find_recording(NvnReplay *replay, uint64_t encoder)
{
    Recording **link = &replay->recordings;
    while (*link && (*link)->encoder != encoder)
        link = &(*link)->next;
    return link;
}

static RecordedList **find_list(NvnReplay *replay, uint64_t handle)
{
    RecordedList **link = &replay->lists;
    while (*link && (*link)->handle != handle)
        link = &(*link)->next;
    return link;
}

bool nvn_replay_take_memory_request(NvnReplay *replay, NvnReplayMemoryRequest *request)
{
    if (!replay || !request || !replay->memory_request.function)
        return false;
    *request = replay->memory_request;
    replay->memory_request = (NvnReplayMemoryRequest){0};
    return true;
}

enum { CONTROL_JUMP_RESERVE = 16, MIN_CONTROL_BLOCK = 256, MIN_COMMAND_BLOCK = 512 };

static NvnReplayMemoryRequest make_memory_request(const NvnReplayConfig *config, uint64_t encoder,
    const NvnFrontCommandBufferInfo *command, NvnReplayMemoryEvent event, size_t bytes)
{
    const bool control = event == NVN_REPLAY_MEMORY_CONTROL;
    const size_t reserve = control ? CONTROL_JUMP_RESERVE : 0;
    const size_t minimum = control ? MIN_CONTROL_BLOCK : MIN_COMMAND_BLOCK;
    if (!command->memory_callback || bytes > SIZE_MAX - reserve)
        return (NvnReplayMemoryRequest){0};
    const uint8_t *enabled = config->read_guest(config->opaque, encoder + 0x14 + event, 1);
    if (!enabled || !*enabled)
        return (NvnReplayMemoryRequest){0};
    bytes += reserve;
    return (NvnReplayMemoryRequest){
        .encoder = encoder, .function = command->memory_callback, .data = command->memory_callback_data,
        .minimum_bytes = bytes < minimum ? minimum : bytes, .event = event,
        .revision = control ? command->control_revision : command->command_revision, .native = command->native,
    };
}

static bool control_fits(NvnReplay *replay, uint64_t encoder, const NvnFrontCommandBufferInfo *command,
                         size_t bytes)
{
    const uint64_t free = command->control_size - command->control_used;
    if (!bytes || (free >= CONTROL_JUMP_RESERVE && bytes <= free - CONTROL_JUMP_RESERVE))
        return true;
    /* The SDK keeps the tail for AddControlMemory's jump record. */
    replay->memory_request = make_memory_request(&replay->config, encoder, command, NVN_REPLAY_MEMORY_CONTROL, bytes);
    return false;
}

static bool charge_memory(NvnReplay *replay, uint64_t encoder, size_t command_bytes, size_t control_bytes)
{
    NvnFront *front = replay->config.front;
    NvnFrontCommandBufferInfo command;
    if (!nvn_front_get_command_buffer(front, encoder, &command))
        return false;
    if (command_bytes > command.command_size - command.command_used) {
        const NvnReplayMemoryRequest request = make_memory_request(&replay->config, encoder, &command,
                                                                    NVN_REPLAY_MEMORY_COMMAND, command_bytes);
        if (!request.function)
            return false;
        Recording *recording = *find_recording(replay, encoder);
        if (recording && recording->command_segment) {
            /* A replacement command span closes the old segment. Account for
             * its control record before the callback can recycle that span. */
            if (!control_fits(replay, encoder, &command, 32) ||
                !nvn_front_charge_command_memory(front, encoder, 0, 32))
                return false;
            recording->command_segment = false;
        }
        replay->memory_request = request;
        return false;
    }
    return control_fits(replay, encoder, &command, control_bytes) &&
           nvn_front_charge_command_memory(front, encoder, command_bytes, control_bytes);
}

static NvnFrontReplyKind begin_recording(NvnReplay *replay, uint64_t encoder)
{
    NvnFrontCommandBufferInfo command;
    if (!nvn_front_get_command_buffer(replay->config.front, encoder, &command) ||
        *find_recording(replay, encoder))
        return NVN_FRONT_INVALID_GUEST;
    if (!command.command_size || !command.control_size)
        return NVN_FRONT_BOUNDARY;
    Recording *recording = calloc(1, sizeof *recording);
    if (!recording)
        return NVN_FRONT_BOUNDARY;
    if (!charge_memory(replay, encoder, 0, 16)) {
        free(recording);
        return NVN_FRONT_BOUNDARY;
    }
    recording->device = command.device;
    recording->encoder = encoder;
    recording->depth_range[1] = 1.0f;
    memset(recording->stencil[STENCIL_VALUE_MASK], 0xff, sizeof recording->stencil[STENCIL_VALUE_MASK]);
    memset(recording->stencil[STENCIL_WRITE_MASK], 0xff, sizeof recording->stencil[STENCIL_WRITE_MASK]);
    recording->next = replay->recordings;
    replay->recordings = recording;
#if defined(__PROSPERO__)
    pokopia_trace_producer("rec-open", 0, encoder, 0);
#endif
    return NVN_FRONT_HANDLED;
}

static bool resolve_copy(NvnReplay *replay, uint64_t device, Copy copy, const void **source, void **destination)
{
    const NvnReplayConfig *config = &replay->config;
    NvnFrontPoolHit hit;
    if (!nvn_front_resolve_pool(config->front, device, copy.source, copy.size, &hit) ||
        !nvn_front_resolve_pool(config->front, device, copy.destination, copy.size, &hit))
        return false;
    *source = config->read_guest(config->opaque, copy.source, copy.size);
    *destination = config->write_guest(config->opaque, copy.destination, copy.size);
    return *source && *destination;
}

static void evict_lists_for_heap(NvnReplay *replay);

static bool append_operation(NvnReplay *replay, Recording *recording, const Operation *operation)
{
    const size_t payload = operation_payload_size(operation->kind);
    if (!payload)
        return false;
    const size_t data_offset = operation_data_offset(recording->used, operation->kind);
    const size_t need = data_offset - recording->used + payload;
    if (!recording->stream) {
        size_t pooled = 0;
        uint8_t *data = stream_take(need, &pooled);
        if (data) {
            recording->stream = data;
            recording->capacity = pooled;
            recording->used = 0;
        }
    }
    if (recording->capacity - recording->used < need) {
        if (recording->capacity > SIZE_MAX / 4)
            return false;
        /* Grow by half: a long recording's doubling needed twice its size at once. */
        size_t capacity = recording->capacity ? recording->capacity + recording->capacity / 2 : 16384;
        if (capacity - recording->used < need)
            capacity = recording->used + need;
        uint8_t *stream = realloc(recording->stream, capacity);
        if (!stream) {
            evict_lists_for_heap(replay);
            stream = realloc(recording->stream, capacity);
        }
        if (!stream && recording->used + need * 64 < capacity) {
            capacity = recording->used + need * 64;
            stream = realloc(recording->stream, capacity);
        }
        if (!stream)
            return false;
        recording->stream = stream;
        recording->capacity = capacity;
    }
    recording->stream[recording->used] = (uint8_t)operation->kind;
    const void *data = operation->kind == OP_DRAW ? (const void *)&operation->data.draw : &operation->data;
    memcpy(recording->stream + data_offset, data, payload);
    recording->used += need;
    recording->last_size = need;
    ++recording->count;
    return true;
}

static bool charge_recording(NvnReplay *replay, Recording *recording, size_t command_bytes, size_t control_bytes)
{
    if (control_bytes && !command_bytes && recording->command_segment) {
        /* The SDK emits the pending segment first. A callback for the following
         * control packet must see the bytes already spent on that segment. */
        if (!charge_memory(replay, recording->encoder, 0, 32))
            return false;
        recording->command_segment = false;
    }
    const bool combined_segment = control_bytes && command_bytes;
    if (!charge_memory(replay, recording->encoder, command_bytes, control_bytes + (combined_segment ? 32 : 0)))
        return false;
    recording->command_segment = !control_bytes && (command_bytes || recording->command_segment);
    return true;
}

static NvnFrontReplyKind append_metered(NvnReplay *replay, Recording *recording, const Operation *operation,
                                       size_t command_bytes, size_t control_bytes)
{
    if (!append_operation(replay, recording, operation))
        return NVN_FRONT_BOUNDARY;
    if (charge_recording(replay, recording, command_bytes, control_bytes))
        return NVN_FRONT_HANDLED;
    --recording->count;
    recording->used -= recording->last_size;
    recording->last_size = 0;
    return NVN_FRONT_BOUNDARY;
}

static NvnFrontReplyKind record_copy(NvnReplay *replay, const uint64_t x[32])
{
    Recording *recording = *find_recording(replay, x[0]);
    if (!recording || x[3] > SIZE_MAX)
        return NVN_FRONT_INVALID_GUEST;
    if ((uint32_t)x[4])
        return NVN_FRONT_BOUNDARY;
    if (!x[3])
        return NVN_FRONT_HANDLED;
    const Copy copy = { .source = x[1], .destination = x[2], .size = (size_t)x[3] };
    const void *source;
    void *destination;
    if (!resolve_copy(replay, recording->device, copy, &source, &destination))
        return NVN_FRONT_INVALID_GUEST;
    NvnFrontCommandBufferInfo command;
    if (!nvn_front_get_command_buffer(replay->config.front, x[0], &command))
        return NVN_FRONT_INVALID_GUEST;
    Operation operation = { .kind = OP_COPY, .data.copy = copy };
    if (command.recording_mode == 1 || command.recording_mode == 4)
        return append_metered(replay, recording, &operation, 0, 32);
    const size_t packets = (copy.size - 1) / 0x3fffff + 1;
    return append_metered(replay, recording, &operation, 36 * packets + (command.recording_mode <= 4 ? 8 : 0), 0);
}

static CounterReport *resolve_counter_report(const NvnReplayConfig *config, uint64_t device, uint64_t address)
{
    NvnFrontPoolHit hit;
    if (!address || (address & (sizeof(CounterReport) - 1)) ||
        !nvn_front_resolve_pool(config->front, device, address, sizeof(CounterReport), &hit))
        return NULL;
    return config->write_guest(config->opaque, address, sizeof(CounterReport));
}

static NvnFrontReplyKind record_timestamp(NvnReplay *replay, const uint64_t x[32])
{
    Recording *recording = *find_recording(replay, x[0]);
    if (!recording)
        return NVN_FRONT_INVALID_GUEST;
    if ((uint32_t)x[1] != 0 || !replay->config.timestamp_ns)
        return NVN_FRONT_BOUNDARY;
    if (!resolve_counter_report(&replay->config, recording->device, x[2]))
        return NVN_FRONT_INVALID_GUEST;
    const Operation operation = { .kind = OP_TIMESTAMP, .data.timestamp_address = x[2] };
    /* A report semaphore packet carries four words after the method header. */
    const NvnFrontReplyKind result = append_metered(replay, recording, &operation, 20, 0);
    if (result != NVN_FRONT_BOUNDARY || replay->memory_request.function)
        return result; /* recorded, or the loader will grow command memory and retry */
    /* R62: R61 stopped here with no memory request pending. A timestamp only
     * feeds GPU timing, so write it now instead of stopping the game. */
    CounterReport *destination = resolve_counter_report(&replay->config, recording->device, x[2]);
    const CounterReport report = { .timestamp = replay->config.timestamp_ns() };
    memcpy(destination, &report, sizeof report);
    note_skip("timestamp written immediately: recording it failed without a memory request");
    return NVN_FRONT_HANDLED;
}

enum { TARGET_2D = 1, TARGET_2D_ARRAY = 4, TARGET_CUBE = 8, FORMAT_RGBA8 = 0x25, FORMAT_D16 = 0x32, FORMAT_D24S8 = 0x35 };

/* R50 skip-and-log. Counters are touched only under the loader's kernel
 * lock (all NVN dispatch holds it), so plain variables suffice. */
enum { SKIP_HASHES = 96 };
typedef enum { SKIP_BIND, SKIP_RECORD, SKIP_RUN, SKIP_CLEAR, SKIP_COPY, SKIP_KINDS } SkipKind;
static uint64_t skip_counts[SKIP_KINDS], skip_totals[SKIP_KINDS];
/* R53: short reason notes for skipped commands. R62: counted, and every
 * report prints the running total of each note that grew since the last one,
 * so the log shows which reasons are common rather than only that they occur. */
enum { SKIP_NOTES = 96 };
static char skip_notes[SKIP_NOTES][160];
static uint64_t skip_note_counts[SKIP_NOTES], skip_note_printed[SKIP_NOTES];
static size_t skip_note_count;

/* R79: recording (recording lock) and the replay worker (graphics lock) both
 * add notes, so appends take this lock. */
static pthread_mutex_t note_mutex = PTHREAD_MUTEX_INITIALIZER;

static void note_skip(const char *text)
{
    pthread_mutex_lock(&note_mutex);
    for (size_t i = 0; i < skip_note_count; ++i)
        if (!strcmp(skip_notes[i], text)) {
            ++skip_note_counts[i];
            pthread_mutex_unlock(&note_mutex);
            return;
        }
    if (skip_note_count < SKIP_NOTES) {
        snprintf(skip_notes[skip_note_count], sizeof skip_notes[0], "%s", text);
        skip_note_counts[skip_note_count++] = 1;
    }
    pthread_mutex_unlock(&note_mutex);
}
/* R73: frame trace. The loader sets nvn_replay_trace_submits (from the debug
 * mailbox) and every executed operation of that many submits is written to
 * nvn_replay_trace_log. */
FILE *nvn_replay_trace_log;
volatile int nvn_replay_trace_submits;
/* R73: after the next general draw whose fragment (or vertex) hash matches,
 * its targets and sampled textures are dumped to the debug mailbox. */
char nvn_replay_dump_hash[24];
unsigned nvn_replay_dump_width; /* R82: only a draw sampling a texture this wide */
unsigned nvn_replay_dump_height; /* R91: ... and this tall (0: any) */
/* R91: skip the first N distinct matching textures (several share a size). */
unsigned nvn_replay_dump_skip, nvn_replay_dump_seen_count;
static const void *dump_seen[16];
volatile int nvn_replay_dump_pending;
static bool tracing;
#define TRACE(...) do { if (tracing) fprintf(nvn_replay_trace_log, __VA_ARGS__); } while (0)

static void trace_texture(const char *label, const PokopiaVkTexture *texture)
{
    if (!tracing)
        return;
    if (!texture)
        fprintf(nvn_replay_trace_log, " %s=-", label);
    else
        fprintf(nvn_replay_trace_log, " %s=%p:f%d:%ux%ux%u", label, (const void *)texture, (int)texture->format,
                texture->extent.width, texture->extent.height, texture->array_layers);
}

static struct { char hash[24]; unsigned stage; uint64_t uses; bool reported; } skip_hashes[SKIP_HASHES];
static size_t skip_hash_count;

static void note_skipped_shader(unsigned stage, const char *hash)
{
    if (!hash)
        hash = "(no-hash)";
    pthread_mutex_lock(&note_mutex);
    for (size_t i = 0; i < skip_hash_count; ++i) {
        if (skip_hashes[i].stage == stage && !strcmp(skip_hashes[i].hash, hash)) {
            ++skip_hashes[i].uses;
            pthread_mutex_unlock(&note_mutex);
            return;
        }
    }
    if (skip_hash_count == SKIP_HASHES) {
        pthread_mutex_unlock(&note_mutex);
        return;
    }
    size_t bytes = strlen(hash);
    if (bytes >= sizeof skip_hashes[0].hash)
        bytes = sizeof skip_hashes[0].hash - 1;
    memcpy(skip_hashes[skip_hash_count].hash, hash, bytes);
    skip_hashes[skip_hash_count].hash[bytes] = 0;
    skip_hashes[skip_hash_count].stage = stage;
    skip_hashes[skip_hash_count].uses = 1;
    ++skip_hash_count;
    pthread_mutex_unlock(&note_mutex);
}

void nvn_replay_report_skips(FILE *log)
{
    static const char *const kinds[] = {"bind", "record", "run", "clear", "copy"};
    bool any = false;
    for (size_t i = 0; i < skip_note_count; ++i)
        any = any || skip_note_counts[i] != skip_note_printed[i];
    for (unsigned i = 0; i < SKIP_KINDS; ++i)
        any = any || skip_counts[i];
    for (size_t i = 0; i < skip_hash_count; ++i)
        any = any || !skip_hashes[i].reported;
    if (!any || !log)
        return;
    fputs("Graphics skipped", log);
    for (unsigned i = 0; i < SKIP_KINDS; ++i) {
        skip_totals[i] += skip_counts[i];
        fprintf(log, " %s=%llu(total %llu)", kinds[i], (unsigned long long)skip_counts[i],
                (unsigned long long)skip_totals[i]);
        skip_counts[i] = 0;
    }
    fprintf(log, " unsupported_shaders=%zu lists=%zu/%zuKiB evicted=%llu evicted_submits=%llu", skip_hash_count,
            nvn_replay_cached_lists, nvn_replay_cached_list_bytes / 1024,
            (unsigned long long)nvn_replay_lists_evicted, (unsigned long long)nvn_replay_evicted_submits);
    for (size_t i = 0; i < skip_hash_count; ++i) {
        if (!skip_hashes[i].reported) {
            fprintf(log, " new:%s/stage%u", skip_hashes[i].hash, skip_hashes[i].stage);
            skip_hashes[i].reported = true;
        }
    }
    for (size_t i = 0; i < skip_note_count; ++i)
        if (skip_note_counts[i] != skip_note_printed[i]) {
            fprintf(log, " note[%llu]:[%s]", (unsigned long long)skip_note_counts[i], skip_notes[i]);
            skip_note_printed[i] = skip_note_counts[i];
        }
    fputc('\n', log);
}

static uint32_t mip_size(uint32_t size, uint32_t level)
{
    size = level < 32 ? size >> level : 0;
    return size ? size : 1;
}

static bool get_attachment(NvnReplay *replay, uint64_t device, uint64_t address, NvnFrontTextureInfo *target)
{
    /* R64: mipmapped textures are accepted; the host attachment view
     * (PokopiaVkTexture.view) always selects mip level 0. R63 showed the scene
     * colour buffer has 9 levels, so every 3D draw was rejected here. */
    return nvn_front_get_texture(replay->config.front, address, target) && target->native &&
           target->create_info.config.device == device && target->create_info.config.levels >= 1;
}

/* R64: colour targets the general path can draw to: 2D, or one layer of a 2D
 * array texture in any colour format (get_color_target only allows RGBA8 arrays). */
static bool get_layered_color_target(NvnReplay *replay, uint64_t device, uint64_t address,
                                     NvnFrontTextureInfo *target)
{
    if (!get_attachment(replay, device, address, target))
        return false;
    const PokopiaVkTexture *texture = target->native;
    if (texture->aspects != VK_IMAGE_ASPECT_COLOR_BIT ||
        (texture->format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && texture->format <= VK_FORMAT_BC7_SRGB_BLOCK))
        return false;
    const NvnFrontTextureConfig *config = &target->create_info.config;
    /* R87: a cube map renders one face (a layer) at a time; the outdoor
     * environment map (256x256x6, 7 levels) the lighting pass samples for the
     * sky was never drawn, so the sky came out black. */
    return (config->target == TARGET_2D && config->depth == 1) ||
           (config->target == TARGET_2D_ARRAY && config->depth >= 1 && texture->array_layers == config->depth) ||
           (config->target == TARGET_CUBE && config->depth == 6 && texture->array_layers == 6);
}

static bool get_color_target(NvnReplay *replay, uint64_t device, uint64_t address, NvnFrontTextureInfo *target)
{
    if (!get_attachment(replay, device, address, target))
        return false;
    const PokopiaVkTexture *texture = target->native;
    if (texture->aspects != VK_IMAGE_ASPECT_COLOR_BIT ||
        (texture->format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && texture->format <= VK_FORMAT_BC7_SRGB_BLOCK))
        return false;
    const NvnFrontTextureConfig *config = &target->create_info.config;
    return (config->target == TARGET_2D && config->depth == 1) ||
           (config->target == TARGET_2D_ARRAY && config->format == FORMAT_RGBA8 && config->depth > 1);
}

static bool get_depth_target(NvnReplay *replay, uint64_t device, uint64_t address, NvnFrontTextureInfo *target)
{
    if (!get_attachment(replay, device, address, target))
        return false;
    const NvnFrontTextureConfig *config = &target->create_info.config;
    return config->target == TARGET_2D && config->depth == 1 &&
           (config->format == FORMAT_D16 || config->format == FORMAT_D24S8);
}

/* R73: a view may also pick one mip level; the intro's bloom/exposure chain
 * renders into levels 1..9 of a 10-level texture. R64-R72 rejected those views
 * but still recorded the texture, so the downsample draws went to level 0. */
static bool select_target_layer(NvnReplay *replay, uint64_t address, const NvnFrontTextureInfo *target,
                                uint32_t *selected_layer, uint32_t *selected_level)
{
    const NvnFrontTextureConfig *config = &target->create_info.config;
    *selected_layer = 0;
    *selected_level = 0;
    if (!address)
        return config->depth == 1;
    const uint8_t *view = replay->config.read_guest(replay->config.opaque, address, 16);
    if (!view)
        return false;
    uint16_t layer, layers;
    uint32_t format;
    memcpy(&layer, view, sizeof layer);
    memcpy(&layers, view + 2, sizeof layers);
    memcpy(&format, view + 8, sizeof format);
    const uint8_t fields = view[7];
    if (((fields & 1u) && (view[5] != 1 || view[4] >= config->levels)) ||
        ((fields & 4u) && format != config->format) ||
        ((fields & 32u) && view[6] != TARGET_2D && view[6] != config->target &&
         !(config->target == TARGET_CUBE && view[6] == TARGET_2D_ARRAY)))
        return false;
    if (fields & 1u)
        *selected_level = view[4];
    if (!(fields & 2u))
        return config->depth == 1;
    if (layers != 1 || layer >= config->depth)
        return false;
    *selected_layer = layer;
    return true;
}

static NvnFrontReplyKind set_render_targets(NvnReplay *replay, const uint64_t x[32])
{
    Recording *recording = *find_recording(replay, x[0]);
    uint32_t count = (uint32_t)x[1];
    if (!recording || count > COLOR_TARGETS)
        return NVN_FRONT_INVALID_GUEST;
    uint64_t targets[COLOR_TARGETS] = {0}, views[COLOR_TARGETS] = {0};
    uint32_t layers[COLOR_TARGETS] = {0}, levels[COLOR_TARGETS] = {0};
    const NvnReplayConfig *config = &replay->config;
    if (count) {
        size_t bytes = count * sizeof targets[0];
        const void *colors = config->read_guest(config->opaque, x[2], bytes);
        const void *view_array = x[3] ? config->read_guest(config->opaque, x[3], bytes) : NULL;
        if (!colors || (x[3] && !view_array))
            return NVN_FRONT_INVALID_GUEST;
        memcpy(targets, colors, bytes);
        if (view_array)
            memcpy(views, view_array, bytes);
    }
    NvnFrontTextureInfo target;
    uint32_t present = 0;
    /* R54: record targets the bridge cannot render to anyway, noting why. Draws
     * against them fail get_color_target()/get_depth_target() in record_draw()
     * and are skipped there, so the game keeps running. */
    bool unsupported = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (targets[i]) {
            if (!get_layered_color_target(replay, recording->device, targets[i], &target) ||
                !select_target_layer(replay, views[i], &target, &layers[i], &levels[i])) {
                char note[160];
                /* R63: say why: unknown texture, no host image, or mipmapped. */
                const bool known = nvn_front_get_texture(config->front, targets[i], &target);
                snprintf(note, sizeof note, "render-target color%u %s format=%#x target=%u depth=%u levels=%u view=%d",
                         i, !known ? "unknown-texture" : !target.native ? "no-host-image" :
                            target.create_info.config.levels != 1 ? "mipmapped" : "rejected",
                         known ? (unsigned)target.create_info.config.format : 0u,
                         known ? (unsigned)target.create_info.config.target : 0u,
                         known ? (unsigned)target.create_info.config.depth : 0u,
                         known ? (unsigned)target.create_info.config.levels : 0u, views[i] != 0);
                note_skip(note);
                layers[i] = 0;
                levels[i] = 0;
                unsupported = true;
            }
            ++present;
        }
    }
    if (x[4]) {
        uint32_t depth_layer, depth_level;
        if (!get_depth_target(replay, recording->device, x[4], &target) ||
            !select_target_layer(replay, x[5], &target, &depth_layer, &depth_level) || depth_level) {
            char note[160];
            const bool known = nvn_front_get_texture(config->front, x[4], &target);
            snprintf(note, sizeof note, "render-target depth %s format=%#x target=%u depth=%u levels=%u view=%d",
                     !known ? "unknown-texture" : !target.native ? "no-host-image" :
                        target.create_info.config.levels != 1 ? "mipmapped" : "rejected",
                     known ? (unsigned)target.create_info.config.format : 0u,
                     known ? (unsigned)target.create_info.config.target : 0u,
                     known ? (unsigned)target.create_info.config.depth : 0u,
                     known ? (unsigned)target.create_info.config.levels : 0u, x[5] != 0);
            note_skip(note);
            unsupported = true;
        }
    }
    if (unsupported)
        ++skip_counts[SKIP_CLEAR];
    /* The NVN2 depth attachment path replaces 16 bytes of
     * disable commands with 124 bytes, then records its 16-byte control entry. */
    if (!charge_recording(replay, recording, 76 + 32 * present + (x[4] ? 108 : 0), x[4] ? 16 : 0))
        return NVN_FRONT_BOUNDARY;
    memcpy(recording->targets, targets, sizeof targets);
    memcpy(recording->target_layers, layers, sizeof layers);
    memcpy(recording->target_levels, levels, sizeof levels);
    recording->target_count = count;
    recording->depth_target = x[4];
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind set_scratch(NvnReplay *replay, const uint64_t x[32])
{
    Recording *recording = *find_recording(replay, x[0]);
    if (!recording || x[2] > INT64_MAX || x[3] > SIZE_MAX)
        return NVN_FRONT_INVALID_GUEST;
    NvnFrontPoolConfig pool;
    if (!nvn_front_get_pool(replay->config.front, x[1], &pool) || pool.device != recording->device ||
        x[2] > pool.size || x[3] > pool.size - x[2] || x[2] > UINT64_MAX - pool.storage)
        return NVN_FRONT_INVALID_GUEST;
    if (!charge_recording(replay, recording, 0, 16))
        return NVN_FRONT_BOUNDARY;
    /* The guest owns this allocation. Vulkan derives scratch for the translated
       shader from its pipeline, with no application scratch binding. */
    recording->scratch = (BufferRange){pool.storage + x[2], (size_t)x[3]};
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind set_descriptor_pool(NvnReplay *replay, const uint64_t x[32], bool sampler)
{
    Recording *recording = *find_recording(replay, x[0]);
    NvnFrontDescriptorPoolInfo pool;
    if (!recording || !nvn_front_get_descriptor_pool(replay->config.front, x[1], sampler, &pool) ||
        pool.device != recording->device)
        return NVN_FRONT_INVALID_GUEST;
    if (!charge_recording(replay, recording, 0, 16))
        return NVN_FRONT_BOUNDARY;
    if (sampler)
        recording->sampler_pool = x[1];
    else
        recording->texture_pool = x[1];
    return NVN_FRONT_HANDLED;
}

typedef enum { COMPUTE_UNSUPPORTED, COMPUTE_RESET_COUNTER, COMPUTE_CLUSTERED_LIGHTS } ComputeAbi;

static ComputeAbi compute_abi(const PokopiaVkProgramStage *stage)
{
    if (!stage->hash || !stage->resources)
        return COMPUTE_UNSUPPORTED;
    /* The pinned Switch 2 c[0] table places the first UBO descriptor at
       0x10 and SSBOs 0 and 1 at 0x20 and 0x30. The texture handles begin
       at 0x40. Cache resource offsets are word indices. */
    if (!strcmp(stage->hash, "082274fc8b17c887") &&
        !strcmp(stage->resources, "cb3=s0b4 sb[c0+0xc]=s1b0"))
        return COMPUTE_RESET_COUNTER;
    if (!strcmp(stage->hash, "b81c48ff486acef4") &&
        !strcmp(stage->resources, "cb3=s0b4 sb[c0+0xc]=s1b0 sb[c0+0x8]=s1b1 "
                                 "texTextureBuffer[c0:16]=s2b64 texTextureBuffer[c0:18]=s2b65"))
        return COMPUTE_CLUSTERED_LIGHTS;
    return COMPUTE_UNSUPPORTED;
}

static bool get_program(NvnFront *front, uint64_t device, uint64_t address, NvnFrontProgramInfo *program)
{
    return nvn_front_get_program(front, address, program) && program->device == device &&
           (program->linked || program->has_unavailable_shader);
}

static bool require_program_stage(NvnFront *front, uint64_t device, uint64_t address, NvnShaderStage stage,
                                  NvnFrontProgramInfo *program, PokopiaVkProgramStage *info)
{
    return get_program(front, device, address, program) &&
           pokopia_vk_program_require_stage(program->native, stage, info);
}

/* R74: set by the loader; bind_compute_program says why it declines. */
FILE *nvn_replay_error_log;

static NvnFrontReplyKind bind_compute_program(NvnReplay *replay, Recording *recording,
                                             uint64_t address, uint32_t stages, const PokopiaVkProgram *program)
{
    NvnFrontCommandBufferInfo command;
    if (!nvn_front_get_command_buffer(replay->config.front, recording->encoder, &command))
        return NVN_FRONT_INVALID_GUEST;
    PokopiaVkProgramStage stage;
    if (stages != (1u << NVN_SHADER_COMPUTE))
        return NVN_FRONT_BOUNDARY;
    if (!pokopia_vk_program_stage(program, NVN_SHADER_COMPUTE, &stage)) {
        /* R74: a compute shader missing from the translated cache (the game
         * world uses some) is bound anyway; its dispatches are dropped at
         * record time. R73 stopped the game here. */
        note_skip("compute program not translated; its dispatches are skipped");
        ++skip_counts[SKIP_BIND];
    } else {
        /* R55: bind compute programs whose resource layout the bridge does not
         * model; record_dispatch()/run_compute() skip their dispatches. */
        if (stage.resources && compute_abi(&stage) == COMPUTE_UNSUPPORTED) {
            note_skipped_shader(NVN_SHADER_COMPUTE, stage.hash);
            ++skip_counts[SKIP_BIND];
        }
        for (unsigned i = 0; i < NVN_SHADER_COMPUTE; ++i)
            if (pokopia_vk_program_stage(program, i, &stage)) {
                if (nvn_replay_error_log)
                    fprintf(nvn_replay_error_log, "bind compute program %#llx: also has stage %u\n",
                            (unsigned long long)address, i);
                return NVN_FRONT_BOUNDARY;
            }
    }

    const uint8_t *encoder = replay->config.read_guest(replay->config.opaque, recording->encoder, 0xa9);
    if (!encoder)
        return NVN_FRONT_INVALID_GUEST;
    /* SDK 0x5ff6e0 writes a 16-byte program control record in the
       ordinary recording mode. Inline compute programs need distinct
       packets and remain a boundary. */
    /* R74: the game world binds compute programs in recording modes 1 and 4
     * (control-record modes) too; charged as the same 16-byte control record
     * (inferred). Other modes still decline. */
    const bool control_mode = !command.recording_mode || command.recording_mode == 1 || command.recording_mode == 4;
    if (control_mode && command.recording_mode)
        note_skip("compute program bound in recording mode 1/4, charged 16 control bytes (inferred)");
    if (encoder[0xa8] || !control_mode || !charge_recording(replay, recording, 0, 16)) {
        /* A pending memory request is the normal grow-and-retry path, not a decline. */
        if (nvn_replay_error_log && !replay->memory_request.function) {
            fprintf(nvn_replay_error_log, "bind compute program %#llx declined: inline=%u recording_mode=%u%s\n",
                    (unsigned long long)address, encoder[0xa8], (unsigned)command.recording_mode,
                    encoder[0xa8] || command.recording_mode ? "" : " (command memory charge failed)");
            fflush(nvn_replay_error_log);
        }
        return NVN_FRONT_BOUNDARY;
    }
    recording->program[NVN_SHADER_COMPUTE] = address;
    return NVN_FRONT_HANDLED;
}

static bool supported_graphics_shader(unsigned stage, const char *hash)
{
    if (stage == NVN_SHADER_VERTEX)
        return !strcmp(hash, "821538a4398f85a7") || !strcmp(hash, "70b2ae4bef0dfe46") ||
               !strcmp(hash, "a26763a4bdd7b361");
    if (stage == NVN_SHADER_FRAGMENT)
        return !strcmp(hash, "7587f31f1945afaf") || !strcmp(hash, "9f3f4772c2ad7edc") ||
               !strcmp(hash, "42c917e6eb2c6411") || !strcmp(hash, "1c913e7b97e7e999") ||
               !strcmp(hash, "ff03fa7b63a79526") || !strcmp(hash, "c4664b38f6a87835") ||
               !strcmp(hash, "2db3327d9fbc5bb3");
    return false;
}

static NvnFrontReplyKind bind_program(NvnReplay *replay, const uint64_t x[32])
{
    Recording *recording = *find_recording(replay, x[0]);
    uint32_t stages = (uint32_t)x[2];
    NvnFrontProgramInfo program = {0};
    if (!recording || (stages & ~((1u << PROGRAM_STAGES) - 1)) ||
        (x[1] && !get_program(replay->config.front, recording->device, x[1], &program)))
        return NVN_FRONT_INVALID_GUEST;
    /* Supported graphics pairs bind the pinned SDK's vertex and
       fragment program packets. No compute packet is emitted here. */
    const size_t enable_bytes[] = {28, 60};
    const size_t disable_bytes[] = {4, 8, 8, 20, 4, 0};
    if (x[1] && (stages & (1u << NVN_SHADER_COMPUTE)))
        return bind_compute_program(replay, recording, x[1], stages, program.native);
    size_t bytes = 0;
    uint32_t bound_stages = 0;
    bool skipped = false;
    for (unsigned stage = 0; stage < PROGRAM_STAGES; ++stage) {
        PokopiaVkProgramStage info;
        const bool registered = x[1] && pokopia_vk_program_stage(program.native, stage, &info);
        const bool requested = (stages & (1u << stage)) != 0;
        /* R50: bind unsupported shaders anyway. run_draw() skips any draw
         * that uses one, so the game keeps running with that draw missing. */
        /* R78: no per-bind check against the strict path's shader list (a
         * linear note search on every bind); draws go to the general path. */
        if (!requested)
            continue;
        if (registered) {
            if (stage > NVN_SHADER_FRAGMENT)
                return NVN_FRONT_BOUNDARY;
            bound_stages |= 1u << stage;
            bytes += enable_bytes[stage];
        } else {
            bytes += disable_bytes[stage];
        }
    }
    if (!charge_recording(replay, recording, bytes, 0))
        return NVN_FRONT_BOUNDARY;
    if (skipped)
        ++skip_counts[SKIP_BIND];
    for (unsigned stage = 0; stage < PROGRAM_STAGES; ++stage)
        if (stages & (1u << stage))
            recording->program[stage] = bound_stages & (1u << stage) ? x[1] : 0;
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind bind_texture(NvnReplay *replay, const uint64_t x[32])
{
    Recording *r = *find_recording(replay, x[0]);
    const uint32_t stage = (uint32_t)x[1], slot = (uint32_t)x[2];
    if (!r || stage >= PROGRAM_STAGES || slot >= TEXTURE_SLOTS)
        return NVN_FRONT_INVALID_GUEST;
    const uint8_t *encoder = replay->config.read_guest(replay->config.opaque, x[0], 0xa9);
    if (!encoder)
        return NVN_FRONT_INVALID_GUEST;
    /* NVN2 BindTexture 0x6022e0 writes 16 bytes for a graphics stage,
       plus a command word when encoder flag 0x13 bit 3 is set.
       Compute uses an inline command or a control record. */
    const bool compute = stage == NVN_SHADER_COMPUTE;
    const size_t command_bytes = compute ? (encoder[0xa8] ? 12 : 0) : (encoder[0x13] & 8 ? 20 : 16);
    if (!charge_recording(replay, r, command_bytes, compute && !command_bytes ? 16 : 0))
        return NVN_FRONT_BOUNDARY;
    r->textures[stage][slot] = x[3];
    return NVN_FRONT_HANDLED;
}

/* R57: NVN's BindImage(cmdbuf, stage, slot, image handle) is BindTexture's
 * twin for writable images. Nothing the bridge runs binds images yet, so
 * dispatches and draws that need one are skipped elsewhere. Command memory is
 * charged like BindTexture (inferred, noted once) to keep the guest's
 * accounting in step. */
static NvnFrontReplyKind bind_image(NvnReplay *replay, const uint64_t x[32])
{
    Recording *r = *find_recording(replay, x[0]);
    const uint32_t stage = (uint32_t)x[1], slot = (uint32_t)x[2];
    if (!r || stage >= PROGRAM_STAGES || slot >= TEXTURE_SLOTS)
        return NVN_FRONT_INVALID_GUEST;
    const uint8_t *encoder = replay->config.read_guest(replay->config.opaque, x[0], 0xa9);
    if (!encoder)
        return NVN_FRONT_INVALID_GUEST;
    const bool compute = stage == NVN_SHADER_COMPUTE;
    const size_t command_bytes = compute ? (encoder[0xa8] ? 12 : 0) : (encoder[0x13] & 8 ? 20 : 16);
    if (!charge_recording(replay, r, command_bytes, compute && !command_bytes ? 16 : 0))
        return NVN_FRONT_BOUNDARY;
    /* R73: compute images are bound for the general compute path. */
    if (compute && slot < 8)
        r->compute_images[slot] = x[3];
    else
        note_skip("BindImage accepted, sized like BindTexture (inferred); graphics images are not bound on the host");
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind bind_compute_buffer(NvnReplay *replay, Recording *recording,
                                            const uint64_t x[32], bool uniform)
{
    const uint32_t slot = (uint32_t)x[2];
    if (!recording || slot >= (uniform ? UNIFORM_BUFFERS : COMPUTE_STORAGE_SLOTS) || x[4] > UINT32_MAX)
        return NVN_FRONT_INVALID_GUEST;
    const uint8_t *encoder = replay->config.read_guest(replay->config.opaque, recording->encoder, 0xa9);
    if (!encoder)
        return NVN_FRONT_INVALID_GUEST;
    /* Both SDK compute binds reserve 20 inline command bytes or a
       24-byte control record. The native packets carry a 32-bit size. */
    if (!charge_recording(replay, recording, encoder[0xa8] ? 20 : 0, encoder[0xa8] ? 0 : 24))
        return NVN_FRONT_BOUNDARY;
    BufferRange *slots = uniform ? recording->uniform[NVN_SHADER_COMPUTE] : recording->compute_storage;
    slots[slot] = (BufferRange){x[3], (size_t)x[4]};
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind bind_buffers(NvnReplay *replay, const uint64_t x[32], bool uniform, bool array)
{
    Recording *recording = *find_recording(replay, x[0]);
    uint32_t stage = uniform ? (uint32_t)x[1] : 0;
    const uint64_t *args = x + (uniform ? 2 : 1);
    uint32_t first = (uint32_t)args[0], count = array ? (uint32_t)args[1] : 1;
    uint32_t slots = uniform ? UNIFORM_BUFFERS : VERTEX_BUFFERS;
    if (!recording || stage >= PROGRAM_STAGES || first > slots || count > slots - first)
        return NVN_FRONT_INVALID_GUEST;
    if (uniform && stage == NVN_SHADER_COMPUTE)
        return array ? NVN_FRONT_BOUNDARY : bind_compute_buffer(replay, recording, x, true);
    BufferRange *bindings = uniform ? recording->uniform[stage] : recording->vertex;
    if (!count) {
        if (uniform && !charge_recording(replay, recording, 8, 0))
            return NVN_FRONT_BOUNDARY;
        return NVN_FRONT_HANDLED;
    }
    if (!array) {
        if (args[2] > SIZE_MAX)
            return NVN_FRONT_INVALID_GUEST;
        const size_t bytes = uniform ? (args[2] ? 20 : 12) : 24;
        if (!charge_recording(replay, recording, bytes, 0))
            return NVN_FRONT_BOUNDARY;
        bindings[first] = (BufferRange){args[1], (size_t)args[2]};
        return NVN_FRONT_HANDLED;
    }
    const uint8_t *ranges = replay->config.read_guest(replay->config.opaque, args[2], count * 16u);
    if (!ranges)
        return NVN_FRONT_INVALID_GUEST;
    BufferRange pending[VERTEX_BUFFERS];
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t address, size;
        memcpy(&address, ranges + i * 16u, sizeof address);
        memcpy(&size, ranges + i * 16u + sizeof address, sizeof size);
        if (size > SIZE_MAX)
            return NVN_FRONT_INVALID_GUEST;
        pending[i] = (BufferRange){address, (size_t)size};
    }
    if (!charge_recording(replay, recording, 8 + (uniform ? 8 : 16) * count, 0))
        return NVN_FRONT_BOUNDARY;
    memcpy(bindings + first, pending, count * sizeof pending[0]);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind set_rectangle(NvnReplay *replay, const uint64_t x[32], bool viewport, bool array)
{
    Recording *recording = *find_recording(replay, x[0]);
    if (!recording)
        return NVN_FRONT_INVALID_GUEST;
    if (array && ((uint32_t)x[1] != 0 || (uint32_t)x[2] != 1))
        return NVN_FRONT_BOUNDARY;
    if (viewport) {
        float next[4];
        if (array) {
            const void *rect = replay->config.read_guest(replay->config.opaque, x[3], sizeof recording->viewport);
            if (!rect)
                return NVN_FRONT_INVALID_GUEST;
            memcpy(next, rect, sizeof next);
        } else {
            for (unsigned i = 0; i < 4; ++i)
                next[i] = (float)(int32_t)x[i + 1];
        }
        if (!charge_recording(replay, recording, 36, 0))
            return NVN_FRONT_BOUNDARY;
        memcpy(recording->viewport, next, sizeof next);
        recording->viewport_set = true;
    } else {
        int32_t next[4];
        if (array) {
            const void *rect = replay->config.read_guest(replay->config.opaque, x[3], sizeof recording->scissor);
            if (!rect)
                return NVN_FRONT_INVALID_GUEST;
            memcpy(next, rect, sizeof next);
        } else {
            for (unsigned i = 0; i < 4; ++i)
                next[i] = (int32_t)x[i + 1];
        }
        if (!charge_recording(replay, recording, 12, 0))
            return NVN_FRONT_BOUNDARY;
        memcpy(recording->scissor, next, sizeof next);
        recording->scissor_set = true;
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind bind_vertex_state(NvnReplay *replay, const uint64_t x[32], bool stream)
{
    Recording *recording = *find_recording(replay, x[0]);
    if (!recording)
        return NVN_FRONT_INVALID_GUEST;
    uint32_t count = (uint32_t)x[1];
    if (!count)
        return NVN_FRONT_HANDLED;
    const size_t rounded = (count + UINT64_C(1)) & ~UINT64_C(1);
    if (stream) {
        NvnFrontVertexStreamInfo streams[NVN_FRONT_MAX_VERTEX_STREAMS];
        if (!nvn_front_read_vertex_streams(replay->config.front, x[2], count, streams))
            return NVN_FRONT_INVALID_GUEST;
        uint32_t divisors = count;
        while (divisors && !streams[divisors - 1].divisor)
            --divisors;
        if (!charge_recording(replay, recording, 4 * (4 + rounded + ((divisors + 1u) & ~1u)), 0))
            return NVN_FRONT_BOUNDARY;
        memcpy(recording->streams, streams, count * sizeof streams[0]);
        recording->stream_count = count;
    } else {
        NvnFrontVertexAttribInfo attributes[NVN_FRONT_MAX_VERTEX_ATTRIBS];
        if (!nvn_front_read_vertex_attribs(replay->config.front, x[2], count, attributes))
            return NVN_FRONT_INVALID_GUEST;
        if (!charge_recording(replay, recording, 4 * (2 + rounded), 0))
            return NVN_FRONT_BOUNDARY;
        memcpy(recording->attributes, attributes, count * sizeof attributes[0]);
        recording->attribute_count = count;
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind bind_raster_state(NvnReplay *replay, const char *name, const uint64_t x[32])
{
    NvnFront *front = replay->config.front;
    Recording *r = *find_recording(replay, x[0]);
    if (!strcmp(name, "nvnCommandBufferBindColorState")) {
        NvnFrontColorInfo state;
        if (!r || !nvn_front_read_color(front, x[1], &state))
            return NVN_FRONT_INVALID_GUEST;
        const size_t bytes = (state.logic_op == 3 ? 8 : 12) + (state.alpha_func == 8 ? 4 : 8);
        if (!charge_recording(replay, r, bytes, 0))
            return NVN_FRONT_BOUNDARY;
        r->blend_enable = (state.blend_mask & 1) != 0;
        r->blend_mask = state.blend_mask;
        r->unsupported_color = state.logic_op != 3 || state.alpha_func != 8;
        r->raster_bound |= 1;
    } else if (!strcmp(name, "nvnCommandBufferBindChannelMaskState")) {
        uint32_t masks;
        if (!r || !nvn_front_read_channel_masks(front, x[1], &masks))
            return NVN_FRONT_INVALID_GUEST;
        if (!charge_recording(replay, r, 20, 0))
            return NVN_FRONT_BOUNDARY;
        r->color_write_mask = masks & 15;
        r->color_write_masks = masks;
        r->raster_bound |= 2;
    } else if (!strcmp(name, "nvnCommandBufferBindPolygonState")) {
        NvnFrontPolygonInfo state;
        if (!r || !nvn_front_read_polygon(front, x[1], &state))
            return NVN_FRONT_INVALID_GUEST;
        if (!charge_recording(replay, r, state.cull_face ? 24 : 20, 0))
            return NVN_FRONT_BOUNDARY;
        r->cull = state.cull_face;
        r->front_face = state.front_face;
        r->polygon_mode = state.mode;
        /* R73: NVN PolygonOffsetEnable POINT 1, LINE 2, FILL 4. Only filled
         * polygons are drawn, so the fill offset becomes a depth bias in the
         * general path; the pinned path still declines any offset. */
        r->polygon_offset_fill = (state.offset_enables & 4u) != 0;
        r->unsupported_polygon = false;
        r->raster_bound |= 4;
    } else if (!strcmp(name, "nvnCommandBufferBindDepthStencilState")) {
        NvnFrontDepthStencilInfo state;
        if (!r || !nvn_front_read_depth_stencil(front, x[1], &state))
            return NVN_FRONT_INVALID_GUEST;
        const size_t bytes = (state.depth_test ? 12 : 4) + (state.stencil_test ? 8 : 4);
        if (!charge_recording(replay, r, bytes, 0))
            return NVN_FRONT_BOUNDARY;
        r->depth_test = state.depth_test;
        r->depth_write = state.depth_write;
        r->depth_func = state.depth_func;
        r->stencil_test = state.stencil_test;
        r->stencil_faces[0] = state.front;
        r->stencil_faces[1] = state.back;
        r->raster_bound |= 8;
    } else if (!strcmp(name, "nvnCommandBufferBindMultisampleState")) {
        NvnFrontMultisampleInfo state;
        if (!r || !nvn_front_read_multisample(front, x[1], &state))
            return NVN_FRONT_INVALID_GUEST;
        if (!charge_recording(replay, r, state.raster_samples > state.samples ? 92 : 80, 0))
            return NVN_FRONT_BOUNDARY;
        r->unsupported_multisample = state.enabled || state.alpha_to_coverage ||
                                     state.samples > 1 || state.raster_samples > 1;
    } else if (!strcmp(name, "nvnCommandBufferBindBlendState")) {
        NvnFrontBlendInfo state;
        if (!r || !nvn_front_read_blend(front, x[1], &state))
            return NVN_FRONT_INVALID_GUEST;
        if (state.advanced_mode || !charge_recording(replay, r, 40, 0))
            return NVN_FRONT_BOUNDARY;
        if (!state.target)
            r->blend = state;
    } else {
        return NVN_FRONT_NOT_OURS;
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind set_depth_range(NvnReplay *replay, const uint64_t x[32], const uint32_t float_args[2])
{
    Recording *r = *find_recording(replay, x[0]);
    if (!r)
        return NVN_FRONT_INVALID_GUEST;
    const void *range = float_args;
    if (!range) {
        if ((uint32_t)x[1] != 0 || (uint32_t)x[2] != 1)
            return NVN_FRONT_BOUNDARY;
        range = replay->config.read_guest(replay->config.opaque, x[3], sizeof r->depth_range);
    }
    if (!range)
        return NVN_FRONT_INVALID_GUEST;
    if (!charge_recording(replay, r, 28, 0))
        return NVN_FRONT_BOUNDARY;
    memcpy(r->depth_range, range, sizeof r->depth_range);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind record_barrier(NvnReplay *replay, const uint64_t x[32])
{
    enum {
        ORDER_PRIMITIVES = 0x1, ORDER_FRAGMENTS = 0x2,
        ORDER_INDIRECT_DATA = 0x8, INVALIDATE_TEXTURE = 0x10, INVALIDATE_SHADER = 0x20,
    };
    Recording *r = *find_recording(replay, x[0]);
    NvnFrontCommandBufferInfo command;
    if (!r || !nvn_front_get_command_buffer(replay->config.front, x[0], &command))
        return NVN_FRONT_INVALID_GUEST;
    const uint32_t flags = (uint32_t)x[1];
    /* R56: accept any mix of the ordering and invalidation bits. The SDK sizes
     * below were confirmed for 0, 0x3, 0x10, 0x12 and 0x21; every confirmed
     * case fits "an extra word when an ordering bit meets an invalidation
     * bit", which is assumed for the rest (first seen 0x11) and noted. */
    const uint32_t ordering = ORDER_PRIMITIVES | ORDER_FRAGMENTS, invalidation = INVALIDATE_TEXTURE | INVALIDATE_SHADER;
    const bool confirmed = !flags || flags == (ORDER_PRIMITIVES | ORDER_FRAGMENTS) || flags == INVALIDATE_TEXTURE ||
        flags == (ORDER_FRAGMENTS | INVALIDATE_TEXTURE) || flags == (ORDER_PRIMITIVES | INVALIDATE_SHADER) ||
        flags == ORDER_INDIRECT_DATA;
    /* R75: the world also sends ORDER_INDIRECT_DATA mixed with other bits
     * (first seen 0x39). Sized as the other bits' packet plus the indirect
     * split below (inferred). */
    const bool indirect_mix = (flags & ORDER_INDIRECT_DATA) && flags != ORDER_INDIRECT_DATA &&
        !(flags & ~(ordering | invalidation | ORDER_INDIRECT_DATA));
    if (!confirmed && !indirect_mix && (flags & ~(ordering | invalidation)))
        return NVN_FRONT_BOUNDARY;
    if (command.recording_mode == 1 || command.recording_mode == 4) {
        /* R74: the game world records barriers in the control-record modes;
         * a boundary here stopped the game. Charged as one 16-byte control
         * record, like the compute program bind (inferred, not confirmed
         * against the SDK writer). Replay already orders every operation. */
        note_skip("barrier in recording mode 1/4 charged 16 control bytes (inferred)");
        return charge_recording(replay, r, 0, 16) ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
    }
    if (!confirmed) {
        char note[112];
        snprintf(note, sizeof note, "barrier flags=%#x sized by the inferred extra-word rule", flags);
        note_skip(note);
    }
    /* Each preceding draw or transfer retires before replay executes the next
       operation. Image operations establish their own access scope. Shader code
       is immutable on the host, and replay uploads shader buffers per operation. */
    /* The native writer at sdk+0x61cb70 emits an extra word for either of
       these flag pairs. The primitives/fragments pair uses the base size. */
    /* An empty mask skips the ordering word, but still emits the SDK's
       barrier prologue and epilogue. */
    const size_t base_bytes = (command.recording_mode > 4 ? 16 : 24) - (flags ? 0 : 4);
    const bool extra_word = (flags & ordering) && (flags & invalidation);
    const size_t bytes = command.recording_mode == 2 ? 0 : base_bytes + (extra_word ? 4 : 0);
    if ((flags == ORDER_INDIRECT_DATA || indirect_mix) && bytes) {
        /* SDK bit 3 splits the commands around an 8-byte control record
           and closes two segments, leaving the final packet in a new segment.
           Replay waits for GPU writes and copies them to canonical guest
           memory before any following operation can read indirect data. */
        if (!charge_memory(replay, r->encoder, bytes, 2 * 32 + 8))
            return NVN_FRONT_BOUNDARY;
        r->command_segment = true;
        return NVN_FRONT_HANDLED;
    }
    return charge_recording(replay, r, bytes, 0) ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}

static NvnFrontReplyKind disable_tiled_cache(NvnReplay *replay, const uint64_t x[32])
{
    Recording *r = *find_recording(replay, x[0]);
    if (!r)
        return NVN_FRONT_INVALID_GUEST;
    if ((uint32_t)x[1] != 2)
        return NVN_FRONT_BOUNDARY;
    /* NVN disables its tile cache here. Our draws use separate rendering
       scopes and retire their attachment writes between operations. */
    return charge_recording(replay, r, 4, 0) ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}

static NvnFrontReplyKind set_stencil_value(NvnReplay *replay, const uint64_t x[32], unsigned field)
{
    Recording *r = *find_recording(replay, x[0]);
    uint32_t faces = (uint32_t)x[1];
    if (!r || (faces & ~3u))
        return NVN_FRONT_INVALID_GUEST;
    size_t bytes = (faces & 1 ? 4 : 0) + (faces & 2 ? 4 : 0);
    if (!charge_recording(replay, r, bytes, 0))
        return NVN_FRONT_BOUNDARY;
    for (unsigned face = 0; face < 2; ++face)
        if (faces & (1u << face))
            r->stencil[field][face] = (uint8_t)x[2];
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind disable_raster_option(NvnReplay *replay, const uint64_t x[32], size_t bytes)
{
    Recording *r = *find_recording(replay, x[0]);
    if (!r)
        return NVN_FRONT_INVALID_GUEST;
    if ((uint8_t)x[1])
        return NVN_FRONT_BOUNDARY;
    return charge_recording(replay, r, bytes, 0) ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}

static NvnFrontReplyKind set_polygon_offset(NvnReplay *replay, uint64_t encoder, const uint32_t float_args[3])
{
    Recording *r = *find_recording(replay, encoder);
    if (!r || !float_args)
        return NVN_FRONT_INVALID_GUEST;
    if (!charge_recording(replay, r, 24, 0))
        return NVN_FRONT_BOUNDARY;
    memcpy(r->polygon_offset, float_args, sizeof r->polygon_offset);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind set_patch_size(NvnReplay *replay, const uint64_t x[32])
{
    Recording *r = *find_recording(replay, x[0]);
    uint32_t size = (uint32_t)x[1];
    if (!r || !size || size > 32)
        return NVN_FRONT_INVALID_GUEST;
    if (!charge_recording(replay, r, 4, 0))
        return NVN_FRONT_BOUNDARY;
    r->patch_size = size;
    return NVN_FRONT_HANDLED;
}

static VkRect2D clear_area(const Recording *recording, const NvnFrontTextureConfig *config);

static NvnFrontReplyKind record_clear(NvnReplay *replay, const uint64_t x[32])
{
    Recording *recording = *find_recording(replay, x[0]);
    uint32_t index = (uint32_t)x[1], mask = (uint32_t)x[3];
    if (!recording || index >= recording->target_count || !recording->targets[index])
        return NVN_FRONT_INVALID_GUEST;
    if (!mask)
        return NVN_FRONT_HANDLED;
    if (mask > 15)
        return NVN_FRONT_BOUNDARY;
    NvnFrontTextureInfo texture;
    if (!get_color_target(replay, recording->device, recording->targets[index], &texture))
        return NVN_FRONT_INVALID_GUEST;
    if (texture.create_info.config.depth != 1)
        return NVN_FRONT_BOUNDARY;
    const NvnFrontTextureConfig *config = &texture.create_info.config;
    const VkRect2D area = clear_area(recording, config);
    if (area.offset.x || area.offset.y || area.extent.width != config->width || area.extent.height != config->height) {
        NvnFrontDeviceModes modes;
        if (!nvn_front_get_device_modes(replay->config.front, recording->device, &modes) ||
            modes.window_origin != NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT)
            return NVN_FRONT_BOUNDARY;
    }
    Operation operation = { .kind = OP_CLEAR, .data.clear = {
        .address = recording->targets[index], .native = texture.native, .mask = mask, .area = area,
    } };
    const void *rgba = replay->config.read_guest(replay->config.opaque, x[2], sizeof operation.data.clear.rgba);
    if (!rgba)
        return NVN_FRONT_INVALID_GUEST;
    memcpy(operation.data.clear.rgba, rgba, sizeof operation.data.clear.rgba);
    NvnFrontCommandBufferInfo command;
    if (!nvn_front_get_command_buffer(replay->config.front, x[0], &command))
        return NVN_FRONT_INVALID_GUEST;
    const uint32_t command_bytes = command.recording_mode <= 4 ? 56 : 48;
    if (!area.extent.width || !area.extent.height)
        return charge_recording(replay, recording, command_bytes, 0) ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
    return append_metered(replay, recording, &operation, command_bytes, 0);
}

static VkRect2D clear_area(const Recording *recording, const NvnFrontTextureConfig *config)
{
    if (!recording->scissor_set)
        return (VkRect2D){{0, 0}, {config->width, config->height}};
    const int32_t *scissor = recording->scissor;
    const int32_t x = scissor[0] < 0 ? 0 : scissor[0];
    const int32_t y = scissor[1] < 0 ? 0 : scissor[1];
    int64_t end_x = (int64_t)scissor[0] + scissor[2];
    int64_t end_y = (int64_t)scissor[1] + scissor[3];
    if (end_x > config->width)
        end_x = config->width;
    if (end_y > config->height)
        end_y = config->height;
    return (VkRect2D){{x, y}, {end_x > x ? (uint32_t)(end_x - x) : 0, end_y > y ? (uint32_t)(end_y - y) : 0}};
}

enum {
    TARGET_3D = 2, COPY_LINEAR_FILTER = 0x1, COPY_MIRROR_MASK = 0x1c,
    /* An estimate: the SDK sizes this 2D-engine packet at run time. Like a
     * buffer copy, recording modes 1 and 4 spend control memory instead. */
    COPY_TEXTURE_COMMAND_BYTES = 128, COPY_TEXTURE_CONTROL_BYTES = 32,
};

/* Resolves one side of a texture copy. Returns false for a malformed guest
 * argument; sets *reason (and returns true) for a copy the bridge skips. */
static bool resolve_copy_side(NvnReplay *replay, uint64_t device, uint64_t texture, uint64_t view_address,
                              uint64_t region_address, NvnReplayTextureCopySide *side, const char **reason)
{
    *side = (NvnReplayTextureCopySide){0};
    if (!nvn_front_get_texture(replay->config.front, texture, &side->texture) ||
        side->texture.create_info.config.device != device)
        return false;
    const NvnFrontTextureConfig *config = &side->texture.create_info.config;
    if (!side->texture.native) {
        *reason = "copy texture: texture has no Vulkan image";
        return true;
    }
    side->volume = config->target == TARGET_3D;
    if (view_address) {
        const uint8_t *view = replay->config.read_guest(replay->config.opaque, view_address, 16);
        if (!view)
            return false;
        uint16_t layer;
        uint32_t format;
        memcpy(&layer, view, sizeof layer);
        memcpy(&format, view + 8, sizeof format);
        const uint8_t fields = view[7];
        if (fields & 1u)
            side->level = view[4];
        if (fields & 2u)
            side->layer = layer;
        if ((fields & 4u) && format != config->format) {
            *reason = "copy texture: view changes the format";
            return true;
        }
    }
    if (side->level >= (config->levels ? config->levels : 1))
        return false;
    const int32_t width = (int32_t)(config->width >> side->level ? config->width >> side->level : 1);
    const int32_t height = (int32_t)(config->height >> side->level ? config->height >> side->level : 1);
    const int32_t depth = side->volume ? (int32_t)(config->depth >> side->level ? config->depth >> side->level : 1) : 1;
    int32_t region[6] = {0, 0, 0, width, height, side->volume ? depth : 1};
    if (region_address) {
        const void *guest = replay->config.read_guest(replay->config.opaque, region_address, sizeof region);
        if (!guest)
            return false;
        memcpy(region, guest, sizeof region);
    }
    if (region[0] < 0 || region[1] < 0 || region[2] < 0 || region[3] < 0 || region[4] < 0 || region[5] < 0)
        return false;
    /* Clip x/y (and z of a 3D level) to the level. For layered targets z and
     * depth select array layers after the view's first layer. */
    side->x = region[0];
    side->y = region[1];
    side->width = region[0] >= width ? 0 : (region[3] < width - region[0] ? region[3] : width - region[0]);
    side->height = region[1] >= height ? 0 : (region[4] < height - region[1] ? region[4] : height - region[1]);
    if (side->volume) {
        side->z = region[2];
        side->depth = region[2] >= depth ? 0 : (region[5] < depth - region[2] ? region[5] : depth - region[2]);
        side->layers = 1;
    } else {
        side->depth = 1;
        side->layer += (uint32_t)region[2];
        side->layers = region[5] ? (uint32_t)region[5] : 1;
        const uint32_t available = config->depth ? config->depth : 1;
        if (side->layer >= available || side->layers > available - side->layer)
            return false;
    }
    return true;
}

static NvnFrontReplyKind record_copy_texture(NvnReplay *replay, const uint64_t x[32])
{
    Recording *recording = *find_recording(replay, x[0]);
    if (!recording)
        return NVN_FRONT_INVALID_GUEST;
    NvnFrontCommandBufferInfo command;
    if (!nvn_front_get_command_buffer(replay->config.front, x[0], &command))
        return NVN_FRONT_INVALID_GUEST;
    const bool control = command.recording_mode == 1 || command.recording_mode == 4;
    const size_t command_bytes = control ? 0 : COPY_TEXTURE_COMMAND_BYTES + (command.recording_mode <= 4 ? 8 : 0);
    const size_t control_bytes = control ? COPY_TEXTURE_CONTROL_BYTES : 0;

    Operation operation = {.kind = OP_COPY_TEXTURE};
    TextureCopy *copy = &operation.data.copy_texture;
    const char *reason = NULL;
    const uint32_t flags = (uint32_t)x[7];
    if (!resolve_copy_side(replay, recording->device, x[1], x[2], x[3], &copy->copy.source, &reason) ||
        !resolve_copy_side(replay, recording->device, x[4], x[5], x[6], &copy->copy.destination, &reason))
        return NVN_FRONT_INVALID_GUEST;
    const NvnReplayTextureCopySide *from = &copy->copy.source, *to = &copy->copy.destination;
    if (!reason && (flags & COPY_MIRROR_MASK))
        reason = "copy texture: mirrored copy";
    if (!reason && !replay->config.copy_texture)
        reason = "copy texture: no host copy callback";
    if (!reason && (from->layers != to->layers || (from->volume != to->volume && from->depth != to->depth)))
        reason = "copy texture: layer counts differ";
    const bool empty = !from->width || !from->height || !from->depth || !to->width || !to->height || !to->depth;
    if (reason || empty) {
        if (!charge_recording(replay, recording, command_bytes, control_bytes))
            return NVN_FRONT_BOUNDARY;
        if (reason) {
            ++skip_counts[SKIP_COPY];
            note_skip(reason);
        }
        return NVN_FRONT_HANDLED;
    }
    copy->source = x[1];
    copy->destination = x[4];
    copy->source_native = from->texture.native;
    copy->destination_native = to->texture.native;
    copy->copy.linear = (flags & COPY_LINEAR_FILTER) != 0;
    return append_metered(replay, recording, &operation, command_bytes, control_bytes);
}

static NvnFrontReplyKind run_copy_texture(NvnReplay *replay, const TextureCopy *recorded, bool execute)
{
    NvnReplayTextureCopy copy = recorded->copy;
    if (!nvn_front_get_texture(replay->config.front, recorded->source, &copy.source.texture) ||
        !nvn_front_get_texture(replay->config.front, recorded->destination, &copy.destination.texture) ||
        copy.source.texture.native != recorded->source_native ||
        copy.destination.texture.native != recorded->destination_native)
        return NVN_FRONT_INVALID_GUEST;
    if (execute && tracing) {
        fprintf(nvn_replay_trace_log, "copy-texture");
        trace_texture("src", copy.source.texture.native);
        trace_texture("dst", copy.destination.texture.native);
        fputc('\n', nvn_replay_trace_log);
    }
    if (execute && !replay->config.copy_texture(replay->config.opaque, &copy)) {
        ++skip_counts[SKIP_COPY];
        note_skip("copy texture: host copy failed");
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind record_depth_clear_strict(NvnReplay *replay, const uint64_t x[32],
                                                   const uint32_t *float_args);

/* R53: a depth/stencil clear the bridge cannot translate is dropped with its
 * 24-byte control record still charged, and the reason noted. */
static NvnFrontReplyKind record_depth_clear(NvnReplay *replay, const uint64_t x[32], const uint32_t *float_args)
{
    const NvnFrontReplyKind result = record_depth_clear_strict(replay, x, float_args);
    if (result != NVN_FRONT_BOUNDARY)
        return result;
    Recording *recording = *find_recording(replay, x[0]);
    if (!recording || !charge_recording(replay, recording, 0, 24))
        return NVN_FRONT_BOUNDARY;
    NvnFrontTextureInfo texture;
    float depth = 0;
    if (float_args)
        memcpy(&depth, float_args, sizeof depth);
    const bool bound = recording->depth_target &&
        get_depth_target(replay, recording->device, recording->depth_target, &texture);
    char note[112];
    snprintf(note, sizeof note, "depth-clear bound=%d format=%#x depth_mask=%u depth=%g stencil_mask=%#x",
             bound, bound ? (unsigned)texture.create_info.config.format : 0u, (unsigned)(uint8_t)x[1],
             (double)depth, (unsigned)((uint32_t)x[3] & 0xff));
    note_skip(note);
    ++skip_counts[SKIP_CLEAR];
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind record_depth_clear_strict(NvnReplay *replay, const uint64_t x[32],
                                                   const uint32_t *float_args)
{
    Recording *recording = *find_recording(replay, x[0]);
    if (!recording || !float_args)
        return NVN_FRONT_INVALID_GUEST;
    NvnFrontTextureInfo texture;
    NvnFrontDeviceModes modes;
    if (!replay->config.clear_depth_stencil ||
        !get_depth_target(replay, recording->device, recording->depth_target, &texture) ||
        !nvn_front_get_device_modes(replay->config.front, recording->device, &modes) ||
        modes.window_origin != NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT)
        return NVN_FRONT_BOUNDARY;

    /* R73: the game clears depth and stencil (mask 0xff) together even on D16
     * targets. D16 has no stencil, so that part is ignored; R53-R72 dropped the
     * whole clear and the intro's D16 depth buffer was never reset. */
    const bool has_stencil = texture.create_info.config.format != FORMAT_D16;
    const uint32_t stencil_mask = has_stencil ? (uint32_t)x[3] & 0xff : 0;
    float depth;
    memcpy(&depth, float_args, sizeof depth);
    if ((stencil_mask && stencil_mask != 0xff) ||
        ((uint8_t)x[1] && !(depth >= 0 && depth <= 1)))
        return NVN_FRONT_BOUNDARY;
    const VkImageAspectFlags aspects = ((uint8_t)x[1] ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                      (stencil_mask ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    const VkRect2D area = clear_area(recording, &texture.create_info.config);
    if (!aspects || !area.extent.width || !area.extent.height)
        return charge_recording(replay, recording, 0, 24) ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
    const Operation operation = {.kind = OP_DEPTH_CLEAR, .data.depth_clear = {
        .address = recording->depth_target, .native = texture.native,
        .area = area, .aspects = aspects, .value = {depth, (uint32_t)x[2] & 0xff},
    }};
    return append_metered(replay, recording, &operation, 0, 24);
}

static bool valid_stencil_enum(uint32_t value)
{
    return value >= 1 && value <= 8;
}

static bool snapshot_stencil(const Recording *r, unsigned face, VkStencilOpState *out)
{
    const NvnFrontStencilInfo state = r->stencil_faces[face];
    if (!valid_stencil_enum(state.func) || !valid_stencil_enum(state.fail) ||
        !valid_stencil_enum(state.depth_pass) || !valid_stencil_enum(state.depth_fail))
        return false;
    /* NVN compare and stencil op enums have the same order as Vulkan, starting at one. */
    *out = (VkStencilOpState){
        .compareOp = (VkCompareOp)(state.func - 1),
        .failOp = (VkStencilOp)(state.fail - 1),
        .passOp = (VkStencilOp)(state.depth_pass - 1),
        .depthFailOp = (VkStencilOp)(state.depth_fail - 1),
        .compareMask = r->stencil[STENCIL_VALUE_MASK][face],
        .writeMask = r->stencil[STENCIL_WRITE_MASK][face],
        .reference = r->stencil[STENCIL_REFERENCE][face],
    };
    return true;
}

static NvnFrontReplyKind record_draw_strict(NvnReplay *replay, const uint64_t x[32], bool indexed,
                                            bool base_vertex, bool instanced);

/* R50: a draw whose state the bridge does not model is dropped, but the
 * guest's command memory is charged exactly as for a recorded draw. */
/* R61: record a draw for the general path. It keeps the full binding state
 * (every uniform slot, vertex and fragment textures, all streams and
 * attributes) so run_draw_generic() can build the draw from the shaders' own
 * resource lists. Anything still outside it returns BOUNDARY, and the caller
 * skips the draw with its command memory charged. */
static NvnFrontReplyKind record_draw_general(NvnReplay *replay, const uint64_t x[32], bool indexed,
                                             bool base_vertex, bool instanced)
{
    Recording *r = *find_recording(replay, x[0]);
    const uint32_t count = (uint32_t)x[3];
    /* R65: instanced draws are supported (DrawElementsInstanced: x[6] base
     * instance, x[7] instance count). */
    if (!r || count > INT32_MAX || !count || (instanced && !(uint32_t)x[7]) || !r->program[NVN_SHADER_VERTEX])
        { note_skip("general record: empty draw or no vertex program"); return NVN_FRONT_BOUNDARY; }
    for (unsigned i = 2; i < NVN_SHADER_COMPUTE; ++i)
        if (r->program[i])
            { note_skip("general record: geometry or tessellation stage bound"); return NVN_FRONT_BOUNDARY; }
    NvnFrontDeviceModes modes;
    const uint32_t mode = (uint32_t)x[1];
    /* NVN DrawPrimitive: POINTS 0, LINES 1, LINE_LOOP 2, LINE_STRIP 3, TRIANGLES 4, TRIANGLE_STRIP 5, TRIANGLE_FAN 6 */
    if (mode > 6 || mode == 2 ||
        !nvn_front_get_device_modes(replay->config.front, r->device, &modes) ||
        modes.window_origin != NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT || modes.depth_mode != NVN_FRONT_DEPTH_NEAR_IS_ZERO)
        { note_skip("general record: primitive mode or device mode unsupported"); return NVN_FRONT_BOUNDARY; }
    /* NVN IndexType: UNSIGNED_BYTE 0, UNSIGNED_SHORT 1, UNSIGNED_INT 2 */
    const uint32_t index_type = indexed ? (uint32_t)x[2] : 0;
    if (indexed && index_type != 1 && index_type != 2)
        { note_skip("general record: 8-bit indices"); return NVN_FRONT_BOUNDARY; }
    NvnFrontTextureInfo target = {0}, depth_target = {0};
    /* R63: up to eight colour targets. color0 must be present when there are
     * several; later empty slots are allowed. R62 lumped every rejection into one
     * note, so each reason is now noted separately. */
    const uint32_t target_count = r->target_count > 8 ? 8 : r->target_count;
    const bool has_color = target_count >= 1 && r->targets[0];
    if (!has_color && !r->depth_target)
        { note_skip("general record: no colour or depth target bound"); return NVN_FRONT_BOUNDARY; }
    if (!has_color && target_count > 1)
        { note_skip("general record: several targets but color0 empty"); return NVN_FRONT_BOUNDARY; }
    if (has_color && !r->program[NVN_SHADER_FRAGMENT])
        { note_skip("general record: colour target without fragment program"); return NVN_FRONT_BOUNDARY; }
    if (has_color && !get_layered_color_target(replay, r->device, r->targets[0], &target))
        { note_skip("general record: color0 not renderable (unknown, 3D, cube or block format)"); return NVN_FRONT_BOUNDARY; }
    for (uint32_t i = 1; i < target_count; ++i) {
        NvnFrontTextureInfo more;
        if (!r->targets[i])
            continue;
        if (!get_layered_color_target(replay, r->device, r->targets[i], &more))
            { note_skip("general record: MRT colorN not renderable (unknown, 3D, cube or block format)"); return NVN_FRONT_BOUNDARY; }
        if (mip_size(more.create_info.config.width, r->target_levels[i]) !=
                mip_size(target.create_info.config.width, r->target_levels[0]) ||
            mip_size(more.create_info.config.height, r->target_levels[i]) !=
                mip_size(target.create_info.config.height, r->target_levels[0]))
            { note_skip("general record: MRT targets differ in size"); return NVN_FRONT_BOUNDARY; }
    }
    if (r->depth_target && !get_depth_target(replay, r->device, r->depth_target, &depth_target))
        { note_skip("general record: depth target not usable (unknown, mipmapped or not D16/D24S8)"); return NVN_FRONT_BOUNDARY; }
    if (!r->viewport_set || !r->scissor_set || r->scissor[2] <= 0 || r->scissor[3] <= 0)
        { note_skip("general record: viewport or scissor not set"); return NVN_FRONT_BOUNDARY; }
    if ((r->raster_bound & 15) != 15)
        { note_skip("general record: colour, mask, polygon or depth state never bound"); return NVN_FRONT_BOUNDARY; }
    if (r->unsupported_color)
        { note_skip("general record: logic op or alpha test enabled"); return NVN_FRONT_BOUNDARY; }
    if (r->unsupported_polygon || r->polygon_mode != 2)
        { note_skip("general record: non-fill polygon mode"); return NVN_FRONT_BOUNDARY; }
    if (r->unsupported_multisample)
        { note_skip("general record: multisample state unsupported"); return NVN_FRONT_BOUNDARY; }
    const NvnFrontTextureConfig *attachment = has_color ? &target.create_info.config : &depth_target.create_info.config;
    const uint32_t color_level = has_color ? r->target_levels[0] : 0;
    const uint32_t attachment_width = mip_size(attachment->width, color_level);
    const uint32_t attachment_height = mip_size(attachment->height, color_level);
    /* R73: a mip-level colour target may keep a larger depth target bound. */
    if (has_color && r->depth_target &&
        (color_level ? (depth_target.create_info.config.width < attachment_width ||
                        depth_target.create_info.config.height < attachment_height)
                     : (depth_target.create_info.config.width != attachment_width ||
                        depth_target.create_info.config.height != attachment_height)))
        { note_skip("general record: depth target size differs from color target"); return NVN_FRONT_BOUNDARY; }

    Operation operation = {.kind = OP_DRAW};
    Draw *draw = &operation.data.draw;
    draw->generic = true;
    draw->target = has_color ? r->targets[0] : 0;
    draw->target_count = has_color ? (uint8_t)target_count : 0;
    for (uint32_t i = 0; has_color && i < target_count; ++i) {
        draw->target_layers[i] = (uint8_t)r->target_layers[i];
        draw->target_levels[i] = (uint8_t)r->target_levels[i];
    }
    for (uint32_t i = 1; has_color && i < target_count; ++i)
        draw->more_targets[i - 1] = r->targets[i];
    draw->blend_mask = (uint8_t)r->blend_mask;
    draw->color_write_masks = r->color_write_masks;
    draw->depth_bias = r->polygon_offset_fill && (r->polygon_offset[0] != 0.0f || r->polygon_offset[1] != 0.0f);
    draw->depth_bias_values[0] = r->polygon_offset[0];
    draw->depth_bias_values[1] = r->polygon_offset[1];
    draw->depth_target = r->depth_target;
    draw->program[0] = r->program[NVN_SHADER_VERTEX];
    draw->program[1] = has_color ? r->program[NVN_SHADER_FRAGMENT] : 0;
    draw->texture_pool = r->texture_pool;
    draw->sampler_pool = r->sampler_pool;
    memcpy(draw->fragment_textures, r->textures[NVN_SHADER_FRAGMENT], sizeof draw->fragment_textures);
    memcpy(draw->vertex_textures, r->textures[NVN_SHADER_VERTEX], sizeof draw->vertex_textures);
    for (unsigned stage = 0; stage < 2; ++stage)
        memcpy(draw->uniforms[stage], r->uniform[stage], sizeof draw->uniforms[stage]);
    memcpy(draw->streams, r->vertex, sizeof draw->streams);
    for (unsigned i = 0; i < r->attribute_count; ++i) {
        const NvnFrontVertexAttribInfo *attribute = &r->attributes[i];
        if (!attribute->enabled)
            continue;
        if (i >= GENERIC_ATTRIBUTE_SLOTS)
            { note_skip("general record: vertex attribute beyond 16"); return NVN_FRONT_BOUNDARY; }
        if (attribute->stream >= GENERIC_STREAM_SLOTS || attribute->stream >= r->stream_count)
            { note_skip("general record: vertex attribute reads a stream beyond 8 or not set"); return NVN_FRONT_BOUNDARY; }
        /* R65: divisor 1 is a per-instance stream; larger divisors need an extension. */
        if (r->streams[attribute->stream].divisor > 1)
            { note_skip("general record: instanced stream divisor above 1"); return NVN_FRONT_BOUNDARY; }
        draw->attribute_count = (uint8_t)(i + 1);
    }
    memcpy(draw->attributes, r->attributes, draw->attribute_count * sizeof draw->attributes[0]);
    memcpy(draw->stream_info, r->streams, sizeof draw->stream_info);
    draw->nvn_topology = (uint8_t)mode;
    draw->index_size = indexed ? (index_type == 1 ? 2 : 4) : 0;
    draw->cull = r->cull;
    draw->depth_func = r->depth_func;
    draw->index = indexed ? (BufferRange){x[4], (size_t)count * draw->index_size} : (BufferRange){0};
    PokopiaLiveDraw *state = &draw->state;
    state->target = target.native;
    state->depth_target = depth_target.native;
    state->extent = (VkExtent2D){attachment_width, attachment_height};
    state->viewport = (VkViewport){r->viewport[0], r->viewport[1] + r->viewport[3], r->viewport[2],
                                   -r->viewport[3], r->depth_range[0], r->depth_range[1]};
    state->scissor = (VkRect2D){{r->scissor[0], r->scissor[1]}, {(uint32_t)r->scissor[2], (uint32_t)r->scissor[3]}};
    state->color_write_mask = r->color_write_mask;
    state->front_face = r->front_face ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    state->blend_enable = r->blend_enable;
    state->blend = r->blend;
    state->depth_test = r->depth_test;
    state->depth_write = r->depth_write;
    const PokopiaVkTexture *depth_texture = depth_target.native;
    state->stencil_test = r->stencil_test && depth_texture && (depth_texture->aspects & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (state->stencil_test && (!snapshot_stencil(r, 0, &state->stencil[0]) || !snapshot_stencil(r, 1, &state->stencil[1])))
        { note_skip("general record: stencil state not mapped"); return NVN_FRONT_BOUNDARY; }
    state->vertex_count = indexed ? 0 : count;
    state->first_vertex = indexed ? 0 : (uint32_t)x[2];
    state->index_count = indexed ? count : 0;
    state->base_vertex = base_vertex ? (int32_t)x[5] : 0;
    state->instance_count = instanced ? (uint32_t)x[7] : 1;
    state->first_instance = instanced ? (uint32_t)x[6] : 0;
    const size_t bytes = !indexed ? 16 : !base_vertex ? 24 : (state->base_vertex ? 72 : 48);
    return append_metered(replay, r, &operation, bytes, 0);
}

static NvnFrontReplyKind record_draw(NvnReplay *replay, const uint64_t x[32], bool indexed,
                                     bool base_vertex, bool instanced)
{
    /* R78: the general path first. The strict path submits and waits for
     * every draw; it remains for draws the general path does not take. */
    NvnFrontReplyKind result = record_draw_general(replay, x, indexed, base_vertex, instanced);
    if (result != NVN_FRONT_BOUNDARY)
        return result;
    result = record_draw_strict(replay, x, indexed, base_vertex, instanced);
    if (result != NVN_FRONT_BOUNDARY)
        return result;
    Recording *r = *find_recording(replay, x[0]);
    const size_t bytes = !indexed ? 16 : !base_vertex ? 24 : ((int32_t)x[5] ? 72 : 48);
    if (!r || !charge_recording(replay, r, bytes, 0))
        return NVN_FRONT_BOUNDARY;
    ++skip_counts[SKIP_RECORD];
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind record_draw_strict(NvnReplay *replay, const uint64_t x[32], bool indexed,
                                            bool base_vertex, bool instanced)
{
    Recording *r = *find_recording(replay, x[0]);
    uint32_t count = (uint32_t)x[3];
    if (!r || count > INT32_MAX || (!indexed && (uint32_t)x[2] > INT32_MAX))
        return NVN_FRONT_INVALID_GUEST;
    if (!count || (instanced && !(uint32_t)x[7]))
        return NVN_FRONT_HANDLED;
    PokopiaVkProgramStage stages[2] = {0};
    for (unsigned stage = NVN_SHADER_VERTEX; stage <= NVN_SHADER_FRAGMENT; ++stage) {
        NvnFrontProgramInfo program;
        if (r->program[stage] && !require_program_stage(replay->config.front, r->device, r->program[stage],
                                                          stage, &program, &stages[stage]))
            return NVN_FRONT_BOUNDARY;
    }
    const bool depth_only = r->target_count <= 1 && !r->targets[0] && r->depth_target &&
        stages[0].hash && !strcmp(stages[0].hash, "821538a4398f85a7") &&
        !r->program[NVN_SHADER_FRAGMENT] && indexed && r->depth_test && r->depth_write && r->depth_func == 8;
    NvnFrontDeviceModes modes;
    NvnFrontTextureInfo target = {0}, depth_target = {0};
    if (!nvn_front_get_device_modes(replay->config.front, r->device, &modes) ||
        modes.window_origin != NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT ||
        modes.depth_mode != NVN_FRONT_DEPTH_NEAR_IS_ZERO || (uint32_t)x[1] != 4 ||
        (indexed && (uint32_t)x[2] != 1) ||
        (!depth_only && (r->target_count != 1 || !get_color_target(replay, r->device, r->targets[0], &target))) ||
        (r->depth_target && !get_depth_target(replay, r->device, r->depth_target, &depth_target)) ||
        target.create_info.config.levels > 1 || depth_target.create_info.config.levels > 1 ||
        (!r->depth_target && target.create_info.config.depth != 1) ||
        !r->viewport_set || !r->scissor_set || r->scissor[2] <= 0 || r->scissor[3] <= 0 ||
        !r->stream_count || r->streams[0].divisor ||
        (indexed ? r->attribute_count < 3 : r->attribute_count != 1) ||
        (r->raster_bound & 15) != 15 || r->unsupported_color || r->unsupported_polygon ||
        r->polygon_offset_fill || r->unsupported_multisample || r->polygon_mode != 2 || r->cull)
        return NVN_FRONT_BOUNDARY;
    for (unsigned i = 2; i < NVN_SHADER_COMPUTE; ++i)
        if (r->program[i])
            return NVN_FRONT_BOUNDARY;

    const NvnFrontTextureConfig *attachment = depth_only ? &depth_target.create_info.config : &target.create_info.config;
    const PokopiaVkTexture *depth_texture = depth_target.native;
    const bool stencil_test = r->stencil_test && depth_texture &&
                              (depth_texture->aspects & VK_IMAGE_ASPECT_STENCIL_BIT);
    Operation operation = {.kind = OP_DRAW, .data.draw = {
        .target = r->targets[0], .depth_target = r->depth_target,
        .program = {r->program[0], r->program[1]},
        .texture_pool = r->texture_pool, .sampler_pool = r->sampler_pool,
        .vertex = r->vertex[0], .index = indexed ? (BufferRange){x[4], (size_t)count * 2} : (BufferRange){0},
        .uniform = {r->uniform[0][0], r->uniform[1][0]},
        .state = {
            .target = target.native, .depth_target = depth_target.native, .target_layer = r->target_layers[0],
            .extent = {attachment->width, attachment->height},
            .vertex_stride = r->streams[0].stride & 0xfff, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .index_type = VK_INDEX_TYPE_UINT16,
            .viewport = {r->viewport[0], r->viewport[1] + r->viewport[3], r->viewport[2],
                         -r->viewport[3], r->depth_range[0], r->depth_range[1]},
            .scissor = {{r->scissor[0], r->scissor[1]}, {(uint32_t)r->scissor[2], (uint32_t)r->scissor[3]}},
            .color_write_mask = r->color_write_mask, .polygon_mode = VK_POLYGON_MODE_FILL,
            .cull_mode = VK_CULL_MODE_NONE,
            .front_face = r->front_face ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
            .blend_enable = r->blend_enable, .blend = r->blend,
            .depth_test = r->depth_test, .depth_write = r->depth_write, .stencil_test = stencil_test,
            .vertex_count = indexed ? 0 : count, .first_vertex = indexed ? 0 : (uint32_t)x[2],
            .index_count = indexed ? count : 0, .base_vertex = base_vertex ? (int32_t)x[5] : 0,
            .instance_count = instanced ? (uint32_t)x[7] : 1, .first_instance = instanced ? (uint32_t)x[6] : 0,
        },
    }};
    if (stencil_test && (!snapshot_stencil(r, 0, &operation.data.draw.state.stencil[0]) ||
                         !snapshot_stencil(r, 1, &operation.data.draw.state.stencil[1])))
        return NVN_FRONT_INVALID_GUEST;
    memcpy(operation.data.draw.fragment_textures, r->textures[NVN_SHADER_FRAGMENT],
           sizeof operation.data.draw.fragment_textures);
    const unsigned required_attributes = indexed ? 3 : 1;
    for (unsigned i = 0; i < r->attribute_count; ++i) {
        const NvnFrontVertexAttribInfo *attribute = &r->attributes[i];
        if (i >= 4 || !attribute->enabled) {
            if (attribute->enabled || i < required_attributes)
                return NVN_FRONT_BOUNDARY;
            continue;
        }
        if (attribute->stream)
            return NVN_FRONT_BOUNDARY;
        operation.data.draw.state.attributes[i] = (VkVertexInputAttributeDescription){
            i, 0, nvn_guest_texture_vk_format(attribute->format), attribute->offset,
        };
    }
    if (instanced)
        return NVN_FRONT_BOUNDARY;
    /* R62: keep the general path's binding state too, so run_draw() can fall
     * back to it when the pinned path rejects the draw at run time (R61's
     * tonemap pass, which puts every frame on screen, failed exactly there). */
    Draw *fallback = &operation.data.draw;
    memcpy(fallback->vertex_textures, r->textures[NVN_SHADER_VERTEX], sizeof fallback->vertex_textures);
    for (unsigned stage = 0; stage < 2; ++stage)
        memcpy(fallback->uniforms[stage], r->uniform[stage], sizeof fallback->uniforms[stage]);
    memcpy(fallback->streams, r->vertex, sizeof fallback->streams);
    const unsigned fallback_attributes = r->attribute_count < GENERIC_ATTRIBUTE_SLOTS ?
                                         r->attribute_count : GENERIC_ATTRIBUTE_SLOTS;
    memcpy(fallback->attributes, r->attributes, fallback_attributes * sizeof fallback->attributes[0]);
    fallback->attribute_count = (uint8_t)fallback_attributes;
    memcpy(fallback->stream_info, r->streams, sizeof fallback->stream_info);
    fallback->nvn_topology = 4; /* the pinned path records triangle lists only */
    fallback->index_size = indexed ? 2 : 0;
    fallback->cull = r->cull;
    fallback->depth_func = r->depth_func;
    if (!indexed)
        return append_metered(replay, r, &operation, 16, 0);
    const size_t bytes = !base_vertex ? 24 : (operation.data.draw.state.base_vertex ? 72 : 48);
    return append_metered(replay, r, &operation, bytes, 0);
}

static NvnFrontReplyKind record_dispatch_strict(NvnReplay *replay, const uint64_t x[32]);

/* R52: a compute dispatch the bridge cannot run (untranslated shader or
 * unknown resource layout) is dropped with its 16-byte control record still
 * charged, matching draws. */
static NvnFrontReplyKind record_dispatch(NvnReplay *replay, const uint64_t x[32])
{
    const NvnFrontReplyKind result = record_dispatch_strict(replay, x);
    if (result != NVN_FRONT_BOUNDARY)
        return result;
    Recording *r = *find_recording(replay, x[0]);
    if (!r || !charge_recording(replay, r, 0, 16))
        return NVN_FRONT_BOUNDARY;
    ++skip_counts[SKIP_RECORD];
    {
        /* R73: say which dispatch was dropped and why. */
        NvnFrontProgramInfo program;
        PokopiaVkProgramStage stage = {0};
        const bool linked = require_program_stage(replay->config.front, r->device, r->program[NVN_SHADER_COMPUTE],
                                                  NVN_SHADER_COMPUTE, &program, &stage);
        char note[128];
        snprintf(note, sizeof note, "record dispatch dropped %s: %s", stage.hash ? stage.hash : "-",
                 !linked ? "program not linked or not translated" : !stage.resources ? "no resource list" :
                                                                    "pinned layout rejected it");
        note_skip(note);
    }
    if (nvn_replay_trace_log && nvn_replay_trace_submits > 0) {
        NvnFrontProgramInfo program;
        PokopiaVkProgramStage stage = {0};
        require_program_stage(replay->config.front, r->device, r->program[NVN_SHADER_COMPUTE], NVN_SHADER_COMPUTE,
                              &program, &stage);
        fprintf(nvn_replay_trace_log, "rec dispatch SKIP %s %ux%ux%u\n", stage.hash ? stage.hash : "-",
                (unsigned)x[1], (unsigned)x[2], (unsigned)x[3]);
    }
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind record_dispatch_strict(NvnReplay *replay, const uint64_t x[32])
{
    Recording *r = *find_recording(replay, x[0]);
    if (!r)
        return NVN_FRONT_INVALID_GUEST;
    if (!replay->config.compute)
        return NVN_FRONT_BOUNDARY;
    NvnFrontProgramInfo program;
    PokopiaVkProgramStage stage;
    if (!require_program_stage(replay->config.front, r->device, r->program[NVN_SHADER_COMPUTE],
                               NVN_SHADER_COMPUTE, &program, &stage))
        return NVN_FRONT_BOUNDARY;
    const ComputeAbi abi = compute_abi(&stage);
    const uint32_t groups[] = {(uint32_t)x[1], (uint32_t)x[2], (uint32_t)x[3]};
    if (abi == COMPUTE_UNSUPPORTED && replay->config.compute_generic && stage.resources) {
        /* R73: keep every compute binding; run_compute_generic() maps the
         * shader's c[0] offsets to these slots when the list is submitted. */
        Operation operation = {.kind = OP_DISPATCH, .data.dispatch = {
            .generic = true, .program = r->program[NVN_SHADER_COMPUTE], .texture_pool = r->texture_pool,
            .sampler_pool = r->sampler_pool, .groups = {groups[0], groups[1], groups[2]},
        }};
        ComputeDispatch *d = &operation.data.dispatch;
        for (unsigned i = 0; i < 8; ++i) {
            d->textures[i] = r->textures[NVN_SHADER_COMPUTE][i];
            d->images[i] = r->compute_images[i];
            d->uniforms[i] = r->uniform[NVN_SHADER_COMPUTE][i];
        }
        for (unsigned i = 0; i < COMPUTE_STORAGE_SLOTS; ++i)
            d->ssbo[i] = r->compute_storage[i];
        return append_metered(replay, r, &operation, 0, 16);
    }
    const size_t cluster_bytes = pokopia_cluster_output_bytes(groups);
    if (abi == COMPUTE_UNSUPPORTED ||
        (abi == COMPUTE_RESET_COUNTER && (groups[0] != 1 || groups[1] != 1 || groups[2] != 1)) ||
        (abi == COMPUTE_CLUSTERED_LIGHTS && (!cluster_bytes || r->compute_storage[0].size != cluster_bytes)))
        return NVN_FRONT_BOUNDARY;
    const Operation operation = {.kind = OP_DISPATCH, .data.dispatch = {
        .program = r->program[NVN_SHADER_COMPUTE], .texture_pool = r->texture_pool,
        .texture_handles = {r->textures[NVN_SHADER_COMPUTE][0], r->textures[NVN_SHADER_COMPUTE][1]},
        .uniform = r->uniform[NVN_SHADER_COMPUTE][0],
        .storage = {r->compute_storage[1], r->compute_storage[0]},
        .groups = {groups[0], groups[1], groups[2]},
    }};
    /* SDK 0x60d9c0 emits a 16-byte dispatch control record. */
    return append_metered(replay, r, &operation, 0, 16);
}

static PokopiaLiveDrawSpan draw_span(const NvnReplayConfig *config, uint64_t device, BufferRange range)
{
    NvnFrontPoolHit hit;
    if (!range.size || !nvn_front_resolve_pool(config->front, device, range.address, range.size, &hit))
        return (PokopiaLiveDrawSpan){0};
    return (PokopiaLiveDrawSpan){config->read_guest(config->opaque, range.address, range.size), range.size};
}

typedef struct {
    bool uniform0;
    int fragment_slots[POKOPIA_LIVE_DRAW_SAMPLE_COUNT];
} DrawAbi;

static bool draw_abi(const char *resources, unsigned stage, DrawAbi *abi)
{
    *abi = (DrawAbi){.fragment_slots = {-1, -1}};
    if (!resources)
        return false;

    const char *uniform = stage == NVN_SHADER_VERTEX ? "cb3=s0b4" : "cb3=s0b22";
    for (const char *token = resources; *token;) {
        const size_t length = strcspn(token, " ");
        if (length == strlen(uniform) && !strncmp(token, uniform, length) && !abi->uniform0) {
            abi->uniform0 = true;
        } else if (length) {
            /* NVN graphics c[2] stores one 64-bit handle per sampler
               slot, beginning at word 8. The cache writes c[2] as c-1. */
            const char prefix[] = "texTexture2D[c-1:";
            const char suffix[] = "]=s2b";
            const size_t prefix_length = sizeof prefix - 1, suffix_length = sizeof suffix - 1;
            if (stage != NVN_SHADER_FRAGMENT ||
                length <= prefix_length + suffix_length + 3 || strncmp(token, prefix, prefix_length))
                return false;
            const char *number = token + prefix_length;
            const size_t digits = strspn(number, "0123456789");
            if (!digits || digits > 2 || length != prefix_length + digits + suffix_length + 3 ||
                strncmp(number + digits, suffix, suffix_length))
                return false;
            const char *binding = number + digits + suffix_length;
            const unsigned word = (unsigned)strtoul(number, NULL, 10);
            if (word < 8 || (word - 8) % 2 || (word - 8) / 2 >= TEXTURE_SLOTS ||
                binding[0] != '1' || binding[1] != '2' || (binding[2] != '8' && binding[2] != '9'))
                return false;
            const unsigned sample = (unsigned)(binding[2] - '8');
            if (abi->fragment_slots[sample] >= 0)
                return false;
            abi->fragment_slots[sample] = (int)(word - 8) / 2;
        }
        token += length;
        if (*token)
            ++token;
    }
    return true;
}

/* R66: swizzle_out (general draws only) accepts a component swizzle; the
 * pinned paths pass NULL and still require the identity. A view of the whole
 * mip chain is the same as no level selection. */
static bool resolve_sampled_view(const NvnFrontTextureDescriptorInfo *descriptor, PokopiaLiveDrawSample *sample,
                                 uint8_t swizzle_out[4])
{
    if (!descriptor->has_view)
        return true;
    const uint8_t *view = descriptor->view;
    const uint8_t fields = view[7];
    const NvnFrontTextureConfig *config = &descriptor->texture.create_info.config;
    uint16_t layer, layers;
    uint32_t format;
    const uint8_t identity[] = {2, 3, 4, 5};
    memcpy(&layer, view, sizeof layer);
    memcpy(&layers, view + 2, sizeof layers);
    memcpy(&format, view + 8, sizeof format);
    const bool all_levels = !view[4] && (view[5] == 1 || view[5] == config->levels);
    /* R66: a view of every layer of an array texture is the whole texture. */
    const uint32_t texture_layers = config->target == 4 || config->target == 9 ? config->depth :
                                    config->target == 8 ? 6 : 1;
    const bool all_layers = !layer && (layers == 1 || layers == texture_layers);
    if (((fields & 1u) && !all_levels) ||
        ((fields & 2u) && !all_layers) ||
        ((fields & 32u) && view[6] != config->target))
        return false;
    if ((fields & 8u) && memcmp(view + 12, identity, sizeof identity)) {
        if (!swizzle_out || view[12] > 5 || view[13] > 5 || view[14] > 5 || view[15] > 5)
            return false;
        memcpy(swizzle_out, view + 12, 4);
    }
    if ((fields & 4u) && format != config->format) {
        if (!sample)
            return false;
        const PokopiaVkTexture *texture = descriptor->texture.native;
        const VkFormat view_format = nvn_guest_texture_vk_format(format);
        if ((!texture->sample_alias_view && !texture->deferred) || view_format == VK_FORMAT_UNDEFINED ||
            view_format != texture->sample_alias_format)
            return false;
        sample->view_format = view_format;
    }
    return true;
}

static bool resolve_fragment_sample(NvnFront *front, uint64_t device, const Draw *record,
                                    unsigned slot, PokopiaLiveDrawSample *sample, NvnFrontTextureInfo *guest_info)
{
    const uint64_t handle = record->fragment_textures[slot];
    NvnFrontDescriptorPoolInfo texture_pool, sampler_pool;
    NvnFrontTextureDescriptorInfo desc;
    if (!nvn_front_get_descriptor_pool(front, record->texture_pool, false, &texture_pool) ||
        !nvn_front_get_descriptor_pool(front, record->sampler_pool, true, &sampler_pool) ||
        texture_pool.device != device || sampler_pool.device != device ||
        !nvn_front_get_texture_descriptor(front, record->texture_pool, (uint32_t)handle & 0xfffff, &desc) ||
        desc.image || !desc.texture.native ||
        !nvn_front_get_sampler_descriptor(front, record->sampler_pool, (uint32_t)(handle >> 20) & 0xfff,
                                          &sample->sampler))
        return false;
    const NvnFrontTextureConfig *texture = &desc.texture.create_info.config;
    if (texture->device != device || texture->target != 1 || texture->depth != 1 || texture->levels != 1)
        return false;
    if (!resolve_sampled_view(&desc, sample, NULL))
        return false;
    *guest_info = desc.texture;
    sample->texture = desc.texture.native;
    sample->guest_info = guest_info;
    return true;
}

/* R61: NVN blend, compare and face enums to Vulkan. NVN BlendFunc: ZERO 1, ONE 2,
 * SRC_COLOR 3 ... SRC_ALPHA_SATURATE 11, SRC1_* 16-19, CONSTANT_* 97-100. */
static bool generic_blend_factor(uint32_t nvn, VkBlendFactor *out)
{
    static const VkBlendFactor basic[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
    };
    static const VkBlendFactor dual[] = {
        VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
        VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
    };
    static const VkBlendFactor constant[] = {
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
    };
    if (nvn >= 1 && nvn <= 11)
        *out = basic[nvn - 1];
    else if (nvn >= 16 && nvn <= 19)
        *out = dual[nvn - 16];
    else if (nvn >= 97 && nvn <= 100)
        *out = constant[nvn - 97];
    else
        return false;
    return true;
}

static bool generic_blend_op(uint32_t nvn, VkBlendOp *out)
{
    /* NVN BlendEquation: ADD 1, SUBTRACT 2, REVERSE_SUBTRACT 3, MIN 4, MAX 5. */
    static const VkBlendOp ops[] = {
        VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
    };
    if (nvn < 1 || nvn > 5)
        return false;
    *out = ops[nvn - 1];
    return true;
}

/* A texture binding from a resource token such as "texTexture2D|Array[c-1:22]=s2b129".
 * NVN graphics stages keep one 64-bit handle per texture slot in c[2] starting at
 * word 8, which the cache writes as c-1. Returns the slot, the binding and the
 * NVN texture target the shader expects. */
enum { GENERIC_TOKEN_IMAGE, GENERIC_TOKEN_SHADOW, GENERIC_TOKEN_BUFFER };

static bool generic_texture_token(const char *token, size_t length, unsigned *slot, unsigned *binding,
                                  uint32_t *nvn_target, unsigned *kind)
{
    /* R73: shadow (depth compare) and buffer textures. */
    static const struct { const char *type; uint32_t target; unsigned kind; } types[] = {
        {"texTexture2D[", 1, GENERIC_TOKEN_IMAGE}, {"texTexture2D|Array[", 4, GENERIC_TOKEN_IMAGE},
        {"texTexture3D[", 2, GENERIC_TOKEN_IMAGE}, {"texTextureCube[", 8, GENERIC_TOKEN_IMAGE},
        {"texTextureCube|Array[", 9, GENERIC_TOKEN_IMAGE},
        {"texTexture2D|Shadow[", 1, GENERIC_TOKEN_SHADOW}, {"texTexture2D|Array|Shadow[", 4, GENERIC_TOKEN_SHADOW},
        {"texTextureCube|Array|Shadow[", 9, GENERIC_TOKEN_SHADOW},
        {"texTextureBuffer[", 0xa, GENERIC_TOKEN_BUFFER},
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; ++i) {
        const size_t prefix = strlen(types[i].type);
        if (length <= prefix || strncmp(token, types[i].type, prefix))
            continue;
        const char *cursor = token + prefix;
        if (strncmp(cursor, "c-1:", 4))
            return false;
        char *end;
        const unsigned long word = strtoul(cursor + 4, &end, 10);
        if (end == cursor + 4 || strncmp(end, "]=s2b", 5) || word < 8 || (word - 8) % 2 ||
            (word - 8) / 2 >= TEXTURE_SLOTS)
            return false;
        char *tail;
        const unsigned long bound = strtoul(end + 5, &tail, 10);
        if (tail == end + 5 || (size_t)(tail - token) != length)
            return false;
        *slot = (unsigned)(word - 8) / 2;
        *binding = (unsigned)bound;
        *nvn_target = types[i].target;
        *kind = types[i].kind;
        return true;
    }
    return false;
}

/* R66: detailed notes for draws the general path skips, capped so they never
 * crowd out the other notes. */
static void note_skip_detail(const char *text)
{
    if (skip_note_count < SKIP_NOTES - 24)
        note_skip(text);
    else
        note_skip("general draw: (more skip details dropped)");
}

/* R87: set by the loader: an RGBA8 texel buffer of this many bytes. */
PokopiaVkTexture *(*nvn_replay_texel_placeholder)(uint64_t bytes);
/* R87: set by the loader (see pokopia_vk_volume_placeholder). */
void *(*nvn_replay_volume_placeholder)(const NvnFrontTextureConfig *like, uint32_t rgba8);

static bool generic_resolve_texture(NvnFront *front, uint64_t device, uint64_t record_texture_pool,
                                    uint64_t record_sampler_pool, uint64_t handle,
                                    uint32_t expected_target, PokopiaGenericTexture *out, NvnFrontTextureInfo *info,
                                    bool execute)
{
    const struct { uint64_t texture_pool, sampler_pool; } pools = {record_texture_pool, record_sampler_pool},
                                                          *record = &pools;
#define note_skip_detail(text) do { if (execute) note_skip_detail(text); } while (0)
    NvnFrontDescriptorPoolInfo texture_pool, sampler_pool;
    NvnFrontTextureDescriptorInfo desc;
    char note[160];
    if (!nvn_front_get_descriptor_pool(front, record->texture_pool, false, &texture_pool) ||
        !nvn_front_get_descriptor_pool(front, record->sampler_pool, true, &sampler_pool) ||
        texture_pool.device != device || sampler_pool.device != device) {
        note_skip_detail("general draw texture: descriptor pools unknown");
        return false;
    }
    if (!nvn_front_get_texture_descriptor(front, record->texture_pool, (uint32_t)handle & 0xfffff, &desc)) {
        note_skip_detail(handle ? "general draw texture: texture id not registered" :
                                  "general draw texture: handle is 0 (nothing bound)");
        return false;
    }
    if (desc.image || !desc.texture.native) {
        const NvnFrontTextureConfig *c = &desc.texture.create_info.config;
        snprintf(note, sizeof note, "general draw texture: %s target=%u fmt=%u %ux%ux%u",
                 desc.image ? "id is an image descriptor" : "no Vulkan backing", c->target, c->format,
                 c->width, c->height, c->depth);
        note_skip_detail(note);
        return false;
    }
    if (!nvn_front_get_sampler_descriptor(front, record->sampler_pool, (uint32_t)(handle >> 20) & 0xfff,
                                          &out->sampler)) {
        note_skip_detail("general draw texture: sampler id not registered");
        return false;
    }
    const NvnFrontTextureConfig *config = &desc.texture.create_info.config;
    const uint32_t target = config->target;
    /* A cube array texture also serves a cube view, and (R73) a 2D texture a
     * 2D array binding through a one-layer array view; otherwise the targets
     * must agree. The game binds 8x8 and 128x128 2D placeholders to array slots. */
    const bool as_array = expected_target == 4 && target == 1;
    /* R87: a shader sampling a 3D texture where the game bound its small 2D
     * placeholder (8x8 RGBA8; ~13 draws a frame outdoors, among them the sky's)
     * gets a 1x1x1 volume of the placeholder's first texel; the GPU would have
     * drawn them anyway. */
    if (expected_target == 2 && target == 1 && config->depth == 1 && config->width <= 64 && config->height <= 64 &&
        nvn_replay_volume_placeholder) {
        uint32_t rgba = UINT32_MAX;
        if (desc.texture.create_info.storage_address)
            memcpy(&rgba, (const void *)(uintptr_t)desc.texture.create_info.storage_address, sizeof rgba);
        void *volume = nvn_replay_volume_placeholder(config, rgba);
        PokopiaLiveDrawSample sample = {0};
        if (volume && resolve_sampled_view(&desc, &sample, out->swizzle)) {
            *info = desc.texture;
            out->texture = volume;
            out->guest_info = NULL;
            out->view_format = VK_FORMAT_UNDEFINED;
            out->as_array = false;
            return true;
        }
    }
    if (target != expected_target && !(expected_target == 8 && target == 9) && !as_array) {
        snprintf(note, sizeof note, "general draw texture: shader wants target %u, texture is %u fmt=%u %ux%ux%u view=%d",
                 expected_target, target, config->format, config->width, config->height, config->depth,
                 desc.has_view ? desc.view[7] : -1);
        note_skip_detail(note);
        return false;
    }
    PokopiaLiveDrawSample sample = {0};
    if (!resolve_sampled_view(&desc, &sample, out->swizzle)) {
        uint16_t layer, layers;
        uint32_t format;
        memcpy(&layer, desc.view, sizeof layer);
        memcpy(&layers, desc.view + 2, sizeof layers);
        memcpy(&format, desc.view + 8, sizeof format);
        snprintf(note, sizeof note,
                 "general draw texture: view rejected fields=0x%x level=%u+%u layer=%u+%u vtarget=%u fmt=%u->%u swz=%u%u%u%u tex target=%u levels=%u",
                 desc.view[7], desc.view[4], desc.view[5], layer, layers, desc.view[6], config->format, format,
                 desc.view[12], desc.view[13], desc.view[14], desc.view[15], target, config->levels);
        note_skip_detail(note);
        return false;
    }
#undef note_skip_detail
    *info = desc.texture;
    out->texture = desc.texture.native;
    out->guest_info = info;
    out->view_format = sample.view_format;
    out->as_array = as_array;
    return true;
}

/* R73: a texTextureBuffer binding: the NVN buffer texture's texel backing
 * (registered when the texture was created) and its guest bytes. */
static bool generic_resolve_texel(const NvnReplayConfig *config, uint64_t device, uint64_t record_texture_pool,
                                  uint64_t handle, PokopiaGenericTexture *out, uint64_t *source, bool execute)
{
    const struct { uint64_t texture_pool; } pools = {record_texture_pool}, *record = &pools;
    NvnFrontDescriptorPoolInfo texture_pool;
    NvnFrontTextureDescriptorInfo desc;
    if (!nvn_front_get_descriptor_pool(config->front, record->texture_pool, false, &texture_pool) ||
        texture_pool.device != device ||
        !nvn_front_get_texture_descriptor(config->front, record->texture_pool, (uint32_t)handle & 0xfffff, &desc) ||
        desc.image || !desc.texture.native) {
        if (execute)
            note_skip(handle ? "general draw texel: texture id unknown" : "general draw texel: handle is 0");
        return false;
    }
    const NvnFrontTextureCreateInfo *info = &desc.texture.create_info;
    PokopiaVkTexture *texture = desc.texture.native;
    /* R87: a 2D placeholder (8x8 RGBA8) bound where the shader reads a buffer
     * texture: its bytes are read as RGBA8 texels, as the GPU would read them
     * (~11 draws a frame outdoors). */
    if (info->config.device == device && info->config.target == 1 && info->storage_size &&
        info->storage_size <= 0x10000 && nvn_replay_texel_placeholder) {
        PokopiaVkTexture *placeholder = nvn_replay_texel_placeholder(info->storage_size);
        if (placeholder) {
            out->texture = placeholder;
            out->texel = &placeholder->texel;
            out->guest_info = NULL;
            *source = info->storage_address;
            return true;
        }
    }
    if (info->config.device != device || info->config.target != 0xa || !texture->texel.device ||
        !texture->texel.bytes || texture->texel.bytes > info->storage_size) {
        if (execute) {
            char note[160];
            snprintf(note, sizeof note, "general draw texel: not a texel-backed buffer texture target=%u fmt=%u bytes=%#llx",
                     info->config.target, info->config.format, (unsigned long long)info->storage_size);
            note_skip(note);
        }
        return false;
    }
    out->texture = texture;
    out->texel = &texture->texel;
    out->guest_info = NULL;
    *source = info->storage_address;
    return true;
}

/* R73: bumped once per submit, so a texel buffer is copied from guest memory
 * by its first general draw in each submit rather than by every draw. */
static uint64_t texel_epoch = 1;

bool pokopia_live_generic_direct(const void *data, size_t bytes);

/* R80: a shader's resource list parsed once (it was re-parsed with strcspn and
 * strtoul for every draw). Entries hold a copy of the string they parsed. */
typedef struct {
    uint8_t texture; /* 0: uniform buffer cbN, 1: texture token */
    uint8_t kind;
    unsigned cb, binding, slot;
    uint32_t nvn_target;
} ParsedResource;
enum { PARSED_RESOURCES = 64, PARSED_CACHE = 512 };
typedef struct {
    const char *key;
    char *copy;
    unsigned count;
    ParsedResource items[PARSED_RESOURCES];
} ParsedResources;
static ParsedResources *parsed_cache[PARSED_CACHE];

static bool generic_texture_token(const char *token, size_t length, unsigned *slot, unsigned *binding,
                                  uint32_t *nvn_target, unsigned *kind);

/* NULL when any token is something the plain loop must judge (and note). */
static const ParsedResources *parsed_resources(const char *resources)
{
    const size_t at = (size_t)(((uintptr_t)resources >> 3) * UINT64_C(0x9e3779b97f4a7c15) >> 55) % PARSED_CACHE;
    ParsedResources *entry = parsed_cache[at];
    if (entry && entry->key == resources && !strcmp(entry->copy, resources))
        return entry->count == UINT_MAX ? NULL : entry;
    if (!entry) {
        entry = calloc(1, sizeof *entry);
        if (!entry)
            return NULL;
        parsed_cache[at] = entry;
    }
    free(entry->copy);
    entry->copy = strdup(resources);
    entry->key = entry->copy ? resources : NULL;
    entry->count = 0;
    bool ok = entry->copy != NULL;
    for (const char *token = resources; *token && ok;) {
        const size_t length = strcspn(token, " ");
        ParsedResource item = {0};
        if (length > 2 && !strncmp(token, "cb", 2)) {
            char *end;
            item.cb = (unsigned)strtoul(token + 2, &end, 10);
            ok = !strncmp(end, "=s0b", 4);
            item.binding = ok ? (unsigned)strtoul(end + 4, NULL, 10) : 0;
        } else if (length > 3 && !strncmp(token, "tex", 3)) {
            item.texture = 1;
            unsigned kind = 0;
            ok = generic_texture_token(token, length, &item.slot, &item.binding, &item.nvn_target, &kind);
            item.kind = (uint8_t)kind;
        } else if (length) {
            ok = false;
        }
        if (ok && length) {
            ok = entry->count < PARSED_RESOURCES;
            if (ok)
                entry->items[entry->count++] = item;
        }
        token += length;
        if (*token)
            ++token;
    }
    if (!ok)
        entry->count = UINT_MAX;
    return ok ? entry : NULL;
}

static NvnFrontReplyKind run_draw_generic(NvnReplay *replay, uint64_t device, const Draw *record, bool execute)
{
    const NvnReplayConfig *config = &replay->config;
#define GENERIC_SKIP(why) do { if (execute) { ++skip_counts[SKIP_RUN]; if (why) note_skip(why); \
                                             TRACE("draw SKIP %s\n", (why) ? (const char *)(why) : "-"); } \
                               return NVN_FRONT_HANDLED; } while (0)
    if (!config->draw_generic)
        GENERIC_SKIP(NULL);
    PokopiaGenericDraw draw = {0};
    NvnFrontTextureInfo attachment;
    /* R63: a target released or re-created since recording skips the draw
     * instead of stopping the game. */
    if (record->target) {
        if (!get_layered_color_target(replay, device, record->target, &attachment))
            GENERIC_SKIP("general draw: color target no longer renderable");
        draw.target = attachment.native;
        draw.color_count = record->target_count > 1 ? record->target_count : 1;
        draw.color_layers[0] = record->target_count ? record->target_layers[0] : record->state.target_layer;
        draw.color_levels[0] = record->target_count ? record->target_levels[0] : 0;
        if (draw.color_levels[0] >= attachment.create_info.config.levels)
            GENERIC_SKIP("general draw: target level beyond the texture");
        for (uint32_t i = 1; i < draw.color_count; ++i) {
            if (!record->more_targets[i - 1])
                continue;
            if (!get_layered_color_target(replay, device, record->more_targets[i - 1], &attachment))
                GENERIC_SKIP("general draw: MRT target no longer renderable");
            draw.more_targets[i - 1] = attachment.native;
            draw.color_layers[i] = record->target_layers[i];
            draw.color_levels[i] = record->target_levels[i];
            if (draw.color_levels[i] >= attachment.create_info.config.levels)
                GENERIC_SKIP("general draw: target level beyond the texture");
        }
        for (uint32_t i = 0; i < draw.color_count; ++i) {
            const PokopiaVkTexture *color = i ? draw.more_targets[i - 1] : draw.target;
            if (color && draw.color_layers[i] >= color->array_layers)
                GENERIC_SKIP("general draw: target layer beyond the texture");
        }
        draw.color_write_masks = record->target_count ? record->color_write_masks :
                                                        (uint32_t)record->state.color_write_mask;
        draw.blend_mask = record->target_count ? record->blend_mask : (record->state.blend_enable ? 1u : 0u);
    }
    if (record->depth_target) {
        if (!get_depth_target(replay, device, record->depth_target, &attachment))
            GENERIC_SKIP("general draw: depth target no longer usable");
        draw.depth_target = attachment.native;
    }
    draw.extent = record->state.extent;

    NvnFrontTextureInfo texture_infos[POKOPIA_GENERIC_TEXTURES];
    uint64_t texel_sources[POKOPIA_GENERIC_TEXTURES] = {0};
    const PokopiaVkProgram *programs[2] = {0};
    PokopiaVkProgramStage stages[2] = {0};
    const unsigned stage_count = record->target ? 2 : 1;
    for (unsigned stage = 0; stage < stage_count; ++stage) {
        NvnFrontProgramInfo program;
        if (!require_program_stage(config->front, device, record->program[stage], stage, &program, &stages[stage]))
            GENERIC_SKIP("general draw: program stage unavailable");
        programs[stage] = program.native;
        const char *resources = stages[stage].resources ? stages[stage].resources : "";
        const ParsedResources *parsed = parsed_resources(resources);
        for (unsigned item_at = 0; parsed && item_at < parsed->count; ++item_at) {
            const ParsedResource *item = &parsed->items[item_at];
            if (!item->texture) {
                const unsigned long cb = item->cb;
                if (cb == 1 && stages[stage].constants && stages[stage].constants_bytes) {
                    if (draw.uniform_count >= POKOPIA_GENERIC_UNIFORMS)
                        GENERIC_SKIP("general draw: uniform slot out of range");
                    draw.uniforms[draw.uniform_count++] = (PokopiaGenericUniform){
                        item->binding, {stages[stage].constants, stages[stage].constants_bytes}};
                } else if (cb < 3) {
                    GENERIC_SKIP("general draw: shader reads a driver constant buffer (cb0-cb2)");
                } else {
                    if (cb - 3 >= GENERIC_UNIFORM_SLOTS || draw.uniform_count >= POKOPIA_GENERIC_UNIFORMS)
                        GENERIC_SKIP("general draw: uniform slot out of range");
                    const PokopiaLiveDrawSpan span = draw_span(config, device, record->uniforms[stage][cb - 3]);
                    if (!span.data)
                        GENERIC_SKIP("general draw: uniform buffer not bound");
                    draw.uniforms[draw.uniform_count++] = (PokopiaGenericUniform){item->binding, span};
                }
                continue;
            }
            const unsigned slot = item->slot;
            if (draw.texture_count >= POKOPIA_GENERIC_TEXTURES ||
                (stage == NVN_SHADER_VERTEX && slot >= GENERIC_VERTEX_TEXTURES))
                GENERIC_SKIP("general draw: too many textures");
            const uint64_t handle = stage == NVN_SHADER_VERTEX ? record->vertex_textures[slot] :
                                                                  record->fragment_textures[slot];
            PokopiaGenericTexture *texture = &draw.textures[draw.texture_count];
            texture->binding = item->binding;
            if (item->kind == GENERIC_TOKEN_BUFFER) {
                if (!generic_resolve_texel(config, device, record->texture_pool, handle, texture,
                                           &texel_sources[draw.texture_count], execute))
                    GENERIC_SKIP("general draw: texture buffer did not resolve");
            } else if (!generic_resolve_texture(config->front, device, record->texture_pool, record->sampler_pool,
                                                handle, item->nvn_target, texture,
                                                &texture_infos[draw.texture_count], execute)) {
                GENERIC_SKIP("general draw: texture handle did not resolve to a matching texture");
            }
            texture->shadow = item->kind == GENERIC_TOKEN_SHADOW;
            ++draw.texture_count;
        }
        if (parsed)
            continue;
        for (const char *token = resources; *token;) {
            const size_t length = strcspn(token, " ");
            unsigned slot, binding, kind;
            uint32_t nvn_target;
            if (length > 2 && !strncmp(token, "cb", 2)) {
                char *end;
                const unsigned long cb = strtoul(token + 2, &end, 10);
                if (strncmp(end, "=s0b", 4))
                    GENERIC_SKIP("general draw: unexpected uniform binding");
                binding = (unsigned)strtoul(end + 4, NULL, 10);
                /* NVN uniform slot n is hardware c[n + 3]; c[0..2] are driver constants.
                 * R66: c[1] holds the shader's own embedded constants. */
                if (cb == 1 && stages[stage].constants && stages[stage].constants_bytes) {
                    if (draw.uniform_count >= POKOPIA_GENERIC_UNIFORMS)
                        GENERIC_SKIP("general draw: uniform slot out of range");
                    draw.uniforms[draw.uniform_count++] = (PokopiaGenericUniform){
                        binding, {stages[stage].constants, stages[stage].constants_bytes}};
                } else if (cb < 3) {
                    if (execute) {
                        char note[160];
                        snprintf(note, sizeof note, "general draw: stage%u %s reads driver cb%lu [%s]", stage,
                                 stages[stage].hash ? stages[stage].hash : "?", cb, resources);
                        note_skip_detail(note);
                    }
                    GENERIC_SKIP("general draw: shader reads a driver constant buffer (cb0-cb2)");
                } else {
                    if (cb - 3 >= GENERIC_UNIFORM_SLOTS || draw.uniform_count >= POKOPIA_GENERIC_UNIFORMS)
                        GENERIC_SKIP("general draw: uniform slot out of range");
                    const PokopiaLiveDrawSpan span = draw_span(config, device, record->uniforms[stage][cb - 3]);
                    if (!span.data)
                        GENERIC_SKIP("general draw: uniform buffer not bound");
                    draw.uniforms[draw.uniform_count++] = (PokopiaGenericUniform){binding, span};
                }
            } else if (length > 3 && !strncmp(token, "tex", 3)) {
                if (!generic_texture_token(token, length, &slot, &binding, &nvn_target, &kind)) {
                    if (execute) {
                        char note[160];
                        snprintf(note, sizeof note, "general draw: texture token %.*s", (int)(length > 100 ? 100 : length),
                                 token);
                        note_skip_detail(note);
                    }
                    GENERIC_SKIP("general draw: texture kind not supported yet (shadow, buffer or bindless)");
                }
                if (draw.texture_count >= POKOPIA_GENERIC_TEXTURES ||
                    (stage == NVN_SHADER_VERTEX && slot >= GENERIC_VERTEX_TEXTURES))
                    GENERIC_SKIP("general draw: too many textures");
                const uint64_t handle = stage == NVN_SHADER_VERTEX ? record->vertex_textures[slot] :
                                                                      record->fragment_textures[slot];
                PokopiaGenericTexture *texture = &draw.textures[draw.texture_count];
                texture->binding = binding;
                if (kind == GENERIC_TOKEN_BUFFER) {
                    if (!generic_resolve_texel(config, device, record->texture_pool, handle, texture,
                                               &texel_sources[draw.texture_count], execute))
                        GENERIC_SKIP("general draw: texture buffer did not resolve");
                } else if (!generic_resolve_texture(config->front, device, record->texture_pool, record->sampler_pool,
                                                    handle, nvn_target, texture,
                                                    &texture_infos[draw.texture_count], execute)) {
                    GENERIC_SKIP("general draw: texture handle did not resolve to a matching texture");
                }
                texture->shadow = kind == GENERIC_TOKEN_SHADOW;
                ++draw.texture_count;
            } else if (length) {
                GENERIC_SKIP("general draw: storage buffers or images not supported yet");
            }
            token += length;
            if (*token)
                ++token;
        }
    }

    /* Vertex input: one binding per stream the enabled attributes read, in first-use order. */
    for (unsigned i = 0; i < record->attribute_count; ++i) {
        const NvnFrontVertexAttribInfo *attribute = &record->attributes[i];
        if (!attribute->enabled)
            continue;
        const VkFormat format = nvn_guest_texture_vk_format(attribute->format);
        if (format == VK_FORMAT_UNDEFINED)
            GENERIC_SKIP("general draw: vertex attribute format not mapped");
        uint32_t index = 0;
        while (index < draw.binding_count && draw.bindings[index].binding != attribute->stream)
            ++index;
        if (index == draw.binding_count) {
            const PokopiaLiveDrawSpan span = draw_span(config, device, record->streams[attribute->stream]);
            if (!span.data || span.bytes > 16u * 1024 * 1024)
                GENERIC_SKIP("general draw: vertex stream not bound or too large");
            draw.bindings[draw.binding_count] = (VkVertexInputBindingDescription){
                attribute->stream, record->stream_info[attribute->stream].stride & 0xfff,
                record->stream_info[attribute->stream].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE :
                                                                 VK_VERTEX_INPUT_RATE_VERTEX};
            draw.streams[draw.binding_count++] = span;
        }
        draw.attributes[draw.attribute_count++] = (VkVertexInputAttributeDescription){
            i, attribute->stream, format, attribute->offset};
    }
    if (record->index_size) {
        draw.indices = draw_span(config, device, record->index);
        if (!draw.indices.data)
            GENERIC_SKIP("general draw: index buffer not bound");
    }

    static const VkPrimitiveTopology topologies[] = {
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
    };
    const PokopiaLiveDraw *state = &record->state;
    draw.topology = topologies[record->nvn_topology];
    draw.index_type = record->index_size == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    draw.viewport = state->viewport;
    draw.scissor = state->scissor;
    draw.cull_mode = (VkCullModeFlags)(record->cull & 3); /* NVN Face NONE/FRONT/BACK/FRONT_AND_BACK */
    draw.front_face = state->front_face;
    draw.color_write_mask = state->color_write_mask;
    draw.blend_enable = state->blend_enable ||
        (record->target_count > 1 && (record->blend_mask & ((1u << record->target_count) - 1u)));
    if (draw.blend_enable &&
        (state->blend.advanced_mode || !generic_blend_factor(state->blend.src_rgb, &draw.src_rgb) ||
         !generic_blend_factor(state->blend.dst_rgb, &draw.dst_rgb) ||
         !generic_blend_factor(state->blend.src_alpha, &draw.src_alpha) ||
         !generic_blend_factor(state->blend.dst_alpha, &draw.dst_alpha) ||
         !generic_blend_op(state->blend.rgb_equation, &draw.rgb_op) ||
         !generic_blend_op(state->blend.alpha_equation, &draw.alpha_op)))
        GENERIC_SKIP("general draw: blend mode not mapped");
    draw.depth_test = state->depth_test;
    draw.depth_write = state->depth_write;
    /* NVN DepthFunc: NEVER 1 ... ALWAYS 8; Vulkan counts from 0. */
    draw.depth_compare = record->depth_func >= 1 && record->depth_func <= 8 ?
        (VkCompareOp)(record->depth_func - 1) : VK_COMPARE_OP_ALWAYS;
    draw.stencil_test = state->stencil_test;
    draw.stencil[0] = state->stencil[0];
    draw.stencil[1] = state->stencil[1];
    draw.depth_bias = record->depth_bias && record->depth_target;
    draw.depth_bias_factor = record->depth_bias_values[0];
    draw.depth_bias_units = record->depth_bias_values[1];
    draw.vertex_count = state->vertex_count;
    draw.first_vertex = state->first_vertex;
    draw.index_count = state->index_count;
    draw.base_vertex = state->base_vertex;
    draw.instance_count = state->instance_count ? state->instance_count : 1;
    draw.first_instance = state->first_instance;
    if (!execute)
        return NVN_FRONT_HANDLED;

    /* R74: per-vertex streams are usually slices of one large vertex pool;
     * copying the whole bound range cost ~1.8 MB per draw (14.6 GB per 5 s in
     * the intro). Copy only the vertices this draw reads, found from its
     * indices (primitive-restart values excluded), and shift the base vertex. */
    bool in_place = true;
    for (uint32_t i = 0; i < draw.binding_count && in_place; ++i)
        in_place = draw.bindings[i].inputRate != VK_VERTEX_INPUT_RATE_VERTEX ||
                   pokopia_live_generic_direct(draw.streams[i].data, draw.streams[i].bytes);
    if (!in_place) {
        int64_t lo = INT64_MAX, hi = -1;
        if (draw.index_count && draw.indices.data) {
            const bool wide = draw.index_type == VK_INDEX_TYPE_UINT32;
            const size_t count = draw.indices.bytes / (wide ? 4 : 2);
            uint32_t min = UINT32_MAX, max = 0;
            if (wide) {
                const uint32_t *index = draw.indices.data;
                for (size_t i = 0; i < count; ++i)
                    if (index[i] != UINT32_MAX) {
                        min = index[i] < min ? index[i] : min;
                        max = index[i] > max ? index[i] : max;
                    }
            } else {
                const uint16_t *index = draw.indices.data;
                for (size_t i = 0; i < count; ++i)
                    if (index[i] != UINT16_MAX) {
                        min = index[i] < min ? index[i] : min;
                        max = index[i] > max ? index[i] : max;
                    }
            }
            if (min <= max) {
                lo = (int64_t)min + draw.base_vertex;
                hi = (int64_t)max + draw.base_vertex;
            }
        } else if (!draw.index_count && draw.vertex_count) {
            lo = draw.first_vertex;
            hi = (int64_t)draw.first_vertex + draw.vertex_count - 1;
        }
        bool sliced = false, sliceable = lo >= 0 && hi >= lo;
        for (uint32_t i = 0; i < draw.binding_count && sliceable; ++i)
            if (draw.bindings[i].inputRate == VK_VERTEX_INPUT_RATE_VERTEX && draw.bindings[i].stride &&
                (uint64_t)lo * draw.bindings[i].stride >= draw.streams[i].bytes)
                sliceable = false; /* all per-vertex streams shift together, or none */
        if (sliceable)
            for (uint32_t i = 0; i < draw.binding_count; ++i) {
                const uint32_t stride = draw.bindings[i].stride;
                if (draw.bindings[i].inputRate != VK_VERTEX_INPUT_RATE_VERTEX || !stride ||
                    (uint64_t)lo * stride >= draw.streams[i].bytes)
                    continue;
                const size_t start = (size_t)lo * stride;
                size_t bytes = (size_t)(hi - lo + 1) * stride;
                if (bytes > draw.streams[i].bytes - start)
                    bytes = draw.streams[i].bytes - start;
                draw.streams[i] = (PokopiaLiveDrawSpan){(const uint8_t *)draw.streams[i].data + start, bytes};
                sliced = true;
            }
        if (sliced) {
            if (draw.index_count)
                draw.base_vertex -= (int32_t)lo;
            else
                draw.first_vertex -= (uint32_t)lo;
        }
    }

    for (uint32_t i = 0; i < draw.texture_count; ++i) {
        PokopiaVkTexel *texel = draw.textures[i].texel;
        if (!texel || texel->refresh_epoch == texel_epoch)
            continue;
        const PokopiaLiveDrawSpan bytes = draw_span(config, device, (BufferRange){texel_sources[i], texel->bytes});
        if (!bytes.data || pokopia_vk_texel_refresh(texel, bytes.data, texel->bytes) != VK_SUCCESS)
            GENERIC_SKIP("general draw: texture buffer refresh failed");
        texel->refresh_epoch = texel_epoch;
    }

    PokopiaVkProgramModule modules[2] = {0};
    PokopiaLiveDrawShader *shaders[] = {&draw.vertex_shader, &draw.fragment_shader};
    bool ready = true;
    for (unsigned stage = 0; stage < stage_count && ready; ++stage) {
        ready = pokopia_vk_program_acquire_module(programs[stage], stage, &modules[stage]);
        if (ready) {
            shaders[stage]->module = modules[stage].module;
            shaders[stage]->entry = modules[stage].entry;
            shaders[stage]->hash = stages[stage].hash;
        }
    }
    const bool drawn = ready && config->draw_generic(config->opaque, &draw);
    if (tracing) {
        fprintf(nvn_replay_trace_log, "draw %s/%s %s n=%u inst=%u vp=%.0f,%.0f,%.0fx%.0f dt=%d dw=%d cmp=%d blend=%d mask=%#x bias=%d",
                stages[0].hash ? stages[0].hash : "-", stage_count > 1 && stages[1].hash ? stages[1].hash : "-",
                drawn ? "ok" : "FAIL", draw.index_count ? draw.index_count : draw.vertex_count, draw.instance_count,
                (double)draw.viewport.x, (double)draw.viewport.y, (double)draw.viewport.width,
                (double)draw.viewport.height, draw.depth_test, draw.depth_write, (int)draw.depth_compare,
                draw.blend_enable, (unsigned)draw.color_write_mask, draw.depth_bias);
        trace_texture("rt", draw.target);
        if (draw.target)
            fprintf(nvn_replay_trace_log, "@L%uA%u", draw.color_levels[0], draw.color_layers[0]);
        for (uint32_t i = 1; i < draw.color_count; ++i)
            trace_texture("rt+", draw.more_targets[i - 1]);
        trace_texture("ds", draw.depth_target);
        for (uint32_t i = 0; i < draw.texture_count; ++i) {
            char label[16];
            snprintf(label, sizeof label, "t%u%s%s%s", draw.textures[i].binding, draw.textures[i].shadow ? "S" : "",
                     draw.textures[i].texel ? "B" : "", draw.textures[i].as_array ? "A" : "");
            trace_texture(label, draw.textures[i].texture);
        }
        fputc('\n', nvn_replay_trace_log);
    }
    bool dump_width_ok = !nvn_replay_dump_width;
    const void *dump_texture = NULL;
    for (uint32_t i = 0; i < draw.texture_count && !dump_width_ok; ++i) {
        dump_width_ok = draw.textures[i].texture && draw.textures[i].texture->extent.width == nvn_replay_dump_width &&
                        (!nvn_replay_dump_height || draw.textures[i].texture->extent.height == nvn_replay_dump_height);
        if (dump_width_ok)
            dump_texture = draw.textures[i].texture;
    }
    const bool dump_shader = (stages[0].hash && !strcmp(stages[0].hash, nvn_replay_dump_hash)) ||
                             (stage_count > 1 && stages[1].hash && !strcmp(stages[1].hash, nvn_replay_dump_hash));
    if (drawn && nvn_replay_dump_pending && dump_width_ok && dump_shader && nvn_replay_dump_skip) {
        bool seen = false;
        for (unsigned i = 0; i < nvn_replay_dump_seen_count && i < 16; ++i)
            seen |= dump_seen[i] == dump_texture;
        if (!seen && nvn_replay_dump_seen_count < 16)
            dump_seen[nvn_replay_dump_seen_count++] = dump_texture;
        if (seen || nvn_replay_dump_seen_count <= nvn_replay_dump_skip)
            dump_width_ok = false;
    }
    if (drawn && nvn_replay_dump_pending && dump_width_ok && dump_shader) {
        nvn_replay_dump_pending = 0;
        PokopiaVkAdapter *adapter = config->opaque;
        char path[256];
        for (uint32_t i = 0; i < (draw.target ? (draw.color_count ? draw.color_count : 1) : 0); ++i) {
            PokopiaVkTexture *color = i ? draw.more_targets[i - 1] : draw.target;
            if (!color)
                continue;
            snprintf(path, sizeof path, "/data/pokopia/debug/dump-%s-rt%u.bin", nvn_replay_dump_hash, i);
            const VkResult dumped = pokopia_vk_texture_dump(adapter, color, draw.color_levels[i], draw.color_layers[i], path);
            TRACE("dump rt%u result=%d\n", i, (int)dumped);
        }
        if (draw.depth_target) {
            snprintf(path, sizeof path, "/data/pokopia/debug/dump-%s-ds.bin", nvn_replay_dump_hash);
            pokopia_vk_texture_dump(adapter, draw.depth_target, 0, 0, path);
        }
        for (uint32_t i = 0; i < draw.texture_count; ++i) {
            if (draw.textures[i].texel)
                continue;
            snprintf(path, sizeof path, "/data/pokopia/debug/dump-%s-t%u.bin", nvn_replay_dump_hash,
                     draw.textures[i].binding);
            pokopia_vk_texture_dump(adapter, draw.textures[i].texture, 0, 0, path);
        }
        if (nvn_replay_trace_log) {
            fprintf(nvn_replay_trace_log, "#### dumped after %s\n", nvn_replay_dump_hash);
            fflush(nvn_replay_trace_log);
        }
    }
    for (unsigned stage = 0; stage < stage_count; ++stage)
        pokopia_vk_program_release_module(programs[stage], &modules[stage]);
    if (!ready)
        GENERIC_SKIP("general draw: shader module unavailable");
    return drawn ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
#undef GENERIC_SKIP
}

static NvnFrontReplyKind run_draw(NvnReplay *replay, uint64_t device, const Draw *record, bool execute)
{
    /* R78: the general path's check pass always succeeds (it skips what it
     * cannot draw), so the game's thread no longer resolves every draw twice. */
    if (record->generic)
        return execute ? run_draw_generic(replay, device, record, execute) : NVN_FRONT_HANDLED;
    const NvnReplayConfig *config = &replay->config;
    NvnFrontTextureInfo target;
    if (record->target &&
        (!get_color_target(replay, device, record->target, &target) || target.native != record->state.target))
        return NVN_FRONT_INVALID_GUEST;
    if (record->depth_target &&
        (!get_depth_target(replay, device, record->depth_target, &target) || target.native != record->state.depth_target))
        return NVN_FRONT_INVALID_GUEST;
    PokopiaLiveDraw draw = record->state;
    NvnFrontTextureInfo sample_info[POKOPIA_LIVE_DRAW_SAMPLE_COUNT];
    PokopiaLiveDrawShader *shaders[] = {&draw.vertex_shader, &draw.fragment_shader};
    PokopiaLiveDrawSpan *uniforms[] = {&draw.vertex_uniform, &draw.fragment_uniform};
    const unsigned stage_count = record->target ? 2 : 1;
    const PokopiaVkProgram *programs[2] = {0};
    PokopiaVkProgramStage stages[2] = {0};
    /* R50: anything this bridge cannot translate skips the draw. */
    /* R62: what the pinned path cannot draw goes to the general path. */
#define SKIP_DRAW() do { return run_draw_generic(replay, device, record, execute); } while (0)
    for (unsigned stage = 0; stage < stage_count; ++stage) {
        NvnFrontProgramInfo program;
        if (!require_program_stage(config->front, device, record->program[stage], stage, &program, &stages[stage]))
            SKIP_DRAW();
        programs[stage] = program.native;
    }
    for (unsigned stage = 0; stage < stage_count; ++stage) {
        const PokopiaVkProgramStage info = stages[stage];
        if (!supported_graphics_shader(stage, info.hash)) {
            note_skipped_shader(stage, info.hash);
            SKIP_DRAW();
        }
        shaders[stage]->hash = info.hash;
        DrawAbi abi;
        if (!draw_abi(info.resources, stage, &abi))
            SKIP_DRAW();
        if (abi.uniform0) {
            *uniforms[stage] = draw_span(config, device, record->uniform[stage]);
            if (!uniforms[stage]->data)
                return NVN_FRONT_INVALID_GUEST;
        }
        for (unsigned sample = 0; sample < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++sample) {
            const int slot = abi.fragment_slots[sample];
            if (slot >= 0 &&
                !resolve_fragment_sample(config->front, device, record, (unsigned)slot,
                                         &draw.fragment_samples[sample], &sample_info[sample]))
                SKIP_DRAW();
        }
    }
#undef SKIP_DRAW
    draw.vertices = draw_span(config, device, record->vertex);
    if (!draw.vertices.data)
        return NVN_FRONT_INVALID_GUEST;
    if (draw.index_count) {
        draw.indices = draw_span(config, device, record->index);
        if (!draw.indices.data)
            return NVN_FRONT_INVALID_GUEST;
    }
    if (!execute)
        return NVN_FRONT_HANDLED;

    PokopiaVkProgramModule modules[2] = {0};
    NvnFrontReplyKind result = NVN_FRONT_BOUNDARY;
    for (unsigned stage = 0; stage < stage_count; ++stage) {
        if (!pokopia_vk_program_acquire_module(programs[stage], stage, &modules[stage]))
            goto done;
        shaders[stage]->module = modules[stage].module;
        shaders[stage]->entry = modules[stage].entry;
    }
    nvn_replay_draw_unsupported = false;
    if (config->draw(config->opaque, &draw))
        result = NVN_FRONT_HANDLED;

done:
    for (unsigned stage = 0; stage < stage_count; ++stage)
        pokopia_vk_program_release_module(programs[stage], &modules[stage]);
    if (result == NVN_FRONT_HANDLED && nvn_replay_draw_unsupported) {
        /* R62: the pinned draw reported an unsupported shape and changed nothing. */
        nvn_replay_draw_unsupported = false;
        return run_draw_generic(replay, device, record, execute);
    }
    return result;
}

static void *get_sync(const NvnReplayConfig *config, uint64_t device, uint64_t address)
{
    void *native;
    if (!nvn_front_get_sync(config->front, address, &native))
        return NULL;
    uint64_t owner;
    const void *field = config->read_guest(config->opaque, address, sizeof owner);
    if (!field)
        return NULL;
    memcpy(&owner, field, sizeof owner);
    return owner == device ? native : NULL;
}

static NvnFrontReplyKind record_sync(NvnReplay *replay, const uint64_t x[32], bool wait)
{
    Recording *r = *find_recording(replay, x[0]);
    if (!r)
        return NVN_FRONT_INVALID_GUEST;
    void *sync = get_sync(&replay->config, r->device, x[1]);
    if (!sync)
        return NVN_FRONT_INVALID_GUEST;
    if (!wait && ((uint32_t)x[2] || (uint32_t)x[3] > 1))
        return NVN_FRONT_BOUNDARY;
    Operation operation = {.kind = OP_SYNC, .data.sync = {x[1], sync, wait}};
    return append_metered(replay, r, &operation, 0, 16);
}

static bool resolve_compute_texel(const NvnReplayConfig *config, uint64_t device, uint64_t pool,
                                  uint64_t handle, size_t expected, PokopiaVkTexel **texel, const void **source)
{
    enum { TEXTURE_BUFFER = 0xa };
    NvnFrontDescriptorPoolInfo descriptor_pool;
    NvnFrontTextureDescriptorInfo descriptor;
    if (!nvn_front_get_descriptor_pool(config->front, pool, false, &descriptor_pool) ||
        descriptor_pool.device != device ||
        !nvn_front_get_texture_descriptor(config->front, pool, (uint32_t)handle & 0xfffff, &descriptor) ||
        descriptor.image || !descriptor.texture.native)
        return false;

    const NvnFrontTextureCreateInfo *info = &descriptor.texture.create_info;
    if (info->config.device != device || info->config.target != TEXTURE_BUFFER ||
        info->storage_size != expected)
        return false;
    if (!resolve_sampled_view(&descriptor, NULL, NULL))
        return false;
    PokopiaVkTexture *texture = descriptor.texture.native;
    if (!texture->texel.device || texture->texel.format != VK_FORMAT_R32G32B32A32_SFLOAT ||
        texture->texel.bytes != expected)
        return false;
    const PokopiaLiveDrawSpan bytes = draw_span(config, device, (BufferRange){info->storage_address, expected});
    if (!bytes.data)
        return false;
    *texel = &texture->texel;
    *source = bytes.data;
    return true;
}

/* R73: a compute shader's resources are found through c[0], which the NVN
 * driver packs per shader: uniform-buffer descriptors (16 bytes) from 0x10 up
 * to the highest uniform slot used, then storage buffers (16 bytes), texture
 * handles (8 bytes) and image handles (8 bytes), each region sized by the
 * highest slot the shader uses. This reproduces the two pinned layouts and
 * places every token of the shaders the intro dispatches. c[0] itself is
 * rebuilt from the bound state for shaders that read it as a uniform. */
static NvnFrontReplyKind run_compute_generic(NvnReplay *replay, uint64_t device, const ComputeDispatch *record,
                                             const NvnFrontProgramInfo *program, const PokopiaVkProgramStage *stage,
                                             bool execute)
{
    const NvnReplayConfig *config = &replay->config;
    const char *hash = stage->hash ? stage->hash : "-";
#define COMPUTE_SKIP(why) do { if (execute) { ++skip_counts[SKIP_RUN]; note_skip(why); \
                                              TRACE("dispatch %s SKIP %s\n", hash, why); } \
                               return NVN_FRONT_HANDLED; } while (0)
    if (!config->compute_generic || !stage->resources)
        COMPUTE_SKIP("general compute: unavailable");
    enum { MAX_TOKENS = 24 };
    struct { char kind; unsigned offset, binding, cb; uint32_t target; } tokens[MAX_TOKENS];
    unsigned token_count = 0;
    int max_ubo_slot = -1;
    unsigned max_ssbo = 0, max_tex = 0, max_byte = 0;
    bool any_ssbo = false, any_tex = false;
    for (const char *token = stage->resources; *token;) {
        const size_t length = strcspn(token, " ");
        if (length) {
            if (token_count == MAX_TOKENS)
                COMPUTE_SKIP("general compute: too many resources");
            char text[96];
            snprintf(text, sizeof text, "%.*s", (int)(length < 95 ? length : 95), token);
            unsigned a, b;
            char type[48];
            if (sscanf(text, "cb%u=s0b%u", &a, &b) == 2) {
                tokens[token_count++] = (typeof(tokens[0])){'u', 0, b, a, 0};
                if (a >= 3 && (int)(a - 3) > max_ubo_slot)
                    max_ubo_slot = (int)(a - 3);
            } else if (sscanf(text, "sb[c0+0x%x]=s1b%u", &a, &b) == 2) {
                tokens[token_count++] = (typeof(tokens[0])){'s', a * 4, b, 0, 0};
                any_ssbo = true;
                if (a * 4 > max_ssbo)
                    max_ssbo = a * 4;
            } else if (sscanf(text, "%47[^[][c0:%u]=s%*1[23]b%u", type, &a, &b) == 3) {
                static const struct { const char *name; char kind; uint32_t target; } kinds[] = {
                    {"texTexture2D", 't', 1}, {"texTexture3D", 't', 2}, {"texTextureCube", 't', 8},
                    {"texTexture2D|Array", 't', 4}, {"texTextureBuffer", 'b', 0xa},
                    {"imgTexture2D", 'i', 1}, {"imgTexture3D", 'i', 2}, {"imgTexture2D|Array", 'i', 4},
                };
                size_t k = 0;
                while (k < sizeof kinds / sizeof kinds[0] && strcmp(kinds[k].name, type))
                    ++k;
                if (k == sizeof kinds / sizeof kinds[0]) {
                    char note[128];
                    snprintf(note, sizeof note, "general compute: resource %s not supported", type);
                    COMPUTE_SKIP(note);
                }
                tokens[token_count++] = (typeof(tokens[0])){kinds[k].kind, a * 4, b, 0, kinds[k].target};
                if (kinds[k].kind != 'i') {
                    any_tex = true;
                    if (a * 4 > max_tex)
                        max_tex = a * 4;
                }
            } else {
                char note[128];
                snprintf(note, sizeof note, "general compute: token %s not understood", text);
                COMPUTE_SKIP(note);
            }
            if (tokens[token_count - 1].offset + 16 > max_byte)
                max_byte = tokens[token_count - 1].offset + 16;
        }
        token += length;
        if (*token)
            ++token;
    }
    const unsigned ubo_end = 0x10 + 16u * (unsigned)(max_ubo_slot + 1);
    const unsigned ssbo_end = any_ssbo ? max_ssbo + 16 : ubo_end;
    const unsigned tex_end = any_tex ? max_tex + 8 : ssbo_end;

    PokopiaGenericCompute c = {.groups = {record->groups[0], record->groups[1], record->groups[2]}};
    uint8_t c0[0x400] = {0};
    if (max_byte > sizeof c0)
        COMPUTE_SKIP("general compute: c[0] larger than expected");
    NvnFrontTextureInfo infos[POKOPIA_COMPUTE_TEXTURES];
    uint64_t texel_sources[POKOPIA_COMPUTE_TEXTURES] = {0};
    int c0_uniform = -1;
    for (unsigned i = 0; i < token_count; ++i) {
        const unsigned offset = tokens[i].offset;
        switch (tokens[i].kind) {
        case 'u': {
            if (c.uniform_count == POKOPIA_COMPUTE_UNIFORMS)
                COMPUTE_SKIP("general compute: too many uniform buffers");
            PokopiaGenericUniform *u = &c.uniforms[c.uniform_count];
            u->binding = tokens[i].binding;
            if (tokens[i].cb == 0) {
                c0_uniform = (int)c.uniform_count;
            } else if (tokens[i].cb == 1) {
                if (!stage->constants)
                    COMPUTE_SKIP("general compute: cb1 without shader constants");
                u->data = (PokopiaLiveDrawSpan){stage->constants, stage->constants_bytes};
            } else if (tokens[i].cb >= 3 && tokens[i].cb - 3 < 8) {
                u->data = draw_span(config, device, record->uniforms[tokens[i].cb - 3]);
                if (!u->data.data)
                    COMPUTE_SKIP("general compute: uniform buffer not bound");
            } else {
                COMPUTE_SKIP("general compute: reads driver cb2");
            }
            ++c.uniform_count;
            break;
        }
        case 's': {
            const unsigned slot = (offset - ubo_end) / 16;
            if (offset < ubo_end || (offset - ubo_end) % 16 || slot >= COMPUTE_STORAGE_SLOTS ||
                c.storage_count == POKOPIA_COMPUTE_STORAGE)
                COMPUTE_SKIP("general compute: storage buffer slot out of range");
            const BufferRange range = record->ssbo[slot];
            const PokopiaLiveDrawSpan span = draw_span(config, device, range);
            void *data = span.data ? config->write_guest(config->opaque, range.address, range.size) : NULL;
            if (!data)
                COMPUTE_SKIP("general compute: storage buffer not bound");
            c.storage[c.storage_count++] = (PokopiaGenericStorage){tokens[i].binding, data, range.size};
            const uint32_t size = (uint32_t)range.size;
            memcpy(c0 + offset, &range.address, 8);
            memcpy(c0 + offset + 8, &size, 4);
            break;
        }
        case 't':
        case 'b': {
            const unsigned slot = (offset - ssbo_end) / 8;
            if (offset < ssbo_end || (offset - ssbo_end) % 8 || slot >= 8 ||
                c.texture_count == POKOPIA_COMPUTE_TEXTURES)
                COMPUTE_SKIP("general compute: texture slot out of range");
            const uint64_t handle = record->textures[slot];
            memcpy(c0 + offset, &handle, 8);
            PokopiaGenericTexture *t = &c.textures[c.texture_count];
            t->binding = tokens[i].binding;
            if (tokens[i].kind == 'b') {
                if (!generic_resolve_texel(config, device, record->texture_pool, handle, t,
                                           &texel_sources[c.texture_count], execute))
                    COMPUTE_SKIP("general compute: texture buffer did not resolve");
            } else if (!generic_resolve_texture(config->front, device, record->texture_pool, record->sampler_pool,
                                                handle, tokens[i].target, t, &infos[c.texture_count], execute)) {
                COMPUTE_SKIP("general compute: texture did not resolve");
            }
            ++c.texture_count;
            break;
        }
        case 'i': {
            const unsigned slot = (offset - tex_end) / 8;
            if (offset < tex_end || (offset - tex_end) % 8 || slot >= 8 || c.image_count == POKOPIA_COMPUTE_IMAGES)
                COMPUTE_SKIP("general compute: image slot out of range");
            const uint64_t handle = record->images[slot];
            memcpy(c0 + offset, &handle, 8);
            NvnFrontTextureDescriptorInfo desc;
            if (!handle || !nvn_front_get_texture_descriptor(config->front, record->texture_pool,
                                                             (uint32_t)handle & 0xfffff, &desc) ||
                !desc.texture.native)
                COMPUTE_SKIP("general compute: image did not resolve");
            c.images[c.image_count++] = (PokopiaGenericImage){tokens[i].binding, desc.texture.native};
            break;
        }
        }
    }
    if (c0_uniform >= 0)
        c.uniforms[c0_uniform].data = (PokopiaLiveDrawSpan){c0, max_byte > 0x100 ? max_byte : 0x100};
    if (!execute)
        return NVN_FRONT_HANDLED;

    for (uint32_t i = 0; i < c.texture_count; ++i) {
        PokopiaVkTexel *texel = c.textures[i].texel;
        if (!texel || texel->refresh_epoch == texel_epoch)
            continue;
        const PokopiaLiveDrawSpan bytes = draw_span(config, device, (BufferRange){texel_sources[i], texel->bytes});
        if (!bytes.data || pokopia_vk_texel_refresh(texel, bytes.data, texel->bytes) != VK_SUCCESS)
            COMPUTE_SKIP("general compute: texture buffer refresh failed");
        texel->refresh_epoch = texel_epoch;
    }
    PokopiaVkProgramModule module = {0};
    if (!pokopia_vk_program_acquire_module(program->native, NVN_SHADER_COMPUTE, &module))
        COMPUTE_SKIP("general compute: shader module unavailable");
    c.shader = (PokopiaLiveDrawShader){module.module, module.entry, stage->hash};
    const bool ok = config->compute_generic(config->opaque, &c);
    pokopia_vk_program_release_module(program->native, &module);
    /* Storage written back to guest memory may back texel buffers. */
    if (c.storage_count)
        ++texel_epoch;
    TRACE("dispatch %s %ux%ux%u general %s ubo=%u ssbo=%u tex=%u img=%u\n", hash, c.groups[0], c.groups[1],
          c.groups[2], ok ? "ok" : "FAIL", c.uniform_count, c.storage_count, c.texture_count, c.image_count);
    if (!ok) {
        ++skip_counts[SKIP_RUN];
        note_skip("general compute: dispatch failed");
    }
    return NVN_FRONT_HANDLED;
#undef COMPUTE_SKIP
}

static NvnFrontReplyKind run_compute(NvnReplay *replay, uint64_t device, const ComputeDispatch *record, bool execute)
{
    const NvnReplayConfig *config = &replay->config;
    NvnFrontProgramInfo program;
    PokopiaVkProgramStage stage;
    /* R52: skip dispatches this bridge cannot run, as with draws. */
    if (!config->compute || !require_program_stage(config->front, device, record->program, NVN_SHADER_COMPUTE,
                                                   &program, &stage)) {
        if (execute)
            ++skip_counts[SKIP_RUN];
        return NVN_FRONT_HANDLED;
    }
    if (record->generic)
        return run_compute_generic(replay, device, record, &program, &stage, execute);
    const ComputeAbi abi = compute_abi(&stage);
    if (execute)
        TRACE("dispatch %s %ux%ux%u %s\n", stage.hash ? stage.hash : "-", record->groups[0], record->groups[1],
              record->groups[2], abi == COMPUTE_UNSUPPORTED ? "SKIP" : "run");
    if (abi == COMPUTE_UNSUPPORTED) {
        note_skipped_shader(NVN_SHADER_COMPUTE, stage.hash);
        if (execute)
            ++skip_counts[SKIP_RUN];
        return NVN_FRONT_HANDLED;
    }

    const bool clustered = abi == COMPUTE_CLUSTERED_LIGHTS;
    const PokopiaLiveDrawSpan uniform = draw_span(config, device, record->uniform);
    if (!uniform.data || uniform.bytes != (clustered ? 512u : 256u))
        return NVN_FRONT_INVALID_GUEST;
    PokopiaLiveCompute dispatch = {
        .hash = stage.hash, .uniform = uniform.data, .uniform_bytes = uniform.bytes,
        .groups = {record->groups[0], record->groups[1], record->groups[2]},
    };
    const size_t expected_storage[] = {4, pokopia_cluster_output_bytes(record->groups)};
    if (clustered && (!expected_storage[1] || !pokopia_cluster_extent_matches(record->groups, uniform.data)))
        return NVN_FRONT_INVALID_GUEST;
    for (unsigned i = 0; i < (clustered ? 2u : 1u); ++i) {
        const BufferRange range = record->storage[i];
        const PokopiaLiveDrawSpan span = draw_span(config, device, range);
        if (!span.data || span.bytes != expected_storage[i])
            return NVN_FRONT_INVALID_GUEST;
        void *canonical = config->write_guest(config->opaque, range.address, range.size);
        if (canonical != span.data)
            return NVN_FRONT_INVALID_GUEST;
        dispatch.storage[i] = (PokopiaComputeStorage){canonical, range.size};
    }
    if (clustered) {
        const uintptr_t first = (uintptr_t)dispatch.storage[0].data;
        const uintptr_t second = (uintptr_t)dispatch.storage[1].data;
        if (first > UINTPTR_MAX - dispatch.storage[0].bytes ||
            second > UINTPTR_MAX - dispatch.storage[1].bytes ||
            (first < second + dispatch.storage[1].bytes && second < first + dispatch.storage[0].bytes))
            return NVN_FRONT_INVALID_GUEST;
    }
    PokopiaVkTexel *texels[2];
    const void *sources[2];
    const size_t texel_bytes[] = {1024 * 16, 64 * 16};
    if (clustered)
        for (unsigned i = 0; i < 2; ++i) {
            if (!resolve_compute_texel(config, device, record->texture_pool, record->texture_handles[i],
                                       texel_bytes[i], &texels[i], &sources[i]))
                return NVN_FRONT_BOUNDARY;
            dispatch.texels[i] = texels[i];
        }
    if (!execute)
        return NVN_FRONT_HANDLED;

    PokopiaVkProgramModule module = {0};
    if (!pokopia_vk_program_acquire_module(program.native, NVN_SHADER_COMPUTE, &module))
        return NVN_FRONT_BOUNDARY;
    dispatch.module = module.module;
    dispatch.entry = module.entry;
    NvnFrontReplyKind result = NVN_FRONT_BOUNDARY;
    if (clustered)
        for (unsigned i = 0; i < 2; ++i)
            if (pokopia_vk_texel_refresh(texels[i], sources[i], texel_bytes[i]) != VK_SUCCESS)
                goto done;
    if (config->compute(config->opaque, &dispatch))
        result = NVN_FRONT_HANDLED;
done:
    pokopia_vk_program_release_module(program.native, &module);
    return result;
}

static NvnFrontReplyKind run_operation(NvnReplay *replay, uint64_t device, const Operation *operation, bool execute)
{
    const NvnReplayConfig *config = &replay->config;
    if (operation->kind == OP_COPY) {
        Copy copy = operation->data.copy;
        if (execute)
            ++pokopia_vk_guest_write_epoch; /* R75: drops general-draw upload reuse */
        const void *source;
        void *destination;
        if (!resolve_copy(replay, device, copy, &source, &destination))
            return NVN_FRONT_INVALID_GUEST;
        return !execute || config->copy(config->opaque, source, destination, copy.size) ?
            NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
    }
    if (operation->kind == OP_COPY_TEXTURE && execute)
        ++pokopia_vk_guest_write_epoch;
    if (operation->kind == OP_COPY_TEXTURE)
        return run_copy_texture(replay, &operation->data.copy_texture, execute);
    if (operation->kind == OP_DRAW)
        return run_draw(replay, device, &operation->data.draw, execute);
    if (operation->kind == OP_DISPATCH)
        return run_compute(replay, device, &operation->data.dispatch, execute);
    if (operation->kind == OP_TIMESTAMP) {
        CounterReport *destination = resolve_counter_report(config, device, operation->data.timestamp_address);
        if (!destination)
            return NVN_FRONT_INVALID_GUEST;
        if (execute) {
            /* Earlier replay operations complete synchronously before this sample. */
            const CounterReport report = { .timestamp = config->timestamp_ns() };
            memcpy(destination, &report, sizeof report);
        }
        return NVN_FRONT_HANDLED;
    }
    if (operation->kind == OP_SYNC) {
        const Sync *sync = &operation->data.sync;
        if (get_sync(config, device, sync->address) != sync->native)
            return NVN_FRONT_INVALID_GUEST;
        if (!execute && !sync->wait && config->sync_queued && config->enqueue)
            config->sync_queued(config->opaque, sync->native);
        return !execute || config->sync(config->opaque, sync->native, sync->wait) ?
            NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
    }
    if (operation->kind == OP_DEPTH_CLEAR) {
        const DepthClear *clear = &operation->data.depth_clear;
        NvnFrontTextureInfo texture;
        if (!get_depth_target(replay, device, clear->address, &texture) || texture.native != clear->native)
            return NVN_FRONT_INVALID_GUEST;
        if (execute && tracing) {
            fprintf(nvn_replay_trace_log, "depth-clear aspects=%#x d=%g s=%u", (unsigned)clear->aspects,
                    (double)clear->value.depth, clear->value.stencil);
            trace_texture("ds", texture.native);
            fputc('\n', nvn_replay_trace_log);
        }
        return !execute || config->clear_depth_stencil(config->opaque, texture.native, clear->area,
                                                        clear->aspects, clear->value) ?
            NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
    }
    const Clear *clear = &operation->data.clear;
    NvnFrontTextureInfo texture;
    if (!get_color_target(replay, device, clear->address, &texture) || texture.native != clear->native)
        return NVN_FRONT_INVALID_GUEST;
    if (execute && tracing) {
        fprintf(nvn_replay_trace_log, "clear %g,%g,%g,%g mask=%#x", (double)clear->rgba[0], (double)clear->rgba[1],
                (double)clear->rgba[2], (double)clear->rgba[3], (unsigned)clear->mask);
        trace_texture("rt", texture.native);
        fputc('\n', nvn_replay_trace_log);
    }
    return !execute || config->clear(config->opaque, texture.native, clear->area, clear->rgba, clear->mask) ?
        NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}

static NvnFrontReplyKind end_recording(NvnReplay *replay, uint64_t x[32])
{
    Recording **link = find_recording(replay, x[0]);
    Recording *recording = *link;
    if (!recording)
        return NVN_FRONT_INVALID_GUEST;
    if (replay->next_handle >= UINT64_C(0x7f20000000))
        { nvn_replay_boundary_reason = "end recording: handle space exhausted"; return NVN_FRONT_BOUNDARY; }

    const size_t bytes = sizeof(RecordedList) + recording->used;
    trim_lists(replay);
    RecordedList *list = malloc(sizeof *list);
    if (!list) {
        evict_lists_for_heap(replay);
        list = malloc(sizeof *list);
    }
    if (!list)
        { nvn_replay_boundary_reason = "end recording: out of host heap for the command list"; return NVN_FRONT_BOUNDARY; }
    if (!charge_recording(replay, recording, 0, 8)) {
        free(list);
        nvn_replay_boundary_reason = replay->memory_request.function ? NULL :
            "end recording: control memory full and no memory callback";
        return NVN_FRONT_BOUNDARY;
    }

    uint8_t *operations = recording->stream;
    const size_t capacity = recording->capacity; /* R78: kept whole for reuse */
    recording->stream = NULL;
    recording->capacity = 0;
    *list = (RecordedList){ .next = replay->lists, .device = recording->device,
        .handle = replay->next_handle, .count = recording->count, .bytes = bytes,
        .last_use = ++replay->use_clock, .operations = operations, .capacity = capacity };
    replay->list_bytes += bytes;
    ++replay->list_count;
    nvn_replay_cached_list_bytes = replay->list_bytes;
    nvn_replay_cached_lists = replay->list_count;
    remove_recording(link);
    replay->lists = list;
    replay->next_handle += 8;
#if defined(__PROSPERO__)
    pokopia_trace_producer("rec-seal", 0, x[0], list->handle);
#endif
    x[0] = list->handle;
    return NVN_FRONT_HANDLED;
}

/* R67: replay time by operation kind, [kind][execute]; the loader reports and clears it. */
uint64_t nvn_replay_op_ns[8][2], nvn_replay_op_calls[8][2], nvn_replay_submit_ns;

static uint64_t replay_now_ns(void)
{
    if (pokopia_now_ns)
        return pokopia_now_ns();
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static NvnFrontReplyKind run_list(NvnReplay *replay, uint64_t device, const RecordedList *list, bool execute)
{
    size_t offset = 0;
    for (size_t i = 0; i < list->count; ++i) {
        const OperationKind kind = (OperationKind)list->operations[offset];
        const size_t payload = operation_payload_size(kind);
        if (!payload)
            return NVN_FRONT_BOUNDARY;
        const size_t data_offset = operation_data_offset(offset, kind);
        const uint8_t *data = list->operations + data_offset;
        offset = data_offset + payload;
        const unsigned flags = pokopia_debug_flags;
        const bool draw = kind == OP_DRAW;
        const bool counted = (unsigned)kind < 8;
        const unsigned weight = (flags & POKOPIA_DEBUG_SAMPLE_DRAWS) && draw ? 16u : 1u;
        const bool timed = counted && (weight == 1 || pokopia_timing_sample(nvn_replay_op_calls[kind][execute]));
        const uint64_t op_start = timed ? replay_now_ns() : 0;
        NvnFrontReplyKind result;
        if (draw && (flags & (1u << 21))) {
            result = run_draw(replay, device, (const Draw *)data, execute);
        } else {
            Operation operation;
            operation.kind = kind;
            memcpy(&operation.data, data, payload);
            result = run_operation(replay, device, &operation, execute);
        }
        if (timed)
            nvn_replay_op_ns[kind][execute] += (replay_now_ns() - op_start) * weight;
        if (counted)
            ++nvn_replay_op_calls[kind][execute];
        if (result != NVN_FRONT_HANDLED)
            return result;
        if (execute && replay->config.execute_yield)
            replay->config.execute_yield(replay->config.opaque);
    }
    return NVN_FRONT_HANDLED;
}

static const RecordedList *command_at(NvnReplay *replay, const uint8_t *handles,
                                      uint32_t index, uint64_t device)
{
    uint64_t handle;
    memcpy(&handle, handles + index * sizeof handle, sizeof handle);
    if (!handle)
        return NULL;
    const RecordedList *list = *find_list(replay, handle);
    return list && list->device == device ? list : NULL;
}

struct NvnReplayJob {
    uint64_t device;
    uint32_t count;
    const RecordedList **batch;
    uint64_t submit_start;
};

#if defined(__PROSPERO__)
void nvn_replay_trace_submission(const NvnReplayJob *job, uint64_t seq)
{
    pokopia_trace_producer("submit-start", job->submit_start, seq, 0);
    for (uint32_t i = 0; i < job->count; ++i)
        if (job->batch[i])
            pokopia_trace_producer("job-list", 0, seq, job->batch[i]->handle);
}
#endif

static NvnFrontReplyKind run_job(NvnReplay *replay, NvnReplayJob *job);

static NvnFrontReplyKind submit(NvnReplay *replay, uint64_t device, uint32_t count, uint64_t array)
{
    const NvnReplayConfig *config = &replay->config;
    if (!count)
        return NVN_FRONT_HANDLED;
    if (count > INT32_MAX)
        return NVN_FRONT_INVALID_GUEST;
    const uint8_t *handles = config->read_guest(config->opaque, array, (size_t)count * sizeof(uint64_t));
    if (!handles)
        return NVN_FRONT_INVALID_GUEST;
    const RecordedList **batch = calloc(count, sizeof *batch);
    if (!batch)
        return NVN_FRONT_BOUNDARY;
    const uint64_t submit_start = replay_now_ns();
    NvnFrontReplyKind result = NVN_FRONT_INVALID_GUEST;
    /* Validate every handle and pool before making any GPU writes. */
    for (uint32_t i = 0; i < count; ++i) {
        const RecordedList *list = command_at(replay, handles, i, device);
        if (!list) {
            uint64_t handle;
            memcpy(&handle, handles + i * sizeof handle, sizeof handle);
            if (handle >= UINT64_C(0x7f10000000) && handle < replay->next_handle && !(handle & 7)) {
                /* R63: evicted from the list cache; its draws are lost for this submit. */
                ++nvn_replay_evicted_submits;
                note_skip("submit: command list was evicted from the list cache");
                continue;
            }
            result = NVN_FRONT_INVALID_GUEST;
            goto done;
        }
        batch[i] = list;
        ((RecordedList *)list)->last_use = ++replay->use_clock;
        result = run_list(replay, device, list, false);
        if (result != NVN_FRONT_HANDLED)
            goto done;
    }
    for (uint32_t i = 0; i < count; ++i)
        if (batch[i])
            __atomic_add_fetch(&((RecordedList *)batch[i])->pinned, 1, __ATOMIC_RELAXED);
    if (config->enqueue) {
        NvnReplayJob *job = malloc(sizeof *job);
        if (job) {
            *job = (NvnReplayJob){device, count, batch, submit_start};
            if (config->enqueue(config->opaque, job)) {
                nvn_replay_submit_ns += replay_now_ns() - submit_start;
                return NVN_FRONT_HANDLED;
            }
            free(job);
        }
    }
    {
        NvnReplayJob job = {device, count, batch, submit_start};
        nvn_replay_submit_ns += replay_now_ns() - submit_start;
        return run_job(replay, &job);
    }
done:
    free(batch);
    nvn_replay_submit_ns += replay_now_ns() - submit_start;
    return result;
}

static NvnFrontReplyKind run_job(NvnReplay *replay, NvnReplayJob *job)
{
    const NvnReplayConfig *config = &replay->config;
    const uint64_t start = replay_now_ns();
    ++texel_epoch;
    ++pokopia_replay_job_epoch;
    tracing = nvn_replay_trace_log && nvn_replay_trace_submits > 0;
    if (tracing) {
        --nvn_replay_trace_submits;
        fprintf(nvn_replay_trace_log, "== submit lists=%u\n", job->count);
    }
    if (config->execute_begin)
        config->execute_begin(config->opaque);
    NvnFrontReplyKind result = NVN_FRONT_HANDLED;
    for (uint32_t i = 0; i < job->count && result == NVN_FRONT_HANDLED; ++i)
        if (job->batch[i])
            result = run_list(replay, job->device, job->batch[i], true);
    for (uint32_t i = 0; i < job->count; ++i)
        if (job->batch[i])
            __atomic_sub_fetch(&((RecordedList *)job->batch[i])->pinned, 1, __ATOMIC_RELEASE);
    if (config->execute_end)
        config->execute_end(config->opaque);
    if (tracing) {
        fflush(nvn_replay_trace_log);
        tracing = false;
    }
    free(job->batch);
    nvn_replay_submit_ns += replay_now_ns() - start;
    return result;
}

NvnFrontReplyKind nvn_replay_run_job(NvnReplay *replay, NvnReplayJob *job)
{
    const NvnFrontReplyKind result = run_job(replay, job);
    free(job);
    return result;
}

static HotReplayCommand lookup_hot_command(NvnReplay *replay, const char *key)
{
    static const char *const names[] = {
        [HOT_DRAW_BASE_VERTEX] = "nvnCommandBufferDrawElementsBaseVertex",
        [HOT_BIND_VERTEX] = "nvnCommandBufferBindVertexBuffer",
        [HOT_BIND_UNIFORM] = "nvnCommandBufferBindUniformBuffer",
        [HOT_BIND_TEXTURE] = "nvnCommandBufferBindTexture",
        [HOT_BIND_PROGRAM] = "nvnCommandBufferBindProgram",
        [HOT_BIND_ATTRIB_STATE] = "nvnCommandBufferBindVertexAttribState",
        [HOT_BIND_STREAM_STATE] = "nvnCommandBufferBindVertexStreamState",
        [HOT_DRAW_INSTANCED] = "nvnCommandBufferDrawElementsInstanced",
    };
    const uint64_t hash = ((uintptr_t)key >> 3) * UINT64_C(0x9e3779b97f4a7c15);
    ReplayDispatchEntry *entry = &replay->dispatch_cache[hash >> 56];
    if (entry->key == key)
        return entry->command;
    HotReplayCommand command = HOT_OTHER;
    for (size_t i = 1; i < sizeof names / sizeof names[0]; ++i) {
        if (!strcmp(key, names[i])) {
            command = (HotReplayCommand)i;
            break;
        }
    }
    *entry = (ReplayDispatchEntry){key, command};
    return command;
}

static inline NvnFrontReplyKind dispatch_hot_command(NvnReplay *replay, uint64_t x[32], HotReplayCommand command)
{
    switch (command) {
    case HOT_DRAW_BASE_VERTEX: return record_draw(replay, x, true, true, false);
    case HOT_BIND_VERTEX: return bind_buffers(replay, x, false, false);
    case HOT_BIND_UNIFORM: return bind_buffers(replay, x, true, false);
    case HOT_BIND_TEXTURE: return bind_texture(replay, x);
    case HOT_BIND_PROGRAM: return bind_program(replay, x);
    case HOT_BIND_ATTRIB_STATE: return bind_vertex_state(replay, x, false);
    case HOT_BIND_STREAM_STATE: return bind_vertex_state(replay, x, true);
    case HOT_DRAW_INSTANCED: return record_draw(replay, x, true, true, true);
    default: return NVN_FRONT_NOT_OURS;
    }
}

NvnFrontReplyKind nvn_replay_dispatch_keyed(NvnReplay *replay, const char *name, const char *static_key,
                                           uint64_t x[32], const uint32_t float_args[3])
{
    if (!replay || !name || !x)
        return NVN_FRONT_NOT_OURS;
    nvn_replay_boundary_reason = NULL;
    replay->memory_request = (NvnReplayMemoryRequest){0};
    if (static_key) {
        const HotReplayCommand command = lookup_hot_command(replay, static_key);
        if (command != HOT_OTHER)
            return dispatch_hot_command(replay, x, command);
    }
    /* R78: the most frequent recording calls first (draws sat ~45 compares down). */
    if (!strcmp(name, "nvnCommandBufferDrawElementsBaseVertex"))
        return dispatch_hot_command(replay, x, HOT_DRAW_BASE_VERTEX);
    if (!strcmp(name, "nvnCommandBufferBindVertexBuffer"))
        return dispatch_hot_command(replay, x, HOT_BIND_VERTEX);
    if (!strcmp(name, "nvnCommandBufferBindUniformBuffer"))
        return dispatch_hot_command(replay, x, HOT_BIND_UNIFORM);
    if (!strcmp(name, "nvnCommandBufferBindTexture"))
        return dispatch_hot_command(replay, x, HOT_BIND_TEXTURE);
    if (!strcmp(name, "nvnCommandBufferBindProgram"))
        return dispatch_hot_command(replay, x, HOT_BIND_PROGRAM);
    if (!strcmp(name, "nvnCommandBufferBeginRecording"))
        return begin_recording(replay, x[0]);
    if (!strcmp(name, "nvnCommandBufferCopyBufferToBuffer"))
        return record_copy(replay, x);
    if (!strcmp(name, "nvnCommandBufferCopyTextureToTexture"))
        return record_copy_texture(replay, x);
    if (!strcmp(name, "nvnCommandBufferReportCounter"))
        return record_timestamp(replay, x);
    if (!strcmp(name, "nvnCommandBufferSetRenderTargets"))
        return set_render_targets(replay, x);
    if (!strcmp(name, "nvnCommandBufferSetShaderScratchMemory"))
        return set_scratch(replay, x);
    if (!strcmp(name, "nvnCommandBufferSetTexturePool"))
        return set_descriptor_pool(replay, x, false);
    if (!strcmp(name, "nvnCommandBufferSetSamplerPool"))
        return set_descriptor_pool(replay, x, true);
    if (!strcmp(name, "nvnCommandBufferBindProgram"))
        return dispatch_hot_command(replay, x, HOT_BIND_PROGRAM);
    if (!strcmp(name, "nvnCommandBufferBindImage"))
        return bind_image(replay, x);
    if (!strcmp(name, "nvnCommandBufferBindTexture"))
        return dispatch_hot_command(replay, x, HOT_BIND_TEXTURE);
    if (!strcmp(name, "nvnCommandBufferBindVertexBuffer"))
        return dispatch_hot_command(replay, x, HOT_BIND_VERTEX);
    if (!strcmp(name, "nvnCommandBufferBindVertexBuffers"))
        return bind_buffers(replay, x, false, true);
    if (!strcmp(name, "nvnCommandBufferBindVertexAttribState"))
        return dispatch_hot_command(replay, x, HOT_BIND_ATTRIB_STATE);
    if (!strcmp(name, "nvnCommandBufferBindVertexStreamState"))
        return dispatch_hot_command(replay, x, HOT_BIND_STREAM_STATE);
    if (!strcmp(name, "nvnCommandBufferBindUniformBuffer"))
        return dispatch_hot_command(replay, x, HOT_BIND_UNIFORM);
    if (!strcmp(name, "nvnCommandBufferBindUniformBuffers"))
        return bind_buffers(replay, x, true, true);
    if (!strcmp(name, "nvnCommandBufferBindStorageBuffer"))
        return (uint32_t)x[1] == NVN_SHADER_COMPUTE ?
            bind_compute_buffer(replay, *find_recording(replay, x[0]), x, false) : NVN_FRONT_BOUNDARY;
    if (!strcmp(name, "nvnCommandBufferSetViewport"))
        return set_rectangle(replay, x, true, false);
    if (!strcmp(name, "nvnCommandBufferSetViewports"))
        return set_rectangle(replay, x, true, true);
    if (!strcmp(name, "nvnCommandBufferSetScissor"))
        return set_rectangle(replay, x, false, false);
    if (!strcmp(name, "nvnCommandBufferSetScissors"))
        return set_rectangle(replay, x, false, true);
    if (!strcmp(name, "nvnCommandBufferSetDepthRange"))
        return float_args ? set_depth_range(replay, x, float_args) : NVN_FRONT_INVALID_GUEST;
    if (!strcmp(name, "nvnCommandBufferSetDepthRanges"))
        return set_depth_range(replay, x, NULL);
    if (!strncmp(name, "nvnCommandBufferBind", sizeof "nvnCommandBufferBind" - 1)) {
        NvnFrontReplyKind raster = bind_raster_state(replay, name, x);
        if (raster != NVN_FRONT_NOT_OURS)
            return raster;
    }
    if (!strcmp(name, "nvnCommandBufferClearColor"))
        return record_clear(replay, x);
    if (!strcmp(name, "nvnCommandBufferClearDepthStencil"))
        return record_depth_clear(replay, x, float_args);
    if (!strcmp(name, "nvnCommandBufferBarrier"))
        return record_barrier(replay, x);
    if (!strcmp(name, "nvnCommandBufferSetTiledCacheAction"))
        return disable_tiled_cache(replay, x);
    if (!strcmp(name, "nvnCommandBufferSetStencilValueMask"))
        return set_stencil_value(replay, x, STENCIL_VALUE_MASK);
    if (!strcmp(name, "nvnCommandBufferSetStencilMask"))
        return set_stencil_value(replay, x, STENCIL_WRITE_MASK);
    if (!strcmp(name, "nvnCommandBufferSetStencilRef"))
        return set_stencil_value(replay, x, STENCIL_REFERENCE);
    if (!strcmp(name, "nvnCommandBufferSetDepthBounds") ||
        !strcmp(name, "nvnCommandBufferSetPrimitiveRestart") ||
        !strcmp(name, "nvnCommandBufferSetRasterizerDiscard"))
        return disable_raster_option(replay, x, 4);
    if (!strcmp(name, "nvnCommandBufferSetPolygonOffsetClamp"))
        return set_polygon_offset(replay, x[0], float_args);
    if (!strcmp(name, "nvnCommandBufferSetPatchSize"))
        return set_patch_size(replay, x);
    if (!strcmp(name, "nvnCommandBufferDrawArrays"))
        return record_draw(replay, x, false, false, false);
    if (!strcmp(name, "nvnCommandBufferDrawElements"))
        return record_draw(replay, x, true, false, false);
    if (!strcmp(name, "nvnCommandBufferDrawElementsBaseVertex"))
        return dispatch_hot_command(replay, x, HOT_DRAW_BASE_VERTEX);
    if (!strcmp(name, "nvnCommandBufferDrawElementsInstanced"))
        return dispatch_hot_command(replay, x, HOT_DRAW_INSTANCED);
    if (!strcmp(name, "nvnCommandBufferDrawArraysIndirect") ||
        !strcmp(name, "nvnCommandBufferDrawElementsIndirect")) {
        /* R75: the world issues indirect draws (first seen DrawArraysIndirect
         * with QUADS, args written by compute). Not replayed yet: skipped with
         * 24 / 32 command bytes charged (inferred), like an unmodelled draw. */
        Recording *r = *find_recording(replay, x[0]);
        const bool elements = strstr(name, "DrawElements") != NULL;
        note_skip(elements ? "DrawElementsIndirect skipped (not replayed yet)"
                           : "DrawArraysIndirect skipped (not replayed yet)");
        if (!r || !charge_recording(replay, r, elements ? 32 : 24, 0))
            return NVN_FRONT_BOUNDARY;
        ++skip_counts[SKIP_RECORD];
        return NVN_FRONT_HANDLED;
    }
    if (!strcmp(name, "nvnCommandBufferDispatchCompute"))
        return record_dispatch(replay, x);
    if (!strcmp(name, "nvnCommandBufferFenceSync"))
        return record_sync(replay, x, false);
    if (!strcmp(name, "nvnCommandBufferWaitSync"))
        return record_sync(replay, x, true);
    if (!strcmp(name, "nvnCommandBufferEndRecording"))
        return end_recording(replay, x);
    if (!strcmp(name, "nvnDeviceFinalizeCommandHandle")) {
        if (!x[1])
            return NVN_FRONT_INVALID_GUEST;
        RecordedList **link = find_list(replay, x[1]);
        if (!*link || (*link)->device != x[0])
            return NVN_FRONT_INVALID_GUEST;
        if (__atomic_load_n(&(*link)->pinned, __ATOMIC_ACQUIRE)) {
            (*link)->handle = 0; /* R76: still executing; the cache evicts it later */
            (*link)->last_use = 0;
            return NVN_FRONT_HANDLED;
        }
        remove_list(replay, link);
        return NVN_FRONT_HANDLED;
    }
    bool submit_commands = !strcmp(name, "nvnQueueSubmitCommands");
    bool finish = !strcmp(name, "nvnQueueFinish");
    if (!submit_commands && !finish && strcmp(name, "nvnQueueFlush"))
        return NVN_FRONT_NOT_OURS;
    NvnFrontQueueInfo queue;
    if (!nvn_front_get_queue(replay->config.front, x[0], &queue))
        return NVN_FRONT_INVALID_GUEST;
    if (submit_commands)
        return submit(replay, queue.device, (uint32_t)x[1], x[2]);
    /* Copies are submitted immediately, so there is no buffered work to flush. */
    return !finish || replay->config.finish(replay->config.opaque) ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}

NvnFrontReplyKind nvn_replay_dispatch(NvnReplay *replay, const char *name, uint64_t x[32],
                                     const uint32_t float_args[3])
{
    return nvn_replay_dispatch_keyed(replay, name, NULL, x, float_args);
}
