/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "account.h"
#include "../horizon/account_identity.h"
#include "../horizon/cmif.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_request(uint32_t tls[64], uint32_t command, uint32_t id, bool pid)
{
    memset(tls, 0, 256);
    tls[0] = 6;
    tls[1] = 8 + (id ? 4 : 0) + (pid ? 2 : 0);
    uint32_t *raw = &tls[4];
    if (pid) {
        tls[1] |= 0x80000000u;
        tls[2] = 1;
        raw = &tls[8];
    }
    if (id) {
        raw[0] = (pid ? 24u : 16u) << 16 | 1;
        raw[1] = id;
        raw += 4;
    }
    raw[0] = HZN_SFCI;
    raw[1] = 1;
    raw[2] = command;
}

static void set_control(uint32_t tls[64], uint32_t command)
{
    set_request(tls, command, 0, false);
    tls[0] = 7;
}

static void set_user_object(uint32_t tls[64], uint32_t command, uint64_t word0, uint64_t word1)
{
    set_request(tls, command, 1, false);
    tls[1] = 16;
    tls[4] = 0x200001;
    const uint64_t uid[2] = {word0, word1};
    memcpy(tls + 12, uid, sizeof uid);
}

static void set_domain_close(uint32_t tls[64], uint32_t id)
{
    memset(tls, 0, 256);
    tls[0] = 4;
    tls[1] = 8;
    tls[4] = 2;
    tls[5] = id;
}

typedef struct {
    HznAccount *aliased;
    unsigned count;
} CloneState;

static uint32_t copy_session(void *opaque, HznAccount *account)
{
    CloneState *state = opaque;
    state->aliased = account;
    return 0x110 + ++state->count;
}

int main(void)
{
    HznAccount account;
    hzn_account_init(&account);
    uint32_t tls[64];
    static const uint32_t initialization[] = {100, 140, 160};
    for (size_t i = 0; i < sizeof initialization / sizeof initialization[0]; ++i) {
        set_request(tls, initialization[i], 0, false);
        assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0xe801);
        set_request(tls, initialization[i], 0, true);
        tls[12] = 1;
        assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0xe801);
        tls[12] = 0;
        assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0);
        assert(tls[1] == 8 && tls[4] == HZN_SFCO && tls[6] == 0);
    }
    set_request(tls, 0, 0, false);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0);
    assert(tls[4] == HZN_SFCO && tls[6] == 0 && tls[8] == 1);
    set_request(tls, 4, 0, false);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0);
    assert(tls[1] == 12 && tls[4] == HZN_SFCO && tls[6] == 0);
    assert(hzn_cmif_read64((const uint8_t *)&tls[8]) == HZN_OFFLINE_UID_WORD0);
    assert(hzn_cmif_read64((const uint8_t *)&tls[10]) == HZN_OFFLINE_UID_WORD1);

    set_control(tls, 3);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0);
    assert(tls[4] == HZN_SFCO && tls[8] == 0x8000 && !account.domain);
    set_control(tls, 0);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0);
    assert(tls[4] == HZN_SFCO && tls[8] == 1 && account.domain && account.root_open);
    set_control(tls, 2);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).kind == HZN_ACCOUNT_BOUNDARY);
    CloneState clones = {0};
    account.clone = copy_session;
    account.clone_opaque = &clones;
    assert(hzn_account_send_sync(&account, tls, 47).result == 0xe801 && !clones.count);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0);
    assert(tls[1] == 0x80000008 && tls[2] == 0x20 && tls[3] == 0x111);
    assert(clones.aliased == &account && clones.count == 1);

    set_request(tls, 160, 1, true);
    assert(hzn_account_send_sync(clones.aliased, tls, sizeof tls).result == 0);
    assert(tls[4] == 0 && tls[8] == HZN_SFCO && tls[10] == 0);
    set_request(tls, 150, 1, false);
    assert(hzn_account_send_sync(clones.aliased, tls, sizeof tls).result == 0);
    assert(tls[1] == 13 && tls[8] == HZN_SFCO && tls[10] == 0 && tls[12] == 0);

    memset(tls, 0, sizeof tls);
    tls[0] = 2;
    assert(hzn_account_send_sync(clones.aliased, tls, sizeof tls).result == 0);
    assert(tls[0] == 0 && account.domain && account.root_open);

    set_request(tls, 130, 1, false);
    uint32_t unchanged[64];
    memcpy(unchanged, tls, sizeof tls);
    HznAccountReply reply = hzn_account_send_sync(&account, tls, sizeof tls);
    assert(reply.kind == HZN_ACCOUNT_BOUNDARY && reply.command == 130 && reply.object_id == 1);
    assert(!strcmp(reply.object_type, "IAccountServiceForApplication"));
    assert(!memcmp(unchanged, tls, sizeof tls));

    set_user_object(tls, 101, 0, 0);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0 && tls[10] == 0x287c);
    set_user_object(tls, 101, HZN_OFFLINE_UID_WORD0, 0);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0 && tls[10] == 0xc87c);
    assert(account.next_object_id == 2);
    set_user_object(tls, 101, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1);
    assert(hzn_account_send_sync(clones.aliased, tls, sizeof tls).result == 0);
    assert(tls[4] == 1 && tls[8] == HZN_SFCO && tls[10] == 0 && tls[12] == 2);

    set_request(tls, 0, 2, false);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0);
    assert(tls[1] == 12 && tls[4] == 0 && tls[8] == HZN_SFCO && tls[10] == 0x1907c);
    set_request(tls, 1, 2, false);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0);
    assert(tls[1] == 14 && tls[4] == 0 && tls[8] == HZN_SFCO && tls[10] == 0);
    assert(hzn_cmif_read64((const uint8_t *)&tls[12]) == (HZN_OFFLINE_UID_WORD0 ^ HZN_OFFLINE_UID_WORD1));
    set_request(tls, 2, 2, false);
    memcpy(unchanged, tls, sizeof tls);
    reply = hzn_account_send_sync(&account, tls, sizeof tls);
    assert(reply.kind == HZN_ACCOUNT_BOUNDARY && reply.command == 2 && reply.object_id == 2);
    assert(!strcmp(reply.object_type, "IManagerForApplication") && !memcmp(unchanged, tls, sizeof tls));

    set_domain_close(tls, 2);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0 && tls[10] == 0);
    set_request(tls, 0, 2, false);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0 && tls[10] == 0xf601);
    set_user_object(tls, 101, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0 && tls[12] == 3);

    set_user_object(tls, 5, 0, 0);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0 && tls[10] == 0x287c);
    set_user_object(tls, 5, HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0);
    assert(tls[4] == 1 && tls[8] == HZN_SFCO && tls[10] == 0 && tls[12] == 4);
    set_request(tls, 1, 4, false);
    memcpy(unchanged, tls, sizeof tls);
    assert(hzn_account_send_sync(&account, tls, 111).result == 0xe801);
    assert(!memcmp(unchanged, tls, sizeof tls));
    assert(hzn_account_send_sync(&account, tls, 112).result == 0);
    assert(tls[1] == 26 && tls[4] == 0 && tls[8] == HZN_SFCO && tls[10] == 0);
    const uint8_t *profile = (const uint8_t *)(tls + 12);
    assert(hzn_cmif_read64(profile) == HZN_OFFLINE_UID_WORD0);
    assert(hzn_cmif_read64(profile + 8) == HZN_OFFLINE_UID_WORD1);
    assert(hzn_cmif_read64(profile + 16) == UINT64_C(1546300800));
    const char nickname[32] = "Player";
    assert(!memcmp(profile + 24, nickname, sizeof nickname));
    set_request(tls, 0, 4, false);
    reply = hzn_account_send_sync(&account, tls, sizeof tls);
    assert(reply.kind == HZN_ACCOUNT_BOUNDARY && reply.command == 0 && reply.object_id == 4);
    assert(!strcmp(reply.object_type, "IProfile"));

    set_domain_close(tls, 1);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0 && !account.root_open);
    set_request(tls, 0, 1, false);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0 && tls[10] == 0xf601);
    set_request(tls, 0, 3, false);
    assert(hzn_account_send_sync(&account, tls, sizeof tls).result == 0 && tls[10] == 0x1907c);

    puts("PASS acc:u0: exact PID requests, cloned handles, account manager and offline profile");
    return 0;
}
