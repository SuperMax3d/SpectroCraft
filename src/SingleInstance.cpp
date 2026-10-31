// SpectroCraft - single-instance coordination, see include/SingleInstance.h

#include "SingleInstance.h"

#if defined (_WIN32)
# include <windows.h>
#endif

#include <atomic>
#include <cstring>
#include <vector>

namespace sc { namespace singleinstance {

//==============================================================================
juce::File defaultStorePath ()
{
    // Must stay in sync with the store Application creates (same app name and
    // suffix, so both read/write the same SpectroCraft.recent file).
    juce::PropertiesFile::Options opts;
    opts.applicationName = "SpectroCraft";
    opts.filenameSuffix  = ".recent";
    return opts.getDefaultFile ();
}

bool allowMultipleInstances (const juce::File& storePath)
{
    const juce::File path = (storePath != juce::File()) ? storePath : defaultStorePath ();
    if (!path.existsAsFile()) return true;   // first run: checked by default

    juce::PropertiesFile store (path, juce::PropertiesFile::Options());
    return store.getIntValue (kSettingKey, 1) != 0;
}

//==============================================================================
namespace {

// Wire format: int32 count, then per path: int32 byte length + UTF-8 bytes.
// The server answers a complete message with one acknowledgement byte (0x01).
constexpr int    kMaxPaths      = 64;
constexpr size_t kMaxPathBytes  = 65536;

void appendI32 (std::vector<char>& out, int32_t v)
{
    char b[4];
    std::memcpy (b, &v, sizeof (b));
    out.insert (out.end (), b, b + 4);
}

bool readExactly (juce::NamedPipe& pipe, void* dest, int totalBytes, int timeoutMs)
{
    char* p = static_cast<char*> (dest);
    int got = 0;
    while (got < totalBytes)
    {
        const int r = pipe.read (p + got, totalBytes - got, timeoutMs);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

std::vector<char> buildPayload (const juce::StringArray& paths)
{
    std::vector<char> payload;
    appendI32 (payload, (int32_t) paths.size());
    for (const auto& p : paths)
    {
        const std::string s = p.toStdString ();   // UTF-8
        if (s.empty () || s.size () > kMaxPathBytes) return {};   // reject the whole batch
        appendI32 (payload, (int32_t) s.size());
        payload.insert (payload.end (), s.begin (), s.end ());
    }
    return payload;
}

}   // anonymous namespace

//==============================================================================
struct Coordinator::Impl final : public juce::Thread
{
    Impl (std::function<void (const juce::StringArray&)> cb, const juce::String& name)
        : juce::Thread ("SC-FileForward"), onFiles (std::move (cb)), pipeName (name) {}

    void start () { startThread (); }

    void requestStop () { stopFlag.store (true); }

    void run () override
    {
        juce::NamedPipe pipe;
        if (!pipe.createNewPipe (pipeName, false)) return;

        while (!stopFlag.load ())
        {
            int32_t n = -1;
            const int got = pipe.read (&n, sizeof (n), 500);   // short timeout so the stop flag is noticed quickly

            if (got != (int) sizeof (n) || n < 0 || n > kMaxPaths)
            {
                // A -1 here is just an idle timeout between clients. Any other
                // result means a connected client sent malformed data: drop the
                // connection by recreating the pipe so framing stays in sync for
                // later clients.
                if (got != -1)
                {
                    pipe.close ();
                    if (!pipe.createNewPipe (pipeName, false)) return;
                }
                continue;
            }

            juce::StringArray paths;
            bool ok = true;
            for (int i = 0; i < n && ok; ++i)
            {
                int32_t len = -1;
                if (!readExactly (pipe, &len, sizeof (len), 5000)) { ok = false; break; }
                if (len <= 0 || (size_t) len > kMaxPathBytes)      { ok = false; break; }

                std::vector<char> buf ((size_t) len);
                if (!readExactly (pipe, buf.data (), len, 5000))   { ok = false; break; }
                // The buffer is not null-terminated: bound the read explicitly.
                paths.add (juce::String (buf.data (), (size_t) len));   // UTF-8
            }

            if (!ok)
            {
                pipe.close ();
                if (!pipe.createNewPipe (pipeName, false)) return;
                continue;
            }

            // Acknowledge before opening the files so the sender can quit even
            // while this instance is still busy with them.
            const uint8_t ack = 1;
            pipe.write (&ack, sizeof (ack), 2000);

            if (!paths.isEmpty () && onFiles != nullptr)
            {
                auto cb = onFiles;
                juce::MessageManager::callAsync ([cb, p = paths] { cb (p); });
            }
        }
    }

    std::atomic<bool> stopFlag { false };
    std::function<void (const juce::StringArray&)> onFiles;
    juce::String pipeName;
};

//==============================================================================
bool forwardFiles (const juce::StringArray& paths, const juce::String& pipeName)
{
    const std::vector<char> payload = buildPayload (paths);
    if (payload.empty () && !paths.isEmpty ()) return false;   // oversized path: open locally instead

    juce::NamedPipe pipe;
    if (!pipe.openExisting (pipeName)) return false;

    int written = 0;
    while (written < (int) payload.size ())
    {
        const int r = pipe.write (payload.data () + written,
                                  (int) payload.size () - written, 2000);
        if (r <= 0) return false;   // no instance listening (or it vanished mid-write)
        written += r;
    }

    uint8_t ack = 0;
    const int gotAck = pipe.read (&ack, sizeof (ack), 2000);
    pipe.close ();
    return gotAck == (int) sizeof (ack) && ack == 1;
}

//==============================================================================
bool handleStartup (const juce::StringArray& cliFiles)
{
    if (allowMultipleInstances ()) return false;

#if defined (_WIN32)
    // A named mutex that exists while any SpectroCraft process is alive.
    // Creating it never blocks: we only use GetLastError() to detect whether an
    // earlier instance already created it, and keep the handle open for the
    // rest of this process's life so later launches can find us too.
    static HANDLE hInstance = nullptr;
    if (hInstance == nullptr)
    {
        hInstance = CreateMutexW (nullptr, false, L"SpectroCraft.Instance");
        if (GetLastError () != ERROR_ALREADY_EXISTS) return false;   // we are the first instance
    }
#else
    // No cheap cross-process probe on POSIX: the pipe attempt below doubles as
    // the detection - it fails quickly when no instance is listening.
#endif

    return forwardFiles (cliFiles);
}

//==============================================================================
Coordinator::Coordinator (std::function<void (const juce::StringArray&)> onFilesReceived,
                          const juce::String& pipeName)
    : impl (std::make_unique<Impl> (std::move (onFilesReceived), pipeName)) {}

Coordinator::~Coordinator() { stopServer (); }

void Coordinator::startServer ()
{
    if (impl != nullptr && !impl->isThreadRunning ()) impl->start ();
}

void Coordinator::stopServer ()
{
    if (impl == nullptr) return;
    impl->requestStop ();
    impl->stopThread (1500);   // the read loop notices the flag within ~500 ms
}

}}   // namespace sc::singleinstance
