# Lot 7 ("core" part) — hardening of the Python bridge, `result.json`, stems / SR services

Branch `ai-control-hardening` (fork `nicolasmarcotteTaintin/SpectroCraft`), created from `ai-control`
= `76de144`. Work done in parallel with lot 0+: nothing here depends on the `CommandRegistry`;
lot 7 will only have to write thin wrappers (see "To do at merge time").

## Done

### 1. Hardened Python bridge (`src/PythonScripts.cpp`, `include/PythonScripts.h`)

- **`runProcessEx(cmd, args, RunOptions, shouldContinue) -> ProcessResult`** (`started`, `cancelled`,
  `timedOut`, `exitCode`, `stdoutText`, `stderrText`, `elapsedMs`). `runProcess()` becomes a
  wrapper with the old contract (no timeout).
  - **Timeout** `RunOptions::timeoutMs` (0 = none). Timeout and cancellation kill **the tree**:
    - Windows: *job object*, process created `CREATE_SUSPENDED`, assigned to the job then resumed;
      `TerminateJobObject`, then a bounded wait for the job to be empty (files released);
      `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (also covers a SpectroCraft crash);
    - POSIX: own process group (`setpgid` on both the child and parent sides), `SIGTERM` to the group,
      `killGraceMs` delay (1 s), then `SIGKILL` to the group; `PR_SET_PDEATHSIG` in addition on Linux.
  - `killTreeOnExit` (true by default): grandchildren still alive when the script exits normally
    are also killed (see Deviations).
  - **stdin** = `NUL` (Windows) / `/dev/null` (POSIX): a script that reads stdin gets EOF.
  - **Environment**: `PYTHONUTF8=1`, `PYTHONIOENCODING=utf-8` (sorted UTF-16 block +
    `CREATE_UNICODE_ENVIRONMENT` under Windows; `envp` + `execve` under POSIX, `PATH` lookup
    done before `fork`, no non-*async-signal-safe* call in the child).
  - **Decoding** `decodeProcessOutput`: UTF-8 if valid, otherwise ANSI code page (Windows) / Latin-1; a
    NUL byte becomes U+FFFD instead of truncating.
  - **Paths** with spaces / non-ASCII: UTF-16 command line with the exact quoting of
    `CommandLineToArgvW` (Windows), UTF-8 argv (POSIX). Tested on macOS (see Tests).
  - `exec` failure detected under POSIX too (*close-on-exec* pipe) → `started=false` (before: true
    with code 127).
  - Windows: `CREATE_NO_WINDOW` (no more console flashing on every script).
- **`scriptSha256(file)`**: SHA-256 of the **bytes on disk** (what gets executed; `replaceWithText`
  writes CRLFs, so it is not the hash of the editor's text). `src/AI/Sha256.cpp` is
  now compiled in every configuration (it used to be compiled only with AI).
- **`isUnderScriptFolder(script, &reason, folder = scriptFolder(), &resolved)`**: symbolic
  links resolved (`realpath` / `GetFinalPathNameByHandleW`), extension checked on the target;
  stable reasons `not_found`, `not_python`, `outside_script_folder`. `resolved` = path to
  execute and hash (the same as the one checked).
- **`runScript(pythonCmd, script, exchange, options, shouldContinue) -> ScriptRunReport`**: a complete
  run (process + `out.wav` + `result.json`) without touching any document.
- **`ScopedScriptRunCapture`** (thread-local, nestable): `Application::runPythonScript` keeps its
  signature; a caller that opens a capture on its thread imposes its `RunOptions` (timeout) and
  receives the `ScriptRunReport` (hash, interpreter, code, outputs, `result.json`, `outWavPresent`,
  `audioChanged`). Without a capture (GUI, recorded actions): behaviour unchanged, no limit.
- `src/AudioCpp.cpp` (`runProcessWithPipes` only): stdin = `NUL`, read ends not
  inherited. Nothing else touched in this file.

### 2. `result.json` (contract version 1)

The script still receives `python <script> <in.wav> <out.wav> <meta.json>`. `meta.json` additionally
contains `inputWav`, `outputWav`, `resultJson` (paths) and `resultContract` (the schema). The script
**may** write a UTF-8 JSON object at `meta["resultJson"]` (same folder as `out.wav`):

```json
{
  "version": 1,
  "timeBase": "input",
  "markers": [ {"time": 1.25, "end": 2.0, "label": "climb"}, {"time": 3.5, "label": "click"} ],
  "text": "Peak -3.1 dBFS at 1.25 s",
  "measures": { "rms_db": -18.2, "peak_db": -3.1 },
  "tables": [ {"name": "bands", "columns": ["Hz", "dB"], "rows": [[1000, -20.5], [2000, -22.0]]} ]
}
```

- Everything is optional; unknown fields and `null` are ignored (kept in `ScriptResult::raw`).
- `timeBase`: `"input"` (default, seconds from the start of `in.wav`) or `"document"`. The bridge
  computes `docTime` / `docEnd` (document seconds) for each marker.
- `markers[]`: finite `time` ≥ 0 mandatory; `end` optional (≥ `time`; absent, `null` or
  negative = point marker); `label` optional string; ≤ 100,000 markers.
- `measures`: free-form object. `tables[]`: non-empty `columns` (strings), each row of `rows` has
  as many values as there are columns.
- File ≤ 16 MiB. NaN/Infinity are not JSON (Python writes them by default): the contract says
  to write `null`.
- **`out.wav` becomes optional** when `result.json` exists (analysis script). An invalid
  `result.json` makes the run fail, document intact. Neither one nor the other → failure ("wrote neither…").
- Existing scripts (examples, user scripts) work without change.
- Contract documented in `resultContract()` (embedded in `meta.json`), `resultContractText()`
  (help text) and the Add button's template (`newScriptTemplate`). The examples are not modified.

### 3. Headless stems / super-resolution services (`src/AI/StemService.*`, `src/AI/SuperResService.*`)

- `sc::StemService::validate(request, &code)` / `run(request, progress, cancel) -> Result`
  (`ok`, `cancelled`, `errorCode`, `error`, `detail`, `stems[]` {name, file, rate, channels,
  frames}, `merged` = `sc::MergeResult` at the source rate). Request: mode (SamAudio /
  HtDemucs6 / HtDemucsFt), descriptions + anchors (document time), binary (default
  `acpp::binaryPath()`), model, backend, `extractFromOriginal`, `extStartSecs`, `sourceRate`,
  `input` (channels), `workDir` (empty = temporary, deleted; otherwise kept and `stems[].file` filled in).
- `sc::SuperResService`: same (FlashSR / AudioSR, `resampleToSourceRate`, `outputFile`, `toolRate`).
- Validation codes: `invalid_mode`, `no_description`, `invalid_backend`, `binary_missing`,
  `model_missing`, `empty_input`, `ragged_input`, `invalid_rate`, `invalid_range`; run codes:
  `cancelled`, `io_error`, `tool_failed`, `no_output`, `unsupported_platform`.
- No UI or `MessageManager` call: callable from any worker thread.
- The windows read the range (as before, on their worker), call the service, then do the
  commit on the message thread exactly as before (same status texts, progress,
  error messages and detail dialogs, cancellation, cleanup of the temporary folder).
- Still Windows-only (`acpp::run*` are stubs elsewhere): `run()` validates then returns
  `unsupported_platform` on macOS / Linux; `validate()` is cross-platform.

## Files

New: `include/AI/StemService.h`, `src/AI/StemService.cpp`, `include/AI/SuperResService.h`,
`src/AI/SuperResService.cpp`, `tests/regression/hardening.inc`, this file.
Modified: `include/PythonScripts.h`, `src/PythonScripts.cpp`, `src/Application.cpp`
(`runPythonScript` only), `src/AudioCpp.cpp` (`runProcessWithPipes` only),
`src/StemSeparation.cpp`, `src/SuperResolution.cpp`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`,
`tests/UnitTests.cpp` (one `#include` line + one `SC_RUN_TEST` line).

## Tests

`tests/regression/hardening.inc`, test "regression: hardening" (in the `--run-unit-tests` suite
run by ctest):

- `result.json`: complete document, input/document time base, top-level `null`,
  15 invalid documents refused, missing file (= no result), UTF-8 disk read;
- `meta.json`: exchange paths + contract present, legacy invocation unchanged;
- hash: FIPS "abc" vector on the bytes, stable, changes with the content, missing file;
- allowlist: in the folder, subfolder, outside the folder, non-`.py`, missing, symbolic link
  to the outside, `.py` link to a non-script (skipped if links cannot be created: Windows
  without privilege);
- services: each validation code, relative anchors, `run()` from a `std::thread` (Windows:
  reaches the CLI with a fake binary → `tool_failed` and work folder kept; elsewhere:
  `unsupported_platform`);
- process: missing executable = not started (all OSes), decoding (invalid UTF-8, NUL); with
  Python: timeout of a sleeping script, **grandchild that ignores SIGTERM killed with the tree**,
  cancellation, stdin = EOF, `PYTHONUTF8=1` + UTF-8 stdout, path with spaces/accents/CJK round
  trip in the script, UTF-8 stderr;
- `Application::runPythonScript`: analysis-only script (`result.json`, no undo, markers
  remapped to the document, hash reported), `Threshold.py` example unchanged, invalid `result.json`,
  time limit through the capture, nesting / thread-local behaviour of the capture.
- The parts that require Python are skipped (`scSkip`) without an interpreter, like the existing Python
  tests.

Local results (macOS arm64, Debug, `-DSPECTRO_ENABLE_AI=OFF -DSPECTRO_ENABLE_FFMPEG=OFF`,
`ctest -j 6`):

- reference (untouched branch): 4614/4615 — failure `views#W4` (5 px repaint);
- after: 4725/4726 — same `views#W4` failure, pre-existing and unrelated (passes alone:
  `SC_TEST_FILTER="regression: views"` → 33/33); "regression: hardening": all
  checks pass.
- Note: on a machine where **several checkouts run ctest at the same time**, the shards
  share `$TMPDIR/SpectroCraft/SpectroCraftTests-tmp-shardN` and delete each other's
  files (random failures in unrelated tests). Run with a private `TMPDIR`.

CI (`gh workflow run build.yml --ref ai-control-hardening`, `workflow_dispatch`):

- run 42162969575 on `8a9cf6f`: **windows-ai ✓, macos-ai ✓, linux-ai ✓** (MSVC Release, AppleClang
  Debug, GCC Debug, AI enabled; ctest 100 %).
- The previous runs (`adec113`, `db710c5`): macOS / Linux ✓, but **Windows stuck for 30 min** in
  the services test. Cause: the test gave the service a text file named
  `audiocpp_cli.exe`; `CreateProcessW` on a non-PE image can raise a system error box
  (*hard error*) that nobody closes on a headless runner. The test now uses
  `System32\where.exe` (rejects the arguments, exits immediately with an error). **Product consequence**: a
  corrupted `audiocpp_cli.exe` (truncated download) can do the same thing in the app; the
  SHA-256 hashes from the "catalogue" agent limit the risk, and `SetErrorMode(SEM_FAILCRITICALERRORS
  | SEM_NOGPFAULTERRORBOX)` around `CreateProcessW` (or a check of the `MZ` header before
  launching) would be a simple protection to add in `runProcessWithPipes`.

## Deviations

- **Base**: `76de144` (= `ai-control` on the fork) and not the requested `5a80afd`: `5a80afd` is an
  earlier local version of the same commit (only `DECISIONS.md` differs); starting from `76de144` avoids
  a history diverging from `ai-control`.
- **`tests/CMakeLists.txt`** modified (not on the list): the console target `SpectroCraftTests` compiles
  `StemSeparation.cpp` / `SuperResolution.cpp`, which now call the services; without the
  new `.cpp` files (and `Sha256.cpp`), it would no longer link.
- **Embedded fix (stems)**: the old code computed the padding length **before**
  resampling the tracks to the source rate, which truncated htdemucs stems
  (44.1 kHz) from a 48 kHz source by about 8 % (and added silence in the other direction). The service pads
  after resampling.
- **`killTreeOnExit` true by default**, for the GUI too: a script that deliberately leaves a
  process running (player, viewer launched with `subprocess.Popen`) will see it killed when it exits.
  Set the option to false in a capture if that is intended.
- The Python panel (GUI) **does not display** `result.json` yet: an analysis-only script succeeds
  with no visible effect (and `MainWindow` needlessly rebuilds the mipmaps). The help text and the
  template say so. `MainWindow.cpp` was not touched (out of scope).
- `include/Application.h` was not touched: its `runPythonScript` comment still says that
  the script "must" write the output WAV (to be updated at merge time).
- In an action batch under a single capture, only the report of the last Python step survives.

## To do at merge time (main agent)

1. **`python.run`** (`LongRunning` job, mutating):
   - `isUnderScriptFolder(script, &reason, scriptFolder(), &resolved)`; refusal →
     `confirm_required{reason}` (unless `confirm:true`); execute **`resolved`**;
   - on the job's worker: `sc::py::ScopedScriptRunCapture cap (opts)` with `opts.timeoutMs` coming
     from the `params` (proposed default 300 s), then `app.runPythonScript (resolved, resolved.loadFileAsString (),
     pyCmd, progress, &err)` (the hop to the message thread for the commit is already done);
   - response: `cap.report ()` → `scriptSha256`, `exitCode`, `timedOut`, `elapsedMs`, `audioChanged`,
     `outWavPresent`, `result.raw` (or `markers` with `docTime/docEnd`, `text`, `measures`, `tables`),
     bounded `stdout/stderr` tails;
   - decide whether the markers from `result.json` are added to the document (undoable) or only
     returned (not done here);
   - `findPython` off the message thread (REPORT finding, `MainWindow.cpp:6334`).
2. **Journal** (`python.run`): `scriptPath` (resolved), **`scriptSha256`**, `interpreter`, `exitCode`,
   `timedOut`, `elapsedMs`, `audioChanged`, number of markers / tables; **never** `text`,
   `measures` nor the content of the tables unless `journal_verbose` (§5.3).
3. **`ai.stems.run`** / **`ai.superRes.run`** (jobs, `WindowsOnly`): build the `Request` (range
   read on the message thread or via `readRange` like the windows, model resolved with
   `acpp::listModels` / `listDemucs6Models` / `demucsFtModelDir` / `flashSrModelFile` /
   `listAudioSRModels`), `validate()` → `invalid_params{code}`, `run(req, progressToJob,
   jobCancellation)` on the worker, then commit on the message thread with
   `createDocFromMerged` / `applyStemSeparationResult` / `applySuperResolutionResult` (same rule
   "source document changed → new document" as the windows); `errorCode` → protocol error
   code, `stems[]` in the response if `workDir` is provided.
4. Update the comment of `Application::runPythonScript` in `include/Application.h`
   (`out.wav` optional with `result.json`, capture).
5. Expected conflicts, trivial: `tests/UnitTests.cpp` (`#include "regression/…"` and
   `SC_RUN_TEST` lines), `src/CMakeLists.txt` / `tests/CMakeLists.txt` (source lists).
