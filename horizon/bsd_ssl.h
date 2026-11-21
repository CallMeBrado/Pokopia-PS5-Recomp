/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_BSD_SSL_H
#define POKOPIA_HORIZON_BSD_SSL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { HZN_NET_BOOT_BSD, HZN_NET_BOOT_SSL } HznBsdSslKind;
typedef struct HznBsdSsl HznBsdSsl;
typedef struct HznBsdSslService HznBsdSslService;
typedef struct HznBsdSslDomain HznBsdSslDomain;

typedef struct {
    void *opaque;
    uint64_t guest_pid;
    uint32_t (*clone_session)(void *, const HznBsdSsl *);
} HznBsdSslConfig;

struct HznBsdSsl {
    HznBsdSslConfig config;
    HznBsdSslService *service;
    HznBsdSslDomain *domain;
    bool closed;
};

typedef struct {
    bool handled;
    uint32_t result, command;
} HznBsdSslReply;

bool hzn_bsd_ssl_init(HznBsdSsl *session, HznBsdSslKind kind, const HznBsdSslConfig *config);
/* The loader serializes session operations and owns every moved handle. */
void hzn_bsd_ssl_clone(HznBsdSsl *session, const HznBsdSsl *source);
void hzn_bsd_ssl_close(HznBsdSsl *session);
HznBsdSslReply hzn_bsd_ssl_send_sync(HznBsdSsl *session, uint8_t *tls, size_t size);

#endif
