// SpectroCraft - AI control: AI commands (ai.*) and transcription.get.
//
// Thin wrappers: models / devices come from sc::ai::ModelCatalog (engine-
// agnostic, it will survive the planned migration of all AI to audio.cpp);
// noise suppression and transcription run through the shared step runner
// (jobs, select/channels, strict device resolution). REPORT §6.5.
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "control/PathPolicy.h"
#include "control/StepRunner.h"
#include "Application.h"
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
 #include "AI/ModelCatalog.h"
 #include "AI/OvRuntime.h"
#endif

namespace sc::control {

namespace {

Outcome invalid (const juce::String& path, const juce::String& expected, const juce::String& msg)
{
    return Outcome::failure (err::invalidParams, msg, obj ({ { "path", path }, { "expected", expected } }));
}

/** Copies ctx.params minus the scope keys into a step params object. */
juce::var stepParamsFrom (Context& ctx, std::initializer_list<const char*> keys)
{
    auto p = obj();
    for (const char* k : keys)
        if (ctx.has (k)) set (p, k, ctx.get (k));
    return p;
}

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
Outcome fromCatalog (const sc::ai::CatalogOutcome& o)
{
    if (o.ok) return Outcome::success (o.result.isVoid() ? obj() : o.result);
    return Outcome::failure (juce::String (o.code.empty() ? std::string (err::failed) : o.code),
                             juce::String (o.message), o.details);
}

Outcome modelsListCmd (Context&)
{
    return Outcome::success (sc::ai::ModelCatalog::instance().listJson());
}

Outcome modelJob (Context& ctx, bool install)
{
    const std::string id = ctx.str ("modelId").toStdString();
    auto& cat = sc::ai::ModelCatalog::instance();
    sc::ai::CatalogOutcome plan;
    if (install)
        plan = cat.installPlan (id);
    else if (cat.find (id) == nullptr)
        return invalid ("modelId", "a modelId from ai.models.list", "unknown model \"" + juce::String (id) + "\"");
    if (install && !plan.ok) return fromCatalog (plan);

    const auto jobId = startJob (ctx, install ? "ai.models.install" : "ai.models.verify",
                                 (install ? "Installing " : "Verifying ") + juce::String (id), false,
        [id, install] (JobControl& jc) {
            auto& c = sc::ai::ModelCatalog::instance();
            auto progress = [&jc] (float f, const std::string& st) {
                jc.progress (f, juce::String (st));
                return !jc.cancelled();
            };
            return fromCatalog (install ? c.install (id, progress) : c.verify (id, progress));
        });
    auto r = Outcome::job (jobId);
    if (install && isObj (plan.result))
        set (r.result, "sizeBytes", plan.result.getProperty ("sizeBytes", 0));
    return r;
}

Outcome modelsInstallCmd (Context& ctx) { return modelJob (ctx, true); }
Outcome modelsVerifyCmd (Context& ctx)  { return modelJob (ctx, false); }

Outcome modelsRemoveCmd (Context& ctx)
{
    const std::string id = ctx.str ("modelId").toStdString();
    auto& cat = sc::ai::ModelCatalog::instance();
    if (cat.find (id) == nullptr)
        return invalid ("modelId", "a modelId from ai.models.list", "unknown model \"" + juce::String (id) + "\"");
    // A job (ours) that uses this model?
    const auto running = ctx.state.jobs.list (false);
    for (int i = 0; i < running.size(); ++i)
    {
        const auto p = running[i].getProperty ("params", {});
        const auto step = p.getProperty ("step", {}).getProperty ("params", {});
        if (p.getProperty ("modelId", {}).toString() == juce::String (id)
            || step.getProperty ("modelId", {}).toString() == juce::String (id))
            return Outcome::failure (err::busy, "a running job uses " + juce::String (id),
                                     obj ({ { "job", running[i].getProperty ("job", {}) } }));
    }
    if (!ctx.confirmed())
    {
        const auto prev = cat.removalPreview (id);
        if (!prev.ok) return fromCatalog (prev);
        return Outcome::failure (err::confirmRequired,
                                 prev.result.getProperty ("message", "Removes the model files").toString(),
                                 obj ({ { "modelId", juce::String (id) }, { "hint", "pass confirm:true" },
                                        { "preview", prev.result } }));
    }
    return fromCatalog (cat.remove (id));
}

Outcome devicesCmd (Context& ctx)
{
    const std::string pref = ctx.app.aiDevicePref;
    if (!ctx.flag ("init", false))
    {
        auto r = sc::ai::devicesJson (pref, false);
        if (isObj (r) && ctx.state.audiocppBackend.isNotEmpty())
            set (r, "audiocppPreference", ctx.state.audiocppBackend);
        return Outcome::success (r);
    }
    // Initialising OpenVINO loads the runtime: a job.
    const auto id = startJob (ctx, "ai.devices", "Initialising the AI runtime", false,
        [pref] (JobControl& jc) {
            jc.progress (-1.0, "loading OpenVINO");
            return Outcome::success (sc::ai::devicesJson (pref, true));
        });
    return Outcome::job (id);
}

Outcome setDeviceCmd (Context& ctx)
{
    const juce::String engine = ctx.str ("engine", "openvino");
    const std::string device = ctx.str ("device").toStdString();
    const bool persist = ctx.flag ("persist", false);
    if (engine == "audiocpp")
    {
        std::string note;
        const auto avail = sc::ai::availableDevices ("audiocpp", false, &note);
        const auto r = sc::ai::resolveAiDevice ("audiocpp", device, avail);
        if (!r.ok)
        {
            const auto ej = r.errorJson();
            return Outcome::failure (ej.getProperty ("code", err::invalidParams).toString(),
                                     ej.getProperty ("message", {}).toString(), ej.getProperty ("details", {}));
        }
        ctx.state.audiocppBackend = juce::String (r.requested);   // session preference (no persisted setting yet)
        auto out = r.toJson();
        set (out, "persisted", false);
        if (persist) set (out, "note", "audio.cpp backends are remembered for this session only");
        return Outcome::success (out);
    }
    // openvino (shared by noise suppression + transcription) / whisper:
    // resolving needs the runtime's real list -> a job; the setting is
    // written on the message thread.
    Application* app = &ctx.app;
    const std::string eng = engine.toStdString();
    const auto id = startJob (ctx, "ai.setDevice", "Checking the AI device", false,
        [app, eng, device, persist] (JobControl& jc) -> Outcome {
            const auto r = sc::ai::resolveAiDevice (eng, device);
            if (!r.ok)
            {
                const auto ej = r.errorJson();
                return Outcome::failure (ej.getProperty ("code", err::invalidParams).toString(),
                                         ej.getProperty ("message", {}).toString(), ej.getProperty ("details", {}));
            }
            auto out = r.toJson();
            if (eng == "openvino")
            {
                if (!jc.onMessageThread ([&] {
                        app->aiDevicePref = r.requested.empty() ? std::string ("auto") : r.requested;
                        if (persist) app->saveAiSettings();
                    }))
                    return Outcome::failure (err::shuttingDown, "the application is shutting down");
            }
            set (out, "persisted", persist && eng == "openvino");
            return Outcome::success (out);
        });
    return Outcome::job (id);
}

Outcome noiseSuppressCmd (Context& ctx)
{
    sc::ActionStep s;
    s.type = sc::actionTypes::kNoiseSuppress;
    s.params = stepParamsFrom (ctx, { "modelId", "device", "attenLimDb", "df3PostFilter", "copyNoise", "autoDownload" });
    return runStepsCommand (ctx, { s }, true, true);
}

Outcome transcribeCmd (Context& ctx)
{
    sc::ActionStep s;
    s.type = sc::actionTypes::kTranscribe;
    s.params = stepParamsFrom (ctx, { "modelId", "language", "translate", "fast", "autoDownload" });
    if (!s.params.hasProperty ("language")) set (s.params, "language", "auto");
    return runStepsCommand (ctx, { s }, true, true);
}

Outcome batchTranscribeCmd (Context& ctx)
{
    const juce::String folderPath = ctx.str ("folder");
    if (!juce::File::isAbsolutePath (folderPath))
        return invalid ("folder", "absolute path", "folder must be an absolute path");
    const juce::File folder (folderPath);
    if (!folder.isDirectory())
        return Outcome::failure (err::failed, "not a folder: " + folder.getFullPathName());
    {
        const auto chk = checkPath (ctx.app, folder, true, ctx.confirmed(), "folder");
        if (!chk.ok) return chk;
    }
    const std::string model = ctx.str ("modelId", "whisper-base").toStdString();
    if (sc::ai::ModelCatalog::instance().find (model) == nullptr)
        return invalid ("modelId", "a transcription modelId", "unknown model \"" + juce::String (model) + "\"");
    const std::string lang = ctx.str ("language", "auto").toStdString();
    const bool translate = ctx.flag ("translate", false), fast = ctx.flag ("fast", false);
    const bool skip = ctx.flag ("skipExisting", false), autoDl = ctx.flag ("autoDownload", true);
    const int channel = ctx.integer ("channel", 1);
    const juce::String mm = ctx.str ("metaMode", "auto");
    const auto target = mm == "embedded" ? sc::MetaSaveTarget::Embedded
                      : mm == "external" ? sc::MetaSaveTarget::External : sc::MetaSaveTarget::Auto;
    Application* app = &ctx.app;
    // Files are loaded headlessly, one by one, never as open documents:
    // nothing in the app changes, the job is not "mutating".
    const auto id = startJob (ctx, "ai.batchTranscribe", "Transcribing " + folder.getFileName(), false,
        [=] (JobControl& jc) -> Outcome {
            std::atomic<bool> cancel { false };
            BatchTranscribeSummary sum;
            std::string e;
            const bool ok = app->batchTranscribeFolder (folder, model, lang, translate, fast, skip, channel,
                [&] (float f) { jc.progress (f); if (jc.cancelled()) cancel.store (true); return !jc.cancelled(); },
                [&] (const juce::String& st) { jc.progress (-1.0, st); },
                &cancel, &sum, &e, autoDl, target);
            if (!ok)
                return Outcome::failure (jc.cancelled() ? err::cancelled : err::failed, juce::String (e));
            juce::Array<juce::var> fails;
            for (auto& f : sum.failures) fails.add (juce::String::fromUTF8 (f.c_str()));
            return Outcome::success (obj ({ { "folder", folder.getFullPathName() }, { "total", sum.total },
                                            { "done", sum.done }, { "skipped", sum.skipped },
                                            { "failed", sum.failed }, { "failures", fails },
                                            { "cancelled", jc.cancelled() } }));
        });
    return Outcome::job (id);
}
#endif   // SC_ENABLE_AI

Outcome unsupported (Context&)
{
    return Outcome::failure (err::unsupportedBuild, "this build has no AI support (SPECTRO_ENABLE_AI)");
}

Outcome transcriptionGetCmd (Context& ctx)
{
    FileDoc& d = *ctx.doc;
    juce::Array<juce::var> segs;
    for (const auto& g : d.sctt.segments)
        segs.add (obj ({ { "start", g.startSec }, { "end", g.endSec },
                         { "text", juce::String::fromUTF8 (g.text.c_str()) } }));
    const juce::String source = d.sctt.empty() ? juce::String ("none")
                              : (bool) d.metaDirty ? juce::String ("memory")
                              : d.metaMode == sc::MetaMode::External ? juce::String ("sidecar") : juce::String ("sctt");
    return Outcome::success (obj ({ { "doc", d.id }, { "source", source }, { "segments", segs },
                                    { "segmentCount", segs.size() } }));
}

} // namespace

void registerAiCommands (CommandRegistry& r)
{
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    const Handler list = modelsListCmd, install = modelsInstallCmd, verify = modelsVerifyCmd,
                  remove = modelsRemoveCmd, devices = devicesCmd, setDevice = setDeviceCmd,
                  ns = noiseSuppressCmd, tr = transcribeCmd, batch = batchTranscribeCmd;
#else
    const Handler list = unsupported, install = unsupported, verify = unsupported, remove = unsupported,
                  devices = unsupported, setDevice = unsupported, ns = unsupported, tr = unsupported,
                  batch = unsupported;
#endif
    const auto modelId = P ("modelId", PType::String).req().describe ("Model id from ai.models.list");

    r.add ({ "ai.models.list", "AI models of every engine: installed, size, checksum, backends, installable", "ai",
             {}, {}, Query | AiBuildOnly, {}, list });
    r.add ({ "ai.models.install", "Download + verify (SHA-256) a model, as a job", "ai",
             { modelId }, {}, LongRunning | AiBuildOnly, {}, install });
    r.add ({ "ai.models.verify", "Re-hash a model's files (SHA-256), as a job", "ai",
             { modelId }, {}, LongRunning | AiBuildOnly | Query, {}, verify });
    r.add ({ "ai.models.remove", "Delete a model's files (confirm required)", "ai",
             { modelId, P ("confirm", PType::Boolean).describe ("Required") },
             {}, Destructive | AiBuildOnly, {}, remove });
    r.add ({ "ai.devices", "Devices each AI engine can really use (init:true loads OpenVINO, as a job)", "ai",
             { P ("init", PType::Boolean).def (false) }, {}, Query | AiBuildOnly | LongRunning, {}, devices });
    r.add ({ "ai.setDevice", "Choose the device of an engine (validated against the real list)", "ai",
             { P ("engine", PType::String).oneOf ({ "openvino", "whisper", "audiocpp" }).def ("openvino"),
               P ("device", PType::String).req().describe ("auto, CPU, GPU, NPU (OpenVINO); cpu, cuda, vulkan (audio.cpp)"),
               P ("persist", PType::Boolean).def (false).describe ("Save as the app setting (OpenVINO)") },
             {}, LongRunning | AiBuildOnly, {}, setDevice });

    auto nsParams = stepScopeParams();
    nsParams.insert (nsParams.begin(), {
        P ("modelId", PType::String).req().describe ("denseunet, deepfilternet2 or deepfilternet3"),
        P ("device", PType::String).describe ("auto (default: the app setting), CPU, GPU, NPU"),
        P ("attenLimDb", PType::Number).range (0, 100).describe ("DeepFilterNet attenuation limit (dB, 0 = none)"),
        P ("df3PostFilter", PType::Boolean).def (false),
        P ("copyNoise", PType::Boolean).def (false).describe ("Also put the removed noise on new tracks"),
        P ("autoDownload", PType::Boolean).def (true).describe ("Download the model when missing") });
    r.add ({ "ai.noiseSuppress", "AI noise suppression of a range, as a job (one undo step)", "ai",
             nsParams, {}, Mutates | NeedsDoc | LongRunning | AiBuildOnly, { "noiseSuppress" }, ns });

    auto trParams = stepScopeParams();
    trParams.erase (trParams.begin());   // the whole file is transcribed: no "select"
    trParams.insert (trParams.begin(), {
        P ("modelId", PType::String).req().describe ("whisper-tiny ... whisper-large-v3"),
        P ("language", PType::String).def ("auto").describe ("auto or a language code (fr, en...)"),
        P ("translate", PType::Boolean).def (false).describe ("Translate to English"),
        P ("fast", PType::Boolean).def (false).describe ("Greedy decoding (faster)"),
        P ("autoDownload", PType::Boolean).def (true) });
    r.add ({ "ai.transcribe", "Whisper transcription of the document (channels mixed), stored in its metadata; returns the segments", "ai",
             trParams, {}, Mutates | NeedsDoc | LongRunning | AiBuildOnly, { "transcribe" }, tr });

    r.add ({ "ai.batchTranscribe", "Transcribe every .wav of a folder tree in place (SCTT chunk / sidecar), as a job", "ai",
             { P ("folder", PType::String).req().describe ("Absolute folder path"),
               P ("modelId", PType::String).def ("whisper-base"),
               P ("language", PType::String).def ("auto"),
               P ("translate", PType::Boolean).def (false), P ("fast", PType::Boolean).def (false),
               P ("skipExisting", PType::Boolean).def (false),
               P ("channel", PType::Integer).def (1).describe ("1-based channel; <= 0 mixes all"),
               P ("autoDownload", PType::Boolean).def (true),
               P ("metaMode", PType::String).oneOf ({ "auto", "embedded", "external" }).def ("auto"),
               P ("confirm", PType::Boolean).describe ("Needed for a folder outside the allowed roots") },
             {}, LongRunning | AiBuildOnly, {}, batch });

    r.add ({ "transcription.get", "Transcription segments of a document", "transcription",
             {}, {}, NeedsDoc | Query, {}, transcriptionGetCmd });
}

} // namespace sc::control
