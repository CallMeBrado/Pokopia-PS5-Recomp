/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_APPLET_H
#define POKOPIA_HORIZON_APPLET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    const char *type;
    uint32_t storage; /* R85: 1 + index into HznApplet.storages; 0 for the launch storage */
} HznAppletObject;

/* R85: an applet storage (AM IStorage) made by the application or a library applet. */
typedef struct {
    uint8_t *data;   /* NULL for a transfer-memory storage (never read here) */
    uint64_t size;
    uint32_t refs;
} HznAppletStorage;

/* R85: what the software keyboard (swkbd) applet was asked for, decoded from
 * its KeyboardConfig storage (SwkbdConfigCommon). Texts are UTF-16, NUL-ended. */
typedef struct {
    uint16_t header[65], sub[129], guide[257];
    uint32_t max_length, min_length;
    bool password;
    /* R86: the raw KeyboardConfig storage (its Switch 2 layout is not fully known). */
    const uint8_t *config;
    uint64_t config_size;
} HznSwkbdRequest;

/* Run the keyboard; true when text was entered (written to text, at most
 * capacity - 1 units, NUL-ended), false when cancelled. */
typedef bool (*HznAppletKeyboard)(void *opaque, const HznSwkbdRequest *request, uint16_t *text, size_t capacity);
typedef bool (*HznAppletSignalEvent)(void *opaque, uint32_t event, bool signaled);

typedef enum {
    HZN_APPLET_SUSPEND_TICKS,
    HZN_APPLET_GPU_ERROR,
    HZN_APPLET_MESSAGE,
    HZN_APPLET_OPERATION_MODE_CHANGED,
    HZN_APPLET_FRIEND_INVITATION,
    /* R85: library applet (software keyboard) events */
    HZN_APPLET_LIBRARY_STATE,
    HZN_APPLET_LIBRARY_OUT,
    HZN_APPLET_LIBRARY_INTERACTIVE,
    HZN_APPLET_LIBRARY_LAUNCHABLE,
} HznAppletEvent;

enum { HZN_APPLET_NO_MESSAGES = 0x680 };

/* Allocate a fresh readable handle to the shared event, or return zero. */
typedef uint32_t (*HznAppletCopyEvent)(void *opaque, HznAppletEvent event);
/* Dequeue the oldest message and clear the event when the queue is empty.
 * Return a CMIF result and write the message on success. */
typedef uint32_t (*HznAppletReceiveMessage)(void *opaque, uint32_t *message);
/* Validate the complete output range and return its writable host mapping. */
typedef void *(*HznAppletWriteGuest)(void *opaque, uint64_t address, size_t size);
/* R85: validate a readable input range and return its host mapping. */
typedef const void *(*HznAppletReadGuest)(void *opaque, uint64_t address, size_t size);
/* Ensure the account save for the validated user and return a Horizon FS result. */
typedef uint32_t (*HznAppletEnsureSave)(void *opaque, uint64_t uid_hi, uint64_t uid_lo);
typedef uint32_t (*HznAppletGetSaveSize)(void *opaque, uint64_t uid_hi, uint64_t uid_lo,
                                        int64_t *normal, int64_t *journal);

typedef struct {
    uint64_t process_id;
    HznAppletCopyEvent copy_event;
    HznAppletReceiveMessage receive_message;
    void *event_opaque;
    HznAppletWriteGuest write_guest;
    HznAppletReadGuest read_guest;
    void *write_guest_opaque;
    HznAppletEnsureSave ensure_save;
    HznAppletGetSaveSize get_save_size;
    void *save_opaque;
    int64_t save_data_limit, save_journal_limit;
    uint32_t next_object_id;
    uint32_t cpu_boost_mode;
    uint32_t terminate_result;
    bool domain, root_open;
    bool docked;
    bool has_foreground_rights;
    bool operation_mode_notification, performance_mode_notification;
    bool graphics_scaling_notification;
    bool focus_state_notification, focus_background, focus_suspend;
    bool restart_message_enabled, exit_request_locked;
    bool out_of_focus_suspending;
    bool rtc_screen_sharing_audio_requested, stream_play_mirroring_audio_requested;
    bool exit_to_library_applet_on_next_program;
    bool preselected_user_popped;
    HznAppletObject objects[32];
    /* R85: library applets (only the software keyboard is run). */
    HznAppletKeyboard keyboard;
    HznAppletSignalEvent signal_event;
    void *keyboard_opaque;
    HznAppletStorage storages[16];
    /* R85: an unknown library-applet command answered with success (for the log). */
    uint32_t noted_command;
    const char *noted_type;
    struct {
        uint32_t applet_id, in[4], out[2];
        unsigned in_count, out_count;
        bool active, completed;
    } library;
} HznApplet;

typedef enum {
    HZN_APPLET_REPLY,
    HZN_APPLET_BOUNDARY,
} HznAppletReplyKind;

typedef struct {
    HznAppletReplyKind kind;
    uint32_t result;
    uint32_t command;
    uint32_t object_id;
    const char *object_type;
} HznAppletReply;

/* Shared identity of the one application served by this runtime. */
extern const uint64_t hzn_applet_resource_user_id;

/* One instance per appletOE service handle. The caller serializes dispatch. */
void hzn_applet_init(HznApplet *applet, uint64_t guest_pid);

/* On a boundary leaves the request unchanged and identifies its target. */
HznAppletReply hzn_applet_send_sync(HznApplet *applet, void *tls, size_t tls_size);

#ifdef __cplusplus
}
#endif
#endif

