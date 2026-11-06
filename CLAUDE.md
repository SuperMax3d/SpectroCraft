# SpectroCraft — instructions for Claude Code

C++20 / JUCE 9 audio/spectral editor. This file explains how to **drive the app** with the `spectrocraft`
MCP tools and how to **build / test** the repository. Protocol reference:
`docs/ai-control/REPORT.md` (§6), adapter installation: `docs/ai-control/mcp.md`.

## Driving SpectroCraft (`spectrocraft` MCP tools)

The adapter (`tools/mcp`, declared in `.mcp.json`, copied from `.mcp.json.example`) talks to the **open**
app over loopback TCP. Nothing goes through the graphical interface: the screen updates live.

Starting a session:

1. `sc_status`: detected instances, version, open documents. If there are none, the message says
   why: app closed (offer `sc_launch`) or app open **without** control (ask the user to
   enable *Window > Allow AI control (localhost)*, or `sc_launch(force=true)` for a second instance).
2. `sc_state`: documents (`docId`), active document, selection, view, transport, running jobs.

Vocabulary:

- **docId**: **stable** integer identifier of a document (`sc_state`, `doc.list`). The `doc` parameter of
  every tool accepts a docId or an absolute path; absent = active document. Never use a list
  index. Acting on a non-active document makes it active for the duration of the command (`docSwitched:true`),
  except `view.*` and `transport.*` (what the window shows or plays): `invalid_params`, call
  `doc.activate` first. A job (AI, Python) started on a non-active document makes it active
  (`activated:true`). Paths: absolute, under the home folder, the scripts folder or the temporary
  folder, otherwise `confirm_required {reason:"outside_roots"}`.
- **select**: range of a step, `"all"` or `{start, end}` in **seconds**. Without `select`, the app's current
  selection is used. `sc_run_step(type, params, start, end, channels)` builds it.
- **channels**: **0-based** channels (`[0]` = first channel). Absent = visible channels.
- **Units**: seconds, Hz, dB. Levels always carry their unit (`dBFS`, or `dB-SPL` when the
  document is calibrated): copy it as is, never guess it.
- **Steps** (`step.run`): the 20 action types — `silence, fadeIn, fadeOut, normalize, reverse, invert,
  changeGain {db}, timeStretch, catTimeScale, filterEq, envelope, resample, delete, trim, genSilence,
  genNoise, genTone, noiseSuppress, transcribe, python`. Exact parameters: the
  `spectrocraft://commands` resource or `step.validate`.
- **Jobs**: long operations (model installation, noise suppression, transcription, large files)
  return `{"job":"j-N"}`; the tools wait for completion while relaying progress (`wait=false` to
  return immediately). `sc_jobs(job)` to follow, `sc_cancel(job)` to cancel. Only one modifying job at a
  time: otherwise a `busy` error.
- **confirm**: destructive operations (overwriting a file, closing an unsaved document, path outside
  the allowed roots) first fail with `confirm_required`. **Ask the user**, then
  call the same tool again with `confirm=true`. Never pass `confirm=true` on your own.
- **History**: every edit can be undone (`sc_undo`, `sc_redo`, `sc_jump(state)`);
  `sc_history` shows the states (opId, params, source `api:…` or `menu`). The first edit after going
  back empties the redo stack (`redoDiscarded` in the response): report it.
- A response may contain `eventsSinceLastCall`: edits made in the meantime by the user (mouse,
  `source:"menu"`) or by another session. Take them into account before continuing.

Tools: `sc_status`, `sc_launch`, `sc_state`, `sc_history`, `sc_undo`, `sc_redo`, `sc_jump`,
`sc_run_step`, `sc_run(cmd, params)` (any command), `sc_open`, `sc_save`, `sc_close`, `sc_models`,
`sc_install_model`, `sc_devices`, `sc_noise_suppress`, `sc_transcribe`, `sc_measure_levels`,
`sc_third_octave`, `sc_spectrum`, `sc_export_spectrogram`, `sc_jobs`, `sc_cancel`, `sc_headless`, plus one
**generated** tool per app command (`sc_<domain>_<command>`, e.g. `sc_selection_set`,
`sc_effect_changeGain`) whose schema comes from `app.describe`. A command not yet shipped by the
app version answers `unknown_cmd`: say so, do not work around it.

Errors: the error text gives the code (`invalid_params`, `no_document`, `busy`, `timeout`,
`needs_ui`, `unsupported_build`, …), the app's hint and what to do. `timeout` almost
always means a modal dialog box is open in the app: ask the user to close it.

Without an open app: `sc_headless([{cmd, params}, …])` runs `SpectroCraft --json -` (no window,
no `session.hello`, jobs awaited inline). In a terminal, without the `mcp` package:
`python3 tools/mcp/sc_client.py app.state` (diagnostics, see `--help`).

## Building and testing the repository

```sh
# Debug, Ninja (macOS / Linux); Windows: same thing in an "x64 Native Tools Command Prompt"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSPECTRO_ENABLE_AI=ON
cmake --build build --target SpectroCraft
ctest --test-dir build -j 8 --output-on-failure          # Linux: under xvfb-run -a

# a subset of the embedded suite (substrings of the test name, comma-separated)
SC_TEST_FILTER=control build/src/SpectroCraft_artefacts/Debug/SpectroCraft --run-unit-tests
#   macOS: build/src/SpectroCraft_artefacts/Debug/SpectroCraft.app/Contents/MacOS/SpectroCraft

# MCP adapter (Python 3.11+, no C++ build)
pip install -e "tools/mcp[test]" && pytest tools/mcp/tests
SPECTROCRAFT_EXE=<exe> pytest tools/mcp/tests/test_real_exe.py   # against the real --json
SPECTROCRAFT_EXE=<exe> tools/mcp/smoke.sh /abs/file.wav          # end to end, app open (local)
```

- `-DSPECTRO_ENABLE_AI=OFF` for a light build (no OpenVINO / whisper.cpp).
- The C++ tests are in `tests/UnitTests.cpp` and `tests/regression/*.inc` (AI control:
  `tests/regression/control.inc`), run by `--run-unit-tests`; ctest splits them into shards.
- AI control code: `include/control/`, `src/control/` (command registry, `--json`, server);
  per-lot handoff notes: `docs/ai-control/progress/`.
