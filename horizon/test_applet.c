/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "applet.h"
#include "account_identity.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { SFCI = 0x49434653, SFCO = 0x4f434653 };

static void set_control(uint32_t *tls, uint32_t command)
{
    memset(tls, 0, 256);
    tls[0] = 7;
    tls[1] = 8;
    tls[4] = SFCI;
    tls[5] = 1;
    tls[6] = command;
}

static void set_open(uint32_t *tls, uint32_t process_handle)
{
    memset(tls, 0, 256);
    tls[0] = 6;
    tls[1] = 0x80000010;
    tls[2] = 3;
    tls[3] = 0xdeadbeef;
    tls[5] = process_handle;
    tls[8] = 0x200001;
    tls[9] = 1;
    tls[12] = SFCI;
    tls[13] = 1;
    tls[14] = 10;
}

static void set_domain_call(uint32_t *tls, uint32_t id, uint32_t command)
{
    memset(tls, 0, 256);
    tls[0] = 6;
    tls[1] = 12;
    tls[4] = 0x100001;
    tls[5] = id;
    tls[8] = SFCI;
    tls[9] = 1;
    tls[10] = command;
}

static uint32_t read_returned_id(uint32_t *tls)
{
    assert(tls[0] == 0 && tls[1] == 13);
    assert(tls[4] == 1 && tls[8] == SFCO && tls[10] == 0);
    return tls[12];
}

typedef struct {
    bool message_pending;
    unsigned copies, receives;
    HznAppletEvent expected_event;
} TestAppletEvents;

static uint32_t copy_event(void *opaque, HznAppletEvent event)
{
    TestAppletEvents *events = opaque;
    assert(event == events->expected_event);
    return 0x300 + ++events->copies;
}

static uint32_t receive_message(void *opaque, uint32_t *message)
{
    TestAppletEvents *events = opaque;
    ++events->receives;
    if (!events->message_pending)
        return HZN_APPLET_NO_MESSAGES;
    events->message_pending = false;
    *message = 15;
    return 0;
}

static const uint64_t storage_guest_address = UINT64_C(0x7cf1234560);

typedef struct {
    uint8_t output[0x88];
    unsigned writes;
    void *overlap;
} TestStorage;

static void *write_storage(void *opaque, uint64_t address, size_t size)
{
    TestStorage *storage = opaque;
    if (address != storage_guest_address || size > sizeof storage->output)
        return NULL;
    ++storage->writes;
    return storage->overlap ? storage->overlap : storage->output;
}

static void set_launch_parameter(uint32_t *tls, uint32_t id, uint32_t kind)
{
    set_domain_call(tls, id, 1);
    tls[1] = 13;
    tls[4] = 0x140001;
    tls[12] = kind;
}

static void set_storage_read(uint32_t *tls, uint32_t id, uint64_t offset, uint16_t size)
{
    memset(tls, 0, 256);
    tls[0] = 0x01000006;
    tls[1] = 0x0c0f;
    tls[8] = 0x180001;
    tls[9] = id;
    tls[12] = SFCI;
    tls[13] = 1;
    tls[14] = 11;
    tls[16] = (uint32_t)offset;
    tls[17] = (uint32_t)(offset >> 32);
    tls[19] = size;
    tls[20] = (uint32_t)storage_guest_address;
    tls[21] = ((uint32_t)size << 16) | (uint32_t)(storage_guest_address >> 32);
}

static void check_launch_parameter(HznApplet *applet, uint32_t application_functions)
{
    uint32_t tls[64], original[64];
    set_launch_parameter(tls, application_functions, 1);
    memcpy(original, tls, sizeof original);
    HznAppletReply reply = hzn_applet_send_sync(applet, tls, sizeof tls);
    assert(reply.kind == HZN_APPLET_BOUNDARY && reply.command == 1);
    assert(!strcmp(reply.object_type, "IApplicationFunctions"));
    assert(!memcmp(tls, original, sizeof tls) && !applet->preselected_user_popped);

    set_launch_parameter(tls, application_functions, 2);
    assert(hzn_applet_send_sync(applet, tls, sizeof tls).result == 0);
    uint32_t storage = read_returned_id(tls);
    assert(applet->preselected_user_popped);
    set_launch_parameter(tls, application_functions, 2);
    assert(hzn_applet_send_sync(applet, tls, sizeof tls).result == 0);
    assert(tls[4] == 0 && tls[8] == SFCO && tls[10] == 0x480);

    set_domain_call(tls, storage, 0);
    assert(hzn_applet_send_sync(applet, tls, sizeof tls).result == 0);
    uint32_t accessor = read_returned_id(tls);
    set_domain_call(tls, accessor, 0);
    assert(hzn_applet_send_sync(applet, tls, sizeof tls).result == 0);
    assert(tls[4] == 0 && tls[8] == SFCO && tls[10] == 0 && tls[12] == 0x88 && tls[13] == 0);

    memset(tls, 0, sizeof tls);
    tls[0] = 6;
    tls[1] = 8;
    tls[4] = 2;
    tls[5] = accessor;
    assert(hzn_applet_send_sync(applet, tls, sizeof tls).result == 0);
    set_domain_call(tls, storage, 0);
    assert(hzn_applet_send_sync(applet, tls, sizeof tls).result == 0);
    uint32_t new_accessor = read_returned_id(tls);
    assert(new_accessor != accessor);

    set_storage_read(tls, new_accessor, 0, 0x88);
    memcpy(original, tls, sizeof original);
    reply = hzn_applet_send_sync(applet, tls, sizeof tls);
    assert(reply.kind == HZN_APPLET_BOUNDARY && reply.command == 11);
    assert(reply.object_id == new_accessor && !strcmp(reply.object_type, "IStorageAccessor"));
    assert(!memcmp(tls, original, sizeof tls));

    TestStorage output = {0};
    memset(output.output, 0xcc, sizeof output.output);
    applet->write_guest = write_storage;
    applet->write_guest_opaque = &output;
    assert(hzn_applet_send_sync(applet, tls, sizeof tls).result == 0);
    assert(tls[4] == 0 && tls[8] == SFCO && tls[10] == 0 && output.writes == 1);
    uint64_t prefix[3];
    memcpy(prefix, output.output, sizeof prefix);
    assert(prefix[0] == UINT64_C(0x00000001c79497ca));
    assert(prefix[1] == HZN_OFFLINE_UID_WORD0 && prefix[2] == HZN_OFFLINE_UID_WORD1);
    for (size_t i = sizeof prefix; i < sizeof output.output; ++i)
        assert(!output.output[i]);

    set_storage_read(tls, new_accessor, UINT64_MAX, 1);
    assert(hzn_applet_send_sync(applet, tls, sizeof tls).result == 0);
    assert(tls[10] == 0x3ee80 && output.writes == 1);
    set_storage_read(tls, new_accessor, 0x87, 2);
    assert(hzn_applet_send_sync(applet, tls, sizeof tls).result == 0);
    assert(tls[10] == 0x3ee80 && output.writes == 1);
    set_storage_read(tls, new_accessor, 0, 1);
    tls[20] += 1;
    assert(hzn_applet_send_sync(applet, tls, sizeof tls).result == 0xcc01 && output.writes == 1);
    set_storage_read(tls, new_accessor, 0, 1);
    output.overlap = tls;
    memcpy(original, tls, sizeof original);
    assert(hzn_applet_send_sync(applet, tls, sizeof tls).result == 0xcc01);
    assert(!memcmp(tls, original, sizeof tls));

    set_domain_call(tls, new_accessor, 10);
    reply = hzn_applet_send_sync(applet, tls, sizeof tls);
    assert(reply.kind == HZN_APPLET_BOUNDARY && reply.command == 10);
    assert(reply.object_id == new_accessor && !strcmp(reply.object_type, "IStorageAccessor"));
}

int main(void)
{
    HznApplet applet;
    uint32_t tls[64];
    hzn_applet_init(&applet, 0x51);

    set_control(tls, 3);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(tls[4] == SFCO && tls[8] == 0x8000);
    set_control(tls, 0);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(tls[4] == SFCO && tls[8] == 1);

    set_open(tls, 0x10001);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0xe401);
    assert(applet.next_object_id == 2);
    set_open(tls, 0xffff8001);
    assert(hzn_applet_send_sync(&applet, tls, 40).result == 0xe801);
    assert(applet.next_object_id == 2);
    set_open(tls, 0xffff8001);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    uint32_t proxy = read_returned_id(tls);
    assert(proxy == 2);

    static const struct { uint32_t command; const char *type; } getters[] = {
        { 20, "IApplicationFunctions" },
        { 60, "IGraphicsScalingParametersFunctions" },
        { 40, "IPerformanceFunctions" },
    };
    for (size_t i = 0; i < sizeof getters / sizeof getters[0]; ++i) {
        set_domain_call(tls, proxy, getters[i].command);
        assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
        uint32_t id = read_returned_id(tls);
        assert(id == i + 3);

        uint32_t request[64];
        set_domain_call(tls, id, 1051);
        memcpy(request, tls, sizeof tls);
        HznAppletReply reply = hzn_applet_send_sync(&applet, tls, sizeof tls);
        assert(reply.kind == HZN_APPLET_BOUNDARY && reply.command == 1051 && reply.object_id == id);
        assert(strcmp(reply.object_type, getters[i].type) == 0);
        assert(!memcmp(request, tls, sizeof tls));

        if (getters[i].command == 20) {
            for (uint32_t command = 320; command <= 321; ++command) {
                set_domain_call(tls, id, command);
                tls[12] = 0xffffffff;
                assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
                assert(tls[1] == 13 && tls[4] == 0 && tls[8] == SFCO && tls[10] == 0);
                assert(tls[12] == 0);
            }
        }
        if (getters[i].command == 60) {
            set_domain_call(tls, id, 1050);
            assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0xe801);
            tls[1] = 13;
            tls[4] = 0x110001;
            tls[12] = 0xdeadbe02;
            assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0xe801);
            assert(!applet.graphics_scaling_notification);
            tls[12] = 0xdeadbe01;
            assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
            assert(tls[1] == 12 && tls[4] == 0 && tls[8] == SFCO && tls[10] == 0);
            assert(applet.graphics_scaling_notification);

            set_domain_call(tls, id, 10);
            assert(hzn_applet_send_sync(&applet, tls, 67).result == 0xe801 && tls[10] == 10);
            assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
            assert(tls[1] == 15 && tls[4] == 0 && tls[8] == SFCO && tls[10] == 0);
            assert(tls[12] == 10 && tls[13] == 0 && tls[14] == 0);

            set_domain_call(tls, id, 1050);
            tls[1] = 13;
            tls[4] = 0x110001;
            assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
            assert(!applet.graphics_scaling_notification);
        }

        memset(tls, 0, sizeof tls);
        tls[0] = 6;
        tls[1] = 8;
        tls[4] = 2;
        tls[5] = id;
        assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
        set_domain_call(tls, id, 1050);
        assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
        assert(tls[4] == 0 && tls[8] == SFCO && tls[10] == 0xf601);
    }

    set_domain_call(tls, proxy, 2);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    uint32_t window = read_returned_id(tls);
    set_domain_call(tls, window, 1);
    assert(hzn_applet_send_sync(&applet, tls, 56).result == 0xe801);
    assert(tls[0] == 6 && tls[10] == 1);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(tls[0] == 0 && tls[1] == 14);
    assert(tls[4] == 0 && tls[8] == SFCO && tls[10] == 0);
    assert(tls[12] == 0x51 && tls[13] == 0);

    set_domain_call(tls, window, 10);
    assert(hzn_applet_send_sync(&applet, tls, 55).result == 0xe801);
    assert(tls[0] == 6 && tls[10] == 10 && !applet.has_foreground_rights);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(tls[1] == 12 && tls[4] == 0 && tls[8] == SFCO && tls[10] == 0);
    assert(applet.has_foreground_rights);
    set_domain_call(tls, window, 11);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(!applet.has_foreground_rights);

    set_domain_call(tls, proxy, 1);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    uint32_t self_controller = read_returned_id(tls);

    set_domain_call(tls, self_controller, 13);
    tls[1] = 13;
    tls[4] = 0x130001;
    tls[12] = 0x55010002;
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0xe801);
    assert(tls[0] == 6 && !applet.focus_suspend);
    tls[12] = 0x55010001;
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(tls[1] == 12 && tls[4] == 0 && tls[8] == SFCO && tls[10] == 0);
    assert(applet.focus_state_notification && !applet.focus_background && applet.focus_suspend);

    static const uint32_t self_flags[] = { 11, 12, 14, 16, 2400, 2401 };
    for (size_t i = 0; i < sizeof self_flags / sizeof self_flags[0]; ++i) {
        set_domain_call(tls, self_controller, self_flags[i]);
        tls[1] = 13;
        tls[4] = 0x110001;
        tls[12] = 0xcccccc01;
        assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
        assert(tls[1] == 12 && tls[8] == SFCO && tls[10] == 0);
    }
    assert(applet.operation_mode_notification && applet.performance_mode_notification);
    assert(applet.restart_message_enabled);
    assert(applet.out_of_focus_suspending);
    assert(applet.rtc_screen_sharing_audio_requested && applet.stream_play_mirroring_audio_requested);
    set_domain_call(tls, self_controller, 2400);
    tls[1] = 13;
    tls[4] = 0x110001;
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(!applet.rtc_screen_sharing_audio_requested && applet.stream_play_mirroring_audio_requested);

    set_domain_call(tls, self_controller, 14);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0xe801);
    tls[1] = 13;
    tls[4] = 0x110001;
    tls[12] = 2;
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0xe801);
    assert(applet.restart_message_enabled && tls[10] == 14);
    tls[12] = 0;
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(!applet.restart_message_enabled);

    set_domain_call(tls, self_controller, 1);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(applet.exit_request_locked && tls[1] == 12 && tls[8] == SFCO && tls[10] == 0);
    set_domain_call(tls, self_controller, 2);
    tls[1] = 13;
    tls[4] = 0x110001;
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0xe801);
    assert(applet.exit_request_locked && tls[10] == 2);
    set_domain_call(tls, self_controller, 2);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(!applet.exit_request_locked);

    set_domain_call(tls, proxy, 0);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    uint32_t common_state = read_returned_id(tls);

    set_domain_call(tls, common_state, 66);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0xe801);
    tls[1] = 13;
    tls[4] = 0x140001;
    tls[12] = 2;
    assert(hzn_applet_send_sync(&applet, tls, 59).result == 0xe801);
    assert(applet.cpu_boost_mode == 0 && tls[10] == 66);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(applet.cpu_boost_mode == 2 && tls[1] == 12 && tls[8] == SFCO && tls[10] == 0);

    set_domain_call(tls, common_state, 66);
    tls[1] = 13;
    tls[4] = 0x140001;
    tls[12] = 1;
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0xe801);
    assert(applet.cpu_boost_mode == 2 && tls[10] == 66);
    tls[12] = 0;
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0 && applet.cpu_boost_mode == 0);

    set_domain_call(tls, common_state, 66);
    tls[1] = 13;
    tls[4] = 0x140001;
    tls[12] = 2;
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0 && applet.cpu_boost_mode == 2);
    set_domain_call(tls, common_state, 67);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0 && applet.cpu_boost_mode == 0);

    TestAppletEvents events = { .message_pending = true, .expected_event = HZN_APPLET_MESSAGE };
    applet.event_opaque = &events;
    applet.copy_event = copy_event;
    applet.receive_message = receive_message;

    set_domain_call(tls, common_state, 0);
    assert(hzn_applet_send_sync(&applet, tls, 60).result == 0xe801);
    assert(tls[0] == 6 && events.copies == 0);
    for (uint32_t handle = 0x301; handle <= 0x302; ++handle) {
        set_domain_call(tls, common_state, 0);
        assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
        assert(tls[0] == 0 && tls[1] == 0x8000000c && tls[2] == 2 && tls[3] == handle);
        assert(tls[4] == 0 && tls[8] == SFCO && tls[10] == 0);
    }

    events.expected_event = HZN_APPLET_OPERATION_MODE_CHANGED;
    set_domain_call(tls, proxy, 20);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    uint32_t application_functions = read_returned_id(tls);
    set_domain_call(tls, application_functions, 210);
    assert(hzn_applet_send_sync(&applet, tls, 60).result == 0xe801);
    assert(tls[0] == 6 && events.copies == 2);
    for (uint32_t handle = 0x303; handle <= 0x304; ++handle) {
        set_domain_call(tls, application_functions, 210);
        assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
        assert(tls[0] == 0 && tls[1] == 0x8000000c && tls[2] == 2 && tls[3] == handle);
        assert(tls[4] == 0 && tls[8] == SFCO && tls[10] == 0);
    }

    set_domain_call(tls, common_state, 1);
    assert(hzn_applet_send_sync(&applet, tls, 56).result == 0xe801);
    assert(tls[0] == 6 && events.receives == 0);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(tls[0] == 0 && tls[1] == 13 && tls[4] == 0 && tls[8] == SFCO);
    assert(tls[10] == 0 && tls[12] == 15 && events.receives == 1 && !events.message_pending);
    set_domain_call(tls, common_state, 1);
    assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
    assert(tls[1] == 12 && tls[4] == 0 && tls[8] == SFCO && tls[10] == 0x680);
    assert(events.receives == 2);

    static const uint32_t mode_and_focus[] = { 5, 6, 9 };
    for (size_t i = 0; i < sizeof mode_and_focus / sizeof mode_and_focus[0]; ++i) {
        set_domain_call(tls, common_state, mode_and_focus[i]);
        assert(hzn_applet_send_sync(&applet, tls, sizeof tls).result == 0);
        assert(tls[1] == 13 && tls[4] == 0 && tls[8] == SFCO && tls[10] == 0 && tls[12] == 1);
    }

    check_launch_parameter(&applet, application_functions);

    puts("PASS Horizon appletOE: domain objects, events and preselected user storage");
    return 0;
}

