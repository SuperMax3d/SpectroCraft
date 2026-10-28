// ============================================================================
// Fake recorder plugin - test double for the Recorder Plugin API v1.
//
// Deterministic: no wall-clock pacing. Each device generates a fixed set of
// sine waves (exact phase accumulation) as fast as possible into an internal
// ring, so unit tests can verify the full load/enumerate/configure/start/
// pull/stop path with exact sample values.
// ============================================================================

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES 1
#endif

#include <atomic>
#include <cmath>
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

struct Ring
{
    std::vector<float> data;      // interleaved, oldest first
    size_t capFrames = 1 << 20;
    mutable std::mutex mtx;

    void push(const float* p, int64_t frames, int ch)
    {
        if (frames <= 0 || ch <= 0) return;
        std::lock_guard<std::mutex> lk(mtx);
        const size_t add = (size_t) frames * (size_t) ch;
        data.insert(data.end(), p, p + add);
        const size_t maxS = capFrames * (size_t) ch;
        if (data.size() > maxS)
            data.erase(data.begin(), data.begin() + (ptrdiff_t) (data.size() - maxS));
    }

    long long available(int ch) const
    {
        std::lock_guard<std::mutex> lk(mtx);
        return ch > 0 ? (long long) (data.size() / (size_t) ch) : 0LL;
    }

    int read(float* dest, int ch, long long maxFrames)
    {
        if (dest == nullptr || ch <= 0 || maxFrames <= 0) return 0;
        std::lock_guard<std::mutex> lk(mtx);
        const size_t have = data.size() / (size_t) ch;
        const size_t take = std::min(have, (size_t) maxFrames);
        if (take == 0) return 0;
        const size_t n = take * (size_t) ch;
        std::copy(data.begin(), data.begin() + n, dest);
        data.erase(data.begin(), data.begin() + n);
        return (int) take;
    }

    void clear() { std::lock_guard<std::mutex> lk(mtx); data.clear(); }
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
    std::vector<double> phase(d.channels, 0.0);
    std::vector<float> block((size_t) d.channels * 256);
    while (!g_stop.load())
    {
        for (int f = 0; f < 256; ++f)
            for (int c = 0; c < d.channels; ++c)
            {
                const double fr = d.freqs[(size_t) c];
                block[(size_t) f * (size_t) d.channels + (size_t) c] =
                    fr > 0.0 ? (float) std::sin(phase[(size_t) c]) : 0.0f;
                phase[(size_t) c] += 2.0 * M_PI * fr / d.rate;
            }
        g_ring.push(block.data(), 256, d.channels);
    }
}

const FakeDevice* find(const char* id)
{
    if (id == nullptr) return nullptr;
    for (const auto& d : g_devices)
        if (d.id == id) return &d;
    return nullptr;
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
    out->version = "1.0";
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
    g_ring.clear();
    g_stop.store(false);
    if (g_thread.joinable()) g_thread.join();
    g_thread = std::thread(genLoop, g_activeIndex);
    for (int i = 0; i < 100 && !g_running.load() && !g_stop.load(); ++i)
        std::this_thread::yield();
    // The generator never sets g_running itself - mark it here once the thread
    // is up (it produces immediately).
    g_running.store(1);
    return 0;
}

SC_REC_EXPORT int sc_rec_stop(void)
{
    if (!g_running.load() && !g_thread.joinable()) return 0;
    g_stop.store(true);
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
    const int ch = g_activeIndex >= 0 ? g_devices[(size_t) g_activeIndex].channels : 1;
    return g_ring.available(ch);
}

SC_REC_EXPORT int sc_rec_read(float* dest, int channels, long long maxFrames)
{
    if (g_activeIndex < 0 && channels <= 0) return 0;
    const int ch = g_activeIndex >= 0 ? g_devices[(size_t) g_activeIndex].channels : channels;
    return g_ring.read(dest, ch, maxFrames);
}

SC_REC_EXPORT const char* sc_rec_last_error(void)
{
    return g_lastError.c_str();
}

}   // extern "C"
