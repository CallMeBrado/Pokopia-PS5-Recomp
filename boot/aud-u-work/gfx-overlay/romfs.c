/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "romfs.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define seek_file _fseeki64
#define tell_file _ftelli64
#else
#define seek_file fseeko
#define tell_file ftello
#endif

typedef struct {
    uint64_t offset, bytes;
    const char *path;
} RomfsExtent;

#include "generated/romfs-layout.inc"

static FILE *open_file(const char *root, const char *relative)
{
    char path[2048];
    int length = snprintf(path, sizeof path, "%s/%s", root, relative);
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
    int error = _wfopen_s(&file, wide, L"rb");
    if (error != 0) {
        errno = error;
        return NULL;
    }
    return file;
#else
    return fopen(path, "rb");
#endif
}

uint64_t hzn_romfs_size(void)
{
    return romfs_table_start + sizeof romfs_tables;
}

static bool verify_error(FILE *log, const char *path, const char *operation, int error)
{
    if (log) {
        fprintf(log, "Pokopia RomFS verify failed path=\"%s\" operation=%s errno=%d\n",
                path, operation, error);
        fflush(log);
    }
    return false;
}

bool hzn_romfs_verify_logged(const char *root, FILE *log)
{
    if (!root || !*root)
        return verify_error(log, "", "root", EINVAL);
    size_t count = ROMFS_FILE_COUNT;
    for (size_t index = 0; index < count; ++index) {
        const RomfsExtent *extent = &romfs_files[index];
        FILE *file = open_file(root, extent->path);
        if (!file)
            return verify_error(log, extent->path, "open", errno);
        const int seek = seek_file(file, 0, SEEK_END);
        const int64_t bytes = seek == 0 ? tell_file(file) : -1;
        const int error = errno;
        fclose(file);
        if (bytes < 0)
            return verify_error(log, extent->path, seek == 0 ? "tell" : "seek", error);
        if ((uint64_t)bytes != extent->bytes) {
            if (log) {
                fprintf(log, "Pokopia RomFS verify failed path=\"%s\" bytes=%" PRIi64 " expected=%" PRIu64 "\n",
                        extent->path, bytes, extent->bytes);
                fflush(log);
            }
            return false;
        }
    }
    return true;
}

bool hzn_romfs_verify(const char *root)
{
    return hzn_romfs_verify_logged(root, NULL);
}

bool hzn_romfs_read_payload(const char *root, uint64_t offset, void *destination, size_t bytes,
                            HznRomfsPayload *first)
{
    HznRomfsPayload payload = {0};
    if (first)
        *first = payload;
    uint64_t size = hzn_romfs_size();
    if (offset > size || bytes > size - offset || (!destination && bytes))
        return false;
    uint8_t *output = destination;
    const uint64_t end = offset + bytes;
    size_t file_index = 0;
    size_t file_count = ROMFS_FILE_COUNT;

    while (offset < end) {
        const uint8_t *metadata = NULL;
        uint64_t stop = end;
        if (offset < sizeof romfs_header) {
            if (stop > sizeof romfs_header)
                stop = sizeof romfs_header;
            metadata = romfs_header + offset;
        } else if (offset >= romfs_table_start) {
            metadata = romfs_tables + (offset - romfs_table_start);
        } else {
            while (file_index < file_count &&
                   romfs_files[file_index].offset + romfs_files[file_index].bytes <= offset)
                ++file_index;
            if (file_index == file_count) {
                if (stop > romfs_table_start)
                    stop = romfs_table_start;
            } else {
                const RomfsExtent *extent = &romfs_files[file_index];
                if (offset >= extent->offset) {
                    if (stop > extent->offset + extent->bytes)
                        stop = extent->offset + extent->bytes;
                    if (!root || !*root)
                        return false;
                    FILE *file = open_file(root, extent->path);
                    if (!file)
                        return false;
                    size_t span = (size_t)(stop - offset);
                    const uint64_t file_offset = offset - extent->offset;
                    bool read = seek_file(file, (int64_t)file_offset, SEEK_SET) == 0 &&
                                fread(output, 1, span, file) == span;
                    fclose(file);
                    if (!read)
                        return false;
                    if (first && !payload.path)
                        payload = (HznRomfsPayload){extent->path, file_offset, span};
                    output += span;
                    offset = stop;
                    continue;
                }
                if (stop > extent->offset)
                    stop = extent->offset;
            }
        }
        size_t span = (size_t)(stop - offset);
        if (metadata)
            memcpy(output, metadata, span);
        else
            memset(output, 0, span);
        output += span;
        offset = stop;
    }
    if (first)
        *first = payload;
    return true;
}

bool hzn_romfs_read(const char *root, uint64_t offset, void *destination, size_t bytes)
{
    return hzn_romfs_read_payload(root, offset, destination, bytes, NULL);
}
