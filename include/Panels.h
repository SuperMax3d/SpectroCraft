#pragma once
// SpectroCraft - Left-side panels
#include <JuceHeader.h>
#include "Application.h"

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

private:
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;

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

    /** Builds the "Set Unit and Scale" calibration dialog for one document.
        The caller owns and deletes the returned window (unit tests inspect it
        without running the modal loop). */
    juce::AlertWindow* createScaleDialog (FileDoc& d);

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

/** Snapshot overlay button (1..10).
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
    // Needed for the value-initialised snapBtns[10] array. Defined out-of-line
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

    // Called by MainWindow while playing: forces a recompute on state change
    // (during playback with no selection the panel tracks the cursor).
    void setFollowPlayhead (bool f);

    // Recompute the spectrum if the analysis region changed. Public so that
    // a single click in either view can refresh the panel immediately.
    void recomputeIfNeeded();

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

    // Local analysis parameters (unit tests + persistence wiring checks).
    int fftSize() const { return localN; }
    int axisMode() const { return axisMode_; }
    juce::ComboBox& fftSizeComboRef() { return fftSizeCombo; }
    juce::ComboBox& axisComboRef() { return axisCombo; }

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

    // Programmatic setting changes (same code path as the UI combos).
    void setFftSize (int n) { fftSizeCombo.setSelectedId (n, juce::sendNotificationSync); }
    void setWindowType (int t) { windowCombo.setSelectedId (t + 1, juce::sendNotificationSync); }

    // Accessors for unit tests.
    int getCachedN() const { return cachedN; }
    const std::vector<std::vector<float>>& getSpectrum() const { return mags; }
    int getAxisMode() const { return axisMode_; }
    const std::vector<std::vector<float>>& getThirdOctMags() const { return thirdOctMags; }

    // Local display ranges (independent of the spectrogram's settings):
    // frequency window [fLo_, fHi_] and level window [dbBottom_, dbTop_].
    double getFreqLo() const { return fLo_; }
    double getFreqHi() const { return fHi_; }
    double getDbTop() const { return dbTop_; }
    double getDbBottom() const { return dbBottom_; }

    // Reset both display ranges to their defaults ("Zoom Out Full").
    void resetDisplayRanges();
    // Called after a resample: re-clamp the frequency window to the new
    // Nyquist and force stored snapshots to be recomputed at the new rate.
    void onSampleRateChanged();

    // Cursor readout text for position (x, y). When a curve passes within a
    // few pixels of the cursor, snapped is set and snapDotX/Y hold that point.
    juce::String buildCursorInfo (int x, int y, bool& snapped,
                                  float& snapDotX, float& snapDotY) const;

    // Mapping helpers exposed for unit tests.
    float xToFreq (int x) const;
    double freqToX (double f) const;
    juce::Rectangle<int> plotBounds() const;
    float levelToY (double db) const;
    double yToLevel (float y) const;

    // Switch frequency-axis mode: 0 = "FFT Linear", 1 = "FFT Log",
    // 2 = "1/3 Octave". sendNotificationSync so the change is applied
    // immediately (JUCE's default notification is async via AsyncUpdater).
    void setAxisMode (int m) { axisCombo.setSelectedId (m + 1, juce::sendNotificationSync); }

    // Standard IEC 61260-1 1/3-octave centre frequencies (50 Hz .. 16 kHz).
    static const double* thirdOctCentres() noexcept;
    static int numThirdOctBands() noexcept;

private:
    Application& app;
    juce::ComboBox fftSizeCombo, windowCombo, axisCombo;
    SnapshotButton snapBtns[10];

    // Fixed normalization floor for the stored magnitudes: independent of the
    // visible level window so zooming out never changes the curve data.
    static constexpr double kNormFloorDb = 160.0;

    int localN = 2048;          // independent from the spectrogram's FFT size
    // Frequency-axis mode: 0 = "FFT Linear", 1 = "FFT Log" (default),
    // 2 = "1/3 Octave" (band plot, 50 Hz .. 16 kHz).
    int axisMode_ = 1;
    bool followPlayhead_ = false;

    // Local display ranges: the frequency window and the level window are
    // independent of app.settings.dBRange and of the spectrogram view.
    double fLo_ = 20.0, fHi_ = 20000.0;   // visible frequency range (Hz)
    double dbTop_ = 0.0, dbBottom_ = -80.0; // visible level window (dB rel.)

    // Interaction state for axis panning / box zooming.
    enum class DragMode { None, PanFreq, PanLevel, ZoomDrag };
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
    Snapshot snapshots[10];     // stored curve overlays, shared across all files

    // Cached spectrum data
    std::vector<std::vector<float>> mags;   // per channel, normalized 0..1 vs kNormFloorDb
    // Per-channel 1/3-octave band values (normalized 0..1), one entry per
    // band with centre <= Nyquist. Only populated when axisMode_ == 2.
    std::vector<std::vector<float>> thirdOctMags;
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

    // Bumped whenever the FFT size or window type changes: stored snapshots
    // remember the generation they were computed with and are recomputed from
    // their own time segment when it no longer matches.
    int settingsGen = 0;

    int axisWidth() const;   // right level-axis width (wider for linear V/g labels)
    void copySpectrumToClipboard();

    // Zoom helpers for the local display ranges.
    void zoomFreqAtX (int x, double factor);
    void zoomLevelAtY (float y, double factor);
    void showContextMenu (bool onLevelAxis, bool onFreqAxis);

    // Core averaging computation: analyse region [t0, t1] with windows of n
    // samples at the current window type; one magnitude vector per channel in
    // chans. Shared by the live spectrum and every stored snapshot (each from
    // its own source file's engine).
    void computeRegionFrom (AudioEngine& eng, const std::vector<int>& chans,
                            double t0, double t1, int n,
                            std::vector<std::vector<float>>& outMags);
    // Live-analysis convenience wrapper: uses the active document.
    void computeRegion (double t0, double t1, int n,
                        std::vector<std::vector<float>>& outMags)
    {
        computeRegionFrom (app.audioEngine(), app.visibleChannels, t0, t1, n, outMags);
    }
    // Recompute all valid snapshots from their stored time segments using the
    // current FFT size / window type, then refresh their 1/3-octave bands.
    void recomputeSnapshots();
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

    // Programmatic row selection (normally done via the table UI).
    void selectRow (int row);

private:
    void mouseDown (const juce::MouseEvent&) override;
    void showMarkerContextMenu();

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
        void cellDoubleClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;
        void selectedRowsChanged (int row) override;
        void deleteKeyPressed (int row) override;

    private:
        MarkerListPanel* owner;
    };

    Model model { this };
    juce::TableListBox table { "", &model };

    juce::TextButton addBtn, deleteBtn;
    juce::TextEditor beginEdit, endEdit, lengthEdit, titleEdit, commentEdit;
    int selectedRow = -1;
    bool editing = false; // guard against feedback loops in text editors

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
    an SCTT RIFF chunk — they never appear in the Markers panel. */
// TextEditor with an Enter callback: this JUCE build's TextEditor has no
// onReturnPressed / placeholder members, so the panel hooks key presses here.
class SearchEditor : public juce::TextEditor
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

protected:
    // Paints a dim italic "Search" placeholder while the box is empty and
    // unfocused; it disappears as soon as the user clicks into the box.
    void paint (juce::Graphics& g) override
    {
        TextEditor::paint (g);
        if (getText ().isEmpty () && !hasKeyboardFocus (false))
        {
            juce::Font f (12.0f, juce::Font::italic);
            g.setFont (f);
            g.setColour (juce::Colour::fromRGB (135, 140, 152));
            g.drawText ("Search", getLocalBounds ().reduced (8, 3),
                        juce::Justification::centredLeft, false);
        }
    }

    void focusGained (juce::Component::FocusChangeType) override { repaint (); }
    void focusLost (juce::Component::FocusChangeType) override { repaint (); }
};

class TranscriptionPanel : public juce::Component
{
public:
    explicit TranscriptionPanel (Application& app);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void refresh();   // call after transcription changes / file open/close

    // Case-insensitive substring search over the segment texts. Selects and
    // scrolls to the next match after the current one (wrapping around),
    // jumps the playhead there like a double-click, and returns false when no
    // segment matches at all.
    bool findNext();

    // Creates a marker spanning the segment's time range (point marker when
    // it has no length), titled with its text; the full text goes into the
    // marker comment. Persists the sidecar and refreshes all panels/views.
    void addMarkerForRow (int rowNumber);

    // Set by MainWindow: opens the model/language dialog and starts the run.
    std::function<void()> onRunRequested;

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

    Model model { this };
    juce::TableListBox table { "", &model };

    juce::TextButton runBtn, clearBtn;
    SearchEditor searchBox;       // Enter jumps to the next match
    int searchAnchorRow = -1;     // row of the last found match (-1 = none)
    juce::String lastSearchNeedle;// resets the anchor when the query changes

    JUCE_DECLARE_NON_COPYABLE (TranscriptionPanel)
};
#endif // SC_ENABLE_AI

// ------------------------------------------------- Selection panel

/** Named spectral selections (store / recall) + "Clone & Multiply" for
    picking out harmonics of the current selection.

    - The table lists this file's saved selections; double-click recalls one
      (it replaces the current spectral selection). Save stores the CURRENT
      selection under a name (same name = update), Delete removes an entry.
    - "Save to disk" keeps the list in sync with "<file>.selections.json"
      next to the audio file, so it survives restarts.
    - Clone & Multiply appends additive clones of the current selection at
      user-given frequency multipliers (e.g. 0.5, 2, 5); clone thickness is
      stretched by the same factor. The interactive "Pick points..." mode
      does the same with clicks: first click = reference frequency, each
      following click adds clones at (clicked freq / ref freq). */
class SelectionPanel : public juce::Component
{
public:
    explicit SelectionPanel (Application& app);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void refresh();   // call after doc changes / selection or list changes

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
        void cellDoubleClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;

    private:
        SelectionPanel* owner;
    };

    Model model { this };
    juce::TableListBox table { "", &model };

    juce::TextButton saveBtn, recallBtn, deleteBtn, diskToggle, applyMultBtn, pickBtn;
    juce::Label savedLab, cloneLab, multHintLab;
    juce::TextEditor multEdit;

    void doSave();
    void doRecall (int row);
    void doDelete();
    void doApplyMultipliers();
    void toggleDisk();
    void togglePick();

    JUCE_DECLARE_NON_COPYABLE (SelectionPanel)
};

// ---------------------------------------------------------------- Status bar

// Thin strip at the bottom of the window: active file name + mouse probe info
// on the left, playhead and selection times on the right.
class StatusBar : public juce::Component
{
public:
    // MSVC 2019 does not generate an implicit default constructor when a
    // deleted copy constructor is declared, so declare it explicitly.
    StatusBar() = default;

    void paint (juce::Graphics& g) override;

    // Left side: free-form probe text from whichever view the cursor is over
    // (empty string clears it).
    void setMouseInfo (const juce::String& s)
    {
        if (s != mouseText) { mouseText = s; repaint(); }
    }
    // Right side: selection range and playhead position.
    void setSelection (double t0, double t1);
    void setPlayhead (double t);
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

private:
    juce::String mouseText, selPart, playPart, fileName, ratePart, modeText;

    JUCE_DECLARE_NON_COPYABLE (StatusBar)
};
