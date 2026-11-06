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

struct Context;

/** Message thread: edits made inside this scope (a job's commit) are
    attributed to `source` in the journal, like edits inside invoke(). */
struct ScopedInvokeSource
{
    ScopedInvokeSource (ControlState& s, const juce::String& source) : st (s), prev (s.invokeSource)
    {
        ++st.invokeDepth;
        st.invokeSource = source;
    }
    ~ScopedInvokeSource() { st.invokeSource = prev; --st.invokeDepth; }
    ControlState& st;
    juce::String prev;
};

/** Starts a job for the command in ctx (message thread). With a window, the
    job shows the app's non-blocking progress dialog (Cancel included), so
    the menus' own "busy" checks and quit refusal see it. Returns the job id. */
juce::String startJob (Context& ctx, const juce::String& cmd, const juce::String& title,
                       bool mutating, JobManager::Work work);

} // namespace sc::control
