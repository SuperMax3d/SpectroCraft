#pragma once
// SpectroCraft - Spectral view component (renders engine tiles + overlays)
#include <JuceHeader.h>
#include "Application.h"
#include "SpectralEngine.h"
#include "Settings.h"
#include "MarkerManager.h"
#include <array>
#include <utility>
#include <vector>
#include <functional>

class SpectralView : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    /** Overlays to bake into an exported spectrogram PNG (File -> Spectrogram).
        All off by default: a plain export is just the spectral plot, exactly as
        before these options existed. */
    struct SpectralExportOptions
    {
        bool axes = false;        // include frequency + time axes around the plot
        bool markers = false;     // draw marker lines and titles
        bool selections = false;  // draw time selection band + spectral selection shapes
        bool drawings = false;    // draw user drawings (text / arrows / rectangles / freehand)
    };

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
    // True while the spectral image needs a full re-render (vs overlay-only
    // repaint); cleared by paint() once fresh tiles have been computed.
    bool isImageDirty() const { return imageDirty; }

    // Keyboard-shortcut helpers (applied by MainWindow to both views).
    void zoomHorizontalAtCenter (double factor);   // <1 zoom in, >1 zoom out
    void zoomFrequencyBy (double factor);          // vertical zoom of the freq axis
    void fitAll();

    // Reset the frequency axis to the settings defaults (20 Hz .. 20 kHz clamped to Nyquist).
    void resetFrequencyRange();

    // Current local frequency-axis limits (for the vertical scrollbar).
    double getFreqMin() const { return freqMin; }
    double getFreqMax() const { return freqMax; }

    // Effective FFT size for the current zoom levels: in auto mode this is
    // recomputed from the time window + frequency axis + row height, otherwise
    // it is just settings_.fftSize. (Also used by unit tests.)
    int effectiveFftSizeForRender (int rowHeightPx) const
    { return effectiveSettingsForRender (settings_, rowHeightPx).fftSize; }

    /** The FFT size Auto mode would resolve to at the current zoom levels,
        using the first channel row's height. Used by the Spectral Controls
        panel's "Auto (N)" combo label and the Alt+Shift +/- shortcuts. */
    int autoFftSizeForCurrentZoom() const;

    // Renders one frame into `img` using the view's current state (unit tests).
    bool renderSpectralContentForTest (juce::Image& img, const Settings& rs)
    { return renderSpectralContent (img, rs); }
    // Set the frequency axis directly (called by the vertical scrollbar).
    void setFrequencyRange (double lo, double hi);

    // ---- New-drawing creation (Drawing panel / t,a,r,d shortcuts) ---------
    /** Arms "new drawing" mode for one of the four kinds. The next left click
        inside the plot places a Text item; Arrow/Rectangle/Freehand are made
        by dragging. Esc or clicking outside the plot cancels the mode. */
    void startPendingDraw (DrawingItem::Kind k);
    void cancelPendingDraw();
    bool isPendingDraw() const { return havePendingDraw_; }
    bool isDrawingNew() const { return drawingNew_; }
    DrawingItem::Kind pendingDrawKind() const { return pendingDraw_; }

    // Arrowhead geometry shared by the renderer and the unit tests: returns
    // the two barb endpoints for an arrow whose tip is at (x0, y0) and tail
    // at (x1, y1). Each barb sits `headLen` pixels behind the tip.
    static std::array<std::pair<float, float>, 2> arrowHeadBarbs (float x0, float y0,
                                                                 float x1, float y1,
                                                                 double headLen);

    // Fired when a Text drawing's anchor was clicked: data coordinates plus
    // the anchor pixel position (for estimating the text's extent).
    std::function<void(double t, double f, int px, int py)> onNewTextPlaced;
    // Fired when an Arrow/Rectangle/Freehand drag finished with a valid item.
    std::function<void(DrawingItem item)> onDrawingCreated;

    /** Approximate data-space extent of `text` rendered at textSizePx with its
        baseline anchor at pixel (px, py): used for a Text item's bounding box
        so clicking it in the Drawing panel can fit it into view. */
    void estimateTextExtent (int px, int py, const juce::String& text,
                             float textSizePx, double& outT1, double& outF1) const;

    /** Render the current spectral view to a PNG file. With default options
        only the spectral plot area is written; `opts` can additionally bake
        in the axes and/or the markers / selections / drawings overlays.
        If cancel points at an atomic set to true, rendering stops early and
        false is returned without writing any file. */
    bool saveAsPng (const juce::File& file,
                   const std::atomic<bool>* cancel = nullptr,
                   const SpectralExportOptions& opts = {});

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
    // Armed scale calibration (Scale / Alternative Scale menu): fires when the
    // user places the reference point in the plot.
    std::function<void(double, double)> onRefClickPlaced;

    // Tables panel alignment flow: while set, a left click inside this view is
    // consumed as an alignment reference point and reported with its time.
    std::function<void(double tSecs)> onAlignClick;

    /** When both views are stacked the spectrogram hides its own time ruler
        (the waveform's top ruler covers it). */
    void setShowOwnTimeRuler (bool show);
    bool showsOwnTimeRuler() const { return showOwnTimeRuler; }

    /** Reserves `px` pixels below the top time ruler for the dedicated tables
        track (MainWindow places it there when the ruler is at the top). */
    void setTablesTopInset (int px)
    {
        const int v = std::max (0, px);
        if (v != tablesTopInset) { tablesTopInset = v; dirty = true; imageDirty = true; repaint(); }
    }
    // While an export is composing its offscreen image, rulerH()/plotRect()
    // address the EXPORT layout instead of the live component bounds.
    int rulerH() const
    {
        return haveExportLayout_ ? exportLayout_.getY()
                                 : (showOwnTimeRuler ? kRulerH : 0);
    }

    double getViewStart() const { return viewStart; }
    double getViewEnd() const { return viewEnd; }
    double xToTime (int x) const;
    double timeToX (double t) const;

    // Axis/ruler placement (View menu): the frequency axis sits on the left by
    // default and can be moved to the right; the time ruler sits at the top
    // by default and can be moved to the bottom. Both apply to both tracks.
    bool axisOnRight() const { return app.settings.trackAxisOnRight; }
    bool rulerAtBottom() const { return app.settings.trackRulerAtBottom; }
    // Hit tests for the two chrome strips (full component height/width).
    bool onAxisX (int x) const
    {
        return axisOnRight() ? x >= getWidth() - kLeftAxisWidth : x < kLeftAxisWidth;
    }
    bool onRulerY (int y) const
    {
        if (!showOwnTimeRuler) return false;   // stacked: the waveform's ruler covers it
        return rulerAtBottom() ? y >= getHeight() - kRulerH : y < rulerH();
    }
    float freqToY (double freq, int height) const;
    double yToFreq (float y, int height) const;
    // Absolute frequency at a screen y position (mapped through the channel row
    // containing it; nearest row when between rows).
    double freqAtY (int y) const;

public:
    static constexpr int kRulerH = 24;   // top time ruler height

    /** Frequency-axis label for a real frequency: plain Hz/kHz when `factor`
        is 1, otherwise the scaled value (custom units are unitless). */
    static juce::String formatFreqAxisLabel (double fHz, double factor);

    // Plot area (below the top time ruler, starting at kLeftAxisWidth so it
    // aligns with the waveform view).
    juce::Rectangle<int> plotRect() const;

    /** Channel rows in absolute view coordinates: full component width (so
        axis drags can hit-test them too), positioned inside the plot area.
        Public so headless unit tests can verify the shared row layout. */
    std::vector<juce::Rectangle<int>> rowRects() const;

    // Full canvas size used by the drawing code: normally the component's own
    // bounds, but the export layout while saveAsPng() is composing.
    int layoutWidth()  const { return haveExportLayout_ ? (kLeftAxisWidth + exportLayout_.getWidth())  : getWidth(); }
    int layoutHeight() const { return haveExportLayout_ ? (exportLayout_.getY() + exportLayout_.getHeight()) : getHeight(); }

    // Overlay pieces, split so an export can bake in a subset of them.
    void drawTimeSelectionOverlay (juce::Graphics& g);
    void drawSpectralShapesOverlay (juce::Graphics& g, bool includeLivePreview);
    void drawMarkersOverlay (juce::Graphics& g);
    void drawChannelLabels (juce::Graphics& g);
    void drawTablesOverlay (juce::Graphics& g);

private:
    Application& app;   // audio data read live via app.audioEngine() (active doc)

    // New-drawing creation state (armed by startPendingDraw()).
    bool havePendingDraw_ = false;
    DrawingItem::Kind pendingDraw_ = DrawingItem::Kind::Text;
    bool drawingNew_ = false;      // a new Arrow/Rectangle/Freehand drag is live
    int drawStartX_ = 0, drawStartY_ = 0;
    double drawStartT_ = 0.0, drawStartF_ = 0.0;
    int drawEndX_ = 0, drawEndY_ = 0;   // live end point (pixels)
    int lastFreeX_ = 0, lastFreeY_ = 0;  // freehand decimation in pixels
    std::vector<std::pair<double, double>> freePts_;   // (tSecs, fHz) path

    bool showOwnTimeRuler = true;
    int tablesTopInset = 0;   // reserved strip below the top ruler (tables track)
    // Offscreen export layout: when set, plotRect()/rulerH()/layoutWidth() /
    // layoutHeight() address this rectangle instead of the component bounds.
    bool haveExportLayout_ = false;
    juce::Rectangle<int> exportLayout_;
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

    // Harmonic markers (Spectral Controls panel): last mouseMove position while the
    // feature is active (-1 = mouse outside). Small white circles are drawn at the
    // harmonics of the frequency under the cursor - no lines.
    int harmMouseX = -1, harmMouseY = -1;

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

    // Resolves the settings to use when rendering one channel row: starts from
    // `base`, applies this view's live frequency-axis limits (which may have
    // been zoomed/panned since the panel last sent its settings), and in auto
    // mode replaces fftSize with chooseFftSize() for the current zoom levels.
    Settings effectiveSettingsForRender (const Settings& base, int rowHeightPx) const;

    void drawTimeRuler (juce::Graphics& g);
    void drawFrequencyRuler (juce::Graphics& g);
    void drawOverlays (juce::Graphics& g);

    // Draws the active doc's drawings plus (optionally) the live in-progress
    // preview on every visible channel row. Exports pass false so only
    // committed items are baked into the image.
    void drawDrawings (juce::Graphics& g, bool includeLivePreview = true);

    // Mirror a live tool shape's time extent into the shared time selection.
    void syncToolTimeExtent (juce::Range<double> ext);

    // Move/Scale selection gesture helpers: SHIFT locks to the axis with the
    // larger screen-space delta from the reference point; updates push the
    // transformed session-start snapshot into the active doc live.
    Application::AxisLock axisLockForEvent (const juce::MouseEvent& e) const;
    void updateSelGestureFromEvent (const juce::MouseEvent& e);
    int64_t selGestLastClickMs = 0;   // guards mouseDoubleClick after a gesture click
    // Draws a spectral selection's shapes (plus an optional live preview
    // shape), evaluated together as one ordered boolean list in every channel
    // row. fillCol/edgeCol let the caller recolour - used for the amber
    // overlays of stored selections highlighted from the Selection panel.
    void drawSpectralShapes (juce::Graphics& g,
                             const std::vector<SpectralShape>& committedIn,
                             const SpectralShape* live,
                             juce::Colour fillCol,
                             juce::Colour edgeCol);
    void zoomAtX (int x, double factor);
    void panByTime (double dt);
    void zoomFreqAtY (float y, double factor);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralView)
};
