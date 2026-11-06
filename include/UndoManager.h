#pragma once
// SpectroCraft - Undo manager: patch-based undo/redo (>=50 levels)
#include "UndoPatch.h"
#include <deque>
#include <cstddef>
#include <mutex>
#include <optional>

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

    void record (Patch&& p)
    {
        p.seq = nextSeq++;
        p.timestampMs = juce::Time::currentTimeMillis();
        sc::undoctx::stamp (p);
        undoStack.push_back (std::move (p));
        redoStack.clear();
        if (undoStack.size() > kMaxLevels)
            undoStack.pop_front();
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

    bool popUndo (Patch& out)
    {
        if (undoStack.empty()) return false;
        Patch p = std::move (undoStack.back());
        undoStack.pop_back();
        redoStack.push_back (p); // keep a copy for redo
        out = std::move (p);
        return true;
    }

    bool popRedo (Patch& out)
    {
        if (redoStack.empty()) return false;
        Patch p = std::move (redoStack.back());
        redoStack.pop_back();
        undoStack.push_back (p); // keep a copy for undo
        out = std::move (p);
        return true;
    }

    void clear() { undoStack.clear(); redoStack.clear(); ++clears; }

    /** Sequence number the next recorded patch will get (journal polling). */
    int64_t peekNextSeq() const { return nextSeq; }
    /** How many times the history was cleared (journal polling). */
    int64_t clearCount() const { return clears; }

private:
    int64_t nextSeq = 1;
    int64_t clears = 0;
    std::deque<Patch> undoStack, redoStack;
};
