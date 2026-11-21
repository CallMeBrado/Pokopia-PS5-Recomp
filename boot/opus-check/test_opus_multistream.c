#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus_multistream.h>

#include "opus-vector.inc"

enum { RATE = 48000, CHANNELS = 2, STREAMS = 2, COUPLED = 0, FRAMES = 960 };

static int16_t pcm[FRAMES * CHANNELS];
static int16_t repeated[FRAMES * CHANNELS];

static int verify_channels(const int16_t *samples, double ratios[CHANNELS]) {
    for (unsigned c = 0; c < CHANNELS; ++c) {
        double power = 0.0, difference_power = 0.0;
        int32_t previous = samples[400 * CHANNELS + c];
        for (size_t i = 401; i < FRAMES; ++i) {
            const int32_t sample = samples[i * CHANNELS + c];
            const int32_t difference = sample - previous;
            power += (double)sample * sample;
            difference_power += (double)difference * difference;
            previous = sample;
        }
        if (power == 0) {
            return 0;
        }
        ratios[c] = difference_power / power;
    }
    return ratios[0] > 0.004 && ratios[0] < 0.018 && ratios[1] > 0.025 && ratios[1] < 0.060;
}

int main(void) {
    const opus_int32 state_bytes = opus_multistream_decoder_get_size(STREAMS, COUPLED);
    if (state_bytes <= 0 || opus_multistream_decoder_get_size(0, 0) != 0) {
        fprintf(stderr, "bad 2/0 Opus state size: %d\n", state_bytes);
        return 1;
    }
    OpusMSDecoder *decoder = malloc((size_t)state_bytes);
    if (!decoder) {
        return 1;
    }
    const unsigned char mapping[] = {0, 1};
    int result = opus_multistream_decoder_init(decoder, RATE, CHANNELS, STREAMS, COUPLED, mapping);
    if (result != OPUS_OK) {
        fprintf(stderr, "decoder init: %d\n", result);
        free(decoder);
        return 1;
    }
    opus_uint32 decoded_range = 0;
    double ratios[CHANNELS] = {0};
    result = opus_multistream_decode(decoder, packet, (opus_int32)sizeof packet, pcm, FRAMES, 0);
    int okay = result == FRAMES && verify_channels(pcm, ratios) &&
        opus_multistream_decoder_ctl(decoder, OPUS_GET_FINAL_RANGE(&decoded_range)) == OPUS_OK && decoded_range == final_range;
    if (!okay) {
        fprintf(stderr, "decode frames=%d, channel ratios=%.5f/%.5f, final range=0x%08x expected=0x%08x\n",
            result, ratios[0], ratios[1], (unsigned)decoded_range, (unsigned)final_range);
    }
    okay = okay && opus_multistream_decoder_ctl(decoder, OPUS_RESET_STATE) == OPUS_OK &&
        opus_multistream_decode(decoder, packet, (opus_int32)sizeof packet, repeated, FRAMES, 0) == FRAMES &&
        memcmp(pcm, repeated, sizeof pcm) == 0;
    free(decoder);
    if (!okay) {
        fputs("2/0 Opus PCM or reset check failed\n", stderr);
        return 1;
    }
    printf("%s 2/0 Opus: state=%d packet=%zu frames=%d ratios=%.5f/%.5f final=0x%08x reset identical\n",
        opus_get_version_string(), state_bytes, sizeof packet, result, ratios[0], ratios[1], (unsigned)decoded_range);
    return 0;
}
