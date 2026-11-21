/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vk_program.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    VkShaderModule module;
    char entry[256];
    char hash[17];
    char *resources;
} ProgramStage;

struct PokopiaVkProgram {
    VkDevice device;
    PFN_vkCreateShaderModule create;
    PFN_vkDestroyShaderModule destroy;
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

static void release_stages(PokopiaVkProgram *program, ProgramStage *stages)
{
    for (unsigned i = 0; i <= NVN_SHADER_COMPUTE; ++i) {
        if (stages[i].module)
            program->destroy(program->device, stages[i].module, NULL);
        free(stages[i].resources);
        stages[i] = (ProgramStage){0};
    }
}

void pokopia_vk_program_destroy(PokopiaVkProgram *program)
{
    if (!program)
        return;
    release_stages(program, program->stages);
    free(program);
}

bool pokopia_vk_program_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                             PokopiaVkProgramStage *info)
{
    if (!program || !info || (unsigned)stage > NVN_SHADER_COMPUTE || !program->stages[stage].module)
        return false;
    const ProgramStage *linked = &program->stages[stage];
    *info = (PokopiaVkProgramStage){linked->module, linked->entry, linked->hash, linked->resources};
    return true;
}

NvnFrontProgramLinkResult pokopia_vk_program_link(PokopiaVkProgram *program, const NvnShaderCache *cache,
    const NvnFrontShaderData *shaders, size_t count, FILE *log)
{
    if (!program || count > NVN_SHADER_COMPUTE + 1 || (count && !shaders))
        return NVN_FRONT_PROGRAM_FAILED;
    if (count && !cache)
        return NVN_FRONT_PROGRAM_UNSUPPORTED;

    ProgramStage linked[NVN_SHADER_COMPUTE + 1] = {0};
    NvnFrontProgramLinkResult outcome = NVN_FRONT_PROGRAM_FAILED;
    for (size_t i = 0; i < count; ++i) {
        const NvnFrontShaderData *shader = &shaders[i];
        if (shader->parsed.stage > NVN_SHADER_COMPUTE || linked[shader->parsed.stage].module)
            goto done;
        NvnCachedShader cached = {0};
        NvnShaderCacheStatus status = nvn_shader_cache_lookup(cache, shader->code, shader->parsed.code_bytes,
                                                              &shader->parsed, &cached);
        if (status != NVN_SHADER_CACHE_OK) {
            fprintf(log, "NVN shader cache result=%d key=%s stage=%u\n", status, cached.hash, (unsigned)shader->parsed.stage);
            nvn_cached_shader_release(&cached);
            if (status == NVN_SHADER_CACHE_MISSING)
                outcome = NVN_FRONT_PROGRAM_UNSUPPORTED;
            goto done;
        }
        const VkShaderModuleCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = cached.word_count * sizeof *cached.words, .pCode = cached.words,
        };
        VkShaderModule module;
        VkResult result = program->create(program->device, &info, NULL, &module);
        fprintf(log, "NVN Vulkan shader module key=%s stage=%u result=%d\n", cached.hash, (unsigned)cached.stage, result);
        if (result != VK_SUCCESS) {
            nvn_cached_shader_release(&cached);
            goto done;
        }
        ProgramStage *stage = &linked[shader->parsed.stage];
        stage->module = module;
        memcpy(stage->entry, cached.entry, sizeof stage->entry);
        memcpy(stage->hash, cached.hash, sizeof stage->hash);
        stage->resources = cached.resources;
        cached.resources = NULL;
        nvn_cached_shader_release(&cached);
    }
    outcome = NVN_FRONT_PROGRAM_LINKED;

done:
    if (outcome != NVN_FRONT_PROGRAM_UNSUPPORTED)
        release_stages(program, program->stages);
    if (outcome == NVN_FRONT_PROGRAM_LINKED)
        memcpy(program->stages, linked, sizeof linked);
    else
        release_stages(program, linked);
    fflush(log);
    return outcome;
}
