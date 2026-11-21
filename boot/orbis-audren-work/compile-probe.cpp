#include "audren/Server/AudioRendererManager.h"

extern "C" uint64_t audren_probe_work_size(nxrt::audren::AudioRendererConfiguration* config)
{
    return nxrt::audren::AudioRendererManager::GetWorkBufferSize(*config);
}

extern "C" uint32_t audren_probe_open(nxrt::audren::AudioRendererManager* manager,
    nxrt::audren::AudioRendererConfiguration* config, uint64_t size)
{
    std::unique_ptr<nxrt::audren::AudioRenderSystem> renderer;
    const auto result = manager->OpenAudioRenderer(renderer, *config, 0, 0, size, 0xffff8001);
    if (renderer) {
        renderer->Start();
        renderer->Stop();
    }
    return static_cast<uint32_t>(result);
}
