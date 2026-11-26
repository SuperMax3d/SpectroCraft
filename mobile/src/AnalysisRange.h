// SpectroCraft Mobile - the part of the file the Analyse tab measures.
#pragma once
#include <JuceHeader.h>
#include <algorithm>

namespace scm::analysis
{
struct RangeInput
{
    bool live = false;                  // recording: the last second
    double duration = 0.0;
    juce::Range<double> selection;      // empty: no selection
    bool playheadByHand = false;        // moved by a tap, the lens, Home / End
    double playhead = 0.0;
    double windowSeconds = 0.0;         // one FFT window
    juce::Range<double> visible;        // the views' time range
};

/** The selection; else (issue #79) one FFT window around a playhead placed by
    hand; else the visible range. While recording, the last second. */
inline juce::Range<double> rangeFor (const RangeInput& in)
{
    const double dur = in.duration;
    if (in.live)
        return { std::max (0.0, dur - 1.0), dur };
    if (! in.selection.isEmpty())
        return in.selection;
    if (in.playheadByHand && in.windowSeconds > 0.0)
    {
        const double t0 = juce::jlimit (0.0, std::max (0.0, dur - in.windowSeconds), in.playhead - in.windowSeconds * 0.5);
        return { t0, std::min (dur, t0 + in.windowSeconds) };
    }
    return { std::max (0.0, in.visible.getStart()), std::min (dur, in.visible.getEnd()) };
}
} // namespace scm::analysis
