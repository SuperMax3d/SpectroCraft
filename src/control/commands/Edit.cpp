// SpectroCraft - AI control lot 6: typed commands for every action step
// (effect.* / gen.* / edit.*, aliases = the old step types), clipboard edits,
// documents, selection tools, settings, recent files, metadata, recording,
// recorded actions, tables.
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "control/PathPolicy.h"
#include "control/StateJson.h"
#include "control/StepRunner.h"
#include "Application.h"

namespace sc::control {

namespace {

Outcome invalid (const juce::String& path, const juce::String& expected, const juce::String& msg)
{
    return Outcome::failure (err::invalidParams, msg, obj ({ { "path", path }, { "expected", expected } }));
}

// ---------------------------------------------------------------- typed steps

struct TypedStep
{
    const char* id;
    const char* type;      // sc::actionTypes
    const char* title;
    std::vector<ParamSpec> params;
};

std::vector<TypedStep> typedSteps()
{
    using namespace sc::actionTypes;
    const auto points = [] (const char* x) {
        return P ("points", PType::Array).req().items (PType::Object)
            .describe (juce::String ("[{") + x + ", g}] (g in dB)");
    };
    return {
        { "effect.silence",      kSilence,     "Silence the range", {} },
        { "effect.fadeIn",       kFadeIn,      "Fade in over the range", {} },
        { "effect.fadeOut",      kFadeOut,     "Fade out over the range", {} },
        { "effect.normalize",    kNormalize,   "Normalize the range", {} },
        { "effect.reverse",      kReverse,     "Reverse the range", {} },
        { "effect.invert",       kInvert,      "Invert the polarity of the range", {} },
        { "effect.changeGain",   kChangeGain,  "Change gain (dB)", { P ("db", PType::Number).req().range (-96, 96).describe ("Gain in dB") } },
        { "effect.timeStretch",  kTimeStretch, "Pitch-preserving tempo change", { P ("pct", PType::Number).req().range (-95, 1000).describe ("+ faster / - slower (%)") } },
        { "effect.catTimeScale", kCatTimeScale, "Content-aware time scale", { P ("reduceGaps", PType::Boolean).def (false),
                                                                              P ("percentile", PType::Number).range (0, 100).def (95),
                                                                              P ("speedFactor", PType::Number).range (0.1, 10).def (1.25) } },
        { "effect.filterEq",     kFilterEq,    "Zero-phase curve EQ", { points ("f") } },
        { "effect.envelope",     kEnvelope,    "Amplitude envelope over time", { points ("t") } },
        { "effect.resample",     kResample,    "Resample the file (not undoable: clears the history)",
                                               { P ("rate", PType::Number).req().range (100, 768000),
                                                 P ("weights", PType::Array).describe ("Channel remix matrix [[...], ...]") } },
        { "gen.silence",         kGenSilence,  "Generate silence (replaces the range, or inserts at the playhead)", { P ("dur", PType::Number).req().range (0, 86400) } },
        { "gen.noise",           kGenNoise,    "Generate noise", { P ("type", PType::String).oneOf ({ "white", "pink", "brown" }).def ("white"),
                                                                   P ("intensity", PType::Number).range (0, 1).def (1),
                                                                   P ("dur", PType::Number).req().range (0, 86400) } },
        { "gen.tone",            kGenTone,     "Generate tones", { P ("tones", PType::Array).req().items (PType::Object).describe ("[{f: Hz, a: 0..1}]"),
                                                                   P ("shape", PType::String).oneOf ({ "sine", "square", "sawtooth", "triangle" }).def ("sine"),
                                                                   P ("dur", PType::Number).req().range (0, 86400) } },
        { "edit.delete",         kDelete,      "Delete the range", {} },
        { "edit.trim",           kTrim,        "Keep only the range", {} },
    };
}

/** effect.changeGain {db, select} == step.run {step:{type:"changeGain", params:{db}}, select}. */
Handler typedHandler (const juce::String& type, std::vector<juce::String> keys)
{
    return [type, keys] (Context& ctx) {
        // Generators: refuse a size the machine cannot hold (no silent huge allocation).
        if (ctx.has ("dur"))
        {
            const double bytes = ctx.num ("dur", 0.0) * ctx.app.audioEngine().sampleRate()
                                 * std::max (1, ctx.app.audioEngine().numChannels()) * 4.0;
            if (bytes > 4.0 * 1024 * 1024 * 1024)
                return invalid ("dur", "<= 4 GiB of audio", "generated audio would exceed 4 GiB");
        }
        sc::ActionStep s;
        s.type = type;
        s.params = obj();
        for (const auto& k : keys)
            if (ctx.has (k.toRawUTF8())) set (s.params, k, ctx.get (k.toRawUTF8()));
        return runStepsCommand (ctx, { s }, true, true);
    };
}

// ---------------------------------------------------------------- clipboard edits

using EditFn = std::function<bool (Application&, std::string*)>;

Handler editHandler (EditFn fn, const char* what)
{
    return [fn, what] (Context& ctx) {
        std::optional<std::vector<int>> none;
        std::string e;
        // Explicit range (optional): same "select" as step.run.
        const auto sel = ctx.get ("select");
        if (isObj (sel))
        {
            const double a = (double) sel.getProperty ("start", 0.0), b = (double) sel.getProperty ("end", 0.0);
            if (b <= a) return invalid ("select.end", "> start", "select is empty");
            ctx.app.selectionRow = -1;
            ctx.app.setSelection ({ a, b });
        }
        else if (sel.isString())
            ctx.app.selectAll();
        FileDoc* d = ctx.app.doc();
        const size_t before = d != nullptr ? d->undo.undoSize() : 0;
        if (!fn (ctx.app, &e))
            return Outcome::failure (err::failed, juce::String (what) + ": " + (e.empty() ? std::string ("nothing done") : e));
        auto r = obj ({ { "applied", what } });
        if ((d = ctx.app.doc()) != nullptr)
        {
            if (d->undo.undoSize() != before && d->undo.canUndo())
                set (r, "description", d->undo.lastRecorded().description);
            set (r, "doc", d->id);
        }
        set (r, "clipboardSeconds", ctx.app.canPaste() && ctx.app.audioEngine().sampleRate() > 0
                                        ? (double) ctx.app.clipboard->getNumSamples() / ctx.app.audioEngine().sampleRate() : 0.0);
        return Outcome::success (r);
    };
}

// ---------------------------------------------------------------- documents / selection tools

Outcome docNewCmd (Context& ctx)
{
    const int ch = ctx.integer ("channels", 2);
    const double rate = ctx.num ("sampleRate", 48000.0);
    if (ctx.num ("durationSec", 0.0) * rate * ch * 4.0 > 4.0 * 1024 * 1024 * 1024)
        return invalid ("durationSec", "<= 4 GiB of audio", "the document would exceed 4 GiB");
    if (!ctx.app.newFile (ch, rate))
        return Outcome::failure (err::failed, "could not create the document");
    FileDoc* d = ctx.app.doc();
    if (d != nullptr && ctx.num ("durationSec", 0.0) > 0)
    {
        sc::ActionStep s;
        s.type = sc::actionTypes::kGenSilence;
        s.params = obj ({ { "dur", ctx.num ("durationSec", 0.0) } });
        std::string e;
        sc::applyActionStep (ctx.app, s, &e);
    }
    auto r = docSummary (ctx.app, *d);
    set (r, "doc", d->id);
    return Outcome::success (r);
}

Outcome selectionToolCmd (Context& ctx)
{
    const juce::String t = ctx.str ("tool");
    const SpectralSelMode m = t == "marquee" ? SpectralSelMode::Marquee
                            : t == "lasso" ? SpectralSelMode::Lasso
                            : t == "paintbrush" ? SpectralSelMode::Paintbrush : SpectralSelMode::TimeRange;
    ctx.app.setSelectionTool (m);
    if (ctx.app.onStateChange) ctx.app.onStateChange();
    return Outcome::success (obj ({ { "tool", t }, { "brushPct", ctx.app.brushPctValue() } }));
}

Outcome clearSpectralCmd (Context& ctx)
{
    ctx.app.clearSpectralSelection();
    return Outcome::success (obj ({ { "hasSpectral", ctx.app.hasSpectralSelection() } }));
}

Outcome brushCmd (Context& ctx)
{
    // Brush size is a percentage of the cursor's own frequency, snapped to
    // the 1/2/4/8/16/32 % ladder.
    if (ctx.has ("pct"))
        ctx.app.setBrushPct (ctx.num ("pct", ctx.app.brushRadiusPct () * 100.0));
    else
        ctx.app.stepBrushRadius (ctx.flag ("larger", true));
    if (ctx.app.onStateChange) ctx.app.onStateChange();
    return Outcome::success (obj ({ { "brushPct", ctx.app.brushPctValue() } }));
}

Outcome splitCmd (Context& ctx)
{
    std::string e;
    if (!ctx.app.splitSpectralSelectionConfigured (&e))
        return Outcome::failure (err::failed, juce::String (e), obj ({ { "hint", "needs a spectral selection and a configured split (Effects menu)" } }));
    return Outcome::success (obj ({ { "channels", ctx.app.audioEngine().numChannels() } }));
}

// ---------------------------------------------------------------- settings

/** API-settable settings: name -> getter/setter with the same clamps as
    loadSettings. Security: control_enabled / control_roots are NOT here (a
    client must not widen its own access). */
struct SettingKey
{
    const char* name;
    PType type;
    std::function<juce::var (const Settings&)> get;
    std::function<juce::var (Settings&, const juce::var&)> set;   // returns the value applied
};

int pow2Clamp (int v, int lo, int hi)
{
    int p = lo;
    while (p < v && p < hi) p *= 2;
    return std::clamp (p, lo, hi);
}

std::vector<SettingKey> settingKeys()
{
    auto b = [] (const char* n, bool Settings::* f) {
        return SettingKey { n, PType::Boolean, [f] (const Settings& s) { return juce::var (s.*f); },
                            [f] (Settings& s, const juce::var& v) { s.*f = (bool) v; return juce::var (s.*f); } };
    };
    return {
        { "fft_size", PType::Integer, [] (const Settings& s) { return juce::var (s.fftSize); },
          [] (Settings& s, const juce::var& v) { s.fftSize = pow2Clamp ((int) v, 512, 65536); return juce::var (s.fftSize); } },
        b ("fft_auto", &Settings::fftAuto),
        b ("fft_multires", &Settings::fftMultiRes),
        b ("log_freq", &Settings::logFrequency),
        b ("snap_zero", &Settings::snapToZeroCrossings),
        { "fft_local_n", PType::Integer, [] (const Settings& s) { return juce::var (s.fftLocalN); },
          [] (Settings& s, const juce::var& v) { s.fftLocalN = pow2Clamp ((int) v, 1024, 65536); return juce::var (s.fftLocalN); } },
        { "fft_axis_mode", PType::Integer, [] (const Settings& s) { return juce::var (s.fftAxisMode); },
          [] (Settings& s, const juce::var& v) { s.fftAxisMode = std::clamp ((int) v, 0, 5); return juce::var (s.fftAxisMode); } },
        { "fft_track", PType::Integer, [] (const Settings& s) { return juce::var (s.fftTrack); },
          [] (Settings& s, const juce::var& v) { s.fftTrack = std::clamp ((int) v, 0, 2); return juce::var (s.fftTrack); } },
        b ("multi_instance", &Settings::allowMultipleInstances),
        b ("check_updates_startup", &Settings::checkUpdatesAtStartup),   // docs/updates.md
        b ("track_axis_right", &Settings::trackAxisOnRight),
        b ("track_ruler_bottom", &Settings::trackRulerAtBottom),
        b ("show_amp_grid", &Settings::showAmpGrid),
        b ("show_freq_grid", &Settings::showFreqGrid),
        b ("show_third_oct_grid", &Settings::showThirdOctaveGrid),
        b ("show_ded_track_grid", &Settings::showDedicatedTrackGrid),
        b ("export_axes", &Settings::exportAxes),
        b ("export_markers", &Settings::exportMarkers),
        b ("export_selections", &Settings::exportSelections),
        b ("export_drawings", &Settings::exportDrawings),
    };
}

Outcome settingsGetCmd (Context& ctx)
{
    auto keys = settingKeys();
    juce::StringArray want;
    const auto k = ctx.get ("keys");
    if (k.isArray()) for (int i = 0; i < k.size(); ++i) want.add (k[i].toString());
    auto values = obj();
    for (const auto& s : keys)
        if (want.isEmpty() || want.contains (s.name)) set (values, s.name, s.get (ctx.app.settings));
    if (want.isEmpty() || want.contains ("ai_device")) set (values, "ai_device", juce::String (ctx.app.aiDevicePref));
    if (want.isEmpty() || want.contains ("ai_auto_download")) set (values, "ai_auto_download", ctx.app.aiAutoDownload);
    if (want.isEmpty() || want.contains ("control_enabled")) set (values, "control_enabled", ctx.app.settings.controlEnabled);
    for (const auto& w : want)
        if (!values.hasProperty (w))
            return invalid ("keys", "known setting names", "unknown setting \"" + w + "\"");
    return Outcome::success (obj ({ { "values", values },
                                    { "file", ctx.app.recentStore != nullptr ? juce::var (ctx.app.recentStore->getFile().getFullPathName()) : juce::var() } }));
}

Outcome settingsSetCmd (Context& ctx)
{
    const auto vals = ctx.get ("values");
    auto keys = settingKeys();
    auto applied = obj();
    juce::Array<juce::var> clamped;
    if (auto* o = vals.getDynamicObject())
    {
        // Validate everything first: unknown key or wrong type -> nothing applied.
        for (const auto& kv : o->getProperties())
        {
            const juce::String n = kv.name.toString();
            const SettingKey* sk = nullptr;
            for (const auto& s : keys) if (n == s.name) sk = &s;
            if (sk == nullptr && n != "ai_auto_download")
                return invalid ("values." + n, "an API-settable setting (see settings.get)",
                                n.startsWith ("control_") ? "control settings cannot be changed through the API"
                                                          : "unknown setting \"" + n + "\"");
            const bool isBool = sk == nullptr || sk->type == PType::Boolean;
            if (isBool ? !kv.value.isBool() : !(kv.value.isInt() || kv.value.isInt64() || kv.value.isDouble()))
                return invalid ("values." + n, isBool ? "boolean" : "integer", "wrong type for " + n);
        }
        for (const auto& kv : o->getProperties())
        {
            const juce::String n = kv.name.toString();
            if (n == "ai_auto_download") { ctx.app.aiAutoDownload = (bool) kv.value; set (applied, n, ctx.app.aiAutoDownload); continue; }
            for (const auto& s : keys)
                if (n == s.name)
                {
                    const auto got = s.set (ctx.app.settings, kv.value);
                    set (applied, n, got);
                    if (got != kv.value && !(kv.value.isBool())) clamped.add (n);
                }
        }
    }
    ctx.app.saveSettings();
    ctx.app.saveAiSettings();
    if (ctx.app.onStateChange) ctx.app.onStateChange();
    return Outcome::success (obj ({ { "applied", applied }, { "clamped", clamped }, { "persisted", true } }));
}

// ---------------------------------------------------------------- recent / meta / record / actions / tables

Outcome recentListCmd (Context& ctx)
{
    juce::Array<juce::var> a;
    for (const auto& f : ctx.app.recentFiles)
        a.add (obj ({ { "path", f.getFullPathName() }, { "exists", f.existsAsFile() } }));
    return Outcome::success (obj ({ { "recent", a } }));
}

Outcome recentRemoveCmd (Context& ctx)
{
    const juce::File f (ctx.str ("path"));
    ctx.app.removeRecentFile (f);
    return recentListCmd (ctx);
}

Outcome recentClearCmd (Context& ctx)
{
    ctx.app.clearRecentFiles();
    return recentListCmd (ctx);
}

Outcome metaSetModeCmd (Context& ctx)
{
    const auto mode = ctx.str ("mode") == "external" ? sc::MetaMode::External : sc::MetaMode::Embedded;
    if (!ctx.app.setActiveDocMetaMode (mode))
        return Outcome::failure (err::failed, "could not switch the metadata storage (embedding needs a .wav)");
    return Outcome::success (obj ({ { "mode", metaModeName (*ctx.doc) }, { "metaDirty", (bool) ctx.doc->metaDirty } }));
}

Outcome metaSaveCmd (Context& ctx)
{
    std::string e;
    if (!ctx.app.saveActiveDocMeta (&e))
        return Outcome::failure (err::failed, juce::String (e));
    return Outcome::success (obj ({ { "mode", metaModeName (*ctx.doc) }, { "metaDirty", (bool) ctx.doc->metaDirty } }));
}

Outcome recordStartCmd (Context& ctx)
{
    if (ctx.app.recording.active)
        return Outcome::failure (err::busy, "already recording");
    std::string e;
    if (!ctx.app.startRecording (&e))
        return Outcome::failure (err::failed, juce::String (e), obj ({ { "hint", "choose a recording device in Transport > Recording Device" } }));
    FileDoc* d = ctx.app.recDoc();
    return Outcome::success (obj ({ { "recording", true }, { "doc", d != nullptr ? juce::var (d->id) : juce::var() } }));
}

Outcome recordStopCmd (Context& ctx)
{
    if (!ctx.app.recording.active)
        return Outcome::failure (err::failed, "not recording");
    FileDoc* d = ctx.app.recDoc();
    const int id = d != nullptr ? d->id : -1;
    ctx.app.stopRecording();
    return Outcome::success (obj ({ { "recording", false }, { "doc", id } }));
}

Outcome recordModeCmd (Context& ctx)
{
    const juce::String m = ctx.str ("mode", "unlimited");
    using M = Application::RecordingState::Mode;
    const M mode = m == "loop" ? M::Loop : m == "timed" ? M::Timed : M::Unlimited;
    if (mode != M::Unlimited && ctx.num ("seconds", 0.0) <= 0)
        return invalid ("seconds", "> 0", "loop and timed modes need seconds");
    ctx.app.setRecordingMode (mode, ctx.num ("seconds", 0.0));
    return Outcome::success (obj ({ { "mode", m }, { "seconds", ctx.app.recording.durationSecs } }));
}

Outcome actionsListCmd (Context& ctx)
{
    juce::Array<juce::var> a;
    for (int i = 0; i < ctx.app.actions.size(); ++i)
    {
        auto v = ctx.app.actions.get (i)->toJson();
        set (v, "index", i);
        a.add (v);
    }
    return Outcome::success (obj ({ { "actions", a },
                                    { "recording", ctx.app.actionRec.active } }));
}

Outcome actionsRunCmd (Context& ctx)
{
    const sc::Action* act = nullptr;
    if (ctx.has ("name"))
    {
        for (int i = 0; i < ctx.app.actions.size(); ++i)
            if (ctx.app.actions.get (i)->name == ctx.str ("name")) act = ctx.app.actions.get (i);
    }
    else
        act = ctx.app.actions.get (ctx.integer ("index", -1));
    if (act == nullptr) return invalid ("name", "a recorded action (actions.list)", "no such action");
    if (act->steps.empty()) return Outcome::failure (err::failed, "the action has no steps");
    return runStepsCommand (ctx, act->steps, ctx.flag ("stopOnError", true), false);
}

Outcome tablesListCmd (Context& ctx)
{
    juce::Array<juce::var> a;
    for (const auto& t : ctx.app.activeTables())
        a.add (obj ({ { "name", t.name }, { "parameters", (int) t.params.size() }, { "sampleRate", t.sampleRate },
                      { "durationSec", t.duration() }, { "visible", t.visible } }));
    return Outcome::success (obj ({ { "tables", a }, { "mode", ctx.app.tablesMode() } }));
}

Outcome tablesImportCmd (Context& ctx)
{
    const juce::String p = ctx.str ("path");
    if (!juce::File::isAbsolutePath (p)) return invalid ("path", "absolute .csv path", "path must be absolute");
    const juce::File f (p);
    {
        const auto chk = checkPath (ctx.app, f, false, ctx.confirmed());
        if (!chk.ok) return chk;
    }
    if (!f.existsAsFile() || f.getSize() > 64 * 1024 * 1024)
        return Outcome::failure (err::failed, "missing or too large (> 64 MB): " + f.getFullPathName());
    std::string e;
    if (!ctx.app.addTableFromCsv (f.getFileNameWithoutExtension(), f.loadFileAsString(), &e))
        return Outcome::failure (err::failed, juce::String (e));
    return tablesListCmd (ctx);
}

} // namespace

void registerEditCommands (CommandRegistry& r)
{
    for (const auto& t : typedSteps())
    {
        auto params = stepScopeParams();
        std::vector<juce::String> keys;
        for (const auto& p : t.params) keys.push_back (p.name);
        params.insert (params.begin(), t.params.begin(), t.params.end());
        const juce::String cat = juce::String (t.id).upToFirstOccurrenceOf (".", false, false);
        r.add ({ t.id, t.title, cat, params, {}, Mutates | NeedsDoc, { t.type }, typedHandler (t.type, keys) });
    }

    const auto sel = P ("select", PType::Range).describe ("\"all\" or {start, end} (default: current selection)");
    r.add ({ "edit.cut", "Cut the range to the app clipboard", "edit", { sel }, {}, Mutates | NeedsDoc, { "cut" },
             editHandler ([] (Application& a, std::string* e) { return a.cut (e); }, "cut") });
    r.add ({ "edit.copy", "Copy the range to the app clipboard", "edit", { sel }, {}, NeedsDoc, { "copy" },
             editHandler ([] (Application& a, std::string*) { a.copy(); return a.canPaste(); }, "copy") });
    r.add ({ "edit.paste", "Insert the clipboard at the selection start / playhead", "edit", { sel }, {}, Mutates | NeedsDoc, { "paste" },
             editHandler ([] (Application& a, std::string* e) { return a.paste (e); }, "paste") });
    r.add ({ "edit.pasteMix", "Mix the clipboard into the file", "edit", { sel }, {}, Mutates | NeedsDoc, { "pasteMix" },
             editHandler ([] (Application& a, std::string* e) { return a.pasteMix (e); }, "pasteMix") });
    r.add ({ "edit.pasteToNewChannels", "Paste the clipboard into new channels", "edit", { sel }, {}, Mutates | NeedsDoc, { "pasteToNewChannels" },
             editHandler ([] (Application& a, std::string* e) { return a.pasteToNewChannels (e); }, "pasteToNewChannels") });
    r.add ({ "edit.copyToNewFile", "Copy the range (visible channels) into a new document", "edit", { sel }, {}, NeedsDoc, { "copyToNewFile" },
             editHandler ([] (Application& a, std::string* e) { return a.copyToNewFile (e); }, "copyToNewFile") });
    r.add ({ "edit.splitSpectralSel", "Run the configured Split Spectral Selection", "edit", {}, {}, Mutates | NeedsDoc, {}, splitCmd });

    r.add ({ "doc.new", "New empty document", "doc",
             { P ("channels", PType::Integer).range (1, 64).def (2), P ("sampleRate", PType::Number).range (100, 768000).def (48000),
               P ("durationSec", PType::Number).range (0, 86400).def (0) },
             {}, NoDocSwitch, {}, docNewCmd });
    r.add ({ "selection.tool", "Spectral selection tool", "selection",
             { P ("tool", PType::String).req().oneOf ({ "timeRange", "marquee", "lasso", "paintbrush" }) },
             {}, None, {}, selectionToolCmd });
    r.add ({ "selection.clearSpectral", "Clear the spectral selection", "selection", {}, {}, NeedsDoc, {}, clearSpectralCmd });
    r.add ({ "selection.brush", "Paintbrush size (% of cursor frequency; snaps to the 1/2/4/8/16/32 ladder)", "selection",
             { P ("pct", PType::Number).range (1, 32), P ("larger", PType::Boolean) }, {}, None, {}, brushCmd });

    r.add ({ "settings.get", "Read settings (same clamps and names as the settings store)", "settings",
             { P ("keys", PType::Array).items (PType::String) }, {}, Query, {}, settingsGetCmd });
    r.add ({ "settings.set", "Change settings (clamped like the app, persisted); control_* refused", "settings",
             { P ("values", PType::Object).req().describe ("{name: value}; see settings.get") }, {}, None, {}, settingsSetCmd });

    r.add ({ "recent.list", "Open Recent list", "recent", {}, {}, Query, {}, recentListCmd });
    r.add ({ "recent.remove", "Remove a path from Open Recent", "recent", { P ("path", PType::String).req() }, {}, None, {}, recentRemoveCmd });
    r.add ({ "recent.clear", "Clear Open Recent", "recent", {}, {}, None, {}, recentClearCmd });

    r.add ({ "meta.setMode", "Metadata storage: embedded RIFF chunks or .meta.json sidecar (migrates on disk)", "meta",
             { P ("mode", PType::String).req().oneOf ({ "embedded", "external" }) }, {}, NeedsDoc | ActiveDocOnly, {}, metaSetModeCmd });
    r.add ({ "meta.save", "Write the metadata only (no audio)", "meta", {}, {}, NeedsDoc | ActiveDocOnly, {}, metaSaveCmd });

    r.add ({ "record.start", "Start recording (device chosen in the Transport menu)", "record", {}, {}, None, {}, recordStartCmd });
    r.add ({ "record.stop", "Stop recording", "record", {}, {}, None, {}, recordStopCmd });
    r.add ({ "record.setMode", "Recording mode", "record",
             { P ("mode", PType::String).req().oneOf ({ "unlimited", "loop", "timed" }), P ("seconds", PType::Number).min (0) },
             {}, None, {}, recordModeCmd });

    r.add ({ "actions.list", "Recorded actions (Actions panel)", "actions", {}, {}, Query, {}, actionsListCmd });
    r.add ({ "actions.run", "Replay a recorded action on a range (one history entry per step)", "actions",
             { P ("name", PType::String), P ("index", PType::Integer).min (0), P ("stopOnError", PType::Boolean).def (true),
               P ("select", PType::Range), P ("channels", PType::Array).items (PType::Integer, 0.0) },
             {}, Mutates | NeedsDoc, {}, actionsRunCmd });

    r.add ({ "tables.list", "Parameter tables of the document", "tables", {}, {}, NeedsDoc | Query, {}, tablesListCmd });
    r.add ({ "tables.importCsv", "Import a CSV table onto the document", "tables",
             { P ("path", PType::String).req(), P ("confirm", PType::Boolean) }, {}, NeedsDoc | ActiveDocOnly, {}, tablesImportCmd });
}

} // namespace sc::control
