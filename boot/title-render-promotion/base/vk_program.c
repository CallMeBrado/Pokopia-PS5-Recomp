/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vk_program.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const NvnShaderCache *cache;
    char hash[17];
    char *resources;
} ProgramStage;

struct PokopiaVkProgram {
    VkDevice device;
    PFN_vkCreateShaderModule create;
    PFN_vkDestroyShaderModule destroy;
    FILE *log;
    ProgramStage stages[NVN_SHADER_COMPUTE + 1];
};

PokopiaVkProgram *pokopia_vk_program_create(PokopiaVkAdapter *adapter)
{
    if (!adapter->device)
        return NULL;
    VkDevice device = adapter->device;
    PFN_vkCreateShaderModule create = (PFN_vkCreateShaderModule)adapter->get_device_proc(device, "vkCreateShaderModule");
    PFN_vkDestroyShaderModule destroy = (PFN_vkDestroyShaderModule)adapter->get_device_proc(device, "vkDestroyShaderModule");
    if (!create || !destroy)
        return NULL;
    PokopiaVkProgram *program = calloc(1, sizeof *program);
    if (program) {
        program->device = device;
        program->create = create;
        program->destroy = destroy;
    }
    return program;
}

static void release_stages(ProgramStage *stages)
{
    for (unsigned i = 0; i <= NVN_SHADER_COMPUTE; ++i) {
        free(stages[i].resources);
        stages[i] = (ProgramStage){0};
    }
}

void pokopia_vk_program_destroy(PokopiaVkProgram *program)
{
    if (!program)
        return;
    release_stages(program->stages);
    free(program);
}

bool pokopia_vk_program_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                             PokopiaVkProgramStage *info)
{
    if (!program || !info || (unsigned)stage > NVN_SHADER_COMPUTE || !program->stages[stage].hash[0])
        return false;
    const ProgramStage *linked = &program->stages[stage];
    *info = (PokopiaVkProgramStage){linked->hash, linked->resources};
    return true;
}

bool pokopia_vk_program_require_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                                     PokopiaVkProgramStage *info)
{
    if (!pokopia_vk_program_stage(program, stage, info))
        return false;
    if (info->resources)
        return true;
    if (program->log) {
        fprintf(program->log, "NVN shader cache miss at use key=%s stage=%u\n", info->hash, (unsigned)stage);
        fflush(program->log);
    }
    return false;
}

bool pokopia_vk_program_acquire_module(const PokopiaVkProgram *program, NvnShaderStage stage,
                                      PokopiaVkProgramModule *out)
{
    if (!program || !out || out->module || (unsigned)stage > NVN_SHADER_COMPUTE)
        return false;
    PokopiaVkProgramStage info;
    if (!pokopia_vk_program_require_stage(program, stage, &info))
        return false;
    const ProgramStage *linked = &program->stages[stage];
    NvnCachedShader cached = {0};
    NvnShaderCacheStatus status = nvn_shader_cache_lookup_key(linked->cache, linked->hash, stage, &cached);
    if (status != NVN_SHADER_CACHE_OK) {
        fprintf(program->log, "NVN shader cache result=%d key=%s stage=%u\n", status, linked->hash, (unsigned)stage);
        fflush(program->log);
        nvn_cached_shader_release(&cached);
        return false;
    }
    const VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = cached.word_count * sizeof *cached.words, .pCode = cached.words,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult result = program->create(program->device, &create_info, NULL, &module);
    if (result == VK_SUCCESS) {
        out->module = module;
        memcpy(out->entry, cached.entry, sizeof out->entry);
    } else {
        fprintf(program->log, "NVN Vulkan shader module key=%s stage=%u result=%d\n",
                cached.hash, (unsigned)cached.stage, (int)result);
        fflush(program->log);
    }
    nvn_cached_shader_release(&cached);
    return result == VK_SUCCESS;
}

void pokopia_vk_program_release_module(const PokopiaVkProgram *program, PokopiaVkProgramModule *module)
{
    if (!module->module)
        return;
    program->destroy(program->device, module->module, NULL);
    *module = (PokopiaVkProgramModule){0};
}

NvnFrontProgramLinkResult pokopia_vk_program_link(PokopiaVkProgram *program, const NvnShaderCache *cache,
    const NvnFrontShaderData *shaders, size_t count, FILE *log)
{
    if (!program || count > NVN_SHADER_COMPUTE + 1 || (count && !shaders))
        return NVN_FRONT_PROGRAM_FAILED;
    if (count && !cache)
        return NVN_FRONT_PROGRAM_UNSUPPORTED;
    program->log = log;

    ProgramStage linked[NVN_SHADER_COMPUTE + 1] = {0};
    NvnFrontProgramLinkResult outcome = NVN_FRONT_PROGRAM_FAILED;
    bool incomplete = false;
    for (size_t i = 0; i < count; ++i) {
        const NvnFrontShaderData *shader = &shaders[i];
        if ((unsigned)shader->parsed.stage > NVN_SHADER_COMPUTE || linked[shader->parsed.stage].hash[0])
            goto done;
        NvnCachedShader cached = {0};
        NvnShaderCacheStatus status = nvn_shader_cache_find(cache, shader->code, shader->parsed.code_bytes,
                                                            &shader->parsed, &cached);
        if (status != NVN_SHADER_CACHE_OK && log)
            fprintf(log, "NVN shader cache result=%d key=%s stage=%u\n", status, cached.hash, (unsigned)shader->parsed.stage);
        if (status != NVN_SHADER_CACHE_OK && status != NVN_SHADER_CACHE_MISSING) {
            nvn_cached_shader_release(&cached);
            goto done;
        }
        ProgramStage *stage = &linked[shader->parsed.stage];
        if (status == NVN_SHADER_CACHE_OK)
            stage->cache = cache;
        else
            incomplete = true;
        memcpy(stage->hash, cached.hash, sizeof stage->hash);
        stage->resources = cached.resources;
        cached.resources = NULL;
        nvn_cached_shader_release(&cached);
    }
    outcome = incomplete ? NVN_FRONT_PROGRAM_CACHE_MISS : NVN_FRONT_PROGRAM_LINKED;

done:
    release_stages(program->stages);
    if (outcome != NVN_FRONT_PROGRAM_FAILED)
        memcpy(program->stages, linked, sizeof linked);
    else
        release_stages(linked);
    if (log)
        fflush(log);
    return outcome;
}
