# Lot 3 — "AI models, devices" core (without the command registry)

Branch `ai-control-catalog`, created from `ai-control` = `76de144` (the docs commit `5a80afd`
given in the instructions was amended into `76de144` on the fork; the only difference: two lines of
`DECISIONS.md`). Depends neither on `src/control/` nor on the `JobManager`: lot 0 of the main line
(`693f3f3`) is not included, the `ai.*` wrappers remain to be written (see "Merge" below).

## Done

1. **`sc::ai::ModelCatalog`** (`include/AI/ModelCatalog.h`, `src/AI/ModelCatalog.cpp`): a single
   view over the two registries. 32 entries: 15 from `ModelManager` (3 OpenVINO + 12 Whisper),
   4 audio.cpp binaries (`audiocpp-cli-<variant>`), 9 SAM-Audio, FlashSR, AudioSR, 2 "manual" htdemucs
   (detected only, no download source in the app).
   - Fields: `id, engine (openvino|whisper|audiocpp), family (noiseSuppress|transcribe|stems|superRes|runtime),
     displayName, folder, files[{name,url,sizeBytes,sha256}], archive (zip of the runtimes), variant,
     backends, platforms, license, source, installable, reason, reasonCode, manual`.
   - Engine = data: the migration to the audio.cpp C API (SuperMax3d's request, **not done here**)
     will add entries / a new `engine`, without touching the JSON format.
   - `status()` (stat only, no hashing), `installPlan()` (synchronous pre-check for the handler),
     `install()` / `verify()` / `remove()` **blocking**, safe from a worker, progress + cancellation
     through `CatalogProgressFn (float, status) -> bool`, per-id "busy" guard (the 4 CLI variants
     share a single key: only one `audiocpp_cli.exe`).
   - Error codes = subset of `sc::control::err`: `invalid_params`, `busy`, `cancelled`,
     `unsupported_platform`, `model_not_installed`, `failed` → a wrapper passes `CatalogOutcome` on as-is.
   - JSON §6.5: `listJson()` (`{"modelsRoot","audiocppRoot","models":[…]}`), `entryJson()`,
     `removalPreview()` (text for `confirm_required`: "Removes 148 MB under …"), install/verify/remove
     results in the `job.done` shapes of §6.5.
2. **SHA-256 of the audio.cpp assets**: the 15 referenced assets were **downloaded and hashed**
   (2026-11-05); each value equals the `digest` published by GitHub (releases) or the LFS oid
   published by Hugging Face. Added to `binaryChoices/modelChoices/flashSrChoices/audioSRChoices`
   (`include/AudioCpp.h`: fields `sha256`, `exeSha256`, `exeSizeBytes`, `catalogBlocker`).
   - `sc::acpp::downloadFile` now verifies the SHA-256 while streaming (optional parameter, otherwise
     `knownSha256ForUrl(url)`): the Stem/Super Resolution dialogs, unchanged, benefit from it.
     `commitDownload` refuses a wrong hash (the `.part` is deleted, the destination left intact).
   - `installBinaryFromZip` extracts into a temporary folder, compares `audiocpp_cli.exe` with the
     published hash of the variant, and only then replaces the binary in place (a refused zip no longer destroys
     the existing binary).
   - FlashSR URL pinned to commit `38156353` instead of `main` (otherwise the published hash could
     stop matching).
   - audio.cpp download/execution still **Windows only** (POSIX stubs unchanged).
3. **Real devices**: `sc::ai::resolveAiDevice(engine, requested, available)` (pure) +
   `resolveAiDevice(engine, requested)` / `resolveAiDevice(requested)` (real lists),
   `availableDevices()`, `audioCppBackendsForVariant()`, `devicesJson()` (§6.5 shape; without `init`,
   OpenVINO is not loaded: `"initialised":false,"devices":null`). An unavailable device is an
   **explicit error** (`invalid_params`, "device GPU is not available for openvino",
   `details.expected = "one of [auto, CPU]"`), never a silent fallback. The old
   `sc::resolveAiDevice` (CPU fallback + note) remains for the dialogs, unchanged.
4. **`SC_MODELS_DIR` on all platforms**: `ModelManager` (root), `sc::acpp::dir()`
   (`<SC_MODELS_DIR>/audiocpp`, before: never), and `SC_OPENVINO_CACHE_DIR` also on Windows.
5. **`file://` installs** in `ModelManager` (`localFileGet`: same size + SHA-256 checks,
   `.part`, cancellation): offline mirror and network-free test fixtures.
6. **`buildNsFailureDetails`** moved from `src/MainWindow.cpp` to `src/NoiseSuppression.cpp`
   (AI module; the declaration was already in `include/NoiseSuppression.h`). MainWindow: removal only.
7. `src/AI/Sha256.cpp` moves into the always-compiled sources (AudioCpp.cpp needs it even without AI).

## Hash table (audio.cpp assets)

| Asset | SHA-256 | Size (bytes) | Source |
|---|---|---|---|
| bin cpu-portable (zip) | `9654c8f66f87bda768196d934059be896399b43703b06e5f2b857430775bec3b` | 28786884 | https://github.com/0xShug0/audio.cpp/releases/download/v0.9.2-drama3-base-small/audio-v0.9.2-drama3-base-small-bin-windows-x64-cpu-portable.zip |
| ↳ `audiocpp_cli.exe` | `427875df84983e950103caa78c435e2dd3cef4b99dc0295c4f941e49598fda06` | 27326464 | (inside the zip) |
| bin cuda12.4 (zip) | `48852da40790c40f3285bca6c53ff7829b3963080f0e95b6742f868fc1d7c757` | 464268606 | …/audio-v0.9.2-drama3-base-small-bin-windows-x64-cuda12.4.zip |
| ↳ `audiocpp_cli.exe` | `ee1ae0b3dce0918f33dd9446e55c21f50120bcf4501a98a9a9a1ef4b7eaa91a9` | 26529280 | (inside the zip) |
| bin cuda13.3 (zip) | `b570efa4b17c2aa32c431c653489c009b58f57f60d68782b3b94b6c06304a9e8` | 275837338 | …/audio-v0.9.2-drama3-base-small-bin-windows-x64-cuda13.3.zip |
| ↳ `audiocpp_cli.exe` | `baf36544d8734099b5fb22c3284f5a0c4d5dba0272f36e8180a16d77bda2e7c9` | 26537472 | (inside the zip) |
| bin vulkan-portable (zip) | `9c943d406af3543cc5005d158ed583210cd7a13371bffcd4b7f262f7a6a5ad16` | 63227361 | …/audio-v0.9.2-drama3-base-small-bin-windows-x64-vulkan-portable.zip |
| ↳ `audiocpp_cli.exe` | `373e76168f317f7b8f63bf0ccdf1f2048c01cc995fdcdd507a17264d3d4e09a4` | 84764672 | (inside the zip) |
| flashsr/flashsr.safetensors | `cfa15eb8576e50cff30bc9851fc2ca8e74b6ea379fbcded03c9cc44458b1c6e2` | 356008 | https://github.com/0xShug0/audio.cpp/raw/38156353396fae68d10388c960b900a8fda8373f/assets/framework/audio_utilities/flashsr/flashsr.safetensors |
| sam-audio-small-q8_0.gguf | `82e7aaf7b8c458f67183a4924950dbb10ad1979a9c057f1a906c79752a4ba243` | 2379290272 | https://huggingface.co/audio-cpp/SAM-Audio-GGUF/resolve/main/sam-audio-small-q8_0.gguf |
| sam-audio-small-bf16.gguf | `48e51ad99e08eb976cbcbf65e1dd725b0e76b00618fd857a05dc7b7dadceb46b` | 2824105568 | …/SAM-Audio-GGUF/resolve/main/sam-audio-small-bf16.gguf |
| sam-audio-small-f32.gguf | `ec80610e076c7408dcc14ba8c7bc6c321f8d691bcf093e94882898a9dbaf34f1` | 5640890208 | …/SAM-Audio-GGUF/resolve/main/sam-audio-small-f32.gguf |
| sam-audio-base-q8_0.gguf | `e5529e940218981b67be2c58904ffca7b7eecba20b65e2e99e7d23eb5eb7bd9b` | 3095951072 | …/SAM-Audio-GGUF/resolve/main/sam-audio-base-q8_0.gguf |
| sam-audio-base-bf16.gguf | `90f658f0043859e195ef08a298dd61d1747bc65245c0768f2d0863f3e42849b2` | 4136791712 | …/SAM-Audio-GGUF/resolve/main/sam-audio-base-bf16.gguf |
| sam-audio-base-f32.gguf | `34b1fec64c1e07027e7b30feb744f59cd00bc10a02daa8f86140f0fe054242cc` | 8265738720 | …/SAM-Audio-GGUF/resolve/main/sam-audio-base-f32.gguf |
| sam-audio-large-q8_0.gguf | `2ff2d4599da19353105156582f995aa433899450c27e0274f0e45cc51369abf3` | 5029984064 | …/SAM-Audio-GGUF/resolve/main/sam-audio-large-q8_0.gguf |
| sam-audio-large-bf16.gguf | `d3582311e5a43ae1e067b3a36c2c85a5dea946757114c611892a0fdb3e67b3ee` | 7705276160 | …/SAM-Audio-GGUF/resolve/main/sam-audio-large-bf16.gguf |
| sam-audio-large-f32.gguf | `bdfe2614aca0a85eebf2304959f9783125655ecb9475e56cd180f4551d32043a` | 15401675328 | …/SAM-Audio-GGUF/resolve/main/sam-audio-large-f32.gguf |
| AudioSR-GGUF/audiosr-basic-f32.gguf | `bf92d26c638e82f5051dfd8cf5899798d64ce5366ff63bad8b5930a82fce5bff` | 6177855616 | https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/main/AudioSR-GGUF/audiosr-basic-f32.gguf |

All files: HTTP 200, size received = catalog size. The sizes were already correct.
No asset was left without a hash.

## `installable` state (built-in catalog)

- OpenVINO / Whisper (15): `installable:true` everywhere (hashes already present).
- audio.cpp, Windows: CPU portable, Vulkan portable, 9 SAM-Audio, FlashSR, AudioSR → `true`.
- audio.cpp outside Windows: `false`, `reasonCode:"unsupported_platform"`.
- **CUDA 12.4 / 13.3: `false`, `reasonCode:"blocked"`** — finding: their `audiocpp_cli.exe` imports
  `ggml.dll` and `ggml-base.dll` (shipped in the same zip), whereas `installBinaryFromZip` only extracts
  the exe; a CUDA binary installed this way cannot start (not executed: deduced from the import
  table). The "portable" variants only import the MSVC runtime. The dialogs still offer
  CUDA (StemSeparation.cpp outside my scope): to be fixed by extracting all the DLLs.
- htdemucs-6stems / htdemucs-ft: `false`, `reasonCode:"manual"` (the files exist on
  `audio-cpp/audio.cpp-gguf/HTDemucs-6stems-GGUF`, but the app has no URL; not added).

## Tests

`tests/regression/ai_catalog.inc` (suite `regression: ai_catalog`, registered in `tests/UnitTests.cpp`,
2 lines) — no network:
- acpp hashes (all present, lookup by URL, FlashSR pinned), `validateSha256`, `commitDownload`
  with wrong/correct hash, `sha256OfFile`, `installBinaryFromZip` (exe refused, existing binary preserved,
  unknown variant accepted);
- `SC_MODELS_DIR` (ModelManager + `acpp::dir()`, relative value ignored) — also runs on Windows;
- built-in catalog: 32 entries, unique ids, consistent fields, `installable ⇒ sha256 + size + URL`,
  reasons present, CUDA blocked, platform;
- JSON §6.5 after a text round-trip (`whisper-base`, sizes > 4 GB, runtime), `devicesJson`;
- `resolveAiDevice`: auto/GPU/NPU/CPU, `GPU.n`, case, explicit refusals, unknown engine, whisper, audio.cpp;
- install/verify/remove via `file://` (cancellation without a partial file, progress, corruption
  detected then repaired, wrong published hash refused, non-installable entry, concurrent busy per thread,
  removal preview, idempotent removal, unknown id);
- audio.cpp install path (zip + exe + variant, model in a subfolder, wrong hash refused, shared CLI slot
  busy) via an injected downloader (`setAudioCppDownloaderForTests`), hence tested on macOS/Linux.

Results (macOS 26 arm64, AppleClang 21, Debug, `SPECTRO_ENABLE_AI=ON`, ctest 8 shards):
- base `76de144`: 10/10 ctest OK;
- after the lot: 10/10 ctest OK (two full passes, before and after the review fixes);
  `regression: ai_catalog` ≈ 0.04 s.
- `SPECTRO_ENABLE_AI=OFF` build (macOS): compiles; `regression: ai*` 43/43 (the catalog parts
  are skipped with `SKIP`, the audio.cpp SHA-256 and `SC_MODELS_DIR` checks run).
- local interference, unrelated to the lot: other agents were running the suite at the same time
  on the same machine. (a) `scTestTempDir()` = `<temp>/SpectroCraftTests-tmp-shard<k>` is global and
  emptied at the start of each pass → one pass had dozens of "write wav" failures + SegFaults;
  with a private `TMPDIR`, gone. (b) `single instance mode` (global named pipe) sometimes fails
  ("empty list acknowledged", "second client's forward acknowledged"); alone: 13/13. Last
  full pass (private TMPDIR): 9/10 shards OK, the 10th only on this pipe test.
  CI (isolated machines) is authoritative.
- **CI** `build.yml` (workflow_dispatch on `f12f175`, run 42155383758): `windows-ai` ✅ (MSVC Release,
  audio.cpp Windows paths compiled and tested), `macos-ai` ✅, `linux-ai` ✅.

## Deviations

- Base `76de144` and not `5a80afd` (see the top of the file).
- Files outside the list touched: none, except `tests/UnitTests.cpp` (`SC_RUN_TEST` line in addition to
  the `#include`, needed to run the suite).
- `tests/CMakeLists.txt` not modified (out of scope): the console target `SpectroCraftTests` (not
  built by CI, `--target SpectroCraft`) should list `src/AI/Sha256.cpp` outside the AI block and
  `src/AI/ModelCatalog.cpp` inside the AI block if it is ever revived.
- On Windows, ctest and `--run-unit-tests` still do not set `SC_MODELS_DIR` (tests/CMakeLists.txt,
  Main.cpp out of scope): the override is honoured but nobody uses it yet on the Windows side.
- `verified` in the list = "SHA-256 verified in this process" (no hashing during `list`);
  `false` does not mean corrupted: call `ai.models.verify`.

## Merge: what the main line must do

Proposed file: `src/control/commands/Ai.cpp`, flags `AiBuildOnly`. All the work is in the
catalog; each handler is 5-15 lines.

- `ai.models.list` (`Query`) → `Outcome::success (ModelCatalog::instance().listJson())`.
- `ai.models.install {modelId}` (`LongRunning`):
  1. `auto plan = cat.installPlan (id); if (!plan.ok) return Outcome::failure (plan.code, plan.message, plan.details);`
  2. `jobs.start ({ "ai.models.install", false /*non-mutating*/, … }, [id] (JobControl& jc) {
       auto o = ModelCatalog::instance().install (id, [&] (float f, const std::string& s)
                    { jc.progress (f, s); return !jc.cancelled(); });
       return o.ok ? Outcome::success (o.result) : Outcome::failure (o.code, o.message, o.details); });`
  3. immediate response `{"job":…, "sizeBytes": plan.result.sizeBytes}`.
- `ai.models.verify {modelId}`: same pattern with `cat.verify`.
- `ai.models.remove {modelId, confirm}` (`Destructive`): without `confirm` →
  `confirm_required` with `cat.removalPreview(id).result.message`; refuse with `busy` if an AI job
  (noiseSuppress/transcribe) uses this model (the catalog only knows its own operations);
  then `cat.remove(id)` (fast for OpenVINO/Whisper; as a job if one wants to be careful with the large GGUFs).
- `ai.devices {init}` (`Query`): `init:false` → `devicesJson (app.aiDevicePref, false)` directly;
  `init:true` → job (loads OpenVINO), result `devicesJson (app.aiDevicePref, true)`.
- `ai.setDevice {engine, device, persist}`: `openvino` → `resolveAiDevice ("openvino", device)` in a
  job if the runtime is not ready (or `availableDevices(…, false)` if `ready()`), error →
  `Outcome::failure (r.code, r.message, r.errorJson()["details"])`; success → `app.aiDevicePref = r.requested`
  (`"auto"` or name), `persist` → `saveSettings`, result `r.toJson()` + `persisted`.
  `audiocpp` → store the backend separately (no existing setting).
- **Actions.cpp** (lot 0, `f1cfa62`): the `noiseSuppress` step still uses the old
  `sc::resolveAiDevice` (silent CPU fallback). For an API command, call
  `sc::ai::resolveAiDevice ("openvino", pref)` and fail on `!r.ok`; keep the fallback for the
  replay of recorded actions if compatibility is wanted.
- `--test-ns-failure-alert`, dialogs: `buildNsFailureDetails` unchanged (same declaration).
- Conflicts expected at merge: `tests/UnitTests.cpp` (lines neighbouring `regression/ai.inc`),
  `src/CMakeLists.txt` (list of sources) — trivial.

## Debts / next steps

- Extract all the DLLs from the CUDA zips (StemSeparation/SuperResolution/`installBinaryFromZip`), then
  remove `catalogBlocker`.
- The "busy" lock does not cover downloads started by the dialogs (first NS /
  Whisper use, Stem/SR setup); on Windows two simultaneous downloads of the same file share
  the same `.part` (pre-existing).
- audio.cpp migration (C API, cpu/vulkan/cuda/metal backends): add an `engine` and entries;
  `resolveAiDevice` already accepts backends as data.
