/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef _WIN32
#define NOMINMAX
#endif
#include "audren/Server/AudioRendererManager.h"

#include <cstring>

extern "C" uint32_t pokopia_audio_work_buffer_size(const void *configuration, size_t bytes, uint64_t *size)
{
    constexpr uint32_t audio_module = 153;
    constexpr uint32_t invalid_combination = 0xe801;
    using namespace nxrt::audren;
    if (!configuration || !size || bytes < sizeof(AudioRendererConfiguration))
        return invalid_combination;
    AudioRendererConfiguration parameters;
    std::memcpy(&parameters, configuration, sizeof parameters);
    if (!BehaviourInfo::CheckValidRevision(parameters.revision))
        return audio_module | (2u << 9);
    *size = AudioRendererManager::GetWorkBufferSize(parameters);
    return 0;
}
