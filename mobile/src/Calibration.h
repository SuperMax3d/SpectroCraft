// SpectroCraft Mobile - amplitude calibration.
//
// The calibration is the desktop's ScaleCalibration in Pascal: the physical
// pressure at digital full scale (|1.0|). It is written into every recording's
// metadata ("scale"), so the desktop shows the file in dB-SPL as it does for its
// own calibrated files.
//
// Two ways to set it, per input (the phone's mic, a USB mic, ...):
//   - typed, like the desktop: full scale in Pa, or the level in dB-SPL that a
//     full-scale signal stands for;
//   - with an acoustic calibrator (default 1 kHz, 94 dB-SPL = 1 Pa RMS): the
//     app records a few seconds, measures the RMS in the 1/3-octave band around
//     the tone and solves fullScale = pRef * 10^(L/20) / rms.
#pragma once
#include <JuceHeader.h>
#include "ScaleCalibration.h"

namespace scm::cal
{
constexpr double kRefPressurePa = 2e-5;

/** dB-SPL of a full-scale signal for a full-scale pressure (Pa). */
double fullScaleDbSpl (double fullScalePa);
/** Full-scale pressure (Pa) for a full-scale level in dB-SPL. */
double fullScalePaFromDbSpl (double dbSpl);

/** Full-scale pressure (Pa) given the measured RMS (linear, full scale = 1)
    of a calibrator producing `refDbSpl`. 0 when rms <= 0. */
double fullScaleFromCalibrator (double rmsLinear, double refDbSpl = 94.0);

/** What a calibrator capture looked like. */
struct CalibratorReading
{
    bool ok = false;
    juce::String problem;      // why it is not usable ("" when ok)
    double bandRms = 0.0;      // RMS in the 1/3-octave band around toneHz
    double totalRms = 0.0;     // RMS of the whole capture
    double bandShare = 0.0;    // band power / total power (0..1)
    double peakHz = 0.0;       // strongest spectral peak
    double peak = 0.0;         // max |sample|
    double spreadDb = 0.0;     // max - min of the band level over 0.25 s slices
    double fullScalePa = 0.0;  // the resulting calibration (0 when !ok)
};

/** Analyses a mono capture of a calibrator (at least 1 s). The tone must
    dominate (>= 90 % of the power in its band), sit within 3 % of toneHz,
    stay steady (spread <= 0.5 dB) and not clip. */
CalibratorReading analyseCalibrator (const float* samples, int numSamples, double sampleRate,
                                     double toneHz = 1000.0, double refDbSpl = 94.0);

/** One stored calibration of an input. */
struct InputCalibration
{
    juce::String input;        // the input's name (device / route)
    double fullScalePa = 0.0;
    juce::String method;       // "typed" or "calibrator 94.0 dB @ 1000 Hz"
    juce::Time when;

    ScaleCalibration toScale() const;
    bool valid() const noexcept { return fullScalePa > 0.0; }
};

/** Per-input calibrations kept in the app's settings. */
class Store
{
public:
    explicit Store (juce::PropertySet* props);
    InputCalibration get (const juce::String& input) const;
    void set (const InputCalibration&);
    void clear (const juce::String& input);
private:
    static juce::String key (const juce::String& input);
    juce::PropertySet* props;
};
} // namespace scm::cal
