#pragma once
// SpectroCraft - AI control: append-only session journal.
//
// Every API command, every recorded action step (menus/dialogs) and every
// undo/redo lands here with a monotonic seq. In-memory ring of 10 000
// entries; optionally mirrored to a JSON Lines file (lot 5).
// Content: metadata only (ids, types, params, paths, counts) - never the
// control token, never transcription text or table values.
#include <JuceHeader.h>
#include <deque>
#include <functional>
#include <mutex>

namespace sc::control {

class Journal
{
public:
    static constexpr size_t kCapacity = 10000;

    Journal() = default;
    ~Journal();

    /** Appends an entry (an object var; seq and ts are filled in) and
        returns its seq. kind: command | job | history | meta | doc | alert | step.
        Thread-safe; listeners (onAppend) are called on the calling thread. */
    juce::int64 append (const juce::String& kind, juce::var entry);

    /** Entries with seq > sinceSeq, optionally filtered by kind and doc id
        (docId < 0 = any), at most `limit`. */
    juce::var read (juce::int64 sinceSeq, const juce::StringArray& kinds = {},
                    int limit = 200, int docId = -1) const;

    /** Snapshot copy of the entries with seq > sinceSeq (any kind). */
    std::vector<juce::var> entriesSince (juce::int64 sinceSeq) const;

    juce::int64 lastSeq() const;
    juce::int64 firstSeq() const;   // oldest seq still in the ring (0 when empty)

    /** JSONL mirror (lot 5). Empty = memory only. */
    void setFile (const juce::File& f);
    juce::File file() const;

    /** Called after every append (e.g. to broadcast an event). */
    std::function<void (const juce::var&)> onAppend;

private:
    mutable std::mutex lock;
    std::deque<juce::var> ring;
    juce::int64 nextSeq = 1;
    juce::File jsonl;
    std::unique_ptr<juce::FileOutputStream> out;
};

} // namespace sc::control
