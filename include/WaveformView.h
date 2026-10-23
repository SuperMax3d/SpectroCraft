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

    // Mouse interaction: left-drag = select, middle-drag = pan,
    // right-drag = box-zoom (drag on the track -> BOTH axes: time from the
    // box's x span + amplitude from its y span; drag starting on the left
    // amplitude axis -> vertical only; drag starting on the top time ruler
    // -> horizontal only), plain right-click = context menu,
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

    // Set the visible amplitude window directly: [lo, hi] in amplitude units
    // (the vertical scrollbar drives this). The centre becomes the offset and
    // the span sets the zoom.
    void setVerticalRange (double lo, double hi)
    {
        vOffset = std::clamp ((lo + hi) * 0.5, -2.0, 2.0);
        const double span = std::max (1e-6, hi - lo);
        setVerticalZoom (std::clamp (2.0 / (0.9 * span), 0.05, 64.0));
    }

    // Reset the vertical axis to full scale centred on zero.
    void resetVerticalZoom();

    // Pixels per unit amplitude for one row at the current zoom (tests).
    double rowAmpPx() const
    {
        const auto pr = plotRect();
        int visCount = 0;
        for (int ch = 0; ch < app.audioEngine().numChannels(); ++ch)
            if (ch < (int) chanVis.size() && chanVis[ch]) ++visCount;
        return std::max (1e-6, (double) pr.getHeight() / std::max (1, visCount) * 0.45
                         * (double) vZoom);
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
    // Current vertical offset: the amplitude value sitting at each row's centre.
    double getVerticalOffset() const { return vOffset; }

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
    double vOffset = 0;   // amplitude value at each row's centre (vertical pan)
    juce::Range<double> selection{ 0, 0 };
    double playhead = 0;
    const std::vector<Marker>* markerList = nullptr;   // same overlay as the spectral view
    std::vector<uint8_t> chanVis;
    bool dirty = true;

    // Interaction state
    enum class DragMode { None, Select, Pan, VPan, TimePan, ZoomDrag };
    DragMode dragMode = DragMode::None;
    int dragStartX = 0;
    int dragStartY = 0;
    double dragStartTime = 0.0;
    int panLastX = 0;   // last delivered cursor x during time panning
    juce::Rectangle<int> zoomRect;   // right-drag box-zoom rubber band
    bool haveZoomRect = false;

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

// ---------------------------------------------------------------- time axis

/** Chooses a "nice" major tick step (seconds) for a time ruler drawn at
    `pxPerSec` pixels per second, so labels land roughly every 80 px:
    classic 1-2-5 metric steps below one minute (down to microseconds at deep
    zooms), then clean time-unit steps above it (1m/2m/5m/10m/15m/30m,
    1h/2h/5h/...). */
double niceTimeStep (double pxPerSec);

/** Formats a ruler tick value for the chosen step. When the visible range
    `span` exceeds one minute the labels switch to clock style - m:ss above a
    minute, h:mm:ss above an hour (fractional seconds only if the step is
    sub-second). Below that: whole seconds when coarse ("45"), and ms/us at
    deep zooms ("2ms", "500us"). */
juce::String formatTimeTickLabel (double t, double step, double span);

/** Clock-style formatting of an absolute time or duration:
    m:ss[.dddd] below one hour, h:mm:ss[.dddd] above it, with up to
    `maxDecimals` fractional digits and trailing zeros trimmed ("2:35",
    "1:02:35.127"). */
juce::String formatClockTime (double t, int maxDecimals = 4);

/** Footer/readout formatting for an absolute time or duration: decimal
    seconds below one minute ("12.500"), m:ss above it, h:mm:ss above an hour.
    Used by the status bar's editable selection fields and playhead readout. */
juce::String formatFooterTime (double t);

/** Parses user-entered time text: plain seconds ("12.5") or clock style
    ("2:35", "02:35.5", "1:02:35"). Returns false for anything else; `out`
    receives the value in seconds on success. */
bool parseTimeText (const juce::String& s, double& out);

// ---- Time-axis scale helpers (shared by rulers, footer and marker panel) ----

/** Clock text with a fixed 4-digit fraction: "hh:mm:ss.xxxx" (hours only
    when non-zero). Used for the Time Offset prompt and its display. */
juce::String formatOffsetTime (double t);

/** Like parseTimeText but also accepts a leading +/- sign. */
bool parseSignedClock (const juce::String& s, double& outSecs);

/** Displayed value for the active time-axis mode:
    Time -> seconds, TimeOffset -> seconds + offset, Sample -> sample count. */
double timeToDisplayValue (double tSecs, TimeAxisMode m, double offsetSecs, double rate);

/** Footer/marker text for a real time under the active axis mode. */
juce::String formatDisplayedTime (double tSecs, TimeAxisMode m, double offsetSecs, double rate);

/** Parse footer/marker text back to real seconds under the active axis mode.
    Sample mode accepts integer/decimal sample counts; the time modes accept
    signed plain-seconds or clock text. */
bool parseDisplayedTime (const juce::String& s, TimeAxisMode m, double offsetSecs,
                         double rate, double& outSecs);

/** Nice 1-2-5 tick step in SAMPLES for a ruler showing `samplesPerPixel`. */
double niceSampleStep (double samplesPerPixel);

// ---- Waveform vertical-axis scale labels ----
// mode: 0=Wav Units (file calibration), 1=dB, 2=Normalized.
/** Label text for the grid line `gridDb` dB below full scale.
    `positiveSide` selects the +a / -a sign in Normalized mode. */
juce::String formatWaveVScaleLabel (double gridDb, int mode, const ScaleCalibration& sc,
                                    bool positiveSide);
/** Axis title for the waveform's left axis under the active vertical scale. */
juce::String waveVScaleTitle (int mode, const ScaleCalibration& sc);
