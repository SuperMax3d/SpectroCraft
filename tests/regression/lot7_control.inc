// AI control lot 7: python.run over the hardened bridge, Windows-only audio.cpp
// commands, NDJSON fuzzing, load test. Included by tests/UnitTests.cpp after
// control_server.inc (uses CtlTestClient). "control load" is a serial test.

static void testPythonRunCommand()
{
    std::string py;
    if (!sc::py::findPython (&py))
    {
        scSkip ("python.run: no Python interpreter");
        return;
    }
    const juce::File folder = sc::py::scriptFolder();
    folder.createDirectory();
    const juce::File script = folder.getChildFile ("sc_test_lot7_analysis.py");
    script.replaceWithText (
        "import json, sys\n"
        "in_path, out_path, meta_path = sys.argv[1:4]\n"
        "meta = json.load(open(meta_path, encoding='utf-8'))\n"
        "json.dump({'version': 1, 'markers': [{'time': 0.25, 'end': 0.5, 'label': 'peak'}],\n"
        "           'text': 'SECRET-TEXT-NOT-FOR-THE-JOURNAL', 'measures': {'rms_db': -12.5}},\n"
        "          open(meta['resultJson'], 'w', encoding='utf-8'))\n");
    const juce::File sleeper = folder.getChildFile ("sc_test_lot7_sleep.py");
    sleeper.replaceWithText ("import time\ntime.sleep(60)\n");

    Application app;
    const juce::File wav = ctlMakeWav ("lot7_py.wav", 1, 8000.0, 16000);
    ctlCall (app, "doc.open", scctl::obj ({ { "path", wav.getFullPathName() } }));

    // Analysis-only script: result.json returned, markers added on request.
    const auto r = ctlCall (app, "python.run", scctl::obj ({ { "script", script.getFullPathName() }, { "select", "all" },
                                                           { "addMarkers", true } }));
    CHECK (ctlOk (r), "python.run: job started " + scctl::toLine (r).substring (0, 200));
    const auto inf = ctlWaitJob (app, ctlResult (r).getProperty ("job", {}).toString(), 60000);
    const auto res = inf.getProperty ("result", {});
    CHECK (inf.getProperty ("state", {}).toString() == "done", "python.run: done " + scctl::toLine (inf).substring (0, 300));
    CHECK (res.getProperty ("scriptSha256", {}).toString() == sc::py::scriptSha256 (script)
               && res.getProperty ("scriptSha256", {}).toString().length() == 64, "python.run: SHA-256 of the script reported");
    CHECK ((int) res.getProperty ("markersAdded", 0) == 1 && app.markers().markers().size() == 1
               && std::abs (app.markers().markers()[0].start - 0.25) < 1e-6, "python.run: result.json marker added at document time");
    CHECK (!(bool) res.getProperty ("audioChanged", true) && app.undo().undoSize() == 0, "python.run: analysis script leaves the audio alone");
    CHECK ((double) res.getProperty ("resultJson", {}).getProperty ("measures", {}).getProperty ("rms_db", 0) == -12.5,
           "python.run: measures returned to the client");
    const juce::String jr = scctl::toLine (ctlResult (ctlCall (app, "journal.read", scctl::obj ({ { "limit", 1000 } }))));
    CHECK (jr.contains (res.getProperty ("scriptSha256", {}).toString()) && !jr.contains ("SECRET-TEXT"),
           "python.run: journal has the script hash, never the text");

    // Outside the scripts folder: confirm required.
    const juce::File outside = makeTempFile ("lot7_outside.py");
    outside.replaceWithText ("print('x')\n");
    CHECK (ctlCode (ctlCall (app, "python.run", scctl::obj ({ { "script", outside.getFullPathName() } }))) == "confirm_required",
           "python.run: script outside the scripts folder needs confirm");

    // Timeout kills the script.
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    const auto rt = ctlCall (app, "python.run", scctl::obj ({ { "script", sleeper.getFullPathName() }, { "timeoutMs", 1500 },
                                                            { "select", "all" } }));
    const auto tinf = ctlWaitJob (app, ctlResult (rt).getProperty ("job", {}).toString(), 30000);
    CHECK (tinf.getProperty ("error", {}).getProperty ("code", {}).toString() == "timeout"
               && juce::Time::getMillisecondCounterHiRes() - t0 < 15000.0,
           "python.run: timeoutMs kills the script (" + scctl::toLine (tinf.getProperty ("error", {})).substring (0, 200) + ")");
    script.deleteFile();
    sleeper.deleteFile();
}

static void testAudioCppCommands()
{
    Application app;
    const juce::File wav = ctlMakeWav ("lot7_acpp.wav", 1, 8000.0, 8000);
    ctlCall (app, "doc.open", scctl::obj ({ { "path", wav.getFullPathName() } }));
#if JUCE_WINDOWS
    // Windows: validation runs (no binary/model in the test environment).
    const auto r = ctlCall (app, "ai.stems.run", scctl::obj ({ { "mode", "samAudio" },
                                                             { "descriptions", scctl::arr ({ scctl::obj ({ { "text", "voice" } }) }) } }));
    CHECK (ctlOk (r) || ctlCode (r) == "model_not_installed" || ctlCode (r) == "invalid_params",
           "ai.stems.run: validated on Windows (" + ctlCode (r) + ")");
#else
    CHECK (ctlCode (ctlCall (app, "ai.stems.run")) == "unsupported_platform", "ai.stems.run: unsupported_platform off Windows");
    CHECK (ctlCode (ctlCall (app, "ai.superRes.run")) == "unsupported_platform", "ai.superRes.run: unsupported_platform off Windows");
#endif
}

// 10 000 random NDJSON lines through the --json session: no crash, every
// line answered or refused, the app still answers afterwards.
static void testControlFuzz()
{
    Application app;
    const juce::File wav = ctlMakeWav ("lot7_fuzz.wav", 2, 8000.0, 8000);
    juce::Random rnd (20261105);
    juce::StringArray cmds;
    for (const auto& s : scctl::CommandRegistry::instance().all())
        if (!s.id.startsWith ("debug.") && !s.id.startsWith ("ai.models.install") && s.id != "ai.models.remove"
            && !s.id.startsWith ("ai.batch") && s.id != "app.quit" && s.id != "record.start"
            && !s.id.startsWith ("transport."))   // no sound out of the test machine
            cmds.add (s.id);
    auto randVal = [&] (int depth) -> juce::var {
        std::function<juce::var (int)> gen = [&] (int d) -> juce::var {
            switch (rnd.nextInt (d > 2 ? 5 : 8))
            {
                case 0: return rnd.nextInt (2000) - 1000;
                case 1: return rnd.nextDouble() * 1e6 - 5e5;
                case 2: return rnd.nextBool();
                case 3: return juce::String::repeatedString ("x", rnd.nextInt (8)) + juce::String (rnd.nextInt());
                case 4: return juce::var();
                case 5: { juce::Array<juce::var> a; for (int i = rnd.nextInt (4); --i >= 0;) a.add (gen (d + 1)); return a; }
                default:
                {
                    auto o = scctl::obj();
                    for (int i = rnd.nextInt (4); --i >= 0;)
                        scctl::set (o, juce::String::charToString ((juce::juce_wchar) ('a' + rnd.nextInt (26))) + "x", gen (d + 1));
                    return o;
                }
            }
        };
        return gen (depth);
    };
    juce::StringArray lines;
    lines.add ("{\"continueOnError\":true}");
    lines.add ("{\"id\":0,\"cmd\":\"doc.open\",\"params\":{\"path\":" + juce::JSON::toString (wav.getFullPathName()) + "}}");
    for (int i = 0; i < 10000; ++i)
    {
        const int kind = rnd.nextInt (10);
        if (kind == 0)
        {
            juce::String junk;
            for (int k = rnd.nextInt (64); --k >= 0;) junk += juce::String::charToString ((juce::juce_wchar) (32 + rnd.nextInt (95)));
            lines.add (junk);
        }
        else
        {
            auto req = scctl::obj ({ { "id", i }, { "cmd", kind == 1 ? juce::String ("nope." + juce::String (i)) : cmds[rnd.nextInt (cmds.size())] } });
            if (rnd.nextInt (4) != 0) scctl::set (req, "params", randVal (0));
            if (rnd.nextInt (10) == 0) scctl::set (req, "timeoutMs", 200);
            lines.add (scctl::toLine (req));
        }
    }
    const juce::String dump = juce::SystemStats::getEnvironmentVariable ("SC_FUZZ_DUMP", {});
    if (dump.isNotEmpty()) juce::File (dump).replaceWithText (lines.joinIntoString ("\n") + "\n");
    juce::StringArray out;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    const int rc = ctlRunLines (app, lines, out);
    CHECK (rc == 1 || rc == 2, "fuzz: session ends with an error code (some lines are invalid)");
    int expected = 0;   // blank lines get no response, the option line neither
    for (const auto& l : lines) if (l.trim().isNotEmpty() && !l.contains ("continueOnError")) ++expected;
    CHECK (out.size() == expected, "fuzz: every non-blank line answered (" + juce::String (out.size()) + "/"
                                       + juce::String (expected) + ")");
    CHECK (ctlOk (ctlCall (app, "app.state")), "fuzz: the app still answers after 10 000 random lines ("
                                                  + juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 1) + " s)");
    scDeliverPostedMessages();
}

// Load: 1000 sequential app.state over the real socket, p99 < 50 ms, total < 10 s.
static void testControlLoad()
{
    Application app;
    scctl::ControlServer::Options o;
    o.discoveryDir = makeTempFile ("ctl_load_disc");
    scctl::ControlServer srv (app, o);
    CHECK (srv.start(), "load: server starts");
    if (!srv.isRunning()) return;
    std::vector<double> lat;
    double total = 0;
    ctlOnClientThread ([&] {
        CtlTestClient c;
        if (!c.connect (srv.port()) || !ctlOk (c.hello (srv.token()))) return;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 1000; ++i)
        {
            const double a = juce::Time::getMillisecondCounterHiRes();
            const auto r = c.call ("app.state");
            if (!ctlOk (r)) break;
            lat.push_back (juce::Time::getMillisecondCounterHiRes() - a);
        }
        total = juce::Time::getMillisecondCounterHiRes() - t0;
    }, 60000);
    CHECK (lat.size() == 1000, "load: 1000 app.state answered");
    if (lat.size() == 1000)
    {
        std::sort (lat.begin(), lat.end());
        const double p99 = lat[989];
        CHECK (total < 10000.0, "load: 1000 app.state in < 10 s (" + juce::String (total, 0) + " ms)");
        CHECK (p99 < 50.0, "load: p99 < 50 ms (" + juce::String (p99, 2) + " ms)");
        std::cerr << "[control load] total " << total << " ms, p50 " << lat[500] << " ms, p99 " << p99 << " ms\n";
    }
    srv.stop();
}

static void runRegression_lot7Control()
{
    testPythonRunCommand();
    testAudioCppCommands();
    testControlFuzz();
}
