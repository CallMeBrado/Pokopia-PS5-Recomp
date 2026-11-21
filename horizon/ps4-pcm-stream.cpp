/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ps4-pcm-stream.h"
#include "ps4-pcm-native.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

namespace {

using ps4_pcm_native::BlockFrames;
using ps4_pcm_native::Channels;
constexpr uint32_t MaxBuffers = 64;
constexpr int32_t AlreadyStarted = -1;

struct Entry {
    std::unique_ptr<int16_t[]> samples;
    uint64_t tag = 0;
    uint32_t frames = 0;
    uint32_t consumed = 0;
};

}  // namespace

struct Ps4PcmStream {
    Ps4PcmStream(Ps4PcmEvent callback, void *opaque) : event(callback), eventOpaque(opaque) {}

    bool Launch();
    void Join();
    void Worker();
    void Release(uint32_t count);
    void UpdateEvent();
    void RecordError(int32_t result);

    Ps4PcmEvent event;
    void *eventOpaque;
    std::mutex control;
    std::mutex lock;
    std::condition_variable changed;
#ifdef _WIN32
    HANDLE thread = nullptr;
#else
    pthread_t thread{};
#endif
    std::array<Entry, MaxBuffers> queue{};
    /* The first `released` entries hold tags only. The worker consumes the rest in order. */
    uint32_t head = 0;
    uint32_t count = 0;
    uint32_t released = 0;
    uint64_t played = 0;
    int32_t port = 0;
    int32_t nativeError = 0;
    float volume = 1.0f;
    bool started = false;
    bool flushing = false;
    bool busy = false;
    bool closing = false;
    alignas(64) std::array<int16_t, BlockFrames * Channels> block{};
};

#ifdef _WIN32
static unsigned __stdcall WorkerEntry(void *opaque) {
    static_cast<Ps4PcmStream *>(opaque)->Worker();
    return 0;
}

bool Ps4PcmStream::Launch() {
    thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, WorkerEntry, this, 0, nullptr));
    return thread != nullptr;
}

void Ps4PcmStream::Join() {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}
#else
static void *WorkerEntry(void *opaque) {
    static_cast<Ps4PcmStream *>(opaque)->Worker();
    return nullptr;
}

bool Ps4PcmStream::Launch() { return pthread_create(&thread, nullptr, WorkerEntry, this) == 0; }
void Ps4PcmStream::Join() { pthread_join(thread, nullptr); }
#endif

void Ps4PcmStream::UpdateEvent() {
    if (event) {
        event(eventOpaque, released != 0);
    }
}

void Ps4PcmStream::Release(uint32_t complete) {
    if (complete == 0) {
        return;
    }
    for (uint32_t i = 0; i < complete; ++i) {
        queue[(head + released + i) % MaxBuffers].samples.reset();
    }
    released += complete;
    UpdateEvent();
}

void Ps4PcmStream::RecordError(int32_t result) {
    if (result < 0 && nativeError == 0) {
        nativeError = result;
    }
}

void Ps4PcmStream::Worker() {
    std::unique_lock<std::mutex> guard(lock);
    while (!closing) {
        changed.wait(guard, [this] { return closing || (started && !flushing && released != count); });
        if (closing) {
            break;
        }

        block.fill(0);
        uint32_t validFrames = 0;
        uint32_t complete = 0;
        while (released + complete < count) {
            Entry &entry = queue[(head + released + complete) % MaxBuffers];
            const uint32_t frames = std::min(BlockFrames - validFrames, entry.frames - entry.consumed);
            if (frames != 0 && entry.samples) {
                const size_t samples = static_cast<size_t>(frames) * Channels;
                const auto *input = entry.samples.get() + static_cast<size_t>(entry.consumed) * Channels;
                auto *output = block.data() + validFrames * Channels;
                if (volume == 1.0f) {
                    std::copy_n(input, samples, output);
                } else {
                    std::transform(input, input + samples, output,
                        [gain = volume](int16_t sample) { return static_cast<int16_t>(sample * gain); });
                }
            }
            validFrames += frames;
            entry.consumed += frames;
            if (entry.consumed == entry.frames) {
                ++complete;
            } else {
                break;
            }
        }
        if (validFrames == 0) {
            Release(complete);
            continue;
        }

        busy = true;
        const int32_t outputPort = port;
        guard.unlock();
        const int32_t submit = ps4_pcm_native::Submit(outputPort, block.data());
        const int32_t done = submit < 0 ? submit : ps4_pcm_native::Wait(outputPort);
        guard.lock();
        if (done < 0) {
            RecordError(done);
            started = false;
            Release(count - released);
        } else {
            played += validFrames;
            Release(complete);
        }
        busy = false;
        changed.notify_all();
    }
}

Ps4PcmStream *ps4_pcm_create(Ps4PcmEvent event, void *opaque) {
    auto stream = std::unique_ptr<Ps4PcmStream>(new (std::nothrow) Ps4PcmStream(event, opaque));
    return stream && stream->Launch() ? stream.release() : nullptr;
}

void ps4_pcm_destroy(Ps4PcmStream *stream) {
    if (!stream) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(stream->lock);
        stream->closing = true;
        stream->started = false;
        stream->changed.notify_all();
    }
    stream->Join();
    if (stream->port > 0) {
        ps4_pcm_native::Close(stream->port);
    }
    if (stream->event) {
        stream->event(stream->eventOpaque, false);
    }
    delete stream;
}

int32_t ps4_pcm_start(Ps4PcmStream *stream) {
    std::lock_guard<std::mutex> control(stream->control);
    std::lock_guard<std::mutex> guard(stream->lock);
    if (stream->started) {
        return AlreadyStarted;
    }
    if (stream->port > 0) {
        const int32_t close = ps4_pcm_native::Close(stream->port);
        if (close < 0) {
            return close;
        }
        stream->port = 0;
    }
    const int32_t port = ps4_pcm_native::Open();
    if (port < 0) {
        return port;
    }
    stream->port = port;
    stream->nativeError = 0;
    stream->started = true;
    stream->changed.notify_all();
    return 0;
}

int32_t ps4_pcm_stop(Ps4PcmStream *stream) {
    std::lock_guard<std::mutex> control(stream->control);
    std::unique_lock<std::mutex> guard(stream->lock);
    if (stream->started) {
        stream->started = false;
        stream->changed.wait(guard, [stream] { return !stream->busy; });
        stream->Release(stream->count - stream->released);
    }
    if (stream->port <= 0) {
        return 0;
    }
    const int32_t result = ps4_pcm_native::Close(stream->port);
    if (result >= 0) {
        stream->port = 0;
    }
    return result;
}

bool ps4_pcm_started(Ps4PcmStream *stream) {
    std::lock_guard<std::mutex> guard(stream->lock);
    return stream->started;
}

Ps4PcmAppendResult ps4_pcm_append(Ps4PcmStream *stream, uint64_t tag, const void *pcm, uint32_t frames) {
    std::lock_guard<std::mutex> guard(stream->lock);
    if (stream->count == MaxBuffers) {
        return PS4_PCM_APPEND_FULL;
    }
    const size_t samples = static_cast<size_t>(frames) * Channels;
    std::unique_ptr<int16_t[]> copy;
    if (pcm && samples != 0) {
        copy.reset(new (std::nothrow) int16_t[samples]);
        if (!copy) {
            return PS4_PCM_APPEND_FAILED;
        }
        std::memcpy(copy.get(), pcm, samples * sizeof(int16_t));
    }
    stream->queue[(stream->head + stream->count) % MaxBuffers] = Entry{std::move(copy), tag, frames, 0};
    ++stream->count;
    stream->changed.notify_all();
    return PS4_PCM_APPEND_OK;
}

uint32_t ps4_pcm_take_released(Ps4PcmStream *stream, uint64_t *tags, uint32_t max) {
    std::lock_guard<std::mutex> guard(stream->lock);
    const uint32_t taken = std::min(stream->released, max);
    for (uint32_t i = 0; i < taken; ++i) {
        tags[i] = stream->queue[(stream->head + i) % MaxBuffers].tag;
    }
    stream->head = (stream->head + taken) % MaxBuffers;
    stream->count -= taken;
    stream->released -= taken;
    stream->UpdateEvent();
    return taken;
}

bool ps4_pcm_contains(Ps4PcmStream *stream, uint64_t tag) {
    std::lock_guard<std::mutex> guard(stream->lock);
    for (uint32_t i = 0; i < stream->count; ++i) {
        if (stream->queue[(stream->head + i) % MaxBuffers].tag == tag) {
            return true;
        }
    }
    return false;
}

uint32_t ps4_pcm_queued(Ps4PcmStream *stream) {
    std::lock_guard<std::mutex> guard(stream->lock);
    return stream->count - stream->released;
}

uint64_t ps4_pcm_played(Ps4PcmStream *stream) {
    std::lock_guard<std::mutex> guard(stream->lock);
    return stream->played;
}

bool ps4_pcm_flush(Ps4PcmStream *stream) {
    std::lock_guard<std::mutex> control(stream->control);
    std::unique_lock<std::mutex> guard(stream->lock);
    if (!stream->started) {
        return false;
    }
    stream->flushing = true;
    stream->changed.wait(guard, [stream] { return !stream->busy; });
    stream->Release(stream->count - stream->released);
    stream->flushing = false;
    return true;
}

void ps4_pcm_set_volume(Ps4PcmStream *stream, float volume) {
    std::lock_guard<std::mutex> guard(stream->lock);
    stream->volume = std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 0.0f;
}

float ps4_pcm_get_volume(Ps4PcmStream *stream) {
    std::lock_guard<std::mutex> guard(stream->lock);
    return stream->volume;
}

int32_t ps4_pcm_error(Ps4PcmStream *stream) {
    std::lock_guard<std::mutex> guard(stream->lock);
    return stream->nativeError;
}
