/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_SHADER_CACHE_H
#define POKOPIA_SHADER_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { NVN_SHADER_CONTROL_PREFIX_BYTES = 0x58 };

/* NVN2 GLSLC stage IDs, as carried by the control block. */
typedef enum {
    NVN_SHADER_VERTEX,
    NVN_SHADER_FRAGMENT,
    NVN_SHADER_GEOMETRY,
    NVN_SHADER_TESS_CONTROL,
    NVN_SHADER_TESS_EVAL,
    NVN_SHADER_COMPUTE,
} NvnShaderStage;

typedef struct {
    uint32_t code_bytes;
    uint32_t constants_offset;
    uint32_t constants_bytes;
    NvnShaderStage stage;
} NvnShaderControl;

typedef enum {
    NVN_SHADER_CACHE_OK,
    NVN_SHADER_CACHE_MISSING,
    NVN_SHADER_CACHE_INVALID_CONTROL,
    NVN_SHADER_CACHE_INVALID_CACHE,
    NVN_SHADER_CACHE_IO,
    NVN_SHADER_CACHE_OUT_OF_MEMORY,
} NvnShaderCacheStatus;

typedef struct NvnShaderCache NvnShaderCache;

typedef struct {
    char hash[17];
    NvnShaderStage stage;
    uint32_t *words;
    size_t word_count;
    char *resources;
    char entry[256];
} NvnCachedShader;

/* The index stays read-only after open. Keep the directory contents stable
 * while the cache is open. A results.tsv spv_offset column selects byte ranges
 * in shaders.spvpack. Without that column, lookups use individual .spv files. */
NvnShaderCache *nvn_shader_cache_open(const char *directory, NvnShaderCacheStatus *status);
void nvn_shader_cache_close(NvnShaderCache *cache);
size_t nvn_shader_cache_count(const NvnShaderCache *cache);

/* Reads only NVN_SHADER_CONTROL_PREFIX_BYTES. A caller mapping guest memory can
 * validate the returned code_bytes before acquiring the shader code span. */
bool nvn_shader_control_info(const void *control, size_t control_bytes, NvnShaderControl *out);

/* out must be empty or previously released. On a cache miss, hash still names
 * the verified code block. A successful lookup owns words and resources. */
NvnShaderCacheStatus nvn_shader_cache_lookup(const NvnShaderCache *cache, const void *code,
                                          size_t code_bytes, const NvnShaderControl *control,
                                          NvnCachedShader *out);
/* Find validates metadata and returns owned resources without reading SPIR-V.
 * words, word_count and entry are empty. */
NvnShaderCacheStatus nvn_shader_cache_find(const NvnShaderCache *cache, const void *code,
    size_t code_bytes, const NvnShaderControl *control, NvnCachedShader *out);
/* Lookup a canonical lowercase, 16-character hash and materialize its shader. */
NvnShaderCacheStatus nvn_shader_cache_lookup_key(const NvnShaderCache *cache, const char *hash,
    NvnShaderStage expected_stage, NvnCachedShader *out);
void nvn_cached_shader_release(NvnCachedShader *shader);

#endif
