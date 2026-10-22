#pragma once
// SpectroCraft - Undo manager: patch-based undo/redo (>=50 levels)
#include "UndoPatch.h"
#include <deque>
#include <cstddef>

class UndoManager
{
public:
    static constexpr size_t kMaxLevels = 64;

    void record (Patch&& p)
    {
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

    void clear() { undoStack.clear(); redoStack.clear(); }

private:
    std::deque<Patch> undoStack, redoStack;
};
