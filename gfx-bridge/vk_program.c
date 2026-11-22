/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vk_program.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* R95: SPIR-V rewrites made when a module is created.
 * - InverseSqrt(x) becomes InverseSqrt(max(x, 1e-30)) for 32-bit scalars, so a
 *   zero-length vector gets a large finite scale instead of +Inf.
 * - With fmz, scalar a * b becomes (a == 0 || b == 0) ? 0 : a * b and
 *   fma(a, b, c) becomes (a == 0 || b == 0) ? c : fma(a, b, c): the GPU's
 *   "zero times anything is zero" multiply, which plain IEEE math turns into
 *   NaN when the other operand is infinite. Float stores to the outputs also
 *   turn NaN into 0 and clamp to the largest finite R11G11B10 value (AMD blends
 *   those targets at half precision, where larger colours overflow).
 *   The sky's cloud layer (fragment 8010f4846df5fa37) wrote NaN into the HDR
 *   target, shown as black clouds with a white bloom halo.
 * Returns a new word stream (free it), or NULL when nothing changed. */
static uint32_t *spirv_patch(const uint32_t *words, size_t count, bool fmz, size_t *out_count)
{
    enum { OP_EXT_INST_IMPORT = 11, OP_EXT_INST = 12, OP_TYPE_BOOL = 20, OP_TYPE_FLOAT = 22, OP_CONSTANT = 43,
           OP_FUNCTION = 54, OP_FMUL = 133, OP_LOGICAL_OR = 166, OP_SELECT = 169, OP_FORD_EQUAL = 180,
           OP_VARIABLE = 59, OP_STORE = 62, OP_ACCESS_CHAIN = 65, OP_IS_NAN = 156, STORAGE_OUTPUT = 3,
           GLSL_INVERSE_SQRT = 32, GLSL_FMAX = 40, GLSL_FCLAMP = 43, GLSL_FMA = 50 };
    if (!words || count < 5 || words[0] != 0x07230203u)
        return NULL;
    uint32_t glsl = 0, float32 = 0, boolean = 0;
    size_t rsqrts = 0, muls = 0, stores = 0, function_at = 0;
    /* With fmz, float stores to the fragment outputs also replace NaN with 0. */
    uint32_t outputs[16], output_count = 0, chains[64], chain_count = 0;
    for (size_t at = 5; at < count;) {
        const uint32_t length = words[at] >> 16, op = words[at] & 0xffffu;
        if (!length || at + length > count)
            return NULL;
        if (op == OP_EXT_INST_IMPORT && length >= 3 && !strcmp((const char *)&words[at + 2], "GLSL.std.450"))
            glsl = words[at + 1];
        else if (op == OP_TYPE_FLOAT && length == 3 && words[at + 2] == 32)
            float32 = words[at + 1];
        else if (op == OP_TYPE_BOOL && length == 2)
            boolean = words[at + 1];
        else if (op == OP_FUNCTION && !function_at)
            function_at = at;
        else if (op == OP_VARIABLE && length >= 4 && words[at + 3] == STORAGE_OUTPUT && output_count < 16)
            outputs[output_count++] = words[at + 2];
        else if (fmz && op == OP_ACCESS_CHAIN && length >= 4 && chain_count < 64) {
            for (uint32_t i = 0; i < output_count; ++i)
                if (words[at + 3] == outputs[i])
                    chains[chain_count++] = words[at + 2];
        } else if (fmz && op == OP_STORE && length == 3) {
            for (uint32_t i = 0; i < chain_count; ++i)
                stores += words[at + 1] == chains[i];
        }
        else if (op == OP_EXT_INST && length == 6 && glsl && float32 && words[at + 3] == glsl &&
                 words[at + 4] == GLSL_INVERSE_SQRT && words[at + 1] == float32)
            ++rsqrts;
        else if (fmz && float32 && ((op == OP_FMUL && length == 5 && words[at + 1] == float32) ||
                                    (op == OP_EXT_INST && length == 8 && glsl && words[at + 3] == glsl &&
                                     words[at + 4] == GLSL_FMA && words[at + 1] == float32)))
            ++muls;
        at += length;
    }
    if ((!rsqrts && !muls && !stores) || !function_at)
        return NULL;
    uint32_t *out = malloc((count + 19 + rsqrts * 7 + muls * 21 + stores * 18) * sizeof *out);
    if (!out)
        return NULL;
    uint32_t bound = words[3];
    const uint32_t tiny = bound++, zero = bound++, low = bound++, high = bound++;
    const bool add_bool = (muls || stores) && !boolean;
    if (add_bool)
        boolean = bound++;
    size_t n = 0;
    for (size_t at = 0; at < 5; ++at)
        out[n++] = words[at];
    for (size_t at = 5; at < count;) {
        const uint32_t length = words[at] >> 16, op = words[at] & 0xffffu;
        const uint32_t *w = words + at;
        if (at == function_at) {
            const float value = 1e-30f;
            uint32_t bits;
            memcpy(&bits, &value, sizeof bits);
            if (add_bool) {
                out[n++] = 2u << 16 | OP_TYPE_BOOL;
                out[n++] = boolean;
            }
            out[n++] = 4u << 16 | OP_CONSTANT;
            out[n++] = float32;
            out[n++] = tiny;
            out[n++] = bits;
            out[n++] = 4u << 16 | OP_CONSTANT;
            out[n++] = float32;
            out[n++] = zero;
            out[n++] = 0;
            /* the largest finite R11G11B10 value; AMD blends these targets at
             * half precision, where a larger colour became Inf and then NaN */
            const float limit = 65024.0f, negative = -65024.0f;
            uint32_t limit_bits, negative_bits;
            memcpy(&limit_bits, &limit, sizeof limit_bits);
            memcpy(&negative_bits, &negative, sizeof negative_bits);
            out[n++] = 4u << 16 | OP_CONSTANT;
            out[n++] = float32;
            out[n++] = low;
            out[n++] = negative_bits;
            out[n++] = 4u << 16 | OP_CONSTANT;
            out[n++] = float32;
            out[n++] = high;
            out[n++] = limit_bits;
        }
        const bool is_rsqrt = op == OP_EXT_INST && length == 6 && w[3] == glsl && w[4] == GLSL_INVERSE_SQRT &&
                              w[1] == float32;
        const bool is_mul = fmz && op == OP_FMUL && length == 5 && w[1] == float32;
        const bool is_fma = fmz && op == OP_EXT_INST && length == 8 && w[3] == glsl && w[4] == GLSL_FMA &&
                            w[1] == float32;
        if (is_rsqrt) {
            const uint32_t clamped = bound++;
            const uint32_t fmax[] = {7u << 16 | OP_EXT_INST, float32, clamped, glsl, GLSL_FMAX, w[5], tiny};
            memcpy(out + n, fmax, sizeof fmax);
            n += 7;
            memcpy(out + n, w, 5 * sizeof *out);
            out[n + 5] = clamped;
            n += 6;
        } else if (is_mul || is_fma) {
            const uint32_t a = w[is_mul ? 3 : 5], b = w[is_mul ? 4 : 6];
            const uint32_t product = bound++, a_zero = bound++, b_zero = bound++, either = bound++;
            memcpy(out + n, w, length * sizeof *out);
            out[n + 2] = product;
            n += length;
            const uint32_t tail[] = {
                5u << 16 | OP_FORD_EQUAL, boolean, a_zero, a, zero,
                5u << 16 | OP_FORD_EQUAL, boolean, b_zero, b, zero,
                5u << 16 | OP_LOGICAL_OR, boolean, either, a_zero, b_zero,
                6u << 16 | OP_SELECT, float32, w[2], either, is_mul ? zero : w[7], product,
            };
            memcpy(out + n, tail, sizeof tail);
            n += sizeof tail / sizeof *tail;
        } else if (fmz && op == OP_STORE && length == 3 && stores) {
            bool output = false;
            for (uint32_t i = 0; i < chain_count; ++i)
                output |= w[1] == chains[i];
            if (output) {
                const uint32_t nan = bound++, clean = bound++, clamped = bound++;
                const uint32_t fix[] = {
                    4u << 16 | OP_IS_NAN, boolean, nan, w[2],
                    6u << 16 | OP_SELECT, float32, clean, nan, zero, w[2],
                    8u << 16 | OP_EXT_INST, float32, clamped, glsl, GLSL_FCLAMP, clean, low, high,
                    3u << 16 | OP_STORE, w[1], clamped,
                };
                memcpy(out + n, fix, sizeof fix);
                n += sizeof fix / sizeof *fix;
            } else {
                memcpy(out + n, w, length * sizeof *out);
                n += length;
            }
        } else {
            memcpy(out + n, w, length * sizeof *out);
            n += length;
        }
        at += length;
    }
    out[3] = bound;
    *out_count = n;
    return out;
}

/* R95: shaders that get the fmz rewrite (found with debug/skip, see nvn_replay.c). */
static bool shader_wants_fmz(const char *hash)
{
    return hash && !strcmp(hash, "8010f4846df5fa37");
}

typedef struct {
    const NvnShaderCache *cache;
    char hash[17];
    char *resources;
    void *constants;
    size_t constants_bytes;
    /* R67: created on first use and kept until the program is relinked or
     * destroyed; loading SPIR-V from the cache files per draw cost ~1.4 s per 5 s. */
    VkShaderModule module;
    char entry[256];
    bool module_shared; /* R78: owned by module_cache */
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

/* R78: shader modules are kept per shader hash and stage across relinks; the
 * game relinks ~35 programs a second and each relink reloaded the SPIR-V from
 * the cache files and created the module again. */
enum { MODULE_CACHE = 4096 };
static struct {
    char hash[17];
    unsigned stage;
    VkDevice device;
    VkShaderModule module;
    char entry[256];
} module_cache[MODULE_CACHE];

static size_t module_slot(const char *hash, unsigned stage)
{
    uint64_t h = UINT64_C(1469598103934665603) ^ stage;
    for (const char *c = hash; *c; ++c)
        h = (h ^ (unsigned char)*c) * UINT64_C(1099511628211);
    return (size_t)h & (MODULE_CACHE - 1);
}

static bool module_cached(VkDevice device, const char *hash, unsigned stage, VkShaderModule *module, char entry[256])
{
    size_t slot = module_slot(hash, stage);
    for (unsigned probe = 0; probe < 16; ++probe, slot = (slot + 1) & (MODULE_CACHE - 1)) {
        if (!module_cache[slot].module)
            return false;
        if (module_cache[slot].device == device && module_cache[slot].stage == stage &&
            !strcmp(module_cache[slot].hash, hash)) {
            *module = module_cache[slot].module;
            memcpy(entry, module_cache[slot].entry, 256);
            return true;
        }
    }
    return false;
}

static bool module_keep(VkDevice device, const char *hash, unsigned stage, VkShaderModule module, const char entry[256])
{
    size_t slot = module_slot(hash, stage);
    for (unsigned probe = 0; probe < 16; ++probe, slot = (slot + 1) & (MODULE_CACHE - 1))
        if (!module_cache[slot].module) {
            snprintf(module_cache[slot].hash, sizeof module_cache[slot].hash, "%s", hash);
            module_cache[slot].stage = stage;
            module_cache[slot].device = device;
            module_cache[slot].module = module;
            memcpy(module_cache[slot].entry, entry, 256);
            return true;
        }
    return false;
}

static void release_stages(const PokopiaVkProgram *program, ProgramStage *stages)
{
    for (unsigned i = 0; i <= NVN_SHADER_COMPUTE; ++i) {
        if (stages[i].module && !stages[i].module_shared)
            program->destroy(program->device, stages[i].module, NULL);
        free(stages[i].resources);
        free(stages[i].constants);
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
    if (!program || !info || (unsigned)stage > NVN_SHADER_COMPUTE || !program->stages[stage].hash[0])
        return false;
    const ProgramStage *linked = &program->stages[stage];
    *info = (PokopiaVkProgramStage){linked->hash, linked->resources, linked->constants, linked->constants_bytes};
    return true;
}

bool pokopia_vk_program_require_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                                     PokopiaVkProgramStage *info)
{
    if (!pokopia_vk_program_stage(program, stage, info))
        return false;
    if (info->resources)
        return true;
    /* R74: once per shader; this ran on every draw (3k lines a minute). */
    static char logged[64][24];
    static unsigned logged_count;
    bool first = logged_count < 64;
    for (unsigned i = 0; i < logged_count && first; ++i)
        first = strcmp(logged[i], info->hash ? info->hash : "-") != 0;
    if (first && program->log) {
        snprintf(logged[logged_count++], sizeof logged[0], "%s", info->hash ? info->hash : "-");
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
    ProgramStage *linked = (ProgramStage *)&program->stages[stage];
    if (!linked->module && module_cached(program->device, linked->hash, stage, &linked->module, linked->entry))
        linked->module_shared = true;
    if (linked->module) {
        out->module = linked->module;
        memcpy(out->entry, linked->entry, sizeof out->entry);
        out->cached = true;
        return true;
    }
    NvnCachedShader cached = {0};
    NvnShaderCacheStatus status = nvn_shader_cache_lookup_key(linked->cache, linked->hash, stage, &cached);
    if (status != NVN_SHADER_CACHE_OK) {
        fprintf(program->log, "NVN shader cache result=%d key=%s stage=%u\n", status, linked->hash, (unsigned)stage);
        fflush(program->log);
        nvn_cached_shader_release(&cached);
        return false;
    }
    size_t guarded_count = 0;
    uint32_t *guarded = spirv_patch(cached.words, cached.word_count, shader_wants_fmz(linked->hash), &guarded_count);
    if (guarded && shader_wants_fmz(linked->hash) && program->log) {
        fprintf(program->log, "NVN SPIR-V patched key=%s stage=%u words %zu -> %zu\n", linked->hash, (unsigned)stage,
                cached.word_count, guarded_count);
        fflush(program->log);
    }
    const VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (guarded ? guarded_count : cached.word_count) * sizeof *cached.words,
        .pCode = guarded ? guarded : cached.words,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult result = program->create(program->device, &create_info, NULL, &module);
    free(guarded);
    if (result == VK_SUCCESS) {
        linked->module = module;
        memcpy(linked->entry, cached.entry, sizeof linked->entry);
        linked->module_shared = module_keep(program->device, linked->hash, stage, module, linked->entry);
        out->module = module;
        memcpy(out->entry, cached.entry, sizeof out->entry);
        out->cached = true;
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
    if (!module->cached)
        program->destroy(program->device, module->module, NULL);
    *module = (PokopiaVkProgramModule){0};
}

/* R88: called for each shader the translated cache lacks (the loader saves
 * its blocks so it can be translated offline and added to the pack). */
void (*pokopia_vk_program_missing_hook)(const NvnFrontShaderData *shader, const char *hash);

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
        if (status == NVN_SHADER_CACHE_MISSING && pokopia_vk_program_missing_hook)
            pokopia_vk_program_missing_hook(shader, cached.hash);
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
        const uint32_t constants_offset = shader->parsed.constants_offset;
        const uint32_t constants_bytes = shader->parsed.constants_bytes;
        if (constants_bytes && (uint64_t)constants_offset + constants_bytes <= shader->parsed.code_bytes) {
            stage->constants = malloc(constants_bytes);
            if (!stage->constants)
                goto done;
            memcpy(stage->constants, (const uint8_t *)shader->code + constants_offset, constants_bytes);
            stage->constants_bytes = constants_bytes;
        }
    }
    outcome = incomplete ? NVN_FRONT_PROGRAM_CACHE_MISS : NVN_FRONT_PROGRAM_LINKED;

done:
    release_stages(program, program->stages);
    if (outcome != NVN_FRONT_PROGRAM_FAILED)
        memcpy(program->stages, linked, sizeof linked);
    else
        release_stages(program, linked);
    if (log)
        fflush(log);
    return outcome;
}
