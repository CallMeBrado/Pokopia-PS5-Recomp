/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nvn_replay.h"
#include "nvn_guest_layout.h"
#include "vk_program.h"

#include <stdlib.h>
#include <string.h>

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

enum { TEXTURE_SLOTS = 32, COMPUTE_STORAGE_SLOTS = 2 };

typedef struct {
    uint64_t target, depth_target, program[2], texture_pool, sampler_pool;
    uint64_t fragment_textures[TEXTURE_SLOTS];
    BufferRange vertex, index, uniform[2];
    PokopiaLiveDraw state;
} Draw;

typedef struct {
    uint64_t program, texture_pool, texture_handles[2];
    BufferRange uniform, storage[COMPUTE_STORAGE_SLOTS];
    uint32_t groups[3];
} ComputeDispatch;

typedef struct {
    uint64_t address;
    void *native;
    bool wait;
} Sync;

typedef struct {
    uint64_t counter, timestamp;
} CounterReport;

typedef struct {
    enum { OP_COPY, OP_CLEAR, OP_DEPTH_CLEAR, OP_DRAW, OP_DISPATCH, OP_SYNC, OP_TIMESTAMP } kind;
    union {
        Copy copy;
        Clear clear;
        DepthClear depth_clear;
        Draw draw;
        ComputeDispatch dispatch;
        Sync sync;
        uint64_t timestamp_address;
    } data;
} Operation;

enum {
    COLOR_TARGETS = 8, VERTEX_BUFFERS = NVN_FRONT_MAX_VERTEX_STREAMS,
    UNIFORM_BUFFERS = 16, PROGRAM_STAGES = NVN_SHADER_COMPUTE + 1,
};

typedef struct Recording {
    struct Recording *next;
    uint64_t encoder, device, handle;
    uint64_t targets[COLOR_TARGETS];
    uint32_t target_layers[COLOR_TARGETS];
    uint64_t depth_target, texture_pool, sampler_pool;
    BufferRange scratch;
    uint64_t program[PROGRAM_STAGES];
    uint64_t textures[PROGRAM_STAGES][TEXTURE_SLOTS];
    BufferRange vertex[VERTEX_BUFFERS], uniform[PROGRAM_STAGES][UNIFORM_BUFFERS];
    BufferRange compute_storage[COMPUTE_STORAGE_SLOTS];
    NvnFrontVertexAttribInfo attributes[NVN_FRONT_MAX_VERTEX_ATTRIBS];
    NvnFrontVertexStreamInfo streams[NVN_FRONT_MAX_VERTEX_STREAMS];
    uint32_t attribute_count, stream_count;
    float viewport[4], depth_range[2];
    int32_t scissor[4];
    bool viewport_set, scissor_set;
    uint32_t color_write_mask, cull, polygon_mode;
    uint32_t raster_bound;
    uint32_t patch_size;
    NvnFrontBlendInfo blend;
    float polygon_offset[3];
    uint8_t stencil[3][2];
    bool blend_enable, depth_test, depth_write, stencil_test;
    bool unsupported_color, unsupported_polygon, unsupported_multisample;
    uint32_t target_count;
    Operation *operations;
    size_t count, capacity;
    bool command_segment;
} Recording;

struct NvnReplay {
    NvnReplayConfig config;
    Recording *recordings;
    uint64_t next_handle;
};

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
    free(recording->operations);
    free(recording);
}

void nvn_replay_destroy(NvnReplay *replay)
{
    if (!replay)
        return;
    while (replay->recordings)
        remove_recording(&replay->recordings);
    free(replay);
}

static Recording **find_recording(NvnReplay *replay, uint64_t handle, uint64_t encoder)
{
    Recording **link = &replay->recordings;
    while (*link && ((*link)->handle != handle || (!handle && (*link)->encoder != encoder)))
        link = &(*link)->next;
    return link;
}

static NvnFrontReplyKind begin_recording(NvnReplay *replay, uint64_t encoder)
{
    NvnFrontCommandBufferInfo command;
    if (!nvn_front_get_command_buffer(replay->config.front, encoder, &command) ||
        *find_recording(replay, 0, encoder))
        return NVN_FRONT_INVALID_GUEST;
    if (!command.command_size || !command.control_size)
        return NVN_FRONT_BOUNDARY;
    Recording *recording = calloc(1, sizeof *recording);
    if (!recording)
        return NVN_FRONT_BOUNDARY;
    if (!nvn_front_charge_command_memory(replay->config.front, encoder, 0, 16)) {
        free(recording);
        return NVN_FRONT_BOUNDARY;
    }
    recording->device = command.device;
    recording->encoder = encoder;
    recording->depth_range[1] = 1.0f;
    recording->next = replay->recordings;
    replay->recordings = recording;
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

static bool append_operation(Recording *recording, Operation operation)
{
    if (recording->count == recording->capacity) {
        if (recording->capacity > SIZE_MAX / 2 / sizeof operation)
            return false;
        size_t capacity = recording->capacity ? recording->capacity * 2 : 8;
        Operation *operations = realloc(recording->operations, capacity * sizeof operation);
        if (!operations)
            return false;
        recording->operations = operations;
        recording->capacity = capacity;
    }
    recording->operations[recording->count++] = operation;
    return true;
}

static bool charge_recording(NvnReplay *replay, Recording *recording, size_t command_bytes, size_t control_bytes)
{
    const bool flush = control_bytes && (recording->command_segment || command_bytes);
    if (!nvn_front_charge_command_memory(replay->config.front, recording->encoder, command_bytes,
                                        control_bytes + (flush ? 32 : 0)))
        return false;
    recording->command_segment = !flush && (command_bytes || recording->command_segment);
    return true;
}

static NvnFrontReplyKind append_metered(NvnReplay *replay, Recording *recording, Operation operation,
                                       size_t command_bytes, size_t control_bytes)
{
    if (!append_operation(recording, operation))
        return NVN_FRONT_BOUNDARY;
    if (charge_recording(replay, recording, command_bytes, control_bytes))
        return NVN_FRONT_HANDLED;
    --recording->count;
    return NVN_FRONT_BOUNDARY;
}

static NvnFrontReplyKind record_copy(NvnReplay *replay, const uint64_t x[32])
{
    Recording *recording = *find_recording(replay, 0, x[0]);
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
        return append_metered(replay, recording, operation, 0, 32);
    const size_t packets = (copy.size - 1) / 0x3fffff + 1;
    return append_metered(replay, recording, operation, 36 * packets + (command.recording_mode <= 4 ? 8 : 0), 0);
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
    Recording *recording = *find_recording(replay, 0, x[0]);
    if (!recording)
        return NVN_FRONT_INVALID_GUEST;
    if ((uint32_t)x[1] != 0 || !replay->config.timestamp_ns)
        return NVN_FRONT_BOUNDARY;
    if (!resolve_counter_report(&replay->config, recording->device, x[2]))
        return NVN_FRONT_INVALID_GUEST;
    const Operation operation = { .kind = OP_TIMESTAMP, .data.timestamp_address = x[2] };
    /* A report semaphore packet carries four words after the method header. */
    return append_metered(replay, recording, operation, 20, 0);
}

enum { TARGET_2D = 1, TARGET_2D_ARRAY = 4, FORMAT_RGBA8 = 0x25, FORMAT_D16 = 0x32, FORMAT_D24S8 = 0x35 };

static bool get_attachment(NvnReplay *replay, uint64_t device, uint64_t address, NvnFrontTextureInfo *target)
{
    return nvn_front_get_texture(replay->config.front, address, target) && target->native &&
           target->create_info.config.device == device && target->create_info.config.levels == 1;
}

static bool get_color_target(NvnReplay *replay, uint64_t device, uint64_t address, NvnFrontTextureInfo *target)
{
    if (!get_attachment(replay, device, address, target) ||
        ((const PokopiaVkTexture *)target->native)->aspects != VK_IMAGE_ASPECT_COLOR_BIT)
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

static bool select_target_layer(NvnReplay *replay, uint64_t address, const NvnFrontTextureInfo *target,
                                uint32_t *selected_layer)
{
    const NvnFrontTextureConfig *config = &target->create_info.config;
    *selected_layer = 0;
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
    if (((fields & 1u) && (view[4] || view[5] != 1)) ||
        ((fields & 4u) && format != config->format) ||
        ((fields & 32u) && view[6] != TARGET_2D && view[6] != config->target))
        return false;
    if (!(fields & 2u))
        return config->depth == 1;
    if (layers != 1 || layer >= config->depth)
        return false;
    *selected_layer = layer;
    return true;
}

static NvnFrontReplyKind set_render_targets(NvnReplay *replay, const uint64_t x[32])
{
    Recording *recording = *find_recording(replay, 0, x[0]);
    uint32_t count = (uint32_t)x[1];
    if (!recording || count > COLOR_TARGETS)
        return NVN_FRONT_INVALID_GUEST;
    uint64_t targets[COLOR_TARGETS] = {0}, views[COLOR_TARGETS] = {0};
    uint32_t layers[COLOR_TARGETS] = {0};
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
    for (uint32_t i = 0; i < count; ++i) {
        if (targets[i]) {
            if (!get_color_target(replay, recording->device, targets[i], &target) ||
                !select_target_layer(replay, views[i], &target, &layers[i]))
                return NVN_FRONT_BOUNDARY;
            ++present;
        }
    }
    if (x[4]) {
        uint32_t depth_layer;
        if (!get_depth_target(replay, recording->device, x[4], &target) ||
            !select_target_layer(replay, x[5], &target, &depth_layer))
            return NVN_FRONT_BOUNDARY;
    }
    /* The NVN2 depth attachment path replaces 16 bytes of
     * disable commands with 124 bytes, then records its 16-byte control entry. */
    if (!charge_recording(replay, recording, 76 + 32 * present + (x[4] ? 108 : 0), x[4] ? 16 : 0))
        return NVN_FRONT_BOUNDARY;
    memcpy(recording->targets, targets, sizeof targets);
    memcpy(recording->target_layers, layers, sizeof layers);
    recording->target_count = count;
    recording->depth_target = x[4];
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind set_scratch(NvnReplay *replay, const uint64_t x[32])
{
    Recording *recording = *find_recording(replay, 0, x[0]);
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
    Recording *recording = *find_recording(replay, 0, x[0]);
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

static NvnFrontReplyKind bind_compute_program(NvnReplay *replay, Recording *recording,
                                             uint64_t address, uint32_t stages, const PokopiaVkProgram *program)
{
    NvnFrontCommandBufferInfo command;
    if (!nvn_front_get_command_buffer(replay->config.front, recording->encoder, &command))
        return NVN_FRONT_INVALID_GUEST;
    PokopiaVkProgramStage stage;
    if (stages != (1u << NVN_SHADER_COMPUTE) ||
        !pokopia_vk_program_stage(program, NVN_SHADER_COMPUTE, &stage) ||
        (stage.resources && compute_abi(&stage) == COMPUTE_UNSUPPORTED))
        return NVN_FRONT_BOUNDARY;
    for (unsigned i = 0; i < NVN_SHADER_COMPUTE; ++i)
        if (pokopia_vk_program_stage(program, i, &stage))
            return NVN_FRONT_BOUNDARY;

    const uint8_t *encoder = replay->config.read_guest(replay->config.opaque, recording->encoder, 0xa9);
    if (!encoder)
        return NVN_FRONT_INVALID_GUEST;
    /* SDK 0x5ff6e0 writes a 16-byte program control record in the
       ordinary recording mode. Inline compute programs need distinct
       packets and remain a boundary. */
    if (encoder[0xa8] || command.recording_mode || !charge_recording(replay, recording, 0, 16))
        return NVN_FRONT_BOUNDARY;
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
    Recording *recording = *find_recording(replay, 0, x[0]);
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
    for (unsigned stage = 0; stage < PROGRAM_STAGES; ++stage) {
        PokopiaVkProgramStage info;
        const bool registered = x[1] && pokopia_vk_program_stage(program.native, stage, &info);
        const bool requested = (stages & (1u << stage)) != 0;
        if (registered && !program.has_unavailable_shader &&
            (!requested || !supported_graphics_shader(stage, info.hash)))
            return NVN_FRONT_BOUNDARY;
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
    for (unsigned stage = 0; stage < PROGRAM_STAGES; ++stage)
        if (stages & (1u << stage))
            recording->program[stage] = bound_stages & (1u << stage) ? x[1] : 0;
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind bind_texture(NvnReplay *replay, const uint64_t x[32])
{
    Recording *r = *find_recording(replay, 0, x[0]);
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
    Recording *recording = *find_recording(replay, 0, x[0]);
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
    Recording *recording = *find_recording(replay, 0, x[0]);
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
    Recording *recording = *find_recording(replay, 0, x[0]);
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
    Recording *r = *find_recording(replay, 0, x[0]);
    if (!strcmp(name, "nvnCommandBufferBindColorState")) {
        NvnFrontColorInfo state;
        if (!r || !nvn_front_read_color(front, x[1], &state))
            return NVN_FRONT_INVALID_GUEST;
        const size_t bytes = (state.logic_op == 3 ? 8 : 12) + (state.alpha_func == 8 ? 4 : 8);
        if (!charge_recording(replay, r, bytes, 0))
            return NVN_FRONT_BOUNDARY;
        r->blend_enable = (state.blend_mask & 1) != 0;
        r->unsupported_color = state.logic_op != 3 || state.alpha_func != 8;
        r->raster_bound |= 1;
    } else if (!strcmp(name, "nvnCommandBufferBindChannelMaskState")) {
        uint32_t masks;
        if (!r || !nvn_front_read_channel_masks(front, x[1], &masks))
            return NVN_FRONT_INVALID_GUEST;
        if (!charge_recording(replay, r, 20, 0))
            return NVN_FRONT_BOUNDARY;
        r->color_write_mask = masks & 15;
        r->raster_bound |= 2;
    } else if (!strcmp(name, "nvnCommandBufferBindPolygonState")) {
        NvnFrontPolygonInfo state;
        if (!r || !nvn_front_read_polygon(front, x[1], &state))
            return NVN_FRONT_INVALID_GUEST;
        if (!charge_recording(replay, r, state.cull_face ? 24 : 20, 0))
            return NVN_FRONT_BOUNDARY;
        r->cull = state.cull_face;
        r->polygon_mode = state.mode;
        r->unsupported_polygon = state.offset_enables != 0;
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
        r->stencil_test = state.stencil_test;
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
    Recording *r = *find_recording(replay, 0, x[0]);
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
    Recording *r = *find_recording(replay, 0, x[0]);
    NvnFrontCommandBufferInfo command;
    if (!r || !nvn_front_get_command_buffer(replay->config.front, x[0], &command))
        return NVN_FRONT_INVALID_GUEST;
    const uint32_t flags = (uint32_t)x[1];
    if ((flags != (ORDER_PRIMITIVES | ORDER_FRAGMENTS) && flags != INVALIDATE_TEXTURE &&
         flags != (ORDER_PRIMITIVES | INVALIDATE_SHADER) && flags != ORDER_INDIRECT_DATA) ||
        command.recording_mode == 1 || command.recording_mode == 4)
        return NVN_FRONT_BOUNDARY;
    /* Each preceding draw or transfer retires before replay executes the next
       operation. Image operations establish their own access scope. Shader code
       is immutable on the host, and replay uploads shader buffers per operation. */
    /* The native writer at sdk+0x61cb70 emits one extra word to invalidate shaders. */
    const size_t base_bytes = command.recording_mode > 4 ? 16 : 24;
    const size_t bytes = command.recording_mode == 2 ? 0 : base_bytes + (flags & INVALIDATE_SHADER ? 4 : 0);
    if (flags == ORDER_INDIRECT_DATA && bytes) {
        /* SDK bit 3 splits the commands around an 8-byte control record
           and closes two segments, leaving the final packet in a new segment.
           Replay waits for GPU writes and copies them to canonical guest
           memory before any following operation can read indirect data. */
        if (!nvn_front_charge_command_memory(replay->config.front, r->encoder, bytes, 2 * 32 + 8))
            return NVN_FRONT_BOUNDARY;
        r->command_segment = true;
        return NVN_FRONT_HANDLED;
    }
    return charge_recording(replay, r, bytes, 0) ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}

static NvnFrontReplyKind disable_tiled_cache(NvnReplay *replay, const uint64_t x[32])
{
    Recording *r = *find_recording(replay, 0, x[0]);
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
    Recording *r = *find_recording(replay, 0, x[0]);
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
    Recording *r = *find_recording(replay, 0, x[0]);
    if (!r)
        return NVN_FRONT_INVALID_GUEST;
    if ((uint8_t)x[1])
        return NVN_FRONT_BOUNDARY;
    return charge_recording(replay, r, bytes, 0) ? NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}

static NvnFrontReplyKind set_polygon_offset(NvnReplay *replay, uint64_t encoder, const uint32_t float_args[3])
{
    Recording *r = *find_recording(replay, 0, encoder);
    if (!r || !float_args)
        return NVN_FRONT_INVALID_GUEST;
    if (!charge_recording(replay, r, 24, 0))
        return NVN_FRONT_BOUNDARY;
    memcpy(r->polygon_offset, float_args, sizeof r->polygon_offset);
    return NVN_FRONT_HANDLED;
}

static NvnFrontReplyKind set_patch_size(NvnReplay *replay, const uint64_t x[32])
{
    Recording *r = *find_recording(replay, 0, x[0]);
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
    Recording *recording = *find_recording(replay, 0, x[0]);
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
    return append_metered(replay, recording, operation, command_bytes, 0);
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

static NvnFrontReplyKind record_depth_clear(NvnReplay *replay, const uint64_t x[32], const uint32_t *float_args)
{
    Recording *recording = *find_recording(replay, 0, x[0]);
    if (!recording || !float_args)
        return NVN_FRONT_INVALID_GUEST;
    NvnFrontTextureInfo texture;
    NvnFrontDeviceModes modes;
    if (!replay->config.clear_depth_stencil ||
        !get_depth_target(replay, recording->device, recording->depth_target, &texture) ||
        !nvn_front_get_device_modes(replay->config.front, recording->device, &modes) ||
        modes.window_origin != NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT)
        return NVN_FRONT_BOUNDARY;

    const uint32_t stencil_mask = (uint32_t)x[3] & 0xff;
    float depth;
    memcpy(&depth, float_args, sizeof depth);
    if ((stencil_mask && (stencil_mask != 0xff || texture.create_info.config.format == FORMAT_D16)) ||
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
    return append_metered(replay, recording, operation, 0, 24);
}

static NvnFrontReplyKind record_draw(NvnReplay *replay, const uint64_t x[32], bool indexed,
                                     bool base_vertex, bool instanced)
{
    Recording *r = *find_recording(replay, 0, x[0]);
    uint32_t count = (uint32_t)x[3];
    if (!r || count > INT32_MAX || (!indexed && (uint32_t)x[2] > INT32_MAX))
        return NVN_FRONT_INVALID_GUEST;
    if (!count || (instanced && !(uint32_t)x[7]))
        return NVN_FRONT_HANDLED;
    for (unsigned stage = NVN_SHADER_VERTEX; stage <= NVN_SHADER_FRAGMENT; ++stage) {
        NvnFrontProgramInfo program;
        PokopiaVkProgramStage info;
        if (r->program[stage] && !require_program_stage(replay->config.front, r->device, r->program[stage],
                                                          stage, &program, &info))
            return NVN_FRONT_BOUNDARY;
    }
    NvnFrontDeviceModes modes;
    NvnFrontTextureInfo target, depth_target = {0};
    if (!nvn_front_get_device_modes(replay->config.front, r->device, &modes) ||
        modes.window_origin != NVN_FRONT_WINDOW_ORIGIN_UPPER_LEFT ||
        modes.depth_mode != NVN_FRONT_DEPTH_NEAR_IS_ZERO || (uint32_t)x[1] != 4 ||
        (indexed && (uint32_t)x[2] != 1) ||
        r->target_count != 1 || !get_color_target(replay, r->device, r->targets[0], &target) ||
        (r->depth_target && !get_depth_target(replay, r->device, r->depth_target, &depth_target)) ||
        (!r->depth_target && target.create_info.config.depth != 1) ||
        !r->viewport_set || !r->scissor_set || r->scissor[2] <= 0 || r->scissor[3] <= 0 ||
        (indexed && (r->attribute_count < 3 || !r->stream_count || r->streams[0].divisor)) ||
        (r->raster_bound & 15) != 15 || r->unsupported_color || r->unsupported_polygon ||
        r->unsupported_multisample || r->polygon_mode != 2 || r->cull)
        return NVN_FRONT_BOUNDARY;
    for (unsigned i = 2; i < NVN_SHADER_COMPUTE; ++i)
        if (r->program[i])
            return NVN_FRONT_BOUNDARY;

    Operation operation = {.kind = OP_DRAW, .data.draw = {
        .target = r->targets[0], .depth_target = r->depth_target,
        .program = {r->program[0], r->program[1]},
        .texture_pool = r->texture_pool, .sampler_pool = r->sampler_pool,
        .vertex = r->vertex[0], .index = indexed ? (BufferRange){x[4], (size_t)count * 2} : (BufferRange){0},
        .uniform = {r->uniform[0][0], r->uniform[1][0]},
        .state = {
            .target = target.native, .depth_target = depth_target.native, .target_layer = r->target_layers[0],
            .extent = {target.create_info.config.width, target.create_info.config.height},
            .vertex_stride = indexed ? r->streams[0].stride & 0xfff : 0, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .index_type = VK_INDEX_TYPE_UINT16,
            .viewport = {r->viewport[0], r->viewport[1] + r->viewport[3], r->viewport[2],
                         -r->viewport[3], r->depth_range[0], r->depth_range[1]},
            .scissor = {{r->scissor[0], r->scissor[1]}, {(uint32_t)r->scissor[2], (uint32_t)r->scissor[3]}},
            .color_write_mask = r->color_write_mask, .polygon_mode = VK_POLYGON_MODE_FILL,
            .cull_mode = VK_CULL_MODE_NONE, .blend_enable = r->blend_enable, .blend = r->blend,
            .depth_test = r->depth_test, .depth_write = r->depth_write, .stencil_test = r->stencil_test,
            .vertex_count = indexed ? 0 : count, .first_vertex = indexed ? 0 : (uint32_t)x[2],
            .index_count = indexed ? count : 0, .base_vertex = base_vertex ? (int32_t)x[5] : 0,
            .instance_count = instanced ? (uint32_t)x[7] : 1, .first_instance = instanced ? (uint32_t)x[6] : 0,
        },
    }};
    memcpy(operation.data.draw.fragment_textures, r->textures[NVN_SHADER_FRAGMENT],
           sizeof operation.data.draw.fragment_textures);
    for (unsigned i = 0; indexed && i < r->attribute_count; ++i) {
        const NvnFrontVertexAttribInfo *attribute = &r->attributes[i];
        if (i >= 4 || !attribute->enabled) {
            if (attribute->enabled || i < 3)
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
    if (!indexed)
        return append_metered(replay, r, operation, 16, 0);
    const size_t bytes = !base_vertex ? 24 : (operation.data.draw.state.base_vertex ? 72 : 48);
    return append_metered(replay, r, operation, bytes, 0);
}

static NvnFrontReplyKind record_dispatch(NvnReplay *replay, const uint64_t x[32])
{
    Recording *r = *find_recording(replay, 0, x[0]);
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
    if (abi == COMPUTE_UNSUPPORTED ||
        (abi == COMPUTE_RESET_COUNTER && (groups[0] != 1 || groups[1] != 1 || groups[2] != 1)) ||
        (abi == COMPUTE_CLUSTERED_LIGHTS && (groups[0] != 30 || groups[1] != 17 || groups[2] != 16)))
        return NVN_FRONT_BOUNDARY;
    const Operation operation = {.kind = OP_DISPATCH, .data.dispatch = {
        .program = r->program[NVN_SHADER_COMPUTE], .texture_pool = r->texture_pool,
        .texture_handles = {r->textures[NVN_SHADER_COMPUTE][0], r->textures[NVN_SHADER_COMPUTE][1]},
        .uniform = r->uniform[NVN_SHADER_COMPUTE][0],
        .storage = {r->compute_storage[1], r->compute_storage[0]},
        .groups = {groups[0], groups[1], groups[2]},
    }};
    /* SDK 0x60d9c0 emits a 16-byte dispatch control record. */
    return append_metered(replay, r, operation, 0, 16);
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

static bool resolve_sampled_view(const NvnFrontTextureDescriptorInfo *descriptor, PokopiaLiveDrawSample *sample)
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
    if (((fields & 1u) && (view[4] || view[5] != 1)) ||
        ((fields & 2u) && (layer || layers != 1)) ||
        ((fields & 8u) && memcmp(view + 12, identity, sizeof identity)) ||
        ((fields & 32u) && view[6] != config->target))
        return false;
    if ((fields & 4u) && format != config->format) {
        if (!sample)
            return false;
        const PokopiaVkTexture *texture = descriptor->texture.native;
        const VkFormat view_format = nvn_guest_texture_vk_format(format);
        if (!texture->sample_alias_view || view_format != texture->sample_alias_format)
            return false;
        sample->view_format = view_format;
    }
    return true;
}

static bool resolve_fragment_sample(NvnFront *front, uint64_t device, const Draw *record,
                                    unsigned slot, PokopiaLiveDrawSample *sample)
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
    if (!resolve_sampled_view(&desc, sample))
        return false;
    sample->texture = desc.texture.native;
    return true;
}

static NvnFrontReplyKind run_draw(NvnReplay *replay, uint64_t device, const Draw *record, bool execute)
{
    const NvnReplayConfig *config = &replay->config;
    NvnFrontTextureInfo target;
    if (!get_color_target(replay, device, record->target, &target) || target.native != record->state.target)
        return NVN_FRONT_INVALID_GUEST;
    if (record->depth_target &&
        (!get_depth_target(replay, device, record->depth_target, &target) || target.native != record->state.depth_target))
        return NVN_FRONT_INVALID_GUEST;
    PokopiaLiveDraw draw = record->state;
    PokopiaLiveDrawShader *shaders[] = {&draw.vertex_shader, &draw.fragment_shader};
    PokopiaLiveDrawSpan *uniforms[] = {&draw.vertex_uniform, &draw.fragment_uniform};
    const PokopiaVkProgram *programs[2];
    PokopiaVkProgramStage stages[2];
    for (unsigned stage = 0; stage < 2; ++stage) {
        NvnFrontProgramInfo program;
        if (!require_program_stage(config->front, device, record->program[stage], stage, &program, &stages[stage]))
            return NVN_FRONT_BOUNDARY;
        programs[stage] = program.native;
    }
    for (unsigned stage = 0; stage < 2; ++stage) {
        const PokopiaVkProgramStage info = stages[stage];
        if (!supported_graphics_shader(stage, info.hash))
            return NVN_FRONT_BOUNDARY;
        shaders[stage]->hash = info.hash;
        DrawAbi abi;
        if (!draw_abi(info.resources, stage, &abi))
            return NVN_FRONT_BOUNDARY;
        if (abi.uniform0) {
            *uniforms[stage] = draw_span(config, device, record->uniform[stage]);
            if (!uniforms[stage]->data)
                return NVN_FRONT_INVALID_GUEST;
        }
        for (unsigned sample = 0; sample < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++sample) {
            const int slot = abi.fragment_slots[sample];
            if (slot >= 0 &&
                !resolve_fragment_sample(config->front, device, record, (unsigned)slot, &draw.fragment_samples[sample]))
                return NVN_FRONT_BOUNDARY;
        }
    }
    if (draw.index_count) {
        draw.vertices = draw_span(config, device, record->vertex);
        draw.indices = draw_span(config, device, record->index);
        if (!draw.vertices.data || !draw.indices.data)
            return NVN_FRONT_INVALID_GUEST;
    }
    if (!execute)
        return NVN_FRONT_HANDLED;

    PokopiaVkProgramModule modules[2] = {0};
    NvnFrontReplyKind result = NVN_FRONT_BOUNDARY;
    for (unsigned stage = 0; stage < 2; ++stage) {
        if (!pokopia_vk_program_acquire_module(programs[stage], stage, &modules[stage]))
            goto done;
        shaders[stage]->module = modules[stage].module;
        shaders[stage]->entry = modules[stage].entry;
    }
    if (config->draw(config->opaque, &draw))
        result = NVN_FRONT_HANDLED;

done:
    for (unsigned stage = 0; stage < 2; ++stage)
        pokopia_vk_program_release_module(programs[stage], &modules[stage]);
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
    Recording *r = *find_recording(replay, 0, x[0]);
    if (!r)
        return NVN_FRONT_INVALID_GUEST;
    void *sync = get_sync(&replay->config, r->device, x[1]);
    if (!sync)
        return NVN_FRONT_INVALID_GUEST;
    if (!wait && ((uint32_t)x[2] || (uint32_t)x[3] > 1))
        return NVN_FRONT_BOUNDARY;
    Operation operation = {.kind = OP_SYNC, .data.sync = {x[1], sync, wait}};
    return append_metered(replay, r, operation, 0, 16);
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
    if (!resolve_sampled_view(&descriptor, NULL))
        return false;
    PokopiaVkTexture *texture = descriptor.texture.native;
    if (!texture->texel.view || texture->texel.format != VK_FORMAT_R32G32B32A32_SFLOAT ||
        texture->texel.bytes != expected)
        return false;
    const PokopiaLiveDrawSpan bytes = draw_span(config, device, (BufferRange){info->storage_address, expected});
    if (!bytes.data)
        return false;
    *texel = &texture->texel;
    *source = bytes.data;
    return true;
}

static NvnFrontReplyKind run_compute(NvnReplay *replay, uint64_t device, const ComputeDispatch *record, bool execute)
{
    const NvnReplayConfig *config = &replay->config;
    NvnFrontProgramInfo program;
    PokopiaVkProgramStage stage;
    if (!config->compute || !require_program_stage(config->front, device, record->program, NVN_SHADER_COMPUTE,
                                                   &program, &stage))
        return NVN_FRONT_BOUNDARY;
    const ComputeAbi abi = compute_abi(&stage);
    if (abi == COMPUTE_UNSUPPORTED)
        return NVN_FRONT_BOUNDARY;

    const bool clustered = abi == COMPUTE_CLUSTERED_LIGHTS;
    const PokopiaLiveDrawSpan uniform = draw_span(config, device, record->uniform);
    if (!uniform.data || uniform.bytes != (clustered ? 512u : 256u))
        return NVN_FRONT_INVALID_GUEST;
    PokopiaLiveCompute dispatch = {
        .hash = stage.hash, .uniform = uniform.data, .uniform_bytes = uniform.bytes,
        .groups = {record->groups[0], record->groups[1], record->groups[2]},
    };
    const size_t expected_storage[] = {4, 4065280};
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
        const void *source;
        void *destination;
        if (!resolve_copy(replay, device, copy, &source, &destination))
            return NVN_FRONT_INVALID_GUEST;
        return !execute || config->copy(config->opaque, source, destination, copy.size) ?
            NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
    }
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
        return !execute || config->sync(config->opaque, sync->native, sync->wait) ?
            NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
    }
    if (operation->kind == OP_DEPTH_CLEAR) {
        const DepthClear *clear = &operation->data.depth_clear;
        NvnFrontTextureInfo texture;
        if (!get_depth_target(replay, device, clear->address, &texture) || texture.native != clear->native)
            return NVN_FRONT_INVALID_GUEST;
        return !execute || config->clear_depth_stencil(config->opaque, texture.native, clear->area,
                                                        clear->aspects, clear->value) ?
            NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
    }
    const Clear *clear = &operation->data.clear;
    NvnFrontTextureInfo texture;
    if (!get_color_target(replay, device, clear->address, &texture) || texture.native != clear->native)
        return NVN_FRONT_INVALID_GUEST;
    return !execute || config->clear(config->opaque, texture.native, clear->area, clear->rgba, clear->mask) ?
        NVN_FRONT_HANDLED : NVN_FRONT_BOUNDARY;
}

static Recording *command_at(NvnReplay *replay, const uint8_t *handles, uint32_t index, uint64_t device)
{
    uint64_t handle;
    memcpy(&handle, handles + index * sizeof handle, sizeof handle);
    if (!handle)
        return NULL;
    Recording *recording = *find_recording(replay, handle, 0);
    return recording && recording->device == device ? recording : NULL;
}

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
    Recording **batch = calloc(count, sizeof *batch);
    if (!batch)
        return NVN_FRONT_BOUNDARY;
    NvnFrontReplyKind result = NVN_FRONT_INVALID_GUEST;
    /* Validate every handle and pool before making any GPU writes. */
    for (uint32_t i = 0; i < count; ++i) {
        Recording *recording = command_at(replay, handles, i, device);
        if (!recording) {
            result = NVN_FRONT_INVALID_GUEST;
            goto done;
        }
        batch[i] = recording;
        for (size_t j = 0; j < recording->count; ++j) {
            result = run_operation(replay, device, &recording->operations[j], false);
            if (result != NVN_FRONT_HANDLED)
                goto done;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        Recording *recording = batch[i];
        for (size_t j = 0; j < recording->count; ++j) {
            result = run_operation(replay, device, &recording->operations[j], true);
            if (result != NVN_FRONT_HANDLED)
                goto done;
        }
    }
    result = NVN_FRONT_HANDLED;
done:
    free(batch);
    return result;
}

NvnFrontReplyKind nvn_replay_dispatch(NvnReplay *replay, const char *name, uint64_t x[32],
                                     const uint32_t float_args[3])
{
    if (!replay || !name || !x)
        return NVN_FRONT_NOT_OURS;
    if (!strcmp(name, "nvnCommandBufferBeginRecording"))
        return begin_recording(replay, x[0]);
    if (!strcmp(name, "nvnCommandBufferCopyBufferToBuffer"))
        return record_copy(replay, x);
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
        return bind_program(replay, x);
    if (!strcmp(name, "nvnCommandBufferBindTexture"))
        return bind_texture(replay, x);
    if (!strcmp(name, "nvnCommandBufferBindVertexBuffer"))
        return bind_buffers(replay, x, false, false);
    if (!strcmp(name, "nvnCommandBufferBindVertexBuffers"))
        return bind_buffers(replay, x, false, true);
    if (!strcmp(name, "nvnCommandBufferBindVertexAttribState"))
        return bind_vertex_state(replay, x, false);
    if (!strcmp(name, "nvnCommandBufferBindVertexStreamState"))
        return bind_vertex_state(replay, x, true);
    if (!strcmp(name, "nvnCommandBufferBindUniformBuffer"))
        return bind_buffers(replay, x, true, false);
    if (!strcmp(name, "nvnCommandBufferBindUniformBuffers"))
        return bind_buffers(replay, x, true, true);
    if (!strcmp(name, "nvnCommandBufferBindStorageBuffer"))
        return (uint32_t)x[1] == NVN_SHADER_COMPUTE ?
            bind_compute_buffer(replay, *find_recording(replay, 0, x[0]), x, false) : NVN_FRONT_BOUNDARY;
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
        return set_stencil_value(replay, x, 0);
    if (!strcmp(name, "nvnCommandBufferSetStencilMask"))
        return set_stencil_value(replay, x, 1);
    if (!strcmp(name, "nvnCommandBufferSetStencilRef"))
        return set_stencil_value(replay, x, 2);
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
        return record_draw(replay, x, true, true, false);
    if (!strcmp(name, "nvnCommandBufferDrawElementsInstanced"))
        return record_draw(replay, x, true, true, true);
    if (!strcmp(name, "nvnCommandBufferDispatchCompute"))
        return record_dispatch(replay, x);
    if (!strcmp(name, "nvnCommandBufferFenceSync"))
        return record_sync(replay, x, false);
    if (!strcmp(name, "nvnCommandBufferWaitSync"))
        return record_sync(replay, x, true);
    if (!strcmp(name, "nvnCommandBufferEndRecording")) {
        Recording *recording = *find_recording(replay, 0, x[0]);
        if (!recording)
            return NVN_FRONT_INVALID_GUEST;
        if (replay->next_handle >= UINT64_C(0x7f20000000))
            return NVN_FRONT_BOUNDARY;
        if (!charge_recording(replay, recording, 0, 8))
            return NVN_FRONT_BOUNDARY;
        recording->handle = replay->next_handle;
        replay->next_handle += 8;
        x[0] = recording->handle;
        return NVN_FRONT_HANDLED;
    }
    if (!strcmp(name, "nvnDeviceFinalizeCommandHandle")) {
        if (!x[1])
            return NVN_FRONT_INVALID_GUEST;
        Recording **link = find_recording(replay, x[1], 0);
        if (!*link || (*link)->device != x[0])
            return NVN_FRONT_INVALID_GUEST;
        remove_recording(link);
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


