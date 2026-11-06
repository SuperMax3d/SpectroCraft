# SpectroCraft — Audit phase 9: AI control (VS Code + Claude Code)

Date: 2026-11-05
Audited repository: `SpectroCraft` (C++20 / JUCE 9, ~46,800 lines in `src/`), read-only, no build.
Addressee: Nicolas (can be passed on to Max, maintainer).
Convention: every claim about the code is cited as `file:line`; line numbers are those of the repository as of 2026-11-05.

---

## 1. Executive summary

- **Verdict**: SpectroCraft can already be driven without its interface (268 occurrences of `Application app` in `tests/UnitTests.cpp`, CLI `--run-action` on a headless `Application` at `src/Main.cpp:301`), and the "JSON in" dispatcher exists (`sc::applyActionStep`, `src/Actions.cpp:412`). But **none** of the five needs is covered as is: no flag writes JSON, the only IPC to the open app is a one-way "open these files" pipe (`src/SingleInstance.cpp:38-75`), the history is only an undo stack with no timestamps or parameters (`include/UndoPatch.h:53-105`), the measurements are locked inside a `juce::Component` (`src/Panels.cpp:1825`) and the device choice is largely fictitious.
- **Chosen architecture** (unanimous among the three judges): a **single command registry** (`sc::control::CommandRegistry`) consumed by four facades — a loopback NDJSON TCP server in the open app, a `--json` CLI mode, action replay, menus — plus a **thin Python MCP adapter** (outside the app) that generates its tools from `app.describe`.
- **First work package ("lot")** (2.5 d): minimal registry + generic `step.run` over the 20 existing step types + `--json` mode + session journal + fix for three confirmed replay defects (`envelope` not replayable, device `"auto"` not resolved, `noiseSuppress` → `normalize` in a non-AI build). Until lot 5, this journal only sees edits going through `recordActionStep` and API commands (see §5.2 Journal).
- **Control of the open app** from VS Code: reached at the end of lot 2 (~9 days).
- **Total effort**: 8 lots, **23 nominal days, range 23-30 d** (assumptions and margin in §8); lots 3-7 are independent or can be deferred without undoing the previous ones. Coverage of requirement (2) can be checked MenuID by MenuID in appendix §11.

---

## 2. What the software can already do for external control

### 2.1 Table of entry points

| Entry point | File:line | Headless? | Thread | Observations |
|---|---|---|---|---|
| CLI parser `initialiseInternal` | `src/Main.cpp:148` | yes | message (in `initialise()`) | a single flag per process; unknown flags silently ignored and the UI starts (`:816`); home-made tokenisation that strips every quote (`:158-163`, **all platforms**, no `#if`); on POSIX, JUCE rebuilds `commandLine` upstream from `argv`: inline JSON is impractical everywhere |
| `--test-ns-failure-alert`, `--generate-test-files` | `src/Main.cpp:646`, `:807` | yes | synchronous | test utilities (NS failure alert, fixtures); free text; not detailed here |
| `--run-action <name> <in> <out>` | `src/Main.cpp:246` | yes | message, synchronous | only existing JSON input; action looked up **by name** in `SpectroCraft.recent` (`:268-273`); temporary store (`:295-301`); codes 0/1/2 |
| `--batch <name> <inDir> <outDir>` | `src/Main.cpp:246` → `src/Actions.cpp:649` | yes | synchronous | text summary; per-file failures invisible in the return code |
| `--test-spectral`, `--test-load` | `src/Main.cpp:197`, `:354` | yes | synchronous | free text, code 0 even on failure (`:220-222`) |
| `--test-noise-suppression` | `src/Main.cpp:385` | yes | synchronous | device hard-wired to `"CPU"` (`:436-438`); code 0 on failure (`:443-445`) |
| `--test-transcribe` | `src/Main.cpp:453` | yes | synchronous | downloads the model (`:501-502`); channel 0 only (`:518`); result printed, not persisted |
| `--test-batch-transcribe` | `src/Main.cpp:533` | yes | synchronous | `Application app;` **without an isolated store** (`:608`): writes the real `.recent` |
| `--list-ai-devices` | `src/Main.cpp:760` | yes | synchronous | free text |
| `--run-unit-tests` | `src/Main.cpp:784` | yes | synchronous | proof that the whole app runs without a window |
| Bare arguments (files) | `src/Main.cpp:816` | no | message | only command that reaches an already open app |
| `sc::singleinstance::Coordinator` (pipe server) | `src/SingleInstance.cpp:80` | yes | `juce::Thread` "SC-FileForward" → `MessageManager::callAsync` (`:142`) | hosted by **every** instance (`src/Main.cpp:840-845`); 1-byte ack **before** execution (`:136-137`) |
| `sc::singleinstance::forwardFiles` (client) | `src/SingleInstance.cpp:167` | yes | blocking 2 s | native int32, no magic or version (`:38-41`, `:63-75`) |
| `Application` (constructor) | `include/Application.h:38`, `src/Application.cpp:84` | yes | message (it is a `juce::Timer`) | `useIsolatedSettingsStore` (`Application.h:45`) |
| `sc::applyActionStep(app, step, &err)` | `src/Actions.cpp:412` | yes | synchronous, calling thread | 20 types; requires a loaded file (`:422`); target = `app.selection`, **not** the JSON |
| `sc::validateStepParams` | `src/Actions.cpp:346` | yes | none | a single caller in the app: `src/Panels.cpp:11759` |
| `Application::recordActionStep` | `src/Application.cpp:353` | yes | message | 27 call sites in `src/MainWindow.cpp`; no-op if `actionRec.active` is false (`:355`) |
| `Application::undoSteps/redoSteps` | `src/Application.cpp:4175` | yes | message | stack **cleared** on `applyPatch` failure (`:4192`, `:4224`) + asynchronous alert (`:4170`) |
| `UndoManager::undoAt/redoAt` | `include/UndoManager.h:29-33` | yes | message (unsynchronised deque) | "read-only enumeration for the History tab" |
| `HistoryPanel::refresh` | `src/Panels.cpp:909-930` | instantiable without a window | message | text "Loaded <name>" + description of each Patch |
| `Application::noiseSuppress` | `src/Application.cpp:3491` | yes | **blocking, worker** | `copyNoise` commit via `callAsync`+`WaitableEvent` (`:3603-3613`); default branch commits **from the worker** (`:3560-3566`) |
| `Application::transcribe` | `src/Application.cpp:3901` | yes | blocking, worker | `d->setTranscription` **from the worker** (`:3951`) |
| `Application::batchTranscribeFolder` | `src/Application.cpp:4006` | yes | blocking, worker | only structured result in the repository (`BatchTranscribeSummary`, `Application.h:24-31`) |
| `Application::runPythonScript` | `src/Application.cpp:3629` | yes | blocking, worker | hop to the message thread (`:3876-3886`); output = WAV only (`:3741-3755`) |
| `sc::ai::ModelManager` (registry/find/isInstalled/ensureInstalled) | `src/AI/ModelManager.cpp:241`, `:319` | yes | `ensureInstalled` blocking, worker | 15 models with URL + SHA-256 + size (`:71-133`); progress/cancellation by callback |
| `sc::acpp::run*` (audio.cpp) | `src/AudioCpp.cpp:650` | yes | blocking, worker | **Windows only** (POSIX stubs `:713-749`, download `:367-376`) |
| `StemSeparationWindow::runWorker` / `SuperResolutionWindow::runWorker` | `src/StemSeparation.cpp:1020`, `src/SuperResolution.cpp:550` | **no** | window + detached thread + modal dialog | all the orchestration is in the lambdas |
| `FFTAnalysisView::thirdOctBandsRef` (static) | `src/Panels.cpp:648` (declaration `include/Panels.h:622`) | yes | none (expensive: FFT 65536 × ≤2048 frames) | only spectral measurement fully separated from the UI; dB-SPL correction (+98.5 dB `:747`, ref. 2e-5 Pa `:752`) |
| `FFTAnalysisView::computeRegionFrom` | `src/Panels.cpp:1659` | no | message | parameterised except for the window type read from `windowCombo` (`:1694`) |
| `SpectralEngine::computeTile / probeMagnitude / renderToPixels` | `src/SpectralEngine.cpp:207`, `:460`, `:491` | yes | internal pool | dB per frame/bin available; only existing output = PNG |
| `sc::serialiseBundle / parseBundle` | `src/FileMeta.cpp:262`, `:394` | yes | none | the complete `.meta.json` |
| `FileDoc::buildMetaJson` | `include/FileDoc.h:173` | yes | message | JSON serialiser of the document (markers, selections, drawings, calibration, tables, transcription) |
| `scttToJson` | `src/ScttChunk.cpp:57` | yes | none | transcription segments |
| `MarkerManager::saveCsv/loadCsv` | `src/MarkerManager.cpp:745`, `:757` | yes | none | — |
| "Worker → commit on message thread" pattern | `src/Application.cpp:3603-3613` | — | — | `callAsync` + `WaitableEvent`, `wait(-1)` safe only because the message thread never waits for the worker (`:3608-3609`) |
| Test harness: `scPumpUntil`, `ScModalWatchdog`, `setHeadless` | `tests/UnitTests.cpp:207-216`, `:27082`, `:27118` | yes | test thread = message thread (`ScopedJuceInitialiser_GUI`, `:2482`) | `testSingleInstance` template (`:25849-25922`) for testing an in-process IPC server |

### 2.2 Coverage inventory

| Subsystem | Controllable today | Detail |
|---|---|---|
| **CLI flags** | 11 main flags + 5 sub-options, **0 in JSON** | main: `--batch`, `--generate-test-files`, `--list-ai-devices`, `--run-action`, `--run-unit-tests`, `--test-batch-transcribe`, `--test-load`, `--test-noise-suppression`, `--test-ns-failure-alert`, `--test-spectral`, `--test-transcribe`; sub-options: `--subfolders` (of `--batch`, `src/Main.cpp:338`), `--translate`, `--no-auto-download`, `--meta-mode`, `--channel` (of `--test-transcribe`/`--test-batch-transcribe`, `:571-589`). Only 3 set a reliable return code (`--run-action`, `--batch`, `--run-unit-tests`); 6 `--test-*` flags return 0 even on failure |
| **Action step types** | 20 types (`include/Actions.h:77-96`); **19 executable** headless | `envelope` validated (`src/Actions.cpp:389`) and recorded (`src/MainWindow.cpp:5481`) but no `case 'e'` in `applyActionStep` (cases s, f, n, c, r, i, t, d, g, p at `:427-565`) |
| **MenuIDs** | 91 functional identifiers (93 enumerators `include/MainWindow.h:485-547` minus the sentinels `RecNoDeviceItem` `:513` and `RecDeviceEnd` `:546`), 90 handled by `menuItemSelected` (`src/MainWindow.cpp:2773-3139`), 1 dead (`ViewColourMap`, `:503`) | 18 covered by an action step (20 %); ~50 others reachable through a public `Application` method without UI (≈75 % cumulative); the rest requires `MainWindow`: vertical zoom, panels, Stem/SuperRes, **save** (`FileSave/SaveAs` → `runSaveAsync`, `src/MainWindow.cpp:2318`), **PNG export with overlays** (`preparePngExport`, `:2536`). **Complete MenuID → JSON command → lot table: appendix §11** (24 in lot 0, 23 in lot 1, 5 in lot 4, 36 in lot 6, 2 in lot 7, 1 dead) |
| **AI models** | 15 in `ModelManager` (3 NS OpenVINO + 12 Whisper), all list/install/run headless | + 4 audio.cpp binaries, 9 SAM-Audio GGUF, FlashSR, AudioSR: catalogues with no stable id or SHA-256 (`src/AudioCpp.cpp:101-166`, `validateDownload` by size `:205-223`), Windows only; htdemucs not downloadable (`:53-75`) |
| **Devices / backends** | 3 vocabularies | OpenVINO `auto/CPU/GPU/NPU` but only the CPU plugin is shipped (`extern/OpenVINO/README.md:5-6`); Whisper `use_gpu=false` (`src/AI/WhisperTranscribe.cpp:91`); audio.cpp `cpu/cuda/vulkan` real, with a mere warning if inconsistent with `variant.txt` (`src/StemSeparation.cpp:497-503`) |
| **Computed measurements** | **0 exportable as data** | spectrum, 1/3 octave, curves, harmonics: clipboard TSV limited to the visible window and 4 channels (`src/Panels.cpp:4676`, `:4819`, `:4842`); no reusable RMS/peak (local `static` RMS at `src/MainWindow.cpp:4358`, peak inline in `normalize` `src/Application.cpp:2829`) |
| **Annotations** | ~90 % JSON-ready | markers, named selections, drawings, tables (with values), calibration, transcription via `serialiseBundle` / `scttToJson` / `serialiseTables` (`src/Tables.cpp:665`) |
| **History** | per-document undo stack, 64 levels (`include/UndoManager.h:10`) | `Patch` = region + full audio copies + free-form `description` string (`include/UndoPatch.h:74`); no timestamp, id, type or params; no notification (`include/UndoManager.h:7-59`) |
| **Settings** | 79 keys in a single `PropertiesFile` "SpectroCraft.recent" | `millisecondsBeforeSaving = -1`, never re-read, rewritten in full by every `save()` (`src/Application.cpp:106-123`, `:350`, `:367`, `:380`) |

---

## 3. Verified findings

Each row was re-read in the code by an adversarial verifier distinct from the initial reader. "Partial" = the claim holds but required a correction that changes the design.

| # | Claim | Verdict | Evidence | Consequence for the design |
|---|---|---|---|---|
| 1 | The single-instance pipe carries only paths and replies with a 0x01 byte even before opening the files | **confirmed** | `src/SingleInstance.cpp:38-41`, `:63-75`, `:136-137` | a new channel is mandatory; the pipe stays reserved for "open files" |
| 2 | No CLI flag emits JSON; the `--test-*` flags exit without `setApplicationReturnValue` | **confirmed** | `src/Main.cpp:220-222`, `:443-445`, `:483-486`, `:520-524`, `:630-634` | new `--json` flag with an output contract, rather than adapting the 11 existing flags |
| 3 | Every instance hosts the pipe server, `multi_instance` defaults to 1 | **confirmed** | server in every instance: `src/Main.cpp:840-845`, `src/SingleInstance.cpp:92` (`mustNotExist=false`); JUCE does not enforce a single instance: `moreThanOneInstanceAllowed() → true` (`src/Main.cpp:133`); default 1 of the **setting**: `getIntValue (kSettingKey, 1)` (`src/SingleInstance.cpp:32`), key `multi_instance` (`include/SingleInstance.h:18`); at 0, `handleStartup` (`:191-213`) forwards and the process exits (`src/Main.cpp:825-832`) | per-instance identity: ephemeral port + `<pid>.json` discovery file; `--control` must bypass `handleStartup` (§7.5) |
| 4 | `applyActionStep` is pure, synchronous, UI-free | **partial** | no `case 'e'` (`src/Actions.cpp:427-565`); `noiseSuppress` → `normalize` outside AI (`:436-445`); literal `"auto"` (`:441`); dispatch on the 1st character of the type ("sxxx" → silence); can launch Python and download models | complete and harden the dispatcher, not just wrap it; always call `validateStepParams` first |
| 5 | `--run-action` accepts only an action name from the user store | **confirmed** | `src/Main.cpp:268-291` | inline/stdin JSON input is the first lock to remove |
| 6 | 91 MenuIDs; 18 by step, 54 by `Application`, 19 pure UI | **partial** | 90 handled (`src/MainWindow.cpp:2773-3139`); `FileSave/SaveAs` and `FileExportSpectral` have **no** public equivalent in `Application` (`src/MainWindow.cpp:2316-2340`, `:2536`); horizontal zoom can be controlled via `viewStart/viewEnd` + `ensureTimeRangeVisible` (`include/Application.h:120`, `:483`) | two or three extractions of logic out of `MainWindow` (save, PNG export) |
| 7 | A single history recording point: `recordReplace` | **confirmed** | `src/AudioEngine.cpp:843-871` (only call to `UndoManager::record`) | timestamp/id/opId are added in a single place |
| 8 | `Patch` has only a free-form string as meta-information | **partial** | `include/UndoPatch.h:53-105` exact; **but** `recordActionStep` already captures type + JSON params of the same edits (`src/Application.cpp:353-362`, e.g. `{"db":6.0}` at `src/MainWindow.cpp:3808`), opt-in only | generalise this mechanism (always on, timestamped, linked to the Patch) rather than starting from scratch |
| 9 | `UndoManager` has no notification; History panel refreshed by polling | **partial** | no notification: exact; but it is **not** polling: event-driven refresh driven by `MainWindow` (`refreshAfterEdit` `:3006-3007`, `onStateChange` → `syncSharedState` `:388`); the 30 Hz timer (`:504`) does not touch the panels | an undo launched outside the UI (CLI, MCP) produces no signal: add `onChanged` in `record/popUndo/popRedo/clear` |
| 10 | History is cleared by load, resample, undo failure, closeDoc; markers alone never enter it | **confirmed** | `src/FileDoc.cpp:173`; `src/AudioEngine.cpp:1817`; `src/Application.cpp:4192`, `:4224`, `:857`, `:1961` | separate append-only journal, which also records the clears and their reason |
| 11 | Stems and super-resolution have no headless function | **partial** | orchestration in `runWorker` (`src/StemSeparation.cpp:1055-1280`, `src/SuperResolution.cpp:579-673`); but both ends are reusable: `sc::acpp::run*` (`src/AudioCpp.cpp:650-711`) and `createDocFromMerged/applyStemSeparationResult` (`src/Application.cpp:2296`, `:2347`, `:2475`, tested without UI `tests/UnitTests.cpp:6821-6931`) | extract only the "middle" of the worker; no macOS/Linux audio.cpp binary is referenced (`binaryChoices` `:127-144`, `binaryPath` `:25-28`) |
| 12 | Two incompatible model registries | **confirmed** | `src/AI/ModelManager.cpp:71-133` vs `src/AudioCpp.cpp:101-166` | unified view `{id, engine, family, installed, checksum, platforms}` |
| 13 | Denoising and transcription controllable without UI but without JSON | **partial** | `--test-noise-suppression/--test-transcribe` bypass `Application` (`src/Main.cpp:385-532`); action steps pass `nullptr` as progress (`src/Actions.cpp:438-489`); **the JSON already exists**: `scttToJson` writes into the SCTT chunk or the sidecar (`src/Application.cpp:4131`, `:4137`) | the work = exposing this JSON, not creating it |
| 14 | The device choice is fictitious | **partial** | fictitious for OpenVINO (only CPU shipped) and Whisper (`use_gpu=false`); **real** for audio.cpp (`--backend` passed to the CLI); for actions, `"auto"` goes as is into `ov::Core::compile_model` (`src/AI/OvRuntime.cpp:169`) whereas neither a device named "auto" nor the AUTO meta-plugin is shipped → the `noiseSuppress` step under `--run-action` should fail (not proven by execution; no test replays this step, `tests/UnitTests.cpp:26681`) | resolve through `sc::resolveAiDevice` (`src/AI/OvRuntime.cpp:24`, only real call `src/MainWindow.cpp:6276`); publish the real devices per engine |
| 15 | `Application` edits do not notify the UI and do not record an action | **partial** | true for **destructive audio edits** (silence, gain, delete, normalize…): neither `onStateChange` nor `refreshAfterEdit`; only `applySuperResolutionResult` notifies (`:2593`). **False in general**: `onStateChange` has 14 call sites in `src/Application.cpp` — `closeDoc` (`:866`), `startRecording`/`stopRecording` (`:1129`, `:1194`), `timerCallback` (`:1256`), `setSelection` (`:1296`), `clearSpectralSelection` (`:1329`), `refreshSpectralSelectionUI` (`:1359`), `setShowAllDrawings` (`:1521`), clone pick (`:1669`, `:1681`, `:1714`), selection gestures (`:1784`, `:1799`). `recordActionStep` called only from `MainWindow.cpp`; the side-state hooks are fired (`onTimeRangeDeleted/Inserted/Stretched`, `onPatchSideStateRestored`, `:2717`, `:2047`, `:2952`, `:4289`); only `refreshAfterEdit` (`src/MainWindow.cpp:724-747`) purges the tile cache | `refreshAfterEdit` hook via `UiBridge` after each mutating **audio** command; `selection.set`, `doc.close`, `record.*` do not need it, `onStateChange` is enough |
| 16 | The "worker → commit on message thread" pattern is used by `noiseSuppress` and `runPythonScript` | **partial** | in `noiseSuppress` it covers only the `copyNoise` branch (`:3603-3613`); the default branch commits from the worker (`:3560-3566`), as does `transcribe` (`:3951`); only `runPythonScript` applies it to its whole commit (`:3876-3888`) | align these two paths before exposing `ai.*` as jobs |
| 17 | `AlertGate` is a global flag enabled only by the tests | **partial** | exact (`include/AlertGate.h:20-33`, `tests/UnitTests.cpp:27118`); but `AlertGate` covers only its two wrappers: **60 raw `runModalLoop`** (32 in `src/MainWindow.cpp`, 28 in `src/Panels.cpp`) and synchronous `FileChooser`s (`src/MainWindow.cpp:2304`, `:2423`, `:6659`) bypass it; the test harness has to be backed by a `ScModalWatchdog` (`:27082-27105`). On the other hand, the two `Application` alerts (`:1187` `sc::alert::showMessageBox`, `:4170` `sc::alert::showMessageBoxAsync`) **do go** through the `AlertGate` wrappers (`include/AlertGate.h:63`, `:83-91`): swallowed in headless mode, blocking only in GUI without capture | a command must never reach the raw paths; alert capture **by scope** (`ScopedCapture`), not by global flag — which is precisely what `:1187`/`:4170` justify |
| 18 | `computeRegionFrom` reads the window type from the ComboBox | **confirmed** | `src/Panels.cpp:1694` | one-line signature change to make it pure |
| 19 | `thirdOctBandsRef` is static and pure | **confirmed** | `src/Panels.cpp:648-754`; tests `tests/UnitTests.cpp:2688-2775` (≈ 91 dB-SPL for a full-scale sine at 1 Pa) | reusable as is; the tests serve as the equality reference |
| 20 | `doRecompute` requires `getWidth()>0`, takes the region from the UI selection and throttles at 60 ms | **partial** | exact, but the throttle applies only when four conditions are met together (existing cache, region > one window, expensive previous computation, < 60 ms; `src/Panels.cpp:1915-1917`); `computeTimeCurveFrom/computeSpectrumCurveFrom` already exist, parameterised but private (`include/Panels.h:796`, `:803`) | the "analyseRegion" API is mostly about exposing and decoupling, not creating |
| 21 | No JSON output for a computed measurement | **partial** | exact for spectrum/bands/curves/harmonics; but the repository produces JSON for actions (`src/Actions.cpp:115`), Python meta (`src/PythonScripts.cpp:620`), transcription, tables; and `getThirdOctMags()` / `getLiveCurves()` (`include/Panels.h:421`, `:442`) already expose the raw data without the clipboard | the result schema remains to be defined; the data is accessible |
| 22 | The Python bridge only reads back the output WAV; stdout is discarded on success | **confirmed** | `src/Application.cpp:3741-3755` | add an optional `result.json` for markers/measurements |
| 23 | Subprocesses inherit stdin | **confirmed** | `src/PythonScripts.cpp:752`, `:859-860`; `src/AudioCpp.cpp:564` | **rules out a stdio MCP transport inside the app** |
| 24 | `runProcess` has no timeout; in action replay `onProgress` is `nullptr` | **confirmed** | `src/PythonScripts.cpp:800`; `src/Actions.cpp:579`; `src/Application.cpp:3640` | timeout enforced on the dispatcher side |
| 25 | audio.cpp is Windows-only (execution and download) | **confirmed** | `src/AudioCpp.cpp:713-749`, `:367-376` | report `unsupported_platform`; not testable on Nicolas's Mac |
| 26 | All persistence lives in a single `PropertiesFile` that is never re-read | **partial** | exact for the 79 settings keys; Python scripts, models, OpenVINO cache, `.meta.json` sidecars are separate files re-read from disk | only `.recent` requires an in-process command; the rest can be written by a third party |
| 27 | `Application` can be driven without `MainWindow` | **partial** | exact; but a JSON mode must arm `setHeadless`, have a `MessageManager`, set `SC_NO_AUDIO_DEVICE`, and `--run-action` does **not** call `validateStepParams` | this missing wiring has to be added, not "reused" |
| 28 | The FileForward pipe is the only IPC to the open app | **partial** | only **functional** IPC; on macOS JUCE routes `application:openFiles:` to `anotherInstanceStarted`, overridden with an empty body (`src/Main.cpp:897`) | no consequence for the design |

**Refutations raised by the judges against the candidate architectures** (all verified in the code, all incorporated into §5):

- `StreamingSocket::close()` unblocks `accept()` by connecting to the `portNumber` member, which stays at **0** after `createListener(0, …)` (`extern/JUCE/modules/juce_core/network/juce_Socket.cpp:157-158`, `:613`): on Linux the server thread does not stop cleanly → self-connect to `getBoundPort()` before `close()`.
- `userApplicationDataDirectory` = `~/Library` on macOS (`juce_Files_mac.mm:210`), not `~/Library/Application Support`; consistent with `scriptFolder` (`src/PythonScripts.cpp:519-523`) and `modelRoot` (`src/AI/ModelManager.cpp:175-177`).
- `replaceSamples` calls `ensureEditable` itself (`src/AudioEngine.cpp:1391`): a "short" edit on a streamed file loads the whole file on the message thread; the UI avoids this through `runDestructiveEdit` on a worker (`src/MainWindow.cpp:1996-2040`).
- `closeDoc` does `docs.erase` (`src/Application.cpp:857-859`): addressing documents by index is wrong between two commands.
- `history.jumpTo` never clears the redo stack (`popUndo` pushes onto `redoStack`, `include/UndoManager.h:35-42`); it is the next edit that clears it (`record()`, `:13-18`).
- Switching `setHeadless(false)` after a command would disable the global headless mode set by the harness (`include/AlertGate.h:33`).
- In CLI mode, job commits go through `callAsync` (`src/Application.cpp:3610`): without a message pump, a job never finishes when the message thread is reading stdin.
- `FileDoc::toBundle()` does not exist; the equivalent is `FileDoc::buildMetaJson` (`include/FileDoc.h:173`).

---

## 4. Gaps and risks, by severity

### Blockers (none of the five needs can be met without removing them)

| Gap | Evidence | Need affected |
|---|---|---|
| No JSON output, misleading return codes | `src/Main.cpp:241`, `:443` | (4) |
| No request/response channel to the open app; one-way pipe, unversioned, fixed shared name | `src/SingleInstance.cpp:38`, `:92`, `:134` | (5) |
| No external JSON input (action name only) | `src/Main.cpp:272` | (2), (4) |
| Step vocabulary limited to destructive effects; implicit target (`app.selection`); no open/save/select/undo/marker/export | `src/Actions.cpp:425`; `selectAll` forced `src/Main.cpp:309` | (2) |
| No "app state → JSON" serialiser | `include/Application.h:53` | (4), (5) |
| **Destructive audio edits** in `Application` do not refresh the screen (`refreshAfterEdit` is in `MainWindow`; `onStateChange` is not called there, unlike selection/recording/closeDoc) | `src/Application.cpp:2724` (end of `del`), `src/MainWindow.cpp:724-747` | (5) |
| Spectrum computation buried in a `Component` that depends on the geometry and the UI selection | `src/Panels.cpp:1825-1834`, `:1694` | (4) |
| Stems/SR with no headless function; audio.cpp Windows-only | `src/StemSeparation.cpp:1020`, `src/AudioCpp.cpp:713`, `:367` | (3) |
| The settings store is never re-read: any external write is lost | `src/Application.cpp:109` | (2) |
| The Python script can only return audio | `src/Application.cpp:3741` | (4) |

### Important

- Return codes missing on six `--test-*` flags (`src/Main.cpp:443`); unknown flags ignored and the UI starts (`:816`); `--test-batch-transcribe` on the real store (`:608`); no device choice in the CLI (`:436`); home-made tokenisation that strips every quote (`:158-163`, **all platforms**) and, on POSIX, rebuilding of `commandLine` by JUCE from `argv`: inline JSON is impractical everywhere, hence `--json <file|->`.
- `envelope` not replayable (`src/Actions.cpp:425`); `noiseSuppress` → `normalize` in a non-AI build (`:436-445`); device `"auto"` not resolved (`:441`); inconsistent validation between `validateStepParams` and `applyActionStep` (`:346`, `:567-576`).
- Replay in the open app finished/failed through modal boxes (`src/MainWindow.cpp:2076`, `:2089`, `:2056`); `noiseSuppress` without prompt blocking on the message thread via the `runDestructiveEdit` fast path (`:2197`, `:2000-2007`).
- History: no timestamp or stable id (`include/UndoPatch.h:74`, `include/UndoManager.h:12`); no notification (`:7`); non-audio operations missing (`src/Application.cpp:1961`); silent clears (`src/AudioEngine.cpp:1817`); no on-disk journal (`include/StartupTrace.h:28` only traces startup).
- Remote commands never recorded in actions (`src/Application.cpp:353` called only by `MainWindow`).
- Alerts in `Application` via the `AlertGate` wrappers (`:1187` synchronous, `:4170` asynchronous: swallowed in headless mode, blocking in GUI without capture); global `AlertGate` (`include/AlertGate.h:33`); 60 raw `runModalLoop` and synchronous `FileChooser`s outside any guard.
- No lock on `docs` (`include/Application.h:864`: only mutex = audio init); `runDestructiveEdit` relies on the modal dialog to freeze the UI (`src/MainWindow.cpp:2021-2030`).
- No job model: detached threads + `WorkProgressDialog` as the only cancellation handle (`src/MainWindow.cpp:6255`, `:6539`).
- Two model registries; audio.cpp assets checked by size only (`src/AudioCpp.cpp:205`) — **security risk** if an AI installs executable binaries of 30 to 465 MB.
- AI results not returned in structured form (`src/Application.cpp:3951`).
- Zoom/view/panels/save/PNG export outside `Application` (`include/Application.h:120`, `src/MainWindow.cpp:2318`, `:2536`).
- Measurements: no reusable RMS/peak (`src/MainWindow.cpp:4358`); no peak detection ("harmonics" = multiples of a mouse anchor, `src/Panels.cpp:4188`); 1/3-octave helpers are file-scope `static` (`:773-803`); three level conventions (`include/Panels.h:634`, relative dB, offset dB-SPL) with per-channel full scale.
- Global pipe name + server in every instance (`include/SingleInstance.h:15`, `src/Main.cpp:840-844`).
- `runProcess` without timeout (`src/PythonScripts.cpp:800`); inherited stdin (`:752`); Python output in ANSI code page decoded as UTF-8 (`:815`).
- Without `MainWindow`, settings are never persisted automatically (`src/MainWindow.cpp:1878`); writing `app.settings` bypasses the clamps (`src/Application.cpp:168-242`).

### Minor

No POSIX instance probe + 2 s delay (`src/SingleInstance.cpp:206`, `:178-179`); FIFO surviving a crash (`juce_NamedPipe_posix.cpp:57-60`); `--test-transcribe` channel 0 not persisted (`src/Main.cpp:518`); stdout invisible for the Windows GUI exe (`src/CMakeLists.txt:2`); `resample` outside `Application` and outside undo (`src/Actions.cpp:464`); `ViewColourMap` dead (`include/MainWindow.h:503`); depth 64 (`include/UndoManager.h:10`); one action = N unnamed entries; `transcribe` mutates the doc from the worker (`src/Application.cpp:3951`); `openFile` without `errOut` (`:408`); `play()` blocking without timeout (`:4433`, overload `ensureAudioReady(int)` at `include/Application.h:778`); AI API conditional on `SC_ENABLE_AI` (`:680`, `:745`); FFT snapshots not persisted (`include/Panels.h:678`); spectrogram matrix with no data format (`src/SpectralEngine.cpp:207`); `SC_MODELS_DIR` ignored on Windows (`src/AI/ModelManager.cpp:157`); `buildNsFailureDetails` defined in the UI file (`src/MainWindow.cpp:6058`); audio.cpp progress = raw log lines (`src/AudioCpp.cpp:586`); `python` action records only the script path (`src/MainWindow.cpp:260`); 2^31-sample limit in the WAV exchange (`src/PythonScripts.cpp:637`); `findPython` on the message thread on every run (`src/MainWindow.cpp:6334`); ctest based on a stdout regex (`tests/CMakeLists.txt`).

---

## 5. Recommended architecture

### 5.1 Diagram

```
VS Code + Claude Code ──stdio MCP──▶ tools/mcp/spectrocraft_mcp (Python, FastMCP)
                                        │ reads <appData>/SpectroCraft/control/<pid>.json {port, token}
                                        │ TCP 127.0.0.1:<port>, NDJSON, 1st line = session.hello
                                        ▼
┌──────────────── SpectroCraft (GUI) ─────────────────────────────────────────────────────┐
│ ControlServer thread ─▶ ClientSession ──callAsync + PendingCall(timeout, abandoned)──▶  │
│                                              MESSAGE THREAD                             │
│   CommandRegistry::invoke  ◀── MainWindow menus ◀── applyActionStep ◀── --json (CLI)    │
│     flags → busy/modal → streamed? job : handler → recordActionStep → Journal → refresh │
│     LongRunning ─▶ JobManager (std::thread, 1 mutant max) ─progress─▶ events            │
│                      └─ commit ─▶ callAsync + WaitableEvent (Application.cpp:3602-3613) │
│   Application ─ FileDoc(id) ─ AudioEngine ─ UndoManager(onChanged) ─ ModelManager        │
│   Journal (ring + JSONL, seq) ─▶ events.subscribe {sinceSeq} with replay                │
└─────────────────────────────────────────────────────────────────────────────────────────┘
   same binary: SpectroCraft --json req.ndjson --json-out res.ndjson  (headless, pump)
   tests: testControlRegistry / testControlServer (port 0, ScModalWatchdog :27082)
```

### 5.2 Components (new folder `src/control/`, `include/control/`)

**CommandRegistry** — `Spec { id, title, category, paramsSchema, resultSchema, flags, aliases, handler }`, flags `Mutates | NeedsDoc | LongRunning | NeedsUi | Destructive | AiBuildOnly | WindowsOnly`. `invoke()` always does the following, on the message thread (assertion `JUCE_ASSERT_MESSAGE_THREAD`), in this order:

1. resolution of `doc` (stable id `FileDoc::id` or path → index, never the raw index). The legacy handlers (`applyActionStep`, `app.selection`, `app.audioEngine()`) act on the **active document** (`activeDocIndex` `include/Application.h:54`, accessors `:262-263`). Rule: if `doc` designates a non-active document, `invoke()` activates it for the duration of the handler (`activeDocIndex` + `syncActiveDoc()`, `Application.h:395`) then **restores** the previous active one and resynchronises; the UI sees a view round trip, flagged in the envelope by `"docSwitched":true`. Only `doc.activate` changes the active document durably. `selection.set` on a non-active doc writes into the per-document copy that `syncActiveDoc` maintains (`src/MainWindow.cpp:2840`), not into `app.selection`; if this copy does not yet include the time selection, lot 0 adds it;
2. flags → `no_document`, `unsupported_build`, `unsupported_platform`, `needs_ui`, `confirm_required`;
3. busy guard: `WorkProgressDialog::numActive() > 0` (`include/WorkProgress.h:86`), mutating job in progress, or `juce::Component::getCurrentlyModalComponent() != nullptr` — evaluated **here**, on the message thread, never from the network thread;
4. **streamed-file detour**: if `Mutates` and `audioEngine().fullyInMemory() == false` (`include/AudioEngine.h:45`), conversion into a job that does `ensureEditable` on a worker with progress, then the handler (`runDestructiveEdit` pattern, `src/MainWindow.cpp:1996-2040`);
5. `AlertGate::ScopedCapture` → handler → `recordActionStep` if recording is active → `journal.append` (always) → `ui->refreshAfterEdit()` if `Mutates` and `UiBridge`;
6. envelope enriched with `undoDepth`, `redoDepth`, `redoDiscarded`, `contentVersion`, `journalSeq`.

Validation per handler with an `expect(params, "db", Number, min, max)` helper → `invalid_params{path, expected}`. **No** generic JSON Schema validator on the C++ side (debt refused), but `paramsSchema` is **not** written by hand: each handler declares its parameters only once in a `ParamSpec { name, type, required, min, max, enumValues, doc }` array; this same array feeds `expect()` (validation) **and** the generation of a **restricted JSON Schema draft 2020-12** object (`type`, `properties`, `required`, `minimum`/`maximum`, `enum`, `items`, `description`, `additionalProperties:false`), published by `app.describe` and passed as is as the MCP `inputSchema` (which MCP requires). A single source, hence no "sixth list". Test `testControlDescribe`: for each command, a minimal `params` generated from the schema passes `expect()`, and every `invalid_params` emitted cites a `path` present in the schema. Example output in §6.2.

**Lot 0: generic `step.run`** — a single entry point that wraps `ActionStep::fromJson` → `validateStepParams` (`src/Actions.cpp:346`) → `applyActionStep` (`:412`) → `recordActionStep` → `refreshAfterEdit`, with explicit `select {start,end}|"all"` and `channels[]`. The 20 types are available from lot 0; lot 6 splits them into `edit.*`/`effect.*`/`gen.*`/`ai.*` behind aliases, and `applyActionStep` becomes a 5-line facade over `invoke`.

**UiBridge** — abstract interface `{ refreshAfterEdit, applyTimeWindow, zoomToSelection/Fit/OutFull, setPanelVisible, isPanelOpen, beginJobDialog, setJobProgress, endJobDialog, openFileAsync }` implemented by `MainWindow` (`include/MainWindow.h:180-234`; `openFile` `:1929` switches to `openFileWithProgress` above 200 MB), `nullptr` in CLI/tests. No command calls `menuItemSelected`, `runModalLoop`, `FileChooser` or `showEditError`.

**ControlServer** — `juce::Thread "SC-Control"`, `StreamingSocket::createListener(0, "127.0.0.1")` + `getBoundPort()`; one `ClientSession` per client (max 4), `waitUntilReady(true, 500)` to see `threadShouldExit()` (same 500 ms as `src/SingleInstance.cpp:97`), splitting on `\n`, `juce::JSON::parse`; a single writer per socket (outgoing queue under a `CriticalSection`). Execution of a short command:

```cpp
auto st = std::make_shared<PendingCall>();   // WaitableEvent done; Outcome out; std::atomic<bool> abandoned{false};
if (!juce::MessageManager::callAsync ([st, req, this] { if (st->abandoned) return; st->out = dispatch (req); st->done.signal(); }))
    return error ("shutting_down");
if (!st->done.wait (req.timeoutMs /*30000*/)) { st->abandoned = true; return error ("timeout", {{"hint","a modal dialog is probably open"}}); }
```

(the `src/Application.cpp:3602-3613` pattern made safe by the `shared_ptr` and the timeout; the infinite wait is never used.) **Shutdown**: `shuttingDown = true; { StreamingSocket poke; poke.connect("127.0.0.1", boundPort, 500); } listener.close(); stopThread(2000);` — mandatory because `close()` connects to `portNumber`, which stayed at 0 (`juce_Socket.cpp:157-158`, `:613`). Startup in `initialiseInternal` after `forwarder->startServer()` (`src/Main.cpp:845`) if the `control_enabled` setting (default 0) or the `--control` flag is set; shutdown in `shutdown()` **before** `mainWindow.reset()` (`src/Main.cpp:856-857`). The FileForward pipe stays intact. When `--control` is present, `initialiseInternal` **skips** `handleStartup` (`src/Main.cpp:825-832`): the process launched by the adapter always starts its own instance, even with `multi_instance=0`; otherwise it would forward its files to the existing instance and exit without writing a discovery file (`src/SingleInstance.cpp:191-213`). macOS: no entitlement is set by the CMake build today (no App Sandbox), so `createListener` works; if the app were one day sandboxed/notarised with App Sandbox, `com.apple.security.network.server` would be needed.

**Discovery file** — `<userApplicationDataDirectory>/SpectroCraft/control/<pid>.json` = `{pid, port, token, version, startedAt, exe, protocol}`, 0600. Actual paths: macOS `~/Library/SpectroCraft/control`, Linux `$XDG_CONFIG_HOME` or `~/.config/SpectroCraft/control`, Windows `%APPDATA%\SpectroCraft\control`. Dead PIDs are swept at startup. `0600` exists only on POSIX; on Windows the protection is the default ACL of the user profile (`%APPDATA%`), no explicit ACL is set: the token is readable by any process of the same user, which is the chosen threat model (loopback + same account, no intra-user defence).

**JobManager** — `Job { id "j-<n>", cmd, state, progress, status, result, error, atomic cancel, mutating, docId }`; one `std::thread` per job (`runNoiseSuppressAsync` model, `src/MainWindow.cpp:6255`); **only one mutating job at a time**; commit through the `callAsync` + `WaitableEvent` hop after aligning `transcribe` (`:3951`) and the default branch of `noiseSuppress` (`:3562-3565`). Via `UiBridge`, a mutating job opens the same non-blocking `WorkProgressDialog` as the UI, so `numActive()`, `isBackgroundJobRunning` (`src/MainWindow.cpp:2445-2449`) and `systemRequestedQuit` (`src/Main.cpp:868`) see it without modification.

**Journal** — lot 0: two lines in `Application::recordActionStep` **before** the `if (!actionRec.active …) return;` (`src/Application.cpp:355`) + in `undoSteps/redoSteps` + in `CommandRegistry::invoke` (every API command). **Actual scope in lot 0**: `recordActionStep` is only called from 27 sites in `src/MainWindow.cpp` for the 20 step types (effect dialogs, Delete/Trim, Python, NS, transcription); `EditCut`/`EditPaste`/`EditPasteMix`/`EditPasteToNewChannels` (`src/MainWindow.cpp:3008-3010`, `:3020-3023`), markers, selections, drawings, tables, recording and mouse undo/redo (`:3006-3007`) do not go through it. Until lot 5, `history.list.journal` therefore sees **dialog-driven edits and all API commands**, not everything Nicolas does with the mouse; the undo stack (`states[]`) remains exhaustive for audio, but without type/params for those entries. Lot 5: ring of 10,000 entries + JSON Lines `<appData>/SpectroCraft/journal/session-<pid>-<date>.jsonl` (`StartupTrace::State::emit` pattern, `include/StartupTrace.h:28-40`), monotonic `seq`, `kind ∈ command|job|history|meta|doc|alert`, `MarkerManager::onChange` hooks (`include/MarkerManager.h:64`), `FileDoc` `metaDirty` setters, history clears with reason. Enriched `Patch` (`include/UndoPatch.h:53-75`: `seq, timestampMs, opId, params, source`) set in `recordReplace` via a thread-local operation context and completed via `lastRecorded()` (pattern `src/Application.cpp:2038-2049`); `UndoManager::onChanged` in `record/popUndo/popRedo/clear`.

**AlertGate::ScopedCapture** — `captureDepth` + `captured` list in `State` (`include/AlertGate.h:21-26`); `swallow()` pushes when `captureDepth > 0`; `invoke()` returns `alerts[]`. Never `setHeadless(false)` after a command.

**StateJson / Measures / ModelCatalog** — app state (atomic getters `include/AudioEngine.h:44-98`), history (`HistoryPanel::refresh` semantics, never `oldData/newData`), meta (`FileDoc::buildMetaJson`); `Measures` module extracted from `FFTAnalysisView` (`computeRegionFrom` with `windowType` as an argument, `computeTimeCurveFrom`/`computeSpectrumCurveFrom` made static, helpers moved out of file scope `:773-803`, `thirdOctBandsRef` unchanged) + `levels()` (RMS/peak/crest on `readSamples`, thread-safe `include/AudioEngine.h:60-61`) + `peaks()`; long measurements are jobs. `ModelCatalog` unifies `ModelManager` (`include/AI/ModelManager.h:35-57`) and the `sc::acpp` catalogues, read-only.

**`--json` CLI mode** — `--json <file|->` `[--json-out <f>]` `[--use-user-settings]` `[--list-commands [--markdown]]` on the skeleton of `--run-action` (`src/Main.cpp:293-330`), `setHeadless(true)`, `SC_NO_AUDIO_DEVICE`; each job awaited by pumping `MessageManager::runDispatchLoopUntil` (`scPumpUntil` pattern, `tests/UnitTests.cpp:207-216`) and inlined; codes 0/1/2; `else if (tok.startsWith("--"))` branch → `unknown flag`, code 2. stdin is read **line by line** (streaming, never "read everything then execute") on the message thread between two pumps: each line is executed before the next one is read and its response is written then flushed (`flush`) immediately, which allows a dialogue from a Python subprocess; line > 1 MiB → `invalid_params`. A `LongRunning` command is pumped until `timeoutMs` (default 30 s, `0` = unlimited), then cancelled (`job.cancel`) and reported as `timeout`; `continueOnError` decides what follows. Windows: the exe is `juce_add_gui_app` (`src/CMakeLists.txt:2`), hence without a console; stdin/stdout handles **inherited** from a parent that provides `PIPE`s work (`GetStdHandle` sees them), which is what `sc_headless` does; launched from an interactive terminal, nothing is displayed — behaviour to be asserted by a `testControlJsonCli` test run in the CI `windows-ai` job.

### 5.3 Threading and security, in summary

- Every short handler runs on the message thread; a handler never waits for a worker (the guarantee of `src/Application.cpp:3608-3609` only holds in one direction). Anything that waits goes into a job.
- The network thread never touches `Application` or `docs` (no lock, `include/Application.h:53`); it only parses, posts and waits with a timeout.
- Loopback only, per-instance `Uuid` token, mandatory `session.hello`, 3 failures → 2 s pause; `confirm:true` required for `Destructive` (overwriting save, closing unsaved, python outside `scriptFolder()`); audio.cpp assets listed but `installable:false` as long as they have no SHA-256; the token is never journaled.
- Persistent opt-in "Window > Allow AI control (localhost)" modelled on `WindowAllowMultiple` (`src/MainWindow.cpp:2796`) + status bar indicator when a client is connected.
- **Path perimeter**: `doc.open`, `doc.save/saveAs`, `export.*`, `markers.importCsv/exportCsv`, `python.run {script}` accept only absolute paths **under a list of allowed roots** (`control_roots` setting, default: the user's home folder + `scriptFolder()` (`src/PythonScripts.cpp:519-523`) + temporary folder), resolved after `File::getLinkedTarget()`; outside the perimeter → `confirm_required{reason:"outside_roots"}`, never silent; existing target on write → `confirm`. The `control/` folder (tokens) and `SpectroCraft.recent` are refused with no possible `confirm`.
- **Limits**: NDJSON line ≤ 1 MiB (socket and `--json`), `params` ≤ 256 KiB after parsing, 64 in-flight requests per client, bounded event queue (1000; beyond that, the oldest are lost and an `events.dropped{count}` is emitted), 4 clients, 2 s pause after 3 `unauthorized`.
- **Multiple clients** (two VS Code windows): commands are serialised by the message thread; only one mutating job at a time, whatever the client; `history.undo` acts on the document's stack, so one client can undo another's edit — intended (one app, one stack), but every journal entry carries `source:"api:<client>"` (`client` field of `session.hello`) and `history.changed` is broadcast to all subscribers, so that the adapter can report it to Claude.
- **Journal content**: metadata only (ids, types, `params`, paths, `segmentCount`, durations); never the transcription text or table values (potentially sensitive content) unless the `journal_verbose` setting is on; the token never appears in it.

### 5.4 Why not the other two architectures

**A — Minimal bridge (socket + `switch` dispatcher over `applyActionStep`, hand-written Python MCP)**: the fastest (8.5 d) but it adds a **sixth** hand-maintained list of types next to `actionTypes` / `isKnownActionType` / `validateStepParams` / `applyActionStep` / `continueReplay`, i.e. the exact mechanism that produced the `envelope` bug; menus and dispatcher remain two paths to test; it addressed documents by index and, in `--json` mode, did not pump the message loop (jobs never finished).

**C — Full command bus (registry + JSON Schema validator + journal + server)**: equivalent to B in quality and discoverability, but 28 days, with the control server in lot 3 (~16 d before driving the open app, the opposite of the main need), a home-made JSON Schema validator (~150 lines with no JUCE reuse) from lot 0, and an internal inconsistency (`docId` resolution on the network thread, which reads `docs` without a lock).

---

## 6. JSON protocol

### 6.1 Envelope (NDJSON; identical on the socket and in `--json` mode)

```
Request  : {"id":"r12","cmd":"<domain.command>","params":{...},"doc":<docId|path>,"timeoutMs":30000}
Response : {"id":"r12","ok":true,"result":{...},"alerts":[],
            "undoDepth":5,"redoDepth":0,"redoDiscarded":0,"contentVersion":56,"journalSeq":413,"durationMs":21}
Error    : {"id":"r12","ok":false,"error":{"code":"<code>","message":"...","details":{...}}}
Event    : {"event":"<name>","seq":414,"ts":"2026-11-05T14:21:09.002Z","data":{...}}
Job      : {"ok":true,"result":{"job":"j-7","state":"running"}}   (in --json: awaited and inlined)
```

`doc` absent = active document. The envelope state fields are present only for `Mutates` commands. Closed set of codes: `unauthorized`, `unknown_cmd`, `invalid_params` (`details.path`, `details.expected`), `no_document`, `unknown_doc`, `busy` (`details.job` or `details.modalActive`), `timeout`, `cancelled`, `confirm_required` (`details.hint`), `needs_ui`, `unsupported_build`, `unsupported_platform`, `model_not_installed`, `job_not_found`, `shutting_down`, `failed`. Units: seconds, 0-based channels, levels in dB with an explicit `unit` (`dBFS` or `dB-SPL`/`dBV` via `ScaleCalibration`).

### 6.2 App state

```json
{"id":"1","cmd":"session.hello","params":{"token":"7f3c…","client":"spectrocraft-mcp/0.1","subscribe":["job","history","doc"],"sinceSeq":0}}
{"id":"1","ok":true,"result":{"app":"SpectroCraft","version":"1.0.72","pid":51233,"protocol":1,"platform":"macos","capabilities":{"ai":true,"audiocpp":false,"python":true,"ui":true},"commands":48,"journalSeq":413}}

{"id":"3","cmd":"app.state","params":{}}
{"id":"3","ok":true,"result":{"docs":[{"id":7,"path":"/Users/nicolas/audio/recording_a.wav","name":"recording_a.wav","durationSec":12.345,"channels":2,"sampleRate":48000,"unsaved":true,"fullyInMemory":true,"undoDepth":3,"redoDepth":0,"contentVersion":41}],"activeDoc":7,"selection":{"start":1.0,"end":2.5,"hasSpectral":false},"playhead":1.0,"visibleChannels":[0,1],"view":{"t0":0.0,"t1":12.345,"fMin":20,"fMax":24000},"transport":{"playing":false,"paused":false,"looping":false,"audioReady":true},"recording":{"active":false},"ai":{"device":"auto","resolved":"CPU","lastNsModel":"deepfilternet2","lastTranscribeModel":"whisper-base","autoDownload":true},"busy":false,"modalActive":false,"jobs":[]}}
```

`version` comes from `generated/Version.h` / `SC_VERSION.txt` (patch auto-incremented by the CMake build, `CMakeLists.txt:53-65`, `VERSION.txt` = `1.0.72` as of 2026-11-05), **not** from `getApplicationVersion()`, which returns a hard-coded `"1.0.0"` (`src/Main.cpp:132`); the adapter compares `protocol`, never `version`.

`app.describe {category, withSchemas}` returns `{commands:[{id, aliases, title, category, flags, params:<schema>, result:<schema>}]}` — the source of the dynamic MCP tools. The schema is generated from the handlers' `ParamSpec` (§5.2), in restricted JSON Schema format:

```json
{"id":"2","cmd":"app.describe","params":{"category":"effect","withSchemas":true}}
{"id":"2","ok":true,"result":{"protocol":1,"commands":[
 {"id":"effect.changeGain","aliases":["changeGain"],"title":"Change gain","category":"effect","flags":["Mutates","NeedsDoc"],
  "params":{"type":"object","properties":{"db":{"type":"number","minimum":-96,"maximum":96,"description":"Gain in dB"},"select":{"oneOf":[{"type":"string","enum":["all"]},{"type":"object","properties":{"start":{"type":"number","minimum":0},"end":{"type":"number","minimum":0}},"required":["start","end"]}]},"channels":{"type":"array","items":{"type":"integer","minimum":0}}},"required":["db"],"additionalProperties":false},
  "result":{"type":"object","properties":{"patchSeq":{"type":"integer"},"description":{"type":"string"}}}}]}}
```

### 6.3 History

```json
{"id":"5","cmd":"history.list","params":{"includeJournal":true,"since":400},"doc":7}
{"id":"5","ok":true,"result":{"docId":7,"currentState":3,"maxLevels":64,
 "states":[
  {"state":0,"label":"Loaded recording_a.wav"},
  {"state":1,"patchSeq":101,"ts":"2026-11-05T14:18:02.410Z","opId":"effect.changeGain","params":{"db":6.0},"description":"Changed gain by +6.00 dB","region":{"startSec":1.0,"endSec":2.5},"lengthDelta":0,"channelsChanged":false,"rateChanged":false,"sideState":false,"dataBytes":576000,"source":"menu","future":false},
  {"state":2,"patchSeq":102,"opId":"edit.delete","description":"Deleted samples","lengthDelta":-24000,"sideState":true,"markersBefore":3,"markersAfter":2,"source":"api:claude-code","future":false},
  {"state":3,"patchSeq":103,"opId":"ai.noiseSuppress","params":{"modelId":"deepfilternet2","device":"CPU"},"description":"Noise suppressed (deepfilternet2)","future":false}],
 "journal":[
  {"seq":409,"ts":"…","kind":"history","doc":5,"what":"cleared","reason":"resample"},
  {"seq":412,"ts":"…","kind":"meta","doc":7,"what":"marker.add","data":{"name":"door_slam","start":4.1}}],
 "recordedActions":[{"name":"Clean takeoff","steps":[{"type":"normalize","params":{}}]}]}}

{"id":"6","cmd":"history.jumpTo","params":{"state":1},"doc":7}
{"id":"6","ok":true,"result":{"currentState":1,"undone":2,"redone":0},"undoDepth":1,"redoDepth":2}

{"id":"6a","cmd":"history.undo","params":{"steps":1},"doc":7}
{"id":"6a","ok":true,"result":{"currentState":0,"undone":1,"description":"Changed gain by +6.00 dB"},"undoDepth":0,"redoDepth":3,"redoDiscarded":0,"contentVersion":57,"journalSeq":415}
{"id":"6b","cmd":"history.redo","params":{},"doc":7}
{"id":"6b","ok":true,"result":{"currentState":1,"redone":1},"undoDepth":1,"redoDepth":2,"redoDiscarded":0,"contentVersion":58,"journalSeq":416}
{"id":"6b","ok":false,"error":{"code":"failed","message":"Redo failed (not enough memory?); history cleared","details":{"historyCleared":true,"see":"src/Application.cpp:4224"}}}

{"id":"6c","cmd":"journal.read","params":{"sinceSeq":400,"kinds":["command","history"],"limit":200}}
{"id":"6c","ok":true,"result":{"entries":[
  {"seq":401,"ts":"2026-11-05T14:18:02.410Z","kind":"command","cmd":"step.run","doc":7,"source":"api:claude-code","ok":true,"durationMs":41,"params":{"step":{"type":"changeGain","params":{"db":-3.0}}}},
  {"seq":409,"ts":"…","kind":"history","doc":5,"what":"cleared","reason":"resample"}],
 "nextSeq":417,"file":"/Users/nicolas/Library/SpectroCraft/journal/session-51233-2026-11-05.jsonl"}}
```

`history.undo/redo` = `Application::undoSteps/redoSteps` (`src/Application.cpp:4175`); if `applyPatch` fails the stack is cleared (`:4192`, `:4224`) and the command says so (`historyCleared`) instead of the asynchronous alert (`:4170`, captured by `ScopedCapture`). `journal.read` reads the ring (lot 0: memory only, `file:null`) then the JSONL (lot 5).

Before lot 5, `opId/params/ts/source` come from the session journal joined by undo index; afterwards, from the enriched `Patch`. `history.jumpTo` requires no `confirm`: a jump never destroys the redo stack (`include/UndoManager.h:35-42`); it is the next edit that clears it, flagged by `redoDiscarded`.

### 6.4 Running a command / a step

```json
{"id":"4","cmd":"step.run","params":{"select":{"start":1.25,"end":3.8},"channels":[0],"step":{"type":"changeGain","params":{"db":-3.0}}},"doc":7}
{"id":"4","ok":true,"result":{"applied":"changeGain","patchSeq":18,"description":"Changed gain by -3.00 dB","elapsedMs":41},"alerts":[],"undoDepth":4,"redoDepth":0,"redoDiscarded":0,"contentVersion":42,"journalSeq":414}

{"id":"4","ok":false,"error":{"code":"invalid_params","message":"changeGain: missing numeric param \"db\"","details":{"path":"step.params.db","expected":"number"}}}
```

On a streamed file (`fullyInMemory:false`) the same request returns `{"job":"j-3"}`. If `doc` is not the active document, the app switches to it for the duration of the command and then switches back (§5.2 step 1), and the envelope carries `"docSwitched":true`.

```json
{"id":"4b","cmd":"steps.run","params":{"select":"all","steps":[{"type":"normalize","params":{}},{"type":"fadeOut","params":{}}],"stopOnError":true},"doc":7}
{"id":"4b","ok":true,"result":{"applied":2,"results":[{"type":"normalize","ok":true,"patchSeq":19},{"type":"fadeOut","ok":true,"patchSeq":20}]},"undoDepth":6,"redoDepth":0,"redoDiscarded":0,"contentVersion":44,"journalSeq":418}

{"id":"4c","cmd":"step.validate","params":{"step":{"type":"envelop","params":{}}}}
{"id":"4c","ok":false,"error":{"code":"invalid_params","message":"unknown step type \"envelop\"","details":{"path":"step.type","expected":"one of actionTypes (include/Actions.h:77-96)"}}}
{"id":"4d","cmd":"step.validate","params":{"step":{"type":"changeGain","params":{"db":6.0}}}}
{"id":"4d","ok":true,"result":{"valid":true,"type":"changeGain","mutates":true,"longRunning":false}}
```

`steps.run` = one history entry per step (like `continueReplay`), `stopOnError:false` continues and reports each error in `results[]`; `step.validate` = `ActionStep::fromJson` + `validateStepParams` (`src/Actions.cpp:346`) without ever touching the document.

```json
{"id":"7","cmd":"doc.open","params":{"path":"/Users/nicolas/audio/recording_b.wav","activate":true}}
{"id":"7","ok":true,"result":{"doc":8,"durationSec":30.0,"channels":4,"sampleRate":48000,"fullyInMemory":true}}
{"id":"8","cmd":"selection.set","params":{"start":2.0,"end":3.5,"channels":[0,1]},"doc":8}
{"id":"9","cmd":"doc.save","params":{"path":"/Users/nicolas/audio/recording_b_clean.wav","meta":"sidecar"},"doc":8}
{"id":"9","ok":false,"error":{"code":"confirm_required","message":"Target exists; resend with params.confirm=true","details":{"hint":"pass confirm:true, or choose a new path"}}}
{"id":"10","cmd":"doc.close","params":{},"doc":7}
{"id":"10","ok":false,"error":{"code":"confirm_required","message":"Document has unsaved changes","details":{"unsaved":true,"hint":"call doc.save first or pass confirm:true"}}}
```

`doc.open` > 200 MB (threshold `src/MainWindow.cpp:1929`) → `{"job":"j-12"}`.

```json
{"id":"7a","cmd":"doc.list","params":{}}
{"id":"7a","ok":true,"result":{"active":8,"docs":[{"id":7,"path":"/Users/nicolas/audio/recording_a.wav","unsaved":true,"undoDepth":3,"durationSec":12.345},{"id":8,"path":"/Users/nicolas/audio/recording_b.wav","unsaved":false,"undoDepth":0,"durationSec":30.0}]}}

{"id":"9b","cmd":"doc.save","params":{"path":"/Users/nicolas/audio/recording_b_clean.wav","meta":"sidecar","confirm":true},"doc":8}
{"id":"9b","ok":true,"result":{"path":"/Users/nicolas/audio/recording_b_clean.wav","bytes":5760044,"meta":{"mode":"sidecar","written":true},"unsaved":false,"durationMs":180}}

{"id":"11","cmd":"doc.meta","params":{"sections":["markers","selections","calibration","transcription"]},"doc":8}
{"id":"11","ok":true,"result":{"mode":"sidecar","bundle":{"markers":[…],"selections":[…],"calibration":{…},"transcription":{…}}}}

{"id":"24","cmd":"view.set","params":{"t0":1.5,"t1":4.0},"doc":8}
{"id":"24","ok":true,"result":{"t0":1.5,"t1":4.0,"visibleChannels":[0,1,2,3]}}
{"id":"25","cmd":"view.zoom","params":{"to":"selection"}}
{"id":"25","ok":true,"result":{"t0":2.0,"t1":3.5}}
{"id":"26","cmd":"view.channels","params":{"visible":[0]}}
{"id":"26","ok":true,"result":{"visibleChannels":[0]}}

{"id":"27","cmd":"transport.play","params":{"from":2.0,"loop":false}}
{"id":"27","ok":true,"result":{"playing":true,"position":2.0,"audioReady":true}}
{"id":"28","cmd":"transport.stop","params":{}}
{"id":"28","ok":true,"result":{"playing":false,"position":2.73}}
{"id":"27","ok":false,"error":{"code":"failed","message":"No audio device (SC_NO_AUDIO_DEVICE or init timeout)","details":{"audioReady":false}}}

{"id":"29","cmd":"markers.add","params":{"name":"door_slam","start":4.1,"end":4.35},"doc":8}
{"id":"29","ok":true,"result":{"index":3,"count":4}}
{"id":"30","cmd":"markers.list","params":{},"doc":8}
{"id":"30","ok":true,"result":{"markers":[{"index":0,"name":"start","start":0.0,"end":0.0},{"index":3,"name":"door_slam","start":4.1,"end":4.35}]}}
{"id":"31","cmd":"markers.exportCsv","params":{"path":"/Users/nicolas/audio/recording_b_markers.csv"},"doc":8}
{"id":"31","ok":true,"result":{"path":"/Users/nicolas/audio/recording_b_markers.csv","count":4}}

{"id":"32","cmd":"settings.get","params":{"keys":["ai_device","multi_instance","control_enabled"]}}
{"id":"32","ok":true,"result":{"values":{"ai_device":"auto","multi_instance":1,"control_enabled":1},"file":"/Users/nicolas/Library/Preferences/SpectroCraft.recent"}}
{"id":"33","cmd":"settings.set","params":{"values":{"ai_device":"CPU","fft_size":99999}}}
{"id":"33","ok":true,"result":{"applied":{"ai_device":"CPU","fft_size":65536},"clamped":["fft_size"],"persisted":true}}

{"id":"34","cmd":"python.run","params":{"script":"/Users/nicolas/Library/SpectroCraft/scripts/hp_filter.py","timeoutMs":120000},"doc":8}
{"id":"34","ok":true,"result":{"job":"j-20"}}
{"event":"job.done","seq":480,"data":{"job":"j-20","ok":true,"result":{"patchSeq":21,"stdoutTail":"…","scriptSha256":"…","resultJson":null}}}
```

`doc.meta` = output of `FileDoc::buildMetaJson` (`include/FileDoc.h:173`), filtered by `sections`. `view.*` = `viewStart/viewEnd` + `ensureTimeRangeVisible` (`include/Application.h:120`, `:483`) and `visibleChannels` (`:260`); `view.zoom {vertical}` and `view.show {waveform|spectral}` are `NeedsUi` (members of `MainWindow`) → `needs_ui` in the CLI. `transport.*` = `play()/stop()/pause()/toggleLoop()` with `ensureAudioReady(2000)` (`include/Application.h:778`) instead of the wait without timeout (`src/Application.cpp:4433`). `markers.*` = `MarkerManager` (fields = those of `saveCsv`, `src/MarkerManager.cpp:745`; `importCsv` = `loadCsv` `:757`), outside the undo stack (§3 #10), journaled from lot 5 via `onChange` (`include/MarkerManager.h:64`). `settings.*` goes through the clamps of `src/Application.cpp:168-242` (never bare `app.settings`), persists through `saveSettings` (`:350-380`, full rewrite of `.recent`), unknown key → `invalid_params`. `python.run` = `runPythonScript` (`src/Application.cpp:3629`) as a job with a new timeout; `confirm` required outside `scriptFolder()`; `resultJson` (markers/measurements returned by the script) only exists from lot 7 onwards.

### 6.5 AI models: list / install / run (with backend)

```json
{"id":"12","cmd":"ai.models.list","params":{}}
{"id":"12","ok":true,"result":{"modelsRoot":"/Users/nicolas/Library/SpectroCraft/models","models":[
 {"id":"deepfilternet2","engine":"openvino","family":"noiseSuppress","displayName":"DeepFilterNet 2 (recommended)","installed":true,"verified":true,"bytesOnDisk":9700000,"path":"…/models/deepfilternet2","checksum":"sha256","backends":["CPU"]},
 {"id":"whisper-base","engine":"whisper","family":"transcribe","installed":false,"sizeBytes":147951465,"missing":["ggml-base.bin"],"checksum":"sha256","backends":["CPU"]},
 {"id":"sam-audio-base-q8_0","engine":"audiocpp","family":"stems","installed":false,"sizeBytes":2400000000,"checksum":"size-only","platforms":["windows-x64"],"installable":false,"reason":"no checksum (AudioCpp.cpp:205)"}]}}

{"id":"13","cmd":"ai.models.install","params":{"modelId":"whisper-base"}}
{"id":"13","ok":true,"result":{"job":"j-14","sizeBytes":147951465}}
{"event":"job.progress","seq":420,"data":{"job":"j-14","progress":0.63,"status":"downloading ggml-base.bin 93/148 MB"}}
{"event":"job.done","seq":431,"data":{"job":"j-14","ok":true,"result":{"modelId":"whisper-base","installed":true,"verified":true}}}

{"id":"15","cmd":"ai.devices","params":{}}
{"id":"15","ok":true,"result":{"openvino":{"available":true,"devices":["CPU"],"preference":"auto","resolved":"CPU","note":"only the CPU plugin ships"},"whisper":{"devices":["CPU"]},"audiocpp":{"available":false,"reason":"unsupported_platform","installedVariant":null}}}

{"id":"16","cmd":"ai.noiseSuppress","params":{"modelId":"deepfilternet2","device":"auto","attenLimDb":12,"df3PostFilter":false,"copyNoise":false,"autoDownload":true},"doc":8}
{"id":"16","ok":true,"result":{"job":"j-15"}}
{"event":"job.done","seq":450,"data":{"job":"j-15","ok":true,"result":{"patchSeq":19,"deviceUsed":"CPU","compileMs":8400,"inferMs":2210,"region":{"start":2.0,"end":3.5}},"undoDepth":2,"contentVersion":44}}

{"id":"17","cmd":"ai.transcribe","params":{"modelId":"whisper-base","language":"fr","translate":false,"fast":false,"channels":[0],"storeInDoc":true},"doc":8}
{"event":"job.done","seq":460,"data":{"job":"j-16","ok":true,"result":{"language":"en","segments":[{"start":0.42,"end":2.10,"text":"Tower, Alpha 21, ready at the holding point."}],"segmentCount":1,"storedInDoc":true}}}
```

```json
{"id":"36","cmd":"ai.setDevice","params":{"engine":"openvino","device":"GPU"}}
{"id":"36","ok":false,"error":{"code":"invalid_params","message":"device GPU is not available for openvino","details":{"path":"device","expected":"one of [auto, CPU]"}}}
{"id":"37","cmd":"ai.setDevice","params":{"engine":"openvino","device":"CPU","persist":true}}
{"id":"37","ok":true,"result":{"engine":"openvino","device":"CPU","resolved":"CPU","persisted":true}}

{"id":"38","cmd":"ai.models.verify","params":{"modelId":"whisper-base"}}
{"id":"38","ok":true,"result":{"job":"j-17"}}
{"event":"job.done","seq":436,"data":{"job":"j-17","ok":true,"result":{"modelId":"whisper-base","verified":true,"files":[{"name":"ggml-base.bin","bytes":147951465,"sha256Ok":true}]}}}
{"id":"39","cmd":"ai.models.remove","params":{"modelId":"whisper-base"}}
{"id":"39","ok":false,"error":{"code":"confirm_required","message":"Removes 148 MB under <modelsRoot>/whisper-base","details":{"hint":"pass confirm:true"}}}

{"id":"35","cmd":"transcription.get","params":{"channel":0},"doc":8}
{"id":"35","ok":true,"result":{"source":"sctt","language":"en","model":"whisper-base","segments":[{"start":0.42,"end":2.10,"text":"Tower, Alpha 21, ready at the holding point."}]}}
```

- `ai.devices`: the OpenVINO list requires `OvRuntime::init` (`src/AI/OvRuntime.cpp:89`, mutex `:95`, `ov::Core` `:126`), expensive the first time (library loading); `devices()` (`:148`) calls `get_available_devices` (`:152`) and an empty list means "init failed" (`:34`). By default `ai.devices {init:false}` returns `"openvino":{"initialised":false,"devices":null,"preference":"auto"}` without loading anything; `init:true` starts the initialisation in a **job** and returns the real list. Whisper: `use_gpu=false` hard-coded (`src/AI/WhisperTranscribe.cpp:91`) → `["CPU"]`. audio.cpp: `cpu|cuda|vulkan` according to `variant.txt` (`src/StemSeparation.cpp:497-503`), `unsupported_platform` outside Windows.
- `ai.setDevice {engine, device, persist}`: for `openvino` writes `aiDevicePref` (`include/Application.h:385`, shared by NS + transcription, read at `src/Application.cpp:145`, saved at `:379`) after checking against the real list (`resolveAiDevice`, `src/AI/OvRuntime.cpp:24`); `persist:true` calls `saveSettings` (full rewrite of `.recent`, §2.2); without `persist`, the preference holds for the session. `engine:"audiocpp"` → `--backend` passed to the CLI, stored separately.
- `ai.models.verify`: job that recomputes the SHA-256 (`src/AI/Sha256.cpp`) of every file in `dirFor()` and complements `isInstalled` (presence only). `ai.models.remove`: `confirm` mandatory, `busy` if a job is using the model, recursive deletion of `dirFor()` only under `modelsRoot`.
- `transcription.get`: same shape as `scttToJson` (`src/ScttChunk.cpp:57`), `source ∈ sctt|sidecar|memory` depending on the origin (`src/Application.cpp:4131`, `:4137`); `text` is never journaled (§5.3).

### 6.6 Measuring a region

```json
{"id":"18","cmd":"measure.levels","params":{"start":2.0,"end":3.5,"channels":[0,1]},"doc":8}
{"id":"18","ok":true,"result":{"region":{"start":2.0,"end":3.5,"samples":72000},"unit":"dBFS","calibration":{"unit":"Pa","fullScale":[1.0,1.0],"levelOffsetDb":[94.0,94.0]},"channels":[{"ch":0,"rmsDb":-18.3,"peakDb":-6.1,"peakAtSec":2.313,"crestDb":12.2,"dcOffset":0.0003,"clipped":0,"rmsAbsDb":75.7,"absUnit":"dB-SPL"}]}}

{"id":"19","cmd":"measure.thirdOctave","params":{"start":5.0,"end":8.0,"channel":0,"mode":"reference","order":40},"doc":8}
{"id":"19","ok":true,"result":{"mode":"reference","window":"hanning-65536","order":40,"unit":"dB-SPL","levelOffsetDb":94.0,"framesUsed":1024,"framesCap":2048,"bands":[{"fcHz":50.1,"relDb":-62.3,"absDb":31.7},{"fcHz":19953,"relDb":-71.6,"absDb":22.4}],"emptyBandValue":-999}}

{"id":"20","cmd":"measure.spectrum","params":{"start":2.0,"end":3.5,"channels":[0],"fftSize":4096,"window":"hann","peaks":{"maxCount":10,"minProminenceDb":6}},"doc":8}
{"id":"20","ok":true,"result":{"analysis":{"fftSize":4096,"window":"hann","enbwBins":1.5,"framesAveraged":64,"framesCapped":false,"binHz":11.72,"levelConvention":"RMS single-sided, dB re full scale"},"channels":[{"ch":0,"relDb":[-96.1],"absDb":[-2.1],"absUnit":"dB-SPL"}],"peaks":[{"ch":0,"freqHz":1203.4,"relDb":-21.5,"absDb":72.5,"prominenceDb":18.2}]}}

{"id":"21","cmd":"measure.harmonics","params":{"start":2.0,"end":3.5,"channel":0,"anchorHz":1203.4,"n":1,"maxK":10},"doc":8}
{"id":"21","ok":true,"result":{"fundamentalHz":1203.4,"unit":"dB-SPL","harmonics":[{"k":1,"freqHz":1203.4,"relDb":-21.5,"absDb":72.5},{"k":2,"freqHz":2406.8,"relDb":-33.0,"absDb":61.0}]}}
```

`measure.thirdOctave` = `thirdOctBandsRef` (`src/Panels.cpp:648`) + `ScaleCalibration::levelOffsetDb(ch)`; test reference ≈ 91 dB-SPL (`tests/UnitTests.cpp:2688-2775`). `mode:"iec"` = 26 uncalibrated bands (`toThirdOctBands`, `:803`). Long measurements are jobs.

```json
{"id":"40","cmd":"measure.curve","params":{"start":2.0,"end":3.5,"channel":0,"kind":"rms","windowMs":50,"hopMs":10},"doc":8}
{"id":"40","ok":true,"result":{"kind":"rms","unit":"dBFS","t0":2.0,"hopSec":0.01,"count":150,"values":[-18.9,-18.2,-17.6]}}

{"id":"41","cmd":"measure.probe","params":{"t":2.31,"f":1200.0,"channel":0},"doc":8}
{"id":"41","ok":true,"result":{"t":2.31,"f":1200.0,"frameSec":2.304,"binHz":11.72,"magDb":-21.7,"unit":"dBFS"}}

{"id":"42","cmd":"measure.spectrogramData","params":{"start":2.0,"end":3.5,"channel":0,"fftSize":2048,"hop":512,"fMax":8000,"format":"auto"},"doc":8}
{"id":"42","ok":true,"result":{"job":"j-21"}}
{"event":"job.done","seq":482,"data":{"job":"j-21","ok":true,"result":{"frames":141,"bins":342,"binHz":23.44,"hopSec":0.01067,"unit":"dBFS","inline":false,"format":"f32","path":"/tmp/sc-spec-j21.f32","layout":"frames x bins, float32 little-endian, C order"}}}

{"id":"43","cmd":"export.spectrogram","params":{"path":"/Users/nicolas/audio/recording_b_spec.png","start":0,"end":30,"width":1920,"height":600,"overlays":{"axes":true,"markers":true,"selections":false,"drawings":false}},"doc":8}
{"id":"43","ok":true,"result":{"job":"j-22"}}
{"event":"job.done","seq":490,"data":{"job":"j-22","ok":true,"result":{"path":"/Users/nicolas/audio/recording_b_spec.png","width":1920,"height":600,"bytes":412000,"overlays":{"axes":true,"markers":true}}}}
```

`measure.curve` = `computeTimeCurveFrom`/`computeSpectrumCurveFrom` made static (`include/Panels.h:796`, `:803`); `measure.probe` = `SpectralEngine::probeMagnitude` (`src/SpectralEngine.cpp:460`); `measure.spectrogramData` = `computeTile` (`:207`) tile by tile, as a job. **Large volumes**: above 1 M values the result is written to a file (`inline:false`, `path`); formats `csv` (written line by line, no writer to reuse: the existing TSV targets the clipboard, `src/Panels.cpp:4676`) and raw `f32` (readable with `numpy.fromfile(...).reshape(frames, bins)`). **No `.npy` writer exists in the repository**; adding one (ASCII v1.0 header + float32, ~60 lines) costs ~0.25 d, counted in lot 4 but optional since `f32` is enough. `export.spectrogram`: without UI, `renderToPng` (`include/SpectralEngine.h:129`) without overlays; with overlays, `preparePngExport` via `UiBridge` (`src/MainWindow.cpp:2536`) → `needs_ui` in the CLI if `overlays` is requested.

### 6.7 Progress / cancellation

```json
{"id":"14","cmd":"job.get","params":{"job":"j-14"}}
{"id":"14","ok":true,"result":{"job":"j-14","cmd":"ai.models.install","state":"done","progress":1.0,"result":{"modelId":"whisper-base","installed":true}}}
{"id":"22","cmd":"job.cancel","params":{"job":"j-15"}}
{"id":"22","ok":true,"result":{"job":"j-15","state":"cancelling"}}
{"event":"job.done","seq":455,"data":{"job":"j-15","ok":false,"error":{"code":"cancelled","message":"cancelled by client"}}}

{"id":"23","cmd":"events.subscribe","params":{"kinds":["job","history","doc","meta"],"sinceSeq":470}}
{"id":"23","ok":true,"result":{"subscribed":true,"replayed":3}}
{"event":"history.changed","seq":474,"data":{"doc":8,"undoDepth":4,"redoDepth":0,"patchSeq":124,"opId":"edit.silence","source":"menu"}}
{"event":"doc.changed","seq":475,"data":{"doc":8,"kind":"selection"}}
```

Cancellation: `ensureInstalled` (ProgressFn → false), whisper `abort_callback` (`src/AI/WhisperTranscribe.cpp:129-133`), OpenVINO at phase boundaries, audio.cpp through `TerminateProcess`, Python through `shouldContinue` polled every 200 ms + new timeout.

Multiple clients: every event is broadcast to **all** subscribers; `history.changed.source` and `job.*.source` carry `api:<client>` (`client` field of the emitting `session.hello`) or `menu`, so that a Claude session sees the edits made by the other VS Code window or with the mouse; a `job.cancel` is accepted only from the client that started the job, unless `force:true`.

### 6.8 Typical errors

```json
{"id":"30","ok":false,"error":{"code":"busy","message":"A mutating job is running","details":{"job":"j-15","cmd":"ai.noiseSuppress"}}}
{"id":"31","ok":false,"error":{"code":"timeout","message":"Message thread did not respond within 30000 ms","details":{"modalActive":true,"hint":"a modal dialog is probably open"}}}
{"id":"32","ok":false,"error":{"code":"unsupported_platform","message":"audio.cpp runner is Windows-only","details":{"platform":"macos","see":"src/AudioCpp.cpp:713"}}}
{"id":"33","ok":false,"error":{"code":"model_not_installed","message":"whisper-large-v3 is not installed","details":{"missing":["ggml-large-v3.bin"],"sizeBytes":3100000000,"hint":"call ai.models.install"}}}
{"id":"34","ok":false,"error":{"code":"failed","message":"AI processing failed on CPU: …","details":{"manualInstall":"<buildNsFailureDetails text>"}}}
```

### 6.9 CLI form

`SpectroCraft --json req.ndjson --json-out res.ndjson` (or `-` for stdin): same lines, without `session.hello`; read line by line (streaming, §5.2), response flushed after each line; jobs awaited (message pump) and inlined, `timeoutMs` per line (default 30 s), otherwise `timeout` + cancellation; `{"continueOnError":true}` as the first line; return code 0 (all ok) / 1 (at least one error) / 2 (usage, unreadable JSON, unknown flag, line > 1 MiB). Windows: GUI exe without a console; works only with inherited handles (`stdout=PIPE`, `--json-out <file>` recommended for safety).

---

## 7. VS Code + Claude Code integration

### 7.1 Choice of transport

MCP adapter in **Python 3.11+** (official `mcp` SDK, FastMCP), **stdio** transport between Claude Code and the adapter, **loopback TCP NDJSON** between the adapter and the app. Verified reasons: JUCE has neither an HTTP server nor SSE (`extern/JUCE/modules/juce_core/network/` contains only IPAddress, MACAddress, NamedPipe, Socket, URL, WebInputStream); a stdio transport inside the app would be corrupted by the subprocesses that inherit stdin (`src/PythonScripts.cpp:752`, `:857-863`, `src/AudioCpp.cpp:564`); the Windows GUI exe has no console (`src/CMakeLists.txt:2`).

### 7.2 `tools/mcp/` tree

- `pyproject.toml` — `[project.scripts] spectrocraft-mcp = "spectrocraft_mcp.server:main"`, dependency `mcp>=1.2`.
- `spectrocraft_mcp/discovery.py` — lists `<appData>/SpectroCraft/control/*.json` at the actual paths, deletes the files whose PID is dead, checks `startedAt` against the process creation time (PID reused on Windows), picks `SPECTROCRAFT_PID`, otherwise the most recent.
- `spectrocraft_mcp/client.py` — `socket.create_connection`, `makefile("rw")`, `session.hello` with `subscribe` and `sinceSeq`, reader thread that routes by `id` and pushes `event`s into a queue, `call()`, `wait_job(job, on_progress)`, silent reconnection.
- `sc_client.py` — **dependency-free** client (stdlib socket + json): `python3 tools/mcp/sc_client.py app.state`, for diagnosing when the `mcp` package is missing.
- `spectrocraft_mcp/server.py` — FastMCP.

### 7.3 Exposed tools

| Tool | Description | Underlying command |
|---|---|---|
| `sc_status` | detected instances, build, active document; otherwise an explicit message ("control disabled in Window > Allow AI control" / "use sc_launch") | discovery + `session.hello` |
| `sc_launch(files=[])` | **before** launching: probes the `SpectroCraft.FileForward` pipe (`include/SingleInstance.h:15`) by sending `int32 0` and waiting for the `0x01` ack (`src/SingleInstance.cpp:112-137`: zero paths, no callback, harmless); if an instance answers but no live `<pid>.json` exists → immediate error "instance open without control: Window > Allow AI control" instead of waiting 20 s. Otherwise launches `SPECTROCRAFT_EXE --control`, stdin=DEVNULL, waits ≤ 20 s for the discovery file; `--control` skips `handleStartup`, so the launch succeeds even with `multi_instance=0` (§5.2 ControlServer) | — |
| `sc_state` | full state | `app.state` |
| `sc_history(doc=None, include_journal=True)` | history + journal | `history.list` |
| `sc_undo` / `sc_redo` / `sc_jump(state)` | — | `history.*` |
| `sc_run_step(type, params, start, end, channels)` | any of the 20 types on an explicit range | `step.run` |
| `sc_run(cmd, params, doc, wait=True, timeout_ms)` | pass-through + job wait with `ctx.report_progress` | any command |
| `sc_open` / `sc_save` / `sc_close` | `confirm` mandatory in the description | `doc.*` |
| `sc_models` / `sc_install_model` / `sc_devices` | — | `ai.models.*`, `ai.devices` |
| `sc_noise_suppress` / `sc_transcribe` | jobs with relayed progress | `ai.*` |
| `sc_measure_levels` / `sc_third_octave` / `sc_spectrum` / `sc_export_spectrogram` | — | `measure.*`, `export.spectrogram` |
| `sc_jobs` / `sc_cancel` | — | `job.*` |
| `sc_headless(requests)` | subprocess `SPECTROCRAFT_EXE --json - --json-out <tmp>` (stdout=PIPE, close_fds) | CLI mode |
| **Dynamic tools** `sc.<domain>.<command>` | generated on first contact from `app.describe`: the `params` of each command (restricted JSON Schema generated from the `ParamSpec`s, §5.2 and example §6.2) becomes the low-level `list_tools` `inputSchema` without transformation; `doc`, `timeoutMs` and `confirm` are added by the adapter as optional properties; any command added in C++ appears without touching the Python. Pytest test: every published `inputSchema` is validated by `jsonschema.Draft202012Validator.check_schema` | — |
| Resources `spectrocraft://commands`, `spectrocraft://journal/tail` | full describe, JSONL of the day | — |

### 7.4 `.mcp.json.example` (versioned template) → `.mcp.json` (local, in `.gitignore`)

The executable path is specific to each machine: the repository versions a **template** `.mcp.json.example`; everyone copies it to `.mcp.json` (ignored by git) and adjusts `SPECTROCRAFT_EXE`. Template contents (macOS values):

```json
{"mcpServers":{"spectrocraft":{
  "command":"uv","args":["run","--directory","tools/mcp","spectrocraft-mcp"],
  "env":{"SPECTROCRAFT_EXE":"/Applications/SpectroCraft.app/Contents/MacOS/SpectroCraft",
         "SPECTROCRAFT_LAUNCH":"ask"}}}}
```

Without `uv`: `"command":"python3","args":["-m","spectrocraft_mcp"]` after `pip install -e tools/mcp`. Windows: `"command":"py","args":["-3","-m","spectrocraft_mcp"]`, `SPECTROCRAFT_EXE` pointing to `build\SpectroCraft_artefacts\Release\SpectroCraft.exe`. A `CLAUDE.md` in the repository describes the vocabulary (docId, seconds, dB, `confirm`).

### 7.5 Lifecycle

1. Nicolas opens SpectroCraft; the `control_enabled` setting (Window menu > "Allow AI control (localhost)") or the `--control` flag starts `ControlServer`, which writes `<pid>.json`. Status bar indicator when a client is connected.
2. Claude Code launches the adapter (stdio) when the workspace is opened; lazy connection on the first tool call.
3. Multiple instances (`multi_instance=1` by default, `src/SingleInstance.cpp:32`): each has its own port and file; `sc_status` lists them with their open documents; `sc_run` accepts `pid`. **Instance open but control disabled**: no `<pid>.json`, but the `FileForward` pipe answers the probe (§7.3) → `sc_status` returns "instance open without control (pid unknown on POSIX): enable Window > Allow AI control, or relaunch with `--control`" and `sc_launch` launches nothing without `force=True`. With `multi_instance=0`, a `SpectroCraft --control` launched by the adapter **must not** forward-then-exit (`src/SingleInstance.cpp:191-213`): `--control` skips `handleStartup` (§5.2); the new controllable instance then coexists with the old one, which `sc_status` reports.
4. No instance: established by "no live `<pid>.json` **and** no answer to the pipe probe" (on POSIX there is no other probe, `src/SingleInstance.cpp:206-208`; the probe costs at most the pipe connection delay). Depending on `SPECTROCRAFT_LAUNCH` — `ask` (explicit error offering `sc_launch`), `auto` (detached Popen), `never` (falls back to `sc_headless`).
5. App shutdown: `shutdown()` stops the server before `mainWindow.reset()` and deletes the file; the adapter retries discovery on the next call. VS Code shutdown: the app keeps running; a mutating job in progress runs to completion and remains queryable via `job.get` from a new session.
6. Development loop: `SC_TEST_FILTER=control ./SpectroCraftTests --run-unit-tests` then `python3 tools/mcp/sc_client.py app.state` against the open app.

### 7.6 Example Claude ↔ app dialogue

> **Nicolas**: Open recording_b.wav, select 2–3.5 s, measure the levels, denoise with DeepFilterNet2 on CPU and tell me what changed.

```
Claude → sc_status                      ← {instances:[{pid:51233, docs:["recording_a.wav"]}]}
Claude → sc_open(path="…/recording_b.wav")← {doc:8, durationSec:30.0, channels:4, sampleRate:48000}
Claude → sc.selection.set(start=2.0,end=3.5,channels=[0,1],doc=8)
Claude → sc_measure_levels(start=2.0,end=3.5,channels=[0,1],doc=8)
                                        ← ch0 rmsDb:-18.3 peakDb:-6.1 (75.7 dB-SPL); ch1 …
Claude → sc_noise_suppress(modelId="deepfilternet2",device="CPU",attenLimDb=12,doc=8,wait=True)
         (progress relayed: compiling 20 % … inferring 85 %)
                                        ← job.done {patchSeq:19, deviceUsed:"CPU", compileMs:8400, inferMs:2210}
Claude → sc_measure_levels(…)           ← ch0 rmsDb:-24.9 peakDb:-7.0 …
Claude → sc_history(doc=8)              ← states:[Loaded, {opId:"ai.noiseSuppress", params:{…}, source:"api:claude-code"}]
```

> **Claude**: Denoising applied on 2.0–3.5 s (DeepFilterNet2, CPU, 10.6 s). The RMS of channel 0 went from -18.3 to -24.9 dBFS (75.7 → 69.1 dB-SPL), the peak from -6.1 to -7.0 dBFS. One history entry, undoable (`sc_undo`). The app's History panel shows the line with the source `api:claude-code`.

---

## 8. Delivery plan by lot

| Lot | Contents | Main files | Days | Value for Nicolas | Depends on |
|---|---|---|---|---|---|
| **0** — Minimal registry, `step.run`, session journal, `--json`, Actions fixes | `CommandRegistry`/`Context`/`Outcome`/`Spec`/`invoke`; `UiBridge` (interface + null impl); `step.run`/`steps.run`/`step.validate`, `app.describe`, `app.state`, `doc.open` (sync for small files / pumped `loadFileAsync`), `doc.list`, `selection.set/all`, `history.list/jumpTo/undo/redo`, `doc.meta`, `doc.saveAs`; `FileDoc::id`; session sink in `recordActionStep` (`src/Application.cpp:355`); `AlertGate::ScopedCapture`; `src/Actions.cpp` fixes (case `'e'`, `resolveAiDevice`, `unsupported_build`); `--json` mode with pump, codes 0/1/2, rejection of unknown flags; tests `testControlRegistry` (walks through **all** `actionTypes` with validate + apply), `testControlDescribe` (generated schema ↔ `expect()`), `testControlJsonCli` (streaming stdin, codes 0/1/2, timeout of a `LongRunning`) — all in the `--run-unit-tests` suite that ctest already runs on the three CI OSes | `include/control/CommandRegistry.h`, `src/control/CommandRegistry.cpp`, `include/control/UiBridge.h`, `src/control/StateJson.cpp`, `src/control/commands/Core.cpp`, `src/control/commands/History.cpp`, `include/AlertGate.h`, `include/FileDoc.h`, `src/Actions.cpp`, `src/Application.cpp`, `src/Main.cpp`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/regression/control.inc` | **2.5** | Claude Code drives SpectroCraft from Bash (`SpectroCraft --json -`) with a JSON contract: open, select a precise range, apply any of the 20 steps, read the history, save; three replay defects fixed for the existing batch. **Limitation**: until lot 5, the journal only sees dialog-driven edits (`recordActionStep`) and API commands, not cut/paste, markers or mouse undo (§5.2 Journal) | — |
| **1** — Control server in the open app + minimal JobManager | `ControlServer` + `ClientSession` (port 0, NDJSON, max 4, one writer), shutdown with self-connect before `close()`; `PendingCall` + 30 s timeout + `abandoned`; modal guard inside the lambda; token, `session.hello`, pause after 3 failures; 0600 discovery file, sweep; `control_enabled` setting + Window item + `--control` + indicator; `UiBridge` implemented by `MainWindow`; `JobManager` (1 mutating max, commit through hop) for streamed/open > 200 MB/`doc.save` (`runSaveAsync` extracted into `Application::saveDocument`); `view.*`, `transport.*` (`ensureAudioReady(2000)`), `doc.close`, `markers.add/list`; events by chaining the hooks; `control_roots` path perimeter + size limits (§5.3); `testControlServer` with `ScModalWatchdog`, added to `scIsSerialTest` (`:26973`): hello/token, 3 failures → pause, `--control` + `shutdown()` race (client connected and command in flight during shutdown → `shutting_down`, thread stopped < 2 s on Linux), sweep of dead `<pid>.json` files (forged file with a non-existent PID), line limit, `docSwitched`; ephemeral loopback port, hence compatible with GitHub runners (no network rule to open) | `include/control/ControlServer.h`, `src/control/ControlServer.cpp`, `src/control/ClientSession.cpp`, `src/control/JobManager.cpp`, `src/control/commands/View.cpp`, `src/control/commands/Transport.cpp`, `src/Main.cpp`, `src/MainWindow.cpp`, `include/MainWindow.h`, `include/Settings.h`, `tools/mcp/sc_client.py` | **3.5** | The **open** app can be driven from a terminal: state, history, undo/redo, step on a precise range with immediate screen refresh, never blocking on a dialog; opt-in, loopback, token | 0 |
| **2** — Python MCP adapter + VS Code | `tools/mcp` package (discovery at the real paths, client with reader thread, `wait_job`, reconnection + `sinceSeq`), FastMCP `server.py` (fixed + dynamic tools + resources), `.mcp.json.example`, `CLAUDE.md`, "AI control" README, pytest tests against a fake NDJSON server (hello, routing by `id`, reconnection + `sinceSeq` replay, `wait_job`, validation of the `inputSchema`s) and against `--json` (real exe, skipped if `SPECTROCRAFT_EXE` is absent), end-to-end `smoke.sh` adapter ↔ real open app (local only, not in CI); CI job `mcp-tests` (`actions/setup-python` 3.11, `pip install -e tools/mcp[test]`, pytest, no C++ build) | `tools/mcp/pyproject.toml`, `tools/mcp/spectrocraft_mcp/{discovery,client,server}.py`, `tools/mcp/tests/test_client.py`, `.mcp.json.example`, `.gitignore`, `CLAUDE.md`, `README.md`, `.github/workflows/build.yml` | **3** | From VS Code, "open recording_b.wav, select 2–3.5 s, apply a fade-out and show me the history" works in natural language, with the app reacting live | 1 |
| **3** — AI models, devices, denoising, transcription as jobs | unified `ModelCatalog`; `ai.models.list/install/remove/verify` (`ensureInstalled` as a job); audio.cpp assets `installable:false` without SHA-256; `ai.devices` (`OvRuntime::devices` `src/AI/OvRuntime.cpp:148` + `resolveAiDevice` `:24`), `ai.setDevice`; `ai.noiseSuppress/transcribe/batchTranscribe` as jobs after aligning the worker-side commits (`:3951`, `:3562-3565`); structured results; `transcription.get`; `buildNsFailureDetails` moved out of `MainWindow.cpp:6058`; `SC_MODELS_DIR` on Windows; MCP tools; `testAiCommands` with graceful skips | `include/AI/ModelCatalog.h`, `src/AI/ModelCatalog.cpp`, `src/AI/ModelManager.cpp`, `src/control/commands/Ai.cpp`, `src/Application.cpp`, `src/NoiseSuppression.cpp`, `src/MainWindow.cpp`, `tools/mcp/spectrocraft_mcp/server.py`, `tests/regression/ai_commands.inc` | **3** | "Install whisper-base and transcribe channel 0 in French", "denoise with DeepFilterNet3 on CPU": Claude launches, follows, cancels, receives the segments as JSON; the advertised devices are those actually available | 1 |
| **4** — Exportable numeric measurements | `Measures` module extracted from `FFTAnalysisView` (static, `windowType` as an argument, helpers moved out of file scope); `levels()`, `peaks()`, `harmonics()`; `measure.levels/spectrum/thirdOctave/curve/probe/spectrogramData/harmonics`; `export.spectrogram` (`renderToPng` `include/SpectralEngine.h:129` + overlays via `UiBridge`); `markers.exportCsv/importCsv`; numeric equality tests Measures vs view caches and vs dB-SPL tests (`:2688-2775`) | `include/Measures.h`, `src/Measures.cpp`, `src/control/commands/Measure.cpp`, `src/control/commands/Export.cpp`, `src/Panels.cpp`, `include/Panels.h`, `tests/regression/measures.inc` | **3** | For test and qualification reports: per-channel RMS/peak levels and dB-SPL 1/3-octave bands identical to the screen, with units and full scale, usable in a report without TSV | 1 |
| **5** — Persistent journal, enriched Patch, events with replay | ring of 10,000 + JSONL per session/PID, `seq`, `kind`, rotation; `MarkerManager::onChange` hooks, `metaDirty` setters, clears with reason; enriched `Patch` (`seq, timestampMs, opId, params, source`) via thread-local context in `recordReplace` + `lastRecorded()`; `UndoManager::nextSeq` + `onChanged`; complete `history.list`, `journal.read`, `events.subscribe` with replay; History panel with timestamp and source; `testJournal`, `testHistoryJson` (history memory `:21143` unchanged) | `include/control/Journal.h`, `src/control/Journal.cpp`, `include/UndoPatch.h`, `include/UndoManager.h`, `src/AudioEngine.cpp`, `src/Application.cpp`, `src/FileDoc.cpp`, `src/MarkerManager.cpp`, `src/Panels.cpp`, `tests/regression/journal.inc` | **2.5** | "What has been done to this file since this morning, with which parameters?" gets an exact, timestamped answer, including markers and undo clears; the adapter loses no event after reconnecting | 1 |
| **6** — Splitting the commands and migrating the menus | `edit.*`/`effect.*`/`gen.*`/`ai.*`/`python.run` with alias = old type; `applyActionStep`/`validateStepParams`/`isKnownActionType` as facades; `edit.cut/copy/paste*`, `selection.*`, `tables.*`, `meta.*`, `settings.get/set` (factored clamps), `recent.*`, `doc.new/activate`; migration of `menuItemSelected` (`src/MainWindow.cpp:2773-3139`) and of the 27 `recordActionStep` sites (dialogs kept as forms, `runDestructiveEdit` = UI wrapper of the registry); `continueReplay` with a completion callback instead of `showMessageBox` (`:2076`, `:2089`); "menu == command" test via `getLastEffectId` (`include/MainWindow.h:264`); generated `docs/commands.md` | `src/control/commands/{Edit,Markers,Tables,Meta,Settings}.cpp`, `src/Actions.cpp`, `include/Actions.h`, `src/MainWindow.cpp`, `src/Application.cpp`, `include/Settings.h`, `docs/commands.md` | **3** | Every useful menu feature has its typed, discoverable JSON equivalent; mouse and API go through the same code (same refresh, same recording, same journal) | 5 |
| **7** — Hardening, Python bridge, legacy flags, stems/SR (Windows) | `runProcess`: timeout, stdin → NUL / `/dev/null`, `PYTHONUTF8=1`, SHA-256 hash of the script (`src/AI/Sha256.cpp`) in the journal, `scriptFolder()` allow-list + confirm; optional `result.json` read back (markers, text, measurements), `out.wav` optional; `runProcessWithPipes` with stdin closed; `--test-*` flags reimplemented on top of `--json` (return codes, `--test-batch-transcribe` on an isolated store); `StemService`/`SuperResService` extracted (`src/StemSeparation.cpp:1055-1280`, `src/SuperResolution.cpp:579-673`) → `ai.stems.run`/`ai.superRes.run` (`WindowsOnly`); SHA-256 of the audio.cpp assets; **security review** against a closed list: `control_roots` perimeter and symbolic links, token (generation, non-journaling, Windows ACL), size limits and event queue, journal content (no transcription text), execution of scripts (`scriptFolder` + hash) and of downloaded binaries (SHA-256 mandatory before `installable:true`), behaviour under `multi_instance=0`, fuzzing with 10,000 random NDJSON lines without a crash; **load test**: 1000 sequential `app.state` on the dev machine < 10 s, p99 < 50 ms, stable memory (± 5 MB) after 10,000 commands | `src/PythonScripts.cpp`, `src/AudioCpp.cpp`, `src/Application.cpp`, `src/Main.cpp`, `src/AI/StemService.cpp`, `src/StemSeparation.cpp`, `src/SuperResolution.cpp`, `src/control/commands/Ai.cpp` | **2.5** | A Python script launched by Claude can no longer freeze the app or corrupt a channel, and can return markers or measurements; on Windows, stems and super-resolution can be driven; protocol documented for other clients (test tools, scripts) | 3 |
| **Total** | | | **23 d** nominal; **range 23-30 d** (lot 1: 3.5-5; lot 5: 2.5-3.5; lot 6: 3-5, the most uncertain; the others ± 0.5) | Control of the open app from VS Code at the end of lot 2 (**9 d**, 9-11 with margin); AI and measurements (lots 3-4) independent of each other | |

### 8.1 Test strategy and estimation assumptions

**Existing CI** (`.github/workflows/build.yml`): three jobs, `windows-latest` (Release, `ctest -j 1` to serialise model downloads), `macos-15` and `ubuntu-24.04` (Debug, parallel shards, Linux under `xvfb-run`), all with `-DSPECTRO_ENABLE_AI=ON`, models cached, tests = the embedded `--run-unit-tests` suite that ctest runs (tests/CMakeLists.txt: `SC_TEST_SHARDS`). No Python is installed by the CI today.

| Level | What | Where it runs |
|---|---|---|
| C++ unit (lots 0, 1, 3-7) | `testControlRegistry`, `testControlDescribe`, `testControlJsonCli`, `testControlServer`, `testAiCommands`, `testMeasures`, `testJournal`, `testHistoryJson`; `testControlServer` opens an ephemeral loopback port (`createListener(0,"127.0.0.1")`), allowed on all three runners; marked `scIsSerialTest` (`tests/UnitTests.cpp:26973`) so as not to cross shards; guarded by `ScModalWatchdog` (`:27082`) like `testSingleInstance` (`:25849-25922`) | the 3 existing jobs, with no workflow change |
| OS-specific behaviours | Windows: `--json` with inherited handles (GUI exe), ACL of the discovery file; Linux: listener shutdown (`portNumber=0`, `juce_Socket.cpp:613`); macOS: `~/Library/SpectroCraft/control` | each job via `#if` in the tests, assertion rather than skip |
| Python (lot 2) | pytest against a fake NDJSON server (no exe): hello, routing, `sinceSeq` reconnection, `wait_job`, valid `inputSchema`s; against `--json` when `SPECTROCRAFT_EXE` is provided | new `mcp-tests` job (`actions/setup-python` 3.11 + `pip install -e tools/mcp[test]`), ~1 min, no C++ build |
| End to end | `smoke.sh`: launches the app with `--control`, `sc_client.py app.state`, one `step.run`, one `history.undo`, closes; requires a graphical session | dev machine only (checklist before handing over to Max), not in CI |
| Load / robustness (lot 7) | 1000 `app.state` < 10 s, p99 < 50 ms; 10,000 commands without memory drift; NDJSON fuzzing | dev machine, results recorded in `docs/commands.md` |

**Estimation assumptions**: person-days of a developer who knows JUCE and the repository (Nicolas, or Max for lots 6-7), full-time equivalent; each lot includes its tests (~30 % of the lot) and its documentation (~10 %); no CI build to repair along the way; audio.cpp not tested outside Windows (a Windows machine is required for lot 7). Uncertainty: ± 0.5 d on lots 0, 2, 3, 4, 7; lot 1 3.5-5 d (server threading, shutdown race); lot 5 2.5-3.5 d (scattered hooks); lot 6 3-5 d (migration of 90 cases and 27 sites, possible UI regressions). Nominal total 23 d, range 23-30 d.

---

## 9. Questions for Nicolas / Max to decide

1. **Order of lots 3 and 4**: the plan delivers AI (lot 3) before measurements (lot 4). If measurements matter more to users, should they be swapped? Both depend only on lot 1.
2. **Opt-in**: "Allow AI control (localhost)" disabled by default (recommended) or enabled on the dev machine? Should there also be a non-modal visual confirmation in the app (30-60 s toast) for destructive operations launched by Claude?
3. **Location of the discovery file**: `<userApplicationDataDirectory>/SpectroCraft/control` (macOS `~/Library/SpectroCraft/control`, consistent with `scriptFolder` and `models`) — OK, or next to `SpectroCraft.recent` (`~/Library/Preferences` on macOS)?
4. **MCP adapter**: Python with `uv` (consistent with the Python bridge) or `pip install -e`? TypeScript if zero Python dependencies are required on locked-down machines?
5. **MCP tools**: a large typed set (`sc_noise_suppress`, `sc_measure_levels`…) or a minimal set (`sc_run` + `sc_commands`) complemented by the dynamic tools? Both are planned; the question is what should appear in Claude Code's list.
6. **Persistent journal** (JSONL in `~/Library/SpectroCraft/journal`, contains file paths): wanted from lot 5, or memory only?
7. **First edit after going back in history**: accept silently (redo stack cleared, flagged by `redoDiscarded` — recommended) or require `confirm:true` for commands coming from the API?
8. **`--json` mode on a streamed file (> 200 MB)**: materialise without limit or refuse beyond a RAM threshold? Should `--json` eventually replace the `--test-*`/`--run-action` flags (deprecation)?
9. **Menu migration (lot 6, 3 d)**: do it, or accept that menus and API remain two paths and that mouse edits have no type/params before lot 5? It is the riskiest lot but it is the one that guarantees "the same code everywhere".
10. **Stems / super-resolution (lot 7, Windows only)**: a priority, or to be decided with Max depending on the migration to audio.cpp? Add the SHA-256 of the audio.cpp assets to the catalogue (a prerequisite for any installation through the API)?
11. **Protocol version**: freeze `protocol:1` from lot 1 with a compatibility commitment for other clients, or keep it experimental until lot 6?
12. **For Max**: the three `src/Actions.cpp` fixes (case `'e'`, `resolveAiDevice`, error in a non-AI build) and the alignment of the worker-side commits (`src/Application.cpp:3951`, `:3562-3565`) are bugs independent of AI control — deliver them as a separate PR before lot 0?
13. **Fork or upstream contribution**: a `src/control/` folder, a `tools/mcp` Python package, one more CI job and a `.mcp.json.example` template commit the maintainer (SuperMax3d). Does Max accept these additions in his repository (with him as reviewer of lots 0-1), or does Nicolas work on a synchronised fork? In the latter case, lots 6-7 (menu migration) become very costly to rebase.
14. **CMake option `SC_ENABLE_CONTROL`** (default ON) to ship builds without the control server or the `--json` flag (locked-down machines)? If so, `applyActionStep` and the registry remain compiled (replay depends on them); only `ControlServer`, `--control` and the menu item disappear.
15. **`--control` and `multi_instance=0`**: accept that a `SpectroCraft --control` launched by the adapter creates a second instance next to the first one (recommended, §7.5), or prefer that it fails with a message?
16. **Protocol language**: command identifiers and error `message`s in English (recommended: consistent with the code, the alerts and Claude Code), `docs/commands.md` documentation in French? **Decided: everything in English (DECISIONS.md).**
17. **Pinned versions on locked-down machines**: can Python 3.11+ and the `mcp>=1.2` SDK be installed (proxy, software policy)? If not, the dependency-free `sc_client.py` is enough for a first use, and a TypeScript adapter remains possible (question 4).
18. **Path perimeter** (§5.3): default roots = home folder + `scriptFolder()` + temporary folder; should the team's network shares be added right away, or should `confirm` be required for every access outside the home folder?

---

## 10. Appendix: audit method and figures

**Method.** Read-only review of the repository, no build, no execution.

1. **Eight specialised readers**, one per subsystem: CLI/IPC (`Main.cpp`, `SingleInstance.cpp`), Actions, history (`UndoManager`/`UndoPatch`/`HistoryPanel`), AI models (`ModelManager`, `NoiseSuppression`, `Whisper*`, `AudioCpp`, `StemSeparation`, `SuperResolution`), `Application` API, measurements and analysis data (`FFTAnalysisView`, `SpectralEngine`, `FileMeta`, `Tables`, `MarkerManager`), Python bridge and subprocesses, persistence/settings/test harness. Each reader produced a map (entry points, inventory, gaps, reusables, risks) and four key claims.
2. **Adversarial verifications**: the 32 claims were re-read in the code by a distinct verifier; 15 confirmed, 17 partial (none refuted outright). The corrections are incorporated into §3.
3. **Three architects** each proposed a complete design (minimal bridge; embedded server + single registry; full command bus) with protocol, VS Code integration and lot plan.
4. **Three independent judges** scored each design (coverage, effort, risk, maintainability, AI usability) with a feasibility check in the code for every technical claim; they raised seven factual errors (JUCE listener shutdown, macOS path, non-existent `toBundle`, addressing by index, `setHeadless(false)`, `wait(-1)` under modal loops, missing pump in `--json`) — all verified and corrected in the final design.
5. **Synthesis**: design B chosen unanimously, reordered (control of the open app first) and grafted with the best ideas from A and C (§5, §8).
6. **Final critique** by the author: spot checks on `src/Actions.cpp` (switch cases, line 441), `juce_Socket.cpp:155-160`, counts of `recordActionStep` (27) and `runModalLoop` (32 + 28), `JUCE_MODAL_LOOPS_PERMITTED`, `kMaxLevels`, `forwarder`/`mainWindow` order in `shutdown`.
7. **Revision after external critique** (~150 references re-read by a distinct reviewer): six claims corrected (`onStateChange`: 14 sites and not just one; 11 flags + 5 sub-options and not 17; version `1.0.72`; `:1187`/`:4170` covered by `AlertGate`; tokenisation on all platforms; proof of the `multi_instance` default), and ten gaps filled (`--control` × `multi_instance=0`, non-active document, path perimeter and limits, `paramsSchema` format, appendix §11, missing JSON examples, journal scope in lot 0, test strategy tied to the CI, estimation margin, questions 13-18).

**Figures.**

| Measure | Value |
|---|---|
| Lines in `src/*.cpp` | 46,762 |
| Lines in `tests/UnitTests.cpp` | 27,429 (+ 9 `tests/regression/*.inc`) |
| Occurrences of `Application app` in the tests | 268 |
| Recognised CLI flags | 11 main + 5 sub-options; 0 in JSON; 3 with a reliable return code |
| Action step types | 20; 19 executable headless |
| MenuIDs | 91 declared, 90 handled, 1 dead |
| `recordActionStep` sites in `MainWindow.cpp` | 27 |
| Raw `runModalLoop` | 60 (32 `MainWindow.cpp` + 28 `Panels.cpp`) |
| AI models with SHA-256 | 15; audio.cpp assets without checksum: 4 binaries + 11 models |
| `PropertiesFile` keys | 79 (69 named + `recent_1..10`) |
| Undo depth | 64 |
| Verified claims | 32 (15 confirmed, 17 partial) |
| Designs / judges | 3 / 3 |
| Lots / days | 8 / 23 nominal (range 23-30) |
| MenuIDs per lot (appendix §11) | lot 0: 24; lot 1: 23; lot 4: 5; lot 6: 36; lot 7: 2; dead: 1 |

---

## 11. Appendix: coverage of the 91 MenuIDs (requirement 2)

Source: the `MenuIDs` enum (`include/MainWindow.h:485-547`, 93 enumerators, with the sentinels `RecNoDeviceItem` and `RecDeviceEnd` excluded) and `menuItemSelected` (`src/MainWindow.cpp:2773-3139`). "Current path" = what the menu calls today; "Lot" = first lot in which the JSON command exists. The 18 MenuIDs covered by an action step are those marked `step.run`.

| Menu | MenuID(s) | Current path (`src/MainWindow.cpp`) | JSON command | Lot |
|---|---|---|---|---|
| File | `FileOpen` | `handleFileOpen()` `:2959` | `doc.open` | 0 |
| File | `FileSave`, `FileSaveAs` | `handleFileSave/SaveAs()` `:2971-2972` → `runSaveAsync` `:2318` | `doc.save` / `doc.saveAs` (sync for small files in lot 0; job + extracted `Application::saveDocument` in lot 1) | 0 / 1 |
| File | `FileClose`, `FileCloseAll` | `handleCloseFile()` `:2968`, `handleCloseAll()` `:2961` | `doc.close {confirm}`, `doc.closeAll` | 1 |
| File | `FileQuit` | `closeButtonPressed()` `:3004` (refuses if a job is running) | `app.quit {confirm}` | 1 |
| File | `FileNew` | `showNewFileDialog()` `:2958` | `doc.new {channels, sampleRate, durationSec}` | 6 |
| File | `MetaEmbedded`, `MetaExternal`, `MetaSaveOnly` | `setActiveDocMetaMode` `:2973-2978` (`include/Application.h:412`), `handleSaveMetadataOnly()` `:2965` | `meta.setMode`, `meta.save` | 6 |
| File | `FileExportSpectral` | `handleExportSpectral()` `:2979` → `preparePngExport` `:2536` | `export.spectrogram` (overlays via `UiBridge`) | 4 |
| File | `ExpAxes`, `ExpMarkers`, `ExpSelections`, `ExpDrawings` | `app.settings.export*` toggles `:2988-3003` | `overlays` parameter of `export.spectrogram` + `settings.set` | 4 |
| File | `FileSnapshot` | `takeSpectrogramSnapshot()` `:2983` (snapshots not persisted, `include/Panels.h:678`) | `export.snapshot` (`NeedsUi`) | 6 |
| File | `RecentBase+i`, `ClearRecent` | `app.recentFiles` `:2807-2822` | `recent.list/open/remove/clear` | 6 |
| Edit | `EditUndo`, `EditRedo` | `undoStep/redoStep` + `refreshAfterEdit` `:3006-3007` | `history.undo/redo` | 0 |
| Edit | `EditDelete`, `EditTrimSelection` | `recordActionStep` + `runDestructiveEdit` `:3024-3029` | `step.run {type:"delete"|"trim"}` | 0 |
| Edit | `EditSelectAll` | `app.selectAll()` `:3030` | `selection.all` | 0 |
| Edit | `EditCut`, `EditCopy`, `EditPaste`, `EditPasteMix`, `EditPasteToNewChannels`, `EditCopyToNewFile` | `app.cut/copy/paste/pasteMix/pasteToNewChannels/copyToNewFile` `:3008-3023` (no step type, internal clipboard) | `edit.cut/copy/paste/pasteMix/pasteToNewChannels/copyToNewFile` | 6 |
| Edit | `EditSnapToZeroCrossings` | `app.settings.snapToZeroCrossings` `:3031-3034` | `settings.set` | 6 |
| View | `ViewZoomIn/Out/Fit/ToSelection/OutFull` | `zoomHorizontal/zoomToFit/zoomToSelection/zoomOutFull` `:3036-3040` | `view.zoom {factor|to}` (`viewStart/viewEnd`, `include/Application.h:120`, `:483`) | 1 |
| View | `ViewVerticalZoomIn/Out` | `zoomVerticalBoth` `:3041-3042` (`MainWindow` member) | `view.zoom {vertical}` (`NeedsUi`) | 1 |
| View | `ViewShowWaveform`, `ViewShowSpectral`, `ViewShowLeftPanels` | `MainWindow` members `:2887-2906` | `view.show {waveform, spectral, leftPanels}` (`NeedsUi`) | 1 |
| View | `ViewShowAllChannels`, `ChannelBase+i` | `:2849`, `toggleChannelVisible`/`visibleChannels` `:2829-2844` (`include/Application.h:260`) | `view.channels {visible}` | 1 |
| View | `ViewAxisOnRight`, `ViewRulerAtBottom`, `ViewShowAmpGrid`, `ViewShowFreqGrid`, `ViewShowThirdOctGrid`, `ViewShowDedTrackGrid` | `app.settings.*` toggles + `onStateChange` `:2911-2952` | `settings.set` | 6 |
| View | `ViewColourMap` | no case (`include/MainWindow.h:503`) | — (dead) | — |
| Selection | `SelectionToolTimeRange/Marquee/Lasso/Paintbrush`, `SelectionClear`, `SelectionBrushSmaller/Larger` | `setSelectionTool`, `clearSpectralSelection`, `stepBrushRadius` `:2858-2877` | `selection.tool`, `selection.clearSpectral`, `selection.brush` | 6 |
| Transport | `TransportPlay/Stop/Pause/Loop` | `playWithAudioInit`, `app.stop/pause/toggleLoop` `:3044-3047` | `transport.play/stop/pause/loop` (`ensureAudioReady(2000)`) | 1 |
| Transport | `TransportPlaySpectralSel` | `app.playSpectralSel` + `pushSpectralMaskToEngine` `:3048-3056` | `transport.playSpectralSel` | 1 |
| Transport | `TransportRecord` | `handleStartStopRecording()` `:3058` → `startRecording/stopRecording` (`src/Application.cpp:1129`, `:1194`, already notify `onStateChange`) | `record.start/stop` | 6 |
| Transport | `RecOptUnlimited/Loop/Timed`, `RecDeviceBase+n` | `setRecordingMode` `:3065-3090` (duration dialogs), devices submenu | `record.setMode {mode, seconds}`, `record.setDevice` | 6 |
| Effects | `ToolsSilence`, `ToolsFadeIn`, `ToolsFadeOut`, `ToolsNormalize`, `ToolsReverse`, `ToolsInvert` | `recordActionStep` + `runDestructiveEdit` `:3096-3103`, `:3126-3129` | `step.run` | 0 |
| Effects | `ToolsChangeGain`, `ToolsTimeStretch`, `ToolsCatTimeScale`, `ToolsFilterEq`, `ToolsEnvelope`, `ToolsResample` | `show*Dialog()` dialogs `:3104-3105`, `:3130-3133` → `recordActionStep` (`:3808`, `:4735`, `:4991`, `:5236`, `:5481`, `:5674`) | `step.run` (the dialog becomes a form; `envelope` after the `case 'e'` fix) | 0 |
| Effects | `ToolsGenSilence`, `ToolsGenNoise`, `ToolsGenTone` | dialogs `:3114-3116` → `recordActionStep` (`:4350`, `:4505`, `:4626`) | `step.run {type:"gen*"}` | 0 |
| Effects | `ToolsNoiseSuppress` | `showNoiseSuppressDialog()` `:3118` → `runNoiseSuppressAsync` `:6255` | `step.run {type:"noiseSuppress"}` (sync, lot 0); `ai.noiseSuppress` as a job (lot 3) | 0 / 3 |
| Effects | `ToolsSplitSpectralSelCfg`, `ToolsSplitSpectralSelExec` | `showSplitConfigDialog()`, `runSplitExecute()` `:3108-3113` | `edit.splitSpectralSel {mapping}` | 6 |
| Effects | `ToolsStemSeparate`, `ToolsSuperResolve` | `showStemSeparationWindow/showSuperResolutionWindow` `:3106-3107` | `ai.stems.run`, `ai.superRes.run` (`WindowsOnly`) | 7 |
| Actions | `ActionsRunSelected` | `runAction(index)` `:3120` | `steps.run` (lot 0); `actions.list/run {name}` (lot 6) | 0 / 6 |
| Actions | `ActionsBatch` | `showActionBatchWindow` `:3123` → `sc::runBatch` (`src/Actions.cpp:649`) | `actions.batch {name, inDir, outDir}` | 6 |
| Window | `WindowPanelBase+i` | `setPanelVisible/isPanelOpen` `:2785-2790` | `ui.panel.set {id, visible}` (`NeedsUi`, via `UiBridge`) | 1 |
| Window | `WindowAllowMultiple` | `app.settings.allowMultipleInstances` + `saveSettings` `:2796-2799` | `settings.set {multi_instance}` | 6 |
| Help | `HelpAbout` | `showAboutDialog` `:3135` | `session.hello` (version, build); no dialog | 1 |

**Totals**: lot 0: 24; lot 1: 23; lot 4: 5; lot 6: 36; lot 7: 2; dead: 1 — i.e. 91. At the end of lot 1, 47 MenuIDs (52 %) have a JSON equivalent; at the end of lot 6, 88 (97 %), the remaining two being Windows-only (lot 7).

**Outside MenuIDs** (panel and mouse actions, to be covered for "enabling the features" to be complete): CSV markers (`MarkerManager::saveCsv/loadCsv`, `src/MarkerManager.cpp:745`, `:757` → `markers.exportCsv/importCsv`, lot 4); tables (`serialiseTables`, `src/Tables.cpp:665` → `tables.*`, lot 6); drawings, clone pick and selection gestures (`startClonePick/endClonePick/clonePickClick`, `startSelGesture/selGestureClick`, `src/Application.cpp:1669-1799` → `drawing.*`, `clone.*`, lot 6; the gestures themselves remain mouse-only); transcription from the panel (`:6516` → `ai.transcribe`, lot 3); Python script from the panel (`:260` → `python.run`, lot 6); named selections (`SavedSelections` → `selection.saved.*`, lot 6).

The audit working files are kept outside the repository.
