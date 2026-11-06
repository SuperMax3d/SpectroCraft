// AI control (docs/ai-control/RAPPORT.md §5-§6): command registry, step.run,
// history, journal, --json CLI. Included by tests/UnitTests.cpp; uses its
// helpers (CHECK, scSkip, writeWav, makeTempFile, scPumpUntil, ...).
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "control/JsonCli.h"
#include "control/StateJson.h"

namespace scctl = sc::control;

static juce::var ctlCall (Application& app, const juce::String& cmd, const juce::var& params = {},
                          const juce::var& doc = {})
{
    scctl::InvokeOptions io;
    io.source = "test";
    return scctl::CommandRegistry::instance().invoke (app, cmd, params, doc, io);
}

static bool ctlOk (const juce::var& r) { return (bool) r.getProperty ("ok", false); }
static juce::String ctlCode (const juce::var& r)
{
    return r.getProperty ("error", {}).getProperty ("code", {}).toString();
}
static juce::var ctlResult (const juce::var& r) { return r.getProperty ("result", {}); }

static juce::File ctlMakeWav (const juce::String& name, int chans = 2, double rate = 8000.0, int64_t frames = 16000)
{
    const juce::File f = makeTempFile (name);
    writeWav (f, chans, rate, frames, [] (int ch, int64_t i, double r) {
        return (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * (220.0 + 110.0 * ch) * (double) i / r));
    });
    return f;
}

// Pumps until the job finished (in-process jobs commit via callAsync).
static juce::var ctlWaitJob (Application& app, const juce::String& id, int timeoutMs = 60000)
{
    scPumpUntil ([&] { return app.control().jobs.isFinished (id); }, timeoutMs);
    scDeliverPostedMessages();
    return app.control().jobs.info (id);
}

// ---------------------------------------------------------------------------
// testControlRegistry: every action step type through step.validate +
// step.run on an explicit range, plus doc / selection / history / journal.
// ---------------------------------------------------------------------------

/** Minimal valid params for each step type ({} when parameterless).
    Returns false when the type cannot run here (reason in skipWhy). */
static bool ctlStepParams (const juce::String& type, juce::var& params, juce::String& skipWhy,
                           const juce::File& scriptDir)
{
    using namespace sc::actionTypes;
    params = scctl::obj();
    auto pts = [] (std::initializer_list<std::pair<double, double>> v, const char* k1) {
        juce::Array<juce::var> a;
        for (auto& p : v) a.add (scctl::obj ({ { k1, p.first }, { "g", p.second } }));
        return juce::var (a);
    };
    if (type == kChangeGain)    params = scctl::obj ({ { "db", -6.0 } });
    else if (type == kTimeStretch) params = scctl::obj ({ { "pct", 10.0 } });
    else if (type == kCatTimeScale) params = scctl::obj ({ { "reduceGaps", false }, { "speedFactor", 1.1 } });
    else if (type == kFilterEq) params = scctl::obj ({ { "points", pts ({ { 100.0, -6.0 }, { 2000.0, -6.0 } }, "f") } });
    else if (type == kEnvelope) params = scctl::obj ({ { "points", pts ({ { 0.0, -3.0 }, { 0.2, -6.0 } }, "t") } });
    else if (type == kGenSilence) params = scctl::obj ({ { "dur", 0.1 } });
    else if (type == kGenNoise) params = scctl::obj ({ { "type", "pink" }, { "intensity", 0.2 }, { "dur", 0.1 } });
    else if (type == kGenTone)
    {
        juce::Array<juce::var> tones;
        tones.add (scctl::obj ({ { "f", 440.0 }, { "a", 0.3 } }));
        params = scctl::obj ({ { "tones", tones }, { "shape", "sine" }, { "dur", 0.1 } });
    }
    else if (type == kResample) params = scctl::obj ({ { "rate", 16000.0 } });
    else if (type == kTranscribe || type == kNoiseSuppress)
    {
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        const char* model = type == kTranscribe ? "whisper-base" : "denseunet";
        auto& mm = sc::ai::ModelManager::instance();
        const auto* spec = mm.find (model);
        if (spec == nullptr || !mm.isInstalled (*spec))
        {
            skipWhy = juce::String (model) + " not installed";
            return false;
        }
#if defined(_MSC_VER) && defined(_DEBUG)
        if (type == kNoiseSuppress) { skipWhy = "OpenVINO needs a Release build on Windows"; return false; }
#endif
        params = scctl::obj ({ { "modelId", model }, { "autoDownload", false } });
        if (type == kNoiseSuppress) scctl::set (params, "device", "auto");
#else
        params = scctl::obj ({ { "modelId", type == kTranscribe ? "whisper-base" : "denseunet" } });
#endif
    }
    else if (type == kPython)
    {
        std::string py;
        if (!sc::py::findPython (&py)) { skipWhy = "no Python interpreter"; return false; }
        params = scctl::obj ({ { "script", scriptDir.getChildFile ("Threshold.py").getFullPathName() } });
    }
    return true;
}

static void testControlRegistry()
{
    const char* all[] = {
        sc::actionTypes::kSilence, sc::actionTypes::kFadeIn, sc::actionTypes::kFadeOut,
        sc::actionTypes::kNormalize, sc::actionTypes::kChangeGain, sc::actionTypes::kReverse,
        sc::actionTypes::kInvert, sc::actionTypes::kTimeStretch, sc::actionTypes::kCatTimeScale,
        sc::actionTypes::kFilterEq, sc::actionTypes::kEnvelope, sc::actionTypes::kGenSilence,
        sc::actionTypes::kGenNoise, sc::actionTypes::kGenTone, sc::actionTypes::kResample,
        sc::actionTypes::kTranscribe, sc::actionTypes::kTrim, sc::actionTypes::kDelete,
        sc::actionTypes::kPython, sc::actionTypes::kNoiseSuppress };
    CHECK ((int) std::size (all) == 20, "control: 20 step types enumerated");

    const juce::File scriptDir = makeTempFile ("ctl_scripts");
    scriptDir.createDirectory();
    sc::py::ensureExampleScripts (scriptDir);

    for (const char* t : all)
    {
        const juce::String type (t);
        CHECK (sc::isKnownActionType (type), "control: known type " + type);
        juce::var params;
        juce::String why;
        const bool runnable = ctlStepParams (type, params, why, scriptDir);
        const auto step = scctl::obj ({ { "type", type }, { "params", params } });

        Application app;
        const juce::var v = ctlCall (app, "step.validate", scctl::obj ({ { "step", step } }));
#if !(defined(SC_ENABLE_AI) && SC_ENABLE_AI)
        if (type == sc::actionTypes::kTranscribe || type == sc::actionTypes::kNoiseSuppress)
        {
            CHECK (ctlCode (v) == "unsupported_build", "control: " + type + " is unsupported_build without AI");
            continue;
        }
#endif
        CHECK (ctlOk (v), "control: step.validate accepts " + type + " " + scctl::toLine (v));
        if (!runnable)
        {
            scSkip ("control step.run " + type + ": " + why);
            continue;
        }

        const juce::File wav = ctlMakeWav ("ctl_step_" + type + ".wav", 2, 8000.0, 16000);
        const auto opened = ctlCall (app, "doc.open", scctl::obj ({ { "path", wav.getFullPathName() } }));
        CHECK (ctlOk (opened), "control: doc.open for " + type);
        if (!ctlOk (opened)) continue;
        const int docId = (int) ctlResult (opened).getProperty ("doc", -1);

        juce::Array<juce::var> chans;
        chans.add (0);
        const auto r = ctlCall (app, "step.run",
                                scctl::obj ({ { "step", step },
                                              { "select", scctl::obj ({ { "start", 0.25 }, { "end", 1.25 } }) },
                                              { "channels", chans } }),
                                docId);
        CHECK (ctlOk (r), "control: step.run applies " + type + " " + scctl::toLine (r).substring (0, 300));
        if (!ctlOk (r)) continue;
        CHECK ((int) r.getProperty ("journalSeq", 0) > 0, "control: " + type + " envelope has journalSeq");
        CHECK (r.hasProperty ("undoDepth") && r.hasProperty ("contentVersion"),
               "control: " + type + " envelope has undo/content state");
        // Explicit channels are restored afterwards (the step did not add channels).
        if (app.audioEngine().numChannels() == 2)
            CHECK (app.visibleChannels.size() == 2, "control: " + type + " restored the visible channels");
        if (type != sc::actionTypes::kTranscribe && type != sc::actionTypes::kResample)
            CHECK ((int) r.getProperty ("undoDepth", 0) == 1, "control: " + type + " recorded one undo step");
    }
    scriptDir.deleteRecursively();
}

static void testControlDocsHistory()
{
    Application app;
    const juce::File a = ctlMakeWav ("ctl_doc_a.wav", 2);
    const juce::File b = ctlMakeWav ("ctl_doc_b.wav", 1);

    // No document yet.
    CHECK (ctlCode (ctlCall (app, "step.run", scctl::obj ({ { "step", scctl::obj ({ { "type", "silence" } }) } })))
               == "no_document", "control: step.run without a document is no_document");
    CHECK (ctlCode (ctlCall (app, "doc.open", scctl::obj ({ { "path", "relative.wav" } }))) == "invalid_params",
           "control: doc.open rejects a relative path");
    CHECK (ctlCode (ctlCall (app, "doc.open", scctl::obj ({ { "path", a.getFullPathName() + ".missing" } })))
               == "failed", "control: doc.open of a missing file fails");

    const auto ra = ctlCall (app, "doc.open", scctl::obj ({ { "path", a.getFullPathName() } }));
    const auto rb = ctlCall (app, "doc.open", scctl::obj ({ { "path", b.getFullPathName() } }));
    CHECK (ctlOk (ra) && ctlOk (rb), "control: two documents opened");
    const int idA = (int) ctlResult (ra).getProperty ("doc", -1);
    const int idB = (int) ctlResult (rb).getProperty ("doc", -1);
    CHECK (idA > 0 && idB > 0 && idA != idB, "control: stable distinct document ids");
    CHECK (app.doc() != nullptr && app.doc()->id == idB, "control: the last opened document is active");

    const auto list = ctlResult (ctlCall (app, "doc.list"));
    CHECK (list.getProperty ("docs", {}).size() == 2 && (int) list.getProperty ("active", -1) == idB,
           "control: doc.list lists both, active = b");
    CHECK (ctlCode (ctlCall (app, "history.list", {}, 99999)) == "unknown_doc", "control: unknown doc id");

    // Edit the NON-active document a: the app switches for the command only.
    app.setSelection ({ 0.1, 0.2 });   // b's selection must survive
    const auto r = ctlCall (app, "step.run",
                            scctl::obj ({ { "select", scctl::obj ({ { "start", 0.5 }, { "end", 1.0 } }) },
                                          { "step", scctl::obj ({ { "type", "changeGain" },
                                                                  { "params", scctl::obj ({ { "db", -6.0 } }) } }) } }),
                            idA);
    CHECK (ctlOk (r) && (bool) r.getProperty ("docSwitched", false), "control: step.run on a non-active doc reports docSwitched");
    CHECK (app.doc()->id == idB, "control: the active document is restored");
    CHECK (std::abs (app.selection.getStart() - 0.1) < 1e-9 && std::abs (app.selection.getEnd() - 0.2) < 1e-9,
           "control: the active document's selection is untouched");
    FileDoc* da = nullptr;
    for (auto& d : app.docs) if (d->id == idA) da = d.get();
    CHECK (da != nullptr && da->undo.undoSize() == 1, "control: the edit landed in document a");
    CHECK (da != nullptr && std::abs (da->selection.getStart() - 0.5) < 1e-9, "control: a's own selection copy was set");
    CHECK (app.docs[1]->undo.undoSize() == 0, "control: document b is unchanged");

    // selection.set on a non-active doc writes that doc's copy.
    CHECK (ctlOk (ctlCall (app, "selection.set", scctl::obj ({ { "start", 1.0 }, { "end", 1.5 } }), idA)),
           "control: selection.set on doc a");
    CHECK (da != nullptr && std::abs (da->selection.getEnd() - 1.5) < 1e-9, "control: selection stored in doc a");
    CHECK (std::abs (app.selection.getEnd() - 0.2) < 1e-9, "control: active selection still b's");
    CHECK (ctlCode (ctlCall (app, "selection.set", scctl::obj ({ { "start", 1.0 }, { "end", 0.5 } }), idA))
               == "invalid_params", "control: selection end < start rejected");

    // History on a: second edit, list, undo, redo, jumpTo.
    CHECK (ctlOk (ctlCall (app, "step.run", scctl::obj ({ { "select", "all" },
               { "step", scctl::obj ({ { "type", "fadeOut" } }) } }), idA)), "control: fadeOut on a");
    auto h = ctlResult (ctlCall (app, "history.list", {}, idA));
    const auto states = h.getProperty ("states", {});
    CHECK ((int) h.getProperty ("currentState", -1) == 2 && states.size() == 3, "control: history has 3 states");
    if (states.size() == 3)
    {
        CHECK (states[1].getProperty ("opId", {}).toString() == "step.run", "control: state 1 joined to its command");
        CHECK (states[1].getProperty ("source", {}).toString() == "test", "control: state 1 source is the client");
        CHECK (states[1].getProperty ("params", {}).getProperty ("step", {}).getProperty ("type", {}).toString()
                   == "changeGain", "control: state 1 carries its params");
        CHECK (states[2].getProperty ("description", {}).toString().isNotEmpty(), "control: state 2 has a description");
    }
    auto u = ctlCall (app, "history.undo", {}, idA);
    CHECK (ctlOk (u) && (int) u.getProperty ("undoDepth", -1) == 1 && (int) u.getProperty ("redoDepth", -1) == 1,
           "control: history.undo moved one state");
    auto j = ctlCall (app, "history.jumpTo", scctl::obj ({ { "state", 0 } }), idA);
    CHECK (ctlOk (j) && (int) ctlResult (j).getProperty ("currentState", -1) == 0
               && (int) j.getProperty ("redoDepth", -1) == 2, "control: jumpTo 0 keeps the redo stack");
    h = ctlResult (ctlCall (app, "history.list", {}, idA));
    const auto st2 = h.getProperty ("states", {});
    CHECK (st2.size() == 3 && (bool) st2[1].getProperty ("future", false)
               && st2[1].getProperty ("description", {}).toString().containsIgnoreCase ("gain"),
           "control: future states listed in redo order");
    CHECK (ctlOk (ctlCall (app, "history.redo", scctl::obj ({ { "steps", 2 } }), idA))
               && da->undo.undoSize() == 2, "control: redo 2 steps");
    CHECK (ctlCode (ctlCall (app, "history.jumpTo", scctl::obj ({ { "state", 9 } }), idA)) == "invalid_params",
           "control: jumpTo out of range");
    // An edit after an undo reports the discarded redo entries.
    ctlCall (app, "history.undo", {}, idA);
    auto e = ctlCall (app, "step.run", scctl::obj ({ { "step", scctl::obj ({ { "type", "invert" } }) } }), idA);
    CHECK (ctlOk (e) && (int) e.getProperty ("redoDiscarded", 0) == 1, "control: redoDiscarded reported");

    // doc.saveAs: confirm required on an existing target, then saved.
    const juce::File out = makeTempFile ("ctl_saved.wav");
    out.replaceWithText ("x");
    CHECK (ctlCode (ctlCall (app, "doc.saveAs", scctl::obj ({ { "path", out.getFullPathName() } }), idA))
               == "confirm_required", "control: saveAs over an existing file needs confirm");
    auto s = ctlCall (app, "doc.saveAs", scctl::obj ({ { "path", out.getFullPathName() }, { "confirm", true } }), idA);
    CHECK (ctlOk (s) && out.getSize() > 1000, "control: saveAs with confirm wrote the audio");
    CHECK (da->file == out && !app.docIsUnsaved (*da), "control: saveAs moved the doc path, now clean");
    CHECK (ctlCode (ctlCall (app, "doc.save", {}, idA)) == "confirm_required", "control: doc.save needs confirm");

    // doc.meta returns the bundle.
    da->markers.addPointMarker (0.3, "door_slam");
    const auto m = ctlCall (app, "doc.meta", scctl::obj ({ { "sections", scctl::arr ({ "markers" }) } }), idA);
    CHECK (ctlOk (m) && scctl::toLine (ctlResult (m)).contains ("door_slam"), "control: doc.meta returns markers");

    // Journal: commands, history moves and recorded steps are all there.
    const auto jr = ctlResult (ctlCall (app, "journal.read", scctl::obj ({ { "kinds", scctl::arr ({ "history" }) } })));
    CHECK (jr.getProperty ("entries", {}).size() >= 3, "control: journal has the undo/redo entries");
    const juce::String all = scctl::toLine (ctlResult (ctlCall (app, "journal.read", scctl::obj ({ { "limit", 1000 } }))));
    CHECK (all.contains ("\"cmd\":\"step.run\"") && all.contains ("\"kind\":\"step\""), "control: journal has commands and steps");

    // A menu-style edit (recordActionStep outside any command) is journaled with source "menu".
    app.recordActionStep ("normalize", {});
    const auto last = ctlResult (ctlCall (app, "journal.read",
                                          scctl::obj ({ { "sinceSeq", (int) (app.control().journal.lastSeq() - 1) } })));
    CHECK (scctl::toLine (last).contains ("\"source\":\"menu\""), "control: menu edits journaled with source menu");

    // Alerts are captured into the response, never shown.
    {
        sc::alert::ScopedCapture cap;
        sc::alert::showMessageBoxAsync (juce::AlertWindow::WarningIcon, "T", "captured!");
        CHECK (cap.take().size() == 1, "control: ScopedCapture records an alert");
    }
    CHECK (sc::alert::state().captureDepth.load() == 0, "control: capture depth back to zero");

    // Busy: a mutating job blocks mutating commands, not reads.
    const auto jb = ctlCall (app, "debug.sleep", scctl::obj ({ { "ms", 3000 }, { "mutating", true } }));
    CHECK (ctlOk (jb), "control: debug.sleep job started");
    const juce::String jid = ctlResult (jb).getProperty ("job", {}).toString();
    const auto busy = ctlCall (app, "step.run", scctl::obj ({ { "step", scctl::obj ({ { "type", "silence" } }) } }));
    CHECK (ctlCode (busy) == "busy", "control: mutating command refused while a mutating job runs");
    CHECK (ctlOk (ctlCall (app, "app.state")), "control: reads still work during a job");
    CHECK (ctlOk (ctlCall (app, "job.cancel", scctl::obj ({ { "job", jid } }))), "control: job.cancel accepted");
    const auto inf = ctlWaitJob (app, jid, 5000);
    CHECK (inf.getProperty ("state", {}).toString() == "cancelled", "control: cancelled job state " + scctl::toLine (inf));
    CHECK (ctlCode (ctlCall (app, "job.get", scctl::obj ({ { "job", "j-999" } }))) == "job_not_found", "control: job_not_found");

    // A job outliving its Application is stopped and joined by the destructor.
    {
        Application tmp;
        const auto j2 = ctlCall (tmp, "debug.sleep", scctl::obj ({ { "ms", 60000 } }));
        CHECK (ctlOk (j2), "control: long job started");
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        tmp.control().shutdown();
        CHECK (juce::Time::getMillisecondCounterHiRes() - t0 < 2000.0, "control: shutdown stops jobs promptly");
    }
}

// ---------------------------------------------------------------------------
// testControlDescribe: the schema generated from the ParamSpec tables agrees
// with validation.
// ---------------------------------------------------------------------------
static void testControlDescribe()
{
    auto& reg = scctl::CommandRegistry::instance();
    const auto d = reg.describe ({}, true, true);
    const auto cmds = d.getProperty ("commands", {});
    CHECK ((int) d.getProperty ("protocol", 0) == scctl::kProtocolVersion, "describe: protocol");
    CHECK (cmds.size() == (int) reg.all().size(), "describe: every command listed (includeInternal)");
    CHECK (reg.describe ({}, false, false).getProperty ("commands", {}).size() < cmds.size(),
           "describe: debug helpers hidden by default");

    juce::StringArray ids;
    for (const auto& s : reg.all())
    {
        CHECK (!ids.contains (s.id), "describe: unique id " + s.id);
        ids.add (s.id);
        CHECK (s.id.containsChar ('.') && s.handler != nullptr, "describe: " + s.id + " is domain.command with a handler");

        const auto schema = scctl::CommandRegistry::schemaFor (s.params);
        CHECK (schema.getProperty ("type", {}).toString() == "object"
                   && schema.getProperty ("additionalProperties", true).isBool()
                   && ! (bool) schema.getProperty ("additionalProperties", true),
               "describe: " + s.id + " schema is a closed object");

        // A minimal params object generated from the table passes validation.
        const auto minimal = scctl::CommandRegistry::minimalParams (s.params);
        scctl::Error e;
        CHECK (scctl::CommandRegistry::validate (s.params, minimal, e),
               "describe: minimal params of " + s.id + " validate (" + e.message + ")");

        // Every invalid_params path cites a property of the schema.
        const auto props = schema.getProperty ("properties", {});
        e = {};
        auto bogus = scctl::CommandRegistry::minimalParams (s.params);
        scctl::set (bogus, "__bogus__", 1);
        CHECK (!scctl::CommandRegistry::validate (s.params, bogus, e)
                   && e.details.getProperty ("path", {}).toString() == "__bogus__",
               "describe: unknown parameter rejected for " + s.id);
        for (const auto& p : s.params)
        {
            auto wrong = scctl::CommandRegistry::minimalParams (s.params);
            if (p.type == scctl::PType::Any) continue;
            scctl::set (wrong, p.name, p.type == scctl::PType::String ? juce::var (12345) : juce::var ("x?"));
            e = {};
            const bool ok = scctl::CommandRegistry::validate (s.params, wrong, e);
            if (ok) continue;   // e.g. a free-form string accepted
            const juce::String path = e.details.getProperty ("path", {}).toString();
            const juce::String root = path.upToFirstOccurrenceOf (".", false, false).upToFirstOccurrenceOf ("[", false, false);
            CHECK (e.code == "invalid_params" && props.hasProperty (root),
                   "describe: " + s.id + "." + p.name + " error path " + path + " is in the schema");
        }
    }
    for (const char* must : { "app.describe", "app.state", "doc.open", "doc.list", "doc.saveAs", "doc.meta",
                              "selection.set", "selection.all", "history.list", "history.undo", "history.redo",
                              "history.jumpTo", "step.run", "steps.run", "step.validate", "journal.read",
                              "job.get", "job.cancel" })
        CHECK (ids.contains (must), juce::String ("describe: lot-0 command present: ") + must);

    // Spot checks of the generated schema (RAPPORT §6.2 shape).
    const auto* run = reg.find ("step.run");
    if (run != nullptr)
    {
        const auto sch = scctl::CommandRegistry::schemaFor (run->params);
        const auto sel = sch.getProperty ("properties", {}).getProperty ("select", {});
        CHECK (sel.hasProperty ("oneOf"), "describe: step.run select is oneOf(\"all\", {start,end})");
        CHECK (sch.getProperty ("required", {}).size() == 1, "describe: step.run requires only step");
    }
    CHECK (reg.find ("select") == reg.find ("selection.set"), "describe: aliases resolve");

    // Validation details.
    std::vector<scctl::ParamSpec> t { scctl::P ("db", scctl::PType::Number).req().range (-96, 96),
                                      scctl::P ("n", scctl::PType::Integer).min (1),
                                      scctl::P ("mode", scctl::PType::String).oneOf ({ "a", "b" }),
                                      scctl::P ("chs", scctl::PType::Array).items (scctl::PType::Integer, 0.0),
                                      scctl::P ("sel", scctl::PType::Range) };
    scctl::Error e;
    CHECK (!scctl::CommandRegistry::validate (t, scctl::obj(), e) && e.details.getProperty ("path", {}) == juce::var ("db"),
           "validate: missing required");
    CHECK (!scctl::CommandRegistry::validate (t, scctl::obj ({ { "db", 200 } }), e), "validate: maximum");
    CHECK (!scctl::CommandRegistry::validate (t, scctl::obj ({ { "db", 1 }, { "n", 1.5 } }), e), "validate: integer");
    CHECK (!scctl::CommandRegistry::validate (t, scctl::obj ({ { "db", 1 }, { "mode", "c" } }), e), "validate: enum");
    CHECK (!scctl::CommandRegistry::validate (t, scctl::obj ({ { "db", 1 }, { "chs", scctl::arr ({ 0, -1 }) } }), e)
               && e.details.getProperty ("path", {}) == juce::var ("chs[1]"), "validate: array item minimum");
    CHECK (!scctl::CommandRegistry::validate (t, scctl::obj ({ { "db", 1 }, { "sel", "some" } }), e), "validate: range string");
    CHECK (!scctl::CommandRegistry::validate (t, scctl::obj ({ { "db", 1 },
               { "sel", scctl::obj ({ { "start", 2 }, { "end", 1 } }) } }), e), "validate: range end < start");
    CHECK (scctl::CommandRegistry::validate (t, scctl::obj ({ { "db", true ? 1.5 : 0 }, { "sel", "all" }, { "confirm", true } }), e),
           "validate: confirm always accepted");
    CHECK (!scctl::CommandRegistry::validate (t, scctl::obj ({ { "db", true } }), e), "validate: bool is not a number");
}

// ---------------------------------------------------------------------------
// testControlJsonCli: line-by-line streaming, exit codes, job timeout.
// ---------------------------------------------------------------------------
static int ctlRunLines (Application& app, const juce::StringArray& lines, juce::StringArray& out,
                        bool* streamedOk = nullptr)
{
    int next = 0, expected = 0;
    bool streamed = true;
    const int rc = scctl::runJsonSession (app,
        [&] (std::string& l) {
            if (next >= lines.size()) return false;
            // Streaming: every earlier request line was answered before the
            // next one is read (blank and option lines get no response).
            if (out.size() < expected)
                streamed = false;
            const auto& t = lines[next++];
            if (t.trim().isNotEmpty() && !t.contains ("continueOnError")) ++expected;
            l = t.toStdString();
            return true;
        },
        [&] (const juce::String& l) { out.add (l); });
    if (streamedOk != nullptr) *streamedOk = streamed;
    return rc;
}

static void testControlJsonCli()
{
    const juce::File wav = ctlMakeWav ("ctl_cli.wav", 1);
    const juce::String open = "{\"id\":\"o\",\"cmd\":\"doc.open\",\"params\":{\"path\":"
                              + juce::JSON::toString (wav.getFullPathName()) + "}}";
    {
        Application app;
        juce::StringArray out;
        bool streamed = false;
        const int rc = ctlRunLines (app, { open,
                                           "{\"id\":2,\"cmd\":\"step.run\",\"params\":{\"select\":\"all\",\"step\":{\"type\":\"fadeIn\"}}}",
                                           "",
                                           "{\"id\":3,\"cmd\":\"debug.sleep\",\"params\":{\"ms\":50}}",
                                           "{\"id\":4,\"cmd\":\"history.list\"}" }, out, &streamed);
        CHECK (rc == 0, "json cli: all ok -> exit code 0");
        CHECK (out.size() == 4, "json cli: one response per request (blank line skipped)");
        CHECK (streamed, "json cli: each line answered before the next is read");
        const auto r3 = juce::JSON::parse (out[2]);
        CHECK ((bool) r3.getProperty ("ok", false) && (int) r3.getProperty ("result", {}).getProperty ("sleptMs", 0) == 50
                   && r3.getProperty ("job", {}).toString().startsWith ("j-"), "json cli: job awaited and inlined");
        CHECK (juce::JSON::parse (out[1]).getProperty ("id", {}) == juce::var (2), "json cli: id echoed");
    }
    {
        Application app;
        juce::StringArray out;
        const int rc = ctlRunLines (app, { "{\"id\":1,\"cmd\":\"nope\"}", "{\"id\":2,\"cmd\":\"app.state\"}" }, out);
        CHECK (rc == 1 && out.size() == 1, "json cli: an error stops the run with exit code 1");
    }
    {
        Application app;
        juce::StringArray out;
        const int rc = ctlRunLines (app, { "{\"continueOnError\":true}", "{\"id\":1,\"cmd\":\"nope\"}",
                                           "{\"id\":2,\"cmd\":\"app.state\"}" }, out);
        CHECK (rc == 1 && out.size() == 2 && (bool) juce::JSON::parse (out[1]).getProperty ("ok", false),
               "json cli: continueOnError goes on, exit code still 1");
    }
    {
        Application app;
        juce::StringArray out;
        const int rc = ctlRunLines (app, { "{\"continueOnError\":true}", "this is not json", "{\"id\":2,\"cmd\":\"app.state\"}" }, out);
        CHECK (rc == 2 && out.size() == 2, "json cli: unreadable JSON -> exit code 2");
    }
    {
        Application app;
        juce::StringArray out;
        const juce::String big = "{\"id\":1,\"cmd\":\"app.state\",\"params\":{\"x\":\""
                                 + juce::String::repeatedString ("a", scctl::kMaxLineBytes) + "\"}}";
        const int rc = ctlRunLines (app, { big }, out);
        CHECK (rc == 2 && out.size() == 1 && out[0].contains ("1 MiB"), "json cli: line > 1 MiB -> exit code 2");
    }
    {
        // LongRunning past its timeoutMs: cancelled and reported as timeout.
        Application app;
        juce::StringArray out;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const int rc = ctlRunLines (app, { "{\"id\":1,\"cmd\":\"debug.sleep\",\"params\":{\"ms\":20000},\"timeoutMs\":150}" }, out);
        const double el = juce::Time::getMillisecondCounterHiRes() - t0;
        const auto r = juce::JSON::parse (out[0]);
        CHECK (rc == 1 && r.getProperty ("error", {}).getProperty ("code", {}).toString() == "timeout",
               "json cli: job past timeoutMs -> timeout");
        CHECK ((bool) r.getProperty ("error", {}).getProperty ("details", {}).getProperty ("finished", false),
               "json cli: timed-out job was cancelled and finished");
        CHECK (el < 5000.0, "json cli: timeout honoured promptly (" + juce::String (el, 0) + " ms)");
    }

    // The real executable (app suite only): --json <file> --json-out <file>,
    // exit codes 0/1/2, unknown flag -> 2.
    const juce::File exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    if (exe.getFileNameWithoutExtension() != "SpectroCraft")
    {
        scSkip ("json cli subprocess: not running inside the SpectroCraft executable");
        return;
    }
    auto runExe = [&] (const juce::StringArray& extra, juce::String* outText, int timeoutMs = 60000) {
        juce::StringArray cmd;
        cmd.add (exe.getFullPathName());
        cmd.addArray (extra);
        juce::ChildProcess p;
        if (!p.start (cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) return -1;
        const juce::String text = p.readAllProcessOutput();
        if (!p.waitForProcessToFinish (timeoutMs)) { p.kill(); return -2; }
        if (outText != nullptr) *outText = text;
        return (int) p.getExitCode();
    };
    const juce::File req = makeTempFile ("ctl req with spaces.ndjson");
    const juce::File res = makeTempFile ("ctl res.ndjson");
    req.replaceWithText (open + "\n{\"id\":2,\"cmd\":\"step.run\",\"params\":{\"step\":{\"type\":\"reverse\"}}}\n");
    int rc = runExe ({ "--json", req.getFullPathName(), "--json-out", res.getFullPathName() }, nullptr);
    juce::StringArray lines;
    lines.addLines (res.loadFileAsString().trim());
    CHECK (rc == 0 && lines.size() == 2 && lines[1].contains ("\"ok\":true"),
           "json cli exe: --json file --json-out file, exit code 0 (rc " + juce::String (rc) + ")");
    req.replaceWithText ("{\"id\":1,\"cmd\":\"doc.open\",\"params\":{\"path\":\"/nonexistent/x.wav\"}}\n");
    rc = runExe ({ "--json", req.getFullPathName(), "--json-out", res.getFullPathName() }, nullptr);
    CHECK (rc == 1, "json cli exe: failing request -> exit code 1 (rc " + juce::String (rc) + ")");
    req.replaceWithText ("garbage\n");
    rc = runExe ({ "--json", req.getFullPathName(), "--json-out", res.getFullPathName() }, nullptr);
    CHECK (rc == 2, "json cli exe: unreadable JSON -> exit code 2 (rc " + juce::String (rc) + ")");
    rc = runExe ({ "--definitely-not-a-flag" }, nullptr);
    CHECK (rc == 2, "json cli exe: unknown flag -> exit code 2 (rc " + juce::String (rc) + ")");
    juce::String listed;
    rc = runExe ({ "--list-commands", "--markdown" }, &listed);
    CHECK (rc == 0 && listed.contains ("`step.run`"), "json cli exe: --list-commands --markdown");
}

static void runRegression_control()
{
    testControlDescribe();
    testControlRegistry();
    testControlDocsHistory();
    testControlJsonCli();
}
