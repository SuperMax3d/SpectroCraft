#pragma once
// SpectroCraft - AI control: loopback NDJSON server inside the open app.
//
// Listens on 127.0.0.1:<ephemeral port> and publishes {pid, port, token, ...}
// in <userApplicationDataDirectory>/SpectroCraft/control/<pid>.json (0600 on
// POSIX). Each client (max 4) must start with session.hello {token}; every
// other request is executed on the message thread through
// CommandRegistry::invoke with a timeout (PendingCall + abandoned flag):
// the network threads never touch Application or its documents.
// Opt-in: setting control_enabled (Window > Allow AI control) or --control.
#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class Application;

namespace sc::control {

class ClientSession;
class Journal;

class ControlServer : private juce::Thread
{
public:
    struct Options
    {
        juce::File discoveryDir;          // default: discoveryFolder()
        bool writeDiscoveryFile = true;
        int maxClients = 4;
        int callTimeoutMs = 30000;        // default per request (request timeoutMs overrides)
        juce::String token;               // default: a fresh Uuid
    };

    explicit ControlServer (Application& app);
    ControlServer (Application& app, Options opts);
    ~ControlServer() override;

    /** Opens the listener and starts the accept thread. Message thread. */
    bool start (juce::String* errOut = nullptr);
    /** Stops everything (poke + close + join, < 2 s): in-flight requests get
        shutting_down, the discovery file is removed. Message thread. */
    void stop();
    bool isRunning() const { return running.load(); }

    int port() const { return boundPort; }
    juce::String token() const { return opts.token; }
    juce::File discoveryFile() const { return discovery; }
    int numClients() const;

    /** <userApplicationDataDirectory>/SpectroCraft/control */
    static juce::File discoveryFolder();
    /** Removes <pid>.json files whose process is gone. Returns how many. */
    static int sweepStaleDiscoveryFiles (const juce::File& folder);
    static bool processAlive (int pid);

    /** Called on the message thread when the number of clients changes. */
    std::function<void (int)> onClientsChanged;

    // ---- internal (ClientSession) -------------------------------------------
    Application& application() { return app; }
    const Options& options() const { return opts; }
    std::atomic<bool> shuttingDown { false };
    /** Global brute-force brake: after 3 bad tokens, 2 s pause. */
    void noteAuthFailure();
    void noteAuthSuccess();
    bool authPaused() const;
    void sessionEnded (ClientSession*);
    void broadcast (const juce::String& eventName, const juce::var& data);
    /** The app journal (thread-safe; resolved on the message thread at start). */
    Journal& journal() { return *journalPtr; }

private:
    void run() override;
    bool writeDiscovery (juce::String* errOut);

    Application& app;
    Options opts;
    std::unique_ptr<juce::StreamingSocket> listener;
    int boundPort = 0;
    juce::File discovery;
    std::atomic<bool> running { false };

    mutable std::mutex sessionsLock;
    std::vector<std::unique_ptr<ClientSession>> sessions;
    std::vector<std::unique_ptr<ClientSession>> finished;   // joined later

    std::atomic<int> authFailures { 0 };
    std::atomic<double> authPauseUntil { 0.0 };
    std::shared_ptr<std::atomic<bool>> alive;               // cleared on stop (posted callbacks check it)
    Journal* journalPtr = nullptr;
};

} // namespace sc::control
