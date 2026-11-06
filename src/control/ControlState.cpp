// SpectroCraft - AI control: per-Application control state.
#include "control/ControlState.h"
#include "control/CommandRegistry.h"

namespace sc::control {

ControlState::ControlState (Application& a) : app (a)
{
    jobs.onEvent = [this] (const juce::String& name, const juce::var& data) { emit (name, data); };
    jobs.onFinished = [this] (const juce::String& id, const juce::var& inf) {
        auto e = obj ({ { "what", "finished" }, { "job", id },
                        { "cmd", inf.getProperty ("cmd", {}) },
                        { "ok", inf.getProperty ("ok", false) },
                        { "elapsedMs", inf.getProperty ("elapsedMs", 0) },
                        { "source", inf.getProperty ("source", {}) } });
        if (inf.hasProperty ("doc")) set (e, "doc", inf.getProperty ("doc", -1));
        if (inf.hasProperty ("error"))
            set (e, "error", inf.getProperty ("error", {}).getProperty ("code", {}));
        journal.append ("job", e);
    };
}

ControlState::~ControlState()
{
    shutdown();
}

void ControlState::shutdown()
{
    if (isShutDown) return;
    isShutDown = true;
    eventSink = nullptr;
    jobs.shutdown();
}

void ControlState::emit (const juce::String& name, const juce::var& data)
{
    if (eventSink)
        eventSink (name, data);
}

} // namespace sc::control
