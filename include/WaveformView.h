#pragma once
// SpectroCraft - Waveform view component (own time window + vertical zoom,
// left amplitude axis, top time ruler)
#include <JuceHeader.h>
#include "Application.h"
#include "MarkerManager.h"
#include <vector>
#include <cstdint>
#include <functional>

class WaveformView : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    explicit WaveformView (Application& app);
    ~WaveformView();

    void paint (juce::Graphics&) override;
    void resized() override;

    // Mouse interaction: left-drag = select, middle/right-drag = pan,
    // wheel over the plot or time ruler = horizontal zoom at cursor,
    // wheel over the left amplitude axis (or Shift+wheel) = vertical zoom,
    // double-click = fit whole file. Clicking moves the playhead.
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;   // status-bar probe
    void mouseExit (const juce::MouseEvent&) override;     // clears the probe
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

    // Reset the view to a time range (file open / fit).
    void setViewRange (double t0, double t1) { setWindow (t0, t1); }
    void setVerticalZoom (double z)
    {
        if (z != vZoom) { vZoom = z; dirty = true; repaint(); }
    }
    void setSelection (juce::Range<double> s)
    {
        if (s != selection) { selection = s; dirty = true; repaint(); }
    }
    void setMarkers (const std::vector<Marker>* m)
    {
        if (m != markerList) { markerList = m; repaint(); }
    }
    void setPlayhead (double t)
    {
        if (t != playhead) { playhead = t; dirty = true; repaint(); }
    }
    double getPlayhead() const { return playhead; }
    void setVisibleChannels (const std::vector<int>& chs);

    // Keyboard-shortcut helpers (applied by MainWindow to both views).
    void zoomHorizontalAtCenter (double factor);   // <1 zoom in, >1 zoom out
    void zoomVerticalBy (double factor);           // <1 zoom in, >1 zoom out
    void fitAll();

    // Visible half-amplitude at a row edge (for the vertical scrollbar):
    // 1.0 means full scale fits exactly in the row height.
    double getVisibleAmpHalf() const { return 1.0 / (0.9 * vZoom); }

    // Notify MainWindow that the time window changed: (newStart, newEnd).
    // MainWindow applies it to BOTH views so they always show the same range.
    std::function<void(double t0, double t1)> onChange;
    // Notified when the local vertical zoom changes (wheel / keys / fit).
    std::function<void()> onVerticalZoomChanged;
    // Right-click context menu action selected (ContextMenuIDs); handled by MainWindow.
    std::function<void(int)> onContextAction;
    // Status-bar probe: cursor time + amplitude under the mouse (empty clears).
    std::function<void(const juce::String&)> onCursorInfo;

    double getViewStart() const { return viewStart; }
    double getViewEnd() const { return viewEnd; }
    double xToTime (int x) const;
    double timeToX (double t) const;

    static constexpr int kRulerH = 24;  // top time ruler height

private:
    Application& app;   // audio data read live via app.audioEngine() (active doc)
    double viewStart = 0, viewEnd = 10, vZoom = 1;
    juce::Range<double> selection{ 0, 0 };
    double playhead = 0;
    const std::vector<Marker>* markerList = nullptr;   // same overlay as the spectral view
    std::vector<uint8_t> chanVis;
    bool dirty = true;

    // Interaction state
    enum class DragMode { None, Select, Pan };
    DragMode dragMode = DragMode::None;
    int dragStartX = 0;
    double dragStartTime = 0.0;

    juce::Rectangle<int> plotRect() const
    {
        return { kLeftAxisWidth, kRulerH,
                 std::max (1, getWidth() - kLeftAxisWidth),
                 std::max (1, getHeight() - kRulerH) };
    }

    // Single path for all time-window changes: updates state and notifies
    // MainWindow with the new range.
    void setWindow (double t0, double t1)
    {
        if (t0 != viewStart || t1 != viewEnd)
        {
            viewStart = t0;
            viewEnd = t1;
            dirty = true;
            repaint();
            if (onChange) onChange (viewStart, viewEnd);
        }
    }

    void paintChannel (juce::Graphics& g, int ch, int chanY, int chanH, float center, float amp);
    void drawRuler (juce::Graphics& g);
    void drawAmplitudeAxis (juce::Graphics& g, int visCount, int chanH, bool showChanLabels);
    void zoomAtX (int x, double factor);
    void panByTime (double dt);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};
