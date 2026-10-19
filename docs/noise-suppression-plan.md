# Noise Suppression via OpenVINO — Integration Plan (DRAFT, for review)

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

### Phase 0 — Shared AI plumbing (foundation, ~1–2 days)
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

### Phase 0.5 — Resample tool (Effects → Resample…)
- **Vendor SoXR** (`extern/soxr`, LGPL-2.1) — this is *exactly* Audacity's resampler: their `Resample` class wraps libsoxr, vendored in-tree at `lib-src/libsoxr` (v0.1.3). We take the same source tree and drive it with the same "Best" quality recipe (`SOXR_VHQ`, 28-bit precision) Audacity uses for its Best method.
- LGPL-2.1 + AGPL-3.0 = compatible; combined app stays AGPL-3.0 (ship SoXR source + licence text).
- Use `soxr_create(oldRate, newRate, nch)` with **planar float32 I/O** (`SOXR_FLOAT32_S`) — maps directly onto JUCE's planar `AudioBuffer`, zero copies; OpenMP enabled for multi-core speed.
- (Rubber Band considered first: it's GPL-2-or-later and would also work, but modern Audacity no longer uses it — SoXR is the current Audacity method per your request.)
- Dialog: shows current rate, target-rate field (any positive float), notes it applies to the **whole file**.
- After resample: update engine `sampleRate`/`totalSamples`, rebuild mipmaps, scale selection + playhead by the rate ratio to preserve context. Markers are time-based so they survive automatically (verify storage units during implementation).
- **Undo semantics (design decision needed):** a whole-file resample is a full-buffer snapshot — one such entry per undo level would cost 1× file RAM each, which is too much at 50 levels for multi-GB files. v1 recommendation: **non-undoable with a confirm dialog** ("Resampling changes the entire file and cannot be undone; consider saving first"). Revisit later if a snapshot-eviction scheme proves acceptable.
- Unit tests: round-trip rate accuracy, passband ripple / stopband attenuation vs a reference tone + noise bed, extreme ratios (8 kHz → 352.8 kHz).

### Phase 1 — denseunet end-to-end (~2–3 days)
- Port `noise_suppression_omz_model.*` → `DenseUnetModel` operating on `std::vector<float>` in/out (no Audacity types).
- Resampling reuses the Phase 0.5 libsamplerate wrapper: file rate → 16 kHz → back.
- `Application::noiseSuppress(modelId, device, options)` mirroring `changeGain`: requires non-empty selection; runs async on a worker thread with the existing `WorkProgressDialog` (indeterminate while compiling — first run 10–30 s per Intel's docs — then determinate per channel); cancellable.
- Undo: capture pre-edit samples via the existing patch mechanism, action name e.g. `"Noise suppressed (denseunet)"`.
- UI: **Effects → Noise Suppression…** dialog (same pattern as Change Gain): model combo (only models present/downloaded), device combo, Apply/Cancel; focus-grab + Enter-submit behaviour per the gain-dialog fix.
- Tests: resampler unit tests; pipeline test on synthetic WAV (white noise + 400 Hz tone → assert noise-floor RMS drops ≥ N dB and tone amplitude within tolerance); offline CLI flag `--test-noise-suppression in.wav out.wav` for headless verification (skipped gracefully when models absent).

### Phase 2 — DeepFilterNet2/3, torch-free (~1–2 weeks)
- Port the three OpenVINO sub-model runners (`dfnet_model.*`) as-is (they're already pure OpenVINO + tensor shuffling).
- Rewrite the DSP layer without libtorch:
  - STFT/ISTFT: reuse project FFT with Hann window, fft 960 / hop 480.
  - ERB filterbank: precomputed boxcar-average matrix (weights are `1/num` over contiguous bins — no general GEMM needed).
  - Normalisation states (`_reg_*`), masking, crossfade chunking: plain loops.
- Validate numerically against Intel's reference behaviour on a fixed test clip (compare per-frame masks / output RMS; small tolerance for float ordering differences).
- Expose advanced options: attenuation limit (6–100 dB), DF3 post-filter checkbox — same semantics as Audacity dialog.

### Phase 3 — Whisper transcription
- Vendor whisper.cpp (MIT) built with its OpenVINO backend.
- Port Intel's `OVWhisperTranscription` logic (GPL-3.0-only, headers preserved): model-size combo, beam search / best-of options, language auto-detect.
- New **Transcription panel** next to Markers: segment list, double-click → playhead/selection jump, text search, Run button, Clear.
- `SCTT` custom RIFF chunk storage (JSON segments) — written on save, parsed on load; never appears in the Markers panel.
- **Batch transcription**: folder + subfolders scan, process each WAV headless (stream read → infer → write/replace SCTT chunk), per-file progress + summary.

### Phase 4 — Polish
- Settings persistence (last model/device, auto-download toggle) in the existing settings store.
- README section: feature description, model sources + licences, manual install instructions, performance notes (CPU vs GPU).
- Optional: "noise profile" convenience (record N ms of noise → but that's a *different* algorithm — out of scope; these models are speech-enhancement oriented and work best on voice content).

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
2. **Generic model manager/downloader** — Whisper models come from `Intel/whisper.cpp-openvino-models` as ggml + OpenVINO-IR pairs: base ≈ 171 MB, small ≈ 610 MB, medium ≈ 2.1 GB, large-v3 ≈ 4.3 GB. The disk-space pre-check, progress, checksum and manual-install popup designed for the ~9 MB noise models become critical at this scale — hence designing it generically now.
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
- [ ] `extern/OpenVINO/` vendored runtime + CMake option (`SPECTRO_ENABLE_AI`)
- [ ] `extern/soxr/` vendored SoXR (Audacity's resampler, LGPL-2.1)
- [ ] `src/AI/` module (runtime, generic model manager/downloader with auto-download + manual-install fallback)
- [ ] Effects → Resample… tool (whole-file, arbitrary rate, confirm dialog) — reusable by AI features internally
- [ ] Effects → Noise Suppression… (denseunet first, then DFNet2/3 torch-free port): dialog + async progress + undo integration
- [ ] Unit tests (resampler quality, NS pipeline assertions) + CLI flag `--test-noise-suppression in.wav out.wav`
- [ ] `third_party/licenses/` (GPLv3 text for copied code, Apache/MIT/LGPL notices for OpenVINO/models/SoXR), README update
