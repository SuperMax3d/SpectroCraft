# Lot 0 — minimal registry, `step.run`, session journal, `--json`, Actions fixes

Status: **done** (macOS arm64, Debug build with AI). Linux/Windows: see CI (note at the end of the file).

## Commits

1. `f1cfa62` — *Fix action-step replay defects and worker-side AI commits*: **only** the independent
   fixes, deliverable to SuperMax3d as a separate PR (Q12):
   - `applyActionStep`: `case 'e'` (`envelope`) added;
   - `noiseSuppress` step: `device` (optional param, otherwise `app.aiDevicePref`) resolved by
     `sc::resolveAiDevice` against `OvRuntime::devices()` instead of the literal `"auto"`; `df3PostFilter`,
     `copyNoise`, `autoDownload` honoured;
   - build without AI: `noiseSuppress` fails ("no AI support") instead of running `normalize`;
   - hardened dispatch: unknown type rejected before the `switch`, each `case` compares the full name
     ("sxxx" launched `silence`);
   - `Application::transcribe` and the default branch of `Application::noiseSuppress` commit on the
     message thread (`callAsync` + `WaitableEvent`, helper `commitOnMessageThread`), like the
     `copyNoise` branch and `runPythonScript`. The transcription is only stored if the document is still open.
   - Tests: `testApplyActionSteps` (envelope, look-alike types, NS without AI) and
     `testRegAiNoiseSuppressCommitHop` (`regression/ai.inc`), which **proves** the wait on the message thread
     (worker blocked as long as the message thread is not pumping) and replays NS with `device:"auto"`.
2. The lot 0 commit proper (registry, commands, `--json`, tests) — see `git log`.

## What was done

New files (`include/control/`, `src/control/`):

| File | Role |
|---|---|
| `CommandRegistry.h/.cpp` | `Spec {id, title, category, params (ParamSpec), flags, aliases, handler}`, `ParamSpec` → validation **and** restricted JSON Schema draft 2020-12 (a single source), `Request::parse`, `invoke()` (order of §5.2: doc resolution → flags → busy guard → temporary doc switch → `ScopedCapture` → handler → journal → `refreshAfterEdit` → envelope) |
| `ControlState.h/.cpp` | per-`Application` state: `Journal`, `JobManager`, `UiBridge*`, event sink; `Application::control()` (created on demand, stopped first by `~Application`) |
| `Journal.h/.cpp` | ring of 10,000 entries, monotonic `seq`, `kind ∈ command|step|history|job`, thread-safe, JSONL mirror planned (lot 5) |
| `JobManager.h/.cpp` | jobs `j-<n>` (one `std::thread` each, never detached, joined on shutdown), only one mutating job at a time, throttled progress, `JobControl::onMessageThread` (commit via `callAsync`, bounded wait: abandoned on shutdown) |
| `UiBridge.h` | interface (implemented by `MainWindow` in lot 1; `nullptr` in CLI/tests) |
| `StateJson.h/.cpp` | `app.state`, doc summary, `history.list` (`HistoryPanel` semantics, never `oldData/newData`), `helloInfo`, version from `Version.h` |
| `JsonCli.h/.cpp` | `--json <f|->`, `--json-out`, `--use-user-settings`, `--list-commands [--markdown]`; line-by-line session (`runJsonSession`, testable without a process) |
| `commands/Core.cpp` | `app.describe`, `app.state`, `doc.list/open/activate/meta/saveAs/save`, `selection.set/all`, `journal.read`, `debug.sleep` |
| `commands/History.cpp` | `history.list/undo/redo/jumpTo` (`applyPatch` failure → `failed {historyCleared:true}`) |
| `commands/Steps.cpp` | `step.run`, `steps.run`, `step.validate` (the 20 types, explicit `select`, `channels[]`) |
| `commands/Jobs.cpp` | `job.get/cancel/list` (cancellation reserved to the launching client unless `force`) |

Changes: `FileDoc::id` (stable, atomic id); `AlertGate::ScopedCapture` (depth + captured list,
never touches the headless flag); `Application::recordActionStep` journals **before** the
`actionRec.active` guard; `undoSteps/redoSteps` journal undo/redo/clearing; `Application::saveDocument`
(+ `beginSave/completeSave`, logic extracted from `MainWindow::runSaveAsync`); `Main.cpp`: branch for
`--json/--list-commands`, **unknown flag → exit code 2** (before: ignored, the UI started); CMake:
`SC_CONTROL_SOURCES` shared by the app and `SpectroCraftTests`.

## Tests added (`tests/regression/control.inc`, "regression: control")

- `testControlRegistry`: all **20** `actionTypes` pass `step.validate` then `step.run` on an
  explicit range and one channel (AI: `denseunet` with `device:"auto"`, `whisper-base`, Python: `Threshold.py`
  — explicit skip if model/interpreter is missing; without AI: `unsupported_build`).
- `testControlDocsHistory`: `no_document`, relative paths refused, stable ids, `unknown_doc`,
  **editing a non-active document** (`docSwitched`, active document and selection restored), `selection.set` on a
  non-active doc, `history.list` (opId/params/source join), undo/redo/jumpTo (redo stack kept),
  `redoDiscarded`, `doc.saveAs` (`confirm_required` then write), `doc.save` requires `confirm`, `doc.meta`,
  journal (commands, steps, undo/redo, source `menu`), `ScopedCapture`, `busy` during a mutating job,
  `job.cancel`, `job_not_found`, jobs stopped in < 2 s on destruction.
- `testControlDescribe`: every command has a closed object schema; its generated minimal `params` pass
  validation; every `invalid_params` cites a `path` present in the schema; min/max/enum/integer/
  items/range checks.
- `testControlJsonCli`: line-by-line streaming (each line answered before the next one is read),
  exit codes 0/1/2, `continueOnError`, unreadable JSON → 2, line > 1 MiB → 2, **timeout of a LongRunning**
  (`debug.sleep` cancelled and reported as `timeout` in < 5 s); then the **real executable** (`--json file
  --json-out file` with spaces in the path, exit codes 0/1/2, unknown flag → 2, `--list-commands --markdown`).

## Results (macOS arm64, Debug + AI, `ctest -j 8`)

- "regression: control" alone: **431/431** (with `apply action steps`), no skips (models and Python present).
- Full suite: **5283/5284** checks (baseline: 4859/4859). The only failure,
  `single instance mode: empty list acknowledged`, is the fixed 2 s delay of the FileForward pipe under
  machine load (five agents are building and testing in parallel on this Mac); it passes 5 times out of 5
  on its own, touches no code changed here, and had already appeared once on the fixes-only tree.
- **Test infrastructure fix**: the suite's temporary folder was named
  `SpectroCraftTests-tmp-shard<k>` for **all** processes on the machine; two checkouts tested at
  the same time (here: the other agents) deleted each other's files mid-run (failures
  "file loaded", "doc.open", a shard segfault). It now includes the PID
  (`...-shard<k>-<pid>`, `tests/UnitTests.cpp`, `scTestTempDir`).

## Deviations from the plan

- **JobManager brought forward to lot 0** (planned for lot 1): without it, the "timeout of a
  LongRunning" test required by `testControlJsonCli` is impossible. A test command `debug.sleep` (category `debug`,
  flag `Internal`, hidden from `app.describe` by default) serves as the reference job.
- `doc.save` (in place, `confirm` mandatory) added already in lot 0 alongside `doc.saveAs` (same code).
- `doc.activate`, `job.get/cancel/list` added already in lot 0 (cheap, useful for the CLI).
- `history.list`: before lot 5, `opId/params/source/ts` come from a **best-effort join** with the
  journal (undo depth + description); it gets it wrong if the stack has been truncated at 64 levels.
- The list of future states is in **redo order** (next redo first). Finding: the
  History panel (`HistoryPanel::refresh`, `src/Panels.cpp:920-925`) lists `redoAt(0..n)`, i.e.
  the reverse order: the first "future" row displayed is not the one that clicking it replays.
  Not fixed here (UI, to confirm with Max); to be handled in lot 5, which reworks this panel.
- No local Linux build (macOS machine, see `baseline-macos.md`).

## Debts / known limitations

- `step.run` is **synchronous** in lot 0: in the GUI, a long `noiseSuppress`/`transcribe`/`python` would block
  the message thread (the lot 1 server would answer `timeout`). Lot 1 converts these types into jobs.
- No "streamed file → job" detour yet, and no path perimeter (`control_roots`): lot 1.
- `recordActionStep` is called by the menus **before** the edit: an edit that fails leaves a
  `step` entry in the journal.
- The journal does not have a JSONL file yet (lot 5).

## Instructions for what comes next

- Lot 1: wire `ControlState::eventSink` and `Journal::onAppend` to the server; implement `UiBridge`
  in `MainWindow`; convert the long `step.run` types and the streamed detour into jobs via
  `JobManager::start` + `JobControl::onMessageThread`; add the path perimeter to `doc.open`,
  `doc.saveAs`, `python`.
- Any new command: a `Spec` with its `ParamSpec`s (the schema follows) and, if it is a pure read, the
  `Query` flag (not journaled on success).
