/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vi_adapter.h"

#include <string.h>

/* Export offsets in the exact 655360 SDK. vi2 uses the original console
 * GraphicsSupervisor session, which host graphics initialization replaces. */
enum {
    VI_INITIALIZE = 0x55e260,
    VI_FINALIZE = 0x55e2d0,
    VI_OPEN_DEFAULT_DISPLAY = 0x55e430,
    VI_CLOSE_DISPLAY = 0x55e590,
    VI_CREATE_LAYER = 0x55e5d0,
    VI_DESTROY_LAYER = 0x55e830,
    VI_GET_NATIVE_WINDOW = 0x55e870,
    VI_SET_DISPLAY_VRR = 0x55eb70,
    VI_SET_EXTENDED_LUMINANCE = 0x55ece0,
    VI_SET_STANDARD_LUMINANCE = 0x55ecf0,
    STREAMPLAY_CREATE_INDIVIDUAL_DISPLAYS = 0x6af060,
    STREAMPLAY_GET_NATIVE_WINDOW = 0x6af0c0,
    VI_OPERATION_FAILED = 0x272,
    VI_INVALID_ARGUMENT = 0xc72,
    VI_LAYER_IDENTITY = 0x100,
    VI_WINDOW_IDENTITY = 0x200,
};

static const struct {
    uint32_t offset;
    const char *name;
} calls[] = {
    {VI_INITIALIZE, "nn::vi::Initialize"},
    {VI_FINALIZE, "nn::vi::Finalize"},
    {VI_OPEN_DEFAULT_DISPLAY, "nn::vi::OpenDefaultDisplay"},
    {VI_CLOSE_DISPLAY, "nn::vi::CloseDisplay"},
    {VI_CREATE_LAYER, "nn::vi::CreateLayer"},
    {VI_DESTROY_LAYER, "nn::vi::DestroyLayer"},
    {VI_GET_NATIVE_WINDOW, "nn::vi::GetNativeWindow"},
    {VI_SET_DISPLAY_VRR, "nn::vi::SetDisplayVrrCapability"},
    {VI_SET_EXTENDED_LUMINANCE, "nn::vi::SetLayerNextHdrExtendedContentLuminanceRangeMetadata"},
    {VI_SET_STANDARD_LUMINANCE, "nn::vi::SetLayerNextHdrStandardContentLuminanceRangeMetadata"},
    {STREAMPLAY_CREATE_INDIVIDUAL_DISPLAYS, "nn::streamplay::CreateIndividualDisplays"},
    {STREAMPLAY_GET_NATIVE_WINDOW, "nn::streamplay::GetNativeWindowHandle"},
};

bool vi_adapter_init(ViAdapter *adapter, const ViAdapterConfig *config)
{
    if (!adapter || adapter->config.sdk_base || !config || !config->sdk_base ||
        config->sdk_base > UINT64_MAX - STREAMPLAY_GET_NATIVE_WINDOW ||
        !config->guest_object_base || (config->guest_object_base & 7) ||
        config->guest_object_base > UINT64_MAX - VI_WINDOW_IDENTITY ||
        !config->graphics_ready || !config->query_output || !config->write_guest)
        return false;
    *adapter = (ViAdapter){.config = *config};
    return true;
}

static bool query_output(const ViAdapter *adapter, ViAdapterOutput *output)
{
    ViAdapterOutput current = {0};
    if (!adapter->initialize_count || !adapter->config.graphics_ready(adapter->config.opaque) ||
        !adapter->config.query_output(adapter->config.opaque, &current) || !current.width || !current.height)
        return false;
    *output = current;
    return true;
}

bool vi_adapter_get_output(const ViAdapter *adapter, uint64_t window, ViAdapterOutput *output)
{
    return adapter && output && adapter->layer_open &&
           window == adapter->config.guest_object_base + VI_WINDOW_IDENTITY && query_output(adapter, output);
}

static void destroy_layer(ViAdapter *adapter)
{
    adapter->layer_open = false;
    adapter->extended_luminance = (ViAdapterLuminance){0};
    adapter->standard_luminance = (ViAdapterLuminance){0};
}

static ViAdapterReplyKind write_identity(const ViAdapter *adapter, uint64_t address, uint64_t value)
{
    if (!address || (address & 7))
        return VI_ADAPTER_INVALID_GUEST;
    void *out = adapter->config.write_guest(adapter->config.opaque, address, sizeof value);
    if (!out)
        return VI_ADAPTER_INVALID_GUEST;
    memcpy(out, &value, sizeof value);
    return VI_ADAPTER_HANDLED;
}

static ViAdapterReplyKind handle_call(ViAdapter *adapter, uint32_t offset, uint64_t x[32])
{
    const uint64_t display = adapter->config.guest_object_base;
    const uint64_t layer = display + VI_LAYER_IDENTITY;
    const uint64_t window = display + VI_WINDOW_IDENTITY;
    ViAdapterOutput output;
    ViAdapterReplyKind reply;

    switch (offset) {
    case VI_INITIALIZE:
        if (adapter->initialize_count == UINT64_MAX || !adapter->config.graphics_ready(adapter->config.opaque))
            return VI_ADAPTER_BOUNDARY;
        ++adapter->initialize_count;
        return VI_ADAPTER_HANDLED;
    case VI_FINALIZE:
        if (adapter->initialize_count && !--adapter->initialize_count) {
            destroy_layer(adapter);
            adapter->display_count = 0;
            adapter->vrr_requested = false;
        }
        return VI_ADAPTER_HANDLED;
    case VI_OPEN_DEFAULT_DISPLAY:
        if (adapter->display_count == UINT64_MAX || !query_output(adapter, &output))
            return VI_ADAPTER_BOUNDARY;
        reply = write_identity(adapter, x[0], display);
        if (reply == VI_ADAPTER_HANDLED) {
            ++adapter->display_count;
            x[0] = 0;
        }
        return reply;
    case VI_CLOSE_DISPLAY:
        if (!adapter->display_count || x[0] != display)
            return VI_ADAPTER_INVALID_GUEST;
        if (!--adapter->display_count)
            adapter->vrr_requested = false;
        return VI_ADAPTER_HANDLED;
    case VI_CREATE_LAYER: {
        uint32_t result = 0;
        if (adapter->layer_open)
            result = VI_OPERATION_FAILED;
        else if (!adapter->display_count || x[1] != display)
            result = VI_INVALID_ARGUMENT;
        else if (!query_output(adapter, &output))
            return VI_ADAPTER_BOUNDARY;
        reply = write_identity(adapter, x[0], result ? 0 : layer);
        if (reply == VI_ADAPTER_HANDLED) {
            if (!result)
                adapter->layer_open = true;
            x[0] = result;
        }
        return reply;
    }
    case VI_DESTROY_LAYER:
        if (!adapter->layer_open || x[0] != layer)
            return VI_ADAPTER_INVALID_GUEST;
        destroy_layer(adapter);
        return VI_ADAPTER_HANDLED;
    case VI_GET_NATIVE_WINDOW:
        if (!adapter->layer_open || x[1] != layer)
            return VI_ADAPTER_INVALID_GUEST;
        if (!query_output(adapter, &output))
            return VI_ADAPTER_BOUNDARY;
        reply = write_identity(adapter, x[0], window);
        if (reply == VI_ADAPTER_HANDLED)
            x[0] = 0;
        return reply;
    case STREAMPLAY_CREATE_INDIVIDUAL_DISPLAYS:
    case STREAMPLAY_GET_NATIVE_WINDOW:
        /* This host exposes one local output and no remote GameShare displays.
         * Resolve the SDK's streamplay display calls to that existing window. */
        if (!vi_adapter_get_output(adapter, window, &output))
            return VI_ADAPTER_BOUNDARY;
        x[0] = offset == STREAMPLAY_GET_NATIVE_WINDOW ? window : 0;
        return VI_ADAPTER_HANDLED;
    case VI_SET_DISPLAY_VRR:
        /* This void SDK wrapper ignores a foreign display identity. The
         * request is distinct from the target's actual VRR capability. */
        if (x[0] == display && adapter->display_count)
            adapter->vrr_requested = (x[1] & 1) != 0;
        return VI_ADAPTER_HANDLED;
    case VI_SET_EXTENDED_LUMINANCE:
    case VI_SET_STANDARD_LUMINANCE: {
        if (!adapter->layer_open || x[0] != layer)
            return VI_ADAPTER_INVALID_GUEST;
        ViAdapterLuminance *setting = offset == VI_SET_EXTENDED_LUMINANCE ?
                                      &adapter->extended_luminance : &adapter->standard_luminance;
        *setting = (ViAdapterLuminance){.minimum = (uint16_t)x[1], .maximum = (uint16_t)x[2], .set = true};
        x[0] = 0;
        return VI_ADAPTER_HANDLED;
    }
    default:
        return VI_ADAPTER_NOT_OURS;
    }
}

bool vi_adapter_may_own(const ViAdapter *adapter, uint64_t pc)
{
    if (!adapter || !adapter->config.sdk_base || pc < adapter->config.sdk_base)
        return false;
    const uint64_t offset = pc - adapter->config.sdk_base;
    return offset >= VI_INITIALIZE && offset <= STREAMPLAY_GET_NATIVE_WINDOW;
}

bool vi_adapter_is_call(const ViAdapter *adapter, uint64_t pc)
{
    if (!vi_adapter_may_own(adapter, pc))
        return false;
    const uint64_t offset = pc - adapter->config.sdk_base;
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; ++i)
        if (offset == calls[i].offset)
            return true;
    return false;
}

ViAdapterReply vi_adapter_dispatch(ViAdapter *adapter, uint64_t pc, uint64_t x[32])
{
    if (!adapter || !adapter->config.sdk_base || !x || pc < adapter->config.sdk_base)
        return (ViAdapterReply){VI_ADAPTER_NOT_OURS, NULL};
    const uint64_t offset = pc - adapter->config.sdk_base;
    if (offset < VI_INITIALIZE || offset > STREAMPLAY_GET_NATIVE_WINDOW)
        return (ViAdapterReply){VI_ADAPTER_NOT_OURS, NULL};
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; ++i) {
        if (offset == calls[i].offset)
            return (ViAdapterReply){handle_call(adapter, calls[i].offset, x), calls[i].name};
    }
    return (ViAdapterReply){VI_ADAPTER_NOT_OURS, NULL};
}
