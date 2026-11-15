// SpectroCraft - opt-in diagnostic log for AI-assisted support.
// See include/Diagnostics.h for the API and docs/diagnostics.md for the schema.
#include "Diagnostics.h"
#include "AlertGate.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
 #undef min
 #undef max
 #pragma comment (lib, "psapi.lib")
#else
 #include <cerrno>
 #include <execinfo.h>
 #include <fcntl.h>
 #include <signal.h>
 #include <sys/types.h>
 #include <time.h>
 #include <unistd.h>
#endif
#if JUCE_MAC
 #include <mach/mach.h>
#endif

namespace sc::diag
{

std::atomic<bool>& enabledFlag() noexcept
{
    static std::atomic<bool> flag { false };
    return flag;
}

namespace
{
    constexpr int kSchemaVersion = 1;
    constexpr double kDedupWindowS = 10.0;
    constexpr int kRatePerMinute = 60;          // written lines per event name and minute
    constexpr size_t kMaxQueue = 10000;         // pending events (dropped and counted beyond)
    constexpr double kStallWarnMs = 250.0;
    constexpr double kStallErrorMs = 2000.0;
    constexpr double kSlowPaintMs = 50.0;
    constexpr int kWorstKept = 10;
    constexpr double kMarkerStaleMs = 10.0 * 60.0 * 1000.0;   // a live session touches its marker every minute

    double nowMs() noexcept { return juce::Time::getMillisecondCounterHiRes(); }

    const char* levelName (Level l)
    {
        switch (l)
        {
            case Level::debug: return "debug";
            case Level::warn:  return "warn";
            case Level::error: return "error";
            case Level::info:  break;
        }
        return "info";
    }

    int currentPid()
    {
       #if JUCE_WINDOWS
        return (int) GetCurrentProcessId();
       #else
        return (int) getpid();
       #endif
    }

    bool processAlive (int pid)
    {
        if (pid <= 0) return false;
       #if JUCE_WINDOWS
        HANDLE h = OpenProcess (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
        if (h == nullptr)
            return GetLastError() == ERROR_ACCESS_DENIED;
        const bool alive = WaitForSingleObject (h, 0) == WAIT_TIMEOUT;
        CloseHandle (h);
        return alive;
       #else
        return ::kill ((pid_t) pid, 0) == 0 || errno == EPERM;
       #endif
    }

    // Resident memory of this process and memory still available on the
    // machine, in MB (-1 when unknown).
    double processMemoryMb()
    {
       #if JUCE_MAC
        task_vm_info_data_t vm {};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info (mach_task_self(), TASK_VM_INFO, (task_info_t) &vm, &count) == KERN_SUCCESS)
            return (double) vm.phys_footprint / (1024.0 * 1024.0);
        return -1.0;
       #elif JUCE_WINDOWS
        PROCESS_MEMORY_COUNTERS pmc {};
        if (GetProcessMemoryInfo (GetCurrentProcess(), &pmc, sizeof (pmc)))
            return (double) pmc.WorkingSetSize / (1024.0 * 1024.0);
        return -1.0;
       #else
        long pages = 0, resident = 0;
        if (FILE* f = std::fopen ("/proc/self/statm", "r"))
        {
            if (std::fscanf (f, "%ld %ld", &pages, &resident) != 2) resident = -1;
            std::fclose (f);
            if (resident >= 0)
                return (double) resident * (double) sysconf (_SC_PAGESIZE) / (1024.0 * 1024.0);
        }
        return -1.0;
       #endif
    }

    double availableMemoryMb()
    {
       #if JUCE_MAC
        static const mach_port_t host = mach_host_self();
        vm_statistics64_data_t vs {};
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (host_statistics64 (host, HOST_VM_INFO64, (host_info64_t) &vs, &count) != KERN_SUCCESS)
            return -1.0;
        const double pages = (double) vs.free_count + (double) vs.inactive_count
                             + (double) vs.purgeable_count + (double) vs.speculative_count;
        return pages * (double) vm_page_size / (1024.0 * 1024.0);
       #elif JUCE_WINDOWS
        MEMORYSTATUSEX ms {};
        ms.dwLength = sizeof (ms);
        if (GlobalMemoryStatusEx (&ms))
            return (double) ms.ullAvailPhys / (1024.0 * 1024.0);
        return -1.0;
       #else
        if (FILE* f = std::fopen ("/proc/meminfo", "r"))
        {
            char key[64] = { 0 };
            long kb = 0;
            double out = -1.0;
            while (std::fscanf (f, "%63s %ld kB\n", key, &kb) == 2)
                if (std::strcmp (key, "MemAvailable:") == 0) { out = (double) kb / 1024.0; break; }
            std::fclose (f);
            return out;
        }
        return -1.0;
       #endif
    }

    // createDirectory() fails when another process creates a missing parent at
    // the same moment (mkdir: EEXIST), e.g. the ctest shards' common diag/
    // folder: try again once the parent exists.
    bool ensureFolder (const juce::File& d)
    {
        for (int attempt = 0; attempt < 4 && !d.isDirectory(); ++attempt)
            d.createDirectory();
        return d.isDirectory();
    }

    // Another instance still writing its session: alive and its marker fresh
    // (a stale marker with a reused pid is a crashed session).
    bool markerIsLive (const juce::File& marker, int pid)
    {
        if (pid == currentPid() || !processAlive (pid)) return false;
        const auto age = juce::Time::getCurrentTime() - marker.getLastModificationTime();
        return (double) age.inMilliseconds() < kMarkerStaleMs;
    }

    double round3 (double v) { return std::isfinite (v) ? std::round (v * 1000.0) / 1000.0 : 0.0; }
    double round1 (double v) { return std::isfinite (v) ? std::round (v * 10.0) / 10.0 : 0.0; }

    juce::String toJson (const juce::var& v)
    {
        return juce::JSON::toString (v, juce::JSON::FormatOptions{}
                                            .withSpacing (juce::JSON::Spacing::none)
                                            .withMaxDecimalPlaces (3));
    }

    //==========================================================================
    // Content policy
    std::mutex& secretsLock() { static std::mutex m; return m; }
    juce::StringArray& secrets() { static juce::StringArray s; return s; }

    // Keys never written, whatever the event: content, not metadata.
    bool excludedKey (const juce::Identifier& k)
    {
        static const char* const keys[] = { "token", "text", "script", "scriptText", "code", "content",
                                            "values", "samples", "prompt", "transcript", "rows" };
        for (auto* s : keys)
            if (k.toString() == s) return true;
        return false;
    }

    juce::var cleanVar (const juce::var& v, int depth)
    {
        if (v.isString())
            return clean (v.toString());
        if (v.isDouble() && !std::isfinite ((double) v))
            return {};
        if (auto* a = v.getArray())
        {
            juce::Array<juce::var> out;
            if (depth >= 4) return juce::var ("...");
            const int n = std::min (a->size(), 100);
            for (int i = 0; i < n; ++i)
                out.add (cleanVar (a->getReference (i), depth + 1));
            if (a->size() > n) out.add ("...");
            return out;
        }
        if (auto* o = v.getDynamicObject())
        {
            if (depth >= 4) return juce::var ("...");
            auto* c = new juce::DynamicObject();
            for (const auto& p : o->getProperties())
                if (!excludedKey (p.name) && !p.value.isVoid())
                    c->setProperty (p.name, cleanVar (p.value, depth + 1));
            return juce::var (c);
        }
        if (v.isMethod() || v.isBinaryData())
            return {};
        return v;
    }

    //==========================================================================
    // Lock-free latency histogram (log-spaced buckets, ms).
    class Histogram
    {
    public:
        void add (double ms) noexcept
        {
            int b = 0;
            while (b < kBounds && ms > bound (b)) ++b;
            counts[b].fetch_add (1, std::memory_order_relaxed);
            const auto us = (uint64_t) std::max (0.0, ms * 1000.0);
            auto cur = maxUs.load (std::memory_order_relaxed);
            while (us > cur && !maxUs.compare_exchange_weak (cur, us, std::memory_order_relaxed)) {}
        }

        struct Snap { int n = 0; double p50 = 0.0, p95 = 0.0, max = 0.0; };

        Snap take() noexcept
        {
            Snap s;
            uint32_t c[kBounds + 1];
            uint64_t total = 0;
            for (int i = 0; i <= kBounds; ++i)
            {
                c[i] = counts[i].exchange (0, std::memory_order_relaxed);
                total += c[i];
            }
            s.max = (double) maxUs.exchange (0, std::memory_order_relaxed) / 1000.0;
            s.n = (int) total;
            if (total > 0)
            {
                s.p50 = quantile (c, total, 0.50, s.max);
                s.p95 = quantile (c, total, 0.95, s.max);
            }
            return s;
        }

    private:
        static constexpr int kBounds = 24;
        static double bound (int i) noexcept
        {
            static constexpr double b[kBounds] = { 1, 2, 3, 5, 8, 12, 16, 25, 35, 50, 75, 100, 150, 200,
                                                   300, 500, 750, 1000, 1500, 2000, 3000, 5000, 10000, 30000 };
            return b[i];
        }
        static double quantile (const uint32_t* c, uint64_t total, double q, double max) noexcept
        {
            const double target = q * (double) total;
            double acc = 0.0;
            for (int i = 0; i <= kBounds; ++i)
            {
                acc += c[i];
                if (acc >= target)
                    return i < kBounds ? std::min (max, bound (i)) : max;
            }
            return max;
        }

        std::atomic<uint32_t> counts[kBounds + 1] {};
        std::atomic<uint64_t> maxUs { 0 };
    };

    //==========================================================================
    // Crash report state: plain buffers only, read by the signal handler.
    constexpr int kCrashRingSize = 64;
    constexpr int kCrashLineMax = 480;
    char gCrashRing[kCrashRingSize][kCrashLineMax];
    std::atomic<unsigned> gCrashRingNext { 0 };
    char gCrashPath[2048] = { 0 };          // UTF-8 path of crash-<sid>.txt ("" = not running)
   #if JUCE_WINDOWS
    wchar_t gCrashPathW[1024] = { 0 };
   #endif
    std::atomic<long long> gStartMonoMs { 0 };

    long long monoMs() noexcept
    {
       #if JUCE_WINDOWS
        return (long long) GetTickCount64();
       #else
        timespec ts {};
        clock_gettime (CLOCK_MONOTONIC, &ts);   // async-signal-safe
        return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
       #endif
    }

    void pushCrashRing (const juce::String& line)
    {
        const unsigned i = gCrashRingNext.load (std::memory_order_relaxed) % (unsigned) kCrashRingSize;
        line.copyToUTF8 (gCrashRing[i], (size_t) kCrashLineMax);
        gCrashRingNext.fetch_add (1, std::memory_order_release);
    }

   #if ! JUCE_WINDOWS
    void rawWrite (int fd, const char* s) noexcept
    {
        if (s == nullptr) return;
        size_t n = std::strlen (s);
        while (n > 0)
        {
            const ssize_t w = ::write (fd, s, n);
            if (w <= 0) return;
            s += w;
            n -= (size_t) w;
        }
    }

    void rawWriteNumber (int fd, unsigned long long v, int base) noexcept
    {
        char buf[32];
        int i = 31;
        buf[i] = 0;
        if (v == 0) buf[--i] = '0';
        while (v > 0 && i > 0)
        {
            const int d = (int) (v % (unsigned) base);
            buf[--i] = (char) (d < 10 ? '0' + d : 'a' + d - 10);
            v /= (unsigned) base;
        }
        rawWrite (fd, buf + i);
    }

    const char* signalName (int sig) noexcept
    {
        switch (sig)
        {
            case SIGSEGV: return "SIGSEGV";
            case SIGBUS:  return "SIGBUS";
            case SIGILL:  return "SIGILL";
            case SIGFPE:  return "SIGFPE";
            case SIGABRT: return "SIGABRT";
            default:      return "signal";
        }
    }

    void crashReport (int sig, const void* addr) noexcept
    {
        if (gCrashPath[0] == 0) return;
        const int fd = ::open (gCrashPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) return;
        rawWrite (fd, "crash: ");
        rawWrite (fd, signalName (sig));
        rawWrite (fd, " (");
        rawWriteNumber (fd, (unsigned long long) sig, 10);
        rawWrite (fd, ")");
        if (addr != nullptr)
        {
            rawWrite (fd, " at address 0x");
            rawWriteNumber (fd, (unsigned long long) (uintptr_t) addr, 16);
        }
        rawWrite (fd, "\nsession time: ");
        const long long start = gStartMonoMs.load();
        rawWriteNumber (fd, (unsigned long long) std::max (0LL, monoMs() - start), 10);
        rawWrite (fd, " ms\n---- stack ----\n");
        void* frames[64];
        const int n = backtrace (frames, 64);
        backtrace_symbols_fd (frames, n, fd);
        rawWrite (fd, "---- last events ----\n");
        const unsigned end = gCrashRingNext.load (std::memory_order_acquire);
        const unsigned count = std::min<unsigned> (end, (unsigned) kCrashRingSize);
        for (unsigned k = end - count; k != end; ++k)
        {
            gCrashRing[k % (unsigned) kCrashRingSize][kCrashLineMax - 1] = 0;
            rawWrite (fd, gCrashRing[k % (unsigned) kCrashRingSize]);
            rawWrite (fd, "\n");
        }
        ::close (fd);
    }

    void onFatalSignal (int sig, siginfo_t* info, void*)
    {
        crashReport (sig, info != nullptr ? info->si_addr : nullptr);
        // SA_RESETHAND restored the default action: re-raise so the process
        // still ends with the original signal (OS crash report unchanged).
        ::signal (sig, SIG_DFL);
        ::raise (sig);
    }

    void installSignalHandlers()
    {
        static bool installed = false;
        if (installed) return;
        installed = true;
        {
            // Load the unwinder now: the first backtrace() may allocate.
            void* warm[2];
            backtrace (warm, 2);
        }
        static char altStack[64 * 1024];   // stack overflows on the message thread still get a report
        stack_t ss {};
        ss.ss_sp = altStack;
        ss.ss_size = sizeof (altStack);
        ss.ss_flags = 0;
        sigaltstack (&ss, nullptr);

        struct sigaction sa {};
        sa.sa_sigaction = onFatalSignal;
        sigemptyset (&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
        for (int s : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT })
            sigaction (s, &sa, nullptr);
    }
   #endif

    // Appends text to the crash report (normal context: std::terminate).
    void appendCrashText (const juce::String& text)
    {
       #if JUCE_WINDOWS
        if (gCrashPathW[0] == 0) return;
        HANDLE h = CreateFileW (gCrashPathW, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile (h, text.toRawUTF8(), (DWORD) text.getNumBytesAsUTF8(), &written, nullptr);
        CloseHandle (h);
       #else
        if (gCrashPath[0] == 0) return;
        const int fd = ::open (gCrashPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) return;
        rawWrite (fd, text.toRawUTF8());
        ::close (fd);
       #endif
    }

    //==========================================================================
    struct Pending
    {
        juce::String name;
        Level level = Level::info;
        double t = 0.0;
        juce::var fields;   // DynamicObject or void
    };

    struct Dedup
    {
        double firstT = 0.0;
        int suppressed = 0;
        double maxDurMs = 0.0;
        juce::var lastLine;
    };

    struct Worst
    {
        double durMs = 0.0;
        juce::var line;
    };

    class DiagLogger : public juce::Logger
    {
    public:
        void logMessage (const juce::String& message) override
        {
            juce::Logger::outputDebugString (message);   // the default behaviour stays
            if (!enabled()) return;
            if (message.contains ("killing thread by force"))
                event ("thread.killed", Level::error, { { "msg", message } });
            else if (message.startsWith ("JUCE Assertion failure"))
                event ("assert", Level::warn, { { "msg", message } });
            else
                event ("log", Level::debug, { { "msg", message } });
        }
    };

    std::terminate_handler gPreviousTerminate = nullptr;

    [[noreturn]] void onTerminate();

    void alertObserver (int icon, const juce::String& title, const juce::String& message)
    {
        noteAlert (icon, title, message);
    }

    //==========================================================================
    class Recorder
    {
    public:
        static Recorder& get()
        {
            // Never destroyed: its threads may still exist when static
            // destructors run (exit() without shutdown()).
            static Recorder* r = new Recorder();
            return *r;
        }

        bool start (const Config& c);
        void stop (bool clean);
        void stopWatchdog();   // message thread
        bool running() const { return running_; }

        void post (const juce::String& name, Level level, juce::var fields);
        void flush (int timeoutMs);

        Config config() const { std::lock_guard<std::mutex> l (stateLock); return cfg; }
        juce::File folder() const { std::lock_guard<std::mutex> l (stateLock); return dir; }
        juce::File file() const { std::lock_guard<std::mutex> l (stateLock); return currentFile_; }
        juce::String sid() const { std::lock_guard<std::mutex> l (stateLock); return sid_; }
        double sessionTime() const { return running_ ? (nowMs() - t0Ms) / 1000.0 : 0.0; }

        // Stall context
        void setActivity (const juce::String& a) { std::lock_guard<std::mutex> l (activityLock); activity = a; }
        juce::String getActivity() const { std::lock_guard<std::mutex> l (activityLock); return activity; }
        void setCrumb (const juce::String& c)
        {
            std::lock_guard<std::mutex> l (activityLock);
            crumb = c;
            crumbT = sessionTime();
        }

        // Metrics
        Histogram tileBatches;
        std::atomic<int> tilesComputed { 0 }, tileHits { 0 }, tileFft { 0 };
        void addAudioLoad (double v)
        {
            std::lock_guard<std::mutex> l (audioLock);
            if (audioLoads.size() < 600) audioLoads.push_back ((float) v);
        }

        std::function<void (juce::DynamicObject&)> headerProvider;
        std::function<void (bool)> sampler;
        void minuteTickForTests() { minuteAggregates(); }
        std::atomic<int> foregroundOverride { -1 };
        std::atomic<int> heartbeatMs { 50 };
        DiagLogger logger;

    private:
        Recorder() = default;

        // Message thread
        void beat();
        void secondTick();
        void minuteAggregates();
        juce::var buildHeader (int part) const;
        void collectUncleanSessions (std::vector<Pending>& out);

        // Writer thread
        void writerLoop();
        bool process (Pending& p);
        void writeLine (const juce::String& s);
        void writeObject (const juce::var& line);
        void openPart();
        void prune();
        void housekeeping (bool final);
        void noteStats (const juce::String& name, Level level, const juce::var& line);
        void writeSessionEnd();
        juce::String dedupKey (const juce::String& name, Level level, const juce::var& fields) const;

        // Watchdog thread
        void watchdogLoop();

        struct Heartbeat : juce::Timer
        {
            explicit Heartbeat (Recorder& r) : owner (r) {}
            void timerCallback() override { owner.beat(); }
            Recorder& owner;
        };

        mutable std::mutex stateLock;
        Config cfg;
        juce::File dir, currentFile_;
        juce::String sid_, fileStem;
        juce::var header;   // session.start fields (built once on the message thread)
        std::atomic<bool> running_ { false };
        std::atomic<double> t0Ms { 0.0 };

        // queue
        std::mutex qLock;
        std::condition_variable qCv;
        std::deque<Pending> q;
        uint64_t postedSeq = 0;
        bool accepting = false;
        std::atomic<int> droppedFull { 0 };
        std::atomic<bool> writerExit { false }, writeEndOnExit { true }, flushRequested { false };
        std::atomic<int> flushWaiters { 0 };   // flush() callers waiting: every batch reaches the disk
        std::mutex doneLock;
        std::condition_variable doneCv;
        uint64_t flushedSeq = 0;   // everything posted up to this seq is on disk
        std::thread writer;
        std::atomic<std::thread::id> writerId {};

        // writer state (writer thread only while running)
        std::unique_ptr<juce::FileOutputStream> out;
        juce::int64 fileBytes = 0;
        int part = 1;
        std::deque<juce::String> flight;   // debug lines not written (flight recorder)
        std::map<juce::String, Dedup> dedup;
        std::map<juce::String, int> minuteCounts, minuteDropped;
        int minuteIndex = 0;
        std::map<juce::String, int> counts;
        int nErrors = 0, nWarnings = 0;
        std::vector<Worst> worst;

        // watchdog
        std::thread watchdog;
        std::mutex wdLock;
        std::condition_variable wdCv;
        bool wdExit = false;
        std::atomic<double> lastBeatMs { 0.0 };
        std::atomic<uint64_t> beats { 0 };
        std::atomic<bool> foreground { true };
        std::unique_ptr<Heartbeat> heartbeat;
        double lastSecondMs = 0.0;
        int lastMinute = 0;
        double lastMemMb = -1.0;

        mutable std::mutex activityLock;
        juce::String activity, crumb;
        double crumbT = -1.0;

        std::mutex audioLock;
        std::vector<float> audioLoads;
    };

    //==========================================================================
    juce::String newSessionId()
    {
        juce::Random r;
        return juce::String::toHexString ((juce::int64) (r.nextInt64() & 0xffffff)).paddedLeft ('0', 6);
    }

    juce::String utcStamp (const juce::Time& t)
    {
        // 20261109T210412.381Z (file names: sortable to the millisecond, no colons)
        const juce::int64 ms = t.toMilliseconds();
        const std::time_t secs = (std::time_t) (ms / 1000);
        std::tm tmv {};
       #if JUCE_WINDOWS
        gmtime_s (&tmv, &secs);
       #else
        gmtime_r (&secs, &tmv);
       #endif
        char buf[40];
        std::snprintf (buf, sizeof (buf), "%04d%02d%02dT%02d%02d%02d.%03dZ", tmv.tm_year + 1900, tmv.tm_mon + 1,
                       tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int) (ms % 1000));
        return buf;
    }

    juce::String isoUtc (juce::int64 msSinceEpoch)
    {
        const std::time_t secs = (std::time_t) (msSinceEpoch / 1000);
        std::tm tmv {};
       #if JUCE_WINDOWS
        gmtime_s (&tmv, &secs);
       #else
        gmtime_r (&secs, &tmv);
       #endif
        char buf[40];
        std::snprintf (buf, sizeof (buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tmv.tm_year + 1900,
                       tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                       (int) (msSinceEpoch % 1000));
        return buf;
    }

    // The complete lines of a log file, decoded as UTF-8. A part cut by a
    // crash can end in the middle of a line or of a character: such a line is
    // dropped instead of turning the whole text into Latin-1. With fromOffset,
    // reading starts at the first full line after that byte.
    juce::StringArray readLogLines (const juce::File& f, juce::int64 fromOffset = 0)
    {
        juce::StringArray out;
        juce::FileInputStream in (f);
        if (!in.openedOk()) return out;
        if (fromOffset > 0) in.setPosition (fromOffset);
        juce::MemoryBlock mb;
        in.readIntoMemoryBlock (mb);
        const char* data = static_cast<const char*> (mb.getData());
        const size_t n = mb.getSize();
        size_t pos = 0;
        if (fromOffset > 0)
        {
            const void* nl = std::memchr (data, '\n', n);
            if (nl == nullptr) return out;
            pos = (size_t) (static_cast<const char*> (nl) - data) + 1;
        }
        while (pos < n)
        {
            const void* nl = std::memchr (data + pos, '\n', n - pos);
            if (nl == nullptr) break;   // incomplete last line
            const size_t end = (size_t) (static_cast<const char*> (nl) - data);
            const int len = (int) (end - pos);
            if (len > 0 && juce::CharPointer_UTF8::isValidString (data + pos, len))
                out.add (juce::String::fromUTF8 (data + pos, len).trimEnd());
            pos = end + 1;
        }
        return out;
    }

    juce::var obj (std::initializer_list<std::pair<const char*, juce::var>> props)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : props)
            if (k != nullptr && !v.isVoid())
                o->setProperty (juce::Identifier (k), v);
        return juce::var (o);
    }

    //==========================================================================
    bool Recorder::start (const Config& c)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        if (running_) stop (true);

        Config conf = c;
        if (conf.maxFileBytes < 1024) conf.maxFileBytes = 1024;
        if (conf.maxFiles < 1) conf.maxFiles = 1;
        if (conf.flightRecorderSize < 0) conf.flightRecorderSize = 0;
        const juce::File folderToUse = conf.folder != juce::File() ? conf.folder : defaultFolder();
        if (!ensureFolder (folderToUse))
            return false;

        const auto startTime = juce::Time::getCurrentTime();
        {
            std::lock_guard<std::mutex> l (stateLock);
            cfg = conf;
            dir = folderToUse;
            sid_ = newSessionId();
            fileStem = "diag-" + utcStamp (startTime) + "-" + sid_;
        }
        t0Ms = nowMs();
        gStartMonoMs.store (monoMs());
        part = 1;
        fileBytes = 0;
        flight.clear();
        dedup.clear();
        minuteCounts.clear();
        minuteDropped.clear();
        minuteIndex = 0;
        counts.clear();
        nErrors = nWarnings = 0;
        worst.clear();
        droppedFull = 0;
        lastMemMb = -1.0;
        lastMinute = 0;
        {
            std::lock_guard<std::mutex> l (audioLock);
            audioLoads.clear();
        }
        tileBatches.take();
        tilesComputed = 0;
        tileHits = 0;

        // Previous sessions that never wrote session.end (crash, force quit).
        std::vector<Pending> unclean;
        collectUncleanSessions (unclean);

        auto* h = new juce::DynamicObject();
        h->setProperty ("v", kSchemaVersion);
        h->setProperty ("ev", "session.start");
        h->setProperty ("ts", isoUtc (startTime.toMilliseconds()));
        h->setProperty ("t", 0);
        h->setProperty ("sid", sid_);
        h->setProperty ("pid", currentPid());
        h->setProperty ("reason", conf.reason);
        h->setProperty ("app", conf.appVersion.isNotEmpty() ? conf.appVersion
                                                            : juce::JUCEApplicationBase::getInstance() != nullptr
                                                                  ? juce::JUCEApplicationBase::getInstance()->getApplicationVersion()
                                                                  : juce::String ("unknown"));
       #if JUCE_DEBUG
        h->setProperty ("build", "Debug");
       #else
        h->setProperty ("build", "Release");
       #endif
        h->setProperty ("os", juce::SystemStats::getOperatingSystemName());
        h->setProperty ("arch", juce::SystemStats::isOperatingSystem64Bit() ? "64-bit" : "32-bit");
        h->setProperty ("cpu", juce::SystemStats::getCpuModel());
        h->setProperty ("cores", juce::SystemStats::getNumCpus());
        h->setProperty ("ramGb", round1 (juce::SystemStats::getMemorySizeInMegabytes() / 1024.0));
        {
            juce::Array<juce::var> screens;
            if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
                for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
                    screens.add (obj ({ { "w", d.logicalBounds.getWidth() }, { "h", d.logicalBounds.getHeight() },
                                        { "scale", round3 (d.scale) }, { "main", d.isMain } }));
            h->setProperty ("screens", screens);
        }
        h->setProperty ("budgetMb", round1 ((double) conf.maxFileBytes * conf.maxFiles / (1024.0 * 1024.0)));
        if (headerProvider)
        {
            juce::DynamicObject extra;
            headerProvider (extra);
            for (const auto& p : extra.getProperties())
                if (!h->hasProperty (p.name)) h->setProperty (p.name, p.value);
        }
        header = juce::var (h);

        // Crash report path, then the "running" marker.
        {
            const juce::File crash = dir.getChildFile ("crash-" + sid_ + ".txt");
            crash.getFullPathName().copyToUTF8 (gCrashPath, sizeof (gCrashPath));
           #if JUCE_WINDOWS
            crash.getFullPathName().copyToUTF16 (gCrashPathW, sizeof (gCrashPathW));   // wchar_t is UTF-16 on Windows
           #endif
        }
        dir.getChildFile ("running-" + sid_ + ".json")
            .replaceWithText (toJson (obj ({ { "sid", sid_ }, { "pid", currentPid() },
                                             { "started", isoUtc (startTime.toMilliseconds()) },
                                             { "file", fileStem } })));

        openPart();
        prune();
        if (out == nullptr)
        {
            dir.getChildFile ("running-" + sid_ + ".json").deleteFile();
            gCrashPath[0] = 0;
           #if JUCE_WINDOWS
            gCrashPathW[0] = 0;
           #endif
            return false;
        }

        {
            std::lock_guard<std::mutex> l (qLock);
            q.clear();
            postedSeq = 0;
            accepting = true;
        }
        {
            std::lock_guard<std::mutex> l (doneLock);
            flushedSeq = 0;
        }
        writerExit = false;
        writeEndOnExit = true;
        running_ = true;
        writer = std::thread ([this] { writerLoop(); });

        for (auto& p : unclean)
            post (p.name, p.level, p.fields);

        // Hooks: alerts, JUCE Logger, crashes.
        sc::alert::observer().store (&alertObserver);
        if (juce::Logger::getCurrentLogger() == nullptr)
            juce::Logger::setCurrentLogger (&logger);
        if (conf.crashHandlers)
        {
            static bool terminateInstalled = false;
            if (!terminateInstalled)
            {
                terminateInstalled = true;
                gPreviousTerminate = std::set_terminate (onTerminate);
            }
           #if ! JUCE_WINDOWS
            installSignalHandlers();
           #endif
        }

        // Baseline memory, then heartbeat (sampler + watchdog).
        {
            const double rss = processMemoryMb();
            const double avail = availableMemoryMb();
            lastMemMb = rss;
            post ("mem", Level::info, obj ({ { "rssMb", juce::roundToInt (rss) },
                                             { "availMb", juce::roundToInt (avail) },
                                             { "totalMb", juce::SystemStats::getMemorySizeInMegabytes() } }));
        }
        lastBeatMs = nowMs();
        lastSecondMs = nowMs();
        foreground = true;
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
        {
            heartbeat = std::make_unique<Heartbeat> (*this);
            heartbeat->startTimer (std::max (10, heartbeatMs.load()));
        }
        if (conf.watchdog)
        {
            {
                std::lock_guard<std::mutex> l (wdLock);
                wdExit = false;
            }
            watchdog = std::thread ([this] { watchdogLoop(); });
        }
        enabledFlag().store (true);
        return true;
    }

    void Recorder::stopWatchdog()
    {
        heartbeat.reset();
        if (watchdog.joinable())
        {
            {
                std::lock_guard<std::mutex> l (wdLock);
                wdExit = true;
            }
            wdCv.notify_all();
            watchdog.join();
        }
    }

    void Recorder::stop (bool clean)
    {
        if (!running_) return;
        enabledFlag().store (false);
        stopWatchdog();
        if (sc::alert::observer().load() == &alertObserver)
            sc::alert::observer().store (nullptr);
        if (juce::Logger::getCurrentLogger() == &logger)
            juce::Logger::setCurrentLogger (nullptr);

        {
            std::lock_guard<std::mutex> l (qLock);
            accepting = false;
            writeEndOnExit = clean;
            writerExit = true;
        }
        qCv.notify_all();
        if (writer.joinable())
            writer.join();
        out.reset();
        running_ = false;
        // The recorder itself is never destroyed: drop the objects it still
        // holds, or JUCE's leak detector reports them at exit.
        header = juce::var();
        flight.clear();
        dedup.clear();
        worst.clear();
        counts.clear();
        minuteCounts.clear();
        minuteDropped.clear();

        juce::String s;
        juce::File d;
        {
            std::lock_guard<std::mutex> l (stateLock);
            s = sid_;
            d = dir;
            currentFile_ = juce::File();
        }
        if (clean)
        {
            d.getChildFile ("running-" + s + ".json").deleteFile();
            gCrashPath[0] = 0;
           #if JUCE_WINDOWS
            gCrashPathW[0] = 0;
           #endif
        }
        else
        {
            // Simulated crash: the handlers must not write for this session
            // any more, but crash-<sid>.txt (if any) stays for the next start.
            gCrashPath[0] = 0;
           #if JUCE_WINDOWS
            gCrashPathW[0] = 0;
           #endif
        }
    }

    void Recorder::post (const juce::String& name, Level level, juce::var fields)
    {
        Pending p;
        p.name = name;
        p.level = level;
        p.t = sessionTime();
        p.fields = std::move (fields);
        {
            std::lock_guard<std::mutex> l (qLock);
            if (!accepting) return;
            if (q.size() >= kMaxQueue)
            {
                droppedFull.fetch_add (1);
                return;
            }
            q.push_back (std::move (p));
            ++postedSeq;
        }
        if (level == Level::error)
            qCv.notify_one();
    }

    void Recorder::flush (int timeoutMs)
    {
        if (!running_ || std::this_thread::get_id() == writerId.load())
            return;
        uint64_t target = 0;
        flushWaiters.fetch_add (1);
        {
            std::lock_guard<std::mutex> l (qLock);
            target = postedSeq;
            flushRequested = true;
        }
        qCv.notify_one();
        {
            std::unique_lock<std::mutex> l (doneLock);
            doneCv.wait_for (l, std::chrono::milliseconds (std::max (1, timeoutMs)),
                             [&] { return flushedSeq >= target || !running_; });
        }
        flushWaiters.fetch_sub (1);
    }

    //==========================================================================
    // Writer thread
    void Recorder::writerLoop()
    {
        writerId.store (std::this_thread::get_id());
        std::vector<Pending> batch;
        double lastFlush = nowMs();
        for (;;)
        {
            uint64_t seq = 0;
            bool exiting = false;
            {
                std::unique_lock<std::mutex> l (qLock);
                qCv.wait_for (l, std::chrono::milliseconds (250),
                              [this] { return !q.empty() || writerExit.load() || flushRequested.load(); });
                exiting = writerExit.load();
                for (auto& p : q)
                    batch.push_back (std::move (p));
                q.clear();
                seq = postedSeq;
            }
            bool urgent = false;
            for (auto& p : batch)
                urgent = process (p) || urgent;
            batch.clear();
            housekeeping (exiting);
            if (exiting && writeEndOnExit)
                writeSessionEnd();
            // While a flush() caller waits, every batch goes to the disk: the
            // batch that reaches its target may not be the one that saw the request.
            const bool asked = flushRequested.exchange (false) || flushWaiters.load() > 0;
            bool flushed = out == nullptr;   // nothing buffered to wait for
            if (out != nullptr && (urgent || exiting || asked || nowMs() - lastFlush > 1000.0))
            {
                out->flush();
                lastFlush = nowMs();
                flushed = true;
            }
            if (flushed)
            {
                std::lock_guard<std::mutex> l (doneLock);
                flushedSeq = seq;
            }
            doneCv.notify_all();
            if (exiting) break;
        }
        writerId.store (std::thread::id());
    }

    juce::String Recorder::dedupKey (const juce::String& name, Level level, const juce::var& fields) const
    {
        juce::String k = name + "|" + juce::String ((int) level);
        if (auto* o = fields.getDynamicObject())
            for (const auto& p : o->getProperties())
                if (p.value.isString() || p.value.isBool())
                    k << "|" << p.name.toString() << "=" << p.value.toString();
        return k;
    }

    bool Recorder::process (Pending& p)
    {
        // The line: ev, lvl, t, then the cleaned fields.
        auto* o = new juce::DynamicObject();
        o->setProperty ("ev", p.name);
        if (p.level != Level::info) o->setProperty ("lvl", levelName (p.level));
        o->setProperty ("t", round3 (p.t));
        juce::var fields = cleanVar (p.fields, 0);
        if (auto* f = fields.getDynamicObject())
            for (const auto& prop : f->getProperties())
                if (prop.name != juce::Identifier ("ev") && prop.name != juce::Identifier ("t")
                    && prop.name != juce::Identifier ("lvl"))
                    o->setProperty (prop.name, prop.value);
        juce::var line (o);

        if (p.level == Level::debug)
        {
            o->setProperty ("fr", 1);   // flight recorder: written only before an error
            const auto s = toJson (line);
            pushCrashRing (s);
            if (cfg.flightRecorderSize > 0)
            {
                flight.push_back (s);
                while ((int) flight.size() > cfg.flightRecorderSize)
                    flight.pop_front();
            }
            return false;
        }

        // Collapse identical events (same name, level and text fields) within 10 s.
        const bool exempt = p.name.startsWith ("session.") || p.name.startsWith ("diag.");
        const double durMs = (double) line.getProperty ("durMs", 0.0);
        if (!exempt)
        {
            const auto key = dedupKey (p.name, p.level, fields);
            auto it = dedup.find (key);
            if (it != dedup.end() && p.t - it->second.firstT < kDedupWindowS)
            {
                it->second.suppressed++;
                it->second.maxDurMs = std::max (it->second.maxDurMs, durMs);
                it->second.lastLine = line;
                noteStats (p.name, p.level, line);
                return false;
            }
            if (dedup.size() > 512)
                dedup.clear();
            Dedup d;
            d.firstT = p.t;
            d.maxDurMs = durMs;
            dedup[key] = d;

            // Rate limit per event name and minute.
            const int limit = p.level == Level::error ? 2 * kRatePerMinute : kRatePerMinute;
            if (++minuteCounts[p.name] > limit)
            {
                minuteDropped[p.name]++;
                noteStats (p.name, p.level, line);
                return false;
            }
        }

        noteStats (p.name, p.level, line);
        writeObject (line);

        if (p.level == Level::error && !flight.empty())
        {
            // Context of the error: the debug events kept in memory.
            writeObject (obj ({ { "ev", "flight" }, { "t", round3 (p.t) }, { "n", (int) flight.size() },
                                { "cause", p.name } }));
            for (const auto& s : flight)
                writeLine (s);
            flight.clear();
        }
        return p.level == Level::error;
    }

    void Recorder::housekeeping (bool final)
    {
        const double t = sessionTime();
        // Repeats whose 10 s window is over (or every one, at the end).
        for (auto it = dedup.begin(); it != dedup.end();)
        {
            if (final || t - it->second.firstT >= kDedupWindowS)
            {
                if (it->second.suppressed > 0 && it->second.lastLine.isObject())
                {
                    auto copy = it->second.lastLine.clone();
                    if (auto* o = copy.getDynamicObject())
                    {
                        // t = when this line is written (diag.tail follows the
                        // log by t), lastT = the last of the repeated events.
                        o->setProperty ("lastT", o->getProperty ("t"));
                        o->setProperty ("t", round3 (t));
                        o->setProperty ("repeat", it->second.suppressed);
                        if (it->second.maxDurMs > 0.0) o->setProperty ("maxDurMs", juce::roundToInt (it->second.maxDurMs));
                    }
                    writeObject (copy);
                }
                it = dedup.erase (it);
            }
            else
                ++it;
        }
        // Minute boundary: report what the rate limit dropped.
        const int minute = (int) (t / 60.0);
        if (final || minute != minuteIndex)
        {
            minuteIndex = minute;
            const int full = droppedFull.exchange (0);
            if (!minuteDropped.empty() || full > 0)
            {
                auto* c = new juce::DynamicObject();
                for (const auto& [name, n] : minuteDropped) c->setProperty (juce::Identifier (name), n);
                writeObject (obj ({ { "ev", "diag.dropped" }, { "lvl", "warn" }, { "t", round3 (t) },
                                    { "counts", juce::var (c) }, { "queueFull", full > 0 ? juce::var (full) : juce::var() } }));
            }
            minuteDropped.clear();
            minuteCounts.clear();
        }
    }

    void Recorder::noteStats (const juce::String& name, Level level, const juce::var& line)
    {
        counts[name]++;
        if (level == Level::error) ++nErrors;
        if (level == Level::warn) ++nWarnings;
        const double durMs = (double) line.getProperty ("durMs", 0.0);
        if (durMs <= 0.0 || name == "session.end") return;
        if ((int) worst.size() < kWorstKept || durMs > worst.back().durMs)
        {
            worst.push_back ({ durMs, line });
            std::sort (worst.begin(), worst.end(), [] (const Worst& a, const Worst& b) { return a.durMs > b.durMs; });
            if ((int) worst.size() > kWorstKept) worst.resize ((size_t) kWorstKept);
        }
    }

    void Recorder::writeSessionEnd()
    {
        auto* c = new juce::DynamicObject();
        for (const auto& [name, n] : counts) c->setProperty (juce::Identifier (name), n);
        juce::Array<juce::var> w;
        for (const auto& x : worst)
        {
            auto* e = new juce::DynamicObject();
            for (const char* k : { "ev", "t", "durMs", "during", "cmd", "view", "title", "path", "doc", "job" })
                if (x.line.hasProperty (k)) e->setProperty (k, x.line.getProperty (k, {}));
            w.add (juce::var (e));
        }
        writeObject (obj ({ { "ev", "session.end" }, { "t", round3 (sessionTime()) },
                            { "durS", juce::roundToInt (sessionTime()) }, { "errors", nErrors },
                            { "warnings", nWarnings }, { "counts", juce::var (c) }, { "worst", w } }));
    }

    void Recorder::writeObject (const juce::var& line)
    {
        writeLine (toJson (line));
    }

    void Recorder::writeLine (const juce::String& s)
    {
        if (out == nullptr) return;
        const auto bytes = (juce::int64) s.getNumBytesAsUTF8() + 1;
        if (fileBytes > 0 && fileBytes + bytes > cfg.maxFileBytes)
        {
            ++part;
            openPart();
            prune();
            if (out == nullptr) return;
        }
        out->writeText (s + "\n", false, false, "\n");
        fileBytes += bytes;
        pushCrashRing (s);
    }

    void Recorder::openPart()
    {
        out.reset();
        const juce::File f = dir.getChildFile (fileStem + "-" + juce::String (part).paddedLeft ('0', 3) + ".jsonl");
        f.deleteFile();
        auto s = std::make_unique<juce::FileOutputStream> (f);
        if (!s->openedOk())
            return;
        out = std::move (s);
        {
            std::lock_guard<std::mutex> l (stateLock);
            currentFile_ = f;
        }
        // Every part starts with the session header: each file reads on its own.
        auto h = header.clone();
        if (part > 1)
            if (auto* o = h.getDynamicObject())
            {
                o->setProperty ("part", part);
                o->setProperty ("t", round3 (sessionTime()));
            }
        const auto line = toJson (h);
        out->writeText (line + "\n", false, false, "\n");
        fileBytes = (juce::int64) line.getNumBytesAsUTF8() + 1;
    }

    void Recorder::prune()
    {
        // Oldest files first (names start with the UTC start time), never the
        // current one nor the files of another instance still running.
        auto files = dir.findChildFiles (juce::File::findFiles, false, "diag-*.jsonl");
        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) {
            return a.getFileName() < b.getFileName();
        });
        juce::StringArray liveSids;
        for (const auto& m : dir.findChildFiles (juce::File::findFiles, false, "running-*.json"))
        {
            const auto s = m.getFileNameWithoutExtension().fromFirstOccurrenceOf ("running-", false, false);
            if (s != sid_ && markerIsLive (m, (int) juce::JSON::parse (m.loadFileAsString()).getProperty ("pid", 0)))
                liveSids.add (s);
        }
        int excess = files.size() - cfg.maxFiles;
        for (const auto& f : files)
        {
            if (excess <= 0) break;
            if (f == currentFile_) continue;
            bool live = false;
            for (const auto& s : liveSids)
                if (f.getFileName().contains ("-" + s + "-")) live = true;
            if (live) continue;
            f.deleteFile();
            --excess;
        }
        // Crash reports: the 5 most recent.
        auto crashes = dir.findChildFiles (juce::File::findFiles, false, "crash-*.txt");
        std::sort (crashes.begin(), crashes.end(), [] (const juce::File& a, const juce::File& b) {
            return a.getLastModificationTime() > b.getLastModificationTime();
        });
        for (int i = 5; i < crashes.size(); ++i)
            crashes.getReference (i).deleteFile();
    }

    void Recorder::collectUncleanSessions (std::vector<Pending>& outEvents)
    {
        for (const auto& m : dir.findChildFiles (juce::File::findFiles, false, "running-*.json"))
        {
            const auto v = juce::JSON::parse (m.loadFileAsString());
            const int pid = (int) v.getProperty ("pid", 0);
            const juce::String oldSid = v.getProperty ("sid", {}).toString();
            if (oldSid == sid_) continue;
            if (markerIsLive (m, pid)) continue;   // another instance, still running

            auto* f = new juce::DynamicObject();
            f->setProperty ("prevSid", oldSid);
            f->setProperty ("prevStarted", v.getProperty ("started", {}));
            // The last line the previous session wrote.
            auto parts = dir.findChildFiles (juce::File::findFiles, false, "diag-*-" + oldSid + "-*.jsonl");
            std::sort (parts.begin(), parts.end(), [] (const juce::File& a, const juce::File& b) {
                return a.getFileName() < b.getFileName();
            });
            if (!parts.isEmpty())
            {
                const juce::File last = parts.getLast();
                f->setProperty ("prevFile", last.getFileName());
                const auto lines = readLogLines (last, std::max ((juce::int64) 0, last.getSize() - 8192));
                for (int i = lines.size(); --i >= 0;)
                {
                    const auto lastEv = juce::JSON::parse (lines[i]);
                    if (lastEv.isObject())
                    {
                        f->setProperty ("lastEvent", lastEv);
                        break;
                    }
                }
            }
            // A crash report written by the signal / terminate handlers.
            const juce::File crash = dir.getChildFile ("crash-" + oldSid + ".txt");
            if (crash.existsAsFile())
            {
                auto lines = readLogLines (crash);
                lines.removeEmptyStrings();
                juce::Array<juce::var> arr;
                for (int i = 0; i < std::min (lines.size(), 100); ++i)
                    arr.add (lines[i].substring (0, 400));
                f->setProperty ("crash", arr);
                const auto first = lines.isEmpty() ? juce::String() : lines[0];
                if (first.isNotEmpty()) f->setProperty ("cause", first.substring (0, 200));
                crash.deleteFile();
            }
            Pending p;
            p.name = "session.unclean";
            p.level = Level::error;
            p.fields = juce::var (f);
            outEvents.push_back (std::move (p));
            m.deleteFile();
        }
    }

    //==========================================================================
    // Message thread: heartbeat (20 Hz), 1 Hz sampler, minute aggregates.
    void Recorder::beat()
    {
        const double now = nowMs();
        lastBeatMs.store (now);
        beats.fetch_add (1);
        const int ov = foregroundOverride.load();
        foreground.store (ov >= 0 ? ov == 1 : juce::Process::isForegroundProcess());
        if (now - lastSecondMs >= 1000.0)
        {
            lastSecondMs = now;
            secondTick();
        }
    }

    void Recorder::secondTick()
    {
        const int minute = (int) (sessionTime() / 60.0);
        const bool minuteTick = minute != lastMinute;
        if (sampler)
            sampler (minuteTick);
        if (minuteTick)
        {
            lastMinute = minute;
            minuteAggregates();
            // Keep the marker fresh: other instances treat a marker untouched
            // for 10 minutes as a crashed session, whatever its pid.
            dir.getChildFile ("running-" + sid_ + ".json").setLastModificationTime (juce::Time::getCurrentTime());
        }
    }

    void Recorder::minuteAggregates()
    {
        // Spectrogram tiles.
        const auto batches = tileBatches.take();
        const int computed = tilesComputed.exchange (0);
        const int hits = tileHits.exchange (0);
        if (batches.n > 0 || hits > 0)
        {
            const int total = computed + hits;
            post ("spectral.tiles", batches.p95 > 200.0 ? Level::warn : Level::info,
                  obj ({ { "n", computed }, { "batches", batches.n },
                         { "hit", total > 0 ? juce::var (round3 ((double) hits / total)) : juce::var() },
                         { "p50Ms", juce::roundToInt (batches.p50) }, { "p95Ms", juce::roundToInt (batches.p95) },
                         { "maxMs", juce::roundToInt (batches.max) }, { "fft", tileFft.load() } }));
        }
        // Audio callback load while playing.
        std::vector<float> loads;
        {
            std::lock_guard<std::mutex> l (audioLock);
            loads.swap (audioLoads);
        }
        if (!loads.empty())
        {
            std::sort (loads.begin(), loads.end());
            const auto at = [&] (double frac) { return (double) loads[(size_t) std::min ((double) loads.size() - 1, frac * (double) (loads.size() - 1))]; };
            const double p95 = at (0.95);
            post ("audio.load", p95 > 0.8 ? Level::warn : Level::info,
                  obj ({ { "n", (int) loads.size() }, { "p50", round3 (at (0.5)) }, { "p95", round3 (p95) },
                         { "max", round3 (loads.back()) } }));
        }
        // Memory: when it moved by 200 MB, or when the machine runs low.
        const double rss = processMemoryMb();
        const double avail = availableMemoryMb();
        const double total = (double) juce::SystemStats::getMemorySizeInMegabytes();
        const bool low = avail >= 0.0 && total > 0.0 && avail < 0.1 * total;
        if (rss >= 0.0 && (lastMemMb < 0.0 || std::abs (rss - lastMemMb) >= 200.0 || low))
        {
            lastMemMb = rss;
            post ("mem", low ? Level::warn : Level::info,
                  obj ({ { "rssMb", juce::roundToInt (rss) }, { "availMb", juce::roundToInt (avail) },
                         { "totalMb", juce::roundToInt (total) } }));
        }
    }

    //==========================================================================
    // Watchdog thread: the message thread missed its heartbeat.
    void Recorder::watchdogLoop()
    {
        bool inStall = false, hangReported = false;
        double beatAtStall = 0.0, lastLoop = nowMs();
        uint64_t beatCountAtStall = 0;
        juce::String during, last;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> l (wdLock);
                wdCv.wait_for (l, std::chrono::milliseconds (50), [this] { return wdExit; });
                if (wdExit) return;
            }
            const double now = nowMs();
            // The whole machine slept (laptop lid): not a freeze.
            if (now - lastLoop > 1500.0)
            {
                inStall = false;
                lastLoop = now;
                continue;
            }
            lastLoop = now;
            const double beat = lastBeatMs.load();
            const double gap = now - beat;
            if (!inStall)
            {
                if (gap > kStallWarnMs && foreground.load())
                {
                    inStall = true;
                    hangReported = false;
                    beatAtStall = beat;
                    beatCountAtStall = beats.load();
                    during = getActivity();
                    std::lock_guard<std::mutex> l (activityLock);
                    last = (crumbT >= 0.0 && sessionTime() - crumbT < 30.0) ? crumb : juce::String();
                }
            }
            else if (beats.load() != beatCountAtStall)
            {
                // The message thread is back: the blocked time is the gap
                // between two beats minus one heartbeat period.
                const double dur = lastBeatMs.load() - beatAtStall - (double) heartbeatMs.load();
                if (dur >= kStallWarnMs && dur < 10.0 * 60.0 * 1000.0)
                    post ("ui.stall", dur >= kStallErrorMs ? Level::error : Level::warn,
                          obj ({ { "durMs", juce::roundToInt (dur) },
                                 { "during", during.isNotEmpty() ? juce::var (during) : juce::var() },
                                 { "last", last.isNotEmpty() ? juce::var (last) : juce::var() } }));
                inStall = false;
            }
            else if (!hangReported && gap > kStallErrorMs && gap < 10.0 * 60.0 * 1000.0)
            {
                // Still frozen: report now (and dump the flight recorder), in
                // case the user force-quits before it recovers.
                hangReported = true;
                post ("ui.hang", Level::error,
                      obj ({ { "durMs", juce::roundToInt (gap) },
                             { "during", during.isNotEmpty() ? juce::var (during) : juce::var() },
                             { "last", last.isNotEmpty() ? juce::var (last) : juce::var() } }));
            }
        }
    }

    //==========================================================================
    [[noreturn]] void onTerminate()
    {
        juce::String what;
        if (auto ep = std::current_exception())
        {
            try { std::rethrow_exception (ep); }
            catch (const std::exception& e) { what = juce::String ("std::exception: ") + e.what(); }
            catch (...) { what = "exception of unknown type"; }
        }
        else
            what = "std::terminate() without an active exception";
        appendCrashText ("terminate: " + clean (what, 400) + "\n");
        if (enabled())
        {
            event ("exception", Level::error, { { "what", what } });
            flush (500);
        }
        if (gPreviousTerminate != nullptr)
            gPreviousTerminate();
        std::abort();
    }

    //==========================================================================
    // Reading the files back.
    struct SessionFiles
    {
        juce::String sid;
        juce::Array<juce::File> files;
    };

    SessionFiles sessionFilesFor (const juce::File& folderToScan, const juce::String& wantedSid)
    {
        SessionFiles s;
        auto files = folderToScan.findChildFiles (juce::File::findFiles, false, "diag-*.jsonl");
        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) {
            return a.getFileName() < b.getFileName();
        });
        s.sid = wantedSid;
        bool found = false;
        for (const auto& f : files)
            found = found || (s.sid.isNotEmpty() && f.getFileName().contains ("-" + s.sid + "-"));
        if (!found) s.sid = {};
        if (s.sid.isEmpty() && !files.isEmpty())
        {
            // diag-<utc>-<sid>-<part>.jsonl
            juce::StringArray tok;
            tok.addTokens (files.getLast().getFileNameWithoutExtension(), "-", "");
            if (tok.size() >= 4) s.sid = tok[2];
        }
        for (const auto& f : files)
            if (s.sid.isNotEmpty() && f.getFileName().contains ("-" + s.sid + "-"))
                s.files.add (f);
        return s;
    }

    std::vector<juce::var> readEvents (const SessionFiles& s)
    {
        std::vector<juce::var> events;
        for (const auto& f : s.files)
            for (const auto& ln : readLogLines (f))
            {
                auto v = juce::JSON::parse (ln);
                if (v.isObject()) events.push_back (v);
            }
        return events;
    }

    juce::String describeEvent (const juce::var& e)
    {
        juce::String s;
        for (const char* k : { "title", "msg", "what", "cause", "cmd", "item", "during", "last", "path", "err", "view",
                               "dev", "n", "durMs", "repeat", "hint" })
        {
            if (!e.hasProperty (k)) continue;
            const auto v = e.getProperty (k, {});
            if (v.isObject() || v.isArray()) continue;
            if (s.isNotEmpty()) s << ", ";
            s << k << "=" << v.toString();
        }
        return s;
    }
} // namespace

//==============================================================================
bool start (const Config& config) { return Recorder::get().start (config); }
void stop() { JUCE_ASSERT_MESSAGE_THREAD Recorder::get().stop (true); }
bool isRunning() { return Recorder::get().running(); }
void stopFreezeWatchdog() { JUCE_ASSERT_MESSAGE_THREAD if (Recorder::get().running()) Recorder::get().stopWatchdog(); }
bool isProcessAlive (int pid) { return processAlive (pid); }
Config currentConfig() { return Recorder::get().config(); }

juce::File defaultFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("SpectroCraft").getChildFile ("diagnostics");
}

juce::File folder()
{
    auto& r = Recorder::get();
    if (r.running()) return r.folder();
    const auto c = r.config();
    return c.folder != juce::File() ? c.folder : defaultFolder();
}

juce::File currentFile() { return Recorder::get().running() ? Recorder::get().file() : juce::File(); }
juce::String sessionId() { return Recorder::get().running() ? Recorder::get().sid() : juce::String(); }
double sessionTime() { return Recorder::get().sessionTime(); }

void event (const char* name, Level level, Fields fields)
{
    if (!enabled() || name == nullptr) return;
    Recorder::get().post (name, level, obj (fields));
}

void eventObject (const char* name, Level level, const juce::var& fieldsObject)
{
    if (!enabled() || name == nullptr) return;
    Recorder::get().post (name, level, fieldsObject.getDynamicObject() != nullptr ? fieldsObject : juce::var());
}

void flush (int timeoutMs) { Recorder::get().flush (timeoutMs); }

ScopedActivity::ScopedActivity (const juce::String& what)
{
    if (!enabled() || !juce::MessageManager::existsAndIsCurrentThread()) return;
    previous = Recorder::get().getActivity();
    Recorder::get().setActivity (what);
    active = true;
}

ScopedActivity::~ScopedActivity()
{
    if (active) Recorder::get().setActivity (previous);
}

void setHeaderProvider (std::function<void (juce::DynamicObject&)> provider)
{
    Recorder::get().headerProvider = std::move (provider);
}

void setSampler (std::function<void (bool)> sampler) { Recorder::get().sampler = std::move (sampler); }

void recordTiles (double ms, int computed, int hits, int fftSize)
{
    if (!enabled()) return;
    auto& r = Recorder::get();
    if (hits > 0) r.tileHits.fetch_add (hits, std::memory_order_relaxed);
    if (computed > 0)
    {
        r.tilesComputed.fetch_add (computed, std::memory_order_relaxed);
        r.tileBatches.add (ms);
        r.tileFft.store (fftSize, std::memory_order_relaxed);
        if (ms > 50.0)
            event ("spectral.batch", Level::debug, { { "durMs", juce::roundToInt (ms) }, { "n", computed },
                                                     { "fft", fftSize } });
    }
}

void recordAudioLoad (double load)
{
    if (!enabled() || !std::isfinite (load)) return;
    Recorder::get().addAudioLoad (load);
}

ScopedPaint::ScopedPaint (const char* v, int width, int height) noexcept
    : view (v), w (width), h (height)
{
    if (enabled()) t0 = nowMs();
}

ScopedPaint::~ScopedPaint()
{
    if (t0 <= 0.0 || !enabled()) return;
    const double ms = nowMs() - t0;
    if (ms > kSlowPaintMs)
        event ("paint.slow", Level::warn, { { "view", juce::String (view) }, { "durMs", juce::roundToInt (ms) },
                                            { "w", w }, { "h", h } });
}

void noteJournalEntry (const juce::var& e)
{
    if (!enabled() || !e.isObject()) return;
    auto& r = Recorder::get();
    const juce::String kind = e.getProperty ("kind", {}).toString();
    if (kind == "command")
    {
        const juce::String cmd = e.getProperty ("cmd", {}).toString();
        const bool ok = (bool) e.getProperty ("ok", false);
        const int ms = (int) e.getProperty ("durationMs", 0);
        auto* f = new juce::DynamicObject();
        f->setProperty ("cmd", cmd);
        f->setProperty ("src", e.getProperty ("source", {}));
        f->setProperty ("ok", ok);
        if (e.hasProperty ("durationMs")) f->setProperty ("durMs", ms);
        for (const char* k : { "doc", "job" })
            if (e.hasProperty (k)) f->setProperty (k, e.getProperty (k, {}));
        if (e.hasProperty ("error")) f->setProperty ("err", e.getProperty ("error", {}));
        const auto params = e.getProperty ("params", {});
        if (params.getDynamicObject() != nullptr && params.getDynamicObject()->getProperties().size() > 0)
        {
            const auto p = toJson (cleanVar (params, 1));
            if (p.length() <= 200) f->setProperty ("params", juce::JSON::parse (p));
        }
        const bool slow = ms >= 1000;
        r.setCrumb ("cmd " + cmd);
        eventObject (slow ? "cmd.slow" : "cmd", (!ok || slow) ? Level::warn : Level::info, juce::var (f));
    }
    else if (kind == "job")
    {
        const bool ok = (bool) e.getProperty ("ok", false);
        const juce::String err = e.getProperty ("error", {}).toString();
        auto* f = new juce::DynamicObject();
        for (const char* k : { "job", "cmd", "doc" })
            if (e.hasProperty (k)) f->setProperty (k, e.getProperty (k, {}));
        f->setProperty ("src", e.getProperty ("source", {}));
        f->setProperty ("ok", ok);
        f->setProperty ("durMs", (int) e.getProperty ("elapsedMs", 0));
        if (err.isNotEmpty()) f->setProperty ("err", err);
        for (const char* k : { "exitCode", "timedOut", "deviceUsed", "modelId" })
            if (e.hasProperty (k)) f->setProperty (k, e.getProperty (k, {}));
        eventObject ("job.end", ok || err == "cancelled" ? Level::info : Level::error, juce::var (f));
    }
    else if (kind == "step")
    {
        const juce::String what = e.getProperty ("what", {}).toString();
        auto* f = new juce::DynamicObject();
        f->setProperty ("what", what);
        if (e.hasProperty ("doc")) f->setProperty ("doc", e.getProperty ("doc", {}));
        f->setProperty ("src", e.getProperty ("source", {}));
        const auto params = e.getProperty ("params", {});
        if (params.getDynamicObject() != nullptr && params.getDynamicObject()->getProperties().size() > 0)
        {
            const auto p = toJson (cleanVar (params, 1));
            if (p.length() <= 200) f->setProperty ("params", juce::JSON::parse (p));
        }
        const bool viaCommand = (bool) e.getProperty ("viaCommand", false);
        if (!viaCommand) r.setCrumb ("step " + what);
        eventObject ("step", viaCommand ? Level::debug : Level::info, juce::var (f));
    }
    else
    {
        // history / meta / doc entries: detail for the flight recorder only.
        event ("journal", Level::debug, { { "kind", kind }, { "what", e.getProperty ("what", {}) },
                                          { "doc", e.getProperty ("doc", {}) } });
    }
}

void noteMenu (const juce::String& item)
{
    if (!enabled() || item.isEmpty()) return;
    Recorder::get().setCrumb ("menu " + item);
    event ("ui.menu", Level::info, { { "item", item } });
}

void noteAlert (int iconType, const juce::String& title, const juce::String& message)
{
    if (!enabled()) return;
    const bool warning = iconType == (int) juce::MessageBoxIconType::WarningIcon;
    const auto hint = hintFor (title + " " + message);
    const char* icon = warning ? "warning"
                     : iconType == (int) juce::MessageBoxIconType::InfoIcon ? "info"
                     : iconType == (int) juce::MessageBoxIconType::QuestionIcon ? "question" : "none";
    event ("alert", warning ? Level::error : Level::info,
           { { "title", title }, { "msg", message }, { "icon", juce::String (icon) },
             { "hint", hint.isNotEmpty() ? juce::var (hint) : juce::var() } });
}

juce::String clean (const juce::String& text, int maxChars)
{
    juce::String s = text;
    {
        std::lock_guard<std::mutex> l (secretsLock());
        for (const auto& secret : secrets())
            if (s.contains (secret))
                s = s.replace (secret, "[redacted]");
    }
    if (maxChars > 0 && s.length() > maxChars)
        s = s.substring (0, maxChars) + "...";
    return s;
}

void addSecret (const juce::String& secret)
{
    if (secret.length() < 6) return;   // too short to be a secret, too common to redact
    std::lock_guard<std::mutex> l (secretsLock());
    secrets().addIfNotAlreadyThere (secret);
}

juce::String hintFor (const juce::String& message)
{
    static const struct { const char* needle; const char* hint; } table[] = {
        { "openvino", "ai.runtime" },
        { "whisper", "ai.runtime" },
        { "not installed", "ai.model" },
        { "model is missing", "ai.model" },
        { "download", "network" },
        { "disk full", "fs.write" },
        { "not writable", "fs.write" },
        { "could not save", "fs.write" },
        { "could not be written", "fs.write" },
        { "permission", "fs.permission" },
        { "unsupported format", "file.format" },
        { "damaged", "file.format" },
        { "could not open", "file.open" },
        { "audio device", "audio.device" },
        { "output device", "audio.device" },
        { "python", "python" },
        { "ffmpeg", "video.decoder" },
        { "out of memory", "memory" },
        { "not enough memory", "memory" },
        { "timed out", "timeout" },
    };
    const auto m = message.toLowerCase();
    for (const auto& e : table)
        if (m.contains (e.needle))
            return e.hint;
    return {};
}

//==============================================================================
juce::var tail (double since, const juce::StringArray& kinds, int limit)
{
    if (enabled()) flush();
    // Running: this session. Off: the last session of this process, else the
    // latest one on disk.
    const auto files = sessionFilesFor (folder(), Recorder::get().sid());
    const auto events = readEvents (files);
    juce::Array<juce::var> picked;
    double next = since;
    bool inFlight = false;   // the debug lines written after a "flight" line keep their older t
    for (const auto& e : events)
    {
        const double t = (double) e.getProperty ("t", 0.0);
        const juce::String ev = e.getProperty ("ev", {}).toString();
        const bool fromFlight = (bool) e.getProperty ("fr", false);
        if (fromFlight)
        {
            if (!inFlight) continue;
        }
        else
        {
            inFlight = false;
            if (t <= since) continue;
            inFlight = ev == "flight";
        }
        if (!kinds.isEmpty())
        {
            bool match = false;
            for (const auto& k : kinds)
                if (ev == k || ev.startsWith (k + ".")) match = true;
            if (!match) continue;
        }
        picked.add (e);
        if (!fromFlight)
            next = std::max (next, t);
    }
    if (limit > 0 && picked.size() > limit)
        picked.removeRange (0, picked.size() - limit);
    auto* o = new juce::DynamicObject();
    o->setProperty ("enabled", enabled());
    o->setProperty ("sid", files.sid);
    o->setProperty ("file", files.files.isEmpty() ? juce::var() : juce::var (files.files.getLast().getFullPathName()));
    o->setProperty ("events", picked);
    o->setProperty ("next", round3 (next));
    return juce::var (o);
}

juce::var status()
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("enabled", enabled());
    o->setProperty ("sid", sessionId());
    o->setProperty ("file", enabled() ? juce::var (currentFile().getFullPathName()) : juce::var());
    o->setProperty ("folder", folder().getFullPathName());
    juce::Array<juce::var> files;
    juce::int64 total = 0;
    for (const auto& f : bundleFiles())
    {
        files.add (obj ({ { "name", f.getFileName() }, { "sizeBytes", (juce::int64) f.getSize() } }));
        total += f.getSize();
    }
    o->setProperty ("files", files);
    o->setProperty ("totalBytes", total);
    const auto c = Recorder::get().config();
    o->setProperty ("budgetBytes", (juce::int64) c.maxFileBytes * c.maxFiles);
    return juce::var (o);
}

juce::Array<juce::File> bundleFiles()
{
    const juce::File d = folder();
    auto files = d.findChildFiles (juce::File::findFiles, false, "diag-*.jsonl");
    files.addArray (d.findChildFiles (juce::File::findFiles, false, "crash-*.txt"));
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) {
        return a.getFileName() < b.getFileName();
    });
    return files;
}

juce::String summaryMarkdown (int maxBytes)
{
    if (enabled()) flush();
    const auto files = sessionFilesFor (folder(), Recorder::get().sid());
    const auto events = readEvents (files);
    juce::String md;
    md << "# SpectroCraft diagnostic summary\n\n";
    if (events.empty())
    {
        md << (enabled() ? "The diagnostic log is on but has no events yet.\n"
                         : "No diagnostic log found. Turn it on with Help > Diagnostic Log, "
                           "reproduce the problem, then copy the summary again.\n");
        return md;
    }

    juce::var start;
    std::map<juce::String, int> counts;
    std::vector<juce::var> problems, docs, perf, slow;
    const juce::var* end = nullptr;
    for (const auto& e : events)
    {
        const juce::String ev = e.getProperty ("ev", {}).toString();
        const juce::String lvl = e.getProperty ("lvl", "info").toString();
        if (ev == "session.start" && !start.isObject()) start = e;
        if (ev == "session.end") end = &e;
        if (ev == "session.start" || ev == "session.end") continue;
        if (!(bool) e.getProperty ("fr", false)) counts[ev] += 1 + (int) e.getProperty ("repeat", 0);
        if ((lvl == "error" || lvl == "warn") && !(bool) e.getProperty ("fr", false)) problems.push_back (e);
        if (ev == "doc.open") docs.push_back (e);
        if (ev == "spectral.tiles" || ev == "audio.load" || ev == "mem") perf.push_back (e);
        if ((double) e.getProperty ("durMs", 0.0) > 0.0 && !(bool) e.getProperty ("fr", false)) slow.push_back (e);
    }

    // Session
    md << "Session `" << start.getProperty ("sid", files.sid).toString() << "`";
    if (start.hasProperty ("ts")) md << ", started " << start.getProperty ("ts", {}).toString();
    const double lastT = (double) events.back().getProperty ("t", 0.0);
    md << ", " << juce::String (lastT / 60.0, 1) << " min logged";
    md << (enabled() ? " (running).\n" : (end != nullptr ? " (ended cleanly).\n" : " (did not end cleanly).\n"));
    md << "App " << start.getProperty ("app", "?").toString() << " (" << start.getProperty ("build", "?").toString()
       << "), " << start.getProperty ("os", "?").toString() << ", " << start.getProperty ("cpu", "?").toString()
       << ", " << start.getProperty ("cores", "?").toString() << " cores, "
       << start.getProperty ("ramGb", "?").toString() << " GB RAM.\n";
    for (const char* k : { "ws", "docs", "aiControl", "reason" })
        if (start.hasProperty (k)) md << "- " << k << ": " << start.getProperty (k, {}).toString() << "\n";
    md << "\n";

    md << "## Counts\n\n";
    for (const auto& [ev, n] : counts) md << "- " << ev << ": " << n << "\n";
    md << "\n";

    md << "## Errors and warnings (" << (int) problems.size() << ", most recent last)\n\n";
    if (problems.empty()) md << "None.\n";
    const size_t firstProblem = problems.size() > 40 ? problems.size() - 40 : 0;
    for (size_t i = firstProblem; i < problems.size(); ++i)
    {
        const auto& e = problems[i];
        md << "- t=" << juce::String ((double) e.getProperty ("t", 0.0), 1) << " s **"
           << e.getProperty ("ev", {}).toString() << "** (" << e.getProperty ("lvl", {}).toString() << ") "
           << clean (describeEvent (e), 300) << "\n";
    }
    md << "\n";

    std::sort (slow.begin(), slow.end(), [] (const juce::var& a, const juce::var& b) {
        return (double) a.getProperty ("durMs", 0.0) > (double) b.getProperty ("durMs", 0.0);
    });
    md << "## Slowest operations\n\n";
    if (slow.empty()) md << "None.\n";
    for (size_t i = 0; i < std::min<size_t> (10, slow.size()); ++i)
        md << "- " << (int) slow[i].getProperty ("durMs", 0) << " ms " << slow[i].getProperty ("ev", {}).toString()
           << " (t=" << juce::String ((double) slow[i].getProperty ("t", 0.0), 1) << " s) "
           << clean (describeEvent (slow[i]), 200) << "\n";
    md << "\n";

    if (!docs.empty())
    {
        md << "## Documents opened\n\n";
        for (size_t i = docs.size() > 20 ? docs.size() - 20 : 0; i < docs.size(); ++i)
        {
            const auto& d = docs[i];
            md << "- doc " << d.getProperty ("doc", "?").toString() << ": " << d.getProperty ("path", "?").toString()
               << " (" << d.getProperty ("fmt", "?").toString() << ", " << d.getProperty ("ch", "?").toString()
               << " ch, " << d.getProperty ("sr", "?").toString() << " Hz, " << d.getProperty ("durS", "?").toString()
               << " s, " << d.getProperty ("sizeMb", "?").toString() << " MB, " << d.getProperty ("mode", "?").toString()
               << "), opened in " << d.getProperty ("durMs", "?").toString() << " ms\n";
        }
        md << "\n";
    }
    if (!perf.empty())
    {
        md << "## Recent performance aggregates\n\n";
        for (size_t i = perf.size() > 12 ? perf.size() - 12 : 0; i < perf.size(); ++i)
            md << "- `" << toJson (perf[i]) << "`\n";
        md << "\n";
    }

    // The raw tail, as much as fits.
    const juce::String head = "## Last events (JSON Lines)\n\n```jsonl\n";
    const juce::String foot = "```\n";
    const int budget = maxBytes - (int) md.getNumBytesAsUTF8() - (int) head.getNumBytesAsUTF8()
                       - (int) foot.getNumBytesAsUTF8() - 64;
    if (budget > 200)
    {
        juce::StringArray lines;
        int used = 0;
        for (auto it = events.rbegin(); it != events.rend() && lines.size() < 200; ++it)
        {
            const auto line = toJson (*it).substring (0, 600);
            const int bytes = (int) line.getNumBytesAsUTF8() + 1;
            if (used + bytes > budget) break;
            used += bytes;
            lines.insert (0, line);
        }
        md << head << lines.joinIntoString ("\n") << "\n" << foot;
    }
    if ((int) md.getNumBytesAsUTF8() > maxBytes)
    {
        // Every character is at least one byte: dropping the excess in
        // characters always brings the size under the cap.
        const int excess = (int) md.getNumBytesAsUTF8() - std::max (0, maxBytes - 4);
        md = md.dropLastCharacters (excess) + "\n...";
    }
    return md;
}

bool writeSupportBundle (const juce::File& zip, juce::String* error)
{
    if (enabled()) flush();
    const auto files = bundleFiles();
    if (files.isEmpty())
    {
        if (error != nullptr) *error = "There is no diagnostic log to bundle yet.";
        return false;
    }
    juce::ZipFile::Builder builder;
    for (const auto& f : files)
        builder.addFile (f, 9, f.getFileName());
    const auto md = summaryMarkdown (512 * 1024);
    builder.addEntry (std::make_unique<juce::MemoryInputStream> (md.toRawUTF8(), md.getNumBytesAsUTF8(), true), 9,
                      "summary.md", juce::Time::getCurrentTime());
    const juce::String readme =
        "SpectroCraft support bundle\n"
        "\n"
        "summary.md            readable summary of the latest session\n"
        "diag-*.jsonl          diagnostic log (JSON Lines, schema: docs/diagnostics.md)\n"
        "crash-*.txt           crash reports not yet folded into a log (if any)\n"
        "\n"
        "Contains file paths, commands, timings and error messages.\n"
        "Never audio, transcribed text, table values, scripts or the AI control token.\n";
    builder.addEntry (std::make_unique<juce::MemoryInputStream> (readme.toRawUTF8(), readme.getNumBytesAsUTF8(), true), 9,
                      "README.txt", juce::Time::getCurrentTime());
    zip.deleteFile();
    juce::FileOutputStream out (zip);
    if (!out.openedOk() || !builder.writeToStream (out, nullptr))
    {
        if (error != nullptr) *error = "Could not write " + zip.getFullPathName() + ".";
        return false;
    }
    return true;
}

void writeWindowsCrash (unsigned long code) noexcept
{
   #if JUCE_WINDOWS
    if (gCrashPathW[0] == 0) return;
    HANDLE h = CreateFileW (gCrashPathW, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[640];
    DWORD w = 0;
    int n = std::snprintf (buf, sizeof (buf), "crash: exception code 0x%08lX on thread %lu\nsession time: %lld ms\n---- stack ----\n",
                           code, GetCurrentThreadId(), (long long) (monoMs() - gStartMonoMs.load()));
    WriteFile (h, buf, (DWORD) std::max (0, n), &w, nullptr);
    void* frames[48] = {};
    const USHORT count = RtlCaptureStackBackTrace (0, 48, frames, nullptr);
    for (USHORT i = 0; i < count; ++i)
    {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = { 0 };
        if (GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR) frames[i], &mod) && mod != nullptr)
            GetModuleFileNameA (mod, name, sizeof (name));
        const char* base = std::strrchr (name, '\\');
        n = std::snprintf (buf, sizeof (buf), "#%u %s + 0x%llX\n", (unsigned) i, base != nullptr ? base + 1 : (name[0] ? name : "?"),
                           (unsigned long long) ((const char*) frames[i] - (const char*) mod));
        WriteFile (h, buf, (DWORD) std::max (0, n), &w, nullptr);
    }
    const char* sep = "---- last events ----\n";
    WriteFile (h, sep, (DWORD) std::strlen (sep), &w, nullptr);
    const unsigned endIdx = gCrashRingNext.load (std::memory_order_acquire);
    const unsigned nLines = std::min<unsigned> (endIdx, (unsigned) kCrashRingSize);
    for (unsigned k = endIdx - nLines; k != endIdx; ++k)
    {
        char* line = gCrashRing[k % (unsigned) kCrashRingSize];
        line[kCrashLineMax - 1] = 0;
        WriteFile (h, line, (DWORD) std::strlen (line), &w, nullptr);
        WriteFile (h, "\n", 1, &w, nullptr);
    }
    CloseHandle (h);
   #else
    juce::ignoreUnused (code);
   #endif
}

namespace testing
{
    void abandon() { JUCE_ASSERT_MESSAGE_THREAD Recorder::get().stop (false); }

    void writeCrashReport (int signalNumber)
    {
       #if JUCE_WINDOWS
        writeWindowsCrash ((unsigned long) signalNumber);
       #else
        crashReport (signalNumber, nullptr);
       #endif
    }

    void setForegroundOverride (int value) { Recorder::get().foregroundOverride = value; }
    void runMinuteTick() { JUCE_ASSERT_MESSAGE_THREAD if (enabled()) Recorder::get().minuteTickForTests(); }
    void setHeartbeatMs (int ms) { Recorder::get().heartbeatMs = std::max (10, ms); }
}

} // namespace sc::diag
