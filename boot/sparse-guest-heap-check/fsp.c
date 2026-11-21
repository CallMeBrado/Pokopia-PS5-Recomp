/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fsp.h"
#include "romfs.h"
#include "../horizon/cmif.h"
#include "../horizon/account_identity.h"
#include "../horizon/save_store.h"

#include <inttypes.h>
#include <string.h>

#ifdef _WIN32
#define seek_file _fseeki64
#define tell_file _ftelli64
#else
#define seek_file fseeko
#define tell_file ftello
#endif

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_COMBINATION = 0xe801,
    INVALID_OBJECT = 0xf601,
    FS_GENERIC_FAILURE = 0x3e802,
    FS_TARGET_NOT_FOUND = 0x7d402,
    FS_OUT_OF_RANGE = 0x177a02,
    FS_INVALID_ARGUMENT = 0x2ee202,
    DOMAIN_ROOT = 1,
};

static const char proxy_type[] = "IFileSystemProxy";
static const char storage_type[] = "IStorage";
static const char save_type[] = "IFileSystem";
static const char file_type[] = "IFile";

void hzn_fsp_init(HznFsp *fsp, const char *romfs_root, HznFspWriteGuest write_guest,
                  HznFspClone clone, void *opaque)
{
    *fsp = (HznFsp){ .romfs_root = romfs_root, .write_guest = write_guest, .clone = clone,
                     .opaque = opaque, .next_object_id = DOMAIN_ROOT + 1 };
}

void hzn_fsp_set_read_log(HznFsp *fsp, FILE *log)
{
    fsp->read_log = log;
}

void hzn_fsp_set_save_root(HznFsp *fsp, const char *save_root, HznFspReadGuest read_guest)
{
    fsp->save_root = save_root;
    fsp->read_guest = read_guest;
}

static void close_save_file(HznSaveFile *file)
{
    if (file->stream)
        fclose(file->stream);
    *file = (HznSaveFile){0};
}

void hzn_fsp_close(HznFsp *fsp)
{
    for (size_t i = 0; i < sizeof fsp->files / sizeof fsp->files[0]; ++i)
        close_save_file(&fsp->files[i]);
}

static HznFspReply svc_result(uint32_t result)
{
    return (HznFspReply){ .kind = HZN_FSP_REPLY, .result = result };
}

static HznFspReply boundary(uint32_t command, uint32_t id, const char *type)
{
    return (HznFspReply){ .kind = HZN_FSP_BOUNDARY, .command = command,
                          .object_id = id, .object_type = type };
}

static HznFspReply reply_cmif(uint8_t *tls, size_t size, bool domain, uint32_t result,
                              const uint32_t *output)
{
    return svc_result(hzn_cmif_reply(tls, size, domain, result, output) ? 0 : INVALID_COMBINATION);
}

static HznFspReply control(HznFsp *fsp, uint8_t *tls, size_t size, const HznCmifRequest *request)
{
    const uint8_t *raw = request->raw;
    if (request->raw_size < 16 || hzn_cmif_read32(raw) != HZN_SFCI ||
        request->has_pid || request->has_copy_handle || request->has_buffer_descriptors)
        return svc_result(INVALID_COMBINATION);
    uint32_t command = hzn_cmif_read32(raw + 8);
    if (command == 3) {
        const uint16_t bytes = 0x8000;
        return svc_result(hzn_cmif_reply_data(tls, size, false, 0, &bytes, sizeof bytes)
                          ? 0 : INVALID_COMBINATION);
    }
    if (command == 0) {
        const uint32_t id = DOMAIN_ROOT;
        HznFspReply reply = reply_cmif(tls, size, false, 0, &id);
        if (!reply.result && !fsp->domain) {
            fsp->domain = true;
            fsp->root_open = true;
        }
        return reply;
    }
    if (command == 2 || command == 4) {
        if (request->raw_size < (command == 4 ? 20u : 16u) || size < HZN_CMIF_SESSION_REPLY_BYTES + 8)
            return svc_result(INVALID_COMBINATION);
        if (!fsp->clone)
            return boundary(command, 0, proxy_type);
        uint32_t handle = fsp->clone(fsp->opaque, fsp);
        return svc_result(hzn_cmif_reply_move_handle(tls, size, false,
                          handle ? 0 : OUT_OF_RESOURCE, handle) ? 0 : INVALID_COMBINATION);
    }
    return boundary(command, 0, proxy_type);
}

static uint32_t *find_object(uint32_t *objects, size_t count, uint32_t id)
{
    for (size_t i = 0; i < count; ++i)
        if (objects[i] == id)
            return &objects[i];
    return NULL;
}

#define FIND_OBJECT(objects, id) find_object((objects), sizeof (objects) / sizeof (objects)[0], (id))

static HznFspReply add_object(HznFsp *fsp, uint8_t *tls, size_t size, uint32_t *slot)
{
    if (!slot || fsp->next_object_id == UINT32_MAX)
        return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
    uint32_t id = fsp->next_object_id;
    HznFspReply reply = reply_cmif(tls, size, true, 0, &id);
    if (!reply.result) {
        *slot = id;
        ++fsp->next_object_id;
    }
    return reply;
}

static HznFspReply open_storage(HznFsp *fsp, uint8_t *tls, size_t size)
{
    if (!fsp->romfs_verified) {
        if (!hzn_romfs_verify_logged(fsp->romfs_root, fsp->read_log))
            return reply_cmif(tls, size, true, FS_TARGET_NOT_FOUND, NULL);
        fsp->romfs_verified = true;
    }
    return add_object(fsp, tls, size, FIND_OBJECT(fsp->storage_objects, 0));
}

static HznFspReply open_save(HznFsp *fsp, uint8_t *tls, size_t size,
                             const HznCmifRequest *request, size_t in_size)
{
    if (in_size != 88 || request->has_pid || request->has_copy_handle || request->has_buffer_descriptors)
        return svc_result(INVALID_COMBINATION);
    const uint8_t *args = request->raw + 32;
    const uint8_t *attr = args + 8;
    uint64_t title_id = hzn_cmif_read64(attr);
    uint64_t uid_hi = hzn_cmif_read64(attr + 8), uid_lo = hzn_cmif_read64(attr + 16);
    if (!fsp->process_set || (title_id && title_id != HZN_SAVE_TITLE_ID) ||
        hzn_cmif_read64(attr + 24) || hzn_cmif_read32(attr + 32) != 1)
        return reply_cmif(tls, size, true, FS_INVALID_ARGUMENT, NULL);
    if (!fsp->save_root || args[0] != 1 || uid_hi != HZN_OFFLINE_UID_WORD0 || uid_lo != HZN_OFFLINE_UID_WORD1)
        return reply_cmif(tls, size, true, FS_TARGET_NOT_FOUND, NULL);
    char path[HZN_SAVE_PATH_MAX];
    uint32_t result = hzn_save_open_account(fsp->save_root, uid_hi, uid_lo, path);
    if (result)
        return reply_cmif(tls, size, true, result, NULL);
    return add_object(fsp, tls, size, FIND_OBJECT(fsp->save_objects, 0));
}

static HznSaveFile *find_save_file(HznFsp *fsp, uint32_t id)
{
    for (size_t i = 0; i < sizeof fsp->files / sizeof fsp->files[0]; ++i)
        if (fsp->files[i].id == id)
            return &fsp->files[i];
    return NULL;
}

static HznFspReply open_save_file(HznFsp *fsp, uint8_t *tls, size_t size,
                                  const HznCmifRequest *request, size_t in_size)
{
    const size_t path_bytes = 0x301;
    if (in_size != 20 || hzn_cmif_read32(tls) >> 16 != 1 || request->has_pid ||
        request->has_copy_handle || request->input.size != path_bytes || !fsp->read_guest)
        return svc_result(INVALID_COMBINATION);
    if (hzn_cmif_read32(request->raw + 32) != 1)
        return reply_cmif(tls, size, true, FS_INVALID_ARGUMENT, NULL);
    const char *path = fsp->read_guest(fsp->opaque, request->input.address, path_bytes);
    if (!path || !memchr(path, '\0', path_bytes))
        return svc_result(INVALID_ADDRESS);
    HznSaveFile *file = find_save_file(fsp, 0);
    if (!file)
        return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
    uint32_t result = hzn_save_open_read(fsp->save_root, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1,
                                         path, &file->stream);
    if (result)
        return reply_cmif(tls, size, true, result, NULL);
    HznFspReply reply = add_object(fsp, tls, size, &file->id);
    if (!file->id)
        close_save_file(file);
    return reply;
}

extern bool sparse_heap_prepare_io(void *destination, size_t bytes);

static HznFspReply read_save_file(HznFsp *fsp, uint8_t *tls, size_t size, const HznCmifRequest *request,
                                  size_t in_size, FILE *file)
{
    if (in_size != 40 || request->has_pid || request->has_copy_handle || !request->has_output_buffer)
        return svc_result(INVALID_COMBINATION);
    uint64_t offset = hzn_cmif_read64(request->raw + 40), bytes = hzn_cmif_read64(request->raw + 48);
    if (offset > INT64_MAX || bytes > SIZE_MAX || bytes > request->output.size)
        return reply_cmif(tls, size, true, FS_OUT_OF_RANGE, NULL);
    uint64_t bytes_read = 0;
    if (bytes) {
        void *output = fsp->write_guest ? fsp->write_guest(fsp->opaque, request->output.address, (size_t)bytes) : NULL;
        if (!output || !sparse_heap_prepare_io(output, (size_t)bytes))
            return svc_result(INVALID_ADDRESS);
        clearerr(file);
        if (seek_file(file, (int64_t)offset, SEEK_SET))
            return reply_cmif(tls, size, true, FS_GENERIC_FAILURE, NULL);
        bytes_read = fread(output, 1, (size_t)bytes, file);
        if (ferror(file))
            return reply_cmif(tls, size, true, FS_GENERIC_FAILURE, NULL);
    }
    return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &bytes_read, sizeof bytes_read)
                      ? 0 : INVALID_COMBINATION);
}

static HznFspReply storage_read(HznFsp *fsp, uint8_t *tls, size_t size,
                                const HznCmifRequest *request, size_t in_size, uint32_t id)
{
    if (in_size != 32 || !request->has_output_buffer || request->has_pid || request->has_copy_handle ||
        size < HZN_CMIF_DOMAIN_REPLY_BYTES)
        return svc_result(INVALID_COMBINATION);
    uint64_t offset = hzn_cmif_read64(request->raw + 32);
    uint64_t bytes = hzn_cmif_read64(request->raw + 40);
    uint64_t image_size = hzn_romfs_size();
    if (offset > image_size || bytes > image_size - offset)
        return reply_cmif(tls, size, true, FS_OUT_OF_RANGE, NULL);
    if (bytes > request->output.size || bytes > SIZE_MAX)
        return reply_cmif(tls, size, true, FS_INVALID_ARGUMENT, NULL);
    if (!bytes)
        return reply_cmif(tls, size, true, 0, NULL);
    if (!fsp->write_guest)
        return boundary(0, id, storage_type);
    void *output = fsp->write_guest(fsp->opaque, request->output.address, (size_t)bytes);
    if (!output || !sparse_heap_prepare_io(output, (size_t)bytes))
        return svc_result(INVALID_ADDRESS);
    uintptr_t dest = (uintptr_t)output, reply = (uintptr_t)tls;
    if (dest >= reply ? dest - reply < size : reply - dest < bytes)
        return svc_result(INVALID_ADDRESS);
    HznRomfsPayload payload = {0};
    HznRomfsPayload *trace = fsp->read_log && !fsp->first_asset_read ? &payload : NULL;
    uint32_t result = hzn_romfs_read_payload(fsp->romfs_root, offset, output, (size_t)bytes, trace)
                    ? 0 : FS_GENERIC_FAILURE;
    HznFspReply response = reply_cmif(tls, size, true, result, NULL);
    if (!result && !response.result && payload.path) {
        fsp->first_asset_read = true;
        fprintf(fsp->read_log, "Pokopia RomFS first asset read path=\"%s\" file_offset=%#" PRIx64 " bytes=%zu\n",
                payload.path, payload.file_offset, payload.bytes);
        fflush(fsp->read_log);
    }
    return response;
}

HznFspReply hzn_fsp_send_sync(HznFsp *fsp, void *buffer, size_t size)
{
    if (!buffer)
        return svc_result(INVALID_ADDRESS);
    if (size < 8)
        return svc_result(INVALID_COMBINATION);
    uint8_t *tls = buffer;
    uint32_t type = hzn_cmif_read32(tls);
    if ((type == 2 || type == 15) && !hzn_cmif_read32(tls + 4)) {
        memset(tls, 0, 8);
        return svc_result(0);
    }

    HznCmifRequest request;
    if (!hzn_cmif_parse_with_nonsecure_output(tls, size, &request))
        return svc_result(INVALID_COMBINATION);
    if (request.type == 5 || request.type == 7)
        return control(fsp, tls, size, &request);
    const uint8_t *raw = request.raw;
    if (request.type != 4 && request.type != 6)
        return boundary(UINT32_MAX, 0, proxy_type);
    if (!fsp->domain)
        return boundary(request.raw_size >= 16 && hzn_cmif_read32(raw) == HZN_SFCI
                        ? hzn_cmif_read32(raw + 8) : UINT32_MAX, 0, proxy_type);
    if (request.raw_size < 16)
        return svc_result(INVALID_COMBINATION);

    uint32_t id = hzn_cmif_read32(raw + 4);
    bool root = id == DOMAIN_ROOT && fsp->root_open;
    uint32_t *object = id > DOMAIN_ROOT ? FIND_OBJECT(fsp->storage_objects, id) : NULL;
    uint32_t *save = id > DOMAIN_ROOT ? FIND_OBJECT(fsp->save_objects, id) : NULL;
    HznSaveFile *file = id > DOMAIN_ROOT ? find_save_file(fsp, id) : NULL;
    uint32_t header = hzn_cmif_read32(raw);
    if (header == 2) {
        if (request.has_pid || request.has_copy_handle || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        HznFspReply reply = reply_cmif(tls, size, true, root || object || save || file ? 0 : INVALID_OBJECT, NULL);
        if (!reply.result) {
            if (root)
                fsp->root_open = false;
            if (object)
                *object = 0;
            if (save)
                *save = 0;
            if (file)
                close_save_file(file);
        }
        return reply;
    }
    size_t in_size = header >> 16;
    if ((header & 0xffff) != 1 || in_size < 16 || in_size > request.raw_size - 16 ||
        hzn_cmif_read32(raw + 16) != HZN_SFCI)
        return svc_result(INVALID_COMBINATION);
    uint32_t command = hzn_cmif_read32(raw + 24);
    if (root) {
        if (command == 1) {
            if (in_size != 24 || !request.has_pid || request.has_copy_handle ||
                request.has_buffer_descriptors || hzn_cmif_read64(raw + 32) != 0)
                return svc_result(INVALID_COMBINATION);
            HznFspReply reply = reply_cmif(tls, size, true, 0, NULL);
            if (!reply.result)
                fsp->process_set = true;
            return reply;
        }
        if (command == 51)  /* OpenSaveDataFileSystem */
            return open_save(fsp, tls, size, &request, in_size);
        if (command == 200) {
            if (in_size != 16 || request.has_pid || request.has_copy_handle || request.has_buffer_descriptors)
                return svc_result(INVALID_COMBINATION);
            if (!fsp->process_set)
                return reply_cmif(tls, size, true, FS_INVALID_ARGUMENT, NULL);
            return open_storage(fsp, tls, size);
        }
        if (command == 1005) {  /* GetGlobalAccessLogMode */
            if (in_size != 16 || request.has_pid || request.has_copy_handle || request.has_buffer_descriptors)
                return svc_result(INVALID_COMBINATION);
            /* This service has no filesystem access-log sink. */
            const uint32_t mode = 0;
            return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &mode, sizeof mode)
                              ? 0 : INVALID_COMBINATION);
        }
        return boundary(command, id, proxy_type);
    }
    if (save)
        return command == 8 ? open_save_file(fsp, tls, size, &request, in_size) : boundary(command, id, save_type);
    if (file) {
        if (command == 0)
            return read_save_file(fsp, tls, size, &request, in_size, file->stream);
        if (command == 4) {
            if (in_size != 16 || request.has_pid || request.has_copy_handle || request.has_buffer_descriptors)
                return svc_result(INVALID_COMBINATION);
            int64_t bytes = seek_file(file->stream, 0, SEEK_END) ? -1 : tell_file(file->stream);
            if (bytes < 0)
                return reply_cmif(tls, size, true, FS_GENERIC_FAILURE, NULL);
            return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &bytes, sizeof bytes)
                              ? 0 : INVALID_COMBINATION);
        }
        return boundary(command, id, file_type);
    }
    if (!object)
        return reply_cmif(tls, size, true, INVALID_OBJECT, NULL);
    if (command == 0)
        return storage_read(fsp, tls, size, &request, in_size, id);
    if (command == 2 || command == 4) {
        if (in_size != 16 || request.has_pid || request.has_copy_handle || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        if (command == 2)
            return reply_cmif(tls, size, true, 0, NULL);
        uint64_t bytes = hzn_romfs_size();
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &bytes, sizeof bytes)
                          ? 0 : INVALID_COMBINATION);
    }
    return boundary(command, id, storage_type);
}
