# AI control — summary of the plan execution (lots 0 to 7)

Date: 2026-11-05/06. Branch `ai-control` of the fork `nicolasmarcotteTaintin/SpectroCraft` (nothing pushed
to SuperMax3d, no PR). Machine: Nicolas's Mac (macOS 26.6 arm64, AppleClang 21, Debug + AI)
in an isolated worktree, **not** the Linux cloud agent called for by the instructions (see `baseline-macos.md`).
Linux and Windows: GitHub CI (`gh workflow run build.yml --ref ai-control`).

## Status per lot

| Lot | Status | Commits (`ai-control`) | Note |
|---|---|---|---|
| 0 — registry, `step.run`, journal, `--json`, fixes | **done** | `f1cfa62` (fixes only, separable PR), `693f3f3` | `lot-0.md` |
| 1 — loopback server, jobs, `UiBridge` | **done** | `e423b9d` | `lot-1.md` (independent review + fixes) |
| 2 — Python MCP adapter | **done** | merge `80a1f4e` (`ai-control-mcp` @ `6779c5c`), `cda3390` | `lot-2.md` |
| 3 — AI models, devices, NS/transcription as jobs | **done** | `502128c` (merge of `ai-control-catalog` @ `23c5d28` + commands) | `lot-3-core.md`, `lot-3.md` |
| 4 — measurements | **done** | merge `2985e97` (`ai-control-measures` @ `8d4ace9`), `a217236` | `lot-4-core.md`, `lot-4.md` |
| 5 — persistent journal, enriched Patch, replay | **done** | `5679059` | `lot-5.md` |
| 6 — splitting of the commands, menu migration | **partial** | `7770ce9` | `lot-6.md` (exact list of what remains) |
| 7 — hardening, Python, flags, stems/SR | **done** (stems/SR not run: Windows) | merge `630dc66` (`ai-control-hardening` @ `db710c5`), `7fac071`, merge `3a12d55` (@ `0306a32`) | `lot-7-core.md`, `lot-7.md` |

Parallel branches merged (work by other agents; integrating them is my responsibility):
`ai-control-mcp` @ `6779c5c`, `ai-control-catalog` @ `23c5d28`, `ai-control-measures` @ `8d4ace9`,
`ai-control-hardening` @ `db710c5` then again @ `0306a32` (merge `3a12d55`: Windows test of the
services that runs `System32\where.exe` instead of a fake `audiocpp_cli.exe`). Fixed while integrating them: `tools/mcp/smoke.sh` (repository path
with spaces), source lists of the `SpectroCraftTests` console target (ModelCatalog, Sha256
duplicated), trivial conflicts in `tests/UnitTests.cpp`. Commit `502128c` is both the merge of the
catalog and the command layer of lot 3.

## Tests (macOS arm64, Debug + AI, `ctest -j 8`)

| Stage | Checks |
|---|---|
| Baseline (`76de144`) | 4859/4859 |
| Lot 0 | 5283/5284 (1 failure: single-instance pipe under load, see below) |
| Lot 1 | 5482/5482 |
| Lot 3 | 5717/5717 |
| Lot 4 | 5998/5998 |
| Lot 5 | 6026/6026 |
| Lot 6 | 6410/6410 |
| Lot 7 merge | 6521/6521 |
| Lot 7 (final) | **6571/6571** |

Real end to end (app launched with `--control`, real window): NDJSON script (lot 1),
`tools/mcp/smoke.sh` (lot 2), **MCP stdio** client ↔ `spectrocraft_mcp` ↔ app (lot 2): OK.
`pytest tools/mcp/tests`: 98 passed + 4 skipped; **102 passed** with `SPECTROCRAFT_EXE`.
Load: 1000 `app.state` over the socket in ~250 ms, p99 ~0.3 ms. Fuzz: 10,000 random NDJSON
lines without a crash (after fixing the crash it found).

Environmental instabilities observed: (1) several agents were testing at the same time on this Mac and the
suite's temporary folder (`SpectroCraftTests-tmp-shard<k>`) was shared by all checkouts →
**fixed** (the name includes the PID); (2) `single instance mode` (fixed 2 s delay of the FileForward pipe)
sometimes fails under load, passes on its own; (3) after the fuzz crashes, macOS showed at the next
launch a modal alert "reopen windows?" that blocked the suite:
`defaults write com.SpectroCraft.SpectroCraft ApplePersistenceIgnoreState -bool YES` was set on this Mac.

## CI (workflow_dispatch on the fork)

| Run | Tip | windows-ai | macos-ai | linux-ai | mcp-tests |
|---|---|---|---|---|---|
| 42152420659 | lot 0 `693f3f3` | ✅ | ✅ | ❌ `int64_t`→`juce::var` ambiguous (fixed in lot 1) | — |
| 42156792380 | lot 1 `e423b9d` | ✅ | ✅ | ✅ | — |
| 42158668184 | lot 4 `a217236` | ✅ | ✅ | ✅ | ✅ ubuntu + macos |
| 42160626670 | lot 6 `7770ce9` | ✅ | ✅ | ✅ | ✅ ubuntu + macos |
| 42162848694 | lot 7 `7fac071` | ⏹ cancelled (stuck in the stems/SR services test, system error box; fixed by `8a9cf6f`) | ✅ | ✅ | ✅ ubuntu + macos |
| 42164487280 | final `3a12d55` | ✅ | ✅ | ✅ | ✅ ubuntu + macos |

## Main deviations

- macOS machine instead of Linux; local build and tests on macOS only, Linux/Windows through CI.
- `JobManager` brought forward to lot 0 (needed by the `--json` timeout test).
- Lot 6 partial (effect dialogs, `applyActionStep` facades, File/View/Transport menus not migrated).
- Journal by polling (around commands, at the end of a job, 30 Hz in the window when the server
  is running) rather than by scattered `onChange` hooks.
- History tab: order of the future rows fixed (pre-existing bug: a click replayed a different entry
  than the one displayed).
- `tools/mcp/sc_client.py` written by the MCP agent (lot 2), not in lot 1.

## Findings to report to SuperMax3d

1. **Pre-existing bug** (spotted by the catalog agent): the CUDA 12.4/13.3 audio.cpp binaries do not
   start once installed (`installBinaryFromZip` only extracts `audiocpp_cli.exe`, not
   `ggml.dll` / `ggml-base.dll`). The catalog marks them `installable:false`, but the
   Stem/Super Resolution dialogs still offer them.
2. **Windows product risk** (spotted by the hardening agent): a corrupted or
   truncated `audiocpp_cli.exe` can make `CreateProcessW` raise a system error box (*hard error*) that freezes the app
   the same way it froze the CI runner. Protection proposed in `lot-7-core.md`: `SetErrorMode(SEM_FAILCRITICALERRORS
   | SEM_NOGPFAULTERRORBOX)` around `CreateProcessW` in `runProcessWithPipes`, or checking the `MZ` header
   before launching. Not applied (out of scope, to be validated on Windows).
3. **Pre-existing bug** fixed: History tab, future rows in the wrong order (lot 5).
4. Fixes independent of AI control, deliverable as a separate PR: `f1cfa62` (envelope, `auto`
   device, `noiseSuppress` without AI, dispatch by 1st letter, worker-side commits of `transcribe` and
   `noiseSuppress`); exit codes of `--test-*` and `--test-batch-transcribe` on the real store
   (lot 7); suite temporary folder shared between checkouts (lot 0).
5. Migration of all AI to audio.cpp: **not done** (separate request); the `ai.*` API depends only
   on the catalog (`engine` = data) and will survive the migration.

## Questions to settle (Nicolas / Max)

- Q10/Q13/Q17 of the report (left open): actual priority of stems/SR; fork or upstream
  contribution (lots 6-7 touch `MainWindow.cpp`, costly to rebase); are Python 3.11+ and `mcp>=1.6,<2`
  installable on locked-down workstations?
- Lot 6: should the effect dialogs call the registry (form → `runMenuCommand`) and the
  action batch go through `--json` (facades)?
- Path scope: add the programs' network shares right away (Q18)?
- `killTreeOnExit` true by default for the GUI too (lot 7 core): a script that deliberately launches
  a background process will see it killed.
- Protocol `1` still "experimental": when to freeze it (Q11)?

## Remaining work

- Lot 6: see `lot-6.md` (facades, 19 `recordActionStep` sites in the dialogs, File/View/
  Transport/Selection/Record menus, `export.snapshot`, `drawing.*`, `clone.*`, `selection.saved.*`,
  `actions.batch`).
- `export.spectrogram` with overlays (`UiBridge` → `preparePngExport`).
- Stems/SR to be run on a Windows workstation with the audio.cpp binaries and models.
- Windows: resolution of links in the middle of a path (scope), ACL of the discovery file.
- `step.run {type:"python"}` without timeout (prefer `python.run`).
- Memory drift measurement over 10,000 commands.
