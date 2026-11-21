#include "capture_stream.h"
#include <stdlib.h>
#include <time.h>
static uint64_t pixel(uint32_t x, uint32_t y) { return 4u * (y * 1920u + x); }
static uint64_t changed_flip(void) { return 1; }
int main(void) {
    uint8_t *pixels = calloc(1, PS4_REMOTE_COMPOSITOR_BYTES);
    if (!pixels || !ps4_remote_stream_start(pixel)) return 1;
    for (size_t i = 0; i < PS4_REMOTE_COMPOSITOR_BYTES; i += 4) {
        pixels[i] = 20;
        pixels[i + 1] = 70;
        pixels[i + 2] = 210;
    }
    const struct timespec delay = {0, 100000000};
    uint64_t (*snapshot)(void) = getenv("PS4_TEST_FLIP") ? changed_flip : NULL;
    for (int i = 0; i < 65; ++i) {
        ps4_remote_stream_submit(pixels, PS4_REMOTE_COMPOSITOR_BYTES, 0, snapshot);
        nanosleep(&delay, 0);
    }
    ps4_remote_stream_stop();
    free(pixels);
    return 0;
}
