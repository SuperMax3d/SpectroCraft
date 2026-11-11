// SpectroCraft - AI control lot 7: python.run (hardened bridge, result.json),
// ai.stems.run / ai.superRes.run (audio.cpp services, Windows only).
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "control/PathPolicy.h"
#include "control/StepRunner.h"
#include "Application.h"
#include "PythonScripts.h"
#include "AI/StemService.h"
#include "AI/SuperResService.h"

namespace sc::control {

namespace {

Outcome invalid (const juce::String& path, const juce::String& expected, const juce::String& msg)
{
    return Outcome::failure (err::invalidParams, msg, obj ({ { "path", path }, { "expected", expected } }));
}

juce::String tail (const juce::String& s, int maxChars = 4000)
{
    return s.length() <= maxChars ? s : "..." + s.substring (s.length() - maxChars);
}

/** "select": "all" / {start, end} / absent (= current selection). */
bool applySelect (Context& ctx, Outcome& bad)
{
    const auto sel = ctx.get ("select");
    const double dur = ctx.app.audioEngine().duration();
    if (sel.isString()) { ctx.app.selectAll(); return true; }
    if (!isObj (sel)) return true;
    const double a = (double) sel.getProperty ("start", 0.0), b = std::min (dur, (double) sel.getProperty ("end", 0.0));
    if (a >= dur || b <= a) { bad = invalid ("select", "a non-empty range inside the file", "empty or out-of-file range"); return false; }
    ctx.app.selectionRow = -1;
    ctx.app.setSelection ({ a, b });
    return true;
}

bool channelsOf (Context& ctx, std::vector<int>& out, Outcome& bad)
{
    out.clear();
    const auto v = ctx.get ("channels");
    const int n = ctx.app.audioEngine().numChannels();
    if (!v.isArray()) { out = ctx.app.visibleChannels; if (out.empty()) for (int c = 0; c < n; ++c) out.push_back (c); return true; }
    for (int i = 0; i < v.size(); ++i)
    {
        const int c = (int) v[i];
        if (c < 0 || c >= n) { bad = invalid ("channels[" + juce::String (i) + "]", "0.." + juce::String (n - 1), "no such channel"); return false; }
        if (std::find (out.begin(), out.end(), c) == out.end()) out.push_back (c);
    }
    std::sort (out.begin(), out.end());
    if (out.empty()) { bad = invalid ("channels", "non-empty", "no channel"); return false; }
    return true;
}

// ---------------------------------------------------------------- python.run

Outcome pythonRunCmd (Context& ctx)
{
    const juce::String p = ctx.str ("script");
    if (!juce::File::isAbsolutePath (p)) return invalid ("script", "absolute .py path", "script must be an absolute path");
    const juce::File script (p);
    juce::String reason;
    juce::File resolved;
    if (!sc::py::isUnderScriptFolder (script, &reason, sc::py::scriptFolder(), &resolved))
    {
        if (reason == "not_found") return Outcome::failure (err::failed, "script not found: " + script.getFullPathName());
        if (reason == "not_python") return invalid ("script", "a .py file", "not a Python script");
        // Outside the scripts folder: arbitrary code -> explicit confirmation,
        // and still inside the allowed roots.
        if (!ctx.confirmed())
            return Outcome::failure (err::confirmRequired, "the script is outside the scripts folder; resend with params.confirm=true",
                                     obj ({ { "reason", reason }, { "script", script.getFullPathName() },
                                            { "scriptFolder", sc::py::scriptFolder().getFullPathName() },
                                            { "hint", "it runs arbitrary code: check it, then pass confirm:true" } }));
        const auto chk = checkPath (ctx.app, script, false, true, "script");
        if (!chk.ok) return chk;
        resolved = canonicalFile (script);
    }

    Outcome bad;
    if (!applySelect (ctx, bad)) return bad;
    std::vector<int> chans;
    if (!channelsOf (ctx, chans, bad)) return bad;
    const auto restore = ctx.app.visibleChannels;
    const int chansBefore = ctx.app.audioEngine().numChannels();
    ctx.app.visibleChannels = chans;
    ctx.app.syncActiveDoc();

    const int timeoutMs = ctx.integer ("timeoutMs", 300000);
    const bool addMarkers = ctx.flag ("addMarkers", false);
    ctx.keepDocSwitch = true;   // the worker works on the active document
    Application* app = &ctx.app;
    const juce::String source = ctx.source, cmd = ctx.cmd;
    const juce::var params = ctx.params;
    const auto id = startJob (ctx, "python.run", "Running " + script.getFileName(), true,
        [=] (JobControl& jc) -> Outcome {
            std::string pyCmd, e;
            if (!sc::py::findPython (&pyCmd, &e))   // off the message thread (it may launch processes)
            {
                jc.onMessageThread ([&] { app->visibleChannels = restore; app->syncActiveDoc(); });
                return Outcome::failure (err::failed, "no Python interpreter found: " + juce::String (e));
            }
            sc::py::RunOptions ro;
            ro.timeoutMs = timeoutMs;
            sc::py::ScopedScriptRunCapture cap (ro);
            sc::undoctx::setPending (sc::undoctx::Op { cmd, params, source });   // the commit hop records the patch
            const bool ok = app->runPythonScript (resolved, resolved.loadFileAsString(), pyCmd,
                                                  [&jc] (float f) { jc.progress (f); return !jc.cancelled(); }, &e);
            const auto& rep = cap.report();
            auto res = obj ({ { "script", resolved.getFullPathName() }, { "scriptSha256", rep.scriptSha256 },
                              { "interpreter", rep.interpreter }, { "exitCode", rep.process.exitCode },
                              { "timedOut", rep.process.timedOut }, { "elapsedMs", (int) rep.process.elapsedMs },
                              { "audioChanged", rep.audioChanged }, { "outWavPresent", rep.outWavPresent },
                              { "stdoutTail", tail (rep.process.stdoutText) }, { "stderrTail", tail (rep.process.stderrText) } });
            int markersAdded = 0;
            if (rep.result.present && rep.result.error.isEmpty())
            {
                set (res, "resultJson", rep.result.raw);
                juce::Array<juce::var> mk;
                for (const auto& m : rep.result.markers)
                    mk.add (obj ({ { "label", m.label }, { "time", m.docTime }, { "end", m.docEnd } }));
                set (res, "markers", mk);
                set (res, "markerCount", mk.size());
            }
            jc.onMessageThread ([&] {
                if (app->audioEngine().numChannels() == chansBefore) { app->visibleChannels = restore; app->syncActiveDoc(); }
                if (ok && addMarkers && app->doc() != nullptr)
                    for (const auto& m : rep.result.markers)
                    {
                        if (m.docEnd > m.docTime) app->markers().addRegionMarker (m.docTime, m.docEnd, m.label);
                        else app->markers().addPointMarker (m.docTime, m.label);
                        ++markersAdded;
                    }
                if (markersAdded > 0 && app->onStateChange) app->onStateChange();
            });
            set (res, "markersAdded", markersAdded);
            if (!ok)
            {
                const juce::String code = rep.process.timedOut ? juce::String (err::timeout)
                                        : jc.cancelled() ? juce::String (err::cancelled) : juce::String (err::failed);
                return Outcome::failure (code, juce::String (e), res);
            }
            return Outcome::success (res);
        });
    return Outcome::job (id);
}

// ---------------------------------------------------------------- stems / super resolution

/** Reads the selected range (or the whole file) of `chans` on the message thread. */
bool readInput (Application& app, const std::vector<int>& chans, std::vector<std::vector<float>>& out,
                double& startSecs, Outcome& bad)
{
    auto& eng = app.audioEngine();
    const double rate = eng.sampleRate();
    auto range = app.selection.getLength() > 0 ? app.selection : juce::Range<double> (0.0, eng.duration());
    const auto s0 = (int64_t) std::floor (range.getStart() * rate);
    const auto s1 = std::min (eng.numSamples(), (int64_t) std::ceil (range.getEnd() * rate));
    if (s1 <= s0) { bad = invalid ("select", "non-empty range", "empty range"); return false; }
    if ((s1 - s0) * (int64_t) chans.size() > (int64_t) 2000000000) { bad = invalid ("select", "a shorter range", "range too large"); return false; }
    out.assign (chans.size(), std::vector<float> ((size_t) (s1 - s0)));
    for (size_t i = 0; i < chans.size(); ++i)
        if (!eng.readSamples (chans[i], s0, s1 - s0, out[i].data()))
        {
            bad = Outcome::failure (err::failed, "could not read the audio");
            return false;
        }
    startSecs = (double) s0 / rate;
    return true;
}

int outputModeOf (const juce::String& s) { return s == "replace" ? 2 : s == "append" ? 3 : 1; }

juce::File pickModel (const std::vector<juce::File>& candidates, const juce::String& want)
{
    for (const auto& f : candidates)
        if (want.isEmpty() || f.getFileName() == want || f.getFullPathName() == want) return f;
    return {};
}

Outcome stemsCmd (Context& ctx)
{
    sc::StemService::Request req;
    const juce::String mode = ctx.str ("mode", "samAudio");
    req.mode = mode == "htdemucs6" ? sc::StemService::Mode::HtDemucs6
             : mode == "htdemucsFt" ? sc::StemService::Mode::HtDemucsFt
             : mode == "whineHiss" ? sc::StemService::Mode::HtWhineHiss
             : mode == "whineHissClickBump" ? sc::StemService::Mode::HtWhineHissClickBump
             : sc::StemService::Mode::SamAudio;
    const juce::String model = ctx.str ("model");
    req.model = req.mode == sc::StemService::Mode::SamAudio ? pickModel (sc::acpp::listModels(), model)
              : req.mode == sc::StemService::Mode::HtDemucs6 ? pickModel (sc::acpp::listDemucs6Models(), model)
              : req.mode == sc::StemService::Mode::HtWhineHiss ? pickModel (sc::acpp::listWhineHissModels(), model)
              : req.mode == sc::StemService::Mode::HtWhineHissClickBump ? pickModel (sc::acpp::listWhineHissClickBumpModels(), model)
              : sc::acpp::demucsFtModelDir();
    req.backend = ctx.str ("backend", ctx.state.audiocppBackend.isNotEmpty() ? ctx.state.audiocppBackend : juce::String ("cpu"));
    req.extractFromOriginal = ctx.flag ("extractFromOriginal", true);
    const auto ds = ctx.get ("descriptions");
    for (int i = 0; ds.isArray() && i < ds.size(); ++i)
    {
        sc::StemService::Description d;
        d.text = ds[i].getProperty ("text", {}).toString();
        const auto an = ds[i].getProperty ("anchors", {});
        for (int k = 0; an.isArray() && k < an.size(); ++k)
            d.anchors.push_back ({ (bool) an[k].getProperty ("include", true),
                                   (double) an[k].getProperty ("start", 0.0), (double) an[k].getProperty ("end", 0.0) });
        req.descriptions.push_back (d);
    }
    Outcome bad;
    if (!applySelect (ctx, bad)) return bad;
    std::vector<int> chans;
    if (!channelsOf (ctx, chans, bad)) return bad;
    req.sourceRate = ctx.app.audioEngine().sampleRate();
    if (!readInput (ctx.app, chans, req.input, req.extStartSecs, bad)) return bad;
    juce::String code;
    const juce::String why = sc::StemService::validate (req, &code);
    if (why.isNotEmpty())
        return Outcome::failure (code == "binary_missing" || code == "model_missing" ? err::modelNotInstalled : err::invalidParams,
                                 why, obj ({ { "code", code }, { "path", code == "no_description" ? "descriptions" : code == "model_missing" ? "model" : "mode" },
                                             { "hint", "ai.models.list shows the audio.cpp assets" } }));
    const int outMode = outputModeOf (ctx.str ("output", "newDoc"));
    Application* app = &ctx.app;
    FileDoc* doc = ctx.doc;
    const juce::int64 ver = (juce::int64) doc->engine.contentVersion();
    const juce::String name = doc->name + " - stems";
    const juce::String source = ctx.source;
    const juce::var params = ctx.params;
    ctx.keepDocSwitch = true;
    const auto id = startJob (ctx, "ai.stems.run", "Separating stems", true,
        [=] (JobControl& jc) mutable -> Outcome {
            auto r = sc::StemService::run (req, [&jc] (double f, const juce::String& st) { if (f >= 0) jc.progress (f, st); else if (st.isNotEmpty()) jc.progress (-1.0, st); },
                                           [&jc] { return jc.cancelled(); });
            if (!r.ok)
                return Outcome::failure (r.cancelled ? err::cancelled : r.errorCode == "unsupported_platform" ? err::unsupportedPlatform : err::failed,
                                         r.error, obj ({ { "code", r.errorCode }, { "detail", tail (r.detail, 2000) } }));
            juce::Array<juce::var> stems;
            for (const auto& s : r.stems)
                stems.add (obj ({ { "name", s.name }, { "rate", s.rate }, { "channels", s.channels }, { "frames", (juce::int64) s.frames } }));
            Outcome o;
            jc.onMessageThread ([&] {
                // Same rule as the window: the source changed meanwhile -> a new document.
                int mode = outMode;
                if (!app->isDocLoaded (doc) || (juce::int64) doc->engine.contentVersion() != ver) mode = 1;
                std::string e;
                ScopedInvokeSource src (app->control(), source, "ai.stems.run", params);
                if (!app->applyStemSeparationResult (r.merged, mode, req.extStartSecs, name, &e))
                    o = Outcome::failure (err::failed, juce::String (e));
                else
                    o = Outcome::success (obj ({ { "stems", stems }, { "output", mode == 1 ? "newDoc" : mode == 2 ? "replace" : "append" } }));
            });
            return o;
        });
    return Outcome::job (id);
}

Outcome superResCmd (Context& ctx)
{
    sc::SuperResService::Request req;
    const juce::String mode = ctx.str ("mode", "flashsr");
    req.mode = mode == "audiosr" ? sc::SuperResService::Mode::AudioSR : sc::SuperResService::Mode::FlashSR;
    req.model = req.mode == sc::SuperResService::Mode::FlashSR ? sc::acpp::flashSrModelFile()
                                                               : pickModel (sc::acpp::listAudioSRModels(), ctx.str ("model"));
    req.backend = ctx.str ("backend", ctx.state.audiocppBackend.isNotEmpty() ? ctx.state.audiocppBackend : juce::String ("cpu"));
    const int outMode = outputModeOf (ctx.str ("output", "newDoc"));
    req.resampleToSourceRate = outMode == 3;   // appending needs the document's rate
    Outcome bad;
    if (!applySelect (ctx, bad)) return bad;
    std::vector<int> chans;
    if (!channelsOf (ctx, chans, bad)) return bad;
    double start = 0.0;
    req.sourceRate = ctx.app.audioEngine().sampleRate();
    if (!readInput (ctx.app, chans, req.input, start, bad)) return bad;
    juce::String code;
    const juce::String why = sc::SuperResService::validate (req, &code);
    if (why.isNotEmpty())
        return Outcome::failure (code == "binary_missing" || code == "model_missing" ? err::modelNotInstalled : err::invalidParams,
                                 why, obj ({ { "code", code } }));
    Application* app = &ctx.app;
    FileDoc* doc = ctx.doc;
    const juce::int64 ver = (juce::int64) doc->engine.contentVersion();
    const juce::String name = doc->name + " - super resolution";
    const juce::String source = ctx.source;
    const juce::var params = ctx.params;
    ctx.keepDocSwitch = true;
    const auto id = startJob (ctx, "ai.superRes.run", "Super resolution", true,
        [=] (JobControl& jc) mutable -> Outcome {
            auto r = sc::SuperResService::run (req, [&jc] (double f, const juce::String& st) { jc.progress (f, st); },
                                               [&jc] { return jc.cancelled(); });
            if (!r.ok)
                return Outcome::failure (r.cancelled ? err::cancelled : r.errorCode == "unsupported_platform" ? err::unsupportedPlatform : err::failed,
                                         r.error, obj ({ { "code", r.errorCode }, { "detail", tail (r.detail, 2000) } }));
            Outcome o;
            jc.onMessageThread ([&] {
                int mode = outMode;
                if (!app->isDocLoaded (doc) || (juce::int64) doc->engine.contentVersion() != ver) mode = 1;
                std::string e;
                ScopedInvokeSource src (app->control(), source, "ai.superRes.run", params);
                if (!app->applySuperResolutionResult (r.merged, mode, start, name, &e))
                    o = Outcome::failure (err::failed, juce::String (e));
                else
                    o = Outcome::success (obj ({ { "toolRate", r.toolRate }, { "output", mode == 1 ? "newDoc" : mode == 2 ? "replace" : "append" } }));
            });
            return o;
        });
    return Outcome::job (id);
}

} // namespace

void registerPythonCommands (CommandRegistry& r)
{
    auto scope = stepScopeParams();
    auto py = scope;
    py.insert (py.begin(), {
        P ("script", PType::String).req().describe ("Absolute path of the .py script (scripts folder, else confirm)"),
        P ("timeoutMs", PType::Integer).range (1000, 3600000).def (300000).describe ("Kills the whole process tree beyond it"),
        P ("addMarkers", PType::Boolean).def (false).describe ("Add the markers of the script's result.json to the document"),
        P ("confirm", PType::Boolean).describe ("Required for a script outside the scripts folder") });
    r.add ({ "python.run", "Run a Python script on a range (stdin closed, timeout, SHA-256 journaled, result.json returned)", "python",
             py, {}, Mutates | NeedsDoc | LongRunning, { "python" }, pythonRunCmd });

    auto st = scope;
    st.insert (st.begin(), {
        P ("mode", PType::String).oneOf ({ "samAudio", "htdemucs6", "htdemucsFt", "whineHiss", "whineHissClickBump" }).def ("samAudio").describe ("Separation family; whineHiss / whineHissClickBump need stereo input (mono is duplicated, >2 channels run per channel)"),
        P ("descriptions", PType::Array).items (PType::Object).describe ("samAudio: [{text, anchors:[{start, end, include}]}] (document seconds)"),
        P ("model", PType::String).describe ("Model file name (default: the first installed)"),
        P ("backend", PType::String).oneOf ({ "cpu", "cuda", "vulkan", "metal" }).describe ("Default: ai.setDevice audiocpp, else cpu"),
        P ("extractFromOriginal", PType::Boolean).def (true),
        P ("output", PType::String).oneOf ({ "newDoc", "replace", "append" }).def ("newDoc") });
    r.add ({ "ai.stems.run", "Stem separation with audio.cpp (Windows x64, macOS, Linux x64), as a job", "ai",
             st, {}, Mutates | NeedsDoc | LongRunning | NeedsAudioCpp, {}, stemsCmd });

    auto sr = scope;
    sr.insert (sr.begin(), {
        P ("mode", PType::String).oneOf ({ "flashsr", "audiosr" }).def ("flashsr"),
        P ("model", PType::String).describe ("AudioSR model file name (default: the first installed)"),
        P ("backend", PType::String).oneOf ({ "cpu", "cuda", "vulkan", "metal" }).describe ("Default: ai.setDevice audiocpp, else cpu"),
        P ("output", PType::String).oneOf ({ "newDoc", "replace", "append" }).def ("newDoc") });
    r.add ({ "ai.superRes.run", "Super resolution with audio.cpp (Windows x64, macOS, Linux x64), as a job", "ai",
             sr, {}, Mutates | NeedsDoc | LongRunning | NeedsAudioCpp, {}, superResCmd });
}

} // namespace sc::control
