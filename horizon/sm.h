/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_SM_H
#define POKOPIA_HORIZON_SM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t handle;
    uint64_t registered_pid;
} HznSmSession;

typedef struct {
    uint64_t process_id;
    uint32_t first_handle, next_handle, handle_limit;
    HznSmSession sessions[8];
} HznSm;

typedef enum {
    HZN_SM_REPLY,
    HZN_SM_SERVICE_BOUNDARY,
    HZN_SM_COMMAND_BOUNDARY,
} HznSmReplyKind;

typedef struct {
    HznSmReplyKind kind;
    uint32_t result;
    uint32_t command;
    char service[9];
} HznSmReply;

/* Allocate from an unused handle range in the caller's boot process.
 * The boot dispatcher serializes access to one HznSm. */
bool hzn_sm_init(HznSm *sm, uint64_t guest_pid, uint32_t first_handle, uint32_t handle_limit);

/* ConnectToNamedPort: result in w0 and the new client handle in w1. */
uint32_t hzn_sm_connect(HznSm *sm, const char *name, size_t name_bytes,
                        uint32_t *handle);

/* Includes retired handles so the dispatcher can route stale handles here.
 * A caller-owned service handle is dispatched by the caller first. */
bool hzn_sm_owns_handle(const HznSm *sm, uint32_t handle);
uint32_t hzn_sm_allocate_handle(HznSm *sm);
uint32_t hzn_sm_close_handle(HznSm *sm, uint32_t handle);

/* Service-manager TIPC on the calling thread's TLS message buffer.
 * On a boundary the request buffer is untouched. On a reply result is the
 * SendSyncRequest SVC result, separate from any service result in the reply. */
HznSmReply hzn_sm_send_sync(HznSm *sm, uint32_t handle, void *tls,
                            size_t tls_size);

/* Finish a validated GetServiceHandle request for a service owned by the
 * caller. All sessions draw their handles from the same process namespace. */
HznSmReply hzn_sm_move_service(HznSm *sm, void *tls, size_t tls_size,
                              uint32_t *handle);

#ifdef __cplusplus
}
#endif
#endif
