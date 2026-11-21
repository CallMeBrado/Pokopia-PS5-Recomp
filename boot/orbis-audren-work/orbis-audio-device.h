#pragma once

#include <array>
#include <atomic>

#include "audren/Integration/IHardwareDeviceDriver.h"

namespace nxrt::audren {

class OrbisAudioDevice;

class OrbisAudioDeviceDriver final : public IHardwareDeviceDriver {
  public:
    ~OrbisAudioDeviceDriver() override;

    // The pinned DSP eagerly opens both devices and assumes they exist.
    // Call Prepare before constructing the renderer manager. The driver outlives it.
    int32_t Prepare();
    int32_t GetOutputError() const;

    std::unique_ptr<IHardwareDevice> OpenHardwareDevice(uint32_t sampleRate, uint32_t channelCount) override;
    bool SupportsChannelCount(uint32_t channelCount) const override;

  private:
    friend class OrbisAudioDevice;
    struct Port {
        int32_t handle = 0;
        bool leased = false;
    };

    void ClosePorts();
    void RecordOutputError(const char* operation, int32_t result);

    std::array<Port, Constants::AudioRendererSessionCountMax> _ports{};
    std::atomic<int32_t> _outputError{0};
};

}  // namespace nxrt::audren
