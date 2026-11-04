// Regression tests for the 2026-11 audit fixes (application). Included by tests/UnitTests.cpp;
// uses its helpers (CHECK, progress, writeWav, makeTempFile, ...).

/** Loop recording, multi-channel, with pulls SMALLER than the loop capacity
    (the real-world case: ~40 ms per timer tick against a loop of seconds).
    pullRecFrames() used to pass an interleaved index (frame * channels) to
    overwriteSamples(), which takes a frame position: blocks landed at 2*pos,
    and once 2*pos reached the capacity every pull failed after having already
    consumed the plugin's ring, so the recording silently stopped advancing. */
static void testRegressionLoopRecordSmallPulls()
{
    auto& host = sc::rec::RecorderHost::instance ();
    host.scanPlugins ();
    if (host.findPlugin ("fake") == nullptr)
        return;   // the fake recorder plugin is not built on this platform

    Application l;
    l.recording.pluginId = "fake";
    l.recording.deviceId = "dev2ch";   // 2 channels @ 8 kHz
    l.recording.channels = 2;
    l.recording.rate     = 8000.0;
    l.recording.bitDepth = 32;
    l.setRecordingMode (Application::RecordingState::Mode::Loop, 0.5);

    std::string err;
    CHECK (l.startRecording (&err), "regression loop rec: session starts: " + err);
    if (!l.recording.active || l.recDoc () == nullptr)
        return;

    const int64_t cap = l.recording.loopCapFrames;   // 4000 frames
    CHECK (cap == 4000, "regression loop rec: capacity from duration");

    // Pull in 300-frame blocks (does not divide the capacity, so the wrap-edge
    // clamp is exercised too) until three full wraps were recorded.
    FileDoc* d = l.recDoc ();
    const uint32_t deadline = juce::Time::getMillisecondCounter () + 10000;
    int failedPulls = 0;
    while (l.recording.framesRecorded < 3 * cap
           && juce::Time::getMillisecondCounter () < deadline)
    {
        if (host.availableFrames () <= 0) { juce::Thread::yield (); continue; }
        if (!l.pullRecFrames (d, 300))
            ++failedPulls;   // no audio device is open: a pull can only fail on the bug
    }

    CHECK (l.recording.framesRecorded >= 3 * cap,
           "regression loop rec: small pulls keep advancing through several wraps (got "
               + juce::String (l.recording.framesRecorded) + " frames)");
    CHECK (failedPulls == 0,
           "regression loop rec: no pull fails once the loop buffer is full ("
               + juce::String (failedPulls) + " failed)");
    CHECK (l.audioEngine ().numSamples () == cap,
           "regression loop rec: document length stays pinned to the capacity");

    l.stopRecording ();
}

/** findPython() returns "py -3" on Windows machines without python.exe on the
    PATH: the command must be split into executable + flags. */
static void testRegressionPythonSplitCommand()
{
    const juce::StringArray a = sc::py::splitCommand ("py -3");
    CHECK (a.size () == 2 && a[0] == "py" && a[1] == "-3", "regression py: 'py -3' splits in two");

    const juce::StringArray b = sc::py::splitCommand ("  python  ");
    CHECK (b.size () == 1 && b[0] == "python", "regression py: single command is trimmed");

    const juce::StringArray c = sc::py::splitCommand ("\"C:/Program Files/Python 3/python.exe\" -u");
    CHECK (c.size () == 2 && c[0] == "C:/Program Files/Python 3/python.exe" && c[1] == "-u",
           "regression py: quoted interpreter path keeps its spaces");

    CHECK (sc::py::splitCommand ("").isEmpty (), "regression py: empty command has no tokens");
}

/** meta.json: region ends are seconds like starts and must not be rounded. */
static void testRegressionPythonMetaRegionEnd()
{
    std::vector<Marker> ms (2);
    ms[0].name = "Climb";  ms[0].start = 1.25; ms[0].end = 2.75;   // region
    ms[1].name = "Point";  ms[1].start = 0.5;  ms[1].end = -1.0;   // point marker

    const juce::String json = sc::py::buildMetaJson (48000.0, 1, 10.0, { 0.0, 10.0 }, true,
                                                     "f.wav", ms);
    const juce::var v = juce::JSON::parse (json);
    const juce::var arr = v.getProperty ("markers", juce::var ());
    CHECK (arr.isArray () && arr.size () == 2, "regression py meta: two markers");
    if (!arr.isArray () || arr.size () != 2)
        return;

    CHECK (std::abs ((double) arr[0].getProperty ("end", 0.0) - 2.75) < 1e-9,
           "regression py meta: region end keeps its fraction (was rounded to 3)");
    CHECK ((double) arr[1].getProperty ("end", 0.0) == -1.0,
           "regression py meta: point marker end stays -1");
}

/** The remaining Python tests need an interpreter; they are skipped when none
    is installed (like the existing Python end-to-end tests). */
static void testRegressionPythonBridge()
{
    std::string pyCmd;
    if (!sc::py::findPython (&pyCmd))
        return;

    const juce::StringArray pyTokens = sc::py::splitCommand (pyCmd);
    if (pyTokens.isEmpty ())
        return;

    const juce::File dir = scTestTempDir()
        .getChildFile ("sc_regr_py_"
                       + juce::String ((int64_t) juce::Time::getCurrentTime ().toMilliseconds ()));
    dir.deleteRecursively ();
    dir.createDirectory ();
    sc::py::ensureExampleScripts (dir);

    // --- 1) Verbose child: more output than a pipe buffer on BOTH streams. ---
    // The pipes used to be read only after the child exited, so the child
    // blocked on a full pipe forever (deadlock). Output was also decoded with
    // juce::String (const char*) on a buffer that is not NUL-terminated.
    {
        const juce::File big = dir.getChildFile ("big_output.py");
        big.replaceWithText ("import sys\n"
                             "sys.stdout.buffer.write(b'x' * 1000000)\n"
                             "sys.stdout.buffer.flush()\n"
                             "sys.stderr.buffer.write(('\\u00e9' * 300000).encode('utf-8'))\n"
                             "sys.stderr.buffer.flush()\n");

        juce::StringArray args;
        for (int i = 1; i < pyTokens.size (); ++i)
            args.add (pyTokens[i]);
        args.add (big.getFullPathName ());

        // A deadlocked child is killed after 30 s so the test fails instead of hanging.
        const uint32_t deadline = juce::Time::getMillisecondCounter () + 30000;
        int rc = -1;
        juce::String out, errText;
        const bool finished = sc::py::runProcess (pyTokens[0].toStdString (), args,
            [deadline] { return juce::Time::getMillisecondCounter () < deadline; },
            &rc, &out, &errText);

        CHECK (finished && rc == 0, "regression py: verbose child does not deadlock on full pipes");
        CHECK (out.length () == 1000000 && out.containsOnly ("x"),
               "regression py: stdout captured exactly (" + juce::String (out.length ()) + " chars)");
        CHECK (errText.length () == 300000 && errText[0] == (juce::juce_wchar) 0xE9
                   && errText.getLastCharacter () == (juce::juce_wchar) 0xE9,
               "regression py: stderr decoded as UTF-8 across read boundaries ("
                   + juce::String (errText.length ()) + " chars)");
    }

    // Small mono ramp used by the runPythonScript tests below.
    const juce::File wav = makeTempFile ("regr_py_ramp.wav");
    writeWav (wav, 1, 8000.0, 4000, [] (int, int64_t f, double) { return float ((double) f / 4000.0); });

    // --- 2) Interpreter command with extra flags ("py -3" style). ---
    {
        Application app;
        CHECK (app.openFile (wav), "regression py flags: opened wav");
        const juce::File script = dir.getChildFile ("Threshold.py");
        std::string err;
        const bool ok = app.runPythonScript (script, script.loadFileAsString (), pyCmd + " -u",
                                             nullptr, &err);
        CHECK (ok, "regression py: interpreter command with extra flags runs"
                       + (ok ? juce::String () : ": " + juce::String (err)));
    }

    // --- 3) NaN in the script's output is rejected, the document is untouched. ---
    {
        const juce::File nanScript = dir.getChildFile ("nan_out.py");
        const juce::String nanText =
            "import sys, struct\n"
            "src, dst = sys.argv[1], sys.argv[2]\n"
            "b = bytearray(open(src, 'rb').read())\n"
            "i = b.find(b'data') + 8\n"
            "b[i + 40:i + 44] = struct.pack('<f', float('nan'))\n"
            "open(dst, 'wb').write(bytes(b))\n";

        Application app;
        CHECK (app.openFile (wav), "regression py nan: opened wav");
        std::string err;
        const bool ok = app.runPythonScript (nanScript, nanText, pyCmd, nullptr, &err);
        CHECK (!ok && juce::String (err).containsIgnoreCase ("NaN"),
               "regression py: NaN output is rejected with a clear message");
        CHECK (app.undo ().undoSize () == 0, "regression py: NaN output leaves no undo step");
        CHECK (std::isfinite (readOneSample (app.audioEngine (), 0, 10)),
               "regression py: document unchanged after a NaN output");
    }

    // --- 4) Length-changing script run from a worker thread (as the Python
    //        panel does): the commit (markers, selection, playhead, undo side
    //        state, onTimeRangeDeleted -> FFT snapshot buttons) must happen on
    //        the message thread, not on the worker. ---
    {
        auto* mm = juce::MessageManager::getInstance ();
        if (mm->isThisTheMessageThread ())
        {
            Application app;
            CHECK (app.openFile (wav), "regression py worker: opened wav");
            app.setSelection ({ 0.0, 0.5 });   // whole file: [0, 4000)
            app.markers ().addRegionMarker (0.45, 0.48, "tail");   // inside the removed tail

            std::atomic<int> cbOnMessageThread { -1 };
            app.onTimeRangeDeleted = [&cbOnMessageThread] (FileDoc*, double, double, Patch&, bool)
            {
                cbOnMessageThread = juce::MessageManager::getInstance ()->isThisTheMessageThread () ? 1 : 0;
            };

            const juce::File script = dir.getChildFile ("Make Loop.py");
            const juce::String text = script.loadFileAsString ();
            const uint32_t deadline = juce::Time::getMillisecondCounter () + 60000;
            std::atomic<bool> done { false };
            bool ok = false;
            std::string err;
            std::thread worker ([&] {
                ok = app.runPythonScript (script, text, pyCmd,
                                          [deadline] (float) { return juce::Time::getMillisecondCounter () < deadline; },
                                          &err);
                done = true;
            });
            while (!done.load ())
                mm->runDispatchLoopUntil (5);   // keep the message thread alive, like the UI does
            worker.join ();

            CHECK (ok, "regression py worker: make loop ran" + (ok ? juce::String () : ": " + juce::String (err)));
            if (ok)
            {
                CHECK (app.audioEngine ().numSamples () == 3200, "regression py worker: file shortened");
                CHECK (cbOnMessageThread.load () == 1,
                       "regression py worker: length-change side effects committed on the message thread");
                CHECK (app.markers ().markers ().empty (),
                       "regression py worker: marker inside the removed tail dropped");
                CHECK (app.undo ().undoSize () == 1, "regression py worker: one undo step");
            }
        }
    }

    wav.deleteFile ();
    dir.deleteRecursively ();
}


// integration (audio_engine#1): undo/redo of a patch the engine rejects
// (stale/inconsistent, or out of memory) must not leave the history out of
// step with the samples: the history is cleared, the audio is untouched.
// integration (persistence#2): a subtractive-only spectral selection is no
// selection at all for the app (no playback mask, no "has selection").
static void testRegressionUndoRejectedPatchAndSubtractiveSel ()
{
    progress ("regression application (integration): rejected undo patch clears the history; subtractive-only selection");
    const juce::File wav = makeTempFile ("reg_app_undo_reject.wav");
    writeWav (wav, 1, 8000.0, 1000, [] (int, int64_t, double) { return 0.25f; });
    Application app;
    CHECK (app.openFile (wav), "reg app undo: open");
    if (app.doc () == nullptr) return;
    auto& eng = app.audioEngine ();
    std::string err;
    std::vector<std::vector<float>> rows (1, std::vector<float> (10, 0.5f));
    CHECK (eng.replaceSamples ({ 100, 110 }, rows, "edit", &err) && app.undo ().canUndo (),
           "reg app undo: edit recorded");
    app.undo ().lastRecorded ().lengthDelta = 50;   // corrupt it: 10 -> 10 samples needs 0

    app.undoSteps (1);
    float x = 0.0f;
    CHECK (eng.readSamples (0, 105, 1, &x) && x == 0.5f, "reg app undo: rejected undo left the audio as it was");
    CHECK (!app.undo ().canUndo () && app.undo ().redoSize () == 0,
           "reg app undo: the out-of-step history was cleared");
    // The warning is shown asynchronously: let it appear, then dismiss it.
    for (int i = 0; i < 5; ++i)
        juce::MessageManager::getInstance ()->runDispatchLoopUntil (5);
    juce::ModalComponentManager::getInstance ()->cancelAllModalComponents ();
    for (int i = 0; i < 5; ++i)
        juce::MessageManager::getInstance ()->runDispatchLoopUntil (5);

    // Subtractive-only selection.
    SpectralSelection sub;
    sub.addRect (0.01, 0.05, 100.0, 1000.0, /*subtract=*/true);
    app.doc ()->spectralSel = sub;
    CHECK (!app.hasSpectralSelection (), "reg app: a subtractive-only selection is no selection");
    SpectralSelection add;
    add.addRect (0.01, 0.05, 100.0, 1000.0, /*subtract=*/false);
    app.doc ()->spectralSel = add;
    CHECK (app.hasSpectralSelection (), "reg app: an additive shape is a selection");

    wav.deleteFile ();
}

static void runRegression_application()
{
    testRegressionLoopRecordSmallPulls ();
    testRegressionPythonSplitCommand ();
    testRegressionPythonMetaRegionEnd ();
    testRegressionPythonBridge ();
    testRegressionUndoRejectedPatchAndSubtractiveSel ();
}
