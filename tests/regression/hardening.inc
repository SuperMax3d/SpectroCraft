// Lot 7 (AI control) hardening tests: Python bridge (timeout + process-tree
// kill, stdin closed, UTF-8 environment and paths, script SHA-256, script
// folder allow-list, result.json) and the headless stem / super-resolution
// services (request validation, no UI). Included by tests/UnitTests.cpp; uses
// its helpers (CHECK, scSkip, makeTempFile, writeWav, ...). Tests that need a
// Python interpreter are skipped when none is found, like the other Python tests.

#include "AI/StemService.h"
#include "AI/SuperResService.h"

#if !JUCE_WINDOWS
#  include <signal.h>
#  include <cerrno>
#endif

namespace {

juce::File hardeningDir (const juce::String& name)
{
    const juce::File d = makeTempFile (name);
    d.deleteRecursively ();
    d.createDirectory ();
    return d;
}

// Writes bytes exactly (replaceWithText would turn "\n" into "\r\n").
void writeBytes (const juce::File& f, const juce::String& text)
{
    f.replaceWithData (text.toRawUTF8 (), text.getNumBytesAsUTF8 ());
}

// True once process `pid` no longer exists (polls up to waitMs).
bool processGone (int pid, int waitMs)
{
    const double end = juce::Time::getMillisecondCounterHiRes () + waitMs;
    for (;;)
    {
#if JUCE_WINDOWS
        HANDLE h = OpenProcess (SYNCHRONIZE, FALSE, (DWORD) pid);
        if (h == nullptr)
            return true;
        const DWORD w = WaitForSingleObject (h, 50);
        CloseHandle (h);
        if (w == WAIT_OBJECT_0)
            return true;
#else
        if (kill ((pid_t) pid, 0) != 0 && errno == ESRCH)
            return true;
        juce::Thread::sleep (50);
#endif
        if (juce::Time::getMillisecondCounterHiRes () >= end)
            return false;
    }
}

juce::StringArray pyArgs (const juce::StringArray& pyTokens, const juce::StringArray& rest)
{
    juce::StringArray a;
    for (int i = 1; i < pyTokens.size (); ++i)
        a.add (pyTokens[i]);
    a.addArray (rest);
    return a;
}

} // namespace

// ---------------------------------------------------------------------------
// result.json parsing (no interpreter needed)
// ---------------------------------------------------------------------------
static void testHardeningResultJson()
{
    {
        const auto r = sc::py::parseScriptResult (R"({
            "version": 1,
            "markers": [ {"time": 1.5, "end": 2.25, "label": "climb"},
                         {"time": 0.5},
                         {"time": 3, "end": -1, "label": "pt"} ],
            "text": "Peak -3.1 dBFS",
            "measures": {"rms_db": -18.5, "bands": [1, 2, 3]},
            "tables": [ {"name": "bands", "columns": ["Hz", "dB"], "rows": [[1000, -20.5], [2000, -22]]} ],
            "futureField": true
        })", 10.0);
        CHECK (r.present && r.error.isEmpty (), "hardening result.json: valid document accepted (" + r.error + ")");
        CHECK (r.timeBase == "input", "hardening result.json: timeBase defaults to input");
        CHECK (r.markers.size () == 3, "hardening result.json: three markers");
        if (r.markers.size () == 3)
        {
            CHECK (r.markers[0].label == "climb" && std::abs (r.markers[0].time - 1.5) < 1e-12
                       && std::abs (r.markers[0].end - 2.25) < 1e-12,
                   "hardening result.json: region marker fields");
            CHECK (std::abs (r.markers[0].docTime - 11.5) < 1e-12 && std::abs (r.markers[0].docEnd - 12.25) < 1e-12,
                   "hardening result.json: input time base offset by the selection start");
            CHECK (r.markers[1].end < 0.0 && r.markers[1].docEnd < 0.0 && r.markers[1].label.isEmpty (),
                   "hardening result.json: end omitted = point marker");
            CHECK (r.markers[2].end < 0.0 && std::abs (r.markers[2].docTime - 13.0) < 1e-12,
                   "hardening result.json: end -1 = point marker, integer time accepted");
        }
        CHECK (r.text == "Peak -3.1 dBFS", "hardening result.json: text");
        CHECK (std::abs ((double) r.measures.getProperty ("rms_db", 0.0) - (-18.5)) < 1e-12
                   && r.measures.getProperty ("bands", juce::var ()).size () == 3,
               "hardening result.json: measures kept as a free-form object");
        CHECK (r.tables.size () == 1 && r.tables[0].name == "bands" && r.tables[0].columns.size () == 2
                   && r.tables[0].rows.size () == 2,
               "hardening result.json: table parsed");
        CHECK ((bool) r.raw.getProperty ("futureField", false), "hardening result.json: unknown fields ignored, kept in raw");
    }
    {
        const auto r = sc::py::parseScriptResult (R"({"timeBase": "document", "markers": [{"time": 4.0}]})", 10.0);
        CHECK (r.error.isEmpty () && r.markers.size () == 1 && std::abs (r.markers[0].docTime - 4.0) < 1e-12,
               "hardening result.json: document time base is not offset");
    }
    {
        const auto r = sc::py::parseScriptResult ("{}");
        CHECK (r.present && r.error.isEmpty () && r.markers.empty () && r.text.isEmpty () && r.measures.isVoid (),
               "hardening result.json: empty object is a valid, empty result");
    }

    const char* const invalid[] = {
        "not json",
        "[1, 2]",
        R"({"version": 2})",
        R"({"timeBase": "seconds"})",
        R"({"markers": {"time": 1}})",
        R"({"markers": [{"label": "no time"}]})",
        R"({"markers": [{"time": -1}]})",
        R"({"markers": [{"time": "1"}]})",
        R"({"markers": [{"time": 2, "end": 1}]})",
        R"({"markers": [{"time": 1, "label": 5}]})",
        R"({"text": 42})",
        R"({"measures": [1, 2]})",
        R"({"tables": [{"columns": []}]})",
        R"({"tables": [{"columns": ["a", "b"], "rows": [[1]]}]})",
        R"({"tables": [{"columns": ["a"], "rows": [1]}]})"
    };
    for (const char* doc : invalid)
    {
        const auto r = sc::py::parseScriptResult (doc);
        CHECK (r.present && r.error.isNotEmpty () && r.markers.empty (),
               "hardening result.json: rejected " + juce::String (doc));
    }

    const juce::File dir = hardeningDir ("result_json");
    CHECK (!sc::py::readScriptResult (dir.getChildFile ("absent.json")).present,
           "hardening result.json: absent file = no result (not an error)");
    // One JSON escape (é) and one raw UTF-8 character (U+4E2D) on disk.
    writeBytes (dir.getChildFile ("r.json"),
                juce::String (juce::CharPointer_UTF8 ("{\"text\": \"h\\u00e9llo \xe4\xb8\xad\"}")));
    const auto fromFile = sc::py::readScriptResult (dir.getChildFile ("r.json"));
    CHECK (fromFile.present && fromFile.error.isEmpty ()
               && fromFile.text == juce::String (juce::CharPointer_UTF8 ("h\xc3\xa9llo \xe4\xb8\xad")),
           "hardening result.json: read from disk, escapes decoded");
    dir.deleteRecursively ();
}

// meta.json carries the exchange paths and the contract; without an
// exchange (legacy callers) it is unchanged.
static void testHardeningMetaContract()
{
    const juce::File dir = hardeningDir ("meta_contract");
    const auto ex = sc::py::makeExchange (dir, "stamp", 2.5);
    CHECK (ex.resultJson.getParentDirectory () == ex.outWav.getParentDirectory (),
           "hardening meta: result.json sits next to out.wav");

    const juce::var with = juce::JSON::parse (sc::py::buildMetaJson (48000.0, 2, 10.0, { 2.5, 4.0 }, false,
                                                                     "f.wav", {}, &ex));
    CHECK (with.getProperty ("resultJson", "").toString () == ex.resultJson.getFullPathName ()
               && with.getProperty ("outputWav", "").toString () == ex.outWav.getFullPathName ()
               && with.getProperty ("inputWav", "").toString () == ex.inWav.getFullPathName (),
           "hardening meta: exchange paths present");
    CHECK ((int) with.getProperty ("resultContract", juce::var ()).getProperty ("version", 0) == 1,
           "hardening meta: result contract version 1 embedded");

    const juce::var without = juce::JSON::parse (sc::py::buildMetaJson (48000.0, 2, 10.0, { 0.0, 10.0 }, true,
                                                                        "f.wav", {}));
    CHECK (!without.hasProperty ("resultJson") && !without.hasProperty ("resultContract")
               && (int) without.getProperty ("numChannels", 0) == 2,
           "hardening meta: legacy call unchanged");
    CHECK (juce::String (sc::py::resultContractText ()).contains ("result.json")
               && sc::py::newScriptTemplate ().contains ("resultJson"),
           "hardening meta: contract documented in the help text and the new-script template");
    dir.deleteRecursively ();
}

static void testHardeningScriptHash()
{
    const juce::File dir = hardeningDir ("script_hash");
    const juce::File f = dir.getChildFile ("h.py");
    writeBytes (f, "abc");
    const juce::String h1 = sc::py::scriptSha256 (f);
    CHECK (h1 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "hardening hash: SHA-256 of the file bytes (FIPS 180-4 'abc' vector)");
    CHECK (sc::py::scriptSha256 (f) == h1, "hardening hash: stable across calls");
    writeBytes (f, "abd");
    CHECK (sc::py::scriptSha256 (f) != h1 && sc::py::scriptSha256 (f).length () == 64,
           "hardening hash: changes with the content");
    CHECK (sc::py::scriptSha256 (dir.getChildFile ("missing.py")).isEmpty (),
           "hardening hash: missing file has no hash");
    dir.deleteRecursively ();
}

static void testHardeningScriptFolder()
{
    const juce::File root = hardeningDir (juce::String (juce::CharPointer_UTF8 ("script folder \xc3\xa9")));
    const juce::File folder = root.getChildFile ("python");
    folder.createDirectory ();
    const juce::File inside = folder.getChildFile ("ok script.py");
    writeBytes (inside, "print(1)\n");
    const juce::File nested = folder.getChildFile ("sub").getChildFile ("n.py");
    nested.getParentDirectory ().createDirectory ();
    writeBytes (nested, "print(1)\n");
    const juce::File outside = root.getChildFile ("evil.py");
    writeBytes (outside, "print(1)\n");
    const juce::File txt = folder.getChildFile ("notes.txt");
    writeBytes (txt, "x");

    juce::String why;
    CHECK (sc::py::isUnderScriptFolder (inside, &why, folder) && why.isEmpty (),
           "hardening allow-list: script in the folder accepted");
    CHECK (sc::py::isUnderScriptFolder (nested, &why, folder), "hardening allow-list: subfolder accepted");
    CHECK (!sc::py::isUnderScriptFolder (outside, &why, folder) && why == "outside_script_folder",
           "hardening allow-list: script outside refused (" + why + ")");
    CHECK (!sc::py::isUnderScriptFolder (folder.getChildFile ("sub").getChildFile ("..").getChildFile ("..")
                                             .getChildFile ("evil.py"), &why, folder)
               && why == "outside_script_folder",
           "hardening allow-list: '..' cannot escape");
    CHECK (!sc::py::isUnderScriptFolder (txt, &why, folder) && why == "not_python",
           "hardening allow-list: non-.py refused (" + why + ")");
    CHECK (!sc::py::isUnderScriptFolder (folder.getChildFile ("missing.py"), &why, folder) && why == "not_found",
           "hardening allow-list: missing script refused (" + why + ")");

    const juce::File link = folder.getChildFile ("link.py");
    if (outside.createSymbolicLink (link, true) && link.exists ())
        CHECK (!sc::py::isUnderScriptFolder (link, &why, folder) && why == "outside_script_folder",
               "hardening allow-list: symbolic link leading outside refused (" + why + ")");
    else
        scSkip ("hardening allow-list: symbolic links cannot be created here (Windows without the privilege)");
    root.deleteRecursively ();
}

// ---------------------------------------------------------------------------
// Headless stem / super-resolution services: validation, no UI
// ---------------------------------------------------------------------------
static void testHardeningStemServiceValidation()
{
    using SS = sc::StemService;
    const juce::File dir = hardeningDir ("stem_service");
    const juce::File bin = dir.getChildFile ("audiocpp_cli.exe");
    const juce::File model = dir.getChildFile ("sam-audio-small-q8_0.gguf");
    const juce::File ftDir = dir.getChildFile ("htdemucs_ft");
    writeBytes (bin, "not a real binary");
    writeBytes (model, "gguf");
    ftDir.createDirectory ();

    auto good = [&] {
        SS::Request r;
        r.mode = SS::Mode::SamAudio;
        r.descriptions.push_back ({ "voice", { { true, 1.0, 2.0 } } });
        r.binary = bin;
        r.model = model;
        r.backend = "cpu";
        r.sourceRate = 48000.0;
        r.extStartSecs = 1.0;
        r.input.assign (2, std::vector<float> (4800, 0.1f));
        return r;
    };
    auto codeOf = [] (const SS::Request& r) { juce::String c; SS::validate (r, &c); return c; };

    juce::String code;
    CHECK (SS::validate (good (), &code).isEmpty () && code.isEmpty (), "hardening stem: valid request accepted");

    { auto r = good (); r.mode = (SS::Mode) 7;               CHECK (codeOf (r) == "invalid_mode",    "hardening stem: bad mode"); }
    { auto r = good (); r.descriptions.clear ();             CHECK (codeOf (r) == "no_description",  "hardening stem: no description"); }
    { auto r = good (); r.descriptions[0].text = "   ";      CHECK (codeOf (r) == "no_description",  "hardening stem: blank description"); }
    { auto r = good (); r.backend = "metal";                 CHECK (codeOf (r) == "invalid_backend", "hardening stem: bad backend"); }
    { auto r = good (); r.sourceRate = 0.0;                  CHECK (codeOf (r) == "invalid_rate",    "hardening stem: bad rate"); }
    { auto r = good (); r.extStartSecs = -1.0;               CHECK (codeOf (r) == "invalid_range",   "hardening stem: negative start"); }
    { auto r = good (); r.input.clear ();                    CHECK (codeOf (r) == "empty_input",     "hardening stem: no input"); }
    { auto r = good (); r.input[1].resize (10);              CHECK (codeOf (r) == "ragged_input",    "hardening stem: ragged input"); }
    { auto r = good (); r.binary = dir.getChildFile ("x");   CHECK (codeOf (r) == "binary_missing",  "hardening stem: binary missing"); }
    { auto r = good (); r.model = dir.getChildFile ("m");    CHECK (codeOf (r) == "model_missing",   "hardening stem: model missing"); }
    { auto r = good (); r.mode = SS::Mode::HtDemucsFt;       CHECK (codeOf (r) == "model_missing",   "hardening stem: ft model must be a folder"); }
    {
        auto r = good ();
        r.mode = SS::Mode::HtDemucsFt;
        r.model = ftDir;
        r.descriptions.clear ();   // htdemucs needs none
        CHECK (codeOf (r).isEmpty (), "hardening stem: htdemucs_ft request with a model folder accepted");
    }

    const auto rel = SS::anchorsRelativeToInput ({ { true, 0.5, 1.5 }, { false, 2.0, 9.0 }, { true, 20.0, 30.0 } },
                                                 1.0, 5.0);
    CHECK (rel.size () == 2 && std::abs (rel[0].startSecs) < 1e-12 && std::abs (rel[0].stopSecs - 0.5) < 1e-12
               && !rel[1].include && std::abs (rel[1].startSecs - 1.0) < 1e-12 && std::abs (rel[1].stopSecs - 5.0) < 1e-12,
           "hardening stem: anchors clamped to the input, empty ones dropped");

    // run() validates first, then (Windows) tries the CLI or (elsewhere)
    // reports the platform; it is called from a plain worker thread, with no
    // message loop involvement, and must neither block nor touch the UI.
    {
        auto r = good ();
        r.backend = "bogus";
        SS::Result res;
        std::thread t ([&] { res = SS::run (r); });
        t.join ();
        CHECK (!res.ok && res.errorCode == "invalid_backend" && res.error.isNotEmpty (),
               "hardening stem: run() refuses an invalid request with the validation code");
    }
    {
        int progressCalls = 0;
        SS::Result res;
        const juce::File work = dir.getChildFile ("work");
        std::thread t ([&] {
            auto r = good ();
            r.workDir = work;
            res = SS::run (r, [&progressCalls] (double, const juce::String&) { ++progressCalls; },
                           [] { return false; });
        });
        t.join ();
#if JUCE_WINDOWS
        // The fake binary cannot start: the run fails after writing its input.
        CHECK (!res.ok && res.errorCode == "tool_failed" && progressCalls > 0,
               "hardening stem: worker-thread run reaches the CLI (" + res.errorCode + ")");
        CHECK (work.getChildFile ("input.wav").existsAsFile () && work.getChildFile ("input_mono.wav").existsAsFile (),
               "hardening stem: caller-provided work folder kept with the input files");
#else
        CHECK (!res.ok && res.errorCode == "unsupported_platform" && !SS::supported (),
               "hardening stem: run() reports the unsupported platform (" + res.errorCode + ")");
        juce::ignoreUnused (progressCalls);
#endif
    }
    dir.deleteRecursively ();
}

static void testHardeningSuperResServiceValidation()
{
    using SR = sc::SuperResService;
    const juce::File dir = hardeningDir ("superres_service");
    const juce::File bin = dir.getChildFile ("audiocpp_cli.exe");
    const juce::File model = dir.getChildFile ("flashsr.safetensors");
    writeBytes (bin, "not a real binary");
    writeBytes (model, "weights");

    auto good = [&] {
        SR::Request r;
        r.mode = SR::Mode::FlashSR;
        r.binary = bin;
        r.model = model;
        r.sourceRate = 16000.0;
        r.input.assign (1, std::vector<float> (1600, 0.1f));
        return r;
    };
    auto codeOf = [] (const SR::Request& r) { juce::String c; SR::validate (r, &c); return c; };

    CHECK (codeOf (good ()).isEmpty (), "hardening superres: valid request accepted");
    { auto r = good (); r.mode = (SR::Mode) 3;               CHECK (codeOf (r) == "invalid_mode",    "hardening superres: bad mode"); }
    { auto r = good (); r.backend = "metal";                 CHECK (codeOf (r) == "invalid_backend", "hardening superres: bad backend"); }
    { auto r = good (); r.sourceRate = -1.0;                 CHECK (codeOf (r) == "invalid_rate",    "hardening superres: bad rate"); }
    { auto r = good (); r.input.clear ();                    CHECK (codeOf (r) == "empty_input",     "hardening superres: no input"); }
    { auto r = good (); r.input.push_back ({ 1.0f });        CHECK (codeOf (r) == "ragged_input",    "hardening superres: ragged input"); }
    { auto r = good (); r.binary = dir.getChildFile ("x");   CHECK (codeOf (r) == "binary_missing",  "hardening superres: binary missing"); }
    { auto r = good (); r.model = dir.getChildFile ("m");    CHECK (codeOf (r) == "model_missing",   "hardening superres: model missing"); }

    SR::Result res;
    std::thread t ([&] { auto r = good (); r.mode = SR::Mode::AudioSR; res = SR::run (r); });
    t.join ();
#if JUCE_WINDOWS
    CHECK (!res.ok && res.errorCode == "tool_failed", "hardening superres: worker-thread run reaches the CLI (" + res.errorCode + ")");
#else
    CHECK (!res.ok && res.errorCode == "unsupported_platform" && !SR::supported (),
           "hardening superres: run() reports the unsupported platform (" + res.errorCode + ")");
#endif
    dir.deleteRecursively ();
}

// ---------------------------------------------------------------------------
// Process execution (interpreter needed for most of it)
// ---------------------------------------------------------------------------
static void testHardeningProcess()
{
    {
        const auto r = sc::py::runProcessEx ("definitely_not_a_real_exe_xyz", {}, {});
        CHECK (!r.started && !r.timedOut, "hardening process: missing executable is 'not started' on every OS");
    }

    std::string pyCmd;
    if (!sc::py::findPython (&pyCmd))
    {
        scSkip ("hardening process: no Python interpreter found (findPython)");
        return;
    }
    const juce::StringArray pyTokens = sc::py::splitCommand (pyCmd);
    const std::string exe = pyTokens[0].toStdString ();
    const juce::File dir = hardeningDir (juce::String (juce::CharPointer_UTF8 ("process dir \xc3\xa9\xc3\xa8 \xe4\xb8\xad")));

    // 1) Timeout kills a sleeping script.
    {
        const juce::File s = dir.getChildFile ("sleep.py");
        writeBytes (s, "import time\ntime.sleep(60)\n");
        sc::py::RunOptions o;
        o.timeoutMs = 500;
        const auto r = sc::py::runProcessEx (exe, pyArgs (pyTokens, { s.getFullPathName () }), o);
        CHECK (r.started && r.timedOut && !r.cancelled && r.exitCode == -1,
               "hardening process: timeout reported");
        CHECK (r.elapsedMs >= 450.0 && r.elapsedMs < 10000.0,
               "hardening process: killed shortly after the limit (" + juce::String (r.elapsedMs, 0) + " ms)");
    }

    // 2) The whole tree goes: a grandchild that ignores SIGTERM dies too.
    {
        const juce::File pidFile = dir.getChildFile ("grandchild.pid");
        const juce::File s = dir.getChildFile ("tree.py");
        writeBytes (s, "import os, signal, subprocess, sys, time\n"
                       "code = 'import signal, time\\n'"
                       " + ('signal.signal(signal.SIGTERM, signal.SIG_IGN)\\n' if os.name != 'nt' else '')"
                       " + 'time.sleep(60)\\n'\n"
                       "p = subprocess.Popen([sys.executable, '-c', code])\n"
                       "open(sys.argv[1], 'w').write(str(p.pid))\n"
                       "time.sleep(60)\n");
        sc::py::RunOptions o;
        o.timeoutMs = 3000;
        o.killGraceMs = 300;
        const auto r = sc::py::runProcessEx (exe, pyArgs (pyTokens, { s.getFullPathName (), pidFile.getFullPathName () }), o);
        const int gpid = pidFile.loadFileAsString ().trim ().getIntValue ();
        CHECK (r.timedOut && gpid > 0, "hardening process: tree script started a grandchild (pid "
                                           + juce::String (gpid) + ")" + r.stderrText);
        if (gpid > 0)
            CHECK (processGone (gpid, 5000), "hardening process: grandchild killed with the tree");
    }

    // 3) Cancellation also kills (shouldContinue polled every 200 ms).
    {
        const juce::File s = dir.getChildFile ("sleep2.py");
        writeBytes (s, "import time\ntime.sleep(60)\n");
        const uint32_t t0 = juce::Time::getMillisecondCounter ();
        const auto r = sc::py::runProcessEx (exe, pyArgs (pyTokens, { s.getFullPathName () }), {},
                                             [t0] { return juce::Time::getMillisecondCounter () - t0 < 400; });
        CHECK (r.started && r.cancelled && !r.timedOut && r.elapsedMs < 10000.0,
               "hardening process: cancel kills the script");
    }

    // 4) stdin is the null device: reading it returns EOF at once.
    {
        const juce::File s = dir.getChildFile ("stdin.py");
        writeBytes (s, "import sys\n"
                       "print('NONE' if sys.stdin is None else 'EOF %d' % len(sys.stdin.read()))\n");
        sc::py::RunOptions o;
        o.timeoutMs = 20000;
        const auto r = sc::py::runProcessEx (exe, pyArgs (pyTokens, { s.getFullPathName () }), o);
        CHECK (r.started && !r.timedOut && r.exitCode == 0 && r.stdoutText.trim () == "EOF 0",
               "hardening process: stdin closed, script gets EOF (" + r.stdoutText.trim () + r.stderrText + ")");
    }

    // 5) UTF-8 environment + non-ASCII paths with spaces, both directions.
    {
        const juce::String accented = juce::String (juce::CharPointer_UTF8 ("d\xc3\xa2ta \xc3\xb1 \xe4\xb8\xad.txt"));
        const juce::File inFile  = dir.getChildFile (accented);
        const juce::File outFile = dir.getChildFile (juce::String (juce::CharPointer_UTF8 ("o\xc3\xbat \xc3\xbc.txt")));
        writeBytes (inFile, "payload");
        const juce::File s = dir.getChildFile (juce::String (juce::CharPointer_UTF8 ("scr\xc3\xafpt \xc3\xbcni.py")));
        writeBytes (s, "import os, sys\n"
                       "data = open(sys.argv[1], encoding='utf-8').read()\n"
                       "open(sys.argv[2], 'w', encoding='utf-8').write(data + '|' + sys.argv[1])\n"
                       "print(os.environ.get('PYTHONUTF8'), sys.stdout.encoding.lower().replace('-', ''))\n"
                       "print(sys.argv[1])\n"
                       "sys.stderr.write('\\u00e9\\u4e2d\\n')\n");
        sc::py::RunOptions o;
        o.timeoutMs = 20000;
        const auto r = sc::py::runProcessEx (exe, pyArgs (pyTokens, { s.getFullPathName (), inFile.getFullPathName (),
                                                                        outFile.getFullPathName () }), o);
        CHECK (r.started && r.exitCode == 0, "hardening utf8: script with a non-ASCII path ran (" + r.stderrText + ")");
        const juce::StringArray lines = juce::StringArray::fromLines (r.stdoutText.trim ());
        CHECK (lines.size () >= 2 && lines[0].trim () == "1 utf8",
               "hardening utf8: PYTHONUTF8=1 and UTF-8 stdout in the child (" + lines[0] + ")");
        CHECK (lines.size () >= 2 && lines[1].trim () == inFile.getFullPathName (),
               "hardening utf8: argv path decoded back intact");
        CHECK (outFile.existsAsFile ()
                   && outFile.loadFileAsString () == "payload|" + inFile.getFullPathName (),
               "hardening utf8: path with spaces/accents round-trips through the script");
        CHECK (r.stderrText.trim () == juce::String (juce::CharPointer_UTF8 ("\xc3\xa9\xe4\xb8\xad")),
               "hardening utf8: stderr decoded as UTF-8");
    }

    // 6) Invalid UTF-8 output never vanishes (Latin-1 / ANSI fallback).
    {
        const char bad[] = { 'a', (char) 0xE9, 'b' };
        const juce::String d = sc::py::decodeProcessOutput (bad, sizeof (bad));
        CHECK (d.length () == 3 && d[0] == 'a' && d[2] == 'b', "hardening decode: invalid UTF-8 falls back, keeps every byte");
    }
    dir.deleteRecursively ();
}

// ---------------------------------------------------------------------------
// Application::runPythonScript with result.json, timeout and the capture
// ---------------------------------------------------------------------------
static void testHardeningRunPythonScript()
{
    std::string pyCmd;
    if (!sc::py::findPython (&pyCmd))
    {
        scSkip ("hardening python run: no Python interpreter found (findPython)");
        return;
    }
    const juce::File dir = hardeningDir ("py run");
    sc::py::ensureExampleScripts (dir);
    const juce::File wav = makeTempFile ("hardening_ramp.wav");
    writeWav (wav, 1, 8000.0, 8000, [] (int, int64_t f, double) { return float ((double) f / 8000.0) * 0.5f; });

    // 1) Analysis-only script: result.json, no out.wav. Succeeds, audio untouched.
    {
        const juce::File s = dir.getChildFile ("analyse.py");
        const juce::String text =
            "import json, sys\n"
            "meta = json.load(open(sys.argv[3], encoding='utf-8'))\n"
            "json.dump({'version': 1, 'markers': [{'time': 0.25, 'end': 0.5, 'label': 'r\\u00e9gion'}],\n"
            "           'text': 'rate %d' % meta['sampleRate'], 'measures': {'n': 3}},\n"
            "          open(meta['resultJson'], 'w', encoding='utf-8'))\n";
        Application app;
        CHECK (app.openFile (wav), "hardening python run: opened wav");
        app.selection = { 0.5, 0.75 };
        const size_t undoBefore = app.undo ().undoSize ();
        sc::py::ScopedScriptRunCapture cap;
        std::string err;
        const bool ok = app.runPythonScript (s, text, pyCmd, nullptr, &err);
        const auto& rep = cap.report ();
        CHECK (ok, "hardening python run: result-only script succeeds (" + juce::String (err) + ")");
        CHECK (cap.ran () && rep.process.exitCode == 0 && !rep.outWavPresent && !rep.audioChanged,
               "hardening python run: report says no audio was produced");
        CHECK (app.undo ().undoSize () == undoBefore, "hardening python run: no undo step, audio untouched");
        CHECK (rep.result.present && rep.result.text == "rate 8000" && rep.result.markers.size () == 1,
               "hardening python run: result.json read back through the capture");
        if (rep.result.markers.size () == 1)
        {
            CHECK (rep.result.markers[0].label == juce::String (juce::CharPointer_UTF8 ("r\xc3\xa9gion")),
                   "hardening python run: UTF-8 label");
            CHECK (std::abs (rep.result.markers[0].docTime - 0.75) < 1e-9
                       && std::abs (rep.result.markers[0].docEnd - 1.0) < 1e-9,
                   "hardening python run: marker times mapped to the document (selection starts at 0.5 s)");
        }
        CHECK ((int) rep.result.measures.getProperty ("n", 0) == 3, "hardening python run: measures");
        CHECK (rep.scriptSha256 == sc::py::scriptSha256 (s) && rep.scriptSha256.length () == 64,
               "hardening python run: SHA-256 of the script that ran is reported");
    }

    // 2) Existing scripts keep working (out.wav only), the capture sees the edit.
    {
        Application app;
        CHECK (app.openFile (wav), "hardening python run legacy: opened wav");
        const juce::File s = dir.getChildFile ("Threshold.py");
        sc::py::ScopedScriptRunCapture cap;
        std::string err;
        const bool ok = app.runPythonScript (s, s.loadFileAsString (), pyCmd, nullptr, &err);
        CHECK (ok && cap.report ().outWavPresent && cap.report ().audioChanged && !cap.report ().result.present,
               "hardening python run legacy: example script unchanged (" + juce::String (err) + ")");
        CHECK (app.undo ().undoSize () == 1, "hardening python run legacy: one undo step");
    }

    // 3) Invalid result.json fails the run even when out.wav is fine.
    {
        const juce::File s = dir.getChildFile ("bad_result.py");
        const juce::String text =
            "import json, shutil, sys\n"
            "meta = json.load(open(sys.argv[3], encoding='utf-8'))\n"
            "shutil.copyfile(sys.argv[1], sys.argv[2])\n"
            "open(meta['resultJson'], 'w').write('{\"markers\": 5}')\n";
        Application app;
        CHECK (app.openFile (wav), "hardening python run bad: opened wav");
        std::string err;
        const bool ok = app.runPythonScript (s, text, pyCmd, nullptr, &err);
        CHECK (!ok && juce::String (err).contains ("result.json"), "hardening python run bad: invalid result.json reported");
        CHECK (app.undo ().undoSize () == 0, "hardening python run bad: document untouched");
    }

    // 4) Time limit from the capture: the script is killed, nothing changes.
    {
        const juce::File s = dir.getChildFile ("hang.py");
        Application app;
        CHECK (app.openFile (wav), "hardening python run timeout: opened wav");
        sc::py::RunOptions o;
        o.timeoutMs = 700;
        sc::py::ScopedScriptRunCapture cap (o);
        std::string err;
        const double t0 = juce::Time::getMillisecondCounterHiRes ();
        const bool ok = app.runPythonScript (s, "import time\ntime.sleep(60)\n", pyCmd, nullptr, &err);
        const double ms = juce::Time::getMillisecondCounterHiRes () - t0;
        CHECK (!ok && cap.report ().process.timedOut && juce::String (err).contains ("time limit"),
               "hardening python run timeout: reported (" + juce::String (err) + ")");
        CHECK (ms < 15000.0, "hardening python run timeout: returned promptly (" + juce::String (ms, 0) + " ms)");
        CHECK (app.undo ().undoSize () == 0, "hardening python run timeout: document untouched");
    }

    // 5) No capture: runs exactly as before (no time limit applied, no report).
    CHECK (sc::py::ScopedScriptRunCapture::current () == nullptr, "hardening python run: capture scope restored");

    wav.deleteFile ();
    dir.deleteRecursively ();
}

static void runRegression_hardening()
{
    testHardeningResultJson ();
    testHardeningMetaContract ();
    testHardeningScriptHash ();
    testHardeningScriptFolder ();
    testHardeningStemServiceValidation ();
    testHardeningSuperResServiceValidation ();
    testHardeningProcess ();
    testHardeningRunPythonScript ();
}
