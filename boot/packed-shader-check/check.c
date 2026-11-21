/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "shader_cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TEST_VULKAN
#include "vk_adapter.h"

static PokopiaVkAdapter adapter;

static void check_vulkan(const NvnCachedShader *shader)
{
    PFN_vkCreateShaderModule create = (PFN_vkCreateShaderModule)
        adapter.get_device_proc(adapter.device, "vkCreateShaderModule");
    PFN_vkDestroyShaderModule destroy = (PFN_vkDestroyShaderModule)
        adapter.get_device_proc(adapter.device, "vkDestroyShaderModule");
    assert(create && destroy);
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader->word_count * sizeof *shader->words,
        .pCode = shader->words,
    };
    VkShaderModule module;
    assert(create(adapter.device, &info, NULL, &module) == VK_SUCCESS);
    destroy(adapter.device, module, NULL);
}
#endif

static uint8_t *read_fixture(const char *root, const char *name, size_t *size)
{
    char path[2048];
    int length = snprintf(path, sizeof path, "%s/%s", root, name);
    assert(length > 0 && (size_t)length < sizeof path);
#ifdef _WIN32
    FILE *file = NULL;
    assert(fopen_s(&file, path, "rb") == 0);
#else
    FILE *file = fopen(path, "rb");
#endif
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long bytes = ftell(file);
    assert(bytes > 0 && fseek(file, 0, SEEK_SET) == 0);
    *size = (size_t)bytes;
    uint8_t *buffer = malloc(*size);
    assert(buffer && fread(buffer, 1, *size, file) == *size);
    fclose(file);
    return buffer;
}

static void check_fixture(NvnShaderCache *cache, const char *root, const char *code_path,
                          const char *control_path, const char *hash, NvnShaderStage stage,
                          uint32_t expected_code_size, size_t spirv_bytes, const char *resources)
{
    size_t code_size, control_size;
    uint8_t *code = read_fixture(root, code_path, &code_size);
    uint8_t *control = read_fixture(root, control_path, &control_size);
    NvnShaderControl info;
    assert(!nvn_shader_control_info(control, NVN_SHADER_CONTROL_PREFIX_BYTES - 1, &info));
    assert(nvn_shader_control_info(control, NVN_SHADER_CONTROL_PREFIX_BYTES, &info));
    assert(info.code_bytes == expected_code_size && info.stage == stage);

    NvnCachedShader shader = {0};
    assert(nvn_shader_cache_lookup(cache, code, expected_code_size - 1, &info, &shader) ==
           NVN_SHADER_CACHE_INVALID_CONTROL);
    assert(nvn_shader_cache_lookup(cache, code, code_size, &info, &shader) == NVN_SHADER_CACHE_OK);
    assert(!strcmp(shader.hash, hash) && !strcmp(shader.entry, "main"));
    assert(shader.stage == stage && shader.word_count * sizeof *shader.words == spirv_bytes);
    assert(!strncmp(shader.resources, resources, strlen(resources)));
#ifdef TEST_VULKAN
    check_vulkan(&shader);
#endif
    nvn_cached_shader_release(&shader);

    info.stage = stage == NVN_SHADER_VERTEX ? NVN_SHADER_FRAGMENT : NVN_SHADER_VERTEX;
    assert(nvn_shader_cache_lookup(cache, code, code_size, &info, &shader) == NVN_SHADER_CACHE_INVALID_CACHE);
    assert(!strcmp(shader.hash, hash) && !shader.words && !shader.resources);
    nvn_cached_shader_release(&shader);
    free(control);
    free(code);
}

static NvnShaderCache *open_case(const char *root, const char *scenario, NvnShaderCacheStatus expected)
{
    char directory[2048];
    int length = snprintf(directory, sizeof directory, "%s/%s", root, scenario);
    assert(length > 0 && (size_t)length < sizeof directory);
    NvnShaderCacheStatus status;
    NvnShaderCache *cache = nvn_shader_cache_open(directory, &status);
    assert(status == expected && (!!cache == (expected == NVN_SHADER_CACHE_OK)));
    return cache;
}

static void compare_fixture(NvnShaderCache *legacy, NvnShaderCache *packed, const char *root,
                            const char *key, NvnShaderStage stage)
{
    char name[40];
    size_t code_size;
    snprintf(name, sizeof name, "code_%s.bin", key);
    uint8_t *code = read_fixture(root, name, &code_size);
    NvnShaderControl control = {.code_bytes = (uint32_t)code_size, .stage = stage};
    NvnCachedShader first = {0}, second = {0};
    assert(nvn_shader_cache_lookup(legacy, code, code_size, &control, &first) == NVN_SHADER_CACHE_OK);
    assert(nvn_shader_cache_lookup(packed, code, code_size, &control, &second) == NVN_SHADER_CACHE_OK);
    assert(!strcmp(first.hash, second.hash) && !strcmp(first.entry, second.entry) &&
           !strcmp(first.resources, second.resources) && first.stage == second.stage &&
           first.word_count == second.word_count &&
           !memcmp(first.words, second.words, first.word_count * sizeof *first.words));
    NvnCachedShader metadata = {0}, by_key = {0};
    assert(nvn_shader_cache_find(packed, code, code_size, &control, &metadata) == NVN_SHADER_CACHE_OK);
    assert(metadata.stage == stage && !strcmp(metadata.hash, key) &&
           !strcmp(metadata.resources, first.resources) && !metadata.words && !metadata.word_count && !metadata.entry[0]);
    memcpy(by_key.hash, metadata.hash, sizeof by_key.hash);
    assert(nvn_shader_cache_lookup_key(packed, by_key.hash, stage, &by_key) == NVN_SHADER_CACHE_OK);
    assert(by_key.stage == stage && !strcmp(by_key.hash, key) && !strcmp(by_key.entry, first.entry) &&
           !strcmp(by_key.resources, first.resources) && by_key.word_count == first.word_count &&
           !memcmp(by_key.words, first.words, first.word_count * sizeof *first.words));
    nvn_cached_shader_release(&metadata);
    nvn_cached_shader_release(&by_key);
    nvn_cached_shader_release(&first);
    nvn_cached_shader_release(&second);
    free(code);
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    NvnShaderCacheStatus status;
    NvnShaderCache *legacy = nvn_shader_cache_open(argv[1], &status);
    assert(legacy && status == NVN_SHADER_CACHE_OK && nvn_shader_cache_count(legacy) == 3);
    NvnShaderCache *packed = open_case(argv[3], "valid", NVN_SHADER_CACHE_OK);
    assert(nvn_shader_cache_count(packed) == 4);
    assert(pokopia_vk_open(&adapter, stdout));
    check_fixture(packed, argv[2], "code_f48bbb163f22a70a.bin", "ctrl_f48bbb163f22a70a.bin",
                  "f48bbb163f22a70a", NVN_SHADER_VERTEX, 0x300, 2624, "cb3=s0b4");
    check_fixture(packed, argv[2], "code_b3ac194a334f246a.bin", "ctrl_b3ac194a334f246a.bin",
                  "b3ac194a334f246a", NVN_SHADER_VERTEX, 0x400, 3916, "cb3=s0b4");
    check_fixture(packed, argv[2], "code_358f044f947e710a.bin", "ctrl_358f044f947e710a.bin",
                  "358f044f947e710a", NVN_SHADER_FRAGMENT, 0x300, 2932, "texTexture2D[c-1:8]=s2b128");
    compare_fixture(legacy, packed, argv[2], "f48bbb163f22a70a", NVN_SHADER_VERTEX);
    compare_fixture(legacy, packed, argv[2], "b3ac194a334f246a", NVN_SHADER_VERTEX);
    compare_fixture(legacy, packed, argv[2], "358f044f947e710a", NVN_SHADER_FRAGMENT);
    NvnCachedShader shader = {0};
    NvnShaderControl control = {.code_bytes = 3, .stage = NVN_SHADER_VERTEX};
    assert(nvn_shader_cache_lookup(packed, "abc", 3, &control, &shader) == NVN_SHADER_CACHE_MISSING);
    assert(!strcmp(shader.hash, "a9993e364706816a") && !shader.words && !shader.resources);
    assert(nvn_shader_cache_lookup_key(packed, "f48bbb163f22a70a", NVN_SHADER_FRAGMENT, &shader) ==
           NVN_SHADER_CACHE_INVALID_CACHE);
    assert(nvn_shader_cache_lookup_key(packed, "F48bbb163f22a70a", NVN_SHADER_VERTEX, &shader) ==
           NVN_SHADER_CACHE_INVALID_CONTROL && !shader.hash[0]);
    assert(nvn_shader_cache_lookup_key(packed, "f48bbb163f22a70", NVN_SHADER_VERTEX, &shader) ==
           NVN_SHADER_CACHE_INVALID_CONTROL && !shader.hash[0]);
    assert(nvn_shader_cache_lookup_key(packed, "f48bbb163f22a70a0", NVN_SHADER_VERTEX, &shader) ==
           NVN_SHADER_CACHE_INVALID_CONTROL && !shader.hash[0]);
    assert(nvn_shader_cache_lookup_key(packed, "f48bbb163f22a70a", (NvnShaderStage)7, &shader) ==
           NVN_SHADER_CACHE_INVALID_CONTROL && !shader.hash[0]);
    assert(nvn_shader_cache_lookup_key(packed, "0123456789abcdef", NVN_SHADER_VERTEX, &shader) ==
           NVN_SHADER_CACHE_MISSING && !strcmp(shader.hash, "0123456789abcdef"));
    nvn_shader_cache_close(packed);
    nvn_shader_cache_close(legacy);
    pokopia_vk_close(&adapter);
    puts("PASS packed shader metadata and lookup-by-key match legacy bytes and create Vulkan modules");

    open_case(argv[3], "missing-pack", NVN_SHADER_CACHE_INVALID_CACHE);
    open_case(argv[3], "unaligned-pack", NVN_SHADER_CACHE_INVALID_CACHE);
    size_t code_size;
    uint8_t *code = read_fixture(argv[2], "code_f48bbb163f22a70a.bin", &code_size);
    control.code_bytes = (uint32_t)code_size;
    static const char *const invalid[] = {
        "offset-blank", "offset-negative", "offset-unaligned", "offset-out-of-range",
        "offset-overflow", "length-unaligned", "length-overflow", "truncated", "bad-spirv",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        packed = open_case(argv[3], invalid[i], NVN_SHADER_CACHE_OK);
        const bool payload_only = !strcmp(invalid[i], "bad-spirv");
        assert(nvn_shader_cache_find(packed, code, code_size, &control, &shader) ==
               (payload_only ? NVN_SHADER_CACHE_OK : NVN_SHADER_CACHE_INVALID_CACHE));
        assert(!shader.words && !shader.word_count && !shader.entry[0]);
        nvn_cached_shader_release(&shader);
        assert(nvn_shader_cache_lookup(packed, code, code_size, &control, &shader) ==
               NVN_SHADER_CACHE_INVALID_CACHE);
        assert(!strcmp(shader.hash, "f48bbb163f22a70a") && !shader.words && !shader.resources && !shader.entry[0]);
        nvn_shader_cache_close(packed);
    }
    free(code);
    puts("PASS missing, malformed, unaligned, overflowing, truncated, and bad SPIR-V cases");
    return 0;
}
