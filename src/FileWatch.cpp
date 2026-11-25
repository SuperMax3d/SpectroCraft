// SpectroCraft - open files changed or deleted on disk (issue #70); see include/FileWatch.h
#include "FileWatch.h"

namespace sc::files
{
DiskStamp DiskStamp::of (const juce::File& f)
{
    DiskStamp s;
    s.exists = f.existsAsFile();
    if (s.exists)
    {
        s.modMs = f.getLastModificationTime().toMilliseconds();
        s.size = f.getSize();
    }
    return s;
}

namespace
{
struct Registry
{
    std::mutex lock;
    std::map<juce::String, int> writing;          // path -> writes running
    std::map<juce::String, OwnWrite> lastOwn;     // path -> the last one's stamp and end
};
Registry& registry()
{
    static Registry r;
    return r;
}
} // namespace

ScopedWrite::ScopedWrite (juce::File f) : file_ (std::move (f))
{
    auto& r = registry();
    std::lock_guard<std::mutex> g (r.lock);
    ++r.writing[file_.getFullPathName()];
}

double nowMs()
{
    return juce::Time::getMillisecondCounterHiRes();
}

ScopedWrite::~ScopedWrite()
{
    const auto stamp = DiskStamp::of (file_);
    const double end = nowMs();
    auto& r = registry();
    std::lock_guard<std::mutex> g (r.lock);
    const auto key = file_.getFullPathName();
    r.lastOwn[key] = OwnWrite { stamp, end };
    if (--r.writing[key] <= 0)
        r.writing.erase (key);
}

bool isBeingWritten (const juce::File& f)
{
    auto& r = registry();
    std::lock_guard<std::mutex> g (r.lock);
    return r.writing.count (f.getFullPathName()) > 0;
}

std::optional<OwnWrite> lastOwnWrite (const juce::File& f)
{
    auto& r = registry();
    std::lock_guard<std::mutex> g (r.lock);
    const auto it = r.lastOwn.find (f.getFullPathName());
    if (it == r.lastOwn.end())
        return std::nullopt;
    return it->second;
}

DiskState diskStateFor (const DiskStamp& known, const DiskStamp& now, bool beingWritten,
                        const std::optional<DiskStamp>& ownWrite,
                        const std::optional<DiskStamp>& ignored, DiskStamp* adopt)
{
    if (beingWritten || now == known)
        return DiskState::same;
    if (ownWrite.has_value() && *ownWrite == now)
    {
        if (adopt != nullptr) *adopt = now;
        return DiskState::same;
    }
    if (ignored.has_value() && *ignored == now)
        return DiskState::same;
    return now.exists ? DiskState::modified : DiskState::deleted;
}

DiskWatcher::DiskWatcher (int intervalMs)
    : juce::Thread ("SpectroCraft disk watch"), interval_ (juce::jmax (100, intervalMs))
{
}

DiskWatcher::~DiskWatcher()
{
    signalThreadShouldExit();
    notify();
    stopThread (5000);
    cancelPendingUpdate();
}

void DiskWatcher::start()
{
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::low);
}

void DiskWatcher::setFiles (const juce::Array<juce::File>& files)
{
    std::lock_guard<std::mutex> g (lock_);
    files_ = files;
}

std::optional<DiskWatcher::Seen> DiskWatcher::seenOf (const juce::File& f) const
{
    std::lock_guard<std::mutex> g (lock_);
    const auto it = seen_.find (f.getFullPathName());
    if (it == seen_.end())
        return std::nullopt;
    return it->second;
}

void DiskWatcher::scan()
{
    juce::Array<juce::File> files;
    {
        std::lock_guard<std::mutex> g (lock_);
        files = files_;
    }
    std::map<juce::String, Seen> next;
    for (const auto& f : files)
        if (f.getFullPathName().isNotEmpty())
        {
            const double at = nowMs();   // before the look: a write ending after it is not trusted
            next[f.getFullPathName()] = Seen { DiskStamp::of (f), at };   // the slow part, outside the lock
        }
    std::lock_guard<std::mutex> g (lock_);
    seen_ = std::move (next);
}

void DiskWatcher::scanNow()
{
    scan();
    if (onScanned)
        onScanned();
}

void DiskWatcher::run()
{
    while (! threadShouldExit())
    {
        scan();
        triggerAsyncUpdate();
        wait (interval_);
    }
}

void DiskWatcher::handleAsyncUpdate()
{
    if (onScanned)
        onScanned();
}
} // namespace sc::files
