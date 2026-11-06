#pragma once
// SpectroCraft - AI control: per-Application control state (journal, jobs,
// UI bridge, event sink). Owned by Application (Application::control()).
#include <JuceHeader.h>
#include "control/Journal.h"
#include "control/JobManager.h"
#include "UndoManager.h"   // sc::undoctx (operation context of recorded patches)
#include <functional>
#include <map>

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

    /** audio.cpp backend chosen with ai.setDevice (session only; the stem /
        super-resolution commands pass it as --backend). Empty = auto. */
    juce::String audiocppBackend;

    /** Stops jobs (called first by ~Application). */
    void shutdown();

    /** Journals what changed since the last call in every document (message
        thread): new undo patches (with their op/params/source), history
        clears (with the last reason noted), metadata changes (markers,
        selections...), documents opened/closed. Called by invoke(), job
        completions, history.list and the window's timer, so edits made with
        the mouse are journaled too. */
    void pollDocuments();
    /** Reason of the next history clear pollDocuments() will see. */
    void noteClearReason (int docId, const juce::String& reason);

private:
    bool isShutDown = false;
    struct DocSeen
    {
        juce::int64 nextPatchSeq = 1;
        juce::int64 clears = 0;
        uint64_t metaChanges = 0;
        double rate = 0.0;
        bool first = true;
    };
    std::map<int, DocSeen> seen;
    std::map<int, juce::String> clearReasons;
};

struct Context;

/** Message thread: edits made inside this scope (a job's commit) are
    attributed to `source` in the journal, like edits inside invoke(). */
struct ScopedInvokeSource
{
    ScopedInvokeSource (ControlState& s, const juce::String& source,
                        const juce::String& opId = {}, const juce::var& params = {})
        : st (s), prev (s.invokeSource), op (sc::undoctx::Op { opId, params, source })
    {
        ++st.invokeDepth;
        st.invokeSource = source;
    }
    ~ScopedInvokeSource() { st.invokeSource = prev; --st.invokeDepth; }
    ControlState& st;
    juce::String prev;
    sc::undoctx::Scoped op;   // patches recorded in this scope carry opId/params/source
};

/** Starts a job for the command in ctx (message thread). With a window, the
    job shows the app's non-blocking progress dialog (Cancel included), so
    the menus' own "busy" checks and quit refusal see it. Returns the job id. */
juce::String startJob (Context& ctx, const juce::String& cmd, const juce::String& title,
                       bool mutating, JobManager::Work work);

} // namespace sc::control
