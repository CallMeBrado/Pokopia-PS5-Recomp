#include "live_compute.h"
#include "nvn_guest_layout.h"

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static void check(VkResult result, const char *where)
{
    if (result == VK_SUCCESS)
        return;
    fprintf(stderr, "%s failed with VkResult %d\n", where, (int)result);
    exit(EXIT_FAILURE);
}
#define CHECK(call) check((call), #call)

static void *read_file(const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    assert(f && fseek(f, 0, SEEK_END) == 0);
    const long length = ftell(f);
    assert(length > 0 && fseek(f, 0, SEEK_SET) == 0);
    void *data = malloc((size_t)length);
    assert(data && fread(data, 1, (size_t)length, f) == (size_t)length);
    fclose(f);
    *bytes = (size_t)length;
    return data;
}

static VkShaderModule load(PokopiaVkAdapter *adapter, const char *key)
{
    char path[128];
    int len = snprintf(path, sizeof path, "../../frame-reference-655360/shader_cache/spirv/%s.spv", key);
    assert(len > 0 && len < sizeof path);
    size_t bytes;
    void *code = read_file(path, &bytes);
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes, .pCode = code,
    };
    PFN_vkCreateShaderModule create = (PFN_vkCreateShaderModule)
        adapter->get_device_proc(adapter->device, "vkCreateShaderModule");
    VkShaderModule module;
    CHECK(create(adapter->device, &info, NULL, &module));
    free(code);
    return module;
}

static void open_texel(PokopiaVkAdapter *adapter, PokopiaVkTexel *out, uint32_t count)
{
    VkFormatProperties caps;
    const VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
    adapter->format_properties(adapter->physical, format, &caps);
    CHECK(pokopia_vk_texel_open(out, adapter->device, adapter->get_device_proc, &adapter->memory,
                               caps.bufferFeatures, format, count, 16, adapter->properties.limits.maxTexelBufferElements));
    void *zeros = calloc(count, 16);
    assert(zeros);
    CHECK(pokopia_vk_texel_refresh(out, zeros, (size_t)count * 16));
    free(zeros);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    PokopiaVkAdapter adapter = {0};
    assert(pokopia_vk_open(&adapter, stdout));
    const char *first_key = "082274fc8b17c887", *clustered_key = "b81c48ff486acef4";
    const VkShaderModule first = load(&adapter, first_key), clustered = load(&adapter, clustered_key);
    size_t first_ubo_bytes, clustered_ubo_bytes;
    uint32_t *first_ubo = read_file("uniform1.bin", &first_ubo_bytes);
    uint32_t *clustered_ubo = read_file("uniform2.bin", &clustered_ubo_bytes);

    uint32_t counter = 0x12345678;
    PokopiaLiveCompute first_dispatch = {
        .module = first, .entry = "main", .hash = first_key,
        .uniform = first_ubo, .uniform_bytes = first_ubo_bytes, .groups = {1, 1, 1},
        .storage = {{&counter, sizeof counter}},
    };
    CHECK(pokopia_live_compute(&adapter, &first_dispatch));
    assert(counter == 0x00003fc0);
    first_ubo[0] = 0x81234567;
    CHECK(pokopia_live_compute(&adapter, &first_dispatch));
    assert(counter == 0x81234567);
    first_ubo[0] = 0x00003fc0;
    CHECK(pokopia_live_compute(&adapter, &first_dispatch));
    printf("PASS 1x1x1 NVN dispatch wrote traced and changed UBO values to canonical counter (%#x)\n", counter);

    PokopiaVkTexel texels[2] = {0};
    open_texel(&adapter, &texels[0], 1024);
    open_texel(&adapter, &texels[1], 64);
    const size_t output_bytes = 4065280;
    uint32_t *output = calloc(output_bytes, 1);
    assert(output);
    memset(output, 0xa5, output_bytes);
    PokopiaLiveCompute clustered_dispatch = {
        .module = clustered, .entry = "main", .hash = clustered_key,
        .uniform = clustered_ubo, .uniform_bytes = clustered_ubo_bytes, .groups = {30, 17, 16},
        .storage = {{&counter, sizeof counter}, {output, output_bytes}},
        .texels = {&texels[0], &texels[1]},
    };
    CHECK(pokopia_live_compute(&adapter, &clustered_dispatch));
    size_t nonzero = 0, changed = 0;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < output_bytes / 4; ++i) {
        nonzero += output[i] != 0;
        changed += output[i] != 0xa5a5a5a5;
        hash = (hash ^ output[i]) * 16777619u;
    }
    printf("Clustered 30x17x16: counter=%#x, SSBO nonzero dwords=%zu, changed=%zu, word hash=%#x\n", counter, nonzero, changed, hash);
    assert(counter == 0x3fc0 && changed > 0);

    /* Enable one point light without changing anything else between the
     * two dispatches. A larger radius must change the GPU's light list. */
    clustered_ubo[12] = 1;
    float point_data[1024 * 4] = {0};
    uint32_t counters[2];
    for (unsigned i = 0; i < 2; ++i) {
        point_data[2] = -1000.0f;
        point_data[3] = i ? 100000.0f : 0.0f;
        CHECK(pokopia_vk_texel_refresh(&texels[0], point_data, sizeof point_data));
        memset(output, 0, output_bytes);
        counter = 0x3fc0;
        CHECK(pokopia_live_compute(&adapter, &clustered_dispatch));
        counters[i] = counter;
        printf("Point light radius %.0f: counter=%#x\n", point_data[3], counter);
    }
    assert(counters[0] != counters[1]);
    puts("PASS changing one RGBA32F texel changes the GPU's light list allocation");

    const uint32_t previous = counter;
    clustered_dispatch.texels[1] = NULL;
    assert(pokopia_live_compute(&adapter, &clustered_dispatch) == VK_ERROR_FEATURE_NOT_PRESENT);
    assert(counter == previous);
    puts("PASS missing texel binding is rejected without guest writes");

    const struct { uint32_t width, height, x, y; size_t bytes; } modes[] = {
        {1280, 720, 20, 12, 4030720},
        {1600, 900, 25, 15, 4048000},
    };
    clustered_ubo[12] = 0;
    for (size_t mode = 0; mode < sizeof modes / sizeof modes[0]; ++mode) {
        const uint32_t header = 2u * modes[mode].x * modes[mode].y * 16u;
        first_ubo[0] = header;
        CHECK(pokopia_live_compute(&adapter, &first_dispatch));
        assert(counter == header);
        clustered_ubo[14] = modes[mode].width;
        clustered_ubo[15] = modes[mode].height;
        clustered_dispatch.groups[0] = modes[mode].x;
        clustered_dispatch.groups[1] = modes[mode].y;
        clustered_dispatch.storage[1].bytes = modes[mode].bytes;
        clustered_dispatch.texels[1] = &texels[1];
        memset(output, 0xa5, output_bytes);
        CHECK(pokopia_live_compute(&adapter, &clustered_dispatch));
        size_t changed = 0;
        for (uint32_t i = 0; i < header; ++i)
            changed += output[i] != UINT32_C(0xa5a5a5a5);
        assert(changed == header && counter == header);
        assert(output[modes[mode].bytes / 4] == UINT32_C(0xa5a5a5a5));

        const uint32_t first_word = output[0];
        clustered_dispatch.storage[1].bytes -= 4;
        assert(pokopia_live_compute(&adapter, &clustered_dispatch) == VK_ERROR_FEATURE_NOT_PRESENT);
        clustered_dispatch.storage[1].bytes += 4;
        clustered_ubo[14] = 1920;
        assert(pokopia_live_compute(&adapter, &clustered_dispatch) == VK_ERROR_FEATURE_NOT_PRESENT);
        assert(counter == header && output[0] == first_word);
        printf("PASS %ux%u cluster grid %ux%ux16, %zu bytes, %zu header words written\n",
               modes[mode].width, modes[mode].height, modes[mode].x, modes[mode].y, modes[mode].bytes, changed);
    }
    free(first_ubo);
    free(clustered_ubo);
    free(output);
    pokopia_vk_texel_close(&texels[0]);
    pokopia_vk_texel_close(&texels[1]);
    PFN_vkDestroyShaderModule destroy = (PFN_vkDestroyShaderModule)
        adapter.get_device_proc(adapter.device, "vkDestroyShaderModule");
    destroy(adapter.device, first, NULL);
    destroy(adapter.device, clustered, NULL);
    pokopia_vk_close(&adapter);
    return 0;
}

VkFormat nvn_guest_texture_vk_format(uint32_t format) { (void)format; abort(); }
uint32_t nvn_guest_buffer_texel_bytes(uint32_t format) { (void)format; abort(); }

