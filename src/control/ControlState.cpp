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
        if (inf.hasProperty ("doc")) set (e, "doc", (int) inf.getProperty ("doc", -1));
        // Metadata of the result worth keeping (never text, measures or table values, §5.3).
        const auto res = inf.getProperty ("result", {});
        for (const char* k : { "script", "scriptSha256", "interpreter", "exitCode", "timedOut", "audioChanged",
                               "markerCount", "markersAdded", "segmentCount", "deviceUsed", "modelId", "verified" })
            if (res.hasProperty (k)) set (e, k, res.getProperty (k, {}));
        if (inf.hasProperty ("error"))
            set (e, "error", inf.getProperty ("error", {}).getProperty ("code", {}));
        journal.append ("job", e);
        pollDocuments();   // the patches the job committed, as history entries
    };
}

void ControlState::noteClearReason (int docId, const juce::String& reason)
{
    clearReasons[docId] = reason;
}

static juce::var patchEntry (const FileDoc& d, const Patch& p, const juce::String& what)
{
    auto e = obj ({ { "what", what }, { "doc", d.id }, { "patchSeq", (juce::int64) p.seq },
                    { "ts", isoTime ((juce::int64) p.timestampMs) },
                    { "opId", p.opId }, { "source", p.source }, { "description", p.description },
                    { "undoDepth", (int) d.undo.undoSize() }, { "redoDepth", (int) d.undo.redoSize() } });
    if (isObj (p.params)) set (e, "params", p.params);
    return e;
}

void ControlState::pollDocuments()
{
    std::map<int, bool> alive;
    for (auto& dp : app.docs)
    {
        FileDoc& d = *dp;
        alive[d.id] = true;
        auto& s = seen[d.id];
        if (s.first)
        {
            s.first = false;
            s.clears = d.undo.clearCount();
            s.metaChanges = d.metaDirty.changeCount();
            s.rate = d.engine.sampleRate();
            // Patches already present (a document opened with history: none) start the count.
            s.nextPatchSeq = d.undo.peekNextSeq();
            journal.append ("doc", obj ({ { "what", "opened" }, { "doc", d.id }, { "name", d.name },
                                          { "path", d.file == juce::File() ? juce::var() : juce::var (d.file.getFullPathName()) } }));
            continue;
        }
        if (d.undo.clearCount() != s.clears)
        {
            s.clears = d.undo.clearCount();
            auto it = clearReasons.find (d.id);
            // Resample drops the history (patches address the old rate).
            const juce::String reason = it != clearReasons.end() ? it->second
                                      : d.engine.sampleRate() != s.rate ? juce::String ("resample")
                                                                        : juce::String ("unknown");
            if (it != clearReasons.end()) clearReasons.erase (it);
            journal.append ("history", obj ({ { "what", "cleared" }, { "doc", d.id }, { "reason", reason },
                                              { "undoDepth", (int) d.undo.undoSize() },
                                              { "redoDepth", (int) d.undo.redoSize() } }));
        }
        // New patches (seq is monotonic per document; they sit on the undo stack).
        std::vector<const Patch*> fresh;
        for (size_t i = 0; i < d.undo.undoSize(); ++i)
            if (d.undo.undoAt (i).seq >= s.nextPatchSeq) fresh.push_back (&d.undo.undoAt (i));
        for (size_t i = 0; i < d.undo.redoSize(); ++i)   // recorded then undone before we looked
            if (d.undo.redoAt (i).seq >= s.nextPatchSeq) fresh.push_back (&d.undo.redoAt (i));
        std::sort (fresh.begin(), fresh.end(), [] (const Patch* a, const Patch* b) { return a->seq < b->seq; });
        for (const Patch* p : fresh)
            journal.append ("history", patchEntry (d, *p, "recorded"));
        s.nextPatchSeq = d.undo.peekNextSeq();
        s.rate = d.engine.sampleRate();

        if (d.metaDirty.changeCount() != s.metaChanges)
        {
            s.metaChanges = d.metaDirty.changeCount();
            journal.append ("meta", obj ({ { "what", "changed" }, { "doc", d.id },
                                           { "markers", (int) d.markers.markers().size() },
                                           { "savedSelections", (int) d.savedSelections.size() },
                                           { "drawings", (int) d.drawings.size() },
                                           { "tables", (int) d.tables.size() },
                                           { "transcriptionSegments", (int) d.sctt.segments.size() },
                                           { "metaDirty", (bool) d.metaDirty } }));
        }
    }
    for (auto it = seen.begin(); it != seen.end();)
    {
        if (alive.count (it->first) == 0)
        {
            journal.append ("doc", obj ({ { "what", "closed" }, { "doc", it->first } }));
            it = seen.erase (it);
        }
        else ++it;
    }
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
