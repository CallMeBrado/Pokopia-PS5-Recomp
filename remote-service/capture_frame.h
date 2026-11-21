/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS4_REMOTE_CAPTURE_FRAME_H
#define PS4_REMOTE_CAPTURE_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    PS4_REMOTE_CAPTURE_WIDTH = 960,
    PS4_REMOTE_CAPTURE_HEIGHT = 540,
    PS4_REMOTE_COMPOSITOR_BYTES = 1920 * 1088 * 4,
};

typedef struct Ps4RemoteCaptureFrame Ps4RemoteCaptureFrame;
typedef uint64_t (*Ps4RemotePixelOffset)(uint32_t x, uint32_t y);

/* Prepare the sampling addresses for a base PS4 compositor buffer. */
Ps4RemoteCaptureFrame *ps4_remote_capture_frame_create(Ps4RemotePixelOffset pixel_offset, int *error);
void ps4_remote_capture_frame_destroy(Ps4RemoteCaptureFrame *frame);

/* Copy from a validated, readable compositor buffer before encoding. */
bool ps4_remote_capture_frame_copy(Ps4RemoteCaptureFrame *frame, const void *tiled, size_t tiled_bytes);

/* Encode the copied pixels into a private destination. A failure sets written to zero. */
bool ps4_remote_capture_jpeg(const Ps4RemoteCaptureFrame *frame, void *jpeg,
                             size_t jpeg_capacity, size_t *written);

#endif
