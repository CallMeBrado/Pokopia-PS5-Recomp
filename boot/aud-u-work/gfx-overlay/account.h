/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_ACCOUNT_H
#define POKOPIA_HORIZON_ACCOUNT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HznAccount HznAccount;
typedef uint32_t (*HznAccountClone)(void *opaque, HznAccount *same_domain);

typedef struct {
    uint32_t id;
    const char *type;
} HznAccountObject;

struct HznAccount {
    HznAccountClone clone;
    void *clone_opaque;
    bool domain, root_open;
    uint32_t next_object_id;
    HznAccountObject objects[16];
};

typedef enum { HZN_ACCOUNT_REPLY, HZN_ACCOUNT_BOUNDARY } HznAccountReplyKind;

typedef struct {
    HznAccountReplyKind kind;
    uint32_t result;
    uint32_t command;
    uint32_t object_id;
    const char *object_type;
} HznAccountReply;

/* One instance per SM acquisition, shared by cloned handles. The caller owns
 * handle lifetime and serializes IPC. */
void hzn_account_init(HznAccount *account);

/* A boundary leaves the request intact and identifies its target. */
HznAccountReply hzn_account_send_sync(HznAccount *account, void *tls, size_t tls_size);

#ifdef __cplusplus
}
#endif
#endif
