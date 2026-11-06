# SpectroCraft MCP adapter (Claude Code)

`tools/mcp` contains the MCP server that lets Claude Code (VS Code or terminal) drive an **open**
SpectroCraft: state, history, undo/redo, editing steps on a precise range, AI jobs,
measurements. Architecture and protocol: `REPORT.md` §5-§7. Usage instructions for Claude: `CLAUDE.md`
at the root.

```
Claude Code ──stdio MCP──▶ spectrocraft-mcp (Python) ──TCP 127.0.0.1:<port>, NDJSON──▶ SpectroCraft
                             reads <appData>/SpectroCraft/control/<pid>.json {port, token}
```

## Prerequisites

- **Python 3.11+** and the `mcp` SDK **1.x** (`mcp>=1.6,<2`; the 2.x series renamed FastMCP and is not
  supported yet). Nothing else.
- A SpectroCraft version that accepts `--control` / *Window > Allow AI control (localhost)*
  (control server, lot 1). The windowless `--json` mode (`sc_headless` tool) exists since lot 0.
- No firewall rule: everything goes through the local loopback `127.0.0.1` on an ephemeral port; nothing
  listens on the network.

## Installation

### `uv` variant (recommended)

`uv` creates and updates the environment by itself (`tools/mcp/.venv`, ignored by git).

```sh
# macOS / Linux
brew install uv            # or: curl -LsSf https://astral.sh/uv/install.sh | sh
cp .mcp.json.example .mcp.json
# edit SPECTROCRAFT_EXE in .mcp.json, then open the folder in VS Code / run `claude`
```

`.mcp.json` (local, ignored by git) — macOS values of the template:

```json
{"mcpServers":{"spectrocraft":{
  "command":"uv","args":["run","--directory","tools/mcp","spectrocraft-mcp"],
  "env":{"SPECTROCRAFT_EXE":"/Applications/SpectroCraft.app/Contents/MacOS/SpectroCraft",
         "SPECTROCRAFT_LAUNCH":"ask"}}}}
```

### `pip` variant

```sh
python3 -m venv .venv && . .venv/bin/activate      # Windows: py -3 -m venv .venv ; .venv\Scripts\activate
pip install -e tools/mcp                           # + "[test]" to run the tests
```

then, in `.mcp.json`, replace `command`/`args` with the interpreter of that environment:

```json
"command": "/path/to/repo/.venv/bin/python", "args": ["-m", "spectrocraft_mcp"]
```

(or `"command":"spectrocraft-mcp"` if the installed script is in Claude Code's `PATH`).

### Windows

```json
{"mcpServers":{"spectrocraft":{
  "command":"py","args":["-3","-m","spectrocraft_mcp"],
  "env":{"SPECTROCRAFT_EXE":"C:\\path\\to\\repo\\build\\src\\SpectroCraft_artefacts\\Release\\SpectroCraft.exe",
         "SPECTROCRAFT_LAUNCH":"ask"}}}}
```

after `py -3 -m pip install -e tools/mcp`. The `uv` variant also works as is (`uv` for
Windows). The actual path of the exe is `build\src\SpectroCraft_artefacts\<Config>\SpectroCraft.exe`
(report §7.4 omitted `src\`).

### Linux

Same as macOS; `SPECTROCRAFT_EXE` points to `build/src/SpectroCraft_artefacts/<Config>/SpectroCraft`.
Without a graphical session, only `sc_headless` (`--json` mode) works.

### Checking

```sh
python3 tools/mcp/sc_client.py --list        # detected instances (no dependencies)
python3 tools/mcp/sc_client.py app.state     # state of the open app
```

In Claude Code: `/mcp` must show `spectrocraft` connected; ask for "sc_status".

## Environment variables

| Variable | Role |
|---|---|
| `SPECTROCRAFT_EXE` | executable (or `.app` bundle) for `sc_launch` and `sc_headless` |
| `SPECTROCRAFT_LAUNCH` | `ask` (default: clear error that offers `sc_launch`), `auto` (launches the app on the first tool call), `never` (never: use `sc_headless`) |
| `SPECTROCRAFT_PID` | instance to drive when several are open (otherwise the most recent one) |
| `SPECTROCRAFT_JOB_WAIT_S` | maximum time a tool waits for a job (default 600 s; beyond that the tool returns, the job continues) |
| `SPECTROCRAFT_CONTROL_DIR` | folder of the discovery files (tests; default below) |
| `SPECTROCRAFT_MCP_LOG` | log level on stderr (`INFO`, `DEBUG`) |

Discovery files `<pid>.json`: macOS `~/Library/SpectroCraft/control`, Linux
`$XDG_CONFIG_HOME/SpectroCraft/control` (default `~/.config/…`), Windows `%APPDATA%\SpectroCraft\control`.

## Tools

Fixed tools (§7.3): `sc_status`, `sc_launch`, `sc_state`, `sc_history`, `sc_undo`, `sc_redo`,
`sc_jump`, `sc_run_step`, `sc_run`, `sc_open`, `sc_save`, `sc_close`, `sc_models`, `sc_install_model`,
`sc_devices`, `sc_noise_suppress`, `sc_transcribe`, `sc_measure_levels`, `sc_third_octave`,
`sc_spectrum`, `sc_export_spectrogram`, `sc_jobs`, `sc_cancel`, `sc_headless`.

**Dynamic** tools: one per command published by `app.describe` (except `session.*` and internal
commands), named `sc_<domain>_<command>` (`sc_selection_set`, `sc_step_run`…). Their `inputSchema` is
the command's `params` schema, unchanged, plus three optional properties added by
the adapter: `doc`, `timeoutMs`, `confirm`. They appear on first contact with the app (`tools/list_changed`
notification): a command added on the C++ side is available without touching the Python.

Resources: `spectrocraft://commands` (full describe) and `spectrocraft://journal/tail` (end of the
session journal, JSON Lines).

## Troubleshooting

| Symptom | Cause / solution |
|---|---|
| `control_disabled`: "SpectroCraft is open but its AI control server is off" | the app is open (the `SpectroCraft.FileForward` pipe answers) but control is off: *Window > Allow AI control (localhost)*, or restart the app with `--control`, or `sc_launch(force=true)` (second instance) |
| `no_instance` | no app: `sc_launch` (with `SPECTROCRAFT_EXE`), `SPECTROCRAFT_LAUNCH=auto`, or `sc_headless` |
| `launch_failed` "did not write its discovery file" | app version older than the control server (lot 1), or crash at startup: run `SpectroCraft --control` by hand |
| `unauthorized` | stale token (app restarted): `sc_status` rereads the files; three failures → 2 s pause on the app side |
| `timeout` | a modal dialog box is blocking the app's thread: close it |
| `busy` | a modifying job or a progress dialog is active: `sc_jobs`, wait or `sc_cancel` |
| `protocol_mismatch` | app and adapter have different protocol versions: update one of the two |
| No `sc_<domain>_…` tools | normal as long as no controllable app has been contacted; call `sc_status` |
| `ModuleNotFoundError: mcp.server.fastmcp` | `mcp` 2.x installed by hand: `pip install "mcp<2"` (the `pyproject` already enforces it) |
| Firewall | no setting needed: loopback only, ephemeral port |

Orphaned `<pid>.json` files (crash): deleted automatically by the adapter (dead PID, or PID
reused by a process more recent than `startedAt`).

## Tests

```sh
pip install -e "tools/mcp[test]"
pytest tools/mcp/tests                                   # fake NDJSON server + fake exe, ~10 s
SPECTROCRAFT_EXE=<exe> pytest tools/mcp/tests/test_real_exe.py   # real --json mode
SPECTROCRAFT_EXE=<exe> tools/mcp/smoke.sh /abs/file.wav          # app open (--control), local
```
