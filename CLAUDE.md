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
ctest --test-dir build -j 8 --output-on-failure          # Linux: under xvfb-run -a, or configure
                                                         # with -DSC_TEST_XVFB_PER_SHARD=ON (one X server per shard, as CI)

# a subset of the embedded suite (substrings of the test name, comma-separated)
SC_TEST_FILTER=control build/src/SpectroCraft_artefacts/Debug/SpectroCraft --run-unit-tests
#   macOS: build/src/SpectroCraft_artefacts/Debug/SpectroCraft.app/Contents/MacOS/SpectroCraft

# MCP adapter (Python 3.11+, no C++ build)
pip install -e "tools/mcp[test]" && pytest tools/mcp/tests
SPECTROCRAFT_EXE=<exe> pytest tools/mcp/tests/test_real_exe.py   # against the real --json
SPECTROCRAFT_EXE=<exe> tools/mcp/smoke.sh /abs/file.wav          # end to end, app open (local)
python3 tools/acpp_e2e.py --exe <exe> [--backend metal]          # real audio.cpp CLI: install, super resolution, stems (~100 MB download)
```

- `-DSPECTRO_ENABLE_AI=OFF` for a light build (no OpenVINO / whisper.cpp).
- The C++ tests are in `tests/UnitTests.cpp` and `tests/regression/*.inc` (AI control:
  `tests/regression/control.inc`), run by `--run-unit-tests`; ctest splits them into shards.
- Debug builds route JUCE assertions through `juce::Logger` (`JUCE_LOG_ASSERTIONS`, src/CMakeLists.txt):
  the runner counts them per test (`ScAssertCounter` in tests/UnitTests.cpp) and lists the tests that
  triggered any at the end of the run. A test can require none: compare `scAssertCounter().count`
  before and after the step (the control fuzz and `testAppRegPasteMixAtEnd` do).
- Waits in tests: `scPumpUntil` / `scDeliverPostedMessages` default to 10 s and return as soon as the
  condition holds. To drive a `TextEditor`'s link to another field, prefer `setText (t, false)` plus a
  direct `onTextChange()` call over waiting for the posted change message (flaky on loaded runners).
- Scale form (per-file axes: units, offset, scale, order, visibility): the model is
  `include/AxisScale.h` + `src/AxisScale.cpp`, the dialog `include/ScaleForm.h` +
  `src/ScaleForm.cpp`, applied through `Application::setAxisEntries`; behaviour and
  limits: `docs/scale-form.md`, tests `tests/regression/axes.inc`.
- Sessions (issue #21, experimental; other WAV files on the open file's timeline): the model is
  `include/Session.h` + `src/Session.cpp`, the views' overlay and Move (V) drag
  `include/SessionOverlay.h`, wired in `Application::addSessionFilesAt` (drops on the tracks,
  `MainWindow::dropFilesOnTracks`) / `addSessionTrack` / `removeSessionTrack` / `moveSessionFile` /
  `removeSessionFile` / `openSessionOf` / `saveSessionFiles`; playback by track through
  `AudioEngine::setPlaybackRoutes` (set by `FileDoc::applySessionSaveRegion`). While a session is open the
  engine holds the composite and `AudioEngine::setSaveRegion` limits every save to the main
  file's region. Behaviour and limits: `docs/sessions.md`, tests `tests/regression/sessions.inc`.
- Model panel (issue #40; Window > Model, a tab next to Tables in the default workspace since #51): the sound model is
  `include/SoundModel.h` + `src/SoundModel.cpp` (analysis, synthesis, JSON; no UI dependency),
  kept in `FileDoc::soundModel` (metadata key `soundModel`), driven by `src/ApplicationModel.cpp`
  (`Application::analyseModel` / `compareModel` / `renderModelSelection` / `renderModelCsv` /
  `modelPick*`), shown by `include/ModelPanel.h`; the spectrogram guide is
  `SpectralView::drawModelPickGuide`. Behaviour: `docs/model-panel.md`, tests
  `tests/regression/model.inc`.
- Spectrogram Readability (issue #69; Spectral Controls, display only): `SpectralEngine::balanceLevels`
  evens the dB picture's levels per frequency row / time column before `renderToPixels` colours it
  (`Settings::readability`, `freqBalance`, `timeBalance`); tests `tests/regression/readability.inc`.
- Files changed on disk (issue #70): `include/FileWatch.h` + `src/FileWatch.cpp` (`DiskStamp`, the pure
  `diskStateFor`, `ScopedWrite` around every write of an open file by the app - `AudioEngine::saveFile`
  and `patchRiffChunks` have one: add one to any new writer - and the background `DiskWatcher`);
  `FileDoc::diskStamp` / `diskState`, `MainWindow::checkDiskStates` / `runDiskChoice`,
  `Application::reloadDoc`; tests `tests/regression/disk_changes.inc`.
- File Info panel (issue #67; closed by default, `DockManager::setClosedByDefault`): WAV text metadata in
  `include/RiffInfo.h` + `src/RiffInfo.cpp` (LIST/INFO + DISP payloads, `FileDoc::riffInfo`, written by
  `AudioEngine::saveWavWithMarkers` and `Application::saveActiveDocMeta`), basic Markdown in
  `include/Markdown.h`, the panel in `include/FileInfoPanel.h`; tests `tests/regression/file_info.inc`.
- Blind source separation (issue #68; Effects > Blind Source Separation, 2+ channels): the DSP is headless in
  `include/BlindSeparation.h` + `src/BlindSeparation.cpp` (`sc::bss::separate`: AuxIVA, ILRMA, cACGMM,
  FastMNMF2, blocks with order matching), the window `include/BlindSeparationWindow.h`; the result goes through
  `Application::applyStemSeparationResult` (undo label parameter). Tests `tests/regression/bss.inc`;
  timings `SC_BENCH=1 SC_TEST_FILTER=bench` in a Release build.
- Draggable separators (waveform/spectrogram, video picture/tracks, table
  list/preview plot) share one widget style: `include/SplitterStyle.h`
  (`sc::ui::kSplitterH`, `sc::ui::drawSplitterGrip`). Do not draw a private grip
  in a new splitter.
- Toolbar (issues #43, #50, #65; menu bar row after the menus by default, movable by its grip): buttons, vector icons, the calculator (`sc::toolbar::calc`), key
  helpers, the placement model (`sc::toolbar::Placement`, `placementAt`; laid out by `MainWindow::toolbarLayoutFor`)
  and the custom shortcuts' model in `include/MainToolbar.h` + `src/MainToolbar.cpp`;
  wired in `MainWindow` (`runToolbarCommand`, `runCustomShortcut`, `setCustomShortcut`,
  `shortcutUsedBy` - add a key handled in `MainWindow::keyPressed` to `mainWindowKeys()` there
  so a custom shortcut cannot take it). Test "toolbar: tools, shortcuts 1-9 (#43)".
- Undo and speed: a same-length edit records an in-place patch (`AudioEngine::recordInPlace`,
  `Patch::inPlace`: one version, swapped by `applyPatch`); `UndoManager::popUndo` / `popRedo`
  return the patch now on the other stack (apply that one, never a copy); the histories share one byte
  budget across documents (`UndoManager::globalByteBudget`, oldest step first). Loaded AI models are
  held by `sc::ai::ModelCache` (`include/AI/ModelCache.h`): one per kind, freed after 5 idle minutes.
  Long sample loops use `sc::par::forRange` (`include/Parallel.h`). Timings:
  `SC_BENCH=1 SC_TEST_FILTER=bench` (optional `SC_BENCH_MINUTES`), meaningful in a Release build.
- File dialogs: pass `sc::files::useNativeDialogs()` (`include/FileDialogs.h`) as every `juce::FileChooser`'s
  `useOSNativeDialogBox` (issue #71): the system's dialog, JUCE's where Windows cannot create it.
- Mobile app (issue #73; iOS / Android, JUCE): its own CMake project in `mobile/` (configure
  `mobile/`, not the root) that compiles the desktop's UI-free engine sources (`AudioEngine`,
  `SpectralEngine`, `Measures`, `FileDoc`, `FileMeta`, `AxisScale`, `TimeFormat`, ...) with a touch UI
  in `mobile/src`; Android wrapper `mobile/android` (Gradle), CI `.github/workflows/mobile.yml`, unit
  tests `mobile/tests/MobileTests.cpp` (`-DSC_MOBILE_APP=OFF` builds only them). Keep those engine
  files free of `Application` / view includes: the UI halves live in `AxisScaleApp.cpp` and the views.
  Phone or tablet layout (smaller side >= 600 points: two panel groups as in the desktop's left column)
  and the hardware keyboard's shortcuts: `mobile/src/ScreenLayout.{h,cpp}` (no components, tested),
  applied by `MainScreen::layoutPhone` / `layoutTablet`.
  Files written by the app: `videoTracks[].relFile` (resolved next to the WAV, `sc::resolveVideoTracks`)
  and `storedPlots` (carried by the desktop). Crash reports (Sentry) and statistics (PostHog) live in
  `mobile/src/Telemetry*`, compiled in only with keys (`SC_SENTRY_DSN`, `SC_POSTHOG_KEY`); never send
  file names, paths or device names in events or error texts. Details: `mobile/README.md`.
- Menu and label text must stay **7-bit ASCII**: `juce::String` treats a plain `const char*`
  literal as `CharPointer_ASCII` and asserts on bytes > 127, so a literal `…` or `×` shows up as
  a garbage glyph. Use `...` in menus, or `juce::String::fromUTF8 (u8"…")` when a real
  non-ASCII character is wanted.
- AI control code: `include/control/`, `src/control/` (command registry, `--json`, server);
  per-lot handoff notes: `docs/ai-control/progress/`.
- Auto-update (Windows, macOS, Linux; issue #37): `include/Updates.h` + `src/Updates.cpp` (GitHub
  lookup, signed manifest, download/stage, per-platform install folder and layout), `include/Ed25519.h`
  + `src/Ed25519.cpp` (signature), `updater/` (`spectrocraft-updater`, a plain C++20 console program
  built with the app and shipped inside the release archive), keys in `tools/updates/`, macOS / Linux
  archive payload `tools/make_release_payload.py`, manifest generator `tools/make_update_manifest.py`.
  Flow and release requirements: `docs/updates.md`; tests: `SC_TEST_FILTER=updates`.
