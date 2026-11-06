# Lot 2 — Python MCP adapter + Claude Code integration

Status: **done on the Python side, tested**; end-to-end against the **running** app impossible as long as the
control server (lot 1) does not exist. Branch `ai-control-mcp` of the fork, branched from `ai-control` =
`76de144` (the `5a80afd` cited in the instructions is the local, unpushed version of the same docs commit:
only one sentence of `DECISIONS.md` differs; `76de144` is the fork's, hence a conflict-free merge).

## What was done

| File | Role |
|---|---|
| `tools/mcp/pyproject.toml` | package `spectrocraft-mcp`, script `spectrocraft-mcp = spectrocraft_mcp.server:main`, `mcp>=1.6,<2`, extra `[test]` (pytest, jsonschema); works with `uv run --directory tools/mcp spectrocraft-mcp` and `pip install -e tools/mcp` (both verified) |
| `spectrocraft_mcp/protocol.py` | constants (protocol 1, line ≤ 1 MiB, 30 s), closed error codes §6.1, `ControlError.describe()` (code, message, `details.hint`, per-code next steps, details) |
| `spectrocraft_mcp/discovery.py` | `<appData>/SpectroCraft/control/*.json` at the real paths (macOS `~/Library/…`, Linux `$XDG_CONFIG_HOME` or `~/.config/…`, Windows `%APPDATA%\…`); deletes files whose PID is dead; **reused PID**: file discarded if the live process started more than 5 s after `startedAt` (creation time via `/proc` on Linux, `ps -o lstart` on macOS, `GetProcessTimes` on Windows); selection by `SPECTROCRAFT_PID`, otherwise the most recent; the token never appears in outputs |
| `spectrocraft_mcp/client.py` | thread-safe blocking client: `session.hello {token, client, subscribe, sinceSeq}`, reader thread that routes by `id` (out-of-order responses OK) and queues events (bounded at 1000), `call()`/`result()`/`run()`, `wait_job(job, on_progress)` (`job.progress`/`job.done` events **plus** polling `job.get` every 2 s, hence robust if the app does not emit events yet), **silent reconnection** on the next call with `sinceSeq` replay (reset to zero if it is a different process), 1 MiB limit in both directions; an unsent request is resent after reconnection, a request lost in flight is only resent if the command is read-only (`Query` in `app.describe`) |
| `spectrocraft_mcp/dynamic.py` | dynamic tools from `app.describe` (`params` schema unchanged + optional `doc`, `timeoutMs`, `confirm`; renamed to `sc_doc` if a command already has a `doc` param) |
| `spectrocraft_mcp/pipe_probe.py` | probe of the `SpectroCraft.FileForward` pipe (`int32 0` → ack `0x01`): FIFOs `/tmp/SpectroCraft.FileForward_in/_out` (POSIX, orphan FIFO → "nobody") and `\\.\pipe\SpectroCraft.FileForward` (Windows) |
| `spectrocraft_mcp/launcher.py` | `resolve_exe` (`.app` bundle accepted), `launch()` (probe first, then `SPECTROCRAFT_EXE --control`, stdin=DEVNULL, detached, wait ≤ 20 s for the `<pid>.json`), `run_headless()` (`--json - --json-out <tmp>`, ids `h1…`, `continueOnError`, exit code, tail of stderr) |
| `spectrocraft_mcp/server.py` | FastMCP: the 24 fixed tools of §7.3, dynamic tools, resources `spectrocraft://commands` and `spectrocraft://journal/tail`, progress relayed through `ctx.report_progress`, app errors → MCP tool errors (`isError`) with code + hint + next steps, `SPECTROCRAFT_LAUNCH` policy (`ask`/`auto`/`never`), `eventsSinceLastCall` (edits made in the meantime with the mouse or by another client), `tools/list_changed` notification on first contact |
| `tools/mcp/sc_client.py` | **dependency-free** CLI (`python3 -I -S` verified): `--list`, `app.state`, `step.run '{"type":"fadeOut",…}'` (flat form accepted), waiting for jobs with progress on stderr, `--events N`, exit codes 0/1/2 |
| `tools/mcp/smoke.sh` | local end-to-end against the running app (`--control`): `app.state`, `doc.open`, `step.run`, `history.list`, `history.undo` |
| `.mcp.json.example`, `.gitignore` | macOS template from §7.4; `.mcp.json`, `tools/mcp/.venv`, `uv.lock`, caches ignored |
| `CLAUDE.md` | instructions for Claude Code: tools, vocabulary (docId, select, channels, jobs, confirm, units), Ninja Debug build, ctest, `SC_TEST_FILTER`, Python tests |
| `docs/ai-control/mcp.md` | macOS/Windows/Linux installation, `uv`/`pip` variants, variables, troubleshooting |

Tests (`tools/mcp/tests`, pytest): in-process fake NDJSON server (`fake_server.py`: token,
routing by `id`, events + `sinceSeq` replay, jobs with progress/failure/cancellation, error codes,
forced disconnections, giant lines) and fake executable (`fake_exe.py`: `--json` and `--control`).
Files: `test_discovery` (per-OS paths, dead PID, reused PID, `startedAt` ISO/s/ms, corrupted
files), `test_client` (hello, `unauthorized`, out-of-order routing, 6 error codes, events,
`wait_job` with/without events, failure, cancellation, timeout, reconnection + replay, app relaunched, in-flight
mutation not replayed, read replayed, 1 MiB limits), `test_dynamic` (**every `inputSchema` validated by
`jsonschema.Draft202012Validator.check_schema`**, original schema intact, names, collisions, schema
missing or not an object, argument validation, `describe_lot0.json` = **real** describe from lot 0),
`test_pipe_probe` (POSIX), `test_launcher`, `test_server` (in-memory MCP client ↔ server ↔ fake app:
fixed/dynamic tools, routing, errors, progress, resources, `sc_launch`, `sc_headless`),
`test_stdio` (`python -m spectrocraft_mcp` over real stdio), `test_sc_client`, `test_real_exe`
(**skip** without `SPECTROCRAFT_EXE`).

## Results

- macOS arm64, Python 3.11.15, `mcp` 1.30.0: **98 passed, 4 skipped** (the 4 `test_real_exe` tests) in ~9 s;
  same result via `pip install -e "tools/mcp[test]"` then `pytest tools/mcp/tests` from the root.
- With `SPECTROCRAFT_EXE` = **real** build of `ai-control` `693f3f3` (lot 0, Release, `-DSPECTRO_ENABLE_AI=OFF`,
  compiled outside the repository in a temporary folder): **102 passed**; real `--json` scenario verified:
  `doc.open` → `step.run fadeOut` on 0.5–1.5 s channel 0 → `history.list` → `history.undo` →
  `history.redo` → `doc.saveAs` (file written) → `doc.saveAs` on an existing file = `confirm_required`.
- `mcp` 1.6.0 (+ pydantic < 2.10): 96 passed, 4 skipped (the progress test is skipped: the
  `ClientSession` of 1.6 does not receive progress; the server does send it). `mcp` 1.2–1.5 no longer
  import with recent pydantic versions, hence the 1.6 floor.
- **Not run**: Linux and Windows (macOS machine, no container). The Linux-specific code
  (`/proc/<pid>/stat` for PID reuse) and the Windows-specific code (`OpenProcess`, pipe `\\.\pipe\…`) have
  not run anywhere; the CI job below covers it for Linux.
- **End-to-end with the running app: not done** — `ControlServer`/`--control` do not exist yet on
  `ai-control` (lot 1). `smoke.sh` is ready for that moment.

## Deviations from the report

1. **SDK pinned to `mcp>=1.6,<2`** (report: `mcp>=1.2`). `mcp` 2.x (current on PyPI) renamed `FastMCP` to
   `MCPServer` and redid the low-level API; the adapter targets 1.x (FastMCP + `report_progress`). A
   2.x port remains to be done the day 1.x is no longer enough.
2. **Dynamic tool names `sc_<domain>_<command>`** (report: `sc.<domain>.<command>`):
   the Anthropic API only accepts `[a-zA-Z0-9_-]{1,64}` and Claude Code prefixes `mcp__spectrocraft__`
   (19 chars), hence ≤ 45 characters (hash suffix beyond that). `session.*`, the `debug` category and the
   `Internal` flag are excluded.
3. **Dynamic tools by overriding `FastMCP.list_tools/call_tool`** (FastMCP cannot publish a raw
   `inputSchema`) rather than a second low-level server; resources served by overriding
   `list_resources/read_resource` (older FastMCP 1.x versions do not await an `async` resource
   function).
4. `confirm` is only added to the schema if the command does not already declare one, and is only passed on
   if it is `true` (the C++ validator tolerates an undeclared top-level boolean `confirm`,
   `CommandRegistry.cpp`, `validate`).
5. `sc_save`: without `path` → `doc.save` (in place), with `path` → `doc.saveAs` (actual lot 0 format:
   `doc.save` has no `path`). `sc_open` passes `confirm` only if requested.
6. Windows exe path: `build\src\SpectroCraft_artefacts\Release\SpectroCraft.exe` (§7.4 omits `src\`).
7. Actual lot 0 format differs from the §6 examples (no effect on the adapter, which relays as-is):
   `doc.open` returns the document summary with `id` (report: `doc`); `history.list` returns `doc`
   (report: `docId`) and state 0 has `description` (report: `label`); `step.run` does not return a
   `patchSeq`; an error on a `Mutates` command still carries the state fields.
8. Additions: `eventsSinceLastCall` in tool responses, `SPECTROCRAFT_JOB_WAIT_S` (maximum wait for a
   job, default 600 s, then "still running" without cancelling), `SPECTROCRAFT_CONTROL_DIR`,
   `SPECTROCRAFT_PIPE_BASE`, `SPECTROCRAFT_JOURNAL_DIR` (tests), `sc_client.py --events`.
9. The report does not fix the format of `startedAt`: the adapter accepts ISO-8601, epoch seconds or
   epoch milliseconds.

## To do by the main agent when merging

### Merge

`git merge ai-control-mcp` (no C++/CMake/README/workflow file touched; `.gitignore`: addition at the end
of the file). Then paste the two blocks below.

### README section (English, like the rest of the README; after "Building" for example)

```markdown
## AI control (Claude Code / MCP)

SpectroCraft can be driven by Claude Code (VS Code extension or terminal) while it is open: state,
history, undo/redo, any of the 20 action steps on an explicit time range and channels, AI jobs with
progress, measurements. The app runs a control server on the loopback interface only
(`127.0.0.1`, ephemeral port, per-instance token) when *Window > Allow AI control (localhost)* is
checked or when it is started with `--control`; a small Python MCP adapter in `tools/mcp` connects
Claude Code to it. `SpectroCraft --json <requests.ndjson|->` runs the same commands headless.

Setup (Python 3.11+):

    cp .mcp.json.example .mcp.json      # set SPECTROCRAFT_EXE to your executable
    # with uv: nothing else to install (uv run builds tools/mcp/.venv)
    # without uv: pip install -e tools/mcp, then use "command": "python3", "args": ["-m", "spectrocraft_mcp"]

Open the folder in VS Code / run `claude`, then ask e.g. "open recording_b.wav, select 2-3.5 s,
fade it out and show me the history". `python3 tools/mcp/sc_client.py app.state` checks the
connection without the `mcp` package. Details, Windows/Linux variants and troubleshooting:
[docs/ai-control/mcp.md](docs/ai-control/mcp.md); protocol: [docs/ai-control/REPORT.md](docs/ai-control/REPORT.md) §6.
```

### CI job (to add under `jobs:` in `.github/workflows/build.yml`)

```yaml
  # Python MCP adapter (tools/mcp): pytest against a fake NDJSON server and a
  # fake executable. No C++ build; test_real_exe.py skips (no SPECTROCRAFT_EXE).
  mcp-tests:
    runs-on: ${{ matrix.os }}
    timeout-minutes: 10
    strategy:
      fail-fast: false
      matrix:
        os: [ubuntu-24.04, macos-15]
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: "3.11"

      - name: Install the adapter and its test dependencies
        run: pip install -e "tools/mcp[test]"

      - name: pytest
        run: pytest tools/mcp/tests -ra
```

(`windows-latest` can be added to the matrix, but the Windows code has never run: try it on
a branch first. Tests that use FIFOs or a shell script are skipped on Windows.)

### Protocol details that lot 1 must respect (the adapter depends on them)

- **Discovery file** `<userApplicationDataDirectory>/SpectroCraft/control/<pid>.json` =
  `{pid, port, token, version, startedAt, exe, protocol}`, written **after** the listener accepts
  (the adapter connects as soon as it sees the file; `sc_launch` waits for the file whose `pid` = the
  launched process), deleted on shutdown. `startedAt` in ISO-8601 UTC (`isoNow()`) **later than or equal to**
  the process start (otherwise the adapter assumes a reused PID: 5 s tolerance).
- **`session.hello`**: `params {token, client, subscribe:[kinds], sinceSeq?}`; result = `helloInfo`
  (`protocol`, `journalSeq`…). Wrong token → `ok:false` `unauthorized` response (closing allowed).
  Any command before the hello → `unauthorized`.
- **Events** `{"event", "seq", "ts", "data"}`: `seq` **in the same space as the hello's `journalSeq`**
  and as `sinceSeq`, strictly increasing. The adapter initialises its last `seq` to `journalSeq` and
  **ignores any event with `seq` ≤ last seen** (replay deduplication) — except `events.dropped`. If
  events had their own counter, they would be lost.
- **Subscription**: `subscribe` lists event name prefixes (`job`, `history`, `doc`, `meta`).
- **Replay**: with `sinceSeq`, the app sends the events with `seq > sinceSeq` still in memory (before or
  after the hello response, it does not matter). Until that is done (lot 5), `wait_job` falls back on
  `job.get`: `job.get` must remain available during a mutating job (it is, category `job`).
- **Jobs**: handle `{"job":"j-N","state":"running"}`; `job.progress {job, progress ∈ [0,1], status?}`;
  `job.done` = `infoOf` (`ok`, `result` | `error`) — as in `JobManager.cpp` today.
- **Sources**: `history.changed.data.source` = `"api:" + client` (the adapter's client is named
  `spectrocraft-mcp/0.1`, the CLI's `sc_client/0.1`) or `"menu"`; the adapter uses it to report
  to Claude only the edits made by others.
- **Lines**: ≤ 1 MiB; a line that is too long received by the adapter drops the connection (it
  reconnects). Responses may arrive out of order (routing by `id`, ids `c1, c2…` as strings).
- **FileForward pipe**: keep "`int32 0` → ack `0x01`, no callback" (it is the probe of `sc_launch`
  and `sc_status`). `--control` must skip `handleStartup` (§5.2).
- **`Query` flag**: after a disconnection the adapter replays the `Query` commands of `app.describe`;
  do not set it on a command that modifies anything.

## Debts / follow-ups

- End-to-end with the running app (`smoke.sh`, real MCP tools) as soon as lot 1 is merged; then check
  clean shutdown (`shutting_down`) and recovery after the app is relaunched.
- Port to `mcp` 2.x (`MCPServer`) when needed.
- Windows: run the suite once (`OpenProcess`/`GetProcessTimes`, named pipe, `py -3`).
- The typed tools of lots 3-4 (`sc_noise_suppress`, `sc_transcribe`, `sc_measure_*`,
  `sc_export_spectrogram`, `sc_models`, `sc_devices`) follow the parameters of the §6.5-§6.6 examples;
  when these commands exist in C++, align the parameter names with their `ParamSpec`s (a
  mismatch gives `invalid_params` with the offending `path`, nothing silent).

## Merge into `ai-control` (main agent, 2026-11-05)

- `ai-control-mcp` merged at `6779c5c` (merge commit on `ai-control`), without conflict.
- Pasted: README section "AI control (Claude Code / MCP)" (extended
  with `--json`/`--control`/security) and CI job `mcp-tests` (ubuntu-24.04 + macos-15,
  Python 3.11). `CLAUDE.md` completed: `view.*`/`transport.*` require the active document, jobs on a non-active
  doc (`activated`), path perimeter.
- Fixed: `tools/mcp/smoke.sh` broke on a repository path containing spaces
  (`SC="$PY $HERE/sc_client.py"` split by the shell) → function `sc()`.
- Verified on macOS arm64 with the real lot 1 app (Debug + AI):
  - `pytest tools/mcp/tests`: **98 passed, 4 skipped**; with `SPECTROCRAFT_EXE` = real build:
    **102 passed**;
  - `tools/mcp/smoke.sh` against the app launched with `--control`: **smoke OK** (discovery, `app.state`,
    `doc.open`, `step.run fadeOut`, `history.list`, `history.undo`);
  - end-to-end **MCP stdio** (client `mcp` 1.x ↔ `python -m spectrocraft_mcp` ↔ real app,
    `SPECTROCRAFT_LAUNCH=auto`): `sc_launch`, `sc_open`, `sc_run_step`, `sc_history`, 61 tools listed
    including the dynamic ones (`sc_view_*`, `sc_markers_*`), `sc_markers_add`, `sc_undo`,
    `sc_run app.quit {confirm}`: **OK**.
