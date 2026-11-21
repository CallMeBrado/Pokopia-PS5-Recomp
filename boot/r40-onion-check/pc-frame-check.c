/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _CRT_SECURE_NO_WARNINGS
#include "pc-frame-check.h"
#include "../boot.h"

/* Reuse the runtime's synchronized image readback. The test reads into its
 * own tightly packed buffer and never writes into the guest texture pool. */
#include "../../gfx-bridge/live_texture_upload.c"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static bool new_draw;

void pokopia_pc_frame_draw(const PokopiaLiveDraw *draw, uint64_t serial, FILE *log)
{
    enum { HASH_SIZE = 17, MAX_PROGRAMS = 256 };
    static struct { char vertex[HASH_SIZE], fragment[HASH_SIZE]; } programs[MAX_PROGRAMS];
    static size_t seen;
    const char *vertex = draw->vertex_shader.hash;
    const char *fragment = draw->fragment_shader.hash;
    for (size_t i = 0; i < seen; ++i) {
        if (!strcmp(programs[i].vertex, vertex) && !strcmp(programs[i].fragment, fragment))
            return;
    }
    if (seen == MAX_PROGRAMS)
        return;
    snprintf(programs[seen].vertex, HASH_SIZE, "%s", vertex);
    snprintf(programs[seen].fragment, HASH_SIZE, "%s", fragment);
    ++seen;
    new_draw = true;
    fprintf(log, "PC frame check first draw vs=%s fs=%s serial=%" PRIu64 " extent=%ux%u\n",
            vertex, fragment, serial, draw->extent.width, draw->extent.height);
    fflush(log);
}

static bool save_rgb(const char *path, uint8_t *pixels, VkExtent2D extent, bool bgra)
{
    const size_t count = (size_t)extent.width * extent.height;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t red = pixels[4 * i + (bgra ? 2 : 0)];
        const uint8_t green = pixels[4 * i + 1];
        const uint8_t blue = pixels[4 * i + (bgra ? 0 : 2)];
        pixels[3 * i] = red;
        pixels[3 * i + 1] = green;
        pixels[3 * i + 2] = blue;
    }
    FILE *out = fopen(path, "wb");
    if (!out)
        return false;
    const bool wrote = fprintf(out, "P6\n%u %u\n255\n", extent.width, extent.height) > 0 &&
        fwrite(pixels, 3, count, out) == count;
    const bool closed = fclose(out) == 0;
    if (!wrote || !closed)
        remove(path);
    return wrote && closed;
}

static VkResult read_present(PokopiaVkAdapter *adapter, const PokopiaVkTexture *texture,
                             const Storage *storage, Region full, uint8_t *pixels)
{
    /* The desktop may expose uncached coherent memory first. Prefer a cached
     * readback for large screenshots without changing the game's allocator. */
    if (adapter->guest_commands) {
        PokopiaVkAdapter cached = *adapter;
        for (uint32_t i = 0; i < cached.memory.memoryTypeCount; ++i) {
            VkMemoryPropertyFlags *flags = &cached.memory.memoryTypes[i].propertyFlags;
            if (!(*flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
                *flags &= ~VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        }
        const VkResult result = read_image(&cached, texture, storage, full, pixels);
        if (result != VK_ERROR_FEATURE_NOT_PRESENT)
            return result;
    }
    return read_image(adapter, texture, storage, full, pixels);
}

void pokopia_pc_frame_check(PokopiaVkAdapter *adapter, const PokopiaVkTexture *texture, FILE *log)
{
    static unsigned budget_frames;
    if (++budget_frames % 32 == 0) {
        pokopia_boot_report_heap(log, "physical-budget");
        pokopia_vk_ledger_report(log, "physical-budget");
        fflush(log);
    }

    static bool done, reported_uniform, initial_saved;
    static uint64_t presented, checked, interval, first_broad;
    static double total_ms;
    const char *path = getenv("POKOPIA_PC_FRAME_PPM");
    const char *broad_path = getenv("POKOPIA_PC_BROAD_PPM");
    const char *scene_path = getenv("POKOPIA_PC_SCENE_PPM");
    if (done || !path || !*path)
        return;
    if (!interval) {
        const char *option = getenv("POKOPIA_PC_FRAME_INTERVAL");
        const uint64_t configured = option ? strtoull(option, NULL, 10) : 0;
        interval = configured && configured <= 120 ? configured : 1;
    }
    ++presented;
    const uint64_t sample_interval = first_broad ? 120 : interval;
    if (presented > 8 && presented % sample_interval && !new_draw)
        return;
    new_draw = false;

    const VkFormat format = texture->format;
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    const VkExtent2D extent = texture->extent;
    Storage storage = {.layers = 1};
    if ((!bgra && format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB) ||
        nvn_texture_upload_footprint(format, extent, 0, &storage.footprint) != VK_SUCCESS) {
        fprintf(log, "PC frame check unsupported format=%d extent=%ux%u\n", format, extent.width, extent.height);
        fflush(log);
        done = true;
        return;
    }
    storage.pitch = storage.footprint.row_bytes;
    storage.layer_stride = storage.footprint.packed_bytes;
    uint8_t *pixels = malloc(storage.layer_stride);
    const Region full = {.width = extent.width, .height = extent.height};
    LARGE_INTEGER start, end, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    VkResult result = pixels ? read_present(adapter, texture, &storage, full, pixels) : VK_ERROR_OUT_OF_HOST_MEMORY;
    if (result != VK_SUCCESS) {
        fprintf(log, "PC frame check readback failed serial=%" PRIu64 " result=%d\n", texture->render_serial, result);
        fflush(log);
        free(pixels);
        done = true;
        return;
    }

    uint64_t nonblack = 0, varied = 0;
    uint32_t left = extent.width, top = extent.height, right = 0, bottom = 0;
    uint32_t varied_left = extent.width, varied_top = extent.height, varied_right = 0, varied_bottom = 0;
    const uint8_t *pixel = pixels;
    for (uint32_t y = 0; y < extent.height; ++y) {
        for (uint32_t x = 0; x < extent.width; ++x, pixel += 4) {
            if (pixel[0] != pixels[0] || pixel[1] != pixels[1] || pixel[2] != pixels[2]) {
                ++varied;
                if (x < varied_left) varied_left = x;
                if (x > varied_right) varied_right = x;
                if (y < varied_top) varied_top = y;
                if (y > varied_bottom) varied_bottom = y;
            }
            if ((pixel[0] | pixel[1] | pixel[2]) == 0)
                continue;
            ++nonblack;
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    QueryPerformanceCounter(&end);
    total_ms += (end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
    ++checked;
    const uint64_t area = (uint64_t)extent.width * extent.height;
    const bool broad = varied && varied >= area / 25 &&
        varied_right - varied_left >= extent.width / 3 && varied_bottom - varied_top >= extent.height / 5;
    const bool later_scene = first_broad && presented - first_broad >= 300 && broad && varied >= area / 4;
    if (checked <= 8 || checked % 60 == 0 || (varied && !initial_saved) || (broad && !first_broad) ||
        later_scene || (nonblack && !reported_uniform)) {
        fprintf(log, "PC frame check serial=%" PRIu64 " checked=%" PRIu64 " presented=%" PRIu64
                " %ux%u nonblack=%" PRIu64 " spatial=%u average_ms=%.2f",
                texture->render_serial, checked, presented, extent.width, extent.height, nonblack,
                (unsigned)(varied != 0), total_ms / checked);
        if (nonblack)
            fprintf(log, " bbox=%u,%u-%u,%u", left, top, right, bottom);
        fputc('\n', log);
        fflush(log);
    }
    reported_uniform |= nonblack != 0;
    const char *capture = NULL;
    const char *phase = NULL;
    if (varied && !initial_saved) {
        capture = path;
        phase = "first-visible";
        initial_saved = true;
        done = !broad_path || !*broad_path;
    } else if (broad && !first_broad && broad_path && *broad_path) {
        capture = broad_path;
        phase = "first-broad";
        first_broad = presented;
        done = !scene_path || !*scene_path;
    } else if (later_scene && scene_path && *scene_path) {
        capture = scene_path;
        phase = "later-scene";
        done = true;
    }
    if (capture) {
        fprintf(log, "PC frame check phase=%s varied_pixels=%" PRIu64 " broad=%u\n", phase, varied, (unsigned)broad);
#ifdef POKOPIA_SPARSE_HEAP
        pokopia_boot_report_heap(log, phase);
#endif
#ifdef POKOPIA_VK_MEMORY_TRACE
        pokopia_vk_ledger_report(log, phase);
#endif
        if (save_rgb(capture, pixels, extent, bgra))
            fprintf(log, "PC frame check saved serial=%" PRIu64 " to %s\n", texture->render_serial, capture);
        else
            fprintf(log, "PC frame check could not save %s\n", capture);
        fflush(log);
    }
    free(pixels);
}
