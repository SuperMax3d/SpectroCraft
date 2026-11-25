// SpectroCraft Mobile - the spectrogram: the desktop's SpectralEngine picture
// (same FFT tiles, colour map and auto FFT size as SpectralView), with a
// frequency axis, a time ruler and touch zoom / pan / selection.
#pragma once
#include <JuceHeader.h>
#include "MobileDocument.h"
#include "TimeViewport.h"
#include <atomic>

namespace scm
{
class SpectrogramPane : public juce::Component,
                        private juce::ChangeListener
{
public:
    SpectrogramPane (MobileDocument&, TimeViewport&);
    ~SpectrogramPane() override;

    void setShowRuler (bool show);
    void setChannel (int ch);
    int channel() const noexcept { return chan; }

    /** Before the document is replaced: stop rendering and wait. */
    void pauseRendering();
    void resumeRendering();
    /** The audio changed (an edit, or new frames while recording). */
    void contentChanged();
    /** Loop recording: the write head, drawn as a marker. */
    void setWriteHead (double t, bool show);

    /** Frequency range shown (Hz); log or linear axis. */
    void setFrequencyRange (double lo, double hi);
    void setLogFrequency (bool log);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr int kAxisW = 44;
    static constexpr int kRulerH = 22;

private:
    class Renderer;
    struct Request
    {
        Settings settings;
        int w = 0, h = 0;
        double t0 = 0, t1 = 0;
        int channel = 0;
        int serial = 0;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    juce::Rectangle<float> plotArea() const;
    double nyquist() const;
    double yToFreq (double y) const;
    double freqToY (double f) const;
    void requestRender();
    void imageReady (juce::Image img, const Request& r);
    void drawFrequencyAxis (juce::Graphics&, juce::Rectangle<float> plot);
    void drawRuler (juce::Graphics&, juce::Rectangle<float> plot);
    void zoomFrequency (double anchorHz, double factor);

    MobileDocument& document;
    TimeViewport& view;
    std::unique_ptr<Renderer> renderer;
    GestureTracker gestures;

    juce::Image image;
    Request shown;            // what `image` was rendered for
    int serial = 0;
    bool showRuler = true;
    int chan = 0;
    double fLo = 20.0, fHi = 20000.0;
    bool logAxis = true;
    double writeHead = -1.0;

    // pinch start state
    double pinchT0 = 0, pinchT1 = 0, pinchFLo = 0, pinchFHi = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramPane)
};
} // namespace scm
