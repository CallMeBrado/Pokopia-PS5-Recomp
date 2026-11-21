#define _CRT_SECURE_NO_WARNINGS
#include "../../gfx-bridge/fsp.h"
#include "../../horizon/account_identity.h"
#include "../../horizon/cmif.h"
#include "../../horizon/save_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static void set_open(uint32_t tls[64], uint64_t title, uint64_t uid, uint8_t space)
{
    memset(tls, 0, 256);
    tls[0] = 6;
    tls[1] = 0x1e;
    tls[4] = 0x00580001;
    tls[5] = 1;
    tls[8] = HZN_SFCI;
    tls[9] = 1;
    tls[10] = 51;
    tls[12] = space;
    memcpy(tls + 14, &title, 8);
    memcpy(tls + 16, &uid, 8);
    uint64_t uid_lo = HZN_OFFLINE_UID_WORD1;
    memcpy(tls + 18, &uid_lo, 8);
    tls[22] = 1;
}


static char guest_buffer[0x301];
static const uint64_t guest_address = UINT64_C(0x7c12340000);

static const void *read_guest(void *unused, uint64_t address, size_t size)
{
    (void)unused;
    return address == guest_address && size <= sizeof guest_buffer ? guest_buffer : NULL;
}

static void *write_guest(void *unused, uint64_t address, size_t size)
{
    (void)unused;
    return address == guest_address && size <= sizeof guest_buffer ? guest_buffer : NULL;
}

static void set_domain(uint32_t tls[64], uint32_t id, uint32_t command)
{
    memset(tls, 0, 256);
    tls[0] = 6;
    tls[1] = 12;
    tls[4] = 0x100001;
    tls[5] = id;
    tls[8] = HZN_SFCI;
    tls[10] = command;
}

static void set_path_command(uint32_t tls[64], uint32_t command, const char *path)
{
    set_domain(tls, 3, command);
    tls[0] |= 1 << 16;
    tls[2] = (0x301 << 16) | ((guest_address >> 32 & 15) << 12) | ((guest_address >> 36 & 7) << 6);
    tls[3] = (uint32_t)guest_address;
    if (command == 8) {
        tls[1] = 13;
        tls[4] = 0x140001;
        tls[12] = 1;
    }
    memset(guest_buffer, 0, sizeof guest_buffer);
    strcpy(guest_buffer, path);
}

int main(void)
{
    char root[96], collision[96], path[HZN_SAVE_PATH_MAX], name[HZN_SAVE_PATH_MAX + 32];
    snprintf(root, sizeof root, "store-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    snprintf(collision, sizeof collision, "%s-collision", root);
    HznFsp fsp;
    hzn_fsp_init(&fsp, NULL, write_guest, NULL, NULL);
    hzn_fsp_set_save_root(&fsp, root, read_guest);
    fsp.domain = fsp.root_open = fsp.process_set = true;
    uint32_t tls[64];
    set_open(tls, 0, HZN_OFFLINE_UID_WORD0, 1);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0x7d402);
    assert(hzn_save_ensure_account(NULL, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1) == 0x2ee202);
    assert(hzn_save_ensure_account(root, 0, 0) == 0x2ee202);
    assert(hzn_save_ensure_account(root, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1) == 0);
    assert(hzn_save_open_account(root, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1, path) == 0);
    assert(strstr(path, "/account/0100000000000000726573757472786e"));
    snprintf(name, sizeof name, "%s.meta", path);
    FILE *file = fopen(name, "rb");
    assert(file);
    char metadata[224] = {0};
    assert(fread(metadata, 1, sizeof metadata, file) == 187);
    assert(!fclose(file));
    assert(!strcmp(metadata, "size=67108864\njournal=67108864\nowner=400a3a01ee4c000\n"
                             "attr=00c0e41ea0a300040100000000000000726573757472786e00000000000000000100000000000000000000000000000000000000000000000000000000000000\n"));
    snprintf(name, sizeof name, "%s/sentinel", path);
    file = fopen(name, "wb");
    assert(file && fputs("existing", file) >= 0 && !fclose(file));
    assert(hzn_save_ensure_account(root, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1) == 0);
    file = fopen(name, "rb");
    assert(file && fgetc(file) == 'e' && !fclose(file));
    assert(hzn_save_ensure_account(root, 2, 3) == 0);
    assert(hzn_save_open_account(root, 2, 3, path) == 0 && strstr(path, "/02000000000000000300000000000000"));
    file = fopen(collision, "wb");
    assert(file && !fclose(file));
    assert(hzn_save_ensure_account(collision, 2, 3) == 0x3e802);
    assert(DeleteFileA(collision));
    assert(CreateDirectoryA(collision, NULL));
    snprintf(name, sizeof name, "%s/account", collision);
    file = fopen(name, "wb");
    assert(file && !fclose(file));
    assert(hzn_save_ensure_account(collision, 2, 3) == 0x3e802);
    assert(hzn_save_open_account(collision, 2, 3, path) == 0x3e802);
    assert(DeleteFileA(name));
    assert(RemoveDirectoryA(collision));

    set_open(tls, 0, HZN_OFFLINE_UID_WORD0, 1);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0 && tls[12] == 2);
    set_open(tls, HZN_SAVE_TITLE_ID, HZN_OFFLINE_UID_WORD0, 1);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0 && tls[12] == 3);
    set_open(tls, 0, HZN_OFFLINE_UID_WORD0, 1);
    tls[4] = 0x00540001;
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0xe801);
    set_open(tls, 0, HZN_OFFLINE_UID_WORD0, 2);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0x7d402);
    set_open(tls, 0, 2, 1);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0x7d402);
    set_open(tls, 1, HZN_OFFLINE_UID_WORD0, 1);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0x2ee202);
    memset(tls, 0, sizeof tls);
    tls[0] = 6;
    tls[1] = 12;
    tls[4] = 0x100001;
    tls[5] = 2;
    tls[8] = HZN_SFCI;
    tls[9] = 1;
    tls[10] = 9;
    HznFspReply boundary = hzn_fsp_send_sync(&fsp, tls, sizeof tls);
    assert(boundary.kind == HZN_FSP_BOUNDARY && boundary.command == 9 && !strcmp(boundary.object_type, "IFileSystem"));
    tls[4] = 2;
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0);
    assert(fsp.save_objects[0] == 0 && fsp.save_objects[1] == 3);

    set_domain(tls, 3, 7);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0xe801);
    set_path_command(tls, 7, "/");
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[4] == 0 && tls[10] == 0 && tls[12] == 0);
    set_path_command(tls, 7, "/sentinel");
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[4] == 0 && tls[10] == 0 && tls[12] == 1);
    set_path_command(tls, 7, "/G00/CMNDAT.BIN");
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0x202);
    set_path_command(tls, 7, "/../sentinel");
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0x2ee202);
    set_path_command(tls, 7, "/sentinel");
    memset(guest_buffer, 'x', sizeof guest_buffer);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0xcc01);
    assert(hzn_save_open_account(root, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1, path) == 0);
    snprintf(name, sizeof name, "%s/G00", path);
    assert(CreateDirectoryA(name, NULL));
    set_path_command(tls, 7, "/G00");
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0 && tls[12] == 0);
    assert(RemoveDirectoryA(name));

    set_path_command(tls, 8, "/G00/CMNDAT.BIN");
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0x202);
    set_path_command(tls, 8, "/../sentinel");
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0x2ee202);
    set_path_command(tls, 8, "/sentinel");
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0 && tls[12] == 4);
    set_domain(tls, 4, 4);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0 && tls[12] == 8 && tls[13] == 0);
    memset(tls, 0, sizeof tls);
    tls[0] = 0x01000006;
    tls[1] = 18;
    tls[2] = 16;
    tls[3] = (uint32_t)guest_address;
    tls[4] = 1 | (uint32_t)((guest_address >> 36) << 2) | (uint32_t)((guest_address >> 32) & 15) << 28;
    tls[8] = 0x280001;
    tls[9] = 4;
    tls[12] = HZN_SFCI;
    tls[18] = 2;
    tls[20] = 16;
    memset(guest_buffer, 0, sizeof guest_buffer);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0 && tls[12] == 6);
    assert(!strcmp(guest_buffer, "isting"));
    set_domain(tls, 4, 0);
    tls[4] = 2;
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0);
    set_domain(tls, 4, 4);
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0xf601);
    hzn_fsp_close(&fsp);
    puts("PASS FSP isolated save entry type, missing save, traversal rejection, open, size, read and close");
}


