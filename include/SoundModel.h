/*
 * SoundModel.h - the sound model learned in the Model panel (issue #40).
 *
 * A model is an ordered list of components. Each one is either
 *   - Harmonic: whines, i.e. lines whose frequency follows a parameter of the
 *     Tables panel (f = order x f0(p), f0 proportional to the parameter), or
 *     a "track" measured inside stored spectral selections; or
 *   - Noise: broadband hiss, one power spectral density per state (a time
 *     range), interpolated between states by the parameter.
 *
 * Analysis measures each component over its own time ranges. The components
 * ABOVE it in the list are predicted from their own parameters and taken out
 * first (harmonics are notched out of a noise analysis and subtracted from a
 * harmonic's peak; noise spectra are subtracted from a noise analysis), so a
 * fan modelled first can be removed from the engine analysed later.
 *
 * Rendering synthesises the enabled components from parameter values:
 * additive sines for the harmonic lines, white noise shaped by the
 * interpolated spectrum (STFT overlap-add) for the noise.
 *
 * Levels are in dB re full scale: harmonic lines as RMS level (dBFS, a full
 * scale sine is -3 dB), noise as power spectral density (dB re 1/Hz). The
 * whole model round-trips through JSON (Export / Import, and the file's
 * metadata). Behaviour and formulas: docs/model-panel.md.
 */
#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace sc::model
{
    enum class Kind : int { Harmonic = 0, Noise = 1 };

    struct TimeRange
    {
        double t0 = 0.0, t1 = 0.0;   // document seconds
        double length() const { return t1 - t0; }
    };

    /** Values learned against the parameter: sorted p, one value each.
        at() interpolates linearly and holds the end values outside. */
    struct ParamCurve
    {
        std::vector<double> p, v;
        bool empty() const { return p.empty(); }
        double at (double x) const;
    };

    /** One line of a harmonic component. */
    struct HarmonicLine
    {
        double order = 1.0;                     // f = order x f0(p) x tuning
        std::vector<juce::String> selections;   // non-empty: a track measured in these stored selections

        // Learned by the analysis.
        ParamCurve levelDb;          // RMS level vs parameter (steady frames when dynamics are on)
        ParamCurve accelDb, decelDb; // while the parameter rises / falls (dynamics)
        ParamCurve freqHz;           // tracks: measured frequency vs parameter
        double tuning = 1.0;         // order lines: measured / nominal frequency
        double levelSpreadDb = 0.0;  // spread of the frame levels around levelDb (variation)
        double freqSpread = 0.0;     // relative spread of the frame frequencies (variation)
        int frames = 0;              // frames measured
        int framesBelowNoise = 0;    // of which the line did not stand out of the noise

        bool isTrack() const { return ! selections.empty(); }
        juce::String label() const;  // "H2", "H1.5", "Track (Sel A, Sel B)"
    };

    /** One state of a noise component: a time range and its spectrum. */
    struct NoiseState
    {
        TimeRange range;
        double param = 0.0;          // mean parameter value over the range
        std::vector<float> psdDb;    // one value per Model::bandHz entry
    };

    struct Component
    {
        juce::String id;             // stable, survives renames and moves
        juce::String name;
        Kind kind = Kind::Harmonic;
        bool enabled = true;
        juce::String parameter;      // "Table.Param" or a unique "Param"; "" = constant
        int channel = 0;             // analysed channel
        std::vector<TimeRange> ranges;

        // Harmonic: f0(p) = f0Hz x p / f0AtParam (f0Hz when there is no parameter).
        double f0Hz = 100.0;
        double f0AtParam = 100.0;
        std::vector<HarmonicLine> lines;
        bool dynamics = false;       // separate levels while accelerating / decelerating
        bool variation = false;      // reproduce the measured level / frequency variation

        // Noise: one state per range.
        std::vector<NoiseState> states;

        // Analysis results.
        bool analysed = false;
        double pLo = 0.0, pHi = 0.0; // parameter values seen by the analysis
        juce::String report;         // one line per finding, for the panel

        bool isHarmonic() const { return kind == Kind::Harmonic; }
        double fundamentalAt (double p) const;
        /** Nominal frequency of a line at parameter p (tuning applied). */
        double lineFreqAt (const HarmonicLine& line, double p) const;
        /** 0..1: fades a component out as the parameter falls below what the
            analysis saw (the fan is off at 0 %), 1 above. */
        double rangeGain (double p) const;
    };

    /** The model of one file. */
    struct Model
    {
        static constexpr int kFormatVersion = 1;

        std::vector<Component> components;   // list order = analysis order

        // Analysis settings.
        int fftOrder = 12;                   // FFT size 2^fftOrder
        double tolerancePct = 3.0;           // harmonic search window around the nominal frequency
        double steadyRatePct = 2.0;          // |dp/dt| under this % of the learned span per second = steady
        int paramBins = 24;                  // parameter bins of the learned curves
        int bandsPerOctave = 24;             // noise spectrum resolution

        double analysisRate = 0.0;           // sample rate of the analysed file
        std::vector<float> bandHz;           // noise band centres

        Component& add (Kind kind, const juce::String& name);
        int indexOf (const juce::String& id) const;
        Component* find (const juce::String& id);
        bool empty() const { return components.empty(); }

        juce::var toVar() const;
        static bool fromVar (const juce::var& v, Model& out, juce::String* error = nullptr);
        juce::String toJson() const;
        static bool fromJson (const juce::String& text, Model& out, juce::String* error = nullptr);
    };

    // ---------------------------------------------------------------- inputs

    /** The audio the analysis reads (one file). read() fills n samples of
        channel ch from start, zeros outside the file. */
    struct Audio
    {
        double sampleRate = 0.0;
        int numChannels = 0;
        int64_t numSamples = 0;
        std::function<void (int ch, int64_t start, int n, float* out)> read;
    };

    /** Value of a named parameter at document time t; *found = false when
        the name matches nothing. An empty name is never asked for. */
    using ParamFn = std::function<double (const juce::String& name, double t, bool* found)>;

    /** A stored spectral selection as the track analysis sees it: its time
        span, and the frequency band it covers at a time t (false outside). */
    struct SelectionShape
    {
        double t0 = 0.0, t1 = 0.0;
        std::function<bool (double t, double& fLo, double& fHi)> bandAt;
    };
    using SelectionFn = std::function<bool (const juce::String& name, SelectionShape& out)>;

    // -------------------------------------------------------------- analysis

    /** Analyses component `index` over its ranges, the components above it
        being taken out first. Fills its learned data and report; false with
        *error when it cannot run (no range, unknown parameter, no audio...). */
    bool analyse (Model& model, int index, const Audio& audio, const ParamFn& param,
                  const SelectionFn& selection, juce::String* error = nullptr);

    /** Analyses every component in list order; stops at the first error. */
    bool analyseAll (Model& model, const Audio& audio, const ParamFn& param,
                     const SelectionFn& selection, juce::String* error = nullptr);

    // ------------------------------------------------------------- rendering

    /** Renders the enabled, analysed components (or only component `only`
        when >= 0) for document times tStart + i / sampleRate, adding into
        out[0 .. numSamples). The noise is seeded, so a render repeats. */
    void render (const Model& model, double tStart, int64_t numSamples, double sampleRate,
                 const ParamFn& param, float* out, uint32_t seed = 1, int only = -1);

    /** Predicted RMS power (linear) of one line at parameter p, rising at
        `rate` parameter units per second (dynamics), after rangeGain. */
    double linePower (const Component& c, const HarmonicLine& line, double p, double rate,
                      const Model& model);

    /** Predicted noise PSD (linear, per Hz) of a noise component at frequency
        f for parameter p, after rangeGain. */
    double noisePsd (const Component& c, const Model& model, double f, double p);

    /** Learned span of a component's parameter (pHi - pLo, 0 when none). */
    inline double span (const Component& c) { return c.pHi - c.pLo; }
}
