#pragma once
// SpectroCraft - Undo manager: patch-based undo/redo (>=50 levels)
#include "UndoPatch.h"
#include <deque>
#include <cstddef>
#include <mutex>
#include <optional>
#include <algorithm>
#include <atomic>
#include <limits>
#include <vector>

/** Who is editing (AI control journal): the operation context stamped onto
    the next recorded Patch. A scope on the current thread (control commands
    and job commits) wins; otherwise a one-shot "pending" operation announced
    by a menu/dialog edit (Application::recordActionStep) is taken by the next
    record() on any thread (the edit may run on a worker), within 60 s. */
namespace sc::undoctx
{
    struct Op
    {
        juce::String opId;
        juce::var params;
        juce::String source;
    };

    inline Op*& current()   { static thread_local Op* op = nullptr; return op; }

    struct Scoped
    {
        explicit Scoped (Op o) : op (std::move (o)), prev (current()) { current() = &op; }
        ~Scoped() { current() = prev; }
        Op op;
        Op* prev;
        Scoped (const Scoped&) = delete;
        Scoped& operator= (const Scoped&) = delete;
    };

    struct PendingState
    {
        std::mutex lock;
        bool has = false;
        juce::String opId, source, paramsJson;   // plain strings: a static must not own a var
        juce::int64 atMs = 0;
    };
    inline PendingState& pendingState() { static PendingState s; return s; }

    inline void setPending (Op o)
    {
        auto& s = pendingState();
        std::lock_guard<std::mutex> l (s.lock);
        s.has = true;
        s.opId = o.opId;
        s.source = o.source;
        s.paramsJson = o.params.isVoid() ? juce::String() : juce::JSON::toString (o.params, true);
        s.atMs = juce::Time::currentTimeMillis();
    }

    inline std::optional<Op> takePending()
    {
        auto& s = pendingState();
        std::lock_guard<std::mutex> l (s.lock);
        std::optional<Op> o;
        if (s.has && juce::Time::currentTimeMillis() - s.atMs < 60000)
            o = Op { s.opId, s.paramsJson.isEmpty() ? juce::var() : juce::JSON::parse (s.paramsJson), s.source };
        s.has = false;
        s.opId.clear(); s.source.clear(); s.paramsJson.clear();
        return o;
    }

    inline void stamp (Patch& p)
    {
        if (Op* c = current())
        {
            p.opId = c->opId; p.params = c->params; p.source = c->source;
        }
        else if (auto o = takePending())
        {
            p.opId = o->opId; p.params = o->params; p.source = o->source;
        }
        else if (p.source.isEmpty())
            p.source = "menu";
        if (p.opId.isEmpty() && p.source == "menu")
            p.opId = "menu";   // an edit with no recorded step type (cut, paste, mouse...)
    }
}

class UndoManager
{
public:
    static constexpr size_t kMaxLevels = 64;

    UndoManager()  { auto& r = registry(); std::lock_guard<std::mutex> l (r.lock); r.all.push_back (this); }
    ~UndoManager()
    {
        auto& r = registry();
        std::lock_guard<std::mutex> l (r.lock);
        r.all.erase (std::remove (r.all.begin(), r.all.end(), this), r.all.end());
    }
    UndoManager (const UndoManager&) = delete;
    UndoManager& operator= (const UndoManager&) = delete;

    /** The default memory budget of the histories: a quarter of the RAM,
        between 512 MB and 8 GB. */
    static int64_t defaultByteBudget()
    {
        const int64_t ramMb = (int64_t) juce::SystemStats::getMemorySizeInMegabytes();
        return std::clamp<int64_t> (ramMb / 4, 512, 8192) * 1024 * 1024;
    }

    /** One budget shared by the histories of all open documents: they use the
        same RAM, so ten documents must not each keep a quarter of it. Past it
        the oldest steps across all documents are dropped first (each history
        always keeps its newest step), so a few whole-file edits on long
        recordings do not push the machine into swap. */
    static int64_t globalByteBudget() { return globalBudget().load(); }
    static void setGlobalByteBudget (int64_t bytes)
    {
        globalBudget().store (std::max<int64_t> (0, bytes));
        trimAllToGlobalBudget();
    }

    /** Drops the oldest steps across all histories until they fit the global
        budget. Runs on the message thread (record() posts it there when the
        edit ran on a worker): it changes other documents' histories, which
        the message thread reads. Returns the number of steps dropped. */
    static int64_t trimAllToGlobalBudget()
    {
        auto& r = registry();
        std::lock_guard<std::mutex> rl (r.lock);
        return trimGroupLocked (r.all, globalBudget().load());
    }

    /** The same rule applied to some histories only, with their own budget
        (tests: the app's other documents are left alone). */
    static int64_t trimGroupToBudget (const std::vector<UndoManager*>& group, int64_t budget)
    {
        std::lock_guard<std::mutex> rl (registry().lock);
        return trimGroupLocked (group, budget);
    }

    /** Audio bytes held by the histories of all documents. */
    static int64_t bytesHeldByAll()
    {
        auto& r = registry();
        std::lock_guard<std::mutex> rl (r.lock);
        int64_t b = 0;
        for (auto* um : r.all) b += um->bytesHeld();
        return b;
    }

    /** This history's own cap, on top of the global budget (none by default;
        tests set one). The newest step is always kept. */
    void setByteBudget (int64_t bytes)
    {
        std::lock_guard<std::mutex> l (lock);
        budget = std::max<int64_t> (0, bytes);
        trimToBudgetLocked();
    }
    int64_t byteBudget() const noexcept { return budget; }
    /** Audio bytes held by both stacks. */
    int64_t bytesHeld() const
    {
        std::lock_guard<std::mutex> l (lock);
        return bytesHeldLocked();
    }
    /** Steps dropped to stay within the budgets since the last clear. */
    int64_t droppedForMemory() const noexcept { return dropped; }

    void record (Patch&& p)
    {
        {
            std::lock_guard<std::mutex> l (lock);
            p.seq = nextSeq++;
            p.timestampMs = juce::Time::currentTimeMillis();
            p.recordOrder = ++recordCounter();
            sc::undoctx::stamp (p);
            undoStack.push_back (std::move (p));
            redoStack.clear();
            if (undoStack.size() > kMaxLevels)
                undoStack.pop_front();
            trimToBudgetLocked();
        }
        requestGlobalTrim();
    }

    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }

    // The most recently recorded patch (valid when canUndo()). Lets the caller
    // attach non-audio side state to an edit right after it was recorded.
    Patch& lastRecorded() { return undoStack.back(); }
    size_t depth() const { return undoStack.size(); }

    // Read-only enumeration for the History tab. Index 0 is always the
    // OLDEST entry of each stack; back() is the most recent.
    size_t undoSize() const { return undoStack.size(); }
    const Patch& undoAt (size_t i) const { return undoStack[(size_t) i]; }
    size_t redoSize() const { return redoStack.size(); }
    const Patch& redoAt (size_t i) const { return redoStack[(size_t) i]; }

    /** Moves the most recent step to the redo stack and returns it there
        (nullptr when there is none). No copy is made: apply the returned
        step (AudioEngine::applyPatch) - an in-place step swaps its samples
        with the buffer, so the step kept for redo must be the one applied. */
    Patch* popUndo()
    {
        std::lock_guard<std::mutex> l (lock);
        if (undoStack.empty()) return nullptr;
        redoStack.push_back (std::move (undoStack.back()));
        undoStack.pop_back();
        return &redoStack.back();   // a deque keeps references on push_back
    }

    /** The same towards the undo stack. */
    Patch* popRedo()
    {
        std::lock_guard<std::mutex> l (lock);
        if (redoStack.empty()) return nullptr;
        undoStack.push_back (std::move (redoStack.back()));
        redoStack.pop_back();
        return &undoStack.back();
    }

    void clear()
    {
        std::lock_guard<std::mutex> l (lock);
        undoStack.clear(); redoStack.clear(); dropped = 0; ++clears;
    }

    /** Sequence number the next recorded patch will get (journal polling). */
    int64_t peekNextSeq() const { return nextSeq; }
    /** How many times the history was cleared (journal polling). */
    int64_t clearCount() const { return clears; }

private:
    struct Registry
    {
        std::mutex lock;
        std::vector<UndoManager*> all;
    };
    // Never destroyed: a history may outlive the statics at exit.
    static Registry& registry() { static auto* r = new Registry(); return *r; }
    static std::atomic<int64_t>& recordCounter() { static std::atomic<int64_t> n { 0 }; return n; }
    static std::atomic<int64_t>& globalBudget() { static std::atomic<int64_t> b { defaultByteBudget() }; return b; }

    /** Applies the global budget now on the message thread (or when there is
        none), otherwise posts it there once. */
    static void requestGlobalTrim()
    {
        auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        if (mm == nullptr || mm->isThisTheMessageThread())
        {
            trimAllToGlobalBudget();
            return;
        }
        static std::atomic<bool> posted { false };
        if (posted.exchange (true))
            return;
        juce::MessageManager::callAsync ([] { posted = false; trimAllToGlobalBudget(); });
    }

    // The oldest step across the group goes first, until the group fits.
    static int64_t trimGroupLocked (const std::vector<UndoManager*>& group, int64_t budget)
    {
        int64_t droppedNow = 0;
        for (;;)
        {
            int64_t total = 0;
            UndoManager* oldest = nullptr;
            int64_t oldestOrder = 0;
            for (auto* um : group)
            {
                std::lock_guard<std::mutex> l (um->lock);
                total += um->bytesHeldLocked();
                if (um->undoStack.size() > 1
                    && (oldest == nullptr || um->undoStack.front().recordOrder < oldestOrder))
                {
                    oldest = um;
                    oldestOrder = um->undoStack.front().recordOrder;
                }
            }
            if (total <= budget || oldest == nullptr)
                return droppedNow;
            std::lock_guard<std::mutex> l (oldest->lock);
            oldest->undoStack.pop_front();
            ++oldest->dropped;
            ++droppedNow;
        }
    }

    int64_t nextSeq = 1;
    int64_t clears = 0;
    int64_t budget = std::numeric_limits<int64_t>::max();
    int64_t dropped = 0;
    mutable std::mutex lock;   // the stacks: the global trim may run while a worker records
    std::deque<Patch> undoStack, redoStack;

    int64_t bytesHeldLocked() const
    {
        int64_t b = 0;
        for (const auto& p : undoStack) b += p.audioBytes();
        for (const auto& p : redoStack) b += p.audioBytes();
        return b;
    }

    void trimToBudgetLocked()
    {
        while (undoStack.size() > 1 && bytesHeldLocked() > budget)
        {
            undoStack.pop_front();
            ++dropped;
        }
    }
};
