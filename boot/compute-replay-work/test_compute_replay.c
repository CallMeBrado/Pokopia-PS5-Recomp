#define nvn_front_get_command_buffer mock_command_buffer
#define nvn_front_charge_command_memory mock_charge
#define nvn_front_get_queue mock_queue
#define nvn_front_get_program mock_program
#define nvn_front_get_descriptor_pool mock_descriptor_pool
#define nvn_front_get_texture_descriptor mock_texture_descriptor
#define nvn_front_resolve_pool mock_resolve_pool
#include "nvn_replay.c"
#include "../../gfx-bridge/vk_program.c"

#include <assert.h>
#include <inttypes.h>

enum {
    DEVICE = 0x100, ENCODER = 0x200, QUEUE = 0x300,
    PROGRAM_RESET = 0x400, PROGRAM_LIGHTS = 0x500, TEXTURE_POOL = 0x600,
    UNIFORM_RESET = 0x1000, UNIFORM_LIGHTS = 0x2000, COUNTER = 0x3000,
    TEXELS_POINT = 0x4000, TEXELS_SPOT = 0x9000, CLUSTERS = 0xa00000,
    HANDLES = 0x800,
};
enum { CLUSTER_BYTES = 4065280 };
static uint8_t encoder[0xa9], reset_ubo[256], lights_ubo[512];
static uint32_t counter;
static float point_texels[1024 * 4], spot_texels[64 * 4];
static uint32_t clusters[CLUSTER_BYTES / 4];
static PokopiaVkTexture textures[2];
static PokopiaVkProgram *programs[2];
static uint64_t submitted_handles[1];
static size_t command_used, control_used;
static uint32_t mode, executions;
static bool spot_available = true;

static void *guest_write(void *unused, uint64_t address, size_t count)
{
    (void)unused;
    const struct GuestSpan { uint64_t address; void *host; size_t size; } guest[] = {
        {ENCODER, encoder, sizeof encoder},
        {HANDLES, submitted_handles, sizeof submitted_handles},
        {UNIFORM_RESET, reset_ubo, sizeof reset_ubo},
        {UNIFORM_LIGHTS, lights_ubo, sizeof lights_ubo},
        {COUNTER, &counter, sizeof counter},
        {TEXELS_POINT, point_texels, sizeof point_texels},
        {TEXELS_SPOT, spot_texels, sizeof spot_texels},
        {CLUSTERS, clusters, sizeof clusters},
    };
    for (size_t i = 0; i < sizeof guest / sizeof guest[0]; ++i) {
        if (address < guest[i].address)
            continue;
        const uint64_t offset = address - guest[i].address;
        if (offset <= guest[i].size && count <= guest[i].size - offset)
            return (uint8_t *)guest[i].host + offset;
    }
    return NULL;
}

static const void *guest_read(void *unused, uint64_t address, size_t count)
{
    return guest_write(unused, address, count);
}

bool mock_command_buffer(NvnFront *front, uint64_t address, NvnFrontCommandBufferInfo *out)
{
    (void)front;
    if (address != ENCODER)
        return false;
    *out = (NvnFrontCommandBufferInfo){.device = DEVICE, .command_size = 0x2000,
                                      .control_size = 0x2000, .recording_mode = mode};
    return true;
}

bool mock_charge(NvnFront *front, uint64_t address, size_t command_bytes, size_t control_bytes)
{
    (void)front;
    if (address != ENCODER || command_bytes > 0x2000 - command_used || control_bytes > 0x2000 - control_used)
        return false;
    command_used += command_bytes;
    control_used += control_bytes;
    return true;
}

bool mock_queue(NvnFront *front, uint64_t address, NvnFrontQueueInfo *out)
{
    (void)front;
    if (address != QUEUE)
        return false;
    *out = (NvnFrontQueueInfo){.device = DEVICE};
    return true;
}

bool mock_program(NvnFront *front, uint64_t address, NvnFrontProgramInfo *out)
{
    (void)front;
    if (address != PROGRAM_RESET && address != PROGRAM_LIGHTS)
        return false;
    *out = (NvnFrontProgramInfo){.device = DEVICE, .native = programs[address == PROGRAM_LIGHTS], .linked = true};
    return true;
}

bool mock_descriptor_pool(NvnFront *front, uint64_t address, bool sampler, NvnFrontDescriptorPoolInfo *out)
{
    (void)front;
    if (address != TEXTURE_POOL || sampler)
        return false;
    *out = (NvnFrontDescriptorPoolInfo){.device = DEVICE, .count = 512};
    return true;
}

bool mock_texture_descriptor(NvnFront *front, uint64_t address, uint32_t slot, NvnFrontTextureDescriptorInfo *out)
{
    (void)front;
    if (address != TEXTURE_POOL || (slot != 22 && slot != 23) || (slot == 23 && !spot_available))
        return false;
    const bool spot = slot == 23;
    *out = (NvnFrontTextureDescriptorInfo){
        .texture = {.native = &textures[spot], .create_info = {
            .config = {.device = DEVICE, .target = 0xa, .format = 0x22},
            .storage_address = spot ? TEXELS_SPOT : TEXELS_POINT,
            .storage_size = spot ? sizeof spot_texels : sizeof point_texels,
        }},
    };
    return true;
}

bool mock_resolve_pool(const NvnFront *front, uint64_t device, uint64_t address, size_t bytes, NvnFrontPoolHit *out)
{
    (void)front;
    if (device != DEVICE || !guest_read(NULL, address, bytes))
        return false;
    *out = (NvnFrontPoolHit){0};
    return true;
}

static bool do_copy(void *p, const void *src, void *dst, size_t bytes)
{ (void)p; (void)src; (void)dst; (void)bytes; abort(); }
static bool do_clear(void *p, void *texture, const float rgba[4], uint32_t mask)
{ (void)p; (void)texture; (void)rgba; (void)mask; abort(); }
static bool do_draw(void *p, const PokopiaLiveDraw *draw)
{ (void)p; (void)draw; abort(); }
static bool do_sync(void *p, void *s, bool wait)
{ (void)p; (void)s; (void)wait; abort(); }
static bool do_finish(void *p)
{ (void)p; abort(); }

static bool do_compute(void *adapter, const PokopiaLiveCompute *dispatch)
{
    ++executions;
    assert(dispatch->module && dispatch->entry);
    VkResult result = pokopia_live_compute(adapter, dispatch);
    if (result != VK_SUCCESS)
        fprintf(stderr, "Vulkan compute failed (%d) for %s\n", result, dispatch->hash);
    return result == VK_SUCCESS;
}

static uint64_t dispatch_expected(NvnReplay *replay, NvnFrontReplyKind expected,
                                  const char *call, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    uint64_t args[32] = {a0, a1, a2, a3, a4};
    const NvnFrontReplyKind result = nvn_replay_dispatch(replay, call, args, NULL);
    if (result != expected) {
        fprintf(stderr, "%s returned %d expected %d\n", call, result, expected);
        abort();
    }
    return args[0];
}
#define HANDLED(replay, call, a0, a1, a2, a3, a4) \
    dispatch_expected(replay, NVN_FRONT_HANDLED, call, a0, a1, a2, a3, a4)

static void load_file(const char *path, void *destination, size_t bytes)
{
    FILE *file = fopen(path, "rb");
    assert(file && fread(destination, 1, bytes, file) == bytes && fgetc(file) == EOF);
    fclose(file);
}

static PokopiaVkProgram *load_program(PokopiaVkAdapter *adapter, NvnShaderCache *cache, const char *hash)
{
    /* Raw guest GLSLC code isn't part of this fixture. Seed linked metadata
       from the real cache to exercise production lazy module materialization. */
    PokopiaVkProgram *program = pokopia_vk_program_create(adapter);
    assert(program);
    NvnCachedShader cached = {0};
    assert(nvn_shader_cache_lookup_key(cache, hash, NVN_SHADER_COMPUTE, &cached) == NVN_SHADER_CACHE_OK);
    program->log = stdout;
    ProgramStage *stage = &program->stages[NVN_SHADER_COMPUTE];
    stage->cache = cache;
    memcpy(stage->hash, cached.hash, sizeof stage->hash);
    stage->resources = cached.resources;
    cached.resources = NULL;
    nvn_cached_shader_release(&cached);
    return program;
}

static void make_texel(PokopiaVkAdapter *adapter, unsigned i, uint32_t count)
{
    const VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkFormatProperties props;
    adapter->format_properties(adapter->physical, format, &props);
    assert(pokopia_vk_texel_open(&textures[i].texel, adapter->device, adapter->get_device_proc,
        &adapter->memory, props.bufferFeatures, format, count, 16,
        adapter->properties.limits.maxTexelBufferElements) == VK_SUCCESS);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    PokopiaVkAdapter adapter = {0};
    assert(pokopia_vk_open(&adapter, stdout));
    NvnShaderCacheStatus status;
    NvnShaderCache *cache = nvn_shader_cache_open("../../frame-reference-655360/shader_cache/spirv", &status);
    assert(cache && status == NVN_SHADER_CACHE_OK);
    programs[0] = load_program(&adapter, cache, "082274fc8b17c887");
    programs[1] = load_program(&adapter, cache, "b81c48ff486acef4");
    load_file("uniform1.bin", reset_ubo, sizeof reset_ubo);
    load_file("uniform2.bin", lights_ubo, sizeof lights_ubo);
    make_texel(&adapter, 0, 1024);
    make_texel(&adapter, 1, 64);
    NvnFront *mock = (NvnFront *)(uintptr_t)DEVICE;
    NvnReplay *replay = nvn_replay_create(&(NvnReplayConfig){
        .front = mock, .opaque = &adapter, .read_guest = guest_read, .write_guest = guest_write,
        .copy = do_copy, .clear = do_clear, .draw = do_draw, .compute = do_compute,
        .sync = do_sync, .finish = do_finish,
    });
    assert(replay);

    HANDLED(replay, "nvnCommandBufferBeginRecording", ENCODER, 0, 0, 0, 0);
    assert(control_used == 16);
    encoder[0xa8] = 1;
    dispatch_expected(replay, NVN_FRONT_BOUNDARY, "nvnCommandBufferBindProgram", ENCODER, PROGRAM_RESET, 32, 0, 0);
    assert(control_used == 16 && !command_used);
    encoder[0xa8] = 0;
    mode = 3;
    dispatch_expected(replay, NVN_FRONT_BOUNDARY, "nvnCommandBufferBindProgram", ENCODER, PROGRAM_RESET, 32, 0, 0);
    assert(control_used == 16 && !command_used);
    mode = 0;
    HANDLED(replay, "nvnCommandBufferBindProgram", ENCODER, PROGRAM_RESET, 32, 0, 0);
    HANDLED(replay, "nvnCommandBufferBindUniformBuffer", ENCODER, NVN_SHADER_COMPUTE, 0, UNIFORM_RESET, sizeof reset_ubo);
    HANDLED(replay, "nvnCommandBufferBindStorageBuffer", ENCODER, NVN_SHADER_COMPUTE, 1, COUNTER, sizeof counter);
    HANDLED(replay, "nvnCommandBufferDispatchCompute", ENCODER, 1, 1, 1, 0);
    assert(control_used == 96 && !command_used);
    HANDLED(replay, "nvnCommandBufferBindProgram", ENCODER, PROGRAM_LIGHTS, 32, 0, 0);
    HANDLED(replay, "nvnCommandBufferBindUniformBuffer", ENCODER, NVN_SHADER_COMPUTE, 0, UNIFORM_LIGHTS, sizeof lights_ubo);
    HANDLED(replay, "nvnCommandBufferBindStorageBuffer", ENCODER, NVN_SHADER_COMPUTE, 0, CLUSTERS, sizeof clusters);
    HANDLED(replay, "nvnCommandBufferSetTexturePool", ENCODER, TEXTURE_POOL, 0, 0, 0);
    HANDLED(replay, "nvnCommandBufferBindTexture", ENCODER, NVN_SHADER_COMPUTE, 0, 22, 0);
    HANDLED(replay, "nvnCommandBufferBindTexture", ENCODER, NVN_SHADER_COMPUTE, 1, 23, 0);
    HANDLED(replay, "nvnCommandBufferDispatchCompute", ENCODER, 30, 17, 16, 0);
    assert(control_used == 224 && !command_used);
    /* Rebinding after a dispatch may not alter its earlier snapshot. */
    HANDLED(replay, "nvnCommandBufferBindTexture", ENCODER, NVN_SHADER_COMPUTE, 0, 511, 0);
    submitted_handles[0] = HANDLED(replay, "nvnCommandBufferEndRecording", ENCODER, 0, 0, 0, 0);
    assert(control_used == 248 && !command_used);
    counter = 0xdecafbad;
    memset(clusters, 0xa5, sizeof clusters);
    spot_available = false;
    dispatch_expected(replay, NVN_FRONT_BOUNDARY, "nvnQueueSubmitCommands", QUEUE, 1, HANDLES, 0, 0);
    assert(!executions && counter == 0xdecafbad && clusters[0] == 0xa5a5a5a5);

    spot_available = true;
    const uint32_t point_count = 1;
    memcpy(lights_ubo + 12 * sizeof(uint32_t), &point_count, sizeof point_count);
    point_texels[2] = -1000.0f;
    point_texels[3] = 100000.0f;
    memset(clusters, 0, sizeof clusters);
    HANDLED(replay, "nvnQueueSubmitCommands", QUEUE, 1, HANDLES, 0, 0);
    assert(executions == 2 && counter == 0xbf40);
    puts("PASS replay accounting, full-list preflight, immutable handles, refreshed guest texels, lazy shaders, and two ordered GPU dispatches");
    HANDLED(replay, "nvnDeviceFinalizeCommandHandle", DEVICE, submitted_handles[0], 0, 0, 0);
    nvn_replay_destroy(replay);
    pokopia_vk_texel_close(&textures[0].texel);
    pokopia_vk_texel_close(&textures[1].texel);
    pokopia_vk_program_destroy(programs[0]);
    pokopia_vk_program_destroy(programs[1]);
    nvn_shader_cache_close(cache);
    pokopia_vk_close(&adapter);
    return 0;
}

VkFormat nvn_guest_texture_vk_format(uint32_t format) { (void)format; abort(); }
uint32_t nvn_guest_buffer_texel_bytes(uint32_t format) { (void)format; abort(); }
bool nvn_guest_texture_layout(const NvnFrontTextureConfig *config, uint64_t *size, uint64_t *alignment)
{ (void)config; (void)size; (void)alignment; abort(); }

