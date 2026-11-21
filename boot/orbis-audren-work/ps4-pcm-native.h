/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include "ps4-pcm-stream.h"

namespace ps4_pcm_native {

constexpr uint32_t SampleRate = 48000;
constexpr uint32_t Channels = 2;
constexpr uint32_t BlockFrames = 256;

struct Diagnostics {
    Ps4PcmDiagnostic report;
    void *opaque;

    void Begin(const char *operation) const {
        if (report) report(opaque, operation, true, 0);
    }

    void End(const char *operation, int32_t result) const {
        if (report) report(opaque, operation, false, result);
    }
};

/* A positive port on success, negative native status on failure. */
int32_t Open(const Diagnostics &diagnostics);
int32_t Submit(int32_t port, const int16_t *samples);
int32_t Wait(int32_t port);
int32_t Close(int32_t port);

}  // namespace ps4_pcm_native
