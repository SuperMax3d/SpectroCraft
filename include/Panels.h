#pragma once
// SpectroCraft - Left-side panels
#include <JuceHeader.h>
#include "Application.h"
#include "PythonScripts.h"

#include <cmath>

/** Formats seconds as M:SS.mmm (e.g. 0:04.536) */
juce::String formatTimeCode (double secs);

// ------------------------------------------------- Draggable view divider

/** Thin horizontal bar that lets the user drag to resize the waveform /
    spectral views against each other. Reports absolute mouse Y via onDrag.
*/
class ViewSplitter : public juce::Component,
                       public juce::SettableTooltipClient
{
public:
    ViewSplitter() = default;

    // false = horizontal bar between stacked views (up/down grip),
    // true  = vertical bar between side-by-side areas (left/right grip).
    bool vertical = false;

    std::function<void (const juce::MouseEvent&)> onDrag;
    std::function<void ()> onRelease;   // optional: called when the drag ends

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour::fromRGB (40, 40, 44));
        g.setColour (juce::Colour::fromRGB (110, 110, 120));
        if (!vertical)
        {
            const float cx = getWidth() * 0.5f;
            const float y1 = getHeight() * 0.30f, y2 = getHeight() * 0.70f;
            for (int i = -1; i <= 1; ++i)
                g.drawVerticalLine (cx + i * 3.0f, y1, y2);
        }
        else
        {
            const float cy = getHeight() * 0.5f;
            const float x1 = getWidth() * 0.30f, x2 = getWidth() * 0.70f;
            for (int i = -1; i <= 1; ++i)
                g.drawHorizontalLine (cy + i * 3.0f, x1, x2);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        setMouseCursor (vertical ? juce::MouseCursor::LeftRightResizeCursor
                                 : juce::MouseCursor::UpDownResizeCursor);
        if (onDrag) onDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (onDrag) onDrag (e);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        if (onRelease) onRelease ();
    }

private:
    JUCE_DECLARE_NON_COPYABLE (ViewSplitter)
};

// ------------------------------------------------- Draggable left-panel tabs

/** Shared state for dragging panel tabs between/within the two left-column
    tab bars. MainWindow registers both bars and receives move requests.
*/
struct TabDragContext
{
    std::vector<juce::TabbedComponent*> bars;   // every bar that participates
    /** Move a tab from (from, fromIndex) to position insertIndex in `to`
        (insertIndex is an index into the destination's CURRENT list).
    */
    std::function<void (juce::TabbedComponent* from, int fromIndex,
                        juce::TabbedComponent* to, int insertIndex)> onMove;
};

/** A tab button that can be dragged to a new position - within its own bar or
    into any other bar registered in the same TabDragContext. A plain click
    still selects the tab as usual.
*/
class DragTabButton : public juce::TabBarButton
{
public:
    DragTabButton (const juce::String& name, juce::TabbedButtonBar& ownerBar,
                   juce::TabbedComponent* ownerTabbed, TabDragContext* ctx);
    ~DragTabButton() override;

    // Public so headless unit tests can drive a full drag sequence.
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;

private:

    TabDragContext* context = nullptr;
    juce::TabbedComponent* ownerTabbed_ = nullptr;
    bool dragArmed = false;     // button pressed, waiting for the movement threshold
    bool dragging  = false;     // a live drag is in progress
    juce::Point<float> startLocalPos;
    juce::Component* ghost = nullptr;   // owned; follows the cursor while dragging

    void beginDrag();
    void endDrag (const juce::MouseEvent&);
    static DragTabButton* findHovered (TabDragContext* ctx, const juce::Point<int>& globalPos);

    JUCE_DECLARE_NON_COPYABLE (DragTabButton)
};

/** A TabbedComponent whose tabs are DragTabButtons. */
class DragTabBar : public juce::TabbedComponent
{
public:
    DragTabBar (juce::TabbedButtonBar::Orientation orientation, TabDragContext* ctx);

protected:
    juce::TabBarButton* createTabButton (const juce::String& tabName, int tabIndex) override;

private:
    TabDragContext* context = nullptr;
};

// ---------------------------------------------------------------- Files panel

// Lists every loaded file with its actual properties. Double-clicking a row
// makes that file active (loads it into the views + marker list); right-click
// offers Activate/Close.
class FileListPanel : public juce::Component
{
public:
    explicit FileListPanel (Application& app);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void refresh(); // call after file open/close

    // Set by MainWindow to trigger the Open dialog
    std::function<void()> onOpenRequested;

    // Right-click "Close Panel": set by MainWindow, invoked from this panel's
    // context menus. requestClose() is the testable entry point.
    std::function<void()> onCloseRequested;
    void requestClose() { if (onCloseRequested) onCloseRequested(); }

    /** Builds the "Set Unit and Scale" calibration dialog for one document.
        The caller owns and deletes the returned window (unit tests inspect it
        without running the modal loop). */
    juce::AlertWindow* createScaleDialog (FileDoc& d);

    /** Applies confirmed scale-dialog values to a document: unit None clears
        the calibration; equal per-channel values collapse into one shared
        full-scale value, differing ones are stored per channel. Returns false
        when any value is not positive (unit None always succeeds). */
    static bool applyScaleValues (FileDoc& d, ScaleCalibration::Unit unit,
                                  const std::vector<double>& vals);

private:
    Application& app;
    juce::TextButton openBtn, closeBtn;
    int listScroll = 0;   // first visible row (mouse-wheel scrolling)

    static constexpr int kRowH = 32;      // two text lines per file entry
    static constexpr int kListTop = 34;   // button strip height above the list

    void mouseDown (const juce::MouseEvent& e) override;        // right-click = menu
    void mouseDoubleClick (const juce::MouseEvent& e) override; // activate the file
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override; // scroll

    juce::Rectangle<int> rowRect (int i) const;
    int rowAt (int x, int y) const;       // -1 when no row is under the point
    void activateDoc (int i);
    void closeDoc (int i);
    void showFileMenu (int row);
    void showScaleDialog (int row);
    void openContainingFolder (int row);

    JUCE_DECLARE_NON_COPYABLE (FileListPanel)
};

// ------------------------------------------------- History panel
// Lists every undo state of the ACTIVE file, oldest first:
//   row 0      = "Loaded <file name>" (the state right after opening),
//   rows 1..N  = one per recorded edit action ("Deleted samples", ...).
// The row matching the current state is highlighted; rows beyond it are
// dimmed (they can be redone). Clicking any row jumps straight to that
// state, undoing or redoing as many steps as needed in one go.
class HistoryPanel : public juce::Component
{
public:
    explicit HistoryPanel (Application& app);
    void paint (juce::Graphics& g) override;
    void refresh();   // rebuild the row list from the active doc's undo stacks

    // Set by MainWindow: called with a target STATE index (0 = right after
    // file load, N = end of history). The window undoes/redo-es to it.
    std::function<void(int)> onJumpToState;

    // Right-click "Close Panel": set by MainWindow. requestClose() is the
    // testable entry point.
    std::function<void()> onCloseRequested;
    void requestClose() { if (onCloseRequested) onCloseRequested(); }

    // Unit-test accessors.
    int numRows() const { return (int) rows_.size(); }
    juce::String rowText (int i) const { return rows_[(size_t) i].text; }
    bool rowIsFuture (int i) const { return rows_[(size_t) i].future; }
    int currentState() const { return currentState_; }

private:
    Application& app;
    struct Row { juce::String text; bool future = false; };
    std::vector<Row> rows_;
    int currentState_ = 0;   // number of applied actions (== undo stack size)
    int scroll_ = 0;         // first visible row

    static constexpr int kHeaderH = 24;
    static constexpr int kRowH = 20;

    juce::Rectangle<int> listArea() const;
    juce::Rectangle<int> rowRect (int i) const;
    int rowAt (int x, int y) const;   // -1 when no row is under the point

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    JUCE_DECLARE_NON_COPYABLE (HistoryPanel)
};

// ------------------------------------------------- Frequency Analysis panel
// The FFT size / window / axis here are INDEPENDENT of the spectrogram's
// settings: this panel analyses a short region around the selection or the
// playhead with its own parameters.

/** Snapshot overlay button (1..8).
    Its background always shows the colour of the curve it reproduces:
      - toggled ON  -> full curve colour + thick white border (most obvious),
      - OFF but a value is stored -> dimmed curve colour,
      - OFF and empty            -> very dark tint of the curve colour.
*/
// Note: juce::Button already implements SettableTooltipClient, so setTooltip()
// is available without adding the base class again.
class SnapshotButton : public juce::Button
{
public:
    // Needed for the value-initialised snapBtns[8] array. Defined out-of-line
    // because juce::Button's constructor takes no default argument.
    SnapshotButton();

    void setCurveColour (const juce::Colour& c) { col = c; repaint(); }

    // Whether this slot currently holds stored data (affects the look and
    // is kept in sync by FFTAnalysisView).
    void setStored (bool s)
    {
        if (stored != s) { stored = s; repaint(); }
    }
    bool isStored() const { return stored; }

    // Right-click on the button: context menu (Convert to marker / Clear).
    std::function<void()> onRightClick;

    void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                      bool /*shouldDrawButtonAsDown*/) override;
    void mouseDown (const juce::MouseEvent& e) override;

private:
    juce::Colour col = juce::Colours::grey;
    bool stored = false;
};

class FFTAnalysisView : public juce::Component
{
public:
    explicit FFTAnalysisView (Application& app);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent&) override;
    // Right-click = context menu / right-drag box zoom; left-drag on the
    // bottom Hz axis or right level axis pans that range; wheel zooms.
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    // Right-click "Close Panel": set by MainWindow. requestClose() is the
    // testable entry point.
    std::function<void()> onCloseRequested;
    void requestClose() { if (onCloseRequested) onCloseRequested(); }

    // Called by MainWindow while playing: forces a recompute on state change
    // (during playback with no selection the panel tracks the cursor).
    void setFollowPlayhead (bool f);

    // Recompute the spectrum if the analysis region changed. Public so that
    // a single click in either view can refresh the panel immediately.
    void recomputeIfNeeded();

    /** Re-reads the per-file Frequency Analysis settings from app.settings
        into the panel's combos (called by MainWindow when switching to a
        document whose saved settings differ). No-op when nothing changed. */
    void resyncFromSettings();

    // Snapshot button semantics (also used by unit tests):
    //   - empty slot:  plain click stores the currently analysed time segment
    //                  into slot i and shows its curve;
    //   - stored slot: plain click only toggles the overlay on/off (the stored
    //                  value is NOT updated); Ctrl+click re-stores it from the
    //                  current selection/cursor; Shift+click clears the slot.
    // The modifier keys default to whatever is physically held, so tests can
    // pass explicit ones.
    void handleSnapshotClick (int i,
                              juce::ModifierKeys mods =
                                  juce::ModifierKeys::getCurrentModifiers());
    // Curve-mode variant: the same semantics applied to curveSnaps[i] in
    // axis modes 3..5.
    void handleCurveSnapshotClick (int i, juce::ModifierKeys mods);
    // True when slot i holds data for the CURRENT mode (classic store or
    // matching curve store).
    bool slotHasData (int i) const;
    // Update all eight buttons' stored/toggle/tooltip state for the current
    // mode (called after an axis-mode change).
    void refreshSnapshotButtonStates();
    // Recompute every curve slot whose settings generation is stale.
    void recomputeCurveSlots();

    // Local analysis parameters (unit tests + persistence wiring checks).
    int fftSize() const { return localN; }
    int axisMode() const { return axisMode_; }
    juce::ComboBox& fftSizeComboRef() { return fftSizeCombo; }
    juce::ComboBox& windowComboRef() { return windowCombo; }
    juce::ComboBox& axisComboRef() { return axisCombo; }

    // Curve-mode options (axis modes 3..5):
    //   refTime: 0 = Relative (time axis starts at the selection),
    //            1 = Absolute (file time scale).
    //   smoothing window in frames: 0 = None, or 3/5/7/9/11.
    //   track: 0 = Max (strongest component), 1 = Total (sum / centroid),
    //          2 = Weighted (amplitude-weighted average frequency; level like Total).
    int refTimeMode() const { return refTime_; }
    int smoothingWindow() const { return smooth_; }
    int trackMode() const { return track_; }
    juce::ComboBox& refTimeComboRef() { return refTimeCombo; }
    juce::ComboBox& smoothComboRef() { return smoothCombo; }
    juce::ComboBox& trackComboRef() { return trackCombo; }

    // Stored-snapshot state (unit tests).
    double snapshotT0 (int i) const { return snapshots[i].t0; }
    double snapshotT1 (int i) const { return snapshots[i].t1; }
    int snapshotN (int i) const { return snapshots[i].n; }
    bool snapshotShown (int i) const { return snapshots[i].shown; }
    const std::vector<std::vector<float>>& snapshotMags (int i) const { return snapshots[i].mags; }

    // Snapshots are shared across all open files: each one remembers the file it
    // was stored from, so closing a file clears exactly its own entries.
    void clearSnapshotsFor (const FileDoc* d);
    // Clear slot i (same as Shift+click / Ctrl+click-clear).
    void clearSnapshotSlot (int i);
    // Create a region marker in the snapshot's source file spanning [t0, t1].
    bool convertSnapshotToMarker (int i);

    // Called after [a, b] seconds were removed from file d: shifts / trims /
    // clears every stored snapshot whose source is d and records the before/
    // after state of each touched slot into p.snapshotSlots for undo/redo.
    void recordAndAdjustSnapshotsForDeletion (FileDoc* d, double a, double b, Patch& p);

    // Restores the snapshot slots recorded in patch p after an undo (!isRedo)
    // or redo (isRedo). Curves are recomputed from each slot's source file.
    void restoreSnapshotSlots (FileDoc* d, const Patch& p, bool isRedo);

    // Programmatic setting changes (same code path as the UI combos).
    void setFftSize (int n) { fftSizeCombo.setSelectedId (n, juce::sendNotificationSync); }
    void setWindowType (int t) { windowCombo.setSelectedId (t + 1, juce::sendNotificationSync); }

    // Accessors for unit tests.
    int getCachedN() const { return cachedN; }
    const std::vector<std::vector<float>>& getSpectrum() const { return mags; }
    int getAxisMode() const { return axisMode_; }
    const std::vector<std::vector<float>>& getThirdOctMags() const { return thirdOctMags; }

    // Centred moving-average smoothing of a series (window in points).
    static void smoothSeries (std::vector<float>& v, int window);

    // Live curve data for the new analysis modes (one entry per visible
    // channel, same order as app.visibleChannels). x holds seconds in the
    // time-based modes and Hz in Amp vs Freq mode.
    struct CurveData
    {
        std::vector<double> x;      // time (s) or frequency (Hz), per point
        std::vector<float> ampDb;   // amplitude, dB rel. full scale (floor -160)
        std::vector<float> freqHz;  // tracked frequency per point (time modes);
                                     // equals x in Amp vs Freq mode
    };
    const std::vector<CurveData>& getLiveCurves() const { return liveCurves; }

    // Curve snapshots (the 1..8 buttons in the new modes). Each slot stores
    // BOTH amplitude and frequency series at once, kept separate from the
    // classic FFT snapshot store.
    bool curveSnapShown (int i) const { return curveSnaps[i].mode >= 0; }
    int curveSnapMode (int i) const { return curveSnaps[i].mode; }
    double curveSnapshotT0 (int i) const { return curveSnaps[i].t0; }
    double curveSnapshotT1 (int i) const { return curveSnaps[i].t1; }
    const std::vector<CurveData>& curveSnapData (int i) const { return curveSnaps[i].data; }

    // Local display ranges (independent of the spectrogram's settings):
    // frequency window [fLo_, fHi_] and level window [dbBottom_, dbTop_].
    double getFreqLo() const { return fLo_; }
    double getFreqHi() const { return fHi_; }
    double getDbTop() const { return dbTop_; }
    double getDbBottom() const { return dbBottom_; }

    // Horizontal view window of the time-based curve modes (absolute seconds,
    // always inside the analysed region [cachedT0, cachedT1]).
    double getCurveViewT0() const { return curveViewT0_; }
    double getCurveViewT1() const { return curveViewT1_; }

    // Reset both display ranges to their defaults ("Zoom Out Full").
    void resetDisplayRanges();

    // Public so headless unit tests can drive the zoom/copy paths directly.
    void copySpectrumToClipboard();
    void zoomFreqAtX (int x, double factor);
    // Zoom the horizontal time window around the cursor x position
    // (time-based curve modes); stays inside the analysed region.
    void zoomTimeAtX (int x, double factor);
    // Called after a resample: re-clamp the frequency window to the new
    // Nyquist and force stored snapshots to be recomputed at the new rate.
    void onSampleRateChanged();

    // Cursor readout text for position (x, y). When a curve passes within a
    // few pixels of the cursor, snapped is set and snapDotX/Y hold that point.
    juce::String buildCursorInfo (int x, int y, bool& snapped,
                                  float& snapDotX, float& snapDotY) const;
    // Curve-mode variant of the readout (modes 3-5).
    juce::String buildCurveCursorInfo (int x, int y, bool& snapped,
                                       float& snapDotX, float& snapDotY) const;

    // "| Alt N=value" suffixes for every enabled alternative frequency
    // reference (app.altFreqRefs); empty when none are enabled. Appended to
    // the cursor readout wherever a frequency value is shown.
    static juce::String altFreqSuffix (const Application& appIn, double freqHz);

    // Mapping helpers exposed for unit tests.
    float xToFreq (int x) const;
    double freqToX (double f) const;
    juce::Rectangle<int> plotBounds() const;
    float levelToY (double db) const;
    double yToLevel (float y) const;

    // Time-axis mapping for the curve modes: absolute seconds <-> pixel x.
    // Relative mode maps [cachedT0, cachedT1] onto the full plot width with a
    // zero origin at the selection start; Absolute uses file time directly.
    float timeToX (double tSecs) const;
    double xToTime (int x) const;
    // Vertical frequency mapping used by Freq vs Time mode.
    float freqToY (double fHz) const;
    double yToFreq (float y) const;

    // Grid/label helpers shared between the classic and curve modes.
    void buildFreqAxisTicks (std::vector<double>& freqTicks,
                             std::vector<std::pair<double, bool>>& subTicks,
                             bool& thirdOctFallback) const;
    void drawFreqXLabels (juce::Graphics& g, const juce::Rectangle<int>& plot, int axisB,
                          const std::vector<double>& freqTicks,
                          const std::vector<std::pair<double, bool>>& subTicks,
                          bool thirdOctFallback) const;
    // Grid + curves + labels for the curve analysis modes (3-5).
    void paintCurveModes (juce::Graphics& g);
    // Cursor readout strip and right-drag rubber band (shared by all modes).
    void paintCursorReadout (juce::Graphics& g, const juce::Rectangle<int>& plot) const;
    void paintZoomRubberBand (juce::Graphics& g, const juce::Rectangle<int>& plot) const;

    // Harmonic reference mode (right-click "Set Harmonic References"): the
    // frequency under the cursor is treated as harmonic #N of a fundamental,
    // and harmonics 1..N get delicate vertical lines wherever they are inside
    // the visible frequency window. The anchor follows the mouse X position.
    bool harmonicRefActive() const { return harmonicRefActive_; }
    int harmonicN() const { return harmonicN_; }
    double harmonicAnchorF() const { return harmonicAnchorF_; }
    void setHarmonicReference (int n, double anchorHz);
    void resetHarmonicReference();
    // TSV table of harmonics 1..N (header + one row each), CRLF line endings,
    // Excel-paste friendly. Empty when the mode is off or no spectrum exists.
    juce::String buildHarmonicsTable() const;

    // Switch frequency-axis mode: 0 = "FFT Linear", 1 = "FFT Log",
    // 2 = "1/3 Octave", 3 = "Freq vs Time", 4 = "Amp vs Time",
    // 5 = "Amp vs Freq". sendNotificationSync so the change is applied
    // immediately (JUCE's default notification is async via AsyncUpdater).
    void setAxisMode (int m) { axisCombo.setSelectedId (m + 1, juce::sendNotificationSync); }

    // True for the time-based curve modes (Freq vs Time / Amp vs Time), where
    // the horizontal axis is time instead of frequency.
    bool isTimeAxisMode() const { return axisMode_ == 3 || axisMode_ == 4; }

    // Standard IEC 61260-1 1/3-octave centre frequencies (50 Hz .. 16 kHz).
    static const double* thirdOctCentres() noexcept;
    static int numThirdOctBands() noexcept;

private:
    Application& app;
    juce::ComboBox fftSizeCombo, windowCombo, axisCombo;
    // Curve-mode options (visible only in the new analysis modes):
    juce::ComboBox refTimeCombo, smoothCombo, trackCombo;
    SnapshotButton snapBtns[8];

    // Fixed normalization floor for the stored magnitudes: independent of the
    // visible level window so zooming out never changes the curve data.
    static constexpr double kNormFloorDb = 160.0;

    int localN = 2048;          // independent from the spectrogram's FFT size
    // Axis mode: 0 = "FFT Linear", 1 = "FFT Log" (default),
    // 2 = "1/3 Octave" (band plot, 50 Hz .. 16 kHz),
    // 3 = "Freq vs Time", 4 = "Amp vs Time", 5 = "Amp vs Freq".
    int axisMode_ = 1;
    bool followPlayhead_ = false;

    // Curve-mode options (persisted in app.settings):
    int refTime_ = 0;           // 0 = Relative, 1 = Absolute (time-based modes)
    int smooth_ = 0;            // smoothing window in frames: 0 / 3 / 5 / 7 / 9 / 11
    int track_ = 0;             // 0 = Max, 1 = Total, 2 = Weighted

    // Local display ranges: the frequency window and the level window are
    // independent of app.settings.dBRange and of the spectrogram view.
    double fLo_ = 20.0, fHi_ = 20000.0;   // visible frequency range (Hz)
    double dbTop_ = 0.0, dbBottom_ = -80.0; // visible level window (dB rel.)

    // Horizontal view window of the time-based curve modes (absolute seconds),
    // always clamped inside the analysed region [cachedT0, cachedT1].
    double curveViewT0_ = 0.0;
    double curveViewT1_ = 0.0;
    bool haveCurveView_ = false;

    // Interaction state for axis panning / box zooming.
    enum class DragMode { None, PanFreq, PanLevel, PanFreqV, PanTime, ZoomDrag };
    // PanFreqV: vertical frequency panning in Freq vs Time mode.
    DragMode dragMode = DragMode::None;
    int dragStartX = 0, dragStartY = 0;
    juce::Rectangle<int> zoomRect;
    bool haveZoomRect = false;

    struct Snapshot
    {
        double t0 = 0, t1 = 0;   // time segment of the file that was analysed
        int n = 0;               // FFT size used for the last computation
        int gen = -1;            // settings generation (FFT size + window type)
        bool shown = false;
        std::vector<std::vector<float>> mags;   // per channel, dB-normalized 0..1
        std::vector<std::vector<float>> bands;  // 1/3-octave conversion (per channel)
        FileDoc* source = nullptr;              // file this snapshot was stored from
        std::vector<int> chans;                 // visible channels at store time
    };
    Snapshot snapshots[8];      // stored curve overlays, shared across all files

    // Stored curves for the new analysis modes (modes 3..5). Kept in a
    // separate array from `snapshots`: saving a curve never touches the
    // classic FFT snapshot slots and vice versa. Each slot stores BOTH the
    // amplitude series and the frequency series captured at save time.
    struct CurveSnapshot
    {
        int mode = -1;          // axis mode it was stored in (3..5); -1 = empty
        double t0 = 0, t1 = 0;  // analysed time segment of the source file
        int gen = -1;           // settings generation at store time
        int track = 0;          // tracking option at store time (0=Max, 1=Total)
        int smooth = 0;         // smoothing window at store time (0/3/5)
        bool shown = false;
        std::vector<CurveData> data;   // per visible channel (same order)
        FileDoc* source = nullptr;
        std::vector<int> chans;
    };
    CurveSnapshot curveSnaps[8];

    // Cached spectrum data
    std::vector<std::vector<float>> mags;   // per channel, normalized 0..1 vs kNormFloorDb
    // Per-channel 1/3-octave band values (normalized 0..1), one entry per
    // band with centre <= Nyquist. Only populated when axisMode_ == 2.
    std::vector<std::vector<float>> thirdOctMags;
    // Live curves for the new analysis modes (one per visible channel).
    std::vector<CurveData> liveCurves;
    int cachedCurveMode = -1;   // axis mode the live curves were computed in
    int cachedCurveTrack = 0;   // track option at compute time
    int cachedCurveSmooth = 0;  // smoothing window at compute time

    int cachedN = 0;
    double cachedT0 = -1, cachedT1 = -1;
    // The sample rate must match too: after a resample the same time region
    // maps to different samples (and the frequency axis moves), so a cache
    // hit on t0/t1 alone would draw stale curves.
    double cachedRate = -1.0;
    juce::File cachedFile;
    int64_t lastComputeMs = 0;
    bool lastComputeWasExpensive = false;   // multi-window averaging (throttle key)
    int cursorX = -1, cursorY = -1;

    // Harmonic reference mode state (see the public accessors above).
    bool harmonicRefActive_ = false;
    int harmonicN_ = 1;              // which harmonic number the anchor is
    double harmonicAnchorF_ = 0.0;   // Hz, follows the mouse while active

    // Bumped whenever the FFT size or window type changes: stored snapshots
    // remember the generation they were computed with and are recomputed from
    // their own time segment when it no longer matches.
    int settingsGen = 0;

    int axisWidth() const;   // right level-axis width (wider for linear V/g labels)

    // Zoom helpers for the local display ranges.
    void zoomFreqAtY (float y, double factor);
    void zoomLevelAtY (float y, double factor);
    void showContextMenu (bool onLevelAxis, bool onFreqAxis, int clickX);
    // Popup asking which harmonic number the clicked frequency is.
    void showSetHarmonicDialog (double clickFreqHz);
    void copyHarmonicsToClipboard();

    // Core averaging computation: analyse region [t0, t1] with windows of n
    // samples at the current window type; one magnitude vector per channel in
    // chans. Shared by the live spectrum and every stored snapshot (each from
    // its own source file's engine).
    void computeRegionFrom (AudioEngine& eng, const std::vector<int>& chans,
                            double t0, double t1, int n,
                            std::vector<std::vector<float>>& outMags);

    // Curve computations for the new analysis modes. `mask` is nullptr when
    // there is no spectral selection (all bins count); otherwise only bins
    // inside mask.contains(tSecs, fHz) contribute.
    //
    // Time-based modes (Freq vs Time / Amp vs Time): one point per FFT frame
    // under [t0, t1] (fixed 2048-point Hann frames; the hop grows so that at
    // most kMaxCurveFrames points are produced). Per point: x = frame centre
    // time, ampDb/freqHz follow `track` (Max: strongest bin's level +
    // frequency; Total: summed level + amplitude-weighted centroid).
    void computeTimeCurveFrom (const AudioEngine& eng, const std::vector<int>& chans,
                               double t0, double t1, int track,
                               const SpectralSelection* mask,
                               std::vector<CurveData>& out);

    // Amp vs Freq mode: one point per FFT bin; ampDb is the per-bin Max or
    // Total (summed linear magnitudes) over all frames under [t0, t1].
    void computeSpectrumCurveFrom (const AudioEngine& eng, const std::vector<int>& chans,
                                   double t0, double t1, int n, int windowType,
                                   int track, const SpectralSelection* mask,
                                   std::vector<CurveData>& out);

    // Recompute the live curves for the current mode/track/smoothing over
    // region [t0, t1] (called from recomputeIfNeeded()).
    void recomputeCurve (double t0, double t1);

    // Top control area height: 52 px for the classic modes, 78 px when the
    // third option row (Smoothing / Track) is visible.
    // Classic modes and Amp vs Freq have two control rows (52 px); the
    // time-based curve modes add a Ref Time row on top (78 px).
    // Two control rows in the classic modes and Amp vs Freq, three in the
    // time-based curve modes (Ref Time row).
    int topHeight() const { return isTimeAxisMode() ? 78 : 52; }

    // Rebuild curve slot i from its source file using the parameters stored
    // in the slot itself. Clears the slot when the source is gone.
    void recomputeCurveSlot (int i);
    // Clear curve slot i (Shift+click on a curve-mode button).
    void clearCurveSlot (int i);
    // Live-analysis convenience wrapper: uses the active document.
    void computeRegion (double t0, double t1, int n,
                        std::vector<std::vector<float>>& outMags)
    {
        computeRegionFrom (app.audioEngine(), app.visibleChannels, t0, t1, n, outMags);
    }
    // Recompute all valid snapshots from their stored time segments using the
    // current FFT size / window type, then refresh their 1/3-octave bands.
    void recomputeSnapshots();
    // Rebuild one slot's curve from its source file at the current FFT size.
    void recomputeSnapshotSlot (int i);
    // Right-click context menu on snapshot button i (Convert to marker / Clear).
    void showSnapshotMenu (int i);
    // Re-derive the 1/3-octave band values of every snapshot (cheap: no FFT).
    void refreshSnapshotBands();
    // Refresh button i's tooltip to describe its current stored state.
    void updateSnapshotTooltip (int i);

    JUCE_DECLARE_NON_COPYABLE (FFTAnalysisView)
};

// TextEditor that commits its content when Return is pressed (this JUCE build
// has no onReturnPressed member). Also used for focus-lost commits via the
// onFocusLost callback wired by the owner.
class LevelEdit : public juce::TextEditor
{
public:
    std::function<void()> onEnter;

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress (juce::KeyPress::returnKey))
        {
            if (onEnter != nullptr) onEnter ();
            return true;
        }
        return TextEditor::keyPressed (k);
    }
};

// ---------------------------------------------------- Spectral Controls panel

class SpectralControlsPanel : public juce::Component
{
public:
    explicit SpectralControlsPanel (Application& app);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void refresh(); // call after file open/close or settings change

    /** The three level labels drawn above the colour-map gradient:
        left = bottom of the level window, centre = middle, right = top.
        Values are absolute levels in the calibration's unit (linear V/g for
        volts/grams files). */
    static std::array<juce::String, 3> levelScaleLabels (double minDb, double maxDb,
                                                         const ScaleCalibration& sc)
    {
        // formatSpectralLevel() takes a RELATIVE level (dB below full scale) and
        // applies the calibration itself.
        return { ScaleCalibration::formatSpectralLevel (minDb, sc),
                 ScaleCalibration::formatSpectralLevel ((minDb + maxDb) * 0.5, sc),
                 ScaleCalibration::formatSpectralLevel (maxDb, sc) };
    }

    /** Inverse of formatSpectralLevel(): parses a level typed in the file's
        display unit ("-80 dB", "0.01 V", "94 dB-SPL") into relative dB below
        full scale. Returns false when the text is not a valid level. */
    static bool parseSpectralLevel (const juce::String& text, const ScaleCalibration& sc,
                                    double& outRelativeDb);

    // Commit helpers: apply a user-entered value to the settings and refresh
    // the controls. Return false when the value is rejected; the editors are
    // reverted to their canonical text in that case.
    bool applyDbRangeValue (double rangeDb);   // keeps the top edge fixed
    bool applyDbRangeText (const juce::String& text);
    bool applyDbMinText (const juce::String& text);
    bool applyDbMaxText (const juce::String& text);
    void syncLevelControls();                  // slider + editors + preview from settings

    // Set by MainWindow: resets the spectral view's frequency axis.
    std::function<void()> onResetFreq;

    // Right-click "Close Panel": set by MainWindow. requestClose() is the
    // testable entry point.
    std::function<void()> onCloseRequested;
    void requestClose() { if (onCloseRequested) onCloseRequested(); }

    // Current dB-range slider position (tests).
    double getDbRangeSliderValue() const { return dBRangeSlider.getValue(); }
    juce::String getDbMinText() const { return dbMinEdit.getText(); }
    juce::String getDbMaxText() const { return dbMaxEdit.getText(); }

    // True when the FFT-size combo offers an item with this label (tests).
    bool hasFftSizeOption (const juce::String& label) const
    {
        for (int i = 0; i < fftSizeCombo.getNumItems(); ++i)
            if (fftSizeCombo.getItemText (i) == label) return true;
        return false;
    }

private:
    void mouseDown (const juce::MouseEvent&) override;   // right-click = Close Panel

    Application& app;
    juce::ComboBox colourMapCombo, fftSizeCombo;
    juce::Slider dBRangeSlider;
    LevelEdit dbRangeEdit, dbMinEdit, dbMaxEdit;
    juce::TextButton logToggle;
    juce::Label colourMapLab, dbRangeLab;
    juce::TextButton resetFreqBtn;

    class MapPreview : public juce::Component
    {
    public:
        explicit MapPreview (Application& a) : app (a) {}
        void paint (juce::Graphics& g) override
        {
            const auto b = getLocalBounds();
            const int labelH = 18;
            const auto grad = juce::Rectangle<int> (b.getX(), b.getY() + labelH,
                                                    b.getWidth(), std::max (1, b.getHeight() - labelH));

            // Colour-map gradient: left end = quietest (bottom of the dB range),
            // right end = full scale.
            const auto lut = buildColourLUT (app.colourMap);
            for (int x = 0; x < grad.getWidth(); ++x)
            {
                const float t = (float) x / std::max (1, grad.getWidth() - 1);
                const int i = (int) (t * 255.0f + 0.5f);
                g.setColour (juce::Colour ((int) lut[i * 4], (int) lut[i * 4 + 1], (int) lut[i * 4 + 2]));
                g.fillRect (grad.getX() + x, grad.getY(), 1, grad.getHeight());
            }

            // Level scale above the gradient: absolute values in the active file's
            // unit (dB / dB-SPL / dBV / dB-g). The left and right ends are editable
            // LevelEdit fields laid out by SpectralControlsPanel::resized(); only
            // the centre label is drawn here. refresh() repaints this component,
            // so the labels follow both the level controls and Set Scale changes.
            const auto labs = levelScaleLabels ((double) app.settings.dbMinDb,
                                                (double) app.settings.dbMaxDb, app.scale());

            g.setFont (11.0f);
            g.setColour (juce::Colours::lightgrey);
            {
                const int tw = std::max (1, juce::GlyphArrangement::getStringWidthInt
                                                (g.getCurrentFont(), labs[1]));
                g.drawText (labs[1], b.getCentreX() - tw / 2, b.getY(), tw, labelH,
                           juce::Justification::centredLeft);
            }
        }
    private:
        Application& app;
    };

    MapPreview preview;

    JUCE_DECLARE_NON_COPYABLE (SpectralControlsPanel)
};

// ---------------------------------------------------------- Markers panel

// Modal "Find" dialog shared by the Markers and Transcription panels. The box
// is pre-filled with lastNeedle; on OK, needle receives the trimmed query and
// true is returned (an empty query cancels the search).
bool showFindDialog (juce::Component* parent, const juce::String& title,
                     const juce::String& lastNeedle, juce::String& needle);

// Note: juce::Component already inherits from juce::MouseListener in JUCE 9,
// so overriding mouseDown() below is sufficient (no extra base needed).
class MarkerListPanel : public juce::Component
{
public:
    explicit MarkerListPanel (Application& app);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void refresh(); // call after marker changes / file open/close

    // Right-click context menu actions (also used by unit tests).
    void copyAllMarkers();
    int pasteMarkersFromClipboard();

    // Right-click "Close Panel": set by MainWindow. requestClose() is the
    // testable entry point.
    std::function<void()> onCloseRequested;
    void requestClose() { if (onCloseRequested) onCloseRequested(); }

    // CSV export/import via file chooser, and "Remove All" with a confirm box.
    void exportMarkersCsv();
    int importMarkersCsv();   // returns how many markers were added
    void removeAllMarkers();

    // Programmatic row selection (normally done via the table UI).
    void selectRow (int row);

    // Time text under the active time-axis scale mode. Durations never carry
    // the Time Offset; Sample mode shows sample counts.
    juce::String markerTimeText (double t, bool duration) const;
    /** Parses a begin/end/length field: displayed value -> real seconds.
        Empty/invalid text yields 0 in displayed units (parseTimeCode parity). */
    double parseMarkerTime (const juce::String& s, bool duration) const;

    // Double-click a row: selects it - which sets the time selection to the
    // marker's range and makes sure both views' time window includes it (+10%
    // margin). Also used by unit tests.
    void activateRow (int row);

    // Excel-style click handling: Shift+click selects the exact anchor..row
    // range (replacing any previous selection); plain clicks move the anchor;
    // Ctrl/Cmd+click toggles one row. Every resulting selection set becomes
    // the time selection and fits in view (via onSelectionSetChanged).
    void onRowClicked (int row, const juce::MouseEvent& e);
    // Removes every marker whose row is selected; refreshes + notifies.
    // The context menu's Delete Selected item does this - DEL no longer
    // touches the list (it deletes audio data instead).
    void deleteSelected ();
    // DEL key: actually deletes the WAV data of the active selection - zeros
    // a spectral selection, or trims out a time range (app.del).
    void deleteAudioData ();
    // Pans/zooms both views so that every selected marker is visible
    // (ensureTimeRangeVisible adds the usual 10% margin).
    void fitViewToSelected ();
    // First selected row (-1 when nothing is selected): the detail editors and
    // double-click activation follow it, while Delete Selected removes ALL
    // selected rows.
    int primaryRow() const;
    // Called on any selection change: re-syncs the detail editors, makes the
    // union of the selected markers the time selection and fits the view to it.
    void onSelectionSetChanged ();

    // Accessors for headless unit tests.
    int numSelectedRows() const { return table.getNumSelectedRows (); }
    std::vector<int> selectedRowIndices() const;

    // Find (Ctrl+F / F3 / Ctrl+F3): case-insensitive substring search over
    // marker titles and comments. The query/anchor state is independent of
    // the transcription panel's own search.
    void showFindDialog();
    bool findNext();
    bool findPrevious();
    void setFindQuery (const juce::String& q);
    const juce::String& getFindQuery() const { return searchNeedle; }

private:
    void mouseDown (const juce::MouseEvent&) override;
    void showMarkerContextMenu();
    bool findInDirection (int dir);

private:
    Application& app;

    class Model : public juce::TableListBoxModel
    {
    public:
        explicit Model (MarkerListPanel* owner);
        int getNumRows() override;
        void paintRowBackground (juce::Graphics& g, int rowNumber, int width, int height,
                                 bool selected) override;
        void paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height,
                        bool selected) override;
        // Excel-style multi-selection: plain click = anchor + single select
        // (JUCE default), Ctrl/Cmd+click toggles one row (JUCE default),
        // Shift+click replaces the selection with the exact range from the
        // anchor to this row - handled by the panel, which also fits view.
        void cellClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;
        void cellDoubleClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;
        void selectedRowsChanged (int) override;
        // DEL removes every selected marker (not just the last-clicked one).
        void deleteKeyPressed (int) override;

    private:
        MarkerListPanel* owner;
    };

    Model model { this };
    juce::TableListBox table { "", &model };

    juce::TextButton addBtn, deleteBtn;
    juce::TextEditor beginEdit, endEdit, lengthEdit, titleEdit, commentEdit;
    int selectedRow = -1;
    bool editing = false; // guard against feedback loops in text editors
    int anchorRow = -1;   // last plain-clicked row: the Shift+click range origin

    juce::String searchNeedle;   // current find query (empty = none)
    int searchAnchorRow = -1;    // row of the last found match (-1 = none)

    void updateDetails();
    void onBeginEndChanged (bool isBegin);
    void onLengthChanged();
    void onTitleCommentChanged (bool isTitle);

    JUCE_DECLARE_NON_COPYABLE (MarkerListPanel)
};

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
// ------------------------------------------------- Transcription panel

/** Whisper transcription segments for the active file (Phase 3 of the AI plan).
    A read-only table of [start, end, text] rows plus a "Transcribe..." button
    (which opens the model/language dialog via onRunRequested) and a "Clear"
    button. Double-clicking a row jumps the playhead to its start and selects
    its range. Segments live in FileDoc::sctt and are saved into WAV files as
    an SCTT RIFF chunk — they never appear in the Markers panel.

    Searching is shortcut-driven (Ctrl+F / F3 / Ctrl+F3, or right-click >
    Find): there is no on-panel search box. The query/anchor state is
    independent of the markers panel's own search. */
class TranscriptionPanel : public juce::Component
{
public:
    explicit TranscriptionPanel (Application& app);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void refresh();   // call after transcription changes / file open/close

    // Case-insensitive substring search over the segment texts. Selects and
    // scrolls to the next/previous match (wrapping around), jumps the
    // playhead there like a double-click, and returns false when no segment
    // matches at all.
    void showFindDialog();
    bool findNext();
    bool findPrevious();
    void setFindQuery (const juce::String& q);
    const juce::String& getFindQuery() const { return searchNeedle; }

    // Creates a marker spanning the segment's time range (point marker when
    // it has no length), titled with its text; the full text goes into the
    // marker comment. Persists the sidecar and refreshes all panels/views.
    void addMarkerForRow (int rowNumber);

    // Right-click "Edit...": modal dialog editing start/end/text of the given
    // segment; commits through applySegmentEdit.
    void showEditDialog (int rowNumber);
    // Applies an edit to one segment (times in seconds). Returns false when
    // there is no document, the times are not finite or negative. Swaps
    // start/end when they arrive reversed. Persists and refreshes on success.
    bool applySegmentEdit (int rowNumber, double startSec, double endSec,
                           const juce::String& text);
    // Inserts a new empty segment at the selection start (or playhead) in
    // sorted order and selects it; returns the new row number (-1 = no doc).
    int addEntry();
    // Removes every selected row (the fallback row when nothing is selected),
    // persists and refreshes.
    void removeSelectedRows (int fallbackRow);

    // Jumps to a segment: moves the playhead, selects its time range and pans
    // both views so the whole selection is visible (with 10% breathing room,
    // like marker / stored-selection double-clicks). Used by double-clicks
    // and find jumps.
    void activateRow (int rowNumber);

    // Set by MainWindow: opens the model/language dialog and starts the run.
    std::function<void()> onRunRequested;

    // Right-click "Close Panel": set by MainWindow. requestClose() is the
    // testable entry point.
    std::function<void()> onCloseRequested;
    void requestClose() { if (onCloseRequested) onCloseRequested(); }

private:
    Application& app;

    class Model : public juce::TableListBoxModel
    {
    public:
        explicit Model (TranscriptionPanel* owner);
        int getNumRows() override;
        void paintRowBackground (juce::Graphics& g, int rowNumber, int width, int height,
                                 bool selected) override;
        void paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height,
                        bool selected) override;
        void cellClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;
        void cellDoubleClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;

    private:
        TranscriptionPanel* owner;
    };

    void mouseDown (const juce::MouseEvent&) override;   // right-click = Close Panel

    Model model { this };
    juce::TableListBox table { "", &model };

    juce::TextButton runBtn, clearBtn;

    bool findInDirection (int dir);

    juce::String searchNeedle;   // current find query (empty = none)
    int searchAnchorRow = -1;    // row of the last found match (-1 = none)

    JUCE_DECLARE_NON_COPYABLE (TranscriptionPanel)
};
#endif // SC_ENABLE_AI

// ------------------------------------------------- Selection panel

/** Named spectral selections (store / recall).

    - The table lists this file's saved selections (Name + Comment columns;
      no inline editing anywhere). Save stores the CURRENT selection under a
      name (auto-named "Selection 01", "Selection 02"...; with no spectral
      selection active a plain time range is stored as a full-height band).
    - Double-clicking a row recalls that entry as the live selection. Rows
      support Excel-style multi-selection: plain click selects one row (and
      sets the anchor), Ctrl/Cmd+click toggles a single row, Shift+click
      range-selects from the anchor to the clicked row; the union of all
      selected entries becomes the live mask and both views fit its time
      extent (+10%).
    - Right-click offers "Edit..." (name + comment dialog) and "Delete
      Selected" (removes list rows only). DEL deletes the AUDIO under the
      live selection instead (undoable); the Delete button removes selected
      rows.
    - The list persists automatically with the file's unified metadata
      (embedded SCMD chunk or .meta.json sidecar) on every Save. */
// Derives from FileDragAndDropTarget so .selections.json files can be
// dropped straight onto the panel (JUCE routes a drop to the nearest
// ancestor target under the cursor).
class SelectionPanel : public juce::Component,
                       public juce::FileDragAndDropTarget
{
public:
    explicit SelectionPanel (Application& app);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void refresh();   // call after doc changes / selection or list changes

    // Store the current selection (time range in Time Range mode, live
    // spectral selection otherwise) under an auto-assigned name. Also used
    // by unit tests.
    void doSave();
    // Recall a stored selection: replaces the live spectral selection with it
    // and makes sure both views' time window includes its time range (+10%
    // margin). Also used by unit tests.
    void doRecall (int row);
    // Replaces the live spectral selection with the UNION of every selected
    // stored selection (shapes appended in row order) and fits the view to
    // their combined time range. Called on any table selection change.
    void recallSelected ();

    // Excel-style click handling: Shift+click selects the exact anchor..row
    // range; plain clicks move the anchor; Ctrl/Cmd+click toggles one row.
    // Every resulting selection set becomes the live spectral selection and
    // fits in view (via onSelectionSetChanged). Double-clicking any cell
    // recalls that row's stored selection.
    void onRowClicked (int row, const juce::MouseEvent& e);
    // Removes every stored selection whose row is selected; refreshes +
    // notifies. The Delete button and the context-menu item do this - DEL no
    // longer touches the list (it deletes audio data instead).
    void deleteSelected ();
    // DEL key: actually deletes the WAV data of the active selection - zeros
    // a spectral selection, or trims out a time range (app.del).
    void deleteAudioData ();
    // Pans/zooms both views so that every selected stored selection's time
    // range is visible (ensureTimeRangeVisible adds the usual 10% margin).
    void fitViewToSelected ();
    // First selected row (-1 when nothing is selected): recall and Edit follow
    // it, while Delete Selected removes ALL selected rows.
    int primaryRow() const;
    // Called on any selection change: makes the union of the selected stored
    // selections the live spectral selection and fits the view to it.
    void onSelectionSetChanged ();

    // "Clone Harmonics by Numbers..." dialog (replaces the old inline
    // multiplier field + Apply button). Returns without doing anything when
    // there is no current spectral selection to clone.
    void showCloneNumbersDialog();
    // Toggles the interactive "Clone Harmonics by Mouse" pick mode on/off.
    void toggleClonePick ();

    // Right-click "Edit...": dialog to rename the primary selected stored
    // selection and change its comment.
    void showEditDialog ();
    // Applies a new name/comment to the primary selected row (an empty name
    // keeps the old one). The Edit dialog commits through this; unit tests
    // call it directly.
    void applyNameComment (const juce::String& name, const juce::String& comment);
    // Next free auto-assigned store name: "Selection 01", "Selection 02", ...
    // (the smallest index not already used as a name).
    juce::String nextSelectionName() const;

    // Accessors for headless unit tests.
    int numSelectedRows() const { return table.getNumSelectedRows (); }
    std::vector<int> selectedRowIndices() const;

    // Loads `f` into the active doc's saved-selection list (replacing it).
    // Returns false - leaving everything unchanged - when the file is missing,
    // not .json, or not a valid saved-selections document. Used by the
    // drag & drop import; the loaded list persists with the file's unified
    // metadata on the next Save.
    bool loadSelectionsFromFile (const juce::File& f);

    // Drag & drop of .selections.json files onto the panel (also used by unit tests).
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // Right-click "Close Panel": set by MainWindow. requestClose() is the
    // testable entry point.
    std::function<void()> onCloseRequested;
    void requestClose() { if (onCloseRequested) onCloseRequested(); }

private:
    Application& app;

    class Model : public juce::TableListBoxModel
    {
    public:
        explicit Model (SelectionPanel* owner);
        int getNumRows() override;
        void paintRowBackground (juce::Graphics& g, int rowNumber, int width, int height,
                                 bool selected) override;
        void paintCell (juce::Graphics& g, int rowNumber, int columnId, int width, int height,
                        bool selected) override;
        // Excel-style multi-selection: plain click = anchor + single select
        // (JUCE default), Ctrl/Cmd+click toggles one row (JUCE default),
        // Shift+click replaces the selection with the exact range from the
        // anchor to this row - handled by the panel, which also fits view.
        void cellClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;
        // Double-clicking any cell recalls that row's stored selection.
        void cellDoubleClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;
        void selectedRowsChanged (int) override;
        // DEL deletes the audio data of the active selection (app.del).
        void deleteKeyPressed (int) override;

    private:
        SelectionPanel* owner;
    };

    Model model { this };
    juce::TableListBox table { "", &model };

    juce::TextButton saveBtn, recallBtn, deleteBtn;
    juce::Label savedLab;

    FileDoc* selListDoc = nullptr;   // doc whose list the current rows belong to
    int anchorRow = -1;              // last plain-clicked row: Shift+click origin

    void mouseDown (const juce::MouseEvent&) override;
    void showLoadContextMenu();

    // Drag-over highlight feedback.
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    bool dragOver = false;

    void doDelete();

    JUCE_DECLARE_NON_COPYABLE (SelectionPanel)
};

// ---------------------------------------------------------------- Status bar

// Thin strip at the bottom of the window: active file name + mouse probe info
// on the left, playhead and selection times on the right.
class StatusBar : public juce::Component,
                  public juce::TextEditor::Listener
{
public:
    // MSVC 2019 does not generate an implicit default constructor when a
    // deleted copy constructor is declared, so declare it explicitly.
    StatusBar();

    void paint (juce::Graphics& g) override;
    void resized() override;

    // Left side: free-form probe text from whichever view the cursor is over
    // (empty string clears it).
    void setMouseInfo (const juce::String& s)
    {
        if (s != mouseText) { mouseText = s; repaint(); }
    }
    /** Called when the user edits one of the selection fields below. The
        callback receives the new start/stop in seconds; MainWindow wires it
        to Application::setSelection. */
    std::function<void (double, double)> onSelectionEdited;

    /** Called when the user edits the playhead-position field below; the
        callback receives the new position in seconds. MainWindow wires it to
        Application::setPlayhead (which clamps it to the file). */
    std::function<void (double)> onPositionEdited;

    // Right side: editable selection range and playhead position.
    void setSelection (double t0, double t1);
    void setPlayhead (double t);

    /** Time-axis scale context for the footer fields + playhead readout:
        values are displayed/parsed in the active mode's units. */
    void setTimeContext (TimeAxisMode mode, double offsetSecs, double rate)
    {
        timeCtxMode = mode;
        timeCtxOffset = offsetSecs;
        timeCtxRate = std::max (1.0, rate);
        reformatField (startEdit);
        reformatField (stopEdit);
        reformatField (lenEdit);
        reformatField (posEdit);
    }

    // Current field texts (unit tests).
    juce::String getSelStartText() const { return startEdit.getText(); }
    juce::String getSelStopText()  const { return stopEdit.getText(); }
    juce::String getSelLenText()   const { return lenEdit.getText(); }
    bool selFieldsVisible() const { return startEdit.isVisible(); }
    juce::String getPosText()      const { return posEdit.getText(); }
    bool posFieldVisible()         const { return posEdit.isVisible(); }
    juce::String getPlayheadText() const { return playPart; }

    // The editable fields themselves (unit tests).
    juce::TextEditor& selStartField() { return startEdit; }
    juce::TextEditor& selStopField()  { return stopEdit; }
    juce::TextEditor& selLenField()   { return lenEdit; }
    juce::TextEditor& selPosField()   { return posEdit; }
    // Left side: name of the currently active file (empty when none is loaded).
    void setFileName (const juce::String& s)
    {
        if (s != fileName) { fileName = s; repaint(); }
    }
    juce::String getFileName() const { return fileName; }

    // Left side: the active file's actual sample rate, shown right after the
    // file name (empty when no file is loaded or the rate is unknown).
    void setSampleRate (double rate)
    {
        juce::String s;
        if (rate > 0.0 && std::isfinite (rate))
            s = (std::abs (rate - std::round (rate)) < 1e-9
                    ? juce::String ((int64_t) std::llround (rate))
                    : juce::String (rate, 3)) + " Hz";
        if (s != ratePart) { ratePart = s; repaint(); }
    }
    juce::String getSampleRateText() const { return ratePart; }

    // Left side: the active selection tool name (e.g. "Marquee", "Paintbrush
    // 200 Hz"); empty in Time Range mode.
    void setModeText (const juce::String& s)
    {
        if (s != modeText) { modeText = s; repaint(); }
    }

    // TextEditor::Listener: normalise a field when it loses focus.
    void textEditorFocusLost (juce::TextEditor& ed) override;

private:
    void applyFieldVisibility();
    void commitField (juce::TextEditor& ed);
    void reformatField (juce::TextEditor& ed);
    double fieldTime (const juce::TextEditor& ed) const;

    // Field text under the active time-axis scale. Durations (the length
    // field) never carry the Time Offset; Sample mode scales them linearly.
    juce::String formatFieldTime (double v, bool duration) const;
    bool parseFieldText (const juce::String& s, double& outSecs, bool duration) const;

    // Right-side layout: [Sel:] [start] [-] [stop] [(] [len] [)]  gap  [Pos:] [pos]
    struct RightLayout { int selLabelX, startX, dashX, stopX, openX, lenX,
                          closeX, posLabelX, posX; };
    RightLayout computeRightLayout (int width) const;

    juce::String mouseText, playPart, fileName, ratePart, modeText;
    bool selVisible_ = false, posVisible_ = false, rightFits_ = true;
    double curStart = 0.0, curStop = 0.0, curPos = 0.0;

    // Time-axis scale context (set by MainWindow via setTimeContext).
    TimeAxisMode timeCtxMode = TimeAxisMode::Time;
    double timeCtxOffset = 0.0, timeCtxRate = 48000.0;

    juce::TextEditor startEdit, stopEdit, lenEdit, posEdit;

    JUCE_DECLARE_NON_COPYABLE (StatusBar)
};

// ------------------------------------------------- Python script panel

/** Top-group tab next to History. Lists the user's Python scripts (stored in
    %APPDATA%/SpectroCraft/python), lets you edit them and run one against the
    current selection (or the whole file when nothing is selected). The actual
    execution - temp files, interpreter launch, re-import - is orchestrated by
    MainWindow through onRunRequested. */
class PythonPanel : public juce::Component
{
public:
    explicit PythonPanel (Application& appIn,
                          const juce::File& folder = sc::py::scriptFolder());

    /** Rescans the script folder and reloads the current script's text. */
    void refreshScripts();

    /** Called by MainWindow to run `script` with exactly this editor text.
        Signature: (script file, script text). */
    std::function<void (const juce::File&, const juce::String&)> onRunRequested;

    // Right-click "Close Panel": set by MainWindow. requestClose() is the
    // testable entry point.
    std::function<void()> onCloseRequested;
    void requestClose() { if (onCloseRequested) onCloseRequested(); }

private:
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;   // right-click = Close Panel
    void loadCurrentScript();
    void showAddDialog();
    void confirmRemove();
    void requestRun();

    Application& app;
    juce::File scriptDir;
    std::vector<juce::File> scripts;
    int currentIdx = -1;

    juce::ComboBox scriptBox;
    juce::TextButton addBtn, removeBtn, runBtn;
    juce::TextEditor editor;

    JUCE_DECLARE_NON_COPYABLE (PythonPanel)
};
