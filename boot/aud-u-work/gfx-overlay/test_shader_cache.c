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

int main(int argc, char **argv)
{
    assert(argc == 3);
    const char *root = argv[1], *shader_work = argv[2];
    char directory[2048];
    int length = snprintf(directory, sizeof directory, "%s/spirv", root);
    assert(length > 0 && (size_t)length < sizeof directory);
    NvnShaderCacheStatus status;
    NvnShaderCache *primary = nvn_shader_cache_open(directory, &status);
    assert(primary && status == NVN_SHADER_CACHE_OK && nvn_shader_cache_count(primary) == 99012);
#ifdef TEST_VULKAN
    assert(pokopia_vk_open(&adapter, stdout));
#endif
    check_fixture(primary, shader_work, "corpus/rdb/e9e6defca60e9dd5-stage0-ca882d0bd9cb5df8.program.bin",
                  "corpus-ctl/71912c95f9c9e445.ctl", "caa3cba58abf8cc9", NVN_SHADER_VERTEX, 768, 3052,
                  "cb1=s0b2 cb5=s0b6");
    check_fixture(primary, shader_work, "corpus/rdb/188278fd5255d788-stage4-3c9930e54124c21d.program.bin",
                  "corpus-ctl/48a15ecf29169b6c.ctl", "b98b93ee98f5d316", NVN_SHADER_FRAGMENT, 9472, 98116,
                  "cb1=s0b20 cb5=s0b24 texTexture2D[c-1:12]=s2b128");

    static const char input[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const NvnShaderControl nonexistent = {.code_bytes = sizeof input - 1, .stage = NVN_SHADER_VERTEX};
    NvnCachedShader missing = {0};
    assert(nvn_shader_cache_lookup(primary, input, sizeof input - 1, &nonexistent, &missing) ==
           NVN_SHADER_CACHE_MISSING);
    assert(!strcmp(missing.hash, "84983e441c3bd26e") && !missing.words && !missing.resources);
    nvn_cached_shader_release(&missing);
    nvn_shader_cache_close(primary);

    length = snprintf(directory, sizeof directory, "%s/stage-missing", root);
    assert(length > 0 && (size_t)length < sizeof directory);
    NvnShaderCache *fallback = nvn_shader_cache_open(directory, &status);
    assert(fallback && status == NVN_SHADER_CACHE_OK && nvn_shader_cache_count(fallback) == 212);
    check_fixture(fallback, root, "missing/code_00f07fce7296eaa0.bin", "missing/ctrl_00f07fce7296eaa0.bin",
                  "00f07fce7296eaa0", NVN_SHADER_FRAGMENT, 3072, 20376, "cb3=s0b22");
    nvn_shader_cache_close(fallback);
#ifdef TEST_VULKAN
    pokopia_vk_close(&adapter);
    puts("PASS Vulkan created shader modules from all three exact cache hits");
#endif
    puts("PASS 99012 primary and 212 fallback cache records with real NVN2 control/code fixtures");
    return 0;
}
