/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS4_REMOTE_CAPTURE_STREAM_H
#define PS4_REMOTE_CAPTURE_STREAM_H

#include "capture_frame.h"

void ps4_remote_capture_status(const char *stage, int result);

/* Start and stop on the shell worker. A snapshot callback lets submission
 * prefer copies that stay within one flip. It never performs network I/O. */
bool ps4_remote_stream_start(Ps4RemotePixelOffset pixel_offset);
void ps4_remote_stream_submit(const void *tiled, size_t tiled_bytes, uint64_t sequence,
                              uint64_t (*snapshot)(void));
void ps4_remote_stream_stop(void);

#endif
