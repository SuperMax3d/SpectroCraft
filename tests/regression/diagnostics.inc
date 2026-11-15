// Diagnostic log for AI-assisted support (src/Diagnostics.cpp,
// docs/diagnostics.md). Included by tests/UnitTests.cpp after control.inc
// and docking.inc; uses their helpers (CHECK, makeTempFile, ctlCall...).
#include "Diagnostics.h"
#include <csignal>

namespace scdiagtest
{
    // Every event of every diag-*.jsonl file in `dir`, oldest file first.
    static std::vector<juce::var> readAll (const juce::File& dir)
    {
        std::vector<juce::var> out;
        auto files = dir.findChildFiles (juce::File::findFiles, false, "diag-*.jsonl");
        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) {
            return a.getFileName() < b.getFileName();
        });
        for (const auto& f : files)
        {
            juce::StringArray lines;
            f.readLines (lines);
            for (const auto& l : lines)
                if (l.trim().isNotEmpty())
                    out.push_back (juce::JSON::parse (l));
        }
        return out;
    }

    static int count (const std::vector<juce::var>& evs, const juce::String& ev)
    {
        int n = 0;
        for (const auto& e : evs)
            if (e.getProperty ("ev", {}).toString() == ev) ++n;
        return n;
    }

    static juce::var first (const std::vector<juce::var>& evs, const juce::String& ev)
    {
        for (const auto& e : evs)
            if (e.getProperty ("ev", {}).toString() == ev) return e;
        return {};
    }

    static juce::var last (const std::vector<juce::var>& evs, const juce::String& ev)
    {
        for (auto it = evs.rbegin(); it != evs.rend(); ++it)
            if (it->getProperty ("ev", {}).toString() == ev) return *it;
        return {};
    }

    static juce::String allText (const juce::File& dir)
    {
        juce::String s;
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*"))
            s << f.loadFileAsString();
        return s;
    }

    static sc::diag::Config config (const juce::File& dir)
    {
        sc::diag::Config c;
        c.folder = dir;
        c.reason = "tests";
        c.watchdog = false;
        c.crashHandlers = false;
        c.appVersion = "test";
        return c;
    }

    static juce::File freshDir (const juce::String& name)
    {
        const juce::File d = makeTempFile (name);
        d.deleteRecursively();
        d.createDirectory();
        return d;
    }

    // The suite may run with its own log (SC_DIAG_LOG_DIR, CI): stop it while
    // these tests drive the recorder, start it again afterwards.
    struct SuiteLogGuard
    {
        SuiteLogGuard() : wasRunning (sc::diag::isRunning()), cfg (sc::diag::currentConfig())
        {
            if (wasRunning) sc::diag::stop();
        }
        ~SuiteLogGuard()
        {
            if (sc::diag::isRunning()) sc::diag::stop();
            sc::diag::testing::setForegroundOverride (-1);
            if (wasRunning) sc::diag::start (cfg);
        }
        bool wasRunning;
        sc::diag::Config cfg;
    };
}

static void diagTest_basics()
{
    progress ("diagnostics: off by default, header, levels, flight recorder");
    using namespace scdiagtest;
    using sc::diag::Level;

    // Off: nothing is written anywhere, hooks are no-ops.
    const juce::File dir = freshDir ("diag_basics");
    CHECK (!sc::diag::enabled() && !sc::diag::isRunning(), "diag: off by default");
    sc::diag::event ("test.off", Level::error, { { "k", 1 } });
    sc::diag::recordTiles (12.0, 3, 1, 2048);
    { sc::diag::ScopedPaint p ("test", 1, 1); }
    CHECK (dir.findChildFiles (juce::File::findFiles, false, "*").isEmpty(), "diag: off writes no file");
    CHECK (sc::diag::currentFile() == juce::File() && sc::diag::sessionId().isEmpty(), "diag: off has no session");

    CHECK (sc::diag::start (config (dir)), "diag: starts in a writable folder");
    CHECK (sc::diag::enabled() && sc::diag::currentFile().existsAsFile(), "diag: on, file created");
    const juce::String sid = sc::diag::sessionId();
    CHECK (sid.length() == 6, "diag: short session id (" + sid + ")");
    CHECK (sc::diag::currentFile().getFileName().startsWith ("diag-")
               && sc::diag::currentFile().getFileName().contains ("-" + sid + "-001.jsonl"),
           "diag: file name diag-<utc>-<sid>-001.jsonl (" + sc::diag::currentFile().getFileName() + ")");
    CHECK (dir.getChildFile ("running-" + sid + ".json").existsAsFile(), "diag: running marker while on");

    sc::diag::event ("test.info", Level::info, { { "k", 1 } });
    sc::diag::event ("test.warn", Level::warn, { { "k", 2 } });
    sc::diag::event ("test.detail", Level::debug, { { "k", 3 } });
    sc::diag::flush();
    auto evs = readAll (dir);
    const auto header = evs.empty() ? juce::var() : evs.front();
    CHECK (header.getProperty ("ev", {}).toString() == "session.start" && (int) header.getProperty ("v", 0) == 1
               && header.getProperty ("sid", {}).toString() == sid,
           "diag: first line is the versioned session.start header");
    CHECK (header.getProperty ("reason", {}).toString() == "tests" && header.getProperty ("app", {}).toString() == "test"
               && header.getProperty ("os", {}).toString().isNotEmpty() && (int) header.getProperty ("cores", 0) > 0
               && header.getProperty ("screens", {}).isArray(),
           "diag: header describes the environment once");
    {
        const juce::String headerText = juce::JSON::toString (header);
        const juce::String home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
        CHECK (!headerText.contains (home), "diag: header has no home folder path");
    }
    CHECK (first (evs, "mem").isObject() && (int) first (evs, "mem").getProperty ("rssMb", -1) > 0,
           "diag: baseline memory at start");
    const auto info = first (evs, "test.info");
    CHECK (info.isObject() && !info.hasProperty ("lvl") && (double) info.getProperty ("t", -1.0) >= 0.0
               && (int) info.getProperty ("k", 0) == 1,
           "diag: info events have no lvl, a session time and their fields");
    CHECK (first (evs, "test.warn").getProperty ("lvl", {}).toString() == "warn", "diag: warn events carry lvl");
    CHECK (count (evs, "test.detail") == 0, "diag: debug events stay in memory (flight recorder)");

    // flush() after the writer already took the event (its 250 ms wake-up)
    // but before its periodic flush: the line must still be on disk.
    sc::diag::event ("test.idle", Level::info, { { "k", 4 } });
    juce::Thread::sleep (400);
    sc::diag::flush();
    CHECK (count (readAll (dir), "test.idle") == 1, "diag: flush() waits for the data to reach the disk");

    // An error writes the flight recorder before it.
    sc::diag::event ("test.error", Level::error, { { "msg", "boom" } });
    sc::diag::flush();
    evs = readAll (dir);
    const auto flight = first (evs, "flight");
    const auto detail = first (evs, "test.detail");
    CHECK (count (evs, "test.error") == 1 && flight.isObject()
               && flight.getProperty ("cause", {}).toString() == "test.error",
           "diag: an error dumps the flight recorder");
    CHECK (detail.isObject() && (int) detail.getProperty ("fr", 0) == 1 && detail.getProperty ("lvl", {}).toString() == "debug",
           "diag: flight recorder lines are marked fr:1");

    // Clean stop: session.end, no marker left.
    sc::diag::stop();
    CHECK (!sc::diag::enabled() && !dir.getChildFile ("running-" + sid + ".json").existsAsFile(),
           "diag: stop removes the marker");
    evs = readAll (dir);
    const auto end = last (evs, "session.end");
    CHECK (end.isObject() && (int) end.getProperty ("errors", 0) == 1 && (int) end.getProperty ("warnings", 0) == 1
               && (int) end.getProperty ("counts", {}).getProperty ("test.info", 0) == 1,
           "diag: session.end counts the events");
    sc::diag::event ("test.after", Level::error, {});
    CHECK (count (readAll (dir), "test.after") == 0, "diag: nothing is written after stop");
}

static void diagTest_contentPolicy()
{
    progress ("diagnostics: content policy, dedup, rate limit, budget");
    using namespace scdiagtest;
    using sc::diag::Level;

    // Secrets, content keys and long messages.
    {
        const juce::File dir = freshDir ("diag_policy");
        CHECK (sc::diag::start (config (dir)), "diag: start (policy)");
        sc::diag::addSecret ("SECRET-TOKEN-1234567890");
        auto* o = new juce::DynamicObject();
        o->setProperty ("msg", "token is SECRET-TOKEN-1234567890 here");
        o->setProperty ("text", "a transcribed sentence");
        o->setProperty ("token", "abc");
        o->setProperty ("script", "print('x')");
        o->setProperty ("long", juce::String::repeatedString ("x", 2000));
        auto* nested = new juce::DynamicObject();
        nested->setProperty ("values", juce::Array<juce::var> { 1, 2, 3 });
        nested->setProperty ("db", -3.0);
        o->setProperty ("params", juce::var (nested));
        sc::diag::eventObject ("test.policy", Level::info, juce::var (o));
        sc::diag::stop();
        const juce::String all = allText (dir);
        const auto e = first (readAll (dir), "test.policy");
        CHECK (!all.contains ("SECRET-TOKEN-1234567890") && e.getProperty ("msg", {}).toString().contains ("[redacted]"),
               "diag: registered secrets are redacted");
        CHECK (!e.hasProperty ("text") && !e.hasProperty ("token") && !e.hasProperty ("script")
                   && !all.contains ("transcribed sentence"),
               "diag: content keys (text, token, script) are never written");
        CHECK (!e.getProperty ("params", {}).hasProperty ("values") && (double) e.getProperty ("params", {}).getProperty ("db", 0.0) < -2.9,
               "diag: content keys are dropped in nested objects too");
        CHECK (e.getProperty ("long", {}).toString().length() <= 503, "diag: long strings truncated to 500 characters");
        CHECK (sc::diag::clean ("abc", 2) == "ab..." && sc::diag::clean ("short") == "short", "diag: clean() truncates");
    }

    // Identical events within 10 s collapse into one line plus a repeat line.
    {
        const juce::File dir = freshDir ("diag_dedup");
        CHECK (sc::diag::start (config (dir)), "diag: start (dedup)");
        for (int i = 0; i < 30; ++i)
            sc::diag::event ("test.same", Level::warn, { { "msg", "same message" }, { "durMs", i } });
        sc::diag::stop();
        const auto evs = readAll (dir);
        const auto rep = last (evs, "test.same");
        CHECK (count (evs, "test.same") == 2 && (int) rep.getProperty ("repeat", 0) == 29
                   && (int) rep.getProperty ("maxDurMs", 0) == 29,
               "diag: 30 identical events -> 1 line + 1 repeat line (n " + juce::String (count (evs, "test.same")) + ")");
        const auto end = last (evs, "session.end");
        CHECK ((int) end.getProperty ("counts", {}).getProperty ("test.same", 0) == 30, "diag: repeats still counted");
    }

    // Rate limit: at most 60 lines per event name and minute, the rest counted.
    {
        const juce::File dir = freshDir ("diag_rate");
        CHECK (sc::diag::start (config (dir)), "diag: start (rate)");
        for (int i = 0; i < 200; ++i)
            sc::diag::event ("test.flood", Level::info, { { "item", "item " + juce::String (i) } });
        sc::diag::stop();
        const auto evs = readAll (dir);
        const auto dropped = first (evs, "diag.dropped");
        CHECK (count (evs, "test.flood") == 60 && (int) dropped.getProperty ("counts", {}).getProperty ("test.flood", 0) == 140,
               "diag: rate limit 60/min per event, drops reported (" + juce::String (count (evs, "test.flood")) + ")");
    }

    // Budget: rotation by size, oldest files deleted, each part has a header.
    {
        const juce::File dir = freshDir ("diag_budget");
        auto c = config (dir);
        c.maxFileBytes = 4096;
        c.maxFiles = 3;
        CHECK (sc::diag::start (c), "diag: start (budget)");
        static const char* const names[] = { "fill.a", "fill.b", "fill.c", "fill.d", "fill.e",
                                             "fill.f", "fill.g", "fill.h", "fill.i", "fill.j" };
        for (int i = 0; i < 500; ++i)
            sc::diag::event (names[i % 10], Level::info,
                             { { "payload", "line " + juce::String (i) + " " + juce::String::repeatedString ("p", 60) } });
        sc::diag::flush();
        const auto files = dir.findChildFiles (juce::File::findFiles, false, "diag-*.jsonl");
        juce::int64 biggest = 0;
        for (const auto& f : files) biggest = std::max (biggest, f.getSize());
        CHECK (files.size() == 3 && biggest <= 4096, "diag: budget kept (" + juce::String (files.size()) + " files, largest "
                                                         + juce::String (biggest) + " bytes)");
        const auto evs = readAll (dir);
        CHECK (count (evs, "session.start") == 3 && (int) first (evs, "session.start").getProperty ("part", 1) > 1,
               "diag: every part starts with the session header");
        CHECK (sc::diag::currentFile().getFileName().contains ("-" + sc::diag::sessionId() + "-"),
               "diag: current part belongs to the session");
        sc::diag::stop();
    }

    // Hints for known errors.
    CHECK (sc::diag::hintFor ("Could not load the model (OpenVINO runtime missing).") == "ai.runtime"
               && sc::diag::hintFor ("Could not save x (folder not writable, file locked or disk full?)") == "fs.write"
               && sc::diag::hintFor ("all good").isEmpty(),
           "diag: hints for known errors");
}

static void diagTest_uncleanAndCrash()
{
    progress ("diagnostics: unclean session, crash report, logger, terminate text");
    using namespace scdiagtest;
    using sc::diag::Level;

    const juce::File dir = freshDir ("diag_unclean");
    CHECK (sc::diag::start (config (dir)), "diag: start (crash)");
    const juce::String oldSid = sc::diag::sessionId();
    sc::diag::event ("test.before", Level::info, { { "k", 7 } });
    sc::diag::flush();
    sc::diag::testing::writeCrashReport (SIGSEGV);
    CHECK (dir.getChildFile ("crash-" + oldSid + ".txt").existsAsFile(), "diag: crash report written");
    sc::diag::testing::abandon();   // like a crash: no session.end, marker kept
    CHECK (dir.getChildFile ("running-" + oldSid + ".json").existsAsFile(), "diag: marker survives a crash");

    CHECK (sc::diag::start (config (dir)), "diag: restart after the crash");
    sc::diag::flush();
    const auto evs = readAll (dir);
    const auto u = first (evs, "session.unclean");
    CHECK (u.isObject() && u.getProperty ("lvl", {}).toString() == "error"
               && u.getProperty ("prevSid", {}).toString() == oldSid,
           "diag: next session reports the unclean end");
    CHECK (u.getProperty ("crash", {}).isArray() && u.getProperty ("cause", {}).toString().startsWith ("crash:"),
           "diag: the crash report is folded into session.unclean (" + u.getProperty ("cause", {}).toString() + ")");
    CHECK (u.getProperty ("lastEvent", {}).isObject(), "diag: last event of the previous session reported");
    CHECK (!dir.getChildFile ("crash-" + oldSid + ".txt").existsAsFile()
               && !dir.getChildFile ("running-" + oldSid + ".json").existsAsFile(),
           "diag: crash report and old marker consumed");
    sc::diag::stop();
    sc::diag::flush();

    // Markers of other instances: a live pid with a fresh marker is a running
    // instance; the same pid with a marker untouched for 20 minutes is a
    // crashed session whose pid was reused.
    {
       #if JUCE_WINDOWS
        const int livePid = 4;    // System
       #else
        const int livePid = 1;    // init / launchd
       #endif
        auto marker = [&] (const juce::String& sid, int ageMinutes) {
            const juce::File m = dir.getChildFile ("running-" + sid + ".json");
            m.replaceWithText ("{\"sid\":\"" + sid + "\",\"pid\":" + juce::String (livePid) + "}");
            m.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::minutes (ageMinutes));
            return m;
        };
        const juce::File fresh = marker ("aaaaaa", 0);
        const juce::File stale = marker ("bbbbbb", 20);
        CHECK (sc::diag::start (config (dir)), "diag: start (other markers)");
        sc::diag::stop();
        bool freshReported = false, staleReported = false;
        for (const auto& e : readAll (dir))
            if (e.getProperty ("ev", {}).toString() == "session.unclean")
            {
                freshReported = freshReported || e.getProperty ("prevSid", {}).toString() == "aaaaaa";
                staleReported = staleReported || e.getProperty ("prevSid", {}).toString() == "bbbbbb";
            }
        CHECK (!freshReported && fresh.existsAsFile(), "diag: a running instance (fresh marker) is left alone");
        CHECK (staleReported && !stale.existsAsFile(), "diag: a stale marker is a crash even if its pid is alive");
        fresh.deleteFile();
    }

    // A clean stop: no session.unclean next time.
    CHECK (sc::diag::start (config (dir)), "diag: start after a clean stop");
    sc::diag::stop();
    CHECK (count (readAll (dir), "session.unclean") == 2, "diag: a clean stop is not reported as unclean");

    // JUCE Logger messages: "killing thread by force" is an error.
    CHECK (sc::diag::start (config (dir)), "diag: start (logger)");
    juce::Logger::writeToLog ("!! killing thread by force !!");
    juce::Logger::writeToLog ("an ordinary log line");
    sc::diag::stop();
    const auto evs2 = readAll (dir);
    CHECK (count (evs2, "thread.killed") == 1 && last (evs2, "thread.killed").getProperty ("lvl", {}).toString() == "error",
           "diag: killed threads (JUCE Logger) are errors");
    CHECK (juce::Logger::getCurrentLogger() == nullptr, "diag: the logger is removed on stop");
}

static void diagTest_breadcrumbs()
{
    progress ("diagnostics: commands, documents, alerts, saves, metrics");
    using namespace scdiagtest;
    using sc::diag::Level;

    const juce::File dir = freshDir ("diag_crumbs");
    CHECK (sc::diag::start (config (dir)), "diag: start (breadcrumbs)");
    {
        Application app;
        const juce::File wav = ctlMakeWav ("diag_doc.wav", 2, 8000.0, 16000);
        CHECK (ctlOk (ctlCall (app, "doc.open", scctl::obj ({ { "path", wav.getFullPathName() } }))), "diag: doc.open ok");
        ctlCall (app, "step.run", scctl::obj ({ { "select", "all" },
                                                { "step", scctl::obj ({ { "type", "changeGain" },
                                                                        { "params", scctl::obj ({ { "db", -3.0 } }) } }) } }));
        ctlCall (app, "no.such.command");
        const juce::File out = makeTempFile ("diag_saved.wav");
        std::string err;
        CHECK (app.saveDocument (*app.doc(), out, true, &err), "diag: save as ok");
        const juce::File missing = makeTempFile ("diag_missing.wav");
        CHECK (!app.openFile (missing), "diag: missing file does not open");
        sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Open failed",
                                        "Could not open x.wav.\nThe file may be missing, damaged or in an unsupported format.");
        sc::alert::clearSuppressedMessages();
        app.closeDoc (0);
        sc::diag::flush();
        const auto evs = readAll (dir);

        const auto open = first (evs, "doc.open");
        CHECK (open.getProperty ("path", {}).toString() == wav.getFullPathName() && (int) open.getProperty ("ch", 0) == 2
                   && (int) open.getProperty ("sr", 0) == 8000 && open.getProperty ("mode", {}).toString().isNotEmpty()
                   && open.hasProperty ("durMs") && open.hasProperty ("doc"),
               "diag: doc.open has the path (once), format and load time");
        bool sawOpenCmd = false, sawStep = false, sawUnknown = false;
        for (const auto& e : evs)
        {
            if (e.getProperty ("ev", {}).toString() != "cmd") continue;
            const auto c = e.getProperty ("cmd", {}).toString();
            if (c == "doc.open" && (bool) e.getProperty ("ok", false) && e.getProperty ("src", {}).toString() == "test")
                sawOpenCmd = true;
            if (c == "step.run" && e.hasProperty ("durMs")) sawStep = true;
            if (c == "no.such.command" && !(bool) e.getProperty ("ok", true)
                && e.getProperty ("err", {}).toString() == "unknown_cmd" && e.getProperty ("lvl", {}).toString() == "warn")
                sawUnknown = true;
        }
        CHECK (sawOpenCmd && sawStep, "diag: commands become cmd breadcrumbs (source, duration)");
        CHECK (sawUnknown, "diag: a refused command is a warning with its error code");
        const auto save = first (evs, "file.save");
        CHECK ((bool) save.getProperty ("ok", false) && save.getProperty ("path", {}).toString() == out.getFullPathName()
                   && save.getProperty ("fmt", {}).toString() == "wav",
               "diag: file.save with path, format and result");
        const auto failed = first (evs, "file.load");
        CHECK (failed.getProperty ("lvl", {}).toString() == "error" && !(bool) failed.getProperty ("exists", true),
               "diag: a failed load is an error");
        const auto alert = first (evs, "alert");
        CHECK (alert.getProperty ("lvl", {}).toString() == "error" && alert.getProperty ("title", {}).toString() == "Open failed"
                   && alert.getProperty ("hint", {}).toString().isNotEmpty(),
               "diag: warning message boxes are errors with a hint");
        CHECK (first (evs, "doc.close").isObject(), "diag: doc.close");
    }

    // Metrics: slow paint, spectrogram tiles, audio load (minute aggregates).
    {
        sc::diag::ScopedPaint p ("testview", 640, 480);
        juce::Thread::sleep (80);
    }
    sc::diag::recordTiles (12.0, 4, 0, 2048);
    sc::diag::recordTiles (250.0, 2, 6, 4096);
    sc::diag::recordTiles (0.0, 0, 8, 4096);
    for (int i = 0; i < 20; ++i)
        sc::diag::recordAudioLoad (i < 19 ? 0.2 : 0.95);
    sc::diag::testing::runMinuteTick();
    sc::diag::flush();
    const auto evs = readAll (dir);
    const auto paint = first (evs, "paint.slow");
    CHECK (paint.getProperty ("view", {}).toString() == "testview" && (int) paint.getProperty ("durMs", 0) >= 60
               && (int) paint.getProperty ("w", 0) == 640,
           "diag: paint.slow above 50 ms");
    const auto tiles = first (evs, "spectral.tiles");
    CHECK ((int) tiles.getProperty ("n", 0) == 6 && (int) tiles.getProperty ("batches", 0) == 2
               && std::abs ((double) tiles.getProperty ("hit", 0.0) - 14.0 / 20.0) < 0.01
               && (int) tiles.getProperty ("maxMs", 0) == 250 && (int) tiles.getProperty ("fft", 0) == 4096,
           "diag: spectral.tiles aggregate (computed, batches, cache hits, max)");
    const auto load = first (evs, "audio.load");
    CHECK ((int) load.getProperty ("n", 0) == 20 && (double) load.getProperty ("max", 0.0) > 0.9
               && (double) load.getProperty ("p50", 1.0) < 0.3,
           "diag: audio.load aggregate (p50, p95, max)");
    sc::diag::stop();
}

// Startup phase timings (include/StartupTrace.h), merged into the log: one
// "startup" event with the phases once the window is shown, a phase marked
// later as its own "startup.phase" event.
static void diagTest_startup()
{
    progress ("diagnostics: startup phase timings");
    using namespace scdiagtest;
    const scStart::State saved = scStart::state;   // the process's own startup trace

    const juce::File dir = freshDir ("diag_startup");
    CHECK (sc::diag::start (config (dir)), "diag: start (startup)");
    scStart::begin();
    scStart::state.files = 2;
    scStart::mark ("before-audio-device-create");
    scStart::mark ("after-setup-views");
    scStart::mark ("window-shown(+150ms)");
    scStart::report();
    scStart::report();                             // posted once only
    scStart::mark ("audio-device-ready(async)");   // after the report
    sc::diag::flush();

    const auto evs = readAll (dir);
    CHECK (count (evs, "startup") == 1, "diag: one startup event");
    const auto st = first (evs, "startup");
    const auto phases = st.getProperty ("phases", {});
    CHECK ((int) st.getProperty ("files", -1) == 2 && (int) st.getProperty ("ms", -1) >= 0
               && phases.hasProperty ("before-audio-device-create") && phases.hasProperty ("window-shown(+150ms)")
               && ! phases.hasProperty ("audio-device-ready(async)"),
           "diag: startup carries the phases marked so far and the command-line files");
    const auto late = first (evs, "startup.phase");
    CHECK (late.getProperty ("phase", {}).toString() == "audio-device-ready(async)" && (int) late.getProperty ("ms", -1) >= 0,
           "diag: a phase marked after the report gets its own event");
    sc::diag::stop();

    // Log off: nothing is posted, nothing is written.
    const auto linesAfterStop = readAll (dir).size();   // + session.end
    scStart::begin();
    scStart::report();
    CHECK (! sc::diag::isRunning() && readAll (dir).size() == linesAfterStop, "diag: report() with the log off writes nothing");

    scStart::state = saved;
}

static void diagTest_readBackAndBundle()
{
    progress ("diagnostics: summary, diag.tail / diag.summary, support bundle");
    using namespace scdiagtest;
    using sc::diag::Level;

    Application app;
    const juce::File dir = freshDir ("diag_read");
    {
        const auto r = ctlCall (app, "diag.summary");
        CHECK (ctlOk (r) && !(bool) ctlResult (r).getProperty ("enabled", true)
                   && ctlResult (r).getProperty ("hint", {}).toString().contains ("Help > Diagnostic Log"),
               "diag.summary: says how to turn the log on");
    }
    CHECK (sc::diag::start (config (dir)), "diag: start (read back)");
    for (int i = 0; i < 5; ++i)   // a text field each: identical events would be collapsed
        sc::diag::event ("test.read", Level::info, { { "item", "read " + juce::String (i) } });
    sc::diag::event ("audio.xrun", Level::warn, { { "n", 2 } });
    juce::Thread::sleep (5);   // a later session time (t has millisecond resolution)
    sc::diag::event ("test.problem", Level::error, { { "msg", "something failed" } });
    for (int i = 0; i < 400; ++i)
        sc::diag::event (i % 2 == 0 ? "bulk.a" : "bulk.b", Level::info,
                         { { "payload", juce::String::repeatedString ("z", 120) + juce::String (i) } });

    const auto t = ctlCall (app, "diag.tail", scctl::obj ({ { "kinds", scctl::arr ({ "test.read", "audio" }) },
                                                          { "limit", 4 } }));
    const auto events = ctlResult (t).getProperty ("events", {});
    CHECK (ctlOk (t) && events.size() == 4 && events[3].getProperty ("ev", {}).toString() == "audio.xrun"
               && (bool) ctlResult (t).getProperty ("enabled", false),
           "diag.tail: kinds (domain prefix) and limit (the most recent)");
    const double next = (double) ctlResult (t).getProperty ("next", 0.0);
    const auto t2 = ctlCall (app, "diag.tail", scctl::obj ({ { "since", next }, { "kinds", scctl::arr ({ "test" }) } }));
    bool onlyNewer = ctlOk (t2);
    for (const auto& e : *ctlResult (t2).getProperty ("events", {}).getArray())
        onlyNewer = onlyNewer && (double) e.getProperty ("t", 0.0) > next;
    CHECK (onlyNewer && ctlResult (t2).getProperty ("events", {}).size() >= 1, "diag.tail: since");

    const auto s = ctlCall (app, "diag.summary", scctl::obj ({ { "maxBytes", 8192 } }));
    const juce::String md = ctlResult (s).getProperty ("markdown", {}).toString();
    CHECK (ctlOk (s) && md.startsWith ("# SpectroCraft diagnostic summary") && md.contains ("something failed")
               && md.contains ("## Errors and warnings") && (int) md.getNumBytesAsUTF8() <= 8192,
           "diag.summary: markdown with the problems, within maxBytes (" + juce::String ((int) md.getNumBytesAsUTF8()) + ")");
    CHECK ((int) sc::diag::summaryMarkdown().getNumBytesAsUTF8() <= 30 * 1024, "diag: default summary <= 30 KB");

    const juce::File zip = makeTempFile ("diag_bundle.zip");
    juce::String err;
    CHECK (sc::diag::writeSupportBundle (zip, &err), "diag: support bundle written " + err);
    {
        juce::ZipFile z (zip);
        bool summary = false, readme = false, log = false;
        for (int i = 0; i < z.getNumEntries(); ++i)
        {
            const auto name = z.getEntry (i)->filename;
            summary = summary || name == "summary.md";
            readme = readme || name == "README.txt";
            log = log || (name.startsWith ("diag-") && name.endsWith (".jsonl"));
        }
        CHECK (summary && readme && log, "diag: bundle has the logs, summary.md and README.txt");
    }
    const auto st = sc::diag::status();
    CHECK ((bool) st.getProperty ("enabled", false) && st.getProperty ("files", {}).size() >= 1
               && (juce::int64) st.getProperty ("totalBytes", 0) > 0,
           "diag: status lists the files");
    sc::diag::stop();

    // Following the log with "next": repeat lines (written when their 10 s
    // window ends, here at stop) and flight blocks (older t) are not skipped.
    {
        const juce::File dir2 = freshDir ("diag_follow");
        CHECK (sc::diag::start (config (dir2)), "diag: start (follow)");
        for (int i = 0; i < 5; ++i)
            sc::diag::event ("test.rep", Level::info, { { "msg", "same" } });
        sc::diag::event ("test.ctx", Level::debug, { { "msg", "context" } });
        juce::Thread::sleep (10);
        const auto first1 = sc::diag::tail (-1.0, { "test" }, 100);
        const double cursor = (double) first1.getProperty ("next", 0.0);
        juce::Thread::sleep (10);
        sc::diag::event ("test.err", Level::error, { { "msg", "failure" } });
        sc::diag::flush();
        const auto afterErr = sc::diag::tail (cursor, { "test" }, 100).getProperty ("events", {});
        bool sawErr = false, sawCtx = false;
        for (const auto& e : *afterErr.getArray())
        {
            sawErr = sawErr || e.getProperty ("ev", {}).toString() == "test.err";
            sawCtx = sawCtx || (e.getProperty ("ev", {}).toString() == "test.ctx" && (int) e.getProperty ("fr", 0) == 1);
        }
        CHECK (sawErr && sawCtx, "diag.tail: the flight block of a new error comes with it (older t)");
        const double cursor2 = (double) sc::diag::tail (cursor, { "test" }, 100).getProperty ("next", 0.0);
        juce::Thread::sleep (10);
        sc::diag::stop();   // writes the pending repeat line
        const auto rep = sc::diag::tail (cursor2, { "test.rep" }, 100).getProperty ("events", {});
        CHECK (rep.size() == 1 && (int) rep[0].getProperty ("repeat", 0) == 4
                   && (double) rep[0].getProperty ("lastT", 99.0) < (double) rep[0].getProperty ("t", 0.0),
               "diag.tail: a repeat line written later is not skipped (t = when written, lastT)");
        CHECK (sc::diag::start (config (dir)), "diag: back to the read-back session");
    }

    // Non-ASCII text: the size cap is in bytes.
    for (int i = 0; i < 300; ++i)
        sc::diag::event (i % 2 == 0 ? "accent.a" : "accent.b", Level::warn,
                         { { "msg", juce::String (juce::CharPointer_UTF8 ("\xc3\xa9t\xc3\xa9 \xe2\x80\x94 ")) + juce::String (i) } });
    CHECK ((int) sc::diag::summaryMarkdown (4096).getNumBytesAsUTF8() <= 4096, "diag: summary cap counted in bytes");
    sc::diag::stop();

    // Off again: the commands read the latest session on disk.
    const auto t3 = ctlCall (app, "diag.tail", scctl::obj ({ { "kinds", scctl::arr ({ "accent" }) }, { "limit", 1 } }));
    CHECK (ctlOk (t3) && ctlResult (t3).getProperty ("events", {}).size() == 1
               && !(bool) ctlResult (t3).getProperty ("enabled", true),
           "diag.tail: reads the latest session when the log is off");
}

static void diagTest_menusAndStall()
{
    progress ("diagnostics: Window / Help menus, footer indicator, freeze watchdog");
    using namespace scdiagtest;
    using sc::diag::Level;

    const juce::File dir = freshDir ("diag_menu");
    Application app;
    std::unique_ptr<MainWindow> win (new MainWindow (app, "diag menu test"));
    win->onDiagToggled = [dir] (bool on) {
        if (on) sc::diag::start (config (dir));
        else    sc::diag::stop();
    };

    const auto names = win->getMenuBarNames();
    const int wIdx = names.indexOf ("Window"), hIdx = names.indexOf ("Help");
    auto findItem = [] (const juce::PopupMenu& m, const juce::String& text) -> const juce::PopupMenu::Item* {
        for (juce::PopupMenu::MenuItemIterator it (m, true); it.next();)
            if (it.getItem().text == text) return &it.getItem();
        return nullptr;
    };
    {
        // Issue #11: the toggle lives in the Help menu, next to the folder,
        // summary and bundle actions; the Window menu no longer lists it.
        const auto wm = win->getMenuForIndex (wIdx, "Window");
        CHECK (findItem (wm, "Diagnostic Log") == nullptr, "diag: not in the Window menu");
        const auto hm = win->getMenuForIndex (hIdx, "Help");
        const auto* item = findItem (hm, "Diagnostic Log");
        CHECK (item != nullptr && !item->isTicked && !app.settings.diagLogEnabled,
               "diag: Help menu item, unchecked by default");
        CHECK (findItem (hm, "Open Diagnostic Log Folder") != nullptr && findItem (hm, "Copy Diagnostic Summary") != nullptr
                   && findItem (hm, "Create Support Bundle...") != nullptr,
               "diag: Help menu actions");

        // Issue #12: Help > Show Tooltips, on by default, turns every tip
        // off (no tooltip window) and back on, and is remembered.
        const auto* tips = findItem (hm, "Show Tooltips");
        CHECK (tips != nullptr && tips->isTicked && app.settings.showTooltips && win->tooltipsActive (),
               "tooltips: Help > Show Tooltips, checked by default");
        win->menuItemSelected (MainWindow::HelpShowTooltips, hIdx);
        const auto hmOff = win->getMenuForIndex (hIdx, "Help");
        CHECK (! app.settings.showTooltips && ! win->tooltipsActive ()
                   && findItem (hmOff, "Show Tooltips") != nullptr && ! findItem (hmOff, "Show Tooltips")->isTicked
                   && app.recentStore->getIntValue ("show_tooltips", 1) == 0,
               "tooltips: unchecked, no tooltip shows, and the choice is saved");
        win->menuItemSelected (MainWindow::HelpShowTooltips, hIdx);
        CHECK (app.settings.showTooltips && win->tooltipsActive ()
                   && app.recentStore->getIntValue ("show_tooltips", 0) == 1,
               "tooltips: checked again, they show again");
    }

    // The dialog shown before the log starts (headless runs skip it): no AI
    // wording, the folder as a link that opens it.
    {
        auto d = MainWindow::makeDiagLogDialog (nullptr);
        CHECK (d.window != nullptr && d.window->getName() == "Diagnostic Log" && d.window->getNumButtons() == 2,
               "diag dialog: titled Diagnostic Log, Turn On / Cancel");
        CHECK (! MainWindow::kDiagLogDialogText.containsIgnoreCase ("AI ") && ! MainWindow::kDiagLogDialogText.containsIgnoreCase ("assistant"),
               "diag dialog: no AI assistant wording");
        CHECK (d.link != nullptr && d.link->getButtonText() == sc::diag::folder().getFullPathName()
                   && d.link->onClick != nullptr && d.link->getParentComponent() == d.window.get(),
               "diag dialog: the folder path is a link inside the dialog");
    }

    win->menuItemSelected (MainWindow::HelpDiagLog, hIdx);   // headless: no explanation dialog
    CHECK (sc::diag::enabled() && app.settings.diagLogEnabled, "diag: the Help item turns the log on and saves it");
    {
        const auto hm = win->getMenuForIndex (hIdx, "Help");
        const auto* item = findItem (hm, "Diagnostic Log");
        CHECK (item != nullptr && item->isTicked, "diag: the item is checked while the log runs");
    }
    scPumpUntil ([&] { return win->statusBarShowsDiagLog(); }, 3000);   // the window's 30 Hz timer
    CHECK (win->statusBarShowsDiagLog(), "diag: footer indicator while the log runs");

    win->menuItemSelected (MainWindow::HelpOpenDiagFolder, hIdx);
    CHECK (dir.isDirectory(), "diag: Open Diagnostic Log Folder (headless: no file browser)");
    const juce::File zip = makeTempFile ("diag_menu_bundle.zip");
    CHECK (win->writeSupportBundleTo (zip) && zip.existsAsFile(), "diag: support bundle from the window");

    // Freeze watchdog: block the message thread past the hang threshold.
    sc::diag::stop();
    auto c = config (dir);
    c.watchdog = true;
    sc::diag::testing::setForegroundOverride (1);
    CHECK (sc::diag::start (c), "diag: start with the watchdog");
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    {
        const sc::diag::ScopedActivity activity ("test freeze");
        juce::Thread::sleep (3000);
    }
    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
    sc::diag::flush();
    auto evs = readAll (dir);
    const auto hang = first (evs, "ui.hang");
    const auto stall = last (evs, "ui.stall");
    CHECK (hang.getProperty ("lvl", {}).toString() == "error" && hang.getProperty ("during", {}).toString() == "test freeze",
           "diag: ui.hang while still frozen (during the current activity)");
    CHECK ((int) stall.getProperty ("durMs", 0) >= 2500 && stall.getProperty ("lvl", {}).toString() == "error",
           "diag: ui.stall with the freeze duration (" + stall.getProperty ("durMs", 0).toString() + " ms)");

    // Menu breadcrumb + turning the log off from the menu.
    win->menuItemSelected (MainWindow::HelpOpenDiagFolder, hIdx);
    sc::diag::flush();
    evs = readAll (dir);
    CHECK (last (evs, "ui.menu").getProperty ("item", {}).toString() == "Help > Open Diagnostic Log Folder",
           "diag: menu items become ui.menu breadcrumbs");
    win->menuItemSelected (MainWindow::HelpDiagLog, hIdx);
    CHECK (!sc::diag::enabled() && !app.settings.diagLogEnabled, "diag: the Help item turns the log off and saves it");
    scPumpUntil ([&] { return !win->statusBarShowsDiagLog(); }, 3000);
    CHECK (!win->statusBarShowsDiagLog(), "diag: no footer indicator when off");
    win.reset();
}

static void diagTest_cost()
{
    progress ("diagnostics: cost off and on");
    using namespace scdiagtest;
    using sc::diag::Level;

    // Off: one relaxed atomic load per hook.
    double t0 = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < 1000000; ++i)
        sc::diag::event ("test.cost", Level::info, {});
    const double offMs = juce::Time::getMillisecondCounterHiRes() - t0;
    CHECK (offMs < 300.0, "diag: 1 000 000 hooks while off in " + juce::String (offMs, 1) + " ms (< 300)");

    // On: posting is a small copy into a queue (formatting is on the writer).
    const juce::File dir = freshDir ("diag_cost");
    CHECK (sc::diag::start (config (dir)), "diag: start (cost)");
    t0 = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < 10000; ++i)
        sc::diag::event ("test.cost", Level::debug, { { "i", i } });
    const double onMs = juce::Time::getMillisecondCounterHiRes() - t0;
    CHECK (onMs < 2000.0, "diag: 10 000 events posted in " + juce::String (onMs, 1) + " ms (< 2000)");
    sc::diag::stop();
}

static void runRegression_diagnostics()
{
    // One stop / restart of the suite's own log (SC_DIAG_LOG_DIR) around the
    // whole group: restarting it per test would leave only empty sessions
    // in its 5-file budget.
    const scdiagtest::SuiteLogGuard guard;
    diagTest_basics();
    diagTest_contentPolicy();
    diagTest_uncleanAndCrash();
    diagTest_breadcrumbs();
    diagTest_startup();
    diagTest_readBackAndBundle();
    diagTest_menusAndStall();
    diagTest_cost();
}
