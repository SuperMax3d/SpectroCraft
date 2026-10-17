/**
 * SpectroCraft - Custom scrollbars (zoom/pan sliders)
 * The thumb shows the currently displayed portion of the track. Dragging the
 * thumb centre pans; dragging a thumb edge zooms in/out in that direction;
 * clicking outside the thumb jumps there and continues as a pan.
 */

#include "ScrollBars.h"
#include <algorithm>

// ---------------------------------------------------------------- TimeScrollBar

juce::Rectangle<int> TimeScrollBar::thumbRect() const
{
    const int w = std::max (1, getWidth());
    float x0 = (float) (winT0 / duration * w);
    float x1 = (float) (winT1 / duration * w);
    if (x1 - x0 < 8.0f) // keep the thumb grabbable when zoomed in far
    {
        const float c = 0.5f * (x0 + x1);
        x0 = std::max (0.0f, c - 4.0f);
        x1 = std::min ((float) w, c + 4.0f);
    }
    return { (int) x0, 2, (int) std::ceil (x1 - x0), getHeight() - 4 };
}

void TimeScrollBar::applyWindow (double t0, double t1)
{
    const double minSpan = 8.0 / std::max (1, getWidth()) * duration; // ~8 px
    if (t1 - t0 < minSpan || t0 < -1e-9 || t1 > duration + 1e-9) return;
    t0 = std::clamp (t0, 0.0, duration);
    t1 = std::clamp (t1, 0.0, duration);
    if (t0 == winT0 && t1 == winT1) return;
    winT0 = t0; winT1 = t1;
    repaint();
    if (onChange) onChange (t0, t1);
}

TimeScrollBar::Mode TimeScrollBar::hitTest (const juce::Rectangle<int>& thumb, int x, int y)
{
    if (thumb.getWidth() > 12 && std::abs ((float) x - (float) thumb.getX()) <= 5.0f)
        return Mode::LeftEdge;
    if (thumb.getWidth() > 12 && std::abs ((float) x - (float) thumb.getRight()) <= 5.0f)
        return Mode::RightEdge;
    if (thumb.contains (juce::Point<int> (x, y)))
        return Mode::Pan;
    return Mode::None; // outside the thumb -> jump
}

void TimeScrollBar::jumpTo (double t)
{
    const double span = winT1 - winT0;
    const double ns = std::clamp (t - 0.5 * span, 0.0, std::max (0.0, duration - span));
    applyWindow (ns, ns + span);
}

void TimeScrollBar::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds();
    g.fillAll (juce::Colour::fromRGB (24, 24, 26));
    if (duration <= 0.0) return;

    const auto tr = thumbRect();
    g.setColour (juce::Colour::fromRGB (72, 72, 80));
    g.fillRect (tr);
    g.setColour (juce::Colour::fromRGB (120, 120, 130));
    g.drawRect (tr, 1);
}

void TimeScrollBar::mouseDown (const juce::MouseEvent& e)
{
    const int w = std::max (1, getWidth());
    if (duration <= 0.0) return;
    dragStartX = e.x;
    dragWinT0 = winT0; dragWinT1 = winT1;

    const auto tr = thumbRect();
    const double tClick = (double) std::clamp (e.x, 0, w) / (double) w * duration;

    mode = hitTest (tr, (int) e.x, (int) e.y);
    if (mode == Mode::None)
    {
        // Jump: centre the window on the clicked position, then pan from there.
        jumpTo (tClick);
        dragWinT0 = winT0; dragWinT1 = winT1;
        mode = Mode::Pan;
    }
}

void TimeScrollBar::mouseDrag (const juce::MouseEvent& e)
{
    if (mode == Mode::None || duration <= 0.0) return;
    const int w = std::max (1, getWidth());
    const double dt = (double) (e.x - dragStartX) / (double) w * duration;

    switch (mode)
    {
        case Mode::Pan:
        {
            const double span = dragWinT1 - dragWinT0;
            const double ns = std::clamp (dragWinT0 + dt, 0.0, std::max (0.0, duration - span));
            applyWindow (ns, ns + span);
            break;
        }
        case Mode::LeftEdge: // drag the start edge -> zoom in/out on the left
        {
            const double minSpan = 8.0 / std::max (1, getWidth()) * duration;
            const double nt0 = std::clamp (dragWinT0 + dt, 0.0, dragWinT1 - minSpan);
            applyWindow (nt0, dragWinT1);
            break;
        }
        case Mode::RightEdge: // drag the end edge -> zoom in/out on the right
        {
            const double minSpan = 8.0 / std::max (1, getWidth()) * duration;
            const double nt1 = std::clamp (dragWinT1 + dt, dragWinT0 + minSpan, duration);
            applyWindow (dragWinT0, nt1);
            break;
        }
        default: break;
    }
}

void TimeScrollBar::mouseUp (const juce::MouseEvent&) { mode = Mode::None; }

// -------------------------------------------------------------- ValueScrollBar

juce::Rectangle<int> ValueScrollBar::thumbRect() const
{
    const int h = std::max (1, getHeight());
    const double range = rangeMax - rangeMin;
    float y0 = (float) ((rangeMax - winHi) / range * h); // maximum value at the top
    float y1 = (float) ((rangeMax - winLo) / range * h);
    if (y1 - y0 < 8.0f)
    {
        const float c = 0.5f * (y0 + y1);
        y0 = std::max (0.0f, c - 4.0f);
        y1 = std::min ((float) h, c + 4.0f);
    }
    return { 2, (int) y0, getWidth() - 4, (int) std::ceil (y1 - y0) };
}

void ValueScrollBar::applyWindow (double lo, double hi)
{
    const double minSpan = 8.0 / std::max (1, getHeight()) * (rangeMax - rangeMin); // ~8 px
    if (hi - lo < minSpan || lo < rangeMin - 1e-9 || hi > rangeMax + 1e-9) return;
    lo = std::clamp (lo, rangeMin, rangeMax);
    hi = std::clamp (hi, rangeMin, rangeMax);
    if (lo == winLo && hi == winHi) return;
    winLo = lo; winHi = hi;
    repaint();
    if (onChange) onChange (lo, hi);
}

ValueScrollBar::Mode ValueScrollBar::hitTest (const juce::Rectangle<int>& thumb, int x, int y)
{
    if (thumb.getHeight() > 12 && std::abs ((float) y - (float) thumb.getY()) <= 5.0f)
        return Mode::TopEdge; // top edge controls the high end of the window
    if (thumb.getHeight() > 12 && std::abs ((float) y - (float) thumb.getBottom()) <= 5.0f)
        return Mode::BottomEdge;
    if (thumb.contains (juce::Point<int> (x, y)))
        return Mode::Pan;
    return Mode::None; // outside the thumb -> jump
}

void ValueScrollBar::jumpToValue (double v)
{
    const double span = winHi - winLo;
    const double nlo = std::clamp (v - 0.5 * span, rangeMin, std::max (rangeMin, rangeMax - span));
    applyWindow (nlo, nlo + span);
}

void ValueScrollBar::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds();
    g.fillAll (juce::Colour::fromRGB (24, 24, 26));
    if (rangeMax <= rangeMin) return;

    const auto tr = thumbRect();
    g.setColour (juce::Colour::fromRGB (72, 72, 80));
    g.fillRect (tr);
    g.setColour (juce::Colour::fromRGB (120, 120, 130));
    g.drawRect (tr, 1);
}

void ValueScrollBar::mouseDown (const juce::MouseEvent& e)
{
    const int h = std::max (1, getHeight());
    if (rangeMax <= rangeMin) return;
    dragStartY = e.y;
    dragWinLo = winLo; dragWinHi = winHi;

    const auto tr = thumbRect();
    const double vClick = rangeMax - (double) std::clamp (e.y, 0, h) / (double) h * (rangeMax - rangeMin);

    mode = hitTest (tr, (int) e.x, (int) e.y);
    if (mode == Mode::None)
    {
        // Jump: centre the window on the clicked value, then pan from there.
        jumpToValue (vClick);
        dragWinLo = winLo; dragWinHi = winHi;
        mode = Mode::Pan;
    }
}

void ValueScrollBar::mouseDrag (const juce::MouseEvent& e)
{
    if (mode == Mode::None || rangeMax <= rangeMin) return;
    const int h = std::max (1, getHeight());
    // Dragging up increases the value.
    const double dv = -(double) (e.y - dragStartY) / (double) h * (rangeMax - rangeMin);

    switch (mode)
    {
        case Mode::Pan:
        {
            const double span = dragWinHi - dragWinLo;
            const double nlo = std::clamp (dragWinLo + dv, rangeMin, std::max (rangeMin, rangeMax - span));
            applyWindow (nlo, nlo + span);
            break;
        }
        case Mode::TopEdge: // drag the top edge -> zoom in/out at the high end
        {
            const double minSpan = 8.0 / std::max (1, getHeight()) * (rangeMax - rangeMin);
            const double nhi = std::clamp (dragWinHi + dv, dragWinLo + minSpan, rangeMax);
            applyWindow (dragWinLo, nhi);
            break;
        }
        case Mode::BottomEdge: // drag the bottom edge -> zoom in/out at the low end
        {
            const double minSpan = 8.0 / std::max (1, getHeight()) * (rangeMax - rangeMin);
            const double nlo = std::clamp (dragWinLo + dv, rangeMin, dragWinHi - minSpan);
            applyWindow (nlo, dragWinHi);
            break;
        }
        default: break;
    }
}

void ValueScrollBar::mouseUp (const juce::MouseEvent&) { mode = Mode::None; }
