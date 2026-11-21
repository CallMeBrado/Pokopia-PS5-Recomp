/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nvn_window.h"

#include <stdio.h>
#include <string.h>

enum { BUILDER = 0x100, ARRAY = 0x200, WINDOW = 0x300, DEVICE = 0x1000, NATIVE = 0x2000 };

typedef struct {
    uint8_t guest[0x600];
    bool display_live;
    NvnFrontTextureInfo textures[3];
} Fixture;

static void *write_guest(void *opaque, uint64_t address, size_t size)
{
    Fixture *fixture = opaque;
    if (!address || address > sizeof fixture->guest || size > sizeof fixture->guest - address)
        return NULL;
    return fixture->guest + address;
}

static const void *read_guest(void *opaque, uint64_t address, size_t size)
{
    return write_guest(opaque, address, size);
}

static bool get_output(void *opaque, uint64_t address, ViAdapterOutput *out)
{
    Fixture *fixture = opaque;
    if (address != NATIVE || !fixture->display_live)
        return false;
    *out = (ViAdapterOutput){.width = 1920, .height = 1080};
    return true;
}

static bool get_texture(void *opaque, uint64_t address, NvnFrontTextureInfo *out)
{
    Fixture *fixture = opaque;
    if (address < 0x3000 || address >= 0x3300 || (address & 0xff))
        return false;
    *out = fixture->textures[(address - 0x3000) / 0x100];
    return true;
}

static NvnFrontReplyKind invoke(NvnWindowBridge *bridge, const char *name, uint64_t object,
    uint64_t arg, uint64_t arg2)
{
    uint64_t x[32] = {object, arg, arg2};
    return nvn_window_dispatch(bridge, name, x);
}

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "NVN window test failed on line %d\n", __LINE__); return 1; } } while (0)

int main(void)
{
    Fixture fixture = {.display_live = true};
    for (size_t i = 0; i < 3; ++i) {
        fixture.textures[i].create_info = (NvnFrontTextureCreateInfo){
            .config = {.device = DEVICE, .flags = 0xd, .width = 1920, .height = 1080,
                .target = 1, .depth = 1, .levels = 1, .format = 0x38},
            .storage_address = 0x7800000000 + i * 0x870000, .storage_size = 0x870000,
        };
        fixture.textures[i].native = &fixture.textures[i];
    }
    NvnWindowConfig config = {&fixture, read_guest, write_guest, get_output, get_texture};
    NvnWindowBridge *bridge = nvn_window_create(&config);
    CHECK(bridge);

    memset(fixture.guest + BUILDER, 0xff, 0x60);
    CHECK(invoke(bridge, "nvnWindowBuilderSetDevice", BUILDER, DEVICE, 0) == NVN_FRONT_HANDLED);
    CHECK(invoke(bridge, "nvnWindowBuilderSetDefaults", BUILDER, 0, 0) == NVN_FRONT_HANDLED);
    CHECK(fixture.guest[BUILDER + 7] == 0xff && fixture.guest[BUILDER + 9] == 0x10);
    CHECK(fixture.guest[BUILDER + 0x10] == 0 && fixture.guest[BUILDER + 0x30] == 0x0a);
    CHECK(invoke(bridge, "nvnWindowBuilderSetTextures", BUILDER, 3, ARRAY) == NVN_FRONT_HANDLED);
    CHECK(invoke(bridge, "nvnWindowBuilderSetNativeWindow", BUILDER, NATIVE, 0) == NVN_FRONT_HANDLED);
    CHECK(invoke(bridge, "nvnWindowBuilderSetPresentInterval", BUILDER, 0, 0) == NVN_FRONT_HANDLED);
    CHECK(invoke(bridge, "nvnWindowInitialize", WINDOW, BUILDER, 0) == NVN_FRONT_BOUNDARY);
    CHECK(invoke(bridge, "nvnWindowBuilderSetPresentInterval", BUILDER, 1, 0) == NVN_FRONT_HANDLED);

    uint64_t addresses[] = {0x3000, 0x3100, 0x3100};
    memcpy(fixture.guest + ARRAY, addresses, sizeof addresses);
    memset(fixture.guest + WINDOW, 0xab, 0x218);
    CHECK(invoke(bridge, "nvnWindowInitialize", WINDOW, BUILDER, 0) == NVN_FRONT_INVALID_GUEST);
    CHECK(fixture.guest[WINDOW] == 0xab);
    addresses[2] = 0x3200;
    memcpy(fixture.guest + ARRAY, addresses, sizeof addresses);
    fixture.display_live = false;
    CHECK(invoke(bridge, "nvnWindowInitialize", WINDOW, BUILDER, 0) == NVN_FRONT_BOUNDARY);
    fixture.display_live = true;
    fixture.textures[2].create_info.config.device = DEVICE + 8;
    CHECK(invoke(bridge, "nvnWindowInitialize", WINDOW, BUILDER, 0) == NVN_FRONT_BOUNDARY);
    fixture.textures[2].create_info.config.device = DEVICE;
    fixture.textures[2].create_info.storage_address--;
    CHECK(invoke(bridge, "nvnWindowInitialize", WINDOW, BUILDER, 0) == NVN_FRONT_INVALID_GUEST);
    fixture.textures[2].create_info.storage_address++;
    CHECK(invoke(bridge, "nvnWindowBuilderSetTextures", BUILDER, 3, 0x5f8) == NVN_FRONT_HANDLED);
    CHECK(invoke(bridge, "nvnWindowInitialize", WINDOW, BUILDER, 0) == NVN_FRONT_INVALID_GUEST);
    CHECK(invoke(bridge, "nvnWindowBuilderSetTextures", BUILDER, 3, ARRAY) == NVN_FRONT_HANDLED);
    CHECK(fixture.guest[WINDOW] == 0xab);

    uint64_t x[32] = {WINDOW, BUILDER};
    CHECK(nvn_window_dispatch(bridge, "nvnWindowInitialize", x) == NVN_FRONT_HANDLED && x[0] == 1);
    CHECK(fixture.guest[WINDOW + 0x20] == 3 && fixture.guest[WINDOW + 0x24] == 3);
    CHECK(fixture.guest[WINDOW + 0x28] == 1);
    memset(fixture.guest + ARRAY, 0, sizeof addresses);
    NvnWindowInfo info;
    CHECK(nvn_window_get(bridge, WINDOW, &info));
    CHECK(info.texture_count == 3 && info.active_texture_count == 3 && info.present_interval == 1);
    CHECK(info.device == DEVICE && info.native_window == NATIVE && info.format == 0x38);
    CHECK(info.width == 1920 && info.height == 1080 && memcmp(info.textures, addresses, sizeof addresses) == 0);
    CHECK(info.crop.x == 0 && info.crop.y == 0 && info.crop.width == 1920 && info.crop.height == 1080);
    uint64_t crop_x[32] = {WINDOW, 16, 8, 640, 480};
    CHECK(nvn_window_dispatch(bridge, "nvnWindowSetCrop", crop_x) == NVN_FRONT_HANDLED);
    CHECK(invoke(bridge, "nvnWindowGetCrop", WINDOW, ARRAY, 0) == NVN_FRONT_HANDLED);
    NvnWindowCrop crop;
    memcpy(&crop, fixture.guest + ARRAY, sizeof crop);
    CHECK(crop.x == 16 && crop.y == 8 && crop.width == 640 && crop.height == 480);
    crop_x[3] = 1920;
    CHECK(nvn_window_dispatch(bridge, "nvnWindowSetCrop", crop_x) == NVN_FRONT_INVALID_GUEST);
    CHECK(nvn_window_get(bridge, WINDOW, &info) && info.crop.width == 640);
    crop_x[4] = 0;
    CHECK(nvn_window_dispatch(bridge, "nvnWindowSetCrop", crop_x) == NVN_FRONT_HANDLED);
    CHECK(invoke(bridge, "nvnWindowGetCrop", WINDOW, 0x5f8, 0) == NVN_FRONT_INVALID_GUEST);
    CHECK(nvn_window_get(bridge, WINDOW, &info) && info.crop.x == 0 && info.crop.width == 1920);
    CHECK(invoke(bridge, "nvnWindowAcquireTexture", WINDOW, 0, 0) == NVN_FRONT_NOT_OURS);
    fixture.textures[2].native = NULL;
    CHECK(!nvn_window_get(bridge, WINDOW, &info));
    CHECK(invoke(bridge, "nvnWindowFinalize", WINDOW, 0, 0) == NVN_FRONT_HANDLED);
    CHECK(!nvn_window_get(bridge, WINDOW, &info));
    CHECK(fixture.guest[WINDOW + 0x20] == 0);
    CHECK(invoke(bridge, "nvnWindowFinalize", WINDOW, 0, 0) == NVN_FRONT_INVALID_GUEST);
    nvn_window_destroy(bridge);
    puts("NVN2 window bridge test passed");
    return 0;
}
