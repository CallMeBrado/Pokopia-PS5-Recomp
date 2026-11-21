/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>

namespace ps4_pcm_native {

constexpr uint32_t SampleRate = 48000;
constexpr uint32_t Channels = 2;
constexpr uint32_t BlockFrames = 256;

/* A positive port on success, negative native status on failure. */
int32_t Open();
int32_t Submit(int32_t port, const int16_t *samples);
int32_t Wait(int32_t port);
int32_t Close(int32_t port);

}  // namespace ps4_pcm_native
