// SpectroCraft Mobile - the visible time range shared by the waveform and the
// spectrogram, and the touch gestures both views use.
//
// Gestures (touch; the desktop build maps the mouse to the same actions):
//   - one finger drag: time selection (as the desktop's drag-select)
//   - one finger tap: playhead, clears the selection; double tap: show all
//   - two fingers: pinch to zoom (horizontal spread = time, vertical spread =
//     frequency on the spectrogram), move together to pan
//   - mouse wheel: time zoom; shift + wheel: frequency zoom
#pragma once
#include <JuceHeader.h>
#include <map>

namespace scm
{
class TimeViewport : public juce::ChangeBroadcaster
{
public:
    double start() const noexcept { return t0; }
    double end() const noexcept { return t1; }
    double span() const noexcept { return t1 - t0; }
    double duration() const noexcept { return dur; }

    /** The document length changed. `followEnd` keeps the newest audio in view
        (recording); otherwise a view past the new end is pulled back. */
    void setDuration (double d, bool followEnd);
    void setRange (double a, double b);
    /** factor > 1 zooms in around `anchor` (seconds). */
    void zoom (double anchor, double factor);
    void pan (double dt);
    void showAll();
    /** While recording: the view may extend past the audio up to this span. */
    void setMinimumVisible (double secs) { minVisible = std::max (0.0, secs); }

    double timeToX (double t, juce::Rectangle<float> plot) const noexcept
    {
        return plot.getX() + (t - t0) / std::max (1e-12, span()) * plot.getWidth();
    }
    double xToTime (double x, juce::Rectangle<float> plot) const noexcept
    {
        return t0 + (x - plot.getX()) / std::max (1.0f, plot.getWidth()) * span();
    }

    static constexpr double kMinSpan = 0.002;     // 2 ms across the screen
    static constexpr double kEmptySpan = 10.0;    // shown before any audio

private:
    double limit() const noexcept { return dur > 0.0 ? std::max (dur, minVisible) : std::max (kEmptySpan, minVisible); }
    double t0 = 0.0, t1 = kEmptySpan, dur = 0.0, minVisible = 0.0;
};

/** Tracks the fingers on a view and turns them into selection / pinch / pan
    callbacks. Feed it the view's mouse events. */
class GestureTracker
{
public:
    struct Pinch
    {
        juce::Point<float> startMid, mid;   // midpoints at the start and now
        float startDx = 0, startDy = 0;     // finger spreads at the start
        float dx = 0, dy = 0;               // and now
    };

    std::function<void (juce::Point<float> from, juce::Point<float> to)> onDrag;   // one finger
    std::function<void (juce::Point<float> from, juce::Point<float> to)> onDragEnd;
    std::function<void (juce::Point<float> at)> onTap;
    std::function<void (juce::Point<float> at)> onDoubleTap;
    std::function<void()> onPinchStart;
    std::function<void (const Pinch&)> onPinch;     // relative to the state at onPinchStart
    std::function<void()> onPinchEnd;

    void mouseDown (const juce::MouseEvent&);
    void mouseDrag (const juce::MouseEvent&);
    void mouseUp (const juce::MouseEvent&);
    bool pinching() const noexcept { return pinchActive; }

private:
    struct Finger { juce::Point<float> down, now; };
    std::map<int, Finger> fingers;
    bool pinchActive = false;
    bool dragActive = false;
    bool cancelledSingle = false;
    Pinch pinch;
    juce::uint32 lastTapMs = 0;
    juce::Point<float> lastTapAt;
    void updatePinch();
};
} // namespace scm
