#pragma once
// SpectroCraft - recorder plugin host (loads DLLs implementing RecorderPlugin.h)
#include <JuceHeader.h>
#include "RecorderPlugin.h"
#include <functional>
#include <vector>

namespace sc {
namespace rec {

/** One capture device as reported by a plugin. */
struct DeviceInfo
{
    juce::String id;
    juce::String name;
    int minChannels = 1;
    int maxChannels = 2;
    double defaultRate = 48000.0;
    std::vector<double> rates;      // empty = any rate in [minRate, maxRate]
    double minRate = 0.0;
    double maxRate = 99999.0;
    std::vector<int> depths;        // e.g. {16}, {24}, {32}
};

/** A loaded recorder plugin DLL. */
struct Plugin
{
    juce::File file;
    juce::String id, name, version;
    void* module = nullptr;         // HMODULE (Windows) / handle

    std::function<int()> getApiVersion;
    std::function<int(sc_rec_plugin_info*)> getInfo;
    std::function<int()> countDevices;
    std::function<int(int, sc_rec_device_info*)> getDevice;
    std::function<int(const char*, int, double, int, sc_rec_negotiated*)> configure;
    std::function<int()> start;
    std::function<int()> stop;
    std::function<int()> isRunning;
    std::function<long long()> availableFrames;
    std::function<int(float*, int, long long)> read;
    std::function<const char*()> lastError;

    std::vector<DeviceInfo> devices;   // filled by scanPlugins()

    bool valid() const { return module != nullptr && getApiVersion != nullptr; }
};

/** Loads recorder plugins from the standard search directories and drives one
    of them (configure / start / stop / pull). Thread safety: all calls must be
    made from the message thread except read()/availableFrames(), which are
    safe to call from any thread. */
class RecorderHost
{
public:
    static RecorderHost& instance();

    /** Scans <exe dir>/recorders and %APPDATA%/SpectroCraft/recorders (plus an
        optional extra directory) for plugin DLLs. Idempotent per directory;
        call again after the user drops a new DLL in place. */
    void scanPlugins(const juce::File& extraDir = {});

    const std::vector<Plugin>& plugins() const { return plugins_; }
    Plugin* findPlugin(const juce::String& id);

    /** Selects device + format through the plugin. On success *actual holds
        what the hardware will really deliver (may be NULL). */
    bool configure(const juce::String& pluginId, const juce::String& deviceId,
                   int wantChannels, double wantRate, int wantBitDepth,
                   sc_rec_negotiated* actual, std::string* errOut);

    bool start(std::string* errOut);
    void stop();
    bool running() const;
    long long availableFrames() const;
    /** Pulls up to maxFrames frames (interleaved float32) into dest. */
    int read(float* dest, int channels, long long maxFrames);

private:
    RecorderHost() = default;
    ~RecorderHost();
    void unloadAll();
    bool loadOne(const juce::File& dll);

    std::vector<Plugin> plugins_;
    Plugin* active_ = nullptr;
    juce::String activeDeviceId_;
    mutable std::vector<juce::String> scannedDirs_;   // normalised full paths
};

}   // namespace rec
}   // namespace sc
