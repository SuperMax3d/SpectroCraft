#include "TimeViewport.h"

namespace scm
{
void TimeViewport::setDuration (double d, bool followEnd)
{
    const double old = dur;
    dur = std::max (0.0, d);
    if (followEnd && dur > t1)
    {
        const double s = span();
        t1 = dur;
        t0 = std::max (0.0, t1 - s);
    }
    else if (old <= 0.0 && dur > 0.0 && ! followEnd)
    {
        t0 = 0.0;
        t1 = dur;
    }
    else if (t1 > limit())
    {
        const double s = std::min (span(), limit());
        t1 = limit();
        t0 = std::max (0.0, t1 - s);
    }
    sendChangeMessage();
}

void TimeViewport::setRange (double a, double b)
{
    if (b < a) std::swap (a, b);
    double s = juce::jlimit (kMinSpan, limit(), b - a);
    a = juce::jlimit (0.0, limit() - s, a);
    if (a == t0 && a + s == t1) return;
    t0 = a;
    t1 = a + s;
    sendChangeMessage();
}

void TimeViewport::zoom (double anchor, double factor)
{
    if (factor <= 0.0) return;
    const double s = juce::jlimit (kMinSpan, limit(), span() / factor);
    const double rel = span() > 0.0 ? (anchor - t0) / span() : 0.5;
    setRange (anchor - rel * s, anchor - rel * s + s);
}

void TimeViewport::pan (double dt)
{
    setRange (t0 + dt, t1 + dt);
}

void TimeViewport::showAll()
{
    t0 = 0.0;
    t1 = limit();
    sendChangeMessage();
}

// ---------------------------------------------------------------- gestures

void GestureTracker::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.position;
    fingers[e.source.getIndex()] = { p, p };
    if (fingers.size() == 2)
    {
        // A second finger turns the gesture into a pinch: the selection drag
        // the first finger may have started is abandoned.
        dragActive = false;
        cancelledSingle = true;
        pinchActive = true;
        auto it = fingers.begin();
        const auto a = it->second.now, b = std::next (it)->second.now;
        pinch.startMid = pinch.mid = (a + b) * 0.5f;
        pinch.startDx = pinch.dx = std::abs (a.x - b.x);
        pinch.startDy = pinch.dy = std::abs (a.y - b.y);
        if (onPinchStart) onPinchStart();
    }
    else if (fingers.size() == 1)
    {
        cancelledSingle = false;
        dragActive = false;
    }
}

void GestureTracker::updatePinch()
{
    if (fingers.size() < 2) return;
    auto it = fingers.begin();
    const auto a = it->second.now, b = std::next (it)->second.now;
    pinch.mid = (a + b) * 0.5f;
    pinch.dx = std::abs (a.x - b.x);
    pinch.dy = std::abs (a.y - b.y);
    if (onPinch) onPinch (pinch);
}

void GestureTracker::mouseDrag (const juce::MouseEvent& e)
{
    auto it = fingers.find (e.source.getIndex());
    if (it == fingers.end()) return;
    it->second.now = e.position;
    if (pinchActive)
    {
        updatePinch();
        return;
    }
    if (cancelledSingle || fingers.size() != 1) return;
    if (! dragActive && it->second.down.getDistanceFrom (e.position) < 8.0f) return;
    dragActive = true;
    if (onDrag) onDrag (it->second.down, e.position);
}

void GestureTracker::mouseUp (const juce::MouseEvent& e)
{
    auto it = fingers.find (e.source.getIndex());
    if (it == fingers.end()) return;
    const auto finger = it->second;
    fingers.erase (it);

    if (pinchActive)
    {
        if (fingers.size() < 2)
        {
            pinchActive = false;
            if (onPinchEnd) onPinchEnd();
        }
        return;
    }
    if (cancelledSingle) return;
    if (dragActive)
    {
        dragActive = false;
        if (onDragEnd) onDragEnd (finger.down, e.position);
        return;
    }
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastTapMs < 350 && lastTapAt.getDistanceFrom (e.position) < 30.0f)
    {
        lastTapMs = 0;
        if (onDoubleTap) onDoubleTap (e.position);
        return;
    }
    lastTapMs = now;
    lastTapAt = e.position;
    if (onTap) onTap (e.position);
}
} // namespace scm
