# Lot 4 — exportable numeric measures

Status: **done**. The computation module `sc::measures` (extracted from `FFTAnalysisView`, bit-for-bit
equal to the panel, new levels/peaks/harmonics measures) was delivered by the parallel agent
`ai-control-measures` (see `lot-4-core.md`); this file covers the merge and the commands.

## Merge

`ai-control-measures` merged at `8d4ace9` (trivial conflict in `tests/UnitTests.cpp`, both sides kept).

## Commands (`src/control/commands/Measure.cpp`)

| Command | Function | Notes |
|---|---|---|
| `measure.levels {start, end, channels, clipThreshold}` | `measures::levels` | RMS/peak/peak time/crest factor/DC/min/max/clipped per channel, `unit:"dBFS"`, `calibration{unit, fullScale[], levelOffsetDb[], calibrated[]}`, and if calibrated `rmsAbsDb/peakAbsDb/absUnit/rmsPhysical/peakPhysical` |
| `measure.spectrum {…, fftSize, window, overlap, values, peaks{…}}` | `measures::spectrum` + `peaks` | default `fftSize` = the panel's (`settings.fftLocalN`) so the on-screen numbers are reproduced; refused beyond 1 M inline values |
| `measure.thirdOctave {…, mode, order, fftSize, window}` | `measures::thirdOctave` | default = the panel's choice: `reference` (dB-SPL) for a file in Pascal, `iec` otherwise; empty bands `-999`; order ∉ {1,2,3,4,5,10,20,30,40} → `invalid_params` |
| `measure.harmonics {…, channel, fundamentalHz | anchorHz+n | auto, maxK, search, toleranceHz}` | `measures::harmonics` | THD if the fundamental is valid; no fundamental → `failed` with a hint |
| `measure.curve {kind: rms|peak|ampVsTime|freqVsTime|ampVsFreq, …}` | `levelCurve` / `timeCurve` / `spectrumCurve` | `track`, `smooth`, `useSpectralSelection` (copy of the spectral selection) |
| `measure.probe {t, f, channel}` | `SpectralEngine::probeMagnitude` | message thread (tile cache shared with the views), spectrogram settings |
| `measure.spectrogramData {…, fftSize, hop, fMax, format}` | `SpectralEngine::computeTile` on a **local instance** (no shared cache: safe on a worker) | inline ≤ 1 M values, otherwise an `f32` file (readable with `numpy.fromfile(p,'<f4').reshape(frames,bins)`) or `csv` in `<temp>/SpectroCraft/control-out/`; refused beyond 50 M values |
| `export.spectrogram {path, start, end, width, height, overlays, confirm}` | `renderToPng` (local instance) | job; `confirm` if the target exists; **overlays → `needs_ui`** (see deviations) |
| `markers.exportCsv {path, confirm}` / `markers.importCsv {path}` | `MarkerManager::saveCsv/loadCsv` | path perimeter |

Execution: small region → inline on the message thread; large (> 20 M sample-channels for
`levels`, 5 M for the FFT analyses), 1/3-octave reference mode, spectrogram, export → **exclusive
job** (`mutating:true`): while it reads the document, neither edits nor closing get through (the
registry's busy guard); and a measure job refuses to start alongside a mutating job (the
`measure.*` commands are `Query`, so the general guard does not stop them). The worker receives only a
reference to the `AudioEngine` (`readSamples` reads, thread-safe) and a **copy** of the calibration.

## Tests (`tests/regression/measure_commands.inc`, "regression: measure_commands")

Full-scale sine on an exact bin (~1 kHz), file calibrated to 1 Pa:
`measure.levels` peak 0 dBFS, RMS −3.01 dBFS, **90.97 dB-SPL**; `measure.spectrum` **identical to
`sc::measures::spectrum` (the panel's code)** to within 1/1000 dB (JSON rounding), tone bin −3.01 dB,
strongest peak at the tone; `measure.thirdOctave` reference mode as a job, unit dB-SPL, **1 kHz band
≈ 91 dB-SPL** (same reference as the dB-SPL tests of 2026-11-03), invalid order refused;
harmonics; rms curves (−3.01 dBFS per window) and ampVsFreq; probe; spectrogram inline and
f32 file of size frames×bins×4; PNG export, `confirm_required`, `needs_ui` for the overlays;
marker CSV round trip; measure job refused during a mutating job.

Results (macOS arm64, Debug + AI): "measure,control" 1018/1018; full suite **5998/5998**.

## Deviations

- `export.spectrogram` with overlays: the command answers `needs_ui` **even in the open app**.
  `MainWindow::preparePngExport` (`SpectralView::PngExportJob`) is not yet exposed by `UiBridge`;
  export without overlays (headless rendering identical to `--test-spectral`) is available everywhere.
  To do in lot 6 with the migration of `FileExportSpectral`.
- No `.npy` writing (optional in the plan): `f32` + shape is enough.
- `measure.probe` does not give `frameSec` (the `SpectralEngine` function does not expose it).

## Instructions

- The "on-screen" values assume the same settings as the panel (FFT size, window): the
  commands' defaults read the app's current settings.
