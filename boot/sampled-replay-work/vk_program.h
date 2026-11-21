/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_VK_PROGRAM_H
#define POKOPIA_VK_PROGRAM_H

#include "vk_adapter.h"

typedef struct PokopiaVkProgram PokopiaVkProgram;

typedef struct {
    VkShaderModule module;
    const char *entry, *hash, *resources;
} PokopiaVkProgramStage;

PokopiaVkProgram *pokopia_vk_program_create(PokopiaVkAdapter *adapter);
void pokopia_vk_program_destroy(PokopiaVkProgram *program);
bool pokopia_vk_program_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                             PokopiaVkProgramStage *info);
NvnFrontProgramLinkResult pokopia_vk_program_link(PokopiaVkProgram *program, const NvnShaderCache *cache,
    const NvnFrontShaderData *shaders, size_t count, FILE *log);

#endif
