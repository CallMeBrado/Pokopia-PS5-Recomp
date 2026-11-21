/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Desktop bring-up only: model the AudioOut completion clock without a PS4. */
#include "ps4-pcm-native.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace ps4_pcm_native {

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto BlockTime = std::chrono::nanoseconds(1000000000ll * BlockFrames / SampleRate);
thread_local Clock::time_point nextCompletion;
}

int32_t Open(const Diagnostics &) { return 1; }

int32_t Submit(int32_t, const int16_t *) {
    nextCompletion = std::max(nextCompletion, Clock::now()) + BlockTime;
    return 0;
}

int32_t Wait(int32_t) {
    std::this_thread::sleep_until(nextCompletion);
    return 0;
}

int32_t Close(int32_t) { return 0; }

}  // namespace ps4_pcm_native
