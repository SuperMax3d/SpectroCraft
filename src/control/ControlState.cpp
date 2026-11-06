// SpectroCraft - AI control: per-Application control state.
#include "control/ControlState.h"
#include "control/CommandRegistry.h"
#include "control/UiBridge.h"
#include "Application.h"

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
        if (inf.hasProperty ("params")) set (e, "params", inf.getProperty ("params", {}));
        if (inf.hasProperty ("doc"))
        {
            const int docId = (int) inf.getProperty ("doc", -1);
            set (e, "doc", docId);
            // A mutating job's resulting undo state (history.list joins on it).
            if ((bool) inf.getProperty ("mutating", false) && (bool) inf.getProperty ("ok", false))
                for (auto& d : app.docs)
                    if (d->id == docId
                        && (juce::int64) d->engine.contentVersion()
                               != (juce::int64) inf.getProperty ("contentVersionBefore", -1))
                    {
                        set (e, "undoDepth", (int) d->undo.undoSize());
                        if (d->undo.canUndo()) set (e, "description", d->undo.lastRecorded().description);
                    }
        }
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

juce::String startJob (Context& ctx, const juce::String& cmd, const juce::String& title,
                       bool mutating, JobManager::Work work)
{
    JobManager::StartInfo si;
    si.cmd = cmd;
    si.mutating = mutating;
    si.docId = ctx.doc != nullptr ? ctx.doc->id : -1;
    si.source = ctx.source;
    si.params = ctx.params;
    if (ctx.doc != nullptr) si.contentVersionBefore = (juce::int64) ctx.doc->engine.contentVersion();
    ControlState* st = &ctx.state;
    if (UiBridge* ui = ctx.ui)
    {
        void* h = ui->beginJobDialog (title, title + " (" + ctx.source + ") ...");
        si.externalCancel = ui->jobCancelFlag (h);
        si.uiProgress = [st, h] (double p, const juce::String& s) {
            if (st->ui != nullptr) st->ui->setJobProgress (h, p, s);
        };
        si.uiDone = [st, h] { if (st->ui != nullptr) st->ui->endJobDialog (h); };
    }
    return ctx.state.jobs.start (si, std::move (work));
}

} // namespace sc::control
