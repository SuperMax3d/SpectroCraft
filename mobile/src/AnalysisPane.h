// SpectroCraft Mobile - Frequency Analysis: FFT Linear, FFT Log and 1/3 octave
// of the selection (the visible range when nothing is selected, the last
// second while recording), computed by the desktop's sc::measures functions so
// the numbers match the desktop panel, plus the stored plots.
//
// Levels are dBFS, or dB-SPL when the file is calibrated in Pascal. 1/3 octave
// uses the desktop's reference (dB-SPL) pipeline for Pascal-calibrated files
// and its IEC pipeline otherwise, as the desktop panel does.
#pragma once
#include <JuceHeader.h>
#include "MobileDocument.h"
#include "TimeViewport.h"
#include "StoredPlots.h"
#include <atomic>

namespace scm
{
class AnalysisPane : public juce::Component,
                     private juce::ChangeListener,
                     private juce::Timer
{
public:
    AnalysisPane (MobileDocument&, TimeViewport&);
    ~AnalysisPane() override;

    void pauseAnalysis();
    void resumeAnalysis();
    /** Live mode while recording: analyse the newest second, a few times a second. */
    void setLive (bool live);
    void contentChanged();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    struct Curve
    {
        AnalysisMode mode = AnalysisMode::FftLog;
        std::vector<double> f, db;
        juce::String unit = "dBFS";
        double t0 = 0, t1 = 0;
        int fftSize = 0;
        int channel = 0;
        double rmsDb = -300, peakDb = -300;   // in `unit` (peak in dBFS when uncalibrated)
        int serial = 0;
    };

private:
    class Worker;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void request();
    void curveReady (Curve c);
    void storeCurrent();
    void showPlotMenu (int index);
    void rebuildChips();

    juce::Rectangle<float> plotArea() const;
    double xToFreq (double x) const;
    double freqToX (double f) const;
    double dbToY (double db) const;
    void drawCurve (juce::Graphics&, const std::vector<double>& f, const std::vector<double>& db,
                    AnalysisMode mode, juce::Colour c, float thickness);

    MobileDocument& document;
    TimeViewport& view;
    std::unique_ptr<Worker> worker;

    juce::ComboBox modeBox, fftBox;
    juce::TextButton storeButton { "Store" };
    juce::Label summary;
    juce::OwnedArray<juce::TextButton> chips;

    Curve current;
    int serial = 0;
    bool live = false;
    bool dirty = true;
    double fLo = 20.0, fHi = 20000.0;
    double dbTop = 0.0, dbRange = 100.0;
    bool cursorOn = false;
    juce::Point<float> cursor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalysisPane)
};
} // namespace scm
