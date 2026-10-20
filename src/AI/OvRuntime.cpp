// SpectroCraft — OpenVINO runtime wrapper (Phase 0 of the AI plan).
// See include/AI/OvRuntime.h for the design notes.

#include "AI/OvRuntime.h"

#include <juce_core/juce_core.h>
#include <cstdio>
#if defined(_WIN32)
    #include <windows.h>   // LoadLibraryA / GetLastError (JUCE has already configured the Windows headers)
#endif

namespace sc { namespace ai {

OvRuntime& OvRuntime::instance()
{
    static OvRuntime rt;
    return rt;
}

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI

static juce::File cacheDirPath()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                  .getChildFile ("SpectroCraft")
                  .getChildFile ("openvino-cache");
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

#if defined(_WIN32)
    // openvino.dll is delay-loaded (/DELAYLOAD), so the process starts even
    // when the runtime DLLs are missing. Probe it here for a precise error:
    // LoadLibrary resolves openvino.dll's static import (tbb12.dll) as well,
    // but the IR frontend and CPU plugin DLLs load dynamically later - their
    // absence surfaces at read_model/compile time instead (sc::buildNsFailureDetails
    // classifies those errors so users still get the four-DLL instructions).
    // On success the DLL stays loaded and the delayed import stubs find it
    // already present.
    if (!LoadLibraryA ("openvino.dll"))
    {
        char msg [300];
        std::snprintf (msg, sizeof msg,
                       "OpenVINO runtime could not be loaded (Windows error %lu).\n"
                       "The OpenVINO DLLs are missing — install them next to SpectroCraft.exe.",
                       (unsigned) GetLastError ());
        if (errOut) *errOut = msg;
        return false;
    }
#endif

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
