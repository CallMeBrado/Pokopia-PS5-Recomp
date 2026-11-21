#include "ps4-pcm-native.h"
#include "ps4-pcm-stream.h"

#include <orbis/AudioOut.h>
#include <orbis/UserService.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int32_t Port = 41;
constexpr int32_t PlaybackError = static_cast<int32_t>(ORBIS_AUDIO_OUT_ERROR_BUSY);
using Block = std::array<int16_t, ps4_pcm_native::BlockFrames * ps4_pcm_native::Channels>;

std::mutex fixtureLock;
std::condition_variable fixtureChanged;
std::vector<Block> blocks;
struct DiagnosticEntry {
    std::string_view operation;
    bool entering;
    int32_t result;
};
std::vector<DiagnosticEntry> diagnosticEntries;
const int16_t *nativeBuffer = nullptr;
size_t permitted = 0;
size_t completed = 0;
int32_t playbackError = 0;
uint32_t opens = 0;
uint32_t closes = 0;
bool signaled = false;

void Event(void *, bool value) {
    std::lock_guard<std::mutex> guard(fixtureLock);
    signaled = value;
    fixtureChanged.notify_all();
}

void Diagnostic(void *, const char *operation, bool entering, int32_t result) {
    std::lock_guard<std::mutex> guard(fixtureLock);
    diagnosticEntries.push_back({operation, entering, result});
}

bool HasDiagnostic(std::string_view operation, bool entering, int32_t result) {
    std::lock_guard<std::mutex> guard(fixtureLock);
    return std::any_of(diagnosticEntries.begin(), diagnosticEntries.end(), [=](const DiagnosticEntry &entry) {
        return entry.operation == operation && entry.entering == entering && entry.result == result;
    });
}

template <class Predicate>
void WaitFor(Predicate predicate) {
    std::unique_lock<std::mutex> guard(fixtureLock);
    assert(fixtureChanged.wait_for(guard, std::chrono::seconds(3), predicate));
}

void Allow(size_t count, int32_t error = 0) {
    std::lock_guard<std::mutex> guard(fixtureLock);
    permitted = count;
    playbackError = error;
    fixtureChanged.notify_all();
}

bool Signaled() {
    std::lock_guard<std::mutex> guard(fixtureLock);
    return signaled;
}

void CheckBlock(size_t index, size_t validFrames, int firstFrame, float gain = 1.0f) {
    std::lock_guard<std::mutex> guard(fixtureLock);
    const Block &block = blocks.at(index);
    for (size_t frame = 0; frame < block.size() / 2; ++frame) {
        const int16_t expected = frame < validFrames ? static_cast<int16_t>((firstFrame + frame) * gain) : 0;
        assert(block[2 * frame] == expected);
        assert(block[2 * frame + 1] == -expected);
    }
}

std::vector<int16_t> Samples(int first, int frames) {
    std::vector<int16_t> pcm(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        pcm[static_cast<size_t>(i) * 2] = static_cast<int16_t>(first + i);
        pcm[static_cast<size_t>(i) * 2 + 1] = static_cast<int16_t>(-first - i);
    }
    return pcm;
}

}  // namespace

extern "C" int32_t sceUserServiceInitialize(void *) {
    return static_cast<int32_t>(ORBIS_USER_SERVICE_ERROR_ALREADY_INITIALIZED);
}
extern "C" int32_t sceAudioOutInit() { return static_cast<int32_t>(ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT); }
extern "C" int32_t sceAudioOutOpen(OrbisUserServiceUserId user, enum OrbisAudioOutPort type, int32_t index,
    uint32_t length, uint32_t rate, uint32_t format) {
    assert(user == ORBIS_USER_SERVICE_USER_ID_SYSTEM && type == ORBIS_AUDIO_OUT_PORT_TYPE_MAIN && index == 0);
    assert(length == 256 && rate == 48000 && format == ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    ++opens;
    return Port;
}
extern "C" int32_t sceAudioOutClose(int32_t handle) {
    assert(handle == Port);
    ++closes;
    return 0;
}
extern "C" int32_t sceAudioOutOutput(int32_t handle, const void *pcm) {
    assert(handle == Port);
    std::unique_lock<std::mutex> guard(fixtureLock);
    if (pcm) {
        assert(nativeBuffer == nullptr);
        nativeBuffer = static_cast<const int16_t *>(pcm);
        blocks.emplace_back();
        std::copy_n(nativeBuffer, blocks.back().size(), blocks.back().data());
        fixtureChanged.notify_all();
        return 0;
    }
    assert(fixtureChanged.wait_for(guard, std::chrono::seconds(3), [] { return permitted > completed; }));
    assert(nativeBuffer && std::equal(blocks.back().begin(), blocks.back().end(), nativeBuffer));
    nativeBuffer = nullptr;
    ++completed;
    return playbackError;
}

int main() {
    Ps4PcmStream *stream = ps4_pcm_create_with_diagnostics(Event, Diagnostic, nullptr);
    assert(stream && !ps4_pcm_started(stream));
    assert(!ps4_pcm_flush(stream));
    {
        auto first = Samples(100, 240);
        auto second = Samples(340, 240);
        assert(ps4_pcm_append(stream, 11, first.data(), 240) == PS4_PCM_APPEND_OK);
        assert(ps4_pcm_append(stream, 12, second.data(), 240) == PS4_PCM_APPEND_OK);
        std::fill(first.begin(), first.end(), 0);
        std::fill(second.begin(), second.end(), 0);
    }
    assert(ps4_pcm_stop(stream) == 0 && ps4_pcm_queued(stream) == 2);
    assert(ps4_pcm_start(stream) == 0 && ps4_pcm_started(stream));
    assert(HasDiagnostic("sceUserServiceInitialize", false, static_cast<int32_t>(ORBIS_USER_SERVICE_ERROR_ALREADY_INITIALIZED)));
    assert(HasDiagnostic("sceAudioOutInit", false, static_cast<int32_t>(ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT)));
    assert(HasDiagnostic("sceAudioOutOpen", true, 0));
    assert(HasDiagnostic("sceAudioOutOpen", false, Port));
    assert(ps4_pcm_start(stream) < 0);
    WaitFor([] { return blocks.size() == 1; });
    CheckBlock(0, 256, 100);
    uint64_t tags[64]{};
    assert(!Signaled() && ps4_pcm_take_released(stream, tags, 64) == 0);
    assert(ps4_pcm_played(stream) == 0 && ps4_pcm_contains(stream, 11));
    Allow(1);
    WaitFor([] { return blocks.size() == 2 && signaled; });
    assert(HasDiagnostic("sceAudioOutOutput samples", true, 0));
    assert(HasDiagnostic("sceAudioOutOutput samples", false, 0));
    assert(HasDiagnostic("sceAudioOutOutput completion", true, 0));
    assert(HasDiagnostic("sceAudioOutOutput completion", false, 0));
    CheckBlock(1, 224, 356);
    assert(ps4_pcm_take_released(stream, tags, 64) == 1 && tags[0] == 11);
    assert(!Signaled() && ps4_pcm_played(stream) == 256 && ps4_pcm_queued(stream) == 1);
    auto third = Samples(580, 17);
    assert(ps4_pcm_append(stream, 13, third.data(), 17) == PS4_PCM_APPEND_OK);
    Allow(2);
    WaitFor([] { return blocks.size() == 3 && signaled; });
    CheckBlock(2, 17, 580);
    assert(ps4_pcm_take_released(stream, tags, 64) == 1 && tags[0] == 12);
    assert(ps4_pcm_played(stream) == 480);
    Allow(3);
    WaitFor([] { return signaled; });
    assert(ps4_pcm_take_released(stream, tags, 64) == 1 && tags[0] == 13);
    assert(ps4_pcm_played(stream) == 497 && ps4_pcm_queued(stream) == 0);

    auto fourth = Samples(600, 300);
    assert(ps4_pcm_append(stream, 14, fourth.data(), 300) == PS4_PCM_APPEND_OK);
    WaitFor([] { return blocks.size() == 4; });
    assert(ps4_pcm_append(stream, 15, nullptr, 20) == PS4_PCM_APPEND_OK);
    Allow(4, PlaybackError);
    WaitFor([] { return signaled; });
    assert(!ps4_pcm_started(stream) && ps4_pcm_error(stream) == PlaybackError);
    assert(HasDiagnostic("sceAudioOutOutput completion", false, PlaybackError));
    assert(ps4_pcm_take_released(stream, tags, 1) == 1 && tags[0] == 14 && Signaled());
    assert(ps4_pcm_take_released(stream, tags, 64) == 1 && tags[0] == 15 && !Signaled());
    assert(ps4_pcm_played(stream) == 497);

    for (uint64_t i = 0; i < 64; ++i) {
        assert(ps4_pcm_append(stream, 100 + i, nullptr, 0) == PS4_PCM_APPEND_OK);
    }
    assert(ps4_pcm_append(stream, 200, nullptr, 0) == PS4_PCM_APPEND_FULL);
    assert(ps4_pcm_start(stream) == 0 && ps4_pcm_error(stream) == 0);
    WaitFor([] { return signaled; });
    assert(ps4_pcm_take_released(stream, tags, 64) == 64 && !Signaled());
    for (uint64_t i = 0; i < 64; ++i) {
        assert(tags[i] == 100 + i);
    }
    ps4_pcm_set_volume(stream, 0.5f);
    assert(ps4_pcm_get_volume(stream) == 0.5f);
    auto fifth = Samples(800, 3);
    assert(ps4_pcm_append(stream, 16, fifth.data(), 3) == PS4_PCM_APPEND_OK);
    WaitFor([] { return blocks.size() == 5; });
    CheckBlock(4, 3, 800, 0.5f);
    Allow(5);
    WaitFor([] { return signaled; });
    assert(ps4_pcm_take_released(stream, tags, 64) == 1 && tags[0] == 16);
    assert(ps4_pcm_played(stream) == 500 && ps4_pcm_flush(stream));

    auto sixth = Samples(1000, 300);
    assert(ps4_pcm_append(stream, 17, sixth.data(), 300) == PS4_PCM_APPEND_OK);
    WaitFor([] { return blocks.size() == 6; });
    assert(ps4_pcm_append(stream, 18, nullptr, 20) == PS4_PCM_APPEND_OK);
    auto stop = std::async(std::launch::async, [stream] { return ps4_pcm_stop(stream); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (ps4_pcm_started(stream) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    assert(!ps4_pcm_started(stream) && stop.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    assert(!Signaled() && ps4_pcm_played(stream) == 500);
    Allow(6);
    assert(stop.get() == 0 && Signaled());
    assert(ps4_pcm_take_released(stream, tags, 64) == 2 && tags[0] == 17 && tags[1] == 18);
    assert(ps4_pcm_played(stream) == 756 && !Signaled());
    ps4_pcm_destroy(stream);
    assert(opens == 2 && closes == 2 && !Signaled());
    assert(HasDiagnostic("sceAudioOutClose", false, 0));
    std::puts("AudioOut PCM verified: stereo packing, completion-gated FIFO tags/event, full/wrap, diagnostic phases, volume, and in-flight Stop.");
}
