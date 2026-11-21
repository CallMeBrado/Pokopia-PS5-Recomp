/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "time_service.h"
#include "cmif.h"

#include <string.h>
#include <time.h>

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_COMBINATION = 0xe801,
    SESSION_CLOSED = 0xf601,
    DOMAIN_ROOT = 1,
    GET_TIME_ZONE_SERVICE = 3,
    GET_SHARED_MEMORY_NATIVE_HANDLE = 20,
    TO_CALENDAR_TIME_WITH_MY_RULE = 101,
    TO_POSIX_TIME_WITH_MY_RULE = 202,
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
} CalendarTime;

typedef struct {
    CalendarTime time;
    uint32_t weekday, yearday;
    char abbreviation[8];
    int32_t is_dst, utc_offset;
} CalendarResult;

_Static_assert(sizeof(CalendarTime) == 8 && offsetof(CalendarResult, weekday) == 8, "calendar time ABI");
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
        .time = { .year = (int16_t)year, .month = (int8_t)month, .day = (int8_t)day,
                  .hour = (int8_t)(seconds / 3600), .minute = (int8_t)(seconds / 60 % 60),
                  .second = (int8_t)(seconds % 60) },
        .weekday = (uint32_t)(weekday < 0 ? weekday + 7 : weekday),
        .yearday = (uint32_t)(days_before_month[month - 1] + day - 1 + (leap && month > 2)),
        .abbreviation = "UTC",
    };
    return true;
}

static bool utc_posix(const CalendarTime *time, int64_t *posix)
{
    if (time->month < 1 || time->month > 12 || time->day < 1 || time->day > 31)
        return false;
    /* The inverse days_from_civil calculation. Calendar overflow normalizes
     * as in the pinned time-zone service's mktime-style conversion. */
    const int64_t year = time->year - (time->month <= 2);
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t year_of_era = year - era * 400;
    const int64_t march_month = time->month > 2 ? time->month - 3 : time->month + 9;
    const int64_t march_day = (153 * march_month + 2) / 5 + time->day - 1;
    const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + march_day;
    const int64_t days = era * 146097 + day_of_era - 719468;
    *posix = days * SECONDS_PER_DAY + time->hour * 3600 + time->minute * 60 + time->second;
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
                      .write_guest = config->write_guest, .next_id = DOMAIN_ROOT + 1 };
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

/* R59: clock objects. IStaticService 0/1/4/5 return an ISystemClock (user,
 * network, local, ephemeral network) and 2 an ISteadyClock. Both answer from
 * the host clock: system time is UTC seconds, the steady clock counts seconds
 * since the service first needed it, under one fixed source UUID. */
enum { CLOCK_SYSTEM = 1, CLOCK_STEADY = 2 };

static int64_t host_utc_seconds(void)
{
    struct timespec now;
#if defined(__PROSPERO__)
    /* The PS5 libc has no C11 timespec_get. */
    return clock_gettime(CLOCK_REALTIME, &now) == 0 ? (int64_t)now.tv_sec : 0;
#else
    return timespec_get(&now, TIME_UTC) ? (int64_t)now.tv_sec : 0;
#endif
}

static int64_t steady_seconds(void)
{
    static int64_t origin;
    const int64_t now = host_utc_seconds();
    if (!origin)
        origin = now - 1; /* time points start at 1, so 0 never looks unset */
    return now - origin;
}

typedef struct {
    int64_t time_point;
    uint8_t source[16];
} SteadyClockTimePoint;

static void steady_point(SteadyClockTimePoint *point)
{
    static const uint8_t source[16] = {0x50, 0x6f, 0x6b, 0x6f, 0x70, 0x69, 0x61, 0x2d,
                                       0x50, 0x53, 0x34, 0x2d, 0x63, 0x6c, 0x6b, 0x01};
    point->time_point = steady_seconds();
    memcpy(point->source, source, sizeof source);
}

static int find_clock(const HznTime *time, uint32_t id)
{
    for (size_t i = 0; i < sizeof time->clock_ids / sizeof time->clock_ids[0]; ++i)
        if (time->clock_ids[i] == id)
            return (int)i;
    return -1;
}

static HznTimeReply open_clock(HznTime *time, uint8_t kind, uint8_t *tls, size_t size)
{
    const int slot = find_clock(time, 0);
    if (slot < 0 || time->next_id == UINT32_MAX)
        return reply_cmif(tls, size, true, OUT_OF_RESOURCE, NULL);
    HznTimeReply reply = reply_cmif(tls, size, true, 0, &time->next_id);
    if (!reply.result) {
        time->clock_ids[slot] = time->next_id++;
        time->clock_kinds[slot] = kind;
    }
    return reply;
}

static HznTimeReply clock_command(HznTime *time, int slot, uint32_t command, size_t in_size,
                                  uint8_t *tls, size_t size)
{
    if (time->clock_kinds[slot] == CLOCK_SYSTEM) {
        if (command == 0) { /* GetCurrentTime */
            const int64_t now = host_utc_seconds();
            return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &now, sizeof now) ? 0 : INVALID_COMBINATION);
        }
        if (command == 1 || command == 3) /* SetCurrentTime, SetSystemClockContext: accept, keep host time */
            return reply_cmif(tls, size, true, 0, NULL);
        if (command == 2) { /* GetSystemClockContext: offset plus the steady time point it is based on */
            struct { int64_t offset; SteadyClockTimePoint point; } context;
            steady_point(&context.point);
            context.offset = host_utc_seconds() - context.point.time_point;
            return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &context, sizeof context) ? 0 : INVALID_COMBINATION);
        }
        return boundary(command, time->clock_ids[slot], "ISystemClock");
    }
    if (command == 0) { /* GetCurrentTimePoint */
        SteadyClockTimePoint point;
        steady_point(&point);
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &point, sizeof point) ? 0 : INVALID_COMBINATION);
    }
    if (command == 2 || command == 200) { /* GetTestOffset, GetInternalOffset */
        const int64_t zero = 0;
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &zero, sizeof zero) ? 0 : INVALID_COMBINATION);
    }
    (void)in_size;
    return boundary(command, time->clock_ids[slot], "ISteadyClock");
}

static HznTimeReply to_posix(HznTime *time, const HznCmifRequest *request, uint8_t *tls, size_t size)
{
    if (!time->write_guest || !request->output.address || request->output.size < sizeof(int64_t))
        return svc_result(INVALID_COMBINATION);
    CalendarTime calendar;
    memcpy(&calendar, request->raw + 32, sizeof calendar);
    int64_t posix;
    const uint32_t count = utc_posix(&calendar, &posix) ? 1 : 0;
    void *output = NULL;
    if (count) {
        output = time->write_guest(time->opaque, request->output.address, sizeof posix);
        if (!output)
            return svc_result(INVALID_ADDRESS);
        const uintptr_t address = (uintptr_t)output, message = (uintptr_t)tls;
        if (address >= message ? address - message < size : message - address < sizeof posix)
            return svc_result(INVALID_COMBINATION);
    }
    if (!hzn_cmif_reply_data(tls, size, true, 0, &count, sizeof count))
        return svc_result(INVALID_COMBINATION);
    if (output)
        memcpy(output, &posix, sizeof posix);
    return svc_result(0);
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
    const bool pointer_output = !hzn_cmif_parse_with_output(tls, size, &request);
    if (pointer_output && (!time->domain || !hzn_cmif_parse_with_pointer_output(tls, size, &request)))
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
            const int clock = id > DOMAIN_ROOT ? find_clock(time, id) : -1;
            if (clock >= 0)
                time->clock_ids[clock] = 0;
        }
        return reply;
    }
    if (raw[0] != 1 || raw[1] || in_size < 16 || in_size > request.raw_size - 16 ||
        hzn_cmif_read32(raw + 16) != HZN_SFCI)
        return svc_result(INVALID_COMBINATION);

    uint32_t command = hzn_cmif_read32(raw + 24);
    if (pointer_output && (!timezone || command != TO_POSIX_TIME_WITH_MY_RULE))
        return svc_result(INVALID_COMBINATION);
    if (timezone) {
        if (command == TO_POSIX_TIME_WITH_MY_RULE) {
            if (in_size != 24 || !pointer_output)
                return svc_result(INVALID_COMBINATION);
            return to_posix(time, &request, tls, size);
        }
        /* R60: location queries. The host converts with UTC (see below), so the
         * device location is reported as "UTC", one location, rule version "2024a". */
        if (command == 0 && in_size == 16 && !has_extras) { /* GetDeviceLocationName */
            char location[0x24] = "UTC";
            return svc_result(hzn_cmif_reply_data(tls, size, true, 0, location, sizeof location) ? 0 : INVALID_COMBINATION);
        }
        if (command == 1 && !has_extras) /* SetDeviceLocationName: accept, keep UTC */
            return reply_cmif(tls, size, true, 0, NULL);
        if (command == 2 && in_size == 16 && !has_extras) { /* GetTotalLocationNameCount */
            const uint32_t count = 1;
            return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &count, sizeof count) ? 0 : INVALID_COMBINATION);
        }
        if (command == 5 && in_size == 16 && !has_extras) { /* GetTimeZoneRuleVersion */
            char version[0x10] = "2024a";
            return svc_result(hzn_cmif_reply_data(tls, size, true, 0, version, sizeof version) ? 0 : INVALID_COMBINATION);
        }
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
    const int clock = id > DOMAIN_ROOT ? find_clock(time, id) : -1;
    if (clock >= 0) {
        if (has_extras)
            return svc_result(INVALID_COMBINATION);
        return clock_command(time, clock, command, in_size, tls, size);
    }
    if (!root)
        return reply_cmif(tls, size, true, SESSION_CLOSED, NULL);
    if ((command <= 2 || command == 4 || command == 5) && in_size == 16 && !has_extras)
        return open_clock(time, command == 2 ? CLOCK_STEADY : CLOCK_SYSTEM, tls, size);
    if (command == 100 && in_size == 16 && !has_extras) {
        /* IsStandardUserSystemClockAutomaticCorrectionEnabled: no network correction here */
        const uint8_t enabled = 0;
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, &enabled, sizeof enabled) ? 0 : INVALID_COMBINATION);
    }
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
