/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_VK_PROGRAM_H
#define POKOPIA_VK_PROGRAM_H

#include "vk_adapter.h"

typedef struct PokopiaVkProgram PokopiaVkProgram;

typedef struct {
    const char *hash, *resources;
    /* R66: the shader's embedded constant section, which NVN binds as hardware c[1]. */
    const void *constants;
    size_t constants_bytes;
} PokopiaVkProgramStage;

typedef struct {
    VkShaderModule module;
    char entry[256];
    bool cached; /* R67: owned by the program; release only forgets it */
} PokopiaVkProgramModule;

PokopiaVkProgram *pokopia_vk_program_create(PokopiaVkAdapter *adapter);
void pokopia_vk_program_destroy(PokopiaVkProgram *program);
/* Registered stages retain their hash. Resources are null on a cache miss. */
bool pokopia_vk_program_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                             PokopiaVkProgramStage *info);
bool pokopia_vk_program_require_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                                     PokopiaVkProgramStage *info);
bool pokopia_vk_program_acquire_module(const PokopiaVkProgram *program, NvnShaderStage stage,
                                      PokopiaVkProgramModule *out);
void pokopia_vk_program_release_module(const PokopiaVkProgram *program, PokopiaVkProgramModule *module);
NvnFrontProgramLinkResult pokopia_vk_program_link(PokopiaVkProgram *program, const NvnShaderCache *cache,
    const NvnFrontShaderData *shaders, size_t count, FILE *log);

#endif
