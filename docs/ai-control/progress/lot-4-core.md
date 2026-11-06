# Lot 4 (core) — headless measures module `sc::measures`

Branch `ai-control-measures` (fork `nicolasmarcotteTaintin/SpectroCraft`), created from `ai-control`
(`e04d711`, see "Deviations"). Work of a parallel agent: it contains **no**
`measure.*` command; it provides the computation, shared with the FFT panel, on top of which the main agent
will only have to write thin wrappers when it reaches lot 4.

## Done

- **Module `sc::measures`** (`include/Measures.h`, `src/Measures.cpp`): free functions, no
  `Component`, no geometry, no `app.selection`. Inputs: `const AudioEngine&` (reads through
  `readSamples`, thread-safe), channels, region in seconds, FFT size, window type, overlap,
  calibration (`const ScaleCalibration*`, copied by value on the caller's side). Outputs: plain structures
  (frequencies, levels, unit strings). Long computations take a `ProgressFn`
  (`bool(double fraction)`, `false` = cancel) and report `cancelled`.
- **Extraction from `FFTAnalysisView`, arithmetic unchanged** (code moved as is):
  - `computeRegionFrom` → `regionSpectrumNorm(eng, chans, t0, t1, n, windowType, …)`: the window
    type is an argument instead of being read from `windowCombo`; window placement is
    isolated in `spectrumWindowStarts`;
  - body of `thirdOctBandsRef` → `thirdOctaveRefBands` (same signature + progress;
    `FFTAnalysisView::thirdOctBandsRef` remains, with identical signature and results, and delegates);
  - file-`static` helpers moved out of `Panels.cpp`: `makeAnalysisWindow`, `windowEnbwBins`,
    `toThirdOctBands`, `bandsNormalizedToDb`, `curveFrameStarts`, IEC/reference centre tables,
    constants `kMaxAvgWindows`, `kMaxCurveFrames`, `kTimeModeFFT`, `kNormFloorDb`;
  - `computeTimeCurveFrom` / `computeSpectrumCurveFrom` → free functions `timeCurve` /
    `spectrumCurve` (removed from the class); `smoothSeries` shared;
  - `FFTAnalysisView::CurveData` is now an alias of `sc::measures::CurveData`.
- **The panel uses the module**: live spectrum and snapshots, IEC bands, dB-SPL pipeline, live
  curves and curve slots, and the "Copy Harmonics" table (`buildHarmonicsTable` goes through
  `harmonics(..., Search::NearestBin)`, same bin, same value). No behaviour change:
  cache, throttle, cache keys, drawing untouched.
- **New measures**:
  - `levels()`: RMS, peak (+ time), crest factor, DC offset, min/max, number of clipped
    samples, per channel, in dBFS and, if calibrated, in absolute terms (`dB-SPL`/`dBV`/`dB/g`) and as a
    physical value (Pa/V/g); full scale **per channel** (`fullScaleFor`/`levelOffsetDb`);
  - `levelCurve()`: RMS or peak as a function of time (window/hop in seconds), for
    `measure.curve` `kind:"rms"|"peak"`;
  - `findPeaks()` / `peaks()`: spectral peaks (local maxima, plateaus = first bin), topographic
    prominence (`scipy.signal.peak_prominences` definition, O(n log n)), sorted by prominence,
    top-N, frequency window, frequency and level refined by parabola (dB domain);
  - `harmonics()`: given fundamental, or anchor/N (panel semantics), or auto-detected from
    the peaks (candidates within 40 dB of the strongest, score = cumulative power of the peaks
    falling on its multiples); harmonics 1..N, search for the max within ±tolerance (default
    max(2 bins, 3 % of f0), capped at 0.45·f0) or nearest bin; level relative to the
    fundamental, THD (%), harmonics beyond Nyquist marked `valid=false`.
  - `spectrum()` / `thirdOctave()`: "full result" wrappers (metadata, per-channel calibration,
    units) around the low-level functions; `spectrum()` accepts an `overlap` (≥ 0: fixed
    hop n·(1−overlap); < 0: panel placement, default).

## Files

| File | Change |
|---|---|
| `include/Measures.h`, `src/Measures.cpp` | new (module) |
| `src/Panels.cpp`, `include/Panels.h` | the computation is replaced by calls to the module (−528 net lines) |
| `src/CMakeLists.txt` | + `Measures.cpp` (app target) |
| `tests/CMakeLists.txt` | + `Measures.cpp` (console target `SpectroCraftTests`, otherwise it no longer links: `Panels.cpp` depends on it) |
| `tests/regression/measures.inc` | new tests |
| `tests/UnitTests.cpp` | 2 lines: `#include "regression/measures.inc"` + `SC_RUN_TEST ("regression: measures", …)` |

Nothing else (neither `Application.cpp`, `Actions.cpp`, `Main.cpp`, `MainWindow.cpp`, `src/control/**`,
`tools/**`, nor CI).

## Tests and results

`tests/regression/measures.inc` ("regression: measures", 9 tests, 145 checks):

- **Exact equality (bit for bit, `==` on the `std::vector<float>`) panel ↔ module** for the same
  region and the same settings: spectrum (3 windows, N = 1024…8192, multi-window region,
  point, whole file, short region at end of file, 2 channels); IEC bands (Hann/Hamming,
  N = 1024/4096); reference dB-SPL bands (orders 40 and 10, Pascal file); curves of modes
  3/4/5 × tracking Max/Total/Weighted; "Copy Harmonics" table (format and values).
- **Known signals**: full-scale sine → peak 0 dBFS, RMS −3.01 dBFS (RMS = peak − 3.01 dB),
  90.97 dB-SPL at 1 Pa; spectrum bin at −3.01 dB / 90.97 dB-SPL; 1 kHz band ≈ 91 dB-SPL (same
  reference as the existing dB-SPL test); DC included in the RMS and reported separately; per-channel
  full scales; silence → −300 floor; RMS curve of a step (−9.03 then −23.01 dBFS); two off-bin
  tones found within half a bin, levels within ±0.5 dB; prominences and plateau on a
  synthetic spectrum; harmonic series 441.7 Hz × 6: fundamental auto-detected and harmonics within
  half a bin, levels within ±0.5 dB, dBV offset, THD ≈ 70.1 %; anchor/N and Nyquist; NearestBin.
- **Cancellation / progress / threads**: monotonic progress up to 1; cancellation of
  `spectrum`, `levels`, `thirdOctaveRefBands`, `thirdOctave`, `timeCurve`; two concurrent
  `spectrum()` calls on `std::thread`s give exactly the main thread's result; cap of
  2048 windows reported; 50 % overlap → 92 windows.
- **Panel non-regression checked outside the repository**: since the panel now calls the module,
  the equality above is structural. So I also compared, once, **verbatim copies of the
  pre-refactoring functions** (extracted from `ai-control`) against the module, on ~1,300 random cases
  (3 sample rates, N = 128…65536, 3 windows, random regions including capped ones,
  point, end of file, 3 trackings, IEC bands, reference bands orders 40/3, harmonics table):
  **0 differences** (bit-for-bit equality). This temporary test is not versioned (it duplicated 400 lines
  of the old code, and "golden" values would not be portable: the AVX2 FFT and the JUCE fallback
  do not produce the same bits). The proof of panel non-regression therefore rests on this
  one-off comparison **and** on the panel's existing tests (dB-SPL, bands, normalisation),
  unchanged and green; the versioned equality tests, for their part, only guarantee that panel and
  module stay wired to the same code.
- **Full suite** (macOS arm64, Debug, AI OFF, FFmpeg OFF, `ctest -j 8`): see the section
  "Suite results" at the end of the file.
- The existing dB-SPL / 1/3-octave tests (`tests/UnitTests.cpp` "1/3 octave dB-SPL",
  `regression/spectral_dsp.inc` W4, `regression/panels.inc` band sum) pass **without modification**.

## Deviations

- **Base**: the instructions said `ai-control` at `5a80afd`; that commit is on no branch of the fork,
  `fork/ai-control` points to `e04d711` (same message, only difference: two lines of
  `DECISIONS.md`). Branched from `e04d711` so that the merge is a fast-forward history-wise.
- `tests/CMakeLists.txt` modified (one line): required so that the console target
  `SpectroCraftTests` keeps linking (CI does not build it, but it exists).
- `tests/UnitTests.cpp`: two lines instead of one (the `#include` **and** the `SC_RUN_TEST`, without which
  the fragment is never executed).
- Build and tests done on **macOS** (Nicolas's machine), not on Linux: the agent was running on the Mac.
  The code uses only standard C++20 + JUCE (no platform-specific construct;
  `int64_t` everywhere, like the original code).
- `measure.probe` and `measure.spectrogramData` are **not** in the module: they already correspond
  to headless functions of `SpectralEngine` (see the API sheet).

## Independent review (sub-agent) and fixes

An independent reviewer confirmed the verbatim move (diff of the function bodies) and found
defects in the **new** functions, fixed in the 2nd commit, each with a test:

- `levels` / `levelCurve`: `t1 = duration` sometimes truncated by the last sample
  (`(int64_t)(duration·fs) = N−1` for ~7 % of lengths) → `sampleAt()` returns `N` at end of file;
- `harmonics`: 300 % THD on silence → no THD (`< 0`) when the fundamental is at the floor;
- `levelCurve`: empty region → no value (instead of a −300); window longer than the region →
  block-wise reading (no more allocation of the region's size);
- `detectFundamental`: a weaker peak at f0/2 won (octave error) → score weighted by the
  proportion of harmonic slots filled (test: fails without the fix, passes with it);
- validation: window type outside 0..2 and order outside {1,…,40} rejected (result with no channel) instead
  of being silently corrected; `overlap` NaN = panel placement; huge frequency bounds of
  `findPeaks` clamped before conversion to `int`;
- tests: `scRecomputeUntil` timeout raised to 10 s (slow Debug/CI machines).

Not fixed (documented): the `numSamples()/duration()/sampleRate()` accessors of `AudioEngine` are not
synchronised; a measure job during a recording or a reload is a formal race
→ it is up to the registry's busy guard to prevent it (see "Lifetime" below).

## API sheet for the `measure.*` wrappers (§6.6)

Common rules:

- **Thread**: on the message thread, resolve the document then **copy** what the computation
  needs: `const AudioEngine& eng = doc->engine`, `ScaleCalibration cal = doc->scale` (copy by
  value), channel list, and for the curves a **copy** of `doc->spectralSel` if not empty. Short
  measures (one window, `levels` over a few seconds) can be executed inline; the
  others are **jobs** (`LongRunning` flag), the `ProgressFn` relays the job's progress and
  cancellation.
- **Lifetime**: the module owns nothing. During a job, the document must be neither closed
  (`closeDoc` does `docs.erase`) nor modified (edit, resample: `readSamples` is safe but
  `sampleRate()/duration()/numSamples()` are not synchronised) → the busy guard of `invoke()` must
  refuse `doc.close` and the `Mutates` commands on this document while a measure job is running (or
  cancel and wait for the job first).
- **Calibration** (to be copied into every absolute result): `ChannelCalibration { calibrated,
  fullScale, levelOffsetDb }` per channel; `absUnitFor(&cal)` → `"dB-SPL"|"dBV"|"dB/g"|""`;
  `physicalUnitFor(&cal)` → `"Pa"|"V"|"g"|""`; `ScaleCalibration::unitName(cal.unit)` →
  `"Pascal"|"Volts"|"g"|"None"`. Emit the `abs*` fields only if `calibrated`.
- **Conventions**: `unit:"dBFS"` = 0 dB for a **peak** |1.0|; spectra in single-sided RMS
  corrected for coherent gain (full-scale sine = −3.01 dB); absolute = relative + `levelOffsetDb(ch)`.
  −300 floor = digital silence (`levels`, `levelCurve`, invalid harmonics); −160 floor
  for spectra/curves (`kNormFloorDb`). JSON windows: `windowTypeFromName("hann"|"hanning"|
  "hamming"|"rect"|"rectangular")` (−1 → `invalid_params`), `windowName(type)` on output.
- **Validation** to be done in the wrapper: `fftSize` a power of 2 in [2, 65536] (the module returns
  an empty result otherwise), `order` ∈ {1,2,3,4,5,10,20,30,40}, `start ≤ end`, existing channels
  (the module silently ignores invalid channels; `invalid_params` is better).
- **Large volumes**: beyond 1 M values (65536 spectrum × channels, curves, spectrogram), write
  to a file (`inline:false`, `path`, format `f32`/`csv`) as planned in §6.6.

| Command | Function(s) | Parameters → arguments | JSON fields to return |
|---|---|---|---|
| `measure.levels` | `levels(eng, LevelsParams{t0,t1,channels,clipThreshold}, &cal, progress)` | `start`/`end` (absent = whole file: `t1 ≤ t0` ⇒ up to the end), `channels` (absent = all) | `region{start:r.t0,end:r.t1,samples:r.s1-r.s0}`, `unit:"dBFS"`, `calibration{unit:physicalUnit, fullScale:[…], levelOffsetDb:[…], calibrated:[…]}`, `channels[{ch, rmsDb, peakDb, peakAtSec, crestDb, dcOffset, minSample, maxSample, clipped, samples` + if calibrated `rmsAbsDb, peakAbsDb, absUnit, rmsPhysical, peakPhysical}]` |
| `measure.spectrum` | `spectrum(eng, SpectrumParams{t0,t1,channels,fftSize,windowType,overlap}, &cal, progress)`; if `peaks` is requested: `peaks(spec, PeakParams{maxCount,minProminenceDb,minLevelDb,minFreqHz,maxFreqHz})` | recommended defaults = the document's panel settings (`settings.fftLocalN`, Hann window) to get "the on-screen numbers"; `overlap` absent = panel placement | `analysis{fftSize, window:windowName, enbwBins, framesAveraged, framesCapped, binHz, sampleRate, levelConvention, unit:"dBFS"}`, `channels[{ch, relDb[], absDb[]?, absUnit?, calibrated, fullScale, levelOffsetDb}]` (`relDb` = n/2+1 values, bin k ↔ k·binHz), `peaks[{ch, freqHz, relDb, absDb?, prominenceDb, bin, binRelDb}]` |
| `measure.thirdOctave` | `thirdOctave(eng, ThirdOctaveParams{t0,t1,channels,mode,order,fftSize,windowType}, &cal, progress)` | `mode:"reference"` (dB-SPL pipeline: 65536 Hanning 50 %, Butterworth order 2·`order`) or `"iec"` (26 bands summed from the spectrum, ENBW); recommended default = the panel's: `reference` if the document is in Pascal, otherwise `iec` | `mode`, `window` (`"hanning-65536"` in reference mode, otherwise `windowName-fftSize`), `order`, `unit` (= `absUnit` if calibrated, otherwise `"dB"`), `levelOffsetDb` per channel, `framesUsed`, `framesCap`, `bands[{fcHz:centresHz[b], relDb, absDb?}]` per channel, `emptyBandValue:-999`. If `noData[b]`: write `-999` for `relDb` **and** `absDb` (the raw stored value is −999 + 20·log10(2e-5)). In reference mode, absolute = `relDb + levelOffsetDb` (= level of the reference analyser) |
| `measure.curve` | `kind:"freqVsTime"\|"ampVsTime"` → `timeCurve(eng, chans, t0, t1, track, mask, out, progress)`; `kind:"ampVsFreq"` → `spectrumCurve(eng, chans, t0, t1, kTimeModeFFT, Hann, track, mask, out, progress)`; smoothing → `smoothSeries(v, window)` (amplitude; frequency too outside ampVsFreq, like the panel); `kind:"rms"\|"peak"` → `levelCurve(eng, ch, t0, t1, windowMs/1000, hopMs/1000, LevelKind, progress)` | `track`: `"max"\|"total"\|"weighted"` → 0/1/2; `mask` = copy of the spectral selection (or `nullptr`, option `useSpectralSelection`) | curves: `{kind, track, unit:"dBFS", channels[{ch, x[], ampDb[], freqHz[]}]}` (`x` in s or Hz); rms/peak: `{kind, unit:"dBFS", t0, hopSec, windowSec, count, values[]}` |
| `measure.probe` | outside the module: `app.spectralEngine().probeMagnitude(eng, ch, t, f, settings, 0)` (non-const, shared tile cache → **message thread**) | `t`, `f`, `channel`; FFT/hop/window = spectrogram settings (`app.settings`) | `{t, f, binHz: rate/fftSize, fftSize, magDb, unit:"dBFS", absDb?: magDb+levelOffsetDb(ch), absUnit?}` (`frameSec` from §6.6: not provided by `probeMagnitude`, omit it or recompute it) |
| `measure.spectrogramData` | outside the module: `SpectralEngine::computeTile(eng, ch, startSample, numSamples, fftSize, hop, windowType)` (const, no cache, usable from a worker) tile by tile in a job | `fMax` → truncate to `ceil(fMax/binHz)` bins | `{frames, bins, binHz, hopSec, unit:"dBFS", inline, format:"f32"\|"csv", path, layout:"frames x bins, float32 little-endian, C order"}` |
| `measure.harmonics` | `spectrum(...)` then `harmonics(spec, 0, HarmonicsParams{fundamentalHz \| anchorHz+anchorN, maxK, search, toleranceHz, detect})` | `anchorHz`+`n` = panel semantics (the anchor is harmonic no. n); explicit `fundamentalHz`; nothing = auto-detection; `search:"peak"\|"nearestBin"` (the panel uses `nearestBin`) | `{fundamentalHz, autoDetected, toleranceHz, unit, fftSize, window, harmonics[{k, expectedHz, freqHz, relDb, absDb?, relToFundamentalDb, valid, isLocalMax}], thdPercent}` (`fundamentalHz:0` → `not_found` error advised) |

## Instructions for the main agent (merge)

1. Merge `ai-control-measures` into `ai-control` (conflicts expected only on
   `src/CMakeLists.txt` — one line added after `SpectralEngine.cpp` — and on the two lines of
   `tests/UnitTests.cpp` if lot 0/1 also adds fragments: keep both).
2. Write `src/control/commands/Measure.cpp` with the wrappers from the sheet; no computation to add
   apart from `probe`/`spectrogramData`. Mark `thirdOctave` (reference), multi-window `spectrum`,
   `curve`, `spectrogramData` as `LongRunning`; `levels` too beyond a few seconds.
3. Add to the busy guard the refusal of `doc.close` / mutating commands on a document that a measure
   job is reading (see "Lifetime").
4. Wrapper tests: reuse the signals from `measures.inc` (full-scale sine at 1 Pa →
   `rmsAbsDb` 90.97; 1 kHz band ≈ 91 dB-SPL) and check the JSON fields/units.
5. REPORT §2.2 ("local `static` RMS at `MainWindow.cpp:4358`", inline peak in
   `normalize`) was not touched: those files belong to other agents. They can be made to
   go through `measures::levels` later if a single definition is wanted.

## Suite results

macOS 26 arm64, AppleClang, Debug, `-DSPECTRO_ENABLE_AI=OFF -DSPECTRO_ENABLE_FFMPEG=OFF`,
`ctest --test-dir build -j 8` (8 shards):

| Run | Result |
|---|---|
| Base (`ai-control` untouched) | 1 failure: `views#W4: a 5 px repaint draws the same selection pixels as a full paint` (shard 1); the "regression: views" test alone passes again (33/33) → **flaky under load, pre-existing** |
| Branch, commit 1, 1st run | 10/10 shards green |
| Branch, commit 1, 2nd run (`-V`) | 4747/4748 checks; only failure: the same flaky `views#W4` |
| Branch, commit 2 (review fixes, `-V`) | **4760/4760**, 10/10 shards green |
| `SC_TEST_FILTER="regression: measures"` | 145/145 |
| GitHub Actions CI `build.yml` on `52d5267` (run 42154351288, AI ON) | **windows-ai ✓, macos-ai ✓, linux-ai ✓** (MSVC, AppleClang, GCC) |

No regression compared to the base. The only failure observed is the pre-existing flakiness of
`views#W4` (selection rendering, unrelated to the measures).
