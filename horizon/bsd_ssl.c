/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bsd_ssl.h"
#include "cmif.h"

#include <stdlib.h>
#include <string.h>

enum {
    OUT_OF_RESOURCE = 0xce01,
    INVALID_COMBINATION = 0xe801,
    INVALID_OBJECT = 0xf601,
    SESSION_CLOSED = 0xf601,
    DOMAIN_ROOT = 1,
    BSD_START_MONITORING = 1,
    BSD_CONFIGURE_CLIENT = 36,
    SSL_SET_INTERFACE_VERSION = 5,
};

struct HznBsdSslService {
    size_t refs;
    HznBsdSslKind kind;
    uint64_t monitor_pid;
    uint32_t bsd_configuration[4];
    uint32_t ssl_interface_version;
};

struct HznBsdSslDomain {
    size_t refs;
    bool root_open;
};

bool hzn_bsd_ssl_init(HznBsdSsl *session, HznBsdSslKind kind, const HznBsdSslConfig *config)
{
    HznBsdSslService *service = calloc(1, sizeof *service);
    if (!service)
        return false;
    *service = (HznBsdSslService){ .refs = 1, .kind = kind };
    *session = (HznBsdSsl){ .config = *config, .service = service };
    return true;
}

void hzn_bsd_ssl_clone(HznBsdSsl *session, const HznBsdSsl *source)
{
    *session = *source;
    ++session->service->refs;
    if (session->domain)
        ++session->domain->refs;
}

void hzn_bsd_ssl_close(HznBsdSsl *session)
{
    if (session->domain && --session->domain->refs == 0)
        free(session->domain);
    if (session->service && --session->service->refs == 0)
        free(session->service);
    *session = (HznBsdSsl){0};
}

static HznBsdSslReply handled(uint32_t result)
{
    return (HznBsdSslReply){ .handled = true, .result = result };
}

static HznBsdSslReply wrote(bool success)
{
    return handled(success ? 0 : INVALID_COMBINATION);
}

static HznBsdSslReply boundary(uint32_t command)
{
    return (HznBsdSslReply){ .command = command };
}

static HznBsdSslReply control(HznBsdSsl *session, uint8_t *tls, size_t size, const HznCmifRequest *request)
{
    if (request->raw_size < 16 || hzn_cmif_read32(request->raw) != HZN_SFCI || request->has_pid ||
        request->has_copy_handle || request->has_buffer_descriptors)
        return handled(INVALID_COMBINATION);
    const uint32_t command = hzn_cmif_read32(request->raw + 8);
    switch (command) {
    case 0: {
        if (size < HZN_CMIF_SESSION_REPLY_BYTES + sizeof(uint32_t))
            return handled(INVALID_COMBINATION);
        if (!session->domain) {
            session->domain = calloc(1, sizeof *session->domain);
            if (!session->domain)
                return wrote(hzn_cmif_reply(tls, size, false, OUT_OF_RESOURCE, NULL));
            *session->domain = (HznBsdSslDomain){ .refs = 1, .root_open = true };
        }
        const uint32_t root = DOMAIN_ROOT;
        return wrote(hzn_cmif_reply(tls, size, false, 0, &root));
    }
    case 2:
    case 4: {
        if (request->raw_size < (command == 4 ? 20u : 16u) || size < HZN_CMIF_SESSION_REPLY_BYTES + 8)
            return handled(INVALID_COMBINATION);
        if (!session->config.clone_session)
            return boundary(command);
        const uint32_t child = session->config.clone_session(session->config.opaque, session);
        return wrote(hzn_cmif_reply_move_handle(tls, size, false, child ? 0 : OUT_OF_RESOURCE, child));
    }
    case 3: {
        const uint32_t capacity = 0x8000;
        return wrote(hzn_cmif_reply(tls, size, false, 0, &capacity));
    }
    default:
        return boundary(command);
    }
}

static HznBsdSslReply call_bsd(HznBsdSsl *session, uint8_t *tls, size_t size, uint32_t command,
                               const uint8_t *input, size_t bytes, bool domain, bool has_pid)
{
    if (command == BSD_CONFIGURE_CLIENT) {
        if (!domain || !has_pid || bytes != sizeof session->service->bsd_configuration)
            return handled(INVALID_COMBINATION);
        uint32_t configuration[4];
        memcpy(configuration, input, sizeof configuration);
        /* This guest network profile has no optional BSD capabilities. */
        const uint32_t capabilities = 0;
        const bool success = hzn_cmif_reply_data(tls, size, true, 0, &capabilities, sizeof capabilities);
        if (success)
            memcpy(session->service->bsd_configuration, configuration, sizeof configuration);
        return wrote(success);
    }
    if (command == BSD_START_MONITORING) {
        if (domain || !has_pid || bytes < sizeof(uint64_t) || !session->config.guest_pid)
            return handled(INVALID_COMBINATION);
        const uint64_t pid = hzn_cmif_read64(input);
        if (pid && pid != session->config.guest_pid)
            return handled(INVALID_COMBINATION);
        const bool success = hzn_cmif_reply(tls, size, false, 0, NULL);
        if (success)
            session->service->monitor_pid = session->config.guest_pid;
        return wrote(success);
    }
    return boundary(command);
}

HznBsdSslReply hzn_bsd_ssl_send_sync(HznBsdSsl *session, uint8_t *tls, size_t size)
{
    if (!session->service || session->closed)
        return handled(SESSION_CLOSED);
    if (!tls || size < 8)
        return handled(INVALID_COMBINATION);
    const uint32_t type = hzn_cmif_read32(tls);
    if ((type == 2 || type == 15) && !hzn_cmif_read32(tls + 4)) {
        memset(tls, 0, 8);
        session->closed = true;
        return handled(0);
    }

    HznCmifRequest request;
    if (!hzn_cmif_parse(tls, size, &request))
        return handled(INVALID_COMBINATION);
    if (request.type == 5 || request.type == 7)
        return control(session, tls, size, &request);
    if (request.type != 4 && request.type != 6)
        return boundary(UINT32_MAX);
    if (request.has_copy_handle || request.has_buffer_descriptors || request.raw_size < 16)
        return handled(INVALID_COMBINATION);

    const bool domain = session->domain != NULL;
    const uint8_t *cmif = request.raw;
    size_t bytes = request.raw_size;
    if (domain) {
        const uint32_t header = hzn_cmif_read32(cmif);
        const bool root = session->domain->root_open && hzn_cmif_read32(cmif + 4) == DOMAIN_ROOT;
        if (header == 2) {
            if (request.has_pid)
                return handled(INVALID_COMBINATION);
            const bool success = hzn_cmif_reply(tls, size, true, root ? 0 : INVALID_OBJECT, NULL);
            if (success && root)
                session->domain->root_open = false;
            return wrote(success);
        }
        bytes = header >> 16;
        if ((header & 0xffff) != 1 || bytes < 16 || bytes > request.raw_size - 16)
            return handled(INVALID_COMBINATION);
        if (!root)
            return wrote(hzn_cmif_reply(tls, size, true, INVALID_OBJECT, NULL));
        cmif += 16;
    }
    if (hzn_cmif_read32(cmif) != HZN_SFCI)
        return handled(INVALID_COMBINATION);
    const uint32_t command = hzn_cmif_read32(cmif + 8);
    if (session->service->kind == HZN_NET_BOOT_BSD)
        return call_bsd(session, tls, size, command, cmif + 16, bytes - 16, domain, request.has_pid);
    if (command != SSL_SET_INTERFACE_VERSION)
        return boundary(command);
    if (!domain || request.has_pid || bytes != 20)
        return handled(INVALID_COMBINATION);
    const uint32_t version = hzn_cmif_read32(cmif + 16);
    const bool success = hzn_cmif_reply(tls, size, true, 0, NULL);
    if (success)
        session->service->ssl_interface_version = version;
    return wrote(success);
}
