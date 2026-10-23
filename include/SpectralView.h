#pragma once
// SpectroCraft - Spectral view component (renders engine tiles + overlays)
#include <JuceHeader.h>
#include "Application.h"
#include "SpectralEngine.h"
#include "Settings.h"
#include "MarkerManager.h"
#include <vector>
#include <functional>

class SpectralView : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    explicit SpectralView (Application& app);
    ~SpectralView();

    void paint (juce::Graphics& g) override;
    void resized() override;

    // Mouse interaction: left-drag = select, middle-drag = pan,
    // right-drag = box-zoom (drag on the track -> BOTH axes: time from the
    // box's x span + frequency from its y span; drag starting on the left
    // frequency axis -> vertical only; drag starting on the top time ruler
    // -> horizontal only), plain right-click = context menu,
    // wheel over the plot = time zoom at cursor, wheel over the left
    // frequency axis (or Shift+wheel) = frequency-range zoom,
    // double-click = fit whole file. Clicking moves the playhead.
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;   // status-bar probe
    void mouseExit (const juce::MouseEvent&) override;     // clears the probe
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

    // Called by MainWindow after shared state changes
    void setViewRange (double t0, double t1) { setWindow (t0, t1); }
    void setVisibleChannels (const std::vector<int>& ch)
    {
        if (ch != channels) { channels = ch; imageDirty = true; repaint(); }
    }
    void setSelection (juce::Range<double> s)
    {
        if (s != selection) { selection = s; repaint(); }
    }
    // The spectral selection lives in the active doc (app.doc()->spectralSel);
    // call this after committing/clearing shapes so overlays refresh.
    void setSpectralSelectionChanged() { repaint(); }
    void setPlayhead (double t)
    {
        if (t != playhead) { playhead = t; repaint(); }
    }
    double getPlayhead() const { return playhead; }
    void setMarkers (const std::vector<Marker>* m)
    {
        if (m != markerList) { markerList = m; repaint(); }
    }
    void setSettings (const Settings& s)
    {
        if (s != settings_) { settings_ = s; imageDirty = true; repaint(); }
    }

    // Force a full spectral re-render (e.g. after the colour map changed,
    // which is not part of Settings).
    void invalidateImage() { imageDirty = true; repaint(); }

    // Keyboard-shortcut helpers (applied by MainWindow to both views).
    void zoomHorizontalAtCenter (double factor);   // <1 zoom in, >1 zoom out
    void zoomFrequencyBy (double factor);          // vertical zoom of the freq axis
    void fitAll();

    // Reset the frequency axis to the settings defaults (20 Hz .. 20 kHz clamped to Nyquist).
    void resetFrequencyRange();

    // Current local frequency-axis limits (for the vertical scrollbar).
    double getFreqMin() const { return freqMin; }
    double getFreqMax() const { return freqMax; }
    // Set the frequency axis directly (called by the vertical scrollbar).
    void setFrequencyRange (double lo, double hi);

    // Render the current spectral content (no axes/overlays) to a PNG file.
    // If cancel points at an atomic set to true, rendering stops early and
    // false is returned without writing any file.
    bool saveAsPng (const juce::File& file, const std::atomic<bool>* cancel = nullptr);

    // Notify MainWindow that the time window changed: (newStart, newEnd).
    // MainWindow applies it to BOTH views so they always show the same range.
    std::function<void(double t0, double t1)> onChange;
    // Notified when the local frequency range changes (wheel / keys / reset).
    std::function<void(double lo, double hi)> onFrequencyRangeChanged;
    // Right-click context menu action selected (ContextMenuIDs); handled by MainWindow.
    std::function<void(int)> onContextAction;
    // Status-bar probe: cursor time + frequency + level under the mouse
    // (empty string clears it).
    std::function<void(const juce::String&)> onCursorInfo;

    double getViewStart() const { return viewStart; }
    double getViewEnd() const { return viewEnd; }
    double xToTime (int x) const;
    double timeToX (double t) const;
    float freqToY (double freq, int height) const;
    double yToFreq (float y, int height) const;
    // Absolute frequency at a screen y position (mapped through the channel row
    // containing it; nearest row when between rows).
    double freqAtY (int y) const;

public:
    static constexpr int kRulerH = 24;   // top time ruler height

    // Plot area (below the top time ruler, starting at kLeftAxisWidth so it
    // aligns with the waveform view).
    juce::Rectangle<int> plotRect() const;

private:
    Application& app;   // audio data read live via app.audioEngine() (active doc)
    SpectralEngine& spectral;
    double viewStart = 0, viewEnd = 10;
    double freqMin = 20.0, freqMax = 20000.0;   // local frequency axis limits
    std::vector<int> channels;
    juce::Range<double> selection{ 0, 0 };
    double playhead = 0;
    const std::vector<Marker>* markerList = nullptr;
    bool dirty = true;
    bool imageDirty = true; // spectral image needs recomputation (vs overlay-only repaint)

    Settings settings_;

    // Interaction state
    enum class DragMode { None, Select, Pan, TimePan, VPan, ZoomDrag,
                          Marquee, Lasso, Paint, LineDrag };
    DragMode dragMode = DragMode::None;
    int dragStartX = 0;
    int dragStartY = 0;
    double dragStartTime = 0.0;
    int panLastX = 0;   // last delivered cursor x during time panning
    juce::Rectangle<int> zoomRect;   // right-drag box-zoom rubber band
    bool haveZoomRect = false;

    // Spectral-tool drag state (committed to app.doc()->spectralSel on mouseUp).
    // Plain drag replaces the selection, SHIFT adds, CTRL subtracts.
    bool toolSubtract = false;
    bool toolReplace = true;
    double liveT0 = 0, liveT1 = 0, liveF0 = 0, liveF1 = 0;   // marquee (t,f) space
    std::vector<std::pair<double, double>> lassoPts;        // (tSecs, fHz)
    std::vector<std::pair<double, double>> strokePts;       // paintbrush path

    // Paintbrush straight-line mode (ALT+click): a line from the last stroke's
    // end point (the anchor) to the current cursor position. The anchor is set
    // by every paint mouse-up (freehand drag or ALT+click line).
    bool haveLineAnchor = false;
    double lineAnchorT = 0.0, lineAnchorF = 0.0;
    double lineEndT = 0.0, lineEndF = 0.0;   // live end point while LineDrag
    int probeX = -1, probeY = -1;   // last cursor position (status-bar probe / brush)

    // Per-pixel boolean raster of the spectral selection (1 byte per view
    // pixel), rebuilt on every paint so subtractive shapes render as holes.
    std::vector<uint8_t> selMask;

    // Cached rendered image
    juce::Image cachedImage;
    bool haveImage = false;
    int imgW = 0, imgH = 0;
    double imgT0 = -1, imgT1 = -1;

    // Single path for all time-window changes: updates state and notifies
    // MainWindow with the new range.
    void setWindow (double t0, double t1);

    // Renders the spectral content (one row per visible channel with dark
    // gaps) into an ARGB image of size w×h. Used by paint() and saveAsPng().
    // Returns false if cancelled mid-render.
    bool renderSpectralContent (juce::Image& img, const Settings& rs,
                                const std::atomic<bool>* cancel = nullptr);

    // Channel rows in absolute view coordinates: full component width (so
    // axis drags can hit-test them too), positioned inside the plot area.
    std::vector<juce::Rectangle<int>> rowRects() const;

    void drawTimeRuler (juce::Graphics& g);
    void drawFrequencyRuler (juce::Graphics& g);
    void drawOverlays (juce::Graphics& g);

    // Mirror a live tool shape's time extent into the shared time selection.
    void syncToolTimeExtent (juce::Range<double> ext);
    // Draws the committed spectral selection plus an optional live preview
    // shape, evaluated together as one ordered boolean list in every channel row.
    void drawSpectralShapes (juce::Graphics& g,
                             const std::vector<SpectralShape>& committedIn,
                             const SpectralShape* live);
    void zoomAtX (int x, double factor);
    void panByTime (double dt);
    void zoomFreqAtY (float y, double factor);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralView)
};
