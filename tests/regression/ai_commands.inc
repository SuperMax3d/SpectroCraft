// AI control lot 3: ai.* commands (models, devices, noise suppression,
// transcription as jobs). Included by tests/UnitTests.cpp after control.inc.
// Graceful skips when a model is not installed here (no network needed).

static void testAiCommands()
{
    auto& reg = scctl::CommandRegistry::instance();
    for (const char* id : { "ai.models.list", "ai.models.install", "ai.models.verify", "ai.models.remove",
                            "ai.devices", "ai.setDevice", "ai.noiseSuppress", "ai.transcribe",
                            "ai.batchTranscribe", "transcription.get" })
        CHECK (reg.find (id) != nullptr, juce::String ("ai commands: registered ") + id);
    CHECK (reg.find ("noiseSuppress") == reg.find ("ai.noiseSuppress"), "ai commands: alias = old step type");

    Application app;
#if !(defined(SC_ENABLE_AI) && SC_ENABLE_AI)
    CHECK (ctlCode (ctlCall (app, "ai.models.list")) == "unsupported_build", "ai commands: unsupported_build without AI");
    scSkip ("ai commands: non-AI build, only unsupported_build checked");
#else
    // --- models -----------------------------------------------------------------
    const auto list = ctlResult (ctlCall (app, "ai.models.list"));
    const auto models = list.getProperty ("models", {});
    CHECK (models.size() >= 15, "ai.models.list: >= 15 entries (" + juce::String (models.size()) + ")");
    bool haveWhisper = false, anyAcppInstallable = false, runtimeForThisPlatform = false;
    for (int i = 0; i < models.size(); ++i)
    {
        const auto m = models[i];
        if (m.getProperty ("id", {}).toString() == "whisper-base")
            haveWhisper = m.getProperty ("engine", {}).toString() == "whisper"
                          && m.getProperty ("checksum", {}).toString().isNotEmpty();
        if (m.getProperty ("engine", {}).toString() == "audiocpp" && (bool) m.getProperty ("installable", false))
        {
            anyAcppInstallable = true;
            const auto* plats = m.getProperty ("platforms", {}).getArray();
            if (m.getProperty ("family", {}).toString() == "runtime" && plats != nullptr
                && plats->contains (juce::String (sc::acpp::platformTag())))
                runtimeForThisPlatform = true;
        }
    }
    CHECK (haveWhisper, "ai.models.list: whisper-base listed with engine + checksum");
    if (sc::acpp::platformSupported())
        CHECK (runtimeForThisPlatform, "ai.models.list: an audio.cpp CLI build installable for "
                                           + juce::String (sc::acpp::platformTag()));
    else
        CHECK (!anyAcppInstallable, "ai.models.list: audio.cpp assets not installable without a CLI build");
    CHECK (ctlCode (ctlCall (app, "ai.models.install", scctl::obj ({ { "modelId", "nope" } }))) != "",
           "ai.models.install: unknown model refused");
    CHECK (ctlCode (ctlCall (app, "ai.models.remove", scctl::obj ({ { "modelId", "denseunet" } }))) == "confirm_required",
           "ai.models.remove: confirm required");

    auto& cat = sc::ai::ModelCatalog::instance();
    const auto* dense = cat.find ("denseunet");
    const bool denseInstalled = dense != nullptr && cat.status (*dense).installed;
    if (denseInstalled)
    {
        const auto v = ctlCall (app, "ai.models.verify", scctl::obj ({ { "modelId", "denseunet" } }));
        const auto inf = ctlWaitJob (app, ctlResult (v).getProperty ("job", {}).toString(), 60000);
        CHECK (inf.getProperty ("state", {}).toString() == "done"
                   && (bool) inf.getProperty ("result", {}).getProperty ("verified", false),
               "ai.models.verify: job hashes the files " + scctl::toLine (inf).substring (0, 200));
    }
    else
        scSkip ("ai.models.verify: denseunet not installed here");

    // --- devices ----------------------------------------------------------------
    const auto dev = ctlResult (ctlCall (app, "ai.devices"));
    CHECK (dev.getProperty ("openvino", {}).isObject(), "ai.devices: openvino section without init");
    const auto dj = ctlCall (app, "ai.devices", scctl::obj ({ { "init", true } }));
    const auto dinf = ctlWaitJob (app, ctlResult (dj).getProperty ("job", {}).toString(), 60000);
    const auto ovDevs = dinf.getProperty ("result", {}).getProperty ("openvino", {}).getProperty ("devices", {});
    const juce::String devList = scctl::toLine (ovDevs);
    CHECK (dinf.getProperty ("state", {}).toString() == "done", "ai.devices init: job done " + devList);
    const bool hasGpu = devList.contains ("GPU");
    if (!hasGpu)
    {
        const auto sj = ctlCall (app, "ai.setDevice", scctl::obj ({ { "device", "GPU" } }));
        const auto sinf = ctlWaitJob (app, ctlResult (sj).getProperty ("job", {}).toString(), 60000);
        CHECK (sinf.getProperty ("error", {}).getProperty ("code", {}).toString() == "invalid_params"
                   && scctl::toLine (sinf).contains ("one of"),
               "ai.setDevice GPU without a GPU: invalid_params with the real list " + scctl::toLine (sinf).substring (0, 200));
    }
    const auto cj = ctlCall (app, "ai.setDevice", scctl::obj ({ { "device", "CPU" } }));
    const auto cinf = ctlWaitJob (app, ctlResult (cj).getProperty ("job", {}).toString(), 60000);
    CHECK (cinf.getProperty ("state", {}).toString() == "done" && app.aiDevicePref == "CPU",
           "ai.setDevice CPU: applied to the app setting");
    app.aiDevicePref = "auto";

    // --- noise suppression --------------------------------------------------------
    const juce::File wav = ctlMakeWav ("ai_cmd_ns.wav", 1, 48000.0, 48000);
    ctlCall (app, "doc.open", scctl::obj ({ { "path", wav.getFullPathName() } }));
    CHECK (ctlCode (ctlCall (app, "ai.noiseSuppress", scctl::obj ({ { "modelId", "nope" } }))) == "invalid_params",
           "ai.noiseSuppress: unknown model -> invalid_params");
    {
        const auto* df3 = cat.find ("deepfilternet3");
        if (df3 != nullptr && !cat.status (*df3).installed)
            CHECK (ctlCode (ctlCall (app, "ai.noiseSuppress", scctl::obj ({ { "modelId", "deepfilternet3" },
                                                                          { "autoDownload", false } })))
                       == "model_not_installed", "ai.noiseSuppress: model_not_installed without autoDownload");
        else
            scSkip ("ai.noiseSuppress model_not_installed: deepfilternet3 is installed here");
    }
    if (denseInstalled)
    {
        if (!hasGpu)
        {
            const auto g = ctlCall (app, "ai.noiseSuppress", scctl::obj ({ { "modelId", "denseunet" }, { "device", "GPU" },
                                                                          { "select", "all" } }));
            const auto ginf = ctlWaitJob (app, ctlResult (g).getProperty ("job", {}).toString(), 60000);
            CHECK (ginf.getProperty ("error", {}).getProperty ("code", {}).toString() == "invalid_params"
                       && app.undo().undoSize() == 0,
                   "ai.noiseSuppress device GPU (absent): strict error, nothing applied");
        }
        const auto n = ctlCall (app, "ai.noiseSuppress", scctl::obj ({ { "modelId", "denseunet" }, { "select", "all" } }));
        CHECK (ctlOk (n), "ai.noiseSuppress: job started");
        const auto ninf = ctlWaitJob (app, ctlResult (n).getProperty ("job", {}).toString(), 120000);
        CHECK (ninf.getProperty ("state", {}).toString() == "done"
                   && ninf.getProperty ("result", {}).getProperty ("deviceUsed", {}).toString().isNotEmpty()
                   && app.undo().undoSize() == 1,
               "ai.noiseSuppress: done, device reported, one undo step " + scctl::toLine (ninf).substring (0, 300));
        const auto h = ctlResult (ctlCall (app, "history.list"));
        CHECK (scctl::toLine (h).contains ("\"opId\":\"ai.noiseSuppress\"") && scctl::toLine (h).contains ("\"source\":\"test\""),
               "ai.noiseSuppress: history entry credited to the command and its client");
    }
    else
        scSkip ("ai.noiseSuppress run: denseunet not installed here");

    // --- transcription --------------------------------------------------------------
    const auto* wb = cat.find ("whisper-base");
    if (wb != nullptr && cat.status (*wb).installed)
    {
        const auto t = ctlCall (app, "ai.transcribe", scctl::obj ({ { "modelId", "whisper-base" }, { "language", "en" },
                                                                    { "fast", true } }));
        const auto tinf = ctlWaitJob (app, ctlResult (t).getProperty ("job", {}).toString(), 180000);
        CHECK (tinf.getProperty ("state", {}).toString() == "done"
                   && tinf.getProperty ("result", {}).getProperty ("segments", {}).isArray(),
               "ai.transcribe: job returns the segments " + scctl::toLine (tinf).substring (0, 200));
        const auto g = ctlResult (ctlCall (app, "transcription.get"));
        CHECK ((int) g.getProperty ("segmentCount", -1)
                   == (int) tinf.getProperty ("result", {}).getProperty ("segmentCount", -2),
               "transcription.get: same segments as the job");
        // The journal never holds the transcription text.
        const juce::String jr = scctl::toLine (ctlResult (ctlCall (app, "journal.read", scctl::obj ({ { "limit", 1000 } }))));
        bool leaked = false;
        const auto segs = g.getProperty ("segments", {});
        for (int i = 0; i < segs.size(); ++i)
        {
            const auto txt = segs[i].getProperty ("text", {}).toString().trim();
            if (txt.length() > 3 && jr.contains (txt)) leaked = true;
        }
        CHECK (!leaked, "ai.transcribe: transcription text not in the journal");
    }
    else
        scSkip ("ai.transcribe: whisper-base not installed here");
#endif
}

static void runRegression_aiCommands()
{
    testAiCommands();
}
