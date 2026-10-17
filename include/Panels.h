#pragma once
// SpectroCraft - Left-side panels
#include <JuceHeader.h>
#include "Application.h"

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
    }

private:
    JUCE_DECLARE_NON_COPYABLE (ViewSplitter)
};

// ---------------------------------------------------------------- Files panel

class FileListPanel : public juce::Component
{
public:
    explicit FileListPanel (Application& app);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void refresh(); // call after file open/close

    // Set by MainWindow to trigger the Open dialog
    std::function<void()> onOpenRequested;

private:
    Application& app;
    juce::TextButton openBtn, closeBtn;
    std::vector<juce::String> fileInfoLines;

    JUCE_DECLARE_NON_COPYABLE (FileListPanel)
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

    void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                      bool /*shouldDrawButtonAsDown*/) override;

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
    void mouseDown (const juce::MouseEvent& e) override; // right-click = context menu

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

    // Stored-snapshot state (unit tests).
    double snapshotT0 (int i) const { return snapshots[i].t0; }
    double snapshotT1 (int i) const { return snapshots[i].t1; }
    int snapshotN (int i) const { return snapshots[i].n; }
    bool snapshotShown (int i) const { return snapshots[i].shown; }
    const std::vector<std::vector<float>>& snapshotMags (int i) const { return snapshots[i].mags; }

    // Programmatic setting changes (same code path as the UI combos).
    void setFftSize (int n) { fftSizeCombo.setSelectedId (n, juce::sendNotificationSync); }
    void setWindowType (int t) { windowCombo.setSelectedId (t + 1, juce::sendNotificationSync); }

    // Accessors for unit tests.
    int getCachedN() const { return cachedN; }
    const std::vector<std::vector<float>>& getSpectrum() const { return mags; }
    int getAxisMode() const { return axisMode_; }
    const std::vector<std::vector<float>>& getThirdOctMags() const { return thirdOctMags; }

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

    int localN = 2048;          // independent from the spectrogram's FFT size
    // Frequency-axis mode: 0 = "FFT Linear", 1 = "FFT Log" (default),
    // 2 = "1/3 Octave" (band plot, 50 Hz .. 16 kHz).
    int axisMode_ = 1;
    bool followPlayhead_ = false;

    struct Snapshot
    {
        double t0 = 0, t1 = 0;   // time segment of the file that was analysed
        int n = 0;               // FFT size used for the last computation
        int gen = -1;            // settings generation (FFT size + window type)
        bool shown = false;
        std::vector<std::vector<float>> mags;   // per channel, dB-normalized 0..1
        std::vector<std::vector<float>> bands;  // 1/3-octave conversion (per channel)
    };
    Snapshot snapshots[10];     // stored curve overlays (buttons 1..10)

    // Cached spectrum data
    std::vector<std::vector<float>> mags;   // per channel, dB-normalized 0..1
    // Per-channel 1/3-octave band values (dB-normalized 0..1), one entry per
    // band with centre <= Nyquist. Only populated when axisMode_ == 2.
    std::vector<std::vector<float>> thirdOctMags;
    int cachedN = 0;
    double cachedT0 = -1, cachedT1 = -1;
    float cachedDB = -1;         // dB range used for the last computation
    juce::File cachedFile;
    int64_t lastComputeMs = 0;
    bool lastComputeWasExpensive = false;   // multi-window averaging (throttle key)
    int cursorX = -1, cursorY = -1;

    // Bumped whenever the FFT size or window type changes: stored snapshots
    // remember the generation they were computed with and are recomputed from
    // their own time segment when it no longer matches.
    int settingsGen = 0;

    float xToFreq (int x) const;
    double freqToX (double f) const;
    juce::Rectangle<int> plotBounds() const;
    void copySpectrumToClipboard();

    // Core averaging computation: analyse region [t0, t1] with windows of n
    // samples at the current window type; one magnitude vector per visible
    // channel. Shared by the live spectrum and every stored snapshot.
    void computeRegion (double t0, double t1, int n,
                        std::vector<std::vector<float>>& outMags);
    // Recompute all valid snapshots from their stored time segments using the
    // current FFT size / window type, then refresh their 1/3-octave bands.
    void recomputeSnapshots();
    // Re-derive the 1/3-octave band values of every snapshot (cheap: no FFT).
    void refreshSnapshotBands();
    // Refresh button i's tooltip to describe its current stored state.
    void updateSnapshotTooltip (int i);

    JUCE_DECLARE_NON_COPYABLE (FFTAnalysisView)
};

// ---------------------------------------------------- Spectral Controls panel

class SpectralControlsPanel : public juce::Component
{
public:
    explicit SpectralControlsPanel (Application& app);
    void paint (juce::Graphics& g) override;
    void resized() override;
    void refresh(); // call after file open/close or settings change

    // Set by MainWindow: resets the spectral view's frequency axis.
    std::function<void()> onResetFreq;

private:
    Application& app;
    juce::ComboBox colourMapCombo, fftSizeCombo;
    juce::Slider dBRangeSlider;
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
            for (int x = 0; x < b.getWidth(); ++x)
            {
                const float t = (float) x / std::max (1, b.getWidth() - 1);
                const auto lut = buildColourLUT (app.colourMap);
                const int i = (int) (t * 255.0f + 0.5f);
                g.setColour (juce::Colour ((int) lut[i * 4], (int) lut[i * 4 + 1], (int) lut[i * 4 + 2]));
                g.fillRect (x, b.getY(), 1, b.getHeight());
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

// ---------------------------------------------------------------- Status bar

// Thin strip at the bottom of the window: mouse probe info on the left, playhead
// and selection times on the right.
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

private:
    juce::String mouseText, selPart, playPart;

    JUCE_DECLARE_NON_COPYABLE (StatusBar)
};
