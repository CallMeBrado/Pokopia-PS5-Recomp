/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bsd_ssl.h"
#include "cmif.h"
#include <assert.h>
#include <stdio.h>

static HznBsdSsl clones[28];
static uint32_t count;

static uint32_t clone_session(void *opaque, const HznBsdSsl *source)
{
    assert(!opaque && count < sizeof clones / sizeof clones[0]);
    hzn_bsd_ssl_clone(&clones[count], source);
    return 0x1001a + count++;
}

static void control(HznBsdSsl *session, uint32_t command, uint32_t output)
{
    uint32_t tls[64] = {7, command == 4 ? 9 : 8, 0, 0, HZN_SFCI, 1, command, 0, 1};
    const HznBsdSslReply reply = hzn_bsd_ssl_send_sync(session, (uint8_t *)tls, sizeof tls);
    assert(reply.handled && !reply.result && tls[4] == HZN_SFCO);
    if (command == 2 || command == 4)
        assert(tls[2] == 0x20 && tls[3] == output);
    else
        assert(tls[8] == output);
}

int main(void)
{
    const HznBsdSslConfig config = {.guest_pid = 0x51, .clone_session = clone_session};
    HznBsdSsl monitor;
    assert(hzn_bsd_ssl_init(&monitor, HZN_NET_BOOT_BSD, &config));
    control(&monitor, 4, 0x1001a);
    uint32_t close_transport[64] = {2, 0};
    HznBsdSslReply reply = hzn_bsd_ssl_send_sync(&monitor, (uint8_t *)close_transport, sizeof close_transport);
    assert(reply.handled && !reply.result && !close_transport[0]);
    hzn_bsd_ssl_close(&monitor);

    uint32_t start_monitoring[64] = {6, 0x8000000a, 1, 0, 0, 0, 0, 0, HZN_SFCI, 1, 1, 0};
    reply = hzn_bsd_ssl_send_sync(&clones[0], (uint8_t *)start_monitoring, sizeof start_monitoring);
    assert(reply.handled && !reply.result && start_monitoring[4] == HZN_SFCO && !start_monitoring[6]);

    HznBsdSsl client;
    assert(hzn_bsd_ssl_init(&client, HZN_NET_BOOT_BSD, &config));
    control(&client, 3, 0x8000);
    control(&client, 0, 1);
    for (uint32_t i = 1; i < 28; ++i)
        control(i == 1 ? &client : &clones[i - 1], 2, 0x1001a + i);
    uint32_t bsd_config[64] = {
        6, 0x80000010, 1, 0, 0, 0, 0, 0, 0x00200001, 1, 0, 0, HZN_SFCI, 1, 36, 0, 0x1000a,
    };
    reply = hzn_bsd_ssl_send_sync(&clones[27], (uint8_t *)bsd_config, sizeof bsd_config);
    assert(reply.handled && !reply.result && bsd_config[8] == HZN_SFCO && !bsd_config[10] && !bsd_config[12]);
    hzn_bsd_ssl_close(&client);
    for (uint32_t i = 0; i < count; ++i)
        hzn_bsd_ssl_close(&clones[i]);

    HznBsdSsl ssl;
    assert(hzn_bsd_ssl_init(&ssl, HZN_NET_BOOT_SSL, &config));
    control(&ssl, 3, 0x8000);
    control(&ssl, 0, 1);
    uint32_t version[64] = {6, 13, 0, 0, 0x00140001, 1, 0, 0, HZN_SFCI, 1, 5, 0, 4};
    reply = hzn_bsd_ssl_send_sync(&ssl, (uint8_t *)version, sizeof version);
    assert(reply.handled && !reply.result && version[8] == HZN_SFCO && !version[10]);
    hzn_bsd_ssl_close(&ssl);
    puts("PASS BSD 27 live clones, monitoring and configuration; SSL interface version.");
}
