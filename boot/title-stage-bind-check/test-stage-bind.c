#define nvn_front_get_program fixture_get_program
#define nvn_front_get_command_buffer fixture_get_command_buffer
#define nvn_front_charge_command_memory fixture_charge_command_memory
#define pokopia_vk_program_stage fixture_program_stage
#include "nvn_replay.c"

#include <assert.h>

enum { DEVICE = 1, ENCODER, PROGRAM, VERTEX_PROGRAM, FRAGMENT_PROGRAM, CALLBACK };

static int native_program;
static uint64_t command_size = 256, command_used, control_used;
static uint32_t registered_stages, queried_stages;
static const uint8_t enabled = 1;

VkFormat nvn_guest_texture_vk_format(uint32_t format)
{
    (void)format;
    abort();
}

bool nvn_guest_texture_layout(const NvnFrontTextureConfig *config,
                              uint64_t *storage_size, uint64_t *alignment)
{
    (void)config;
    (void)storage_size;
    (void)alignment;
    abort();
}

bool fixture_get_program(NvnFront *front, uint64_t address, NvnFrontProgramInfo *info)
{
    (void)front;
    if (address != PROGRAM)
        return false;
    *info = (NvnFrontProgramInfo){.device = DEVICE, .native = &native_program, .linked = true};
    return true;
}

bool fixture_program_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                           PokopiaVkProgramStage *info)
{
    assert((const void *)program == &native_program);
    const uint32_t bit = 1u << stage;
    queried_stages |= bit;
    if (!(registered_stages & bit))
        return false;
    *info = (PokopiaVkProgramStage){stage == NVN_SHADER_VERTEX ? "821538a4398f85a7" : "7c8c05da24a1f5a2", "resources"};
    return true;
}

bool fixture_get_command_buffer(NvnFront *front, uint64_t address, NvnFrontCommandBufferInfo *info)
{
    (void)front;
    assert(address == ENCODER);
    *info = (NvnFrontCommandBufferInfo){.device = DEVICE, .command_size = command_size,
        .command_used = command_used, .control_size = 256, .control_used = control_used,
        .memory_callback = CALLBACK, .command_revision = 2};
    return true;
}

bool fixture_charge_command_memory(NvnFront *front, uint64_t encoder, size_t command, size_t control)
{
    (void)front;
    assert(encoder == ENCODER);
    command_used += command;
    control_used += control;
    return true;
}

static const void *fixture_read_guest(void *opaque, uint64_t address, size_t bytes)
{
    (void)opaque;
    assert(address == ENCODER + 0x14 && bytes == 1);
    return &enabled;
}

int main(void)
{
    Recording recording = {.device = DEVICE, .encoder = ENCODER,
        .program = {VERTEX_PROGRAM, FRAGMENT_PROGRAM}};
    NvnReplay replay = {.config = {.read_guest = fixture_read_guest}, .recordings = &recording};
    uint64_t x[32] = {ENCODER, PROGRAM, 2};

    registered_stages = 3;
    assert(!supported_graphics_shader(NVN_SHADER_FRAGMENT, "7c8c05da24a1f5a2"));
    assert(bind_program(&replay, x) == NVN_FRONT_HANDLED);
    assert(recording.program[0] == VERTEX_PROGRAM && recording.program[1] == PROGRAM);
    assert(queried_stages == 2 && command_used == 60);

    registered_stages = 1;
    assert(bind_program(&replay, x) == NVN_FRONT_HANDLED);
    assert(recording.program[0] == VERTEX_PROGRAM && recording.program[1] == 0);
    assert(command_used == 68);

    x[2] = 4;
    registered_stages = 4;
    assert(bind_program(&replay, x) == NVN_FRONT_BOUNDARY);
    assert(recording.program[2] == 0 && command_used == 68);

    x[2] = 2;
    registered_stages = 3;
    command_size = 127;
    assert(bind_program(&replay, x) == NVN_FRONT_BOUNDARY);
    assert(recording.program[1] == 0 && command_used == 68 && control_used == 32);
    assert(replay.memory_request.event == NVN_REPLAY_MEMORY_COMMAND);
    assert(replay.memory_request.function == CALLBACK && replay.memory_request.minimum_bytes == 512);

    command_size = 128;
    assert(bind_program(&replay, x) == NVN_FRONT_HANDLED);
    assert(recording.program[0] == VERTEX_PROGRAM && recording.program[1] == PROGRAM);
    assert(command_used == 128);
    puts("PASS fragment mask ignores unrequested vertex, binds unknown fragment, clears absent stage and requests refill transactionally");
}
