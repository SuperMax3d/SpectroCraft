// ============================================================================
// Fake recorder plugin - test double for the Recorder Plugin API v1.
//
// Deterministic: no wall-clock pacing. Each device generates a fixed set of
// sine waves (exact phase accumulation) into a bounded ring buffer. The
// generator runs as fast as the host drains: when the ring holds kRingFrames
// frames it sleeps on a condition variable until a read frees space (or stop
// is requested). Frames are therefore never dropped, the stream read by the
// host is one continuous sine per channel, and an idle session costs no CPU
// (the former version produced without pause into an 8 MB vector and erased
// its head on every 256-frame block).
//
// Portable (std::thread / std::condition_variable, C ABI): built and loaded
// by dlopen on macOS / Linux as well as by LoadLibrary on Windows.
// ============================================================================

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES 1
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "RecorderPlugin.h"

namespace {

struct FakeDevice
{
    std::string id;
    std::string name;
    int channels = 1;
    double rate = 8000.0;
    // One sine per channel: frequency in Hz (0 = silence).
    std::vector<double> freqs;
};

// Ring capacity in frames: ~4 s at 8 kHz, ~2 s at 16 kHz. Larger than what
// the host pulls per timer tick (up to one second of audio), so a pull always
// finds a full second available as soon as the generator had a moment to run.
constexpr size_t kRingFrames = 1u << 15;
constexpr int kBlockFrames = 256;

// Fixed-size circular buffer of interleaved frames (head = oldest frame).
struct Ring
{
    std::vector<float> data;
    size_t capFrames = 0;
    size_t head = 0;      // index of the oldest frame
    size_t count = 0;     // frames stored
    int ch = 1;
    mutable std::mutex mtx;
    std::condition_variable spaceFreed;

    void reset(int channels, size_t frames)
    {
        std::lock_guard<std::mutex> lk(mtx);
        ch = std::max(1, channels);
        capFrames = frames;
        data.assign(capFrames * (size_t) ch, 0.0f);
        head = 0;
        count = 0;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lk(mtx);
        head = 0;
        count = 0;
    }

    // Blocks (without spinning) until `frames` frames fit or `stop` is set.
    // Returns false when stopped before the block could be stored.
    bool pushBlocking(const float* p, size_t frames, const std::atomic<bool>& stop)
    {
        std::unique_lock<std::mutex> lk(mtx);
        while (!stop.load() && capFrames - count < frames)
            spaceFreed.wait_for(lk, std::chrono::milliseconds(20));
        if (stop.load())
            return false;
        size_t tail = (head + count) % capFrames;
        for (size_t f = 0; f < frames; ++f)
        {
            std::memcpy(&data[tail * (size_t) ch], p + f * (size_t) ch, sizeof(float) * (size_t) ch);
            tail = (tail + 1) % capFrames;
        }
        count += frames;
        return true;
    }

    void wakeAll() { std::lock_guard<std::mutex> lk(mtx); spaceFreed.notify_all(); }

    long long available() const
    {
        std::lock_guard<std::mutex> lk(mtx);
        return (long long) count;
    }

    int read(float* dest, long long maxFrames)
    {
        if (dest == nullptr || maxFrames <= 0) return 0;
        size_t take = 0;
        {
            std::lock_guard<std::mutex> lk(mtx);
            take = std::min(count, (size_t) maxFrames);
            if (take == 0) return 0;
            // At most two contiguous runs (before and after the wrap).
            const size_t first = std::min(take, capFrames - head);
            std::memcpy(dest, &data[head * (size_t) ch], sizeof(float) * first * (size_t) ch);
            if (take > first)
                std::memcpy(dest + first * (size_t) ch, data.data(),
                            sizeof(float) * (take - first) * (size_t) ch);
            head = (head + take) % capFrames;
            count -= take;
        }
        spaceFreed.notify_all();
        return (int) take;
    }
};

std::vector<FakeDevice> g_devices = {
    { "dev2ch", "Fake 2 ch @ 8 kHz", 2, 8000.0, { 440.0, 0.0 } },
    { "dev4ch", "Fake 4 ch @ 16 kHz", 4, 16000.0, { 100.0, 200.0, 300.0, 400.0 } },
};

Ring g_ring;
std::atomic<bool> g_stop{ false };
std::atomic<int> g_running{ 0 };
std::thread g_thread;
int g_activeIndex = -1;
std::string g_lastError;

void genLoop(int index)
{
    const FakeDevice& d = g_devices[(size_t) index];
    std::vector<double> phase((size_t) d.channels, 0.0);
    std::vector<float> block((size_t) d.channels * (size_t) kBlockFrames);
    while (!g_stop.load())
    {
        for (int f = 0; f < kBlockFrames; ++f)
            for (int c = 0; c < d.channels; ++c)
            {
                const double fr = d.freqs[(size_t) c];
                block[(size_t) f * (size_t) d.channels + (size_t) c] =
                    fr > 0.0 ? (float) std::sin(phase[(size_t) c]) : 0.0f;
                phase[(size_t) c] += 2.0 * M_PI * fr / d.rate;
            }
        if (!g_ring.pushBlocking(block.data(), (size_t) kBlockFrames, g_stop))
            break;
    }
}

const FakeDevice* find(const char* id)
{
    if (id == nullptr) return nullptr;
    for (const auto& d : g_devices)
        if (d.id == id) return &d;
    return nullptr;
}

int activeChannels()
{
    return g_activeIndex >= 0 ? g_devices[(size_t) g_activeIndex].channels : 1;
}

}   // namespace

extern "C" {

SC_REC_EXPORT int sc_rec_get_api_version(void)
{
    return SC_RECORDER_API_VERSION;
}

SC_REC_EXPORT int sc_rec_get_plugin_info(sc_rec_plugin_info* out)
{
    if (out == nullptr) { g_lastError = "null"; return 1; }
    out->id = "fake";
    out->name = "Fake Recorder (test)";
    out->version = "1.1";
    return 0;
}

SC_REC_EXPORT int sc_rec_count_devices(void)
{
    return (int) g_devices.size();
}

SC_REC_EXPORT int sc_rec_get_device(int index, sc_rec_device_info* out)
{
    if (out == nullptr || index < 0 || index >= (int) g_devices.size())
    { g_lastError = "bad index"; return 1; }
    const FakeDevice& d = g_devices[(size_t) index];

    static thread_local std::vector<double> rates;
    static thread_local std::vector<int> depths;
    rates.clear(); depths.clear();
    rates.push_back(d.rate);
    depths.push_back(32);

    out->id = d.id.c_str();
    out->name = d.name.c_str();
    out->minChannels = d.channels;
    out->maxChannels = d.channels;
    out->defaultRate = d.rate;
    out->numRates = 1;
    out->rates = rates.data();
    out->minRate = d.rate;
    out->maxRate = d.rate;
    out->numDepths = 1;
    out->depths = depths.data();
    return 0;
}

SC_REC_EXPORT int sc_rec_configure(const char* deviceId, int wantChannels, double wantRate,
                                   int wantBitDepth, sc_rec_negotiated* actual)
{
    if (g_running.load()) { g_lastError = "already running"; return 1; }
    const FakeDevice* d = find(deviceId);
    if (d == nullptr) { g_lastError = "unknown device"; return 2; }
    if (wantChannels != d->channels || std::abs(wantRate - d->rate) > 0.5 || wantBitDepth != 32)
    { g_lastError = "unsupported format for this fake device"; return 3; }

    g_activeIndex = 0;
    for (int i = 0; i < (int) g_devices.size(); ++i)
        if (g_devices[(size_t) i].id == d->id) { g_activeIndex = i; break; }

    if (actual != nullptr)
    {
        actual->channels = d->channels;
        actual->rate = d->rate;
        actual->bitDepth = 32;
    }
    return 0;
}

SC_REC_EXPORT int sc_rec_start(void)
{
    if (g_running.load()) { g_lastError = "already running"; return 1; }
    if (g_activeIndex < 0) { g_lastError = "no device configured"; return 2; }
    if (g_thread.joinable()) { g_stop.store(true); g_ring.wakeAll(); g_thread.join(); }
    g_ring.reset(activeChannels(), kRingFrames);
    g_stop.store(false);
    g_thread = std::thread(genLoop, g_activeIndex);
    // The generator produces immediately; the session counts as running as
    // soon as its thread exists.
    g_running.store(1);
    return 0;
}

SC_REC_EXPORT int sc_rec_stop(void)
{
    if (!g_running.load() && !g_thread.joinable()) return 0;
    g_stop.store(true);
    g_ring.wakeAll();
    if (g_thread.joinable()) g_thread.join();
    g_running.store(0);
    return 0;
}

SC_REC_EXPORT int sc_rec_is_running(void)
{
    return g_running.load();
}

SC_REC_EXPORT long long sc_rec_available_frames(void)
{
    return g_ring.available();
}

SC_REC_EXPORT int sc_rec_read(float* dest, int channels, long long maxFrames)
{
    if (g_activeIndex < 0 && channels <= 0) return 0;
    return g_ring.read(dest, maxFrames);
}

SC_REC_EXPORT const char* sc_rec_last_error(void)
{
    return g_lastError.c_str();
}

}   // extern "C"
