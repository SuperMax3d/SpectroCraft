// SpectroCraft Mobile - a draggable separator between the tablet's panels and
// views, drawn like the desktop's (include/SplitterStyle.h). A finger drags
// it; a double tap puts it back where it started.
#pragma once
#include <JuceHeader.h>
#include "SplitterStyle.h"

namespace scm
{
class Splitter : public juce::Component
{
public:
    Splitter()
    {
        setTitle ("Resize the panels");
        setVertical (true);
    }

    /** true: a bar that splits left / right (dragged sideways). */
    void setVertical (bool v)
    {
        vertical = v;
        setMouseCursor (v ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
        repaint();
    }

    /** The bar's new centre (x when vertical, else y) in the parent's coordinates. */
    std::function<void (int)> onDrag;
    std::function<void()> onDragEnd, onDoubleTap;

    void paint (juce::Graphics& g) override
    {
        sc::ui::drawSplitterGrip (g, getLocalBounds().toFloat(), vertical);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // keep the finger where it took the bar
        grabOffset = vertical ? e.x - getWidth() / 2 : e.y - getHeight() / 2;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (onDrag == nullptr || getParentComponent() == nullptr) return;
        const auto p = e.getEventRelativeTo (getParentComponent()).getPosition();
        onDrag ((vertical ? p.x : p.y) - grabOffset);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (onDragEnd) onDragEnd();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (onDoubleTap) onDoubleTap();
    }

private:
    bool vertical = true;
    int grabOffset = 0;
};
} // namespace scm
