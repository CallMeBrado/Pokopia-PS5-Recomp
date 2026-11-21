/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lm.h"

#include <assert.h>
#include <string.h>

enum { SFCI = 0x49434653, SFCO = 0x4f434653 };
static const uint64_t packet_address = UINT64_C(0x7500000200);
static const size_t packet_size = 29;

typedef struct {
    uint8_t packet[29];
    bool denied;
    unsigned reads;
} Input;

static const void *read_packet(void *opaque, uint64_t address, size_t size)
{
    Input *input = opaque;
    ++input->reads;
    if (input->denied || address != packet_address || size != packet_size)
        return NULL;
    return input->packet;
}

static void control(uint32_t *tls, uint32_t command)
{
    memset(tls, 0, 256);
    tls[0] = 7;
    tls[1] = 8;
    tls[4] = SFCI;
    tls[5] = 1;
    tls[6] = command;
}

static void logger_request(uint32_t *tls, uint32_t id, uint32_t command, bool use_a)
{
    memset(tls, 0, 256);
    tls[0] = 0x00110006;
    tls[1] = 12;
    if (use_a) {
        tls[4] = (uint32_t)packet_size;
        tls[5] = 0x200;
        tls[6] = 0x5000001c;
    } else {
        tls[2] = (uint32_t)(packet_size << 16) | 0x51c0;
        tls[3] = 0x200;
    }
    tls[8] = 0x100001;
    tls[9] = id;
    tls[12] = SFCI;
    tls[13] = 1;
    tls[14] = command;
}

static void assert_control_result(uint32_t *tls, uint32_t value)
{
    assert(tls[0] == 0 && tls[1] == 9);
    assert(tls[4] == SFCO && tls[6] == 0 && tls[8] == value);
}

static void assert_domain_result(uint32_t *tls, uint32_t result)
{
    assert(tls[0] == 0 && tls[1] == 12);
    assert(tls[4] == 0 && tls[8] == SFCO && tls[10] == result);
}

int main(void)
{
    HznLm lm;
    uint32_t tls[64];
    hzn_lm_init(&lm, 0x51);

    control(tls, 3);
    HznLmReply reply = hzn_lm_send_sync(&lm, tls, sizeof tls, NULL, NULL, NULL);
    assert(reply.kind == HZN_LM_REPLY && reply.result == 0);
    assert_control_result(tls, 0x8000);
    control(tls, 0);
    assert(hzn_lm_send_sync(&lm, tls, sizeof tls, NULL, NULL, NULL).result == 0);
    assert_control_result(tls, 1);

    memset(tls, 0, sizeof tls);
    tls[0] = 6;
    tls[1] = 0x8000000e;
    tls[2] = 1;
    tls[3] = 0xdeadbeef; /* SendPid placeholder supplied by the client. */
    tls[8] = 0x180001;
    tls[9] = 1;
    tls[12] = SFCI;
    tls[13] = 1;
    assert(hzn_lm_send_sync(&lm, tls, sizeof tls, NULL, NULL, NULL).result == 0);
    assert(tls[0] == 0 && tls[1] == 13);
    assert(tls[4] == 1 && tls[8] == SFCO && tls[10] == 0 && tls[12] == 2);
    assert(lm.loggers[0].pid == 0x51);

    Input input = { .packet = { [0x10] = 1, [0x12] = 2, [0x14] = 5,
                               [0x18] = 2, [0x19] = 3, 'a', 'b', 'c' } };
    FILE *log = NULL;
#ifdef _WIN32
    assert(tmpfile_s(&log) == 0);
#else
    log = tmpfile();
#endif
    assert(log);
    logger_request(tls, 2, 0, false);
    assert(hzn_lm_send_sync(&lm, tls, sizeof tls, read_packet, &input, log).result == 0);
    assert_domain_result(tls, 0);
    assert(ftell(log) == 0);

    input.packet[0x10] = 2;
    memcpy(input.packet + 0x1a, "def", 3);
    logger_request(tls, 2, 0, true);
    assert(hzn_lm_send_sync(&lm, tls, sizeof tls, read_packet, &input, log).result == 0);
    assert_domain_result(tls, 0);
    assert(input.reads == 2);
    rewind(log);
    char line[128];
    assert(fgets(line, sizeof line, log));
    assert(strcmp(line, "[guestlog severity=2] abcdef\n") == 0);
    fclose(log);

    input.denied = true;
    logger_request(tls, 2, 0, true);
    uint32_t request[64];
    memcpy(request, tls, sizeof request);
    assert(hzn_lm_send_sync(&lm, tls, sizeof tls, read_packet, &input, NULL).result == 0xcc01);
    assert(!memcmp(request, tls, sizeof request));

    memset(tls, 0, sizeof tls);
    tls[0] = 6;
    tls[1] = 13;
    tls[4] = 0x140001;
    tls[5] = 2;
    tls[8] = SFCI;
    tls[9] = 1;
    tls[10] = 1;
    tls[12] = 4;
    assert(hzn_lm_send_sync(&lm, tls, sizeof tls, NULL, NULL, NULL).result == 0);
    assert_domain_result(tls, 0);
    assert(lm.destination == 4);

    memset(tls, 0, sizeof tls);
    tls[0] = 6;
    tls[1] = 8;
    tls[4] = 2;
    tls[5] = 2;
    assert(hzn_lm_send_sync(&lm, tls, sizeof tls, NULL, NULL, NULL).result == 0);
    assert_domain_result(tls, 0);
    assert(lm.loggers[0].object_id == 0);

    logger_request(tls, 2, 0, false);
    assert(hzn_lm_send_sync(&lm, tls, sizeof tls, read_packet, &input, NULL).result == 0);
    assert_domain_result(tls, 0xf601);
    puts("PASS Horizon lm: CMIF controls, domain objects, and X/A logger packets");
    return 0;
}
