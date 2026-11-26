/*
 * RoomAcoustics.h - reverberation from a recording (no UI): the room's impulse
 * response, then the decay per band and what ISO 3382 derives from it.
 *
 * Measuring: a loudspeaker that stays in place plays an exponential sine sweep
 * (makeSweep) followed by silence; a recording of it, started any time before
 * the sweep, is deconvolved by the same sweep (fromSweep). The player and the
 * recorder need no synchronisation: the response is found by its peak. The
 * loudspeaker's harmonic distortion lands before the response and is cut off
 * (Farina's method). A recorded bang (balloon, clapper) is an impulse response
 * as it is (fromImpulse).
 *
 * Analysis (analyse), per octave or 1/3-octave band:
 *   - the response is filtered by a 6th-order Butterworth band-pass (the
 *     IEC 61260 shape, applied in the frequency domain, causal);
 *   - where the decay meets the background noise is found by Lundeby's method;
 *     the squared response is integrated backwards from there (Schroeder), with
 *     the energy of the cut tail added back from the late slope;
 *   - straight lines fitted on that curve give EDT (0 to -10 dB), T20 (-5 to
 *     -25 dB) and T30 (-5 to -35 dB), each as the time for 60 dB;
 *   - clarity C50 / C80, definition D50 and the centre time come from the same
 *     energies.
 * A time is marked reliable only when the noise is low enough (ISO 3382: the
 * response at least 35 dB above the noise for T20, 45 dB for T30; 25 dB here
 * for EDT), its range is reached before the noise, and the band is not too
 * narrow for so short a decay (bandwidth x time > 16, else the filter's own
 * ringing is measured). The fit's r2 and the curvature 100 (T30 / T20 - 1) tell
 * a decay that is not one straight slope (coupled or open spaces).
 *
 * Times in seconds, levels in dB, energies relative. Formulas: docs/reverberation.md.
 */
#pragma once

#include <JuceHeader.h>

#include <cmath>
#include <limits>
#include <vector>

namespace sc::room
{
/** An exponential sine sweep: f(t) = startHz (endHz / startHz)^(t / seconds). */
struct SweepSpec
{
    double startHz = 20.0;
    double endHz = 20000.0;
    double seconds = 8.0;           ///< the sweep itself
    double silenceSeconds = 6.0;    ///< after it, in the file made for the loudspeaker: the room's decay
    double level = 0.5;             ///< peak amplitude (1 = full scale)

    bool valid() const noexcept { return startHz > 0.0 && endHz > startHz * 2.0 && seconds >= 0.5; }

    /** For a file's metadata: { type: "exp-sweep", startHz, endHz, seconds, silenceSeconds, level }. */
    juce::var toVar() const;
    static bool fromVar (const juce::var&, SweepSpec&);
};

/** The sweep's samples at a sample rate (short fades at both ends, no
    silence). endHz is kept under 0.45 x the sample rate. */
std::vector<float> makeSweep (const SweepSpec&, double sampleRate);

struct ImpulseResponse
{
    std::vector<float> h;            ///< starts kPreSeconds before the peak
    double sampleRate = 0.0;
    int direct = 0;                  ///< where the direct sound starts in h (first sample within 20 dB of the peak)
    int peak = 0;                    ///< the largest sample
    double startInRecording = 0.0;   ///< seconds: h[0] in the recording
    double lowHz = 0.0, highHz = 0.0;///< the frequencies the measurement covers (0: unknown, an impulse)
    juce::String problem;            ///< why it cannot be used (empty: usable)

    bool ok() const noexcept { return problem.isEmpty() && ! h.empty(); }
};

inline constexpr double kPreSeconds = 0.05;

/** The response from a recording of a sweep and the silence after it. When the
    sweep's file plays in a loop, a recording of two periods (sweep + silence)
    started at any time holds one whole sweep: the first whole one is used, and
    its response stops before the next sweep's distortion. */
ImpulseResponse fromSweep (const float* recording, juce::int64 numSamples, double sampleRate, const SweepSpec&);

/** The response from a recorded impulse: from just before its peak to the end. */
ImpulseResponse fromImpulse (const float* recording, juce::int64 numSamples, double sampleRate);

/** A reverberation time from a straight line fitted on the decay curve. */
struct Fit
{
    double seconds = std::numeric_limits<double>::quiet_NaN();   ///< the time for 60 dB (NaN: its range is not in the curve)
    double r2 = 0.0;          ///< the fit's coefficient of determination
    bool reliable = false;    ///< low enough noise, range reached, band wide enough

    bool measured() const noexcept { return ! std::isnan (seconds); }
};

struct BandResult
{
    double centreHz = 0.0;        ///< the band's exact centre (0: the whole response, unfiltered)
    double nominalHz = 0.0;       ///< its usual name (31.5, 63, 125 ...)
    Fit edt, t20, t30;
    double inrDb = 0.0;           ///< the response's peak above the noise, both smoothed (impulse-to-noise ratio)
    double truncationSeconds = 0.0;   ///< from the direct sound to where the decay meets the noise
    bool noiseFound = false;      ///< false: no decay above the noise could be followed
    bool filterLimited = false;   ///< bandwidth x T20 <= 16: the filter's ringing, not the room
    double c50 = std::numeric_limits<double>::quiet_NaN();   ///< dB
    double c80 = std::numeric_limits<double>::quiet_NaN();   ///< dB
    double d50 = std::numeric_limits<double>::quiet_NaN();   ///< 0 .. 1
    double centreTimeMs = std::numeric_limits<double>::quiet_NaN();
    double curvaturePct = std::numeric_limits<double>::quiet_NaN();   ///< 100 (T30 / T20 - 1)
    std::vector<float> decayDb;   ///< the decay curve from the direct sound to the truncation, every decayStepSeconds
    double decayStepSeconds = 0.001;
};

enum class Bands { Octave, ThirdOctave };

struct Options
{
    Bands bands = Bands::Octave;
    double lowestHz = 63.0, highestHz = 8000.0;   ///< nominal centres kept (inside what the measurement covers)
    bool wholeResponse = true;                    ///< also the unfiltered response, first
};

/** The standard minimum of bandwidth x reverberation time for the band filter (ISO 3382-1). */
inline constexpr double kMinBandwidthTime = 16.0;

/** The band centres analyse would use for a response (nominal values). */
std::vector<double> bandCentres (const ImpulseResponse&, const Options&);

/** One result per band (the whole response first when asked). Empty when the response is not usable. */
std::vector<BandResult> analyse (const ImpulseResponse&, const Options& = {});

/** "1 kHz", "31.5 Hz", or "All" for the whole response. */
juce::String bandName (const BandResult&);

/** One row per band: band_hz, EDT_s, T20_s, T30_s, C50_dB, C80_dB, D50, Ts_ms, INR_dB,
    EDT_ok, T20_ok, T30_ok, curvature_pct, filter_limited (empty where not measured). */
juce::String toCsv (const std::vector<BandResult>&);
} // namespace sc::room
