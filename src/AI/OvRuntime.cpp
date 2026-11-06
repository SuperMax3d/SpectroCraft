// SpectroCraft — OpenVINO runtime wrapper (Phase 0 of the AI plan).
// See include/AI/OvRuntime.h for the design notes.

#include "AI/OvRuntime.h"

#include <juce_core/juce_core.h>
#include <cctype>
#include <cstdio>
#include <mutex>
#if defined(_WIN32)
    // JUCE's headers do not pull in windows.h on their own; keep NOMINMAX set
    // so the min/max macros never leak into later C++ headers (openvino.hpp is
    // already parsed above, but this makes it include-order independent).
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #undef max
    #undef min
#endif

namespace sc {

namespace {
bool hasDevice(const std::vector<std::string>& v, const char* name)
{
    for (const auto& d : v) if (d == name) return true;
    return false;
}
} // namespace

std::string resolveAiDevice(const std::string& prefIn,
                            const std::vector<std::string>& available,
                            std::string* noteOut)
{
    if (noteOut != nullptr) noteOut->clear ();

    std::string pref = prefIn;
    for (auto& c : pref) c = (char) std::toupper ((unsigned char) c);

    // "auto" (or empty/unknown): prefer GPU over NPU over CPU. The runtime
    // always lists "CPU" when it initialised; an empty list means init failed,
    // in which case the pipelines produce their usual clear errors.
    if (pref.empty () || pref == "AUTO")
        return hasDevice (available, "GPU") ? "GPU"
               : hasDevice (available, "NPU") ? "NPU" : "CPU";

    // A specific device: use it when the runtime exposes it, else fall back to CPU.
    if (hasDevice (available, pref.c_str ())) return pref;

    if (noteOut != nullptr)
        *noteOut = pref + " is not available - using CPU instead.";
    return "CPU";
}

namespace ai {

OvRuntime& OvRuntime::instance()
{
#if defined(_WIN32)
    static OvRuntime rt;
    return rt;
#else
    // macOS / Linux: never destroyed. Run at exit, ~ov::Core unloads the CPU
    // plugin after the plugin's own static objects are gone (libc++ aborts
    // with "mutex lock failed" in the plugin). The OS reclaims everything.
    static OvRuntime* rt = new OvRuntime ();
    return *rt;
#endif
}

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI

static juce::File cacheDirPath()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                  .getChildFile ("SpectroCraft")
                  .getChildFile ("openvino-cache");
    // SC_OPENVINO_CACHE_DIR=<absolute folder> replaces it on every platform
    // (the macOS / Linux unit tests set it; see SC_MODELS_DIR in ModelManager.cpp).
    {
        const juce::String env = juce::SystemStats::getEnvironmentVariable ("SC_OPENVINO_CACHE_DIR", {}).trim ();
        if (env.isNotEmpty () && juce::File::isAbsolutePath (env))
            dir = juce::File (env);
    }
#if SC_TESTS_EXE
    // Keep unit tests away from the user's real cache.
    dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
              .getChildFile ("SpectroCraftTests-openvino-cache");
#endif
    if (!dir.exists()) dir.createDirectory ();
    return dir;
}

bool OvRuntime::init(std::string* errOut)
{
    if (_ready) return true;

    // Called from the noise-suppression and transcription workers and the
    // CLI: serialise so two threads never create / replace _core together.
    static std::mutex initMutex;
    std::lock_guard<std::mutex> lock (initMutex);
    if (_ready) return true;

#if defined(_WIN32)
    // openvino.dll is delay-loaded (/DELAYLOAD), so the process starts even
    // when the runtime DLLs are missing. Probe it here for a precise error:
    // LoadLibrary resolves openvino.dll's static import (tbb12.dll) as well,
    // but the IR frontend and CPU plugin DLLs load dynamically later - their
    // absence surfaces at read_model/compile time instead (sc::buildNsFailureDetails
    // classifies those errors so users still get the four-DLL instructions).
    // The runtime DLLs live in the SpectroCraft subfolder next to the exe, which
    // sc::setupRuntimeDllSearchPath() adds to the search path at startup; the
    // exe directory is searched first, so old flat layouts keep working.
    // On success the DLL stays loaded and the delayed import stubs find it
    // already present.
    if (!LoadLibraryA ("openvino.dll"))
    {
        char msg [300];
        std::snprintf (msg, sizeof msg,
                       "OpenVINO runtime could not be loaded (Windows error %lu).\n"
                       "The OpenVINO DLLs are missing — they should be in the SpectroCraft folder next to SpectroCraft.exe.",
                       (unsigned) GetLastError ());
        if (errOut) *errOut = msg;
        return false;
    }
#endif
    // macOS / Linux: libopenvino is linked normally (no delay-load); the build
    // ships it with the CPU plugin, the IR frontend and oneTBB in the app
    // (Contents/Frameworks, or next to the executable on Linux). OpenVINO
    // finds the plugin and the frontend in the folder it was loaded from.

    try
    {
        _core = std::make_shared<ov::Core> ();
        _core->set_property (ov::cache_dir (cacheDirPath ().getFullPathName ().toStdString ()));
        _ready = true;
        return true;
    }
    catch (const std::exception& e)
    {
        if (errOut) *errOut = e.what ();
        return false;
    }
#if defined(_WIN32) && defined(_MSC_VER)
    // Backstop: this translation unit is compiled with /EHa, so catch(...) also
    // intercepts the delay-load failure SEH code (0xC06D007E) should it ever
    // reach here despite the LoadLibrary probe above.
    catch (...)
    {
        if (errOut) *errOut = "OpenVINO runtime failed to initialise.";
        return false;
    }
#endif
}

std::vector<std::string> OvRuntime::devices() const
{
    std::vector<std::string> out;
    if (!_core) return out;
    try { out = _core->get_available_devices (); }
    catch (const std::exception&) {}
    return out;
}

ov::CompiledModel OvRuntime::compile(const std::string& xmlPath,
                                     const std::string& device,
                                     std::string* errOut)
{
    if (!_core)
    {
        if (errOut) *errOut = "OpenVINO runtime not initialised.";
        return {};
    }
    try
    {
        auto model = _core->read_model (xmlPath);
        return _core->compile_model (model, device, {});
    }
    catch (const std::exception& e)
    {
        if (errOut) *errOut = e.what ();
        return {};
    }
}

#else   // AI disabled at build time

bool OvRuntime::init(std::string* errOut)
{
    if (errOut) *errOut = "This build was configured with -DSPECTRO_ENABLE_AI=OFF.";
    return false;
}

#endif

}} // namespace sc::ai
