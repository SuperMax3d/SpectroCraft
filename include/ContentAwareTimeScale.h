#pragma once
// SpectroCraft - content-aware time scaling.
//
// The effect changes a range's duration by removing or duplicating short
// time-domain segments chosen from the STFT: gaps between consecutive FFT
// frames whose spectral magnitude barely changed are "stable" (silence,
// steady tones) and are safe to cut or repeat; onsets/transients are
// detected via spectral flux and protected. Edits are applied in the time
// domain with short crossfades so the joins stay smooth - STFT frames are
// never dropped or repeated directly.

#include <vector>
#include <cstdint>
#include "juce_core/juce_core.h"   // juce::Random, juce::String

namespace sc { namespace cat {

/** Pass-1 STFT analysis of an in-memory sample range. All positions are
    local: 0 = first sample of the analysed range. */
struct Analysis
{
    int fftSize = 0;          // N (power of two)
    int hop = 0;              // H = N/4 samples between consecutive frames
    int numFrames = 0;        // F frames -> F-1 gaps
    double rate = 0.0;

    std::vector<double> diff;         // per gap: mean |mag[i+1]-mag[i]| over bins (raw)
    std::vector<double> normDiff;     // per gap: min-max normalised to [0,1]
    std::vector<double> stability;    // per gap: exp(-k * normDiff), k = 3
    std::vector<bool> protectedGap;   // gaps inside onset-protected zones

    int numGaps() const { return numFrames > 1 ? numFrames - 1 : 0; }
};

/** A run of consecutive stable gaps (local gap indices). */
struct Region
{
    int gapStart = 0;        // first gap index of the run
    int gapCount = 0;        // number of consecutive gaps in the run
    double normDiffMean = 0.0;   // mean normalised diff over the region's gaps
};

/** A time-domain edit in local sample coordinates (0 = start of range). */
struct Edit
{
    int64_t start = 0;       // first sample of the segment
    int64_t len = 0;         // length of the segment in samples
    bool duplicate = false;  // true: insert a copy right after [start, start+len)
                             // false: remove it (crossfading both sides)
};

/** Pass 1: STFT magnitude differences between consecutive frames plus onset
    protection. Returns false when the range is too short to analyse. */
bool analyze (const std::vector<std::vector<float>>& chans, int nchans, double rate, Analysis& out);

/** Group gaps with stability >= thresholdStab into regions. Protected gaps
    are never included and break runs; regions shorter than minGaps are
    dropped. Regions come back in gap order. */
std::vector<Region> findRegions (const Analysis& a, double thresholdStab, int minGaps);

/** All removal edits for the "Reduce Gaps" mode: every run of gaps whose raw
    diff is below the given percentile (0-100) of all diffs in the range. */
std::vector<Edit> reduceGapsEdits (const Analysis& a, double percentile, int minGaps);

/** Total number of samples removed by reduceGapsAtPercentile() - used for
    the dialog's live "resulting duration" preview. */
int64_t removableSamplesAtPercentile (const Analysis& a, double percentile, int minGaps);

/** Plan edits that change the range length by deltaSamples (negative =
    shorten). Stable regions are preferred exponentially over their spectral
    difference (with a light bonus for longer regions) and picked with
    weighted randomness so the same region is not always reused; protected
    onset gaps are never touched. */
std::vector<Edit> planTimeScaleEdits (const Analysis& a, int64_t deltaSamples, juce::Random& rng);

/** Apply the planned edits in the time domain with short crossfades. Returns
    the new per-channel content for [0, rangeLen). With no edits this is an
    exact copy of the input. */
std::vector<std::vector<float>> applyEdits (const std::vector<std::vector<float>>& chans, int nchans,
                                            double rate, const std::vector<Edit>& edits);

}}  // namespace sc::cat
