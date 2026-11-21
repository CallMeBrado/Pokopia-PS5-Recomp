/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "applet.h"
#include "account_identity.h"
#include "cmif.h"

#include <stdlib.h>
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
    SET_TERMINATE_RESULT = 22,
    GET_SAVE_DATA_SIZE = 26,
    GET_SAVE_DATA_SIZE_MAX = 28,
    SAVE_DATA_ACCOUNT = 1,
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
static const char library_creator[] = "ILibraryAppletCreator";
static const char library_accessor[] = "ILibraryAppletAccessor";
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
    { 11, library_creator },
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

/* ------------------------------------------------- R85: library applets */

enum { APPLET_SWKBD = 0x11, SWKBD_OUTPUT_BYTES = 0x7d8, SWKBD_CONFIG_BYTES = 0x3d4 };

static uint32_t storage_new(HznApplet *applet, uint64_t size, bool backed)
{
    for (uint32_t i = 0; i < sizeof applet->storages / sizeof applet->storages[0]; ++i) {
        HznAppletStorage *storage = &applet->storages[i];
        if (storage->refs)
            continue;
        uint8_t *data = NULL;
        if (backed) {
            if (size > (UINT64_C(1) << 24))
                return 0;
            data = calloc(1, size ? (size_t)size : 1);
            if (!data)
                return 0;
        }
        *storage = (HznAppletStorage){ .data = data, .size = size, .refs = 0 };
        return i + 1;
    }
    return 0;
}

static HznAppletStorage *storage_at(HznApplet *applet, uint32_t index)
{
    return index && index <= sizeof applet->storages / sizeof applet->storages[0] ? &applet->storages[index - 1] : NULL;
}

static void storage_release(HznApplet *applet, uint32_t index)
{
    HznAppletStorage *storage = storage_at(applet, index);
    if (storage && storage->refs && !--storage->refs) {
        free(storage->data);
        *storage = (HznAppletStorage){0};
    }
}

/* A new domain object that references a storage (taking a reference). */
static HznAppletReply new_storage_object(HznApplet *applet, uint8_t *tls, size_t size, const char *type,
                                         uint32_t storage)
{
    bool created = false;
    HznAppletReply reply = new_object(applet, tls, size, type, &created);
    if (created) {
        find_object(applet, applet->next_object_id - 1)->storage = storage;
        ++storage_at(applet, storage)->refs;
    }
    return reply;
}

static void utf16_copy(uint16_t *out, size_t capacity, const uint8_t *in)
{
    size_t i = 0;
    for (; i + 1 < capacity; ++i) {
        uint16_t unit;
        memcpy(&unit, in + i * 2, 2);
        if (!unit)
            break;
        out[i] = unit;
    }
    out[i] = 0;
}

/* Runs the keyboard for a started swkbd applet and queues its output:
 * u32 close result (0 entered, 1 cancelled), then the text (UTF-16, or UTF-8
 * when the config asks for it) in a 0x7d4-byte field. */
static bool run_swkbd(HznApplet *applet)
{
    HznSwkbdRequest request = { .max_length = 16 };
    bool utf8 = false;
    /* In data: common arguments, then the keyboard config, then the work buffer. */
    const HznAppletStorage *config = applet->library.in_count > 1 ? storage_at(applet, applet->library.in[1]) : NULL;
    if (config && config->data) {
        request.config = config->data;
        request.config_size = config->size;
    }
    if (config && config->data && config->size >= SWKBD_CONFIG_BYTES) {
        const uint8_t *c = config->data;
        utf16_copy(request.header, 65, c + 0x24);
        utf16_copy(request.sub, 129, c + 0xa6);
        utf16_copy(request.guide, 257, c + 0x1a8);
        request.max_length = hzn_cmif_read32(c + 0x3ac);
        request.min_length = hzn_cmif_read32(c + 0x3b0);
        request.password = hzn_cmif_read32(c + 0x3b4) != 0;
        (void)c;
    }
    /* R86: the game reads the text as UTF-8 (a UTF-16 "Player" showed as "P");
     * the Switch 2 config's own flag is not at the Switch 1 offset. */
    utf8 = true;
    if (!request.max_length || request.max_length > 500)
        request.max_length = 500;
    uint16_t text[501] = {0};
    const bool entered = applet->keyboard &&
                         applet->keyboard(applet->keyboard_opaque, &request, text, request.max_length + 1);
    const uint32_t index = storage_new(applet, SWKBD_OUTPUT_BYTES, true);
    HznAppletStorage *out = storage_at(applet, index);
    if (!out)
        return false;
    const uint32_t close = entered ? 0 : 1;
    memcpy(out->data, &close, sizeof close);
    uint8_t *field = out->data + 4;
    if (entered && utf8) {
        size_t at = 0;
        for (size_t i = 0; text[i] && at + 4 < SWKBD_OUTPUT_BYTES - 4; ++i) {
            uint32_t cp = text[i];
            if (cp >= 0xd800 && cp < 0xdc00 && text[i + 1] >= 0xdc00 && text[i + 1] < 0xe000)
                cp = 0x10000 + ((cp - 0xd800) << 10) + (text[++i] - 0xdc00);
            if (cp < 0x80) {
                field[at++] = (uint8_t)cp;
            } else if (cp < 0x800) {
                field[at++] = (uint8_t)(0xc0 | cp >> 6);
                field[at++] = (uint8_t)(0x80 | (cp & 0x3f));
            } else if (cp < 0x10000) {
                field[at++] = (uint8_t)(0xe0 | cp >> 12);
                field[at++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
                field[at++] = (uint8_t)(0x80 | (cp & 0x3f));
            } else {
                field[at++] = (uint8_t)(0xf0 | cp >> 18);
                field[at++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
                field[at++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
                field[at++] = (uint8_t)(0x80 | (cp & 0x3f));
            }
        }
    } else if (entered) {
        for (size_t i = 0; text[i] && (i + 1) * 2 < SWKBD_OUTPUT_BYTES - 4; ++i)
            memcpy(field + i * 2, &text[i], 2);
    }
    out->refs = 1; /* held by the out-data queue */
    applet->library.out[0] = index;
    applet->library.out_count = 1;
    return true;
}

static HznAppletReply library_applet_command(HznApplet *applet, uint8_t *tls, size_t size,
                                             const HznCmifRequest *request, size_t in_size, uint32_t command,
                                             HznAppletObject *object, uint32_t input_object)
{
    const uint8_t *payload = request->raw + 32;
    if (object->type == library_creator) {
        /* CreateLibraryApplet(applet id, mode). R85: the Switch 2 SDK sends
         * command 3 with the id in the low half of the first word (0x01020011
         * for the keyboard), then the mode and one more word (0x46). */
        if ((command == 0 || command == 3) && in_size >= 24) {
            const uint32_t applet_id = hzn_cmif_read32(payload) & (command == 3 ? 0xffffu : 0xffffffffu);
            if (applet_id != APPLET_SWKBD)
                return boundary(command, object->id, object->type);
            for (unsigned i = 0; i < applet->library.in_count; ++i)
                storage_release(applet, applet->library.in[i]);
            for (unsigned i = 0; i < applet->library.out_count; ++i)
                storage_release(applet, applet->library.out[i]);
            applet->library = (__typeof__(applet->library)){ .applet_id = applet_id, .active = true };
            if (applet->signal_event) {
                applet->signal_event(applet->event_opaque, HZN_APPLET_LIBRARY_STATE, false);
                applet->signal_event(applet->event_opaque, HZN_APPLET_LIBRARY_OUT, false);
            }
            return new_object(applet, tls, size, library_accessor, NULL);
        }
        if ((command == 10 || command == 11 || command == 12) && in_size >= 24) {
            /* CreateStorage(size); CreateTransferMemoryStorage(writable, size, handle);
             * CreateHandleStorage(size, handle). Transfer memory is not read. */
            const uint64_t bytes = hzn_cmif_read64(payload + (command == 11 ? 8 : 0));
            const uint32_t index = storage_new(applet, bytes, command == 10);
            if (!index)
                return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
            return new_storage_object(applet, tls, size, launch_storage, index);
        }
        return boundary(command, object->id, object->type);
    }

    /* ILibraryAppletAccessor */
    switch (command) {
    case 0:   /* GetAppletStateChangedEvent */
    case 105: /* GetPopOutDataEvent */
    case 106: /* GetPopInteractiveOutDataEvent */ {
        const HznAppletEvent event = command == 0 ? HZN_APPLET_LIBRARY_STATE :
                                     command == 105 ? HZN_APPLET_LIBRARY_OUT : HZN_APPLET_LIBRARY_INTERACTIVE;
        const uint32_t handle = applet->copy_event ? applet->copy_event(applet->event_opaque, event) : 0;
        if (!handle)
            return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
        return svc_result(hzn_cmif_reply_copy_handle(tls, size, true, 0, handle) ? 0 : INVALID_COMBINATION);
    }
    case 1: { /* IsCompleted */
        const uint8_t done = applet->library.completed;
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &done, sizeof done) ? 0 : INVALID_COMBINATION);
    }
    case 10: /* Start: the keyboard runs here, to completion */
        if (applet->library.applet_id == APPLET_SWKBD && !applet->library.completed) {
            if (!run_swkbd(applet))
                return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
            applet->library.completed = true;
            if (applet->signal_event) {
                applet->signal_event(applet->event_opaque, HZN_APPLET_LIBRARY_OUT, true);
                applet->signal_event(applet->event_opaque, HZN_APPLET_LIBRARY_STATE, true);
            }
        }
        return reply_cmif(tls, size, true, 0, NULL);
    case 20: /* RequestExit */
    case 25: /* Terminate */
        applet->library.completed = true;
        if (applet->signal_event)
            applet->signal_event(applet->event_opaque, HZN_APPLET_LIBRARY_STATE, true);
        return reply_cmif(tls, size, true, 0, NULL);
    case 30:  /* GetResult */
    case 50:  /* SetOutOfFocusApplicationSuspendingEnabled */
    case 150: /* RequestForAppletToGetForeground */
        return reply_cmif(tls, size, true, 0, NULL);
    case 100: /* PushInData(IStorage) */
    case 103: /* PushInteractiveInData(IStorage) */ {
        HznAppletObject *input = input_object ? find_object(applet, input_object) : NULL;
        if (!input || input->type != launch_storage || !input->storage)
            return reply_cmif(tls, size, true, INVALID_OBJECT, NULL);
        if (command == 100 && applet->library.in_count < 4) {
            applet->library.in[applet->library.in_count++] = input->storage;
            ++storage_at(applet, input->storage)->refs;
        }
        return reply_cmif(tls, size, true, 0, NULL);
    }
    case 101: /* PopOutData */
    case 104: /* PopInteractiveOutData */ {
        if (command == 104 || !applet->library.out_count)
            return reply_cmif(tls, size, true, AM_NO_DATA, NULL);
        const uint32_t index = applet->library.out[0];
        applet->library.out_count = 0;
        HznAppletReply reply = new_storage_object(applet, tls, size, launch_storage, index);
        storage_release(applet, index); /* the queue's reference */
        if (applet->signal_event)
            applet->signal_event(applet->event_opaque, HZN_APPLET_LIBRARY_OUT, false);
        return reply;
    }
    }
    /* Switch 2 additions (seen: 90 with 32 zero bytes) and other settings
     * calls: succeed with no output, noted for the log. */
    applet->noted_command = command;
    applet->noted_type = object->type;
    return reply_cmif(tls, size, true, 0, NULL);
}

static HznAppletReply storage_command(HznApplet *applet, uint8_t *tls, size_t size,
                                      const HznCmifRequest *request, size_t in_size, uint32_t command,
                                      HznAppletObject *object)
{
    HznAppletStorage *storage = storage_at(applet, object->storage);
    if (!storage)
        return reply_cmif(tls, size, true, INVALID_OBJECT, NULL);
    if (object->type == launch_storage) {
        if (command == 0) /* Open */
            return new_storage_object(applet, tls, size, launch_accessor, object->storage);
        if (command == 1) /* OpenTransferStorage */
            return boundary(command, object->id, object->type);
        return boundary(command, object->id, object->type);
    }
    if (command == 0) { /* GetSize */
        const uint32_t words[2] = {(uint32_t)storage->size, (uint32_t)(storage->size >> 32)};
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, words, sizeof words) ? 0 : INVALID_COMBINATION);
    }
    if ((command == 10 || command == 11) && in_size >= 24) { /* Write / Read (offset, buffer) */
        const uint64_t offset = hzn_cmif_read64(request->raw + 32);
        const HznGuestBuffer buffer = command == 10 ? request->input : request->output;
        if (!storage->data || offset > storage->size || buffer.size > storage->size - offset)
            return reply_cmif(tls, size, true, AM_INVALID_STORAGE_OFFSET, NULL);
        if (buffer.size && command == 10) {
            const uint8_t *guest = applet->read_guest ?
                applet->read_guest(applet->write_guest_opaque, buffer.address, (size_t)buffer.size) : NULL;
            if (!guest)
                return svc_result(INVALID_ADDRESS);
            memcpy(storage->data + offset, guest, (size_t)buffer.size);
        } else if (buffer.size) {
            uint8_t *guest = applet->write_guest ?
                applet->write_guest(applet->write_guest_opaque, buffer.address, (size_t)buffer.size) : NULL;
            if (!guest)
                return svc_result(INVALID_ADDRESS);
            memcpy(guest, storage->data + offset, (size_t)buffer.size);
        }
        return reply_cmif(tls, size, true, 0, NULL);
    }
    return boundary(command, object->id, object->type);
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
            if (object) {
                if (object->storage)
                    storage_release(applet, object->storage);
                *object = (HznAppletObject){ 0 };
            }
        }
        return reply;
    }

    size_t in_size = hzn_cmif_read32(raw) >> 16;
    /* R85: one input object (PushInData's storage) follows the payload. */
    if (raw[0] != 1 || raw[1] > 1 || in_size < 16 || in_size + raw[1] * 4u > request.raw_size - 16 ||
        hzn_cmif_read32(raw + 16) != HZN_SFCI)
        return svc_result(INVALID_COMBINATION);
    const uint32_t input_object = raw[1] ? hzn_cmif_read32(raw + 16 + in_size) : 0;

    uint32_t command = hzn_cmif_read32(raw + 24);
    if (request.has_output_buffer && (!object || object->type != launch_accessor || command != READ_STORAGE))
        return svc_result(INVALID_COMBINATION);
    if (object && object->type == self_controller && command == 9) {
        /* R85: GetLibraryAppletLaunchableEvent, asked before a library applet
         * starts; a library applet can always be launched here. */
        if (applet->signal_event)
            applet->signal_event(applet->event_opaque, HZN_APPLET_LIBRARY_LAUNCHABLE, true);
        const uint32_t handle =
            applet->copy_event ? applet->copy_event(applet->event_opaque, HZN_APPLET_LIBRARY_LAUNCHABLE) : 0;
        if (!handle)
            return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
        return svc_result(hzn_cmif_reply_copy_handle(tls, size, true, 0, handle) ? 0 : INVALID_COMBINATION);
    }
    if (object && (object->type == library_creator || object->type == library_accessor))
        return library_applet_command(applet, tls, size, &request, in_size, command, object, input_object);
    if (object && object->storage && (object->type == launch_storage || object->type == launch_accessor))
        return storage_command(applet, tls, size, &request, in_size, command, object);
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
    if (object->type == application_functions && command == GET_SAVE_DATA_SIZE) {
        int64_t quotas[2] = {0};
        if (in_size != 40 || request.has_pid || request.has_copy_handle || request.has_buffer_descriptors ||
            size < HZN_CMIF_DOMAIN_REPLY_BYTES + sizeof quotas)
            return svc_result(INVALID_COMBINATION);
        if (raw[32] != SAVE_DATA_ACCOUNT || !applet->get_save_size)
            return boundary(command, id, application_functions);
        const uint64_t uid_hi = hzn_cmif_read64(raw + 40);
        const uint64_t uid_lo = hzn_cmif_read64(raw + 48);
        const uint32_t result = applet->get_save_size(applet->save_opaque, uid_hi, uid_lo, &quotas[0], &quotas[1]);
        return svc_result(hzn_cmif_reply_data(tls, size, true, result, quotas, sizeof quotas)
                          ? 0 : INVALID_COMBINATION);
    }
    if (object->type == application_functions && command == GET_SAVE_DATA_SIZE_MAX) {
        const int64_t quotas[] = {applet->save_data_limit, applet->save_journal_limit};
        if (in_size != 16 || request.has_pid || request.has_copy_handle || request.has_buffer_descriptors ||
            size < HZN_CMIF_DOMAIN_REPLY_BYTES + sizeof quotas)
            return svc_result(INVALID_COMBINATION);
        if (quotas[0] <= 0 || quotas[1] <= 0)
            return boundary(command, id, application_functions);
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, quotas, sizeof quotas)
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
    if (object->type == application_functions && command == 23) {
        /* R74: GetDisplayVersion, a 16-byte NUL-padded version string. The game
         * asks for it after character creation. */
        if (in_size != 16 || request.has_copy_handle || request.has_pid || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        const char version[16] = "1.0.0";
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, version, sizeof version)
                          ? 0 : INVALID_COMBINATION);
    }
    if (object->type == application_functions && command == SET_TERMINATE_RESULT) {
        if (in_size != 20 || request.has_copy_handle || request.has_pid || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION);
        const uint32_t result = hzn_cmif_read32(raw + 32);
        HznAppletReply reply = reply_cmif(tls, size, true, 0, NULL);
        if (!reply.result)
            applet->terminate_result = result;
        return reply;
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


