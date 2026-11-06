// SpectroCraft - AI control: loopback NDJSON server (see include/control/ControlServer.h).
//
// Threads: "SC-Control" accepts; one "SC-ControlClient" thread per client
// reads lines, answers session-level requests itself and posts every command
// to the message thread (PendingCall: shared state + timeout + abandoned).
// Writes to a socket happen only on its session thread (events are queued).
#include "control/ControlServer.h"
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "control/JsonCli.h"     // kMaxLineBytes
#include "control/StateJson.h"
#include "Application.h"

#include <deque>
#include <random>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <cerrno>
 #include <csignal>
 #include <sys/stat.h>
 #include <sys/types.h>
 #include <unistd.h>
#endif

namespace sc::control {

// ------------------------------------------------------------------ helpers

/** 256-bit token from the OS CSPRNG (std::random_device: /dev/urandom on
    macOS / Linux, RtlGenRandom on MSVC). juce::Uuid uses juce::Random, which
    is not meant for secrets. */
static juce::String makeToken()
{
    std::random_device rd;
    juce::String t;
    for (int i = 0; i < 8; ++i)
        t << juce::String::toHexString ((juce::int64) rd()).paddedLeft ('0', 8);
    return t;
}

static double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

static bool tokensEqual (const juce::String& a, const juce::String& b)
{
    // Constant-time for equal lengths (the token length is public).
    const auto* x = a.toRawUTF8();
    const auto* y = b.toRawUTF8();
    const size_t n = a.getNumBytesAsUTF8(), m = b.getNumBytesAsUTF8();
    unsigned diff = (unsigned) (n ^ m);
    for (size_t i = 0; i < std::max (n, m); ++i)
        diff |= (unsigned) ((i < n ? (unsigned char) x[i] : 0u) ^ (i < m ? (unsigned char) y[i] : 0u));
    return diff == 0 && n > 0;
}

/** Maps a journal entry to a protocol event name (empty = no specific event). */
static juce::String eventForEntry (const juce::var& e)
{
    const auto kind = e.getProperty ("kind", {}).toString();
    // Every new undo patch, undo/redo and history clear is a "history" entry
    // (ControlState::pollDocuments / Application::undoSteps), whoever made it.
    if (kind == "history") return "history.changed";
    if (kind == "doc") return "doc.changed";
    if (kind == "meta") return "meta.changed";
    return {};
}

/** True when the JSON text nests deeper than maxDepth ([ / { outside
    strings). juce::JSON::parse is recursive: a line of 100 000 '[' would
    overflow the session thread's stack before any authentication. */
static bool jsonTooDeep (const std::string& t, int maxDepth)
{
    int depth = 0;
    bool inStr = false, esc = false;
    for (char ch : t)
    {
        if (inStr)
        {
            if (esc) esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"') inStr = false;
            continue;
        }
        if (ch == '"') inStr = true;
        else if (ch == '[' || ch == '{') { if (++depth > maxDepth) return true; }
        else if (ch == ']' || ch == '}') --depth;
    }
    return false;
}

/** Subscription kind of an event name ("job.done" -> "job"). */
static juce::String kindOfEvent (const juce::String& name)
{
    if (name == "journal.entry") return "journal";
    if (name.startsWith ("events.")) return "*";
    return name.upToFirstOccurrenceOf (".", false, false);
}

// ------------------------------------------------------------------ ClientSession

class ClientSession : public juce::Thread
{
public:
    ClientSession (ControlServer& s, std::unique_ptr<juce::StreamingSocket> sock, int n)
        : juce::Thread ("SC-ControlClient-" + juce::String (n)), server (s), socket (std::move (sock))
    {
    }

    ~ClientSession() override
    {
        signalThreadShouldExit();
        closeSocket();
        stopThread (3000);
    }

    void closeSocket()
    {
        std::lock_guard<std::mutex> l (sockLock);
        if (socket != nullptr) socket->close();
    }

    /** Any thread: queue an event for this client (bounded, oldest dropped). */
    void enqueueEvent (const juce::String& name, const juce::var& data, juce::int64 seq)
    {
        if (!authed.load()) return;
        {
            std::lock_guard<std::mutex> l (qLock);
            if (!subscribedTo (kindOfEvent (name))) return;
            auto ev = obj ({ { "event", name }, { "seq", seq }, { "ts", isoNow() }, { "data", data } });
            queue.push_back (toLine (ev));
            while (queue.size() > 1000)
            {
                queue.pop_front();
                ++dropped;
            }
        }
        notify();
    }

    /** Any thread: one event per journal entry, with the entry's own seq: the
        specific event (history.changed...) when subscribed to it, otherwise
        journal.entry when subscribed to "journal". */
    void enqueueJournalEntry (const juce::var& e)
    {
        const auto seq = (juce::int64) e.getProperty ("seq", 0);
        const auto ev = eventForEntry (e);
        bool specific = false;
        {
            std::lock_guard<std::mutex> l (qLock);
            specific = ev.isNotEmpty() && subscribedTo (kindOfEvent (ev));
        }
        enqueueEvent (specific ? ev : juce::String ("journal.entry"), e, seq);
    }

    void run() override
    {
        std::string buf;
        const double startMs = nowMs();
        bool discarding = false;   // inside an oversized line
        char chunk[16384];
        while (!threadShouldExit() && !server.shuttingDown.load())
        {
            flushQueue();
            // A connection must authenticate within 10 s (it holds one of the 4 slots).
            if (!authed.load() && nowMs() - startMs > 10000.0)
            {
                send (toLine (obj ({ { "id", juce::var() }, { "ok", false },
                                     { "error", errorVar ({ err::unauthorized, "no session.hello within 10 s", {} }) } })));
                break;
            }
            // Before authentication, lines are tiny (a hello is < 1 KiB).
            const size_t maxLine = authed.load() ? (size_t) kMaxLineBytes : (size_t) 4096;
            int ready = 0;
            {
                juce::StreamingSocket* s = socket.get();
                ready = s != nullptr ? s->waitUntilReady (true, 50) : -1;
            }
            if (ready < 0) break;
            if (ready == 0) continue;
            const int n = socket->read (chunk, (int) sizeof (chunk), false);
            if (n <= 0) break;   // closed by the peer
            buf.append (chunk, (size_t) n);

            size_t pos;
            while ((pos = buf.find ('\n')) != std::string::npos)
            {
                std::string line = buf.substr (0, pos);
                buf.erase (0, pos + 1);
                if (discarding) { discarding = false; continue; }
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() > maxLine)
                {
                    send (toLine (obj ({ { "id", juce::var() }, { "ok", false },
                                         { "error", errorVar ({ err::invalidParams, maxLine == (size_t) kMaxLineBytes
                                                                   ? juce::String ("line exceeds 1 MiB")
                                                                   : juce::String ("line too long before session.hello"),
                                                               obj ({ { "path", "" }, { "expected", "<= 1048576 bytes" } }) }) } })));
                    continue;
                }
                handleLine (line);
                if (threadShouldExit() || closeAfterReply) break;
            }
            if (closeAfterReply) break;
            if (buf.size() > maxLine)
            {
                send (toLine (obj ({ { "id", juce::var() }, { "ok", false },
                                     { "error", errorVar ({ err::invalidParams, "line exceeds 1 MiB", {} }) } })));
                buf.clear();
                discarding = true;
            }
        }
        flushQueue();
        closeSocket();
        server.sessionEnded (this);
    }

    std::atomic<bool> authed { false };

private:
    bool subscribedTo (const juce::String& kind) const
    {
        return kind == "*" || subscriptions.contains (kind) || subscriptions.contains ("*");
    }

    bool send (const juce::String& line)
    {
        std::lock_guard<std::mutex> l (sockLock);
        if (socket == nullptr || !socket->isConnected()) return false;
        const juce::String withNl = line + "\n";
        const char* p = withNl.toRawUTF8();
        int left = (int) withNl.getNumBytesAsUTF8();
        const double deadline = nowMs() + 5000.0;   // a client that stops reading is dropped
        while (left > 0)
        {
            if (server.shuttingDown.load() || nowMs() > deadline) return false;
            const int r = socket->waitUntilReady (false, 100);
            if (r < 0) return false;
            if (r == 0) continue;
            const int w = socket->write (p, left);
            if (w <= 0) return false;
            p += w; left -= w;
        }
        return true;
    }

    void flushQueue()
    {
        std::deque<juce::String> lines;
        int lost = 0;
        {
            std::lock_guard<std::mutex> l (qLock);
            lines.swap (queue);
            lost = dropped;
            dropped = 0;
        }
        if (lost > 0)
            send (toLine (obj ({ { "event", "events.dropped" }, { "ts", isoNow() },
                                 { "data", obj ({ { "count", lost } }) } })));
        for (auto& l : lines)
            if (!send (l)) break;
    }

    void respond (const juce::var& id, bool ok, const juce::var& resultOrError)
    {
        auto r = obj ({ { "id", id }, { "ok", ok } });
        set (r, ok ? "result" : "error", resultOrError);
        send (toLine (r));
    }

    void respondError (const juce::var& id, const juce::String& code, const juce::String& msg,
                       const juce::var& details = {})
    {
        respond (id, false, errorVar ({ code, msg, details }));
    }

    /** Replays journal entries after sinceSeq as events (subscribed kinds). */
    int replay (juce::int64 sinceSeq)
    {
        int n = 0;
        for (const auto& e : server.journal().entriesSince (sinceSeq))
        {
            enqueueJournalEntry (e);
            ++n;
        }
        return n;
    }

    int countReplay (juce::int64 sinceSeq)
    {
        int n = 0;
        for (const auto& e : server.journal().entriesSince (sinceSeq))
        {
            const auto ev = eventForEntry (e);
            std::lock_guard<std::mutex> l (qLock);
            if ((ev.isNotEmpty() && subscribedTo (kindOfEvent (ev))) || subscribedTo ("journal")) ++n;
        }
        return n;
    }

    void setSubscriptions (const juce::var& kinds)
    {
        std::lock_guard<std::mutex> l (qLock);
        subscriptions.clear();
        if (kinds.isArray())
            for (int i = 0; i < kinds.size(); ++i) subscriptions.add (kinds[i].toString());
    }

    void handleLine (const std::string& raw)
    {
        const juce::String text = juce::String::fromUTF8 (raw.data(), (int) raw.size()).trim();
        if (text.isEmpty()) return;
        if (jsonTooDeep (raw, 64))
        {
            respondError ({}, err::invalidParams, "JSON nested deeper than 64 levels",
                          obj ({ { "path", "" }, { "expected", "nesting <= 64" } }));
            return;
        }
        juce::var v;
        if (juce::JSON::parse (text, v).failed() || !isObj (v))
        {
            respondError ({}, err::invalidParams, "unreadable JSON",
                          obj ({ { "path", "" }, { "expected", "one JSON object per line" } }));
            return;
        }
        Request req;
        Error perr;
        if (!Request::parse (v, req, perr))
        {
            respondError (v.getProperty ("id", {}), perr.code, perr.message, perr.details);
            return;
        }

        // --- authentication: session.hello {token} must come first ---------
        if (!authed.load())
        {
            if (req.cmd != "session.hello")
            {
                respondError (req.id, err::unauthorized, "send session.hello {token} first");
                return;
            }
            while (server.authPaused() && !threadShouldExit())
                juce::Thread::sleep (50);
            const juce::String tok = req.params.getProperty ("token", {}).toString();
            if (!tokensEqual (tok, server.token()))
            {
                server.noteAuthFailure();
                respondError (req.id, err::unauthorized, "bad token",
                              obj ({ { "hint", "read the token from the discovery file <pid>.json" } }));
                return;
            }
            server.noteAuthSuccess();
            client = req.params.getProperty ("client", "client").toString().substring (0, 64);
            if (client.isEmpty()) client = "client";
            setSubscriptions (req.params.getProperty ("subscribe", {}));
            auto info = dispatch (Request { req.id, "app.hello.internal", {}, {}, 10000 });
            if (!isObj (info))
                return;   // dispatch already answered (timeout / shutting down); still unauthenticated
            // Count what the replay will send, answer, THEN authenticate and
            // replay: no event may precede the hello response.
            const juce::int64 since = req.params.hasProperty ("sinceSeq")
                                          ? (juce::int64) (double) req.params.getProperty ("sinceSeq", 0) : -1;
            set (info, "client", client);
            set (info, "replayed", since >= 0 ? countReplay (since) : 0);
            respond (req.id, true, info);
            authed.store (true);
            if (since >= 0) replay (since);
            return;
        }

        if (req.cmd == "session.hello")
        {
            auto info = dispatch (Request { req.id, "app.hello.internal", {}, {}, 10000 });
            if (isObj (info)) respond (req.id, true, info);   // else: dispatch already answered
            return;
        }
        if (req.cmd == "events.subscribe")
        {
            setSubscriptions (req.params.getProperty ("kinds", {}));
            const juce::int64 since = req.params.hasProperty ("sinceSeq")
                                          ? (juce::int64) (double) req.params.getProperty ("sinceSeq", 0) : -1;
            respond (req.id, true, obj ({ { "subscribed", true }, { "replayed", since >= 0 ? countReplay (since) : 0 } }));
            if (since >= 0) replay (since);
            return;
        }
        if (req.cmd == "session.bye")
        {
            respond (req.id, true, obj ({ { "bye", true } }));
            closeAfterReply = true;
            return;
        }

        const juce::var r = dispatchFull (req);
        send (toLine (r));
    }

    struct PendingCall
    {
        juce::WaitableEvent done;
        juce::var out;
        std::atomic<bool> abandoned { false };
    };

    /** Posts a request to the message thread and waits (bounded). Returns the
        full response envelope. Never touches Application on this thread. */
    juce::var dispatchFull (const Request& req)
    {
        auto st = std::make_shared<PendingCall>();
        Application* app = &server.application();
        const juce::String source = "api:" + client;
        auto alive = aliveFlag;
        if (!juce::MessageManager::callAsync ([st, app, req, source, alive] {
                if (st->abandoned.load() || !alive->load()) return;
                if (req.cmd == "app.hello.internal")
                {
                    st->out = obj ({ { "id", req.id }, { "ok", true },
                                     { "result", helloInfo (*app, app->control().ui != nullptr) } });
                }
                else
                {
                    InvokeOptions io;
                    io.source = source;
                    st->out = CommandRegistry::instance().invoke (*app, req, io);
                }
                st->done.signal();
            }))
            return obj ({ { "id", req.id }, { "ok", false },
                          { "error", errorVar ({ err::shuttingDown, "the application is shutting down", {} }) } });

        const int timeout = req.timeoutMs > 0 ? req.timeoutMs : server.options().callTimeoutMs;
        const double t0 = nowMs();
        while (!st->done.wait (20))
        {
            flushQueue();
            if (threadShouldExit() || server.shuttingDown.load())
            {
                st->abandoned.store (true);
                return obj ({ { "id", req.id }, { "ok", false },
                              { "error", errorVar ({ err::shuttingDown, "the application is shutting down", {} }) } });
            }
            if (nowMs() - t0 > timeout)
            {
                st->abandoned.store (true);
                return obj ({ { "id", req.id }, { "ok", false },
                              { "error", errorVar ({ err::timeout,
                                                     "Message thread did not respond within " + juce::String (timeout) + " ms",
                                                     obj ({ { "hint", "a modal dialog is probably open in the app (or the command is still running)" } }) }) } });
            }
        }
        return st->out;
    }

    juce::var dispatch (const Request& req)
    {
        const auto r = dispatchFull (req);
        if (!(bool) r.getProperty ("ok", false))
        {
            send (toLine (r));
            return {};
        }
        return r.getProperty ("result", {});
    }

public:
    std::shared_ptr<std::atomic<bool>> aliveFlag;

private:
    ControlServer& server;
    std::unique_ptr<juce::StreamingSocket> socket;
    std::mutex sockLock;
    std::mutex qLock;
    std::deque<juce::String> queue;
    int dropped = 0;
    juce::StringArray subscriptions;
    juce::String client = "client";
    bool closeAfterReply = false;
};

// ------------------------------------------------------------------ ControlServer

ControlServer::ControlServer (Application& a) : ControlServer (a, Options {}) {}

ControlServer::ControlServer (Application& a, Options o)
    : juce::Thread ("SC-Control"), app (a), opts (std::move (o)),
      alive (std::make_shared<std::atomic<bool>> (false))
{
    if (opts.token.isEmpty())
        opts.token = makeToken();
    if (opts.discoveryDir == juce::File())
        opts.discoveryDir = discoveryFolder();
}

ControlServer::~ControlServer()
{
    stop();
}

juce::File ControlServer::discoveryFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("SpectroCraft").getChildFile ("control");
}

bool ControlServer::processAlive (int pid)
{
    if (pid <= 0) return false;
#if JUCE_WINDOWS
    HANDLE h = OpenProcess (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (h == nullptr)
        return GetLastError() == ERROR_ACCESS_DENIED;   // exists, owned by someone else
    const bool running = WaitForSingleObject (h, 0) == WAIT_TIMEOUT;
    CloseHandle (h);
    return running;
#else
    return ::kill ((pid_t) pid, 0) == 0 || errno == EPERM;
#endif
}

int ControlServer::sweepStaleDiscoveryFiles (const juce::File& folder)
{
    int removed = 0;
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const int pid = f.getFileNameWithoutExtension().getIntValue();
        if (pid <= 0 || pid == processId()) continue;
        if (!processAlive (pid) && f.deleteFile()) ++removed;
    }
    return removed;
}

bool ControlServer::writeDiscovery (juce::String* errOut)
{
    const juce::File dir = opts.discoveryDir;
    if (!dir.createDirectory())
    {
        if (errOut) *errOut = "cannot create " + dir.getFullPathName();
        return false;
    }
#if !JUCE_WINDOWS
    ::chmod (dir.getFullPathName().toRawUTF8(), 0700);
#endif
    sweepStaleDiscoveryFiles (dir);

    const auto content = juce::JSON::toString (obj ({
        { "pid", processId() }, { "port", boundPort }, { "token", opts.token },
        { "version", appVersion() }, { "protocol", kProtocolVersion },
        { "startedAt", isoNow() },
        { "exe", juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName() },
        { "platform", platformName() } }));

    discovery = dir.getChildFile (juce::String (processId()) + ".json");
    const juce::File tmp = dir.getChildFile (juce::String (processId()) + ".json.tmp");
    tmp.deleteFile();
    // Create the file empty, restrict it, THEN write the token into it.
    if (!tmp.create())
    {
        if (errOut) *errOut = "cannot write " + tmp.getFullPathName();
        return false;
    }
#if !JUCE_WINDOWS
    ::chmod (tmp.getFullPathName().toRawUTF8(), 0600);
#endif
    if (!tmp.replaceWithText (content) || !tmp.moveFileTo (discovery))
    {
        tmp.deleteFile();
        if (errOut) *errOut = "cannot write " + discovery.getFullPathName();
        return false;
    }
#if !JUCE_WINDOWS
    ::chmod (discovery.getFullPathName().toRawUTF8(), 0600);
#endif
    return true;
}

bool ControlServer::start (juce::String* errOut)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (running.load()) return true;
    shuttingDown.store (false);
    journalPtr = &app.control().journal;   // created on the message thread, before any network thread exists

    listener = std::make_unique<juce::StreamingSocket>();
    if (!listener->createListener (0, "127.0.0.1"))
    {
        if (errOut) *errOut = "cannot listen on 127.0.0.1";
        listener.reset();
        return false;
    }
    boundPort = listener->getBoundPort();
    if (boundPort <= 0)
    {
        if (errOut) *errOut = "no port bound";
        listener->close();
        listener.reset();
        return false;
    }
    alive->store (true);
    // Events: every journal entry and job event fans out to the clients.
    auto aliveCopy = alive;
    app.control().eventSink = [this, aliveCopy] (const juce::String& name, const juce::var& data) {
        if (aliveCopy->load()) broadcast (name, data);
    };
    app.control().journal.onAppend = [this, aliveCopy] (const juce::var& e) {
        if (!aliveCopy->load()) return;
        std::lock_guard<std::mutex> l (sessionsLock);
        for (auto& s : sessions)
            s->enqueueJournalEntry (e);
    };

    running.store (true);
    startThread();
    // Publish only once the accept thread runs: a client reading the file
    // can connect at once.
    if (opts.writeDiscoveryFile && !writeDiscovery (errOut))
    {
        stop();
        return false;
    }
    return true;
}

void ControlServer::stop()
{
    if (!running.exchange (false)) return;
    shuttingDown.store (true);
    alive->store (false);
    app.control().eventSink = nullptr;
    app.control().journal.onAppend = nullptr;

    // JUCE's close() unblocks accept() by connecting to portNumber, which is
    // still 0 after createListener(0, ...): poke our real port first.
    {
        juce::StreamingSocket poke;
        poke.connect ("127.0.0.1", boundPort, 500);
    }
    signalThreadShouldExit();
    if (listener != nullptr) listener->close();
    stopThread (2000);
    listener.reset();

    std::vector<std::unique_ptr<ClientSession>> all;
    {
        std::lock_guard<std::mutex> l (sessionsLock);
        for (auto& s : sessions) all.push_back (std::move (s));
        for (auto& s : finished) all.push_back (std::move (s));
        sessions.clear();
        finished.clear();
    }
    for (auto& s : all) { s->signalThreadShouldExit(); s->closeSocket(); }
    all.clear();   // joins (each < 1 s: 50 ms polls, 20 ms pending-call polls)

    if (discovery != juce::File())
        discovery.deleteFile();
    discovery = juce::File();
    if (onClientsChanged) onClientsChanged (0);
}

int ControlServer::numClients() const
{
    std::lock_guard<std::mutex> l (sessionsLock);
    int n = 0;
    for (auto& s : sessions) if (s->authed.load()) ++n;
    return n;
}

void ControlServer::noteAuthFailure()
{
    if (authFailures.fetch_add (1) + 1 >= 3)
    {
        authPauseUntil.store (nowMs() + 2000.0);
        authFailures.store (0);
    }
}

void ControlServer::noteAuthSuccess()
{
    authFailures.store (0);
    auto aliveCopy = alive;
    juce::MessageManager::callAsync ([this, aliveCopy] {
        if (aliveCopy->load() && onClientsChanged) onClientsChanged (numClients());
    });
}

bool ControlServer::authPaused() const
{
    return nowMs() < authPauseUntil.load();
}

void ControlServer::sessionEnded (ClientSession* s)
{
    {
        std::lock_guard<std::mutex> l (sessionsLock);
        for (auto it = sessions.begin(); it != sessions.end(); ++it)
            if (it->get() == s)
            {
                finished.push_back (std::move (*it));   // joined by the accept thread or stop()
                sessions.erase (it);
                break;
            }
    }
    auto aliveCopy = alive;
    juce::MessageManager::callAsync ([this, aliveCopy] {
        if (aliveCopy->load() && onClientsChanged) onClientsChanged (numClients());
    });
}

void ControlServer::broadcast (const juce::String& eventName, const juce::var& data)
{
    // Same counter as the journal (clients drop events whose seq is not
    // greater than the last one seen): a fresh seq, no journal entry.
    const auto seq = app.control().journal.reserveSeq();
    std::lock_guard<std::mutex> l (sessionsLock);
    for (auto& s : sessions) s->enqueueEvent (eventName, data, seq);
}

void ControlServer::run()
{
    int counter = 0;
    while (!threadShouldExit() && !shuttingDown.load())
    {
        // Join sessions that ended (their threads have returned from run()).
        {
            std::vector<std::unique_ptr<ClientSession>> done;
            {
                std::lock_guard<std::mutex> l (sessionsLock);
                done.swap (finished);
            }
            done.clear();
        }

        if (listener == nullptr || listener->waitUntilReady (true, 500) <= 0)
            continue;
        if (threadShouldExit() || shuttingDown.load()) break;
        std::unique_ptr<juce::StreamingSocket> c (listener->waitForNextConnection());
        if (c == nullptr) continue;
        if (threadShouldExit() || shuttingDown.load()) break;   // the shutdown poke

        size_t count = 0;
        {
            std::lock_guard<std::mutex> l (sessionsLock);
            count = sessions.size();
        }
        if ((int) count >= opts.maxClients)
        {
            const juce::String line = toLine (obj ({ { "id", juce::var() }, { "ok", false },
                { "error", errorVar ({ err::busy, "too many clients (max " + juce::String (opts.maxClients) + ")", {} }) } })) + "\n";
            c->write (line.toRawUTF8(), (int) line.getNumBytesAsUTF8());
            c->close();
            continue;
        }
        auto s = std::make_unique<ClientSession> (*this, std::move (c), ++counter);
        s->aliveFlag = alive;
        ClientSession* raw = s.get();
        {
            std::lock_guard<std::mutex> l (sessionsLock);
            sessions.push_back (std::move (s));
        }
        raw->startThread();
    }
}

} // namespace sc::control
