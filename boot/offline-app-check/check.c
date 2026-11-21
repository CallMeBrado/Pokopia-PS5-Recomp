#include "offline_app.h"
#include "cmif.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    HznOfflineApp *shared;
    uint32_t calls;
} Sessions;

static uint32_t clone_session(void *opaque, HznOfflineApp *shared)
{
    Sessions *sessions = opaque;
    sessions->shared = shared;
    return 0x10020 + ++sessions->calls;
}

static void command(uint32_t *tls, bool domain, bool control, uint32_t id, uint32_t number, bool pid, uint64_t arg)
{
    memset(tls, 0, 256);
    tls[0] = control ? 7 : 6;
    tls[1] = 8 + (domain ? 4 : 0) + (pid ? 2 : 0);
    uint32_t *raw = tls + (pid ? 8 : 4);
    if (pid) {
        tls[1] |= 0x80000000;
        tls[2] = 1;
    }
    if (domain) {
        raw[0] = (pid ? 0x180000 : 0x100000) | 1;
        raw[1] = id;
        raw += 4;
    }
    raw[0] = HZN_SFCI;
    raw[1] = 1;
    raw[2] = number;
    if (pid)
        memcpy(raw + 4, &arg, sizeof arg);
}

static void check(HznOfflineAppKind root_kind, uint32_t create)
{
    HznOfflineApp app;
    Sessions sessions = {0};
    uint32_t tls[64], before[64];
    hzn_offline_app_init(&app, root_kind, clone_session, &sessions);

    command(tls, false, true, 0, 3, false, 0);
    assert(hzn_offline_app_send(&app, tls, 256).result == 0);
    assert(tls[1] == 9 && tls[4] == HZN_SFCO && tls[8] == 0x8000);
    command(tls, false, true, 0, 0, false, 0);
    assert(hzn_offline_app_send(&app, tls, 43).result == 0xe801 && !app.domain);
    assert(hzn_offline_app_send(&app, tls, 256).result == 0 && app.domain);
    assert(tls[4] == HZN_SFCO && tls[8] == 1);
    command(tls, false, true, 0, 2, false, 0);
    assert(hzn_offline_app_send(&app, tls, 256).result == 0);
    assert(tls[1] == 0x80000008 && tls[2] == 0x20 && tls[3] == 0x10021);
    assert(sessions.calls == 1 && sessions.shared == &app);

    command(tls, true, false, 1, create, false, 0);
    assert(hzn_offline_app_send(&app, tls, 256).result == 0xe801 && app.next_id == 2);
    command(tls, true, false, 1, create, true, 1);
    assert(hzn_offline_app_send(&app, tls, 256).result == 0xe801 && app.next_id == 2);
    command(tls, true, false, 1, create, true, 0);
    assert(hzn_offline_app_send(&app, tls, 256).result == 0);
    assert(tls[1] == 13 && tls[4] == 1 && tls[8] == HZN_SFCO && tls[10] == 0 && tls[12] == 2);
    assert(app.next_id == 3 && app.objects[0] == 2);
    command(tls, true, false, 2, create, true, 0);
    memcpy(before, tls, sizeof tls);
    HznOfflineAppReply child = hzn_offline_app_send(&app, tls, 256);
    assert(child.boundary_type && child.object_id == 2 && child.command == create);
    assert(!memcmp(before, tls, sizeof tls));

    memset(tls, 0, sizeof tls);
    tls[0] = 6;
    tls[1] = 8;
    tls[4] = 2;
    tls[5] = 2;
    assert(hzn_offline_app_send(&app, tls, 256).result == 0 && !app.objects[0]);
    command(tls, true, false, 2, create, true, 0);
    assert(hzn_offline_app_send(&app, tls, 256).result == 0 && tls[10] == 0xf601);
    command(tls, true, false, 1, create, true, 0);
    assert(hzn_offline_app_send(&app, tls, 256).result == 0 && tls[12] == 3);

    hzn_offline_app_init(&app, root_kind, clone_session, &sessions);
    command(tls, false, false, 0, create, true, 0);
    assert(hzn_offline_app_send(&app, tls, 256).result == 0xe801 && sessions.calls == 1);
}

int main(void)
{
    check(HZN_OFFLINE_NIFM_STATIC, 5);
    check(HZN_OFFLINE_BCAT_CREATOR, 0);
    puts("PASS NIFM/BCAT captured startup controls, PID-only domain child IDs, clone, close and malformed packets");
    return 0;
}
