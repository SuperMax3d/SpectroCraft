/**
 * SpectroCraft - Main Window Implementation
 * Layout: menu bar on top; left column with two tab groups
 *   (top: Files | Frequency Analysis, bottom: Spectral Controls | Markers);
 * main area: waveform view on top, spectral view below.
 */

#include "MainWindow.h"
#include "Application.h"
#include "StartupTrace.h"
#include <thread>
#include "WaveformView.h"
#include "SpectralView.h"
#include "Panels.h"
#include "MarkerManager.h"
#include "VideoAudio.h"
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    #include "AI/ModelManager.h"
    #include "AI/OvRuntime.h"
    #include "NoiseSuppression.h"
#endif
#include "IconData.h"
#include "Version.h"   // auto-generated: SC_VERSION_STRING (bumped on every build)
#include <algorithm>

// Dark tab background matching the panel backgrounds (Panels.cpp).
static const juce::Colour kDarkTabBg (45, 45, 48);

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// The icon is embedded at runtime rather than via a .rc resource: the rc.exe in
// this toolchain emits ICON statements as RT_CURSOR entries, which Windows never
// uses for module/window icons. CreateIconFromResourceEx + WM_SETICON works
// reliably on any machine.
static void applyWindowIcon (void* hwndPtr)
{
#if JUCE_WINDOWS
    HWND hwnd = static_cast<HWND> (hwndPtr);
    if (hwnd == nullptr) return;

    unsigned char* p = const_cast<unsigned char*> (scapp::kAppIconIcoData);
    const unsigned int totalSize = scapp::kAppIconIcoSize;
    if (totalSize < 6u + 16u) return;

    unsigned short count = 0;
    std::memcpy (&count, p + 4, sizeof (count));
    if (count == 0 || totalSize < 6u + 16u * static_cast<unsigned int> (count)) return;

    struct IcoEntry { int w, h, size, offset; };
    std::vector<IcoEntry> entries;
    for (unsigned short i = 0; i < count; ++i)
    {
        const unsigned char* e = p + 6u + static_cast<unsigned int> (i) * 16u;
        IcoEntry en {};
        en.w = e[0]; if (en.w == 0) en.w = 256;   // 0 means 256
        en.h = e[1]; if (en.h == 0) en.h = 256;
        std::memcpy (&en.size,   e + 8, sizeof (int));
        std::memcpy (&en.offset, e + 12, sizeof (int));
        entries.push_back (en);
    }

    auto pickClosest = [&entries] (int want) -> const IcoEntry*
    {
        const IcoEntry* best = nullptr;
        int bestDiff = 1 << 30;
        for (const auto& e : entries)
        {
            int diff = std::abs (e.w - want);
            if (diff < bestDiff) { bestDiff = diff; best = &e; }
        }
        return best;
    };

    if (const IcoEntry* big = pickClosest (32))
    {
        HICON h = CreateIconFromResourceEx (p + big->offset,
                                             static_cast<DWORD> (big->size), TRUE,
                                             0x00030000, big->w, big->h, 0);
        if (h != nullptr) SendMessageW (hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM> (h));
    }
    // NB: 'small' is a macro in rpcndr.h (#define small char) - avoid it.
    if (const IcoEntry* entrySmall = pickClosest (16))
    {
        HICON h = CreateIconFromResourceEx (p + entrySmall->offset,
                                             static_cast<DWORD> (entrySmall->size), TRUE,
                                             0x00030000, entrySmall->w, entrySmall->h, 0);
        if (h != nullptr) SendMessageW (hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM> (h));
    }
#else
    juce::ignoreUnused (hwndPtr);
#endif
}

// The window background shows through in the small gaps around the time
// scrollbar (left of it, right of it, and the 2 px strip above it). Use the
// same neutral dark grey as the views' axis backgrounds instead of JUCE's
// default bluish scheme colour so those areas blend with the tracks.
static juce::Colour mainWindowBackground () { return juce::Colour::fromRGB (32, 32, 36); }

MainWindow::MainWindow (Application& appIn, const juce::String& name)
    : DocumentWindow (name,
                      mainWindowBackground (),
                      DocumentWindow::allButtons),
      app (appIn),
      tabDragCtx (),
      topTabs (juce::TabbedButtonBar::TabsAtTop, &tabDragCtx),
      bottomTabs (juce::TabbedButtonBar::TabsAtTop, &tabDragCtx)
{
    setUsingNativeTitleBar (true);
    setResizable (true, true);
    setResizeLimits (900, 600, 10000, 10000);

    // Register ourselves as a key listener so the keyboard shortcuts in
    // keyPressed() actually fire. Inheriting juce::KeyListener alone is not
    // enough: JUCE only calls listeners registered on components along the
    // focus chain (the virtual one-arg Component::keyPressed is what gets
    // called otherwise, and we don't override that).
    addKeyListener (this);

    // Be keyboard-focusable so clicking neutral areas (splitters, bars, empty
    // space) lands focus here instead of leaving it on a previously clicked
    // widget - the arrow keys then keep controlling the playhead/selection.
    setWantsKeyboardFocus (true);

    menuBar.setModel (this);
    // DocumentWindow inherits ResizableWindow, whose addAndMakeVisible() asserts;
    // our layout adds children directly to the window and positions them in
    // resized(), so call the base Component implementation explicitly.
    juce::Component::addAndMakeVisible (&menuBar);

    // Left-side panels in two tab groups
    fileListPanel = std::make_unique<FileListPanel> (app);
    fftPanel      = std::make_unique<FFTAnalysisView> (app);
    controlsPanel = std::make_unique<SpectralControlsPanel> (app);
    markersPanel  = std::make_unique<MarkerListPanel> (app);
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    transcriptionPanel = std::make_unique<TranscriptionPanel> (app);
#endif
    selectionPanel = std::make_unique<SelectionPanel> (app);
    drawingPanel   = std::make_unique<DrawingPanel> (app);
    historyPanel  = std::make_unique<HistoryPanel> (app);
    pythonPanel   = std::make_unique<PythonPanel> (app);

    fileListPanel->onOpenRequested = [this] { handleFileOpen(); };

    // Clicking a history row jumps to that state: undo or redo as many steps
    // as needed in one batched operation, then refresh everything.
    historyPanel->onJumpToState = [this] (int target) {
        const int current = (int) app.undo().undoSize();
        if (target < 0 || target > current + (int) app.undo().redoSize()) return;
        if (target < current)
            app.undoSteps (current - target);
        else if (target > current)
            app.redoSteps (target - current);
        refreshAfterEdit();
        historyPanel->refresh();
    };

    topTabs.addTab ("Files", kDarkTabBg, fileListPanel.get(), false);
    topTabs.addTab ("Frequency Analysis", kDarkTabBg, fftPanel.get(), false);
    topTabs.addTab ("History", kDarkTabBg, historyPanel.get(), false);
    topTabs.addTab ("Python", kDarkTabBg, pythonPanel.get(), false);
    bottomTabs.addTab ("Spectral Controls", kDarkTabBg, controlsPanel.get(), false);
    bottomTabs.addTab ("Markers", kDarkTabBg, markersPanel.get(), false);
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    bottomTabs.addTab ("Transcription", kDarkTabBg, transcriptionPanel.get(), false);
    if (transcriptionPanel)
        transcriptionPanel->onRunRequested = [this] { showTranscribeDialog (); };
#endif
    pythonPanel->onRunRequested = [this] (const juce::File& f, const juce::String& t)
                                  { runPythonScriptAsync (f, t); };
    bottomTabs.addTab ("Selection", kDarkTabBg, selectionPanel.get(), false);
    bottomTabs.addTab ("Drawing", kDarkTabBg, drawingPanel.get(), false);
    // Frequency Analysis is the default tab in the top group.
    topTabs.setCurrentTabIndex (1, false);
    bottomTabs.setCurrentTabIndex (0, false);

    // Drag-and-drop reordering of the panel tabs: within a group or across
    // groups. The arrangement is persisted by saveLeftLayout().
    tabDragCtx.bars = { &topTabs, &bottomTabs };
    tabDragCtx.onMove = [this] (juce::TabbedComponent* from, int fi,
                                juce::TabbedComponent* to, int ii)
                        { moveLeftPanelTab (from, fi, to, ii); };

    // Right-click "Close Panel" in each left-column panel (the Window menu
    // offers the same toggles for every panel).
    fileListPanel->onCloseRequested   = [this] { setPanelVisible ("files", false); };
    fftPanel->onCloseRequested        = [this] { setPanelVisible ("fft", false); };
    historyPanel->onCloseRequested    = [this] { setPanelVisible ("history", false); };
    pythonPanel->onCloseRequested     = [this] { setPanelVisible ("python", false); };
    controlsPanel->onCloseRequested   = [this] { setPanelVisible ("controls", false); };
    markersPanel->onCloseRequested    = [this] { setPanelVisible ("markers", false); };
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    if (transcriptionPanel)
        transcriptionPanel->onCloseRequested =
            [this] { setPanelVisible ("transcription", false); };
#endif
    selectionPanel->onCloseRequested  = [this] { setPanelVisible ("selection", false); };
    drawingPanel->onCloseRequested    = [this] { setPanelVisible ("drawing", false); };

    // Restore the user's saved arrangement + column geometry.
    applySavedLeftLayout();
    leftPanelW = std::clamp ((int) app.recentStore->getIntValue ("left_panel_w", leftPanelW), 220, 4000);
    topRatio   = std::clamp (app.recentStore->getIntValue ("top_ratio_mil",
                                                          (int) std::lround (topRatio * 1000.0)) / 1000.0,
                             0.15, 0.85);
    leftPanelsVisible = app.recentStore->getIntValue ("left_panels_visible", 1) != 0;

    juce::Component::addAndMakeVisible (&topTabs);
    juce::Component::addAndMakeVisible (&bottomTabs);

    splitter = std::make_unique<ViewSplitter>();
    splitter->onDrag = [this] (const juce::MouseEvent& e) { handleSplitterDrag (e); };
    splitter->setTooltip ("Drag to adjust the height of the waveform and spectrogram views.");
    juce::Component::addAndMakeVisible (splitter.get());

    // Draggable dividers: vertical one between the left panel column and the
    // main area, horizontal one between the two tab groups.
    leftSplitter = std::make_unique<ViewSplitter>();
    leftSplitter->vertical = true;
    leftSplitter->onDrag = [this] (const juce::MouseEvent& e) { handleLeftSplitterDrag (e); };
    leftSplitter->onRelease = [this] () { saveLeftLayout (); };
    leftSplitter->setTooltip ("Drag to adjust the width of the left panel column.");
    topSplitter = std::make_unique<ViewSplitter>();
    topSplitter->onDrag = [this] (const juce::MouseEvent& e) { handleTopSplitterDrag (e); };
    topSplitter->onRelease = [this] () { saveLeftLayout (); };
    topSplitter->setTooltip ("Drag to adjust the height of the top tab group.");
    juce::Component::addAndMakeVisible (leftSplitter.get());
    juce::Component::addAndMakeVisible (topSplitter.get());

    // Zoom/pan scrollbars: time bar at the bottom.
    timeBar     = std::make_unique<TimeScrollBar>();
    waveVScroll = std::make_unique<ValueScrollBar>();
    specVScroll = std::make_unique<ValueScrollBar>();

    timeBar->setTooltip ("Time zoom/pan bar: drag the thumb to pan, drag an edge to zoom, "
                         "click outside the thumb to jump there.");
    waveVScroll->setTooltip ("Visible amplitude range of the waveform view: drag the thumb to pan, "
                             "drag an edge to zoom, click outside the thumb to jump.");
    specVScroll->setTooltip ("Visible frequency range of the spectrogram: drag the thumb to pan, "
                             "drag an edge to zoom, click outside the thumb to jump.");

    timeBar->onChange = [this] (double t0, double t1) { applyTimeWindow (t0, t1); };
    waveVScroll->onChange = [this] (double lo, double hi) {
        if (waveformView && hi > lo)
            waveformView->setVerticalZoom (2.0 / (0.9 * (hi - lo)));
    };
    specVScroll->onChange = [this] (double lo, double hi) {
        if (spectralView && hi > lo) spectralView->setFrequencyRange (lo, hi);
    };

    // Status bar at the very bottom of the window.
    statusBar = std::make_unique<StatusBar>();
    juce::Component::addAndMakeVisible (statusBar.get());

    // One tooltip window for the whole app: it watches for mouse hovers and
    // shows the tip of whichever TooltipClient component is under the cursor.
    tooltipWindow = std::make_unique<juce::TooltipWindow> (nullptr, 600);

    juce::Component::addAndMakeVisible (timeBar.get());
    juce::Component::addAndMakeVisible (waveVScroll.get());
    juce::Component::addAndMakeVisible (specVScroll.get());

    lastSyncedColourMap = app.colourMap;
    lastSyncedSettings  = app.settings;   // baseline for the settings-save trigger

    scStart::mark ("before-setup-views");
    setupViews();
    scStart::mark ("after-setup-views");
    updateTitle();   // a file may already be loaded (opened from the CLI)
    if (statusBar)   // ...and the footer must show its name too
        statusBar->setFileName (app.audioEngine().isFileLoaded()
                                ? app.audioEngine().getFile().getFileName()
                                : juce::String());

    // Panels notify us when they change shared state
    app.onStateChange = [this] { syncSharedState(); };

    // The footer's editable selection fields push new start/stop values back
    // into the shared selection (Application clamps them to the file).
    if (statusBar)
        statusBar->onSelectionEdited = [this] (double a, double b)
        { app.setSelection (juce::Range<double> (a, b)); };

    // The footer's playhead field pushes the typed position back into the
    // shared state (Application clamps it to the file and notifies the views).
    if (statusBar)
        statusBar->onPositionEdited = [this] (double t) { app.setPlayhead (t); };

    // Panels can ask for a time range to be shown in both views (e.g.
    // double-clicking a marker or a stored selection).
    app.onSetTimeWindow = [this] (double t0, double t1) { applyTimeWindow (t0, t1); };

    // Drawing panel <-> spectrogram wiring: arming a new element from the
    // panel's context menu, refreshing after any list change, and fitting
    // the spectral frequency axis when a drawing is recalled.
    app.onArmNewDrawing = [this] (DrawingItem::Kind k)
    { if (spectralView) spectralView->startPendingDraw (k); };
    app.onDrawingsChanged = [this]
    {
        if (drawingPanel) drawingPanel->refresh();
        if (spectralView) spectralView->repaint();
    };
    app.onSetSpectralFreqRange = [this] (double lo, double hi)
    { if (spectralView) spectralView->setFrequencyRange (lo, hi); };

    app.getSpectralFreqRange = [this] () -> std::pair<double, double>
    { return spectralView != nullptr ? std::make_pair (spectralView->getFreqMin (),
                                                     spectralView->getFreqMax ())
                                     : std::make_pair (0.0, 0.0); };

    // Keep the playhead in both views identical whenever it moves (clicks,
    // Home/End, file load). The 30 Hz timer already pushes playback position to
    // both views directly.
    app.onPlayheadChanged = [this] (double t)
    {
        if (waveformView) waveformView->setPlayhead (t);
        if (spectralView) spectralView->setPlayhead (t);
        // A single click in either view re-analyses the spectrum: centred on
        // the cursor when there is no selection, averaged over the whole
        // selection when one exists.
        if (fftPanel) { fftPanel->recomputeIfNeeded(); fftPanel->repaint(); }
        if (statusBar) statusBar->setPlayhead (t);
    };

    // Multi-file support: whenever a document becomes active (open, switch,
    // close), reset both views to that file and push its state into every panel.
    app.onActiveDocChanged = [this] { activateDocUI(); };

    // When a file is closed, drop the stored snapshots that came from it.
    app.onFileClosed = [this] (const FileDoc& d)
    {
        if (fftPanel) fftPanel->clearSnapshotsFor (&d);
    };

    // Deleting a time range also shifts / trims / drops the FFT snapshot slots
    // stored from that file; record their before/after state into the patch.
    app.onTimeRangeDeleted = [this] (FileDoc* d, double a, double b, Patch& p)
    {
        if (fftPanel) fftPanel->recordAndAdjustSnapshotsForDeletion (d, a, b, p);
    };

    // Undo/redo of such a patch restores the snapshot slots too.
    app.onPatchSideStateRestored = [this] (FileDoc* d, const Patch& p, bool isRedo)
    {
        if (fftPanel) fftPanel->restoreSnapshotSlots (d, p, isRedo);
    };

    startTimerHz (30); // playhead animation
    centreWithSize (1400, 900);

    // Keep the whole window inside a display's user area so the title bar is
    // always reachable: centring a 1400x900 window on a small screen can put
    // its top edge off-screen.
    {
        const auto& ds = juce::Desktop::getInstance().getDisplays();
        const juce::Displays::Display* mainD = nullptr;
        for (const auto& d : ds.displays)
            if (d.isMain) { mainD = &d; break; }
        if (mainD == nullptr && !ds.displays.isEmpty())
            mainD = &ds.displays.getReference (0);

        if (mainD != nullptr && !mainD->userBounds.isEmpty())
        {
            juce::Rectangle<int> safe ((int) std::lround (mainD->userBounds.getX()),
                                      (int) std::lround (mainD->userBounds.getY()),
                                      (int) std::lround (mainD->userBounds.getWidth()),
                                      (int) std::lround (mainD->userBounds.getHeight()));

            // JUCE window bounds are the CLIENT area: setBoundsPhysical() adds
            // the native frame (title bar + resize borders) around them. If we
            // only fit the client into userBounds, the title bar ends up above
            // y=0 and is unreachable. Inset by the frame size so the whole
            // window - decorations included - stays inside the display.
#if defined (_WIN32)
            {
                RECT cr { 0, 0, 1, 1 };
                const DWORD style = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME
                                    | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
                if (AdjustWindowRectExForDpi (&cr, style, FALSE, 0,
                                              (UINT) std::lround (mainD->dpi)))
                {
                    const double sc = mainD->scale > 0.0 ? mainD->scale : 1.0;
                    // Frame insets in logical pixels (asymmetric: the title
                    // bar makes the top inset much larger than the others).
                    const int il = (int) std::ceil (-cr.left / sc);
                    const int it = (int) std::ceil (-cr.top / sc);
                    const int ir = (int) std::ceil (cr.right / sc);
                    const int ib = (int) std::ceil (cr.bottom / sc);
                    safe.setX (safe.getX() + il);
                    safe.setY (safe.getY() + it);
                    safe.setWidth (std::max (0, safe.getWidth() - il - ir));
                    safe.setHeight (std::max (0, safe.getHeight() - it - ib));
                }
            }
#endif

            if (!safe.isEmpty())
            {
            auto r = getBounds();
            if (r.getWidth() > safe.getWidth())
                r.setWidth (std::max (400, safe.getWidth()));
            if (r.getHeight() > safe.getHeight())
                r.setHeight (std::max (300, safe.getHeight()));
            r = r.withCentre (safe.getCentre());
            // If it still overflows (tiny screen), pin the top-left so the
            // title bar stays visible and resizable.
            if (r.getX() < safe.getX()) r.setX (safe.getX());
            if (r.getY() < safe.getY()) r.setY (safe.getY());
            setBounds (r);
            }
        }
    }

    scStart::mark ("end-of-MainWindow-ctor");
    // Proxy for "window visible": the first paint happens shortly after the
    // window is shown, so a short delay gives a stable end-to-end number.
    if (scStart::isEnabled())
        juce::Timer::callAfterDelay (150, [] {
            const int64 ms = scStart::mark ("window-shown(+150ms)");
            scStart::note (juce::String ("total launch->window: T+") + juce::String (ms) + " ms");
        });

    setVisible (true);
}

// Modal progress dialog: a message plus a progress bar below it. Used for
// long-running background work (loading, saving, exporting) so the UI stays
// responsive and Windows never shows its own wait cursor over our windows.
class WorkProgressDialog : public juce::AlertWindow
{
public:
    // withCancel adds a Cancel button that sets `cancelled` and exits the
    // modal loop. The worker thread must keep owning the dialog's lifetime:
    // it checks `cancelled` between units of work and deletes the dialog.
    WorkProgressDialog (const juce::String& title, const juce::String& message,
                        bool withCancel = false, Component* centreAround = nullptr)
        : juce::AlertWindow (title, message, juce::AlertWindow::NoIcon, centreAround)
    {
        // Use the AlertWindow's own progress bar so its layout code accounts
        // for it: when setStatusText() changes the message, updateLayout()
        // repositions text, bar and buttons together. A manually added child
        // bar plus a setSize() hack breaks that invariant - on the first
        // status change the Cancel button drops down onto the bar strip.
        addProgressBarComponent (progressValue);

        if (withCancel)
            addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

        for (auto* c : getChildren())
        {
            if (auto* pb = dynamic_cast<juce::ProgressBar*> (c))
            {
                // The default LookAndFeel tables define no ProgressBar colours,
                // which resolve to transparent - set them explicitly so the bar
                // is visible on the dark dialog background.
                pb->setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff232327));
                pb->setColour (juce::ProgressBar::foregroundColourId, juce::Colours::dodgerblue);

                // The bar's self-polling timer only starts when visibility
                // *changes*; toggle it once so updates are picked up from the start.
                pb->setVisible (false);
                pb->setVisible (true);
            }
        }

        if (withCancel)
        {
            for (auto* c : getChildren())
                if (auto* b = dynamic_cast<juce::TextButton*> (c))
                {
                    // The AlertWindow's own click listener exits the modal state
                    // when the button is pressed; this flag tells the worker
                    // thread to stop.
                    if (b->getButtonText () == "Cancel")
                        b->onClick = [this] { cancelled.store (true); };
                }
        }
    }

    // Set by the Cancel button / ESC; the worker thread checks it between
    // units of work and then deletes this dialog.
    std::atomic<bool> cancelled { false };

    void setProgress (double v) { progressValue = v; }

    // Update the message text (call on the message thread).
    void setStatusText (const juce::String& s) { setMessage (s); }

    /** Enters modal state and grabs keyboard focus so that ESC reaches the
        Cancel button's shortcut (AlertWindow::keyPressed checks its buttons'
        registered shortcuts). Progress dialogs contain no text fields, so
        taking focus is safe. The worker thread still owns this dialog's
        lifetime: after a cancel it finishes the current unit of work and
        deletes the dialog. */
    void beginModal()
    {
        enterModalState (false);
        setWantsKeyboardFocus (true);   // AlertWindow disables this when it has children
        grabKeyboardFocus ();
    }

private:
    double progressValue = 0.0;
};

MainWindow::~MainWindow()
{
    if (audioInitDialog != nullptr)
    {
        auto* dlg = audioInitDialog;   // deleting exits the modal state
        audioInitDialog = nullptr;
        delete dlg;
    }
}

void MainWindow::setupViews()
{
#if defined (_WIN32)
#endif
    waveformView = std::make_unique<WaveformView> (app);
    spectralView = std::make_unique<SpectralView> (app);

    juce::Component::addAndMakeVisible (waveformView.get());
    juce::Component::addAndMakeVisible (spectralView.get());

    // Both views always show the SAME time window: when either one changes its
    // range (zoom/pan/fit), applyTimeWindow() pushes it to both views and keeps
    // the canonical app.viewStart/viewEnd in sync (used by the Frequency
    // Analysis panel). Vertical zoom levels stay independent per view.
    auto onViewChanged = [this] (double t0, double t1) { applyTimeWindow (t0, t1); };
    waveformView->onChange = onViewChanged;
    spectralView->onChange = onViewChanged;

    // Keep the vertical scrollbars in sync with each view's zoom level.
    waveformView->onVerticalZoomChanged = [this] { updateScrollBars(); };
    spectralView->onFrequencyRangeChanged = [this] (double, double) { updateScrollBars(); };

    // Right-click context menus in either view route to the same actions.
    waveformView->onContextAction = [this] (int id) { handleContextMenuAction (id); };
    spectralView->onContextAction = [this] (int id) { handleContextMenuAction (id); };

    // Armed scale calibration: once the reference point is placed in the
    // spectrogram, prompt for its value and apply the resulting scale.
    spectralView->onRefClickPlaced = [this] (double t, double f)
    {
        handleRefClickPlaced (t, f);
    };

    // Placing a new text drawing prompts for its content, then commits it.
    spectralView->onNewTextPlaced = [this] (double t, double f, int px, int py)
    { showNewTextDialog (t, f, px, py); };

    // Arrow / Rectangle / Freehand commit once the drag ends with a usable size.
    spectralView->onDrawingCreated = [this] (DrawingItem item)
    { app.addDrawing (item); };

    // Cursor probes feed the status bar: time + amplitude over the waveform,
    // time + frequency + level over the spectrogram.
    auto onCursorInfo = [this] (const juce::String& s)
    {
        if (statusBar) statusBar->setMouseInfo (s);
    };
    waveformView->onCursorInfo = onCursorInfo;
    spectralView->onCursorInfo = onCursorInfo;

    controlsPanel->onResetFreq = [this] {
        if (spectralView) { spectralView->resetFrequencyRange(); spectralView->repaint(); }
    };

    // Reset both views to the whole file with default zoom levels.
    const double dur = app.audioEngine().duration();
    app.playhead = 0.0;
    if (waveformView) waveformView->fitAll();
    if (spectralView)
    {
        spectralView->fitAll();
        spectralView->resetFrequencyRange();
    }

    syncSharedState();
}

// Apply a time window to BOTH views (they always show the same range) and keep
// the canonical app.viewStart/viewEnd in sync for the Frequency Analysis panel.
void MainWindow::applyTimeWindow (double t0, double t1)
{
    const double dur = app.audioEngine().duration();
    if (dur > 0.0)
    {
        t0 = std::clamp (t0, 0.0, dur);
        t1 = std::clamp (t1, t0, dur);
    }

    app.viewStart = t0;
    app.viewEnd = t1;

    // No-op on the view that already has this range; the other view's change
    // fires its own onChange -> applyTimeWindow again with identical values,
    // which terminates immediately.
    if (waveformView) waveformView->setViewRange (t0, t1);
    if (spectralView) spectralView->setViewRange (t0, t1);

    // Selection sync: whichever view the user selected in, BOTH views show it.
    if (waveformView) waveformView->setSelection (app.selection);
    if (spectralView) spectralView->setSelection (app.selection);

    refreshPanels();
    updateScrollBars();
}

void MainWindow::refreshAfterEdit()
{
    // Edits can change the total duration: clamp the shared time window,
    // re-clamp the selection, repaint the waveform and force a full spectral
    // re-render. The tile cache must be cleared too — tiles are keyed by sample
    // position only, so they would otherwise show pre-edit data.
    const double dur = app.audioEngine().duration();
    auto r = [&] (double a, double b) -> std::pair<double, double>
    {
        if (b <= 0.0 || dur <= 0.0) return { 0.0, dur };
        a = std::clamp (a, 0.0, dur);
        b = std::clamp (b, a, dur);
        return { a, b };
    } (app.viewStart, app.viewEnd);

    applyTimeWindow (r.first, r.second);
    app.setSelection (app.selection); // re-clamp to the new duration

    syncSharedState();
    if (waveformView) waveformView->repaint();
    app.spectralEngine.clearCache();   // tiles are stale after any data change
    if (spectralView) spectralView->invalidateImage();
}

void MainWindow::resized()
{
    auto bounds = getLocalBounds();
    menuBar.setBounds (bounds.removeFromTop (24));

    // Status bar at the very bottom, full window width. (resized() can run
    // from setUsingNativeTitleBar() during construction, before these members
    // exist, so guard every pointer member.)
    if (statusBar)
        statusBar->setBounds (bounds.removeFromBottom (20));

    const bool haveFile = app.audioEngine().isFileLoaded();

    // Left panel column with the two tab groups and draggable dividers:
    // a vertical one between the column and the main area, a horizontal one
    // between the top and bottom tab groups. The whole column can be hidden
    // from View > Left Panels to focus on the tracks.
    // A tab group with no open panels takes no space; the other bar fills
    // the whole column when one of them is empty.
    const bool topEmpty = topTabs.getNumTabs () == 0;
    const bool botEmpty = bottomTabs.getNumTabs () == 0;
    const bool showLeft = leftPanelsVisible;
    topTabs.setVisible (showLeft && !topEmpty);
    bottomTabs.setVisible (showLeft && !botEmpty);
    if (leftSplitter) leftSplitter->setVisible (showLeft);

    auto rest = bounds;
    if (showLeft)
    {
        const int splitW = 6;
        const int leftW = std::clamp (leftPanelW, 220,
                                      std::max (221, (int) (rest.getWidth() * 0.6)));
        auto leftArea = rest.removeFromLeft (leftW + splitW);
        if (leftSplitter)
            leftSplitter->setBounds (leftArea.getRight() - splitW, leftArea.getY(),
                                     splitW, leftArea.getHeight());

        auto col = juce::Rectangle<int> (leftArea.getX(), leftArea.getY(),
                                         leftW, leftArea.getHeight());
        const int topH = (int) std::lround (col.getHeight() * topRatio);
        if (topEmpty || botEmpty)
        {
            if (!topEmpty) topTabs.setBounds (col);
            else           bottomTabs.setBounds (col);
            if (topSplitter) topSplitter->setVisible (false);
        }
        else
        {
            topTabs.setBounds (col.removeFromTop (topH));
            col.removeFromTop (splitW);   // strip for the horizontal divider
            bottomTabs.setBounds (col);
            if (topSplitter)
            {
                topSplitter->setVisible (true);
                topSplitter->setBounds (leftArea.getX(), leftArea.getY() + topH,
                                        leftW, splitW);
            }
        }
    }

    // Main area: waveform on top, draggable splitter, spectral below; a time
    // scrollbar along the bottom and one vertical scrollbar per view on its
    // right edge. Either view can be hidden from the View menu; at least one
    // is always visible and takes the whole main area when it is alone.
    const bool showWave = showWaveformView && (waveformView != nullptr);
    const bool showSpec = showSpectralView && (spectralView != nullptr);
    if (waveformView) waveformView->setVisible (showWave);
    if (spectralView)
    {
        spectralView->setVisible (showSpec);
        // In the stacked dual view the spectrogram drops its own time ruler -
        // the waveform's top ruler covers it. Alone, it draws one again.
        spectralView->setShowOwnTimeRuler (!showWave);
    }

    // Time scrollbar at the bottom of the main area.
    if (timeBar) timeBar->setVisible (haveFile);
    auto mainArea = rest;
    if (haveFile)
        mainArea.removeFromBottom (18);

    const int vScrollW = 14;
    if (waveVScroll) waveVScroll->setVisible (showWave && haveFile);
    if (specVScroll) specVScroll->setVisible (showSpec && haveFile);

    if (splitter)
        splitter->setVisible (showWave && showSpec);

    auto placeViewWithScrollBar = [&](juce::Rectangle<int> full, juce::Component* view,
                                      juce::Component* vbar) -> void
    {
        const juce::Rectangle<int> barRect (full.getRight() - vScrollW, full.getY(),
                                            vScrollW, full.getHeight());
        // removeFromRight() returns the REMOVED strip; we want the view to take
        // the remaining area, so shrink `full` first and then use it.
        full = full.withSize (std::max (0, full.getWidth() - vScrollW), full.getHeight());
        view->setBounds (full);
        if (vbar) vbar->setBounds (barRect);
    };

    if (showWave && showSpec)
    {
        const int waveH = (int) std::lround (mainArea.getHeight() * splitRatio);
        const int barH = 6;
        placeViewWithScrollBar (juce::Rectangle<int> (mainArea.getX(), mainArea.getY(),
                                                      mainArea.getWidth(), waveH),
                                waveformView.get(), waveVScroll.get());
        splitter->setBounds (mainArea.getX(), mainArea.getY() + waveH,
                             mainArea.getWidth(), barH);
        placeViewWithScrollBar (juce::Rectangle<int> (mainArea.getX(), mainArea.getY() + waveH + barH,
                                                      mainArea.getWidth(),
                                                      std::max (0, mainArea.getHeight() - waveH - barH)),
                                spectralView.get(), specVScroll.get());
    }
    else if (showWave)
    {
        placeViewWithScrollBar (mainArea, waveformView.get(), waveVScroll.get());
    }
    else if (showSpec)
    {
        placeViewWithScrollBar (mainArea, spectralView.get(), specVScroll.get());
    }

    // Aligned with the views' plot area: starts at the left-axis edge and
    // stops where their vertical scrollbars begin, so the thumb tracks
    // exactly the visible data.
    if (haveFile && timeBar)
        timeBar->setBounds (mainArea.getX() + kLeftAxisWidth, mainArea.getBottom() + 2,
                            std::max (0, mainArea.getWidth() - vScrollW - kLeftAxisWidth), 16);

    // Now that the bars have real sizes, sync their thumbs with the views.
    updateScrollBars();

}

void MainWindow::handleLeftSplitterDrag (const juce::MouseEvent& e)
{
    if (!e.eventComponent) return;
    const auto winTL = localPointToGlobal (juce::Point<int> (0, 0));
    const auto g = e.eventComponent->localPointToGlobal (
        juce::Point<int> ((int) e.position.x, (int) e.position.y));
    const double xInWindow = (double) g.x - (double) winTL.x;
    leftPanelW = std::clamp ((int) xInWindow - 6, 220,
                            std::max (221, (int) (getWidth() * 0.6)));
    resized();
}

void MainWindow::handleTopSplitterDrag (const juce::MouseEvent& e)
{
    if (!e.eventComponent) return;
    const auto winTL = localPointToGlobal (juce::Point<int> (0, 0));
    const auto g = e.eventComponent->localPointToGlobal (
        juce::Point<int> ((int) e.position.x, (int) e.position.y));
    const double yInWindow = (double) g.y - (double) winTL.y;
    const double colH = std::max (10.0, (double) getHeight() - 24.0 - 20.0);
    topRatio = std::clamp ((yInWindow - 24.0) / colH, 0.15, 0.85);
    resized();
}

// ------------------------------------------------- Left panel layout

const char* MainWindow::panelIdFor (juce::Component* c) const
{
    if (c == fileListPanel.get())       return "files";
    if (c == fftPanel.get())            return "fft";
    if (c == historyPanel.get())        return "history";
    if (c == pythonPanel.get())         return "python";
    if (c == controlsPanel.get())       return "controls";
    if (c == markersPanel.get())        return "markers";
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    if (c == transcriptionPanel.get())  return "transcription";
#endif
    if (c == selectionPanel.get())      return "selection";
    if (c == drawingPanel.get())        return "drawing";
    return nullptr;
}

void MainWindow::moveLeftPanelTab (juce::TabbedComponent* from, int fromIndex,
                                   juce::TabbedComponent* to, int insertIndex)
{
    if (!from || !to) return;
    const int nFrom = from->getNumTabs ();
    if (fromIndex < 0 || fromIndex >= nFrom) return;

    if (from == to)
    {
        // moveTab() indexes the list AFTER removal: dropping one slot to the
        // right therefore lands at insertIndex - 1.
        int target = (insertIndex > fromIndex) ? insertIndex - 1 : insertIndex;
        target = std::clamp (target, 0, nFrom - 1);
        if (target == fromIndex) return;
        from->moveTab (fromIndex, target);
    }
    else
    {
        const juce::String name   = from->getTabNames().getReference (fromIndex);
        const juce::Colour colour = from->getTabBackgroundColour (fromIndex);
        juce::Component* content  = from->getTabContentComponent (fromIndex);
        if (!content) return;

        insertIndex = std::clamp (insertIndex, 0, to->getNumTabs ());
        const int remaining = nFrom - 1;
        from->removeTab (fromIndex);
        to->addTab (name, colour, content, false, insertIndex);

        // Keep both bars in a sensible selected state: the moved tab becomes
        // active in its new bar, and if any tabs remain behind, the neighbour
        // that slid into the vacated slot takes over there.
        to->setCurrentTabIndex (insertIndex, false);
        if (remaining > 0)
            from->setCurrentTabIndex (std::min (fromIndex, remaining - 1), false);
    }

    saveLeftLayout ();
}

// Canonical list of the left-column panels: id, tab name and which group it
// starts in. The Window menu, setPanelVisible() and layout persistence all
// iterate this list, so a new panel only needs one entry here.
std::vector<MainWindow::LeftPanelEntry> MainWindow::leftPanelEntries() const
{
    return {
        { "files",       "Files",              true  },
        { "fft",         "Frequency Analysis", true  },
        { "history",     "History",            true  },
        { "python",      "Python",             true  },
        { "controls",    "Spectral Controls",  false },
        { "markers",     "Markers",            false },
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        { "transcription","Transcription",     false },
#endif
        { "selection",   "Selection",          false },
        { "drawing",     "Drawing",            false },
    };
}

juce::Component* MainWindow::panelForId (const juce::String& id) const
{
    if (id == "files")       return fileListPanel.get ();
    if (id == "fft")         return fftPanel.get ();
    if (id == "history")     return historyPanel.get ();
    if (id == "python")      return pythonPanel.get ();
    if (id == "controls")    return controlsPanel.get ();
    if (id == "markers")     return markersPanel.get ();
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    if (id == "transcription") return transcriptionPanel.get ();
#endif
    if (id == "selection")   return selectionPanel.get ();
    if (id == "drawing")     return drawingPanel.get ();
    return nullptr;
}

bool MainWindow::isPanelOpen (const juce::String& id) const
{
    juce::Component* comp = panelForId (id);
    if (!comp) return false;
    for (int i = 0; i < topTabs.getNumTabs (); ++i)
        if (topTabs.getTabContentComponent (i) == comp) return true;
    for (int i = 0; i < bottomTabs.getNumTabs (); ++i)
        if (bottomTabs.getTabContentComponent (i) == comp) return true;
    return false;
}

void MainWindow::setPanelVisible (const juce::String& id, bool visible)
{
    const auto entries = leftPanelEntries ();
    int idx = -1;
    for (size_t i = 0; i < entries.size (); ++i)
        if (juce::String (entries[i].id) == id) { idx = (int) i; break; }
    if (idx < 0 || isPanelOpen (id) == visible) return;

    if (!visible)
    {
        // Remove the tab from whichever bar currently holds it.
        for (auto* bar : { &topTabs, &bottomTabs })
            for (int i = 0; i < bar->getNumTabs (); ++i)
                if (bar->getTabContentComponent (i) == panelForId (id))
                {
                    bar->removeTab (i);
                    saveLeftLayout ();
                    resized();   // an emptied group must give up its space now
                    return;
                }
        return;
    }

    // Reopen: insert at the canonical position among the panels that are
    // currently open in this panel's default group, so reopening restores a
    // natural order without fighting user drags.
    juce::TabbedComponent& bar = entries[(size_t) idx].defaultTop ? topTabs : bottomTabs;
    int insertAt = 0;
    for (int i = 0; i < idx; ++i)
    {
        juce::Component* c = panelForId (entries[i].id);
        if (!c) continue;
        bool inThisBar = false;
        for (int t = 0; t < bar.getNumTabs (); ++t)
            if (bar.getTabContentComponent (t) == c) { inThisBar = true; break; }
        if (inThisBar) ++insertAt;
    }
    bar.addTab (entries[(size_t) idx].name, kDarkTabBg, panelForId (id), false, insertAt);
    saveLeftLayout ();
    resized();   // a previously empty group must take its space back now
}

void MainWindow::saveLeftLayout()
{
    auto idsFor = [this] (juce::TabbedComponent& bar) -> juce::String
    {
        juce::String s;
        for (int i = 0; i < bar.getNumTabs (); ++i)
        {
            const char* id = panelIdFor (bar.getTabContentComponent (i));
            if (!id) continue;
            if (s.isNotEmpty()) s << ";";
            s << id;
        }
        return s;
    };

    // Explicit closed list: panels missing from left_tab_layout could be new
    // ones added by a newer version, so "closed" must be stored separately.
    juce::String closed;
    for (const auto& e : leftPanelEntries ())
        if (!isPanelOpen (e.id))
        {
            if (closed.isNotEmpty()) closed << ";";
            closed << e.id;
        }

    app.recentStore->setValue ("left_tab_layout", idsFor (topTabs) + "|" + idsFor (bottomTabs));
    app.recentStore->setValue ("left_panels_closed", closed);
    app.recentStore->setValue ("left_panel_w", leftPanelW);
    app.recentStore->setValue ("top_ratio_mil", (int) std::lround (topRatio * 1000.0));
    app.recentStore->setValue ("left_panels_visible", leftPanelsVisible ? 1 : 0);
    app.recentStore->save ();
}

void MainWindow::applySavedLeftLayout()
{
    struct Entry { const char* id; juce::String name; juce::Component* comp; bool defaultTop; };
    std::vector<Entry> entries = {
        { "files",       "Files",              fileListPanel.get(),  true  },
        { "fft",         "Frequency Analysis", fftPanel.get(),       true  },
        { "history",     "History",            historyPanel.get(),   true  },
        { "python",      "Python",             pythonPanel.get(),    true  },
        { "controls",    "Spectral Controls",  controlsPanel.get(),  false },
        { "markers",     "Markers",            markersPanel.get(),   false },
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        { "transcription","Transcription",     transcriptionPanel.get(), false },
#endif
        { "selection",   "Selection",           selectionPanel.get(),  false },
        { "drawing",     "Drawing",             drawingPanel.get(),    false },
    };

    const juce::String saved = app.recentStore->getValue ("left_tab_layout");
    if (saved.isEmpty()) return;   // first run: keep the default order

    std::vector<std::string> wanted[2];
    auto addGroup = [&wanted] (const juce::String& s, int which)
    {
        int start = 0;
        while (start <= s.length ())
        {
            const int semi = s.indexOfChar (start, ';');
            const juce::String tok = (semi < 0 ? s.substring (start) : s.substring (start, semi)).trim ();
            if (!tok.isEmpty()) wanted[which].push_back (tok.toStdString());
            if (semi < 0) break;
            start = semi + 1;
        }
    };
    const int sep = saved.indexOfChar ('|');
    addGroup (sep < 0 ? saved : saved.substring (0, sep), 0);
    if (sep >= 0) addGroup (saved.substring (sep + 1), 1);

    topTabs.clearTabs ();
    bottomTabs.clearTabs ();

    std::vector<bool> used (entries.size (), false);
    auto addEntry = [&](juce::TabbedComponent& bar, const Entry& e)
    {
        bar.addTab (e.name, kDarkTabBg, e.comp, false);
    };

    for (int g = 0; g < 2; ++g)
        for (const auto& id : wanted[g])
            for (size_t e = 0; e < entries.size (); ++e)
                if (!used[e] && std::string (entries[e].id) == id)
                {
                    used[e] = true;
                    addEntry (g == 0 ? topTabs : bottomTabs, entries[e]);
                }

    // Panels missing from the saved list (e.g. added by a newer version) go to
    // their default group.
    for (size_t e = 0; e < entries.size (); ++e)
        if (!used[e]) addEntry (entries[e].defaultTop ? topTabs : bottomTabs, entries[e]);

    // Panels the user explicitly closed stay closed. (Newer-version panels not
    // in either list default to open.)
    const juce::StringArray closedIds = juce::StringArray::fromTokens (
        app.recentStore->getValue ("left_panels_closed"), ";", "");
    for (const auto& cid : closedIds)
    {
        juce::Component* comp = panelForId (cid);
        if (!comp) continue;
        for (auto* bar : { &topTabs, &bottomTabs })
        {
            int i = 0;
            while (i < bar->getNumTabs () && bar->getTabContentComponent (i) != comp)
                ++i;
            if (i < bar->getNumTabs ()) bar->removeTab (i);
        }
    }

    // Frequency Analysis is the default tab in the top group when present.
    int fftIdx = -1;
    for (int i = 0; i < topTabs.getNumTabs (); ++i)
        if (topTabs.getTabContentComponent (i) == fftPanel.get()) { fftIdx = i; break; }
    if (topTabs.getNumTabs () > 0)
        topTabs.setCurrentTabIndex (fftIdx >= 0 ? fftIdx : 0, false);
    if (bottomTabs.getNumTabs () > 0)
        bottomTabs.setCurrentTabIndex (0, false);
}

void MainWindow::setLeftPanelsVisible (bool visible)
{
    if (leftPanelsVisible == visible) return;
    leftPanelsVisible = visible;
    resized();
    saveLeftLayout ();
}

void MainWindow::handleSplitterDrag (const juce::MouseEvent& e)
{
    if (!waveformView || !spectralView || !e.eventComponent)
        return;

    const auto topLeft = waveformView->localPointToGlobal (juce::Point<int> (0, 0));
    const double mainTop = (double) topLeft.y;
    const double mainH = (double) waveformView->getHeight() + 6.0
                       + (double) spectralView->getHeight();
    if (mainH <= 10.0)
        return;

    // e.position is in the splitter's LOCAL coordinates: convert to global Y
    // before comparing with the main area's global top edge.
    const auto globalPos = e.eventComponent->localPointToGlobal (
        juce::Point<int> ((int) e.position.x, (int) e.position.y));
    splitRatio = std::clamp ((globalPos.y - mainTop) / mainH, 0.15, 0.85);
    resized();
}

void MainWindow::closeButtonPressed()
{
    JUCEApplication::getInstance()->systemRequestedQuit();
}

void MainWindow::timerCallback()
{
    // Kick off the lazy audio device init on the first tick. The timer only
    // runs while this window is alive, so no deferred callback can outlive it.
    if (!audioInitKickedOff)
    {
        audioInitKickedOff = true;
        app.startAudioInitAsync();
    }

    // A "Preparing audio" dialog is open and the init has finished: close it
    // and start the playback that was requested.
    if (audioInitDialog != nullptr && app.audioDeviceReady())
    {
        auto* dlg = audioInitDialog;   // deleting exits the modal state
        audioInitDialog = nullptr;
        delete dlg;
        app.play();
    }

    // Cancel a pending Shift+arrow selection anchor once Shift is released
    // (JUCE has no key-up callback, so poll the live modifier state).
    if (shiftAnchorTime >= 0.0 && !juce::ModifierKeys::getCurrentModifiers().isShiftDown())
        shiftAnchorTime = -1.0;

    // Keep the status-bar tool indicator in sync with live SHIFT/CTRL state
    // (setModeText() is a no-op when nothing changed).
    if (statusBar)
        statusBar->setModeText (currentModeText());

    // The Frequency Analysis panel follows the playhead while playing.
    if (fftPanel) { fftPanel->setFollowPlayhead (app.audioEngine().isPlaying()); fftPanel->repaint(); }

    if (!app.audioEngine().isPlaying() || !waveformView || !spectralView)
        return;
    const double pos = app.audioEngine().position();
    app.playhead = pos; // the UI cursor follows playback
    app.syncActiveDoc ();   // remember it in the active document
    waveformView->setPlayhead (pos);
    spectralView->setPlayhead (pos);
    if (statusBar) statusBar->setPlayhead (pos);
}

void MainWindow::visibilityChanged()
{
    DocumentWindow::visibilityChanged();
#if JUCE_WINDOWS
    if (isVisible() && !iconApplied)
    {
        iconApplied = true;
        if (juce::ComponentPeer* peer = getPeer())
            applyWindowIcon (peer->getNativeHandle());
    }
#endif
}

// ------------------------------------------------------------- drag & drop

// Every extension the engine can open as audio: JUCE's native formats plus
// video containers when built with FFmpeg. Shared by File Open and drag & drop
// so neither ever drifts from what AudioFormatManager actually accepts.
static const juce::String& allOpenableExtensions()
{
    // Trailing space: the video list starts with its own extension, so without
    // it the last native and first video token would glue together (.m4a.mp4).
    static const juce::String exts =
        ".wav .aiff .aif .flac .mp3 .ogg .m4a " + sc::videoContainerExtensions();
    return exts;
}

juce::String MainWindow::openAudioWildcard()
{
    static const juce::String w = [] {
        juce::StringArray list;
        for (const auto& e : juce::StringArray::fromTokens (allOpenableExtensions(), " ", ""))
            if (list.indexOf ("*" + e) < 0)
                list.add ("*" + e);
        juce::String out;
        for (int i = 0; i < list.size(); ++i)
            out += (i == 0 ? String() : ";") + list[i];
        return out;
    } ();
    return w;
}

bool MainWindow::isInterestedInFileDrag (const juce::StringArray& files)
{
    static const juce::String supported = allOpenableExtensions();
    for (const auto& f : files)
        if (supported.contains (juce::File (f).getFileExtension().toLowerCase()))
            return true;
    return false;
}

void MainWindow::fileDragEnter (const juce::StringArray&, int, int) { repaint(); }
void MainWindow::fileDragExit (const juce::StringArray&) { repaint(); }

void MainWindow::filesDropped (const juce::StringArray& files, int, int)
{
    // Collect the valid files first so the merge menu can offer its options.
    std::vector<juce::File> valid;
    for (const auto& f : files)
        if (juce::File (f).existsAsFile ())
            valid.push_back (juce::File (f));
    if (valid.empty ()) return;

    // Shift + drop: choose how to merge the dropped files. JUCE does not pass
    // modifier keys to FileDragAndDropTarget, so read them at drop time.
    if (juce::ModifierKeys::getCurrentModifiersRealtime ().isShiftDown ())
    {
        showMergeMenu (valid);
        return;
    }

    for (const auto& f : valid)
        openFile (f);   // multi-file editor: every valid drop becomes a document
}

juce::PopupMenu MainWindow::buildMergeMenu (bool haveActiveDoc)
{
    juce::PopupMenu m;
    m.addItem (1, "Open");
    m.addSeparator ();
    if (haveActiveDoc)
        m.addItem (2, "Open in sequence in current file");
    m.addItem (3, "Open in sequence in a new file");
    if (haveActiveDoc)
        m.addItem (4, "Open in parallel in current file");
    m.addItem (5, "Open in parallel in a new file");
    return m;
}



// ------------------------------------------------------------- state sync

// Appends the live SHIFT/CTRL selection-mode suffix to a tool indicator:
// CTRL -> "Subtract Selection", SHIFT -> "Add Selection" (the same modifiers
// that change how the next drag edits the spectral selection).
juce::String MainWindow::applyModifierHint (const juce::String& mode,
                                             bool shiftDown, bool ctrlDown) const
{
    if (ctrlDown)
        return mode + "  -  Subtract Selection";
    if (shiftDown)
        return mode + "  +  Add Selection";
    return mode;
}

// The status-bar tool indicator: the active spectral selection tool name,
// overridden by Clone & Multiply picking and Move/Scale gesture hints. While a
// shape-drawing tool is active (and keyboard focus is on a view or nowhere),
// holding SHIFT appends "Add Selection" and CTRL appends "Subtract Selection".
juce::String MainWindow::currentModeText() const
{
    // Armed drawing creation overrides every other tool indicator.
    if (spectralView != nullptr &&
        (spectralView->isPendingDraw () || spectralView->isDrawingNew ()))
        return "New drawing: click / drag in the spectrogram  (Esc cancels)";

    juce::String mode;
    switch (app.selMode)
    {
        case SpectralSelMode::Marquee:
            mode = "Marquee";
            break;
        case SpectralSelMode::Lasso:
            mode = "Lasso";
            break;
        case SpectralSelMode::Paintbrush:
            mode = juce::String ("Paintbrush ") +
                   juce::String ((int) std::lround (app.brushRadiusHz)) + " Hz";
            break;
        default: break;
    }

    // Clone & Multiply picking overrides the tool indicator with a hint.
    if (app.isClonePicking ())
        mode = app.hasClonePickRef ()
                   ? juce::String ("Clone pick  ref ") +
                     juce::String ((int) std::lround (app.clonePickRefFreq())) + " Hz"
                   : "Clone pick: click the reference frequency";

    // Move/Scale selection gestures override it with their own hint.
    if (app.isSelGesturing ())
        mode = app.haveSelGestureRef ()
                   ? juce::String (app.selGestureIsScale () ? "Scale sel" : "Move sel")
                     + "  ref " + juce::String (app.selGestureRefT (), 2) + " s, "
                     + juce::String ((int) std::lround (app.selGestureRefF ())) + " Hz"
                   : juce::String (app.selGestureIsScale () ? "Scale selection: click the reference point"
                                                           : "Move selection: click the reference point");

    // Live modifier hint for shape-drawing tools. JUCE has no key-up callback,
    // so this is polled from the timer as well; setModeText() ignores repeats.
    if (app.selMode != SpectralSelMode::TimeRange && !app.isClonePicking ()
        && !app.isSelGesturing ())
    {
        juce::Component* fc = juce::Component::getCurrentlyFocusedComponent ();
        const bool viewsHaveFocus = fc == nullptr || fc == waveformView.get ()
                                    || fc == spectralView.get ();
        if (viewsHaveFocus)
        {
            const auto mods = juce::ModifierKeys::getCurrentModifiers();
            return applyModifierHint (mode, mods.isShiftDown (), mods.isCtrlDown ());
        }
    }
    return mode;
}

// Push shared state (selection, channels, playhead, settings) into the views.
// Time windows and zoom levels are owned by each view independently.
void MainWindow::syncSharedState()
{
    updateTitle();   // cheap: keeps the * in sync with any state change

    if (waveformView)
    {
        waveformView->setSelection (app.selection);
        waveformView->setVisibleChannels (app.visibleChannels);
        waveformView->setPlayhead (app.playhead);
        waveformView->setMarkers (&app.markers().markers());
        // Unconditional repaint: state that only affects the paint output
        // (e.g. a changed scale calibration) must not rely on the setters.
        waveformView->repaint();
    }
    if (spectralView)
    {
        spectralView->setSelection (app.selection);
        spectralView->setVisibleChannels (app.visibleChannels);
        spectralView->setSettings (app.settings);
        spectralView->setPlayhead (app.playhead);
        spectralView->setMarkers (&app.markers().markers());
        // Cheap overlay-only repaint: marker list contents may have changed
        // even though the pointer did not.
        spectralView->repaint();
    }
    refreshPanels();

    // Persist the session settings whenever any of the persisted fields
    // changed (spectral log/linear + FFT size, frequency-analysis axis mode
    // + FFT size, zero-crossing snap).
    if (!app.settings.samePersistent (lastSyncedSettings))
    {
        lastSyncedSettings = app.settings;
        app.saveSettings();
    }

    // The colour map is not part of Settings, so detect it separately and
    // force a full spectral re-render when it changes.
    if (spectralView && app.colourMap != lastSyncedColourMap)
    {
        lastSyncedColourMap = app.colourMap;
        spectralView->invalidateImage();
    }

    if (statusBar)
    {
        // The file name is set at the open/activate/close transitions; the
        // sample rate can also change in place (resample), so refresh it here.
        statusBar->setSampleRate (app.audioEngine().isFileLoaded()
                                      ? app.audioEngine().sampleRate() : 0.0);
        // Footer fields + playhead readout follow the active time-axis scale.
        statusBar->setTimeContext (app.timeAxisMode, app.timeOffsetSecs,
                                   app.audioEngine().sampleRate());
        statusBar->setSelection (app.selection.getStart(), app.selection.getEnd());
        statusBar->setPlayhead (app.playhead);

        // Active spectral tool indicator + live SHIFT/CTRL selection hint.
        statusBar->setModeText (currentModeText());
    }

    updateScrollBars();
}

void MainWindow::refreshPanels()
{
    if (fileListPanel)  fileListPanel->refresh();
    if (controlsPanel)  controlsPanel->refresh();
    if (markersPanel)   markersPanel->refresh();
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    if (transcriptionPanel) transcriptionPanel->refresh();
#endif
    if (selectionPanel)   selectionPanel->refresh();
    if (drawingPanel)     drawingPanel->refresh();
    if (historyPanel)   historyPanel->refresh();
    if (fftPanel)       fftPanel->repaint();
}

// Files bigger than this are loaded on a background thread with a progress
// dialog so the UI stays responsive during I/O + mipmap construction.
static const int64_t kLargeFileBytes = 200ll * 1024 * 1024;

void MainWindow::openFile (const juce::File& file)
{
    // Already loaded? Just make it active again (fast, no progress needed).
    for (int i = 0; i < app.numDocs(); ++i)
        if (app.docs[(size_t) i]->file == file)
        { app.activate (i); return; }

    if (file.getSize() > kLargeFileBytes)
    {
        openFileWithProgress (file);
        return;
    }

    // app.openFile() activates the new document and fires onActiveDocChanged,
    // which resets both views to this file and syncs every panel.
    if (!app.openFile (file))
        return;
}

void MainWindow::playWithAudioInit()
{
    if (audioInitDialog != nullptr) return;   // already waiting - modal dialog open
    if (app.audioDeviceReady()) { app.play(); return; }

    // Init is almost certainly already running (kicked off at window show);
    // startAudioInitAsync() is idempotent. The 30 Hz timer closes the dialog
    // and starts playback as soon as audioDeviceReady() turns true.
    app.startAudioInitAsync();
    audioInitDialog = new WorkProgressDialog ("Preparing audio",
                                              "Initialising the audio output device...\nThis only takes a moment.",
                                              false, this);
    audioInitDialog->beginModal();
}

void MainWindow::showEditError (const std::string& err)
{
    juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                       "Effect failed",
                                       err.empty () ? "The effect could not be applied."
                                                     : juce::String (err),
                                       "", this);
}

void MainWindow::runDestructiveEdit(std::function<bool(std::string*)> edit)
{
    auto& eng = app.audioEngine();
    if (!eng.isFileLoaded () || eng.fullyInMemory ())
    {   // Fast path: already in memory (or nothing loaded - the edit is a no-op).
        std::string err;
        const bool ok = edit (&err);
        if (ok) refreshAfterEdit();
        else showEditError (err);
        return;
    }

    // Streamed file: materialise it into memory first. Reading multi-GB files
    // can take seconds, so run that on a worker thread behind a progress
    // dialog instead of freezing the UI.
    auto* dlg = new WorkProgressDialog ("Preparing file for editing",
                                        "Loading the file into memory ...", true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, edit, dlg] {
        std::string err;
        bool ok = app.audioEngine().ensureEditable (&err,
                                                    [dlg] (float f) {
                                                        juce::MessageManager::callAsync (
                                                            [dlg, f] { dlg->setProgress (0.9f * f); });
                                                    },
                                                    &dlg->cancelled);
        if (ok)
            ok = edit (&err);   // now in memory; only an OOM inside the edit can fail

        juce::MessageManager::callAsync ([this, dlg, ok, err] {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (ok) refreshAfterEdit();
            else if (!wasCancelled) showEditError (err);
        });
    }).detach();
}

void MainWindow::openFileWithProgress (const juce::File& file)
{
    auto* dlg = new WorkProgressDialog ("Loading", "Opening " + file.getFileName() + " ...",
                                        true, this);

    // Modal with keyboard focus so ESC hits the Cancel button. The worker
    // thread checks `cancelled` between I/O chunks and mipmap blocks; a
    // cancelled load discards the half-built document (nothing is opened).
    dlg->beginModal ();

    app.loadFileAsync (file,
        [dlg] (float frac) {
            juce::MessageManager::callAsync ([dlg, frac]
            { dlg->setProgress ((double) frac); });
        },
        [dlg] (bool) { delete dlg; },
        &dlg->cancelled);
}

void MainWindow::activateDocUI()
{
    // Reset both views to the whole file with default zoom levels, then push
    // the document's playhead/selection/channels/markers into all panels and
    // views. Works for a freshly opened file, a switched-to file (restoring
    // its remembered state) and the "no files left" case.
    if (waveformView) waveformView->fitAll();
    if (spectralView)
    {
        spectralView->fitAll();
        spectralView->resetFrequencyRange();
        // fitAll() only repaints when the time window actually changed - two
        // files of identical duration would leave the previous file's tiles on
        // screen. Application::activate() already cleared the tile cache, so
        // force a re-render from the newly active engine.
        spectralView->invalidateImage();
    }

    // Re-install the playback mask for this doc's spectral selection (or clear
    // it when the new doc has none / Play Spectral Selection is off).
    app.pushSpectralMaskToEngine ();

    // Per-file Frequency Analysis settings: if this document carries its own
    // saved values, mirror them into the panel's combos (no-op otherwise).
    if (fftPanel) fftPanel->resyncFromSettings();

    syncSharedState();
    updateTitle();

    // The footer always shows the name of the file currently in the views
    // (or the display name for a brand-new, not-yet-saved document).
    if (statusBar)
    {
        juce::String fn;
        if (app.audioEngine().isFileLoaded())
        {
            const juce::File f = app.audioEngine().getFile();
            fn = f.getFullPathName().isEmpty()
                     ? (app.doc() != nullptr ? app.doc()->name : juce::String())
                     : f.getFileName();
        }
        statusBar->setFileName (fn);
    }

    // Re-layout so the time scrollbar's visibility/bounds follow the new file
    // state: resized() is the only place they are set, and loading a file does
    // not otherwise resize the window (which left the bar hidden until the
    // user maximised/restored).
    resized();
}

juce::String MainWindow::statusBarFileName() const
{
    return statusBar ? statusBar->getFileName() : juce::String();
}

// useOSNativeDialogBox = false: the native Win32 IFileDialog is unavailable on some
// systems (COM class not registered / activation crashes with STATUS_NO_MEMORY);
// JUCE's built-in browser is pure GDI and works everywhere.
static constexpr bool kUseNativeFileChooser = false;

void MainWindow::handleFileOpen()
{
    juce::FileChooser chooser ("Open Audio File",
        app.audioEngine().isFileLoaded() ? app.audioEngine().getFile().getParentDirectory()
                                       : juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        openAudioWildcard(),
        kUseNativeFileChooser);

    if (chooser.browseForFileToOpen())
        openFile (chooser.getResult());
}

// Save on a background thread with a progress dialog so writing large files
// never blocks the UI thread.
void MainWindow::runSaveAsync (const juce::File& file, bool saveAs)
{
    auto* dlg = new WorkProgressDialog ("Saving", "Saving " + file.getFileName() + " ...",
                                        false, this);
    dlg->setProgress (-1.0);   // out of range -> indeterminate spinning bar
    dlg->enterModalState (false);

    FileDoc* d = app.doc();   // the document being saved (modal loop blocks switches)
    if (d != nullptr && !file.getFileExtension().equalsIgnoreCase (".wav")
        && d->metaMode == sc::MetaMode::Embedded)
        d->metaMode = sc::MetaMode::External;   // non-WAV cannot carry RIFF chunks

    std::thread ([this, file, saveAs, dlg, d] {
        const bool ok = saveAs ? app.audioEngine().saveFileAs (file)
                               : app.audioEngine().saveFile (file);
        juce::MessageManager::callAsync ([this, dlg, ok, file, d, saveAs] {
            delete dlg;
            if (!ok) return;

            // Save As: the document's path now points at the new location.
            if (saveAs && d != nullptr)
                d->file = file;

            // External mode: the sidecar travels with the saved file and the
            // legacy per-category sidecars are consolidated into it.
            if (d != nullptr && d->metaMode == sc::MetaMode::External)
            {
                d->writeMetaSidecar (file);
                juce::File lm (file.getFullPathName() + ".markers.json");
                if (lm.existsAsFile()) lm.deleteFile();
                const juce::File ls = savedSelectionsSidecarPath (file);
                if (ls.existsAsFile()) ls.deleteFile();
            }

            // The save wrote the audio AND every managed metadata chunk or
            // sidecar, so the document's state now matches its disk copy.
            if (d != nullptr)
                d->metaDirty = false;

            updateTitle();
        });
    }).detach();
}

void MainWindow::handleFileSave()
{
    // A brand-new document has no file path yet: Save behaves as Save As.
    const juce::File f = app.audioEngine().getFile();
    if (app.audioEngine().isFileLoaded() && !f.getFullPathName().isEmpty())
        runSaveAsync (f, false);
    else
        handleFileSaveAs();
}

void MainWindow::handleFileSaveAs()
{
    // Start in the current file's folder when one is loaded, so saving a
    // copy / exporting to another format begins where the user expects.
    juce::FileChooser chooser ("Save Audio File As",
        app.audioEngine().isFileLoaded() ? app.audioEngine().getFile().getParentDirectory()
                                         : juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.wav;*.aiff;*.aif;*.flac;*.ogg",
        kUseNativeFileChooser);

    if (chooser.browseForFileToSave (true))
        runSaveAsync (chooser.getResult(), true);
}

void MainWindow::handleSaveMetadataOnly()
{
    std::string err;
    if (!app.saveActiveDocMeta (&err))
        juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Save Metadata",
                                            juce::String::fromUTF8 (err.c_str()),
                                            "", this);
    else
        updateTitle();   // the * disappears if nothing else is unsaved
}

void MainWindow::handleCloseAll()
{
    if (app.numDocs () == 0) return;

    const auto closeMatching = [this] (bool includeUnsaved)
    {
        // Close from the back so earlier indices stay valid.
        for (int i = app.numDocs () - 1; i >= 0; --i)
            if (includeUnsaved || !app.docIsUnsaved (*app.docs[(size_t) i]))
                app.closeDoc (i);
    };

    const auto unsaved = app.unsavedDocNames ();
    if (!unsaved.empty ())
    {
        juce::String list;
        for (const auto& n : unsaved)
        {
            if (!list.isEmpty()) list += "\n";
            list += n;
        }

        juce::AlertWindow dlg ("Close All Files",
            "Some files have unsaved changes:\n\n" + list +
            "\n\nWhat do you want to do?", juce::AlertWindow::WarningIcon, this);
        dlg.addButton ("Close All", 1);
        dlg.addButton ("Close Unmodified Only", 2);
        dlg.addButton ("Cancel", 0);
        const int r = dlg.runModalLoop ();
        if (r == 0) return;
        closeMatching (r == 1);
    }
    else
        closeMatching (true);   // everything is clean: just close it all
}

// Render + write the spectrogram PNG on a background thread.
void MainWindow::exportSpectralAsync (const juce::File& file)
{
    auto* dlg = new WorkProgressDialog ("Exporting",
                                        "Rendering spectrogram to " + file.getFileName() + " ...",
                                        true, this);
    dlg->setProgress (-1.0);   // out of range -> indeterminate spinning bar
    dlg->beginModal ();

    std::thread ([this, file, dlg] {
        const bool ok = spectralView ? spectralView->saveAsPng (file, &dlg->cancelled) : false;
        juce::MessageManager::callAsync ([this, file, dlg, ok] {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (!ok && !wasCancelled)
                juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                                    "Export failed",
                                                    "Could not write the PNG file.", "", this);
        });
    }).detach();
}

void MainWindow::updateTitle()
{
    juce::String title = "SpectroCraft";
    if (app.audioEngine().isFileLoaded())
    {
        // Brand-new documents have no file path yet: show their display name.
        const juce::File f = app.audioEngine().getFile();
        const juce::String name = f.getFullPathName().isEmpty()
                                      ? (app.doc() != nullptr ? app.doc()->name : juce::String())
                                      : f.getFileName();
        title = name + " - " + title;
        // * covers both unsaved audio edits and metadata that differs from
        // what is on disk (markers, selections, calibrations, transcription).
        if (app.doc() != nullptr && app.docIsUnsaved (*app.doc()))
            title = "*" + title;
    }
    // setName() updates the native window caption; Component::setTitle() only
    // sets the accessibility title and leaves the caption unchanged.
    setName (title);
}

void MainWindow::applyColourMap (ColourMap m)
{
    app.colourMap = m;
    app.spectralEngine.setColourMap (m);
    syncSharedState();
}

juce::String MainWindow::buildAboutText()
{
    juce::String about =
        "SpectroCraft v" SC_VERSION_STRING "\n"
        "High-performance waveform + spectral editor\n"
        "\n"
        "This software is licensed under the GNU Affero General Public License v3.0.\n"
        "Source code: https://github.com/SuperMax3d/SpectroCraft\n"
        "\n"
        "Third-party components (full details in third_party/licenses/NOTICE.md):\n"
        "\n"
        "JUCE 9.0.4 - dual-licensed AGPL-3.0/commercial; this project uses the AGPLv3 option.\n"
        "SoXR resampler - LGPL-2.1, Steve Harris (the same engine Audacity uses).\n";

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    about +=
        "\n"
        "OpenVINO runtime 2024.6 - Apache-2.0, Intel Corporation (noise suppression only;\n"
        "loaded at runtime, the app works without it).\n"
        "\nNoise suppression:\n"
        "- DeepFilterNet DSP code ported from Intel's OpenVINO plugin for Audacity\n"
        "  (openvino-plugins-ai-audacity) - GPL-3.0-only,\n"
        "  Copyright (C) 2024 Intel Corporation.\n"
        "- DenseUNet model weights - Apache-2.0, Intel Open Source Model Zoo.\n"
        "- DeepFilterNet 2/3 model weights - see the intel/deepfilternet-openvino\n"
        "  repository on Hugging Face for its licence terms.\n"
        "\nTranscription:\n"
        "- whisper.cpp b5454 prebuilt binaries (+ ggml, llama.cpp) - MIT, ggerganov\n"
        "  (https://github.com/ggml-org/whisper.cpp); loaded at runtime.\n"
        "- Whisper model weights (ggml-*.bin) are OpenAI's Whisper checkpoints; check the\n"        "  upstream terms before commercial use.\n"
        "\nModel weights are downloaded on demand to %APPDATA%/SpectroCraft/models and are\n"
        "never distributed with this source.";
#endif

    return about;
}

// The About text is too long for a plain AlertWindow message: this JUCE build's
// AlertTextComp clamps its height to jmin(width, textHeight), which silently
// truncates the last lines. Show it in a read-only multi-line editor instead -
// scrollable and selectable (same pattern as showAiFailureDialog).
static void showAboutDialog (Component* parent)
{
    auto* w = new juce::AlertWindow ("About SpectroCraft", "",
                                     juce::AlertWindow::NoIcon, parent);
    auto* ed = new juce::TextEditor ();
    ed->setMultiLine (true);
    ed->setReadOnly (true);
    ed->setSize (640, 520);
    ed->setText (MainWindow::buildAboutText(), false);
    w->addCustomComponent (ed);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->enterModalState (false);
    ed->grabKeyboardFocus();
    w->runModalLoop ();
    delete w;
}

// ------------------------------------------------------------------- menus

void MainWindow::menuItemSelected (int menuItemID, int topLevelMenuIndex)
{
    using enum MenuIDs;

    // Window menu: toggle a left-column panel open/closed.
    if (menuItemID >= WindowPanelBase && menuItemID < WindowPanelBase + 16)
    {
        const auto entries = leftPanelEntries ();
        const int i = menuItemID - WindowPanelBase;
        if (i >= 0 && i < (int) entries.size ())
            setPanelVisible (entries[i].id, !isPanelOpen (entries[i].id));
        return;
    }

    // File > Open Recent items.
    if (menuItemID >= RecentBase && menuItemID < RecentBase + Application::kMaxRecentFiles)
    {
        const int i = menuItemID - RecentBase;
        if (i < app.recentFiles.size())
        {
            const juce::File f = app.recentFiles[(size_t) i];
            if (f.existsAsFile())
                openFile (f);
            else
                // Stale entry: the file was deleted or moved since it was opened.
                // Drop it so the menu self-heals on next open.
                app.removeRecentFile (f);
        }
        return;
    }

    if (menuItemID == ClearRecent)
    {
        app.clearRecentFiles();
        return;
    }

    // Per-channel visibility toggles: ChannelBase + channel index.
    // Ctrl/Shift+click on a channel shows ONLY that channel (solo).
    if (menuItemID >= ChannelBase && menuItemID < ChannelBase + app.audioEngine().numChannels())
    {
        const int ch = menuItemID - ChannelBase;
        const auto mods = juce::ModifierKeys::getCurrentModifiers();
        if (mods.isCommandDown() || mods.isShiftDown())
            app.visibleChannels = { ch };
        else
            app.toggleChannelVisible (ch);
        app.syncActiveDoc ();   // keep the active document's copy in step
        syncSharedState();
        return;
    }

    // Show every channel in both views.
    if (menuItemID == ViewShowAllChannels)
    {
        app.visibleChannels.clear();
        for (int i = 0; i < app.audioEngine().numChannels(); ++i)
            app.visibleChannels.push_back (i);
        app.syncActiveDoc ();   // keep the active document's copy in step
        syncSharedState();
        return;
    }

    // Selection tools + brush size + clear.
    switch (menuItemID)
    {
        case SelectionToolTimeRange:
            app.setSelectionTool (SpectralSelMode::TimeRange);
            break;
        case SelectionToolMarquee:
            app.setSelectionTool (SpectralSelMode::Marquee);
            break;
        case SelectionToolLasso:
            app.setSelectionTool (SpectralSelMode::Lasso);
            break;
        case SelectionToolPaintbrush:
            app.setSelectionTool (SpectralSelMode::Paintbrush);
            break;
        case SelectionClear:
            app.clearSpectralSelection ();
            break;
        case SelectionBrushSmaller:
            app.stepBrushRadius (false);
            break;
        case SelectionBrushLarger:
            app.stepBrushRadius (true);
            break;
        default: break;
    }
    if (menuItemID >= SelectionToolTimeRange && menuItemID <= SelectionBrushLarger)
    {
        syncSharedState ();
        return;
    }

    // View visibility: at least one of the two views must always stay visible.
    if (menuItemID == ViewShowWaveform)
    {
        if (!showSpectralView) return; // hiding both is not allowed
        showWaveformView = !showWaveformView;
        resized();
        return;
    }
    if (menuItemID == ViewShowSpectral)
    {
        if (!showWaveformView) return; // hiding both is not allowed
        showSpectralView = !showSpectralView;
        resized();
        return;
    }

    if (menuItemID == ViewShowLeftPanels)
    {
        setLeftPanelsVisible (!leftPanelsVisible);
        return;
    }

    switch (menuItemID)
    {
        case FileNew:  showNewFileDialog(); break;
        case FileOpen: handleFileOpen(); break;
        case FileCloseAll:
            handleCloseAll();
            break;

        case MetaSaveOnly:
            handleSaveMetadataOnly();
            break;

        case FileClose:
            // closeFile() fires onActiveDocChanged (or resets the empty state),
            // which refreshes every panel and view.
            app.closeFile();
            break;
        case FileSave: handleFileSave(); break;
        case FileSaveAs: handleFileSaveAs(); break;
        case MetaEmbedded:
            app.setActiveDocMetaMode (sc::MetaMode::Embedded);
            break;
        case MetaExternal:
            app.setActiveDocMetaMode (sc::MetaMode::External);
            break;
        case FileExportSpectral:
            if (spectralView && app.audioEngine().isFileLoaded())
            {
                juce::FileChooser chooser ("Save Spectrogram As",
                    app.audioEngine().getFile().getParentDirectory(), "*.png",
                    kUseNativeFileChooser);
                if (chooser.browseForFileToSave (true))
                    exportSpectralAsync (chooser.getResult());
            }
            break;
        case FileQuit: JUCEApplication::getInstance()->systemRequestedQuit(); break;

        case EditUndo: app.undoStep(); refreshAfterEdit(); break;
        case EditRedo: app.redoStep(); refreshAfterEdit(); break;
        case EditCut:    runDestructiveEdit ([&] (std::string* e) { return app.cut (e); }); break;
        case EditCopy:   app.copy(); break;   // read-only; works on streamed files too
        case EditPaste:  runDestructiveEdit ([&] (std::string* e) { return app.paste (e); }); break;
        case EditCopyToNewFile:
        {
            std::string err;
            if (!app.copyToNewFile (&err))
                showEditError (err);
            // Success: the new document is already active; onActiveDocChanged
            // has synchronised the UI.
            break;
        }
        case EditPasteMix: runDestructiveEdit ([&] (std::string* e) { return app.pasteMix (e); }); break;
        case EditPasteToNewChannels:
            runDestructiveEdit ([&] (std::string* e) { return app.pasteToNewChannels (e); });
            break;
        case EditDelete: runDestructiveEdit ([&] (std::string* e) { return app.del (e); }); break;
        case EditSelectAll: app.selectAll(); syncSharedState(); break;
        case EditSnapToZeroCrossings:
            app.settings.snapToZeroCrossings = !app.settings.snapToZeroCrossings;
            if (app.onStateChange) app.onStateChange();   // persist the new state
            break;

        case ViewZoomIn: zoomHorizontal (1.5); break;
        case ViewZoomOut: zoomHorizontal (1.0 / 1.5); break;
        case ViewZoomFit: zoomToFit(); break;
        case ViewZoomToSelection: zoomToSelection(); break;
        case ViewZoomOutFull: zoomOutFull(); break;
        case ViewVerticalZoomIn: zoomVerticalBoth (1.5); break;
        case ViewVerticalZoomOut: zoomVerticalBoth (1.0 / 1.5); break;

        case ColourMapClassic: applyColourMap (ColourMap::Classic); break;
        case ColourMapViridis: applyColourMap (ColourMap::Viridis); break;
        case ColourMapPlasma: applyColourMap (ColourMap::Plasma); break;
        case ColourMapInferno: applyColourMap (ColourMap::Inferno); break;
        case ColourMapMagma: applyColourMap (ColourMap::Magma); break;
        case ColourMapHot: applyColourMap (ColourMap::Hot); break;
        case ColourMapCool: applyColourMap (ColourMap::Cool); break;
        case ColourMapGrayscale: applyColourMap (ColourMap::Grayscale); break;

        case TransportPlay: playWithAudioInit(); break;
        case TransportStop: app.stop(); break;
        case TransportPause: app.pause(); break;
        case TransportLoop: app.toggleLoop(); break;
        case TransportPlaySpectralSel:
            // Toggle real-time spectral masking during playback. The flag is
            // auto-cleared if the selection disappears (see Application).
            app.playSpectralSel = !app.playSpectralSel;
            if (!app.hasSpectralSelection ())
                app.playSpectralSel = false;
            app.pushSpectralMaskToEngine ();
            syncSharedState ();
            break;

        case ToolsSilence:   runDestructiveEdit ([&] (std::string* e) { return app.silence (e); }); break;
        case ToolsFadeIn:    runDestructiveEdit ([&] (std::string* e) { return app.fadeIn (e); }); break;
        case ToolsFadeOut:   runDestructiveEdit ([&] (std::string* e) { return app.fadeOut (e); }); break;
        case ToolsNormalize: runDestructiveEdit ([&] (std::string* e) { return app.normalize (e); }); break;
        case ToolsChangeGain: showChangeGainDialog(); break;
        case ToolsResample: showResampleDialog(); break;
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        case ToolsNoiseSuppress: showNoiseSuppressDialog(); break;
        case ToolsTranscribe: showTranscribeDialog(); break;
        case ToolsTranscribeFolder: showTranscribeFolderDialog(); break;
#endif
        case ToolsReverse: runDestructiveEdit ([&] (std::string* e) { return app.reverse (e); }); break;

        case HelpAbout:
            showAboutDialog (this);
            break;
    }
}

// Zoom the shared time window around its centre (applies to both views).
void MainWindow::zoomHorizontal (double factor)
{
    if (!app.audioEngine().isFileLoaded()) return;

    const double t0 = app.viewStart, t1 = app.viewEnd;
    const double c = 0.5 * (t0 + t1);
    const double newHalf = 0.5 * (t1 - t0) / factor;
    applyTimeWindow (c - newHalf, c + newHalf); // clamps inside
}

void MainWindow::zoomVerticalBoth (double factor)
{
    if (!app.audioEngine().isFileLoaded()) return;
    if (waveformView) { waveformView->zoomVerticalBy (factor); waveformView->repaint(); }   // amplitude axis
    if (spectralView) { spectralView->zoomFrequencyBy (factor); spectralView->repaint(); }  // frequency axis
}

void MainWindow::zoomToFit()
{
    if (!app.audioEngine().isFileLoaded()) return;
    double t0 = 0.0, t1 = app.audioEngine().duration();
    if (!app.selection.isEmpty())
    {
        t0 = app.selection.getStart();
        t1 = app.selection.getEnd();
    }
    applyTimeWindow (t0, t1);
}

void MainWindow::zoomToSelection()
{
    if (!app.audioEngine().isFileLoaded() || app.selection.isEmpty()) return;
    applyTimeWindow (app.selection.getStart(), app.selection.getEnd());
}

void MainWindow::zoomOutFull()
{
    if (!app.audioEngine().isFileLoaded()) return;
    // "Zoom Out Full" from the track data resets BOTH axes: the shared time
    // window plus each view's vertical zoom. (Right-clicking an axis handles
    // CtxZoomOutFull locally in that view and only resets its own axis.)
    applyTimeWindow (0.0, app.audioEngine().duration());
    if (waveformView) waveformView->resetVerticalZoom();
    if (spectralView) spectralView->resetFrequencyRange();
}

// F9 cycles the view layout: both -> waveform only -> spectrogram only -> both.
// Shift+F9 runs the same cycle backwards. At least one view is always shown,
// so no extra guarding is needed here.
void MainWindow::cycleViewVisibility (bool forward)
{
    // 0 = both, 1 = waveform only, 2 = spectrogram only.
    int state;
    if      (showWaveformView && showSpectralView) state = 0;
    else if (showWaveformView)                    state = 1;
    else                                          state = 2;

    state = (state + (forward ? 1 : -1) + 3) % 3;

    showWaveformView = (state != 2);
    showSpectralView = (state != 1);
    resized();
}

// Prompts for a plain numeric value with OK/Cancel (defined below).
// clockStyle=true accepts h:mm:ss.xxxx / signed values instead of plain numbers.
static bool promptNumberValue (juce::Component* parent, const juce::String& title,
                               const juce::String& message, const juce::String& initial,
                               double& out, bool clockStyle = false);
// Strict plain-number parser (defined below).
static bool parseGainDbText (const juce::String& s, double& out);

// Actions from the right-click context menu shown by either view.
void MainWindow::handleContextMenuAction (int id)
{
    using enum ContextMenuIDs;
    switch (id)
    {
        case CtxZoomToSelection: zoomToSelection(); break;
        case CtxZoomOutFull:     zoomOutFull(); break;
        case CtxSelectAll:       app.selectAll(); break; // notifies via setSelection()
        case CtxCut:             runDestructiveEdit ([&] (std::string* e) { return app.cut (e); }); break;
        case CtxCopy:            app.copy();  break;   // read-only
        case CtxPaste:           runDestructiveEdit ([&] (std::string* e) { return app.paste (e); }); break;
        case CtxDelete:          runDestructiveEdit ([&] (std::string* e) { return app.del (e); }); break;
        case CtxCopyToNewFile:
        {
            std::string err;
            if (!app.copyToNewFile (&err))
                showEditError (err);
            break;
        }
        case CtxPasteMix:        runDestructiveEdit ([&] (std::string* e) { return app.pasteMix (e); }); break;

        // ---- Scale menu: time axis + vertical axis (independent) ----
        case CtxScaleTimeTime:
            app.timeAxisMode = TimeAxisMode::Time;
            app.timeOffsetSecs = 0.0;
            break;
        case CtxScaleTimeOffset:
        {
            // Ask for the time that should be shown at the playhead position.
            // Clock style (h:mm:ss.xxxx) or plain seconds, sign allowed - the
            // offset may span many hours and go negative.
            double v = 0.0;
            if (promptNumberValue (this, "Time Offset",
                                   "Displayed time at the playhead position\n"
                                   "(h:mm:ss.xxxx or plain seconds, +/- allowed):",
                                   formatOffsetTime (app.playhead), v, true))
            {
                app.timeAxisMode = TimeAxisMode::TimeOffset;
                app.timeOffsetSecs = v - app.playhead;
            }
            break;
        }
        case CtxScaleTimeSample:
            app.timeAxisMode = TimeAxisMode::Sample;
            break;
        case CtxScaleWaveWavUnits: app.waveVScaleMode = 0; break;
        case CtxScaleWaveDb:       app.waveVScaleMode = 1; break;
        case CtxScaleWaveNorm:     app.waveVScaleMode = 2; break;
        case CtxScaleSpecHz:
            app.specFreqScaleMode = 0;
            app.specFreqFactor = 1.0;
            app.pushActiveDocMeta();   // per-file reference calibration
            break;
        case CtxScaleSpecCustom:
            armRefClick (Application::RefClickTarget::SpecFreqCustom);
            break;

        // ---- Alternative Scale menu: "New" arms the ref-click flow and
        // appends a reference; clicking an existing name edits that entry. ----
        case CtxAltScaleTime:
            armRefClick (Application::RefClickTarget::AltTime);
            break;
        case CtxAltScaleFreq:
            armRefClick (Application::RefClickTarget::AltFreq);
            break;
    }

    // Alternative-scale reference edit dialogs (dynamic menu IDs).
    if (id >= CtxAltRefTimeBase && id < CtxAltRefFreqBase)
        showAltRefEditDialog (false, id - CtxAltRefTimeBase);
    else if (id >= CtxAltRefFreqBase && id < CtxAltRefFreqBase + 100)
        showAltRefEditDialog (true, id - CtxAltRefFreqBase);

    // Scale changes affect both views' rulers, the footer and the markers.
    syncSharedState();
}

// Prompts for a plain numeric value with OK/Cancel. Returns false when
// cancelled or the text is not a strict number (parseGainDbText rules).
static bool promptNumberValue (juce::Component* parent, const juce::String& title,
                               const juce::String& message, const juce::String& initial,
                               double& out, bool clockStyle)
{
    auto* w = new juce::AlertWindow (title, message, juce::AlertWindow::QuestionIcon, parent);
    w->addTextEditor ("value", initial);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // AlertWindow disables keyboard focus on its editors; re-enable it and
    // pre-select the text so a new value can be typed immediately.
    if (auto* editor = w->getTextEditor ("value"))
    {
        editor->setWantsKeyboardFocus (true);
        editor->selectAll();
    }

    // Enter modal state WITHOUT taking keyboard focus, then grab it for the
    // field once the window is showing.
    w->enterModalState (false);
    if (auto* editor = w->getTextEditor ("value"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop();
    const juce::String text = w->getTextEditorContents ("value");
    delete w;
    if (result != 1) return false;
    double v = 0.0;
    const bool parsed = clockStyle ? parseSignedClock (text, v)
                                   : parseGainDbText (text, v);
    if (!parsed)
    {
        juce::AlertWindow bad ("Invalid value",
                               "The entered text could not be read as a number.",
                               juce::AlertWindow::WarningIcon, parent);
        bad.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        bad.enterModalState (false);
        bad.runModalLoop ();
        return false;
    }
    out = v;
    return true;
}

void MainWindow::armRefClick (Application::RefClickTarget target)
{
    app.refClickTarget = target;
    app.haveRefClickPt = false;
    if (statusBar) statusBar->setModeText ("Place the reference point in the spectrogram...");
}

void MainWindow::handleRefClickPlaced (double tSecs, double fHz)
{
    const auto target = app.refClickTarget;

    juce::String title, message, initial;
    switch (target)
    {
        case Application::RefClickTarget::SpecFreqCustom:
            title   = "Custom Frequency Scale";
            message = "Value at this frequency:";
            initial = juce::String (fHz, 1);
            break;
        case Application::RefClickTarget::AltTime:
            title   = "Alternative Time Scale";
            message = "Value at this time (seconds):";
            initial = juce::String (tSecs, 3);
            break;
        case Application::RefClickTarget::AltFreq:
            title   = "Alternative Frequency Scale";
            message = "Value at this frequency:";
            initial = juce::String (fHz, 1);
            break;
        default:
            app.refClickTarget = Application::RefClickTarget::None;
            app.haveRefClickPt = false;
            if (spectralView) spectralView->repaint();
            return;
    }

    double v = 0.0;
    const bool ok = promptNumberValue (this, title, message, initial, v);

    // Clear the armed state + reference circle in every outcome.
    app.refClickTarget = Application::RefClickTarget::None;
    app.haveRefClickPt = false;
    if (statusBar) statusBar->setModeText (currentModeText());
    if (!ok)
    {
        if (spectralView) spectralView->repaint();
        return;
    }

    switch (target)
    {
        case Application::RefClickTarget::SpecFreqCustom:
            if (fHz > 1e-9 && v != 0.0)
            {
                app.specFreqScaleMode = 1;
                app.specFreqFactor = v / fHz;   // displayed(f) = f * factor
            }
            break;
        case Application::RefClickTarget::AltTime:
            if (std::abs (tSecs) > 1e-9 && v != 0.0)
            {
                // Append a new "Alt N" reference; existing ones are untouched.
                Application::AltScaleRef r;
                r.name     = Application::nextAltName (app.altTimeRefs);
                r.refMain  = tSecs;             // alt(t) = t * value / ref
                r.altValue = v;
                app.altTimeRefs.push_back (r);
            }
            break;
        case Application::RefClickTarget::AltFreq:
            if (fHz > 1e-9 && v != 0.0)
            {
                Application::AltScaleRef r;
                r.name     = Application::nextAltName (app.altFreqRefs);
                r.refMain  = fHz;               // alt(f) = f * value / ref
                r.altValue = v;
                app.altFreqRefs.push_back (r);
            }
            break;
        default: break;
    }

    app.pushActiveDocMeta();   // reference calibrations are per-file metadata
    syncSharedState();   // refreshes the status bar + spectral axis labels
}

// ---------------------------------------------------------------------------
// Alternative-scale reference editing (Alternative Scale > Time/Frequency)

/** Applies an alternative-reference edit dialog's outcome to the session
    state. result: 1 = OK, 2 = cancel, 3 = delete. Returns true when the
    state changed (so the caller can persist + refresh). */
bool MainWindow::applyAltRefEditResult (Application& app, bool isFreq, int index,
                                         int result, const juce::String& nameText,
                                         const juce::String& mainText,
                                         const juce::String& altText, bool visible)
{
    std::vector<Application::AltScaleRef>& refs = isFreq ? app.altFreqRefs : app.altTimeRefs;
    if (index < 0 || index >= (int) refs.size()) return false;

    if (result == 3)
    {
        refs.erase (refs.begin () + index);
        return true;
    }
    if (result != 1) return false;

    double mainV = 0.0, altV = 0.0;
    if (!parseGainDbText (mainText, mainV) || !parseGainDbText (altText, altV))
        return false;   // invalid text: keep the reference unchanged
    if (isFreq && mainV <= 1e-9)
        return false;   // a frequency anchor must be above zero

    Application::AltScaleRef& r = refs[(size_t) index];
    const juce::String newName = nameText.trim();
    if (!newName.isEmpty()) r.name = newName;
    r.refMain  = mainV;
    r.altValue = altV;
    r.enabled  = visible;
    return true;
}

juce::AlertWindow* MainWindow::createAltRefEditDialog (bool isFreq, int index)
{
    std::vector<Application::AltScaleRef>& refs = isFreq ? app.altFreqRefs : app.altTimeRefs;
    if (index < 0 || index >= (int) refs.size()) return nullptr;
    const Application::AltScaleRef& r = refs[(size_t) index];

    auto* w = new juce::AlertWindow ("Edit Alternative Scale Reference",
                                     "Rename the reference, edit its two endpoint values,\n"
                                     "or delete it. The other references are untouched.",
                                     juce::AlertWindow::NoIcon, this);
    const juce::String mainId = isFreq ? "mainHz" : "mainSecs";
    w->addTextEditor ("name", r.name);
    w->addTextEditor (mainId, juce::String (r.refMain, isFreq ? 1 : 4));
    w->addTextEditor ("altValue", juce::String (r.altValue, 3));

    auto* vis = new juce::ToggleButton ();
    vis->setComponentID ("altref-visible");
    vis->setButtonText ("Show in mouse readout");
    vis->setToggleState (r.enabled, juce::dontSendNotification);
    w->addCustomComponent (vis);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
    w->addButton ("Delete", 3, {});
    return w;
}

void MainWindow::showAltRefEditDialog (bool isFreq, int index)
{
    auto* w = createAltRefEditDialog (isFreq, index);
    if (w == nullptr) return;

    // Focus the name field so renaming is one keystroke away.
    if (auto* ed = w->getTextEditor ("name"))
    {
        ed->setWantsKeyboardFocus (true);
        ed->selectAll();
    }
    w->enterModalState (false);
    if (auto* ed = w->getTextEditor ("name"))
        ed->grabKeyboardFocus();

    const int result = w->runModalLoop ();
    // Read everything before destroying the window (the toggle is a child).
    const juce::String nameText  = w->getTextEditorContents ("name");
    const juce::String mainText  = w->getTextEditorContents (isFreq ? "mainHz" : "mainSecs");
    const juce::String altText   = w->getTextEditorContents ("altValue");
    bool visible = true;
    if (auto* vis = dynamic_cast<juce::ToggleButton*> (w->findChildWithID ("altref-visible")))
        visible = vis->getToggleState ();
    delete w;

    if (applyAltRefEditResult (app, isFreq, index, result, nameText, mainText,
                               altText, visible))
    {
        app.pushActiveDocMeta ();   // reference calibrations are per-file metadata
        syncSharedState ();         // refreshes the status bar + menus
    }
}

// ---------------------------------------------------------------------------
// Go to Time (Ctrl+G): one field per active time unit - the current main-axis
// mode plus every enabled alternative time reference.

MainWindow::GoToTimeFields MainWindow::computeGoToTimeFields (Application& app)
{
    GoToTimeFields f;
    switch (app.timeAxisMode)
    {
        case TimeAxisMode::TimeOffset:  f.mainId = "Time Offset"; break;
        case TimeAxisMode::Sample:      f.mainId = "Sample";      break;
        default:                        f.mainId = "Time (s)";    break;
    }

    // One field per enabled alternative time reference. Degenerate ones
    // (zero alt value) cannot be inverted and are skipped, as are duplicate
    // names (editor ids must stay unique).
    juce::StringArray used;
    used.add (f.mainId);
    for (int i = 0; i < (int) app.altTimeRefs.size(); ++i)
    {
        const auto& r = app.altTimeRefs[(size_t) i];
        if (!r.enabled || std::abs (r.altValue) < 1e-30) continue;
        juce::String id = r.name.trim();
        if (id.isEmpty() || used.contains (id)) continue;
        used.add (id);
        f.alts.push_back (std::make_pair (i, id));
    }
    return f;
}

bool MainWindow::resolveGoToTime (Application& app, const GoToTimeFields& fields,
                                  const juce::String& mainText,
                                  const std::vector<juce::String>& altTexts,
                                  double& outSecs)
{
    const double rate = std::max (1.0, app.audioEngine().sampleRate());

    // Main field first: parsed in the active scale mode's units.
    if (!mainText.trim().isEmpty()
        && parseDisplayedTime (mainText, app.timeAxisMode, app.timeOffsetSecs,
                               rate, outSecs))
        return true;

    // Then the alternative references in field order: invert their scale,
    // alt(x) = x * altValue / refMain  ->  x = v * refMain / altValue.
    for (size_t i = 0; i < fields.alts.size() && i < altTexts.size(); ++i)
    {
        const int idx = fields.alts[(int) i].first;
        if (idx < 0 || idx >= (int) app.altTimeRefs.size()) continue;
        const auto& r = app.altTimeRefs[(size_t) idx];
        double v = 0.0;
        if (!altTexts[i].trim().isEmpty() && parseGainDbText (altTexts[i], v))
        {
            outSecs = v * r.refMain / r.altValue;
            return true;
        }
    }
    return false;
}

juce::AlertWindow* MainWindow::createGoToTimeDialog ()
{
    if (!app.audioEngine().isFileLoaded()) return nullptr;
    const double rate = std::max (1.0, app.audioEngine().sampleRate());
    const GoToTimeFields fields = computeGoToTimeFields (app);

    auto* w = new juce::AlertWindow ("Go to Time",
                                     "Type a time in any field and press OK.\n"
                                     "The first non-empty value (top to bottom) is used.",
                                     juce::AlertWindow::QuestionIcon, this);

    // Main-axis field: label + initial text follow the active scale mode.
    w->addTextEditor (fields.mainId,
                      formatDisplayedTime (app.playhead, app.timeAxisMode,
                                           app.timeOffsetSecs, rate));

    for (const auto& a : fields.alts)
    {
        const auto& r = app.altTimeRefs[(size_t) a.first];
        w->addTextEditor (a.second,
                          juce::String (Application::altScaleValue (r, app.playhead), 3));
    }

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
    return w;
}

void MainWindow::showGoToTimeDialog ()
{
    auto* w = createGoToTimeDialog ();
    if (w == nullptr) return;

    // Focus the main field so a value can be typed immediately.
    const GoToTimeFields fields0 = computeGoToTimeFields (app);
    if (auto* ed = w->getTextEditor (fields0.mainId))
    {
        ed->setWantsKeyboardFocus (true);
        ed->selectAll();
    }
    w->enterModalState (false);
    if (auto* ed = w->getTextEditor (fields0.mainId))
        ed->grabKeyboardFocus();

    const int result = w->runModalLoop ();
    // Read every field before destroying the window.
    juce::String mainText;
    std::vector<juce::String> altTexts;
    if (result == 1)
    {
        const GoToTimeFields fields = computeGoToTimeFields (app);
        if (auto* ed = w->getTextEditor (fields.mainId))
            mainText = ed->getText();
        for (const auto& a : fields.alts)
        {
            juce::String t;
            if (auto* ed = w->getTextEditor (a.second)) t = ed->getText();
            altTexts.push_back (t);
        }
    }
    delete w;

    double tSecs = 0.0;
    if (result == 1 && resolveGoToTime (app, computeGoToTimeFields (app), mainText,
                                        altTexts, tSecs))
        app.setPlayhead (tSecs);   // clamps to the file; updates both views
}

// Prompts for the text content at the clicked anchor and commits a new Text
// drawing. The bounding box is estimated from the rendered pixel size so
// clicking this item in the Drawing panel can fit it into view.
void MainWindow::showNewTextDialog (double tSecs, double fHz, int px, int py)
{
    juce::AlertWindow dlg ("Drawing text", "Type the text to place at this point.",
                          juce::AlertWindow::NoIcon);
    dlg.setColour (juce::AlertWindow::backgroundColourId, juce::Colour::fromRGB (45, 45, 48));

    dlg.addTextEditor ("text", "", "Text:");
    auto* ed = dlg.getTextEditor ("text");
    dlg.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dlg.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    // Enter modal state WITHOUT taking keyboard focus, then grab it for the
    // field once the window is showing so typing can start immediately.
    if (ed != nullptr)
        ed->setWantsKeyboardFocus (true);

    dlg.enterModalState (false);
    if (ed != nullptr)
        ed->grabKeyboardFocus();

    const int result = dlg.runModalLoop ();
    // Read the editor before the window goes out of scope.
    juce::String text;
    if (result == 1 && ed != nullptr)
        text = ed->getText().trim();
    if (text.isEmpty()) return;

    DrawingItem item;
    item.kind       = DrawingItem::Kind::Text;
    item.t0         = tSecs;
    item.f0         = fHz;
    item.text       = text;
    item.colourARGB = app.settings.drawColourARGB;
    item.widthPx    = app.settings.drawWidth;
    item.textSize   = app.settings.drawTextSize;

    double t1 = tSecs, f1 = fHz;
    if (spectralView)
        spectralView->estimateTextExtent (px, py, text, app.settings.drawTextSize, t1, f1);
    item.t1 = std::max (tSecs, t1);
    item.f1 = std::max (fHz, f1);

    app.addDrawing (item);   // fires onDrawingsChanged -> panel refresh + repaint
}

// Parses a dB value typed into the Change Gain dialog. Returns false for
// anything that isn't a plain number (optional leading sign, digits, at most
// one dot) so half-typed text like "-" doesn't yank the slider around.
static bool parseGainDbText (const juce::String& s, double& out)
{
    const auto t = s.trim();
    if (t.isEmpty()) return false;

    int i = 0;
    if (t[i] == '+' || t[i] == '-') ++i;

    bool digitSeen = false, dotSeen = false;
    for (; i < t.length(); ++i)
    {
        const char c = t[i];
        if (c >= '0' && c <= '9')  { digitSeen = true; continue; }
        if (c == '.' && !dotSeen)  { dotSeen = true;   continue; }
        return false;
    }

    if (!digitSeen) return false;
    out = t.getDoubleValue();
    return true;
}

// Effects > Change Gain... — prompt for a dB value and apply it to the
// selection on the visible channels (undoable).
juce::AlertWindow* MainWindow::createGainDialog()
{
    auto* w = new juce::AlertWindow ("Change Gain",
                                     "Apply gain to the selection (dB):",
                                     juce::AlertWindow::QuestionIcon, this);
    w->addTextEditor ("gainDb", "0.0");

    // Slider below the text box: -60..+24 dB in 0.1 steps. The text box stays
    // authoritative — OK applies whatever is typed; the slider simply clamps
    // to its range when a larger value is entered.
    auto* slider = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
    slider->setName ("Gain (dB)");
    slider->setRange (-60.0, 24.0, 0.1);
    slider->setValue (0.0, juce::dontSendNotification);
    slider->setSize (280, 30);

    if (auto* editor = w->getTextEditor ("gainDb"))
    {
        // Let Enter/Escape reach the OK/Cancel shortcuts even while the field
        // has keyboard focus.
        editor->setEscapeAndReturnKeysConsumed (false);

        // Two-way sync between box and slider. Programmatic updates use
        // dontSendNotification, so only genuine user input propagates and no
        // feedback loop can start.
        slider->onValueChange = [editor, slider]
        {
            editor->setText (juce::String (slider->getValue(), 1));
        };

        editor->onTextChange = [slider, editor]
        {
            double v;
            if (!parseGainDbText (editor->getText(), v)) return;   // mid-typing: leave the slider alone
            slider->setValue (jlimit (-60.0, 24.0, v), juce::dontSendNotification);
        };
    }

    w->addCustomComponent (slider);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // AlertWindow disables keyboard focus on its editors, so re-enable it and
    // pre-select the text. The actual grabKeyboardFocus() happens in
    // showChangeGainDialog() once the window is showing: JUCE silently ignores
    // focus grabs for components that are not visible yet.
    if (auto* editor = w->getTextEditor ("gainDb"))
    {
        editor->setWantsKeyboardFocus (true);
        editor->selectAll();
    }

    return w;
}

void MainWindow::showChangeGainDialog()
{
    if (!app.audioEngine().isFileLoaded() || app.selection.isEmpty()) return;

    auto* w = createGainDialog();

    // Enter modal state WITHOUT taking keyboard focus: runModalLoop() would
    // otherwise re-enter the modal state and grab focus for the window itself,
    // stealing it from the text field.
    w->enterModalState (false);

    // Now that the window is showing, give the field the keyboard focus so a
    // new value can be typed in immediately (the old text is pre-selected).
    if (auto* editor = w->getTextEditor ("gainDb"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop();
    const juce::String text = w->getTextEditorContents ("gainDb");
    delete w;
    if (result != 1) return;

    const float db = (float) text.trim().getDoubleValue();
    runDestructiveEdit ([&] (std::string* e) { return app.changeGain (db, e); });
}

// Effects > Resample... — prompt for a new sample rate AND channel count and
// resample/remix the whole file (SoXR, same engine as Audacity). Not undoable:
// the dialog warns about it.
namespace
{
constexpr int kResampleMaxChans = 512;

/** Strict plain-number parse: optional sign, digits, at most one dot. */
bool parseWeightText (const juce::String& text, double& valueOut)
{
    const int n = text.length();
    int i = 0;
    if (i < n && (text[i] == '+' || text[i] == '-')) ++i;
    int digits = 0, dots = 0;
    for (; i < n; ++i)
    {
        const auto ch = text[i];
        if (ch >= '0' && ch <= '9') ++digits;
        else if (ch == '.') { if (++dots > 1) return false; }
        else return false;
    }
    if (digits <= 0) return false;
    const double v = text.getDoubleValue();
    valueOut = v;
    return std::isfinite (v);
}

juce::String formatWeightText (double v)
{
    if (std::abs (v - std::round (v)) < 1e-9 && std::abs (v) < 1e6)
        return juce::String ((int) std::lround (v));
    return juce::String (v, 4).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
}

/** Default mix matrix: identity when the count is unchanged, otherwise each
    output channel averages all inputs. */
std::vector<std::vector<double>> defaultRemixWeights (int outChans, int inChans)
{
    std::vector<std::vector<double>> w ((size_t) outChans,
                                        std::vector<double> ((size_t) inChans, 0.0));
    for (int r = 0; r < outChans; ++r)
        for (int c = 0; c < inChans; ++c)
            w[(size_t) r][(size_t) c] = (outChans == inChans)
                                           ? (r == c ? 1.0 : 0.0)
                                           : 1.0 / (double) inChans;
    return w;
}

/** Divides every row by its sum (zero rows stay silence). */
std::vector<std::vector<double>> normaliseRemixWeights (
    const std::vector<std::vector<double>>& weights)
{
    std::vector<std::vector<double>> out = weights;
    for (auto& row : out)
    {
        double sum = 0.0;
        for (double w : row) sum += w;
        if (sum > 0.0)
            for (double& w : row) w /= sum;
        else
            std::fill (row.begin(), row.end(), 0.0);
    }
    return out;
}

bool isIdentityRemix (const std::vector<std::vector<double>>& weights, int n)
{
    if ((int) weights.size() != n) return false;
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c)
            if (std::abs (weights[(size_t) r][(size_t) c] - (r == c ? 1.0 : 0.0)) > 1e-9)
                return false;
    return true;
}

// One row per output channel: an "Out N" label plus one weight field per
// input channel. Rebuilt whenever the requested output count changes.
class RemixGrid : public juce::Component
{
public:
    explicit RemixGrid (int inChans) : inChans_ (inChans)
    {
        setBounds (0, 0, widthFor (inChans_), kRowH);
    }

    int numRows() const { return (int) rows_.size(); }

    void rebuild (int outChans, const std::vector<std::vector<double>>& defaults)
    {
        removeAllChildren();
        rows_.clear();
        for (int r = 0; r < outChans; ++r)
        {
            Row row;
            row.label = new juce::Label ("", "Out " + juce::String (r + 1));
            addAndMakeVisible (row.label);
            for (int c = 0; c < inChans_; ++c)
            {
                auto* ed = new juce::TextEditor();
                ed->setMultiLine (false);
                ed->setText (formatWeightText (defaults[(size_t) r][(size_t) c]));
                addAndMakeVisible (ed);
                row.editors.push_back (ed);
            }
            rows_.push_back (std::move (row));
        }
        setSize (widthFor (inChans_), (int) rows_.size() * kRowH + 8);
        resized();
    }

    /** Parsed weights; false if any field is not a plain number. */
    bool getWeights (std::vector<std::vector<double>>& out) const
    {
        out.clear();
        for (const auto& row : rows_)
        {
            std::vector<double> w;
            for (auto* ed : row.editors)
            {
                const juce::String t = ed->getText().trim();
                if (t.isEmpty()) { w.push_back (0.0); continue; }
                double v = 0.0;
                if (!parseWeightText (t, v)) return false;
                w.push_back (v);
            }
            out.push_back (std::move (w));
        }
        return true;
    }

    void resized() override
    {
        for (size_t r = 0; r < rows_.size(); ++r)
        {
            auto& row = rows_[r];
            row.label->setBounds (0, (int) r * kRowH + 6, kLabelW, 18);
            for (size_t c = 0; c < row.editors.size(); ++c)
                row.editors[c]->setBounds (kLabelW + kGapX
                                              + (int) c * (kEditorW + kGapX),
                                           (int) r * kRowH + 4, kEditorW, 22);
        }
    }

private:
    struct Row { juce::Label* label; std::vector<juce::TextEditor*> editors; };
    int inChans_ = 0;
    std::vector<Row> rows_;

    static constexpr int kRowH = 26, kLabelW = 48, kGapX = 6, kEditorW = 52;
    static int widthFor (int inChans)
    {
        return kLabelW + kGapX + inChans * (kEditorW + kGapX);
    }
};

// Rebuilds the weight grid while the user edits the channel-count field.
// A (zero-size, invisible) component so the dialog owns it and deletes it
// automatically; plain addChildComponent keeps AlertWindow's layout code
// from touching it.
class NewChansListener : public juce::Component, public juce::TextEditor::Listener
{
public:
    juce::AlertWindow* window = nullptr;
    RemixGrid* grid = nullptr;
    int inChans = 0;

    void textEditorTextChanged (juce::TextEditor& ed) override
    {
        if (window == nullptr || grid == nullptr) return;
        double v = 0.0;
        const juce::String t = ed.getText().trim();
        int outChans = -1;
        if (!t.isEmpty() && parseWeightText (t, v)
            && std::abs (v - std::round (v)) < 1e-9
            && v >= 1.0 && v <= (double) kResampleMaxChans)
            outChans = (int) std::lround (v);
        if (outChans > 0 && grid->numRows() != outChans)
        {
            // AlertWindow's layout is private, so re-size the dialog by hand:
            // rebuild the grid in place, slide everything below it (the
            // OK/Cancel buttons) and grow/shrink the window to match.
            const int oldBottom = grid->getBottom();
            const int oldH = grid->getHeight();
            grid->rebuild (outChans, defaultRemixWeights (outChans, inChans));
            const int delta = grid->getHeight() - oldH;
            if (delta != 0)
            {
                for (int i = 0; i < window->getNumChildComponents(); ++i)
                {
                    auto* c = window->getChildComponent (i);
                    if (c != nullptr && c != grid && c->getBounds().getY() >= oldBottom)
                        c->setBounds (c->getX(), c->getBounds().getY() + delta,
                                      c->getWidth(), c->getHeight());
                }
                window->setSize (window->getWidth(),
                                std::max (100, window->getHeight() + delta));
            }
        }
    }
};
} // namespace

juce::AlertWindow* MainWindow::createResampleDialog()
{
    auto& eng = app.audioEngine();
    const double cur = eng.sampleRate();
    const int curChans = eng.numChannels();

    auto* w = new juce::AlertWindow ("Resample",
                                     "Current rate: " + juce::String (cur, 3) + " Hz, "
                                     + juce::String (curChans) + " channel(s)\n"
                                     "New sample rate (Hz):\n"
                                     "New channel count:\n"
                                     "Mix weights per output channel (each row is normalised to sum to 1):\n"
                                     "(whole file; cannot be undone)",
                                     juce::AlertWindow::QuestionIcon, this);
    w->addTextEditor ("newRate", juce::String (cur, 3));
    w->addTextEditor ("newChans", juce::String (curChans));

    auto* grid = new RemixGrid (curChans);
    grid->rebuild (curChans, defaultRemixWeights (curChans, curChans));
    w->addCustomComponent (grid);

    for (const char* name : { "newRate", "newChans" })
        if (auto* editor = w->getTextEditor (name))
            // Let Enter/Escape reach the OK/Cancel shortcuts even while a
            // field has keyboard focus.
            editor->setEscapeAndReturnKeysConsumed (false);

    // Rebuild the weight grid whenever the requested channel count changes.
    auto* chansListener = new NewChansListener();
    chansListener->window = w;
    chansListener->grid = grid;
    chansListener->inChans = curChans;
    w->addChildComponent (chansListener);   // owned by the dialog
    if (auto* editor = w->getTextEditor ("newChans"))
        editor->addListener (chansListener);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // AlertWindow disables keyboard focus on its editors, so re-enable it and
    // pre-select the text. The actual grabKeyboardFocus() happens in
    // showResampleDialog() once the window is showing.
    for (const char* name : { "newRate", "newChans" })
        if (auto* editor = w->getTextEditor (name))
            editor->setWantsKeyboardFocus (true);
    if (auto* editor = w->getTextEditor ("newRate"))
        editor->selectAll();

    return w;
}

void MainWindow::showResampleDialog()
{
    // Streamed files are fine: resampleTo materialises them into memory first
    // (with a clear error if that is impossible).
    auto& eng = app.audioEngine();
    if (!eng.isFileLoaded()) return;

    auto* w = createResampleDialog();

    // Enter modal state WITHOUT taking keyboard focus (see showChangeGainDialog).
    w->enterModalState (false);

    if (auto* editor = w->getTextEditor ("newRate"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop();
    const juce::String rateText = w->getTextEditorContents ("newRate");
    const juce::String chansText = w->getTextEditorContents ("newChans");
    std::vector<std::vector<double>> weights;
    bool weightsOk = false;
    for (int i = 0; i < w->getNumChildComponents() && !weightsOk; ++i)
        if (auto* grid = dynamic_cast<RemixGrid*> (w->getChildComponent (i)))
            weightsOk = grid->getWeights (weights);
    delete w;
    if (result != 1) return;

    const double newRate = rateText.trim().getDoubleValue();
    if (!std::isfinite (newRate) || newRate <= 0.0)
    {
        juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Invalid rate",
                                            "Please enter a positive sample rate in Hz.", "", this);
        return;
    }

    const int newChans = chansText.trim().getIntValue();
    if (newChans < 1 || newChans > kResampleMaxChans)
    {
        juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Invalid channel count",
                                            "Please enter a channel count between 1 and "
                                            + juce::String (kResampleMaxChans) + ".", "", this);
        return;
    }

    if (!weightsOk || (int) weights.size() != newChans)
    {
        juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Invalid mix weight",
                                            "Every weight field must be a number "
                                            "(empty means 0).", "", this);
        return;
    }

    weights = normaliseRemixWeights (weights);
    const bool sameRate = std::abs (newRate - eng.sampleRate())
                          < 1e-9 * std::max (eng.sampleRate(), newRate);
    if (sameRate && newChans == eng.numChannels() && isIdentityRemix (weights, newChans))
        return;   // nothing to do

    runResampleAsync (newRate, weights);
}

void MainWindow::showNewFileDialog()
{
    // Defaults follow the active document when one is open, so "New" makes a
    // twin of what you are looking at; otherwise 2ch / 44.1 kHz.
    int defChans = 2;
    double defRate = 44100.0;
    if (app.audioEngine().isFileLoaded())
    {
        defChans = app.audioEngine().numChannels();
        defRate = app.audioEngine().sampleRate();
    }

    auto* w = new juce::AlertWindow ("New File",
                                     "Create a new empty audio file.\n"
                                     "Number of channels:\n"
                                     "Sample rate (Hz):",
                                     juce::AlertWindow::QuestionIcon, this);
    w->addTextEditor ("channels", juce::String (defChans));
    w->addTextEditor ("rate", juce::String (defRate, 3));
    for (const char* name : { "channels", "rate" })
        if (auto* editor = w->getTextEditor (name))
            // Let Enter/Escape reach the OK/Cancel shortcuts even while a
            // field has keyboard focus.
            editor->setEscapeAndReturnKeysConsumed (false);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // AlertWindow disables keyboard focus on its editors; re-enable it and
    // pre-select the first field's text.
    if (auto* editor = w->getTextEditor ("channels"))
    {
        editor->setWantsKeyboardFocus (true);
        editor->selectAll();
    }

    // Enter modal state WITHOUT taking keyboard focus (see showChangeGainDialog).
    w->enterModalState (false);
    if (auto* editor = w->getTextEditor ("channels"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop();
    const juce::String chText = w->getTextEditorContents ("channels");
    const juce::String rateText = w->getTextEditorContents ("rate");
    delete w;
    if (result != 1) return;

    const int chans = chText.trim().getIntValue();
    const double rate = rateText.trim().getDoubleValue();
    if (chans < 1 || chans > 512)
    {
        juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Invalid channel count",
                                            "Please enter a channel count between 1 and 512.",
                                            "", this);
        return;
    }
    if (!std::isfinite (rate) || rate <= 0.0)
    {
        juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Invalid rate",
                                            "Please enter a positive sample rate in Hz.",
                                            "", this);
        return;
    }

    app.newFile (chans, rate);
}

// Resample (+ optional channel remix) on a background thread with a progress
// dialog so multi-GB files never block the UI. Progress: resampling 0..85%,
// mipmap rebuild 85..100%. Empty remixWeights keeps the current channels.

void MainWindow::runMergeFilesAsync (const std::vector<juce::File>& files, sc::MergeMode mode,
                                     bool intoCurrent)
{
    const juce::String what = juce::String (mode == sc::MergeMode::Sequence
                                             ? "Merging in sequence"
                                             : "Merging in parallel")
                              + juce::String (intoCurrent ? " into current file" : " into a new file");

    auto* dlg = new WorkProgressDialog ("Merging files",
                                        what + ": reading " + juce::String (files.size ())
                                            + " file(s) ...", true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, files, mode, intoCurrent, dlg] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v) {
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };
        auto progFn = [sendProgress] (float f) { sendProgress ((double) f); };

        if (intoCurrent)
        {
            std::string err;
            const bool ok = app.mergeFilesIntoActiveDoc (files, mode, progFn,
                                                         &dlg->cancelled, &err);
            juce::MessageManager::callAsync ([this, dlg, ok, err] {
                const bool wasCancelled = dlg->cancelled.load ();
                delete dlg;
                if (ok)
                {
                    app.clearClipboard ();   // samples may belong to the old rate
                    refreshAfterEdit();      // clears spectral cache + clamps window/selection
                    if (fftPanel) fftPanel->onSampleRateChanged ();
                    updateTitle ();
                }
                else if (!wasCancelled)
                {
                    const juce::String detail = err.empty () ? juce::String ("")
                                                             : juce::String ("\n") + juce::String (err);
                    juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                                       "Merge failed",
                                                       "The files could not be merged." + detail,
                                                       "", this);
                }
            });
        }
        else
        {
            auto* r = new sc::MergeResult();
            std::string err;
            const bool got = sc::mergeFilesToResult (files, mode, *r, progFn,
                                                     &dlg->cancelled, &err);
            juce::MessageManager::callAsync ([this, dlg, got, err, r] {
                if (!got)
                {
                    delete r;
                    const bool wasCancelled = dlg->cancelled.load ();
                    delete dlg;
                    if (!wasCancelled)
                    {
                        const juce::String detail = err.empty () ? juce::String ("")
                                                                 : juce::String ("\n") + juce::String (err);
                        juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                                           "Merge failed",
                                                           "The files could not be merged." + detail,
                                                           "", this);
                    }
                    return;
                }

                // Document creation + activation must happen on the message thread.
                std::string cerr;
                const bool ok = app.createDocFromMerged (*r, &cerr);
                delete r;
                delete dlg;
                if (ok)
                {
                    app.clearClipboard ();
                    refreshAfterEdit();
                    if (fftPanel) fftPanel->onSampleRateChanged ();
                    updateTitle ();
                }
                else
                {
                    const juce::String detail = cerr.empty () ? juce::String ("")
                                                              : juce::String ("\n") + juce::String (cerr);
                    juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                                       "Merge failed",
                                                       "The merged document could not be created." + detail,
                                                       "", this);
                }
            });
        }
    }).detach ();
}

void MainWindow::showMergeMenu (const std::vector<juce::File>& files)
{
    const bool haveDoc = app.doc () != nullptr && app.audioEngine ().isFileLoaded ();
    const int id = buildMergeMenu (haveDoc).showMenu (juce::PopupMenu::Options ().withMousePosition ());

    switch (id)
    {
        case 1:   // plain open - same as a drop without Shift
            for (const auto& f : files)
                openFile (f);
            break;
        case 2: runMergeFilesAsync (files, sc::MergeMode::Sequence, true);  break;
        case 3: runMergeFilesAsync (files, sc::MergeMode::Sequence, false); break;
        case 4: runMergeFilesAsync (files, sc::MergeMode::Parallel, true);  break;
        case 5: runMergeFilesAsync (files, sc::MergeMode::Parallel, false); break;
        default: break;   // dismissed
    }
}
void MainWindow::runResampleAsync (double newRate,
                                   const std::vector<std::vector<double>>& remixWeights)
{
    auto& eng = app.audioEngine();
    juce::String what = "Resampling to " + juce::String (newRate, 3) + " Hz";
    if (!remixWeights.empty() && (int) remixWeights.size() != eng.numChannels())
        what += ", " + juce::String (eng.numChannels()) + " -> "
                + juce::String ((int) remixWeights.size()) + " channels";

    auto* dlg = new WorkProgressDialog ("Resampling", what + " ...",
                                        true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, newRate, remixWeights, dlg] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v) {
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };

        // Cancellation stops at a SoXR chunk boundary; the buffer swap only
        // happens on success, so a cancelled resample changes nothing.
        std::string err;
        const bool ok = app.audioEngine().resampleTo (
            newRate,
            [sendProgress] (float f) { sendProgress (0.85 * (double) f); },
            &dlg->cancelled, &err,
            remixWeights.empty() ? nullptr : &remixWeights);
        if (ok)
            app.audioEngine().rebuildMipmaps ([sendProgress] (float f) {
                sendProgress (0.85 + 0.15 * (double) f);
            });

        juce::MessageManager::callAsync ([this, dlg, ok, err] {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (ok)
            {
                // The clipboard's samples belong to the old sample rate.
                app.clearClipboard ();

                // A channel-count change invalidates the visibility list:
                // show every channel of the new file.
                const int n = app.audioEngine().numChannels();
                app.visibleChannels.clear ();
                for (int ch = 0; ch < n; ++ch)
                    app.visibleChannels.push_back (ch);

                // refreshAfterEdit() clears the spectral tile cache and forces
                // a full re-render: after a rate change the old tiles are stale
                // (same sample positions now hold different content, and the
                // frequency axis moved), which otherwise shows as a shifted /
                // garbage spectrogram until some setting change happens to
                // invalidate it. It also clamps the time window + selection.
                refreshAfterEdit();
                if (fftPanel) fftPanel->onSampleRateChanged();
                updateTitle();
            }
            else if (!wasCancelled)
            {
                const juce::String detail = err.empty () ? juce::String ("")
                                                         : juce::String ("\n") + juce::String (err);
                juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                                    "Resample failed",
                                                    "The file could not be resampled." + detail, "", this);
            }
        });
    }).detach();
}

// Effects > Noise Suppression... — AI effect using the denseunet or
// DeepFilterNet2/3 models (Phases 1–2 of the AI plan). First use downloads
// the model weights (~9 MB for DenseUNet, ~15 MB per DeepFilterNet variant).
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI

namespace sc {

// Device dropdown shared by the Noise Suppression and Transcription dialogs.
juce::StringArray aiDeviceNames()
{
    return juce::StringArray { "Auto", "CPU", "GPU", "NPU" };
}

int aiDevicePrefIndex (const std::string& pref)
{
    if (pref == "CPU") return 1;
    if (pref == "GPU") return 2;
    if (pref == "NPU") return 3;
    return 0;   // Auto
}

std::string aiDevicePrefFromText (const juce::String& text)
{
    if (text == "CPU" || text == "GPU" || text == "NPU")
        return text.toStdString ();
    return "auto";
}

} // namespace sc
std::string sc::buildNsFailureDetails (const std::string& err, const std::string& modelId,
                                       const std::string& device)
{
    juce::String details = err.empty () ? "The selection could not be processed." : err;
    auto& mm = sc::ai::ModelManager::instance ();
    // When any of the OpenVINO DLLs is missing, pointing the user at model
    // downloads would be a trap (the weights are irrelevant until the runtime
    // loads), so every such case gets the DLL instructions instead.
    //
    // "OpenVINO runtime unavailable" is our own prefix for a failed LoadLibrary
    // probe: openvino.dll itself missing, or its static import tbb12.dll. Two
    // more companion DLLs load *dynamically* later and surface as OpenVINO
    // exceptions at model-read / compile time instead:
    //  - openvino_ir_frontend.dll absent -> "Unable to read the model ..."
    //    with an EMPTY "Available frontends:" list;
    //  - openvino_intel_cpu_plugin.dll absent -> 'Device with "CPU" name is
    //    not registered in the OpenVINO Runtime'.
    const bool runtimeMissing = details.startsWith ("OpenVINO runtime unavailable")
        || details.contains ("is not registered in the OpenVINO Runtime")
        || (details.contains ("Unable to read the model")
            && juce::String (details).trim ().endsWith ("Available frontends:"));
    // Only point at model downloads when the error plausibly involves the
    // model itself; "No WAV files found in ..." does not.
    const bool looksModelRelated = details.isEmpty ()
        || details.contains ("model")
        || details.contains ("install")
        || details.contains ("download");
    if (const auto* spec = mm.find (modelId))
    {
        if (runtimeMissing)
        {
            const juce::String prefix = details.startsWith ("OpenVINO runtime unavailable")
                ? "The OpenVINO runtime could not be loaded - its DLLs are missing."
                : "openvino.dll loaded, but one of its companion DLLs is missing.";
            details += "\n\n" + prefix
                       + " All four must sit in the same folder as SpectroCraft.exe:\n"
                       "  openvino.dll\n"
                       "  openvino_intel_cpu_plugin.dll\n"
                       "  openvino_ir_frontend.dll\n"
                       "  tbb12.dll\n";
            if (device == "GPU")
                details += "  openvino_intel_gpu_plugin.dll   (needed because you selected GPU)\n";
            else if (device == "NPU")
                details += "  openvino_npu_plugin.dll         (needed because you selected NPU)\n";
            details += "\nWhere to get them:\n"
                       "- If you built SpectroCraft from source: bin/Release in the repository.\n"
                       "- Otherwise download OpenVINO 2024.6 from https://github.com/openvinotoolkit/openvino/releases\n"
                       "  and take the DLLs from its runtime\\Windows\\bin folder.";
        }
        else if (looksModelRelated)
        {
            details += "\n\nManual installation: download these files into:\n"
                       + mm.dirFor (spec->id).getFullPathName () + "\n";
            for (const auto& f : spec->files)
                details += "  " + juce::String (f.url) + "\n";
        }
    }
    return details.toStdString ();
}

// Shared failure dialog for the AI features: a copyable read-only text editor
// with the error details (paths/URLs), built by sc::buildNsFailureDetails().
static void showAiFailureDialog (const juce::String& feature, const std::string& err,
                                 const std::string& modelId, Component* parent,
                                 const std::string& device = "CPU")
{
    const juce::String details = sc::buildNsFailureDetails (err, modelId, device);

    // Show the details in a read-only text editor: a plain AlertWindow label is
    // not selectable, and users need to copy paths/URLs out of this box.
    // Enter/Escape still close the dialog because key events bubble up from the
    // editor (read-only editors pass them through).
    auto* w = new juce::AlertWindow (feature + " Failed",
                                     "The operation could not be completed.\n"
                                     "Details below - select any text to copy it.",
                                     juce::AlertWindow::WarningIcon, parent);
    auto* ed = new juce::TextEditor ();
    ed->setMultiLine (true);
    ed->setReadOnly (true);
    ed->setSize (560, 230);
    ed->setText (details, false);
    w->addCustomComponent (ed);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->enterModalState (false);
    ed->grabKeyboardFocus();   // Ctrl+A / Ctrl+C work immediately
    w->runModalLoop ();
    delete w;
}

juce::AlertWindow* MainWindow::createNoiseSuppressDialog()
{
    auto* w = new juce::AlertWindow ("Noise Suppression",
                                     "The selection will be processed by the chosen AI model.\n"
                                     "First use downloads the model weights to\n"
                                     "%APPDATA%/SpectroCraft/models.\n"
                                     "\n"
                                     "Device: Auto picks GPU over NPU over CPU when available;\n"
                                     "pick one explicitly to override (unavailable devices fall back to CPU).\n"
                                     "OpenVINO only uses Intel CPU/GPU/NPU hardware - NVIDIA GPUs are not\n"
                                     "supported by the runtime and will never be listed.\n"
                                     "\n"
                                     "Attenuation limit (dB) applies to the DeepFilterNet models only:\n"
                                     "100 means no attenuation limit (full noise suppression).\n"
                                     "For little noise reduction, set to 6 - 12. For medium, 18 - 24.",
                                     juce::AlertWindow::QuestionIcon, this);

    const juce::StringArray modelNames { "DenseUNet (legacy)", "DeepFilterNet2", "DeepFilterNet3" };
    w->addComboBox ("model", modelNames, "Model");

    // Remember the last choice between sessions.
    if (auto* c = w->getComboBoxComponent ("model"))
    {
        const int idx = app.lastNsModelId == "deepfilternet2" ? 1
                        : app.lastNsModelId == "deepfilternet3" ? 2 : 0;
        c->setSelectedItemIndex (idx);
    }

    w->addComboBox ("device", sc::aiDeviceNames (), "Device");
    if (auto* c = w->getComboBoxComponent ("device"))
        c->setSelectedItemIndex (sc::aiDevicePrefIndex (app.aiDevicePref));

    // Default 100 = no attenuation limit (full noise suppression), matching
    // Audacity's default for this parameter.
    w->addTextEditor ("attenLim", "100", "Attenuation limit dB (100 = no limit)");

    w->addComboBox ("autoDl", juce::StringArray { "Yes", "No" },
                    "Auto-download model if missing");
    // The persisted choice is preselected.
    if (auto* c = w->getComboBoxComponent ("autoDl"))
        c->setSelectedItemIndex (app.aiAutoDownload ? 0 : 1);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
    return w;
}

void MainWindow::showNoiseSuppressDialog()
{
    if (!app.audioEngine().isFileLoaded() || app.selection.isEmpty()) return;

    auto* w = createNoiseSuppressDialog();

    // Enter modal state WITHOUT taking keyboard focus: runModalLoop() would
    // otherwise re-enter the modal state and grab focus for the window itself.
    w->enterModalState (false);

    // Give the model combo the keyboard focus so arrows/Enter work immediately;
    // Tab reaches the attenuation field if needed.
    if (auto* combo = w->getComboBoxComponent ("model"))
        combo->grabKeyboardFocus();

    const int result = w->runModalLoop();
    const juce::String modelText = w->getComboBoxComponent ("model") != nullptr
                                       ? w->getComboBoxComponent ("model")->getText() : "";
    const float attenLimDb = (float) w->getTextEditorContents ("attenLim").trim().getDoubleValue();
    const bool autoDownload = w->getComboBoxComponent ("autoDl") == nullptr
                               || w->getComboBoxComponent ("autoDl")->getText() == "Yes";
    const std::string devicePref = sc::aiDevicePrefFromText (w->getComboBoxComponent ("device") != nullptr
                                                               ? w->getComboBoxComponent ("device")->getText ()
                                                               : juce::String ());
    delete w;
    if (result != 1) return;

    const std::string modelId = modelText == "DeepFilterNet2" ? "deepfilternet2"
                                 : modelText == "DeepFilterNet3" ? "deepfilternet3"
                                                                 : "denseunet";
    // Remember the choices for next time.
    app.lastNsModelId  = modelId;
    app.aiAutoDownload = autoDownload;
    app.aiDevicePref   = devicePref;
    app.saveAiSettings();

    // v1: the DF3 post-filter option is not exposed in the dialog yet.
    runNoiseSuppressAsync (modelId, devicePref, attenLimDb, false, autoDownload);
}

// Noise suppression on a background thread with a progress dialog. Progress:
// model download/compile/inference 0..95%, mipmap rebuild 95..100%.
void MainWindow::runNoiseSuppressAsync (const std::string& modelId, const std::string& devicePref,
                                        float attenLimDb, bool df3PostFilter, bool autoDownload)
{
    auto* dlg = new WorkProgressDialog ("Noise Suppression",
                                        "Applying " + juce::String (modelId) + " ...",
                                        true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, modelId, devicePref, attenLimDb, df3PostFilter, autoDownload, dlg] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v) {
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };

        // Resolve the device preference against what the runtime offers.
        sc::ai::OvRuntime::instance ().init ();   // idempotent; needed for enumeration
        std::string devNote;
        const std::string device = sc::resolveAiDevice (devicePref,
                                                        sc::ai::OvRuntime::instance ().devices (),
                                                        &devNote);
        juce::MessageManager::callAsync ([dlg, modelId, device, devNote] {
            dlg->setStatusText ("Applying " + juce::String (modelId) + " on " + juce::String (device)
                                + (devNote.empty () ? " ..." : "  (" + juce::String (devNote) + ")"));
        });

        std::string err;
        // A false progress return cancels: the pipeline stops at its next
        // stage boundary and no audio is committed.
        const bool ok = app.noiseSuppress (modelId, device, attenLimDb, df3PostFilter,
            [sendProgress, dlg] (float f) {
                sendProgress (0.95 * (double) f);
                return !dlg->cancelled.load ();
            }, &err,
            autoDownload);
        if (ok)
            app.audioEngine().rebuildMipmaps ([sendProgress] (float f) {
                sendProgress (0.95 + 0.05 * (double) f);
            });

        juce::MessageManager::callAsync ([this, modelId, dlg, ok, err, device] {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (ok)
            {
                refreshAfterEdit();
                return;
            }

            // A user cancel is not a failure: just close quietly.
            if (!wasCancelled)
                showAiFailureDialog ("Noise Suppression", err, modelId, this, device);
        });
    }).detach();
}

// ---------------------------------------------------------------------------
// Python panel: run a user script against the selection (or whole file).
// Progress: materialise + extract 0..30%, interpreter run 30..95% (the bar
// holds steady while pure-Python processing runs), re-import 95..100%.

void MainWindow::runPythonScriptAsync (const juce::File& script, const juce::String& scriptText)
{
    std::string pyCmd;
    if (!sc::py::findPython (&pyCmd))
    {
        juce::AlertWindow::showMessageBox (juce::AlertWindow::InfoIcon,
                                           "Python not found",
                                           sc::py::installHelpText (),
                                           juce::String(), this);
        return;
    }

    auto* dlg = new WorkProgressDialog ("Python",
                                        "Running " + script.getFileName () + " ...",
                                        true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, script, scriptText, pyCmd, dlg] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v) {
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };

        std::string err;
        const bool ok = app.runPythonScript (script, scriptText, pyCmd,
            [sendProgress, dlg] (float f) {
                sendProgress (0.95 * (double) f);
                return !dlg->cancelled.load ();
            }, &err);
        if (ok)
            app.audioEngine().rebuildMipmaps ([sendProgress] (float f) {
                sendProgress (0.95 + 0.05 * (double) f);
            });

        juce::MessageManager::callAsync ([this, dlg, ok, err] {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (ok)
                refreshAfterEdit();
            else if (!wasCancelled)   // a user cancel is not a failure
                showEditError (err);
        });
    }).detach();
}

// ---------------------------------------------------------------------------
// Effects > Transcribe... — Whisper transcription of the whole file
// (Phase 3 of the AI plan). First use downloads the model weights.

static juce::StringArray whisperModelIds()
{
    juce::StringArray ids;
    for (const auto& s : sc::ai::ModelManager::instance ().registry ())
        if (s.id.rfind ("whisper-", 0) == 0)
            ids.add (juce::String (s.id));
    return ids;
}

static juce::StringArray whisperModelNames()
{
    juce::StringArray names;
    for (const auto& s : sc::ai::ModelManager::instance ().registry ())
        if (s.id.rfind ("whisper-", 0) == 0)
            names.add (juce::String (s.displayName));
    return names;
}

// Display name -> BCP-47 language code ("auto" stays "auto").
static const juce::StringArray& transcribeLanguageNames()
{
    static const juce::StringArray names {
        "Auto", "English", "German", "French", "Spanish", "Italian",
        "Portuguese", "Russian", "Chinese", "Japanese", "Korean",
        "Arabic", "Hindi", "Dutch", "Polish", "Turkish", "Ukrainian",
        "Vietnamese", "Thai", "Swedish" };
    return names;
}

static const juce::StringArray& transcribeLanguageCodes()
{
    static const juce::StringArray codes {
        "auto", "en", "de", "fr", "es", "it",
        "pt", "ru", "zh", "ja", "ko",
        "ar", "hi", "nl", "pl", "tr", "uk",
        "vi", "th", "sv" };
    return codes;
}

juce::AlertWindow* MainWindow::createTranscribeDialog()
{
    auto* w = new juce::AlertWindow ("Transcription",
                                     "The WHOLE file will be transcribed by Whisper; the visible "
                                     "channels are mixed to mono.\n"
                                     "First use downloads the model weights to\n"
                                     "%APPDATA%/SpectroCraft/models.\n"
                                     "Larger models are more accurate but slower.\n"
                                     "Transcription runs on CPU (whisper.cpp b5454 prebuilt runtime).",
                                     juce::AlertWindow::QuestionIcon, this);

    w->addComboBox ("model", whisperModelNames (), "Model");
    w->addComboBox ("language", transcribeLanguageNames (), "Language");
    w->addComboBox ("mode",
                    juce::StringArray { "Transcribe", "Translate to English" },
                    "Mode");
    w->addComboBox ("speed",
                    juce::StringArray { "Balanced (beam search)", "Fast (greedy)" },
                    "Speed / accuracy");

    // Preselect the persisted choices (defaults when never set).
    if (auto* c = w->getComboBoxComponent ("model"))
    {
        const int mi = whisperModelIds ().indexOf (juce::String (app.lastTranscribeModelId));
        c->setSelectedItemIndex (mi >= 0 ? mi : 0);
    }
    if (auto* c = w->getComboBoxComponent ("language"))
    {
        const int li = transcribeLanguageCodes ().indexOf (juce::String (app.lastTranscribeLanguage));
        c->setSelectedItemIndex (li >= 0 ? li : 0);
    }
    if (auto* c = w->getComboBoxComponent ("mode"))
        c->setSelectedItemIndex (app.lastTranscribeTranslate ? 1 : 0);
    if (auto* c = w->getComboBoxComponent ("speed"))
        c->setSelectedItemIndex (app.lastTranscribeFast ? 1 : 0);

    w->addComboBox ("autoDl", juce::StringArray { "Yes", "No" },
                    "Auto-download model if missing");
    if (auto* c = w->getComboBoxComponent ("autoDl"))
        c->setSelectedItemIndex (app.aiAutoDownload ? 0 : 1);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
    return w;
}

void MainWindow::showTranscribeDialog()
{
    if (!app.audioEngine ().isFileLoaded ()) return;

    auto* w = createTranscribeDialog ();

    // Enter modal state WITHOUT taking keyboard focus (see showNoiseSuppressDialog).
    w->enterModalState (false);
    if (auto* combo = w->getComboBoxComponent ("model"))
        combo->grabKeyboardFocus ();

    const int result = w->runModalLoop ();
    const juce::String modelText   = w->getComboBoxComponent ("model") != nullptr
                                        ? w->getComboBoxComponent ("model")->getText () : "";
    const juce::String langText    = w->getComboBoxComponent ("language") != nullptr
                                        ? w->getComboBoxComponent ("language")->getText () : "auto";
    const bool translate           = w->getComboBoxComponent ("mode") != nullptr
                                        && w->getComboBoxComponent ("mode")->getText () == "Translate to English";
    const bool fast                = w->getComboBoxComponent ("speed") != nullptr
                                        && w->getComboBoxComponent ("speed")->getText ().startsWith ("Fast");
    const bool autoDownload        = w->getComboBoxComponent ("autoDl") == nullptr
                                       || w->getComboBoxComponent ("autoDl")->getText () == "Yes";
    delete w;
    if (result != 1) return;

    // Map the display texts back to registry ids / language codes.
    const auto names = whisperModelNames ();
    const int mi = names.indexOf (modelText);
    if (mi < 0) return;
    const std::string modelId = whisperModelIds ()[(size_t) mi].toStdString ();

    const auto langNames = transcribeLanguageNames ();
    const int li = langNames.indexOf (langText);
    const std::string language = (li >= 0 ? transcribeLanguageCodes ()[(size_t) li] : juce::String ("auto")).toStdString ();

    // Remember the choices for next time.
    app.lastTranscribeModelId   = modelId;
    app.lastTranscribeLanguage  = language;
    app.lastTranscribeTranslate = translate;
    app.lastTranscribeFast      = fast;
    app.aiAutoDownload          = autoDownload;
    app.saveAiSettings ();

    runTranscriptionAsync (modelId, language, translate, fast, autoDownload);
}

// Transcription on a background thread with a progress dialog. Progress:
// download [0, 15%), mix+resample [15, 25%), inference [25, 100%].
void MainWindow::runTranscriptionAsync (const std::string& modelId, const std::string& language,
                                        bool translate, bool fast, bool autoDownload)
{
    auto* dlg = new WorkProgressDialog ("Transcription",
                                        "Transcribing with " + juce::String (modelId) + " ...\n"
                                        "(whole file - this can take a while)\n"
                                        "(Cancel stops after the current phase)",
                                        true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, modelId, language, translate, fast, autoDownload, dlg] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v) {
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };

        juce::MessageManager::callAsync ([dlg, modelId] {
            dlg->setStatusText ("Transcribing with " + juce::String (modelId) + " on CPU ..."
                                + "\n(whole file - this can take a while)"
                                  "\n(Cancel stops after the current phase)");
        });

        std::string err;
        // A false progress return cancels: download and mix/resample stop at
        // their next checkpoint; inference itself cannot be interrupted
        // mid-whisper_full, so a cancel during it finishes that phase first.
        const bool ok = app.transcribe (modelId, language, translate, fast,
            [sendProgress, dlg] (float f) {
                sendProgress ((double) f);
                return !dlg->cancelled.load ();
            }, &err,
            autoDownload);

        juce::MessageManager::callAsync ([this, dlg, ok, err, modelId] {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (ok)
            {
                // Show the results: refresh the panel and switch to its tab.
                if (transcriptionPanel)
                {
                    transcriptionPanel->refresh ();
                    for (int i = 0; i < bottomTabs.getNumTabs (); ++i)
                        if (bottomTabs.getTabContentComponent (i) == transcriptionPanel.get ())
                        {
                            bottomTabs.setCurrentTabIndex (i, false);
                            break;
                        }
                }
                return;
            }

            // A user cancel is not a failure: just close quietly.
            if (!wasCancelled)
                showAiFailureDialog ("Transcription", err, modelId, this, "CPU");
        });
    }).detach ();
}

// ---------------------------------------------------------------------------
// Effects > Transcribe Folder... — batch Whisper transcription of every WAV
// in a folder tree. Files are processed headlessly and their SCTT chunk is
// written back in place; nothing opens in the editor.
juce::AlertWindow* MainWindow::createTranscribeFolderDialog (const juce::File& folder)
{
    auto* w = new juce::AlertWindow ("Transcribe Folder",
                                     "Every .wav file in this folder (including subfolders)\n"
                                     "will be transcribed by Whisper and its SCTT chunk\n"
                                     "updated in place. The files are NOT opened in the editor.\n"
                                     "Transcription runs on CPU (whisper.cpp b5454 prebuilt runtime).\n"
                                     "Channel to analyse: a 1-based channel number; enter 0 or\n"
                                     "less to mix all channels together before transcribing.\n\n"
                                     + folder.getFullPathName (),
                                     juce::AlertWindow::QuestionIcon, this);

    w->addComboBox ("model", whisperModelNames (), "Model");
    w->addComboBox ("language", transcribeLanguageNames (), "Language");
    w->addComboBox ("mode",
                    juce::StringArray { "Transcribe", "Translate to English" },
                    "Mode");
    w->addComboBox ("speed",
                    juce::StringArray { "Balanced (beam search)", "Fast (greedy)" },
                    "Speed / accuracy");
    w->addComboBox ("existing",
                    juce::StringArray { "Overwrite existing SCTT", "Skip files with SCTT" },
                    "Existing transcriptions");

    w->addTextEditor ("channel", juce::String (app.lastTranscribeChannel),
                      "Channel to analyse");

    // Preselect the persisted choices (defaults when never set).
    if (auto* c = w->getComboBoxComponent ("model"))
    {
        const int mi = whisperModelIds ().indexOf (juce::String (app.lastTranscribeModelId));
        c->setSelectedItemIndex (mi >= 0 ? mi : 0);
    }
    if (auto* c = w->getComboBoxComponent ("language"))
    {
        const int li = transcribeLanguageCodes ().indexOf (juce::String (app.lastTranscribeLanguage));
        c->setSelectedItemIndex (li >= 0 ? li : 0);
    }
    if (auto* c = w->getComboBoxComponent ("mode"))
        c->setSelectedItemIndex (app.lastTranscribeTranslate ? 1 : 0);
    if (auto* c = w->getComboBoxComponent ("speed"))
        c->setSelectedItemIndex (app.lastTranscribeFast ? 1 : 0);

    w->addComboBox ("autoDl", juce::StringArray { "Yes", "No" },
                    "Auto-download model if missing");
    if (auto* c = w->getComboBoxComponent ("autoDl"))
        c->setSelectedItemIndex (app.aiAutoDownload ? 0 : 1);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
    return w;
}

void MainWindow::showTranscribeFolderDialog()
{
    // Step 1: pick the folder.
    juce::FileChooser chooser ("Transcribe Folder",
                               juce::File::getSpecialLocation (juce::File::userHomeDirectory),
                               "", false);
    if (!chooser.browseForDirectory ())
        return;
    const juce::File folder = chooser.getResult ();

    // Step 2: options dialog.
    auto* w = createTranscribeFolderDialog (folder);
    w->enterModalState (false);
    if (auto* combo = w->getComboBoxComponent ("model"))
        combo->grabKeyboardFocus ();

    const int result = w->runModalLoop ();
    const juce::String modelText   = w->getComboBoxComponent ("model") != nullptr
                                        ? w->getComboBoxComponent ("model")->getText () : "";
    const juce::String langText    = w->getComboBoxComponent ("language") != nullptr
                                        ? w->getComboBoxComponent ("language")->getText () : "auto";
    const bool translate           = w->getComboBoxComponent ("mode") != nullptr
                                        && w->getComboBoxComponent ("mode")->getText () == "Translate to English";
    const bool fast                = w->getComboBoxComponent ("speed") != nullptr
                                        && w->getComboBoxComponent ("speed")->getText ().startsWith ("Fast");
    const bool skipExisting        = w->getComboBoxComponent ("existing") != nullptr
                                        && w->getComboBoxComponent ("existing")->getText ().startsWith ("Skip");
    const juce::String channelText = w->getTextEditorContents ("channel");
    const bool autoDownload        = w->getComboBoxComponent ("autoDl") == nullptr
                                       || w->getComboBoxComponent ("autoDl")->getText () == "Yes";
    delete w;
    if (result != 1) return;

    int channel = app.lastTranscribeChannel;
    if (!Application::parseChannelText (channelText, channel))
    {
        juce::AlertWindow bad ("Invalid channel",
                               "The channel to analyse must be a whole number\n"
                               "(1-based; 0 or less mixes all channels).\n\n"
                               + channelText,
                               juce::AlertWindow::WarningIcon);
        bad.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        bad.enterModalState (false);
        bad.runModalLoop ();
        return;
    }

    // Map the display texts back to registry ids / language codes.
    const auto names = whisperModelNames ();
    const int mi = names.indexOf (modelText);
    if (mi < 0) return;
    const std::string modelId = whisperModelIds ()[(size_t) mi].toStdString ();

    const auto langNames = transcribeLanguageNames ();
    const int li = langNames.indexOf (langText);
    const std::string language = (li >= 0 ? transcribeLanguageCodes ()[(size_t) li] : juce::String ("auto")).toStdString ();

    // Remember the choices for next time.
    app.lastTranscribeModelId   = modelId;
    app.lastTranscribeLanguage  = language;
    app.lastTranscribeTranslate = translate;
    app.lastTranscribeFast      = fast;
    app.lastTranscribeChannel   = channel;
    app.aiAutoDownload          = autoDownload;
    app.saveAiSettings ();

    runBatchTranscribeAsync (folder, modelId, language, translate, fast, skipExisting,
                             channel, autoDownload);
}

// Batch transcription on a background thread with a cancellable progress
// dialog. Progress: model install [0, 5%), then each file gets its own slice.
void MainWindow::runBatchTranscribeAsync (const juce::File& folder,
                                          const std::string& modelId,
                                          const std::string& language, bool translate,
                                          bool fast, bool skipExisting, int channel,
                                          bool autoDownload)
{
    auto* dlg = new WorkProgressDialog ("Transcription",
                                        "Scanning " + folder.getFileName ()
                                        + " for WAV files ...", true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, folder, modelId, language, translate, fast, skipExisting,
                  channel, autoDownload, dlg] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v) {
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };

        juce::MessageManager::callAsync ([dlg, folder] {
            dlg->setStatusText ("Scanning " + folder.getFileName () + " for WAV files ...\n"
                                + "(CPU)\n(Cancel stops after the current file)");
        });

        BatchTranscribeSummary summary;
        std::string err;
        const bool ok = app.batchTranscribeFolder (
            folder, modelId, language, translate, fast, skipExisting, channel,
            [&sendProgress] (float f) { sendProgress ((double) f); return true; },
            [dlg] (const juce::String& name) {
                juce::MessageManager::callAsync ([dlg, name] {
                    dlg->setStatusText ("Transcribing " + name
                                        + " ...\n(CPU)\n(Cancel stops after the current file)");
                });
            },
            &dlg->cancelled,
            &summary,
            &err,
            autoDownload);

        juce::MessageManager::callAsync ([this, dlg, ok, err, modelId, summary] {
            delete dlg;
            if (!ok)
            {
                showAiFailureDialog ("Transcription", err, modelId, this, "CPU");
                return;
            }

            // Summary: what happened to each file.
            juce::String msg = juce::String (summary.total) + " WAV file(s):\n"
                               + juce::String (summary.done)    + " transcribed\n"
                               + juce::String (summary.skipped) + " skipped (already had SCTT)\n"
                               + juce::String (summary.failed)  + " failed";
            for (const auto& f : summary.failures)
                msg += "\n  - " + juce::String (f);

            auto* w = new juce::AlertWindow ("Transcription Finished",
                                             msg, juce::AlertWindow::InfoIcon);
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->enterModalState (false);
            w->runModalLoop ();
            delete w;
        });
    }).detach ();
}
#endif

// Keep the custom scrollbars in sync with the current view state. The
// setWindow() calls are no-ops when nothing changed, so this is safe to call
// from any of the feedback paths.
void MainWindow::updateScrollBars()
{
    const double dur = app.audioEngine().duration();
    if (timeBar)
    {
        timeBar->setDuration (dur);
        if (dur > 0.0) timeBar->setWindow (app.viewStart, app.viewEnd);
    }

    // Waveform: track is [-1, +1]; the thumb shows the visible amplitude range
    // centred on zero (the waveform view is always vertically centred).
    if (waveVScroll && waveformView && app.audioEngine().isFileLoaded())
    {
        waveVScroll->setRange (-1.0, 1.0);
        const double h = std::min (1.0, waveformView->getVisibleAmpHalf());
        waveVScroll->setWindow (-h, h);
    }

    // Spectrogram: track is [1 Hz, Nyquist]; the thumb shows the visible range.
    if (specVScroll && spectralView && app.audioEngine().isFileLoaded())
    {
        const double nyq = 0.5 * app.audioEngine().sampleRate();
        specVScroll->setRange (1.0, nyq);
        specVScroll->setWindow (spectralView->getFreqMin(), spectralView->getFreqMax());
    }
}

juce::PopupMenu MainWindow::getMenuForIndex (int menuIndex, const juce::String& menuName)
{
    juce::PopupMenu menu;
    using enum MenuIDs;

    if (menuName == "File")
    {
        menu.addItem (FileNew, "New...\tCtrl+Shift+N", true, false);
        menu.addItem (FileOpen, "Open...\tCtrl+O", true, false); // always available

        // Open Recent: up to 10 most recently opened files, most recent first.
        juce::PopupMenu recentMenu;
        if (app.recentFiles.empty())
            recentMenu.addItem (RecentBase, "(No recent files)", false, false);
        else
        {
            for (int i = 0; i < (int) app.recentFiles.size(); ++i)
                recentMenu.addItem (RecentBase + i,
                                     app.recentFiles[(size_t) i].getFileName(),
                                     true, false);
            recentMenu.addSeparator ();
            recentMenu.addItem (ClearRecent, "Clear Recent Files", true, false);
        }
        menu.addSubMenu ("Open Recent", recentMenu);

        menu.addItem (FileClose, "Close\tCtrl+W", app.audioEngine().isFileLoaded(), false);
        if (app.numDocs () > 0)
            menu.addItem (FileCloseAll, "Close All", true, false);
        menu.addSeparator();
        menu.addItem (FileSave, "Save\tCtrl+S", app.audioEngine().isFileLoaded(), false);
        menu.addItem (FileSaveAs, "Save As...\tCtrl+Shift+S", app.audioEngine().isFileLoaded(), false);

        // Metadata storage: where this file's markers/selections/transcription/
        // calibrations live - inside the WAV or in a .meta.json sidecar.
        {
            juce::PopupMenu metaMenu;
            FileDoc* d = app.doc();
            const bool hasDoc = d != nullptr && d->engine.isFileLoaded();
            const bool wavFile = d != nullptr
                && (d->file.getFullPathName().isEmpty()
                    || d->file.getFileExtension().equalsIgnoreCase (".wav"));
            // Save Metadata needs a real path: it writes the metadata in
            // place without touching the audio data.
            const bool canSaveMetaOnly = hasDoc && !d->file.getFullPathName().isEmpty();
            metaMenu.addItem (MetaSaveOnly, "Save Metadata", canSaveMetaOnly);
            metaMenu.addSeparator ();
            metaMenu.addItem (MetaEmbedded, "Embedded into Wav",
                              hasDoc && wavFile,
                              hasDoc && d->metaMode == sc::MetaMode::Embedded);
            metaMenu.addItem (MetaExternal, "External Json file", hasDoc,
                              hasDoc && d->metaMode == sc::MetaMode::External);
            menu.addSubMenu ("Metadata", metaMenu);
        }

        menu.addItem (FileExportSpectral, "Save Spectrogram As...\tCtrl+Shift+E",
                      app.audioEngine().isFileLoaded() && showSpectralView, false);
        menu.addSeparator();
        menu.addItem (FileQuit, "Quit\tCtrl+Q", true, false);
    }
    else if (menuName == "Edit")
    {
        menu.addItem (EditUndo, "Undo\tCtrl+Z", app.undo().canUndo(), false);
        menu.addItem (EditRedo, "Redo\tCtrl+Y", app.undo().canRedo(), false);
        menu.addSeparator();
        menu.addItem (EditCut, "Cut\tCtrl+X", !app.selection.isEmpty(), false);
        menu.addItem (EditCopy, "Copy\tCtrl+C", !app.selection.isEmpty(), false);
        menu.addItem (EditCopyToNewFile, "Copy to New File",
                      app.audioEngine().isFileLoaded()
                          && app.audioEngine().numSamples() > 0,
                      false);
        menu.addItem (EditPaste, "Paste\tCtrl+V", app.canPaste(), false);
        menu.addItem (EditPasteMix, "Paste Mix\tCtrl+Shift+V", app.canPaste(), false);
        menu.addItem (EditPasteToNewChannels, "Paste to New Channel(s)",
                      app.canPaste(), false);
        // With a spectral selection active, DEL zeros the selected (t,f)
        // content instead of trimming samples.
        const bool hasSpectralSel = app.hasSpectralSelection ();
        menu.addItem (EditDelete,
                      hasSpectralSel ? "Zero Spectral Selection\tDel" : "Delete\tDel",
                      !app.selection.isEmpty() || hasSpectralSel, false);
        menu.addItem (EditSelectAll, "Select All\tCtrl+A", app.audioEngine().isFileLoaded(), false);
        menu.addSeparator();
        menu.addItem (EditSnapToZeroCrossings, "Snap to Zero Crossings", true, app.settings.snapToZeroCrossings);
    }
    else if (menuName == "Selection")
    {
        const bool hasFile = app.audioEngine().isFileLoaded();

        menu.addItem (SelectionToolTimeRange, "Time Range\tS", true,
                      app.selMode == SpectralSelMode::TimeRange);
        menu.addItem (SelectionToolMarquee, "Marquee\tM", hasFile,
                      app.selMode == SpectralSelMode::Marquee);
        menu.addItem (SelectionToolLasso, "Lasso\tL", hasFile,
                      app.selMode == SpectralSelMode::Lasso);
        menu.addItem (SelectionToolPaintbrush,
                      juce::String ("Paintbrush (") +
                          juce::String ((int) std::lround (app.brushRadiusHz)) +
                          " Hz)\tP",
                      hasFile, app.selMode == SpectralSelMode::Paintbrush);
        menu.addSeparator ();
        menu.addItem (SelectionBrushSmaller, "Brush Smaller\t[", hasFile, false);
        menu.addItem (SelectionBrushLarger, "Brush Larger\t]", hasFile, false);
        menu.addSeparator ();
        menu.addItem (SelectionClear, "Clear Spectral Selection\tEsc",
                      app.hasSpectralSelection (), false);
    }
    else if (menuName == "View")
    {
        menu.addItem (ViewZoomIn, "Zoom In\tCtrl++", true, false);
        menu.addItem (ViewZoomOut, "Zoom Out\tCtrl+-", true, false);
        menu.addItem (ViewZoomFit, "Zoom Fit\tCtrl+0", app.audioEngine().isFileLoaded(), false);
        menu.addItem (ViewZoomToSelection, "Zoom to Selection", !app.selection.isEmpty(), false);
        menu.addItem (ViewZoomOutFull, "Zoom Out Full", app.audioEngine().isFileLoaded(), false);
        menu.addSeparator();
        menu.addItem (ViewVerticalZoomIn, "Vertical Zoom In\tCtrl+Shift+=", true, false);
        menu.addItem (ViewVerticalZoomOut, "Vertical Zoom Out\tCtrl+Shift+-", true, false);
        menu.addSeparator();
        menu.addItem (ViewShowWaveform, "Waveform View", true, showWaveformView);
        menu.addItem (ViewShowSpectral, "Spectrogram View", true, showSpectralView);
        menu.addItem (ViewShowAllChannels, "Show All Channels", app.audioEngine().isFileLoaded(), false);
        menu.addItem (ViewShowLeftPanels, "Left Panels", true, leftPanelsVisible);
        menu.addSeparator();
        juce::PopupMenu colourMapMenu;
        colourMapMenu.addItem (ColourMapClassic, "Classic (Default)", true, app.colourMap == ColourMap::Classic);
        colourMapMenu.addItem (ColourMapViridis, "Viridis", true, app.colourMap == ColourMap::Viridis);
        colourMapMenu.addItem (ColourMapPlasma, "Plasma", true, app.colourMap == ColourMap::Plasma);
        colourMapMenu.addItem (ColourMapInferno, "Inferno", true, app.colourMap == ColourMap::Inferno);
        colourMapMenu.addItem (ColourMapMagma, "Magma", true, app.colourMap == ColourMap::Magma);
        colourMapMenu.addItem (ColourMapHot, "Hot", true, app.colourMap == ColourMap::Hot);
        colourMapMenu.addItem (ColourMapCool, "Cool", true, app.colourMap == ColourMap::Cool);
        colourMapMenu.addItem (ColourMapGrayscale, "Grayscale", true, app.colourMap == ColourMap::Grayscale);
        menu.addSubMenu ("Colour Map", colourMapMenu);

        // Per-channel visibility toggles (applies to both views).
        // Ctrl/Shift+click shows only that channel.
        const int nch = app.audioEngine().numChannels();
        if (nch > 0)
        {
            menu.addSeparator();
            for (int i = 0; i < nch; ++i)
                menu.addItem (ChannelBase + i, "Channel " + juce::String (i + 1), true,
                              app.isChannelVisible (i));
        }
    }
    else if (menuName == "Transport")
    {
        menu.addItem (TransportPlay, "Play\tSpace", app.audioEngine().isFileLoaded() && !app.audioEngine().isPlaying(), false);
        menu.addItem (TransportStop, "Stop\tCtrl+.", app.audioEngine().isPlaying(), false);
        menu.addItem (TransportPause, "Pause\tSpace", app.audioEngine().isPlaying(), false);
        menu.addSeparator();
        menu.addItem (TransportLoop, "Loop (Selection/All)\tCtrl+L", true, app.audioEngine().isLooping());
        // Play only the selected spectral content (mask applied in real time).
        menu.addItem (TransportPlaySpectralSel, "Play Spectral Selection",
                      app.hasSpectralSelection (),
                      app.playSpectralSel && app.hasSpectralSelection ());
    }
    else if (menuName == "Effects")
    {
        const bool hasSel = !app.selection.isEmpty();
        menu.addItem (ToolsSilence, "Silence Selection", hasSel, false);
        menu.addItem (ToolsFadeIn, "Fade In", hasSel, false);
        menu.addItem (ToolsFadeOut, "Fade Out", hasSel, false);
        menu.addItem (ToolsNormalize, "Normalize", hasSel, false);
        menu.addItem (ToolsChangeGain, "Change Gain...", hasSel, false);
        menu.addItem (ToolsReverse, "Reverse", hasSel, false);
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        // AI effects: first use downloads the model weights.
        menu.addSeparator();
        menu.addItem (ToolsNoiseSuppress, "Noise Suppression...", hasSel, false);
        // Whole-file operation (no selection needed).
        menu.addItem (ToolsTranscribe, "Transcribe...", app.audioEngine().isFileLoaded(), false);
        // Folder batch: no file needs to be open at all.
        menu.addItem (ToolsTranscribeFolder, "Transcribe Folder...", true, false);
#endif
        menu.addSeparator();
        // Whole-file operation. Streamed files are materialised into memory
        // first when the resample actually runs.
        menu.addItem (ToolsResample, "Resample...",
                      app.audioEngine().isFileLoaded(), false);
    }
    else if (menuName == "Window")
    {
        // One entry per left-column panel, in canonical order; a checkmark
        // shows which panels are currently open.
        const auto entries = leftPanelEntries ();
        for (size_t i = 0; i < entries.size (); ++i)
            menu.addItem ((int) (WindowPanelBase + i), entries[i].name, true,
                          isPanelOpen (entries[i].id));
    }
    else if (menuName == "Help")
    {
        menu.addItem (HelpAbout, "About SpectroCraft", true, false);
    }

    return menu;
}

juce::StringArray MainWindow::getMenuBarNames()
{
    return { "File", "Edit", "Selection", "View", "Transport", "Effects",
             "Window", "Help" };
}

// The list panel (Markers or Transcription) currently shown in a left-panel
// tab, or nullptr when another tab is active. Panels can live in either of
// the two tab bars (they are draggable between them).
juce::Component* MainWindow::activeListPanel() const
{
    juce::Component* cur = bottomTabs.getCurrentContentComponent ();
    if (cur == nullptr) cur = topTabs.getCurrentContentComponent ();
    return cur;
}

// Ctrl+F / F3: search the active list panel. With no query set yet, open the
// Find dialog first; otherwise jump to the next match.
void MainWindow::handleFind (bool openDialog)
{
    juce::Component* cur = activeListPanel ();

    if (cur == markersPanel.get ())
    {
        if (openDialog || markersPanel->getFindQuery ().isEmpty ())
            markersPanel->showFindDialog ();
        else
            markersPanel->findNext ();
        return;
    }
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    if (cur == transcriptionPanel.get ())
    {
        if (openDialog || transcriptionPanel->getFindQuery ().isEmpty ())
            transcriptionPanel->showFindDialog ();
        else
            transcriptionPanel->findNext ();
    }
#endif
}

// Ctrl+F3: previous match in the active list panel (dialog first when no query).
void MainWindow::handleFindPrevious ()
{
    juce::Component* cur = activeListPanel ();

    if (cur == markersPanel.get ())
    {
        if (markersPanel->getFindQuery ().isEmpty ())
            markersPanel->showFindDialog ();
        else
            markersPanel->findPrevious ();
        return;
    }
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    if (cur == transcriptionPanel.get ())
    {
        if (transcriptionPanel->getFindQuery ().isEmpty ())
            transcriptionPanel->showFindDialog ();
        else
            transcriptionPanel->findPrevious ();
    }
#endif
}

bool MainWindow::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    // Ctrl+Tab / Ctrl+Shift+Tab cycles through the loaded files.
    if (key.getKeyCode() == juce::KeyPress::tabKey && key.getModifiers().isCommandDown())
    {
        app.cycleActiveDoc (!key.getModifiers().isShiftDown());
        return true;
    }
    if (key == juce::KeyPress ('n', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { showNewFileDialog(); return true; }
    if (key == juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0)) { handleFileOpen(); return true; }
    if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0)) { handleFileSave(); return true; }
    if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { handleFileSaveAs(); return true; }
    if (key == juce::KeyPress ('w', juce::ModifierKeys::commandModifier, 0)) { app.closeFile(); return true; }
    // Ctrl+F4 also closes the active file (browser-style tab close).
    if (key == juce::KeyPress (juce::KeyPress::F4Key, juce::ModifierKeys::commandModifier, 0)) { app.closeFile(); return true; }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0)) { app.undoStep(); refreshAfterEdit(); return true; }
    if (key == juce::KeyPress ('y', juce::ModifierKeys::commandModifier, 0)) { app.redoStep(); refreshAfterEdit(); return true; }
    if (key == juce::KeyPress ('x', juce::ModifierKeys::commandModifier, 0)) { runDestructiveEdit ([&] (std::string* e) { return app.cut (e); }); return true; }
    if (key == juce::KeyPress ('c', juce::ModifierKeys::commandModifier, 0)) { app.copy(); return true; }   // read-only
    if (key == juce::KeyPress ('v', juce::ModifierKeys::commandModifier, 0)) { runDestructiveEdit ([&] (std::string* e) { return app.paste (e); }); return true; }
    if (key == juce::KeyPress ('v', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { runDestructiveEdit ([&] (std::string* e) { return app.pasteMix (e); }); return true; }
    if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0)) { app.selectAll(); syncSharedState(); return true; }
    // Find in the active list panel (Markers / Transcription tab).
    if (key == juce::KeyPress ('f', juce::ModifierKeys::commandModifier, 0)) { handleFind (true); return true; }
    // Go to Time: jump the playhead via any of the active time units.
    if (key == juce::KeyPress ('g', juce::ModifierKeys::commandModifier, 0)) { showGoToTimeDialog (); return true; }

    // Drawing tools (ignored while a text editor has focus): t = Text,
    // a = Arrow, r = Rectangle, d = Free Drawing. Esc cancels an armed or
    // in-progress creation.
    if (!key.getModifiers().isCommandDown() && !key.getModifiers().isAltDown())
    {
        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        const bool inEditor = dynamic_cast<juce::TextEditor*> (focused) != nullptr;
        if (!inEditor)
        {
            // Windows reports letter key codes in uppercase; normalise like the
            // selection-tool shortcuts below.
            const int kc = (key.getKeyCode () >= 'A' && key.getKeyCode () <= 'Z')
                               ? key.getKeyCode () + 32 : key.getKeyCode ();
            switch (kc)
            {
                case 't': if (spectralView) spectralView->startPendingDraw (DrawingItem::Kind::Text); return true;
                case 'a': if (spectralView) spectralView->startPendingDraw (DrawingItem::Kind::Arrow); return true;
                case 'r': if (spectralView) spectralView->startPendingDraw (DrawingItem::Kind::Rectangle); return true;
                case 'd': if (spectralView) spectralView->startPendingDraw (DrawingItem::Kind::Freehand); return true;
                default: break;
            }
        }
    }
    if (key.getKeyCode() == juce::KeyPress::escapeKey && spectralView != nullptr
        && (spectralView->isPendingDraw () || spectralView->isDrawingNew ()))
    {
        spectralView->cancelPendingDraw();
        return true;
    }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) { runDestructiveEdit ([&] (std::string* e) { return app.del (e); }); return true; }
    if (key == juce::KeyPress::spaceKey)
    {
        // Space toggles play/pause (not stop).
        if (app.audioEngine().isPlaying()) app.pause(); else playWithAudioInit();
        return true;
    }
    // Arrow keys nudge the playhead by one frame. Holding Shift selects from
    // where the shift-drag started to the current playhead position (the anchor
    // is cleared automatically once Shift is released - see timerCallback).
    // Note: KeyPress::operator== compares modifiers too, so compare key codes.
    const int keyCode = key.getKeyCode();
    const juce::ModifierKeys mods = key.getModifiers();

    // Channel visibility, like the View menu items: Ctrl+1..9 toggles that
    // channel in both views; Ctrl+Shift+1..9 shows ONLY it (solo), matching
    // the menu's Ctrl/Shift-click behaviour.
    if (mods.isCommandDown() && !mods.isAltDown() && keyCode >= '1' && keyCode <= '9')
    {
        const int ch = keyCode - '1';   // 0-based channel index
        if (app.audioEngine().isFileLoaded() && ch < app.audioEngine().numChannels())
        {
            if (mods.isShiftDown())
                app.visibleChannels = { ch };
            else
                app.toggleChannelVisible (ch);
            app.syncActiveDoc ();   // keep the active document's copy in step
            syncSharedState();
            return true;
        }
    }

    if (app.audioEngine().isFileLoaded() && !mods.isAltDown()
            && (keyCode == juce::KeyPress::leftKey || keyCode == juce::KeyPress::rightKey))
    {
        // Plain / Shift arrows step by one percent of the visible time span
        // per press, so zooming in makes the steps smaller until deep zoom
        // reaches exactly one sample; Ctrl always moves exactly one sample.
        const bool ctrl = mods.isCommandDown() || mods.isCtrlDown();
        const double minStep = 1.0 / app.audioEngine().sampleRate();
        const double step = ctrl ? minStep
                                 : std::max (minStep,
                                             (app.viewEnd - app.viewStart) / 100.0);
        const bool right = (keyCode == juce::KeyPress::rightKey);

        if (mods.isShiftDown())
        {
            // Anchor on the first Shift+arrow press; keep it while Shift is held.
            if (shiftAnchorTime < 0.0)
                shiftAnchorTime = app.playhead;

            const double newPlayhead = app.playhead + (right ? step : -step);
            app.setPlayhead (newPlayhead);
            app.setSelection ({ std::min (shiftAnchorTime, app.playhead),
                                std::max (shiftAnchorTime, app.playhead) });
        }
        else
        {
            shiftAnchorTime = -1.0; // plain move: no selection involved
            app.setPlayhead (app.playhead + (right ? step : -step));
        }
        return true;
    }
    if (key == juce::KeyPress::homeKey || key == juce::KeyPress::endKey)
    {
        // Move the playhead AND pan both views to the very start/end of the
        // file, keeping the current zoom span.
        const bool home = (key == juce::KeyPress::homeKey);
        const double dur  = app.audioEngine().duration();
        const double span = std::max (1e-6, app.viewEnd - app.viewStart);

        if (dur > 0.0)
            applyTimeWindow (home ? 0.0 : std::max (0.0, dur - span),
                             home ? std::min (span, dur) : dur);
        app.setPlayhead (home ? 0.0 : dur);
        syncSharedState();
        return true;
    }
    // Horizontal zoom: Ctrl+= / Ctrl+-   (Shift+ = produces '>', Shift+ - still '-')
    if (key == juce::KeyPress ('=', juce::ModifierKeys::commandModifier, 0)) { zoomHorizontal (1.5); return true; }
    if (key == juce::KeyPress ('-', juce::ModifierKeys::commandModifier, 0)) { zoomHorizontal (1.0 / 1.5); return true; }
    // Vertical zoom: Ctrl+Shift+= / Ctrl+Shift+- (waveform amplitude + spectral frequency)
    if (key == juce::KeyPress ('>', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { zoomVerticalBoth (1.5); return true; }
    if (key == juce::KeyPress ('-', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { zoomVerticalBoth (1.0 / 1.5); return true; }
    if (key == juce::KeyPress ('0', juce::ModifierKeys::commandModifier, 0)) { zoomToFit(); return true; }
    // Selection tools: S = Time Range, M = Marquee, L = Lasso, P = Paintbrush.
    // (M and L were reassigned from "add marker" / "loop", which moved to
    // F2/F8 and Ctrl+L respectively.)
    if (!mods.isCommandDown() && !mods.isAltDown())
    {
        // Windows reports letter key codes in uppercase; normalise for the
        // switch below (KeyPress::operator== does this, raw getKeyCode() does not).
        const int kc = (keyCode >= 'A' && keyCode <= 'Z') ? keyCode + 32 : keyCode;
        switch (kc)
        {
            case 's': app.setSelectionTool (SpectralSelMode::TimeRange); syncSharedState(); return true;
            case 'm': app.setSelectionTool (SpectralSelMode::Marquee);   syncSharedState(); return true;
            case 'l': app.setSelectionTool (SpectralSelMode::Lasso);     syncSharedState(); return true;
            case 'p': app.setSelectionTool (SpectralSelMode::Paintbrush);syncSharedState(); return true;
            case '[': app.stepBrushRadius (false);
                     syncSharedState(); return true;
            case ']': app.stepBrushRadius (true);
                     syncSharedState(); return true;
            default: break;
        }
    }

    // ESC ends an active move/scale gesture first, then a clone-picking
    // session; otherwise it clears the active spectral selection (time
    // selection is untouched).
    if (keyCode == juce::KeyPress::escapeKey)
    {
        if (app.cancelSelGesture ())
            return true;
        if (app.isClonePicking ())
        {
            app.endClonePick ();
            return true;
        }
        if (app.hasSpectralSelection ())
        {
            app.clearSpectralSelection ();
            return true;
        }
    }

    // Find next / previous in the active list panel. F3 used to toggle the
    // loop; that moved to Ctrl+L (below).
    if (key == juce::KeyPress (juce::KeyPress::F3Key, juce::ModifierKeys::noModifiers, 0))
    {
        handleFind (false);
        return true;
    }
    if (key == juce::KeyPress (juce::KeyPress::F3Key, juce::ModifierKeys::commandModifier, 0))
    {
        handleFindPrevious ();
        return true;
    }
    if (key == juce::KeyPress ('l', juce::ModifierKeys::commandModifier, 0))
    {
        app.toggleLoop();
        return true;
    }
    if (key == juce::KeyPress (juce::KeyPress::F2Key, juce::ModifierKeys::noModifiers, 0)
        || key == juce::KeyPress (juce::KeyPress::F8Key, juce::ModifierKeys::noModifiers, 0))
    {
        // Region marker spanning the selection if there is one,
        // otherwise a point marker at the cursor. (Markers persist with the
        // file's unified metadata on Save / mode switch - no live sidecar.)
        app.addMarkerAtCursor();
        if (markersPanel) markersPanel->refresh();
        syncSharedState();
        return true;
    }

    // F9 cycles the view layout (both -> waveform only -> spectrogram only);
    // Shift+F9 goes the other way around.
    if (keyCode == juce::KeyPress::F9Key && !mods.isAltDown() && !mods.isCommandDown())
    {
        cycleViewVisibility (!mods.isShiftDown());
        return true;
    }

    return false;
}
