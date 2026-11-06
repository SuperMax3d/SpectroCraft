#pragma once
// SpectroCraft - AI control: per-Application control state (journal, jobs,
// UI bridge, event sink). Owned by Application (Application::control()).
#include <JuceHeader.h>
#include "control/Journal.h"
#include "control/JobManager.h"
#include <functional>

class Application;

namespace sc::control {

class UiBridge;

class ControlState
{
public:
    explicit ControlState (Application& app);
    ~ControlState();

    Application& app;
    Journal journal;
    JobManager jobs;
    UiBridge* ui = nullptr;                 // set by MainWindow (nullptr headless)

    /** Event fan-out (set by the control server). Message thread. */
    std::function<void (const juce::String& name, const juce::var& data)> eventSink;
    void emit (const juce::String& name, const juce::var& data);

    /** Source label of edits made while a command runs ("api:<client>",
        "cli"...); "menu" otherwise. Read by Application::recordActionStep. */
    juce::String currentSource() const { return invokeDepth > 0 ? invokeSource : juce::String ("menu"); }
    int invokeDepth = 0;
    juce::String invokeSource;

    /** Stops jobs (called first by ~Application). */
    void shutdown();

private:
    bool isShutDown = false;
};

} // namespace sc::control
