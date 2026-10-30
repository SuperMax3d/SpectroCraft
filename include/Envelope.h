#pragma once
// SpectroCraft - Enveloppe (graphical amplitude-over-time effect).
//
// The user draws control points on a linear time axis (seconds, relative to
// the start of the target range) with a gain in dB. The interpolated curve is
// applied as a per-sample amplitude multiplier 10^(dB/20):
//   - over an active spectral selection: only the masked content is scaled
//     (same STFT masking pipeline as the other masked effects),
//   - else over the time selection, or the whole file when nothing is
//     selected.
// Gains are interpolated linearly in time; outside the point range the end
// values hold flat. Presets store absolute seconds and are proportionally
// rescaled to fit the target duration when loaded (a "Fade In" preset fades
// over the whole selection regardless of its length).

#include <JuceHeader.h>
#include <vector>

namespace sc { namespace env {

/** One control point of the envelope curve: time in seconds relative to the
    start of the target range, gain in dB. */
struct EnvPoint
{
    double tSec = 0.0;
    double gainDb = 0.0;
};

/** Interpolated curve gain (dB) at `timeSec`.
    The points must be sorted by ascending time. Gains are interpolated
    linearly in time; outside the point range the end values hold. An empty
    list is flat (0 dB). */
double curveGainDb (const std::vector<EnvPoint>& pts, double timeSec);

/** True when every point sits within 1e-6 dB of zero (a no-op curve). */
bool isFlatCurve (const std::vector<EnvPoint>& pts);

/** Applies the envelope to one mono channel in place.
    @param data   Samples (len samples are read and written).
    @param len    Number of samples.
    @param rate   Sample rate in Hz.
    @param pts    Control points, sorted by ascending time; sample i is at
                  t = i / rate seconds relative to the range start. */
void applyEnvelope (float* data, int len, double rate, const std::vector<EnvPoint>& pts);

/** A named preset curve (times in absolute seconds). */
struct EnvPreset
{
    juce::String name;
    std::vector<EnvPoint> points;
};

/** Serialises a point list to "t,gain;t,gain;..." (empty for no points).
    Times are in seconds, gains in dB. */
juce::String serializePoints (const std::vector<EnvPoint>& pts);

/** Parses the format produced by serializePoints(); invalid entries are skipped.
    The result is sorted by ascending time. */
std::vector<EnvPoint> parsePoints (const juce::String& text);

/** Serialises user presets, one line each: "name|t,gain;...". */
juce::String serializeUserPresets (const std::vector<EnvPreset>& presets);

/** Parses the format produced by serializeUserPresets(); invalid lines are skipped.
    Duplicate names keep their first occurrence. */
std::vector<EnvPreset> parseUserPresets (const juce::String& text);

/** Proportionally rescales a preset's time axis so its last point lands at
    `targetSecs` (a flat/zero-span curve is returned unchanged). Used when
    loading a preset or the remembered curve into a range of a different
    length than it was drawn for. */
std::vector<EnvPoint> scaleToDuration (const std::vector<EnvPoint>& pts, double targetSecs);

/** The built-in preset set: Flat, Fade In, Fade Out and Dip in Middle. */
const std::vector<EnvPreset>& presets();

}} // namespace sc::env
