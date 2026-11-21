/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "time_service.h"
#include "cmif.h"

#include <string.h>

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_COMBINATION = 0xe801,
    SESSION_CLOSED = 0xf601,
    DOMAIN_ROOT = 1,
    GET_TIME_ZONE_SERVICE = 3,
    GET_SHARED_MEMORY_NATIVE_HANDLE = 20,
    TO_CALENDAR_TIME_WITH_MY_RULE = 101,
    TIME_ZONE_CONVERSION_FAILED = 116 | (903 << 9),
    NS_PER_SECOND = 1000000000,
    SECONDS_PER_DAY = 86400,
};

typedef struct {
    int64_t offset_ns;
    uint8_t source_id[16];
} SteadyContext;

typedef struct {
    int64_t offset_sec, point_sec;
    uint8_t source_id[16];
} SystemContext;

typedef struct {
    uint64_t start_ns;
    int64_t multiplier, shift, initial_ns, target_ns;
    uint8_t source_id[16];
} ContinuousAdjustment;

_Static_assert(sizeof(SteadyContext) == 0x18, "steady clock ABI");
_Static_assert(sizeof(SystemContext) == 0x20, "system clock ABI");
_Static_assert(sizeof(ContinuousAdjustment) == 0x38, "adjustment clock ABI");

typedef struct {
    int16_t year;
    int8_t month, day, hour, minute, second;
    uint8_t pad;
    uint32_t weekday, yearday;
    char abbreviation[8];
    int32_t is_dst, utc_offset;
} CalendarResult;

_Static_assert(offsetof(CalendarResult, weekday) == 8, "calendar time ABI");
_Static_assert(sizeof(CalendarResult) == 0x20, "calendar reply ABI");

static bool utc_calendar(int64_t posix, CalendarResult *out)
{
    int64_t days = posix / SECONDS_PER_DAY;
    int64_t seconds = posix % SECONDS_PER_DAY;
    if (seconds < 0) {
        --days;
        seconds += SECONDS_PER_DAY;
    }

    /* Howard Hinnant's civil_from_days, also used by the pinned runtime. */
    const int64_t march_days = days + 719468;
    const int64_t era = (march_days >= 0 ? march_days : march_days - 146096) / 146097;
    const int64_t day_of_era = march_days - era * 146097;
    const int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int64_t march_day = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const int64_t march_month = (5 * march_day + 2) / 153;
    const int64_t month = march_month < 10 ? march_month + 3 : march_month - 9;
    const int64_t day = march_day - (153 * march_month + 2) / 5 + 1;
    const int64_t year = year_of_era + era * 400 + (month <= 2);
    if (year < INT16_MIN || year > INT16_MAX)
        return false;

    static const uint16_t days_before_month[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int64_t weekday = (days + 4) % 7;
    *out = (CalendarResult){
        .year = (int16_t)year, .month = (int8_t)month, .day = (int8_t)day,
        .hour = (int8_t)(seconds / 3600), .minute = (int8_t)(seconds / 60 % 60),
        .second = (int8_t)(seconds % 60), .weekday = (uint32_t)(weekday < 0 ? weekday + 7 : weekday),
        .yearday = (uint32_t)(days_before_month[month - 1] + day - 1 + (leap && month > 2)),
        .abbreviation = "UTC",
    };
    return true;
}

static void seed_slot(uint8_t *block, size_t first_slot, const void *data, size_t size)
{
    const uint32_t index = 1;
    memcpy(block + first_slot + size, data, size);
    memcpy(block, &index, sizeof index);
}

bool hzn_time_init_page(void *page, size_t size, const HznTimeConfig *config)
{
    static const uint8_t zero_source[16];
    if (!page || size < HZN_TIME_PAGE_BYTES || !config || config->tick_ns > INT64_MAX ||
        !memcmp(config->source_id, zero_source, sizeof zero_source))
        return false;

    int64_t utc_seconds = config->utc_ns / NS_PER_SECOND;
    int64_t utc_fraction = config->utc_ns % NS_PER_SECOND;
    if (utc_fraction < 0) {
        --utc_seconds;
        utc_fraction += NS_PER_SECOND;
    }
    int64_t raw = (int64_t)config->tick_ns;
    int64_t raw_fraction = raw % NS_PER_SECOND;
    if (raw - raw_fraction > INT64_MAX - utc_fraction)
        return false;

    /* The lifted SDK adds an adjusted steady timespec to a whole-second
     * system offset. A fixed phase preserves both samples to the nanosecond. */
    int64_t phase = utc_fraction - raw_fraction;
    int64_t steady_seconds = raw / NS_PER_SECOND;
    SteadyContext steady = { .offset_ns = phase };
    SystemContext wall = { .offset_sec = utc_seconds - steady_seconds, .point_sec = steady_seconds };
    ContinuousAdjustment adjust = { .start_ns = config->tick_ns, .initial_ns = phase, .target_ns = phase };
    memcpy(steady.source_id, config->source_id, sizeof steady.source_id);
    memcpy(wall.source_id, config->source_id, sizeof wall.source_id);
    memcpy(adjust.source_id, config->source_id, sizeof adjust.source_id);

    const uint8_t auto_correct = 0;
    uint8_t *bytes = page;
    memset(bytes, 0, HZN_TIME_PAGE_BYTES);
    seed_slot(bytes + 0x000, 8, &steady, sizeof steady);
    seed_slot(bytes + 0x038, 8, &wall, sizeof wall);
    /* Both system contexts start from the host UTC source. */
    seed_slot(bytes + 0x080, 8, &wall, sizeof wall);
    seed_slot(bytes + 0x0c8, 4, &auto_correct, sizeof auto_correct);
    seed_slot(bytes + 0x0d0, 8, &adjust, sizeof adjust);
    return true;
}

bool hzn_time_init(HznTime *time, const HznTimeConfig *config)
{
    if (!time || !config || !config->copy_shared_read)
        return false;
    *time = (HznTime){ .opaque = config->opaque, .copy_shared_read = config->copy_shared_read,
                      .next_id = DOMAIN_ROOT + 1 };
    return true;
}

static HznTimeReply svc_result(uint32_t result)
{
    return (HznTimeReply){ .kind = HZN_TIME_REPLY, .result = result };
}

static HznTimeReply boundary(uint32_t command, uint32_t id, const char *type)
{
    return (HznTimeReply){ .kind = HZN_TIME_BOUNDARY, .command = command,
                           .object_id = id, .object_type = type };
}

static HznTimeReply reply_cmif(uint8_t *tls, size_t size, bool domain, uint32_t result,
                               const uint32_t *output)
{
    return svc_result(hzn_cmif_reply(tls, size, domain, result, output) ? 0 : INVALID_COMBINATION);
}

static uint32_t *find_timezone(HznTime *time, uint32_t id)
{
    for (size_t i = 0; i < sizeof time->timezone_ids / sizeof time->timezone_ids[0]; ++i)
        if (time->timezone_ids[i] == id)
            return &time->timezone_ids[i];
    return NULL;
}

static HznTimeReply get_timezone(HznTime *time, uint8_t *tls, size_t size)
{
    uint32_t *slot = find_timezone(time, 0);
    if (!slot || time->next_id == UINT32_MAX)
        return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
    HznTimeReply reply = reply_cmif(tls, size, true, 0, &time->next_id);
    if (!reply.result)
        *slot = time->next_id++;
    return reply;
}

HznTimeReply hzn_time_send_sync(HznTime *time, void *buffer, size_t size)
{
    if (!time || !buffer)
        return svc_result(INVALID_ADDRESS);
    if (time->closed)
        return svc_result(SESSION_CLOSED);
    if (size < 8)
        return svc_result(INVALID_COMBINATION);
    uint8_t *tls = buffer;
    uint32_t type = hzn_cmif_read32(tls);
    if ((type == 2 || type == 15) && !hzn_cmif_read32(tls + 4)) {
        memset(tls, 0, 8);
        time->closed = true;
        return svc_result(0);
    }

    HznCmifRequest request;
    if (!hzn_cmif_parse_with_output(tls, size, &request))
        return svc_result(INVALID_COMBINATION);
    const uint8_t *raw = request.raw;
    bool has_extras = request.has_copy_handle || request.has_pid || request.has_buffer_descriptors ||
                      request.has_output_buffer;
    if (request.type == 5 || request.type == 7) {
        if (request.raw_size < 16 || hzn_cmif_read32(raw) != HZN_SFCI || has_extras)
            return svc_result(INVALID_COMBINATION);
        uint32_t command = hzn_cmif_read32(raw + 8);
        if (command == 3) {
            const uint32_t capacity = 0x8000;
            return reply_cmif(tls, size, false, 0, &capacity);
        }
        if (command == 0) {
            const uint32_t id = DOMAIN_ROOT;
            HznTimeReply reply = reply_cmif(tls, size, false, 0, &id);
            if (!reply.result && !time->domain) {
                time->domain = true;
                time->root_open = true;
            }
            return reply;
        }
        return boundary(command, 0, "IStaticService");
    }
    if (request.type != 4 && request.type != 6)
        return boundary(UINT32_MAX, 0, "IStaticService");
    if (!time->domain)
        return boundary(request.raw_size >= 16 && hzn_cmif_read32(raw) == HZN_SFCI
                        ? hzn_cmif_read32(raw + 8) : UINT32_MAX, 0, "IStaticService");
    if (request.raw_size < 16)
        return svc_result(INVALID_COMBINATION);

    uint32_t id = hzn_cmif_read32(raw + 4);
    bool root = id == DOMAIN_ROOT && time->root_open;
    uint32_t *timezone = id > DOMAIN_ROOT ? find_timezone(time, id) : NULL;
    size_t in_size = hzn_cmif_read32(raw) >> 16;
    if (raw[0] == 2) {
        if (raw[1] || in_size || has_extras)
            return svc_result(INVALID_COMBINATION);
        HznTimeReply reply = reply_cmif(tls, size, true, 0, NULL);
        if (!reply.result) {
            if (root)
                time->root_open = false;
            if (timezone)
                *timezone = 0;
        }
        return reply;
    }
    if (raw[0] != 1 || raw[1] || in_size < 16 || in_size > request.raw_size - 16 ||
        hzn_cmif_read32(raw + 16) != HZN_SFCI)
        return svc_result(INVALID_COMBINATION);

    uint32_t command = hzn_cmif_read32(raw + 24);
    if (timezone) {
        if (command != TO_CALENDAR_TIME_WITH_MY_RULE)
            return boundary(command, id, "ITimeZoneService");
        if (in_size != 24 || has_extras)
            return svc_result(INVALID_COMBINATION);
        int64_t posix;
        memcpy(&posix, raw + 32, sizeof posix);
        /* The PS4 host exposes UTC as the device rule. */
        CalendarResult calendar;
        if (!utc_calendar(posix, &calendar))
            return reply_cmif(tls, size, true, TIME_ZONE_CONVERSION_FAILED, NULL);
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &calendar, sizeof calendar) ? 0 : INVALID_COMBINATION);
    }
    if (!root)
        return reply_cmif(tls, size, true, SESSION_CLOSED, NULL);
    if (command != GET_TIME_ZONE_SERVICE && command != GET_SHARED_MEMORY_NATIVE_HANDLE)
        return boundary(command, id, "IStaticService");
    if (in_size != 16 || has_extras)
        return svc_result(INVALID_COMBINATION);
    if (command == GET_TIME_ZONE_SERVICE)
        return get_timezone(time, tls, size);

    if (size < HZN_CMIF_DOMAIN_REPLY_BYTES + 8)
        return svc_result(INVALID_COMBINATION);
    uint32_t handle = time->copy_shared_read(time->opaque);
    if (!handle)
        return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
    return svc_result(hzn_cmif_reply_copy_handle(tls, size, true, 0, handle) ? 0 : INVALID_COMBINATION);
}
