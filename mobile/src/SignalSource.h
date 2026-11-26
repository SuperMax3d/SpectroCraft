// SpectroCraft Mobile - streaming signal generator (tone, sweep, noise).
//
// Runs on the audio thread, block by block, so it can play while the app
// records (issue #73). The desktop's sc::generateNoise / generateTone fill a
// whole buffer offline (peak-normalised), which cannot stream.
//
// Level convention: `levelDb` is the RMS level in dBFS of the generated
// signal (a sine at -3.01 dBFS RMS peaks at 0 dBFS), so a noise and a tone at
// the same setting carry the same power. Output is hard-limited to +-1.
//
// Noise is shaped in the frequency domain: each block of kFftSize samples
// gets a random phase and the magnitude |H(f)| = (f / 1 kHz)^(slope / 6.02)
// inside [lowHz, highHz] (0 outside), and blocks are overlap-added at 50 %
// with a sqrt-Hann window, whose squares sum to one, so the variance is
// constant over time. White = 0 dB/octave, pink = -3, brown = -6; Custom
// follows a response drawn by hand (customDb, issue #79), or a slope.
//
// Level and tone frequency changes are smoothed (issue #79): the level follows
// its target within ~30 ms, the tone's frequency glides within ~50 ms (in log
// frequency), so moving a slider makes no click.
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace scm
{
struct GeneratorSettings
{
    enum class Kind { Tone, Sweep, Noise };
    enum class Noise { White, Pink, Brown, Custom };

    Kind kind = Kind::Tone;
    double levelDb = -12.0;          // RMS dBFS

    double toneHz = 1000.0;

    double sweepStartHz = 20.0;
    double sweepEndHz = 20000.0;
    double sweepSeconds = 10.0;
    bool sweepLog = true;            // exponential (equal time per octave)
    bool sweepRepeat = true;         // restart at the start frequency, else stop

    Noise noise = Noise::Pink;
    double slopeDbPerOctave = -3.0;  // Custom without a drawn response
    /** Custom: the drawn response, dB at the kCustomBands third-octave
        centres (customBandHz); empty: the slope instead. */
    std::vector<float> customDb;
    double lowHz = 20.0;             // band edges (all noise types)
    double highHz = 20000.0;

    /** Slope used for the noise type (White 0, Pink -3, Brown -6, Custom). */
    double effectiveSlope() const noexcept;

    static constexpr int kCustomBands = 31;
    /** Centre of custom band i: third octaves from 20 Hz (i = 0) to 20 kHz (i = 30). */
    static double customBandHz (int i) noexcept { return 1000.0 * std::pow (2.0, (double) (i - 17) / 3.0); }
    /** Custom noise: the gain (dB) at f, linear in log frequency between the
        drawn bands and flat beyond them; the slope when nothing is drawn. */
    double customGainDb (double f) const noexcept;
    /** The drawn response a slope stands for (to start drawing from it). */
    static std::vector<float> curveForSlope (double slopeDbPerOctave);

    juce::var toVar() const;
    static GeneratorSettings fromVar (const juce::var&);
    juce::String describe() const;
};

class SignalSource
{
public:
    SignalSource();
    ~SignalSource();

    /** Message thread: new settings, picked up at the next block. A change of
        kind or noise shape restarts the signal; a level or frequency change
        does not (no phase jump on the tone). */
    void setSettings (const GeneratorSettings&);
    GeneratorSettings settings() const;

    void setEnabled (bool on) noexcept { enabled.store (on); }
    bool isEnabled() const noexcept { return enabled.load(); }

    /** Audio thread. */
    void prepare (double sampleRate);
    /** Audio thread: ADDS the signal to every output channel. */
    void addTo (float* const* outputs, int numOutputs, int numSamples) noexcept;

    /** Renders into a mono buffer (tests, offline use). */
    void render (float* out, int numSamples) noexcept;

    /** Sweep progress 0..1 (0 when not sweeping); true once a one-shot sweep ended. */
    double sweepProgress() const noexcept { return sweepPos.load(); }
    bool sweepFinished() const noexcept { return sweepDone.load(); }

    static constexpr int kFftOrder = 13;
    static constexpr int kFftSize = 1 << kFftOrder;   // 8192: 5.9 Hz bins at 48 kHz

private:
    void applyPending() noexcept;
    void renderBlock (float* out, int n) noexcept;
    void renderTone (float* out, int n) noexcept;
    void renderSweep (float* out, int n) noexcept;
    void renderNoise (float* out, int n) noexcept;
    void buildNoiseShape();
    void synthesiseNoiseBlock() noexcept;

    juce::SpinLock pendingLock;
    GeneratorSettings pending;
    std::atomic<bool> pendingDirty { true };
    std::atomic<bool> enabled { false };

    GeneratorSettings cur;
    double rate = 48000.0;
    double phase = 0.0;
    double sweepT = 0.0;
    std::atomic<double> sweepPos { 0.0 };
    std::atomic<bool> sweepDone { false };
    double gainLin = 0.0;
    // smoothing (issue #79): what is played now, moving to gainLin / the tone's frequency
    double gainNow = -1.0, logFreqNow = 0.0;
    bool freqStarted = false;
    static constexpr double kGainTau = 0.03, kFreqTau = 0.05;
    double smoothCoef (double tau) const noexcept { return 1.0 - std::exp (-1.0 / (tau * rate)); }
    /** One sample of the level, moved toward gainLin. */
    double nextGain (double coef) noexcept
    {
        if (gainNow < 0.0) gainNow = gainLin;
        gainNow += (gainLin - gainNow) * coef;
        return gainNow;
    }

    // noise
    juce::dsp::FFT fft { kFftOrder };
    std::vector<float> shape;      // |H| per bin, 0..N/2, normalised to unit RMS
    std::vector<float> window;     // sqrt-Hann, periodic
    std::vector<float> work;       // 2N for the FFT
    std::vector<float> ola;        // overlap tail (N/2)
    std::vector<float> ready;      // the next N/2 output samples
    int readyPos = 0;
    juce::Random random { 0x5c0de };
    std::vector<float> scratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SignalSource)
};
} // namespace scm
