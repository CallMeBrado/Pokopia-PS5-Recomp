/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "account.h"
#include "../../horizon/account_identity.h"
#include "../../horizon/cmif.h"

#include <string.h>

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_COMBINATION = 0xe801,
    INVALID_OBJECT = 0xf601,
    INVALID_USER_ID = 124 | (20 << 9),
    USER_NOT_EXIST = 124 | (100 << 9),
    NETWORK_ACCOUNT_REGISTRATION_REQUIRED = 124 | (200 << 9),
    DOMAIN_ROOT = 1,
    GET_USER_COUNT = 0,
    GET_USER_EXISTENCE = 1,
    GET_LAST_OPENED_USER = 4,
    GET_PROFILE = 5,
    INITIALIZE_APPLICATION_INFO_V0 = 100,
    GET_BAAS_ACCOUNT_MANAGER = 101,
    INITIALIZE_APPLICATION_INFO_V1 = 140,
    IS_USER_ACCOUNT_SWITCH_LOCKED = 150,
    INITIALIZE_APPLICATION_INFO_V2 = 160,
    CHECK_AVAILABILITY = 0,
    GET_ACCOUNT_ID = 1,
    GET_PROFILE_BASE = 1,
};

static const char account_type[] = "IAccountServiceForApplication";
static const char manager_type[] = "IManagerForApplication";
static const char profile_type[] = "IProfile";

void hzn_account_init(HznAccount *account)
{
    *account = (HznAccount){.next_object_id = DOMAIN_ROOT + 1};
}

static HznAccountReply svc_result(uint32_t result)
{
    return (HznAccountReply){ .kind = HZN_ACCOUNT_REPLY, .result = result };
}

static HznAccountReply boundary(uint32_t command, uint32_t id, const char *type)
{
    return (HznAccountReply){ .kind = HZN_ACCOUNT_BOUNDARY, .command = command,
                              .object_id = id, .object_type = type };
}

static HznAccountReply reply_data(uint8_t *tls, size_t size, bool domain, uint32_t result,
                                  const void *data, size_t data_size)
{
    return svc_result(hzn_cmif_reply_data(tls, size, domain, result, data, data_size)
                      ? 0 : INVALID_COMBINATION);
}

static bool has_descriptors(const HznCmifRequest *request)
{
    return request->has_copy_handle || request->has_buffer_descriptors;
}

static HznAccountReply control(HznAccount *account, uint8_t *tls, size_t size,
                               const HznCmifRequest *request)
{
    if (request->raw_size < 16 || hzn_cmif_read32(request->raw) != HZN_SFCI)
        return svc_result(INVALID_COMBINATION);
    uint32_t command = hzn_cmif_read32(request->raw + 8);
    if (command != 0 && command != 2 && command != 3)
        return boundary(command, 0, account_type);
    if (request->raw_size != 24 || request->has_pid || has_descriptors(request))
        return svc_result(INVALID_COMBINATION);

    if (command == 3) {
        const uint16_t bytes = 0x8000;
        return reply_data(tls, size, false, 0, &bytes, sizeof bytes);
    }
    if (command == 0) {
        const uint32_t id = DOMAIN_ROOT;
        HznAccountReply reply = reply_data(tls, size, false, 0, &id, sizeof id);
        if (!reply.result && !account->domain) {
            account->domain = true;
            account->root_open = true;
        }
        return reply;
    }

    if (size < HZN_CMIF_SESSION_REPLY_BYTES + 8)
        return svc_result(INVALID_COMBINATION);
    if (!account->clone)
        return boundary(command, 0, account_type);
    uint32_t handle = account->clone(account->clone_opaque, account);
    return svc_result(hzn_cmif_reply_move_handle(tls, size, false,
                      handle ? 0 : OUT_OF_RESOURCE, handle) ? 0 : INVALID_COMBINATION);
}

static HznAccountObject *find_object(HznAccount *account, uint32_t id)
{
    for (size_t i = 0; i < sizeof account->objects / sizeof account->objects[0]; ++i)
        if (account->objects[i].id == id)
            return &account->objects[i];
    return NULL;
}

static HznAccountReply open_user_object(HznAccount *account, uint8_t *tls, size_t size,
                                       const HznCmifRequest *request, const uint8_t *cmif,
                                       size_t cmif_bytes, const char *type)
{
    if (cmif_bytes != 32 || request->has_pid || has_descriptors(request))
        return svc_result(INVALID_COMBINATION);
    uint64_t word0 = hzn_cmif_read64(cmif + 16);
    uint64_t word1 = hzn_cmif_read64(cmif + 24);
    if (!word0 && !word1)
        return reply_data(tls, size, true, INVALID_USER_ID, NULL, 0);
    if (word0 != HZN_OFFLINE_UID_WORD0 || word1 != HZN_OFFLINE_UID_WORD1)
        return reply_data(tls, size, true, USER_NOT_EXIST, NULL, 0);
    HznAccountObject *slot = find_object(account, 0);
    if (!slot || account->next_object_id == UINT32_MAX)
        return reply_data(tls, size, true, OUT_OF_RESOURCE, NULL, 0);
    uint32_t id = account->next_object_id;
    if (!hzn_cmif_reply(tls, size, true, 0, &id))
        return svc_result(INVALID_COMBINATION);
    *slot = (HznAccountObject){.id = id, .type = type};
    ++account->next_object_id;
    return svc_result(0);
}

static HznAccountReply manager_request(uint8_t *tls, size_t size, uint32_t id,
                                       const HznCmifRequest *request, const uint8_t *cmif,
                                       size_t cmif_bytes)
{
    if (hzn_cmif_read32(cmif) != HZN_SFCI)
        return svc_result(INVALID_COMBINATION);
    uint32_t command = hzn_cmif_read32(cmif + 8);
    if (command != CHECK_AVAILABILITY && command != GET_ACCOUNT_ID)
        return boundary(command, id, manager_type);
    if (cmif_bytes != 16 || request->has_pid || has_descriptors(request))
        return svc_result(INVALID_COMBINATION);
    if (command == CHECK_AVAILABILITY) {
        /* The SDK converts this result into a successful false response for
         * IsNetworkServiceAccountAvailable. The offline user has no linkage. */
        return reply_data(tls, size, true, NETWORK_ACCOUNT_REGISTRATION_REQUIRED, NULL, 0);
    }
    /* Stable local account identifier, as in the pinned runtime's offline profile. */
    const uint64_t local_account_id = HZN_OFFLINE_UID_WORD0 ^ HZN_OFFLINE_UID_WORD1;
    return reply_data(tls, size, true, 0, &local_account_id, sizeof local_account_id);
}

static HznAccountReply profile_request(uint8_t *tls, size_t size, uint32_t id,
                                       const HznCmifRequest *request, const uint8_t *cmif,
                                       size_t cmif_bytes)
{
    if (hzn_cmif_read32(cmif) != HZN_SFCI)
        return svc_result(INVALID_COMBINATION);
    uint32_t command = hzn_cmif_read32(cmif + 8);
    if (command != GET_PROFILE_BASE)
        return boundary(command, id, profile_type);
    if (cmif_bytes != 16 || request->has_pid || has_descriptors(request))
        return svc_result(INVALID_COMBINATION);

    /* AccountProfileBase for the default offline user. The pinned local
     * profile uses the nickname Player and a fixed 2019-01-01 UTC edit time. */
    static const struct {
        uint64_t uid[2], last_edit_timestamp;
        char nickname[32];
    } profile = {
        .uid = {HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1},
        .last_edit_timestamp = UINT64_C(1546300800),
        .nickname = "Player",
    };
    _Static_assert(sizeof profile == 0x38, "Horizon AccountProfileBase size");
    return reply_data(tls, size, true, 0, &profile, sizeof profile);
}

static HznAccountReply account_request(HznAccount *account, uint8_t *tls, size_t size, bool domain, uint32_t id,
                                       const HznCmifRequest *request, const uint8_t *cmif,
                                       size_t cmif_bytes)
{
    if (cmif_bytes < 16 || hzn_cmif_read32(cmif) != HZN_SFCI)
        return svc_result(INVALID_COMBINATION);
    uint32_t command = hzn_cmif_read32(cmif + 8);
    if (command == GET_BAAS_ACCOUNT_MANAGER || command == GET_PROFILE) {
        if (!domain)
            return boundary(command, id, account_type);
        const char *type = command == GET_PROFILE ? profile_type : manager_type;
        return open_user_object(account, tls, size, request, cmif, cmif_bytes, type);
    }
    if (command == GET_USER_EXISTENCE) {
        const size_t expected = domain ? 32 : 40;
        if (cmif_bytes != expected || request->has_pid || has_descriptors(request))
            return svc_result(INVALID_COMBINATION);
        const uint8_t exists = hzn_cmif_read64(cmif + 16) == HZN_OFFLINE_UID_WORD0 &&
                               hzn_cmif_read64(cmif + 24) == HZN_OFFLINE_UID_WORD1;
        return reply_data(tls, size, domain, 0, &exists, sizeof exists);
    }
    bool initialize = command == INITIALIZE_APPLICATION_INFO_V0 ||
                      command == INITIALIZE_APPLICATION_INFO_V1 ||
                      command == INITIALIZE_APPLICATION_INFO_V2;
    if (!initialize && command != GET_USER_COUNT && command != GET_LAST_OPENED_USER &&
        command != IS_USER_ACCOUNT_SWITCH_LOCKED)
        return boundary(command, id, account_type);

    /* Domain data_size excludes padding. With no descriptors, a session has
     * eight trailing alignment bytes, or four when a PID is included. */
    size_t expected = initialize ? 24 : 16;
    if (!domain)
        expected += initialize ? 4 : 8;
    if (cmif_bytes != expected || has_descriptors(request) || request->has_pid != initialize)
        return svc_result(INVALID_COMBINATION);
    if (initialize) {
        /* The kernel owns the PID descriptor. The ClientProcessId input word
         * is a zero placeholder for this one application process. */
        if (hzn_cmif_read64(cmif + 16) != 0)
            return svc_result(INVALID_COMBINATION);
        return reply_data(tls, size, domain, 0, NULL, 0);
    }
    if (command == GET_USER_COUNT) {
        const int32_t count = 1;
        return reply_data(tls, size, domain, 0, &count, sizeof count);
    }
    if (command == GET_LAST_OPENED_USER) {
        const uint64_t uid[2] = {HZN_OFFLINE_UID_WORD0, HZN_OFFLINE_UID_WORD1};
        return reply_data(tls, size, domain, 0, uid, sizeof uid);
    }

    /* The local account profile has no user-switch lock. */
    const uint8_t locked = 0;
    return reply_data(tls, size, domain, 0, &locked, sizeof locked);
}

HznAccountReply hzn_account_send_sync(HznAccount *account, void *buffer, size_t size)
{
    if (!buffer)
        return svc_result(INVALID_ADDRESS);
    if (size < 8)
        return svc_result(INVALID_COMBINATION);
    uint8_t *tls = buffer;
    uint32_t type = hzn_cmif_read32(tls);
    if ((type == 2 || type == 15) && !hzn_cmif_read32(tls + 4)) {
        /* This closes one handle. Clones still reference the same domain. */
        memset(tls, 0, 8);
        return svc_result(0);
    }

    HznCmifRequest request;
    if (!hzn_cmif_parse(tls, size, &request))
        return svc_result(INVALID_COMBINATION);
    if (request.type == 5 || request.type == 7)
        return control(account, tls, size, &request);
    if (request.type != 4 && request.type != 6)
        return boundary(UINT32_MAX, 0, account_type);

    const uint8_t *raw = request.raw;
    if (!account->domain)
        return account_request(account, tls, size, false, 0, &request, raw, request.raw_size);
    if (request.raw_size < 16)
        return svc_result(INVALID_COMBINATION);

    uint32_t id = hzn_cmif_read32(raw + 4);
    bool open = account->root_open && id == DOMAIN_ROOT;
    HznAccountObject *object = id > DOMAIN_ROOT ? find_object(account, id) : NULL;
    uint32_t header = hzn_cmif_read32(raw);
    if (header == 2) {
        if (request.has_pid || has_descriptors(&request))
            return svc_result(INVALID_COMBINATION);
        HznAccountReply reply = reply_data(tls, size, true, open || object ? 0 : INVALID_OBJECT, NULL, 0);
        if (!reply.result) {
            if (open)
                account->root_open = false;
            if (object)
                *object = (HznAccountObject){0};
        }
        return reply;
    }

    size_t cmif_bytes = header >> 16;
    if ((header & 0xffff) != 1 || cmif_bytes < 16 || cmif_bytes > request.raw_size - 16)
        return svc_result(INVALID_COMBINATION);
    if (open)
        return account_request(account, tls, size, true, id, &request, raw + 16, cmif_bytes);
    if (object && object->type == profile_type)
        return profile_request(tls, size, id, &request, raw + 16, cmif_bytes);
    if (object && object->type == manager_type)
        return manager_request(tls, size, id, &request, raw + 16, cmif_bytes);
    return reply_data(tls, size, true, INVALID_OBJECT, NULL, 0);
}
