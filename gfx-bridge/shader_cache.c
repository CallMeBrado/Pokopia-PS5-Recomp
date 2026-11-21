/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SHA-1 compression adapted from Switch2Recomp v0.2.0 nvn_shaders.cpp. */
#define _POSIX_C_SOURCE 200809L
#include "shader_cache.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define seek_file _fseeki64
#define tell_file _ftelli64
#else
#define seek_file fseeko
#define tell_file ftello
#endif

enum { MAX_ROW_BYTES = 65536, MAX_SHADER_BYTES = 16u << 20 };
static const char shader_pack_name[] = "shaders.spvpack";

typedef struct {
    uint64_t key;
    uint32_t offset;
    uint32_t length;
} CacheRow;

struct NvnShaderCache {
    char *directory;
    CacheRow *rows;
    size_t count;
    size_t stage_col, status_col, size_col, resources_col, pack_offset_col;
    uint64_t pack_bytes;
};

typedef struct {
    const char *start;
    size_t length;
} Field;

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

bool nvn_shader_control_info(const void *control, size_t control_bytes, NvnShaderControl *out)
{
    if (!control || !out || control_bytes < NVN_SHADER_CONTROL_PREFIX_BYTES)
        return false;
    const uint8_t *data = control;
    if (read_le32(data) != 0x98761234 || read_le32(data + 4) != 201 ||
        read_le32(data + 0x30) < NVN_SHADER_CONTROL_PREFIX_BYTES)
        return false;
    uint32_t code_bytes = read_le32(data + 0x44);
    uint32_t constants_bytes = read_le32(data + 0x3c);
    uint32_t constants_offset = read_le32(data + 0x40);
    uint32_t stage = (uint32_t)data[0x56] | (uint32_t)data[0x57] << 8;
    if (!code_bytes || code_bytes > MAX_SHADER_BYTES || stage > NVN_SHADER_COMPUTE ||
        (constants_bytes && (uint64_t)constants_offset + constants_bytes > code_bytes))
        return false;
    *out = (NvnShaderControl){code_bytes, constants_offset, constants_bytes, (NvnShaderStage)stage};
    return true;
}

static uint32_t rotate_left(uint32_t word, unsigned count)
{
    return (word << count) | (word >> (32 - count));
}

static void sha1_block(uint32_t hash[5], const uint8_t *bytes)
{
    uint32_t schedule[80];
    for (size_t i = 0; i < 16; ++i) {
        const uint8_t *word = bytes + i * 4;
        schedule[i] = (uint32_t)word[0] << 24 | (uint32_t)word[1] << 16 |
                      (uint32_t)word[2] << 8 | word[3];
    }
    for (size_t i = 16; i < 80; ++i)
        schedule[i] = rotate_left(schedule[i - 3] ^ schedule[i - 8] ^ schedule[i - 14] ^ schedule[i - 16], 1);
    uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3], e = hash[4];
    for (size_t i = 0; i < 80; ++i) {
        uint32_t function, constant;
        if (i < 20) {
            function = (b & c) | (~b & d);
            constant = 0x5a827999;
        } else if (i < 40) {
            function = b ^ c ^ d;
            constant = 0x6ed9eba1;
        } else if (i < 60) {
            function = (b & c) | (b & d) | (c & d);
            constant = 0x8f1bbcdc;
        } else {
            function = b ^ c ^ d;
            constant = 0xca62c1d6;
        }
        uint32_t next = rotate_left(a, 5) + function + e + constant + schedule[i];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = next;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
}

static uint64_t code_key(const uint8_t *code, uint32_t bytes, char text[17])
{
    uint32_t hash[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    uint32_t offset = 0;
    for (; bytes - offset >= 64; offset += 64)
        sha1_block(hash, code + offset);
    uint8_t tail[128] = {0};
    size_t remaining = bytes - offset;
    size_t padded = remaining < 56 ? 64 : sizeof tail;
    memcpy(tail, code + offset, remaining);
    tail[remaining] = 0x80;
    uint64_t bits = (uint64_t)bytes * 8;
    for (size_t i = 0; i < 8; ++i)
        tail[padded - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha1_block(hash, tail);
    if (padded == 128)
        sha1_block(hash, tail + 64);
    uint64_t key = (uint64_t)hash[0] << 32 | hash[1];
    for (size_t i = 0; i < 16; ++i)
        text[i] = "0123456789abcdef"[(key >> (60 - i * 4)) & 15];
    text[16] = '\0';
    return key;
}

static FILE *open_cache_file(const char *directory, const char *name)
{
    char path[2048];
    int length = snprintf(path, sizeof path, "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= sizeof path) {
        errno = ENAMETOOLONG;
        return NULL;
    }
#ifdef _WIN32
    wchar_t wide[2048];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                             (int)(sizeof wide / sizeof wide[0]))) {
        errno = EINVAL;
        return NULL;
    }
    FILE *file = NULL;
    errno_t error = _wfopen_s(&file, wide, L"rb");
    if (error != 0)
        errno = error;
    return file;
#else
    return fopen(path, "rb");
#endif
}

static NvnShaderCacheStatus open_error(void)
{
    return errno == ENOENT || errno == ENOTDIR ? NVN_SHADER_CACHE_MISSING : NVN_SHADER_CACHE_IO;
}

static NvnShaderCacheStatus read_pack_size(NvnShaderCache *cache)
{
    FILE *file = open_cache_file(cache->directory, shader_pack_name);
    if (!file)
        return open_error() == NVN_SHADER_CACHE_MISSING ? NVN_SHADER_CACHE_INVALID_CACHE : NVN_SHADER_CACHE_IO;

    int64_t bytes = seek_file(file, 0, SEEK_END) == 0 ? tell_file(file) : -1;
    fclose(file);
    if (bytes < 0)
        return NVN_SHADER_CACHE_IO;
    if (bytes % 4)
        return NVN_SHADER_CACHE_INVALID_CACHE;
    cache->pack_bytes = (uint64_t)bytes;
    return NVN_SHADER_CACHE_OK;
}

static Field field_at(const char *line, size_t column)
{
    for (size_t index = 0; index < column; ++index) {
        const char *tab = strchr(line, '\t');
        if (!tab)
            return (Field){0};
        line = tab + 1;
    }
    return (Field){line, strcspn(line, "\t\r\n")};
}

static bool field_is(Field field, const char *text)
{
    return field.start && field.length == strlen(text) && !memcmp(field.start, text, field.length);
}

static bool read_key(Field field, uint64_t *key)
{
    if (field.length != 16)
        return false;
    uint64_t value = 0;
    for (size_t i = 0; i < field.length; ++i) {
        char digit = field.start[i];
        if (digit >= '0' && digit <= '9')
            value = value << 4 | (unsigned)(digit - '0');
        else if (digit >= 'a' && digit <= 'f')
            value = value << 4 | (unsigned)(digit - 'a' + 10);
        else
            return false;
    }
    *key = value;
    return true;
}

static int compare_rows(const void *left, const void *right)
{
    uint64_t a = ((const CacheRow *)left)->key;
    uint64_t b = ((const CacheRow *)right)->key;
    return (a > b) - (a < b);
}

static size_t header_column(const char *header, const char *name)
{
    for (size_t i = 0;; ++i) {
        Field field = field_at(header, 0);
        if (field_is(field, name))
            return i;
        if (header[field.length] != '\t')
            return SIZE_MAX;
        header += field.length + 1;
    }
}

static NvnShaderCacheStatus index_rows(NvnShaderCache *cache, FILE *file, char *line)
{
    if (!fgets(line, MAX_ROW_BYTES, file))
        return ferror(file) ? NVN_SHADER_CACHE_IO : NVN_SHADER_CACHE_INVALID_CACHE;
    if (!field_is(field_at(line, 0), "hash") || strlen(line) == MAX_ROW_BYTES - 1)
        return NVN_SHADER_CACHE_INVALID_CACHE;
    cache->stage_col = header_column(line, "stage");
    cache->status_col = header_column(line, "status");
    cache->size_col = header_column(line, "spv_bytes");
    cache->resources_col = header_column(line, "resources");
    cache->pack_offset_col = header_column(line, "spv_offset");
    if (cache->stage_col == SIZE_MAX || cache->status_col == SIZE_MAX ||
        cache->size_col == SIZE_MAX || cache->resources_col == SIZE_MAX)
        return NVN_SHADER_CACHE_INVALID_CACHE;

    uint32_t offset = (uint32_t)strlen(line);
    size_t capacity = 0;
    while (fgets(line, MAX_ROW_BYTES, file)) {
        size_t length = strlen(line);
        uint64_t key;
        if (length == MAX_ROW_BYTES - 1 || length > UINT32_MAX - offset)
            return NVN_SHADER_CACHE_INVALID_CACHE;
        if (line[0] != '\r' && line[0] != '\n') {
            if (!read_key(field_at(line, 0), &key))
                return NVN_SHADER_CACHE_INVALID_CACHE;
            if (cache->count == capacity) {
                capacity = capacity ? 2 * capacity : 256;
                CacheRow *larger = realloc(cache->rows, capacity * sizeof *larger);
                if (!larger)
                    return NVN_SHADER_CACHE_OUT_OF_MEMORY;
                cache->rows = larger;
            }
            cache->rows[cache->count++] = (CacheRow){key, offset, (uint32_t)length};
        }
        offset += (uint32_t)length;
    }
    if (ferror(file))
        return NVN_SHADER_CACHE_IO;
    if (tell_file(file) != (int64_t)offset)
        return NVN_SHADER_CACHE_INVALID_CACHE;
    if (cache->count > 1)
        qsort(cache->rows, cache->count, sizeof cache->rows[0], compare_rows);
    for (size_t i = 1; i < cache->count; ++i)
        if (cache->rows[i - 1].key == cache->rows[i].key)
            return NVN_SHADER_CACHE_INVALID_CACHE;
    return NVN_SHADER_CACHE_OK;
}

NvnShaderCache *nvn_shader_cache_open(const char *directory, NvnShaderCacheStatus *status)
{
    NvnShaderCacheStatus unused;
    if (!status)
        status = &unused;
    *status = NVN_SHADER_CACHE_MISSING;
    if (!directory || !*directory)
        return NULL;
    FILE *file = open_cache_file(directory, "results.tsv");
    if (!file) {
        *status = open_error();
        return NULL;
    }
    NvnShaderCache *cache = calloc(1, sizeof *cache);
    char *line = malloc(MAX_ROW_BYTES);
    *status = NVN_SHADER_CACHE_OUT_OF_MEMORY;
    if (cache && line) {
        size_t directory_bytes = strlen(directory) + 1;
        cache->directory = malloc(directory_bytes);
        if (cache->directory) {
            memcpy(cache->directory, directory, directory_bytes);
            *status = index_rows(cache, file, line);
            if (*status == NVN_SHADER_CACHE_OK && cache->pack_offset_col != SIZE_MAX)
                *status = read_pack_size(cache);
        }
    }
    fclose(file);
    free(line);
    if (*status == NVN_SHADER_CACHE_OK)
        return cache;
    nvn_shader_cache_close(cache);
    return NULL;
}

void nvn_shader_cache_close(NvnShaderCache *cache)
{
    if (!cache)
        return;
    free(cache->rows);
    free(cache->directory);
    free(cache);
}

size_t nvn_shader_cache_count(const NvnShaderCache *cache)
{
    return cache ? cache->count : 0;
}

static bool parse_decimal(Field field, uint64_t *number)
{
    if (!field.length)
        return false;
    uint64_t value = 0;
    for (size_t i = 0; i < field.length; ++i) {
        unsigned digit = (unsigned)(field.start[i] - '0');
        if (digit > 9 || value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    *number = value;
    return true;
}

static bool parse_entry(const uint32_t *words, size_t count, NvnShaderStage stage, char entry[256])
{
    /* SPIR-V ExecutionModel values corresponding to the NVN control stage IDs. */
    static const uint32_t model[] = {0, 4, 3, 1, 2, 5};
    /* The cache and both Windows and Orbis hosts use little-endian words. */
    if (words[0] != 0x07230203 || words[1] < 0x10000 || words[1] > 0x10600 ||
        (words[1] & 0xff) || !words[3] || words[4])
        return false;
    for (size_t offset = 5; offset < count;) {
        size_t length = words[offset] >> 16;
        if (!length || length > count - offset)
            return false;
        if ((words[offset] & 0xffff) == 15) {  /* OpEntryPoint */
            if (length < 4 || words[offset + 1] != model[stage] ||
                !words[offset + 2] || words[offset + 2] >= words[3] || entry[0])
                return false;
            const char *name = (const char *)&words[offset + 3];
            const char *end = memchr(name, 0, (length - 3) * sizeof(uint32_t));
            if (!end || end == name || (size_t)(end - name) >= 256)
                return false;
            memcpy(entry, name, (size_t)(end - name) + 1);
        }
        offset += length;
    }
    return entry[0] != '\0';
}

typedef struct {
    Field resources;
    size_t bytes;
    uint64_t offset;
} ShaderRecord;

static NvnShaderCacheStatus parse_shader_record(const NvnShaderCache *cache, const char *row,
    NvnShaderStage stage, ShaderRecord *record)
{
    static const char *const stage_names[] = {
        "vertex", "fragment", "geometry", "tessellationcontrol", "tessellationevaluation", "compute",
    };
    Field status = field_at(row, cache->status_col);
    if (!field_is(status, "ok") && !field_is(status, "ok-retry")) {
        /* The packed manifest contains compiled shaders only. Loose harness
         * output can also explicitly record a failed translation attempt. */
        const bool untranslated = cache->pack_offset_col == SIZE_MAX &&
            (field_is(status, "invalid") || field_is(status, "exception") || field_is(status, "timeout") ||
             field_is(status, "no-output") || field_is(status, "unsupported"));
        return untranslated ? NVN_SHADER_CACHE_MISSING : NVN_SHADER_CACHE_INVALID_CACHE;
    }
    uint64_t declared_bytes;
    Field resources = field_at(row, cache->resources_col);
    if (!field_is(field_at(row, cache->stage_col), stage_names[stage]) ||
        !parse_decimal(field_at(row, cache->size_col), &declared_bytes) ||
        declared_bytes < 20 || declared_bytes > MAX_SHADER_BYTES || declared_bytes % 4 || !resources.start)
        return NVN_SHADER_CACHE_INVALID_CACHE;
    uint64_t offset = 0;
    if (cache->pack_offset_col != SIZE_MAX &&
        (!parse_decimal(field_at(row, cache->pack_offset_col), &offset) || offset % 4 ||
         offset > cache->pack_bytes || declared_bytes > cache->pack_bytes - offset))
        return NVN_SHADER_CACHE_INVALID_CACHE;
    *record = (ShaderRecord){.resources = resources, .bytes = (size_t)declared_bytes, .offset = offset};
    return NVN_SHADER_CACHE_OK;
}

static NvnShaderCacheStatus load_spirv(const NvnShaderCache *cache, const ShaderRecord *record, NvnCachedShader *out)
{
    const bool packed = cache->pack_offset_col != SIZE_MAX;
    const char *name = shader_pack_name;
    char individual[21];
    if (!packed) {
        memcpy(individual, out->hash, 16);
        memcpy(individual + 16, ".spv", 5);
        name = individual;
    }
    FILE *file = open_cache_file(cache->directory, name);
    if (!file)
        return packed ? NVN_SHADER_CACHE_IO : open_error();
    const size_t bytes = record->bytes;
    uint32_t *words = malloc(bytes);
    NvnShaderCacheStatus result = NVN_SHADER_CACHE_OUT_OF_MEMORY;
    if (words) {
        if (packed && seek_file(file, (int64_t)record->offset, SEEK_SET) != 0)
            result = NVN_SHADER_CACHE_IO;
        else if (fread(words, 1, bytes, file) != bytes || (!packed && fgetc(file) != EOF) || ferror(file))
            result = ferror(file) ? NVN_SHADER_CACHE_IO : NVN_SHADER_CACHE_INVALID_CACHE;
        else if (!parse_entry(words, bytes / sizeof *words, out->stage, out->entry))
            result = NVN_SHADER_CACHE_INVALID_CACHE;
        else
            result = NVN_SHADER_CACHE_OK;
    }
    fclose(file);
    if (result != NVN_SHADER_CACHE_OK) {
        free(words);
        out->entry[0] = '\0';
        return result;
    }
    out->words = words;
    out->word_count = bytes / sizeof *words;
    return NVN_SHADER_CACHE_OK;
}

static NvnShaderCacheStatus lookup_shader(const NvnShaderCache *cache, uint64_t key,
    bool materialize, NvnCachedShader *out)
{
    if (!cache || !cache->count)
        return NVN_SHADER_CACHE_MISSING;
    CacheRow needle = {.key = key};
    const CacheRow *row = bsearch(&needle, cache->rows, cache->count, sizeof *row, compare_rows);
    if (!row)
        return NVN_SHADER_CACHE_MISSING;

    FILE *file = open_cache_file(cache->directory, "results.tsv");
    if (!file)
        return NVN_SHADER_CACHE_IO;
    char *line = malloc((size_t)row->length + 1);
    if (!line) {
        fclose(file);
        return NVN_SHADER_CACHE_OUT_OF_MEMORY;
    }
    NvnShaderCacheStatus result;
    ShaderRecord record;
    if (seek_file(file, row->offset, SEEK_SET) != 0 || fread(line, 1, row->length, file) != row->length) {
        result = ferror(file) ? NVN_SHADER_CACHE_IO : NVN_SHADER_CACHE_INVALID_CACHE;
    } else {
        line[row->length] = '\0';
        uint64_t saved_key;
        result = read_key(field_at(line, 0), &saved_key) && saved_key == key
               ? parse_shader_record(cache, line, out->stage, &record) : NVN_SHADER_CACHE_INVALID_CACHE;
    }
    fclose(file);

    if (result == NVN_SHADER_CACHE_OK) {
        char *resources = malloc(record.resources.length + 1);
        if (!resources) {
            result = NVN_SHADER_CACHE_OUT_OF_MEMORY;
        } else {
            if (materialize)
                result = load_spirv(cache, &record, out);
            if (result == NVN_SHADER_CACHE_OK) {
                memcpy(resources, record.resources.start, record.resources.length);
                resources[record.resources.length] = '\0';
                out->resources = resources;
            } else {
                free(resources);
            }
        }
    }
    free(line);
    return result;
}

static NvnShaderCacheStatus lookup_code(const NvnShaderCache *cache, const void *code,
    size_t code_bytes, const NvnShaderControl *control, bool materialize, NvnCachedShader *out)
{
    if (!out)
        return NVN_SHADER_CACHE_INVALID_CONTROL;
    *out = (NvnCachedShader){0};
    if (!control || !code || !control->code_bytes || control->code_bytes > MAX_SHADER_BYTES ||
        code_bytes < control->code_bytes || (unsigned)control->stage > NVN_SHADER_COMPUTE)
        return NVN_SHADER_CACHE_INVALID_CONTROL;
    out->stage = control->stage;
    uint64_t key = code_key(code, control->code_bytes, out->hash);
    return lookup_shader(cache, key, materialize, out);
}

NvnShaderCacheStatus nvn_shader_cache_lookup(const NvnShaderCache *cache, const void *code,
    size_t code_bytes, const NvnShaderControl *control, NvnCachedShader *out)
{
    return lookup_code(cache, code, code_bytes, control, true, out);
}

NvnShaderCacheStatus nvn_shader_cache_find(const NvnShaderCache *cache, const void *code,
    size_t code_bytes, const NvnShaderControl *control, NvnCachedShader *out)
{
    return lookup_code(cache, code, code_bytes, control, false, out);
}

NvnShaderCacheStatus nvn_shader_cache_lookup_key(const NvnShaderCache *cache, const char *hash,
    NvnShaderStage expected_stage, NvnCachedShader *out)
{
    if (!out)
        return NVN_SHADER_CACHE_INVALID_CONTROL;
    NvnCachedShader initial = {0};
    uint64_t key;
    if ((unsigned)expected_stage > NVN_SHADER_COMPUTE ||
        !hash || !read_key((Field){hash, strlen(hash)}, &key)) {
        *out = initial;
        return NVN_SHADER_CACHE_INVALID_CONTROL;
    }
    memcpy(initial.hash, hash, sizeof initial.hash);
    initial.stage = expected_stage;
    *out = initial;
    return lookup_shader(cache, key, true, out);
}

void nvn_cached_shader_release(NvnCachedShader *shader)
{
    if (!shader)
        return;
    free(shader->words);
    free(shader->resources);
    *shader = (NvnCachedShader){0};
}
