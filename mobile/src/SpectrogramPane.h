// SpectroCraft Mobile - the spectrogram: the desktop's SpectralEngine picture
// (same FFT tiles, colour map and auto FFT size as SpectralView), with a
// frequency axis, a time ruler and touch zoom / pan / selection.
#pragma once
#include <JuceHeader.h>
#include "MobileDocument.h"
#include "TimeViewport.h"
#include <atomic>
#include <set>

namespace scm
{
class SpectrogramPane : public juce::Component,
                        private juce::ChangeListener,
                        private juce::Timer
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
    /** Development hook: the precision reticle at (fx, fy), fractions of the plot. */
    void showPreciseAt (float fx, float fy)
    {
        const auto p = plotArea();
        fingerNow = { p.getX() + fx * p.getWidth(), p.getY() + fy * p.getHeight() };
        enterPrecise();
    }

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
    void timerCallback() override;

    // Precision touch: a finger held still (~0.45 s) shows a magnifier and a
    // readout, and moves a reticle 4x slower than itself. Held at the start of
    // a touch it places the playhead on release (Point); held while dragging a
    // selection it moves the selection's edge (Edge).
    enum class Precise { None, Point, Edge };
    void enterPrecise();
    void drawPrecise (juce::Graphics&, juce::Rectangle<float> plot);
    Precise precise = Precise::None;
    std::set<int> touches;             // fingers down on the plot
    juce::Point<float> fingerNow, fingerAnchor, crossAnchor, cross, stillAt;
    juce::uint32 stillSinceMs = 0;
    bool dragging = false;             // a one-finger selection drag is under way
    double dragFromTime = 0.0;         // its fixed end
    static constexpr int kHoldMs = 450;
    static constexpr float kPreciseGain = 0.25f;
    juce::Rectangle<float> plotArea() const;
    double nyquist() const;
    double yToFreq (double y) const;
    double freqToY (double f) const;
    void requestRender();
    void imageReady (juce::Image img, const Request& r);
    void drawFrequencyAxis (juce::Graphics&, juce::Rectangle<float> plot);
    void drawRuler (juce::Graphics&, juce::Rectangle<float> plot);
    void zoomFrequency (double anchorHz, double factor);
    /** Tap on the frequency scale: minimum / maximum frequency, full range. */
    void showAxisMenu();
    /** Asks for the minimum (or maximum) frequency in Hz. */
    void askFrequency (bool minimum);

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
    bool axisPress = false;   // the current touch started on the scale
    int axisTapSerial = 0;    // a second tap cancels the first one's menu

    // pinch start state
    double pinchT0 = 0, pinchT1 = 0, pinchFLo = 0, pinchFHi = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrogramPane)
};
} // namespace scm
