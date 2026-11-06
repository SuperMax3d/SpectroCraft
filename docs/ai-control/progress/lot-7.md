# Lot 7 — hardening, Python bridge, legacy flags, stems/SR (Windows)

Status: **done**, except the actual execution of stems/SR (Windows only, never executed here: CI).
Core (hardened runProcess, `result.json`, stems/SR services, hash, allowlist) delivered by the
parallel agent `ai-control-hardening` (see `lot-7-core.md`).

## Merge

`ai-control-hardening` merged at `db710c5`; trivial conflicts (source lists, `UnitTests.cpp`);
`Sha256.cpp` appeared twice in each target (moved out of the AI block by both agents):
deduplicated. Comment of `Application::runPythonScript` updated (`out.wav` optional with
`result.json`, capture).

## Commands (`src/control/commands/Python.cpp`)

- **`python.run {script, timeoutMs (default 300 s), addMarkers, select, channels, confirm}`** (alias
  `python`): `isUnderScriptFolder` (links resolved); outside the scripts folder → `confirm_required
  {reason:"outside_script_folder"}` then the path perimeter; job: `findPython` **on the worker**,
  `ScopedScriptRunCapture` (timeout that kills the process tree, stdin closed, UTF-8), patch attributed to
  `python.run`/client. Result: `scriptSha256`, `interpreter`, `exitCode`, `timedOut`, `elapsedMs`,
  `audioChanged`, `outWavPresent`, `resultJson` (markers in document time, text, measures, tables),
  `markersAdded`, tails of stdout/stderr (4 KiB). Timeout → `timeout` code. The **journal** keeps
  `script`, `scriptSha256`, `interpreter`, `exitCode`, `timedOut`, `audioChanged`, `markerCount`… never
  `text`, `measures` nor the tables (allowlist in `ControlState`).
- **`ai.stems.run {mode samAudio|htdemucs6|htdemucsFt, descriptions[{text, anchors}], model, backend,
  extractFromOriginal, output newDoc|replace|append, select, channels}`** and **`ai.superRes.run {mode
  flashsr|audiosr, model, backend, output, …}`**: `WindowsOnly` (elsewhere `unsupported_platform`);
  range read on the message thread, `validate()` → `invalid_params`/`model_not_installed {code}`,
  `run()` on the worker with the job's progress/cancellation, commit on the message thread through
  `applyStemSeparationResult` / `applySuperResolutionResult` with the windows' rule (document
  modified in the meantime → new document). Default `backend` = `ai.setDevice {engine:"audiocpp"}`.
  **Not executed** here (macOS): only compilation and the `unsupported_platform` refusal are tested;
  the Windows CI compiles the full path.

## Legacy flags

Reliable exit codes for `--test-spectral`, `--test-load`, `--test-noise-suppression`,
`--test-transcribe`, `--test-batch-transcribe`, `--test-ns-failure-alert`: 2 = usage, 1 = failure
(opening, reading, pipeline, model installation, transcription, at least one failed file in the
batch), 0 otherwise; `--test-batch-transcribe` runs on an **isolated settings store** (it used to rewrite the
real `SpectroCraft.recent`). (The report proposed reimplementing them on top of `--json`: unnecessary once
the codes were fixed, since `--json` covers the rest; they remain for compatibility.)

## Security review (closed list of §8)

| Item | Status |
|---|---|
| `control_roots` perimeter and links | POSIX: `realpath` on every existing component ✓; Windows: last component only (debt); `control_roots`/`control_enabled` **not modifiable through the API** (`settings.set` refuses) ✓ |
| Token | **replaced**: 256 bits from the OS CSPRNG (`std::random_device`) instead of `juce::Uuid` (non-cryptographic generator); constant-time comparison; never journaled or logged (`sanitizedParams`, the hello is not journaled); Windows: default ACL of the profile (no explicit ACL, threat model §5.2) |
| Limits | 1 MiB line, 256 KiB params, 4 KiB and JSON depth ≤ 64 before authentication, 10 s to authenticate, 4 clients, event queue 1000 + `events.dropped`, sending bounded to 5 s ✓; generators and `doc.new` refused beyond 4 GiB of audio (the fuzz showed that terabytes could otherwise be requested) |
| Journal content | metadata only; no transcription text (lot 3 test), no Python text/measures (test), no table values ✓ |
| Script execution | scripts folder + `confirm` + journaled SHA-256 hash + timeout ✓; the `python` step of `step.run` checks folder/roots/`confirm` but runs without the capture (no timeout): use `python.run` |
| Downloaded binaries | SHA-256 mandatory before `installable:true` (catalogue); CUDA blocked (missing DLLs, pre-existing) ✓ |
| `multi_instance=0` | `--control` skips `handleStartup` ✓ |
| **Fuzz** 10,000 random NDJSON lines | **found a crash**: `juce::var::isObject()` is true for an **array**, `getDynamicObject()` then returned null (`"params":[]` → segfault, denial of service by an authenticated client or a `--json` line). Fixed everywhere (`isObj()` = `getDynamicObject() != nullptr`); the fuzz passes ✓ |
| **Load** 1000 sequential `app.state` over the real socket | **253 ms in total, p50 0.24 ms, p99 0.35 ms** (thresholds: < 10 s, p99 < 50 ms) ✓; memory stability over 10,000 commands was not measured (no reliable measurement tool in the suite) |

## Tests (`tests/regression/lot7_control.inc`; "control load" is serial)

`python.run` (analysis script: hash, `result.json`, marker added in document time, audio intact,
measures returned, journal with hash and no text; script outside the folder → `confirm_required`; script that
sleeps → `timeout` in < 15 s), stems/SR (`unsupported_platform` outside Windows, validation under Windows),
fuzz of 10,000 lines (all answered, app alive), load.

Results: see the bottom.

## Debts

- Stems/SR: never executed here; the CUDA binaries remain blocked (pre-existing DLL
  extraction bug, reported by the catalogue agent); the dialogs still offer them.
- No measurement of memory drift over 10,000 commands.
- `step.run {type:"python"}` without a timeout (prefer `python.run`).

## Results

macOS arm64, Debug + AI, Python 3 present: "lot7_control + control load + control + menus" 1161/1161;
full suite **6571/6571**, 10/10. Load (after switching to the CSPRNG token): 1000 `app.state` in
252 ms, p50 0.24 ms, p99 0.32 ms. Fuzz: 10,000 lines in ~1 min, no loss, app alive.
