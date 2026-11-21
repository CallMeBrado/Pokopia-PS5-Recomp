/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "applet-save.h"
#include "account_identity.h"
#include "cmif.h"

#include <string.h>

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_HANDLE = 0xe401,
    INVALID_COMBINATION = 0xe801,
    INVALID_OBJECT = 0xf601,
    AM_NO_DATA = 128 | (2 << 9),
    AM_INVALID_STORAGE_OFFSET = 128 | (503 << 9),
    DOMAIN_ROOT = 1,
    POP_LAUNCH_PARAMETER = 1,
    ENSURE_SAVE_DATA = 20,
    GET_DESIRED_LANGUAGE = 21,
    GET_FRIEND_INVITATION_EVENT = 140,
    LAUNCH_PARAMETER_PRESELECTED_USER = 2,
    OPEN_STORAGE = 0,
    GET_STORAGE_SIZE = 0,
    READ_STORAGE = 11,
    OPEN_APPLICATION_PROXY2 = 10,
    APPLET_FOCUS_IN_FOCUS = 1,
    LOCK_EXIT = 1,
    UNLOCK_EXIT = 2,
    SET_RESTART_MESSAGE_ENABLED = 14,
    GRAPHICS_SCALE_FULL_1080P = 10,
    GET_GRAPHICS_SCALING_PARAMETERS = 10,
    SET_CPU_BOOST_MODE = 66,
    CANCEL_CPU_BOOST_MODE = 67,
    GET_OPERATION_MODE_CHANGED_EVENT = 210,
    SET_GRAPHICS_SCALING_NOTIFICATION = 1050,
    SET_RTC_SCREEN_SHARING_AUDIO_ENABLED = 2400,
    SET_STREAM_PLAY_MIRRORING_AUDIO_ENABLED = 2401,
};

static const uint32_t current_process = UINT32_C(0xffff8001);
static const char applet_service[] = "IApplicationProxyService";
static const char application_proxy[] = "IApplicationProxy";
static const char self_controller[] = "ISelfController";
static const char application_functions[] = "IApplicationFunctions";
static const char common_state_getter[] = "ICommonStateGetter";
static const char window_controller[] = "IWindowController";
static const char graphics_scaling[] = "IGraphicsScalingParametersFunctions";
static const char launch_storage[] = "IStorage";
static const char launch_accessor[] = "IStorageAccessor";
const uint64_t hzn_applet_resource_user_id = 0x51;

/* LaunchParameterAccountPreselectedUser as consumed by SDK 0x750018f4a0. */
static const struct {
    uint32_t magic, selected;
    uint64_t uid[2];
    uint8_t reserved[0x70];
} preselected_user = {
    .magic = UINT32_C(0xc79497ca),
    .selected = 1,
    .uid = {HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1},
};
_Static_assert(sizeof preselected_user == 0x88, "preselected-user launch parameter size");

static const struct { uint32_t command; const char *type; } proxy_getters[] = {
    { 0, common_state_getter },
    { 1, self_controller },
    { 2, window_controller },
    { 3, "IAudioController" },
    { 4, "IDisplayController" },
    { 10, "IProcessWindingController" },
    { 11, "ILibraryAppletCreator" },
    { 20, application_functions },
    { 40, "IPerformanceFunctions" },
    { 60, graphics_scaling },
    { 1000, "IDebugFunctions" },
};

static HznAppletReply svc_result(uint32_t result)
{
    return (HznAppletReply){ .kind = HZN_APPLET_REPLY, .result = result };
}

static HznAppletReply boundary(uint32_t command, uint32_t id, const char *type)
{
    return (HznAppletReply){ .kind = HZN_APPLET_BOUNDARY, .command = command,
                             .object_id = id, .object_type = type };
}

static HznAppletReply reply_cmif(uint8_t *tls, size_t size, bool domain,
                                  uint32_t result, const uint32_t *output)
{
    return svc_result(hzn_cmif_reply(tls, size, domain, result, output) ? 0 : INVALID_COMBINATION);
}

void hzn_applet_init(HznApplet *applet, uint64_t guest_pid)
{
    *applet = (HznApplet){ .process_id = guest_pid, .next_object_id = 2, .docked = true };
}

static HznAppletObject *find_object(HznApplet *applet, uint32_t id)
{
    for (size_t i = 0; i < sizeof applet->objects / sizeof applet->objects[0]; ++i)
        if (applet->objects[i].id == id)
            return &applet->objects[i];
    return NULL;
}

static HznAppletReply new_object(HznApplet *applet, uint8_t *tls, size_t size,
                                  const char *type, bool *created)
{
    HznAppletObject *slot = find_object(applet, 0);
    if (!slot || applet->next_object_id == UINT32_MAX)
        return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
    uint32_t id = applet->next_object_id;
    HznAppletReply reply = reply_cmif(tls, size, true, 0, &id);
    if (!reply.result) {
        *slot = (HznAppletObject){ .id = id, .type = type };
        ++applet->next_object_id;
        if (created)
            *created = true;
    }
    return reply;
}

static HznAppletReply read_launch_storage(HznApplet *applet, uint8_t *tls, size_t size,
                                         const HznCmifRequest *request, size_t in_size,
                                         uint32_t object_id)
{
    if (in_size != 24 || !request->has_output_buffer || request->has_copy_handle ||
        request->has_pid || size < HZN_CMIF_DOMAIN_REPLY_BYTES)
        return svc_result(INVALID_COMBINATION);

    uint64_t offset = hzn_cmif_read64(request->raw + 32);
    uint64_t bytes = request->output.size;
    if (offset > sizeof preselected_user || bytes > sizeof preselected_user - offset)
        return reply_cmif(tls, size, true, AM_INVALID_STORAGE_OFFSET, NULL);
    if (!bytes)
        return reply_cmif(tls, size, true, 0, NULL);
    if (!applet->write_guest)
        return boundary(READ_STORAGE, object_id, launch_accessor);

    void *output = applet->write_guest(applet->write_guest_opaque, request->output.address, (size_t)bytes);
    if (!output)
        return svc_result(INVALID_ADDRESS);
    uintptr_t dest = (uintptr_t)output, reply = (uintptr_t)tls;
    if (dest >= reply ? dest - reply < size : reply - dest < bytes)
        return svc_result(INVALID_ADDRESS);

    memcpy(output, (const uint8_t *)&preselected_user + offset, (size_t)bytes);
    return reply_cmif(tls, size, true, 0, NULL);
}

static HznAppletReply open_proxy(HznApplet *applet, uint8_t *tls, size_t size,
                                  const HznCmifRequest *request, size_t in_size)
{
    /* SDK cmd 10 sends the calling PID, one copy of its process handle,
     * an applet attribute word and an explicit PID (zero for this caller). */
    if (in_size != 32 || !request->has_pid || request->has_buffer_descriptors)
        return svc_result(INVALID_COMBINATION);
    if (!request->has_copy_handle || request->copy_handle != current_process)
        return svc_result(INVALID_HANDLE);

    uint32_t attributes = hzn_cmif_read32(request->raw + 32);
    uint64_t pid = hzn_cmif_read64(request->raw + 40);
    if (attributes || (pid && pid != applet->process_id))
        return boundary(OPEN_APPLICATION_PROXY2, DOMAIN_ROOT, applet_service);

    /* The process reference belongs to this request, not the returned proxy. */
    return new_object(applet, tls, size, application_proxy, NULL);
}

static HznAppletReply set_self_policy(HznApplet *applet, uint8_t *tls, size_t size,
                                      const HznCmifRequest *request, size_t in_size,
                                      uint32_t command)
{
    size_t inputs = command == 13 ? 3 : 1;
    /* CMIF counts the actual booleans, HIPC rounds the message to a word.
     * The SDK leaves alignment bytes unspecified. */
    if (in_size != 16 + inputs || request->has_copy_handle || request->has_pid ||
        request->has_buffer_descriptors || (hzn_cmif_read32(tls + 4) & 0x3ff) != 13)
        return svc_result(INVALID_COMBINATION);
    const uint8_t *payload = request->raw + 32;
    bool values[3];
    for (size_t i = 0; i < inputs; ++i) {
        if (payload[i] > 1)
            return svc_result(INVALID_COMBINATION);
        values[i] = payload[i] != 0;
    }

    HznAppletReply reply = reply_cmif(tls, size, true, 0, NULL);
    if (reply.result)
        return reply;
    switch (command) {
    case 11:
        applet->operation_mode_notification = values[0];
        break;
    case 12:
        applet->performance_mode_notification = values[0];
        break;
    case 13:
        applet->focus_state_notification = values[0];
        applet->focus_background = values[1];
        applet->focus_suspend = values[2];
        break;
    case SET_RESTART_MESSAGE_ENABLED:
        applet->restart_message_enabled = values[0];
        break;
    case 16:
        applet->out_of_focus_suspending = values[0];
        break;
    case SET_RTC_SCREEN_SHARING_AUDIO_ENABLED:
        applet->rtc_screen_sharing_audio_requested = values[0];
        break;
    case SET_STREAM_PLAY_MIRRORING_AUDIO_ENABLED:
        applet->stream_play_mirroring_audio_requested = values[0];
        break;
    }
    return reply;
}

HznAppletReply hzn_applet_send_sync(HznApplet *applet, void *buffer, size_t size)
{
    if (!buffer)
        return svc_result(INVALID_ADDRESS);
    if (size < 8)
        return svc_result(INVALID_COMBINATION);
    uint8_t *tls = buffer;
    uint32_t type = hzn_cmif_read32(tls);
    if ((type == 2 || type == 15) && !hzn_cmif_read32(tls + 4)) {
        memset(tls, 0, 8);
        applet->has_foreground_rights = false;
        applet->exit_request_locked = false;
        return svc_result(0);
    }

    HznCmifRequest request;
    if (!hzn_cmif_parse(tls, size, &request) &&
        !hzn_cmif_parse_with_auto_output(tls, size, &request))
        return svc_result(INVALID_COMBINATION);
    const uint8_t *raw = request.raw;
    if (request.type == 5 || request.type == 7) {
        if (request.raw_size < 16 || hzn_cmif_read32(raw) != HZN_SFCI ||
            request.has_copy_handle || request.has_pid || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        uint32_t command = hzn_cmif_read32(raw + 8);
        if (command == 3) {
            uint32_t pointer_buffer_size = 0x8000;
            return reply_cmif(tls, size, false, 0, &pointer_buffer_size);
        }
        if (command == 0) {
            uint32_t id = DOMAIN_ROOT;
            HznAppletReply reply = reply_cmif(tls, size, false, 0, &id);
            if (!reply.result && !applet->domain) {
                applet->domain = true;
                applet->root_open = true;
            }
            return reply;
        }
        return boundary(command, 0, applet_service);
    }
    if (request.type != 4 && request.type != 6)
        return boundary(UINT32_MAX, 0, applet_service);
    if (!applet->domain)
        return boundary(request.raw_size >= 16 && hzn_cmif_read32(raw) == HZN_SFCI
                        ? hzn_cmif_read32(raw + 8) : UINT32_MAX, 0, applet_service);
    if (request.raw_size < 16)
        return svc_result(INVALID_COMBINATION);

    uint32_t id = hzn_cmif_read32(raw + 4);
    HznAppletObject *object = id > DOMAIN_ROOT ? find_object(applet, id) : NULL;
    if (raw[0] == 2) {
        if (request.has_output_buffer)
            return svc_result(INVALID_COMBINATION);
        HznAppletReply reply = reply_cmif(tls, size, true, 0, NULL);
        if (!reply.result) {
            if (id == DOMAIN_ROOT) {
                applet->root_open = false;
                applet->has_foreground_rights = false;
            }
            if (object)
                *object = (HznAppletObject){ 0 };
        }
        return reply;
    }

    size_t in_size = hzn_cmif_read32(raw) >> 16;
    if (raw[0] != 1 || raw[1] || in_size < 16 || in_size > request.raw_size - 16 ||
        hzn_cmif_read32(raw + 16) != HZN_SFCI)
        return svc_result(INVALID_COMBINATION);

    uint32_t command = hzn_cmif_read32(raw + 24);
    if (request.has_output_buffer && (!object || object->type != launch_accessor || command != READ_STORAGE))
        return svc_result(INVALID_COMBINATION);
    if (id == DOMAIN_ROOT && applet->root_open) {
        if (command == OPEN_APPLICATION_PROXY2)
            return open_proxy(applet, tls, size, &request, in_size);
        return boundary(command, id, applet_service);
    }
    if (!object)
        return reply_cmif(tls, size, true, INVALID_OBJECT, NULL);
    if (object->type == application_functions && command == ENSURE_SAVE_DATA) {
        if (in_size != 32 || request.has_pid || request.has_copy_handle || request.has_buffer_descriptors ||
            size < HZN_CMIF_DOMAIN_REPLY_BYTES + sizeof(uint64_t))
            return svc_result(INVALID_COMBINATION);
        if (!applet->ensure_save)
            return boundary(command, id, application_functions);
        const uint64_t uid_hi = hzn_cmif_read64(request.raw + 32);
        const uint64_t uid_lo = hzn_cmif_read64(request.raw + 40);
        const uint32_t result = applet->ensure_save(applet->save_opaque, uid_hi, uid_lo);
        const uint64_t required = 0;
        return svc_result(hzn_cmif_reply_data(tls, size, true, result, &required, sizeof required)
                          ? 0 : INVALID_COMBINATION);
    }
    if (object->type == application_functions && command == POP_LAUNCH_PARAMETER) {
        if (in_size != 20 || request.has_copy_handle || request.has_pid || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        if (hzn_cmif_read32(raw + 32) != LAUNCH_PARAMETER_PRESELECTED_USER)
            return boundary(command, id, object->type);
        if (applet->preselected_user_popped)
            return reply_cmif(tls, size, true, AM_NO_DATA, NULL);
        return new_object(applet, tls, size, launch_storage, &applet->preselected_user_popped);
    }
    if (object->type == launch_storage && command == OPEN_STORAGE) {
        if (in_size != 16 || request.has_copy_handle || request.has_pid || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        return new_object(applet, tls, size, launch_accessor, NULL);
    }
    if (object->type == launch_accessor && command == GET_STORAGE_SIZE) {
        if (in_size != 16 || request.has_copy_handle || request.has_pid || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        const uint64_t bytes = sizeof preselected_user;
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &bytes, sizeof bytes)
                          ? 0 : INVALID_COMBINATION);
    }
    if (object->type == launch_accessor && command == READ_STORAGE)
        return read_launch_storage(applet, tls, size, &request, in_size, id);
    if (object->type == application_proxy) {
        for (size_t i = 0; i < sizeof proxy_getters / sizeof proxy_getters[0]; ++i) {
            if (proxy_getters[i].command != command)
                continue;
            if (in_size != 16 || request.has_copy_handle || request.has_pid ||
                request.has_buffer_descriptors)
                return svc_result(INVALID_COMBINATION);
            return new_object(applet, tls, size, proxy_getters[i].type, NULL);
        }
    }
    if (object->type == window_controller && command == 1) {
        if (in_size != 16 || request.has_copy_handle || request.has_pid ||
            request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        bool replied = hzn_cmif_reply_data(tls, size, true, 0, &hzn_applet_resource_user_id,
                                          sizeof hzn_applet_resource_user_id);
        return svc_result(replied ? 0 : INVALID_COMBINATION);
    }
    if (object->type == graphics_scaling && command == SET_GRAPHICS_SCALING_NOTIFICATION) {
        /* SDK +0x1bfd98 sends one bool and leaves the alignment bytes unspecified. */
        if (in_size != 17 || request.has_copy_handle || request.has_pid || request.has_buffer_descriptors ||
            (hzn_cmif_read32(tls + 4) & 0x3ff) != 13 || raw[32] > 1)
            return svc_result(INVALID_COMBINATION);
        bool enabled = raw[32] != 0;
        HznAppletReply reply = reply_cmif(tls, size, true, 0, NULL);
        if (!reply.result)
            applet->graphics_scaling_notification = enabled;
        return reply;
    }
    if (object->type == graphics_scaling && command == GET_GRAPHICS_SCALING_PARAMETERS) {
        if (in_size != 16 || request.has_copy_handle || request.has_pid || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        /* The fixed docked output uses level 0 (1920x1080). The SDK copies 12 bytes. */
        const uint32_t parameters[3] = { GRAPHICS_SCALE_FULL_1080P, 0, 0 };
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, parameters, sizeof parameters)
                          ? 0 : INVALID_COMBINATION);
    }
    if (object->type == window_controller && (command == 10 || command == 11)) {
        if (in_size != 16 || request.has_copy_handle || request.has_pid ||
            request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        /* AcquireForegroundRights and ReleaseForegroundRights have no payload.
         * The session's application owns the right across its window controllers. */
        HznAppletReply reply = reply_cmif(tls, size, true, 0, NULL);
        if (!reply.result)
            applet->has_foreground_rights = command == 10;
        return reply;
    }
    if (object->type == self_controller && ((command >= 11 && command <= SET_RESTART_MESSAGE_ENABLED) ||
        command == 16 || command == SET_RTC_SCREEN_SHARING_AUDIO_ENABLED ||
        command == SET_STREAM_PLAY_MIRRORING_AUDIO_ENABLED))
        return set_self_policy(applet, tls, size, &request, in_size, command);
    if (object->type == self_controller && (command == LOCK_EXIT || command == UNLOCK_EXIT)) {
        /* The SDK keeps a nesting count and sends these at the outer edges. */
        if (in_size != 16 || request.has_copy_handle || request.has_pid || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        HznAppletReply reply = reply_cmif(tls, size, true, 0, NULL);
        if (!reply.result)
            applet->exit_request_locked = command == LOCK_EXIT;
        return reply;
    }
    if (object->type == application_functions && command == GET_DESIRED_LANGUAGE) {
        if (in_size != 16 || request.has_copy_handle || request.has_pid || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        /* The offline runtime's configured language, packed into the guest's LanguageCode. */
        const char language[8] = "en-US";
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, language, sizeof language)
                          ? 0 : INVALID_COMBINATION);
    }
    if (object->type == application_functions && (command == 320 || command == 321)) {
        if (in_size != 16 || request.has_copy_handle || request.has_pid ||
            request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        /* The SDK passes 320's two booleans to SetScreenShotAndContinuousRecordingInitiallyEnabled
         * and 321's one boolean to SetRtcScreenSharingAudioEnabled and ChangeBufferPermission.
         * The offline runtime reports system capture and RTC screen sharing as disabled. */
        const uint8_t permissions_disabled[2] = { 0, 0 };
        size_t bytes = command == 320 ? sizeof permissions_disabled : 1;
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, permissions_disabled, bytes)
                          ? 0 : INVALID_COMBINATION);
    }
    if (object->type == common_state_getter && command == 1 && applet->receive_message) {
        if (in_size != 16 || request.has_copy_handle || request.has_pid ||
            request.has_buffer_descriptors || size < HZN_CMIF_DOMAIN_REPLY_BYTES + sizeof(uint32_t))
            return svc_result(INVALID_COMBINATION);
        uint32_t message;
        uint32_t result = applet->receive_message(applet->event_opaque, &message);
        if (result)
            return reply_cmif(tls, size, true, result, NULL);
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &message, sizeof message)
                          ? 0 : INVALID_COMBINATION);
    }
    if (object->type == common_state_getter && (command == 5 || command == 6 || command == 9)) {
        if (in_size != 16 || request.has_copy_handle || request.has_pid ||
            request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        /* The one guest application holds the foreground on this runtime. */
        uint8_t mode_or_focus = command == 9 ? APPLET_FOCUS_IN_FOCUS : applet->docked;
        uint32_t performance_mode = applet->docked;
        bool replied = command == 6
            ? hzn_cmif_reply_data(tls, size, true, 0, &performance_mode, sizeof performance_mode)
            : hzn_cmif_reply_data(tls, size, true, 0, &mode_or_focus, sizeof mode_or_focus);
        return svc_result(replied ? 0 : INVALID_COMBINATION);
    }
    if (object->type == common_state_getter &&
        (command == SET_CPU_BOOST_MODE || command == CANCEL_CPU_BOOST_MODE)) {
        const bool set = command == SET_CPU_BOOST_MODE;
        if (in_size != (set ? 20 : 16) || request.has_copy_handle || request.has_pid ||
            request.has_buffer_descriptors || (hzn_cmif_read32(tls + 4) & 0x3ff) != (set ? 13u : 12u))
            return svc_result(INVALID_COMBINATION);
        /* The Switch 2 SDK maps the public enabled value to wire mode 2. */
        const uint32_t mode = set ? hzn_cmif_read32(raw + 32) : 0;
        if (mode != 0 && mode != 2)
            return svc_result(INVALID_COMBINATION);
        HznAppletReply reply = reply_cmif(tls, size, true, 0, NULL);
        if (!reply.result)
            applet->cpu_boost_mode = mode;
        return reply;
    }
    if (object->type == common_state_getter && command == 900) {
        if (in_size != 16 || request.has_copy_handle || request.has_pid ||
            request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        HznAppletReply reply = reply_cmif(tls, size, true, 0, NULL);
        if (!reply.result)
            applet->exit_to_library_applet_on_next_program = true;
        return reply;
    }
    bool suspend_event = object->type == self_controller && command == 91;
    bool gpu_error_event = object->type == application_functions && command == 130;
    bool message_event = object->type == common_state_getter && command == 0;
    /* SDK GetOperationMode watches this event before refreshing its cached mode. */
    bool operation_mode_event = object->type == application_functions &&
                                command == GET_OPERATION_MODE_CHANGED_EVENT;
    bool friend_invitation_event = object->type == application_functions && command == GET_FRIEND_INVITATION_EVENT;
    if ((suspend_event || gpu_error_event || message_event || operation_mode_event || friend_invitation_event) &&
        applet->copy_event) {
        if (in_size != 16 || request.has_copy_handle || request.has_pid ||
            request.has_buffer_descriptors || size < HZN_CMIF_DOMAIN_REPLY_BYTES + 8)
            return svc_result(INVALID_COMBINATION);
        HznAppletEvent event = suspend_event ? HZN_APPLET_SUSPEND_TICKS : HZN_APPLET_GPU_ERROR;
        if (message_event)
            event = HZN_APPLET_MESSAGE;
        else if (operation_mode_event)
            event = HZN_APPLET_OPERATION_MODE_CHANGED;
        else if (friend_invitation_event)
            event = HZN_APPLET_FRIEND_INVITATION;
        uint32_t handle = applet->copy_event(applet->event_opaque, event);
        if (!handle)
            return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
        return svc_result(hzn_cmif_reply_copy_handle(tls, size, true, 0, handle)
                          ? 0 : INVALID_COMBINATION);
    }
    return boundary(command, id, object->type);
}


