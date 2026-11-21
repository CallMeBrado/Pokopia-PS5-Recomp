#include "fsp.h"
#include "cmif.h"
#include "save_store.h"
#include "account_identity.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { PATH_BYTES = 0x301 };
static const uint64_t address = UINT64_C(0x7101234000);
static char guest_path[PATH_BYTES];

static const void *read_guest(void *opaque, uint64_t source, size_t size)
{
    (void)opaque;
    return source == address && size <= sizeof guest_path ? guest_path : NULL;
}

static uint32_t get_property(HznFsp *fsp, uint32_t command, const char *path, uint64_t *value)
{
    uint32_t tls[64] = {0};
    memset(guest_path, 0, sizeof guest_path);
    memcpy(guest_path, path, strlen(path));
    tls[0] = 0x00010006;
    tls[1] = 8;
    tls[2] = PATH_BYTES << 16 | (uint32_t)((address >> 32) & 15) << 12 |
             (uint32_t)((address >> 36) & 7) << 6;
    tls[3] = (uint32_t)address;
    tls[4] = 0x100001;
    tls[5] = 4;
    tls[8] = HZN_SFCI;
    tls[9] = 1;
    tls[10] = command;
    const HznFspReply result = hzn_fsp_send_sync(fsp, tls, sizeof tls);
    assert(result.kind == HZN_FSP_REPLY && result.result == 0 && tls[8] == HZN_SFCO);
    if (!tls[10])
        *value = command == 7 ? tls[12] : hzn_cmif_read64((const uint8_t *)(tls + 12));
    return tls[10];
}

static uint32_t create_or_open(HznFsp *fsp, uint32_t command, const char *path, uint32_t *object)
{
    uint32_t tls[64] = {0};
    memset(guest_path, 0, sizeof guest_path);
    memcpy(guest_path, path, strlen(path));
    tls[0] = 0x10006;
    tls[1] = 16;
    tls[2] = PATH_BYTES << 16 | (uint32_t)((address >> 32) & 15) << 12 |
             (uint32_t)((address >> 36) & 7) << 6;
    tls[3] = (uint32_t)address;
    const uint32_t args = command == 0 ? 16 : command == 8 ? 4 : 0;
    tls[4] = (16 + args) << 16 | 1;
    tls[5] = 4;
    tls[8] = HZN_SFCI;
    tls[9] = 1;
    tls[10] = command;
    tls[12] = command == 8 ? 6 : 0;
    tls[14] = command == 0 ? 4 : 0;
    const HznFspReply reply = hzn_fsp_send_sync(fsp, tls, sizeof tls);
    assert(reply.kind == HZN_FSP_REPLY && reply.result == 0 && tls[8] == HZN_SFCO);
    if (object && !tls[10])
        *object = tls[12];
    return tls[10];
}

static void write_and_commit(HznFsp *fsp, uint32_t id)
{
    memcpy(guest_path, "abcd", 4);
    uint32_t tls[64] = {0};
    tls[0] = 0x100006;
    tls[1] = 0x12;
    tls[2] = 4;
    tls[3] = (uint32_t)address;
    tls[4] = 1 | (uint32_t)((address >> 36) & 7) << 2 | (uint32_t)((address >> 32) & 15) << 28;
    tls[8] = 0x280001;
    tls[9] = id;
    tls[12] = HZN_SFCI;
    tls[13] = 1;
    tls[14] = 1;
    tls[16] = 1;
    tls[20] = 4;
    assert(hzn_fsp_send_sync(fsp, tls, sizeof tls).result == 0 && tls[8] == HZN_SFCO && tls[10] == 0);
    memset(tls, 0, sizeof tls);
    tls[0] = 6;
    tls[1] = 12;
    tls[4] = 2;
    tls[5] = id;
    assert(hzn_fsp_send_sync(fsp, tls, sizeof tls).result == 0 && tls[8] == HZN_SFCO && tls[10] == 0);
    memset(tls, 0, sizeof tls);
    tls[0] = 6;
    tls[1] = 12;
    tls[4] = 0x100001;
    tls[5] = 4;
    tls[8] = HZN_SFCI;
    tls[9] = 1;
    tls[10] = 10;
    assert(hzn_fsp_send_sync(fsp, tls, sizeof tls).result == 0 && tls[8] == HZN_SFCO && tls[10] == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *root = argv[1];
    assert(hzn_save_ensure_account(root, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1) == 0);
    char mounted[HZN_SAVE_PATH_MAX], file_path[HZN_SAVE_PATH_MAX + 24];
    assert(hzn_save_open_account(root, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1, mounted) == 0);
    snprintf(file_path, sizeof file_path, "%s/test.bin", mounted);
    FILE *file = NULL;
    assert(fopen_s(&file, file_path, "wb") == 0);
    assert(fputc(0x3b, file) == 0x3b && fclose(file) == 0);
    HznFsp fsp;
    hzn_fsp_init(&fsp, NULL, NULL, NULL, NULL);
    hzn_fsp_set_save_root(&fsp, root, read_guest);
    fsp.domain = fsp.root_open = fsp.process_set = true;
    fsp.save_objects[0] = 4;
    uint64_t type = 99;
    assert(get_property(&fsp, 7, "/", &type) == 0 && type == 0);
    assert(get_property(&fsp, 7, "/test.bin", &type) == 0 && type == 1);
    assert(get_property(&fsp, 7, "/missing.bin", &type) == 0x202 && type == 1);
    assert(get_property(&fsp, 11, "/", &type) == 0 && type == HZN_SAVE_ACCOUNT_DATA_LIMIT - 1);
    uint32_t id = 0;
    fsp.next_object_id = 5;
    assert(create_or_open(&fsp, 2, "/fresh", NULL) == 0);
    assert(create_or_open(&fsp, 0, "/fresh/config", NULL) == 0);
    assert(create_or_open(&fsp, 8, "/fresh/config", &id) == 0 && id == 5);
    write_and_commit(&fsp, id);
    assert(get_property(&fsp, 11, "/", &type) == 0 && type == HZN_SAVE_ACCOUNT_DATA_LIMIT - 5);
    assert(hzn_save_open_read(root, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1, "/fresh/config", &file) == 0);
    char payload[4];
    assert(fread(payload, 1, sizeof payload, file) == sizeof payload && !memcmp(payload, "abcd", 4));
    assert(fclose(file) == 0);
    hzn_fsp_close(&fsp);
    puts("PASS save IPC create directory/file, open mode 6, write+flush, close, commit, readback and quota");
    return 0;
}
