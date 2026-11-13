// Audio output of the browser build (../common/audio_out.h): the Linux build's design with an AudioWorklet in SDL's
// place. A mixer thread renders the engine's audio one quantum at a time into a ring in the shared WebAssembly memory;
// the page's AudioWorklet (page/bo1_audio_worklet.js), on the browser's audio thread, reads the ring directly and
// tells the mixer (a futex wake) when it has taken frames. Without the page's audio (no AudioContext: the player has
// not clicked yet, or the browser has none) the mixer renders in real time and discards the result, so the engine's
// voices advance and their callbacks fire.
//
// No microphone: AudioIn_Open returns nothing (voice chat is off).

#include "../common/audio_out.h"

#include <emscripten.h>
#include <emscripten/threading.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>
#include <thread>
#include <vector>

// The ring's header, read and written by the worklet at these offsets (bo1_audio_worklet.js): change both together.
struct AudioRingHeader
{
    std::atomic<int32_t> writePos; // 0  frames written (wraps at 2^31)
    std::atomic<int32_t> readPos;  // 4  frames taken by the worklet
    std::atomic<int32_t> wake;     // 8  bumped by the worklet after it takes frames (futex word)
    std::atomic<int32_t> glitches; // 12 worklet: quanta it found short while rendering was on
    std::atomic<int32_t> rendering; // 16 1: the engine is playing (an underrun counts as a glitch)
    int32_t capacity;              // 20 frames
    int32_t channels;              // 24
    int32_t sampleRate;            // 28
    std::atomic<int32_t> pull;     // 32 worklet: frames per process() call (128)
    std::atomic<int32_t> connected; // 36 worklet: 1 once it reads the ring
    int32_t pad[6];                // 40
    // float frames[capacity * channels] follow, at 64
};
static_assert(sizeof(AudioRingHeader) == 64, "worklet offsets");

struct AudioOut
{
    AudioRingHeader *ring = nullptr; // header and frames, one allocation
    float *frames = nullptr;
    int channels = 2;
    int sampleRate = 48000;
    int quantum = 480;
    uint32_t capacity = 0;
    uint32_t target = 0;
    AudioOutRender render = nullptr;
    void *user = nullptr;
    std::atomic<bool> quit{false};
    std::thread thread;
    std::vector<float> scratch;
};

namespace
{
void MixerThread(AudioOut *o)
{
    o->scratch.assign((size_t)o->quantum * o->channels, 0.0f);
    AudioRingHeader *h = o->ring;
    int stalls = 0;
    double lastRender = emscripten_get_now();
    while (!o->quit.load())
    {
        const bool connected = h->connected.load(std::memory_order_acquire) != 0;
        if (connected)
        {
            // keep enough queued for the worklet's next pulls
            for (;;)
            {
                const int32_t w = h->writePos.load(std::memory_order_relaxed);
                const int32_t r = h->readPos.load(std::memory_order_acquire);
                const uint32_t queued = (uint32_t)(w - r) & 0x7FFFFFFF;
                if (queued + (uint32_t)o->quantum > o->target || o->quit.load())
                    break;
                if (h->rendering.load(std::memory_order_relaxed))
                    o->render(o->user, o->scratch.data(), o->quantum, o->channels);
                else
                    memset(o->scratch.data(), 0, o->scratch.size() * sizeof(float));
                for (int i = 0; i < o->quantum;)
                {
                    const uint32_t idx = (uint32_t)((uint32_t)(w + i) & 0x7FFFFFFF) % o->capacity;
                    const uint32_t run = std::min(o->capacity - idx, (uint32_t)(o->quantum - i));
                    memcpy(o->frames + (size_t)idx * o->channels, &o->scratch[(size_t)i * o->channels],
                           (size_t)run * o->channels * sizeof(float));
                    i += (int)run;
                }
                h->writePos.store((w + o->quantum) & 0x7FFFFFFF, std::memory_order_release);
                lastRender = emscripten_get_now();
            }
        }
        // woken by the worklet taking frames; the timeout keeps the engine turning if it stops
        const int32_t seen = h->wake.load(std::memory_order_acquire);
        const int ret = emscripten_futex_wait(&h->wake, (uint32_t)seen, 20.0);
        if (o->quit.load())
            break;
        if (ret == 0 && connected)
        {
            stalls = 0;
            continue;
        }
        // No worklet, or it stopped (the page hidden, the AudioContext suspended) for 200 ms: render in real time and
        // discard, so voices advance and buffer callbacks keep firing.
        if (connected && emscripten_get_now() - lastRender < 200.0)
            continue;
        if (++stalls >= (connected ? 10 : 0))
        {
            for (int done = 0; done < o->sampleRate / 50 && !o->quit.load(); done += o->quantum)
                if (h->rendering.load(std::memory_order_relaxed))
                    o->render(o->user, o->scratch.data(), o->quantum, o->channels);
        }
    }
}
} // namespace

extern "C" {

int AudioOut_EnumerateDevices(AudioOutDeviceInfo *out, int maxDevices)
{
    if (maxDevices <= 0)
        return 0;
    memset(&out[0], 0, sizeof(out[0]));
    snprintf(out[0].uid, sizeof(out[0].uid), "@default");
    snprintf(out[0].name, sizeof(out[0].name), "Browser audio");
    out[0].channels = 2;
    out[0].sampleRate = 48000;
    out[0].isDefault = 1;
    return 1;
}

AudioOut *AudioOut_Open(const char *, int channels, int sampleRate, int quantumFrames, AudioOutRender render,
                        void *user)
{
    if (!render)
        return nullptr;
    AudioOut *o = new AudioOut();
    // (the page's output is stereo; the engine mixes surround down when told the device has two channels)
    o->channels = channels > 0 ? std::min(channels, 8) : 2;
    o->sampleRate = sampleRate > 0 ? sampleRate : 48000;
    o->quantum = quantumFrames > 0 ? quantumFrames : o->sampleRate / 100;
    o->render = render;
    o->user = user;
    // about 40 ms queued: the worklet pulls 128 frames at a time on the audio thread, which page jank doesn't touch
    o->target = (uint32_t)std::max(o->quantum * 3, o->sampleRate / 25);
    o->capacity = o->target + (uint32_t)o->quantum * 4 + 1024;
    const size_t bytes = sizeof(AudioRingHeader) + (size_t)o->capacity * o->channels * sizeof(float);
    void *mem = aligned_alloc(64, (bytes + 63) & ~(size_t)63);
    memset(mem, 0, bytes);
    o->ring = new (mem) AudioRingHeader();
    o->frames = (float *)((uint8_t *)mem + sizeof(AudioRingHeader));
    o->ring->capacity = (int32_t)o->capacity;
    o->ring->channels = o->channels;
    o->ring->sampleRate = o->sampleRate;
    o->ring->rendering.store(1);
    // the page connects its worklet to the ring (now, or once the player has clicked: browsers start audio only then)
    MAIN_THREAD_ASYNC_EM_ASM(
        {
            if (Module['bo1AudioOpen']) Module['bo1AudioOpen']($0 >>> 0);
        },
        o->ring);
    o->thread = std::thread(MixerThread, o);
    return o;
}

void AudioOut_Close(AudioOut *o)
{
    if (!o)
        return;
    MAIN_THREAD_EM_ASM(
        {
            if (Module['bo1AudioClose']) Module['bo1AudioClose']($0 >>> 0);
        },
        o->ring);
    o->quit.store(true);
    o->ring->wake.fetch_add(1);
    emscripten_futex_wake(&o->ring->wake, 1);
    if (o->thread.joinable())
        o->thread.join();
    o->ring->~AudioRingHeader();
    free(o->ring);
    delete o;
}

void AudioOut_SetRendering(AudioOut *o, int enabled)
{
    if (o)
        o->ring->rendering.store(enabled ? 1 : 0);
}

int AudioOut_LatencyFrames(AudioOut *o)
{
    if (!o)
        return 0;
    const uint32_t queued = (uint32_t)(o->ring->writePos.load() - o->ring->readPos.load()) & 0x7FFFFFFF;
    // (plus the browser's own output latency, about two of its 128-frame quanta)
    return (int)queued + 256;
}

uint32_t AudioOut_Glitches(AudioOut *o) { return o ? (uint32_t)o->ring->glitches.load() : 0; }

AudioIn *AudioIn_Open(int, int, int, int, AudioInCapture, void *) { return nullptr; }
void AudioIn_Close(AudioIn *) {}

} // extern "C"
