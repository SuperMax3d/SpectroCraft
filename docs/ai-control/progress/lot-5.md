# Lot 5 — persistent journal, enriched Patch, events with replay

Status: **done**.

## What was done

- **Enriched `Patch`** (`include/UndoPatch.h`): `seq` (monotonic per document), `timestampMs`, `opId`,
  `params` (JSON, metadata), `source` (`menu`, `api:<client>`, `cli`, `test`). Set by
  `UndoManager::record` (the only recording point, §3 #7) from an **operation context**
  (`sc::undoctx`, `include/UndoManager.h`):
  - a scope on the current thread (`undoctx::Scoped`): set by `CommandRegistry::invoke` around
    the handler (`opId` = command id, `params` = its parameters without the token, `source` = client)
    and by `ScopedInvokeSource` around job commits;
  - otherwise a "pending" operation announced by `Application::recordActionStep` (dialogs/menus:
    step type + params, source `menu`) and taken by the next `record()` **on any
    thread** (the edit may run on a worker), valid for 60 s; the AI/Python jobs use it too
    (their patch is recorded in the `Application`'s commit hop, outside any scope);
  - otherwise `opId:"menu"`, `source:"menu"` (cut/paste, mouse…).
  The pending stack keeps only strings (a `static` must not own a `juce::var`: JUCE's
  leak detector complained about it).
- **Complete journal** (`ControlState::pollDocuments`): on every command (before/after), at every job
  end, and at 30 Hz via the window's timer **when the control server is running**:
  - `history` `recorded` per new patch (`patchSeq, ts, opId, params, source, description,
    undoDepth, redoDepth`) — including mouse edits; `undo`/`redo` (already journaled by
    `undoSteps/redoSteps`); `cleared` with a reason (`undo failed`, `redo failed`, `resample` inferred from a
    sample-rate change, `unknown`);
  - `meta` `changed` when a document's `metaDirty` counter moves (markers, selections, drawings,
    tables, transcription: counts are given, never the content);
  - `doc` `opened` / `closed`.
  The `command` entries no longer carry `undoDepth/description`: it is the `history recorded` entry
  that attributes each patch.
- **JSON Lines persistence**: `<userApplicationDataDirectory>/SpectroCraft/journal/
  session-<pid>-<yyyy-mm-dd>.jsonl` (macOS `~/Library/SpectroCraft/journal`), enabled when the
  control server starts (Q6), rotation beyond 64 MB (`.1.jsonl`), 30 sessions kept.
  `journal.read {sinceSeq}` re-reads the file for entries that have left the ring (10,000) and stitches
  them to the ring without a gap; `file` gives the path.
- **Events**: one entry = one event (`history.changed`, `meta.changed`, `doc.changed`, otherwise
  `journal.entry` if subscribed), `seq` = the journal's; non-journaled events (`job.*`,
  `doc.changed` from commands) take a reserved `seq` (`Journal::reserveSeq`) from the same counter.
  `sinceSeq` replay at `session.hello` / `events.subscribe` (ascending order).
- **`history.list`** reads `patchSeq/ts/opId/params/source` from the patches (lot 0's best-effort join
  now only serves as a fallback); envelope of `Mutates` commands: **`patchSeq`** of the last patch.
- **History tab**: edits made through the API show `hh:mm:ss  api:<client>` on the right (nothing
  changes for edits made in the window, so as not to touch the look or the rendering tests);
  **fix**: future rows are listed in redo order (lot 0's finding: a click
  on the first future row replayed a different entry from the one displayed).

## Tests (`tests/regression/journal.inc`, "regression: journal")

Standalone journal: ring of 10,000, JSONL mirror of each entry, reading beyond the ring from
the file then gap-free stitching, `reserveSeq`. Patches: API edit (seq, time, command, params,
client, `patchSeq` in the envelope), dialog edit (step type + params + `menu`), edit
without a type (`menu`). Journal: one `recorded` entry per patch, API/menu attributions, polling
idempotence, undo, marker → `meta`, resampling → `cleared {reason:"resample"}`, open/close.
`history.list`: each state carries `patchSeq/ts/opId/source`; futures in redo order; History
tab in the same order, time+client detail for the API, and the 1st future row is indeed the one that
`redoSteps(1)` replays. Server (`control_server.inc`): a new client with `sinceSeq:0` receives the
replayed `history.changed` events in `seq` order.

Results: see the bottom.

## Deviations / debts

- No `MarkerManager::onChange` hook nor `UndoManager::onChanged`: **polling** (on every
  command, job end, and at 30 Hz in the window if the server is running) covers all paths without
  wiring each `FileDoc` creation (a dozen sites); latency ≤ 33 ms in the open app.
  The journal of a headless session (`--json`) is only complete at command boundaries, which there is
  equivalent.
- A `recordActionStep` announcement followed by a dialog cancellation stays "pending" for ≤ 60 s and
  can label the next mouse edit.
- Undo depth remains 64; evicted patches keep their `recorded` entry in the journal.

## Results

macOS arm64, Debug + AI: "journal,control,history" 907/907; full suite **6026/6026**, 10/10.
