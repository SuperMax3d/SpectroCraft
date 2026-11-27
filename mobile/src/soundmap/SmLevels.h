// SpectroCraft Mobile - sound map: the levels of one take.
//
// 1/3-octave band levels through one fixed estimator for every take of a
// map, calibrated or not: the desktop's IEC pipeline (sc::measures::thirdOctave,
// bands summed from 65536-point rectangular frames averaged over the take), the
// calibration applied as a dB offset only. LZeq is the take's RMS level; LAeq
// is summed from the bands with the A weighting.
//
// The source signal: a periodic pink noise (a multisine of period 2^16
// samples, every line from 20 Hz to 20 kHz at a pink level, random phases).
// Played in a loop from a fixed loudspeaker, its band levels at a point do not
// fluctuate from take to take as random noise does (4.34 / sqrt(B T) dB:
// 0.4 dB at 50 Hz for 10 s), so 10 s takes and drift checks hold in every band.
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <vector>

namespace scm::smap
{
struct BandLevels
{
    std::vector<double> centresHz;   // the pipeline's band centres
    std::vector<double> db;          // per band, in `unit`
    juce::String unit;               // "dB-SPL", "dBFS", ...
    double lzeq = -300.0, laeq = -300.0;
    double seconds = 0.0;            // analysed duration
    bool calibrated = false;
    double sampleRate = 0.0;
    juce::String pipeline;           // the estimator, e.g. iec-65536-rect

    bool valid() const noexcept { return ! db.empty(); }
    /** The band whose nominal centre is `nominalHz` (-1 if none). */
    int bandFor (double nominalHz) const noexcept;
};

/** Levels of a WAV, `headSeconds` and `tailSeconds` left out (the tap, the
    shutter sound). The rest must hold one period of the source (65536
    samples); a shorter or silent take gives no levels and an error. Safe on a
    background thread. */
BandLevels analyseTake (const juce::File& wav, double headSeconds, double tailSeconds, juce::String& error,
                        const std::atomic<bool>* cancel = nullptr);

/** The source signal: periodic pink noise, `seconds` long (whole periods),
    16-bit mono WAV, peak -1 dBFS. */
bool writePeriodicPink (const juce::File& wav, double sampleRate = 48000.0, double seconds = 180.0);

/** The reverberation takes' source: `seconds` of exponential sweep (20 Hz -
    20 kHz) then `silenceSeconds` of silence, to play in a loop; 16-bit mono
    WAV, peak -1 dBFS. */
bool writeSweepWav (const juce::File& wav, double seconds, double silenceSeconds, double sampleRate = 48000.0);

/** A-weighting (IEC 61672-1), dB, 0 at 1 kHz. */
double aWeightingDb (double hz) noexcept;

/** IEC nominal 1/3-octave centres, 25 Hz .. 20 kHz. */
const std::vector<double>& nominalCentres();
/** The nominal centre nearest to hz (within 6 %), or 0. */
double nominalFor (double hz) noexcept;
/** "63", "1k", "1.25k", ... */
juce::String bandLabel (double nominalHz);
} // namespace scm::smap
