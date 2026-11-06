// AI control lot 1: the loopback control server (include/control/ControlServer.h).
// Serial test (scIsSerialTest "control server"): it opens loopback sockets and
// writes discovery files. Included by tests/UnitTests.cpp after control.inc.
#include "control/ControlServer.h"
#include "control/PathPolicy.h"
#if !JUCE_WINDOWS
 #include <sys/stat.h>
#endif

/** Minimal NDJSON client for the tests (runs on its own thread). */
struct CtlTestClient
{
    juce::StreamingSocket sock;
    std::string buf;

    bool connect (int port) { return sock.connect ("127.0.0.1", port, 2000); }
    bool send (const juce::String& line)
    {
        const juce::String l = line + "\n";
        const char* p = l.toRawUTF8();
        int left = (int) l.getNumBytesAsUTF8();
        while (left > 0)   // write() may be partial for large lines
        {
            if (sock.waitUntilReady (false, 5000) <= 0) return false;
            const int w = sock.write (p, left);
            if (w <= 0) return false;
            p += w; left -= w;
        }
        return true;
    }
    bool sendVar (const juce::var& v) { return send (scctl::toLine (v)); }
    /** Next line (empty on timeout / close). */
    juce::String readLine (int timeoutMs = 5000)
    {
        const double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        for (;;)
        {
            const auto pos = buf.find ('\n');
            if (pos != std::string::npos)
            {
                const auto line = buf.substr (0, pos);
                buf.erase (0, pos + 1);
                return juce::String::fromUTF8 (line.data(), (int) line.size());
            }
            const int left = (int) (deadline - juce::Time::getMillisecondCounterHiRes());
            if (left <= 0) return {};
            const int r = sock.waitUntilReady (true, std::min (left, 100));
            if (r < 0) return {};
            if (r == 0) continue;
            char tmp[4096];
            const int n = sock.read (tmp, (int) sizeof (tmp), false);
            if (n <= 0) return {};
            buf.append (tmp, (size_t) n);
        }
    }
    /** Next response line (skips events). */
    juce::var readResponse (int timeoutMs = 5000)
    {
        const double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        for (;;)
        {
            const int left = (int) (deadline - juce::Time::getMillisecondCounterHiRes());
            if (left <= 0) return {};
            const auto l = readLine (left);
            if (l.isEmpty()) return {};
            const auto v = juce::JSON::parse (l);
            if (!v.hasProperty ("event")) return v;
            events.add (v);
        }
    }
    juce::var call (const juce::String& cmd, const juce::var& params = {}, const juce::var& doc = {},
                    int timeoutMs = 5000, int reqTimeoutMs = 0)
    {
        auto req = scctl::obj ({ { "id", ++nextId }, { "cmd", cmd } });
        if (!params.isVoid()) scctl::set (req, "params", params);
        if (!doc.isVoid()) scctl::set (req, "doc", doc);
        if (reqTimeoutMs > 0) scctl::set (req, "timeoutMs", reqTimeoutMs);
        if (!sendVar (req)) return {};
        return readResponse (timeoutMs);
    }
    juce::var hello (const juce::String& token, juce::var subscribe = {})
    {
        auto p = scctl::obj ({ { "token", token }, { "client", "unit-test" } });
        if (!subscribe.isVoid()) scctl::set (p, "subscribe", subscribe);
        return call ("session.hello", p);
    }
    juce::Array<juce::var> events;
    int nextId = 0;
};

/** Runs fn on a worker while the test (message) thread pumps. */
static bool ctlOnClientThread (const std::function<void()>& fn, int timeoutMs = 30000)
{
    std::atomic<bool> done { false };
    std::thread t ([&] { fn(); done.store (true); });
    const bool ok = scPumpUntil ([&] { return done.load(); }, timeoutMs);
    if (!ok)
        scPumpUntil ([&] { return done.load(); }, 60000);   // never leave the thread running
    t.join();
    return ok;
}

static void testControlServer()
{
    const juce::File disc = makeTempFile ("ctl_discovery");
    disc.createDirectory();

    // A stale discovery file (dead PID) is swept at start.
    const juce::File stale = disc.getChildFile ("999999.json");
    stale.replaceWithText ("{\"pid\":999999,\"port\":1,\"token\":\"x\"}");
    CHECK (!scctl::ControlServer::processAlive (999999), "server: pid 999999 is not alive");
    CHECK (scctl::ControlServer::processAlive (scctl::processId()), "server: own pid is alive");

    Application app;
    const juce::File wavA = ctlMakeWav ("ctl_srv_a.wav", 2);
    const juce::File wavB = ctlMakeWav ("ctl_srv_b.wav", 1);

    scctl::ControlServer::Options o;
    o.discoveryDir = disc;
    o.callTimeoutMs = 5000;
    scctl::ControlServer srv (app, o);
    juce::String err;
    CHECK (srv.start (&err), "server: starts (" + err + ")");
    if (!srv.isRunning()) return;
    CHECK (srv.port() > 0, "server: ephemeral port bound");
    CHECK (!stale.existsAsFile(), "server: stale discovery file swept");

    const juce::File df = srv.discoveryFile();
    const auto dj = juce::JSON::parse (df.loadFileAsString());
    CHECK (df.existsAsFile() && (int) dj.getProperty ("port", 0) == srv.port()
               && dj.getProperty ("token", {}).toString() == srv.token()
               && (int) dj.getProperty ("pid", 0) == scctl::processId()
               && (int) dj.getProperty ("protocol", 0) == 1,
           "server: discovery file {pid, port, token, protocol}");
#if !JUCE_WINDOWS
    struct stat stt {};
    CHECK (::stat (df.getFullPathName().toRawUTF8(), &stt) == 0 && (stt.st_mode & 0777) == 0600,
           "server: discovery file is 0600");
#endif
    const int port = srv.port();
    const juce::String token = srv.token();

    // --- authentication -----------------------------------------------------
    ctlOnClientThread ([&] {
        CtlTestClient c;
        CHECK (c.connect (port), "server: client connects");
        auto r = c.call ("app.state");
        CHECK (ctlCode (r) == "unauthorized", "server: commands before hello are unauthorized");
        // Hostile input before authentication: deep nesting (recursive JSON
        // parser) and long lines are refused without parsing.
        c.send (juce::String::repeatedString ("[", 3000));
        r = c.readResponse (3000);
        CHECK (ctlCode (r) == "invalid_params" || ctlCode (r) == "unauthorized", "server: deep JSON refused before hello");
        c.send (juce::String::repeatedString ("{\"a\":", 100) + "1" + juce::String::repeatedString ("}", 100));
        r = c.readResponse (3000);
        CHECK (ctlCode (r) == "invalid_params" && r.getProperty ("error", {}).getProperty ("message", {}).toString().contains ("64"),
               "server: nesting > 64 refused");
        c.send ("{\"id\":9,\"cmd\":\"session.hello\",\"params\":{\"token\":\"" + juce::String::repeatedString ("x", 5000) + "\"}}");
        r = c.readResponse (3000);
        CHECK (ctlCode (r) == "invalid_params", "server: > 4 KiB line before hello refused");
        r = c.hello ("wrong");
        CHECK (ctlCode (r) == "unauthorized", "server: bad token refused");
        c.hello ("wrong2");
        c.hello ("wrong3");   // third failure -> 2 s pause
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        r = c.hello (token, scctl::arr ({ "history", "job" }));
        const double el = juce::Time::getMillisecondCounterHiRes() - t0;
        CHECK (ctlOk (r), "server: good token accepted");
        CHECK (el > 1500.0, "server: 3 failures -> ~2 s pause (" + juce::String (el, 0) + " ms)");
        const auto info = ctlResult (r);
        CHECK ((int) info.getProperty ("protocol", 0) == 1 && info.getProperty ("app", {}) == juce::var ("SpectroCraft"),
               "server: hello reports app + protocol");
        CHECK (info.getProperty ("version", {}).toString().containsChar ('.'), "server: hello reports the build version");

        // --- commands over the socket ---------------------------------------
        r = c.call ("doc.open", scctl::obj ({ { "path", wavA.getFullPathName() } }));
        CHECK (ctlOk (r), "server: doc.open over the socket");
        const int idA = (int) ctlResult (r).getProperty ("doc", -1);
        r = c.call ("doc.open", scctl::obj ({ { "path", wavB.getFullPathName() } }));
        CHECK (ctlOk (r), "server: second doc.open");
        r = c.call ("step.run", scctl::obj ({ { "select", scctl::obj ({ { "start", 0.2 }, { "end", 0.6 } }) },
                                              { "step", scctl::obj ({ { "type", "silence" } }) } }), idA);
        CHECK (ctlOk (r) && (bool) r.getProperty ("docSwitched", false), "server: step.run on a non-active doc, docSwitched");
        // The history.changed event of that edit (subscribed in hello).
        const auto ev = c.readLine (2000);
        bool gotHistory = ev.contains ("\"event\":\"history.changed\"");
        for (auto& e : c.events) if (e.getProperty ("event", {}).toString() == "history.changed") gotHistory = true;
        CHECK (gotHistory, "server: history.changed event delivered");

        // Jobs report progress / done events.
        r = c.call ("debug.sleep", scctl::obj ({ { "ms", 300 } }));
        CHECK (ctlOk (r) && ctlResult (r).getProperty ("job", {}).toString().startsWith ("j-"), "server: job started");
        bool done = false;
        for (int i = 0; i < 50 && !done; ++i)
        {
            const auto l = c.readLine (200);
            if (l.contains ("\"event\":\"job.done\"")) done = true;
        }
        CHECK (done, "server: job.done event delivered");

        // Oversized line: refused, the session survives.
        c.send ("{\"id\":1,\"cmd\":\"app.state\",\"params\":{\"x\":\"" + juce::String::repeatedString ("a", scctl::kMaxLineBytes + 10) + "\"}}");
        r = c.readResponse (5000);
        CHECK (ctlCode (r) == "invalid_params" && r.getProperty ("error", {}).getProperty ("message", {}).toString().contains ("1 MiB"),
               "server: line > 1 MiB refused");
        CHECK (ctlOk (c.call ("app.state")), "server: session still usable after an oversized line");
        CHECK (ctlCode (c.call ("nope")) == "unknown_cmd", "server: unknown_cmd");
        c.send ("not json");
        CHECK (ctlCode (c.readResponse()) == "invalid_params", "server: unreadable JSON");
    });
    CHECK (srv.numClients() == 0 || scPumpUntil ([&] { return srv.numClients() == 0; }, 3000),
           "server: client count back to 0 after disconnect");

    // --- message thread busy: bounded wait, never a hang ---------------------
    {
        CtlTestClient c;
        std::atomic<bool> got { false }, helloOk { false }, go { false };
        juce::var r;
        std::thread t ([&] {
            if (c.connect (port) && ctlOk (c.hello (token)))
            {
                helloOk.store (true);
                while (!go.load()) juce::Thread::sleep (5);
                // No pump during this call: the server must give up after timeoutMs.
                r = c.call ("app.state", {}, {}, 5000, 300);
            }
            got.store (true);
        });
        // Pump only for the hello, then block the message thread.
        scPumpUntil ([&] { return helloOk.load() || got.load(); }, 5000);
        go.store (true);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        while (!got.load() && juce::Time::getMillisecondCounterHiRes() - t0 < 4000)
            juce::Thread::sleep (10);   // message thread deliberately NOT pumped
        t.join();
        CHECK (ctlCode (r) == "timeout", "server: unresponsive message thread -> timeout (" + scctl::toLine (r) + ")");
        scDeliverPostedMessages();   // the abandoned call runs (and does nothing)
    }

    // --- max 4 clients ----------------------------------------------------------
    ctlOnClientThread ([&] {
        std::vector<std::unique_ptr<CtlTestClient>> cs;
        for (int i = 0; i < 4; ++i)
        {
            cs.push_back (std::make_unique<CtlTestClient>());
            CHECK (cs.back()->connect (port) && ctlOk (cs.back()->hello (token)), "server: client " + juce::String (i + 1) + " accepted");
        }
        CtlTestClient fifth;
        CHECK (fifth.connect (port), "server: 5th client connects at TCP level");
        const auto r = juce::JSON::parse (fifth.readLine (3000));
        CHECK (ctlCode (r) == "busy", "server: 5th client refused (busy)");
    });

    // --- shutdown with a client connected and a request in flight ------------------
    {
        CtlTestClient c;
        std::atomic<bool> got { false };
        std::atomic<bool> helloOk { false }, go { false };
        juce::var r;
        std::thread t ([&] {
            if (c.connect (port) && ctlOk (c.hello (token)))
            {
                helloOk.store (true);
                while (!go.load()) juce::Thread::sleep (5);
                r = c.call ("app.state", {}, {}, 8000, 20000);   // in flight when stop() runs
            }
            got.store (true);
        });
        scPumpUntil ([&] { return helloOk.load(); }, 5000);
        go.store (true);
        juce::Thread::sleep (200);   // the app.state request is now posted, not served
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        srv.stop();
        const double el = juce::Time::getMillisecondCounterHiRes() - t0;
        t.join();
        CHECK (el < 2000.0, "server: stop() with a request in flight returns in < 2 s (" + juce::String (el, 0) + " ms)");
        CHECK (ctlCode (r) == "shutting_down" || r.isVoid(), "server: in-flight request answered shutting_down (or closed)");
        CHECK (!df.existsAsFile(), "server: discovery file removed on stop");
        CHECK (!srv.isRunning(), "server: stopped");
        scDeliverPostedMessages();
        juce::StreamingSocket probe;
        CHECK (!probe.connect ("127.0.0.1", port, 300), "server: port closed after stop");
    }

    // Restart works (Window menu toggle off/on).
    CHECK (srv.start (&err) && srv.port() > 0, "server: restarts");
    srv.stop();
    disc.deleteRecursively();
}

// Path roots (RAPPORT §5.3): home + scripts + temp; outside -> confirm_required;
// the control folder and the settings store are always refused.
static void testControlPathPolicy()
{
    Application app;
    const juce::File tmpFile = makeTempFile ("ctl_roots.wav");
    CHECK (scctl::checkPath (app, tmpFile, true, false).ok, "roots: temp folder allowed");
#if !JUCE_WINDOWS
    const juce::File outside ("/usr/share/sc-not-allowed.wav");
#else
    const juce::File outside ("C:\\Windows\\sc-not-allowed.wav");
#endif
    const auto o = scctl::checkPath (app, outside, true, false);
    CHECK (!o.ok && o.error.code == "confirm_required"
               && o.error.details.getProperty ("reason", {}).toString() == "outside_roots",
           "roots: outside the roots needs confirm");
    CHECK (scctl::checkPath (app, outside, true, true).ok, "roots: confirm accepts it");
    const auto ctl = scctl::checkPath (app, scctl::ControlServer::discoveryFolder().getChildFile ("1.json"), false, true);
    CHECK (!ctl.ok && ctl.error.code == "failed", "roots: control folder refused even with confirm");
    const auto st = scctl::checkPath (app, app.recentStore->getFile(), true, true);
    CHECK (!st.ok, "roots: settings store refused even with confirm");
    // doc.saveAs routes through it.
    const juce::File wav = ctlMakeWav ("ctl_roots_src.wav", 1);
    ctlCall (app, "doc.open", scctl::obj ({ { "path", wav.getFullPathName() } }));
    CHECK (ctlCode (ctlCall (app, "doc.saveAs", scctl::obj ({ { "path", outside.getFullPathName() } })))
               == "confirm_required", "roots: doc.saveAs outside the roots needs confirm");
#if !JUCE_WINDOWS
    // A symbolic link inside the temp folder pointing outside is resolved.
    const juce::File link = makeTempFile ("ctl_link_dir");
    if (juce::File ("/usr/share").createSymbolicLink (link, true))
    {
        const auto l = scctl::checkPath (app, link.getChildFile ("x.wav"), true, false);
        CHECK (!l.ok && l.error.code == "confirm_required", "roots: symlink out of the roots is resolved");
        link.deleteFile();
    }
    else
        scSkip ("roots: could not create a symbolic link");
#endif
}

// View / transport / markers / close commands (headless parts).
static void testControlViewTransport()
{
    Application app;
    const juce::File wav = ctlMakeWav ("ctl_view.wav", 2, 8000.0, 80000);   // 10 s
    const auto r = ctlCall (app, "doc.open", scctl::obj ({ { "path", wav.getFullPathName() } }));
    const int id = (int) ctlResult (r).getProperty ("doc", -1);
    auto v = ctlCall (app, "view.set", scctl::obj ({ { "t0", 1.5 }, { "t1", 4.0 } }));
    CHECK (ctlOk (v) && std::abs (app.viewStart - 1.5) < 1e-9 && std::abs (app.viewEnd - 4.0) < 1e-9, "view.set");
    v = ctlCall (app, "view.zoom", scctl::obj ({ { "factor", 2.0 } }));
    CHECK (ctlOk (v) && std::abs ((app.viewEnd - app.viewStart) - 1.25) < 1e-6, "view.zoom factor 2 halves the span");
    app.setSelection ({ 2.0, 3.5 });
    v = ctlCall (app, "view.zoom", scctl::obj ({ { "to", "selection" } }));
    CHECK (ctlOk (v) && std::abs (app.viewStart - 2.0) < 1e-9 && std::abs (app.viewEnd - 3.5) < 1e-9, "view.zoom to selection");
    CHECK (ctlCode (ctlCall (app, "view.zoom", scctl::obj ({ { "vertical", 2.0 } }))) == "needs_ui", "view.zoom vertical needs the window");
    CHECK (ctlCode (ctlCall (app, "view.show", scctl::obj ({ { "spectral", false } }))) == "needs_ui", "view.show needs the window");
    v = ctlCall (app, "view.channels", scctl::obj ({ { "visible", scctl::arr ({ 1 }) } }));
    CHECK (ctlOk (v) && app.visibleChannels.size() == 1 && app.visibleChannels[0] == 1, "view.channels");
    CHECK (ctlCode (ctlCall (app, "view.channels", scctl::obj ({ { "visible", scctl::arr ({ 5 }) } }))) == "invalid_params",
           "view.channels rejects a missing channel");

    // Transport: headless tests have no device (SC_NO_AUDIO_DEVICE): play succeeds silently.
    auto p = ctlCall (app, "transport.play", scctl::obj ({ { "from", 1.0 } }));
    CHECK (ctlOk (p) || ctlCode (p) == "failed", "transport.play answers (never blocks)");
    CHECK (ctlOk (ctlCall (app, "transport.stop")), "transport.stop");
    CHECK (ctlOk (ctlCall (app, "transport.loop", scctl::obj ({ { "on", true } }))) && app.audioEngine().isLooping(), "transport.loop on");
    CHECK (ctlOk (ctlCall (app, "transport.loop", scctl::obj ({ { "on", false } }))) && !app.audioEngine().isLooping(), "transport.loop off");

    // Markers.
    CHECK (ctlOk (ctlCall (app, "markers.add", scctl::obj ({ { "name", "door_slam" }, { "start", 4.1 }, { "end", 4.35 } }))),
           "markers.add region");
    CHECK (ctlOk (ctlCall (app, "markers.add", scctl::obj ({ { "name", "click" }, { "start", 1.0 } }))), "markers.add point");
    const auto ml = ctlResult (ctlCall (app, "markers.list"));
    CHECK (ml.getProperty ("markers", {}).size() == 2 && scctl::toLine (ml).contains ("door_slam"), "markers.list");

    // Close: unsaved needs confirm.
    ctlCall (app, "step.run", scctl::obj ({ { "step", scctl::obj ({ { "type", "invert" } }) } }));
    CHECK (ctlCode (ctlCall (app, "doc.close", {}, id)) == "confirm_required", "doc.close of an unsaved doc needs confirm");
    CHECK (ctlOk (ctlCall (app, "doc.close", scctl::obj ({ { "confirm", true } }), id)) && app.numDocs() == 0, "doc.close with confirm");
    CHECK (ctlCode (ctlCall (app, "app.quit")) == "needs_ui", "app.quit needs the window");

    // A streamed (not in memory) document: a mutating command becomes a job
    // that loads it into memory on a worker, then runs on the message thread.
    {
        const juce::File big = ctlMakeWav ("ctl_streamed.wav", 2, 8000.0, 40000);
        CHECK (app.openFile (big), "streamed: opened");
        FileDoc* d = app.doc();
        d->engine.setMaxInMemorySampleChannels (1000);   // force streaming on reload
        CHECK (d->engine.loadFile (big) && !d->engine.fullyInMemory(), "streamed: document is streamed");
        const auto j = ctlCall (app, "step.run", scctl::obj ({ { "select", "all" },
                                    { "step", scctl::obj ({ { "type", "fadeIn" } }) } }));
        CHECK (ctlOk (j) && ctlResult (j).getProperty ("job", {}).toString().startsWith ("j-"),
               "streamed: mutating command answered with a job");
        const auto inf = ctlWaitJob (app, ctlResult (j).getProperty ("job", {}).toString(), 20000);
        CHECK (inf.getProperty ("state", {}).toString() == "done"
                   && (bool) inf.getProperty ("result", {}).getProperty ("materialised", false),
               "streamed: job loaded the file then applied the step " + scctl::toLine (inf));
        CHECK (d->engine.fullyInMemory() && d->undo.undoSize() == 1, "streamed: in memory, one undo step");
    }
}

static void runRegression_controlServer()
{
    testControlServer();
    testControlPathPolicy();
    testControlViewTransport();
}
