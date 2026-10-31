#pragma once
// SpectroCraft - Dedicated tables track (Tables panel, "Dedicated track" mode):
// a strip above the waveform/spectrogram that plots every visible parameter
// on its own auto-scaled value axis. Vertical zoom/pan like the other tracks,
// red playhead, and hover labels showing unscaled values near the curves.
#include <JuceHeader.h>
#include "Application.h"

class TablesTrackView : public juce::Component
{
public:
    explicit TablesTrackView (Application& appIn);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** Shared time window (kept in sync with the waveform/spectral views). */
    void setTimeRange (double t0, double t1);
    void setPlayhead (double tSecs);

    /** Recomputes the visible value range from all visible parameters' raw
        values (with 10% padding) and repaints. Called when tables change. */
    void refit();

    // Value<->pixel mapping of the plot area (public for headless tests).
    double valueAtY (int y) const;
    int yForValue (double v) const;
    std::pair<double, double> visibleRange() const { return { vMin, vMax }; }

    /** Full auto-fit range (what refit() computed): the scrollbar track. */
    std::pair<double, double> fullRange() const { return { fitMin, fitMax }; }

private:
    juce::Rectangle<int> plotRect() const;
    struct HoverLabel { juce::String text; float y; juce::Colour colour; };
    void collectHoverLabels (int mouseX, int mouseY);

    Application& app;
    double t0 = 0.0, t1 = 1.0;      // shared time window
    double playheadT = 0.0;
    double vMin = -1.0, vMax = 1.0; // visible value window (max at top)
    double fitMin = -1.0, fitMax = 1.0;   // full auto-fit range

    bool dragging = false;
    int dragStartY = 0;
    double dragVMin = 0.0, dragVMax = 0.0;

    juce::Point<int> hoverPos {-1, -1};
    std::vector<HoverLabel> hoverLabels;

    static constexpr int kAxisW = kLeftAxisWidth;   // same vertical-axis width as the data tracks

    JUCE_DECLARE_NON_COPYABLE (TablesTrackView)
};
