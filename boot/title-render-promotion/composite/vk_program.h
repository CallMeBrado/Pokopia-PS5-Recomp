/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_VK_PROGRAM_H
#define POKOPIA_VK_PROGRAM_H

#include "vk_adapter.h"

typedef struct PokopiaVkProgram PokopiaVkProgram;

typedef struct {
    const char *hash, *resources;
} PokopiaVkProgramStage;

enum { POKOPIA_COMPOSITE_C1_BYTES = 0x30 };
#define POKOPIA_COMPOSITE_VERTEX "caa3cba58abf8cc9"
#define POKOPIA_COMPOSITE_FRAGMENT "8526214507533768"

typedef struct {
    VkShaderModule module;
    char entry[256];
} PokopiaVkProgramModule;

PokopiaVkProgram *pokopia_vk_program_create(PokopiaVkAdapter *adapter);
void pokopia_vk_program_destroy(PokopiaVkProgram *program);
/* Registered stages retain their hash. Resources are null on a cache miss. */
bool pokopia_vk_program_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                             PokopiaVkProgramStage *info);
bool pokopia_vk_program_require_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                                     PokopiaVkProgramStage *info);
/* Borrow retained constant data from the live program until the next link/destroy. */
bool pokopia_vk_program_constants(const PokopiaVkProgram *program, NvnShaderStage stage,
                                 const void **data, size_t *bytes);
bool pokopia_vk_program_acquire_module(const PokopiaVkProgram *program, NvnShaderStage stage,
                                      PokopiaVkProgramModule *out);
void pokopia_vk_program_release_module(const PokopiaVkProgram *program, PokopiaVkProgramModule *module);
NvnFrontProgramLinkResult pokopia_vk_program_link(PokopiaVkProgram *program, const NvnShaderCache *cache,
    const NvnFrontShaderData *shaders, size_t count, FILE *log);

#endif
