#pragma once
// SpectroCraft - time, ruler and amplitude-scale text helpers.
// Free functions with no view or Application dependency (they used to live in
// WaveformView.h / WaveformView.cpp), so UI-free code such as AxisScale and the
// mobile app can format and parse times without the desktop views.
#include <JuceHeader.h>
#include "Settings.h"           // TimeAxisMode
#include "ScaleCalibration.h"

// ---------------------------------------------------------------- time axis

/** Chooses a "nice" major tick step (seconds) for a time ruler drawn at
    `pxPerSec` pixels per second, so labels land roughly every 80 px:
    classic 1-2-5 metric steps below one minute (down to microseconds at deep
    zooms), then clean time-unit steps above it (1m/2m/5m/10m/15m/30m,
    1h/2h/5h/...). */
double niceTimeStep (double pxPerSec);

/** Formats a ruler tick value for the chosen step. When the visible range
    `span` exceeds one minute the labels switch to clock style - m:ss above a
    minute, h:mm:ss above an hour (fractional seconds only if the step is
    sub-second). Below that: whole seconds when coarse ("45"), and ms/us at
    deep zooms ("2ms", "500us"). */
juce::String formatTimeTickLabel (double t, double step, double span);

/** Clock-style formatting of an absolute time or duration:
    m:ss[.dddd] below one hour, h:mm:ss[.dddd] above it, with up to
    `maxDecimals` fractional digits and trailing zeros trimmed ("2:35",
    "1:02:35.127"). */
juce::String formatClockTime (double t, int maxDecimals = 4);

/** Footer/readout formatting for an absolute time or duration: decimal
    seconds below one minute ("12.500"), m:ss above it, h:mm:ss above an hour.
    Used by the status bar's editable selection fields and playhead readout. */
juce::String formatFooterTime (double t);

/** Parses user-entered time text: plain seconds ("12.5") or clock style
    ("2:35", "02:35.5", "1:02:35"). Returns false for anything else; `out`
    receives the value in seconds on success. */
bool parseTimeText (const juce::String& s, double& out);

// ---- Time-axis scale helpers (shared by rulers, footer and marker panel) ----

/** Clock text with a fixed 4-digit fraction: "hh:mm:ss.xxxx" (hours only
    when non-zero). Used for the Time Offset prompt and its display. */
juce::String formatOffsetTime (double t);

/** Like parseTimeText but also accepts a leading +/- sign. */
bool parseSignedClock (const juce::String& s, double& outSecs);

/** Displayed value for the active time-axis mode. Every mode carries the
    axis entry's offset; `scale` is its multiplier (1.0 when the axis is
    unscaled). */
double timeToDisplayValue (double tSecs, TimeAxisMode m, double offsetSecs, double rate,
                           double scale = 1.0);

/** Footer/marker text for a real time under the active axis mode. */
juce::String formatDisplayedTime (double tSecs, TimeAxisMode m, double offsetSecs, double rate,
                                  double scale = 1.0);

/** Parse footer/marker text back to real seconds under the active axis mode.
    Sample mode accepts integer/decimal sample counts; the time modes accept
    signed plain-seconds or clock text. */
bool parseDisplayedTime (const juce::String& s, TimeAxisMode m, double offsetSecs,
                         double rate, double& outSecs, double scale = 1.0);

/** Nice 1-2-5 tick step in SAMPLES for a ruler showing `samplesPerPixel`. */
double niceSampleStep (double samplesPerPixel);

// ---- Waveform vertical-axis scale labels ----
// mode: 0=Wav Units (file calibration), 1=dB, 2=Normalized.
/** Label text for the grid line `gridDb` dB below full scale.
    `positiveSide` selects the +a / -a sign in Normalized mode. */
juce::String formatWaveVScaleLabel (double gridDb, int mode, const ScaleCalibration& sc,
                                    bool positiveSide, int ch = -1);
/** Axis title for the waveform's left axis under the active vertical scale. */
juce::String waveVScaleTitle (int mode, const ScaleCalibration& sc);
