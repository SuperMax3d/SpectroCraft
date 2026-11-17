# Model panel (issue #40)

The Model panel learns a **sound model** of a recording: tonal whines
(harmonics) and broadband hiss (noise), each one driven by a parameter from the
Tables panel such as fan speed or engine speed. It then renders that model as
audio, to compare it with the recording or to drive it from other parameter
values.

Open it from **Window > Model**. It is closed in the built-in workspaces.

Code:
- `include/SoundModel.h`, `src/SoundModel.cpp`: data model, analysis, synthesis, JSON.
- `src/ApplicationModel.cpp`: actions on the active file.
- `include/ModelPanel.h`, `src/ModelPanel.cpp`: the panel.
- `SpectralView::drawModelPickGuide`: the picking guide.

Tests: `tests/regression/model.inc`.

## Workflow

The example from the issue: a car's cooling fan, then the engine.

1. Import the parameters with **Tables > Import CSV**, for example a `Fan` table with a `Speed` column. The model refers to a parameter as `Table.Param`, here `Fan.Speed`.
2. Click **+ Harmonic** to add the fan's whine.
   1. The component starts on the first parameter. If a time range is selected, it becomes the component's first range.
   2. Set the **fundamental** in one of two ways:
      - type it: *120 Hz at 100* means 120 Hz when `Fan.Speed` = 100;
      - click **Pick**, then click the fundamental on the spectrogram. The click snaps to the strongest peak within 6 % and takes that moment's parameter value.
   3. Set the **harmonics**:
      - type the orders, e.g. `1, 2, 3, 6` (decimals such as 0.5 or 1.5 are allowed);
      - or click **Pick Harmonics** and click on the spectrogram. A guide follows the mouse: the nearest order is highlighted along the whole view, following the parameter, and a click adds or removes it. Esc stops.
   4. **Tracks** (method 2 of the issue): **Add Track...** offers the stored selection groups, then the single stored selections. A group's selections are pieces of the same harmonic, so a harmonic whose path is incomplete can be put back together. A track's frequency is measured in its selections and learned against the parameter.
   5. Add **ranges** (time selection, then **Add Selection**). For a harmonic component, they are pooled.
   6. Options:
      - **Rising / falling levels** learns separate levels while the parameter rises, falls or holds (acceleration, deceleration, steady state).
      - **Reproduce variation** renders the measured level and frequency spread around the learned curves.
3. Click **+ Noise** to add the fan's hiss.
   - Each range is one **state**, taken at its mean parameter value.
   - States are interpolated in dB by the parameter.
   - Use a range for each fan speed you have.
4. **Analyse All** measures every component, top to bottom. Each component is measured with the components **above** it taken out first:
   - Harmonics above are **notched out** of a noise analysis (± tolerance ± 4 bins).
   - Harmonics above are **subtracted** from a harmonic's peak when they fall on it.
   - Noise above is **subtracted** from a noise spectrum, per band, after averaging.

   So the engine, added below the fan and analysed over ranges where the fan also runs, is learned without it. Order matters: after **Move Up / Down**, analyse again.
5. Use the model:
   - **Compare** creates a new file:
     - channel 1 is the simulation, channel 2 the original;
     - it holds every range used for the analysis, merged and back to back;
     - the markers and the Tables parameters of those ranges follow, and the gaps are cut out.
   - **Render Selection** renders the selected time range into a new channel of the file. It is one undo step.
   - **Render CSV...** creates a new file rendered from a CSV of parameters:
     - The CSV is the same format as the Tables import (a time column plus one column per parameter) and becomes the new file's table.
     - A parameter is matched by `Table.Param`, then by its name alone, then by the part after the last dot, so a CSV named differently from the original table still matches.
     - Every component to render needs its parameter in the CSV.
   - **Export...** writes the JSON described below. **Import...** replaces the model.

The model is saved with the file in its metadata (`soundModel`, like the
markers and tables). Editing the model marks the file as modified. Model edits
themselves are not undoable; Render Selection is.

## What the analysis measures

All levels are in dB re full scale:
- **harmonic lines**: RMS level (dBFS; a full-scale sine is -3.01);
- **noise**: power spectral density in dB re 1/Hz (white noise of standard deviation σ at rate *fs* reads 10·log10(2σ²/*fs*)).

Frames:
- Hann window, FFT size from the panel (4096 by default), hop N/4.
- One-sided power per bin, normalised so that the bins of a sine sum to its mean power A²/2 and white noise sums to its variance.

Harmonic lines, per frame:
- **Search band:** the strongest bin within ± tolerance (3 % by default, at least ± 2 bins) of `order × f0(p)`. For a track, the band is the stored selection's band at that time.
- **Power:** summed over ± 3 bins, minus the noise floor of the same bins. The floor is the median of ± 30 bins, leaving out ± 6 around the peak.
- **Frequency:** parabolic interpolation in dB.
- **Below the noise:** a line is counted below the noise when its bins are less than 3 dB over the floor. The report lists lines that are mostly below the noise.
- **Learned curves:**
  - The frames are binned by parameter value (24 bins over the range seen), and each bin's mean power becomes a point of the level curve.
  - With *Rising / falling levels*, a frame counts as steady when |dp/dt| is under 2 % of the learned span per second (dp/dt is taken over ± 0.1 s). Otherwise it goes to the rising or falling curve.
  - Order lines also learn a **tuning**: the mean measured/nominal frequency ratio, within the tolerance.
  - Tracks learn **frequency vs parameter**. Beyond their data they keep their last frequency/parameter ratio.

Noise states:
- **Averaging:** a range's frames are averaged per bin, leaving out the bins of the harmonics above.
- **Bands:** the result is reduced to 1/24-octave bands from 20 Hz up to just below Nyquist.
- **Noise above:** the noise components above are subtracted from the band means, with a floor at -20 dB of the measured value. The report flags it when more than 5 % of the bands hit that floor (over-subtraction).

## Rendering

- **Parameters:** the parameters are read at the rendered times, with their end values held before and after a table.
- **Harmonic lines:** sines with a continuous phase. Frequency and amplitude are updated every 64 samples:
  - `f = order × f0Hz × p / f0AtParameter × tuning`, or the track's curve;
  - `A = √2 × 10^(level/20)`.
- **Rising / falling levels:** with dynamics on, the rising or falling level blends in between one and two times the steady threshold.
- **Variation:** slow random modulation (about 3 Hz), with the measured spreads.
- **Near Nyquist:** lines fade out from 0.9 × Nyquist.
- **Noise:** white noise is shaped by the interpolated spectrum, frame by frame. Square-root Hann windows at 50 % overlap are used, so a flat gain gives the noise back unchanged.
- **Seeding:** the noise is seeded, so a render repeats exactly.
- **Below the learned range:** a component **fades out** when its parameter falls below the range the analysis saw. It is full down to 5 % of the span below the lowest value and silent at 15 %. This way a fan at 0 % (off) is silent. Above the range, the last values are held.

## JSON (Export)

```json
{
  "format": "SpectroCraft sound model", "version": 1,
  "units": "...", "synthesis": "...",
  "analysis": { "sampleRate": 48000, "fftSize": 4096, "tolerancePct": 3, "steadyRatePct": 2,
                "parameterBins": 24, "bandsPerOctave": 24 },
  "bandsHz": [20.0, 20.58, ...],
  "components": [
    { "id": "c1", "name": "Fan", "type": "harmonic", "enabled": true, "parameter": "Fan.Speed",
      "channel": 0, "ranges": [[5, 25]], "analysed": true, "learnedRange": [30, 60],
      "fundamental": { "hz": 120, "atParameter": 60 }, "dynamics": false, "variation": false,
      "lines": [ { "order": 1, "tuning": 1.0,
                   "level": { "p": [30.6, ...], "db": [-29.0, ...] },
                   "accel": {...}, "decel": {...},             // with dynamics
                   "selections": ["Sel A"], "freq": { "p": [...], "hz": [...] },   // tracks
                   "levelSpreadDb": 0.2, "freqSpread": 0.001, "frames": 309, "framesBelowNoise": 0 } ] },
    { "id": "c2", "name": "Hiss", "type": "noise", "parameter": "Fan.Speed", "ranges": [[11, 19], [31, 39]],
      "states": [ { "t0": 31, "t1": 39, "parameter": 30, "psdDb": [-73.1, ...] } ] }
  ]
}
```

The `synthesis` string repeats the rendering formulas for a reader of the file.

## Limits and choices to confirm

- **One parameter per component.** Two parameters, e.g. speed and load, would need a grid of cells. Max's earlier prototype had one; this first version keeps a single parameter.
- **Noise states are one per range.** There is no automatic binning of a long ramp into states.
- **The fundamental scales linearly with the parameter** (f0 ∝ p). A non-proportional fundamental can be modelled with tracks.
- **"Shape and variation"** is reproduced as the measured spread of level and frequency, not as a learned waveform shape.
- **No undo for model edits**, and no AI-control commands yet.
- **Time ranges do not follow edits** of the file (a deletion before a range does not shift it).
