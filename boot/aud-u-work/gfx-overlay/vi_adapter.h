/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_VI_ADAPTER_H
#define POKOPIA_VI_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t width, height;
    bool vrr_supported;
} ViAdapterOutput;

typedef struct {
    uint64_t sdk_base;
    /* Reserve distinct opaque identities at this address and +0x100/+0x200. */
    uint64_t guest_object_base;
    void *opaque;
    bool (*graphics_ready)(void *opaque);
    bool (*query_output)(void *opaque, ViAdapterOutput *output);
    void *(*write_guest)(void *opaque, uint64_t address, size_t size);
} ViAdapterConfig;

typedef struct {
    uint16_t minimum, maximum;
    bool set;
} ViAdapterLuminance;

typedef struct {
    ViAdapterConfig config;
    uint64_t initialize_count, display_count;
    bool layer_open, vrr_requested;
    ViAdapterLuminance extended_luminance, standard_luminance;
} ViAdapter;

typedef enum {
    VI_ADAPTER_NOT_OURS,
    VI_ADAPTER_HANDLED,
    VI_ADAPTER_BOUNDARY,
    VI_ADAPTER_INVALID_GUEST,
} ViAdapterReplyKind;

typedef struct {
    ViAdapterReplyKind kind;
    const char *name;
} ViAdapterReply;

/* Start with a zeroed object. The caller serializes dispatch with backend
 * and guest-memory changes. The backend owns the presentation target. */
bool vi_adapter_init(ViAdapter *adapter, const ViAdapterConfig *config);
ViAdapterReply vi_adapter_dispatch(ViAdapter *adapter, uint64_t pc, uint64_t x[32]);

/* Resolve the guest-native window identity to the live presentation target. */
bool vi_adapter_get_output(const ViAdapter *adapter, uint64_t window, ViAdapterOutput *output);

#endif
