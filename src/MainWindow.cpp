/**
 * SpectroCraft - Main Window Implementation
 * Layout: menu bar on top; left column with two tab groups
 *   (top: Files | Frequency Analysis, bottom: Spectral Controls | Markers);
 * main area: waveform view on top, spectral view below.
 */

#include "MainWindow.h"
#include "Application.h"
#include <thread>
#include "WaveformView.h"
#include "SpectralView.h"
#include "Panels.h"
#include "MarkerManager.h"
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    #include "AI/ModelManager.h"
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

MainWindow::MainWindow (Application& appIn, const juce::String& name)
    : DocumentWindow (name,
                      juce::Desktop::getInstance().getDefaultLookAndFeel()
                          .findColour (juce::ResizableWindow::backgroundColourId),
                      DocumentWindow::allButtons),
      app (appIn)
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
    historyPanel  = std::make_unique<HistoryPanel> (app);

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
    bottomTabs.addTab ("Spectral Controls", kDarkTabBg, controlsPanel.get(), false);
    bottomTabs.addTab ("Markers", kDarkTabBg, markersPanel.get(), false);
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    bottomTabs.addTab ("Transcription", kDarkTabBg, transcriptionPanel.get(), false);
    if (transcriptionPanel)
        transcriptionPanel->onRunRequested = [this] { showTranscribeDialog (); };
#endif
    // Frequency Analysis is the default tab in the top group.
    topTabs.setCurrentTabIndex (1, false);
    bottomTabs.setCurrentTabIndex (0, false);

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
    leftSplitter->setTooltip ("Drag to adjust the width of the left panel column.");
    topSplitter = std::make_unique<ViewSplitter>();
    topSplitter->onDrag = [this] (const juce::MouseEvent& e) { handleTopSplitterDrag (e); };
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

    setupViews();
    updateTitle();   // a file may already be loaded (opened from the CLI)
    if (statusBar)   // ...and the footer must show its name too
        statusBar->setFileName (app.audioEngine().isFileLoaded()
                                ? app.audioEngine().getFile().getFileName()
                                : juce::String());

    // Panels notify us when they change shared state
    app.onStateChange = [this] { syncSharedState(); };

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
            const juce::Rectangle<int> safe ((int) std::lround (mainD->userBounds.getX()),
                                             (int) std::lround (mainD->userBounds.getY()),
                                             (int) std::lround (mainD->userBounds.getWidth()),
                                             (int) std::lround (mainD->userBounds.getHeight()));
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

    setVisible (true);
}

MainWindow::~MainWindow() {}

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
    // between the top and bottom tab groups.
    auto rest = bounds;
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
    topTabs.setBounds (col.removeFromTop (topH));
    col.removeFromTop (splitW);   // strip for the horizontal divider
    bottomTabs.setBounds (col);
    if (topSplitter)
        topSplitter->setBounds (leftArea.getX(), leftArea.getY() + topH, leftW, splitW);

    // Main area: waveform on top, draggable splitter, spectral below; a time
    // scrollbar along the bottom and one vertical scrollbar per view on its
    // right edge. Either view can be hidden from the View menu; at least one
    // is always visible and takes the whole main area when it is alone.
    const bool showWave = showWaveformView && (waveformView != nullptr);
    const bool showSpec = showSpectralView && (spectralView != nullptr);
    if (waveformView) waveformView->setVisible (showWave);
    if (spectralView) spectralView->setVisible (showSpec);

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
    // Cancel a pending Shift+arrow selection anchor once Shift is released
    // (JUCE has no key-up callback, so poll the live modifier state).
    if (shiftAnchorTime >= 0.0 && !juce::ModifierKeys::getCurrentModifiers().isShiftDown())
        shiftAnchorTime = -1.0;

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

bool MainWindow::isInterestedInFileDrag (const juce::StringArray& files)
{
    static const juce::String supported = ".wav .aiff .aif .flac .mp3 .ogg .m4a";
    for (const auto& f : files)
        if (supported.contains (juce::File (f).getFileExtension().toLowerCase()))
            return true;
    return false;
}

void MainWindow::fileDragEnter (const juce::StringArray&, int, int) { repaint(); }
void MainWindow::fileDragExit (const juce::StringArray&) { repaint(); }

void MainWindow::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& f : files)
    {
        const juce::File file (f);
        if (file.existsAsFile())
            openFile (file);   // multi-file editor: every valid drop becomes a document
    }
}

// ------------------------------------------------------------- state sync

// Push shared state (selection, channels, playhead, settings) into the views.
// Time windows and zoom levels are owned by each view independently.
void MainWindow::syncSharedState()
{
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
        statusBar->setSelection (app.selection.getStart(), app.selection.getEnd());
        statusBar->setPlayhead (app.playhead);
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
        : juce::AlertWindow (title, message, juce::AlertWindow::NoIcon, centreAround),
          bar (progressValue)   // JUCE 9: the bar watches this variable
    {
        // The default LookAndFeel tables define no ProgressBar colours, which
        // resolve to transparent - set them explicitly so the bar is visible
        // on the dark dialog background.
        bar.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff232327));
        bar.setColour (juce::ProgressBar::foregroundColourId, juce::Colours::dodgerblue);

        addChildComponent (&bar);

        // The bar's self-polling timer only starts when visibility *changes*;
        // toggle it once so progress updates are picked up from the start.
        bar.setVisible (false);
        bar.setVisible (true);

        if (withCancel)
        {
            addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
            // The AlertWindow's own click listener exits the modal state when
            // the button is pressed; this flag tells the worker thread to stop.
            for (auto* c : getChildren())
                if (auto* b = dynamic_cast<juce::TextButton*> (c))
                    if (b->getButtonText () == "Cancel")
                        b->onClick = [this] { cancelled.store (true); };
        }

        // Grow the window so the bar has its own strip below the message.
        setSize (getWidth(), getHeight() + 34);
    }

    // Set by the Cancel button; the worker thread checks it between files.
    std::atomic<bool> cancelled { false };

    void setProgress (double v) { progressValue = v; }

    // Update the message text (call on the message thread).
    void setStatusText (const juce::String& s) { setMessage (s); }

    void resized() override
    {
        // The bar gets its own strip along the bottom of the window.
        bar.setBounds (16, getHeight() - 28, getWidth() - 32, 20);
    }

private:
    double progressValue = 0.0;   // declared before bar: member init order
    juce::ProgressBar bar;
};

void MainWindow::openFileWithProgress (const juce::File& file)
{
    auto* dlg = new WorkProgressDialog ("Loading", "Opening " + file.getFileName() + " ...",
                                        false, this);

    // Modal but without stealing keyboard focus: the dialog just sits there
    // while the worker thread reports progress.
    dlg->enterModalState (false);

    app.loadFileAsync (file,
        [dlg] (float frac) {
            juce::MessageManager::callAsync ([dlg, frac]
            { dlg->setProgress ((double) frac); });
        },
        [dlg] (bool) { delete dlg; });
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
    }

    syncSharedState();
    updateTitle();

    // The footer always shows the name of the file currently in the views.
    if (statusBar)
        statusBar->setFileName (app.audioEngine().isFileLoaded()
                                ? app.audioEngine().getFile().getFileName()
                                : juce::String());

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
        "*.wav;*.aiff;*.aif;*.flac;*.mp3;*.ogg;*.m4a",
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

    std::thread ([this, file, saveAs, dlg] {
        const bool ok = saveAs ? app.audioEngine().saveFileAs (file)
                               : app.audioEngine().saveFile (file);
        juce::MessageManager::callAsync ([this, dlg, ok] {
            delete dlg;
            if (ok) updateTitle();
        });
    }).detach();
}

void MainWindow::handleFileSave()
{
    if (app.audioEngine().isFileLoaded())
        runSaveAsync (app.audioEngine().getFile(), false);
    else
        handleFileSaveAs();
}

void MainWindow::handleFileSaveAs()
{
    juce::FileChooser chooser ("Save Audio File As",
        juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.wav;*.aiff;*.aif;*.flac;*.ogg",
        kUseNativeFileChooser);

    if (chooser.browseForFileToSave (true))
        runSaveAsync (chooser.getResult(), true);
}

// Render + write the spectrogram PNG on a background thread.
void MainWindow::exportSpectralAsync (const juce::File& file)
{
    auto* dlg = new WorkProgressDialog ("Exporting",
                                        "Rendering spectrogram to " + file.getFileName() + " ...",
                                        false, this);
    dlg->setProgress (-1.0);   // out of range -> indeterminate spinning bar
    dlg->enterModalState (false);

    std::thread ([this, file, dlg] {
        const bool ok = spectralView ? spectralView->saveAsPng (file) : false;
        juce::MessageManager::callAsync ([dlg, ok] { delete dlg; });
    }).detach();
}

void MainWindow::updateTitle()
{
    juce::String title = "SpectroCraft";
    if (app.audioEngine().isFileLoaded())
    {
        title = app.audioEngine().getFile().getFileName() + " - " + title;
        if (app.audioEngine().hasUnsavedChanges())
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


// ------------------------------------------------------------------- menus

void MainWindow::menuItemSelected (int menuItemID, int topLevelMenuIndex)
{
    using enum MenuIDs;

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

    switch (menuItemID)
    {
        case FileOpen: handleFileOpen(); break;
        case FileClose:
            // closeFile() fires onActiveDocChanged (or resets the empty state),
            // which refreshes every panel and view.
            app.closeFile();
            break;
        case FileSave: handleFileSave(); break;
        case FileSaveAs: handleFileSaveAs(); break;
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
        case EditCut: app.cut(); refreshAfterEdit(); break;
        case EditCopy: app.copy(); break;
        case EditPaste: app.paste(); refreshAfterEdit(); break;
        case EditDelete: app.del(); refreshAfterEdit(); break;
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

        case TransportPlay: app.play(); break;
        case TransportStop: app.stop(); break;
        case TransportPause: app.pause(); break;
        case TransportLoop: app.toggleLoop(); break;

        case ToolsSilence: app.silence(); refreshAfterEdit(); break;
        case ToolsFadeIn: app.fadeIn(); refreshAfterEdit(); break;
        case ToolsFadeOut: app.fadeOut(); refreshAfterEdit(); break;
        case ToolsNormalize: app.normalize(); refreshAfterEdit(); break;
        case ToolsChangeGain: showChangeGainDialog(); break;
        case ToolsResample: showResampleDialog(); break;
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        case ToolsNoiseSuppress: showNoiseSuppressDialog(); break;
        case ToolsTranscribe: showTranscribeDialog(); break;
        case ToolsTranscribeFolder: showTranscribeFolderDialog(); break;
#endif
        case ToolsReverse: app.reverse(); refreshAfterEdit(); break;

        case HelpAbout:
            juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon,
                "About SpectroCraft",
                "SpectroCraft v" SC_VERSION_STRING "\nHigh-performance waveform + spectral editor\n"
                "\nThis software is licensed under the GNU Affero General Public License v3.0.\n"
                "\nSource code is available at:\nhttps://github.com/SuperMax3d/SpectroCraft\n"
                "\nThis software uses the JUCE framework (https://juce.com), dual-licensed under "
                "AGPLv3 and a commercial licence. This project uses the AGPLv3 option.",
                "OK", this);
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
    applyTimeWindow (0.0, app.audioEngine().duration());
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

// Actions from the right-click context menu shown by either view.
void MainWindow::handleContextMenuAction (int id)
{
    using enum ContextMenuIDs;
    switch (id)
    {
        case CtxZoomToSelection: zoomToSelection(); break;
        case CtxZoomOutFull:     zoomOutFull(); break;
        case CtxSelectAll:       app.selectAll(); break; // notifies via setSelection()
        case CtxCut:             app.cut();   refreshAfterEdit(); break;
        case CtxCopy:            app.copy();  break;
        case CtxPaste:           app.paste(); refreshAfterEdit(); break;
        case CtxDelete:          app.del();   refreshAfterEdit(); break;
    }
}

// Effects > Change Gain... — prompt for a dB value and apply it to the
// selection on the visible channels (undoable).
juce::AlertWindow* MainWindow::createGainDialog()
{
    auto* w = new juce::AlertWindow ("Change Gain",
                                     "Apply gain to the selection (dB):",
                                     juce::AlertWindow::QuestionIcon, this);
    w->addTextEditor ("gainDb", "0.0");
    if (auto* editor = w->getTextEditor ("gainDb"))
        // Let Enter/Escape reach the OK/Cancel shortcuts even while the field
        // has keyboard focus.
        editor->setEscapeAndReturnKeysConsumed (false);

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
    app.changeGain (db);
    refreshAfterEdit();
}

// Effects > Resample... — prompt for a new sample rate and resample the whole
// file (SoXR, same engine as Audacity). Not undoable: the dialog warns about it.
juce::AlertWindow* MainWindow::createResampleDialog()
{
    const double cur = app.audioEngine().sampleRate();
    auto* w = new juce::AlertWindow ("Resample",
                                     "Current rate: " + juce::String (cur, 3) + " Hz\n"
                                     "New sample rate (Hz):\n"
                                     "(whole file; cannot be undone)",
                                     juce::AlertWindow::QuestionIcon, this);
    w->addTextEditor ("newRate", juce::String (cur, 3));
    if (auto* editor = w->getTextEditor ("newRate"))
        // Let Enter/Escape reach the OK/Cancel shortcuts even while the field
        // has keyboard focus.
        editor->setEscapeAndReturnKeysConsumed (false);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // AlertWindow disables keyboard focus on its editors, so re-enable it and
    // pre-select the text. The actual grabKeyboardFocus() happens in
    // showResampleDialog() once the window is showing.
    if (auto* editor = w->getTextEditor ("newRate"))
    {
        editor->setWantsKeyboardFocus (true);
        editor->selectAll();
    }

    return w;
}

void MainWindow::showResampleDialog()
{
    auto& eng = app.audioEngine();
    if (!eng.isFileLoaded() || !eng.fullyInMemory()) return;

    auto* w = createResampleDialog();

    // Enter modal state WITHOUT taking keyboard focus (see showChangeGainDialog).
    w->enterModalState (false);

    if (auto* editor = w->getTextEditor ("newRate"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop();
    const juce::String text = w->getTextEditorContents ("newRate");
    delete w;
    if (result != 1) return;

    const double newRate = text.trim().getDoubleValue();
    if (!std::isfinite (newRate) || newRate <= 0.0)
    {
        juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Invalid rate",
                                            "Please enter a positive sample rate in Hz.", "", this);
        return;
    }

    runResampleAsync (newRate);
}

// Resample on a background thread with a progress dialog so multi-GB files
// never block the UI. Progress: resampling 0..85%, mipmap rebuild 85..100%.
void MainWindow::runResampleAsync (double newRate)
{
    auto* dlg = new WorkProgressDialog ("Resampling",
                                        "Resampling to " + juce::String (newRate, 3) + " Hz ...",
                                        false, this);
    dlg->setProgress (0.0);
    dlg->enterModalState (false);

    std::thread ([this, newRate, dlg] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v) {
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };

        const bool ok = app.audioEngine().resampleTo (
            newRate, [sendProgress] (float f) { sendProgress (0.85 * (double) f); });
        if (ok)
            app.audioEngine().rebuildMipmaps ([sendProgress] (float f) {
                sendProgress (0.85 + 0.15 * (double) f);
            });

        juce::MessageManager::callAsync ([this, dlg, ok] {
            delete dlg;
            if (ok)
            {
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
            else
            {
                juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                                    "Resample failed",
                                                    "The file could not be resampled.", "", this);
            }
        });
    }).detach();
}

// Effects > Noise Suppression... — AI effect using the denseunet or
// DeepFilterNet2/3 models (Phases 1–2 of the AI plan). First use downloads
// the model weights (~9 MB for DenseUNet, ~15 MB per DeepFilterNet variant).
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
std::string sc::buildNsFailureDetails (const std::string& err, const std::string& modelId)
{
    juce::String details = err.empty () ? "The selection could not be processed." : err;
    auto& mm = sc::ai::ModelManager::instance ();
    // When the OpenVINO runtime itself is missing, pointing the user at model
    // downloads would be a trap (the weights are irrelevant until the DLLs
    // load), so that case gets its own instructions instead.
    const bool runtimeMissing = details.startsWith ("OpenVINO runtime unavailable");
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
            details += "\n\nThe OpenVINO runtime DLLs must sit in the same folder as SpectroCraft.exe:\n"
                       "  openvino.dll\n"
                       "  openvino_intel_cpu_plugin.dll\n"
                       "  openvino_ir_frontend.dll\n"
                       "  tbb12.dll\n"
                       "\nWhere to get them:\n"
                       "- If you built SpectroCraft from source: extern/OpenVINO/bin/ in the repository.\n"
                       "- Otherwise download OpenVINO 2024.6 from https://github.com/openvinotoolkit/openvino/releases\n"
                       "  and take the DLLs from its runtime\\Windows\\bin folder.";
        }
        else if (looksModelRelated)
        {
            details += "\n\nManual installation: download these files into:\n"
                       + mm.dirFor (spec->id).getFullPathName () + "\n";
            if (!spec->zip.url.empty ())
                // Zip-distributed model (e.g. Whisper): one archive holds all files.
                details += "  " + juce::String (spec->zip.url) + "\n";
            else
                for (const auto& f : spec->files)
                    details += "  " + juce::String (f.url) + "\n";
        }
    }
    return details.toStdString ();
}

// Shared failure dialog for the AI features: a copyable read-only text editor
// with the error details (paths/URLs), built by sc::buildNsFailureDetails().
static void showAiFailureDialog (const juce::String& feature, const std::string& err,
                                 const std::string& modelId, Component* parent)
{
    const juce::String details = sc::buildNsFailureDetails (err, modelId);

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
    delete w;
    if (result != 1) return;

    const std::string modelId = modelText == "DeepFilterNet2" ? "deepfilternet2"
                                 : modelText == "DeepFilterNet3" ? "deepfilternet3"
                                                                 : "denseunet";
    // Remember the choices for next time.
    app.lastNsModelId  = modelId;
    app.aiAutoDownload = autoDownload;
    app.saveAiSettings();

    // v1: the DF3 post-filter option is not exposed in the dialog yet.
    runNoiseSuppressAsync (modelId, "CPU", attenLimDb, false, autoDownload);
}

// Noise suppression on a background thread with a progress dialog. Progress:
// model download/compile/inference 0..95%, mipmap rebuild 95..100%.
void MainWindow::runNoiseSuppressAsync (const std::string& modelId, const std::string& device,
                                        float attenLimDb, bool df3PostFilter, bool autoDownload)
{
    auto* dlg = new WorkProgressDialog ("Noise Suppression",
                                        "Applying " + juce::String (modelId) + " ...",
                                        false, this);
    dlg->setProgress (0.0);
    dlg->enterModalState (false);

    std::thread ([this, modelId, device, attenLimDb, df3PostFilter, autoDownload, dlg] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v) {
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };

        std::string err;
        const bool ok = app.noiseSuppress (modelId, device, attenLimDb, df3PostFilter,
            [sendProgress] (float f) { sendProgress (0.95 * (double) f); return true; }, &err,
            autoDownload);
        if (ok)
            app.audioEngine().rebuildMipmaps ([sendProgress] (float f) {
                sendProgress (0.95 + 0.05 * (double) f);
            });

        juce::MessageManager::callAsync ([this, modelId, dlg, ok, err] {
            delete dlg;
            if (ok)
            {
                refreshAfterEdit();
                return;
            }

            showAiFailureDialog ("Noise Suppression", err, modelId, this);
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
                                     "Larger models are more accurate but slower (CPU).",
                                     juce::AlertWindow::QuestionIcon, this);

    w->addComboBox ("model", whisperModelNames (), "Model");
    w->addComboBox ("language", transcribeLanguageNames (), "Language");
    w->addComboBox ("mode",
                    juce::StringArray { "Transcribe", "Translate to English" },
                    "Mode");

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
    app.aiAutoDownload          = autoDownload;
    app.saveAiSettings ();

    runTranscriptionAsync (modelId, language, translate, autoDownload);
}

// Transcription on a background thread with a progress dialog. Progress:
// download [0, 15%), mix+resample [15, 25%), inference [25, 100%].
void MainWindow::runTranscriptionAsync (const std::string& modelId, const std::string& language,
                                        bool translate, bool autoDownload)
{
    auto* dlg = new WorkProgressDialog ("Transcription",
                                        "Transcribing with " + juce::String (modelId) + " ...\n"
                                        "(whole file - this can take a while on CPU)",
                                        false, this);
    dlg->setProgress (0.0);
    dlg->enterModalState (false);

    std::thread ([this, modelId, language, translate, autoDownload, dlg] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v) {
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };

        std::string err;
        const bool ok = app.transcribe (modelId, language, translate,
            [sendProgress] (float f) { sendProgress ((double) f); return true; }, &err,
            autoDownload);

        juce::MessageManager::callAsync ([this, dlg, ok, err, modelId] {
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

            showAiFailureDialog ("Transcription", err, modelId, this);
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
                                     "updated in place. The files are NOT opened in the editor.\n\n"
                                     + folder.getFullPathName (),
                                     juce::AlertWindow::QuestionIcon, this);

    w->addComboBox ("model", whisperModelNames (), "Model");
    w->addComboBox ("language", transcribeLanguageNames (), "Language");
    w->addComboBox ("mode",
                    juce::StringArray { "Transcribe", "Translate to English" },
                    "Mode");
    w->addComboBox ("existing",
                    juce::StringArray { "Overwrite existing SCTT", "Skip files with SCTT" },
                    "Existing transcriptions");

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
    const bool skipExisting        = w->getComboBoxComponent ("existing") != nullptr
                                        && w->getComboBoxComponent ("existing")->getText ().startsWith ("Skip");
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
    app.aiAutoDownload          = autoDownload;
    app.saveAiSettings ();

    runBatchTranscribeAsync (folder, modelId, language, translate, skipExisting, autoDownload);
}

// Batch transcription on a background thread with a cancellable progress
// dialog. Progress: model install [0, 5%), then each file gets its own slice.
void MainWindow::runBatchTranscribeAsync (const juce::File& folder,
                                          const std::string& modelId,
                                          const std::string& language, bool translate,
                                          bool skipExisting, bool autoDownload)
{
    auto* dlg = new WorkProgressDialog ("Transcription",
                                        "Scanning " + folder.getFileName ()
                                        + " for WAV files ...", true, this);
    dlg->setProgress (0.0);
    dlg->enterModalState (false);

    std::thread ([this, folder, modelId, language, translate, skipExisting,
                  autoDownload, dlg] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v) {
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };

        BatchTranscribeSummary summary;
        std::string err;
        const bool ok = app.batchTranscribeFolder (
            folder, modelId, language, translate, skipExisting,
            [&sendProgress] (float f) { sendProgress ((double) f); return true; },
            [dlg] (const juce::String& name) {
                juce::MessageManager::callAsync ([dlg, name] {
                    dlg->setStatusText ("Transcribing " + name
                                        + " ...\n(Cancel stops after the current file)");
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
                showAiFailureDialog ("Transcription", err, modelId, this);
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
        menu.addSeparator();
        menu.addItem (FileSave, "Save\tCtrl+S", app.audioEngine().isFileLoaded(), false);
        menu.addItem (FileSaveAs, "Save As...\tCtrl+Shift+S", app.audioEngine().isFileLoaded(), false);
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
        menu.addItem (EditPaste, "Paste\tCtrl+V", app.audioEngine().canPaste(), false);
        menu.addItem (EditDelete, "Delete\tDel", !app.selection.isEmpty(), false);
        menu.addItem (EditSelectAll, "Select All\tCtrl+A", app.audioEngine().isFileLoaded(), false);
        menu.addSeparator();
        menu.addItem (EditSnapToZeroCrossings, "Snap to Zero Crossings", true, app.settings.snapToZeroCrossings);
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
        menu.addItem (TransportLoop, "Loop (Selection/All)\tL", true, app.audioEngine().isLooping());
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
        // Whole-file operation: needs the file fully in memory.
        menu.addItem (ToolsResample, "Resample...",
                      app.audioEngine().isFileLoaded() && app.audioEngine().fullyInMemory(), false);
    }
    else if (menuName == "Help")
    {
        menu.addItem (HelpAbout, "About SpectroCraft", true, false);
    }

    return menu;
}

juce::StringArray MainWindow::getMenuBarNames()
{
    return { "File", "Edit", "View", "Transport", "Effects", "Help" };
}

bool MainWindow::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    // Ctrl+Tab / Ctrl+Shift+Tab cycles through the loaded files.
    if (key.getKeyCode() == juce::KeyPress::tabKey && key.getModifiers().isCommandDown())
    {
        app.cycleActiveDoc (!key.getModifiers().isShiftDown());
        return true;
    }
    if (key == juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0)) { handleFileOpen(); return true; }
    if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0)) { handleFileSave(); return true; }
    if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { handleFileSaveAs(); return true; }
    if (key == juce::KeyPress ('w', juce::ModifierKeys::commandModifier, 0)) { app.closeFile(); return true; }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0)) { app.undoStep(); refreshAfterEdit(); return true; }
    if (key == juce::KeyPress ('y', juce::ModifierKeys::commandModifier, 0)) { app.redoStep(); refreshAfterEdit(); return true; }
    if (key == juce::KeyPress ('x', juce::ModifierKeys::commandModifier, 0)) { app.cut(); refreshAfterEdit(); return true; }
    if (key == juce::KeyPress ('c', juce::ModifierKeys::commandModifier, 0)) { app.copy(); return true; }
    if (key == juce::KeyPress ('v', juce::ModifierKeys::commandModifier, 0)) { app.paste(); refreshAfterEdit(); return true; }
    if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0)) { app.selectAll(); syncSharedState(); return true; }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) { app.del(); refreshAfterEdit(); return true; }
    if (key == juce::KeyPress::spaceKey)
    {
        // Space toggles play/pause (not stop).
        if (app.audioEngine().isPlaying()) app.pause(); else app.play();
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
        // Ctrl = fine 1-sample steps; plain / Shift arrows move in 10 ms steps.
        const bool ctrl = mods.isCommandDown() || mods.isCtrlDown();
        const double step = ctrl ? (1.0 / app.audioEngine().sampleRate()) : 0.010;
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
    if (key == juce::KeyPress::homeKey)
    {
        app.setPlayhead (0.0);
        syncSharedState();
        return true;
    }
    if (key == juce::KeyPress::endKey)
    {
        app.setPlayhead (app.audioEngine().duration());
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
    if (key == juce::KeyPress ('l', juce::ModifierKeys::noModifiers, 0))
    {
        app.toggleLoop();
        return true;
    }
    if (key == juce::KeyPress ('m', juce::ModifierKeys::noModifiers, 0)
        || key == juce::KeyPress (juce::KeyPress::F8Key, juce::ModifierKeys::noModifiers, 0))
    {
        // Region marker spanning the selection if there is one,
        // otherwise a point marker at the cursor.
        app.addMarkerAtCursor();
        app.markers().saveSidecar (app.audioEngine().getFile());
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
