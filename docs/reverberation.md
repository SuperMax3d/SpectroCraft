# Reverberation: measuring it with the phone, analysing it on both

Goal: the reverberation of a place per frequency band (EDT, T20, T30, clarity),
from a recording. The mobile app takes the recording, at a known position when
it is a sound map take; the same analysis (`sc::room`, `include/RoomAcoustics.h`,
no UI) runs in the mobile app and in the desktop's Effects > Reverberation
Analysis.

## 1. What to record

| Test | Quality | Needs | Use |
|---|---|---|---|
| **Exponential sine sweep** (20 Hz to 20 kHz in a few seconds, then silence) | The best: 20-30 dB more above the noise than an impulse, and the loudspeaker's distortion is separated from the result | A loudspeaker that stays in place | **The method here** |
| Impulse (balloon, clapper) | Poor repeatability, little low-frequency energy | Nothing | Supported: the recording is the response as it is |
| Interrupted noise | 30-40 dB of range at best, several decays to average | The loudspeaker | Not done |
| MLS | Sensitive to distortion and to anything that moves | The loudspeaker | Not done |

A phone suits this measurement: a reverberation time is the slope of a decay,
so the microphone's frequency response and its calibration do not matter. Its
automatic gain must be off (the app's measurement mode on iOS; to check on
Android).

## 2. The maths

**The sweep** of length T from f1 to f2 (short fades at both ends):

    x(t) = sin( 2 pi f1 T / R (e^(t R / T) - 1) ),   R = ln(f2 / f1)

**The impulse response** h: the recording y deconvolved by the sweep,

    H(f) = Y(f) X*(f) / |X(f)|^2    inside the sweep's range, 0 outside

- The player and the recorder need no synchronisation: h is found by its peak.
- The loudspeaker's harmonic distortion of order k lands T ln(k) / R before
  the response and is cut off (Farina's method).
- A sweep file played in a loop: a recording of two periods (sweep + silence)
  holds one whole sweep, whenever it starts; the first whole one is used, and
  the response stops before the next sweep's distortion.
- The direct sound starts at the first sample within 20 dB of the peak
  (ISO 3382-1); everything below is counted from there.

**Per band**: h is filtered by a 6th-order Butterworth band-pass (the IEC 61260
octave or 1/3-octave shape, applied in the frequency domain, causal).

**Where the decay meets the noise** (Lundeby): the squared response is smoothed,
the noise is taken from its end, the late slope is fitted 7-25 dB above the
noise, their crossing tc is found, and this is repeated with the noise taken
after the crossing.

**The decay curve** (Schroeder's backward integration, cut at tc, with the
energy of the cut tail added back from the late slope):

    E(t) = sum from t to tc of h^2 + C        L(t) = 10 log10( E(t) / E(0) )
    C = noise x fs / k,   k = -slope x ln(10) / 10

**The times**: a straight line fitted on L(t), T = -60 / slope.

| | Range | |
|---|---|---|
| EDT | 0 to -10 dB | what is heard while the sound runs |
| T20 | -5 to -25 dB | |
| T30 | -5 to -35 dB | |

**From the same energies**: C50 and C80 = 10 log10 (early / late) at 50 and
80 ms, D50 = early / total, centre time Ts = sum(t h^2) / sum(h^2).

**What makes a time reliable**:

- the response's peak above the noise (INR, both smoothed): 35 dB or more for
  T20, 45 dB for T30 (ISO 3382), 25 dB for EDT;
- the fit's range is reached before tc;
- bandwidth x T20 > 16, else the band filter's own ringing is measured (a 1/3
  octave at 100 Hz, 23 Hz wide, cannot show less than 0.7 s).

Shown beside them: the fit's r2, and the curvature 100 (T30 / T20 - 1). More
than 10 % means the decay is not one slope: coupled spaces, or a place that is
partly open, where "the" reverberation time means little; read EDT and the
decay curve instead.

## 3. On the phone (Map tab)

1. Menu > Sweep signal for reverberation (WAV): an 8 s sweep and 6 s of silence
   (the map's `sweepSeconds` and `sweepSilenceSeconds` in project.json). Play it
   **in a loop** on the fixed loudspeaker, from another device.
2. Measure with the kind **Reverberation (sweep playing)**: countdown, picture,
   a take of two periods and a second (29 s), picture. The take is analysed at
   once: the whole response and the octaves 63 Hz to 8 kHz.
3. View: the **Level** button becomes T20, EDT or C50. The reverberation takes
   are then coloured by their value for the slider's band (the octave that holds
   it; LAeq or LZeq: the whole response), hollow when the value is not reliable.
   There is no surface: a time is not an energy to average. In the level map the
   reverberation takes show as an R. Tap one for its bands (a ? after a value:
   not reliable).

The takes are WAVs like the others (`points/P012.wav`), with their position and
the sweep they hold in their metadata (`soundMapPoint.sweep`); `reverb.csv`
joins the ZIP export.

## 4. On the desktop

Effects > Reverberation Analysis... on any open recording (the selection, or the
whole file), one channel:

- **A sweep and the silence after it**: its start and end frequencies and its
  length. A sound map's reverberation take fills these by itself.
- **An impulse**: the recording from just before its peak.
- Octaves or 1/3 octaves, 25 Hz to 20 kHz inside what the sweep and the sample
  rate cover.

The table gives EDT, T20, T30, C50, C80, D50, Ts and INR per band, a ? after a
time that is not reliable and the reason in Notes; the plot shows the chosen
band's decay curve with the T20 line. Export CSV writes the table. The window
reads the file and changes nothing.

## 5. Limits

- **The loudspeaker**: ISO 3382 asks for a source that radiates in every
  direction. With an ordinary loudspeaker T20 and T30 hold, EDT and C50 depend on
  where it points (point it up). A small one gives nothing under 60-100 Hz: the
  INR per band says so.
- **Distortion in a long reverberation**: the distortion products come before
  the response, but their own reverberation runs under it. Play at a level the
  loudspeaker handles cleanly.
- **Time-variance**: wind, people moving, and a player whose clock drifts during
  the sweep smear the high bands. One long sweep is better than several short
  ones averaged.
- **Low bands in small rooms**: under the Schroeder frequency (2000 sqrt(T / V))
  a band holds a few room modes and its time depends on the position.
- The band filters are causal: their delay (a few periods of the band's centre)
  is inside EDT and C50 in the lowest bands.
- One sweep per analysis, 2^23 samples at most (a little under 3 minutes at
  48 kHz).
- Not done: the strength G (it needs a calibrated source), speech indices (STI),
  an energy-time curve view for single echoes, the mobile app's own screen for a
  take outside a sound map.

## 6. Code

| File | Role |
|---|---|
| `include/RoomAcoustics.h`, `src/RoomAcoustics.cpp` | the sweep, the deconvolution, the band filters, Lundeby, Schroeder, the fits and their reliability, CSV |
| `mobile/src/soundmap/SmProject.*` | the Reverberation kind of take, `measureReverb`, the bands in project.json, `reverb.csv` |
| `mobile/src/SoundMapPane.*`, `SoundMapView.*` | the take, the Level / T20 / EDT / C50 view |
| `include/ReverbWindow.h`, `src/ReverbWindow.cpp` | the desktop window |

Tests: `mobile/tests/RoomAcousticsTests.cpp` (synthetic rooms of known decay:
impulse, noise, a sweep through a distorting loudspeaker, a looped file, a decay
too short for its band, two slopes; `SC_ROOM_VERBOSE=1` prints every band),
`mobile/tests/SoundMapTests.cpp` (a reverberation take through the project) and
`tests/regression/reverb.inc` (the desktop window; `SC_REVERB_SNAPSHOT=<png>`
writes a picture of it).
