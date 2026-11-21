/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hid.h"
#include "cmif.h"

#include <stdatomic.h>
#include <string.h>

enum {
    INVALID_ADDRESS = 0xcc01,
    OUT_OF_RESOURCE = 0xce01,
    INVALID_COMBINATION = 0xe801,
    HANDLE_REPLY_BYTES = HZN_CMIF_SESSION_REPLY_BYTES + 8,
};

static HznHidReply svc_result(uint32_t result, uint32_t command)
{
    return (HznHidReply){.handled = true, .result = result, .command = command};
}

enum {
    ACTIVATE_TOUCH_SCREEN = 11,
    SET_SUPPORTED_STYLES = 100,
    SET_SUPPORTED_IDS = 102,
    ACTIVATE_NPAD = 103,
    DEACTIVATE_NPAD = 104,
    ACTIVATE_NPAD_WITH_REVISION = 109,
};

void hzn_hid_init(HznHidState *state, uint64_t aruid)
{
    *state = (HznHidState){.aruid = aruid, .styles = 0x1f, .ids = 0x1ff};
}

static uint32_t npad_id_bit(uint32_t id)
{
    if (id < 8)
        return 1u << id;
    if (id == 0x20)
        return 1u << 8;
    if (id == 0x10)
        return 1u << 9;
    return 0;
}

static HznHidReply configure(HznHidState *state, const HznHidConfig *config, uint8_t *tls, size_t size,
                             const HznCmifRequest *request, uint32_t command)
{
    const bool ids = command == SET_SUPPORTED_IDS;
    const bool inline_word = command == SET_SUPPORTED_STYLES || command == ACTIVATE_NPAD_WITH_REVISION;
    if (!ids && !inline_word && command != ACTIVATE_TOUCH_SCREEN &&
        command != ACTIVATE_NPAD && command != DEACTIVATE_NPAD)
        return (HznHidReply){.command = command};

    /* SDK 0x7500171fc8 writes PID+aRUID, 0x750017f36c adds a u32
     * before aligned aRUID, and 0x7500671234 includes the Npad ID buffer. */
    if (!request->has_pid || request->has_copy_handle || request->has_buffer_descriptors != ids ||
        request->raw_size != (ids || inline_word ? 36u : 28u) ||
        hzn_cmif_read64(request->raw + (inline_word ? 24 : 16)) != state->aruid)
        return svc_result(INVALID_COMBINATION, command);

    HznHidState next = *state;
    if (ids) {
        const size_t bytes = (size_t)request->input.size;
        if (request->input.size > 40 || bytes % 4)
            return svc_result(INVALID_COMBINATION, command);
        const uint8_t *data = bytes && config && config->read_guest ?
            config->read_guest(config->opaque, request->input.address, bytes) : NULL;
        if (bytes && !data)
            return svc_result(INVALID_ADDRESS, command);
        next.ids = 0;
        for (size_t i = 0; i < bytes; i += 4)
            next.ids |= npad_id_bit(hzn_cmif_read32(data + i));
    } else if (command == SET_SUPPORTED_STYLES) {
        next.styles = hzn_cmif_read32(request->raw + 16);
    } else if (command == ACTIVATE_TOUCH_SCREEN) {
        next.touch_active = true;
    } else {
        next.npad_active = command != DEACTIVATE_NPAD;
    }
    if (!hzn_cmif_reply(tls, size, false, 0, NULL))
        return svc_result(INVALID_COMBINATION, command);
    *state = next;
    return svc_result(0, command);
}

HznHidReply hzn_hid_send_sync(HznHidState *state, bool resource, const HznHidConfig *config,
                             uint8_t *tls, size_t size)
{
    if (!state || !tls)
        return svc_result(INVALID_ADDRESS, UINT32_MAX);
    if (size < 8)
        return svc_result(INVALID_COMBINATION, UINT32_MAX);
    if (hzn_cmif_read32(tls) == 2 && !hzn_cmif_read32(tls + 4)) {
        memset(tls, 0, 8);
        return svc_result(0, UINT32_MAX);
    }

    HznCmifRequest request;
    if (!hzn_cmif_parse(tls, size, &request))
        return svc_result(INVALID_COMBINATION, UINT32_MAX);
    if (request.raw_size < 16 || hzn_cmif_read32(request.raw) != HZN_SFCI)
        return (HznHidReply){.command = UINT32_MAX};
    const uint32_t command = hzn_cmif_read32(request.raw + 8);
    if (hzn_cmif_read32(request.raw + 4) != 1)
        return (HznHidReply){.command = command};
    if (!resource && request.type == 7 && command == 3) {
        /* The SetSupportedNpadIdType SDK proxy queries its pointer buffer
         * capacity before issuing command 102 for the first time. */
        const uint16_t pointer_buffer_bytes = 0x8000;
        if (request.raw_size != 24 || request.has_pid || request.has_copy_handle || request.has_buffer_descriptors)
            return svc_result(INVALID_COMBINATION, command);
        return svc_result(hzn_cmif_reply_data(tls, size, false, 0, &pointer_buffer_bytes, sizeof pointer_buffer_bytes)
                          ? 0 : INVALID_COMBINATION, command);
    }
    if (request.type != 6 || (resource && command))
        return (HznHidReply){.command = command};
    if (command)
        return configure(state, config, tls, size, &request, command);

    /* SDK 0x75001c3aa0 sends a PID and aRUID for CreateAppletResource.
     * The child proxy 0x7500117f10 sends no inputs for GetSharedMemoryHandle. */
    if (request.has_pid == resource || request.has_copy_handle || request.has_buffer_descriptors ||
        request.raw_size != (resource ? 24u : 28u) || size < HANDLE_REPLY_BYTES ||
        (!resource && hzn_cmif_read64(request.raw + 16) != state->aruid))
        return svc_result(INVALID_COMBINATION, command);
    uint32_t handle = 0;
    if (resource && config && config->copy_hid_memory)
        handle = config->copy_hid_memory(config->opaque, HZN_HID_MEMORY_BYTES);
    else if (!resource && config && config->open_applet_resource)
        handle = config->open_applet_resource(config->opaque, state->aruid);
    const uint32_t result = handle ? 0 : OUT_OF_RESOURCE;
    const bool replied = resource ? hzn_cmif_reply_copy_handle(tls, size, false, result, handle)
                                  : hzn_cmif_reply_move_handle(tls, size, false, result, handle);
    return svc_result(replied ? 0 : INVALID_COMBINATION, command);
}

/* Pinned runtime hid_shm.h and SDK 20 reader. All offsets are byte offsets. */
enum {
    RING_ENTRIES = 17, RING_MAX_COUNT = 16, RING_HEADER = 0x20,
    RING_BUFFER_COUNT = 0x08, RING_INDEX = 0x10, RING_COUNT = 0x18,
    TOUCH_RING = 0x400, TOUCH_STATE_BYTES = 0x290,
    NPADS = 0x9a00, NPAD_COUNT = 10, NPAD_BYTES = 0x5000, NPAD_CONDITION = 0x3e200,
    FULLKEY_RING = 0x28, SYSTEM_EXT_RING = 0x1408,
    NPAD_STYLE = 0, NPAD_DEVICE_TYPE = 0x4188, NPAD_PROPERTIES = 0x4190,
    NPAD_CONNECTED = 1,
};

typedef struct {
    uint64_t sampling, buttons;
    int32_t lx, ly, rx, ry;
    uint32_t attributes, reserved;
} CommonState;

_Static_assert(sizeof(CommonState) == 0x28, "NpadCommonState ABI");
_Static_assert(RING_HEADER + RING_ENTRIES * (8 + sizeof(CommonState)) == 0x350, "common ring ABI");
_Static_assert(NPADS + NPAD_COUNT * NPAD_BYTES == 0x3ba00, "Npad block ABI");

static const unsigned common_rings[] = {FULLKEY_RING, 0x378, 0x6c8, 0xa18, 0xd68, 0x10b8, SYSTEM_EXT_RING};
static const unsigned motion_rings[] = {0x1758, 0x1e60, 0x2568, 0x2c70, 0x3378, 0x3a80};

static void put32(uint8_t *p, uint32_t value)
{
    atomic_store_explicit((_Atomic uint32_t *)p, value, memory_order_release);
}

static void put64(uint8_t *p, uint64_t value, memory_order order)
{
    atomic_store_explicit((_Atomic uint64_t *)p, value, order);
}

static uint64_t get64(const uint8_t *p)
{
    return atomic_load_explicit((const _Atomic uint64_t *)p, memory_order_relaxed);
}

bool hzn_hid_memory_init(void *memory, size_t bytes)
{
    if (!memory || ((uintptr_t)memory & 7u) || bytes != HZN_HID_MEMORY_BYTES)
        return false;
    uint8_t *base = memory;
    memset(base, 0, bytes);
    const unsigned device_rings[] = {0, TOUCH_RING, 0x3400, 0x3800};
    for (size_t i = 0; i < sizeof device_rings / sizeof device_rings[0]; ++i)
        put64(base + device_rings[i] + RING_BUFFER_COUNT, RING_ENTRIES, memory_order_relaxed);
    for (unsigned i = 0; i < NPAD_COUNT; ++i) {
        uint8_t *pad = base + NPADS + i * NPAD_BYTES;
        for (size_t j = 0; j < sizeof common_rings / sizeof common_rings[0]; ++j)
            put64(pad + common_rings[j] + RING_BUFFER_COUNT, RING_ENTRIES, memory_order_relaxed);
        for (size_t j = 0; j < sizeof motion_rings / sizeof motion_rings[0]; ++j)
            put64(pad + motion_rings[j] + RING_BUFFER_COUNT, RING_ENTRIES, memory_order_relaxed);
    }
    const uint32_t condition[] = {0, 1, 1, 1};
    memcpy(base + NPAD_CONDITION, condition, sizeof condition);
    return true;
}

static void publish_ring(uint8_t *ring, const void *state, size_t bytes)
{
    const size_t stride = sizeof(uint64_t) + bytes;
    const uint64_t index = get64(ring + RING_INDEX);
    const uint64_t seq = get64(ring + RING_HEADER + index * stride) + 2;
    const uint64_t next = (index + 1) % RING_ENTRIES;
    uint8_t *entry = ring + RING_HEADER + next * stride;
    put64(entry, seq - 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    put64(entry + 8, seq / 2, memory_order_relaxed);
    const uint8_t *input = state;
    for (size_t i = 8; i < bytes; i += 8) {
        uint64_t word = 0;
        if (input)
            memcpy(&word, input + i, sizeof word);
        put64(entry + 8 + i, word, memory_order_relaxed);
    }
    put64(entry, seq, memory_order_release);
    put64(ring + RING_INDEX, next, memory_order_release);
    const uint64_t count = get64(ring + RING_COUNT);
    if (count < RING_MAX_COUNT)
        put64(ring + RING_COUNT, count + 1, memory_order_release);
}

static uint64_t stick_buttons(int32_t x, int32_t y, unsigned first)
{
    const int32_t threshold = 32767 / 2;
    return ((uint64_t)(x < -threshold) << first) | ((uint64_t)(y > threshold) << (first + 1)) |
           ((uint64_t)(x > threshold) << (first + 2)) | ((uint64_t)(y < -threshold) << (first + 3));
}

void hzn_hid_publish(void *memory, const HznHidState *hid, const HznHidPad *pad)
{
    uint8_t *base = memory;
    if (!base || !hid)
        return;
    if (hid->touch_active)
        publish_ring(base + TOUCH_RING, NULL, TOUCH_STATE_BYTES);

    uint8_t *entry = base + NPADS;
    const bool fullkey_enabled = hid->npad_active && (hid->styles & HZN_HID_STYLE_FULLKEY) && (hid->ids & 1u);
    if (!fullkey_enabled && !atomic_load_explicit((_Atomic uint32_t *)(entry + NPAD_STYLE), memory_order_relaxed))
        return;
    const bool connected = fullkey_enabled && pad && pad->connected;
    CommonState state = {0};
    if (connected) {
        state = (CommonState){
            .buttons = pad->buttons | stick_buttons(pad->lx, pad->ly, 16) | stick_buttons(pad->rx, pad->ry, 20),
            .lx = pad->lx, .ly = pad->ly, .rx = pad->rx, .ry = pad->ry, .attributes = NPAD_CONNECTED,
        };
    } else {
        put32(entry + NPAD_STYLE, 0);
    }
    publish_ring(entry + FULLKEY_RING, &state, sizeof state);
    publish_ring(entry + SYSTEM_EXT_RING, &state, sizeof state);
    put32(entry + NPAD_DEVICE_TYPE, connected ? 1 : 0);
    /* FullKey presents ABXY, Plus and Minus. The PS4 pad API supplies no
     * Switch battery level, so leave its battery fields unreported. */
    put64(entry + NPAD_PROPERTIES, connected ? (UINT64_C(1) << 11) | (UINT64_C(1) << 13) |
                                                 (UINT64_C(1) << 14) : 0, memory_order_release);
    if (connected)
        put32(entry + NPAD_STYLE, HZN_HID_STYLE_FULLKEY);
}


