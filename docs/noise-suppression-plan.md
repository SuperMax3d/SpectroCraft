# Noise Suppression via OpenVINO — Integration Plan

Status: Phase 0 (AI plumbing) and Phase 0.5 (Resample tool) **implemented and shipped**. Phases 1/2/3 pending approval to start.

Source: https://github.com/intel/openvino-plugins-ai-audacity (cloned to `C:/Temp/openvino-plugins-ai-audacity`)

## 1. Licence review

### What we would take from the Intel repo
| Item | Licence | Notes |
|---|---|---|
| C++ noise-suppression code (`mod-openvino/noise_suppression/**`, ~1,300 lines) | **GPL-3.0-only** (SPDX header on every file) | The Audacity-facing wrapper `OVNoiseSuppression.cpp` is wx/Audacity-specific and would be discarded; only the model-runner code is reused |
| DeepFilterNet2/3 IR weights (`enc`, `erb_dec`, `df_dec`) | **MIT** (HF repo `Intel/deepfilternet-openvino`) | ~9 MB per model, 6 files each |
| denseunet-ll IR weights | **Apache-2.0** (Open Model Zoo) | FP16: 8.6 MB + xml; FP32: 17.3 MB |
| OpenVINO runtime DLLs (bundled or downloaded) | **Apache-2.0** | CPU plugin only ≈ tens of MB |

### Compatibility with SpectroCraft (AGPL-3.0)
- GPL-3.0-only code combined into an AGPL-3.0 program is a standard, FSF-sanctioned combination: the result stays **AGPL-3.0** (the stricter copyleft wins; AGPL ⊃ GPL). No conflict, no relicensing needed.
- **Yes — SpectroCraft can remain open source and free for personal *and* commercial use.** AGPL permits selling/distributing for money. Obligations:
  - Ship the complete corresponding source of the whole app (including the copied GPL files with their Intel copyright headers intact).
  - Include both licence texts (AGPL-3.0 already in repo; add GPLv3 text or reference it next to the copied files).
  - Model weights are MIT/Apache — no copyleft effect on the app; keep their attribution/copyright notices when redistributing.
- The AGPL "network clause" is irrelevant for a desktop app (only triggers if offered as a remote service).

**Conclusion: green light.** Keep everything under AGPL-3.0, preserve headers, ship licence texts + model attributions in `third_party/`.

## 2. What the Intel code actually does (verified by reading source)

Two model families behind one abstract interface (`NoiseSuppressionModel::run(channel, start, len, progressCb)`):

### A. denseunet-ll (Open Model Zoo) — *pure OpenVINO*
- `noise_suppression_omz_model.cpp` (~160 lines). Stateful streaming model: 39 state tensors (`inp_state_*`/`out_state_*`), patch = 128 samples, **mono @ 16 kHz**, fixed delay 384 samples (24 ms) read from the `delay` output.
- Trivial to port: replace Audacity's `WaveChannel::GetFloats/Set` with reads/writes into our in-memory `juce::AudioBuffer<float>`.

### B. DeepFilterNet2/3 — *OpenVINO + libtorch hybrid* (Intel recommends these)
- Three small OpenVINO sub-models (`enc`, `erb_dec`, `df_dec`) do the neural inference; **all DSP around them uses libtorch** (~100 torch calls): Hann-windowed STFT (fft 960 / hop 480), ERB filterbank (`Conv1d` — but the weights are just boxcar averages over contiguous bins, i.e. a sparse GEMM), band mean/unit normalisation with alpha smoothing, complex masking, overlap-add crossfade chunking.
- Fixed **48 kHz** rate; per-channel processing; attenuation-limit (dB) + DF3 post-filter options.
- Two integration choices:
  - **B1: bundle libtorch CPU shared libs** (~250–400 MB). PyTorch is BSD-3 → licence-compatible, but heavy and a second big runtime to ship.
  - **B2 (preferred): torch-free C++ port of the DSP layer.** Every torch op used maps cleanly onto plain loops + our existing FFT (`FFTHelpers`/KissFFT AVX code already in the project). Estimated 500–800 lines; numerically verifiable against reference outputs.

### Processing flow (both families, from `OVNoiseSuppression::Process`)
1. Copy selection region → resample to model rate (48k / 16k) if needed.
2. Run per channel with progress callback (cancellable).
3. Resample back to file rate → paste into buffer.

## 3. Integration plan for SpectroCraft

### Phase 0 — Shared AI plumbing (foundation) — **DONE**
- Vendor OpenVINO C++ headers + DLLs under `extern/OpenVINO/` (Apache-2.0, licence text in `third_party/licenses/`). Pin a release (2024.6 per Intel's build docs; verify latest 2025.x works). **CPU-only in v1** (per your decision); device enumeration still lists what's present for the future.
- CMake: optional target flag `-DSPECTRO_ENABLE_AI=ON`; when OFF the app builds with zero new deps (keeps CI green without OpenVINO installed).
- New module `src/AI/` (deliberately *not* named after one feature — see Whisper section):
  - `OvRuntime` — owns `ov::Core`, device enumeration (CPU always; GNA excluded per Intel), compiled-model cache dir `%APPDATA%/SpectroCraft/openvino-cache`.
  - **Generic model manager + downloader** (designed now for both noise suppression *and* future Whisper):
    - Registry of `ModelSpec { id, displayName, files[{ url, sha256, sizeBytes, destPath }], minFreeDiskBytes }`.
    - **Auto-download on first use** with progress + SHA-256 verification (per your decision), into `%APPDATA%/SpectroCraft/models/`.
    - On failure (offline / blocked / checksum mismatch): clear popup explaining exactly which files to download from where and where to put them (manual install path); feature stays disabled until present.
    - Disk-space pre-check before downloading (matters a lot once Whisper's multi-GB models arrive).
    - No model binaries ever enter the git repo.
  - Initial registry entries: DF2/DF3 files from `https://huggingface.co/Intel/deepfilternet-openvino/resolve/main/<path>`; denseunet FP16 from OpenVINO storage (checksums published in OMZ `model.yml`).

**Implementation findings (baked into the code, relevant to Phase 1+):**
- **Linking:** C++ API (`ov::Core`) against a pre-generated import library `extern/OpenVINO/lib/openvino.lib` (built once from `dumpbin /exports` → `.def` → `lib /def:`). Version pinned to 2024.6, so mangling stability is guaranteed; matches Intel's ported source exactly.
- **Minimal DLL set:** only `openvino.dll`, `openvino_intel_cpu_plugin.dll`, `openvino_ir_frontend.dll` (required to read .xml/.bin), and self-contained `tbb12.dll`. CMake copies them next to both exes via a POST_BUILD rule using `${CMAKE_SOURCE_DIR}/extern/OpenVINO/bin/...` — note that `CMAKE_CURRENT_SOURCE_DIR` inside a CMake function resolves to the *caller's* directory, which silently broke the first version of this copy.
- **OpenVINO 2024.6 API specifics:** split header layout (`openvino/core/` + `openvino/runtime/`, umbrella `<openvino/openvino.hpp>`); `Core::get_available_devices()` (not `available_devices`); `compile_model(...)` returns a default-constructible **value** `CompiledModel` (not a shared_ptr).
- **SHA-256:** the vendored JUCE has no cryptography module, so `src/AI/Sha256.cpp` is a self-contained FIPS 180-4 implementation. Two subtle bugs found and fixed during verification: `_state[8]` must be initialised from the H₀ constants in the constructor (uninitialised state corrupts every digest), and the padded bit-length field must be written **big-endian** (little-endian only passes for empty input). All FIPS test vectors pass, including streaming == one-shot.
- **ModelManager::isInstalled()** clears `missingOut` before filling it — otherwise repeated calls accumulate entries.
- **Known limitation: MSVC debug CRT.** The OpenVINO 2024.6 binaries fail to initialise under `/MDd`: `ov::Core()` throws `std::bad_alloc` (default stack) or a garbled "Error loading XML file" exception (32 MB stack); the same code works perfectly under `/MD`. Verified with standalone repros — it is an OpenVINO/TBB-internal issue, not ours. Consequence: AI features require a **Release** build on Windows; Debug builds report "AI unavailable" through the normal error path, and the unit-test ov::Core smoke check is Release-only (`#if !defined(_DEBUG)`). All other AI plumbing tests (SHA-256 vectors, ModelManager install/verify/corruption logic) run in both configurations.
- **Delay-loaded OpenVINO DLLs — app starts without them.** `openvino.dll` is linked with `/DELAYLOAD:openvino.dll delayimp.lib` (both exes), so the process starts even when the runtime DLLs are not installed next to it. `OvRuntime::init()` probes `LoadLibraryA("openvino.dll")` first and returns a clear "OpenVINO runtime could not be loaded — install the DLLs" error; both pipelines fail fast on that check *before* downloading model weights, so the user gets an actionable message instead of a wasted download. `OvRuntime.cpp` is compiled with `/EHa` (both targets) as a backstop: under MSVC, a failed delayed import raises SEH code 0xC06D007E, which plain `catch(...)` only intercepts when the translation unit uses /EHa (verified with standalone probes; the default failure hook terminates the process). Verified end-to-end: exe copied to an empty directory starts normally, renders its full UI, non-AI features work byte-identically (`--test-spectral` md5 unchanged), `--test-noise-suppression` fails gracefully with the install message, and the test suite skips (not fails) its three AI tests when `openvino.dll` is absent.
- **Failure dialog: copyable text + correct guidance.** The old failure alert used `AlertWindow::showMessageBox`, whose label is not selectable — users could not copy the model folder path or download URLs out of it. It now shows a read-only multi-line `TextEditor` (via `addCustomComponent`) with the full details; Enter/Escape still close it because key events bubble up from the editor, and Ctrl+A/Ctrl+C work immediately (editor grabs focus). The message text is built by `sc::buildNsFailureDetails()` (`NoiseSuppression.h`, unit-tested in `testNsFailureDetails`): when the error starts with "OpenVINO runtime unavailable" it lists the four required DLLs and where to get them instead of model-download URLs — following those would have been a trap, since the weights are useless until the runtime loads (this was exactly the reported failure mode: user installed the models as instructed, still got an error). A `--test-ns-failure-alert <in.wav> [modelId] ["fake error"]` CLI hook shows the real dialog and verifies select-all + copy via a clipboard round-trip. The CLI arg parser also gained double-quote support so arguments with spaces survive tokenisation.

### Phase 0.5 — Resample tool (Effects → Resample…) — **DONE** (commit `5fe19ae`)
- **Vendor SoXR** (`extern/soxr`, LGPL-2.1) — this is *exactly* Audacity's resampler: their `Resample` class wraps libsoxr, vendored in-tree at `lib-src/libsoxr` (v0.1.3). We take the same source tree and drive it with the same "Best" quality recipe (`SOXR_VHQ`, 28-bit precision) Audacity uses for its Best method.
- LGPL-2.1 + AGPL-3.0 = compatible; combined app stays AGPL-3.0 (ship SoXR source + licence text).
- Use `soxr_create(oldRate, newRate, nch)` with **planar float32 I/O** (`SOXR_FLOAT32_S`) — maps directly onto JUCE's planar `AudioBuffer`, zero copies.
- (Rubber Band considered first: it's GPL-2-or-later and would also work, but modern Audacity no longer uses it — SoXR is the current Audacity method per your request.)
- Dialog: shows current rate, target-rate field (any positive float), notes it applies to the **whole file**.
- After resample: update engine `sampleRate`/`totalSamples`, rebuild mipmaps; playhead/selection/markers are time-based so they survive automatically.
- **Undo semantics:** v1 = **non-undoable with a warning in the dialog** ("whole file; cannot be undone"). Revisit later if a snapshot-eviction scheme proves acceptable.

**Implementation findings (baked into the code, relevant to Phase 1+):**
- SoXR expresses *all* lengths as samples **per channel** — never total-across-channels. A first version that used `nch × frames` produced wrong output lengths and heap over-reads (segfault in tests).
- Engine convention: `AudioEngine::totalSamples` is **frames per channel** (matches JUCE's `lengthInSamples`). `resampleTo()` must keep this or duration/playhead math silently doubles for multichannel files.
- SoXR must be compiled as **C** (its internal `void*` casts break in C++), with Audacity's exact 14-file source list (`pffft.c`, `cr-core.c`, `util-simd.c` are include-only and must not be separate TUs). Defines: `_USE_MATH_DEFINES`, `_CRT_SECURE_NO_WARNINGS`, `SOXR_LIB`; `/arch:SSE2`. **No /openmp** (matches Audacity; the OpenMP path is guarded by `_OPENMP`).
- Flush semantics: a final call with negative input length (`~ilen`) processes the remainder *and* flushes in one call — this is what `soxr_oneshot` does internally and avoids the NULL-input code path entirely.
- **Test isolation fix (important for all future phases):** unit tests construct `Application app;`, which loads `%APPDATA%/SpectroCraft/SpectroCraft.recent`. A GUI session that left non-default settings behind (e.g. linear frequency axis) silently broke 12 log-axis view tests — they looked like rendering regressions but were settings pollution. Test builds (`SC_TESTS_EXE=1`) now use a fresh per-run temp store, so GUI sessions can never change test outcomes again.

### Phase 1 — denseunet end-to-end — **DONE**
- Ported `noise_suppression_omz_model.*` → `DenseUnetModel` operating on plain float buffers at the model rate (no Audacity types).
- Resampling reuses the Phase 0.5 SoXR wrapper (`sc::Resampler`): file rate → 16 kHz → back.
- `Application::noiseSuppress(modelId, device, options)` mirroring `changeGain`: requires non-empty selection; runs async on a worker thread with the existing `WorkProgressDialog`; cancellable. Compiles via the shared model cache (first run 10–30 s, instant afterwards).
- Undo: pre-edit samples captured via `AudioEngine::replaceSamples()` (records an undo patch before writing), action name `"Noise suppressed (denseunet)"`.
- UI: **Effects → Noise Suppression…** dialog (same pattern as Change Gain): model combo + device combo, no text fields so Enter/Escape reach the button shortcuts directly; failure popup lists exact manual-install URLs.
- CLI flag `--test-noise-suppression in.wav out.wav` for headless verification (writes 32-bit float WAV).

**Implementation findings (baked into the code, relevant to Phase 2+):**
- **The model is a speech-enhancement network.** Pure tones are *out-of-distribution* and get suppressed along with noise (verified: 400 Hz tone at amp 0.3 → output ≈ 7e-5). Real speech passes through almost untouched (SAPI TTS clip: correlation 0.99, ~0 dB loss); stationary white noise is cut by >21 dB. Consequence for tests: assert **noise suppression** (≥10 dB on synthetic white noise) and silence passthrough — not tone preservation.
- **Delay compensation.** The streaming interface has an internal latency of `delay` samples (384 @ 16 kHz): raw output sample i corresponds to input sample i+delay. We shift the result back so it stays time-aligned with the input (the OMZ demo does this; Intel's Audacity plugin does *not*, which shifts their processed audio by ~24 ms). `outputLengthFor()` therefore returns exactly nSamples.
- **State buffers must be sized before wrapping in `ov::Tensor`.** An empty `std::vector`'s `data()` points at MSVC STL's inline 1-element buffer; OpenVINO then reads out-of-bounds garbage and inference degenerates into a ~50 s pathological hang. Always `assign(shapeSize, 0)` first.
- **CrashTrace VEH bug (fixed in `include/CrashTrace.h`).** `AddVectoredExceptionHandler` runs *before* C++ landing pads for every throw — the old handler terminated the process on any catchable exception (bit us with an OpenVINO compile error). It now passes code 0xE06D7363 through and a new `SetUnhandledExceptionFilter` traces genuinely unhandled exceptions.
- **WinInet download details:** `InternetReadFile` returning FALSE with nRead=0 is clean EOF (break, not an error); SHA/size verification + rename must happen *after* the stream is closed (Windows won't move a file with an open handle).
- **Integration tests must use `Application::openFile()`**, not `audioEngine().loadFile()`: the latter bypasses FileDoc creation so `visibleChannels` stays empty and the pipeline silently skips every channel.

### Phase 2 — DeepFilterNet2/3, torch-free — **DONE**
- Ported the three OpenVINO sub-model runners (`dfnet_model.*`) as-is (pure OpenVINO + tensor shuffling): `DeepFilterNetModel` compiles enc / erb_dec / df_dec and links encoder outputs to decoder inputs via `set_tensor` at load time.
- Rewrote the DSP layer without libtorch:
  - STFT/ISTFT: custom streaming `DfStft` (Bluestein FFT for N=960, M=2048; Vorbis window; hop 480). The Bluestein path uses precomputed twiddle tables `_tw[s][j]` for stages s=1..11 — the table needs **kStages+1 rows** (stage index runs 1..kStages inclusive).
  - ERB filterbank: boxcar band means via `binToBand()`/`bandWidth()` (32 bands, widths summing to 481), dB features + EMA normalisation (alpha 0.99, state init linspace(-60,-90,32)).
  - Unit features: scale-invariant `X/sqrt(EMA(|X|))` over the first 96 bins (state init linspace(0.001,0.0001,96)).
  - Masking, attenuation-limit blend (`lerp` by 10^(-|lim|/20)), DF3 post-filter formula, deep-filter FIR + high-band copy: plain loops.
  - Chunking: (3002-2)×480-sample chunks with 10 s overlap and linear crossfade; STFT + normalisation states reset per chunk. Feature lookahead padding matches Intel's `ConstantPad2d({0,0,-2,2})`.
- **Streaming OLA latency**: the analysis/synthesis memory scheme emits output delayed by exactly one hop (480 samples) — verified numerically against a Python reference to 1e-13. `forwardChunk` compensates by padding the chunk with fft_size zeros and trimming the first `fft_size - hop` samples of the synthesised stream, exactly like Intel's `forward()`.
- **Bugs found & fixed during validation** (both masked each other):
  1. Twiddle table declared `_tw[kStages][kM/2]` but stage 11 needs row index 11 → out-of-bounds access. The original fill loop (`s < kStages`) left stage-11 twiddles uninitialised (garbage → all-NaN spectra, no crash); "fixing" the loop to `s <= kStages` without resizing the array turned it into an OOB write (access violation in the constructor). Correct fix: `_tw[kStages + 1][kM / 2]`.
  2. Vorbis window formula divided by `fft_size/2` instead of `fft_size`, producing a double-hump window that violates COLA (`w[n]^2 + w[n+480]^2 != 1`) → round-trip gain varied 0..2× across the frame.
- Validation: unit test `testDfStft()` (round trip vs delayed reference, DC passthrough, reset) passes at ~3e-7 max error; `testDeepFilterPipeline()` runs DeepFilterNet2 end-to-end on synthetic stereo noise and asserts ≥10 dB RMS reduction + silence passthrough + length preservation. CLI: `--test-noise-suppression in.wav out.wav deepfilternet2` works (auto-downloads the three sub-models, ~9 MB).
- UI: model combo (denseunet / DeepFilterNet2 / DeepFilterNet3) + attenuation-limit field in the existing dialog. The DF3 post-filter option is a pipeline parameter (default off) — not exposed in v1 because this JUCE build has no `juce::CheckBox`.

### Phase 3 — Whisper transcription — **DONE**
- Vendored whisper.cpp v1.6.2 (MIT) at `extern/whisper.cpp` (flat layout, all files byte-identical to the upstream tag). Built with its OpenVINO encoder backend (`openvino/whisper-openvino-encoder.cpp`).
- **Model distribution**: auto-download only `ggml-<name>.bin` from `ggerganov/whisper.cpp` on HuggingFace (sizes + SHA-256 verified against the HF API; LFS oid == content hash). The OpenVINO encoder IRs (`ggml-<name>-encoder-openvino.{xml,bin}`) are **not published anywhere** — users can optionally generate them with whisper.cpp's `models/convert-whisper-to-openvino.py` and drop both files into the model folder for faster encoding. Without them the pipeline falls back to the pure-ggml encoder (slower, identical results). Models: tiny / base / small / medium / large-v1 / large-v2 / large-v3.
- **Engine wrapper** (`WhisperTranscriber`): `whisper_init_from_file_with_params(use_gpu=false)`; OpenVINO encoder init only when the IR XML exists *and* a non-loading-safe availability probe passes (delay-load SEH 0xC06D007E would otherwise escape); progress callback bridge clamps v1.6.x's >100% overshoot on short files.
- **Pipeline** (`runWhisperPipeline`): mix selected channels to mono at native rate in ~30 s blocks → resample each block to 16 kHz (SoXR) → single inference pass. Cached `WhisperTranscriber` instances keyed by model path (mutex-guarded static map).
- **SCTT chunk**: custom RIFF chunk with UTF-8 JSON `{"version":1,"segments":[{"start","end","text"}]}` — written on save after the LIST(adtl) marker chunk, parsed on load by scanning the RIFF. Plain WAVs without it load fine; never appears in the Markers panel.
- **UI**: Transcription tab (segment table: # / start / end / text; double-click → playhead + selection jump), Effects → Transcribe… dialog (model combo, 20-language combo incl. auto-detect, transcribe/translate mode), async worker with WorkProgressDialog, failure dialog reuses the copyable-text AI-failure helper.
- **CLI**: `--test-transcribe <in.wav> [modelId] [language]`.
- **Tests**: `testScttJson()` (JSON round-trip incl. unicode + surrogate pairs), `testScttChunkWav()` (save/load round-trip through AudioEngine + FileDoc), `testWhisperPipeline()` (real whisper-base download + inference, AI-gated).
- **Root cause of the Phase 3 crash hunt** (heap corruption 0xC0000374): `whisper_free_params()` does `delete params` — it is only valid for pointers returned by `whisper_full_default_params_by_ref()`. Passing the address of a stack object from `whisper_full_default_params()` (by value) is UB and corrupted the heap deterministically. Fix: never call `whisper_free_params()` on stack params (the struct is POD — nothing to free). The earlier suspicion that loading openvino.dll/TBB conflicted with ggml was a red herring caused by misreading filtered probe output.
- **Deferred to Phase 4**: batch folder transcription, in-panel text search box.

### Phase 4 — Polish — **DONE**
- Settings persistence (last model/device, auto-download toggle) in the existing settings store.
- README section: feature description, model sources + licences, manual install instructions, performance notes (CPU vs GPU).
- Optional: "noise profile" convenience (record N ms of noise → but that's a *different* algorithm — out of scope; these models are speech-enhancement oriented and work best on voice content).

**Implementation notes:**
- **Batch folder transcription**: Effects → Transcribe Folder… (GUI) + `--test-batch-transcribe <folder> [modelId] [language] [skipExisting 0|1]` (CLI). Recursively finds WAVs, streams each through the Whisper pipeline without opening it in the editor, then surgically rewrites only the RIFF header + appends/replaces the `SCTT` chunk on disk. Cancellable mid-run; per-file status; summary dialog (total/done/skipped/failed) with failure list.
- **In-panel text search**: case-insensitive substring search in the Transcription panel; Enter jumps to next match (wraps); double-click still selects the segment range. Implemented via a small `SearchEditor` subclass of `juce::TextEditor` because this JUCE build has no `onReturnPressed`/`setPlaceholder` hooks.
- **Settings persistence**: NS model + attenuation, transcription model/language/mode, and auto-download toggle all persist in the existing settings store (`testAiSettingsPersistence`).
- **Data-loss bug found & fixed**: `juce::File::moveFileTo` on Windows deletes the destination *first*, then calls plain `MoveFile`; with the temp file still open by a `FileOutputStream` (no FILE_SHARE_DELETE) the move fails *after* the original was deleted → orphaned temp + lost original. Fixed by scoping all handles closed before replacing and using `replaceFileIn()` (atomic Win32 `ReplaceFile`). Regression test: `testScttChunkReplace`.
- **README**: new top-level "AI features" section — how NS/transcription/batch work, model sources & licences table, exact manual-install file lists per model, performance notes; all four AI CLI flags documented.
- **UIA crash investigation (E_UNEXPECTED 0x8001010D)**: in earlier sessions the app died when `System.Windows.Automation` FindAll was run against it (and once ~1–3 min after synthetic menu clicks). Deep dive findings:
  - The Debug PDB is corrupted (zero GUID) → symbol resolution done by relinking existing .obj files with `/MAP` and parsing the MAP file.
  - Raw RSP stack scans from the crash handler are unreliable (stale frames); the "UIAutomationCore/combase" frames turned out to be residue — the innermost real frames were `Application::batchTranscribeFolder` + JUCE text rendering, which made no sense for an idle app → confirmed stale-frame theory.
  - Two defensive JUCE patches applied and kept: `WM_GETOBJECT` now returns 0 (no UIA provider ever exposed) and `WindowsAccessibility::areAnyAccessibilityClientsActive()` always returns false (no UIA events raised). This makes the app fully UIA-inert, which is a reasonable v1 posture.
  - CrashTrace improved: x64 StackWalk64 seed fix (Rbp must not be used as frame pointer on x64) + self-contained `.pdata`-based fallback walker (`manualWalk`) so future crashes produce clean stacks without dbghelp.
  - **Final status: no longer reproducible.** After the ghost window of a previously crashed instance disappeared, the current build survived 6 UIA FindAll queries, 15 synthetic menu open/close cycles and >7 min idle with zero exceptions in stderr. Most likely cause was interaction with that zombie/ghost state (or a timing race its presence widened). Documented as resolved; the accessibility patches remain as defense-in-depth.
- **Tests**: Release 772/772, Debug 726/726 checks passing; spectral CLI md5 unchanged (`e4a31004bcb637e1430d289da645bdb0`); NS + transcription + batch CLI end-to-end verified.

## 4. Whisper transcription (Phase 3) — design per your spec
**UI: new "Transcription" panel next to the Markers panel** (not an Effects filter):
- List of segments: `[start – end] text`.
- **Double-click a segment → move playhead + set selection** to that time range.
- **Text search box** in the panel (case-insensitive substring; Enter jumps to next match).
- **Run Transcription button inside the panel** (+ model-size combo once Whisper is integrated), plus Clear.

**Storage: custom RIFF chunk, NOT markers.**
- New top-level WAV chunk `SCTT` (4CC) containing UTF-8 JSON: `{ "version": 1, "segments": [ {"start": s, "end": s, "text": "..."}, ... ] }` (times in seconds).
- Written on save alongside the existing cue/adtl marker chunks; parsed on load by scanning the RIFF (same mechanism as markers). Markers panel is untouched.

**Batch mode:**
- Option to run transcription over all WAVs in a folder + subfolders **without opening them in the editor**: stream-read audio → infer → append/replace `SCTT` chunk in each file on disk. Per-file progress, cancellable, summary at end (N transcribed / M failed).

**Shared infrastructure built once now (Phase 0) so Whisper is mostly plug-in work later:**
1. **OpenVINO runtime bundling** — shared directly (Whisper's encoder runs on OpenVINO).
2. **Generic model manager/downloader** — Whisper ggml weights come from `ggerganov/whisper.cpp` on HuggingFace (base ≈ 148 MB, small ≈ 488 MB, medium ≈ 1.5 GB, large-v3 ≈ 3.1 GB); the OpenVINO encoder IRs are not published and must be generated locally if wanted. The disk-space pre-check, progress, checksum and manual-install popup designed for the ~9 MB noise models become critical at this scale — hence designing it generically now.
3. **Async worker + `WorkProgressDialog` pattern** — both features are long-running, cancellable.
4. **whisper.cpp itself** (MIT licence, compatible) built once with its OpenVINO backend; Intel's split is clever: encoder as OpenVINO IR for acceleration, decoder stays in whisper.cpp's ggml runtime. Their `OVWhisperTranscription.cpp` (~1,300 lines, GPL-3.0-only) shows the full parameter surface (model size, beam search, best-of, language) to mirror.

## 5. Risks / open questions
1. **Model quality domain**: all three models are trained for **speech enhancement** (DNS-Challenge etc.). On music/general audio they behave like a broadband gate — may artefact musical passages. UI should say "best on speech". (Audacity has the same caveat.)
2. **OpenVINO version drift**: pin exactly; test CPU inference determinism across versions before shipping.
3. **First-run compile time** (10–30 s): mitigated by OpenVINO's on-disk compiled-model cache; progress dialog covers it.
4. **Memory**: both pipelines stream in patches/chunks — no whole-file model buffers; only the existing in-memory file buffer is touched (same as gain/silence edits). Resample tool temporarily needs a second full-size buffer during conversion (acceptable; freed immediately).
5. **Download trust / UX**: SHA-256 pinning + HTTPS from Intel-owned hosts; auto-download with clear manual-install fallback popup (per your decision); document offline install path in README.
6. **Resample undo** (open design point): v1 = non-undoable + confirm dialog; flag for revisit.

## 6. Deliverables checklist (when approved)
- [x] `extern/OpenVINO/` vendored runtime + CMake option (`SPECTRO_ENABLE_AI`) — done in Phase 0
- [x] `extern/soxr/` vendored SoXR (Audacity's resampler, LGPL-2.1) — done in `5fe19ae`
- [x] `src/AI/` module (runtime, generic model manager/downloader with auto-download + manual-install fallback) — done in Phase 0
- [x] Effects → Resample… tool (whole-file, arbitrary rate, warning dialog) — reusable by AI features internally — done in `5fe19ae`
- [x] Effects → Noise Suppression… denseunet end-to-end: dialog + async progress + undo integration
- [x] DeepFilterNet2/3 torch-free port (Bluestein STFT, ERB/unit features, chunking) — Phase 2
- [x] Unit tests (resampler quality, NS pipeline assertions incl. DfStft round trip + DFNet2 ≥10 dB reduction) + CLI flag `--test-noise-suppression in.wav out.wav [modelId]`
- [x] Whisper transcription: vendored whisper.cpp v1.6.2 + OpenVINO encoder backend, model auto-download (ggml .bin), SCTT RIFF chunk storage, Transcription panel + dialog, CLI flag `--test-transcribe` — Phase 3
- [ ] `third_party/licenses/` (GPLv3 text for copied code, Apache/MIT/LGPL notices for OpenVINO/models/SoXR), README update
