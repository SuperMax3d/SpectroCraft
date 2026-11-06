// SpectroCraft - AI control: history commands (history.list / undo / redo / jumpTo).
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "control/StateJson.h"
#include "Application.h"

namespace sc::control {

namespace {

/** Runs undoSteps/redoSteps and reports what happened, including the
    "history cleared" failure (applyPatch could not apply a patch). */
Outcome move (Context& ctx, bool undo, int steps)
{
    FileDoc& d = *ctx.doc;
    const int before = (int) d.undo.undoSize();
    const int avail = undo ? before : (int) d.undo.redoSize();
    if (avail == 0)
        return Outcome::failure (err::failed, undo ? "nothing to undo" : "nothing to redo",
                                 obj ({ { "undoDepth", before }, { "redoDepth", (int) d.undo.redoSize() } }));
    steps = std::min (steps, avail);

    // Descriptions of the patches about to move (for the result).
    juce::Array<juce::var> descs;
    for (int k = 0; k < steps; ++k)
        descs.add (undo ? d.undo.undoAt ((size_t) (before - 1 - k)).description
                        : d.undo.redoAt (d.undo.redoSize() - 1 - (size_t) k).description);

    if (undo) ctx.app.undoSteps (steps);
    else      ctx.app.redoSteps (steps);

    const int after = (int) d.undo.undoSize();
    const int moved = undo ? before - after : after - before;
    if (moved != steps && d.undo.undoSize() == 0 && d.undo.redoSize() == 0)
        return Outcome::failure (err::failed,
                                 juce::String (undo ? "Undo" : "Redo") + " failed (not enough memory?); history cleared",
                                 obj ({ { "historyCleared", true }, { "applied", std::max (0, moved) } }));

    auto r = obj ({ { "currentState", after }, { undo ? "undone" : "redone", moved } });
    if (descs.size() == 1) set (r, "description", descs[0]);
    else                   set (r, "descriptions", descs);
    return Outcome::success (r);
}

Outcome listCmd (Context& ctx)
{
    return Outcome::success (historyList (ctx.app, *ctx.doc, ctx.flag ("includeJournal", true),
                                          (juce::int64) ctx.num ("since", 0)));
}

Outcome undoCmd (Context& ctx) { return move (ctx, true, ctx.integer ("steps", 1)); }
Outcome redoCmd (Context& ctx) { return move (ctx, false, ctx.integer ("steps", 1)); }

Outcome jumpCmd (Context& ctx)
{
    FileDoc& d = *ctx.doc;
    const int cur = (int) d.undo.undoSize();
    const int last = cur + (int) d.undo.redoSize();
    const int target = ctx.integer ("state", cur);
    if (target < 0 || target > last)
        return Outcome::failure (err::invalidParams, "state must be in 0.." + juce::String (last),
                                 obj ({ { "path", "state" }, { "expected", "0.." + juce::String (last) } }));
    if (target == cur)
        return Outcome::success (obj ({ { "currentState", cur }, { "undone", 0 }, { "redone", 0 } }));
    auto o = move (ctx, target < cur, std::abs (target - cur));
    if (!o.ok) return o;
    const int now = (int) d.undo.undoSize();
    return Outcome::success (obj ({ { "currentState", now },
                                    { "undone", std::max (0, cur - now) },
                                    { "redone", std::max (0, now - cur) } }));
}

} // namespace

void registerHistoryCommands (CommandRegistry& r)
{
    r.add ({ "history.list", "Undo history of a document (+ the session journal)", "history",
             { P ("includeJournal", PType::Boolean).def (true),
               P ("since", PType::Integer).min (0).def (0).describe ("Journal entries after this seq") },
             {}, NeedsDoc | Query, {}, listCmd });
    r.add ({ "history.undo", "Undo the last edit(s)", "history",
             { P ("steps", PType::Integer).range (1, 64).def (1) },
             {}, NeedsDoc | Mutates, { "undo" }, undoCmd });
    r.add ({ "history.redo", "Redo the next edit(s)", "history",
             { P ("steps", PType::Integer).range (1, 64).def (1) },
             {}, NeedsDoc | Mutates, { "redo" }, redoCmd });
    r.add ({ "history.jumpTo", "Undo/redo to a history state (0 = as loaded); never discards the redo stack", "history",
             { P ("state", PType::Integer).req().min (0) },
             {}, NeedsDoc | Mutates, {}, jumpCmd });
}

} // namespace sc::control
