// SpectroCraft Mobile - a microphone's frequency compensation, measured
// against a calibrated reference microphone (issue #80).
//
// The phone listens to one microphone at a time, so the comparison is two
// takes of the same steady sound at the same place: first the reference
// (an external microphone calibrated in Pa, with its known frequency response
// imported from its maker's file), then the microphone to compensate. Both
// takes are measured in 1/3 octaves (the desktop's IEC pipeline, as the
// Analyse tab). The microphone's calibration is the median gap between the
// two over 250 Hz - 4 kHz (dB-SPL of a full-scale signal); the compensation is
// what remains per band (dB to add to its levels). Bands too far below the
// loudest one in either take are not trusted and take the nearest trusted
// band's value.
//
// The compensation is saved as a CSV text file per microphone in the
// recordings folder's "Calibrations" folder (it survives app updates and can
// be loaded again), and attached to each recording the microphone makes
// (metadata "micCompensation"): the analysis applies it. The recorded sound
// stays raw.
#pragma once
#include <JuceHeader.h>
#include <map>
#include <vector>

namespace scm::miccomp
{
/** A frequency response: dB at rising frequencies. */
struct Response
{
    std::vector<double> hz, db;
    juce::String name;            // the file it came from ("" = none)
    bool empty() const noexcept { return hz.empty(); }
    /** dB at f: linear in log frequency between the points, flat beyond the
        ends; 0 when empty. */
    double at (double f) const noexcept;
};

/** Reads a response file (a maker's CSV, TXT or FRD): one point per line,
    the frequency then the dB (a third column, the phase, is ignored),
    separated by commas, semicolons, tabs or spaces. Header and comment lines
    are skipped; a decimal comma is read when the separator is not a comma.
    The points are sorted by frequency. Empty, with `error` set, when no line
    holds a point. */
Response parseResponse (const juce::String& text, juce::String& error);

/** 1/3-octave band levels in dBFS (the desktop's IEC pipeline, 8192-point
    frames): what a take of one microphone measured. */
struct Bands
{
    std::vector<double> hz, db;
};
Bands thirdOctaveDbfs (const float* samples, int numSamples, double sampleRate);

struct Compensation
{
    juce::String input;                       // the microphone it corrects
    juce::String reference;                   // the reference microphone (and its response file)
    juce::Time when;
    double fullScaleDbSpl = 0.0;              // its calibration: dB-SPL of a full-scale signal
    std::vector<double> hz, correctionDb;     // dB to add to its levels, per 1/3 octave
    std::vector<double> referenceDbSpl, phoneDbfs;   // what the comparison measured (in the CSV)
    std::vector<bool> trusted;                // bands with enough sound in both takes

    bool valid() const noexcept { return ! hz.empty() && hz.size() == correctionDb.size(); }
    /** The correction at f: linear in log frequency between the bands, flat
        beyond the ends; 0 when not valid. */
    double at (double f) const noexcept;
    /** Full-scale pressure (Pa) of the calibration. */
    double fullScalePa() const noexcept;

    juce::String toCsv() const;
    static Compensation fromCsv (const juce::String& text, juce::String& error);
    /** The recording's metadata ("micCompensation"): input, hz, db, fullScaleDbSpl. */
    juce::var toVar() const;
    static Compensation fromVar (const juce::var&);
};

/** Compares the two takes (see the file comment). `error` says why it failed:
    not enough sound between 250 Hz and 4 kHz, or a calibration out of range. */
Compensation compare (const Bands& reference, double referenceFullScaleDbSpl, const Response& referenceResponse,
                      const Bands& phone, juce::String& error);

/** The CSV files' folder: "Calibrations" in the recordings folder. */
juce::File folder();
/** The CSV of a microphone ("<name> compensation.csv"). */
juce::File fileFor (const juce::String& input);
/** The saved compensation of a microphone (not valid when there is none). */
Compensation forInput (const juce::String& input);
/** Writes the compensation's CSV (fileFor its input). */
bool save (const Compensation&, juce::String& error);
} // namespace scm::miccomp
