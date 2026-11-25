// SpectroCraft Mobile - stored analysis plots (the desktop Frequency Analysis
// panel's 8 snapshot slots, kept with the file).
//
// Unlike the desktop snapshots, which are recomputed from the source audio
// and lost when the file closes, a stored plot here holds its curve. It is
// saved in the recording's metadata bundle under "storedPlots" (an array; see
// sc::MetaBundle::storedPlots), so it comes back when the file is reopened, on
// the phone or on the desktop (which carries the key through its saves).
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace scm
{
enum class AnalysisMode { FftLinear = 0, FftLog = 1, ThirdOctave = 2 };

juce::String analysisModeName (AnalysisMode);

struct StoredPlot
{
    juce::String name;               // "Plot 1", or typed by the user
    AnalysisMode mode = AnalysisMode::FftLog;
    double t0 = 0.0, t1 = 0.0;       // analysed region (s)
    int channel = 0;
    int fftSize = 0;                 // 0 for 1/3 octave (reference pipeline)
    juce::String unit;               // "dBFS" or "dB-SPL"
    std::vector<double> freqs;       // Hz (band centres in 1/3 octave)
    std::vector<double> db;          // level per point, in `unit`
    int colourIndex = 0;
    bool visible = true;

    juce::var toVar() const;
    static bool fromVar (const juce::var&, StoredPlot& out);
};

class StoredPlots
{
public:
    static constexpr int kMaxPlots = 8;
    /** Points kept per FFT curve: longer curves are reduced to this many
        log-spaced points, each holding the maximum of the bins it covers, so
        peaks survive (a 65536-point FFT would otherwise weigh ~1 MB of JSON). */
    static constexpr int kMaxPoints = 1024;

    const std::vector<StoredPlot>& plots() const noexcept { return list; }
    bool full() const noexcept { return (int) list.size() >= kMaxPlots; }
    bool empty() const noexcept { return list.empty(); }

    /** Adds a curve (reduced to kMaxPoints); false when all slots are used. */
    bool add (StoredPlot p);
    void remove (int index);
    void clear() { list.clear(); }
    void setVisible (int index, bool v);
    void rename (int index, const juce::String& name);

    juce::var toVar() const;
    void fromVar (const juce::var&);

    /** Max-hold reduction to at most `maxPoints` log-spaced points. */
    static void reduce (std::vector<double>& freqs, std::vector<double>& db, int maxPoints);

private:
    int nextColour() const;
    std::vector<StoredPlot> list;
};
} // namespace scm
