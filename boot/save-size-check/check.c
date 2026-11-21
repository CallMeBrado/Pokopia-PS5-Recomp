#define _CRT_SECURE_NO_WARNINGS
#define main original_applet_test
#include "../../horizon/test_applet.c"
#undef main
#include "../../horizon/save_store.h"

#include <direct.h>
#include <windows.h>

static unsigned lookups;

static uint32_t get_save_size(void *opaque, uint64_t uid_hi, uint64_t uid_lo, int64_t *normal, int64_t *journal)
{
    ++lookups;
    return hzn_save_account_size(opaque, uid_hi, uid_lo, normal, journal);
}

static void set_size_request(uint32_t tls[64], uint32_t app, uint8_t type)
{
    set_domain_call(tls, app, 26);
    tls[1] = 18;
    tls[4] = 0x280001;
    tls[12] = type;
    const uint64_t uid[] = {HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1};
    memcpy(tls + 14, uid, sizeof uid);
}

static void check_sizes(const uint32_t tls[64], uint32_t result, int64_t normal, int64_t journal)
{
    assert(tls[0] == 0 && tls[1] == 16 && tls[4] == 0 && tls[8] == SFCO && tls[10] == result);
    int64_t actual[2];
    memcpy(actual, tls + 12, sizeof actual);
    assert(actual[0] == normal && actual[1] == journal);
}

static void overwrite(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file && fputs(text, file) >= 0 && !fclose(file));
}

int main(void)
{
    assert(original_applet_test() == 0);
    char root[96];
    snprintf(root, sizeof root, "save-size-%lu-%lu", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    HznApplet applet;
    uint32_t tls[64], original[64];
    hzn_applet_init(&applet, 0x51);
    applet.get_save_size = get_save_size;
    applet.save_opaque = root;
    applet.save_data_limit = HZN_SAVE_ACCOUNT_DATA_LIMIT;
    applet.save_journal_limit = HZN_SAVE_ACCOUNT_JOURNAL_LIMIT;
    set_control(tls, 0);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    set_open(tls, 0xffff8001);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    const uint32_t proxy = read_returned_id(tls);
    set_domain_call(tls, proxy, 20);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    const uint32_t app = read_returned_id(tls);

    set_size_request(tls, app, 1);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    check_sizes(tls, 0x7d402, 0, 0);
    assert(GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES && lookups == 1);
    assert(!hzn_save_ensure_account(root, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1));
    set_size_request(tls, app, 1);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    check_sizes(tls, 0, HZN_SAVE_ACCOUNT_DATA_LIMIT, HZN_SAVE_ACCOUNT_JOURNAL_LIMIT);
    assert(lookups == 2);

    char account[HZN_SAVE_PATH_MAX], meta[HZN_SAVE_PATH_MAX + sizeof ".meta"];
    assert(!hzn_save_open_account(root, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1, account));
    snprintf(meta, sizeof meta, "%s.meta", account);
    overwrite(meta, "journal=45580287\r\nsize=19870951\r\n");
    set_size_request(tls, app, 1);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    check_sizes(tls, 0, 19870951, 45580287);

    overwrite(meta, "size=9223372036854775808\njournal=1\n");
    set_size_request(tls, app, 1);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    check_sizes(tls, 0x3e802, 0, 0);
    overwrite(meta, "size=123\nsize=456\njournal=789\n");
    set_size_request(tls, app, 1);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    check_sizes(tls, 0x3e802, 0, 0);

    set_size_request(tls, app, 1);
    tls[4] = 0x270001;
    memcpy(original, tls, sizeof tls);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0xe801);
    assert(!memcmp(original, tls, sizeof tls) && lookups == 5);
    set_size_request(tls, app, 3);
    memcpy(original, tls, sizeof tls);
    HznAppletReply reply = hzn_applet_send_sync(&applet, tls, sizeof tls);
    assert(reply.kind == HZN_APPLET_BOUNDARY && reply.command == 26);
    assert(!memcmp(original, tls, sizeof tls) && lookups == 5);
    puts("PASS applet GetSaveDataSize reads real account metadata, refuses malformed data, and leaves missing saves absent");

    set_domain_call(tls, app, 28);
    memcpy(original, tls, sizeof tls);
    assert(hzn_applet_send_sync(&applet, tls, 63).result == 0xe801 && !memcmp(original, tls, sizeof tls));
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    check_sizes(tls, 0, HZN_SAVE_ACCOUNT_DATA_LIMIT, HZN_SAVE_ACCOUNT_JOURNAL_LIMIT);
    assert(lookups == 5);
    puts("PASS applet GetSaveDataSizeMax reports both fixed host limits without reading or changing save metadata");

    char parent[HZN_SAVE_PATH_MAX];
    snprintf(parent, sizeof parent, "%s/account", root);
    assert(!remove(meta) && !_rmdir(account) && !_rmdir(parent) && !_rmdir(root));
    return 0;
}
