// Regression tests for the 2026-11 audit fixes (ai). Included by tests/UnitTests.cpp;
// uses its helpers (CHECK, progress, writeWav, makeTempFile, ...).

// audio.cpp downloader: Content-Length is parsed as a 64-bit value (the
// model files exceed 4 GB) and malformed values count as unknown.
static void testRegAiAcppParseContentLength()
{
    CHECK (sc::acpp::parseContentLength ("28786884") == 28786884, "acpp content-length: plain value");
    CHECK (sc::acpp::parseContentLength ("6177855616") == (int64_t) 6177855616LL,
           "acpp content-length: value above 4 GB");
    CHECK (sc::acpp::parseContentLength (" 12\r\n") == 12, "acpp content-length: whitespace trimmed");
    CHECK (sc::acpp::parseContentLength ("") == 0, "acpp content-length: empty is unknown");
    CHECK (sc::acpp::parseContentLength ("12x") == 0, "acpp content-length: malformed is unknown");
    CHECK (sc::acpp::parseContentLength ("-5") == 0, "acpp content-length: negative is unknown");
    CHECK (sc::acpp::parseContentLength (nullptr) == 0, "acpp content-length: null is unknown");
}

// audio.cpp downloader: an HTTP error, an interrupted transfer, a truncated
// body or an error page of the wrong size must never be accepted.
static void testRegAiAcppValidateDownload()
{
    CHECK (sc::acpp::validateDownload (200, 100, 100, 100, false).isEmpty (),
           "acpp validate: complete 200 download accepted");
    CHECK (sc::acpp::validateDownload (0, 0, 0, 100, false).isEmpty (),
           "acpp validate: unknown status/length accepted when bytes arrived");
    CHECK (sc::acpp::validateDownload (404, 1234, 0, 1234, false).contains ("HTTP 404"),
           "acpp validate: HTTP 404 rejected");
    CHECK (sc::acpp::validateDownload (407, 0, 0, 0, false).contains ("HTTP 407"),
           "acpp validate: proxy authentication rejected");
    CHECK (sc::acpp::validateDownload (200, 100, 100, 60, true).isNotEmpty (),
           "acpp validate: network error mid-transfer rejected");
    CHECK (sc::acpp::validateDownload (200, 100, 0, 60, false).isNotEmpty (),
           "acpp validate: body shorter than Content-Length rejected");
    // Captive portal: a complete 200 HTML page instead of the model file.
    CHECK (sc::acpp::validateDownload (200, 1234, 356008, 1234, false).isNotEmpty (),
           "acpp validate: error page of the wrong size rejected");
    CHECK (sc::acpp::validateDownload (200, 0, 0, 0, false).isNotEmpty (),
           "acpp validate: empty body rejected");
}

// audio.cpp downloader: the .part file only replaces dest when valid; a
// rejected transfer leaves the previous dest untouched and no .part behind.
static void testRegAiAcppCommitDownload()
{
    const juce::File dir = makeTempFile ("acpp_commit");
    dir.deleteRecursively ();
    dir.createDirectory ();
    const juce::File dest = dir.getChildFile ("model.gguf");
    const juce::File part = dir.getChildFile ("model.gguf.part");
    dest.replaceWithText ("OLD");

    std::string err;
    part.replaceWithText ("0123456789");   // 10 bytes
    CHECK (!sc::acpp::commitDownload (part, dest, 200, 0, 20, 10, false, &err),
           "acpp commit: wrong size rejected");
    CHECK (!err.empty (), "acpp commit: rejection explained");
    CHECK (!part.exists (), "acpp commit: rejected .part deleted");
    CHECK (dest.loadFileAsString () == "OLD", "acpp commit: rejected download leaves dest untouched");

    err.clear ();
    part.replaceWithText ("0123456789");
    CHECK (!sc::acpp::commitDownload (part, dest, 200, 10, 10, 10, true, &err),
           "acpp commit: interrupted transfer rejected");
    CHECK (!part.exists () && dest.loadFileAsString () == "OLD",
           "acpp commit: interrupted transfer leaves dest untouched");

    err.clear ();
    part.replaceWithText ("0123456789");
    CHECK (!sc::acpp::commitDownload (part, dest, 200, 12, 12, 12, false, &err),
           "acpp commit: bytes missing on disk rejected");

    err.clear ();
    part.replaceWithText ("0123456789");
    CHECK (sc::acpp::commitDownload (part, dest, 200, 10, 10, 10, false, &err),
           "acpp commit: valid download accepted");
    CHECK (!part.exists () && dest.loadFileAsString () == "0123456789",
           "acpp commit: valid download renamed into place");

    dir.deleteRecursively ();
}

#if defined(SC_ENABLE_AI)
// ensureInstalled: a file of the right size but the wrong content must be
// deleted (as the error message says), so the next run downloads it again
// instead of failing forever.
static void testRegAiEnsureInstalledDeletesCorruptFile()
{
    const juce::File root = makeTempFile ("mm_corrupt_root");
    root.deleteRecursively ();
    root.createDirectory ();
    sc::ai::ModelManager mm (root.getFullPathName ().toStdString ());

    // No newlines: replaceWithText would expand them to CRLF and change the size.
    const std::string good = "SpectroCraft regression model file";
    const std::string bad  = "SpectroCraft regression model filX";   // same size
    sc::ai::ModelSpec spec;
    spec.id = "regression-corrupt-model";
    spec.displayName = "Regression";
    spec.minFreeDiskBytes = 0;
    spec.files.push_back ({ "https://example.invalid/c.bin",
                            sc::ai::sha256Hex (good.data (), good.size ()),
                            (int64_t) good.size () });

    const juce::File dir = mm.dirFor (spec.id);
    dir.createDirectory ();
    const juce::File f = dir.getChildFile ("c.bin");
    f.replaceWithText (juce::String (bad));
    CHECK (f.getSize () == (int64_t) good.size (), "mm corrupt: fixture has the expected size");

    std::string err;
    CHECK (!mm.ensureInstalled (spec, [] (float, const std::string&) { return true; }, &err),
           "mm corrupt: checksum mismatch reported");
    CHECK (!f.exists (), "mm corrupt: corrupt file deleted");
    CHECK (juce::String (err).contains ("deleted"), "mm corrupt: message matches what happened");

    // A good copy (e.g. re-downloaded or installed by hand) is then accepted.
    f.replaceWithText (juce::String (good));
    err.clear ();
    CHECK (mm.ensureInstalled (spec, [] (float, const std::string&) { return true; }, &err),
           "mm corrupt: good file accepted afterwards");
    CHECK (mm.ensureInstalled (spec, [] (float, const std::string&) { return true; }, &err),
           "mm corrupt: second check (cached verification) still accepts it");

    root.deleteRecursively ();
}

// Application::noiseSuppress (default branch) used to commit the result from
// the worker thread; it now hops to the message thread like the copyNoise
// branch. Proof: while the message thread is NOT pumped, a worker whose
// pipeline already finished must still be waiting, with the undo stack
// untouched; pumping then lets the commit land. Also replays the action step
// with device "auto", which used to reach compile_model("auto") unresolved.
static void testRegAiNoiseSuppressCommitHop()
{
#if defined(_MSC_VER) && defined(_DEBUG)
    scSkip ("ns commit hop: OpenVINO does not initialise under the MSVC debug CRT");
    return;
#else
    auto& mm = sc::ai::ModelManager::instance ();
    const auto* spec = mm.find ("denseunet");
    std::string err;
    if (spec == nullptr || !mm.ensureInstalled (*spec, [] (float, const std::string&) { return true; }, &err))
    {
        scSkip ("ns commit hop: denseunet unavailable (" + juce::String (err) + ")");
        return;
    }

    const juce::File wav = makeTempFile ("ns_commit_hop.wav");
    writeWav (wav, 1, 48000.0, 48000, [] (int, int64_t f, double) {
        uint32_t s = 777u + (uint32_t) f;
        s = s * 1664525u + 1013904223u;
        return (float) (0.05 * ((double) (s >> 8) / 16777216.0 - 1.0));
    });
    Application app;
    if (!app.openFile (wav)) { CHECK (false, "ns commit hop: open"); return; }
    app.selectAll ();

    const size_t undo0 = app.undo ().undoSize ();
    std::atomic<float> maxProgress { 0.0f };
    std::atomic<bool> finished { false };
    bool ok = false;
    std::string wErr;
    std::thread worker ([&] {
        ok = app.noiseSuppress ("denseunet", "CPU", 0.f, false,
                                [&] (float f) { if (f > maxProgress.load ()) maxProgress.store (f); return true; },
                                &wErr);
        finished.store (true);
    });

    // Wait (without pumping) until the pipeline reported completion.
    const double deadline = juce::Time::getMillisecondCounterHiRes () + 120000.0;
    while (!finished.load () && maxProgress.load () < 0.999f
           && juce::Time::getMillisecondCounterHiRes () < deadline)
        juce::Thread::sleep (5);
    juce::Thread::sleep (300);   // time for a worker-side commit to happen, if any
    const bool finishedWithoutPump = finished.load ();
    CHECK (!finishedWithoutPump || !ok,
           "ns commit hop: the worker waits for the message thread to commit");
    CHECK (app.undo ().undoSize () == undo0,
           "ns commit hop: nothing committed while the message thread was busy");

    CHECK (scPumpUntil ([&] { return finished.load (); }, 120000), "ns commit hop: worker finished after pumping");
    worker.join ();
    CHECK (ok, "ns commit hop: noise suppression succeeded (" + juce::String (wErr) + ")");
    CHECK (app.undo ().undoSize () == undo0 + 1, "ns commit hop: one undo step committed");

    // Replay as an action step with device "auto" (resolved, not passed raw).
    sc::ActionStep s;
    s.type = "noiseSuppress";
    s.params = sc::makeVarObject ({ { "modelId", "denseunet" }, { "device", "auto" } });
    err.clear ();
    CHECK (sc::applyActionStep (app, s, &err), "ns step: device auto is resolved (" + juce::String (err) + ")");
    CHECK (app.undo ().undoSize () == undo0 + 2, "ns step: one more undo step");
#endif
}
#endif   // SC_ENABLE_AI

static void runRegression_ai()
{
    testRegAiAcppParseContentLength ();
    testRegAiAcppValidateDownload ();
    testRegAiAcppCommitDownload ();
#if defined(SC_ENABLE_AI)
    testRegAiEnsureInstalledDeletesCorruptFile ();
    testRegAiNoiseSuppressCommitHop ();
#else
    scSkip ("regression ai: ModelManager::ensureInstalled corrupt-file test needs an AI build (SPECTRO_ENABLE_AI)");
#endif
}
