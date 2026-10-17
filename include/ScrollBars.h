#pragma once
// SpectroCraft - Custom scrollbars (zoom/pan sliders)
#include <JuceHeader.h>
#include <functional>

/** Horizontal time scrollbar: the track spans the whole file, the thumb shows
    the current view range. Dragging the thumb centre pans both views;
    dragging a thumb edge changes the window length (zoom in/out); clicking
    outside the thumb jumps there. */
class TimeScrollBar : public juce::Component,
                       public juce::SettableTooltipClient
{
public:
    // Explicit default ctor: the deleted copy ctor below suppresses the
    // implicit one (a user-declared constructor of any kind does).
    TimeScrollBar() = default;

    std::function<void(double t0, double t1)> onChange;

    void setDuration (double d) { duration = std::max (1e-9, d); repaint(); }
    void setWindow (double t0, double t1)
    {
        if (t0 != winT0 || t1 != winT1) { winT0 = t0; winT1 = t1; repaint(); }
    }

    // State accessors (public so the logic can be tested headlessly).
    enum class Mode { None, Pan, LeftEdge, RightEdge };
    juce::Rectangle<int> thumbRect() const;
    void applyWindow (double t0, double t1);   // clamp + notify
    static Mode hitTest (const juce::Rectangle<int>& thumb, int x, int y);
    void jumpTo (double t);                    // centre the window on time t

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    double duration = 1.0, winT0 = 0.0, winT1 = 1.0;
    Mode mode = Mode::None;
    int dragStartX = 0;
    double dragWinT0 = 0.0, dragWinT1 = 0.0;

    JUCE_DECLARE_NON_COPYABLE (TimeScrollBar)
};

/** Vertical value scrollbar: the track spans [minV, maxV] with the maximum at
    the top. The thumb shows the visible subrange. Dragging the centre pans,
    dragging an edge zooms in that direction, clicking outside jumps there. */
class ValueScrollBar : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    // Explicit default ctor (see TimeScrollBar for why).
    ValueScrollBar() = default;

    std::function<void(double lo, double hi)> onChange;

    void setRange (double minV, double maxV)
    {
        rangeMin = minV;
        rangeMax = std::max (minV + 1e-9, maxV);
        repaint();
    }
    void setWindow (double lo, double hi)
    {
        if (lo != winLo || hi != winHi) { winLo = lo; winHi = hi; repaint(); }
    }

    // State accessors (public so the logic can be tested headlessly).
    enum class Mode { None, Pan, TopEdge, BottomEdge };
    juce::Rectangle<int> thumbRect() const;
    void applyWindow (double lo, double hi);   // clamp + notify
    static Mode hitTest (const juce::Rectangle<int>& thumb, int x, int y);
    void jumpToValue (double v);               // centre the window on value v

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    double rangeMin = 0.0, rangeMax = 1.0, winLo = 0.0, winHi = 1.0;
    Mode mode = Mode::None;
    int dragStartY = 0;
    double dragWinLo = 0.0, dragWinHi = 1.0;

    JUCE_DECLARE_NON_COPYABLE (ValueScrollBar)
};
