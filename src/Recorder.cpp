// SpectroCraft - recorder plugin host implementation
#include "Recorder.h"

#include <algorithm>
#include <cstddef>

#if defined (JUCE_WINDOWS)
    #define NOMINMAX
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace sc {
namespace rec {

using FnApiVersion   = int (*)(void);
using FnGetInfo      = int (*)(sc_rec_plugin_info*);
using FnCountDevices = int (*)(void);
using FnGetDevice    = int (*)(int, sc_rec_device_info*);
using FnConfigure    = int (*)(const char*, int, double, int, sc_rec_negotiated*);
using FnStart        = int (*)(void);
using FnStop         = int (*)(void);
using FnIsRunning    = int (*)(void);
using FnAvailable    = long long (*)(void);
using FnRead         = int (*)(float*, int, long long);
using FnLastError    = const char* (*)(void);

static juce::String fromUtf8Ptr(const char* s)
{
    return s != nullptr ? juce::String::fromUTF8 (s) : juce::String ();
}

static void refreshDevices(Plugin& p);

RecorderHost& RecorderHost::instance()
{
    static RecorderHost host;
    return host;
}

RecorderHost::~RecorderHost()
{
    unloadAll();
}

void RecorderHost::unloadAll()
{
    if (active_ != nullptr)
        active_->stop ();
    for (auto& p : plugins_)
    {
#if defined (JUCE_WINDOWS)
        if (p.module != nullptr)
            FreeLibrary ((HMODULE) p.module);
#else
        if (p.module != nullptr)
            dlclose (p.module);
#endif
        p.module = nullptr;
    }
    plugins_.clear ();
    active_ = nullptr;
}

juce::String RecorderHost::pluginFilePattern()
{
    // A plugin is a shared library in the platform's native format. (The scan
    // used to look for *.dll everywhere, so a .dylib / .so plugin was never
    // found on macOS / Linux.)
#if defined (JUCE_WINDOWS)
    return "*.dll";
#elif defined (JUCE_MAC)
    return "*.dylib";
#else
    return "*.so";
#endif
}

void RecorderHost::scanPlugins(const juce::File& extraDir)
{
    std::vector<juce::File> dirs;
    dirs.push_back (juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                        .getParentDirectory ()
                        .getChildFile ("recorders"));
    dirs.push_back (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                        .getChildFile ("SpectroCraft")
                        .getChildFile ("recorders"));
    if (extraDir != juce::File ())
        dirs.push_back (extraDir);

    // Every call lists the directories again, so a plugin dropped in after the
    // first scan is found by the next one (the menu and startRecording() rely
    // on that). Already loaded files are skipped, and so are files that were
    // rejected before, until they change on disk.
    std::vector<juce::String> seenDirs;   // de-duplicates within this call only
    for (const auto& d : dirs)
    {
        const juce::String key = d.getFullPathName ().toLowerCase ();
        if (std::find (seenDirs.begin (), seenDirs.end (), key) != seenDirs.end ()
                || !d.isDirectory ())
            continue;
        seenDirs.push_back (key);

        for (const auto& entry : juce::RangedDirectoryIterator (d, false, pluginFilePattern ()))
        {
            const juce::File f = entry.getFile ();
            bool have = false;
            for (const auto& p : plugins_)
                if (p.file == f) { have = true; break; }
            if (have)
                continue;

            const juce::String path = f.getFullPathName ();
            const juce::int64 stamp = f.getLastModificationTime ().toMilliseconds ();
            const auto rej = std::find_if (rejected_.begin (), rejected_.end (),
                                           [&path] (const auto& r) { return r.first == path; });
            if (rej != rejected_.end () && rej->second == stamp)
                continue;   // same file failed to load before

            if (loadOne (f))
            {
                if (rej != rejected_.end ())
                    rejected_.erase (rej);
            }
            else if (rej != rejected_.end ())
                rej->second = stamp;
            else
                rejected_.emplace_back (path, stamp);
        }
    }

    // Refresh the device lists of everything already loaded so newly added or
    // removed hardware shows up without a restart.
    for (auto& p : plugins_)
        refreshDevices (p);
}

bool RecorderHost::loadOne(const juce::File& dll)
{
#if defined (JUCE_WINDOWS)
    void* mod = LoadLibraryW (dll.getFullPathName ().toWideCharPointer ());
#else
    void* mod = dlopen (dll.getFullPathName ().toStdString ().c_str (), RTLD_NOW);
#endif
    if (mod == nullptr)
        return false;

    Plugin p;
    p.file = dll;
#if defined (JUCE_WINDOWS)
    auto sym = [&](const char* name, void** out) {
        *out = GetProcAddress ((HMODULE) mod, name);
        return *out != nullptr;
    };
#else
    auto sym = [&](const char* name, void** out) {
        *out = dlsym (mod, name);
        return *out != nullptr;
    };
#endif

    auto resolveAll = [&]() -> bool {
        void* s = nullptr;
        if (!sym ("sc_rec_get_api_version", &s))   return false; p.getApiVersion = (FnApiVersion) s;
        if (!sym ("sc_rec_get_plugin_info", &s))   return false; p.getInfo = (FnGetInfo) s;
        if (!sym ("sc_rec_count_devices", &s))     return false; p.countDevices = (FnCountDevices) s;
        if (!sym ("sc_rec_get_device", &s))        return false; p.getDevice = (FnGetDevice) s;
        if (!sym ("sc_rec_configure", &s))         return false; p.configure = (FnConfigure) s;
        if (!sym ("sc_rec_start", &s))             return false; p.start = (FnStart) s;
        if (!sym ("sc_rec_stop", &s))              return false; p.stop = (FnStop) s;
        if (!sym ("sc_rec_is_running", &s))        return false; p.isRunning = (FnIsRunning) s;
        if (!sym ("sc_rec_available_frames", &s))  return false; p.availableFrames = (FnAvailable) s;
        if (!sym ("sc_rec_read", &s))              return false; p.read = (FnRead) s;
        if (!sym ("sc_rec_last_error", &s))        return false; p.lastError = (FnLastError) s;
        return true;
    };

    bool ok = resolveAll () && p.getApiVersion () == SC_RECORDER_API_VERSION;
    if (ok)
    {
        sc_rec_plugin_info info {};
        if (p.getInfo (&info) == 0)
        {
            p.id = fromUtf8Ptr (info.id);
            p.name = fromUtf8Ptr (info.name);
            p.version = fromUtf8Ptr (info.version);
        }
        ok = !p.id.isEmpty ();
    }

    if (!ok)
    {
#if defined (JUCE_WINDOWS)
        FreeLibrary ((HMODULE) mod);
#else
        dlclose (mod);
#endif
        return false;
    }

    p.module = mod;
    refreshDevices (p);
    // active_ points into plugins_: re-point it if push_back reallocates.
    // (All callers run on the message thread, which also pulls the frames.)
    const std::ptrdiff_t activeIdx = active_ != nullptr ? active_ - plugins_.data () : -1;
    plugins_.push_back (std::move (p));
    if (activeIdx >= 0)
        active_ = plugins_.data () + activeIdx;
    return true;
}

void refreshDevices(Plugin& p)
{
    p.devices.clear ();
    if (!p.valid () || !p.countDevices || !p.getDevice)
        return;
    const int n = p.countDevices ();
    for (int i = 0; i < n; ++i)
    {
        sc_rec_device_info di {};
        if (p.getDevice (i, &di) != 0)
            continue;
        DeviceInfo d;
        // Copy everything now: the plugin's pointers may be invalidated by the
        // next call into it.
        d.id = fromUtf8Ptr (di.id);
        d.name = fromUtf8Ptr (di.name);
        d.minChannels = std::max (1, di.minChannels);
        d.maxChannels = std::max (d.minChannels, di.maxChannels);
        d.defaultRate = di.defaultRate;
        if (di.numRates > 0 && di.rates != nullptr)
            for (int r = 0; r < di.numRates; ++r)
                d.rates.push_back (di.rates[r]);
        d.minRate = di.minRate;
        d.maxRate = di.maxRate;
        if (di.numDepths > 0 && di.depths != nullptr)
            for (int r = 0; r < di.numDepths; ++r)
                d.depths.push_back (di.depths[r]);
        p.devices.push_back (std::move (d));
    }
}

Plugin* RecorderHost::findPlugin(const juce::String& id)
{
    for (auto& p : plugins_)
        if (p.id == id && p.valid ())
            return &p;
    return nullptr;
}

bool RecorderHost::configure(const juce::String& pluginId, const juce::String& deviceId,
                             int wantChannels, double wantRate, int wantBitDepth,
                             sc_rec_negotiated* actual, std::string* errOut)
{
    auto* p = findPlugin (pluginId);
    if (p == nullptr || !p->configure)
    {
        if (errOut != nullptr) *errOut = "Recorder plugin not found.";
        return false;
    }

    const std::string idS = deviceId.toStdString ();
    sc_rec_negotiated neg {};
    const int r = p->configure (idS.c_str (), wantChannels, wantRate, wantBitDepth, &neg);
    if (r != 0)
    {
        if (errOut != nullptr)
            *errOut = fromUtf8Ptr (p->lastError ()).toStdString ();
        return false;
    }

    active_ = p;
    activeDeviceId_ = deviceId;
    if (actual != nullptr)
        *actual = neg;
    return true;
}

bool RecorderHost::start(std::string* errOut)
{
    if (active_ == nullptr || !active_->start)
    {
        if (errOut != nullptr) *errOut = "No recording device selected.";
        return false;
    }
    const int r = active_->start ();
    if (r != 0)
    {
        if (errOut != nullptr)
            *errOut = fromUtf8Ptr (active_->lastError ()).toStdString ();
        return false;
    }
    return true;
}

void RecorderHost::stop()
{
    if (active_ != nullptr && active_->stop)
        active_->stop ();
}

bool RecorderHost::running() const
{
    return active_ != nullptr && active_->isRunning != nullptr && active_->isRunning () != 0;
}

juce::String RecorderHost::lastError() const
{
    if (active_ == nullptr || !active_->lastError)
        return juce::String ();
    return fromUtf8Ptr (active_->lastError ());
}

long long RecorderHost::availableFrames() const
{
    if (active_ == nullptr || !active_->availableFrames)
        return 0LL;
    return active_->availableFrames ();
}

int RecorderHost::read(float* dest, int channels, long long maxFrames)
{
    if (active_ == nullptr || !active_->read)
        return 0;
    return active_->read (dest, channels, maxFrames);
}

}   // namespace rec
}   // namespace sc
