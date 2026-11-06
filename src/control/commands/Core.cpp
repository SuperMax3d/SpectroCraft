// SpectroCraft - AI control: core commands (app.*, doc.*, selection.*, journal.*, debug.*).
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "control/StateJson.h"
#include "control/UiBridge.h"
#include "control/PathPolicy.h"
#include "Application.h"

namespace sc::control {

namespace {

/** Same threshold as MainWindow::openFile's async path. */
constexpr juce::int64 kAsyncOpenBytes = (juce::int64) 200 * 1024 * 1024;

int indexOf (Application& app, const FileDoc* d)
{
    for (int i = 0; i < (int) app.docs.size(); ++i)
        if (app.docs[(size_t) i].get() == d) return i;
    return -1;
}

Outcome invalid (const juce::String& path, const juce::String& expected, const juce::String& msg)
{
    return Outcome::failure (err::invalidParams, msg, obj ({ { "path", path }, { "expected", expected } }));
}

/** Absolute path param -> File, or an invalid_params outcome. */
bool absolutePath (Context& ctx, const char* key, juce::File& out, Outcome& error)
{
    const juce::String p = ctx.str (key);
    if (p.isEmpty() || !juce::File::isAbsolutePath (p))
    {
        error = invalid (key, "absolute path", juce::String (key) + " must be an absolute path");
        return false;
    }
    out = juce::File (p);
    return true;
}

Outcome describeCmd (Context& ctx)
{
    return Outcome::success (CommandRegistry::instance().describe (
        ctx.str ("category"), ctx.flag ("withSchemas", true), ctx.flag ("includeInternal", false)));
}

Outcome stateCmd (Context& ctx)
{
    return Outcome::success (appState (ctx.app));
}

Outcome docListCmd (Context& ctx)
{
    juce::Array<juce::var> docs;
    for (auto& d : ctx.app.docs) docs.add (docSummary (ctx.app, *d));
    FileDoc* a = ctx.app.doc();
    return Outcome::success (obj ({ { "active", a != nullptr ? juce::var (a->id) : juce::var() },
                                    { "docs", docs } }));
}

Outcome docOpenCmd (Context& ctx)
{
    juce::File f;
    Outcome bad;
    if (!absolutePath (ctx, "path", f, bad)) return bad;
    if (!f.existsAsFile())
        return Outcome::failure (err::failed, "file not found: " + f.getFullPathName(),
                                 obj ({ { "path", f.getFullPathName() } }));

    {
        const auto chk = checkPath (ctx.app, f, false, ctx.confirmed());
        if (!chk.ok) return chk;
    }

    Application& app = ctx.app;
    FileDoc* prev = app.doc();
    const bool activate = ctx.flag ("activate", true);

    // Large files (same threshold as MainWindow::openFile): load on a worker.
    bool alreadyOpen = false;
    for (auto& d : app.docs) if (d->file == f) alreadyOpen = true;
    if (!alreadyOpen && f.getSize() > kAsyncOpenBytes)
    {
        auto holder = std::make_shared<std::unique_ptr<FileDoc>> (std::make_unique<FileDoc>());
        Application* appP = &app;
        const auto id = startJob (ctx, "doc.open", "Opening " + f.getFileName(), true,
            [appP, holder, f, activate, prev] (JobControl& jc) -> Outcome {
                if (!(*holder)->load (f, [&jc] (float p) { jc.progress (p, "loading"); }, jc.cancelFlag()))
                    return Outcome::failure (jc.cancelled() ? err::cancelled : err::failed,
                                             "could not open " + f.getFullPathName());
                Outcome o;
                if (!jc.onMessageThread ([&] {
                        auto& a = *appP;
                        FileDoc* raw = holder->get();
                        a.inheritSessionMeta (*raw);
                        a.docs.push_back (std::move (*holder));
                        a.addRecentFile (f);
                        if (activate || a.doc() == nullptr) a.activate ((int) a.docs.size() - 1);
                        else if (prev != nullptr && a.isDocLoaded (prev) && a.onStateChange) a.onStateChange();
                        auto r = docSummary (a, *raw);
                        set (r, "doc", raw->id);
                        set (r, "active", a.doc() == raw);
                        o = Outcome::success (r);
                    }))
                    return Outcome::failure (err::shuttingDown, "the application is shutting down");
                return o;
            });
        return Outcome::job (id);
    }

    if (!app.openFile (f))
        return Outcome::failure (err::failed, "could not open " + f.getFullPathName()
                                     + " (unsupported format or unreadable file)",
                                 obj ({ { "path", f.getFullPathName() } }));
    FileDoc* d = app.doc();
    if (d == nullptr)
        return Outcome::failure (err::failed, "the document did not open");
    if (!activate && prev != nullptr && prev != d)
    {
        const int i = indexOf (app, prev);
        if (i >= 0) app.activate (i);
    }
    auto r = docSummary (app, *d);
    set (r, "doc", d->id);
    set (r, "active", app.doc() == d);
    return Outcome::success (r);
}

Outcome docActivateCmd (Context& ctx)
{
    const int i = indexOf (ctx.app, ctx.doc);
    if (i < 0) return Outcome::failure (err::noDocument, "no document to activate");
    ctx.app.activate (i);
    return Outcome::success (obj ({ { "active", ctx.doc->id } }));
}

Outcome docMetaCmd (Context& ctx)
{
    FileDoc& d = *ctx.doc;
    juce::var bundle = juce::JSON::parse (juce::String::fromUTF8 (d.buildMetaJson().c_str()));
    if (!isObj (bundle)) bundle = obj();
    const auto sections = ctx.get ("sections");
    if (sections.isArray() && sections.size() > 0)
    {
        auto filtered = obj();
        for (int i = 0; i < sections.size(); ++i)
        {
            const juce::String key = sections[i].toString();
            if (bundle.hasProperty (key)) set (filtered, key, bundle.getProperty (key, {}));
        }
        bundle = filtered;
    }
    return Outcome::success (obj ({ { "doc", d.id }, { "mode", metaModeName (d) },
                                    { "metaDirty", (bool) d.metaDirty }, { "bundle", bundle } }));
}

Outcome saveImpl (Context& ctx, const juce::File& target, bool saveAs)
{
    FileDoc& d = *ctx.doc;
    const juce::String meta = ctx.str ("meta", "auto");
    if (meta == "sidecar") d.metaMode = sc::MetaMode::External;
    else if (meta == "embedded")
    {
        if (!target.getFileExtension().equalsIgnoreCase (".wav"))
            return invalid ("meta", "\"sidecar\" or \"auto\" for a non-WAV target",
                            "embedded metadata needs a .wav target");
        d.metaMode = sc::MetaMode::Embedded;
    }
    auto resultOf = [] (Application& app, FileDoc& doc, const juce::File& t, bool metaWritten) {
        auto r = obj ({ { "doc", doc.id }, { "path", t.getFullPathName() },
                        { "bytes", (juce::int64) t.getSize() },
                        { "meta", obj ({ { "mode", metaModeName (doc) }, { "written", metaWritten } }) },
                        { "unsaved", app.docIsUnsaved (doc) } });
        if (!metaWritten) set (r, "warning", "The audio was saved, but the metadata sidecar could not be written.");
        return r;
    };

    if (ctx.ui == nullptr)
    {
        // Headless: synchronous (nothing to keep responsive).
        std::string e;
        bool metaWritten = false;
        if (!ctx.app.saveDocument (d, target, saveAs, &e, &metaWritten))
            return Outcome::failure (err::failed, juce::String (e), obj ({ { "path", target.getFullPathName() } }));
        return Outcome::success (resultOf (ctx.app, d, target, metaWritten));
    }

    // Open app: write the audio on a worker behind the usual progress dialog
    // (the same split as File > Save: beginSave / engine save / completeSave).
    const uint64_t snap = ctx.app.beginSave (d, target);
    Application* app = &ctx.app;
    FileDoc* doc = &d;
    const auto id = startJob (ctx, saveAs ? "doc.saveAs" : "doc.save", "Saving " + target.getFileName(), true,
        [app, doc, target, saveAs, snap, resultOf] (JobControl& jc) -> Outcome {
            jc.progress (-1.0, "saving");
            const bool ok = saveAs ? doc->engine.saveFileAs (target) : doc->engine.saveFile (target);
            if (!ok)
                return Outcome::failure (err::failed, "Could not save " + target.getFullPathName()
                                             + " (folder not writable, file locked or disk full?)");
            Outcome o;
            if (!jc.onMessageThread ([&] {
                    if (!app->isDocLoaded (doc)) { o = Outcome::failure (err::unknownDoc, "the document was closed"); return; }
                    const bool metaOk = app->completeSave (*doc, target, saveAs, snap);
                    o = Outcome::success (resultOf (*app, *doc, target, metaOk));
                }))
                return Outcome::failure (err::shuttingDown, "the application is shutting down");
            return o;
        });
    return Outcome::job (id);
}

Outcome docSaveAsCmd (Context& ctx)
{
    juce::File f;
    Outcome bad;
    if (!absolutePath (ctx, "path", f, bad)) return bad;
    if (f.isDirectory())
        return invalid ("path", "file path", "path is a folder");
    {
        const auto chk = checkPath (ctx.app, f, true, ctx.confirmed());
        if (!chk.ok) return chk;
    }
    if (f.existsAsFile() && !ctx.confirmed())
        return Outcome::failure (err::confirmRequired, "Target exists; resend with params.confirm=true",
                                 obj ({ { "path", f.getFullPathName() },
                                        { "hint", "pass confirm:true, or choose a new path" } }));
    if (!f.getParentDirectory().isDirectory())
        return Outcome::failure (err::failed, "the target folder does not exist",
                                 obj ({ { "folder", f.getParentDirectory().getFullPathName() } }));
    return saveImpl (ctx, f, true);
}

Outcome docSaveCmd (Context& ctx)
{
    FileDoc& d = *ctx.doc;
    const juce::File f = d.engine.getFile();
    if (f == juce::File() || f.getFullPathName().isEmpty())
        return Outcome::failure (err::failed, "this document has no file yet",
                                 obj ({ { "hint", "use doc.saveAs {path}" } }));
    if (!ctx.confirmed())
        return Outcome::failure (err::confirmRequired, "Saving overwrites " + f.getFullPathName()
                                     + "; resend with params.confirm=true",
                                 obj ({ { "path", f.getFullPathName() }, { "hint", "pass confirm:true" } }));
    return saveImpl (ctx, f, false);
}

Outcome docCloseCmd (Context& ctx)
{
    Application& app = ctx.app;
    FileDoc& d = *ctx.doc;
    if (app.recording.active && app.recDoc() == &d)
        return Outcome::failure (err::busy, "this document is being recorded into", obj ({ { "recording", true } }));
    if (app.docIsUnsaved (d) && !ctx.confirmed())
        return Outcome::failure (err::confirmRequired, "Document has unsaved changes",
                                 obj ({ { "unsaved", true }, { "doc", d.id },
                                        { "hint", "call doc.save first or pass confirm:true" } }));
    const int id = d.id;
    app.closeDoc (indexOf (app, &d));
    FileDoc* a = app.doc();
    return Outcome::success (obj ({ { "closed", id }, { "active", a != nullptr ? juce::var (a->id) : juce::var() },
                                    { "count", app.numDocs() } }));
}

Outcome docCloseAllCmd (Context& ctx)
{
    Application& app = ctx.app;
    const auto unsaved = app.unsavedDocNames();
    if (!unsaved.empty() && !ctx.confirmed())
    {
        juce::Array<juce::var> names;
        for (auto& n : unsaved) names.add (n);
        return Outcome::failure (err::confirmRequired, "Some documents have unsaved changes",
                                 obj ({ { "unsaved", names }, { "hint", "pass confirm:true to discard them" } }));
    }
    if (app.recording.active) app.stopRecording();
    const int n = app.numDocs();
    while (app.numDocs() > 0) app.closeDoc (app.numDocs() - 1);
    return Outcome::success (obj ({ { "closed", n } }));
}

Outcome appQuitCmd (Context& ctx)
{
    Application& app = ctx.app;
    if (ctx.state.jobs.numRunning() > 0)
        return Outcome::failure (err::busy, "jobs are running", obj ({ { "jobs", ctx.state.jobs.list (false) } }));
    const auto unsaved = app.unsavedDocNames();
    if (!unsaved.empty() && !ctx.confirmed())
    {
        juce::Array<juce::var> names;
        for (auto& n : unsaved) names.add (n);
        return Outcome::failure (err::confirmRequired, "Some documents have unsaved changes",
                                 obj ({ { "unsaved", names }, { "hint", "save them, or pass confirm:true to discard" } }));
    }
    // After the response is sent (the server thread needs a moment); the
    // window re-checks jobs and unsaved work at that point.
    ControlState* st = &ctx.state;
    const bool discard = ctx.confirmed();
    juce::Timer::callAfterDelay (300, [st, discard] { if (st->ui != nullptr) st->ui->requestQuit (discard); });
    return Outcome::success (obj ({ { "quitting", true } }));
}

Outcome selectionSetCmd (Context& ctx)
{
    Application& app = ctx.app;
    const double dur = app.audioEngine().duration();
    const double a = ctx.num ("start", 0.0), b = ctx.num ("end", 0.0);
    if (b < a) return invalid ("end", ">= start", "end must be >= start");
    if (a > dur) return invalid ("start", "<= " + juce::String (dur), "start is beyond the end of the file");

    const auto ch = ctx.get ("channels");
    if (ch.isArray())
    {
        std::vector<int> vis;
        const int n = app.audioEngine().numChannels();
        for (int i = 0; i < ch.size(); ++i)
        {
            const int c = (int) ch[i];
            if (c < 0 || c >= n)
                return invalid ("channels[" + juce::String (i) + "]", "0.." + juce::String (n - 1),
                                "channel " + juce::String (c) + " does not exist");
            if (std::find (vis.begin(), vis.end(), c) == vis.end()) vis.push_back (c);
        }
        if (vis.empty()) return invalid ("channels", "non-empty array", "channels must not be empty");
        std::sort (vis.begin(), vis.end());
        app.visibleChannels = vis;
        app.syncActiveDoc();
    }
    app.selectionRow = -1;
    app.setSelection ({ a, std::min (b, dur) });
    if (app.onStateChange) app.onStateChange();
    juce::Array<juce::var> vis;
    for (int c : app.visibleChannels) vis.add (c);
    return Outcome::success (obj ({ { "start", app.selection.getStart() }, { "end", app.selection.getEnd() },
                                    { "channels", vis } }));
}

Outcome selectionAllCmd (Context& ctx)
{
    ctx.app.selectAll();
    return Outcome::success (obj ({ { "start", ctx.app.selection.getStart() },
                                    { "end", ctx.app.selection.getEnd() } }));
}

Outcome journalReadCmd (Context& ctx)
{
    juce::StringArray kinds;
    const auto k = ctx.get ("kinds");
    if (k.isArray())
        for (int i = 0; i < k.size(); ++i) kinds.add (k[i].toString());
    return Outcome::success (ctx.state.journal.read ((juce::int64) ctx.num ("sinceSeq", 0), kinds,
                                                     ctx.integer ("limit", 200), ctx.integer ("docId", -1)));
}

Outcome sleepCmd (Context& ctx)
{
    const int ms = ctx.integer ("ms", 100);
    JobManager::StartInfo si;
    si.cmd = "debug.sleep";
    si.mutating = ctx.flag ("mutating", false);
    si.source = ctx.source;
    si.params = ctx.params;
    const auto id = ctx.state.jobs.start (si, [ms] (JobControl& jc) {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (;;)
        {
            const double el = juce::Time::getMillisecondCounterHiRes() - t0;
            if (el >= ms) break;
            if (jc.cancelled())
                return Outcome::failure (err::cancelled, "cancelled by client");
            jc.progress (el / std::max (1, ms), "sleeping");
            juce::Thread::sleep (5);
        }
        return Outcome::success (obj ({ { "sleptMs", ms } }));
    });
    return Outcome::job (id);
}

} // namespace

void registerCoreCommands (CommandRegistry& r)
{
    r.add ({ "app.describe", "Describe every command (JSON Schemas of params)", "app",
             { P ("category", PType::String).describe ("Only this domain (e.g. \"doc\")"),
               P ("withSchemas", PType::Boolean).def (true).describe ("Include params/result schemas"),
               P ("includeInternal", PType::Boolean).def (false).describe ("Include debug helpers") },
             {}, Query, {}, describeCmd });

    r.add ({ "app.state", "Application state: documents, selection, view, transport, AI, jobs", "app",
             {}, {}, Query, {}, stateCmd });

    r.add ({ "doc.list", "Open documents (stable ids)", "doc", {}, {}, Query, {}, docListCmd });

    r.add ({ "doc.open", "Open an audio file (or activate it when already open)", "doc",
             { P ("path", PType::String).req().describe ("Absolute path of the audio file"),
               P ("activate", PType::Boolean).def (true).describe ("Make it the active document") },
             {}, NoDocSwitch | LongRunning, {}, docOpenCmd });

    r.add ({ "doc.close", "Close a document (confirm when it has unsaved changes)", "doc",
             { P ("confirm", PType::Boolean).describe ("Discard unsaved changes") },
             {}, NeedsDoc | NoDocSwitch | Destructive, {}, docCloseCmd });

    r.add ({ "doc.closeAll", "Close every document (confirm when some are unsaved)", "doc",
             { P ("confirm", PType::Boolean).describe ("Discard unsaved changes") },
             {}, NoDocSwitch | Destructive, {}, docCloseAllCmd });

    r.add ({ "app.quit", "Quit the application (confirm when documents are unsaved)", "app",
             { P ("confirm", PType::Boolean).describe ("Discard unsaved changes") },
             {}, NeedsUi | Destructive, {}, appQuitCmd });

    r.add ({ "doc.activate", "Make a document the active one (the one the window shows)", "doc",
             {}, {}, NeedsDoc | NoDocSwitch, {}, docActivateCmd });

    r.add ({ "doc.meta", "Document metadata bundle (markers, selections, calibration, transcription...)", "doc",
             { P ("sections", PType::Array).items (PType::String)
                   .describe ("Keys of the bundle to return (default: all)") },
             {}, NeedsDoc | Query, {}, docMetaCmd });

    r.add ({ "doc.saveAs", "Save the document to a new path (audio + metadata)", "doc",
             { P ("path", PType::String).req().describe ("Absolute target path (.wav keeps embedded metadata)"),
               P ("meta", PType::String).oneOf ({ "auto", "embedded", "sidecar" }).def ("auto")
                   .describe ("Metadata storage: keep (auto), RIFF chunks (embedded) or .meta.json (sidecar)"),
               P ("confirm", PType::Boolean).describe ("Required when the target exists") },
             {}, NeedsDoc | Destructive | LongRunning, {}, docSaveAsCmd });

    r.add ({ "doc.save", "Save the document in place (overwrites its file)", "doc",
             { P ("meta", PType::String).oneOf ({ "auto", "embedded", "sidecar" }).def ("auto"),
               P ("confirm", PType::Boolean).describe ("Required: the file on disk is overwritten") },
             {}, NeedsDoc | Destructive | LongRunning, {}, docSaveCmd });

    r.add ({ "selection.set", "Set the time selection (seconds) and optionally the channels edits apply to", "selection",
             { P ("start", PType::Number).req().min (0).describe ("Start (s)"),
               P ("end", PType::Number).req().min (0).describe ("End (s); end == start clears the range"),
               P ("channels", PType::Array).items (PType::Integer, 0.0)
                   .describe ("0-based channels edits apply to (also the visible tracks)") },
             {}, NeedsDoc, { "select" }, selectionSetCmd });

    r.add ({ "selection.all", "Select the whole file", "selection", {}, {}, NeedsDoc, { "selectAll" }, selectionAllCmd });

    r.add ({ "journal.read", "Read the session journal (commands, steps, undo/redo, jobs)", "journal",
             { P ("sinceSeq", PType::Integer).min (0).def (0),
               P ("kinds", PType::Array).items (PType::String).describe ("command|step|history|job|meta|doc|alert"),
               P ("limit", PType::Integer).range (1, 10000).def (200),
               P ("docId", PType::Integer).min (0).describe ("Only entries of this document") },
             {}, Query, {}, journalReadCmd });

    r.add ({ "debug.sleep", "Test helper: a cancellable job that sleeps", "debug",
             { P ("ms", PType::Integer).range (0, 600000).def (100),
               P ("mutating", PType::Boolean).def (false).describe ("Count as the mutating job (busy tests)") },
             {}, LongRunning | Internal, {}, sleepCmd });
}

} // namespace sc::control
