# Lot 1 — control server in the running app + jobs

Status: **done** (macOS arm64). See "Results" and "CI" at the end of the file.

## What was done

### Server (`include/control/ControlServer.h`, `src/control/ControlServer.cpp`)

- `juce::Thread "SC-Control"`: `createListener (0, "127.0.0.1")` + `getBoundPort()`, accept loop
  `waitUntilReady (true, 500)`; one `SC-ControlClient-<n>` thread per client, **4 clients max** (the 5th receives
  `busy` and is then closed).
- NDJSON: split on `\n` (CRLF tolerated), line > 1 MiB → `invalid_params` and the session continues,
  unreadable JSON → `invalid_params`.
- **Authentication**: `session.hello {token, client, subscribe[], sinceSeq}` mandatory first
  (constant-time comparison); 3 failures → 2 s pause; `unauthorized` otherwise. Token = 2 `Uuid`s
  (64 hex), never journaled (`sanitizedParams` strips `token`), only in the discovery file.
- **The network thread never touches `Application`**: each request is posted via `callAsync` with a shared
  `PendingCall` (`WaitableEvent` + `abandoned`) and waited for **at most `timeoutMs`** (default 30 s) →
  `timeout {hint: "a modal dialog is probably open"}`; while waiting, the event queue is drained
  every 20 ms. On shutdown, the wait is abandoned → `shutting_down`.
- **Shutdown** (`stop()`): `shuttingDown`, **self-connection to the actual port** before `listener->close()`
  (JUCE connects to port 0 on Linux, §5.2), `stopThread (2000)`, sessions closed then joined,
  discovery file deleted. Called in `shutdown()` **before** `mainWindow.reset()`.
- **Discovery file** `<userApplicationDataDirectory>/SpectroCraft/control/<pid>.json`
  `{pid, port, token, version, protocol, startedAt, exe, platform}`: folder 0700, file created empty,
  set to **0600**, then written (POSIX); at startup, files whose PID is dead are swept
  (`kill(pid,0)` / `OpenProcess`).
- **Events**: each journal entry → `journal.entry` (subscription `journal`) and, depending on the type,
  `history.changed` (undo/redo, mutating commands and jobs, menu edits); `job.progress`/`job.done`
  (JobManager); `doc.changed` (doc/selection/view/markers commands). Queue bounded at 1000 per client,
  `events.dropped {count}` beyond that. `events.subscribe {kinds, sinceSeq}` and `session.hello {sinceSeq}`
  **replay** the in-memory journal (full/persistent replay is in lot 5).
- `session.bye` closes cleanly.

### Application / window

- Setting `control_enabled` (default 0), menu **Window > Allow AI control (localhost)** (starts/stops the
  server live), flag **`--control`** (starts the server and **skips `handleStartup`**: never any
  forward to an existing instance, even with `multi_instance=0`, Q15).
- CMake option **`SC_ENABLE_CONTROL`** (ON, Q14): when OFF, no server, no `--control` and no menu item
  (the registry and `--json` remain).
- "AI control: N client(s)" indicator in the status bar (`StatusBar::setControlText`).
- `MainWindow` implements `sc::control::UiBridge` (refresh, time window, zooms, views,
  panels, non-blocking progress dialog for jobs, quit) and registers itself in `app.control().ui`.
- `MainWindow::runSaveAsync` now goes through `Application::beginSave/completeSave` (a single
  save path for the menu and the API).

### Jobs

- `startJob (ctx, …)`: with a window, opens the same `WorkProgressDialog` as the menus (Cancel included →
  cancels the job); `isBackgroundJobRunning()` and the quit refusal see it without modification.
- These become jobs: `step.run`/`steps.run` containing `noiseSuppress`, `transcribe` or `python`
  (the worker never edits the document: these methods commit via the message thread; the other
  steps of a sequence go through `JobControl::onMessageThread`); `doc.open` > 200 MB (FileDoc loaded on
  the worker, inserted on the message thread); `doc.save/saveAs` in the running app (audio written on the
  worker, `completeSave` on the message thread); **any `Mutates` on a streamed file** (detour
  §5.2 step 4: `ensureEditable` on the worker then the command, result `materialised:true`).
- A job on a non-active document makes that document active permanently (`activated:true`), since the
  worker works on the active document.
- During a mutating job, any non-`Query` command (except `job.*`) answers `busy`.

### New commands

`view.set`, `view.zoom {factor|to|vertical}`, `view.channels`, `view.show` (NeedsUi), `ui.panels`,
`ui.panel.set` (NeedsUi), `transport.play/stop/pause/loop/playSpectralSel` (`ensureAudioReady (2000)`,
never the infinite wait of `play()`), `markers.add/list`, `doc.close {confirm}`, `doc.closeAll {confirm}`,
`app.quit {confirm}` (NeedsUi; refused if a job is running).

### Security (§5.3)

- Loopback only, token, opt-in, pause after 3 failures, 4 clients, lines ≤ 1 MiB, params ≤ 256 KiB,
  bounded event queue.
- **Path perimeter** (`include/control/PathPolicy.h`): roots = setting `control_roots`
  (separator `;`) or, by default, home folder + `scriptFolder()` + temporary folder; symbolic
  links resolved on every existing component (`realpath`, POSIX); outside the roots →
  `confirm_required {reason:"outside_roots"}`; the `control/` folder and `SpectroCraft.recent` are refused even
  with `confirm`. Applied to `doc.open`, `doc.saveAs`, and to the scripts of `python` steps, which additionally
  require `confirm` outside `scriptFolder()`.

## Tests (`tests/regression/control_server.inc`, serial test "control server")

- server: sweep of a forged `<pid>.json` (dead PID), discovery file `{pid, port, token,
  protocol}` in 0600, `unauthorized` before hello, wrong token, **pause ≥ 1.5 s after 3 failures**,
  hello (protocol, version), `doc.open`, `step.run` on a non-active doc (`docSwitched`), `history.changed`
  event, job + `job.done`, line > 1 MiB then session still usable, `unknown_cmd`, unreadable
  JSON; **message thread blocked → `timeout`** (bounded wait); 4 clients accepted, 5th `busy`;
  **shutdown with a connected client and a request in flight**: `stop()` < 2 s, `shutting_down` response,
  file deleted, port closed; restart.
- path perimeter: temp allowed, outside the roots → `confirm_required`, `confirm` accepted, control
  folder and `.recent` refused, `doc.saveAs` outside the roots, outgoing symbolic link resolved.
- view/transport/markers/close in headless mode (`needs_ui` for vertical/show/quit), **streamed file →
  job that materialises it then applies the step**.

## Independent review (reviewer sub-agent, read-only) and fixes applied

An adversarial review of lots 0-1 found the following, which I then fixed in this lot:

1. **Busy guard too narrow**: only `Mutates|LongRunning` commands were refused during a
   modal dialog / a background operation. JUCE delivers `callAsync` calls inside modal loops: a
   `doc.close` could free the document that the `runSaveAsync` thread is writing (use-after-free). Now
   **any non-`Query` command** (except `job.*`) answers `busy` in these cases.
2. **`app.quit`** bypassed the "job running" refusal: `requestQuit (discardUnsaved)` re-checks, at the moment
   of quitting, `isBackgroundJobRunning()`, the API jobs and the unsaved documents.
3. **Crash before authentication**: JUCE's JSON parser is recursive; a line of 100,000 `[`
   would have overflowed the session thread's stack. Depth limited to 64 (pre-scan); before
   `session.hello`, lines ≤ 4 KiB and **10 s** to authenticate (otherwise closed: the 4 slots can
   no longer be squatted).
4. Steps executed in a job were journaled as `source:"menu"` → `ScopedInvokeSource` around the commits.
5. `history.list` attributed the last edit to `doc.save`/`history.undo`: `undoDepth/description`
   are only journaled if the content actually changed (`contentVersion`), outside the `history` category
   (same for job-completion entries).
6. `transport.*` and `view.*` on a non-active document played/displayed the wrong document: flag
   `ActiveDocOnly` → `invalid_params {hint: doc.activate}`.
7. Server-side `send()` bounded to 5 s and interrupted by shutdown.
8. `session.hello`: a single response even on failure, authentication **after** the response, no
   event before it, replay afterwards.
9. `--json` options can come in any order (`--json-out` before `--json` gave "unknown flag").

Unfixed suspicions (documented): the worker of a save job or of the streamed detour reads/writes
the `AudioEngine` while `Query` commands (`app.state`) read its atomic counters — this is the model of
`MainWindow::runDestructiveEdit`/`runSaveAsync`, which relies on `AudioEngine`'s internal synchronisation;
an app shutdown waits for an ongoing save or download to finish (no cancellation
possible); `job.cancel` trusts the declared client name.

Lot 2 constraints (MCP adapter) respected: event `seq` = journal counter
(`Journal::reserveSeq` for non-journaled events, **a single event per entry**);
discovery file written after the accept thread starts, deleted on shutdown;
`startedAt` ISO-8601 UTC; FileForward unchanged; `--control` skips `handleStartup`; `Query` only
on pure reads. Shapes aligned with §6 at little cost: `history.list` carries `doc` **and**
`docId`, state 0 `label` **and** `description`; `doc.open` returns `id` **and** `doc`. `patchSeq`:
lot 5.

## Deviations from the plan

- `tools/mcp/sc_client.py` **not written here**: the lot 2 agent (`ai-control-mcp`) owns `tools/mcp/**`.
  The manual end-to-end test was done with a throwaway Python script (see "Results").
- `ClientSession` is in `ControlServer.cpp` (no separate `ClientSession.cpp`).
- Requests from a single client are processed **sequentially** (no pipeline of 64 in-flight requests): the
  limit of 64 is therefore trivially respected; a client that wants parallelism opens a 2nd connection.
- `transport.play` without a device (CI, `SC_NO_AUDIO_DEVICE`) succeeds silently (`deviceDisabled:true`)
  instead of failing: this is the behaviour of `Application::play()`.
- Windows: link resolution only applies to the last component of the path.

## Debts / limitations

- Event replay only covers the in-memory ring (10,000 entries); persistence in lot 5.
- Abandoning a request after `timeout` does not cancel its execution: it may still run when
  the message thread frees up (the client must re-read `app.state`).
- No explicit ACL on Windows for the discovery file (user profile, threat model
  adopted in §5.2).

## Results (macOS arm64, Debug + AI)

- "control server": 67/67 (serial), "regression: control" + server: 655/655 with the neighbouring tests.
- Full suite `ctest -j 8`: **5482/5482**, 10/10 (baseline 4859, lot 0 5284).
- **Real end-to-end** (throwaway Python script, app launched with `--control`, real window): discovery
  (`~/Library/SpectroCraft/control/<pid>.json`), `session.hello`, `doc.open`, `step.run fadeOut` with
  `history.changed` event (source `api:smoke`), `history.list`, `view.zoom {vertical}` (NeedsUi),
  `ui.panels`, `history.undo`, `app.state`, `app.quit {confirm}` → the app quits (code 0) and deletes its
  file. An orphan file left by a `kill` was correctly swept at the next launch.
- This test showed that JUCE's temporary folder on macOS is `$TMPDIR/SpectroCraft`: the default roots
  therefore also include `$TMPDIR` and `/tmp` (POSIX).
- Linux: portability fix `int64_t` → `juce::int64` (lot 0's `linux-ai` CI had failed on
  `contentVersion()`: `long` ambiguous for `juce::var`); lot 0's macOS and Windows were green.

## Instructions for what comes next

- Lot 2: the actual protocol is the one in `src/control` (§6 envelope); `session.hello` returns
  `{app, version, pid, protocol, platform, capabilities, commands, journalSeq, client, replayed}`.
  Events: `{"event", "seq", "ts", "data"}`.
- Any long command: `startJob` + `JobControl::onMessageThread` for the commit.
