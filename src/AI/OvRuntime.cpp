// SpectroCraft — OpenVINO runtime wrapper (Phase 0 of the AI plan).
// See include/AI/OvRuntime.h for the design notes.

#include "AI/OvRuntime.h"

#include <juce_core/juce_core.h>

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
