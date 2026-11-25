// SpectroCraft Mobile - the waveform (the desktop WaveformView's colours and
// min/max drawing over the AudioEngine mipmaps), with an amplitude axis in
// dBFS or, when calibrated, in Pa, and the shared touch gestures.
#pragma once
#include <JuceHeader.h>
#include "MobileDocument.h"
#include "TimeViewport.h"

namespace scm
{
class WaveformPane : public juce::Component,
                     private juce::ChangeListener
{
public:
    WaveformPane (MobileDocument&, TimeViewport&);
    ~WaveformPane() override;

    void setShowRuler (bool show);
    void setChannel (int ch);
    void setWriteHead (double t, bool show);
    void contentChanged() { repaint(); }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr int kAxisW = 44;
    static constexpr int kRulerH = 22;

    /** Min / max per pixel column of one channel over [t0, t1). */
    static void columnRanges (const AudioEngine& eng, int ch, double t0, double t1, int width,
                              std::vector<float>& lo, std::vector<float>& hi);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    juce::Rectangle<float> plotArea() const;
    void drawAmplitudeAxis (juce::Graphics&, juce::Rectangle<float> plot);
    void drawRuler (juce::Graphics&, juce::Rectangle<float> plot);

    MobileDocument& document;
    TimeViewport& view;
    GestureTracker gestures;
    bool showRuler = true;
    int chan = 0;
    double writeHead = -1.0;
    float ampZoom = 1.0f, pinchAmp = 1.0f;
    double pinchT0 = 0, pinchT1 = 0;
    std::vector<float> lo, hi;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformPane)
};
} // namespace scm
