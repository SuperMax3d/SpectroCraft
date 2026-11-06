// SpectroCraft - AI control: application / document / history state as JSON.
#include "control/StateJson.h"
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "Application.h"
#include "AudioCpp.h"
#include "WorkProgress.h"
#include "Version.h"

#if JUCE_WINDOWS
 #include <process.h>   // _getpid
#else
 #include <unistd.h>    // getpid
#endif

namespace sc::control {

juce::String appVersion()
{
    return SC_VERSION_STRING;
}

juce::String platformName()
{
#if JUCE_WINDOWS
    return "windows";
#elif JUCE_MAC
    return "macos";
#elif JUCE_LINUX
    return "linux";
#else
    return "other";
#endif
}

int processId()
{
#if JUCE_WINDOWS
    return (int) _getpid();
#else
    return (int) getpid();
#endif
}

juce::var helloInfo (Application& app, bool hasUi)
{
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    const bool ai = true;
#else
    const bool ai = false;
#endif
    // A CLI build exists for this platform (Windows x64, macOS, Linux x64).
    const bool audiocpp = sc::acpp::platformSupported();
    return obj ({ { "app", "SpectroCraft" }, { "version", appVersion() }, { "pid", processId() },
                  { "protocol", kProtocolVersion }, { "platform", platformName() },
                  { "capabilities", obj ({ { "ai", ai }, { "audiocpp", audiocpp },
                                           { "python", true }, { "ui", hasUi } }) },
                  { "commands", (int) CommandRegistry::instance().all().size() },
                  { "journalSeq", app.control().journal.lastSeq() } });
}

juce::String metaModeName (const FileDoc& d)
{
    return d.metaMode == sc::MetaMode::External ? "external" : "embedded";
}

static juce::var channelList (const std::vector<int>& v)
{
    juce::Array<juce::var> a;
    for (int c : v) a.add (c);
    return a;
}

juce::var docSummary (Application& app, const FileDoc& d)
{
    const auto& e = d.engine;
    return obj ({ { "id", d.id },
                  { "path", d.file == juce::File() ? juce::var() : juce::var (d.file.getFullPathName()) },
                  { "name", d.name },
                  { "durationSec", e.duration() },
                  { "samples", (juce::int64) e.numSamples() },
                  { "channels", e.numChannels() },
                  { "sampleRate", e.sampleRate() },
                  { "unsaved", app.docIsUnsaved (d) },
                  { "audioUnsaved", e.hasUnsavedChanges() },
                  { "fullyInMemory", e.fullyInMemory() },
                  { "undoDepth", (int) d.undo.undoSize() },
                  { "redoDepth", (int) d.undo.redoSize() },
                  { "contentVersion", (juce::int64) e.contentVersion() },
                  { "metaMode", metaModeName (d) },
                  { "markers", (int) d.markers.markers().size() },
                  { "hasTranscription", !d.sctt.empty() } });
}

juce::var appState (Application& app)
{
    juce::Array<juce::var> docs;
    for (auto& d : app.docs) docs.add (docSummary (app, *d));
    FileDoc* active = app.doc();
    auto& eng = app.audioEngine();

    auto st = obj ({ { "docs", docs },
                     { "activeDoc", active != nullptr ? juce::var (active->id) : juce::var() } });
    set (st, "selection", obj ({ { "start", app.selection.getStart() },
                                 { "end", app.selection.getEnd() },
                                 { "hasSpectral", app.hasSpectralSelection() } }));
    set (st, "playhead", app.playhead);
    set (st, "visibleChannels", channelList (app.visibleChannels));
    set (st, "view", obj ({ { "t0", app.viewStart }, { "t1", app.viewEnd } }));
    set (st, "transport", obj ({ { "playing", eng.isPlaying() }, { "paused", eng.isPaused() },
                                 { "looping", eng.isLooping() }, { "position", eng.position() },
                                 { "audioReady", app.audioDeviceReady() } }));
    set (st, "recording", obj ({ { "active", app.recording.active } }));
    set (st, "ai", obj ({ { "device", juce::String (app.aiDevicePref) },
                          { "lastNsModel", juce::String (app.lastNsModelId) },
                          { "lastTranscribeModel", juce::String (app.lastTranscribeModelId) },
                          { "autoDownload", app.aiAutoDownload },
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
                          { "available", true }
#else
                          { "available", false }
#endif
                        }));
    const bool modal = juce::Component::getCurrentlyModalComponent() != nullptr;
    auto& jobs = app.control().jobs;
    set (st, "busy", modal || sc::WorkProgressDialog::numActive() > 0
                         || jobs.runningMutating().isNotEmpty());
    set (st, "modalActive", modal);
    set (st, "jobs", jobs.list (false));
    set (st, "actionRecording", obj ({ { "active", app.actionRec.active },
                                       { "action", app.actions.validIndex (app.actionRec.actionIndex)
                                                       ? juce::var (app.actions.get (app.actionRec.actionIndex)->name)
                                                       : juce::var() } }));
    set (st, "journalSeq", app.control().journal.lastSeq());
    return st;
}

// Best-effort join of an undo state to the journal (until lot 5 stores the
// operation in the Patch itself): an API command journals the undo depth it
// produced and the patch description; a menu/dialog edit journals its step
// with the depth before the edit.
static juce::var findOp (const std::vector<juce::var>& entries, int docId, int state,
                         const juce::String& description)
{
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
    {
        const auto& e = *it;
        if ((int) e.getProperty ("doc", -1) != docId) continue;
        const auto kind = e.getProperty ("kind", {}).toString();
        if ((kind == "command" || kind == "job") && (bool) e.getProperty ("ok", false)
            && (int) e.getProperty ("undoDepth", -1) == state
            && e.getProperty ("description", {}).toString() == description)
            return e;
        if (kind == "step" && !(bool) e.getProperty ("viaCommand", false)
            && (int) e.getProperty ("undoDepthBefore", -2) == state - 1)
            return e;
    }
    return {};
}

static juce::var stateOf (const Patch& p, int state, double rate, bool future,
                          const std::vector<juce::var>& journal, int docId)
{
    auto s = obj ({ { "state", state },
                    { "description", p.description.isEmpty() ? juce::String ("Edit") : p.description } });
    if (rate > 0)
        set (s, "region", obj ({ { "startSec", (double) p.oldStart / rate },
                                 { "endSec", (double) p.oldEnd / rate } }));
    set (s, "lengthDelta", (juce::int64) p.lengthDelta);
    set (s, "channelsChanged", p.oldChans != p.newChans);
    set (s, "rateChanged", p.oldRate != p.newRate);
    set (s, "sideState", p.hasSideState || p.hasTimeAnchored);
    if (p.hasSideState)
    {
        set (s, "markersBefore", (int) p.oldMarkers.size());
        set (s, "markersAfter", (int) p.newMarkers.size());
    }
    juce::int64 bytes = 0;
    for (const auto& c : p.oldData) bytes += (juce::int64) c.size() * (juce::int64) sizeof (float);
    for (const auto& c : p.newData) bytes += (juce::int64) c.size() * (juce::int64) sizeof (float);
    set (s, "dataBytes", bytes);
    set (s, "future", future);
    // Provenance stored in the patch itself (UndoManager::record, lot 5).
    if (p.seq > 0)
    {
        set (s, "patchSeq", (juce::int64) p.seq);
        set (s, "ts", isoTime ((juce::int64) p.timestampMs));
        set (s, "opId", p.opId);
        set (s, "source", p.source);
        if (isObj (p.params)) set (s, "params", p.params);
    }
    else if (!future)
    {
        const auto op = findOp (journal, docId, state, p.description);
        if (isObj (op))
        {
            const auto k = op.getProperty ("kind", {}).toString();
            const bool isCmd = k == "command" || k == "job";
            set (s, "opId", isCmd ? op.getProperty ("cmd", {}) : op.getProperty ("what", {}));
            set (s, "params", op.getProperty ("params", {}));
            set (s, "source", op.getProperty ("source", {}));
            set (s, "ts", op.getProperty ("ts", {}));
            set (s, "journalSeq", op.getProperty ("seq", {}));
        }
    }
    return s;
}

juce::var historyList (Application& app, FileDoc& d, bool includeJournal, juce::int64 sinceSeq)
{
    auto& u = d.undo;
    auto& journal = app.control().journal;
    const auto entries = journal.entriesSince (0);
    const double rate = d.engine.sampleRate();

    juce::Array<juce::var> states;
    states.add (obj ({ { "state", 0 }, { "label", "Loaded " + d.name }, { "description", "Loaded " + d.name },
                       { "future", false } }));
    for (size_t i = 0; i < u.undoSize(); ++i)
        states.add (stateOf (u.undoAt (i), (int) i + 1, rate, false, entries, d.id));
    // The redo stack's back() is the NEXT redo: list the future in redo order.
    const int cur = (int) u.undoSize();
    for (size_t k = 0; k < u.redoSize(); ++k)
        states.add (stateOf (u.redoAt (u.redoSize() - 1 - k), cur + 1 + (int) k, rate, true, entries, d.id));

    auto r = obj ({ { "doc", d.id }, { "docId", d.id }, { "currentState", cur },
                    { "maxLevels", (int) ::UndoManager::kMaxLevels }, { "states", states } });
    if (includeJournal)
        set (r, "journal", journal.read (sinceSeq, {}, 500, d.id).getProperty ("entries", {}));

    juce::Array<juce::var> recorded;
    for (int i = 0; i < app.actions.size(); ++i)
        recorded.add (app.actions.get (i)->toJson());
    set (r, "recordedActions", recorded);
    return r;
}

} // namespace sc::control
