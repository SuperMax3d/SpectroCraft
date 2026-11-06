# Lot 3 — AI models, devices, noise suppression and transcription as jobs

Status: **done**. Core (catalog, SHA-256, strict devices) delivered by the parallel agent
`ai-control-catalog` (see `lot-3-core.md`); this file covers the merge and the command layer.

## Merge

- `ai-control-catalog` merged at `23c5d28` (CI green on all three platforms on its side).
  Trivial conflict in `tests/UnitTests.cpp` (neighbouring `#include`/`SC_RUN_TEST` lines), resolved by
  keeping both sides.
- Fixed while merging: the console target `SpectroCraftTests` (`tests/CMakeLists.txt`) now lists
  `src/AI/Sha256.cpp` outside the AI block and `src/AI/ModelCatalog.cpp` inside the AI block (flagged by the
  core's note, outside its scope).

## Commands (`src/control/commands/Ai.cpp`, flag `AiBuildOnly`)

| Command | Behaviour |
|---|---|
| `ai.models.list` (`Query`) | `ModelCatalog::listJson()`: 32 entries, engine, family, installed, size, checksum, backends, `installable` + reason |
| `ai.models.install {modelId}` | `installPlan` (immediate refusal: unknown, not installable, platform, busy) then a non-mutating **job**; response `{job, sizeBytes}`; progress + cancellation passed through to the catalog |
| `ai.models.verify {modelId}` | job that re-hashes (SHA-256) |
| `ai.models.remove {modelId, confirm}` | `confirm_required` with the preview ("Removes 148 MB under …"); `busy` if a running job uses this model |
| `ai.devices {init}` | without `init`: nothing is loaded (`initialised:false`); `init:true` → job that loads OpenVINO and returns the actual list |
| `ai.setDevice {engine, device, persist}` | job (the actual list requires the runtime); missing device → `invalid_params {expected:"one of [auto, CPU]"}`; OpenVINO → `app.aiDevicePref` (+ `saveAiSettings` if `persist`); audio.cpp → session preference (`ControlState::audiocppBackend`, passed on by the stems/SR commands of lot 7) |
| `ai.noiseSuppress {modelId, device, attenLimDb, df3PostFilter, copyNoise, autoDownload, select, channels}` (alias `noiseSuppress`) | same engine as `step.run noiseSuppress`: **job**, one history entry, result `{deviceUsed, description, undoDepth, selection, …}` |
| `ai.transcribe {modelId, language, translate, fast, autoDownload, channels}` (alias `transcribe`) | job, segments **returned** (`segments[]`, `segmentCount`, `storedInDoc:true`); the text never goes into the journal |
| `ai.batchTranscribe {folder, …}` | job on `Application::batchTranscribeFolder` (files loaded without opening them), summary `{total, done, skipped, failed, failures}`; folder subject to the path perimeter |
| `transcription.get` (`Query`) | the document's segments, `source ∈ sctt|sidecar|memory|none` |

Common rules (`src/control/commands/Steps.cpp`, shared with `step.run`):

- **Strict device for the API** (coordinator's request): in the worker, `sc::ai::resolveAiDevice
  ("openvino", pref)`; an unavailable device makes the job fail (`invalid_params`), never a silent
  fallback. Without `device`, the app's preference is frozen at launch. The lenient resolver
  `sc::resolveAiDevice` remains the one used by the dialogs and by the replay of recorded actions (`applyActionStep`,
  lot 0 fix), for compatibility.
- Unknown model → `invalid_params {path: "step.params.modelId", expected: "one of [...]"}`; model missing
  with `autoDownload:false` → `model_not_installed {missing, sizeBytes, hint: "call ai.models.install"}`.
- Jobs carry the command id (`ai.noiseSuppress`, not `step.run`): `history.list` attributes the
  entry to `ai.noiseSuppress` and to the client (`source:"api:<client>"`).
- `buildNsFailureDetails` (moved out of `MainWindow.cpp` by the core) is not yet attached to the
  API's NS errors (`details.manualInstall`): minor debt.

## Tests (`tests/regression/ai_commands.inc`, "regression: ai_commands")

Registration of the 10 commands, aliases; list (≥ 15 models, whisper-base with checksum, audio.cpp
assets not installable outside Windows); unknown model; `remove` without confirm; `verify` (job,
`verified:true`); `ai.devices` without/with init; `ai.setDevice GPU` without a GPU → `invalid_params` with the
actual list; `ai.setDevice CPU` applied; NS unknown model, `model_not_installed`, **GPU device missing
→ strict error without applying anything**, NS `auto` → job done, device reported, 1 undo, history attributed
to `ai.noiseSuppress`/client; transcription → segments, consistent `transcription.get`, text absent from the
journal. Explicit skips if a model is missing (no network required).

Results: see the end of the file.

## Findings to escalate (SUMMARY)

- **Pre-existing bug** (spotted by the catalog agent): the audio.cpp CUDA 12.4/13.3 binaries cannot
  start once installed, because `installBinaryFromZip` only extracts `audiocpp_cli.exe` and not
  `ggml.dll`/`ggml-base.dll`. The catalog marks them `installable:false`, but the Stem/Super
  Resolution dialogs still offer them. Not fixed here (Windows, to be done with Max).
- Migration of all AI to audio.cpp (SuperMax3d's request): **not done**, as requested; the
  `ai.*` API depends only on the catalog (`engine` field = data), it will survive the migration.

## Results

macOS arm64, Debug + AI, models `denseunet`, `deepfilternet2`, `whisper-base` present:
"regression: ai_commands" + AI + control: 927/927; full suite `ctest -j 8`: **5717/5717**,
10/10 (including the catalog agent's `regression: ai_catalog`). No skips in `ai_commands` on this
machine (no OpenVINO GPU on macOS: the "GPU missing" cases were therefore exercised).
