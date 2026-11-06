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

/** Runs one parsed step; returns {type, ok, description?, error?}. */
juce::var runOne (Context& ctx, const sc::ActionStep& s, bool& ok, juce::String& errMsg)
{
    Application& app = ctx.app;
    FileDoc* d = app.doc();
    const size_t undoBefore = d != nullptr ? d->undo.undoSize() : 0;
    std::string e;
    ok = sc::applyActionStep (app, s, &e);
    auto r = obj ({ { "type", s.type }, { "ok", ok } });
    if (ok)
    {
        // Same recording as the menus (no-op unless an action is recording);
        // after success so a failed API step never lands in a user's action.
        app.recordActionStep (s.type, s.params);
        if (d != nullptr && d->undo.undoSize() != undoBefore && d->undo.canUndo())
            set (r, "description", d->undo.lastRecorded().description);
        if (d != nullptr) set (r, "undoDepth", (int) d->undo.undoSize());
    }
    else
    {
        errMsg = juce::String (e);
        set (r, "error", errMsg);
    }
    return r;
}

Outcome validateCmd (Context& ctx)
{
    sc::ActionStep s;
    Outcome bad;
    if (!parseStep (ctx.get ("step"), "step", s, bad)) return bad;
    return Outcome::success (obj ({ { "valid", true }, { "type", s.type }, { "mutates", true },
                                    { "longRunning", isLongType (s.type) }, { "aiOnly", isAiType (s.type) } }));
}

Outcome runCmd (Context& ctx)
{
    sc::ActionStep s;
    Outcome bad;
    if (!parseStep (ctx.get ("step"), "step", s, bad)) return bad;

    std::optional<std::vector<int>> restore;
    const int chans = ctx.app.audioEngine().numChannels();
    if (!applyScope (ctx, restore, bad)) return bad;

    const double t0 = juce::Time::getMillisecondCounterHiRes();
    bool ok = false;
    juce::String e;
    auto r = runOne (ctx, s, ok, e);
    restoreScope (ctx.app, restore, chans);
    if (!ok)
        return Outcome::failure (err::failed, s.type + ": " + e, obj ({ { "type", s.type } }));
    set (r, "applied", s.type);
    set (r, "elapsedMs", (int) (juce::Time::getMillisecondCounterHiRes() - t0));
    set (r, "selection", obj ({ { "start", ctx.app.selection.getStart() }, { "end", ctx.app.selection.getEnd() } }));
    return Outcome::success (r);
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

    std::optional<std::vector<int>> restore;
    const int chans = ctx.app.audioEngine().numChannels();
    Outcome bad;
    if (!applyScope (ctx, restore, bad)) return bad;

    const bool stopOnError = ctx.flag ("stopOnError", true);
    juce::Array<juce::var> results;
    int applied = 0, failedCount = 0;
    juce::String firstError;
    for (size_t i = 0; i < steps.size(); ++i)
    {
        bool ok = false;
        juce::String e;
        results.add (runOne (ctx, steps[i], ok, e));
        if (ok) ++applied;
        else
        {
            ++failedCount;
            if (firstError.isEmpty()) firstError = steps[i].type + ": " + e;
            if (stopOnError) break;
        }
    }
    restoreScope (ctx.app, restore, chans);
    auto r = obj ({ { "applied", applied }, { "failed", failedCount }, { "results", results } });
    if (failedCount > 0 && stopOnError)
        return Outcome::failure (err::failed, firstError, r);
    return Outcome::success (r);
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
