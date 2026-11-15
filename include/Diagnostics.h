#pragma once
// SpectroCraft - opt-in diagnostic log for AI-assisted support.
//
// Help > Diagnostic Log (off by default), --diag-log, or
// SC_DIAG_LOG=1 / SC_DIAG_LOG_DIR=<folder>. Schema and policy: docs/diagnostics.md.
//
// What it is:
//  - JSON Lines, one event per line ("ev":"domain.action"), schema version in
//    the session.start header, units in the keys (durMs, sizeMb).
//  - Written by exception: errors, warnings, slow operations, freezes and audio
//    dropouts, plus compact breadcrumbs (commands, menus, documents) and
//    per-minute aggregates. Normal operation writes very little.
//  - A flight recorder keeps the last debug-level events in memory and writes
//    them to the file only when an error happens.
//  - Budget: files rotate at 2 MB, at most 5 files (10 MB) in the folder.
//  - Local only: nothing is ever sent. Never audio, transcribed text, table
//    values, scripts or the AI control token. File paths are written in clear,
//    once per document (doc.open), later lines use the document id.
//
// Cost: when off, every hook is one relaxed atomic load (enabled()). When on,
// event() copies a small object into a bounded queue; a writer thread formats
// and writes. The audio thread never calls into this module.
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <utility>

namespace sc::diag
{
    enum class Level { debug = 0, info = 1, warn = 2, error = 3 };

    struct Config
    {
        juce::File folder;                          // empty = defaultFolder()
        juce::int64 maxFileBytes = 2 * 1024 * 1024; // rotation size of one file
        int maxFiles = 5;                           // diag-*.jsonl files kept in the folder
        int flightRecorderSize = 500;               // debug events kept in memory
        bool watchdog = true;                       // UI freeze detection (off for the test suite)
        bool crashHandlers = true;                  // signals / std::terminate / Windows hook
        juce::String reason = "setting";            // why it runs: setting | flag | env | tests
        juce::String appVersion;                    // SC_VERSION_STRING (passed in: Version.h changes every build)
    };

    //==========================================================================
    // Lifecycle (message thread)

    /** Starts a new session: opens a file in the folder, writes session.start
        (and session.unclean when the previous session did not end cleanly).
        Restarts when already running. False when the folder is not writable. */
    bool start (const Config& config = {});
    /** Writes session.end (counts, slowest operations), flushes, joins the
        threads and removes the "running" marker. No-op when not running. */
    void stop();
    bool isRunning();
    Config currentConfig();
    /** Application shutdown: the message loop no longer runs, so the freeze
        watchdog would report the teardown as a hang. The log keeps recording
        until stop(). */
    void stopFreezeWatchdog();
    /** True when a process with this id exists (pid reuse aside). */
    bool isProcessAlive (int pid);

    /** One relaxed atomic load: callers test it before building fields. */
    std::atomic<bool>& enabledFlag() noexcept;
    inline bool enabled() noexcept { return enabledFlag().load (std::memory_order_relaxed); }

    /** <user app data>/SpectroCraft/diagnostics */
    juce::File defaultFolder();
    /** Folder of the running session (or the configured default when off). */
    juce::File folder();
    /** File currently written (empty when off). */
    juce::File currentFile();
    /** Short random id of the running session ("7f3c9a"); empty when off. */
    juce::String sessionId();
    /** Seconds since the session started (monotonic clock). */
    double sessionTime();

    //==========================================================================
    // Events (any thread except the audio thread)

    using Fields = std::initializer_list<std::pair<const char*, juce::var>>;

    void event (const char* name, Level level, Fields fields = {});
    /** fieldsObject: a DynamicObject var (other types are ignored). */
    void eventObject (const char* name, Level level, const juce::var& fieldsObject);

    /** Waits (up to timeoutMs) until everything posted so far is on disk. */
    void flush (int timeoutMs = 2000);

    /** Short description of what the message thread is doing (a command, a
        menu item); reported by ui.stall. Message thread only. */
    class ScopedActivity
    {
    public:
        explicit ScopedActivity (const juce::String& what);
        ~ScopedActivity();
    private:
        juce::String previous;
        bool active = false;
        JUCE_DECLARE_NON_COPYABLE (ScopedActivity)
    };

    //==========================================================================
    // Integration hooks

    /** Extra session.start fields (audio device, workspace...). Called on the
        message thread when a session starts. */
    void setHeaderProvider (std::function<void (juce::DynamicObject&)> provider);
    /** Called once per second on the message thread while running (audio
        dropouts, device changes...); minuteTick is true every 60 s. */
    void setSampler (std::function<void (bool minuteTick)> sampler);

    /** Spectrogram: one getTiles() call that computed `computed` tiles in ms
        (computed may be 0: all hits). Any thread; lock-free unless the batch
        was slow (a debug event for the flight recorder). */
    void recordTiles (double ms, int computed, int hits, int fftSize);
    /** Audio callback load sample (0..1) while playing; message thread. */
    void recordAudioLoad (double load);

    /** Times a paint() call; writes paint.slow above 50 ms. */
    class ScopedPaint
    {
    public:
        ScopedPaint (const char* view, int width, int height) noexcept;
        ~ScopedPaint();
    private:
        const char* view;
        int w, h;
        double t0 = 0.0;
        JUCE_DECLARE_NON_COPYABLE (ScopedPaint)
    };

    /** Journal (AI control) entry: becomes a breadcrumb (cmd, job.end, step). */
    void noteJournalEntry (const juce::var& entry);
    /** A message box shown (or swallowed in tests) through sc::alert. */
    void noteAlert (int iconType, const juce::String& title, const juce::String& message);
    /** A menu bar item was chosen ("Effects > Normalize..."): ui.menu
        breadcrumb, also the "last" action reported with a freeze. */
    void noteMenu (const juce::String& item);

    //==========================================================================
    // Content policy (tested)

    /** Truncates to maxChars (keeps the start, adds "...") and replaces every
        registered secret with "[redacted]". */
    juce::String clean (const juce::String& text, int maxChars = 500);
    /** Never written to the log (e.g. the AI control token). */
    void addSecret (const juce::String& secret);
    /** Known error hint for a message ("ai.runtime", "fs.write"...), or empty. */
    juce::String hintFor (const juce::String& message);

    //==========================================================================
    // Reading back (AI support): from the files on disk, after a flush.

    /** Markdown summary of the running session (or the latest one on disk),
        at most maxBytes, ready to paste into a chat. */
    juce::String summaryMarkdown (int maxBytes = 30 * 1024);
    /** Events of the running (or latest) session with t > since, filtered by
        kind ("audio" matches audio.*), the last `limit` ones:
        {enabled, sid, file, events:[...], next}. */
    juce::var tail (double since, const juce::StringArray& kinds, int limit);
    /** {enabled, sid, file, folder, files:[{name, sizeBytes}], totalBytes} */
    juce::var status();
    /** Zip of the log files, crash reports and a summary. */
    bool writeSupportBundle (const juce::File& zip, juce::String* error = nullptr);
    /** Files a support bundle would contain (log files and crash reports). */
    juce::Array<juce::File> bundleFiles();

    //==========================================================================
    // Crash reporting. The handlers write crash-<sid>.txt with async-signal-safe
    // calls only; the next session embeds it in its session.unclean event.
    // Installed by start() when Config::crashHandlers is true.

    /** Windows: called by include/CrashTrace.h before it terminates the process. */
    void writeWindowsCrash (unsigned long code) noexcept;

    namespace testing
    {
        /** Stops the threads like a crash would: no session.end, the marker stays. */
        void abandon();
        /** Writes a crash report as the signal handler would (no signal raised). */
        void writeCrashReport (int signalNumber);
        /** -1 = ask the OS, 0 = background, 1 = foreground (stall detection). */
        void setForegroundOverride (int value);
        /** Heartbeat period of the stall watchdog, ms (default 50). */
        void setHeartbeatMs (int ms);
        /** Writes the per-minute aggregates now (spectral.tiles, audio.load,
            mem) instead of waiting for the next minute. Message thread. */
        void runMinuteTick();
    }
}
