/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sm.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    HznSm sm;
    uint32_t session, other;
    assert(hzn_sm_init(&sm, 0x51, 0x10000, 0x20000));
    assert(hzn_sm_connect(&sm, "fsp-srv", sizeof "fsp-srv", &session) == 0xf201);
    assert(session == 0);
    assert(hzn_sm_connect(&sm, "sm:", 3, &session) == 0xf201);
    assert(hzn_sm_connect(&sm, "sm:", sizeof "sm:", &session) == 0);
    assert(hzn_sm_connect(&sm, "sm:", sizeof "sm:", &other) == 0);
    assert(session == 0x10000 && other == 0x10001);
    assert(hzn_sm_owns_handle(&sm, session));

    uint32_t tls[64] = { 0x10, 0x80000000, 1, 0xdeadbeef, 0xfeedface };
    HznSmReply reply = hzn_sm_send_sync(&sm, session, tls, 19);
    assert(reply.kind == HZN_SM_REPLY && reply.result == 0xe801);
    assert(!sm.sessions[0].registered_pid);
    assert(tls[0] == 0x10 && tls[1] == 0x80000000);

    reply = hzn_sm_send_sync(&sm, session, tls, sizeof tls);
    assert(reply.kind == HZN_SM_REPLY && reply.result == 0);
    assert(tls[0] == 0 && tls[1] == 1 && tls[2] == 0);
    assert(sm.sessions[0].registered_pid == 0x51);
    assert(!sm.sessions[1].registered_pid);

    /* An exact-width service name is still terminated for the caller. */
    tls[0] = 0x11;
    tls[1] = 2;
    memcpy(tls + 2, "abcdefgh", 8);
    uint32_t request[64];
    memcpy(request, tls, sizeof request);
    reply = hzn_sm_send_sync(&sm, session, tls, sizeof tls);
    assert(reply.kind == HZN_SM_SERVICE_BOUNDARY && reply.command == 1);
    assert(strcmp(reply.service, "abcdefgh") == 0);
    assert(!memcmp(request, tls, sizeof request));
    reply = hzn_sm_send_sync(&sm, other, tls, sizeof tls);
    assert(reply.kind == HZN_SM_COMMAND_BOUNDARY && reply.command == 1);
    assert(!memcmp(request, tls, sizeof request));

    tls[2] = 0;
    reply = hzn_sm_send_sync(&sm, session, tls, sizeof tls);
    assert(reply.kind == HZN_SM_REPLY && reply.result == 0);
    assert(tls[0] == 0 && tls[1] == 1 && tls[2] == 0xc15);

    tls[0] = 0x14;
    tls[1] = 0x80000000;
    tls[2] = 1;
    reply = hzn_sm_send_sync(&sm, session, tls, sizeof tls);
    assert(reply.kind == HZN_SM_REPLY && reply.result == 0);
    assert(!sm.sessions[0].registered_pid);

    tls[0] = 15;
    tls[1] = 0;
    reply = hzn_sm_send_sync(&sm, session, tls, sizeof tls);
    assert(reply.kind == HZN_SM_REPLY && reply.result == 0);
    assert(tls[0] == 0 && tls[1] == 0);
    assert(hzn_sm_close_handle(&sm, session) == 0);
    assert(hzn_sm_owns_handle(&sm, session));
    assert(hzn_sm_close_handle(&sm, session) == 0xe401);
    assert(hzn_sm_send_sync(&sm, session, tls, sizeof tls).result == 0xe401);
    assert(hzn_sm_connect(&sm, "sm:", sizeof "sm:", &other) == 0);
    assert(other == 0x10002);
    uint32_t lm;
    assert(hzn_sm_move_service(&sm, tls, 19, &lm).result == 0xe801);
    assert(lm == 0 && !hzn_sm_owns_handle(&sm, 0x10003));
    assert(hzn_sm_move_service(&sm, tls, sizeof tls, &lm).result == 0);
    assert(lm == 0x10003 && hzn_sm_owns_handle(&sm, lm));
    const uint32_t moved[] = { 0, 0x80000001, 0x20, lm, 0 };
    assert(!memcmp(tls, moved, sizeof moved));
    assert(hzn_sm_connect(&sm, "sm:", sizeof "sm:", &other) == 0 && other == 0x10004);
    puts("PASS Horizon sm: registration, service moves, and handle lifetime");
    return 0;
}
