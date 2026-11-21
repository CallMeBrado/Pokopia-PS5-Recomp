#include "orbis-audio-device.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include <orbis/AudioOut.h>
#include <orbis/UserService.h>

#include "audren/Dsp/PcmHelper.h"
#include "nxrt/log.h"

namespace nxrt::audren {

namespace {
constexpr uint32_t SampleRate = 48000;
constexpr uint32_t ChannelCount = 2;
constexpr uint32_t OutputFrames = 256;
constexpr size_t OutputSamples = OutputFrames * ChannelCount;
}

class OrbisAudioDevice final : public IHardwareDevice {
  public:
    OrbisAudioDevice(OrbisAudioDeviceDriver& driver, OrbisAudioDeviceDriver::Port& port)
        : _driver(driver), _port(port) {
        _port.leased = true;
    }

    ~OrbisAudioDevice() override {
        if (_queued) {
            _driver.RecordOutputError("drain", sceAudioOutOutput(_port.handle, nullptr));
        }
        _port.leased = false;
    }

    void SetVolume(float volume) override {
        _volume.store(std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 0.0f);
    }
    float GetVolume() override { return _volume.load(); }
    uint32_t GetSampleRate() override { return SampleRate; }
    uint32_t GetChannelCount() override { return ChannelCount; }

    void AppendBuffer(Span<const int16_t> data, uint32_t channelCount) override {
        if (_failed || data.IsEmpty()) {
            return;
        }
        if (channelCount != ChannelCount || data.Length % ChannelCount != 0) {
            _driver.RecordOutputError("stereo PCM input", static_cast<int32_t>(ORBIS_AUDIO_OUT_ERROR_INVALID_FORMAT));
            _failed = true;
            return;
        }

        const float volume = GetVolume();
        while (!data.IsEmpty()) {
            auto& block = _blocks[_nextBlock];
            const size_t samples = std::min(OutputSamples - _pendingSamples, data.Length);
            auto* destination = block.data() + _pendingSamples;
            if (volume == 1.0f) {
                std::copy_n(data.Data, samples, destination);
            } else {
                std::transform(data.Data, data.Data + samples, destination,
                    [volume](int16_t sample) { return PcmHelper::Saturate(static_cast<float>(sample) * volume); });
            }
            _pendingSamples += samples;
            data = data.Slice(samples);
            if (_pendingSamples == OutputSamples && !Submit(block.data())) {
                _failed = true;
                return;
            }
        }
    }

  private:
    bool Submit(const int16_t* block) {
        // The OpenOrbis audio-wav sample uses a null submission as a completion barrier.
        if (_queued) {
            const int32_t result = sceAudioOutOutput(_port.handle, nullptr);
            if (result < 0) {
                _driver.RecordOutputError("wait", result);
                return false;
            }
            _queued = false;
        }
        const int32_t result = sceAudioOutOutput(_port.handle, block);
        if (result < 0) {
            _driver.RecordOutputError("submit", result);
            return false;
        }
        _queued = true;
        _nextBlock ^= 1;
        _pendingSamples = 0;
        return true;
    }

    OrbisAudioDeviceDriver& _driver;
    OrbisAudioDeviceDriver::Port& _port;
    std::atomic<float> _volume{1.0f};
    alignas(64) std::array<std::array<int16_t, OutputSamples>, 2> _blocks{};
    size_t _nextBlock = 0;
    size_t _pendingSamples = 0;
    bool _queued = false;
    bool _failed = false;
};

OrbisAudioDeviceDriver::~OrbisAudioDeviceDriver() { ClosePorts(); }

int32_t OrbisAudioDeviceDriver::Prepare() {
    if (_ports.front().handle > 0) {
        return 0;
    }
    const int32_t userResult = sceUserServiceInitialize(nullptr);
    if (userResult < 0 && static_cast<uint32_t>(userResult) != ORBIS_USER_SERVICE_ERROR_ALREADY_INITIALIZED) {
        return userResult;
    }
    const int32_t initResult = sceAudioOutInit();
    if (initResult < 0 && static_cast<uint32_t>(initResult) != ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT) {
        return initResult;
    }
    for (auto& port : _ports) {
        const int32_t handle = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
            OutputFrames, SampleRate, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        if (handle <= 0) {
            ClosePorts();
            return handle < 0 ? handle : static_cast<int32_t>(ORBIS_AUDIO_OUT_ERROR_INVALID_PORT);
        }
        port.handle = handle;
    }
    _outputError.store(0);
    return 0;
}

int32_t OrbisAudioDeviceDriver::GetOutputError() const { return _outputError.load(); }

std::unique_ptr<IHardwareDevice> OrbisAudioDeviceDriver::OpenHardwareDevice(uint32_t sampleRate, uint32_t channelCount) {
    if (sampleRate == SampleRate && SupportsChannelCount(channelCount)) {
        for (auto& port : _ports) {
            if (port.handle > 0 && !port.leased) {
                return std::make_unique<OrbisAudioDevice>(*this, port);
            }
        }
    }
    return {};
}

bool OrbisAudioDeviceDriver::SupportsChannelCount(uint32_t channelCount) const { return channelCount == ChannelCount; }

void OrbisAudioDeviceDriver::ClosePorts() {
    for (auto& port : _ports) {
        assert(!port.leased);
        if (port.handle > 0) {
            RecordOutputError("close", sceAudioOutClose(port.handle));
            port.handle = 0;
        }
    }
}

void OrbisAudioDeviceDriver::RecordOutputError(const char* operation, int32_t result) {
    int32_t first = 0;
    if (result < 0 && _outputError.compare_exchange_strong(first, result)) {
        NXLOG_W("AudioRenderer", "Orbis AudioOut %s failed: 0x%08x", operation, static_cast<uint32_t>(result));
    }
}

}  // namespace nxrt::audren
