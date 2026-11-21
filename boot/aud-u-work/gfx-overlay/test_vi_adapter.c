/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vi_adapter.h"

#include <assert.h>
#include <stdio.h>

static const uint64_t sdk = UINT64_C(0x7500000000);
static const uint64_t object_base = UINT64_C(0x7e03010000);
static const uint64_t guest_out = UINT64_C(0x7c00010400);

typedef struct {
    uint64_t output[3];
    bool gpu_live, output_live;
    ViAdapterOutput extent;
} TestBackend;

static bool graphics_ready(void *opaque)
{
    return ((TestBackend *)opaque)->gpu_live;
}

static bool query_output(void *opaque, ViAdapterOutput *out)
{
    const TestBackend *backend = opaque;
    if (!backend->output_live)
        return false;
    *out = backend->extent;
    return true;
}

static void *write_guest(void *opaque, uint64_t address, size_t size)
{
    TestBackend *backend = opaque;
    if (size != sizeof(uint64_t) || address < guest_out || address - guest_out >= sizeof backend->output)
        return NULL;
    return &backend->output[(address - guest_out) / sizeof(uint64_t)];
}

int main(void)
{
    TestBackend backend = {.extent = {.width = 1920, .height = 1080}};
    ViAdapter adapter = {0};
    const ViAdapterConfig config = {
        .sdk_base = sdk, .guest_object_base = object_base, .opaque = &backend,
        .graphics_ready = graphics_ready, .query_output = query_output, .write_guest = write_guest,
    };
    assert(vi_adapter_init(&adapter, &config));
    uint64_t x[32] = {0};
    assert(vi_adapter_dispatch(&adapter, sdk + 0x151fa8, x).kind == VI_ADAPTER_NOT_OURS);
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e260, x).kind == VI_ADAPTER_BOUNDARY);
    assert(vi_adapter_dispatch(&adapter, sdk + 0x6af060, x).kind == VI_ADAPTER_BOUNDARY);
    assert(!adapter.initialize_count);
    backend.gpu_live = true;
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e260, x).kind == VI_ADAPTER_HANDLED);
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e260, x).kind == VI_ADAPTER_HANDLED);
    x[0] = guest_out;
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e430, x).kind == VI_ADAPTER_BOUNDARY);
    assert(!backend.output[0] && !adapter.display_count);

    backend.output_live = true;
    x[0] = guest_out + 1;
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e430, x).kind == VI_ADAPTER_INVALID_GUEST);
    assert(!adapter.display_count);
    x[0] = guest_out;
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e430, x).kind == VI_ADAPTER_HANDLED && x[0] == 0);
    assert(backend.output[0] == object_base);
    x[0] = guest_out + 8;
    x[1] = object_base + 0x100;
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e5d0, x).kind == VI_ADAPTER_HANDLED && x[0] == 0xc72);
    assert(!backend.output[1]);
    x[0] = guest_out + 8;
    x[1] = object_base;
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e5d0, x).kind == VI_ADAPTER_HANDLED && x[0] == 0);
    assert(backend.output[1] == object_base + 0x100);
    x[0] = guest_out + 16;
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e5d0, x).kind == VI_ADAPTER_HANDLED && x[0] == 0x272);
    assert(!backend.output[2]);

    x[0] = guest_out + 16;
    x[1] = backend.output[1];
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e870, x).kind == VI_ADAPTER_HANDLED && x[0] == 0);
    uint64_t window = backend.output[2];
    assert(window == object_base + 0x200);
    x[0] = 1;
    assert(vi_adapter_dispatch(&adapter, sdk + 0x6af060, x).kind == VI_ADAPTER_HANDLED && x[0] == 0);
    assert(vi_adapter_dispatch(&adapter, sdk + 0x6af0c0, x).kind == VI_ADAPTER_HANDLED && x[0] == window);
    x[0] = object_base;
    x[1] = 1;
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55eb70, x).kind == VI_ADAPTER_HANDLED);
    assert(adapter.vrr_requested);
    ViAdapterOutput output;
    assert(vi_adapter_get_output(&adapter, window, &output));
    assert(output.width == 1920 && output.height == 1080 && !output.vrr_supported);
    x[0] = backend.output[1];
    x[1] = 0;
    x[2] = 400;
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55ece0, x).kind == VI_ADAPTER_HANDLED);
    assert(adapter.extended_luminance.set && adapter.extended_luminance.maximum == 400);
    x[0] = backend.output[1];
    x[2] = 300;
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55ecf0, x).kind == VI_ADAPTER_HANDLED);
    assert(adapter.standard_luminance.set && adapter.standard_luminance.maximum == 300);

    backend.output_live = false;
    assert(!vi_adapter_get_output(&adapter, window, &output));
    assert(vi_adapter_dispatch(&adapter, sdk + 0x6af0c0, x).kind == VI_ADAPTER_BOUNDARY);
    backend.output_live = true;
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e2d0, x).kind == VI_ADAPTER_HANDLED);
    assert(vi_adapter_get_output(&adapter, window, &output));
    x[0] = backend.output[1];
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e830, x).kind == VI_ADAPTER_HANDLED);
    assert(!vi_adapter_get_output(&adapter, window, &output));
    assert(!adapter.extended_luminance.set && !adapter.standard_luminance.set);
    x[0] = backend.output[0];
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e590, x).kind == VI_ADAPTER_HANDLED);
    assert(vi_adapter_dispatch(&adapter, sdk + 0x55e2d0, x).kind == VI_ADAPTER_HANDLED);
    assert(!adapter.initialize_count && !adapter.display_count && !adapter.vrr_requested);
    puts("VI lifecycle and backend output vectors passed.");
    return 0;
}
