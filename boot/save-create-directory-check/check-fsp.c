#define _CRT_SECURE_NO_WARNINGS
#include "../../gfx-bridge/fsp.h"
#include "../../horizon/cmif.h"
#include "../../horizon/save_store.h"
#include "../../horizon/account_identity.h"
#include <assert.h>
#include <direct.h>
#include <string.h>
#include <windows.h>

enum { PATH_BYTES = 0x301, SAVE_ID = 4 };
static const uint64_t path_address = UINT64_C(0x7101234000);
static const uint64_t data_address = UINT64_C(0x7831234000);
static char guest_path[PATH_BYTES];
static const uint8_t data[] = {0x28, 0x19, 0x7f, 0xb0, 0x31};

static const void *read_guest(void *opaque, uint64_t address, size_t size)
{
    (void)opaque;
    if (address == path_address && size == sizeof guest_path)
        return guest_path;
    if (address == data_address && size <= sizeof data)
        return data;
    return NULL;
}

static void set_path(uint32_t tls[64], const char *path, uint32_t command, uint32_t in_size)
{
    memset(tls, 0, 256);
    memset(guest_path, 0, sizeof guest_path);
    assert(strlen(path) < sizeof guest_path);
    memcpy(guest_path, path, strlen(path));
    tls[0] = 0x00010006;
    tls[1] = 0xc + (in_size - 16) / 4;
    tls[2] = PATH_BYTES << 16 | (uint32_t)((path_address >> 32) & 15) << 12 |
             (uint32_t)((path_address >> 36) & 7) << 6;
    tls[3] = (uint32_t)path_address;
    tls[4] = in_size << 16 | 1;
    tls[5] = SAVE_ID;
    tls[8] = HZN_SFCI;
    tls[9] = 1;
    tls[10] = command;
}

static uint32_t send_fixture(HznFsp *fsp, uint32_t tls[64])
{
    const HznFspReply response = hzn_fsp_send_sync(fsp, tls, 256);
    assert(response.kind == HZN_FSP_REPLY && response.result == 0 && tls[8] == HZN_SFCO);
    return tls[10];
}

static uint32_t write_data(HznFsp *fsp, uint32_t file)
{
    uint32_t tls[64] = {0};
    tls[0] = 0x00100006;
    tls[1] = 0x12;
    tls[2] = sizeof data;
    tls[3] = (uint32_t)data_address;
    tls[4] = (uint32_t)((data_address >> 32) & 15) << 28 |
             (uint32_t)((data_address >> 36) & 7) << 2 | 1;
    tls[8] = 0x280001;
    tls[9] = file;
    tls[12] = HZN_SFCI;
    tls[13] = 1;
    tls[14] = 1;
    tls[16] = 1;
    tls[20] = sizeof data;
    return send_fixture(fsp, tls);
}

int main(void)
{
    char root[96];
    snprintf(root, sizeof root, "fsp-create-%lu-%lu", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    const uint64_t hi = HZN_OFFLINE_UID_WORD0, lo = HZN_OFFLINE_UID_WORD1;
    assert(!hzn_save_ensure_account(root, hi, lo));
    HznFsp fsp;
    hzn_fsp_init(&fsp, NULL, NULL, NULL, NULL);
    hzn_fsp_set_save_root(&fsp, root, read_guest);
    fsp.domain = fsp.root_open = fsp.process_set = true;
    fsp.save_objects[0] = SAVE_ID;

    uint32_t tls[64];
    set_path(tls, "/profiles", 2, 16);
    tls[1] = 8;
    assert(hzn_fsp_send_sync(&fsp, tls, 48).result == 0xe801);
    uint32_t type;
    assert(hzn_save_get_entry_type(root, hi, lo, "/profiles", &type) == 0x202);
    set_path(tls, "/profiles", 2, 16);
    assert(!send_fixture(&fsp, tls));
    set_path(tls, "/profiles", 2, 16);
    assert(send_fixture(&fsp, tls) == 0x402);
    set_path(tls, "/profiles/missing/child", 2, 16);
    assert(send_fixture(&fsp, tls) == 0x202);

    set_path(tls, "/profiles/test.bin", 0, 32);
    assert(!send_fixture(&fsp, tls));
    set_path(tls, "/profiles/test.bin", 0, 32);
    assert(send_fixture(&fsp, tls) == 0x402);
    set_path(tls, "/profiles/test.bin", 8, 20);
    tls[12] = 1;
    assert(!send_fixture(&fsp, tls));
    const uint32_t read_id = tls[12];
    assert(read_id && write_data(&fsp, read_id) == 0x307602);
    memset(tls, 0, sizeof tls);
    tls[0] = 6; tls[1] = 12; tls[4] = 2; tls[5] = read_id;
    assert(!send_fixture(&fsp, tls));

    set_path(tls, "/profiles/test.bin", 8, 20);
    tls[12] = 6;
    assert(!send_fixture(&fsp, tls));
    const uint32_t write_id = tls[12];
    assert(write_id && !write_data(&fsp, write_id));
    memset(tls, 0, sizeof tls);
    tls[0] = 6; tls[1] = 12; tls[4] = 2; tls[5] = write_id;
    assert(!send_fixture(&fsp, tls));

    memset(tls, 0, sizeof tls);
    tls[0] = 6; tls[1] = 12; tls[4] = 0x100001; tls[5] = SAVE_ID;
    tls[8] = HZN_SFCI; tls[9] = 1; tls[10] = 10;
    assert(!send_fixture(&fsp, tls));
    hzn_fsp_close(&fsp);
    FILE *file = NULL;
    assert(!hzn_save_open_read(root, hi, lo, "/profiles/test.bin", &file));
    uint8_t output[sizeof data];
    assert(fread(output, 1, sizeof output, file) == sizeof output && !memcmp(output, data, sizeof data));
    assert(fgetc(file) == EOF && !fclose(file));

    char mounted[HZN_SAVE_PATH_MAX], path[HZN_SAVE_PATH_MAX + 32];
    assert(!hzn_save_open_account(root, hi, lo, mounted));
    snprintf(path, sizeof path, "%s/profiles/test.bin", mounted);
    assert(!remove(path));
    snprintf(path, sizeof path, "%s/profiles", mounted);
    assert(!_rmdir(path));
    snprintf(path, sizeof path, "%s.meta", mounted);
    assert(!remove(path) && !_rmdir(mounted));
    snprintf(path, sizeof path, "%s/account", root);
    assert(!_rmdir(path) && !_rmdir(root));
    puts("PASS save CMIF command 2, CreateFile, access modes, nonsecure Write, Close, Commit and disk readback");
    return 0;
}

