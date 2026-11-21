/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cmif.h"
#include "pctl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_control(uint32_t tls[64], uint32_t command)
{
    memset(tls, 0, 256);
    tls[0] = 7;
    tls[1] = 8;
    tls[4] = HZN_SFCI;
    tls[5] = 1;
    tls[6] = command;
}

static void set_create(uint32_t tls[64], uint32_t command)
{
    memset(tls, 0, 256);
    tls[0] = 6;
    tls[1] = 0x8000000e;
    tls[2] = 1;
    tls[8] = 0x180001;
    tls[9] = 1;
    tls[12] = HZN_SFCI;
    tls[13] = 1;
    tls[14] = command;
}

static void set_child_call(uint32_t tls[64], uint32_t id, uint32_t command)
{
    memset(tls, 0, 256);
    tls[0] = 6;
    tls[1] = 12;
    tls[4] = 0x100001;
    tls[5] = id;
    tls[8] = HZN_SFCI;
    tls[9] = 1;
    tls[10] = command;
}

static void set_domain_close(uint32_t tls[64], uint32_t id)
{
    memset(tls, 0, 256);
    tls[0] = 4;
    tls[1] = 8;
    tls[4] = 2;
    tls[5] = id;
}

int main(void)
{
    HznPctl pctl, other;
    uint32_t tls[64];
    hzn_pctl_init(&pctl);
    hzn_pctl_init(&other);

    set_control(tls, 3);
    assert(hzn_pctl_send_sync(&pctl, tls, sizeof tls).result == 0);
    assert(tls[4] == HZN_SFCO && tls[8] == 0x8000 && !pctl.domain);
    set_control(tls, 0);
    assert(hzn_pctl_send_sync(&pctl, tls, sizeof tls).result == 0);
    assert(tls[4] == HZN_SFCO && tls[8] == 1 && pctl.domain && pctl.root_open);
    assert(!other.domain && !other.root_open);

    set_create(tls, 1);
    assert(hzn_pctl_send_sync(&pctl, tls, 75).result == 0xe801);
    assert(tls[0] == 6 && pctl.next_object_id == 2);
    tls[16] = 1;
    assert(hzn_pctl_send_sync(&pctl, tls, sizeof tls).result == 0xe801);
    assert(pctl.next_object_id == 2);
    tls[16] = 0;
    assert(hzn_pctl_send_sync(&pctl, tls, sizeof tls).result == 0);
    assert(tls[1] == 13 && tls[4] == 1 && tls[8] == HZN_SFCO && tls[10] == 0 && tls[12] == 2);
    assert(pctl.objects[0].id == 2 && !pctl.objects[0].initialized);

    set_create(tls, 0);
    assert(hzn_pctl_send_sync(&pctl, tls, sizeof tls).result == 0);
    assert(tls[12] == 3 && pctl.objects[1].id == 3 && pctl.objects[1].initialized);
    set_domain_close(tls, 1);
    assert(hzn_pctl_send_sync(&pctl, tls, sizeof tls).result == 0);
    assert(!pctl.root_open && pctl.domain && pctl.objects[0].id == 2);
    set_create(tls, 1);
    assert(hzn_pctl_send_sync(&pctl, tls, sizeof tls).result == 0);
    assert(tls[10] == 0xf601 && pctl.next_object_id == 4);

    set_child_call(tls, 2, 1);
    assert(hzn_pctl_send_sync(&pctl, tls, 55).result == 0xe801 && !pctl.objects[0].initialized);
    assert(hzn_pctl_send_sync(&pctl, tls, sizeof tls).result == 0);
    assert(tls[1] == 12 && tls[4] == 0 && tls[8] == HZN_SFCO && tls[10] == 0);
    assert(pctl.objects[0].initialized);

    uint32_t saved[64];
    set_child_call(tls, 2, 1001);
    memcpy(saved, tls, sizeof saved);
    HznPctlReply reply = hzn_pctl_send_sync(&pctl, tls, sizeof tls);
    assert(reply.kind == HZN_PCTL_BOUNDARY && reply.command == 1001 && reply.object_id == 2);
    assert(strcmp(reply.object_type, "IParentalControlService") == 0);
    assert(!memcmp(saved, tls, sizeof saved));

    set_domain_close(tls, 2);
    assert(hzn_pctl_send_sync(&pctl, tls, sizeof tls).result == 0);
    assert(tls[10] == 0 && !pctl.objects[0].id);
    set_child_call(tls, 2, 1);
    assert(hzn_pctl_send_sync(&pctl, tls, sizeof tls).result == 0);
    assert(tls[10] == 0xf601 && !pctl.objects[0].initialized);

    puts("PASS Horizon pctl: domain controls, factory objects and child initialization");
    return 0;
}
