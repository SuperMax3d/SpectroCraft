#pragma once
// SpectroCraft - Dedicated tables track (Tables panel, "Dedicated track" mode):
// a strip above the waveform/spectrogram that plots every visible parameter
// on its own auto-scaled value axis. Vertical zoom/pan like the other tracks,
// red playhead, hover labels showing unscaled values near the curves, left-drag
// time selection (like the waveform) and middle-drag horizontal panning.
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

    /** Copies the visible parameters' curve data to the clipboard in an
        Excel-friendly tab/CR grid. With an active time selection only the
        samples inside it are copied; otherwise every sample is copied.
        Returns false when there was nothing to copy. */
    bool copyCurveData();

    // ------------------------------------------------------- Edit menu -----
    /** (tableIdx, paramIdx) pairs the Edit actions operate on: the rows
        selected in the Tables panel (a table row means all its parameters),
        or every visible parameter of every visible table when nothing is
        selected there. */
    std::vector<std::pair<int, int>> editTargetParams() const;

    /** Applies an Overwrite ramp over the current time selection to the
        target parameters and records one undo step. Returns false when
        there was nothing to do. */
    bool commitOverwrite (double startVal, double stopVal);

    /** Empties the target parameters' cells inside the current time
        selection (trimming fully-empty boundary rows) and records one undo
        step. Returns false when there was nothing to do. */
    bool commitDelete();

    /** "Edit > Overwrite Values...": prompts for start/stop values, then
        commitOverwrite(). The defaults are the first/last existing values of
        the first target parameter inside the selection. */
    void overwriteSelectionValues();

    /** "Edit > Delete Values": commitDelete() without a prompt. */
    void deleteSelectionValues();

    /** Shared time window (kept in sync with the waveform/spectral views). */
    void setTimeRange (double t0, double t1);
    void setPlayhead (double tSecs);
    double getPlayhead() const { return playheadT; }   // headless tests

    /** Recomputes the visible value range from all visible parameters' raw
        values (with 10% padding) and repaints. Called when tables change. */
    void refit();

    /** Sets the visible value window directly (e.g. from the vertical
        scrollbar). lo is the bottom value, hi the top one. */
    void setValueRange (double lo, double hi);

    /** Shared time selection: repaints only when it actually changed, so
        syncSharedState() can call this on every state change cheaply. */
    void setSelection (const juce::Range<double>& s);

    // Value<->pixel mapping of the plot area (public for headless tests).
    double valueAtY (int y) const;
    int yForValue (double v) const;
    std::pair<double, double> visibleRange() const { return { vMin, vMax }; }

    /** Full auto-fit range (what refit() computed): the scrollbar track. */
    std::pair<double, double> fullRange() const { return { fitMin, fitMax }; }

public:
    // Shared-state actions wired by MainWindow.
    std::function<void(double t0, double t1)> onTimeWindow;   // horizontal zoom / box-zoom time part
    std::function<void()> onZoomToSelection;
    std::function<void(bool axisOnly)> onZoomOutFull;
    /** Set by MainWindow: the (tableIdx, paramIdx) rows selected in the
        Tables panel; empty when nothing is selected there. */
    std::function<std::vector<std::pair<int, int>>()> selectedParams;
    // Notified whenever the visible value window changes (wheel, drag,
    // refit, scrollbar): MainWindow uses it to keep the vertical scrollbar's
    // thumb in sync.
    std::function<void(double lo, double hi)> onValueWindowChanged;

    /** Shared-window time under a pixel column of the plot area. */
    double timeAtX (int x) const;

private:
    juce::Rectangle<int> plotRect() const;
    juce::Rectangle<int> axisStripRect() const;
    void applyValueWindow (double lo, double hi);
    struct HoverLabel { juce::String text; float y; juce::Colour colour; };
    void collectHoverLabels (int mouseX, int mouseY);
    bool onAxisX (int x) const;
    void showTrackContextMenu (bool onAxis);

    Application& app;
    double t0 = 0.0, t1 = 1.0;      // shared time window
    double playheadT = 0.0;
    double vMin = -1.0, vMax = 1.0; // visible value window (max at top)
    double fitMin = -1.0, fitMax = 1.0;   // full auto-fit range
    juce::Range<double> selShown { 0.0, 0.0 };

    enum class DragMode { None, VPan, ZoomAxis, ZoomBox, Select, HPan };
    DragMode dragMode = DragMode::None;
    int dragStartX = 0, dragStartY = 0;
    double dragVMin = 0.0, dragVMax = 0.0;
    double dragT0 = 0.0, dragT1 = 1.0;   // time window at the start of an HPan
    bool haveRubberBand = false;
    juce::Rectangle<int> rubberRect;

    juce::Point<int> hoverPos {-1, -1};
    std::vector<HoverLabel> hoverLabels;

    static constexpr int kAxisW = kLeftAxisWidth;   // same vertical-axis width as the data tracks

    JUCE_DECLARE_NON_COPYABLE (TablesTrackView)
};
