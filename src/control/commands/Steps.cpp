// SpectroCraft - AI control: generic action steps (step.run / steps.run / step.validate).
//
// One entry point for the 20 recorded step types (include/Actions.h):
// ActionStep::fromJson -> validateStepParams -> applyActionStep ->
// recordActionStep, with an explicit range ("select") and channel list.
// Lot 6 splits these into typed edit.* / effect.* / gen.* commands.
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "Application.h"
#include "Actions.h"
#include "PythonScripts.h"
#include "control/PathPolicy.h"
#include "control/StepRunner.h"
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
 #include "AI/ModelCatalog.h"
#endif
#include <optional>

namespace sc::control {

namespace {

Outcome invalid (const juce::String& path, const juce::String& expected, const juce::String& msg)
{
    return Outcome::failure (err::invalidParams, msg, obj ({ { "path", path }, { "expected", expected } }));
}

/** The param validateStepParams checks for each type (for the error path). */
const char* requiredParamOf (const juce::String& type)
{
    using namespace sc::actionTypes;
    if (type == kChangeGain)    return "db";
    if (type == kTimeStretch)   return "pct";
    if (type == kResample)      return "rate";
    if (type == kGenSilence || type == kGenNoise || type == kGenTone) return "dur";
    if (type == kFilterEq || type == kEnvelope) return "points";
    if (type == kTranscribe || type == kNoiseSuppress) return "modelId";
    if (type == kPython)        return "script";
    return nullptr;
}

bool isAiType (const juce::String& t)
{
    return t == sc::actionTypes::kNoiseSuppress || t == sc::actionTypes::kTranscribe;
}

bool isLongType (const juce::String& t)
{
    using namespace sc::actionTypes;
    return t == kNoiseSuppress || t == kTranscribe || t == kPython || t == kResample;
}

/** Parses + validates one step. Returns false with `error` set. */
bool parseStep (const juce::var& v, const juce::String& path, sc::ActionStep& out, Outcome& error)
{
    std::string e;
    out = sc::ActionStep::fromJson (v, &e);
    if (out.type.isEmpty())
    {
        error = invalid (path + ".type", "step type string", juce::String (e));
        return false;
    }
    if (!sc::isKnownActionType (out.type))
    {
        error = invalid (path + ".type", "one of the action step types (include/Actions.h)",
                         "unknown step type \"" + out.type + "\"");
        return false;
    }
    if (!out.params.isVoid() && !out.params.isObject())
    {
        error = invalid (path + ".params", "object", "step params must be an object");
        return false;
    }
    if (!sc::validateStepParams (out, &e))
    {
        const char* key = requiredParamOf (out.type);
        error = invalid (key != nullptr ? path + ".params." + key : path + ".params",
                         "see step type " + out.type, juce::String (e));
        return false;
    }
#if !(defined(SC_ENABLE_AI) && SC_ENABLE_AI)
    if (isAiType (out.type))
    {
        error = Outcome::failure (err::unsupportedBuild, out.type + " needs an AI-enabled build (SPECTRO_ENABLE_AI)");
        return false;
    }
#endif
    return true;
}

/** Applies "select" and "channels"; returns the channel list to restore
    afterwards (empty optional = leave as is). */
bool applyScope (Context& ctx, std::optional<std::vector<int>>& restoreChannels, Outcome& error)
{
    Application& app = ctx.app;
    const double dur = app.audioEngine().duration();
    const auto sel = ctx.get ("select");
    if (sel.isString())   // "all" (validated)
        app.selectAll();
    else if (sel.isObject())
    {
        const double a = (double) sel.getProperty ("start", 0.0);
        const double b = (double) sel.getProperty ("end", 0.0);
        if (a >= dur)
        {
            error = invalid ("select.start", "< " + juce::String (dur), "select.start is beyond the end of the file");
            return false;
        }
        // An empty range would silently mean "whole file" for most effects.
        if (b <= a)
        {
            error = invalid ("select.end", "> start", "select is empty; use \"all\" for the whole file");
            return false;
        }
        app.selectionRow = -1;
        app.setSelection ({ a, std::min (b, dur) });
    }

    const auto ch = ctx.get ("channels");
    if (ch.isArray())
    {
        const int n = app.audioEngine().numChannels();
        std::vector<int> vis;
        for (int i = 0; i < ch.size(); ++i)
        {
            const int c = (int) ch[i];
            if (c < 0 || c >= n)
            {
                error = invalid ("channels[" + juce::String (i) + "]", "0.." + juce::String (n - 1),
                                 "channel " + juce::String (c) + " does not exist");
                return false;
            }
            if (std::find (vis.begin(), vis.end(), c) == vis.end()) vis.push_back (c);
        }
        if (vis.empty())
        {
            error = invalid ("channels", "non-empty array", "channels must not be empty");
            return false;
        }
        std::sort (vis.begin(), vis.end());
        restoreChannels = app.visibleChannels;
        app.visibleChannels = vis;
        app.syncActiveDoc();
    }
    return true;
}

void restoreScope (Application& app, const std::optional<std::vector<int>>& channels, int chansBefore)
{
    // Edits that changed the channel count manage visibleChannels themselves.
    if (channels && app.audioEngine().numChannels() == chansBefore)
    {
        app.visibleChannels = *channels;
        app.syncActiveDoc();
    }
}

/** Applies one parsed step on the calling thread; returns {type, ok,
    description?, undoDepth?, error?}. Message thread, or a job worker for
    the long types (whose Application methods commit on the message thread). */
juce::var applyOne (Application& app, const sc::ActionStep& s, bool& ok, juce::String& errMsg,
                    std::function<bool(float)> onProgress = nullptr)
{
    FileDoc* d = app.doc();
    const size_t undoBefore = d != nullptr ? d->undo.undoSize() : 0;
    std::string e;
    ok = sc::applyActionStep (app, s, &e, std::move (onProgress));
    auto r = obj ({ { "type", s.type }, { "ok", ok } });
    if (!ok)
    {
        errMsg = juce::String (e);
        set (r, "error", errMsg);
    }
    else
        set (r, "undoBefore", (int) undoBefore);
    return r;
}

/** Message thread, after a successful step: record it like the menus do
    (no-op unless an action is recording; after success so a failed API
    step never lands in a user's action) and describe the new undo entry. */
void finishOne (Application& app, const sc::ActionStep& s, juce::var& r)
{
    if (!(bool) r.getProperty ("ok", false)) return;
    app.recordActionStep (s.type, s.params);
    FileDoc* d = app.doc();
    if (s.type == sc::actionTypes::kTranscribe && d != nullptr)
    {
        // The segments (to the client only: the journal never holds the text).
        juce::Array<juce::var> segs;
        for (const auto& g : d->sctt.segments)
            segs.add (obj ({ { "start", g.startSec }, { "end", g.endSec },
                             { "text", juce::String::fromUTF8 (g.text.c_str()) } }));
        set (r, "segments", segs);
        set (r, "segmentCount", segs.size());
        set (r, "storedInDoc", true);
    }
    const int before = (int) r.getProperty ("undoBefore", -1);
    if (d != nullptr && (int) d->undo.undoSize() != before && d->undo.canUndo())
        set (r, "description", d->undo.lastRecorded().description);
    if (d != nullptr) set (r, "undoDepth", (int) d->undo.undoSize());
    if (auto* o = r.getDynamicObject()) o->removeProperty ("undoBefore");
}

/** Runs the sequence. jc == nullptr: everything on the message thread.
    In a job: long types on the worker, everything else (and all the
    bookkeeping) through jc->onMessageThread - the worker never edits the
    document itself. */
Outcome runSequence (Application& app, const std::vector<sc::ActionStep>& steps, bool stopOnError,
                     bool single, JobControl* jc,
                     const std::optional<std::vector<int>>& restore, int chansBefore,
                     const juce::String& source = {})
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    juce::Array<juce::var> results;
    int applied = 0, failedCount = 0;
    juce::String firstError;
    bool shuttingDown = false;
    for (size_t i = 0; i < steps.size(); ++i)
    {
        const auto& s = steps[i];
        if (jc != nullptr && jc->cancelled())
        {
            if (firstError.isEmpty()) firstError = "cancelled by client";
            ++failedCount;
            break;
        }
        bool ok = false;
        juce::String e;
        juce::var r;
        if (jc != nullptr && isLongType (s.type) && s.type != sc::actionTypes::kResample)
        {
            sc::ActionStep step = s;
            juce::String deviceUsed;
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
            if (s.type == sc::actionTypes::kNoiseSuppress)
            {
                // API calls resolve the device strictly against what the
                // runtime really offers (initialising OpenVINO: worker only);
                // an unavailable device is an error, never a silent fallback.
                const auto dr = sc::ai::resolveAiDevice ("openvino",
                                    s.params.getProperty ("device", "auto").toString().toStdString());
                if (!dr.ok)
                {
                    jc->onMessageThread ([&] { restoreScope (app, restore, chansBefore); });
                    const auto ej = dr.errorJson();
                    return Outcome::failure (ej.getProperty ("code", err::invalidParams).toString(),
                                             ej.getProperty ("message", {}).toString(),
                                             ej.getProperty ("details", {}));
                }
                deviceUsed = juce::String (dr.device);
                auto pc = s.params.isObject() ? juce::var (s.params.getDynamicObject()->clone().release()) : obj();
                set (pc, "device", deviceUsed);
                step.params = pc;
            }
#endif
            r = applyOne (app, step, ok, e, [jc] (float f) { jc->progress (f); return !jc->cancelled(); });   // worker; commits hop
            if (deviceUsed.isNotEmpty()) set (r, "deviceUsed", deviceUsed);
            if (!jc->onMessageThread ([&] { ScopedInvokeSource src (app.control(), source); finishOne (app, s, r); }))
            { shuttingDown = true; break; }
        }
        else if (jc != nullptr)
        {
            if (!jc->onMessageThread ([&] {
                    ScopedInvokeSource src (app.control(), source);
                    r = applyOne (app, s, ok, e);
                    finishOne (app, s, r);
                }))
            { shuttingDown = true; break; }
        }
        else
        {
            r = applyOne (app, s, ok, e);
            finishOne (app, s, r);
        }
        results.add (r);
        if (ok) ++applied;
        else
        {
            ++failedCount;
            if (firstError.isEmpty()) firstError = s.type + ": " + e;
            if (stopOnError) break;
        }
    }

    juce::var sel;
    auto finish = [&] {
        restoreScope (app, restore, chansBefore);
        sel = obj ({ { "start", app.selection.getStart() }, { "end", app.selection.getEnd() } });
    };
    if (jc != nullptr) { if (!shuttingDown) jc->onMessageThread (finish); }
    else finish();
    if (shuttingDown)
        return Outcome::failure (err::shuttingDown, "the application is shutting down");

    const int elapsed = (int) (juce::Time::getMillisecondCounterHiRes() - t0);
    if (single)
    {
        if (results.isEmpty())
            return Outcome::failure (jc != nullptr && jc->cancelled() ? err::cancelled : err::failed, firstError);
        auto r = results[0];
        if (!(bool) r.getProperty ("ok", false))
            return Outcome::failure (err::failed, firstError, obj ({ { "type", steps[0].type } }));
        set (r, "applied", steps[0].type);
        set (r, "elapsedMs", elapsed);
        set (r, "selection", sel);
        return Outcome::success (r);
    }
    auto r = obj ({ { "applied", applied }, { "failed", failedCount }, { "results", results },
                    { "elapsedMs", elapsed } });
    if (failedCount > 0 && (stopOnError || results.size() < (int) steps.size()))
        return Outcome::failure (firstError == "cancelled by client" ? err::cancelled : err::failed, firstError, r);
    return Outcome::success (r);
}

/** python steps: absolute script inside the allowed roots; outside
    scriptFolder() needs confirm (it runs arbitrary code). */
bool checkScripts (Context& ctx, const std::vector<sc::ActionStep>& steps, Outcome& error)
{
    for (const auto& s : steps)
    {
        if (s.type != sc::actionTypes::kPython) continue;
        const juce::String p = s.params.getProperty ("script", {}).toString();
        if (!juce::File::isAbsolutePath (p))
        {
            error = invalid ("step.params.script", "absolute path", "python: script must be an absolute path");
            return false;
        }
        const juce::File f (p);
        const auto chk = checkPath (ctx.app, f, false, ctx.confirmed(), "script");
        if (!chk.ok) { error = chk; return false; }
        const juce::File folder = canonicalFile (sc::py::scriptFolder());
        const juce::File c = canonicalFile (f);
        if (!c.isAChildOf (folder) && !ctx.confirmed())
        {
            error = Outcome::failure (err::confirmRequired,
                                      "python: the script is outside the scripts folder; resend with params.confirm=true",
                                      obj ({ { "script", f.getFullPathName() }, { "scriptFolder", folder.getFullPathName() },
                                             { "hint", "it runs arbitrary code: check it, then pass confirm:true" } }));
            return false;
        }
    }
    return true;
}

/** AI steps: known model; not installed + autoDownload:false ->
    model_not_installed (stat only, message thread). The device preference
    is fixed here (app setting when the step names none). */
bool checkAiSteps (Context& ctx, std::vector<sc::ActionStep>& steps, Outcome& error)
{
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    auto& cat = sc::ai::ModelCatalog::instance();
    for (auto& s : steps)
    {
        if (!isAiType (s.type)) continue;
        const std::string id = s.params.getProperty ("modelId", {}).toString().toStdString();
        const auto* e = cat.find (id);
        const auto fam = s.type == sc::actionTypes::kTranscribe ? sc::ai::family::transcribe : sc::ai::family::noiseSuppress;
        if (e == nullptr || e->family != fam)
        {
            juce::StringArray ids;
            for (const auto& c : cat.entries()) if (c.family == fam) ids.add (c.id);
            error = invalid ("step.params.modelId", "one of [" + ids.joinIntoString (", ") + "]",
                             "unknown " + s.type + " model \"" + juce::String (id) + "\"");
            return false;
        }
        const bool autoDl = !s.params.hasProperty ("autoDownload") || (bool) s.params.getProperty ("autoDownload", true);
        const auto st = cat.status (*e);
        if (!st.installed && !autoDl)
        {
            juce::Array<juce::var> missing;
            for (auto& m : st.missing) missing.add (juce::String (m));
            error = Outcome::failure (err::modelNotInstalled, juce::String (id) + " is not installed",
                                      obj ({ { "modelId", juce::String (id) }, { "missing", missing },
                                             { "sizeBytes", (juce::int64) e->sizeBytes() },
                                             { "hint", "call ai.models.install, or pass autoDownload:true" } }));
            return false;
        }
        if (s.type == sc::actionTypes::kNoiseSuppress && !s.params.hasProperty ("device"))
        {
            auto pc = s.params.isObject() ? juce::var (s.params.getDynamicObject()->clone().release()) : obj();
            set (pc, "device", juce::String (ctx.app.aiDevicePref));
            s.params = pc;
        }
    }
#else
    juce::ignoreUnused (ctx, steps, error);
#endif
    return true;
}

Outcome runSteps (Context& ctx, std::vector<sc::ActionStep> steps, bool stopOnError, bool single)
{
    Outcome bad;
    if (!checkScripts (ctx, steps, bad)) return bad;
    if (!checkAiSteps (ctx, steps, bad)) return bad;

    std::optional<std::vector<int>> restore;
    const int chans = ctx.app.audioEngine().numChannels();
    if (!applyScope (ctx, restore, bad)) return bad;

    bool anyLong = false;
    for (const auto& s : steps)
        if (isLongType (s.type) && s.type != sc::actionTypes::kResample) anyLong = true;
    if (!anyLong)
        return runSequence (ctx.app, steps, stopOnError, single, nullptr, restore, chans);

    // Long steps (AI, Python) run in a job: the message thread stays free
    // (repaints, other clients' reads) and the client can follow / cancel.
    // The job addresses the ACTIVE document: keep a temporary switch.
    ctx.keepDocSwitch = true;
    Application* app = &ctx.app;
    const juce::String title = single ? "Running " + steps[0].type : "Running " + juce::String ((int) steps.size()) + " steps";
    const auto id = startJob (ctx, ctx.cmd.isNotEmpty() ? ctx.cmd : juce::String (single ? "step.run" : "steps.run"), title, true,
        [app, steps, stopOnError, single, restore, chans, source = ctx.source] (JobControl& jc) {
            return runSequence (*app, steps, stopOnError, single, &jc, restore, chans, source);
        });
    return Outcome::job (id);
}

Outcome validateCmd (Context& ctx)
{
    sc::ActionStep s;
    Outcome bad;
    if (!parseStep (ctx.get ("step"), "step", s, bad)) return bad;
    return Outcome::success (obj ({ { "valid", true }, { "type", s.type }, { "mutates", true },
                                    { "longRunning", isLongType (s.type) && s.type != sc::actionTypes::kResample },
                                    { "aiOnly", isAiType (s.type) } }));
}

Outcome runCmd (Context& ctx)
{
    sc::ActionStep s;
    Outcome bad;
    if (!parseStep (ctx.get ("step"), "step", s, bad)) return bad;
    return runSteps (ctx, { s }, true, true);
}

Outcome runManyCmd (Context& ctx)
{
    const auto list = ctx.get ("steps");
    std::vector<sc::ActionStep> steps;
    for (int i = 0; i < list.size(); ++i)
    {
        sc::ActionStep s;
        Outcome bad;
        if (!parseStep (list[i], "steps[" + juce::String (i) + "]", s, bad)) return bad;
        steps.push_back (s);
    }
    if (steps.empty()) return invalid ("steps", "non-empty array", "steps must not be empty");
    return runSteps (ctx, std::move (steps), ctx.flag ("stopOnError", true), false);
}

std::vector<ParamSpec> scopeParams()
{
    return { P ("select", PType::Range).describe ("\"all\", or {start, end} in seconds (default: current selection)"),
             P ("channels", PType::Array).items (PType::Integer, 0.0)
                 .describe ("0-based channels the step applies to (default: the visible ones)") };
}

ParamSpec stepParam()
{
    return P ("step", PType::Object).req()
        .props ({ P ("type", PType::String).req().describe ("Step type, e.g. changeGain, fadeOut, envelope"),
                  P ("params", PType::Object).describe ("Step parameters (see app.describe category step)"),
                  P ("prompt", PType::Boolean).describe ("Ignored here (interactive replay only)") })
        .describe ("{type, params}");
}

} // namespace

Outcome runStepsCommand (Context& ctx, std::vector<sc::ActionStep> steps, bool stopOnError, bool single)
{
    return runSteps (ctx, std::move (steps), stopOnError, single);
}

std::vector<ParamSpec> stepScopeParams()
{
    return scopeParams();
}

void registerStepCommands (CommandRegistry& r)
{
    {
        auto params = scopeParams();
        params.insert (params.begin(), stepParam());
        r.add ({ "step.run", "Apply one action step (any of the 20 recorded types) on an explicit range", "step",
                 params, {}, Mutates | NeedsDoc, {}, runCmd });
    }
    {
        auto params = scopeParams();
        params.insert (params.begin(), P ("steps", PType::Array).req().items (PType::Object)
                                           .describe ("[{type, params}, ...] - one history entry per step"));
        params.push_back (P ("stopOnError", PType::Boolean).def (true));
        r.add ({ "steps.run", "Apply several action steps in order", "step",
                 params, {}, Mutates | NeedsDoc, {}, runManyCmd });
    }
    r.add ({ "step.validate", "Check a step without touching any document", "step",
             { stepParam() }, {}, Query, {}, validateCmd });
}

} // namespace sc::control
