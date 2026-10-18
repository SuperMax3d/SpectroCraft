/**
 * SpectroCraft - Main Window Implementation
 * Layout: menu bar on top; left column with two tab groups
 *   (top: Files | Frequency Analysis, bottom: Spectral Controls | Markers);
 * main area: waveform view on top, spectral view below.
 */

#include "MainWindow.h"
#include "Application.h"
#include "WaveformView.h"
#include "SpectralView.h"
#include "Panels.h"
#include "MarkerManager.h"
#include "IconData.h"
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

    fileListPanel->onOpenRequested = [this] { handleFileOpen(); };

    topTabs.addTab ("Files", kDarkTabBg, fileListPanel.get(), false);
    topTabs.addTab ("Frequency Analysis", kDarkTabBg, fftPanel.get(), false);
    bottomTabs.addTab ("Spectral Controls", kDarkTabBg, controlsPanel.get(), false);
    bottomTabs.addTab ("Markers", kDarkTabBg, markersPanel.get(), false);
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
    setVisible (true);
}

MainWindow::~MainWindow() {}

void MainWindow::setupViews()
{
#if defined (_WIN32)
#endif
    waveformView = std::make_unique<WaveformView> (app);
    spectralView = std::make_unique<SpectralView> (app);

    waveformView->setTooltip ("Click: move playhead | Drag: select a region (plain click clears it) "
                              "| Middle/right-drag: pan | Wheel: zoom time (Shift+Wheel or wheel over the dB axis: zoom amplitude) "
                              "| Double-click: select whole file (on a marker: select its range) | Right-click: context menu.");
    spectralView->setTooltip ("Click: move playhead | Drag: select a region | Middle/right-drag: pan "
                              "| Wheel: zoom time (Shift+Wheel or wheel over the frequency axis: zoom frequency) "
                              "| Double-click: select whole file (on a marker: select its range) | Right-click: context menu.");

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

    // Same width as the views above: stop where their vertical scrollbars
    // begin, so the thumb tracks exactly the visible plot area.
    if (haveFile && timeBar)
        timeBar->setBounds (mainArea.getX(), mainArea.getBottom() + 2,
                            std::max (0, mainArea.getWidth() - vScrollW), 16);

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

    // The colour map is not part of Settings, so detect it separately and
    // force a full spectral re-render when it changes.
    if (spectralView && app.colourMap != lastSyncedColourMap)
    {
        lastSyncedColourMap = app.colourMap;
        spectralView->invalidateImage();
    }

    if (statusBar)
    {
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
    if (fftPanel)       fftPanel->repaint();
}

void MainWindow::openFile (const juce::File& file)
{
    // app.openFile() activates the new document and fires onActiveDocChanged,
    // which resets both views to this file and syncs every panel.
    if (!app.openFile (file))
        return;
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

void MainWindow::handleFileSave()
{
    if (app.audioEngine().isFileLoaded())
        app.audioEngine().saveFile (app.audioEngine().getFile());
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
        app.audioEngine().saveFileAs (chooser.getResult());
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
                    spectralView->saveAsPng (chooser.getResult());
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
        case EditSnapToZeroCrossings: app.settings.snapToZeroCrossings = !app.settings.snapToZeroCrossings; break;

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
        case ToolsReverse: app.reverse(); refreshAfterEdit(); break;

        case HelpAbout:
            juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon,
                "About SpectroCraft",
                "SpectroCraft v1.0.0\nHigh-performance waveform + spectral editor",
                "OK");
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
                                     juce::AlertWindow::QuestionIcon);
    w->addTextEditor ("gainDb", "0.0");
    if (auto* editor = w->getTextEditor ("gainDb"))
        // Let Enter/Escape reach the OK/Cancel shortcuts even while the field
        // has keyboard focus.
        editor->setEscapeAndReturnKeysConsumed (false);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // Focus the field with its text pre-selected so a new value can be typed
    // in immediately. AlertWindow disables keyboard focus on its editors, so
    // re-enable it first or grabKeyboardFocus() would bounce to a button.
    if (auto* editor = w->getTextEditor ("gainDb"))
    {
        editor->setWantsKeyboardFocus (true);
        editor->selectAll();
        editor->grabKeyboardFocus();
    }

    return w;
}

void MainWindow::showChangeGainDialog()
{
    if (!app.audioEngine().isFileLoaded() || app.selection.isEmpty()) return;

    auto* w = createGainDialog();
    const int result = w->runModalLoop();
    const juce::String text = w->getTextEditorContents ("gainDb");
    delete w;
    if (result != 1) return;

    const float db = (float) text.trim().getDoubleValue();
    app.changeGain (db);
    refreshAfterEdit();
}

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

    if (app.audioEngine().isFileLoaded() && !mods.isCommandDown() && !mods.isAltDown()
            && !mods.isCtrlDown()
            && (keyCode == juce::KeyPress::leftKey || keyCode == juce::KeyPress::rightKey))
    {
        const double step = 1.0 / app.audioEngine().sampleRate();
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

    return false;
}
