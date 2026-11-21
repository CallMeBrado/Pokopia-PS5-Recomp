/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>

extern "C" uint32_t pokopia_audio_work_buffer_size(const void *, size_t, uint64_t *);

int main()
{
    uint32_t configuration[13] = {48000, 240, 2, 0, 32, 1};
    configuration[12] = 0x3f764552;
    uint64_t size = 0;
    assert(pokopia_audio_work_buffer_size(configuration, sizeof configuration, &size) == 0);
    assert(size && size % 0x1000 == 0);
    const uint64_t original = size;
    configuration[4] = 64;
    assert(pokopia_audio_work_buffer_size(configuration, sizeof configuration, &size) == 0);
    assert(size > original && size % 0x1000 == 0);
    std::printf("Pinned work buffer sizes for 32 and 64 voices: %llu and %llu bytes\n",
                static_cast<unsigned long long>(original), static_cast<unsigned long long>(size));
    configuration[12] = 0x41764552;
    assert(pokopia_audio_work_buffer_size(configuration, sizeof configuration, &size) == 0x499);
    assert(pokopia_audio_work_buffer_size(configuration, sizeof configuration - 1, &size) == 0xe801);
}
