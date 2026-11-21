/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "capture_frame.h"
#include "capture_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include <stb/stb_image_write.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

enum { PIXELS = PS4_REMOTE_CAPTURE_WIDTH * PS4_REMOTE_CAPTURE_HEIGHT };

struct Ps4RemoteCaptureFrame {
    uint32_t offsets[PIXELS];
    uint8_t rgb[PIXELS * 3];
};

Ps4RemoteCaptureFrame *ps4_remote_capture_frame_create(Ps4RemotePixelOffset pixel_offset, int *error)
{
    if (!pixel_offset) {
        *error = EINVAL;
        return NULL;
    }
    Ps4RemoteCaptureFrame *frame = capture_allocate(sizeof *frame, error);
    if (!frame)
        return NULL;
    for (uint32_t y = 0; y < PS4_REMOTE_CAPTURE_HEIGHT; ++y) {
        for (uint32_t x = 0; x < PS4_REMOTE_CAPTURE_WIDTH; ++x) {
            const uint64_t offset = pixel_offset(2 * x + 1, 2 * y + 1);
            if ((offset & 3) || offset > PS4_REMOTE_COMPOSITOR_BYTES - sizeof(uint32_t)) {
                capture_release(frame, sizeof *frame);
                *error = ERANGE;
                return NULL;
            }
            frame->offsets[y * PS4_REMOTE_CAPTURE_WIDTH + x] = (uint32_t)offset;
        }
    }
    return frame;
}

void ps4_remote_capture_frame_destroy(Ps4RemoteCaptureFrame *frame)
{
    capture_release(frame, sizeof *frame);
}

typedef struct {
    uint8_t *next;
    size_t space;
    bool overflow;
} JpegOutput;

static void write_jpeg(void *opaque, void *bytes, int count)
{
    JpegOutput *output = opaque;
    if (output->overflow || count < 0 || (size_t)count > output->space) {
        output->overflow = true;
        return;
    }
    if (count == 1)
        *output->next = *(const uint8_t *)bytes;
    else
        memcpy(output->next, bytes, (size_t)count);
    output->next += count;
    output->space -= (size_t)count;
}

bool ps4_remote_capture_frame_copy(Ps4RemoteCaptureFrame *frame, const void *tiled, size_t tiled_bytes)
{
    if (!frame || !tiled || ((uintptr_t)tiled & (sizeof(uint32_t) - 1)) ||
        tiled_bytes != PS4_REMOTE_COMPOSITOR_BYTES)
        return false;
    const uint8_t *source = tiled;
    for (size_t i = 0; i < PIXELS; ++i) {
        /* Use one word load from video memory. Separate byte loads make the
           copy much slower on the compositor's write-combined mapping. */
        const uint32_t bgra = *(const volatile uint32_t *)(source + frame->offsets[i]);
        frame->rgb[3 * i] = (uint8_t)(bgra >> 16);
        frame->rgb[3 * i + 1] = (uint8_t)(bgra >> 8);
        frame->rgb[3 * i + 2] = (uint8_t)bgra;
    }
    return true;
}

bool ps4_remote_capture_jpeg(const Ps4RemoteCaptureFrame *frame, void *jpeg,
                             size_t jpeg_capacity, size_t *written)
{
    if (!written)
        return false;
    *written = 0;
    if (!frame || !jpeg || !jpeg_capacity)
        return false;
    JpegOutput output = {jpeg, jpeg_capacity, false};
    /* Quality 90 keeps chroma subsampling and the smaller encode cost. */
    const int success = stbi_write_jpg_to_func(write_jpeg, &output, PS4_REMOTE_CAPTURE_WIDTH,
                                                PS4_REMOTE_CAPTURE_HEIGHT, 3, frame->rgb, 90);
    if (!success || output.overflow)
        return false;
    *written = jpeg_capacity - output.space;
    return true;
}
