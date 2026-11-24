#pragma once
// SpectroCraft - open files changed or deleted on disk (issue #70)
#include <JuceHeader.h>
#include <map>
#include <mutex>
#include <optional>

namespace sc::files
{
/** What the disk says about a file: whether it exists, its modification time
    and its size. Two stamps differ as soon as another program rewrites,
    replaces or deletes the file. */
struct DiskStamp
{
    bool exists = false;
    juce::int64 modMs = 0;
    juce::int64 size = 0;
    static DiskStamp of (const juce::File& f);
    bool operator== (const DiskStamp& o) const noexcept
    {
        return exists == o.exists && modMs == o.modMs && size == o.size;
    }
    bool operator!= (const DiskStamp& o) const noexcept { return ! (*this == o); }
};

/** The app's own writes (audio saves, embedded metadata patches): while one
    runs the file is not checked, and the stamp it leaves is recognised as the
    app's, never reported as a change by another program. Any thread. */
class ScopedWrite
{
public:
    explicit ScopedWrite (juce::File f);
    ~ScopedWrite();
private:
    juce::File file_;
    JUCE_DECLARE_NON_COPYABLE (ScopedWrite)
};
bool isBeingWritten (const juce::File& f);
/** The app's last write of a file: the stamp it left and when it ended
    (nowMs() clock). A check made before that moment saw the file mid-write
    and must not be trusted. */
struct OwnWrite { DiskStamp stamp; double endMs = 0.0; };
std::optional<OwnWrite> lastOwnWrite (const juce::File& f);
/** The clock of every disk check and write (monotonic, milliseconds). */
double nowMs();

enum class DiskState { same, modified, deleted };

/** Pure: the state of a document whose file the app last read or wrote as
    `known`, now that the disk says `now`. The app writing it (`beingWritten`),
    the stamp of its own last write (`ownWrite`, then `*adopt` receives it:
    the caller makes it the known stamp) and a change the user chose to ignore
    (`ignored`, the same stamp) count as unchanged. */
DiskState diskStateFor (const DiskStamp& known, const DiskStamp& now, bool beingWritten,
                        const std::optional<DiskStamp>& ownWrite,
                        const std::optional<DiskStamp>& ignored, DiskStamp* adopt);

/** Checks a list of files on a background thread every `intervalMs` (a slow
    or disconnected network drive never blocks the window), then calls
    onScanned on the message thread. start() launches the thread; scanNow()
    does one pass on the calling thread (tests). */
class DiskWatcher : private juce::Thread, private juce::AsyncUpdater
{
public:
    explicit DiskWatcher (int intervalMs = 2000);
    ~DiskWatcher() override;
    void start();
    void setFiles (const juce::Array<juce::File>& files);
    /** What the last scan saw of `f` and when it looked (nowMs(), taken
        before the look); none before the first scan of that file. */
    struct Seen { DiskStamp stamp; double atMs = 0.0; };
    std::optional<Seen> seenOf (const juce::File& f) const;
    void scanNow();
    std::function<void()> onScanned;

private:
    void run() override;
    void handleAsyncUpdate() override;
    void scan();
    mutable std::mutex lock_;
    juce::Array<juce::File> files_;
    std::map<juce::String, Seen> seen_;
    const int interval_;
};
} // namespace sc::files
