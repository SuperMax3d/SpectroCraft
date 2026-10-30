#pragma once
// SpectroCraft - Filter EQ (graphical curve equalizer).
//
// High-quality zero-phase FFT equaliser following Audacity's classic
// "Filter Curve" algorithm (src/effects/Equalization.cpp in the 3.0.x tree):
//   - control points on a log-frequency axis, dB interpolated linearly in
//     log10(f) so the response passes exactly through every point,
//   - the per-bin gain is converted to a windowed complex filter function
//     (Blackman time-domain window of ~4001 samples),
//   - audio is processed with an overlap-add FFT loop (16384-point windows,
//     hop = window - filter length), which reproduces the drawn curve with
//     no phase distortion.

#include <JuceHeader.h>
#include <vector>

namespace sc { namespace eq {

/** One control point of the EQ curve: frequency in Hz, gain in dB. */
struct EqPoint
{
    double freqHz = 1000.0;
    double gainDb = 0.0;
};

/** Interpolated curve gain (dB) at `freqHz`.
    The points must be sorted by ascending frequency. Gains are interpolated
    linearly in log10(frequency); outside the point range the end values hold.
    An empty list is flat (0 dB). */
double curveGainDb (const std::vector<EqPoint>& pts, double fLo, double fHi,
                    double freqHz);

/** True when every point sits within 1e-6 dB of zero (a no-op curve). */
bool isFlatCurve (const std::vector<EqPoint>& pts);

/** Applies the EQ curve to one mono channel in place.
    @param data   Samples (resized never; len samples are read and written).
    @param len    Number of samples.
    @param rate   Sample rate in Hz.
    @param pts    Control points, sorted by ascending frequency. */
void applyEq (float* data, int len, double rate, const std::vector<EqPoint>& pts);

/** A named preset curve. */
struct EqPreset
{
    juce::String name;
    std::vector<EqPoint> points;
};

/** Serialises a point list to "freq,gain;freq,gain;..." (empty for no points).
    Frequencies are in Hz, gains in dB. */
juce::String serializePoints (const std::vector<EqPoint>& pts);

/** Parses the format produced by serializePoints(); invalid entries are skipped.
    The result is sorted by ascending frequency. */
std::vector<EqPoint> parsePoints (const juce::String& text);

/** Serialises user presets, one line each: "name|freq,gain;...". */
juce::String serializeUserPresets (const std::vector<EqPreset>& presets);

/** Parses the format produced by serializeUserPresets(); invalid lines are skipped.
    Duplicate names keep their first occurrence. */
std::vector<EqPreset> parseUserPresets (const juce::String& text);

/** The built-in preset set: Audacity's default curves (EQDefaultCurves.xml)
    plus a Flat reference. */
const std::vector<EqPreset>& presets();

}} // namespace sc::eq
