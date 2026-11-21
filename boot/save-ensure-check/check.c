#define _CRT_SECURE_NO_WARNINGS
#include "overlay/gfx-bridge/fsp.h"
#include "overlay/horizon/account_identity.h"
#include "overlay/horizon/cmif.h"
#include "overlay/horizon/save_store.h"

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

int main(void)
{
    char root[96], collision[96], path[HZN_SAVE_PATH_MAX], name[HZN_SAVE_PATH_MAX + 32];
    snprintf(root, sizeof root, "store-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    snprintf(collision, sizeof collision, "%s-collision", root);
    HznFsp fsp;
    hzn_fsp_init(&fsp, NULL, NULL, NULL, NULL);
    hzn_fsp_set_save_root(&fsp, root);
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
    tls[10] = 7;
    HznFspReply boundary = hzn_fsp_send_sync(&fsp, tls, sizeof tls);
    assert(boundary.kind == HZN_FSP_BOUNDARY && boundary.command == 7 && !strcmp(boundary.object_type, "IFileSystem"));
    tls[4] = 2;
    assert(hzn_fsp_send_sync(&fsp, tls, sizeof tls).result == 0 && tls[10] == 0);
    assert(fsp.save_objects[0] == 0 && fsp.save_objects[1] == 3);
    puts("Save account metadata, idempotent persistence, collisions and exact FSP cmd51 passed");
}


