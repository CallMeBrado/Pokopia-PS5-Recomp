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
} HznAppletObject;

typedef enum {
    HZN_APPLET_SUSPEND_TICKS,
    HZN_APPLET_GPU_ERROR,
    HZN_APPLET_MESSAGE,
    HZN_APPLET_OPERATION_MODE_CHANGED,
    HZN_APPLET_FRIEND_INVITATION,
} HznAppletEvent;

enum { HZN_APPLET_NO_MESSAGES = 0x680 };

/* Allocate a fresh readable handle to the shared event, or return zero. */
typedef uint32_t (*HznAppletCopyEvent)(void *opaque, HznAppletEvent event);
/* Dequeue the oldest message and clear the event when the queue is empty.
 * Return a CMIF result and write the message on success. */
typedef uint32_t (*HznAppletReceiveMessage)(void *opaque, uint32_t *message);
/* Validate the complete output range and return its writable host mapping. */
typedef void *(*HznAppletWriteGuest)(void *opaque, uint64_t address, size_t size);
/* Ensure the account save for the validated user and return a Horizon FS result. */
typedef uint32_t (*HznAppletEnsureSave)(void *opaque, uint64_t uid_hi, uint64_t uid_lo);

typedef struct {
    uint64_t process_id;
    HznAppletCopyEvent copy_event;
    HznAppletReceiveMessage receive_message;
    void *event_opaque;
    HznAppletWriteGuest write_guest;
    void *write_guest_opaque;
    HznAppletEnsureSave ensure_save;
    void *save_opaque;
    uint32_t next_object_id;
    uint32_t cpu_boost_mode;
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
    HznAppletObject objects[16];
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

