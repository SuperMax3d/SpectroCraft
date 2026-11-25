// SpectroCraft Mobile - precision touch on the spectrogram, without the view:
// the reticle that moves 4x slower than the finger, the pause that lets a
// lifted finger carry on, and where the magnifier goes on the screen.
//
// A finger held still shows the magnifier (Point). Lifting it places the
// playhead and keeps the reticle for kResumeMs (Waiting): a finger put down
// again in that time carries on as if it had not been lifted, the reticle
// moving from where it was and the playhead being the selection's fixed end
// (Edge). So a precise selection can run further than one finger stroke, as
// on a trackpad. A hold at the end of a selection drag starts in Edge.
#pragma once
#include <JuceHeader.h>
#include <algorithm>

namespace scm::precise
{
inline constexpr int kHoldMs = 450;          // a finger held still this long
inline constexpr int kResumeMs = 1500;       // a lifted finger may carry on this long
inline constexpr float kGain = 0.25f;        // the reticle's speed against the finger's

// the magnifier and its readout (points)
inline constexpr float kLensSize = 120.0f;
inline constexpr float kLensGap = 45.0f;     // from the lens's bottom to the finger's touch point
inline constexpr float kLensMinGap = 20.0f;  // the least of it, when the room above is short
inline constexpr float kLensSide = 50.0f;    // from the finger to the lens, when it goes beside
inline constexpr float kReadoutH = 24.0f;
inline constexpr float kReadoutGap = 4.0f;

enum class Mode { None, Point, Edge, Waiting };

class Tracker
{
public:
    Mode mode() const noexcept { return state; }
    bool active() const noexcept { return state != Mode::None; }
    bool fingerDown() const noexcept { return state == Mode::Point || state == Mode::Edge; }
    juce::Point<float> reticle() const noexcept { return cross; }

    /** A held finger: the reticle starts under it. */
    void begin (Mode m, juce::Point<float> finger)
    {
        jassert (m == Mode::Point || m == Mode::Edge);
        state = m;
        fingerFrom = crossFrom = cross = finger;
    }

    /** A finger put down: true when it carries on a lifted one (now Edge). */
    bool press (juce::Point<float> finger, juce::uint32 nowMs)
    {
        if (state != Mode::Waiting) return false;
        if (expire (nowMs)) return false;
        state = Mode::Edge;
        fingerFrom = finger;
        crossFrom = cross;
        return true;
    }

    /** The finger moved: the reticle follows at kGain, inside the plot. */
    void move (juce::Point<float> finger, juce::Rectangle<float> plot)
    {
        if (! fingerDown()) return;
        const auto p = crossFrom + (finger - fingerFrom) * kGain;
        cross = { std::clamp (p.x, plot.getX(), std::max (plot.getX(), plot.getRight())),
                  std::clamp (p.y, plot.getY(), std::max (plot.getY(), plot.getBottom())) };
    }

    /** The finger was lifted: the mode it ends (Point or Edge); then Waiting. */
    Mode release (juce::uint32 nowMs)
    {
        const auto ended = state;
        if (fingerDown())
        {
            state = Mode::Waiting;
            waitUntil = nowMs + (juce::uint32) kResumeMs;
        }
        return ended;
    }

    /** True when the pause after a lift is over (back to None). */
    bool expire (juce::uint32 nowMs)
    {
        if (state != Mode::Waiting || (juce::int32) (nowMs - waitUntil) < 0) return false;
        state = Mode::None;
        return true;
    }

    /** A second finger, or the view went away: no precision touch any more. */
    void cancel() noexcept { state = Mode::None; }

private:
    Mode state = Mode::None;
    juce::Point<float> cross, fingerFrom, crossFrom;
    juce::uint32 waitUntil = 0;
};

struct Placement
{
    juce::Rectangle<float> lens, readout;
    bool above = true;   // false: beside the finger, for want of room above
};

/** The magnifier for a finger at `finger`, inside `bounds` (the screen's safe
    area, in the same coordinates), with a readout `readoutWidth` wide on top of
    the lens. Above the finger, over whatever is drawn there, so the finger never
    hides it; beside the finger only when the screen's top is too close. */
inline Placement place (juce::Point<float> finger, juce::Rectangle<float> bounds, float readoutWidth)
{
    const auto clampTo = [] (float v, float lo, float hi) { return std::max (lo, std::min (hi, v)); };
    const float d = kLensSize, readoutBlock = kReadoutH + kReadoutGap;
    Placement p;
    const float room = finger.y - bounds.getY();
    float x, y;
    if (room >= readoutBlock + d + kLensMinGap)
    {
        y = finger.y - std::min (kLensGap, room - readoutBlock - d) - d;
        x = finger.x - d * 0.5f;
    }
    else
    {
        p.above = false;
        const bool right = bounds.getRight() - finger.x >= finger.x - bounds.getX();
        x = right ? finger.x + kLensSide : finger.x - kLensSide - d;
        y = clampTo (finger.y - d * 0.5f, bounds.getY() + readoutBlock, bounds.getBottom() - d);
    }
    p.lens = { clampTo (x, bounds.getX(), bounds.getRight() - d), y, d, d };

    const float w = std::min (readoutWidth, bounds.getWidth());
    p.readout = { clampTo (p.lens.getCentreX() - w * 0.5f, bounds.getX(), bounds.getRight() - w),
                  p.lens.getY() - readoutBlock, w, kReadoutH };
    return p;
}
} // namespace scm::precise
