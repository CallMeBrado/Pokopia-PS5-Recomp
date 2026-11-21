/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fsp.h"
#include "romfs.h"
#include "../horizon/cmif.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define seek_file _fseeki64
#else
#define seek_file fseeko
#endif

static const uint64_t guest_address = UINT64_C(0x7101234000);

typedef struct {
    uint8_t buffer[1024];
    HznFsp *alias;
    unsigned writes, clones;
} TestHost;

static FILE *temporary_log(void)
{
#ifdef _WIN32
    FILE *log = NULL;
    assert(tmpfile_s(&log) == 0);
#else
    FILE *log = tmpfile();
#endif
    assert(log);
    return log;
}

static void *write_guest(void *opaque, uint64_t address, size_t size)
{
    TestHost *host = opaque;
    ++host->writes;
    return address == guest_address && size <= sizeof host->buffer ? host->buffer : NULL;
}

static uint32_t clone_session(void *opaque, HznFsp *same_domain)
{
    TestHost *host = opaque;
    ++host->clones;
    host->alias = same_domain;
    return 0x102;
}

static void set_control(uint32_t tls[64], uint32_t command)
{
    memset(tls, 0, 256);
    tls[0] = 7;
    tls[1] = command == 4 ? 9 : 8;
    tls[4] = HZN_SFCI;
    tls[5] = 1;
    tls[6] = command;
}

static void set_domain(uint32_t tls[64], uint32_t object, uint32_t command, bool pid)
{
    memset(tls, 0, 256);
    tls[0] = 6;
    tls[1] = pid ? 0x8000000e : 12;
    tls[2] = pid;
    uint32_t *raw = tls + (pid ? 8 : 4);
    raw[0] = pid ? 0x180001 : 0x100001;
    raw[1] = object;
    raw[4] = HZN_SFCI;
    raw[5] = 1;
    raw[6] = command;
}

static void set_read(uint32_t tls[64], uint32_t object, uint64_t offset, uint32_t bytes)
{
    memset(tls, 0, 256);
    tls[0] = 0x01000006;
    tls[1] = 16;
    tls[2] = bytes;
    tls[3] = (uint32_t)guest_address;
    tls[4] = 1 | (uint32_t)((guest_address >> 36) << 2) |
                 (uint32_t)((guest_address >> 32) & 15) << 28;
    tls[8] = 0x200001;
    tls[9] = object;
    tls[12] = HZN_SFCI;
    tls[13] = 1;
    tls[16] = (uint32_t)offset;
    tls[17] = (uint32_t)(offset >> 32);
    tls[18] = bytes;
}

static uint64_t file_offset(const char *root, const char *name)
{
    uint8_t header[0x50], entry[0x20], stored_name[255];
    assert(hzn_romfs_read(root, 0, header, sizeof header));
    uint64_t table = hzn_cmif_read64(header + 56);
    uint64_t table_size = hzn_cmif_read64(header + 64);
    for (uint64_t offset = 0; offset < table_size;) {
        assert(hzn_romfs_read(root, table + offset, entry, sizeof entry));
        uint32_t length = hzn_cmif_read32(entry + 28);
        assert(length <= sizeof stored_name);
        assert(hzn_romfs_read(root, table + offset + sizeof entry, stored_name, length));
        if (length == strlen(name) && !memcmp(name, stored_name, length))
            return hzn_cmif_read64(header + 72) + hzn_cmif_read64(entry + 8);
        offset += sizeof entry + ((length + 3) & ~3u);
    }
    assert(!"file is present in fixture");
    return 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *root = argv[1];
    assert(hzn_romfs_size() == UINT64_C(7246055636));
    assert(hzn_romfs_verify(root));
    assert(!hzn_romfs_verify(NULL));
    assert(!hzn_romfs_read(root, hzn_romfs_size(), NULL, 1));

    HznFsp fsp;
    TestHost host = { 0 };
    uint32_t tls[64];
    hzn_fsp_init(&fsp, root, write_guest, clone_session, &host);
    FILE *read_log = temporary_log();
    hzn_fsp_set_read_log(&fsp, read_log);
    set_control(tls, 3);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[8] == 0x8000);
    set_control(tls, 0);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[8] == 1);
    set_control(tls, 4);
    assert(hzn_fsp_send_sync(&fsp, tls, 47).result == 0xe801 && !host.clones);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0);
    assert(tls[1] == 0x80000008 && tls[2] == 0x20 && tls[3] == 0x102);
    assert(host.clones == 1 && host.alias == &fsp);

    set_domain(tls, 1, 1, true);
    assert(hzn_fsp_send_sync(host.alias, tls, sizeof tls).result == 0 && tls[10] == 0);
    set_domain(tls, 1, 1005, false);
    assert(hzn_fsp_send_sync(host.alias, tls, sizeof tls).result == 0);
    assert(tls[4] == 0 && tls[8] == HZN_SFCO && tls[10] == 0 && tls[12] == 0);
    set_domain(tls, 1, 1005, true);
    assert(hzn_fsp_send_sync(host.alias, tls, sizeof tls).result == 0xe801);
    set_domain(tls, 1, 200, false);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0);
    assert(tls[4] == 1 && tls[8] == HZN_SFCO && tls[10] == 0 && tls[12] == 2);
    set_domain(tls, 2, 4, false);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0);
    assert(tls[4] == 0 && hzn_cmif_read64((const uint8_t *)(tls + 12)) == hzn_romfs_size());

    set_read(tls, 2, 0, 0x200);
    assert(hzn_fsp_send_sync(host.alias, tls, sizeof tls).result == 0 && tls[10] == 0);
    assert(host.writes == 1 && hzn_cmif_read64(host.buffer) == 0x50);
    assert(!fsp.first_asset_read && ftell(read_log) == 0);
    for (size_t i = 0x50; i < 0x200; ++i)
        assert(host.buffer[i] == 0);
    uint8_t expect[32];
    char path[2048];
    int length = snprintf(path, sizeof path, "%s/motor_asset_re/KIDS_PLATFORM_OUNCE/0x38f972af.fdata", root);
    assert(length > 0 && (size_t)length < sizeof path);
#ifdef _WIN32
    FILE *file = NULL;
    assert(fopen_s(&file, path, "rb") == 0);
#else
    FILE *file = fopen(path, "rb");
#endif
    assert(file);
    const uint64_t in_file = UINT64_C(0x80000400);
    assert(seek_file(file, (int64_t)in_file, SEEK_SET) == 0);
    assert(fread(expect, 1, sizeof expect, file) == sizeof expect);
    fclose(file);
    uint64_t offset = file_offset(root, "0x38f972af.fdata") + in_file;
    set_read(tls, 2, offset, sizeof expect);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0);
    assert(host.writes == 2 && !memcmp(expect, host.buffer, sizeof expect));
    assert(fsp.first_asset_read);
    const long trace_bytes = ftell(read_log);
    assert(trace_bytes > 0);
    set_read(tls, 2, offset + 1, sizeof expect);
    assert(hzn_fsp_send_sync(host.alias, tls, sizeof tls).result == 0 && tls[10] == 0);
    assert(ftell(read_log) == trace_bytes);
    rewind(read_log);
    char trace[256];
    assert(fgets(trace, sizeof trace, read_log));
    assert(strcmp(trace, "Pokopia RomFS first asset read "
                        "path=\"motor_asset_re/KIDS_PLATFORM_OUNCE/0x38f972af.fdata\" "
                        "file_offset=0x80000400 bytes=32\n") == 0);
    assert(fgetc(read_log) == EOF);
    hzn_fsp_set_read_log(&fsp, NULL);
    fclose(read_log);

    set_read(tls, 2, hzn_romfs_size() - 2, 3);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0x177a02);
    assert(host.writes == 3);
    set_domain(tls, 2, 50, false);
    uint32_t unchanged[64];
    memcpy(unchanged, tls, sizeof unchanged);
    HznFspReply reply = hzn_fsp_send_sync(&fsp, tls, sizeof tls);
    assert(reply.kind == HZN_FSP_BOUNDARY && reply.command == 50 && reply.object_id == 2);
    assert(strcmp(reply.object_type, "IStorage") == 0 && !memcmp(tls, unchanged, sizeof tls));

    FILE *failure_log = temporary_log();
    int bad_length = snprintf(path, sizeof path, "%s/LI_C0_R.IDX", root);
    assert(bad_length > 0 && (size_t)bad_length < sizeof path);
    assert(!hzn_romfs_verify_logged(path, failure_log));
    rewind(failure_log);
    assert(fgets(trace, sizeof trace, failure_log));
    assert(strstr(trace, "Pokopia RomFS verify failed path=\"LD_C0_R.BIN\" operation=open errno=") == trace);
    fclose(failure_log);

    puts("PASS FSP RomFS metadata, nonsecure payload beyond 2 GiB, and verification diagnostics");
    return 0;
}
