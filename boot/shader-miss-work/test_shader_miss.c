#define nvn_front_get_program mock_get_program
#define nvn_front_get_texture mock_get_texture
#define nvn_front_get_command_buffer mock_get_command_buffer
#define nvn_front_charge_command_memory mock_charge_command_memory
#define nvn_front_resolve_pool mock_resolve_pool
#include "nvn_replay.c"

#include <assert.h>

enum { ENCODER = 100, PROGRAM = 200, DEVICE = 300, COMMAND_HANDLE = 400, HANDLE_ARRAY = 500, TARGET = 600,
       COPY_SOURCE = 700, COPY_DEST = 800 };

static NvnFrontProgramInfo program_info;
static PokopiaVkTexture target = {.aspects = VK_IMAGE_ASPECT_COLOR_BIT};
static uint8_t encoder[0xa9];
static const uint64_t handles[] = {COMMAND_HANDLE};
static uint32_t source = 0x12345678, dest = 0x87654321;
static unsigned copies, dispatches, modules;
static size_t command_bytes, control_bytes;

bool nvn_front_get_program(NvnFront *front, uint64_t address, NvnFrontProgramInfo *program)
{
    (void)front;
    if (address != PROGRAM)
        return false;
    *program = program_info;
    return true;
}

bool nvn_front_get_texture(NvnFront *front, uint64_t address, NvnFrontTextureInfo *info)
{
    (void)front;
    if (address != TARGET)
        return false;
    *info = (NvnFrontTextureInfo){.native = &target,
        .create_info.config = {.device = DEVICE, .target = 1, .depth = 1, .levels = 1}};
    return true;
}

bool nvn_front_get_command_buffer(NvnFront *front, uint64_t address, NvnFrontCommandBufferInfo *info)
{
    (void)front;
    if (address != ENCODER)
        return false;
    *info = (NvnFrontCommandBufferInfo){.device = DEVICE};
    return true;
}

bool nvn_front_charge_command_memory(NvnFront *front, uint64_t address, size_t command, size_t control)
{
    (void)front;
    assert(address == ENCODER);
    command_bytes += command;
    control_bytes += control;
    return true;
}

bool nvn_front_resolve_pool(const NvnFront *front, uint64_t device, uint64_t address, size_t bytes, NvnFrontPoolHit *hit)
{
    (void)front;
    (void)hit;
    return device == DEVICE && (address == COPY_SOURCE || address == COPY_DEST) && bytes == sizeof source;
}

static const void *read_guest(void *opaque, uint64_t address, size_t bytes)
{
    (void)opaque;
    if (address == ENCODER && bytes == sizeof encoder)
        return encoder;
    if (address == HANDLE_ARRAY && bytes == sizeof handles)
        return handles;
    if (address == COPY_SOURCE && bytes == sizeof source)
        return &source;
    return NULL;
}

static void *write_guest(void *opaque, uint64_t address, size_t bytes)
{
    (void)opaque;
    return address == COPY_DEST && bytes == sizeof dest ? &dest : NULL;
}

static bool do_copy(void *opaque, const void *src, void *dst, size_t bytes)
{
    (void)opaque;
    ++copies;
    memcpy(dst, src, bytes);
    return true;
}

static bool do_compute(void *opaque, const PokopiaLiveCompute *compute)
{
    (void)opaque;
    (void)compute;
    ++dispatches;
    return true;
}

static VKAPI_ATTR VkResult VKAPI_CALL create_module(VkDevice device, const VkShaderModuleCreateInfo *info,
                                                     const VkAllocationCallbacks *alloc, VkShaderModule *module)
{
    (void)device;
    (void)alloc;
    assert(info->codeSize >= 20 && info->pCode[0] == 0x07230203);
    ++modules;
    *module = (VkShaderModule)(uintptr_t)1;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL destroy_module(VkDevice device, VkShaderModule module, const VkAllocationCallbacks *alloc)
{
    (void)device;
    (void)alloc;
    assert(module == (VkShaderModule)(uintptr_t)1);
    --modules;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_proc(VkDevice device, const char *name)
{
    (void)device;
    if (!strcmp(name, "vkCreateShaderModule"))
        return (PFN_vkVoidFunction)create_module;
    assert(!strcmp(name, "vkDestroyShaderModule"));
    return (PFN_vkVoidFunction)destroy_module;
}

static uint8_t *read_file(const char *type, const char *key, size_t *bytes)
{
    char path[128];
    const int length = snprintf(path, sizeof path, "../../frame-reference-655360/shader_cache/missing/%s_%s.bin", type, key);
    assert(length > 0 && (size_t)length < sizeof path);
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    const long count = ftell(file);
    assert(count > 0 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *data = malloc((size_t)count);
    assert(data && fread(data, 1, (size_t)count, file) == (size_t)count);
    fclose(file);
    *bytes = (size_t)count;
    return data;
}

static NvnFrontShaderData shader(const char *key, NvnShaderStage stage)
{
    size_t control_bytes, code_bytes;
    NvnFrontShaderData out = {.code = read_file("code", key, &code_bytes),
                              .control = read_file("ctrl", key, &control_bytes)};
    assert(nvn_shader_control_info(out.control, control_bytes, &out.parsed));
    assert(out.parsed.stage == stage && out.parsed.code_bytes == code_bytes);
    return out;
}

int main(void)
{
    const char *vs = "f48bbb163f22a70a", *fs = "f53c9e06f6248658", *cs = "3a718dcc49952e74";
    NvnShaderCacheStatus status;
    NvnShaderCache *cache = nvn_shader_cache_open("../shader-cache-655360", &status);
    assert(cache && status == NVN_SHADER_CACHE_OK);
    PokopiaVkAdapter adapter = {.device = (VkDevice)(uintptr_t)1, .get_device_proc = get_proc};
    PokopiaVkProgram *program = pokopia_vk_program_create(&adapter);
    assert(program);
    FILE *log = fopen("fixture.log", "w+");
    assert(log);

    NvnFrontShaderData pair[] = {shader(vs, NVN_SHADER_VERTEX), shader(fs, NVN_SHADER_FRAGMENT)};
    assert(pokopia_vk_program_link(program, cache, pair, 2, log) == NVN_FRONT_PROGRAM_CACHE_MISS);
    PokopiaVkProgramStage metadata;
    assert(pokopia_vk_program_stage(program, NVN_SHADER_VERTEX, &metadata) && metadata.resources &&
           !strcmp(metadata.hash, vs));
    assert(pokopia_vk_program_stage(program, NVN_SHADER_FRAGMENT, &metadata) && !metadata.resources &&
           !strcmp(metadata.hash, fs));
    assert(!modules);
    PokopiaVkProgramModule module = {0};
    assert(pokopia_vk_program_acquire_module(program, NVN_SHADER_VERTEX, &module) && modules == 1);
    pokopia_vk_program_release_module(program, &module);
    assert(!modules);

    Recording recording = {.encoder = ENCODER, .device = DEVICE};
    NvnReplay replay = {.config = {.read_guest = read_guest, .write_guest = write_guest,
                                   .copy = do_copy, .compute = do_compute},
                        .recordings = &recording};
    program_info = (NvnFrontProgramInfo){.device = DEVICE, .native = program, .has_unavailable_shader = true};
    uint64_t x[32] = {ENCODER, PROGRAM, 3};
    assert(bind_program(&replay, x) == NVN_FRONT_HANDLED);
    assert(recording.program[NVN_SHADER_VERTEX] == PROGRAM && recording.program[NVN_SHADER_FRAGMENT] == PROGRAM);
    assert(command_bytes == 88 && !control_bytes);
    const uint64_t draw[32] = {ENCODER, 4, 1, 6};
    assert(record_draw(&replay, draw, true, false, false) == NVN_FRONT_BOUNDARY && !recording.count);
    x[1] = 0;
    assert(bind_program(&replay, x) == NVN_FRONT_HANDLED && !recording.program[NVN_SHADER_FRAGMENT]);
    x[1] = PROGRAM;
    assert(bind_program(&replay, x) == NVN_FRONT_HANDLED && recording.program[NVN_SHADER_FRAGMENT] == PROGRAM);

    Operation operations[] = {
        {.kind = OP_COPY, .data.copy = {COPY_SOURCE, COPY_DEST, sizeof source}},
        {.kind = OP_DRAW, .data.draw = {.target = TARGET, .program = {PROGRAM, PROGRAM}, .state.target = &target}},
    };
    recording.handle = COMMAND_HANDLE;
    recording.operations = operations;
    recording.count = 2;
    assert(submit(&replay, DEVICE, 1, HANDLE_ARRAY) == NVN_FRONT_BOUNDARY);
    assert(!copies && dest == 0x87654321 && !modules);
    recording.handle = 0;
    recording.operations = NULL;
    recording.count = 0;

    NvnFrontShaderData compute = shader(cs, NVN_SHADER_COMPUTE);
    assert(pokopia_vk_program_link(program, cache, &compute, 1, log) == NVN_FRONT_PROGRAM_CACHE_MISS);
    x[2] = 1u << NVN_SHADER_COMPUTE;
    assert(bind_program(&replay, x) == NVN_FRONT_HANDLED && recording.program[NVN_SHADER_COMPUTE] == PROGRAM);
    assert(control_bytes == 48);
    const uint64_t dispatch[32] = {ENCODER, 1, 1, 1};
    assert(record_dispatch(&replay, dispatch) == NVN_FRONT_BOUNDARY && !recording.count && !dispatches);

    assert(pokopia_vk_program_link(program, cache, pair, 1, log) == NVN_FRONT_PROGRAM_LINKED);
    assert(pokopia_vk_program_stage(program, NVN_SHADER_VERTEX, &metadata) && metadata.resources);
    assert(!pokopia_vk_program_stage(program, NVN_SHADER_FRAGMENT, &metadata));
    assert(!pokopia_vk_program_stage(program, NVN_SHADER_COMPUTE, &metadata));
    assert(pokopia_vk_program_link(program, NULL, pair, 1, log) == NVN_FRONT_PROGRAM_UNSUPPORTED);
    assert(pokopia_vk_program_stage(program, NVN_SHADER_VERTEX, &metadata) && metadata.resources);

    pair[0].parsed.stage = NVN_SHADER_GEOMETRY;
    assert(pokopia_vk_program_link(program, cache, pair, 1, log) == NVN_FRONT_PROGRAM_FAILED);
    assert(!pokopia_vk_program_stage(program, NVN_SHADER_VERTEX, &metadata));
    rewind(log);
    char text[2048] = {0};
    assert(fread(text, 1, sizeof text - 1, log) && !ferror(log));
    assert(strstr(text, "NVN shader cache miss at use key=f53c9e06f6248658 stage=1"));
    assert(strstr(text, "NVN shader cache miss at use key=3a718dcc49952e74 stage=5"));
    fclose(log);
    free((void *)compute.code);
    free((void *)compute.control);
    for (unsigned i = 0; i < 2; ++i) {
        free((void *)pair[i].code);
        free((void *)pair[i].control);
    }
    pokopia_vk_program_destroy(program);
    nvn_shader_cache_close(cache);
    puts("PASS actual mixed cache registration, graphics/compute bind and rejection at use, no prior submit writes, relink and malformed metadata");
}


