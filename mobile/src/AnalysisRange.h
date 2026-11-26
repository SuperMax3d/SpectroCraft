// SpectroCraft Mobile - the part of the file the Analyse tab measures.
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

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

/** A pinch on one axis of a plot (issue #79): [lo, hi] shrunk by `factor`
    (> 1 zooms in) so that the value under the fingers' midpoint at the start
    (`anchorStart`, 0..1 along the axis) sits under their midpoint now
    (`anchorNow`): zoom and pan in one. In log space when `logScale`. Kept
    inside [minV, maxV], at least `minSpan` wide (a ratio on a log axis). */
inline std::pair<double, double> zoomAxis (double lo, double hi, double anchorStart, double anchorNow,
                                           double factor, bool logScale, double minV, double maxV, double minSpan)
{
    const auto to = [logScale] (double v) { return logScale ? std::log (std::max (1e-9, v)) : v; };
    const auto from = [logScale] (double u) { return logScale ? std::exp (u) : u; };
    const double a = to (lo), b = to (hi), ma = to (minV), mb = to (maxV);
    const double minS = logScale ? std::log (std::max (1.0 + 1e-9, minSpan)) : minSpan;
    double span = (b - a) / std::max (1e-9, factor);
    span = std::clamp (span, std::min (minS, mb - ma), mb - ma);
    const double pivot = a + anchorStart * (b - a);
    double na = pivot - anchorNow * span, nb = na + span;
    if (na < ma) { nb += ma - na; na = ma; }
    if (nb > mb) { na -= nb - mb; nb = mb; }
    return { from (na), from (nb) };
}

/** The level of a stored curve at frequency f, to show the difference from
    the current one (issue #79). Bands: the band whose centre is nearest (in
    octaves); a spectrum: linear between the two bins around f. Nothing
    outside the curve. `freqs` rises. */
inline std::optional<double> valueAt (const std::vector<double>& freqs, const std::vector<double>& db,
                                      double f, bool bands)
{
    const size_t n = std::min (freqs.size(), db.size());
    if (n == 0 || f <= 0.0) return {};
    if (bands)
    {
        size_t best = 0;
        double bestD = 1e300;
        for (size_t i = 0; i < n; ++i)
            if (const double d = std::abs (std::log2 (freqs[i] / f)); d < bestD) { bestD = d; best = i; }
        if (bestD > 1.0 / 6.0 + 1e-9) return {};   // outside every band
        return db[best];
    }
    if (f < freqs.front() || f > freqs[n - 1]) return {};
    const auto it = std::lower_bound (freqs.begin(), freqs.begin() + (long) n, f);
    const size_t hiI = (size_t) (it - freqs.begin());
    if (hiI == 0) return db[0];
    const size_t loI = hiI - 1;
    const double t = (f - freqs[loI]) / std::max (1e-12, freqs[hiI] - freqs[loI]);
    return db[loI] + t * (db[hiI] - db[loI]);
}
} // namespace scm::analysis
