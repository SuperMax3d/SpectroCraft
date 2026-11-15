/**
 * SpectroCraft - Main Window Implementation
 * Layout: menu bar on top (workspace switcher on its right), status bar at the
 * bottom, and in between the docking workspace (include/Docking.h): panel
 * groups with tabs - the Editor (waveform view on top, spectral view below)
 * among them - that can be rearranged, docked anywhere or undocked into
 * floating windows. The default workspace keeps the classic arrangement: a
 * left column with two groups (Files | Frequency Analysis ... above,
 * Spectral Controls | Markers ... below) beside the Editor.
 */

#include "MainWindow.h"
#include "AlertGate.h"
#include "Application.h"
#include "Recorder.h"
#include "StartupTrace.h"
#include "control/ControlState.h"   // AI control: app.control().ui
#include "control/CommandRegistry.h" // menu actions run through the registry
#include "Diagnostics.h"               // opt-in diagnostic log (Window / Help menus)
#include "Updates.h"                  // Help > Check for Updates (docs/updates.md)
#include <thread>
#include "WaveformView.h"
#include "SpectralView.h"
#include "Panels.h"
#include "MarkerManager.h"
#include "VideoAudio.h"
#include "Resampler.h"
#include "VideoFrames.h"
#include "VideoWindow.h"
#include "WorkProgress.h"
#include "StemSeparation.h"
#include "SuperResolution.h"
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    #include "AI/ModelManager.h"
    #include "AI/OvRuntime.h"
    #include "NoiseSuppression.h"
#endif
#include "IconData.h"
#include "ScaleForm.h"
#include "Version.h"   // auto-generated: SC_VERSION_STRING (bumped on every build)
#include <algorithm>

namespace
{
/** juce::AlertWindow never deletes the components passed to
    addCustomComponent() (see juce_AlertWindow.h), and the dialogs below hand
    them over with `new`. They leaked - and a leaked ComboBox with a pending
    async change message later ran its onChange lambda (capturing this
    MainWindow) after the window was gone: SIGBUS in the Filter EQ dialog.
    This AlertWindow owns and deletes its custom components. */
class OwningAlertWindow : public juce::AlertWindow
{
public:
    using juce::AlertWindow::AlertWindow;

    ~OwningAlertWindow() override
    {
        for (int i = getNumCustomComponents(); --i >= 0;)
            delete removeCustomComponent (i);
        ownedChildren_.clear();   // after the custom components they may listen to
    }

    /** addChildComponent() + ownership: for helper components (listeners)
        that must live exactly as long as the dialog. A plain child is never
        deleted by juce::Component. */
    void addOwnedChildComponent (juce::Component* c)
    {
        addChildComponent (c);
        ownedChildren_.emplace_back (c);
    }

private:
    std::vector<std::unique_ptr<juce::Component>> ownedChildren_;
};

/** Plain container (dialog row) that owns the children added to it with
    addAndMakeVisible (new ...); juce::Component does not delete them. */
class OwningRow : public juce::Component
{
public:
    ~OwningRow() override { deleteAllChildren(); }
};
} // namespace

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

namespace
{
/** The Editor panel of the docking workspace: hosts the waveform and
    spectral views, their scrollbars and the dedicated tables track, which
    MainWindow::layoutEditor() places whenever the panel is resized. */
class EditorPanel : public juce::Component
{
public:
    std::function<void()> onResized;
    void resized() override { if (onResized) onResized(); }
    void paint (juce::Graphics& g) override { g.fillAll (mainWindowBackground ()); }
};

/** One Editor tab: there is one per open document (MainWindow::
    syncEditorTabs). They share the single EditorPanel, which sits in the tab
    of the active document. A tab of another document that shows anyway (moved
    to another group) keeps showing that document as the views last drew it
    (`snapshot`, taken when it stopped being the active one), and a click
    there makes it active. Without a picture yet it names its file. */
class EditorSlot : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    juce::String fileName;
    std::function<void()> onActivate;
    juce::Image snapshot;   // the views showing this document, when it was left

    void setSnapshot (juce::Image img)
    {
        snapshot = std::move (img);
        updateTooltip ();
        repaint ();
    }
    void updateTooltip()
    {
        setTooltip (getNumChildComponents() > 0 ? juce::String()
                                                : fileName + ": click to make it the active file");
    }

    void resized() override
    {
        for (auto* c : getChildren())
            c->setBounds (getLocalBounds());
    }
    void childrenChanged() override
    {
        // The views moved in: the picture is out of date from now on.
        if (getNumChildComponents() > 0)
            snapshot = {};
        updateTooltip ();
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (mainWindowBackground ());
        if (getNumChildComponents() > 0)
            return;
        if (snapshot.isValid())
        {
            // Stretched if the tab was resized since (rare, and the next
            // switch away redraws it at the new size).
            g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
            g.drawImage (snapshot, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
            return;
        }
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.setFont (14.0f);
        g.drawFittedText (fileName + "\nClick to show it here.", getLocalBounds().reduced (16),
                          juce::Justification::centred, 4);
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (getNumChildComponents() == 0 && ! e.mods.isPopupMenu() && onActivate)
            onActivate();
    }
};

/** Tab title of a document: its file name (the display name until it has
    one: a new, never saved document). */
juce::String editorTabTitle (const FileDoc& d)
{
    const juce::File f = d.engine.getFile();
    if (f.getFullPathName().isNotEmpty())
        return f.getFileName();
    if (d.file.getFullPathName().isNotEmpty())
        return d.file.getFileName();
    return d.name.isNotEmpty() ? d.name : juce::String ("Untitled");
}

/** Name prompt for the workspace commands (none in headless test runs). */
juce::String promptForWorkspaceName (const juce::String& title, const juce::String& initial)
{
    if (sc::alert::isHeadless ())
        return {};
    juce::AlertWindow dlg (title, "Workspace name:", juce::AlertWindow::NoIcon);
    dlg.addTextEditor ("name", initial);
    dlg.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dlg.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    auto* ed = dlg.getTextEditor ("name");
    dlg.enterModalState (false);
    if (ed != nullptr)
    {
        ed->grabKeyboardFocus ();
        ed->selectAll ();
    }
    const int r = dlg.runModalLoop ();
    return (r == 1 && ed != nullptr) ? ed->getText ().trim () : juce::String ();
}
} // namespace

MainWindow::MainWindow (Application& appIn, const juce::String& name)
    : DocumentWindow (name,
                      mainWindowBackground (),
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

    // Left-side panels in two tab groups. The document list is not a panel
    // any more: it lives in File > Loaded Files, and its axis settings in the
    // Scale form.
    fftPanel      = std::make_unique<FFTAnalysisView> (app);
    controlsPanel = std::make_unique<SpectralControlsPanel> (app);
    markersPanel  = std::make_unique<MarkerListPanel> (app);
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    transcriptionPanel = std::make_unique<TranscriptionPanel> (app);
#endif
    selectionPanel = std::make_unique<SelectionPanel> (app);
    drawingPanel   = std::make_unique<DrawingPanel> (app);
    historyPanel  = std::make_unique<HistoryPanel> (app);
    actionsPanel  = std::make_unique<ActionsPanel> (app);
    pythonPanel   = std::make_unique<PythonPanel> (app);
    tablesPanel   = std::make_unique<TablesPanel> (app);
    videoDockHost = std::make_unique<VideoDockHost>();

    // Dedicated tables track + its separator and vertical scrollbar.
    tablesTrack   = std::make_unique<TablesTrackView> (app);
    tablesTrack->onTimeWindow      = [this] (double a, double b) { applyTimeWindow (a, b); };
    tablesTrack->onZoomToSelection = [this] () { zoomToSelection (); };
    tablesTrack->onZoomOutFull     = [this] (bool axisOnly)
    {
        if (axisOnly)   // vertical-axis right-click: value range only
            tablesTrack->refit();
        else            // track data: time window + every vertical range
            zoomOutFull ();
    };
    // The dedicated track's vertical scrollbar: dragging the thumb sets the
    // strip's value window, and any wheel/drag/refit change inside the strip
    // moves the thumb back.
    tablesTrack->onValueWindowChanged = [this] (double, double)
    {
        updateScrollBars ();   // keeps the scrollbar's range + thumb in sync
    };
    // The track's Edit menu operates on whatever is selected in the Tables
    // panel; empty selection there means "every visible parameter".
    tablesTrack->selectedParams = [this] ()
    {
        return (tablesPanel != nullptr) ? tablesPanel->selectedParamRows ()
                                        : std::vector<std::pair<int, int>>{};
    };

    tablesSep     = std::make_unique<ViewSplitter>();
    tablesSep->onDrag = [this] (const juce::MouseEvent& e) { handleTablesSepDrag (e); };
    tablesVScroll = std::make_unique<ValueScrollBar>();
    tablesVScroll->setTooltip ("Visible value range of the dedicated tables track: drag the "
                               "thumb to pan, drag an edge to zoom, click outside the thumb to jump.");
    tablesVScroll->onChange = [this] (double lo, double hi)
    {
        if (tablesTrack && hi > lo) tablesTrack->setValueRange (lo, hi);
    };


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

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    if (transcriptionPanel)
    {
        transcriptionPanel->onRunRequested = [this] { showTranscribeDialog (); };
        transcriptionPanel->onRunFolderRequested = [this] { showTranscribeFolderDialog (); };
    }
#endif
    pythonPanel->onRunRequested = [this] (const juce::File& f, const juce::String& t)
                                  {
                                      app.recordActionStep (sc::actionTypes::kPython,
                                          sc::makeVarObject ({ { "script", f.getFullPathName() } }));
                                      runPythonScriptAsync (f, t);
                                  };

    // Panels close from their ≡ tab menu or the Window menu.
    fftPanel->onCloseRequested        = [this] { setPanelVisible ("fft", false); };
    historyPanel->onCloseRequested    = [this] { setPanelVisible ("history", false); };
    actionsPanel->onRunRequested      = [this] (int idx) { runAction (idx); };
    actionsPanel->onBatchRequested    = [this] (int idx) { showActionBatchWindow (idx); };
    actionsPanel->onCloseRequested    = [this] { setPanelVisible ("actions", false); };
    pythonPanel->onCloseRequested     = [this] { setPanelVisible ("python", false); };
    controlsPanel->onCloseRequested   = [this] { setPanelVisible ("controls", false); };
    markersPanel->onCloseRequested    = [this] { setPanelVisible ("markers", false); };
    // DEL in the Markers / Selection lists deletes the selected audio: same
    // route as Edit > Delete (streamed-file materialisation + refreshAfterEdit:
    // window clamp, tile-cache purge, shared-state sync).
    auto deleteSelectedAudio = [this]
    {
        app.recordActionStep (sc::actionTypes::kDelete);
        runDestructiveEdit ([this] (std::string* e) { return app.del (e); });
    };
    markersPanel->onDeleteAudioRequested   = deleteSelectedAudio;
    selectionPanel->onDeleteAudioRequested = deleteSelectedAudio;
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    if (transcriptionPanel)
        transcriptionPanel->onCloseRequested =
            [this] { setPanelVisible ("transcription", false); };
#endif
    selectionPanel->onCloseRequested  = [this] { setPanelVisible ("selection", false); };
    drawingPanel->onCloseRequested    = [this] { setPanelVisible ("drawing", false); };
    tablesPanel->onCloseRequested     = [this] { setPanelVisible ("tables", false); };

    // ---- Docking workspace ----------------------------------------------
    // Every panel lives in a group of the dock tree, the Editor (the views,
    // placed by layoutEditor()) included. The arrangement is the active
    // workspace's layout (Window > Workspaces, or the bar right of the menu).
    {
        auto ed = std::make_unique<EditorPanel> ();
        ed->onResized = [this] { layoutEditor (); };
        editorPanel_ = std::move (ed);
        // The permanent Editor tab; the views move between document tabs.
        auto slot = std::make_unique<EditorSlot> ();
        slot->onActivate = [this]
        {
            for (const auto& [docId, pid] : editorTabOfDoc_)
                if (pid == "editor") { activateDocById (docId); return; }
        };
        slot->addAndMakeVisible (editorPanel_.get ());
        editorSlots_["editor"] = std::move (slot);
    }
    dock_ = std::make_unique<sc::dock::DockManager> ();
    dock_->getMainWindow = [this] () -> juce::Component* { return this; };
    dock_->registerPanel ({ "editor", "Editor", editorSlots_["editor"].get (), false, 900, 600 });
    // A click on a document's Editor tab makes that document the active one.
    dock_->onActiveTabChanged = [this] (const juce::String& id)
    {
        for (const auto& [docId, pid] : editorTabOfDoc_)
            if (pid == id) { activateDocById (docId); return; }
    };
    for (const auto& e : leftPanelEntries ())
        if (auto* c = panelForId (e.id))
            dock_->registerPanel ({ e.id, e.name, c, true, 340, 300 });
    // A panel with no remembered place joins its default group (the one of
    // Frequency Analysis or Spectral Controls).
    for (const auto& e : leftPanelEntries ())
    {
        const juce::String id (e.id);
        dock_->setDefaultBuddy (id, e.defaultTop ? (id == "fft" ? "history" : "fft")
                                                 : (id == "controls" ? "markers" : "controls"));
    }
    dock_->onLayoutChanged = [this]
    {
        if (dockLoading_) return;
        dockLayoutDirty_ = true;   // saved by the timer shortly after
        dockLayoutDirtyAt_ = juce::Time::getMillisecondCounter ();
    };
    dock_->onPanelOpenChanged = [this] (const juce::String& id, bool open) { handlePanelOpenChanged (id, open); };
    // Shortcuts typed in a floating panel window, and files dropped on one,
    // act on the main window.
    dock_->onUnhandledKey = [this] (const juce::KeyPress& k) { return keyPressed (k, this); };
    dock_->isInterestedInFiles = [this] (const juce::StringArray& f) { return isInterestedInFileDrag (f); };
    dock_->onFilesDropped = [this] (const juce::StringArray& f) { filesDropped (f, 0, 0); };

    // Right-clicking a panel header (a tab, or the ⋯ button) also gets the
    // document actions: the Editor's folder and its Scale form.
    dock_->panelMenuExtender = [this] (juce::PopupMenu& menu, const juce::String& panelId)
    {
        if (panelId != "editor") return;
        const bool hasPath = app.doc() != nullptr
                             && ! app.doc()->file.getFullPathName().isEmpty();
        menu.addItem (PanelFolderId, "Open Containing Folder", hasPath, false);
        menu.addItem (PanelScaleId,  "Scale...", app.doc() != nullptr, false);
    };
    juce::Component::addAndMakeVisible (dock_->mainArea ());

    // Workspaces: the built-in arrangements plus the user's own, each with
    // its saved layout and its live modifications (restored at startup).
    workspaces_.load  = [this] (const juce::String& k) { return app.recentStore->getValue (k); };
    workspaces_.store = [this] (const juce::String& k, const juce::String& v)
    {
        app.recentStore->setValue (k, v);
        app.recentStore->save ();
    };
    workspaces_.setBuiltIns (builtInWorkspaces ());
    workspaces_.restore ();
    {
        juce::var layout = workspaces_.layoutFor (workspaces_.activeId ());
        // First start after the update: keep the arrangement of the former
        // two-group left column (tab order, closed panels, sizes).
        if (app.recentStore->getValue ("dock_workspaces_v1").isEmpty ()
            && app.recentStore->getValue ("left_tab_layout").isNotEmpty ())
        {
            layout = legacyLeftLayout ();
            workspaces_.setCurrent (workspaces_.activeId (), layout);
        }
        applyDockLayout (layout);
    }

    workspaceBar_ = std::make_unique<sc::dock::WorkspaceBar> ();
    workspaceBar_->getItems = [this]
    {
        std::vector<std::pair<juce::String, juce::String>> items;
        for (const auto& e : workspaces_.entries ())
            items.emplace_back (e.id, e.name);
        return items;
    };
    workspaceBar_->getActive = [this] { return workspaces_.activeId (); };
    workspaceBar_->onSelect = [this] (const juce::String& id) { switchWorkspace (id); };
    workspaceBar_->buildMenu = [this] (juce::PopupMenu& m, const juce::String& clicked)
    {
        workspaceMenuTarget_ = clicked;
        buildWorkspaceMenu (m, clicked);
    };
    workspaceBar_->onMenuResult = [this] (int id) { handleWorkspaceCommand (id, workspaceMenuTarget_); };
    juce::Component::addAndMakeVisible (workspaceBar_.get ());

    splitter = std::make_unique<ViewSplitter>();
    splitter->onDrag = [this] (const juce::MouseEvent& e) { handleSplitterDrag (e); };
    splitter->setTooltip ("Drag to adjust the height of the waveform and spectrogram views.");
    editorPanel_->addAndMakeVisible (splitter.get());

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
        {
            // Centre -> vertical offset (pan), span -> zoom. setVerticalRange
            // only repaints on a zoom change, so a pure pan needs one here.
            waveformView->setVerticalRange (lo, hi);
            waveformView->repaint();
        }
    };
    specVScroll->onChange = [this] (double lo, double hi) {
        if (spectralView && hi > lo) spectralView->setFrequencyRange (lo, hi);
    };

    // Status bar at the very bottom of the window.
    statusBar = std::make_unique<StatusBar>();
    juce::Component::addAndMakeVisible (statusBar.get());

    // One tooltip window for the whole app: it watches for mouse hovers and
    // shows the tip of whichever TooltipClient component is under the cursor
    // (none at all while Help > Show Tooltips is off).
    applyTooltipsSetting();

    editorPanel_->addAndMakeVisible (timeBar.get());
    editorPanel_->addAndMakeVisible (waveVScroll.get());
    editorPanel_->addAndMakeVisible (specVScroll.get());

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
        {
            // A typed range is not tied to any channel row.
            app.selectionRow = -1;
            app.setSelection (juce::Range<double> (a, b));
        };

    // The footer's playhead field pushes the typed position back into the
    // shared state (Application clamps it to the file and notifies the views).
    if (statusBar)
        statusBar->onPositionEdited = [this] (double t) { app.setPlayhead (t); };
    // The footer's channel numbers show/hide a channel; Ctrl+click (Cmd on
    // macOS) shows only that one, and again on the lone visible one shows
    // them all (the View > Channel N rules).
    if (statusBar)
        statusBar->onChannelClicked = [this] (int ch, bool only)
        {
            if (ch < 0 || ch >= app.audioEngine().numChannels()) return;
            if (only && app.visibleChannels.size() == 1 && app.visibleChannels[0] == ch)
            {   showEveryChannel(); return; }
            if (only)
                app.visibleChannels = { ch };
            else
                app.toggleChannelVisible (ch);
            app.syncActiveDoc ();   // keep the active document's copy in step
            syncSharedState();
        };

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

    // Tables panel <-> views wiring: refresh the list, repaint overlays and
    // relayout when the dedicated track appears/disappears.
    app.onTablesChanged = [this]
    {
        if (tablesPanel) tablesPanel->refresh();
        if (waveformView) waveformView->repaint();
        if (spectralView) spectralView->repaint();
        if (tablesTrack)
        {
            tablesTrack->refit();
            tablesTrack->setTimeRange (app.viewStart, app.viewEnd);
        }
        resized();   // the dedicated track may appear or disappear
    };

    // The Tables panel's 1/2-point alignment arms click capture on one of the
    // data views; negative disarms both.
    app.onArmTableAlignClick = [this] (int view)
    {
        if (waveformView) waveformView->onAlignClick =
            (view == 0 && tablesPanel != nullptr)
                ? std::function<void(double, double)> ([this] (double t, double v) { tablesPanel->handleDocClick (t, v); })
                : std::function<void(double, double)> (nullptr);
        if (spectralView) spectralView->onAlignClick =
            (view == 1 && tablesPanel != nullptr)
                ? std::function<void(double, double)> ([this] (double t, double v) { tablesPanel->handleDocClick (t, v); })
                : std::function<void(double, double)> (nullptr);
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
        if (tablesTrack) tablesTrack->setPlayhead (t);
        if (tablesPanel) tablesPanel->plotView().setPlayhead (t);
        if (videoWindow) videoWindow->getTracksPanel().setPlayhead (t);
        // A single click in either view re-analyses the spectrum: centred on
        // the cursor when there is no selection, averaged over the whole
        // selection when one exists.
        if (fftPanel) { fftPanel->recomputeIfNeeded(); fftPanel->repaint(); }
        if (statusBar) statusBar->setPlayhead (t);
    };

    // Multi-file support: whenever a document becomes active (open, switch,
    // close), reset both views to that file and push its state into every panel.
    app.onActiveDocChanged = [this] { activateDocUI(); };
    // Just before a switch, the tab of the document being left keeps a
    // picture of it: shown when that tab is visible in another group, so
    // two files can sit side by side.
    app.onBeforeActiveDocChange = [this] { snapshotEditorIntoItsTab(); };

    // When a file is closed, drop the stored snapshots that came from it.
    app.onFileClosed = [this] (const FileDoc& d)
    {
        if (fftPanel) fftPanel->clearSnapshotsFor (&d);
    };

    // Deleting a time range also shifts / trims / drops the FFT snapshot slots
    // stored from that file; record their before/after state into the patch.
    app.onTimeRangeDeleted = [this] (FileDoc* d, double a, double b, Patch& p, bool preserveBefore)
    {
        if (fftPanel) fftPanel->recordAndAdjustSnapshotsForDeletion (d, a, b, p, preserveBefore);
    };

    app.onTimeRangeStretched = [this] (FileDoc* d, double a, double bOld, double bNew, Patch& p)
    {
        if (fftPanel) fftPanel->recordAndAdjustSnapshotsForStretch (d, a, bOld, bNew, p);
    };

    app.onTimeRangeInserted = [this] (FileDoc* d, double a, double lenSecs, Patch& p)
    {
        if (fftPanel) fftPanel->recordAndAdjustSnapshotsForInsertion (d, a, lenSecs, p);
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

    // Documents opened before this window existed get their Editor tabs.
    syncEditorTabs ();

    scStart::mark ("end-of-MainWindow-ctor");
    // Proxy for "window visible": the first paint happens shortly after the
    // window is shown, so a short delay gives a stable end-to-end number.
    // The first window of the process then posts the "startup" event to the
    // diagnostic log (when it is on).
    juce::Timer::callAfterDelay (150, [] {
        scStart::mark ("window-shown(+150ms)");
        scStart::report ();
    });

    // AI control: commands may now refresh this window, drive its views and
    // show job progress (sc::control::UiBridge).
    app.control().ui = this;

    setVisible (true);
}

// Modal progress dialog for long-running background work (loading, saving,
// exporting) so the UI stays responsive and Windows never shows its own wait
// cursor over our windows - see WorkProgress.h.
using WorkProgressDialog = sc::WorkProgressDialog;

MainWindow::~MainWindow()
{
    // A pending arrangement change (saves are debounced by the timer).
    if (dockLayoutDirty_)
        saveDockLayout ();

    // The Video window polls this window for the time to show: first to go.
    videoWindow.reset();

    // The Application outlives this window (tests reuse it; shutdown resets
    // the window first): uninstall every callback that captured `this`.
    app.onStateChange = nullptr;
    app.onSetTimeWindow = nullptr;
    app.onArmNewDrawing = nullptr;
    app.onDrawingsChanged = nullptr;
    app.onTablesChanged = nullptr;
    app.onArmTableAlignClick = nullptr;
    app.onSetSpectralFreqRange = nullptr;
    app.onPlayheadChanged = nullptr;
    app.onActiveDocChanged = nullptr;
    app.onBeforeActiveDocChange = nullptr;
    app.onFileClosed = nullptr;
    app.onTimeRangeDeleted = nullptr;
    app.onTimeRangeStretched = nullptr;
    app.onTimeRangeInserted = nullptr;
    app.onPatchSideStateRestored = nullptr;
    if (app.control().ui == this)
        app.control().ui = nullptr;

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

    editorPanel_->addAndMakeVisible (waveformView.get());
    editorPanel_->addAndMakeVisible (spectralView.get());

    // Dedicated tables track strip + its separator and vertical scrollbar.
    // Hidden unless the Tables panel is in "Dedicated track" mode.
    editorPanel_->addAndMakeVisible (tablesTrack.get());
    editorPanel_->addAndMakeVisible (tablesSep.get());
    editorPanel_->addAndMakeVisible (tablesVScroll.get());

    // Both views always show the SAME time window: when either one changes its
    // range (zoom/pan/fit), applyTimeWindow() pushes it to both views and keeps
    // the canonical app.viewStart/viewEnd in sync (used by the Frequency
    // Analysis panel). Vertical zoom levels stay independent per view.
    auto onViewChanged = [this] (double t0, double t1) { applyTimeWindow (t0, t1); };
    waveformView->onChange = onViewChanged;
    spectralView->onChange = onViewChanged;

    // Keep the vertical scrollbars in sync with each view's zoom level.
    waveformView->onVerticalZoomChanged = [this] { updateScrollBars(); };
    spectralView->onFrequencyRangeChanged = [this] (double, double)
    {
        updateScrollBars ();
        updateAutoFftLabel ();   // the resolved Auto size depends on the freq axis
    };

    // Right-click context menus in either view route to the same actions.
    waveformView->onContextAction = [this] (int id) {
        ctxScaleKind = (int) sc::axes::Kind::Amplitude;
        handleContextMenuAction (id);
    };
    spectralView->onContextAction = [this] (int id) {
        ctxScaleKind = (int) sc::axes::Kind::Frequency;
        handleContextMenuAction (id);
    };

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
    // A refused sample-dot edit (read-only file, ...) goes to the status bar.
    waveformView->onEditError = [this] (const juce::String& s)
    {
        if (statusBar) statusBar->setMouseInfo (s);
    };

    // The views report the time under the mouse over either view; kept for
    // the status-bar probes and the tests (the Video window no longer follows it).
    auto onHoverTime = [this] (double t) { videoHoverTime = t; };
    waveformView->onHoverTime = onHoverTime;
    spectralView->onHoverTime = onHoverTime;

    controlsPanel->onResetFreq = [this] {
        if (spectralView) { spectralView->resetFrequencyRange(); spectralView->repaint(); }
    };

    // Spectral Controls > Refresh button (also F5): discard every cached tile
    // and force a full re-render of the spectrogram from the audio.
    controlsPanel->onRefresh = [this] { refreshSpectrogram (); };

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
    if (tablesTrack) tablesTrack->setTimeRange (t0, t1);

    refreshPanels();
    updateScrollBars();
}

void MainWindow::refreshAfterEdit()
{
    // The Video window flags audio / video length mismatches after edits.
    if (videoWindow != nullptr)
        videoWindow->setAudioDuration (app.audioEngine().isFileLoaded() ? app.audioEngine().duration() : 0.0);

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
    app.selectionRow = -1;            // a freshly loaded file spans all rows
    app.setSelection (app.selection); // re-clamp to the new duration

    syncSharedState();
    if (waveformView) waveformView->repaint();
    app.spectralEngine.clearCache();   // tiles are stale after any data change
    if (spectralView) spectralView->invalidateImage();
}

void MainWindow::resized()
{
    auto bounds = getLocalBounds();
    auto menuRow = bounds.removeFromTop (24);
    menuBar.setBounds (menuRow);
    // Workspace switcher at the right end of the menu bar row (Adobe-style),
    // over the menu bar's empty part (added after it: painted above it) so
    // the menu bar's background shows through.
    if (workspaceBar_ != nullptr)
    {
        const int w = std::min (workspaceBar_->idealWidth (), std::max (0, menuRow.getWidth () / 2));
        workspaceBar_->setBounds (menuRow.removeFromRight (w));
    }

    // Status bar at the very bottom, full window width. (resized() can run
    // from setUsingNativeTitleBar() during construction, before these members
    // exist, so guard every pointer member.)
    if (statusBar)
        statusBar->setBounds (bounds.removeFromBottom (20));

    // Everything in between is the docking workspace; the Editor panel is
    // one of its groups.
    if (dock_ != nullptr)
        dock_->mainArea ().setBounds (bounds);

    // The Editor's layout also depends on app state (open files, view
    // toggles, the tables track): callers run resized() to refresh it.
    layoutEditor ();
}

void MainWindow::layoutEditor()
{
    if (editorPanel_ == nullptr)
        return;
    const bool haveFile = app.audioEngine().isFileLoaded();
    auto rest = editorPanel_->getLocalBounds ();

    // Main area: waveform on top, draggable splitter, spectral below; a time
    // scrollbar along the bottom and one vertical scrollbar per view on its
    // right edge. Either view can be hidden from the View menu; at least one
    // is always visible and takes the whole main area when it is alone.
    const bool showWave = showWaveformView && (waveformView != nullptr);
    const bool showSpec = showSpectralView && (spectralView != nullptr);
    if (waveformView) waveformView->setVisible (showWave);
    if (spectralView) spectralView->setVisible (showSpec);

    // The shared time ruler lives in the top view when it is at the top and
    // in the bottom view (the spectrogram) when it is at the bottom; a lone
    // view always draws its own. The other view hides its strip and expands.
    const bool rulerBottom = app.settings.trackRulerAtBottom;
    if (waveformView) waveformView->setShowTimeRuler (!showSpec || !rulerBottom);
    if (spectralView) spectralView->setShowOwnTimeRuler (!showWave || rulerBottom);

    // Marker overlays: the waveform always draws its full overlay (vertical
    // lines + bracket line + titles). While it is stacked above, the
    // spectrogram shows only the vertical delimiter lines; a lone
    // spectrogram draws the full overlay itself.
    if (waveformView) waveformView->setShowMarkerTitles (true);
    if (spectralView) spectralView->setShowMarkerTitles (!showWave);

    // Time scrollbar at the bottom of the main area.
    if (timeBar) timeBar->setVisible (haveFile);
    auto mainArea = rest;
    if (haveFile)
        mainArea.removeFromBottom (18);

    const int vScrollW = 14;
    if (waveVScroll) waveVScroll->setVisible (showWave && haveFile);
    if (specVScroll) specVScroll->setVisible (showSpec && haveFile);

    // Dedicated tables track (Tables panel mode "Dedicated track"): a strip
    // above the data views. When a view owns the top time ruler the strip sits
    // just below that ruler inside the owning view's bounds; otherwise it
    // takes space from the top of the main area.
    const bool showTablesTrack = haveFile && app.tablesMode() == 2 && app.anyVisibleTableParam();
    if (tablesTrack)   tablesTrack->setVisible (showTablesTrack);
    if (tablesSep)     tablesSep->setVisible (showTablesTrack);
    if (tablesVScroll) tablesVScroll->setVisible (showTablesTrack);

    // The tracks' shared axis width grows with the axes in the Scale form, and
    // so does the ruler stack: the bars have to follow them, or the horizontal
    // bar would run under the axis strips and the vertical one past the data.
    const int axisW = app.axisStripWidth();
    const int axL = app.settings.trackAxisOnRight ? 0 : axisW;
    const int axR = app.settings.trackAxisOnRight ? axisW : 0;

    juce::Component* topRulerOwner = nullptr;   // view that draws the top ruler
    if (!rulerBottom && showWave)                    topRulerOwner = waveformView.get ();
    else if (!rulerBottom && !showWave && showSpec)  topRulerOwner = spectralView.get ();

    const int sepH = 6;
    const int tablesRefH = mainArea.getHeight();   // before the strip is removed
    int trackH = 0;
    juce::Rectangle<int> tablesRect;
    if (showTablesTrack)
        trackH = std::max (30, (int) std::lround ((double) tablesRefH * tablesTrackRatio));

    // Reserve the strip: inside the owning view via its top inset, or from the
    // top of the main area when no view owns a top ruler.
    if (waveformView)
        waveformView->setTablesTopInset (showTablesTrack && topRulerOwner == waveformView.get ()
                                          ? trackH + sepH : 0);
    if (spectralView)
        spectralView->setTablesTopInset (showTablesTrack && topRulerOwner == spectralView.get ()
                                          ? trackH + sepH : 0);
    if (showTablesTrack && topRulerOwner == nullptr)
        mainArea.removeFromTop (trackH + sepH);

    auto placeTablesStrip = [&] (juce::Rectangle<int> area) -> void
    {
        if (!showTablesTrack || !tablesTrack) return;
        // Same horizontal span as the data views: full width minus the
        // scrollbar strip. The view draws its own value-axis strip (left or
        // right, per the View menu) inside these bounds.
        tablesRect = juce::Rectangle<int> (area.getX(), area.getY(),
                                           std::max (0, area.getWidth() - vScrollW),
                                           trackH);
        if (topRulerOwner != nullptr)
            tablesRect.setY (area.getY() + app.rulerStackHeight());   // below the top ruler
        else
            tablesRect.setY (area.getY() - trackH - sepH);          // in the removed strip above
        tablesTrack->setBounds (tablesRect);
        if (tablesSep)
            tablesSep->setBounds (tablesRect.getX(), tablesRect.getBottom (),
                                  tablesRect.getWidth (), sepH);
        if (tablesVScroll)
            tablesVScroll->setBounds (area.getRight () - vScrollW, tablesRect.getY (),
                                      vScrollW, trackH);

        // Anchor for the separator drag: strip top + reference area height.
        tablesSepAnchorY = tablesRect.getY ();
        tablesSepRefH    = std::max (1.0, (double) tablesRefH);
    };

    if (splitter)
        splitter->setVisible (showWave && showSpec);

    auto placeViewWithScrollBar = [&](juce::Rectangle<int> full, juce::Component* view,
                                      juce::Component* vbar) -> void
    {
        // Shrink `full` by the scrollbar strip first so the view takes the rest.
        const int barX = full.getRight() - vScrollW;
        full = full.withSize (std::max (0, full.getWidth() - vScrollW), full.getHeight());
        view->setBounds (full);

        // The vertical scrollbar spans only the track data area: when this view
        // draws its own time ruler strip (top or bottom) that strip is excluded
        // so the bar stops at the actual data height.
        int topInset = 0, botInset = 0;
        vScrollInsetsForView (app, view, topInset, botInset);

        if (vbar) vbar->setBounds (juce::Rectangle<int> (barX, full.getY() + topInset,
                                                        vScrollW,
                                                        std::max (0, full.getHeight() - topInset - botInset)));
    };

    if (showWave && showSpec)
    {
        const int waveH = (int) std::lround (mainArea.getHeight() * splitRatio);
        const int barH = sc::ui::kSplitterH;
        placeViewWithScrollBar (juce::Rectangle<int> (mainArea.getX(), mainArea.getY(),
                                                      mainArea.getWidth(), waveH),
                                waveformView.get(), waveVScroll.get());
        splitter->setBounds (mainArea.getX(), mainArea.getY() + waveH,
                             mainArea.getWidth(), barH);
        placeViewWithScrollBar (juce::Rectangle<int> (mainArea.getX(), mainArea.getY() + waveH + barH,
                                                      mainArea.getWidth(),
                                                      std::max (0, mainArea.getHeight() - waveH - barH)),
                                spectralView.get(), specVScroll.get());
        placeTablesStrip (juce::Rectangle<int> (mainArea.getX(), mainArea.getY(),
                                                mainArea.getWidth(), waveH));
    }
    else if (showWave)
    {
        placeViewWithScrollBar (mainArea, waveformView.get(), waveVScroll.get());
        placeTablesStrip (mainArea);
    }
    else if (showSpec)
    {
        placeViewWithScrollBar (mainArea, spectralView.get(), specVScroll.get());
        placeTablesStrip (mainArea);
    }

    // Aligned with the views' plot area: inset by the axis strip on whichever
    // side it is placed and stopping where their vertical scrollbars begin,
    // so the thumb tracks exactly the visible data.
    if (haveFile && timeBar)
        timeBar->setBounds (mainArea.getX() + axL, mainArea.getBottom () + 2,
                            std::max (0, mainArea.getWidth () - vScrollW - axL - axR), 16);

    // Now that the bars have real sizes, sync their thumbs with the views.
    updateScrollBars();

    // The views only get their final row heights here, so this is where the
    // resolved Auto FFT size (and thus the combo's "Auto (N)" label) can
    // first become meaningful - and it changes again whenever the window or
    // the view split is resized.
    updateAutoFftLabel();
}

void MainWindow::handleTablesSepDrag (const juce::MouseEvent& e)
{
    if (!e.eventComponent || !tablesTrack || !editorPanel_) return;
    // tablesSepAnchorY is in Editor panel coordinates (layoutEditor()).
    const auto edTL = editorPanel_->localPointToGlobal (juce::Point<int> (0, 0));
    const auto g = e.eventComponent->localPointToGlobal (
        juce::Point<int> ((int) e.position.x, (int) e.position.y));
    const double yInEditor = (double) g.y - (double) edTL.y;
    const double h = std::clamp (yInEditor - tablesSepAnchorY, 30.0,
                                 tablesSepRefH * 0.8);
    tablesTrackRatio = std::clamp (h / tablesSepRefH, 0.05, 0.6);
    resized();
}

// ------------------------------------------------- Left panel layout

const char* MainWindow::panelIdFor (juce::Component* c) const
{
    if (c == fftPanel.get())            return "fft";
    if (c == historyPanel.get())        return "history";
    if (c == actionsPanel.get())        return "actions";
    if (c == pythonPanel.get())         return "python";
    if (c == tablesPanel.get())         return "tables";
    if (c == controlsPanel.get())       return "controls";
    if (c == markersPanel.get())        return "markers";
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    if (c == transcriptionPanel.get())  return "transcription";
#endif
    if (c == selectionPanel.get())      return "selection";
    if (c == drawingPanel.get())        return "drawing";
    if (c == videoDockHost.get())       return "video";
    return nullptr;
}

// Canonical list of the closable panels: id, tab name and which group of the
// default workspace's left column it starts in (top / bottom). The Window
// menu, the docking registration, the AI-control panel list and the default
// layouts iterate this list, so a new panel only needs one entry here (the
// Editor is registered separately: it cannot be closed).
std::vector<MainWindow::LeftPanelEntry> MainWindow::leftPanelEntries() const
{
    return {
        { "fft",         "Frequency Analysis", true  },
        { "history",     "History",            true  },
        { "actions",     "Actions",            true  },
        { "python",      "Python",             true  },
        { "tables",      "Tables",             true  },
        { "controls",    "Spectral Controls",  false },
        { "markers",     "Markers",            false },
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        { "transcription","Transcription",     false },
#endif
        { "selection",   "Selection",          false },
        { "drawing",     "Drawing",            false },
        { "video",       "Video",              true  },
    };
}

juce::Component* MainWindow::panelForId (const juce::String& id) const
{
    if (id == "editor")      return editorPanel_.get ();
    if (id == "fft")         return fftPanel.get ();
    if (id == "history")     return historyPanel.get ();
    if (id == "actions")     return actionsPanel.get ();
    if (id == "python")      return pythonPanel.get ();
    if (id == "tables")      return tablesPanel.get ();
    if (id == "controls")    return controlsPanel.get ();
    if (id == "markers")     return markersPanel.get ();
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    if (id == "transcription") return transcriptionPanel.get ();
#endif
    if (id == "selection")   return selectionPanel.get ();
    if (id == "drawing")     return drawingPanel.get ();
    if (id == "video")       return videoDockHost.get ();
    return nullptr;
}

bool MainWindow::isPanelOpen (const juce::String& id) const
{
    return dock_ != nullptr && dock_->isOpen (id);
}

void MainWindow::setPanelVisible (const juce::String& id, bool visible)
{
    if (dock_ == nullptr || dock_->panel (id) == nullptr || isPanelOpen (id) == visible)
        return;
    if (visible)
        dock_->openPanel (id);    // back where it was, else in its default group
    else
        dock_->closePanel (id);   // the Video panel then hands its picture back
                                  // to the Video window (handlePanelOpenChanged)
}

juce::StringArray MainWindow::panelGroupTitles (const juce::String& id) const
{
    juce::StringArray titles;
    if (dock_ != nullptr)
        for (const auto& t : dock_->groupTabs (id))
            if (const auto* d = dock_->panel (t))
                titles.add (d->title);
    return titles;
}

void MainWindow::saveDockLayout()
{
    if (dock_ == nullptr || dockLoading_)
        return;
    dockLayoutDirty_ = false;
    workspaces_.setCurrent (workspaces_.activeId (), dock_->saveLayout ());
    workspaces_.persist ();
    if (sc::diag::enabled ())
    {
        const auto* ws = workspaces_.find (workspaces_.activeId ());
        sc::diag::event ("ui.layout", sc::diag::Level::info,
                         { { "ws", ws != nullptr ? ws->name : workspaces_.activeId () },
                           { "floating", dock_->numFloatingWindows () } });
    }
}

void MainWindow::applyDockLayout (const juce::var& layout)
{
    {
        const juce::ScopedValueSetter<bool> loading (dockLoading_, true);
        if (! dock_->loadLayout (layout))
            dock_->loadLayout (builtInWorkspaceLayout ("default"));
    }
    // The Video panel shows the picture itself once the window exists.
    if (videoWindow != nullptr)
    {
        if (dock_->isOpen ("video"))
        {
            if (! isVideoDocked ())
                videoWindow->dockTo (*videoDockHost);
        }
        else if (isVideoDocked ())
        {
            videoWindow->undock ();
            videoWindow->setVisible (true);
        }
    }
    // The active document's Editor tab shows again, with the views in it.
    syncEditorTabs ();
    dockLayoutDirty_ = false;
    resized ();
}

void MainWindow::handlePanelOpenChanged (const juce::String& id, bool open)
{
    if (id == "video" && ! dockLoading_)
    {
        if (open)
        {
            // The tab shows the picture itself: move the view into the host.
            ensureVideoWindow ();
            if (! isVideoDocked ())
                videoWindow->dockTo (*videoDockHost);
        }
        else if (isVideoDocked ())
        {
            // Closing the docked tab sends the picture back to its own
            // window instead of leaving it hidden everywhere (except when
            // Window > Video hides it on purpose).
            videoWindow->undock ();
            if (! videoHideQuietly_)
            {
                videoWindow->setVisible (true);
                videoWindow->toFront (false);
            }
        }
    }
}

bool MainWindow::areLeftPanelsVisible() const
{
    return dock_ == nullptr || dock_->mainArea ().maximizedUid == 0;
}

void MainWindow::setLeftPanelsVisible (bool visible)
{
    // View > Panels (Ctrl+Shift+Enter): hiding the panels maximises the
    // Editor's group (Adobe's maximised frame); showing them restores all.
    if (dock_ == nullptr || visible == areLeftPanelsVisible ())
        return;
    if (visible)
    {
        dock_->restoreMaximized ();
        return;
    }
    const auto loc = dock_->locate ("editor");
    if (loc.area == &dock_->mainArea ())
        dock_->toggleMaximize (loc.group->uid);
}

void MainWindow::toggleMaximizePanelGroup()
{
    if (dock_ == nullptr)
        return;
    // The group under the mouse, else the focused one.
    const auto mouse = juce::Desktop::getMousePosition ();
    for (auto* area : dock_->areas ())
        if (area->isShowing () && area->getScreenBounds ().contains (mouse))
            if (auto* g = area->groupAt (area->getLocalPoint (nullptr, mouse)))
            {
                dock_->toggleMaximize (g->getUid ());
                return;
            }
    if (dock_->focusedGroup () != 0)
        dock_->toggleMaximize (dock_->focusedGroup ());
}

void MainWindow::activeWindowStatusChanged()
{
    juce::DocumentWindow::activeWindowStatusChanged ();
    // Floating panel windows behave like tool windows of this one: when it
    // comes to the front they come back above it.
    if (dock_ != nullptr && isActiveWindow ())
        dock_->bringFloatingWindowsToFront ();
}

// ------------------------------------------------- Workspaces

namespace
{
juce::var dockGroup (std::initializer_list<const char*> tabs, const char* active = nullptr)
{
    auto* o = new juce::DynamicObject ();
    juce::Array<juce::var> t;
    for (const auto* id : tabs)
        t.add (juce::String (id));
    o->setProperty ("tabs", t);
    o->setProperty ("active", juce::String (active != nullptr ? active : *tabs.begin ()));
    return juce::var (o);
}

juce::var dockSplit (bool horizontal, std::initializer_list<std::pair<juce::var, double>> kids)
{
    auto* o = new juce::DynamicObject ();
    o->setProperty ("split", horizontal ? "h" : "v");
    juce::Array<juce::var> c, w;
    for (const auto& [node, weight] : kids)
    {
        c.add (node);
        w.add (weight);
    }
    o->setProperty ("children", c);
    o->setProperty ("weights", w);
    return juce::var (o);
}

juce::var dockLayout (const juce::var& main, const juce::StringArray& closed = {},
                      const juce::String& maximized = {})
{
    auto* o = new juce::DynamicObject ();
    o->setProperty ("version", 1);
    o->setProperty ("main", main);
    o->setProperty ("floating", juce::Array<juce::var> ());
    juce::Array<juce::var> c;
    for (const auto& id : closed)
        c.add (id);
    o->setProperty ("closed", c);
    if (maximized.isNotEmpty ())
        o->setProperty ("maximized", maximized);
    return juce::var (o);
}
} // namespace

// The built-in workspaces. Panels a build does not have (Transcription
// without AI) are skipped when a layout is applied.
juce::var MainWindow::builtInWorkspaceLayout (const juce::String& id) const
{
    if (id == "editing")
        // Editor in the middle, files / history and markers on the left,
        // spectral tools on the right, tables and video below the Editor.
        return dockLayout (dockSplit (true, {
            { dockSplit (false, {
                  { dockGroup ({ "history", "actions", "python" }), 0.5 },
                  { dockGroup ({ "markers", "transcription", "selection" }), 0.5 } }), 0.2 },
            { dockSplit (false, {
                  { dockGroup ({ "editor" }), 0.74 },
                  { dockGroup ({ "tables", "video" }), 0.26 } }), 0.57 },
            { dockSplit (false, {
                  { dockGroup ({ "controls", "drawing" }), 0.55 },
                  { dockGroup ({ "fft" }), 0.45 } }), 0.23 } }));

    if (id == "analysis")
        // A wide Frequency Analysis beside the Editor, three groups below.
        return dockLayout (dockSplit (false, {
            { dockSplit (true, {
                  { dockGroup ({ "editor" }), 0.6 },
                  { dockGroup ({ "fft" }), 0.4 } }), 0.66 },
            { dockSplit (true, {
                  { dockGroup ({ "controls", "selection", "drawing" }), 0.34 },
                  { dockGroup ({ "markers", "transcription", "history" }), 0.33 },
                  { dockGroup ({ "tables", "actions", "python", "video" }), 0.33 } }), 0.34 } }));

    if (id == "video")
        // The picture large above the Editor.
        return dockLayout (dockSplit (true, {
            { dockSplit (false, {
                  { dockGroup ({ "history", "actions", "python", "tables" }), 0.5 },
                  { dockGroup ({ "markers", "transcription", "selection", "drawing" }), 0.5 } }), 0.2 },
            { dockSplit (false, {
                  { dockSplit (true, {
                        { dockGroup ({ "video" }), 0.55 },
                        { dockGroup ({ "controls", "fft" }), 0.45 } }), 0.46 },
                  { dockGroup ({ "editor" }), 0.54 } }), 0.8 } }));

    // "default": the classic arrangement - a left column with two groups
    // beside the Editor.
    return dockLayout (dockSplit (true, {
        { dockSplit (false, {
              { dockGroup ({ "fft", "history", "actions", "python", "tables", "video" }, "fft"), 0.52 },
              { dockGroup ({ "controls", "markers", "transcription", "selection", "drawing" }), 0.48 } }), 0.245 },
        { dockGroup ({ "editor" }), 0.755 } }));
}

std::vector<sc::dock::Workspaces::Entry> MainWindow::builtInWorkspaces() const
{
    std::vector<sc::dock::Workspaces::Entry> list;
    for (const auto& [id, name] : { std::pair<const char*, const char*> { "default", "Default" },
                                    { "editing", "Editing" },
                                    { "analysis", "Analysis" },
                                    { "video", "Video" } })
    {
        sc::dock::Workspaces::Entry e;
        e.id = id;
        e.name = name;
        e.builtIn = true;
        e.saved = builtInWorkspaceLayout (id);
        list.push_back (e);
    }
    return list;
}

juce::var MainWindow::legacyLeftLayout() const
{
    // Settings of the former left column: "top ids|bottom ids", the closed
    // panels, the column width and the top group's share of its height.
    auto ids = [] (const juce::String& s)
    {
        auto a = juce::StringArray::fromTokens (s, ";", "");
        a.trim ();
        a.removeEmptyStrings ();
        return a;
    };
    const juce::String saved = app.recentStore->getValue ("left_tab_layout");
    const int sep = saved.indexOfChar ('|');
    const juce::StringArray closed = ids (app.recentStore->getValue ("left_panels_closed"));
    juce::StringArray top = ids (sep < 0 ? saved : saved.substring (0, sep));
    juce::StringArray bottom = ids (sep < 0 ? juce::String () : saved.substring (sep + 1));
    for (const auto& id : closed)
    {
        top.removeString (id);
        bottom.removeString (id);
    }

    const int leftW = std::clamp (app.recentStore->getIntValue ("left_panel_w", 340), 220, 4000);
    const double topRatio = std::clamp (app.recentStore->getIntValue ("top_ratio_mil", 520) / 1000.0, 0.15, 0.85);
    const double leftShare = std::clamp ((double) leftW / 1400.0, 0.1, 0.6);

    auto group = [] (const juce::StringArray& tabs, const juce::String& preferred)
    {
        auto* o = new juce::DynamicObject ();
        juce::Array<juce::var> t;
        for (const auto& id : tabs)
            t.add (id);
        o->setProperty ("tabs", t);
        o->setProperty ("active", tabs.contains (preferred) ? preferred : tabs[0]);
        return juce::var (o);
    };

    juce::var column;
    if (! top.isEmpty () && ! bottom.isEmpty ())
        column = dockSplit (false, { { group (top, "fft"), topRatio }, { group (bottom, "controls"), 1.0 - topRatio } });
    else if (! top.isEmpty ())
        column = group (top, "fft");
    else if (! bottom.isEmpty ())
        column = group (bottom, "controls");

    const juce::var main = column.isVoid () ? dockGroup ({ "editor" })
                                            : dockSplit (true, { { column, leftShare }, { dockGroup ({ "editor" }), 1.0 - leftShare } });
    const bool panelsHidden = app.recentStore->getIntValue ("left_panels_visible", 1) == 0;
    return dockLayout (main, closed, panelsHidden ? "editor" : "");
}

void MainWindow::switchWorkspace (const juce::String& id)
{
    if (workspaces_.find (id) == nullptr || id == workspaces_.activeId ())
        return;
    saveDockLayout ();   // the workspace we leave keeps its modifications
    workspaces_.setActiveId (id);
    applyDockLayout (workspaces_.layoutFor (id));
    workspaces_.persist ();
    if (workspaceBar_ != nullptr)
        workspaceBar_->repaint ();
}

void MainWindow::resetWorkspace()
{
    const auto id = workspaces_.activeId ();
    workspaces_.resetCurrent (id);
    applyDockLayout (workspaces_.layoutFor (id));
    workspaces_.persist ();
}

void MainWindow::saveWorkspaceChanges()
{
    saveDockLayout ();
    workspaces_.saveCurrentAsSaved (workspaces_.activeId ());
    workspaces_.persist ();
}

juce::String MainWindow::saveWorkspaceAs (const juce::String& name)
{
    if (name.trim ().isEmpty () || dock_ == nullptr)
        return {};
    const auto id = workspaces_.add (name.trim (), dock_->saveLayout ());
    workspaces_.setActiveId (id);
    workspaces_.persist ();
    resized ();   // the workspace bar grows
    if (workspaceBar_ != nullptr)
        workspaceBar_->repaint ();
    return id;
}

bool MainWindow::deleteWorkspace (const juce::String& id)
{
    const bool wasActive = id == workspaces_.activeId ();
    if (! workspaces_.remove (id))
        return false;
    if (wasActive)
        applyDockLayout (workspaces_.layoutFor (workspaces_.activeId ()));
    workspaces_.persist ();
    resized ();
    if (workspaceBar_ != nullptr)
        workspaceBar_->repaint ();
    return true;
}

bool MainWindow::renameWorkspace (const juce::String& id, const juce::String& name)
{
    if (! workspaces_.rename (id, name))
        return false;
    workspaces_.persist ();
    resized ();
    if (workspaceBar_ != nullptr)
        workspaceBar_->repaint ();
    return true;
}

void MainWindow::buildWorkspaceMenu (juce::PopupMenu& m, const juce::String& clickedId)
{
    const auto& entries = workspaces_.entries ();
    for (size_t i = 0; i < entries.size () && i < 48; ++i)
        m.addItem (WindowWorkspaceBase + (int) i, entries[i].name, true,
                   entries[i].id == workspaces_.activeId ());
    m.addSeparator ();
    m.addItem (WindowWsReset, "Reset to Saved Layout");
    m.addItem (WindowWsSaveChanges, "Save Changes to this Workspace");
    m.addItem (WindowWsSaveAs, "Save as New Workspace...");
    const auto* target = workspaces_.find (clickedId.isNotEmpty () ? clickedId : workspaces_.activeId ());
    m.addSeparator ();
    m.addItem (WindowWsRename, "Rename Workspace...", target != nullptr);
    m.addItem (WindowWsDelete, "Delete Workspace", target != nullptr && ! target->builtIn);
}

void MainWindow::handleWorkspaceCommand (int menuId, const juce::String& clickedId)
{
    const juce::String target = clickedId.isNotEmpty () ? clickedId : workspaces_.activeId ();
    if (menuId >= WindowWorkspaceBase && menuId < WindowWorkspaceBase + 48)
    {
        const auto i = (size_t) (menuId - WindowWorkspaceBase);
        if (i < workspaces_.entries ().size ())
            switchWorkspace (workspaces_.entries ()[i].id);
        return;
    }
    if (menuId == WindowWsReset)       { resetWorkspace (); return; }
    if (menuId == WindowWsSaveChanges) { saveWorkspaceChanges (); return; }
    if (menuId == WindowWsSaveAs)
    {
        const auto name = promptForWorkspaceName ("Save as New Workspace",
                                                  "Workspace " + juce::String ((int) workspaces_.entries ().size () + 1));
        if (name.isNotEmpty ())
            saveWorkspaceAs (name);
        return;
    }
    if (menuId == WindowWsRename)
    {
        if (const auto* e = workspaces_.find (target))
        {
            const auto name = promptForWorkspaceName ("Rename Workspace", e->name);
            if (name.isNotEmpty ())
                renameWorkspace (target, name);
        }
        return;
    }
    if (menuId == WindowWsDelete)
    {
        const auto* e = workspaces_.find (target);
        if (e == nullptr || e->builtIn)
            return;
        if (! sc::alert::isHeadless ()
            && ! juce::AlertWindow::showOkCancelBox (juce::AlertWindow::QuestionIcon, "Delete Workspace",
                                                     "Delete the workspace \"" + e->name + "\"?", "Delete", "Cancel", this))
            return;
        deleteWorkspace (target);
    }
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
    // Never quit under a running worker: it still uses the documents (and
    // this window) and deletes its progress dialog when it finishes.
    if (isBackgroundJobRunning ()) return;
    JUCEApplication::getInstance()->systemRequestedQuit();
}

// With a native title bar the X button posts WM_CLOSE, which routes here
// instead of to closeButtonPressed() (both base implementations are no-op
// asserts in this JUCE build).
void MainWindow::userTriedToCloseWindow()
{
    closeButtonPressed ();
}

void MainWindow::timerCallback()
{
    // Docking: save the arrangement shortly after the last change (a gap
    // drag or a burst of tab clicks writes the settings once).
    if (dockLayoutDirty_ && juce::Time::getMillisecondCounter () - dockLayoutDirtyAt_ > 400)
        saveDockLayout ();
    // Floating panel windows go away with the minimised main window.
    if (dock_ != nullptr && isMinimised () != floatersHiddenForMinimise_)
    {
        floatersHiddenForMinimise_ = isMinimised ();
        dock_->setFloatingWindowsVisible (! floatersHiddenForMinimise_);
    }

    // AI control journal: edits made with the mouse/menus become "history"
    // entries (and history.changed events) within a tick. Only while the
    // control server runs: nobody else reads the journal.
    if (controlServerRunning)
        app.control().pollDocuments();

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
    // (setModeText() is a no-op when nothing changed). While recording, show a
    // REC indicator with the elapsed capture length.
    if (statusBar)
    {
        juce::String mode = currentModeText ();
        const bool recNow = app.recording.active && app.recDoc () != nullptr;
        if (recNow)
        {
            // The red dot is drawn by the status bar itself (setRecording);
            // keeping this string pure ASCII avoids font-glyph issues.
            const double secs = app.recDoc ()->engine.duration ();
            // Tag loopback sessions so it's obvious what is being captured
            // (audio playing on that output device, not a microphone).
            const juce::String lbTag = app.recording.deviceId.startsWith ("loopback:")
                                       ? " [loopback]" : juce::String ();
            mode = "REC " + juce::String (secs, 2) + " s" + lbTag
                   + (mode.isEmpty () ? juce::String () : "   " + mode);
        }
        statusBar->setRecording (recNow);
        statusBar->setModeText (mode);
        statusBar->setDiagActive (sc::diag::enabled ());
    }

    // The Frequency Analysis panel follows the playhead while playing.
    if (fftPanel) { fftPanel->setFollowPlayhead (app.audioEngine().isPlaying()); fftPanel->repaint(); }

    if (!app.audioEngine().isPlaying() || !waveformView || !spectralView)
        return;
    const double pos = app.audioEngine().position();
    app.playhead = pos; // the UI cursor follows playback
    app.syncActiveDoc ();   // remember it in the active document
    waveformView->setPlayhead (pos);
    spectralView->setPlayhead (pos);
    if (tablesTrack) tablesTrack->setPlayhead (pos);   // keep the dedicated track in step
    if (tablesPanel) tablesPanel->plotView().setPlayhead (pos);   // ... and the Tables preview plot
    if (videoWindow) videoWindow->getTracksPanel().setPlayhead (pos);
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
    // Whole-token match: a substring test accepted files without an
    // extension (contains ("") is true) and partial ones such as ".wa".
    static const juce::StringArray supported =
        juce::StringArray::fromTokens (allOpenableExtensions(), " ", "");
    for (const auto& f : files)
    {
        // CSV files are not audio: they import as tables in the Tables panel
        // (filesDropped routes them to addTableFromCsv).
        const auto ext = juce::File (f).getFileExtension().toLowerCase();
        if (ext == ".csv" || (ext.isNotEmpty() && supported.contains (ext)))
            return true;
    }
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

    // CSV files become tables in the Tables panel instead of audio docs.
    std::vector<juce::File> csvs;
    for (const auto& f : valid)
    {
        if (f.getFileExtension ().equalsIgnoreCase (".csv"))
            csvs.push_back (f);
        else
            openFile (f);   // multi-file editor: every valid drop becomes a document
    }
    importCsvTables (std::move (csvs));
}

void MainWindow::importCsvTables (std::vector<juce::File> files)
{
    if (files.empty ())
        return;
    const juce::File f = files.front ();
    files.erase (files.begin ());
    // One file at a time: the next starts once this one is in (or failed).
    auto next = [this, files] { importCsvTables (files); };
    auto failed = [f] (const std::string& err)
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
            "Could not import table \"" + f.getFileName () + "\":\n"
            + juce::String (err), "Tables");
    };

    FileDoc* d = app.doc ();
    if (d == nullptr || f.getSize () < kCsvProgressBytes)
    {
        std::string err;
        if (!app.addTableFromCsv (f.getFileNameWithoutExtension (), f.loadFileAsString (), &err))
            failed (err);
        next ();
        return;
    }

    // Issue #14: a big CSV (tens of MB) takes seconds to read and parse; do
    // it on a worker with a progress dialog (Cancel / Esc), and add the
    // table to the document it was dropped on once it is ready.
    const int docId = d->id;
    const double docDuration = app.audioEngine ().duration ();
    auto* dlg = new WorkProgressDialog ("Importing table",
                                        "Reading " + f.getFileName () + " ...", true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, dlg, f, docId, docDuration, next, failed] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v)
        {
            if (v - lastSent < 0.01 && v < 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };
        auto table = std::make_shared<sc::tables::TableData> ();
        std::string err;
        // Reading the file is the first 10 %, the import the rest.
        const juce::String text = f.loadFileAsString ();
        sendProgress (0.1);
        const bool ok = ! dlg->cancelled.load ()
            && sc::tables::importTable (f.getFileNameWithoutExtension (), text, *table, &err, docDuration,
                                        [&] (double frac)
                                        {
                                            sendProgress (0.1 + 0.9 * frac);
                                            return ! dlg->cancelled.load ();
                                        });

        juce::MessageManager::callAsync ([this, dlg, ok, err, table, docId, next, failed] {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (ok && ! wasCancelled)
            {
                std::string addErr;
                if (! app.addImportedTable (docId, std::move (*table), &addErr))
                    failed (addErr);
            }
            else if (! wasCancelled)
            {
                failed (err);
            }
            next ();   // a cancel stops this file only: the next ones still import
        });
    }).detach ();
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

// ============================================================================
// Recording (plugin-based capture; see include/RecorderPlugin.h)
// ============================================================================

std::vector<MainWindow::RecDeviceMenuItem> MainWindow::recordingDeviceItems (
        const Application& app, int pluginIndex)
{
    std::vector<RecDeviceMenuItem> out;
    const auto& plugins = sc::rec::RecorderHost::instance ().plugins ();
    if (pluginIndex < 0 || pluginIndex >= (int) plugins.size ())
        return out;

    const auto& p = plugins[(size_t) pluginIndex];
    // 1000 IDs per plugin (see RecDeviceBase): extra devices would alias the next plugin's.
    for (size_t j = 0; j < std::min<size_t> (p.devices.size (), 1000); ++j)
    {
        RecDeviceMenuItem item;
        item.id      = (int) (RecDeviceBase + pluginIndex * 1000 + j);
        item.text    = p.devices[j].name;
        item.checked = app.recording.pluginId == p.id
                       && app.recording.deviceId == p.devices[j].id;
        out.push_back (item);
    }
    return out;
}

juce::PopupMenu MainWindow::buildRecordingDeviceItems (const Application& app, int pluginIndex)
{
    juce::PopupMenu m;
    const auto items = recordingDeviceItems (app, pluginIndex);
    if (items.empty ())
    {
        m.addItem (RecNoDeviceItem, "(no devices)", false);
        return m;
    }
    for (const auto& item : items)
        m.addItem (item.id, item.text, true, item.checked);
    return m;
}

std::vector<MainWindow::RecOptionMenuItem> MainWindow::recordingOptionItems (
        const Application& app)
{
    std::vector<RecOptionMenuItem> out;
    const auto& r = app.recording;

    RecOptionMenuItem unlim;
    unlim.id      = RecOptUnlimited;
    unlim.text    = "Unlimited";
    unlim.checked = (r.mode == Application::RecordingState::Mode::Unlimited);
    out.push_back (unlim);

    juce::String loopText = "Loop Mode";
    if (r.durationSecs > 0.0)
        loopText += " (" + app.formatRecDurationSecs (r.durationSecs) + ")";
    else
        loopText += "...";
    RecOptionMenuItem loop;
    loop.id      = RecOptLoop;
    loop.text    = loopText;
    loop.checked = (r.mode == Application::RecordingState::Mode::Loop);
    out.push_back (loop);

    juce::String timedText = "Timed";
    if (r.durationSecs > 0.0)
        timedText += " (" + app.formatRecDurationSecs (r.durationSecs) + ")";
    else
        timedText += "...";
    RecOptionMenuItem timed;
    timed.id      = RecOptTimed;
    timed.text    = timedText;
    timed.checked = (r.mode == Application::RecordingState::Mode::Timed);
    out.push_back (timed);

    return out;
}

juce::PopupMenu MainWindow::buildRecordingDeviceMenu (const Application& app)
{
    // Scan on every menu build so plugins dropped into a recorders/ folder
    // after startup show up without a restart. (Device lists are cached per
    // plugin load; new hardware appears on the next app start.)
    sc::rec::RecorderHost::instance ().scanPlugins ();

    juce::PopupMenu m;
    const auto& plugins = sc::rec::RecorderHost::instance ().plugins ();
    if (plugins.empty ())
    {
        m.addItem (RecNoDeviceItem, "(no recorder plugins found)", false);
        return m;
    }

    for (int i = 0; i < (int) plugins.size (); ++i)
        m.addSubMenu (plugins[(size_t) i].name,
                     buildRecordingDeviceItems (app, i));
    return m;
}

void MainWindow::handleStartStopRecording ()
{
    if (app.recording.active)
    {
        app.stopRecording ();
        syncSharedState ();
        return;
    }

    std::string err;
    if (!app.startRecording (&err))
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                           "Recording",
                                           juce::String::fromUTF8 (err.c_str ()));
}

juce::StringArray MainWindow::recChannelComboItems (const sc::rec::DeviceInfo& dev)
{
    juce::StringArray items;
    if (dev.minChannels == dev.maxChannels && dev.minChannels >= 1)
    {
        // Fixed-width device (e.g. a WASAPI endpoint): offer explicit
        // per-channel selection in addition to recording everything.
        const int N = std::min (dev.maxChannels, 256);
        items.add ("All (" + juce::String (N) + ")");
        for (int c = 1; c <= N; ++c)
            items.add ("Channel " + juce::String (c));
    }
    else
    {
        // Variable-width device: every count it offers.
        for (int c = dev.minChannels; c <= std::min (dev.maxChannels, dev.minChannels + 127); ++c)
            items.add (juce::String (c));
    }
    return items;
}

void MainWindow::handleRecDevicePick (int id)
{
    auto& host = sc::rec::RecorderHost::instance ();
    const auto& plugins = host.plugins ();
    const int pi = (id - RecDeviceBase) / 1000;
    const int di = (id - RecDeviceBase) % 1000;
    if (pi < 0 || pi >= (int) plugins.size ()) return;
    const auto& p = plugins[pi];
    if (di < 0 || di >= (int) p.devices.size ()) return;

    const sc::rec::DeviceInfo& dev = p.devices[(size_t) di];

    // --- Build the format dialog ------------------------------------------
    juce::AlertWindow dlg ("Recording Device", "", juce::AlertWindow::NoIcon);
    dlg.addButton ("OK", 1, {});
    dlg.addButton ("Cancel", 0, {});
    dlg.addTextBlock (dev.name + "   [" + p.name + "]");

    // Channels: per-channel options for fixed-width devices, every count the
    // device offers otherwise.
    const bool perChannel = (dev.minChannels == dev.maxChannels && dev.minChannels >= 1);
    juce::StringArray chItems = recChannelComboItems (dev);

    // Sample rates: the device's list, or common rates inside its range.
    juce::StringArray rateItems;
    if (!dev.rates.empty ())
    {
        for (double r : dev.rates)
            rateItems.add (juce::String ((int) std::lround (r)) + " Hz");
    }
    else
    {
        const double common[] = { 8000.0, 16000.0, 22050.0, 32000.0,
                                  44100.0, 48000.0, 88200.0, 96000.0 };
        for (double r : common)
            if (r >= dev.minRate && r <= dev.maxRate)
                rateItems.add (juce::String ((int) std::lround (r)) + " Hz");
    }

    // Bit depths: the device's list, or the usual three.
    juce::StringArray depthItems;
    if (!dev.depths.empty ())
        for (int b : dev.depths)
            depthItems.add (juce::String (b) + " bit");
    else
    {
        depthItems.add ("16 bit");
        depthItems.add ("24 bit");
        depthItems.add ("32 bit");
    }

    dlg.addComboBox ("Channels", chItems);
    dlg.addComboBox ("Sample rate", rateItems);
    dlg.addComboBox ("Bit depth", depthItems);

    // Preselect the currently stored values when they are valid for this device.
    auto pickIndex = [] (const juce::StringArray& items, const juce::String& want) {
        int idx = items.indexOf (want);
        return idx >= 0 ? idx + 1 : 1;
    };
    if (auto* c = dlg.getComboBoxComponent ("Channels"))
    {
        int selId;
        if (perChannel)
        {
            const int m = app.recording.channelMode;   // 0..N
            selId = (m >= 0 && m < (int) chItems.size ()) ? m + 1 : 1;
        }
        else
            selId = pickIndex (chItems, juce::String (app.recording.channels));
        c->setSelectedId (selId, juce::dontSendNotification);
    }
    if (auto* c = dlg.getComboBoxComponent ("Sample rate"))
        c->setSelectedId (pickIndex (rateItems,
                                     juce::String ((int) std::lround (app.recording.rate)) + " Hz"),
                          juce::dontSendNotification);
    if (auto* c = dlg.getComboBoxComponent ("Bit depth"))
        c->setSelectedId (pickIndex (depthItems, juce::String (app.recording.bitDepth) + " bit"),
                          juce::dontSendNotification);

    const int result = dlg.runModalLoop ();
    if (result != 1)
        return;

    auto* chC   = dlg.getComboBoxComponent ("Channels");
    auto* rateC = dlg.getComboBoxComponent ("Sample rate");
    auto* depC  = dlg.getComboBoxComponent ("Bit depth");
    if (chC == nullptr || rateC == nullptr || depC == nullptr)
        return;

    const int chIdx  = std::max (0, chC->getSelectedItemIndex ());
    int wantCh, mode;
    if (perChannel)
    {
        mode   = chIdx;               // 0 = all channels, k = channel k
        wantCh = dev.maxChannels;     // the plugin always captures its full width
    }
    else
    {
        mode   = 0;
        wantCh = chItems[(size_t) chIdx].getIntValue ();
    }
    const double wantRate = rateItems[(size_t) std::max (0, rateC->getSelectedItemIndex ())].getDoubleValue ();
    const int wantDepth   = depthItems[(size_t) std::max (0, depC->getSelectedItemIndex ())].getIntValue ();

    sc_rec_negotiated neg {};
    std::string err;
    if (!host.configure (p.id, dev.id, wantCh, wantRate, wantDepth, &neg, &err))
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                           "Recording Device",
                                           juce::String::fromUTF8 (err.c_str ()));
        return;
    }

    app.recording.pluginId   = p.id;
    app.recording.deviceId   = dev.id;
    app.recording.channelMode = mode;
    app.recording.srcChannels = neg.channels;
    app.recording.channels   = (mode >= 1) ? 1 : neg.channels;
    app.recording.rate       = neg.rate;
    app.recording.bitDepth   = neg.bitDepth;
    app.saveSettings ();
    syncSharedState ();
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
                   juce::String (app.brushPctValue ()) + "%";
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

    // An open Scale form mirrors the document's axes: if something else changed
    // them (an undo, another session), it has to follow, or its controls would
    // act on the state they were built from. refreshFromApp() does nothing when
    // nothing changed, so typing in a field is never thrown away.
    if (scaleForm) scaleForm->refreshFromApp();

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
        // A log/linear frequency switch must remap the vertical bar now,
        // not at the next zoom.
        if (specVScroll && specVScroll->isLogarithmic () != spectralView->isLogFrequency ())
            updateScrollBars ();
    }
    if (tablesPanel)
    {
        tablesPanel->plotView().setPlayhead (app.playhead);
        tablesPanel->plotView().setSelection (app.selection);   // blue band in the preview plot
    }
    if (videoWindow) videoWindow->getTracksPanel().setSelection (app.selection);
    if (tablesTrack)
    {
        tablesTrack->setPlayhead (app.playhead);
        tablesTrack->setSelection (app.selection);   // dedicated track's blue band follows the shared selection
        // Cheap repaint: marker delimiter lines read app.markers() live in
        // paint(), so a changed list must force one even though no setter
        // carries it.
        tablesTrack->repaint();
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
        // Channel numbers: dim when hidden, red when muted.
        statusBar->setChannels (app.audioEngine().isFileLoaded() ? app.audioEngine().numChannels() : 0,
                                app.visibleChannels, app.audioEngine().mutedChannels());

        // Active spectral tool indicator + live SHIFT/CTRL selection hint.
        statusBar->setModeText (currentModeText());
    }

    updateScrollBars();
}

void MainWindow::refreshPanels()
{
    if (controlsPanel)  controlsPanel->refresh();
    if (markersPanel)   markersPanel->refresh();
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    if (transcriptionPanel) transcriptionPanel->refresh();
#endif
    if (selectionPanel)   selectionPanel->refresh();
    if (drawingPanel)     drawingPanel->refresh();
    if (tablesPanel)      tablesPanel->refresh();
    if (historyPanel)   historyPanel->refresh();
    if (fftPanel)       fftPanel->repaint();
    updateAutoFftLabel ();   // keep the "Auto (N)" combo label in sync
}

// Files bigger than this are loaded on a background thread with a progress
// dialog so the UI stays responsive during I/O + mipmap construction.
static const int64_t kLargeFileBytes = 200ll * 1024 * 1024;

void MainWindow::openFile (const juce::File& file)
{
    // A worker still uses the active document (e.g. a file handed over by a
    // second instance during a long edit): activating another one now would
    // retarget it. Open the file once the job has finished.
    if (isBackgroundJobRunning ())
    {
        juce::Component::SafePointer<MainWindow> safe (this);
        juce::Timer::callAfterDelay (250, [safe, file] { if (safe != nullptr) safe->openFile (file); });
        return;
    }

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
        showOpenFailed (file);
}

void MainWindow::showOpenFailed (const juce::File& file)
{
    sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                       "Open failed",
                                       "Could not open " + file.getFullPathName ()
                                           + ".\nThe file may be missing, damaged or in an unsupported format.",
                                       "", this);
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
    // Not a worker job (nothing is touched until the device is ready): keep
    // AlertWindow's ESC-to-dismiss as an escape hatch should the driver hang.
    audioInitDialog->setEscapeKeyCancels (true);
    audioInitDialog->beginModal();
}

void MainWindow::showEditError (const std::string& err)
{
    sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                       "Effect failed",
                                       err.empty () ? "The effect could not be applied."
                                                     : juce::String (err),
                                       "", this);
}

void MainWindow::runDestructiveEdit(std::function<bool(std::string*)> edit,
                                    std::function<void(bool)> onDone)
{
    auto& eng = app.audioEngine();
    if (!eng.isFileLoaded () || eng.fullyInMemory ())
    {   // Fast path: already in memory (or nothing loaded - the edit is a no-op).
        std::string err;
        const bool ok = edit (&err);
        if (ok) refreshAfterEdit();
        else showEditError (err);
        if (onDone) onDone (ok);
        return;
    }

    // Streamed file: materialise it into memory first. Reading multi-GB files
    // can take seconds, so run that on a worker thread behind a progress
    // dialog instead of freezing the UI.
    auto* dlg = new WorkProgressDialog ("Preparing file for editing",
                                        "Loading the file into memory ...", true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    // The engine is resolved here, on the message thread: the worker must
    // materialise the document the edit was requested on.
    AudioEngine* engPtr = &eng;
    std::thread ([this, edit, dlg, onDone, engPtr] {
        std::string err;
        bool ok = engPtr->ensureEditable (&err,
                                                    [dlg] (float f) {
                                                        juce::MessageManager::callAsync (
                                                            [dlg, f] { dlg->setProgress (0.9f * f); });
                                                    },
                                                    &dlg->cancelled);
        if (ok)
            ok = edit (&err);   // now in memory; only an OOM inside the edit can fail

        juce::MessageManager::callAsync ([this, dlg, ok, err, onDone] {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (ok) refreshAfterEdit();
            else if (!wasCancelled) showEditError (err);
            if (onDone) onDone (ok && !wasCancelled);
        });
    }).detach();
}

// ---------------------------------------------------------------------------
// Actions panel: interactive replay of a recorded action.
//
// Steps run in order. Prompt steps open the normal dialog for their type and
// continue when it applies; non-prompt steps apply the recorded values
// directly. The three long-running step types (resample, transcription,
// Python) go through their async helpers so the UI stays responsive.

void MainWindow::runAction (int index)
{
    const auto* act = app.actions.get (index);
    if (act == nullptr || act->steps.empty ()) return;
    if (!app.audioEngine().isFileLoaded())
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Run Action",
                                            "Open a file first - actions are replayed on the active document.",
                                            "", this);
        return;
    }

    replaySteps_ = act->steps;
    replayPos_ = 0;
    replayName_ = act->name;
    continueReplay ();
}

void MainWindow::stopReplay (const juce::String& why)
{
    const auto name = replayName_;
    replaySteps_.clear ();
    replayPos_ = 0;
    replayName_.clear ();
    if (onReplayFinished)   // a caller that reports itself (no blocking message box)
    {
        auto cb = onReplayFinished;
        cb (false, "Action \"" + name + "\" stopped: " + why);
        return;
    }
    if (!name.isEmpty ())
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Run Action",
                                            "Action \"" + name + "\" stopped: " + why, "", this);
}

void MainWindow::continueReplay()
{
    if (replayPos_ >= (int) replaySteps_.size ())
    {
        const auto name = replayName_;
        replaySteps_.clear ();
        replayPos_ = 0;
        replayName_.clear ();
        if (onReplayFinished)
        {
            auto cb = onReplayFinished;
            cb (true, "Action \"" + name + "\" finished.");
            return;
        }
        sc::alert::showMessageBox (juce::AlertWindow::InfoIcon,
                                           "Run Action",
                                           "Action \"" + name + "\" finished.", "", this);
        return;
    }

    const auto s = replaySteps_[(size_t) replayPos_++];   // copy: callbacks may clear the queue
    using namespace sc::actionTypes;
    const auto t = s.type;

    auto cont = [this] (bool ok) {
        if (ok) continueReplay ();
        else stopReplay ("a step failed or was cancelled");
    };

    // Prompt steps: open the normal dialog for this type; it applies and then
    // continues via cont. Types without a dialog fall through to direct apply.
    if (s.prompt)
    {
        bool handled = false;
        switch (t.getCharPointer()[0])
        {
            case 'c':
                if (t == kChangeGain)       { showChangeGainDialog (cont);           handled = true; }
                else if (t == kCatTimeScale) { showContentAwareTimeScaleDialog (cont); handled = true; }
                break;
            case 't':
                if (t == kTimeStretch)      { showTimeStretchDialog (cont);          handled = true; }
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
                else if (t == kTranscribe)  { showTranscribeDialog (cont);           handled = true; }
#endif
                break;
            case 'f':
                if (t == kFilterEq)         { showFilterEqDialog (cont);            handled = true; }
                break;
            case 'e':
                if (t == kEnvelope)         { showEnvelopeDialog (cont);            handled = true; }
                break;
            case 'g':
                if (t == kGenSilence)       { showGenerateSilenceDialog (cont);     handled = true; }
                else if (t == kGenNoise)    { showGenerateNoiseDialog (cont);       handled = true; }
                else if (t == kGenTone)     { showGenerateToneDialog (cont);        handled = true; }
                break;
            case 'r':
                if (t == kResample)         { showResampleDialog (cont);            handled = true; }
                break;
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
            case 'n':
                if (t == kNoiseSuppress)    { showNoiseSuppressDialog (cont);       handled = true; }
                break;
#endif
            default:
                break;
        }
        if (handled) return;   // the dialog continues the replay via cont
    }

    // Long-running steps run asynchronously and continue via cont.
    if (t == kResample)
    {
        const auto v = s.params.getProperty ("rate", juce::var());
        const double rate = sc::isNumericVar (v) ? (double) v : 0.0;
        if (!std::isfinite (rate) || rate <= 0.0)
        { stopReplay ("resample step has an invalid rate"); return; }

        std::vector<std::vector<double>> weights;
        const auto arr = s.params.getProperty ("weights", juce::var());
        if (arr.isArray())
            for (int i = 0; i < arr.size (); ++i)
            {
                const auto rowVar = arr[(size_t) i];
                std::vector<double> row;
                if (rowVar.isArray())
                    for (int j = 0; j < rowVar.size (); ++j)
                        row.push_back ((double) rowVar[(size_t) j]);
                weights.push_back (std::move (row));
            }
        runResampleAsync (rate, weights, cont);
        return;
    }

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    if (t == kTranscribe)
    {
        const auto m = s.params.getProperty ("modelId", juce::var());
        const auto l = s.params.getProperty ("language", juce::var());
        runTranscriptionAsync (m.toString().toStdString (),
                               l.toString().toStdString (),
                               (bool) s.params.getProperty ("translate", juce::var()),
                               (bool) s.params.getProperty ("fast", juce::var()),
                               true, cont);
        return;
    }
#endif

    if (t == kPython)
    {
        const auto pathVar = s.params.getProperty ("script", juce::var());
        const juce::File script (pathVar.toString());
        if (!script.existsAsFile())
        { stopReplay ("python script not found: " + script.getFileName()); return; }
        runPythonScriptAsync (script, script.loadFileAsString (), cont);
        return;
    }

    // Everything else is a synchronous engine edit.
    // s is captured by value: on a streamed file the edit runs on a worker
    // thread after this function has returned.
    runDestructiveEdit ([this, s] (std::string* e) { return sc::applyActionStep (app, s, e); },
                       cont);
}

void MainWindow::showActionBatchWindow (int index)
{
    auto* w = new ActionBatchWindow (app, index);
    w->onCloseRequested = [w] { juce::MessageManager::callAsync ([w] { delete w; }); };
    w->centreWithSize (620, 480);
    w->setVisible (true);
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
        [this, dlg, file] (bool ok) {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (!ok && !wasCancelled) showOpenFailed (file);   // a cancel is not an error
        },
        &dlg->cancelled);
}

// ------------------------------------------------- Editor tabs (one per document)

void MainWindow::activateDocById (int docId)
{
    for (int i = 0; i < (int) app.docs.size (); ++i)
        if (app.docs[(size_t) i]->id == docId)
        {
            if (i != app.activeDocIndex)
                app.activate (i);   // -> activateDocUI -> syncEditorTabs
            return;
        }
}

void MainWindow::refreshEditorTabTitles()
{
    if (dock_ == nullptr)
        return;
    bool editorHasDoc = false;
    for (const auto& d : app.docs)
    {
        const auto it = editorTabOfDoc_.find (d->id);
        if (it == editorTabOfDoc_.end ())
            continue;
        const juce::String title = editorTabTitle (*d);
        dock_->setPanelTitle (it->second, title);
        if (const auto sit = editorSlots_.find (it->second); sit != editorSlots_.end ())
            if (auto* slot = dynamic_cast<EditorSlot*> (sit->second.get ()))
                if (slot->fileName != title)
                {
                    slot->fileName = title;
                    slot->updateTooltip ();
                    slot->repaint ();
                }
        editorHasDoc = editorHasDoc || it->second == "editor";
    }
    if (! editorHasDoc)
        dock_->setPanelTitle ("editor", "Editor");
}

void MainWindow::snapshotEditorIntoItsTab()
{
    // Called by Application::activate() before it switches documents: the
    // views still show the document being left, in its tab. That tab shows
    // this picture whenever it is visible without the views (issue #9: two
    // files side by side in two groups).
    if (editorPanel_ == nullptr)
        return;
    auto* slot = dynamic_cast<EditorSlot*> (editorPanel_->getParentComponent ());
    if (slot == nullptr || editorPanel_->getWidth () <= 0 || editorPanel_->getHeight () <= 0)
        return;
    const float scale = juce::jlimit (1.0f, 3.0f,
        (float) juce::Component::getApproximateScaleFactorForComponent (editorPanel_.get ()));
    slot->setSnapshot (editorPanel_->createComponentSnapshot (editorPanel_->getLocalBounds (),
                                                              true, scale));
}

void MainWindow::applyTooltipsSetting()
{
    // Help > Show Tooltips (issue #12): without the TooltipWindow no tip
    // shows anywhere (floating panel windows and dialogs included); with it,
    // every control's own tooltip text appears after the usual hover delay.
    if (app.settings.showTooltips && tooltipWindow == nullptr)
        tooltipWindow = std::make_unique<juce::TooltipWindow> (nullptr, 600);
    else if (! app.settings.showTooltips)
        tooltipWindow.reset();
}

bool MainWindow::editorTabHasPicture (const juce::String& panelId) const
{
    const auto it = editorSlots_.find (panelId);
    if (it == editorSlots_.end ())
        return false;
    const auto* slot = dynamic_cast<const EditorSlot*> (it->second.get ());
    return slot != nullptr && slot->snapshot.isValid ();
}

void MainWindow::syncEditorTabs()
{
    if (dock_ == nullptr || editorPanel_ == nullptr)
        return;

    // Tabs of closed documents go (the views leave with their slot, if
    // there, and come back below).
    for (auto it = editorTabOfDoc_.begin (); it != editorTabOfDoc_.end ();)
    {
        const int docId = it->first;
        const bool open = std::any_of (app.docs.begin (), app.docs.end (),
                                       [docId] (const auto& d) { return d->id == docId; });
        if (open) { ++it; continue; }
        if (it->second != "editor")
        {
            dock_->unregisterPanel (it->second);
            editorSlots_.erase (it->second);
        }
        it = editorTabOfDoc_.erase (it);
    }

    // The permanent Editor tab always holds a document while any is open:
    // the active one takes it over (its own extra tab goes).
    const FileDoc* active = app.doc ();
    const bool editorTaken = std::any_of (editorTabOfDoc_.begin (), editorTabOfDoc_.end (),
                                          [] (const auto& kv) { return kv.second == "editor"; });
    if (! editorTaken && active != nullptr)
    {
        if (const auto own = editorTabOfDoc_.find (active->id); own != editorTabOfDoc_.end ())
        {
            dock_->unregisterPanel (own->second);
            editorSlots_.erase (own->second);
        }
        editorTabOfDoc_[active->id] = "editor";
    }

    // New documents: a tab of their own in the Editor's group, after the
    // ones already there (a maximised Editor group stays maximised).
    for (const auto& d : app.docs)
    {
        if (editorTabOfDoc_.count (d->id) != 0)
            continue;
        const juce::String pid = "editor-" + juce::String (d->id);
        auto slot = std::make_unique<EditorSlot> ();
        const int docId = d->id;
        slot->onActivate = [this, docId] { activateDocById (docId); };
        dock_->registerPanel ({ pid, editorTabTitle (*d), slot.get (), false, 900, 600 });
        dock_->setDefaultBuddy (pid, "editor");
        editorSlots_[pid] = std::move (slot);
        editorTabOfDoc_[docId] = pid;
        dock_->openPanelAsTabOf (pid, "editor");
    }
    refreshEditorTabTitles ();

    // The views go to the active document's tab, which becomes the visible
    // tab of its group.
    juce::String target ("editor");
    if (active != nullptr)
        if (const auto t = editorTabOfDoc_.find (active->id); t != editorTabOfDoc_.end ())
            target = t->second;
    const auto targetSlot = editorSlots_.find (target);
    if (auto* slot = targetSlot != editorSlots_.end () ? targetSlot->second.get () : nullptr)
    {
        if (editorPanel_->getParentComponent () != slot)
        {
            slot->addAndMakeVisible (editorPanel_.get ());
            editorPanel_->setBounds (slot->getLocalBounds ());
        }
        const auto loc = dock_->locate (target);
        if (loc.area != nullptr && loc.group->active != loc.index)
            dock_->setActiveTab (loc.group->uid, loc.index);
    }
    // Other slots now show their placeholder text.
    for (auto& [pid, slot] : editorSlots_)
        if (pid != target && slot != nullptr)
            slot->repaint ();
}

void MainWindow::activateDocUI()
{
    // Restore this doc's remembered time window (Application::activate() put
    // it into app.viewStart/viewEnd; whole file for a fresh or never-zoomed
    // doc), reset the amplitude and frequency axes to their defaults, then
    // push the document's playhead/selection/channels/markers into all panels
    // and views. Works for a freshly opened file, a switched-to file (restoring
    // its remembered state) and the "no files left" case.
    if (waveformView)
    {
        waveformView->resetVerticalZoom();
        waveformView->setViewRange (app.viewStart, app.viewEnd);
    }
    if (spectralView)
    {
        spectralView->setViewRange (app.viewStart, app.viewEnd);
        spectralView->resetFrequencyRange();
        // setViewRange() only repaints when the time window actually changed -
        // two files of identical duration would leave the previous file's tiles
        // on screen. Application::activate() already cleared the tile cache, so
        // force a re-render from the newly active engine.
        spectralView->invalidateImage();
    }

    // Re-install the playback mask for this doc's spectral selection (or clear
    // it when the new doc has none / Play Spectral Selection is off).
    app.pushSpectralMaskToEngine ();

    // Per-file Frequency Analysis settings: if this document carries its own
    // saved values, mirror them into the panel's combos (no-op otherwise).
    if (fftPanel) fftPanel->resyncFromSettings();

    // A file whose embedded tables are parsed during load never fires
    // onTablesChanged, so push the shared time window and value fit into the
    // dedicated strip here (covers fresh loads and doc switches alike).
    if (tablesTrack && !app.activeTables().empty())
    {
        tablesTrack->refit();
        tablesTrack->setTimeRange (app.viewStart, app.viewEnd);
    }

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

    // The Video window follows the active document (or says it has none),
    // and solo playback is recomputed for its tracks (cleared when the new
    // doc has no soloed video track).
    syncVideoWindow();
    refreshVideoSolo ();

    // One Editor tab per open document; the views sit in the active one's.
    syncEditorTabs ();

    // Re-layout so the time scrollbar's visibility/bounds follow the new file
    // state: resized() is the only place they are set, and loading a file does
    // not otherwise resize the window (which left the bar hidden until the
    // user maximised/restored).
    resized();
}

// ------------------------------------------------- Video window

/** The document's video tracks (see the definition below). */
static std::vector<sc::VideoTrackData> effectiveVideoTracks (const FileDoc& d);

double MainWindow::videoTimeNow() const
{
    auto& eng = app.audioEngine();
    return eng.isPlaying () ? eng.position () : app.playhead;
}

void MainWindow::ensureVideoWindow()
{
    if (videoWindow != nullptr)
        return;

    videoWindow = std::make_unique<VideoWindow> ([this] { return videoTimeNow(); });
    const juce::String state = app.recentStore->getValue ("video_window_state");
    if (state.isNotEmpty())
        videoWindow->restoreWindowStateFromString (state);

    // The saved "Always on top" state applies immediately, not only when it
    // is toggled from a context menu.
    videoWindow->setAlwaysOnTop (app.recentStore->getIntValue ("video_always_on_top", 0) != 0);

    // The picture's Upscaling choice (context menu) is remembered between
    // sessions; Lanczos until the user picks another one.
    videoWindow->view().setUpscaling (
        sc::videoUpscalingFromName (app.recentStore->getValue ("video_upscaling", "Lanczos")));
    videoWindow->view().onUpscalingChanged = [this] (sc::VideoUpscaling m)
    {
        app.recentStore->setValue ("video_upscaling", sc::videoUpscalingName (m));
        app.recentStore->save();
    };

    videoWindow->onUserClosed = [this]
    {
        videoDismissedFor = videoWindow->getVideoFile();
        app.recentStore->setValue ("video_window_state", videoWindow->getWindowStateAsString());
        app.recentStore->save();
    };
    // Transport keys (space, arrows, Home/End...) typed while the Video
    // window has the focus act on the main window.
    videoWindow->onKeyPressed = [this] (const juce::KeyPress& k) { return keyPressed (k, this); };
    videoWindow->onVideoOpened = [this] (bool ok, bool noTrack)
    {
        // Show a new picture automatically, and say why there is none when
        // the file has a video track that cannot be decoded. A file the user
        // closed the window on stays closed until Window > Video reopens it.
        if (videoWindow == nullptr || (! ok && noTrack))
            return;
        if (videoWindow->getVideoFile() == videoDismissedFor || videoWindow->isVisible()
            || videoWindow->isDocked())
            return;   // docked: the panel already shows it
        videoWindow->setVisible (true);
        toFront (false);   // keep the keyboard on the spectrogram
    };

    // The track list's ruler seeks the main playhead; dragging on it selects a
    // time range in the document - the same click-vs-drag split the data
    // tracks use (docs/Features.md).
    auto& vtp = videoWindow->getTracksPanel();
    vtp.onSeekTime = [this] (double t) { app.setPlayhead (t); };
    vtp.onSelectTimeRange = [this] (const juce::Range<double>& r)
    {
        app.setSelection (r);   // clamped to the file; every view shows it
    };
    vtp.setSelection (app.selection);   // a freshly shown panel carries the current selection
    videoWindow->onTrackMenuAction = [this] (int id, int idx)
    {
        handleVideoTrackMenu (id, idx);
    };

    // Multi-track: per-track state is persisted into the document's metadata.
    // The first change materialises the implicit single track as an explicit list.
    videoWindow->onTrackAdjustmentsSaved = [this] (const sc::VideoTrackData& td)
    {
        FileDoc* d = app.doc();
        if (d == nullptr || videoWindow == nullptr) return;
        const int i = videoWindow->displayedTrack();
        auto t = effectiveVideoTracks (*d);
        if (i < 0 || i >= (int) t.size()) return;
        const auto& cur = t[i];
        if (cur.zoom == td.zoom && cur.panX == td.panX && cur.panY == td.panY
            && cur.rotation == td.rotation && cur.solo == td.solo
            && std::abs (cur.contrast - td.contrast) < 1e-9
            && std::abs (cur.brightness - td.brightness) < 1e-9)
            return;   // nothing changed: do not mark the document dirty
        t[i] = td;
        d->setVideoTracks (std::move (t));
    };
    videoWindow->onTrackOffsetChanged = [this] (int i, double off)
    {
        FileDoc* d = app.doc();
        if (d == nullptr) return;
        auto t = effectiveVideoTracks (*d);
        if (i < 0 || i >= (int) t.size()) return;
        if (std::abs (t[i].offsetSecs - off) < 1e-9) return;
        t[i].offsetSecs = off;
        d->setVideoTracks (std::move (t));
    };
    // A band dragged up/down: the document's track list takes the new order
    // (saved with its metadata; the top track wins where tracks overlap).
    videoWindow->onTrackMoved = [this] (int from, int to)
    {
        FileDoc* d = app.doc();
        if (d == nullptr) return;
        auto t = effectiveVideoTracks (*d);
        const int n = (int) t.size();
        if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
        moveVideoTrack (t, from, to);
        int sel = videoTrackIndexAfterMove (d->selectedVideoTrack, from, to);
        d->setVideoTracks (std::move (t), &sel);
        refreshVideoSolo ();   // a soloed track keeps playing under its new index
    };
    videoWindow->onSelectTrackChanged = [this] (int i)
    {
        FileDoc* d = app.doc();
        if (d == nullptr || d->selectedVideoTrack == i) return;
        d->selectedVideoTrack = i;   // transient UI state, not metadata
    };
    // Dropping a video file on the window adds it as a new track of the active
    // document (the spec's "drag/drop into the panel"); with no open document
    // it is opened as one instead, like a drop on the main window.
    videoWindow->onVideoDropped = [this] (const juce::File& f) { addVideoTrackFromFile (f); };
    // The docked picture/track list accepts the same drops: JUCE resolves drag
    // targets from the deepest component, so while docked this host wins over
    // MainWindow's "open as document" handler for drops on the video area.
    videoDockHost->onVideoDropped = [this] (const juce::File& f) { addVideoTrackFromFile (f); };

    // The saved layout may have the Video tab open: restore the docked state.
    if (isPanelOpen ("video"))
        videoWindow->dockTo (*videoDockHost);
}

void MainWindow::addVideoTrackFromFile (const juce::File& f)
{
    FileDoc* d = app.doc();
    if (d == nullptr || !f.existsAsFile())
    {   app.openFile (f); return; }
    auto t = effectiveVideoTracks (*d);
    sc::VideoTrackData nt;
    nt.file = f;
    t.push_back (std::move (nt));
    d->selectedVideoTrack = (int) t.size() - 1;   // the new track renders above
    d->setVideoTracks (std::move (t));
    syncVideoWindow();
}

/** The document's video tracks: the explicit list if it has one, otherwise
    a single implicit track at offset 0 for the auto-detected `videoFile`. */
static std::vector<sc::VideoTrackData> effectiveVideoTracks (const FileDoc& d)
{
    if (!d.videoTracks.empty()) return d.videoTracks;
    if (d.videoFile != juce::File())
    {
        sc::VideoTrackData t;
        t.file = d.videoFile;
        std::vector<sc::VideoTrackData> v;
        v.push_back (std::move (t));
        return v;
    }
    return {};
}

// ------------------------------------------------- Video track menu actions

/** Parses a decimal field (same rules as the tables' manual-align dialog). */
static bool videoParseDouble (const juce::String& s, double& out)
{
    const auto t = s.trim();
    if (t.isEmpty()) return false;
    bool haveDigit = false;
    for (char16_t c : t)
    {
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')
        {   haveDigit = haveDigit || (c >= '0' && c <= '9'); continue; }
        break;
    }
    if (!haveDigit) return false;
    const double v = t.getDoubleValue();
    if (!std::isfinite ((float) v)) return false;
    out = v;
    return true;
}

/** Reads one channel of a reader as float, in chunks. Only slot `ch` gets a
    destination; the others stay null so the reader skips them. */
static bool readChannelFloat (juce::AudioFormatReader& r, int ch, int64_t len,
                              std::vector<float>& out)
{
    out.resize ((size_t) len);
    float* stackPtrs[64] = {};
    std::vector<float*> heapPtrs;
    const int chunk = 1 << 20;   // ~4 MB per pass
    for (int64_t off = 0; off < len; off += chunk)
    {
        const int n = (int) std::min<int64_t> ((int64_t) chunk, len - off);
        float** ptrs = stackPtrs;
        if (ch >= 64)
        {
            heapPtrs.assign ((size_t) ch + 1, nullptr);
            ptrs = heapPtrs.data ();
        }
        ptrs[ch] = out.data () + off;
        if (!r.read (ptrs, ch + 1, off, n)) return false;
    }
    return true;
}

/** Streams one channel of a reader down to ~2 kHz (box decimation), so even
    multi-hour files occupy only a few MB. */
static bool readChannelDecimated (juce::AudioFormatReader& r, int ch, double inRate,
                                  std::vector<float>& out)
{
    const int64_t len = r.lengthInSamples;
    if (inRate <= 0.0 || len <= 0) return false;
    float* stackPtrs[64] = {};
    std::vector<float*> heapPtrs;
    const int chunk = 1 << 22;   // ~16 MB per pass at float32
    for (int64_t off = 0; off < len; off += chunk)
    {
        const int n = (int) std::min<int64_t> ((int64_t) chunk, len - off);
        std::vector<float> part ((size_t) n);
        float** ptrs = stackPtrs;
        if (ch >= 64)
        {
            heapPtrs.assign ((size_t) ch + 1, nullptr);
            ptrs = heapPtrs.data ();
        }
        ptrs[ch] = part.data ();
        if (!r.read (ptrs, ch + 1, off, n)) return false;
        const auto ds = sc::decimateTo (part, inRate, 2000.0);
        out.insert (out.end (), ds.begin (), ds.end ());
    }
    return true;
}

void MainWindow::handleVideoTrackMenu (int id, int idx)
{
    if (app.doc () == nullptr || videoWindow == nullptr) return;
    const auto tracks = effectiveVideoTracks (*app.doc());
    if (idx < 0 || idx >= (int) tracks.size ()) return;

    switch (id)
    {
        case VideoTracksPanel::kMenuAlign:       showVideoAlignDialog (idx); break;
        case VideoTracksPanel::kMenuAdjustImage: showVideoAdjustDialog (idx); break;
        case VideoTracksPanel::kMenuSyncAudio:   startVideoAutoSync (idx); break;
        case VideoTracksPanel::kMenuCopyAudio:   copyVideoAudioToChannels (idx); break;
        case VideoTracksPanel::kMenuAddTrack:    addVideoTrackFromPicker (); break;
        case VideoTracksPanel::kMenuRemoveTrack: removeVideoTrack (idx); break;
        case VideoTracksPanel::kMenuSolo:   toggleVideoSolo (idx); break;
        default: break;   // window-level items are handled by the panel itself
    }
}

void MainWindow::showVideoAlignDialog (int idx)
{
    FileDoc* d = app.doc();
    if (d == nullptr || videoWindow == nullptr) return;
    auto tracks = effectiveVideoTracks (*d);
    if (idx < 0 || idx >= (int) tracks.size ()) return;

    auto* w = new ManualAlignWindow ("Align Video Track", "Manual Alignment:");
    w->addRow ("Time offset (s)",
               "Document time where this video's first frame sits (+ = later, - = the video starts before the audio).",
               juce::String (tracks[idx].offsetSecs, 4));

    // Size the window from its rows: message area + row pitch per field + buttons.
    const int contentH = 26 + w->numRows () * 27 + 10 + 26;
    w->setSize (356, contentH);
    w->setResizable (false, false);

    // Centre on the main window's current position instead of appearing in
    // the screen corner.
    if (juce::Component* top = getTopLevelComponent ())
    {
        const auto tb = top->getBounds ();
        w->setBounds (tb.getCentreX () - 356 / 2, tb.getCentreY () - contentH / 2,
                      356, contentH);
    }
    else
        w->centreWithSize (356, contentH);

    bool okPressed = false;
    w->onOk = [&okPressed] { okPressed = true; };
    w->enterModalState (false);
    // Child order: message, OK, Cancel, then label/editor pairs per row -
    // focus the first editor so a plain Enter accepts the defaults.
    if (w->getNumChildComponents () > 4)
    {
        auto* first = dynamic_cast<juce::TextEditor*> (w->getChildComponent (4));
        if (first != nullptr) first->grabKeyboardFocus();
    }
    const int result = w->runModalLoop ();
    juce::String offText;
    if (result == 1 && okPressed)
        offText = w->rowText (0);
    delete w;

    bool changed = false;
    if (!offText.isEmpty())
    {
        double off = tracks[idx].offsetSecs;
        if (videoParseDouble (offText, off))
        {
            tracks[idx].offsetSecs = off;
            changed = true;
        }
    }
    if (changed)
    {
        d->setVideoTracks (std::move (tracks));
        syncVideoWindow();
        refreshVideoSolo ();   // the offset moved: solo audio must follow it
    }
}

void MainWindow::showVideoAdjustDialog (int idx)
{
    FileDoc* d = app.doc();
    if (d == nullptr || videoWindow == nullptr) return;
    auto tracks = effectiveVideoTracks (*d);
    if (idx < 0 || idx >= (int) tracks.size ()) return;

    auto* dlg = new VideoAdjustDialog (this);
    dlg->setValues (tracks[idx].contrast, tracks[idx].brightness);
    dlg->onValuesChanged = [this] (double c, double b)
    {
        if (videoWindow != nullptr) videoWindow->setAdjustmentsLive (c, b);
    };
    dlg->onApplyToAll = [this]
    {
        FileDoc* dd = app.doc();
        if (dd == nullptr || videoWindow == nullptr) return;
        auto t = effectiveVideoTracks (*dd);
        const double c = videoWindow->getContrast ();
        const double b = videoWindow->getBrightness ();
        for (auto& q : t)
        {
            q.contrast = c;
            q.brightness = b;
        }
        dd->setVideoTracks (std::move (t));
    };
    dlg->onReset = [this]
    {
        if (videoWindow != nullptr) videoWindow->setAdjustmentsLive (1.0, 0.0);
    };
    dlg->onClose = [this, dlg]
    {
        // Persist the displayed track's adjustments before deleting the dialog.
        FileDoc* dd = app.doc();
        if (dd != nullptr && videoWindow != nullptr)
        {
            const int i = videoWindow->displayedTrack ();
            auto t = effectiveVideoTracks (*dd);
            if (i >= 0 && i < (int) t.size ())
            {
                t[i].contrast = videoWindow->getContrast ();
                t[i].brightness = videoWindow->getBrightness ();
                dd->setVideoTracks (std::move (t));
            }
        }
        delete dlg;
    };
    dlg->setVisible (true);
}

void MainWindow::startVideoAutoSync (int idx)
{
    FileDoc* d = app.doc();
    if (d == nullptr || videoWindow == nullptr) return;
    auto tracks = effectiveVideoTracks (*d);
    if (idx < 0 || idx >= (int) tracks.size ()) return;

    const juce::File vf = tracks[idx].file;
    auto& eng = app.audioEngine();
    if (!eng.isFileLoaded () || !vf.existsAsFile())
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon, "Sync based on Audio",
                                   "The document needs a loaded audio file and the video must exist.",
                                   "", this);
        return;
    }

    auto* dlg = new WorkProgressDialog ("Syncing video to audio",
                                        "Cross-correlating " + vf.getFileName() + " ...",
                                        true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, idx, vf, dlg] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v)
        {
            if (v != 1.0 && v - lastSent < 0.02) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };

        double bestOffset = std::nan ("");
        std::string err;
        {
            auto reader = app.audioEngine().createReaderFor (vf);
            if (reader == nullptr)
                err = "The video has no decodable audio track.";
            else if (reader->numChannels < 1 || reader->lengthInSamples <= 0)
                err = "The video's audio stream is empty.";
            else
            {
                // Coarse stage: both first channels box-decimated to ~2 kHz.
                std::vector<float> docDs, vidDs;
                if (!readChannelDecimated (*reader, 0, reader->sampleRate, vidDs))
                    err = "Could not read the video's audio.";
                else
                {
                    const int64_t dLen = app.audioEngine().numSamples();
                    const double dRate = app.audioEngine().sampleRate();
                    // Stream the document channel in chunks and decimate it.
                    const int chunk = 1 << 22;
                    for (int64_t off = 0; off < dLen; off += chunk)
                    {
                        const int n = (int) std::min<int64_t> ((int64_t) chunk, dLen - off);
                        std::vector<float> part ((size_t) n);
                        if (!app.audioEngine().readSamples (0, off, n, part.data()))
                        {
                            err = "Could not read the document's audio.";
                            break;
                        }
                        const auto ds = sc::decimateTo (part, dRate, 2000.0);
                        docDs.insert (docDs.end (), ds.begin (), ds.end ());
                    }
                }

                if (err.empty())
                {
                    double step = 0.0;
                    const double coarseLag = sc::videoSyncCoarse (docDs, vidDs, &step);
                    sendProgress (0.5);
                    if (!std::isnan (coarseLag) && step > 0.0)
                    {
                        // Fine stage: full-rate windows around the best lag.
                        const double dRate = app.audioEngine().sampleRate();
                        const int64_t dLen = app.audioEngine().numSamples();
                        const double vDur = (double) reader->lengthInSamples / reader->sampleRate;
                        const double w2 = std::min (5.0, std::max (1.0, std::min ((double) dLen / dRate, vDur) * 0.9));

                        const double sA = std::max (0.0, coarseLag - step);
                        const double eB = std::min ((double) dLen / dRate, coarseLag + w2 + step);
                        const int64_t nDoc = (int64_t) std::llround ((eB - sA) * dRate);
                        std::vector<float> docWin;
                        docWin.resize ((size_t) nDoc, 0.0f);
                        if (!app.audioEngine().readSamples (0,
                                (int64_t) std::llround (sA * dRate), nDoc, docWin.data()))
                            err = "Could not read the document's audio.";

                        std::vector<float> vidWin;
                        if (err.empty())
                        {
                            const double vEnd = std::min (vDur, w2 + step);
                            if (!readChannelFloat (*reader, 0,
                                    (int64_t) std::llround (vEnd * reader->sampleRate), vidWin))
                                err = "Could not read the video's audio.";
                        }

                        if (err.empty())
                        {
                            const double fineLag = sc::videoSyncFine (docWin, dRate, sA,
                                                                     vidWin, reader->sampleRate,
                                                                     coarseLag, step);
                            bestOffset = std::isnan (fineLag) ? coarseLag : fineLag;
                        }
                    }
                    else if (std::isnan (coarseLag))
                        err = "No correlation found: the video's audio does not match the document.";
                }
            }
        }

        const bool cancelled = dlg->cancelled.load ();
        juce::MessageManager::callAsync ([this, idx, dlg, bestOffset, err, cancelled] {
            delete dlg;
            if (cancelled || app.doc () == nullptr)
                return;
            auto tracks = effectiveVideoTracks (*app.doc());
            // The track may have been removed while the job ran.
            if (idx >= (int) tracks.size ())
            {
                if (!err.empty())
                    sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                               "Sync based on Audio", err, "", this);
                return;
            }
            if (!std::isnan (bestOffset))
            {
                tracks[(size_t) idx].offsetSecs = bestOffset;
                app.doc()->setVideoTracks (std::move (tracks));
                syncVideoWindow();
                refreshVideoSolo ();
            }
            else if (!err.empty())
                sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                           "Sync based on Audio", err, "", this);
        });
    }).detach ();
}

void MainWindow::copyVideoAudioToChannels (int idx)
{
    FileDoc* d = app.doc();
    if (d == nullptr || videoWindow == nullptr) return;
    auto tracks = effectiveVideoTracks (*d);
    if (idx < 0 || idx >= (int) tracks.size ()) return;

    const juce::File vf = tracks[idx].file;
    const double offsetSecs = tracks[idx].offsetSecs;
    auto& eng = app.audioEngine();
    if (!eng.isFileLoaded () || !vf.existsAsFile())
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon, "Copy Audio to New Channel(s)",
                                   "The document needs a loaded audio file and the video must exist.",
                                   "", this);
        return;
    }

    const int oldN = eng.numChannels ();   // only channels added by this job get shown

    auto* dlg = new WorkProgressDialog ("Copying video audio",
                                        "Reading " + vf.getFileName() + " ...",
                                        true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, oldN, vf, offsetSecs, dlg] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v)
        {
            if (v != 1.0 && v - lastSent < 0.02) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };

        std::vector<std::vector<float>> data;
        double outRate = 0.0;
        int64_t samplePos = 0;
        std::string err;
        {
            auto reader = app.audioEngine().createReaderFor (vf);
            if (reader == nullptr)
                err = "The video has no decodable audio track.";
            else if (reader->numChannels < 1 || reader->lengthInSamples <= 0)
                err = "The video's audio stream is empty.";
            else
            {
                const int nch = reader->numChannels;
                const int64_t len = reader->lengthInSamples;
                const double vRate = reader->sampleRate;
                // Cap the decode so a huge movie cannot exhaust memory
                // (~256 MB ≈ 90 min of stereo @ 48 kHz).
                if ((size_t) nch * (size_t) len * sizeof (float) > (size_t) 256 * 1024 * 1024)
                    err = (juce::String ("The video's audio is too long to copy (~")
                           + juce::String (((double) nch * (double) len * 4.0) / 1048576.0, 0)
                           + " MB needed).").toStdString ();
                else
                {
                    juce::AudioBuffer<float> buf (nch, (int) std::min<int64_t> (len, (int64_t) INT_MAX));
                    const int chunk = 1 << 20;   // frames per pass
                    for (int64_t off = 0; err.empty () && off < len; off += chunk)
                    {
                        const int n = (int) std::min<int64_t> ((int64_t) chunk, len - off);
                        float* stackDest[64] = {};
                        std::vector<float*> heapDest;
                        if (nch > 64)
                            heapDest.assign ((size_t) nch, nullptr);
                        for (int ch = 0; ch < nch; ++ch)
                            (nch <= 64 ? stackDest[ch] : heapDest[(size_t) ch])
                                = buf.getWritePointer (ch, (int) off);
                        float** destPtr = nullptr;
                        if (nch <= 64) destPtr = stackDest;
                        else if (!heapDest.empty ()) destPtr = &heapDest[0];
                        if (destPtr == nullptr || !reader->read (destPtr, nch, off, n))
                            err = "Could not read the video's audio.";
                        sendProgress (0.5 * (double) (off + n) / (double) len);
                    }

                    if (err.empty())
                    {
                        const double dRate = app.audioEngine().sampleRate();
                        outRate = dRate;
                        if (std::abs (vRate - dRate) > 1e-9)
                        {
                            juce::AudioBuffer<float> rs;
                            std::atomic<bool> cancelFlag { false };
                            auto progress = [&cancelFlag, dlg, &sendProgress] (float v)
                            {
                                if (dlg->cancelled.load ())
                                    cancelFlag.store (true);
                                else
                                    sendProgress (0.5 + 0.4 * (double) v);
                            };
                            if (!sc::Resampler::resample (buf, vRate, dRate, rs, progress,
                                                         &cancelFlag))
                                err = "Could not resample the video's audio.";
                            else
                            {
                                data.resize ((size_t) rs.getNumChannels());
                                for (int ch = 0; ch < rs.getNumChannels(); ++ch)
                                    data[(size_t) ch].assign (rs.getReadPointer (ch),
                                                              rs.getReadPointer (ch) + rs.getNumSamples());
                            }
                        }
                        else
                        {
                            data.resize ((size_t) nch);
                            for (int ch = 0; ch < nch; ++ch)
                                data[(size_t) ch].assign (buf.getReadPointer (ch),
                                                          buf.getReadPointer (ch) + len);
                        }
                    }
                }

                if (err.empty())
                {
                    samplePos = (int64_t) std::llround (offsetSecs * outRate);
                    if (samplePos < 0)
                        err = "The track starts before the document; align it first.";
                    else if (!app.audioEngine().writeDataToNewChannels (
                                 samplePos, data,
                                 "Video audio: " + vf.getFileName(), &err))
                        ;   // writeDataToNewChannels filled in err
                }
            }
        }

        const bool cancelled = dlg->cancelled.load ();
        juce::MessageManager::callAsync ([this, oldN, dlg, cancelled] {
            delete dlg;
            if (cancelled || app.doc () == nullptr)
                return;
            // Make the freshly added channels visible.
            auto& eng2 = app.audioEngine();
            const int n = eng2.numChannels ();
            for (int ch = oldN; ch < n; ++ch)
                if (!app.isChannelVisible (ch))
                    app.visibleChannels.push_back (ch);
            std::sort (app.visibleChannels.begin (), app.visibleChannels.end ());
            refreshAfterEdit ();
        });
    }).detach ();
}

void MainWindow::addVideoTrackFromPicker ()
{
    FileDoc* d = app.doc();
    if (d == nullptr || videoWindow == nullptr) return;

    const juce::String exts = sc::videoContainerExtensions ();
    juce::FileChooser fc ("Add a video track...",
                          juce::File(),
                          exts.isEmpty () ? "mp4|mov|m4v|3gp|avi|mkv|webm" : exts);
    if (!fc.browseForFileToOpen ())
        return;

    const juce::File f = fc.getResult ();
    auto t = effectiveVideoTracks (*d);
    sc::VideoTrackData nt;
    nt.file = f;
    t.push_back (std::move (nt));
    d->selectedVideoTrack = (int) t.size () - 1;   // the new track renders above
    d->setVideoTracks (std::move (t));
    syncVideoWindow();
}

void MainWindow::removeVideoTrack (int idx)
{
    FileDoc* d = app.doc();
    if (d == nullptr || videoWindow == nullptr) return;
    auto tracks = effectiveVideoTracks (*d);
    if (idx < 0 || idx >= (int) tracks.size ()) return;

    int sel = d->selectedVideoTrack;
    tracks.erase (tracks.begin () + idx);
    if (sel == idx)
        sel = std::min (idx, (int) tracks.size () - 1);   // fall back to the next track (or -1)
    else if (sel > idx)
        --sel;

    d->videoFile = juce::File();   // do not resurrect an implicit track from it
    d->setVideoTracks (std::move (tracks), &sel);
    syncVideoWindow();
    refreshVideoSolo ();
}

void MainWindow::toggleVideoSolo (int idx)
{
    FileDoc* d = app.doc();
    if (d == nullptr || videoWindow == nullptr) return;
    auto tracks = effectiveVideoTracks (*d);
    if (idx < 0 || idx >= (int) tracks.size ()) return;

    tracks[idx].solo = !tracks[idx].solo;
    d->setVideoTracks (std::move (tracks));
    syncVideoWindow();          // refresh the menu checkmark + panel state
    refreshVideoSolo ();        // start/stop solo playback accordingly
}

void MainWindow::refreshVideoSolo ()
{
    FileDoc* d = app.doc();
    auto& eng = app.audioEngine();

    std::vector<sc::VideoTrackData> tracks;
    int soloIdx = -1;
    if (d != nullptr)
    {
        tracks = effectiveVideoTracks (*d);
        for (int i = 0; i < (int) tracks.size (); ++i)
            if (tracks[(size_t) i].solo)
                soloIdx = (d->selectedVideoTrack >= 0 && d->selectedVideoTrack < (int) tracks.size ()
                           && tracks[(size_t) d->selectedVideoTrack].solo)
                             ? d->selectedVideoTrack : i;
    }

    if (soloIdx < 0 || !eng.isFileLoaded ())
    {
        videoSoloCache.reset ();
        videoSoloCacheFor = juce::File();
        eng.setSoloAudio (nullptr);
        return;
    }

    const sc::VideoTrackData& tr = tracks[(size_t) soloIdx];

    if (videoSoloCache == nullptr || videoSoloCacheFor != tr.file)
    {
        // Decode the track's own audio at its native rate. Capped so a huge
        // movie cannot exhaust memory (~128 MB = 45 min of stereo @ 48 kHz).
        const size_t capBytes = (size_t) 128 * 1024 * 1024;
        std::shared_ptr<AudioEngine::SoloAudio> fresh;
        try
        {
            auto reader = eng.createReaderFor (tr.file);
            if (reader == nullptr || reader->numChannels < 1 || reader->lengthInSamples <= 0)
                throw juce::String ("The video has no decodable audio track.");
            const int64_t len = reader->lengthInSamples;
            const size_t need = (size_t) reader->numChannels * (size_t) len * sizeof (float);
            if (need > capBytes)
                throw juce::String ("The video's audio is too long to solo (~"
                                    + juce::String ((double) need / 1048576.0, 0) + " MB needed).");
            fresh = std::make_shared<AudioEngine::SoloAudio>();
            fresh->rate = reader->sampleRate;
            fresh->data.resize ((size_t) reader->numChannels);
            for (int ch = 0; ch < reader->numChannels; ++ch)
                if (!readChannelFloat (*reader, ch, len, fresh->data[(size_t) ch]))
                    throw juce::String ("Could not read the video's audio.");
        }
        catch (const std::bad_alloc&)
        {
            sc::alert::showMessageBox (juce::AlertWindow::WarningIcon, "Solo",
                                       "Not enough memory to decode this video's audio.", "", this);
            return;
        }
        catch (const juce::String& msg)
        {
            sc::alert::showMessageBox (juce::AlertWindow::WarningIcon, "Solo", msg, "", this);
            return;
        }

        videoSoloCache = std::move (fresh);
        videoSoloCacheFor = tr.file;
    }

    videoSoloCache->offsetSecs = tr.offsetSecs;   // the engine maps it onto the doc clock
    eng.setSoloAudio (videoSoloCache);
}


void MainWindow::syncVideoWindow()
{
    const FileDoc* d = app.doc();
    const juce::File vf = d != nullptr ? d->videoFile : juce::File();
    const bool hasTracks = d != nullptr && !d->videoTracks.empty();

    // Created the first time a document with video is active (or when the
    // user asks for it): audio-only sessions never pay for the window.
    if (videoWindow == nullptr && vf == juce::File() && !hasTracks)
        return;
    ensureVideoWindow();

    std::vector<sc::VideoTrackData> t = d != nullptr ? effectiveVideoTracks (*d) : std::vector<sc::VideoTrackData>();
    videoWindow->setTracks (t, d != nullptr ? d->selectedVideoTrack : -1);
    videoWindow->setAudioDuration (app.audioEngine().isFileLoaded() ? app.audioEngine().duration() : 0.0);
}

bool MainWindow::isVideoWindowVisible() const
{
    return videoWindow != nullptr && videoWindow->isVisible();
}

void MainWindow::setVideoWindowVisible (bool visible)
{
    if (! visible)
    {
        if (isVideoWindowVisible())
        {
            app.recentStore->setValue ("video_window_state", videoWindow->getWindowStateAsString());
            app.recentStore->save();
            videoWindow->setVisible (false);
        }
        return;
    }
    ensureVideoWindow();   // even without a video: it then says there is none
    syncVideoWindow();
    videoDismissedFor = juce::File();
    videoWindow->setVisible (true);
    videoWindow->toFront (true);
}

bool MainWindow::isVideoDocked() const
{
    return videoWindow != nullptr && videoWindow->isDocked ();
}

void MainWindow::setVideoDocked (bool docked)
{
    ensureVideoWindow ();
    if (docked)
    {
        setPanelVisible ("video", true);          // open the tab first, then move the view in
        videoWindow->dockTo (*videoDockHost);
    }
    else
    {
        videoWindow->undock ();                   // back into its own window
        setPanelVisible ("video", false);         // close the tab (no-op if already closed)
        videoDismissedFor = juce::File();
        videoWindow->setVisible (true);
        videoWindow->toFront (false);
    }
}

bool MainWindow::statusBarShowsDiagLog() const
{
    return statusBar != nullptr && statusBar->diagActive ();
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
    // Non-WAV targets switch embedded metadata to external, and the metadata
    // change counter is snapshotted BEFORE the worker builds the SCMD payload:
    // an edit made while the save runs keeps the document dirty. Shared with
    // the AI-control doc.save / doc.saveAs (Application::beginSave).
    const uint64_t metaSnapshot = d != nullptr ? app.beginSave (*d, file) : 0;

    // Resolve the engine on the message thread: the worker must not look up
    // "the active document" itself.
    AudioEngine* eng = &app.audioEngine();
    juce::Component::SafePointer<MainWindow> self (this);

    std::thread ([self, eng, file, saveAs, dlg, d, metaSnapshot] {
        const bool ok = saveAs ? eng->saveFileAs (file) : eng->saveFile (file);
        juce::MessageManager::callAsync ([self, dlg, ok, file, d, saveAs, metaSnapshot] {
            // The progress dialog blocks closing while the save runs, but be
            // defensive: the window (or the document) may be gone by now.
            if (self == nullptr)
                return;
            MainWindow* const mw = self.getComponent();
            Application& app = mw->app;
            delete dlg;
            const bool docAlive = d != nullptr
                && std::any_of (app.docs.begin(), app.docs.end(),
                                [d] (const std::unique_ptr<FileDoc>& p) { return p.get() == d; });
            FileDoc* const doc = docAlive ? d : nullptr;
            if (!ok)
            {
                // The * stays in the title; say why instead of failing silently.
                sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                                   "Save failed",
                                                   "Could not save " + file.getFullPathName ()
                                                       + ".\nCheck that the folder is writable, the file is not "
                                                         "locked by another program and the disk is not full.",
                                                   "", mw);
                return;
            }

            // Save As: the document's path AND display name now follow the new
            // location (Files list, footer, unsaved-changes prompts, Open Recent).
            // Path/name/Open Recent (Save As), the external-mode sidecar with
            // the legacy per-category sidecars consolidated into it, and the
            // metadata-dirty flag (unless the metadata changed while the save
            // ran): Application::completeSave, shared with AI control.
            if (doc != nullptr)
            {
                const bool metaWritten = app.completeSave (*doc, file, saveAs, metaSnapshot);
                if (saveAs)
                {
                    if (mw->statusBar && doc == app.doc ())
                        mw->statusBar->setFileName (file.getFileName ());
                }
                if (!metaWritten)
                    sc::alert::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                        "Save metadata failed",
                        "The audio was saved, but the metadata sidecar next to\n"
                            + file.getFullPathName ()
                            + "\ncould not be written. Markers and selections are still unsaved.",
                        "", mw);
            }

            mw->updateTitle();
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
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Save Metadata",
                                            juce::String::fromUTF8 (err.c_str()),
                                            "", this);
    else
        updateTitle();   // the * disappears if nothing else is unsaved
}

bool MainWindow::isBackgroundJobRunning() const
{
    const int waits = audioInitDialog != nullptr ? 1 : 0;   // not a job: nothing to protect
    return sc::WorkProgressDialog::numActive () > waits;
}

bool MainWindow::handleCloseFile()
{
    if (app.numDocs () == 0 || isBackgroundJobRunning ()) return false;

    // Same document Application::closeFile() closes.
    const int idx = (app.activeDocIndex >= 0 && app.activeDocIndex < app.numDocs ())
                        ? app.activeDocIndex : 0;
    const FileDoc& d = *app.docs[(size_t) idx];
    if (app.docIsUnsaved (d))
    {
        const juce::String name = d.name.isNotEmpty () ? d.name : d.file.getFileName ();
        juce::AlertWindow dlg ("Close File",
            "This file has unsaved changes:\n\n" + name +
            "\n\nDo you want to close it and discard the changes?",
            juce::AlertWindow::WarningIcon, this);
        dlg.addButton ("Close", 1);
        dlg.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        if (dlg.runModalLoop () != 1) return false;   // keep it open
    }

    // closeFile() fires onActiveDocChanged (or resets the empty state),
    // which refreshes every panel and view.
    app.closeFile ();
    return true;
}

void MainWindow::handleCloseAll()
{
    if (app.numDocs () == 0 || isBackgroundJobRunning ()) return;

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

// The overlay options currently armed in the File > Spectrogram submenu.
static SpectralView::SpectralExportOptions spectralExportOptionsFromSettings (const Settings& s)
{
    SpectralView::SpectralExportOptions o;
    o.axes       = s.exportAxes;
    o.markers    = s.exportMarkers;
    o.selections = s.exportSelections;
    o.drawings   = s.exportDrawings;
    return o;
}

// Render + write the spectrogram PNG on a background thread.
void MainWindow::exportSpectralAsync (const juce::File& file,
                                      const SpectralView::SpectralExportOptions& opts)
{
    auto* dlg = new WorkProgressDialog ("Exporting",
                                        "Rendering spectrogram to " + file.getFileName() + " ...",
                                        true, this);
    dlg->setProgress (-1.0);   // out of range -> indeterminate spinning bar
    dlg->beginModal ();

    // Snapshot the view on the message thread (channels, time/frequency
    // window, settings, overlays); the worker then never touches the view.
    SpectralView::PngExportJob job = spectralView ? spectralView->preparePngExport (file, opts)
                                                  : SpectralView::PngExportJob();

    std::thread ([this, file, job, dlg] {
        const bool ok = job != nullptr && job (&dlg->cancelled);
        juce::MessageManager::callAsync ([this, file, dlg, ok] {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (!ok && !wasCancelled)
                sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                                    "Export failed",
                                                    "Could not write the PNG file.", "", this);
        });
    }).detach();
}

// File > Spectrogram > Save Spectrogram As...
// (Ctrl+Shift+E is now the Filter EQ effect shortcut).
void MainWindow::handleExportSpectral()
{
    if (!spectralView || !app.audioEngine().isFileLoaded()) return;
    juce::FileChooser chooser ("Save Spectrogram As",
        app.audioEngine().getFile().getParentDirectory(), "*.png",
        kUseNativeFileChooser);
    if (chooser.browseForFileToSave (true))
        exportSpectralAsync (chooser.getResult(),
                             spectralExportOptionsFromSettings (app.settings));
}

// File > Spectrogram > Take snapshot:
// writes a PNG next to the WAV file with a date/time stamp in its name.
// (Ctrl+Shift+F is now the repeat-last-effect shortcut.) If the audio has
// no file location yet, falls back to the normal save dialog.
void MainWindow::takeSpectrogramSnapshot()
{
    if (!spectralView || !app.audioEngine().isFileLoaded()) return;
    const juce::File f = app.audioEngine().getFile();
    juce::File target;
    if (f.getFullPathName().isEmpty())
    {
        juce::FileChooser chooser ("Save Spectrogram As",
            f.getParentDirectory(), "*.png", kUseNativeFileChooser);
        if (!chooser.browseForFileToSave (true)) return;
        target = chooser.getResult();
    }
    else
        target = makeSnapshotPath (f);
    exportSpectralAsync (target, spectralExportOptionsFromSettings (app.settings));
}

// Spectral Controls > Refresh (also F5): discard every cached tile and force a
// full re-render of the spectrogram from the audio.
void MainWindow::refreshSpectrogram()
{
    app.spectralEngine.clearCache ();
    if (spectralView) spectralView->invalidateImage ();
}

// Spectral Controls > "Show harmonics" (also Ctrl+H): toggle the spectrogram's
// harmonic markers. syncSharedState repaints both views and persists the flag.
void MainWindow::toggleHarmonicMarkers()
{
    app.settings.specHarmVisible = !app.settings.specHarmVisible;
    if (controlsPanel) controlsPanel->syncHarmControls ();
    if (app.onStateChange) app.onStateChange ();
}

// Ctrl+Shift+F: re-dispatch the previously run effect. Dialog-based effects
// reopen their dialog (pre-filled with the last-used values where supported);
// immediate effects are applied directly.
void MainWindow::repeatLastEffect()
{
    if (lastEffectId == 0) return;   // no effect has been run yet this session
    handleContextMenuAction (lastEffectId);
}

// Alt+Shift+= / '+' and Alt+Shift+-: step the spectrogram's FFT size one power
// of two along the combo's 256..65536 ladder. In Auto mode the current resolved
// size is used as the starting point and the mode switches to manual.
void MainWindow::stepFftSize (bool up)
{
    if (!spectralView) return;
    const int cur = app.settings.fftAuto ? spectralView->autoFftSizeForCurrentZoom ()
                                         : app.settings.fftSize;
    const int next = up ? std::min (cur * 2, 65536) : cur / 2;
    if (!up && next < 256) return;   // already at/below the ladder minimum
    if (next == cur) return;         // already at the top of the ladder
    app.settings.fftAuto = false;
    app.settings.fftSize = next;
    if (controlsPanel) controlsPanel->setFftSizeSelection (next);
    updateAutoFftLabel ();
    if (app.onStateChange) app.onStateChange ();
}

// Keeps the Spectral Controls combo's "Auto (N)" label in sync with the size
// Auto mode would resolve to at the current zoom levels. Called from
// refreshPanels() and after frequency-axis changes; cheap no-op when N is
// unchanged.
void MainWindow::updateAutoFftLabel()
{
    if (!controlsPanel || !spectralView) return;
    const int n = spectralView->autoFftSizeForCurrentZoom ();
    juce::String label = "Auto";   // no file / un-laid-out view: plain "Auto"
    if (n > 0)
        label = "Auto (" + juce::String (n) + ")";
    if (label == autoFftLabel_) return;
    autoFftLabel_ = label;
    controlsPanel->rebuildFftSizeItems (n);
}

void MainWindow::vScrollInsetsForView (Application& app, juce::Component* view,
                                      int& topInset, int& botInset)
{
    // The vertical scrollbar spans only the track data area: when a view draws
    // its own time ruler strip (top or bottom) that strip is excluded so the
    // bar stops at the actual data height.
    topInset = botInset = 0;
    if (auto* wv = dynamic_cast<WaveformView*> (view))
    {
        if (wv->showsTimeRuler())
        {
            const int h = app.rulerStackHeight();   // the whole stack, extra axes included
            if (wv->rulerAtBottom()) botInset = h;
            else                     topInset = h;
        }
    }
    else if (auto* sv = dynamic_cast<SpectralView*> (view))
    {
        if (sv->showsOwnTimeRuler())
        {
            const int h = app.rulerStackHeight();
            if (sv->rulerAtBottom()) botInset = h;
            else                     topInset = h;
        }
    }
}

juce::File MainWindow::makeSnapshotPath (const juce::File& wav, const juce::Time* when)
{
    const juce::Time now = (when != nullptr) ? *when : juce::Time::getCurrentTime ();
    auto pad2 = [] (int v) { return v < 10 ? juce::String ("0") + juce::String (v) : juce::String (v); };
    // NOTE: this JUCE build's getMonth() is zero-based (January = 0).
    const juce::String stamp = juce::String (now.getYear()) + "-" + pad2 (now.getMonth () + 1) + "-"
                             + pad2 (now.getDayOfMonth ()) + "_" + pad2 (now.getHours ()) + "-"
                             + pad2 (now.getMinutes ()) + "-" + pad2 (now.getSeconds ());
    const juce::File dir = wav.getParentDirectory();
    const juce::String base = wav.getFileNameWithoutExtension ();
    juce::File out = dir.getChildFile (base + "_snapshot_" + stamp + ".png");
    for (int i = 1; out.existsAsFile () && i < 1000; ++i)
        out = dir.getChildFile (base + "_snapshot_" + stamp + "_" + juce::String (i) + ".png");
    return out;
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

    // The Editor tabs carry the file names too (Save As renames one).
    refreshEditorTabTitles ();
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
    auto* w = new OwningAlertWindow ("About SpectroCraft", "",
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

// Forward declaration: the Recording Options cases below prompt for a
// duration with this helper, which is defined further down in this file.
static bool promptNumberValue (juce::Component* parent, const juce::String& title,
                               const juce::String& message, const juce::String& initial,
                               double& out, bool clockStyle);

void MainWindow::menuItemSelected (int menuItemID, int topLevelMenuIndex)
{
    using enum MenuIDs;

    // Diagnostic log: the item is a breadcrumb, and what a freeze happened
    // "during" while its handler runs.
    juce::String diagItem;
    if (sc::diag::enabled ())
    {
        diagItem = menuItemDescription (menuItemID, topLevelMenuIndex);
        sc::diag::noteMenu (diagItem);
    }
    const sc::diag::ScopedActivity diagActivity ("menu " + diagItem);

    if (menuItemID == HelpDiagLog)
    {
        toggleDiagnosticLog ();
        return;
    }

    // Recording Device submenu: RecDeviceBase + pluginIndex*1000 + deviceIndex.
    if (menuItemID >= RecDeviceBase && menuItemID < RecDeviceEnd)
    {
        handleRecDevicePick (menuItemID);
        return;
    }

    // Window > Workspaces (also the workspace bar's menu) and maximise.
    if ((menuItemID >= WindowWorkspaceBase && menuItemID < WindowWorkspaceBase + 48)
        || (menuItemID >= WindowWsReset && menuItemID <= WindowWsDelete))
    {
        handleWorkspaceCommand (menuItemID);
        return;
    }
    if (menuItemID == WindowMaximizeGroup)
    {
        toggleMaximizePanelGroup ();
        return;
    }

    // Window menu: toggle a panel open/closed.
    if (menuItemID >= WindowPanelBase && menuItemID < WindowPanelBase + 16)
    {
        const auto entries = leftPanelEntries ();
        const int i = menuItemID - WindowPanelBase;
        if (i >= 0 && i < (int) entries.size ())
            setPanelVisible (entries[i].id, !isPanelOpen (entries[i].id));
        return;
    }

    // Window menu: "Allow multiple instances" toggle. Persisted immediately;
    // it is read by the next launch before its Application object exists.
    if (menuItemID == WindowAllowMultiple)
    {
        app.settings.allowMultipleInstances = !app.settings.allowMultipleInstances;
        app.saveSettings ();
        return;
    }

    // Window menu: Video, listed with the other panels and toggled like
    // them. Opening docks the picture as the Video panel (dragging its tab
    // out floats it); unchecking hides the picture wherever it is, the
    // Video panel or its own window.
    if (menuItemID == WindowVideo)
    {
        if (isPanelOpen ("video") || isVideoWindowVisible ())
        {
            const juce::ScopedValueSetter<bool> quiet (videoHideQuietly_, true);
            setPanelVisible ("video", false);
            setVideoWindowVisible (false);
        }
        else
        {
            setVideoDocked (true);
        }
        return;
    }
    // Window menu: "Allow AI control (localhost)". Persisted and applied at
    // once (Main.cpp starts/stops the control server).
    if (menuItemID == WindowAllowControl)
    {
        app.settings.controlEnabled = !app.settings.controlEnabled;
        app.saveSettings ();
        if (onControlToggled) onControlToggled (app.settings.controlEnabled);
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

    // File > Loaded Files: activate a document, or act on the active one.
    if (menuItemID >= DocBase && menuItemID < DocEnd)
    {
        const int i = menuItemID - DocBase;
        if (i >= 0 && i < app.numDocs())
            activateDocById (app.docs[(size_t) i]->id);
        return;
    }
    if (menuItemID == DocScaleForm || menuItemID == PanelScaleId)
    {
        openScaleForm (ctxScaleKind);
        return;
    }
    if (menuItemID == DocFolder || menuItemID == PanelFolderId)
    {
        openActiveDocFolder();
        return;
    }
    if (menuItemID == DocClose)
    {
        if (app.doc() != nullptr) handleCloseFile();   // asks about unsaved changes
        return;
    }

    if (menuItemID == ClearRecent)
    {
        app.clearRecentFiles();
        return;
    }

    // Per-channel visibility toggles: ChannelBase + channel index.
    // Ctrl/Shift+click on a channel shows ONLY that channel (solo); doing it
    // again while solo on that channel restores every channel.
    if (menuItemID >= ChannelBase && menuItemID < ChannelBase + app.audioEngine().numChannels())
    {
        const int ch = menuItemID - ChannelBase;
        const auto mods = juce::ModifierKeys::getCurrentModifiers();
        const bool soloMods = mods.isCommandDown() || mods.isShiftDown();
        if (soloMods && app.visibleChannels.size() == 1
                && app.visibleChannels[0] == ch)
            showEveryChannel();   // solo off: back to all channels
        else if (soloMods)
        {
            app.visibleChannels = { ch };
            app.syncActiveDoc ();   // keep the active document's copy in step
            syncSharedState();
        }
        else
        {
            app.toggleChannelVisible (ch);
            syncSharedState();   // views and footer channel numbers follow
        }
        return;
    }

    // Show every channel in both views.
    if (menuItemID == ViewShowAllChannels)
    {
        showEveryChannel();
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
        setLeftPanelsVisible (!areLeftPanelsVisible ());
        return;
    }

    // Track axis placement: toggle, realign the time bar with the plot edge,
    // then repaint both views and persist via syncSharedState().
    if (menuItemID == ViewAxisOnRight)
    {
        app.settings.trackAxisOnRight = !app.settings.trackAxisOnRight;
        resized();
        if (app.onStateChange) app.onStateChange ();
        return;
    }
    if (menuItemID == ViewRulerAtBottom)
    {
        app.settings.trackRulerAtBottom = !app.settings.trackRulerAtBottom;
        resized();
        if (app.onStateChange) app.onStateChange ();
        return;
    }

    // Horizontal grid lines: no layout change, so syncSharedState() repaints
    // both views and persists the flags.
    if (menuItemID == ViewShowAmpGrid)
    {
        app.settings.showAmpGrid = !app.settings.showAmpGrid;
        if (app.onStateChange) app.onStateChange ();
        return;
    }
    if (menuItemID == ViewShowFreqGrid)
    {
        app.settings.showFreqGrid = !app.settings.showFreqGrid;
        if (app.onStateChange) app.onStateChange ();
        return;
    }
    if (menuItemID == ViewShowThirdOctGrid)
    {
        app.settings.showThirdOctaveGrid = !app.settings.showThirdOctaveGrid;
        if (app.onStateChange) app.onStateChange ();   // repaints + persists
        return;
    }
    if (menuItemID == ViewShowDedTrackGrid)
    {
        app.settings.showDedicatedTrackGrid = !app.settings.showDedicatedTrackGrid;
        // syncSharedState() does not unconditionally repaint the dedicated
        // track, so ask for it explicitly.
        if (tablesTrack) tablesTrack->repaint();
        if (app.onStateChange) app.onStateChange ();   // persists the flag
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
            handleCloseFile();   // asks first when the file has unsaved changes
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
            handleExportSpectral ();
            break;

        case FileSnapshot:
            takeSpectrogramSnapshot ();
            break;

        // Spectrogram export overlays: toggle + persist (via onStateChange).
        case ExpAxes:
            app.settings.exportAxes = !app.settings.exportAxes;
            if (app.onStateChange) app.onStateChange();
            break;
        case ExpMarkers:
            app.settings.exportMarkers = !app.settings.exportMarkers;
            if (app.onStateChange) app.onStateChange();
            break;
        case ExpSelections:
            app.settings.exportSelections = !app.settings.exportSelections;
            if (app.onStateChange) app.onStateChange();
            break;
        case ExpDrawings:
            app.settings.exportDrawings = !app.settings.exportDrawings;
            if (app.onStateChange) app.onStateChange();
            break;
        case FileQuit: closeButtonPressed(); break;   // refuses while a job runs

        // Edit menu: through the command registry (same code as the API; a
        // streamed file is loaded into memory by the registry's job detour,
        // behind the same progress dialog runDestructiveEdit showed).
        case EditUndo: runMenuCommand ("history.undo", {}, 0, true); refreshAfterEdit(); break;
        case EditRedo: runMenuCommand ("history.redo", {}, 0, true); refreshAfterEdit(); break;
        case EditCut:    runMenuCommand ("edit.cut"); break;
        case EditCopy:   runMenuCommand ("edit.copy", {}, 0, true); break;   // read-only; works on streamed files too
        case EditPaste:  runMenuCommand ("edit.paste"); break;
        case EditCopyToNewFile: runMenuCommand ("edit.copyToNewFile"); break;
        case EditPasteMix: runMenuCommand ("edit.pasteMix"); break;
        case EditPasteToNewChannels: runMenuCommand ("edit.pasteToNewChannels"); break;
        case EditDelete: runMenuCommand ("edit.delete"); break;
        case EditTrimSelection: runMenuCommand ("edit.trim"); break;
        case EditSelectAll: runMenuCommand ("selection.all", {}, 0, true); syncSharedState(); break;
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

        case TransportRecord:
            handleStartStopRecording ();
            break;

        // Recording Options: Unlimited keeps the current behaviour; Loop and
        // Timed prompt for a duration first (the menu then shows it, e.g.
        // "Loop Mode (10s)").
        case RecOptUnlimited:
            app.setRecordingMode (Application::RecordingState::Mode::Unlimited);
            break;
        case RecOptLoop:
        {
            double secs = 0.0;
            if (promptNumberValue (this, "Loop Mode",
                                   "Loop duration in seconds (e.g. 10 or 1:30):",
                                   juce::String (app.recording.durationSecs > 0.0
                                                  ? app.recording.durationSecs : 10),
                                   secs, true)
                && secs > 0.0)
                app.setRecordingMode (Application::RecordingState::Mode::Loop, secs);
            break;
        }
        case RecOptTimed:
        {
            double secs = 0.0;
            if (promptNumberValue (this, "Timed Recording",
                                   "Recording duration in seconds (e.g. 10 or 1:30):",
                                   juce::String (app.recording.durationSecs > 0.0
                                                  ? app.recording.durationSecs : 10),
                                   secs, true)
                && secs > 0.0)
                app.setRecordingMode (Application::RecordingState::Mode::Timed, secs);
            break;
        }

        // Immediate effects record themselves as the "last effect" so that
        // Ctrl+Shift+F can repeat them; dialog-based effects do the same at
        // their apply points.
        case ToolsSilence:   runMenuCommand ("effect.silence", {}, ToolsSilence); break;
        case ToolsFadeIn:    runMenuCommand ("effect.fadeIn", {}, ToolsFadeIn); break;
        case ToolsFadeOut:   runMenuCommand ("effect.fadeOut", {}, ToolsFadeOut); break;
        case ToolsNormalize: runMenuCommand ("effect.normalize", {}, ToolsNormalize); break;
        case ToolsChangeGain: showChangeGainDialog(); break;
        case ToolsResample: showResampleDialog(); break;
        case ToolsStemSeparate: showStemSeparationWindow(); break;
        case ToolsSuperResolve: showSuperResolutionWindow(); break;
        case ToolsSplitSpectralSelCfg:
            showSplitConfigDialog ();
            break;
        case ToolsSplitSpectralSelExec:
            runSplitExecute ();
            break;
        case ToolsGenSilence: showGenerateSilenceDialog(); break;
        case ToolsGenNoise:   showGenerateNoiseDialog(); break;
        case ToolsGenTone:    showGenerateToneDialog(); break;
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        case ToolsNoiseSuppress: showNoiseSuppressDialog(); break;
#endif
        case ActionsRunSelected:
            runAction (actionsPanel ? actionsPanel->primaryIndex() : -1);
            break;
        case ActionsBatch:
            showActionBatchWindow (actionsPanel ? actionsPanel->primaryIndex() : -1);
            break;
        case ToolsReverse: runMenuCommand ("effect.reverse", {}, ToolsReverse); break;
        case ToolsInvert:  runMenuCommand ("effect.invert", {}, ToolsInvert); break;
        case ToolsTimeStretch: showTimeStretchDialog(); break;
        case ToolsCatTimeScale: showContentAwareTimeScaleDialog(); break;
        case ToolsFilterEq:    showFilterEqDialog(); break;
        case ToolsEnvelope:    showEnvelopeDialog(); break;

        case HelpAbout:
            showAboutDialog (this);
            break;
        case HelpOpenDiagFolder:
        {
            const juce::File dir = sc::diag::folder ();
            dir.createDirectory ();
            if (! sc::alert::isHeadless ())   // never launch the file browser from the tests
                dir.startAsProcess ();
            break;
        }
        case HelpCopyDiagSummary: copyDiagnosticSummary (); break;
        case HelpSupportBundle:   showSupportBundleDialog (); break;
        case HelpCheckUpdates:    checkForUpdates (false); break;
        case HelpCheckUpdatesAtStartup:
            // Help > Check for Updates at Startup: the same lookup once a day
            // when the app starts (docs/updates.md).
            app.settings.checkUpdatesAtStartup = !app.settings.checkUpdatesAtStartup;
            app.saveSettings();
            break;
        case HelpShowTooltips:
            app.settings.showTooltips = !app.settings.showTooltips;
            app.saveSettings();
            applyTooltipsSetting();
            break;
    }
}

juce::String MainWindow::menuItemDescription (int menuItemID, int topLevelMenuIndex)
{
    // Only the menu the item came from is rebuilt (the Transport menu lists
    // recording devices: rebuilding every menu would cost more).
    const auto names = getMenuBarNames ();
    if (juce::isPositiveAndBelow (topLevelMenuIndex, names.size ()))
    {
        const auto menu = getMenuForIndex (topLevelMenuIndex, names[topLevelMenuIndex]);
        for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next ();)
            if (it.getItem ().itemID == menuItemID)
                return names[topLevelMenuIndex] + " > "
                       + it.getItem ().text.upToFirstOccurrenceOf ("\t", false, false).trim ();
    }
    return "menu item " + juce::String (menuItemID);
}

// The diagnostic log folder as a link: a click opens it in the Finder /
// Explorer (created first: nothing is written before the log is on).
std::unique_ptr<juce::HyperlinkButton> MainWindow::makeDiagFolderLink (const juce::File& folder)
{
    auto link = std::make_unique<juce::HyperlinkButton> (folder.getFullPathName (), juce::URL ());
    link->setName ({});   // an AlertWindow writes a custom component's name above it
    link->setFont (juce::Font (juce::FontOptions (15.0f)), false, juce::Justification::centredLeft);
    link->setColour (juce::HyperlinkButton::textColourId, juce::Colour (0xff4ea1f5));
    link->setTooltip ("Open " + folder.getFullPathName ());
    link->onClick = [folder]
    {
        folder.createDirectory ();
        folder.startAsProcess ();
    };
    const int w = juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (15.0f)), folder.getFullPathName ());
    link->setSize (juce::jlimit (120, 560, w + 12), 24);
    return link;
}

const juce::String MainWindow::kDiagLogDialogText =
    "SpectroCraft will keep a small log on this computer to help find performance "
    "problems and bugs.\n\n"
    "Recorded: the errors and warnings you see, crashes, interface freezes, audio "
    "dropouts, slow operations and the startup time, the commands and menus you use, "
    "and the files you open (with their paths).\n"
    "Never recorded: audio, transcribed text, table values, scripts, access tokens.\n\n"
    "Nothing is sent anywhere. The files stay in this folder (10 MB at most), and "
    "Help > Copy Diagnostic Summary or Create Support Bundle prepares what you choose "
    "to share:";

// Says what is recorded before the first byte is written, with a link that
// opens the folder the files go to.
MainWindow::DiagLogDialog MainWindow::makeDiagLogDialog (juce::Component* centreAround)
{
    DiagLogDialog d;
    d.link = makeDiagFolderLink (sc::diag::folder ());
    d.window = std::make_unique<juce::AlertWindow> ("Diagnostic Log", kDiagLogDialogText,
                                                    juce::MessageBoxIconType::InfoIcon, centreAround);
    d.window->addCustomComponent (d.link.get ());
    d.window->addButton ("Turn On", 1, juce::KeyPress (juce::KeyPress::returnKey));
    d.window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    return d;
}

void MainWindow::toggleDiagnosticLog ()
{
    if (sc::diag::enabled ())
    {
        app.settings.diagLogEnabled = false;
        app.saveSettings ();
        if (onDiagToggled) onDiagToggled (false);
        return;
    }
    if (! sc::alert::isHeadless ())
    {
        auto dlg = makeDiagLogDialog (this);
        dlg.window->toFront (true);
        if (dlg.window->runModalLoop () != 1)
            return;
    }
    app.settings.diagLogEnabled = true;
    app.saveSettings ();
    if (onDiagToggled) onDiagToggled (true);
}

void MainWindow::copyDiagnosticSummary ()
{
    const auto md = sc::diag::summaryMarkdown (30 * 1024);
    juce::SystemClipboard::copyTextToClipboard (md);
    sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Diagnostic Summary Copied",
        "The summary (" + juce::String (juce::jmax (1, (int) md.getNumBytesAsUTF8 () / 1024))
            + " KB) is in the clipboard, ready to paste into a support request."
            + (sc::diag::enabled () ? juce::String ()
                                    : juce::String ("\n\nThe diagnostic log is off: turn it on with Window > "
                                                    "Diagnostic Log, then reproduce the problem.")),
        "", this);
}

bool MainWindow::writeSupportBundleTo (const juce::File& zip, juce::String* error)
{
    if (! sc::diag::writeSupportBundle (zip, error))
        return false;
    sc::diag::event ("diag.bundle", sc::diag::Level::info,
                     { { "path", zip.getFullPathName () }, { "sizeKb", (int) (zip.getSize () / 1024) } });
    return true;
}

void MainWindow::showSupportBundleDialog ()
{
    const auto files = sc::diag::bundleFiles ();
    if (files.isEmpty ())
    {
        sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Create Support Bundle",
            "There is no diagnostic log yet. Turn it on with Help > Diagnostic Log, "
            "reproduce the problem, then create the bundle.", "", this);
        return;
    }
    if (sc::alert::isHeadless ())
        return;   // the tests call writeSupportBundleTo() directly

    // Preview: exactly what the zip will contain.
    juce::String text ("The zip will contain:\n\n");
    juce::int64 total = 0;
    for (const auto& f : files)
    {
        text << "  " << f.getFileName () << "  (" << juce::String (juce::jmax ((juce::int64) 1, f.getSize () / 1024)) << " KB)\n";
        total += f.getSize ();
    }
    text << "  summary.md, README.txt\n"
         << "(" << juce::String (juce::jmax ((juce::int64) 1, total / 1024)) << " KB of logs in all)\n\n"
         << "It includes file paths, commands, timings and error messages; never audio, transcribed "
            "text, table values, scripts or the AI control token. Nothing is sent: you choose where "
            "to save it and whom to give it to.";
    juce::AlertWindow dlg ("Create Support Bundle", text, juce::MessageBoxIconType::InfoIcon, this);
    dlg.addButton ("Save Bundle...", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dlg.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    dlg.toFront (true);
    if (dlg.runModalLoop () != 1)
        return;

    const auto now = juce::Time::getCurrentTime ();
    const juce::File initial = juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
        .getChildFile ("SpectroCraft-support-" + now.formatted ("%Y%m%d-%H%M") + ".zip");
    juce::FileChooser chooser ("Save the support bundle", initial, "*.zip");
    if (! chooser.browseForFileToSave (true))
        return;
    const juce::File zip = chooser.getResult ().withFileExtension ("zip");
    juce::String error;
    if (writeSupportBundleTo (zip, &error))
        zip.revealToUser ();
    else
        sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Create Support Bundle",
                                        error, "", this);
}

// ==========================================================================
// Help > Check for Updates (docs/updates.md)
// ==========================================================================
//
// The flow, and why it is split the way it is:
//
//   1. look up the latest GitHub release and compare versions;
//   2. download the manifest + its signature first (a few KB): a release that
//      is not signed with the key built into the app fails before ~100 MB of
//      archive has been fetched;
//   3. download the archive, then unpack it to a staging folder while hashing
//      every file against the manifest - nothing in the install folder has
//      been touched yet, so a failure or a Cancel leaves the running app alone;
//   4. start spectrocraft-updater.exe from the staging folder and quit: the
//      updater waits for this process to disappear, swaps the files (SpectroCraft.exe
//      cannot be overwritten while it runs), writes update-result.json and
//      starts the app again with --finish-update, where reportFinishedUpdate()
//      tells the user what happened.
//
// Windows only: the archive layout and the updater are Windows-specific, and
// so is this wiring. src/Updates.cpp itself compiles everywhere.

#if JUCE_WINDOWS

namespace {

/** The release notes as plain text: an AlertWindow has no clickable links and
    no markdown, so emphasis and link targets are removed and long notes are
    cut. The full notes stay on the release page. */
juce::String updateNotesPreview (const juce::String& notes)
{
    juce::String s = notes.replace ("\r\n", "\n").replace ("\r", "\n");
    s = s.replace ("*", "");

    for (int from = 0;;)
    {
        const int open = s.indexOf (from, "[");
        if (open < 0) break;
        const int close = s.indexOf (open, "]");
        if (close < 0) break;
        int end = close + 1;
        if (end < s.length() && s[end] == '(')
        {
            const int closeParen = s.indexOf (end, ")");
            if (closeParen >= 0) end = closeParen + 1;
        }
        s = s.substring (0, open) + s.substring (open + 1, close) + s.substring (end);
        from = open + juce::jmax (1, close - open);   // never stuck on "[]"
    }

    s = s.trim();
    if (s.length() > 1400) s = s.substring (0, 1400) + "\n...";
    return s;
}

juce::String megabytes (juce::int64 bytes)
{
    if (bytes <= 0) return "? MB";
    return juce::String ((double) bytes / (1024.0 * 1024.0), 1) + " MB";
}

} // namespace

void MainWindow::checkForUpdates (bool automatic)
{
    if (updateCheckRunning.exchange (true)) return;   // one lookup at a time

    const juce::String current = SC_VERSION_STRING;
    const juce::String skipped = app.recentStore != nullptr
                               ? app.recentStore->getValue ("update_skipped_version")
                               : juce::String();

    // "At most once a day" is measured from the attempt, not from a success:
    // an offline startup must not retry on every launch.
    if (app.recentStore != nullptr)
    {
        app.recentStore->setValue ("update_last_check_ms",
                                   (juce::int64) juce::Time::getCurrentTime().toMilliseconds());
        app.recentStore->save();
    }

    juce::Component::SafePointer<MainWindow> self (this);
    std::thread ([this, self, automatic, current, skipped] {
        sc::update::ReleaseInfo rel;
        juce::String err;
        const bool ok = sc::update::fetchLatestRelease (rel, err);

        juce::MessageManager::callAsync ([self, automatic, current, skipped, ok, rel, err] {
            if (self == nullptr) return;
            self->updateCheckRunning = false;

            if (! ok)
            {
                if (automatic)
                {
                    // A startup check must never interrupt or nag.
                    sc::diag::event ("update", sc::diag::Level::debug,
                                     { { "result", "lookup_failed" }, { "error", err } });
                    return;
                }
                sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                    "Check for Updates",
                    "The update check did not work:\n\n" + err
                    + "\n\nNothing was changed.", "", self.getComponent());
                return;
            }

            if (sc::update::compareVersions (rel.version, current) <= 0)
            {
                if (automatic) return;   // nothing new: stay silent
                sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                    "Check for Updates",
                    "You are using the newest release (" + current + ").\n\n"
                    "Releases are listed at " + sc::update::kReleasesUrl + ".",
                    "", self.getComponent());
                return;
            }

            if (automatic && skipped == rel.version)
                return;   // the user already said "Later" about this version

            self->offerUpdate (rel, current, automatic);
        });
    }).detach();
}

void MainWindow::offerUpdate (const sc::update::ReleaseInfo& rel, const juce::String& current,
                              bool automatic)
{
    const sc::update::AssetInfo* asset = sc::update::pickAsset (rel);
    if (asset == nullptr)
    {
        sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Check for Updates",
            "Release " + rel.tag + " has no archive for "
            + sc::update::platformToken() + " " + sc::update::archToken() + ".\n\n"
            + "The release page has the files it does publish: " + rel.pageUrl + ".",
            "", this);
        return;
    }

    const sc::update::AssetInfo* manifestAsset =
        sc::update::findAsset (rel, sc::update::kManifestName);
    const sc::update::AssetInfo* sigAsset =
        sc::update::findAsset (rel, sc::update::kManifestSigName);
    if (manifestAsset == nullptr || sigAsset == nullptr)
    {
        sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Check for Updates",
            "Release " + rel.tag + " has no signed update manifest, so this version "
            "cannot be installed automatically.\n\nInstall it by hand from " + rel.pageUrl
            + ": download the archive and replace the files in this folder.",
            "", this);
        return;
    }

    const juce::String title = rel.title.isNotEmpty() ? rel.title : ("SpectroCraft " + rel.version);
    juce::String text = title + " is available (you are using " + current + ").\n\n";
    const juce::String notes = updateNotesPreview (rel.notes);
    if (notes.isNotEmpty()) text += notes + "\n\n";
    text += "Download: " + asset->name + " (" + megabytes (asset->size) + ").\n";
    text += "SpectroCraft closes while the files are replaced, then starts again.\n";
    text += "Unsaved files are not saved by the update - save them first.";

    juce::AlertWindow dlg ("A new version is available", text,
                           juce::MessageBoxIconType::InfoIcon, this);
    dlg.addButton ("Update Now", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dlg.addButton ("Later", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    dlg.toFront (true);
    if (dlg.runModalLoop () != 1)
    {
        // Remember the refusal: the startup check must not offer the same
        // version again (a newer one will be offered as usual).
        if (app.recentStore != nullptr)
        {
            app.recentStore->setValue ("update_skipped_version", rel.version);
            app.recentStore->save();
        }
        if (automatic && sc::diag::enabled())
            sc::diag::event ("update", sc::diag::Level::debug,
                             { { "result", "deferred" }, { "version", rel.version } });
        return;
    }

    startUpdate (rel, *asset, *manifestAsset, *sigAsset);
}

void MainWindow::startUpdate (const sc::update::ReleaseInfo& rel, const sc::update::AssetInfo& asset,
                              const sc::update::AssetInfo& manifestAsset,
                              const sc::update::AssetInfo& sigAsset)
{
    if (isBackgroundJobRunning() || app.control().jobs.numRunning() > 0)
    {
        sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Update",
            "Another job is running. Wait for it to finish, then start the update "
            "again.", "", this);
        return;
    }

    const juce::File install = sc::update::installDir();
    if (! sc::update::dirIsWritable (install))
    {
        sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Update",
            "SpectroCraft is running from a folder it may not write to:\n\n"
            + install.getFullPathName() + "\n\n"
            "Copy the program to a folder you own (Desktop, Documents...) and start "
            "it from there; the update replaces the files in that folder.",
            "", this);
        return;
    }

    const juce::String version = rel.version;
    const juce::String current = SC_VERSION_STRING;

    auto* dlg = new WorkProgressDialog ("Installing SpectroCraft " + version,
                                        "Downloading the update manifest...", true, this);
    dlg->setProgress (0.0);
    dlg->beginModal();
    updateCheckRunning = true;

    juce::Component::SafePointer<MainWindow> self (this);
    std::thread ([self, dlg, rel, asset, manifestAsset, sigAsset, install, version, current] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v)
        {
            if (v != 1.0 && v - lastSent < 0.02) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };
        // Stages: manifest 0.02, signature 0.05, archive 0.05..0.85, unpack
        // and hash 0.85..1.0. A cancel stops at the stage it is in.
        auto stageProgress = [&sendProgress] (double from, double to, double frac) {
            sendProgress (from + (to - from) * juce::jlimit (0.0, 1.0, frac));
        };

        const juce::File tmp = sc::update::updateTempDir();
        const juce::File staging = tmp.getChildFile ("staging");

        juce::String err;
        bool cancelled = false;
        sc::update::Manifest manifest;

        // A leftover from an earlier attempt is thrown away: the plan and the
        // staged files always belong to the release being installed now.
        tmp.deleteRecursively();
        tmp.createDirectory();

        // 1. manifest + signature (a few KB, so a bad release fails early).
        juce::String manifestText, sigText;
        if (err.isEmpty() && ! dlg->cancelled.load())
        {
            if (! sc::update::httpGetString (manifestAsset.url, manifestText, err))
                err = "Could not download " + manifestAsset.name + ": " + err;
            else
                stageProgress (0.0, 0.02, 1.0);
        }
        if (err.isEmpty() && ! dlg->cancelled.load())
        {
            if (! sc::update::httpGetString (sigAsset.url, sigText, err))
                err = "Could not download " + sigAsset.name + ": " + err;
            else
                stageProgress (0.02, 0.05, 1.0);
        }
        if (err.isEmpty())
        {
            if (! sc::update::parseManifest (manifestText, manifest, err))
                err = "The update manifest is not usable: " + err;
        }
        if (err.isEmpty() && ! sc::update::verifyManifestSignature (manifestText, sigText, err))
            err = "The update manifest could not be verified: " + err;
        if (err.isEmpty() && sc::update::compareVersions (manifest.version, version) != 0)
            err = "The manifest belongs to version " + manifest.version
                + ", not to the release " + version + " it was published with.";
        if (err.isEmpty() && sc::update::compareVersions (manifest.version, current) <= 0)
            err = "The manifest describes version " + manifest.version
                + ", which is not newer than the " + current + " you are using.";

        // 2. the archive.
        const juce::File zip = tmp.getChildFile (asset.name);
        if (err.isEmpty() && ! dlg->cancelled.load())
        {
            if (! sc::update::httpGetToFile (asset.url, zip,
                    [&] (double frac, const juce::String& stage)
                    {
                        stageProgress (0.05, 0.85, frac);
                        juce::MessageManager::callAsync ([dlg, stage] { dlg->setStatusText (stage); });
                        return ! dlg->cancelled.load();
                    }, err))
            {
                if (dlg->cancelled.load()) cancelled = true;
                else err = "Could not download the update: " + err;
            }
        }
        else if (err.isEmpty())
            cancelled = true;

        // 3. unpack to the staging folder, hashing every file.
        if (err.isEmpty() && ! cancelled)
        {
            if (! sc::update::unzipAndVerify (zip, staging, manifest,
                    [&] (double frac, const juce::String& stage)
                    {
                        stageProgress (0.85, 1.0, frac);
                        juce::MessageManager::callAsync ([dlg, stage] { dlg->setStatusText (stage); });
                        return ! dlg->cancelled.load();
                    }, err))
            {
                if (dlg->cancelled.load()) cancelled = true;
                else err = "The downloaded archive does not match the manifest: " + err;
            }
        }

        // 4. the plan the updater reads, and the updater itself.
        if (err.isEmpty() && ! cancelled && ! sc::update::writeUpdatePlan (staging, manifest, err))
            err = "Could not prepare the update: " + err;

        const juce::File updater = err.isEmpty() && ! cancelled
                                 ? sc::update::findUpdaterBinary (staging) : juce::File();
        if (err.isEmpty() && ! updater.existsAsFile())
            err = "The archive does not contain spectrocraft-updater, so the "
                  "files cannot be replaced while SpectroCraft is running.";

        const bool cancelledNow = dlg->cancelled.load();
        juce::MessageManager::callAsync ([self, dlg, err, cancelled, cancelledNow,
                                         staging, updater, install, version] {
            delete dlg;
            if (self == nullptr) return;
            self->updateCheckRunning = false;

            if (! err.isEmpty() || cancelled || cancelledNow)
            {
                // Nothing in the install folder was touched; drop the download.
                sc::update::updateTempDir().deleteRecursively();
                if (cancelled || cancelledNow)
                {
                    sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Update",
                        "The update was cancelled. Nothing was changed.", "", self.getComponent());
                }
                else
                {
                    sc::diag::event ("update", sc::diag::Level::warn,
                                     { { "result", "failed" }, { "version", version },
                                       { "error", err } });
                    sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Update",
                        "The update was not installed:\n\n" + err
                        + "\n\nSpectroCraft is still version " + SC_VERSION_STRING
                        + "; nothing in this folder was changed.", "", self.getComponent());
                }
                return;
            }

            // Everything is verified and staged. Only now does the app ask to
            // close: the updater cannot replace SpectroCraft.exe while it runs.
            const auto unsaved = self->app.unsavedDocNames();
            if (! unsaved.empty())
            {
                juce::String list;
                for (const auto& n : unsaved)
                {
                    if (! list.isEmpty()) list += "\n";
                    list += "  " + n;
                }
                juce::AlertWindow dlg2 ("Install the update", "SpectroCraft has to close "
                    "to replace its own files. These files have unsaved changes:\n\n" + list
                    + "\n\nSave them first (File > Save All) or continue without saving.",
                    juce::MessageBoxIconType::WarningIcon, self.getComponent());
                dlg2.addButton ("Close and update", 1, juce::KeyPress (juce::KeyPress::returnKey));
                dlg2.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                dlg2.toFront (true);
                if (dlg2.runModalLoop () != 1)
                {
                    sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Update",
                        "Cancelled. The verified files stay staged; nothing in this "
                        "folder was changed.", "", self.getComponent());
                    return;
                }
            }

            juce::String launchErr;
            if (! sc::update::launchUpdater (updater, install, staging,
                                             juce::File::getSpecialLocation (
                                                 juce::File::currentExecutableFile), launchErr))
            {
                sc::update::updateTempDir().deleteRecursively();
                sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Update",
                    "The update could not be started:\n\n" + launchErr
                    + "\n\nSpectroCraft is still version " + SC_VERSION_STRING + ".", "",
                    self.getComponent());
                return;
            }

            sc::diag::event ("update", sc::diag::Level::info,
                             { { "result", "installing" }, { "from", SC_VERSION_STRING },
                               { "to", version } });
            // The updater takes over from here: it waits for this process to
            // exit, swaps the files and starts the new version with
            // --finish-update. requestQuit refuses while a job runs.
            self->requestQuit (true);
        });
    }).detach ();
}

void MainWindow::maybeCheckForUpdatesAtStartup()
{
    if (! app.settings.checkUpdatesAtStartup || app.recentStore == nullptr) return;

    const juce::int64 last = app.recentStore->getValue ("update_last_check_ms").getLargeIntValue();
    const juce::int64 now = (juce::int64) juce::Time::getCurrentTime().toMilliseconds();
    if (last > 0 && now - last < 24LL * 3600 * 1000) return;   // once a day

    checkForUpdates (true);
}

void MainWindow::reportFinishedUpdate()
{
    const juce::File install = sc::update::installDir();
    sc::update::UpdateResult res;
    if (! sc::update::readUpdateResult (install, res)) return;   // nothing to report

    // What the updater could not remove itself: the .old backups (one of them
    // is the updater's own running image) and the staging folder.
    std::thread ([res] {
        for (const auto& p : res.oldFiles) juce::File (p).deleteFile();
        for (const auto& p : res.cleanupDirs) juce::File (p).deleteRecursively();
        sc::update::updateTempDir().deleteRecursively();
    }).detach();

    if (res.success)
    {
        sc::diag::event ("update", sc::diag::Level::info,
                         { { "result", "installed" }, { "version", res.version },
                           { "files", (int) res.replaced.size() } });
        if (sc::alert::isHeadless()) return;
        sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Updated",
            "SpectroCraft " + res.version + " is installed.\n\n"
            + juce::String ((int) res.replaced.size()) + " files were replaced.",
            "", this);
        return;
    }

    juce::String text = "The update could not be completed.\n\n";
    if (res.reason.isNotEmpty()) text += res.reason + "\n";
    if (res.failedFile.isNotEmpty()) text += "File: " + res.failedFile + "\n";
    text += "\nSpectroCraft is still the version you had before (" + juce::String (SC_VERSION_STRING)
         + "); the files that were already swapped were put back.";
    sc::diag::event ("update", sc::diag::Level::warn,
                     { { "result", "install_failed" }, { "version", res.version },
                       { "error", res.reason }, { "file", res.failedFile } });
    if (sc::alert::isHeadless()) return;
    sc::alert::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Update failed",
                                    text, "", this);
}

#else   // ! JUCE_WINDOWS: the menu item is not offered; see docs/updates.md.

void MainWindow::checkForUpdates (bool) {}
void MainWindow::offerUpdate (const sc::update::ReleaseInfo&, const juce::String&, bool) {}
void MainWindow::startUpdate (const sc::update::ReleaseInfo&, const sc::update::AssetInfo&,
                              const sc::update::AssetInfo&, const sc::update::AssetInfo&) {}
void MainWindow::maybeCheckForUpdatesAtStartup() {}
void MainWindow::reportFinishedUpdate() {}

#endif

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
    // Ctrl+0 / Zoom Out Full also vertically fits the dedicated track when it
    // is visible.
    if (tablesTrack && tablesTrack->isVisible())
        tablesTrack->refit();
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
        case CtxDelete:          app.recordActionStep (sc::actionTypes::kDelete);
                                 runDestructiveEdit ([&] (std::string* e) { return app.del (e); }); break;
        case CtxCopyToNewFile:
        {
            std::string err;
            if (!app.copyToNewFile (&err))
                showEditError (err);
            break;
        }
        case CtxPasteMix:        runDestructiveEdit ([&] (std::string* e) { return app.pasteMix (e); }); break;

        // ---- The Scale form (replaces the old Scale / Alternative Scale
        // submenus): modeless and always-on-top so the tracks keep reacting
        // while the form is open. ----
        case CtxScaleForm:
            openScaleForm (ctxScaleKind);
            break;
        case CtxOpenFolder:
            openActiveDocFolder();
            break;

        // ---- Legacy Scale menu items (kept for the AI-control path) ----
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

        // ---- FFT Size submenu + Linear/Logarithmic toggle: right-click on
        // the spectrogram's frequency axis. Same actions as Spectral Controls.
        case CtxFftIncrease: stepFftSize (true);  break;
        case CtxFftReduce:   stepFftSize (false); break;
        case CtxFftAuto:
            app.settings.fftAuto = true;
            if (controlsPanel)
            {
                controlsPanel->setFftSizeSelection (SpectralControlsPanel::kFftAutoId);
                controlsPanel->syncAutoControls();
                controlsPanel->resized();   // show the auto parameter rows again
            }
            updateAutoFftLabel ();
            break;
        case CtxAxisLinear:
            if (!app.settings.logFrequency) break;
            app.settings.logFrequency = false;
            if (controlsPanel) controlsPanel->syncLogToggle ();
            break;
        case CtxAxisLog:
            if (app.settings.logFrequency) break;
            app.settings.logFrequency = true;
            if (controlsPanel) controlsPanel->syncLogToggle ();
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
    auto* w = new OwningAlertWindow (title, message, juce::AlertWindow::QuestionIcon, parent);
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

/* Open (or raise) the Scale form. Modeless and always on top on purpose: the
   form stays usable while the playhead moves and while references are clicked
   in the tracks, which is how the old alternative-scale flow was used. */
void MainWindow::openScaleForm (int axisKind)
{
    if (app.doc() == nullptr)
    {
        if (statusBar) statusBar->setModeText ("Open a file to edit its axes.");
        return;
    }

    if (scaleForm == nullptr)
        scaleForm = std::make_unique<ScaleFormWindow> (app);

    scaleForm->setKindForNewAxis ((sc::axes::Kind) std::clamp (axisKind, 0, 2));
    scaleForm->onPickRequest = [this] (int entryIndex, ScaleFormWindow::PickAction action)
    { handleScalePick (entryIndex, action); };
    scaleForm->refreshFromApp();
    scaleForm->setVisible (true);
    scaleForm->toFront (false);
    scaleForm->setAlwaysOnTop (true);
}

/** Reveal the file the active document came from in the file browser. A
   brand-new document has no path yet and says so in the status bar. */
/* Show a file in the OS file browser: the folder opens with the file selected
   where the platform can do it. Opening the parent directory with
   File::startAsProcess left the choice to the shell's association for folders,
   which on this machine meant handing the path back to SpectroCraft itself - a
   second instance instead of a file browser. */
static void revealInFileBrowser (const juce::File& f)
{
    if (sc::alert::isHeadless ()) return;   // never launch a browser from the tests

    const juce::String path = f.getFullPathName();
    if (path.isEmpty()) return;

   #if JUCE_WINDOWS
    // explorer.exe takes /select,<path>; the path is quoted for names with spaces.
    if (f.existsAsFile ())
    {
        juce::File ("explorer.exe").startAsProcess ("/select,\"" + path + "\"");
        return;
    }
   #elif JUCE_MACOS
    if (f.existsAsFile ())
    {
        juce::File ("/usr/bin/open").startAsProcess ("-R \"" + path + "\"");
        return;
    }
   #endif

    f.getParentDirectory().startAsProcess();   // no file to select: just the folder
}

void MainWindow::openActiveDocFolder()
{
    if (FileDoc* d = app.doc(); d != nullptr && ! d->file.getFullPathName().isEmpty())
    {
        revealInFileBrowser (d->file);
        return;
    }
    if (statusBar) statusBar->setModeText ("This document has no file path yet.");
}

/* A Scale form crosshair button. Which arithmetic it asks for depends on the
   axis and on the action: an offset that makes the playhead read a given time,
   an offset + scale that make a selection's two ends read the typed times, or a
   reference point placed by clicking in the track. */
void MainWindow::handleScalePick (int entryIndex, ScaleFormWindow::PickAction action)
{
    auto list = app.axisEntries;
    if (entryIndex < 0 || entryIndex >= (int) list.size()) return;
    sc::axes::Entry& e = list[(size_t) entryIndex];
    const double rate = app.audioEngine().sampleRate();
    const bool clockUnits = (e.kind == sc::axes::Kind::Time && e.units == 0);

    if (action == ScaleFormWindow::PickAction::OffsetFromPlayhead)
    {
        if (! app.audioEngine().isFileLoaded()) return;
        double v = 0.0;
        if (! promptNumberValue (this, "Offset at the Playhead",
                                 "Displayed time at the playhead position\n"
                                 "(h:mm:ss.xxxx, plain seconds, or a sample count):",
                                 sc::axes::formatTime (e, app.playhead, rate), v, clockUnits))
            return;
        e.offset = sc::axes::offsetForDisplayedValue (e, app.playhead, v, rate);
    }
    else if (action == ScaleFormWindow::PickAction::TimeScaleFromSelection)
    {
        const auto sel = app.selection;
        if (sel.getLength() <= 1e-9)
        {
            if (statusBar) statusBar->setModeText (
                "Select a time range first: its start and end set the offset and scale.");
            return;
        }
        double v0 = 0.0, v1 = 0.0;
        if (! promptNumberValue (this, "Offset and Scale from the Selection",
                                 "Displayed time at the selection START:",
                                 sc::axes::formatTime (e, sel.getStart(), rate), v0, clockUnits))
            return;
        if (! promptNumberValue (this, "Offset and Scale from the Selection",
                                 "Displayed time at the selection STOP:",
                                 sc::axes::formatTime (e, sel.getEnd(), rate), v1, clockUnits))
            return;
        const bool scaled = sc::axes::solveTimePoints (e, sel.getStart(), v0,
                                                       sel.getEnd(), v1, rate);
        if (! scaled)
        {
            if (e.units == 2)
            {
                // Sample counts carry no scale factor: only the offset followed.
                if (statusBar) statusBar->setModeText (
                    "Sample units have no scale factor: only the offset was set.");
            }
            else
            {
                if (statusBar) statusBar->setModeText (
                    "Those two points give no usable scale factor - nothing changed.");
                return;
            }
        }
    }
    else
    {
        // Reference: arm the click in the track and wait for handleRefClickPlaced.
        if (! app.audioEngine().isFileLoaded()) return;
        pendingScaleEntry = entryIndex;
        if (e.kind == sc::axes::Kind::Amplitude)
        {
            armRefClick (Application::RefClickTarget::WaveAmplitude);
            if (statusBar) statusBar->setModeText (
                "Click the reference point in the waveform...");
        }
        else
        {
            armRefClick (Application::RefClickTarget::AxisFrequency);
            if (statusBar) statusBar->setModeText (
                "Click the reference point in the spectrogram...");
        }
        return;   // completed by handleRefClickPlaced
    }

    if (! app.setAxisEntries (list, "Scale")) return;
    if (scaleForm) scaleForm->refreshFromApp();
    syncSharedState();
}

/* The scale factor that makes an axis entry show `typedValue` at the point the
   user clicked (`mainValue`, in the axis's own underlying units: normalized
   amplitude, or Hz). Inverting displayValueFrequency / formatAmplitude. */
void MainWindow::applyScaleReference (int entryIndex, double mainValue, double typedValue)
{
    auto list = app.axisEntries;
    if (entryIndex < 0 || entryIndex >= (int) list.size()) return;
    sc::axes::Entry& e = list[(size_t) entryIndex];
    if (std::abs (mainValue) < 1e-9)
    {
        if (statusBar) statusBar->setModeText (
            "That reference sits at zero, which gives no scale factor. Pick another point.");
        return;
    }

    const double nyq = std::max (1.0, app.audioEngine().sampleRate() * 0.5);
    if (e.kind == sc::axes::Kind::Time)
    {
        // A single time point sets the offset, not the scale.
        e.offset = sc::axes::offsetForDisplayedValue (e, app.refClickT, typedValue,
                                                      app.audioEngine().sampleRate());
    }

    const double scale = sc::axes::scaleForReference (e, mainValue, typedValue, nyq);
    if (! std::isfinite (scale) || std::abs (scale) < 1e-12)
    {
        if (statusBar) statusBar->setModeText ("That reference gives an unusable scale.");
        return;
    }
    e.scale = scale;
    if (! app.setAxisEntries (list, "Scale")) return;
    syncSharedState();
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
        case Application::RefClickTarget::WaveAmplitude:
            title   = "Amplitude Scale from a Reference";
            message = "Value at this point of the waveform:";
            initial = juce::String (fHz, 4);   // the clicked amplitude, see below
            break;
        case Application::RefClickTarget::AxisFrequency:
            title   = "Frequency Scale from a Reference";
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

    /* A reference the Scale form asked for: the value typed at the placed point
       is what that axis entry has to show there. */
    if (target == Application::RefClickTarget::WaveAmplitude
        || target == Application::RefClickTarget::AxisFrequency)
    {
        const int entry = pendingScaleEntry;
        pendingScaleEntry = -1;
        if (entry >= 0) applyScaleReference (entry, fHz, v);
        if (scaleForm) scaleForm->refreshFromApp();
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

    auto* w = new OwningAlertWindow ("Edit Alternative Scale Reference",
                                     "Rename the reference, edit its two endpoint values,\n"
                                     "or delete it. The other references are untouched.",
                                     juce::AlertWindow::NoIcon, this);
    const juce::String mainId = isFreq ? "mainHz" : "mainSecs";
    w->addTextEditor ("name", r.name);
    w->addTextEditor (mainId, juce::String (r.refMain, isFreq ? 1 : 4));
    w->addTextEditor ("altValue", juce::String (r.altValue, 3));
    if (auto* e = w->getTextEditor ("name"))
        e->setTooltip ("Name of this scale, shown in the readout.");
    if (auto* e = w->getTextEditor (mainId))
        e->setTooltip (isFreq ? "Reference frequency, in Hz." : "Reference time, in seconds.");
    if (auto* e = w->getTextEditor ("altValue"))
        e->setTooltip (isFreq ? "Value of this scale at the reference frequency. The scale is "
                                "proportional to the frequency."
                              : "Value of this scale at the reference time. The scale is "
                                "proportional to the time.");

    auto* vis = new juce::ToggleButton ();
    vis->setComponentID ("altref-visible");
    vis->setButtonText ("Show in mouse readout");
    vis->setToggleState (r.enabled, juce::dontSendNotification);
    vis->setTooltip ("Show this scale's value next to the mouse position readout.");
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

    // One field per visible "Footer Only" time axis. Duplicate names are
    // skipped (editor ids must stay unique). A caller that filled the derived
    // alternative-reference list directly (without axis entries) still gets its
    // fields: those are encoded with a negative index.
    juce::StringArray used;
    used.add (f.mainId);
    const auto footer = sc::axes::footerOnly (app.axisEntries, sc::axes::Kind::Time);
    if (! footer.empty())
    {
        for (int i = 0; i < (int) app.axisEntries.size(); ++i)
        {
            const auto& e = app.axisEntries[(size_t) i];
            if (e.kind != sc::axes::Kind::Time || e.type != sc::axes::Type::FooterOnly
                || ! e.visible) continue;
            juce::String id = e.name.trim();
            if (id.isEmpty() || used.contains (id)) continue;
            used.add (id);
            f.alts.push_back (std::make_pair (i, id));
        }
        return f;
    }
    for (int i = 0; i < (int) app.altTimeRefs.size(); ++i)
    {
        const auto& r = app.altTimeRefs[(size_t) i];
        if (!r.enabled || std::abs (r.altValue) < 1e-30) continue;
        juce::String id = r.name.trim();
        if (id.isEmpty() || used.contains (id)) continue;
        used.add (id);
        f.alts.push_back (std::make_pair (-1 - i, id));
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

    // Then the alternative fields in field order: an axis entry parses in its
    // own units, a derived reference inverts its scale
    // (alt(x) = x * altValue / refMain  ->  x = v * refMain / altValue).
    for (size_t i = 0; i < fields.alts.size() && i < altTexts.size(); ++i)
    {
        const int idx = fields.alts[(int) i].first;
        if (altTexts[i].trim().isEmpty()) continue;
        if (idx >= 0)
        {
            if (idx >= (int) app.axisEntries.size()) continue;
            double v = 0.0;
            if (sc::axes::parseTime (app.axisEntries[(size_t) idx], altTexts[i], rate, v))
            {
                outSecs = v;
                return true;
            }
            continue;
        }
        const int ri = -1 - idx;
        if (ri >= (int) app.altTimeRefs.size()) continue;
        const auto& r = app.altTimeRefs[(size_t) ri];
        double v = 0.0;
        if (parseGainDbText (altTexts[i], v))
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

    auto* w = new OwningAlertWindow ("Go to Time",
                                     "Type a time in any field and press OK.\n"
                                     "The first non-empty value (top to bottom) is used.",
                                     juce::AlertWindow::QuestionIcon, this);

    // Main-axis field: label + initial text follow the active scale mode.
    w->addTextEditor (fields.mainId,
                      formatDisplayedTime (app.playhead, app.timeAxisMode,
                                           app.timeOffsetSecs, rate));

    if (auto* e = w->getTextEditor (fields.mainId))
        e->setTooltip ("Time to go to, in the time axis's current format.");

    for (const auto& a : fields.alts)
    {
        if (a.first >= 0)
        {
            if (a.first >= (int) app.axisEntries.size()) continue;
            w->addTextEditor (a.second,
                              sc::axes::formatTime (app.axisEntries[(size_t) a.first],
                                                    app.playhead, rate));
            continue;
        }
        const int ri = -1 - a.first;
        if (ri < 0 || ri >= (int) app.altTimeRefs.size()) continue;
        const auto& r = app.altTimeRefs[(size_t) ri];
        w->addTextEditor (a.second,
                          juce::String (Application::altScaleValue (r, app.playhead), 3));
        if (auto* e = w->getTextEditor (a.second))
            e->setTooltip ("Time to go to, as a value of the scale " + r.name + ".");
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
    auto* w = new OwningAlertWindow ("Change Gain",
                                     "Apply gain to the selection (dB):",
                                     juce::AlertWindow::QuestionIcon, this);
    w->addTextEditor ("gainDb", "0.0");
    if (auto* e = w->getTextEditor ("gainDb"))
        e->setTooltip ("Gain in dB: negative values make the selection quieter.");

    // Slider below the text box: -60..+24 dB in 0.1 steps. The text box stays
    // authoritative — OK applies whatever is typed; the slider simply clamps
    // to its range when a larger value is entered.
    auto* slider = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
    slider->setName ("Gain (dB)");
    slider->setRange (-60.0, 24.0, 0.1);
    slider->setValue (0.0, juce::dontSendNotification);
    slider->setSize (280, 30);
    slider->setTooltip ("Drag to set the gain, -60 to +24 dB. A typed value can go beyond "
                        "this range.");

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

void MainWindow::showChangeGainDialog(std::function<void(bool)> onApplied)
{
    if (!app.audioEngine().isFileLoaded() || app.selection.isEmpty())
    { if (onApplied) onApplied (false); return; }

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
    if (result != 1) { if (onApplied) onApplied (false); return; }

    const float db = (float) text.trim().getDoubleValue();
    app.recordActionStep (sc::actionTypes::kChangeGain,
                          sc::makeVarObject ({ { "db", db } }));
    // Parameters are captured by value (here and in the other dialogs): on a
    // streamed file runDestructiveEdit runs the edit on a worker thread after
    // this function has returned.
    runDestructiveEdit ([this, db] (std::string* e)
        { const bool ok = app.changeGain (db, e); if (ok) lastEffectId = ToolsChangeGain; return ok; },
                       [onApplied] (bool ok) { if (onApplied) onApplied (ok); });
}

/** A caption and a text editor laid out side by side on one row, so the
    label sits in front of (left of) its field instead of above it. Global
    scope (not file-local): CatTimeScalePanel in MainWindow.h holds pointers
    to these rows. */
class LabeledField : public juce::Component
{
public:
    LabeledField (const juce::String& labelText, const juce::String& initialText,
                  const juce::String& fieldTooltip)
    {
        label_ = new juce::Label ("", labelText);
        editor_ = new juce::TextEditor();
        editor_->setMultiLine (false);
        editor_->setText (initialText);
        if (!fieldTooltip.isEmpty())
            editor_->setTooltip (fieldTooltip);
        addAndMakeVisible (label_);
        addAndMakeVisible (editor_);
    }

    ~LabeledField() override { deleteAllChildren(); }   // label_ and editor_

    juce::TextEditor* editor() const { return editor_; }

    void resized() override
    {
        label_->setBounds (0, 3, kLabelW, 18);
        editor_->setBounds (kLabelW + 6, 0,
                           std::max (40, getWidth() - kLabelW - 6), 24);
    }

private:
    juce::Label* label_ = nullptr;
    juce::TextEditor* editor_ = nullptr;
    static constexpr int kLabelW = 132;
};

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

    ~RemixGrid() override { deleteAllChildren(); }   // the rows' labels and editors

    int numRows() const { return (int) rows_.size(); }

    void rebuild (int outChans, const std::vector<std::vector<double>>& defaults)
    {
        deleteAllChildren();   // every child is a row label/editor created below
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
                ed->setTooltip ("Weight of input channel " + juce::String (c + 1)
                                + " on output channel " + juce::String (r + 1)
                                + ". Empty means 0; each row is normalised to sum to 1 when applied.");
                addAndMakeVisible (ed);
                row.editors.push_back (ed);
            }
            rows_.push_back (std::move (row));
        }
        setSize (widthFor (inChans_), (int) rows_.size() * kRowH + 8);
        resized();
    }

    /** Replaces the field texts without rebuilding (used by the fill buttons). */
    void setWeights (const std::vector<std::vector<double>>& w)
    {
        for (size_t r = 0; r < rows_.size() && r < w.size(); ++r)
            for (size_t c = 0; c < rows_[r].editors.size() && c < w[r].size(); ++c)
                rows_[r].editors[c]->setText (formatWeightText (w[(size_t) r][(size_t) c]));
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
            grid->rebuild (outChans, sc::defaultRemixWeights (outChans, inChans));
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

// Fixed sizes for the dialog's custom rows. AlertWindow stacks custom
// components by their height at layout time and never resizes them itself.
static constexpr int kInputRowW = 300, kInputRowH = 24;
static constexpr int kFillRowW = 360, kFillRowH = 26;

/** Configuration grid for "Split Spectral Selection": one row per channel -
    a checkbox ("Channel N") selects it as an extraction source and a text
    field holds its destination channel number; preset buttons sit to the side.
    Channel numbers are 1-based in the UI; getPairs() returns 0-based pairs. */
class SplitConfigGrid : public juce::Component
{
public:
    explicit SplitConfigGrid (int nChans)
    {
        nChans_ = std::max (1, nChans);
        for (int i = 0; i < nChans_; ++i)
        {
            const juce::String chName = "Channel " + juce::String (i + 1);
            auto* chk = new juce::TextButton (chName);
            chk->setButtonText (chName);
            chk->setClickingTogglesState (true);
            chk->onClick = [this] { refreshDestStates (); };
            chk->setTooltip ("Extract the spectral selection from this channel.");
            auto* dst = new juce::TextEditor();
            dst->setMultiLine (false);
            dst->setTooltip ("Destination channel number: an existing "
                             "channel mixes in the extracted content; a "
                             "number above the current count creates a new "
                             "channel on first use.");
            addAndMakeVisible (chk);
            addAndMakeVisible (dst);
            checks_.push_back (chk);
            dests_.push_back (dst);
        }

        const char* presetTexts[3] = { "All Tracks", "Last channel into new",
                                       "Second last into last" };
        const char* presetTips[3] = {
            "Extract from every channel, each into a new channel of its own.",
            "Extract from the last channel only, into a new channel.",
            "Extract from the second-last channel and mix it into the last one." };
        for (int i = 0; i < 3; ++i)
        {
            auto* b = new juce::TextButton (presetTexts[i]);
            b->setButtonText (presetTexts[i]);
            const int preset = i;
            b->onClick = [this, preset] { applyPreset (preset); };
            b->setTooltip (presetTips[i]);
            addAndMakeVisible (b);
            presets_.push_back (b);
        }
        if (nChans_ < 2) presets_[2]->setEnabled (false);   // no second-last channel

        setSize (kGridW, std::max ((size_t) nChans_ * kRowH + 8, (size_t) 96));
    }

    ~SplitConfigGrid() override { deleteAllChildren(); }   // checks_, dests_, presets_

    /** Prefills one row: checked state and destination text (1-based). */
    void setRow (int idx0, bool checked, int destOneBased)
    {
        if (idx0 < 0 || idx0 >= nChans_) return;
        checks_[(size_t) idx0]->setToggleState (checked, juce::dontSendNotification);
        dests_[(size_t) idx0]->setText (destOneBased > 0 ? juce::String (destOneBased)
                                                         : juce::String ());
        refreshDestStates ();
    }

    void applyPresetAllTracks()       { applyPreset (0); }
    void applyPresetLastIntoNew()     { applyPreset (1); }
    void applyPresetSecondLastToLast(){ applyPreset (2); }

    /** Pairs for checked rows with a valid destination, in row order.
        Returned as 0-based (input, destination) indices. */
    std::vector<std::pair<int,int>> getPairs() const
    {
        std::vector<std::pair<int,int>> out;
        for (int i = 0; i < nChans_; ++i)
        {
            if (!checks_[(size_t) i]->getToggleState()) continue;
            const int d1 = dests_[(size_t) i]->getText().trim().getIntValue();
            if (d1 >= 1 && d1 <= kMaxDest) out.push_back ({ i, d1 - 1 });
        }
        return out;
    }

    void resized() override
    {
        for (int i = 0; i < nChans_; ++i)
        {
            const int y = 4 + i * kRowH;
            checks_[(size_t) i]->setBounds (kCheckX, y, kCheckW, 24);
            dests_[(size_t) i]->setBounds (kDestX, y + 1, kDestW, 22);
        }
        for (int i = 0; i < 3 && i < (int) presets_.size (); ++i)
            presets_[(size_t) i]->setBounds (kPresetX, 4 + i * 30, kPresetW, 26);
    }

private:
    void applyPreset (int which)
    {
        const int N = nChans_;
        for (int i = 0; i < N; ++i)
        {
            bool checked = false;
            int dest1 = 0;
            switch (which)
            {
                case 0:   // All Tracks: every channel -> its own new track.
                    checked = true;
                    dest1 = N + 1 + i;
                    break;
                case 1:   // Last channel into new: only the last, to N+1.
                    if (i == N - 1) { checked = true; dest1 = N + 1; }
                    break;
                default:  // Second last into last: only N-1 -> N.
                    if (N >= 2 && i == N - 2) { checked = true; dest1 = N; }
                    break;
            }
            checks_[(size_t) i]->setToggleState (checked, juce::dontSendNotification);
            dests_[(size_t) i]->setText (dest1 > 0 ? juce::String (dest1)
                                                  : juce::String ());
        }
        refreshDestStates ();
    }

    void refreshDestStates()
    {
        for (int i = 0; i < nChans_; ++i)
            dests_[(size_t) i]->setEnabled (checks_[(size_t) i]->getToggleState());
    }

    int nChans_ = 1;
    std::vector<juce::TextButton*> checks_;
    std::vector<juce::TextEditor*> dests_;
    std::vector<juce::TextButton*> presets_;

    static constexpr int kCheckX = 4, kCheckW = 150;
    static constexpr int kDestX = 168, kDestW = 64;
    static constexpr int kPresetX = 248, kPresetW = 150;
    static constexpr int kRowH = 26;
    static constexpr int kGridW = 402;
    static constexpr int kMaxDest = 256;
};

/** One row of buttons that fill the remix matrix with preset patterns. */
class RemixFillButtons : public juce::Component
{
public:
    /** Called on a click with the pattern index (0..4, see sc::RemixFill) and the
        modifier keys held at the moment of the click (only Identity and Merge have variants). */
    std::function<void(int, bool shift, bool ctrl)> onFill;

    RemixFillButtons()
    {
        const char* texts[5] = { "Default", "Identity", "Merge", "Ones", "Zeros" };
        const char* tips[5]  = {
            "The default mix: identity when the channel count is unchanged, otherwise each output averages all inputs.",
            "Put 1 on the diagonal and 0 elsewhere - output N takes input N directly (extra outputs stay silent). Shift+click puts ones along the rectangle's diagonal (one evenly spaced input per output); Ctrl+click mirrors the pattern left-right.",
            "Split every input equally onto every output - e.g. mono becomes identical stereo channels. Shift+click merges by channel groups instead: each output averages its own contiguous slice of the inputs (pairs when the count is halved).",
            "Put 1 in every cell; rows are normalised to sum to 1 when applied, so all inputs mix equally into every output.",
            "Put 0 everywhere - every output channel will be silent." };
        for (int i = 0; i < 5; ++i)
        {
            auto* b = new juce::TextButton (texts[i], tips[i]);
            const int mode = i;
            b->onClick = [this, mode]
            {
                if (onFill == nullptr) return;
                // Modifier keys held at the moment of the click (Shift/Ctrl variants).
                const auto mods = juce::ModifierKeys::getCurrentModifiers ();
                onFill (mode, mods.isShiftDown (),
                        mods.isCtrlDown () || mods.isCommandDown ());
            };
            addAndMakeVisible (b);
            buttons_.push_back (b);
        }
    }

    // The buttons are raw children: juce::Component never deletes them.
    ~RemixFillButtons() override
    {
        for (auto* b : buttons_) delete b;
    }

    void resized() override
    {
        const int n = (int) buttons_.size();
        if (n == 0) return;
        const int bw = getWidth() / n;
        for (int i = 0; i < n; ++i)
            buttons_[i]->setBounds (i * bw, 0, bw - 4, 26);
    }

private:
    std::vector<juce::TextButton*> buttons_;
};

/** Finds a named TextEditor anywhere under root (the resample dialog nests
    its fields inside LabeledField rows). */
juce::TextEditor* findNamedEditor (juce::Component& root, const juce::String& name)
{
    for (auto* c : root.getChildren())
    {
        if (auto* ed = dynamic_cast<juce::TextEditor*> (c))
            if (ed->getName() == name) return ed;
        if (auto* e2 = findNamedEditor (*c, name)) return e2;
    }
    return nullptr;
}
} // namespace

// Effects > Generate Silence / Noise / Tone — each opens a small dialog. With
// an active spectral selection only that region is replaced; with a time
// selection it is replaced in full; otherwise the dialog asks for a duration
// and inserts at the playhead.
namespace {

juce::String genTargetMessage (Application& app, const juce::String& what)
{
    if (app.hasSpectralSelection ())
        return "Replaces the content inside the spectral selection with generated " + what + ".";
    if (!app.selection.isEmpty())
        return "Replaces the selected " + juce::String (app.selection.getLength (), 3)
               + "s with generated " + what + ".";
    return "Inserts generated " + what + " at the playhead position.";
}

} // namespace

/** Returns nullptr when a target already exists (time or spectral selection):
    silence has nothing to configure, so the edit runs without any popup. */
juce::AlertWindow* MainWindow::createGenerateSilenceDialog()
{
    if (app.hasSpectralSelection () || !app.selection.isEmpty ())
        return nullptr;

    auto* w = new OwningAlertWindow ("Generate Silence", genTargetMessage (app, "silence"),
                                     juce::AlertWindow::QuestionIcon, this);

    auto* durRow = new LabeledField ("Duration:", "1.0",
        "Length of the inserted silence in seconds.");
    durRow->editor()->setName ("genDuration");
    durRow->setSize (kInputRowW, kInputRowH);
    w->addCustomComponent (durRow);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    if (auto* editor = findNamedEditor (*w, "genDuration"))
    {
        editor->setWantsKeyboardFocus (true);
        editor->selectAll();
    }
    return w;
}

void MainWindow::showGenerateSilenceDialog(std::function<void(bool)> onApplied)
{
    if (!app.audioEngine().isFileLoaded())
    { if (onApplied) onApplied (false); return; }

    auto* w = createGenerateSilenceDialog();
    double dur = 1.0;
    if (w != nullptr)
    {
        w->enterModalState (false);
        if (auto* editor = findNamedEditor (*w, "genDuration"))
            editor->grabKeyboardFocus();

        const int result = w->runModalLoop();
        if (auto* editor = findNamedEditor (*w, "genDuration"))
            dur = editor->getText().trim().getDoubleValue ();
        delete w;
        if (result != 1) { if (onApplied) onApplied (false); return; }
    }

    app.recordActionStep (sc::actionTypes::kGenSilence,
                          sc::makeVarObject ({ { "dur", dur } }));
    runDestructiveEdit ([this, dur] (std::string* e) { return app.generateSilence (dur, e); },
                       [onApplied] (bool ok) { if (onApplied) onApplied (ok); });
}

/** RMS of the selection, or of ±0.5 s around the playhead when nothing is
    selected; -1 when there is no data at all. */
static double generatorTargetRms (Application& app)
{
    auto& eng = app.audioEngine ();
    if (!eng.isFileLoaded ()) return -1.0;
    const double rate = std::max (1.0, eng.sampleRate ());
    const int64_t total = eng.numSamples ();

    juce::Range<int64_t> range;
    if (!app.selection.isEmpty ())
        range = { std::clamp ((int64_t) std::llround (app.selection.getStart () * rate), (int64_t) 0, total),
                  std::clamp ((int64_t) std::llround (app.selection.getEnd ()   * rate), (int64_t) 0, total) };
    else
    {
        const int64_t ph = std::clamp ((int64_t) (app.playhead * rate), (int64_t) 0, total);
        range = { std::max<int64_t> (0, ph - (int64_t) (rate * 0.5)),
                  std::min<int64_t> (total, ph + (int64_t) (rate * 0.5)) };
    }
    if (range.getLength () <= 0) return -1.0;

    double sumSq = 0.0;
    int64_t count = 0;
    std::vector<float> buf ((size_t) range.getLength ());
    for (int ch = 0; ch < eng.numChannels (); ++ch)
    {
        if (!eng.readSamples (ch, range.getStart (), (int) range.getLength (), buf.data ())) continue;
        for (float v : buf) sumSq += (double) v * (double) v;
        count += range.getLength ();
    }
    return count > 0 ? std::sqrt (sumSq / (double) count) : -1.0;
}

juce::AlertWindow* MainWindow::createGenerateNoiseDialog()
{
    auto* w = new OwningAlertWindow ("Generate Noise", genTargetMessage (app, "noise"),
                                     juce::AlertWindow::QuestionIcon, this);

    auto* typeCombo = new juce::ComboBox();
    typeCombo->setName ("Type");
    typeCombo->addItem ("White noise", 1);
    typeCombo->addItem ("Pink noise", 2);
    typeCombo->addItem ("Brown noise", 3);
    typeCombo->setSelectedId (1, juce::dontSendNotification);   // default: white
    typeCombo->setSize (200, 28);
    typeCombo->setTooltip ("White: flat spectrum. Pink: -3 dB per octave. Brown: -6 dB per "
                           "octave (deeper rumble).");
    w->addCustomComponent (typeCombo);

    // Intensity slider + Average Level button in one row.
    auto* noiseRow = new OwningRow ();
    noiseRow->setSize (340, 30);
    auto* intensity = new juce::Slider (juce::Slider::LinearHorizontal,
                                        juce::Slider::TextBoxLeft);
    intensity->setName ("Intensity");
    intensity->setRange (0.0, 1.0, 0.01);
    intensity->setValue (0.25, juce::dontSendNotification);
    intensity->setBounds (0, 0, 214, 30);
    intensity->setTooltip ("Peak amplitude of the noise, 0 to 1 (1 = full scale).");
    auto* avgBtn = new juce::TextButton ("Average Level");
    avgBtn->setBounds (220, 1, 118, 26);
    avgBtn->setTooltip ("Set the intensity so the generated noise matches the "
                        "average level of the selection (or around the playhead).");
    noiseRow->addAndMakeVisible (intensity);
    noiseRow->addAndMakeVisible (avgBtn);

    // Average Level: scale a second of unit-intensity noise of the selected
    // type to the target RMS, then read off the matching intensity.
    avgBtn->onClick = [this, typeCombo, intensity]
    {
        const double targetRms = generatorTargetRms (app);
        if (!(targetRms >= 0.0)) return;

        const int typeId = typeCombo->getSelectedId ();
        const sc::NoiseType type = typeId == 2 ? sc::NoiseType::Pink
                              : (typeId == 3 ? sc::NoiseType::Brown : sc::NoiseType::White);
        std::vector<std::vector<float>> unit;
        sc::generateNoise (type, 1.0f, 1,
                           std::max<int64_t> (1, (int64_t) app.audioEngine().sampleRate ()),
                           0, unit);
        double uSq = 0.0;
        for (float v : unit[0]) uSq += (double) v * (double) v;
        const double unitRms = std::sqrt (uSq / (double) std::max<size_t> (1, unit[0].size ()));
        if (!(unitRms > 1e-9)) return;

        intensity->setValue (jlimit (0.0, 1.0, targetRms / unitRms));
    };
    w->addCustomComponent (noiseRow);

    if (!app.hasSpectralSelection () && app.selection.isEmpty ())
    {
        auto* durRow = new LabeledField ("Duration:", "1.0",
            "Length of the inserted noise in seconds.");
        durRow->editor()->setName ("genDuration");
        durRow->setSize (kInputRowW, kInputRowH);
        w->addCustomComponent (durRow);
    }

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    if (auto* editor = findNamedEditor (*w, "genDuration"))
    {
        editor->setWantsKeyboardFocus (true);
        editor->selectAll();
    }
    return w;
}

void MainWindow::showGenerateNoiseDialog(std::function<void(bool)> onApplied)
{
    if (!app.audioEngine().isFileLoaded())
    { if (onApplied) onApplied (false); return; }

    auto* w = createGenerateNoiseDialog();
    w->enterModalState (false);
    if (auto* editor = findNamedEditor (*w, "genDuration"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop();
    double dur = 1.0;
    if (auto* editor = findNamedEditor (*w, "genDuration"))
        dur = editor->getText().trim().getDoubleValue ();

    // The slider is nested in its row container, so search recursively.
    std::function<juce::Slider* (juce::Component&)> findSlider =
        [&] (juce::Component& c) -> juce::Slider*
    {
        for (int i = 0; i < c.getNumChildComponents (); ++i)
        {
            auto* ch = c.getChildComponent (i);
            if (auto* s = dynamic_cast<juce::Slider*> (ch)) return s;
            if (auto* s2 = findSlider (*ch)) return s2;
        }
        return nullptr;
    };

    int typeId = 1;
    float intensity = 0.25f;
    for (int i = 0; i < w->getNumChildComponents (); ++i)
    {
        auto* c = w->getChildComponent (i);
        if (auto* combo = dynamic_cast<juce::ComboBox*> (c))
            typeId = combo->getSelectedId ();
    }
    if (auto* slider = findSlider (*w))
        intensity = (float) slider->getValue ();
    delete w;
    if (result != 1) { if (onApplied) onApplied (false); return; }

    const sc::NoiseType type = typeId == 2 ? sc::NoiseType::Pink
                              : (typeId == 3 ? sc::NoiseType::Brown : sc::NoiseType::White);
    app.recordActionStep (sc::actionTypes::kGenNoise,
        sc::makeVarObject ({
            { "type", typeId == 2 ? "pink" : (typeId == 3 ? "brown" : "white") },
            { "intensity", intensity },
            { "dur", dur } }));
    runDestructiveEdit ([this, type, intensity, dur] (std::string* e) { return app.generateNoise (type, intensity, dur, e); },
                       [onApplied] (bool ok) { if (onApplied) onApplied (ok); });
}

juce::AlertWindow* MainWindow::createGenerateToneDialog()
{
    auto* w = new OwningAlertWindow ("Generate Tone", genTargetMessage (app, "tone"),
                                     juce::AlertWindow::QuestionIcon, this);

    auto* waveCombo = new juce::ComboBox();
    waveCombo->setName ("Waveform");
    waveCombo->addItem ("Sine", 1);
    waveCombo->addItem ("Square", 2);
    waveCombo->addItem ("Sawtooth", 3);
    waveCombo->addItem ("Triangle", 4);
    waveCombo->setSelectedId (1, juce::dontSendNotification);   // default: sine
    waveCombo->setSize (200, 28);
    waveCombo->setTooltip ("Wave shape used for every tone below.");
    w->addCustomComponent (waveCombo);

    auto* tones = new ToneListEditor();
    tones->setName ("Tones");
    tones->addRow (440.0, 0.5f);
    w->addCustomComponent (tones);

    if (!app.hasSpectralSelection () && app.selection.isEmpty ())
    {
        auto* durRow = new LabeledField ("Duration:", "1.0",
            "Length of the inserted tone in seconds.");
        durRow->editor()->setName ("genDuration");
        durRow->setSize (kInputRowW, kInputRowH);
        w->addCustomComponent (durRow);
    }

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // Focus the first tone's frequency field so a new value can be typed right
    // away; fall back to the duration field when there are no rows.
    ToneListEditor* tonesPtr = nullptr;
    for (int i = 0; i < w->getNumChildComponents (); ++i)
        if (auto* tl = dynamic_cast<ToneListEditor*> (w->getChildComponent (i)))
            tonesPtr = tl;
    juce::TextEditor* focusTarget = nullptr;
    if (tonesPtr != nullptr && tonesPtr->firstFreqEditor() != nullptr)
        focusTarget = tonesPtr->firstFreqEditor();
    else
        focusTarget = findNamedEditor (*w, "genDuration");
    if (focusTarget != nullptr)
    {
        focusTarget->setWantsKeyboardFocus (true);
        focusTarget->selectAll();
    }
    return w;
}

void MainWindow::showGenerateToneDialog(std::function<void(bool)> onApplied)
{
    if (!app.audioEngine().isFileLoaded())
    { if (onApplied) onApplied (false); return; }

    auto* w = createGenerateToneDialog();
    w->enterModalState (false);

    ToneListEditor* tonesPtr = nullptr;
    for (int i = 0; i < w->getNumChildComponents (); ++i)
        if (auto* tl = dynamic_cast<ToneListEditor*> (w->getChildComponent (i)))
            tonesPtr = tl;
    if (tonesPtr != nullptr && tonesPtr->firstFreqEditor() != nullptr)
        tonesPtr->firstFreqEditor()->grabKeyboardFocus();
    else if (auto* editor = findNamedEditor (*w, "genDuration"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop();
    double dur = 1.0;
    if (auto* editor = findNamedEditor (*w, "genDuration"))
        dur = editor->getText().trim().getDoubleValue ();

    std::vector<sc::ToneSpec> tones;
    int waveId = 1;
    for (int i = 0; i < w->getNumChildComponents (); ++i)
    {
        auto* c = w->getChildComponent (i);
        if (auto* combo = dynamic_cast<juce::ComboBox*> (c))
            waveId = combo->getSelectedId ();
        else if (auto* tl = dynamic_cast<ToneListEditor*> (c))
        {
            const auto pairs = tl->getTones ();
            tones.reserve (pairs.size());
            for (const auto& p : pairs)
                tones.push_back ({ p.first, (float) p.second });
        }
    }
    delete w;
    if (result != 1) { if (onApplied) onApplied (false); return; }

    if (tones.empty())
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Invalid tone list",
                                            "Please enter at least one tone with a positive frequency.",
                                            "", this);
        if (onApplied) onApplied (false);
        return;
    }

    const sc::WaveShape shape = waveId == 2 ? sc::WaveShape::Square
                              : (waveId == 3 ? sc::WaveShape::Sawtooth
                              : (waveId == 4 ? sc::WaveShape::Triangle : sc::WaveShape::Sine));
    juce::Array<juce::var> toneArr;
    for (const auto& t : tones)
    {
        juce::Array<juce::var> pair;
        pair.add ((double) t.freqHz);
        pair.add ((double) t.amplitude);
        toneArr.add (pair);
    }
    app.recordActionStep (sc::actionTypes::kGenTone,
        sc::makeVarObject ({
            { "tones", toneArr },
            { "shape", waveId == 2 ? "square"
                     : (waveId == 3 ? "sawtooth"
                     : (waveId == 4 ? "triangle" : "sine")) },
            { "dur", dur } }));
    runDestructiveEdit ([this, tones, shape, dur] (std::string* e) { return app.generateTone (tones, shape, dur, e); },
                       [onApplied] (bool ok) { if (onApplied) onApplied (ok); });
}

// Effects > Time Stretch... — Audacity's Change Tempo: a pitch-preserving
// tempo change (SoundTouch WSOLA) that changes the file length. Positive
// percent = faster/shorter, negative = slower/longer.
/** The length the stretch is expressed against: the selection when there is
    one, else the whole file (in seconds). */
static double stretchReferenceDuration (Application& app)
{
    auto& eng = app.audioEngine ();
    const double rate = std::max (1.0, eng.sampleRate ());
    if (!app.selection.isEmpty ())
        return app.selection.getLength ();
    return (double) eng.numSamples () / rate;
}

juce::AlertWindow* MainWindow::createTimeStretchDialog()
{
    auto& eng = app.audioEngine();
    const double rate = std::max (1.0, eng.sampleRate());

    juce::String msg = "Change the tempo of ";
    if (!app.selection.isEmpty())
        msg += juce::String (app.selection.getLength (), 3) + "s selection";
    else
        msg += "the whole file (" + juce::String (eng.numSamples () / rate, 3) + "s).";
    msg += "\nThe pitch is preserved. The two fields are linked: a speed factor of 2 makes the audio twice as fast (half the duration), 0.5 twice as slow.";

    auto* w = new OwningAlertWindow ("Time Stretch", msg,
                                     juce::AlertWindow::QuestionIcon, this);

    const double refDur = std::max (1e-9, stretchReferenceDuration (app));
    auto* durRow = new LabeledField ("Time Duration:", formatWeightText (refDur),
        "New length of the stretched audio in seconds.");
    durRow->editor()->setName ("stretchDuration");
    auto* facRow = new LabeledField ("Speed Factor:", "1",
        "How much faster (> 1) or slower (< 1) the audio plays back.");
    facRow->editor()->setName ("stretchFactor");

    // Keep the two fields in sync: duration d <-> factor refDur / d. Editing
    // one rewrites the other; invalid input leaves it untouched.
    durRow->editor()->setEscapeAndReturnKeysConsumed (false);
    facRow->editor()->setEscapeAndReturnKeysConsumed (false);
    durRow->editor()->onTextChange = [durRow, facRow, refDur]
    {
        double d;
        if (parseGainDbText (durRow->editor()->getText (), d) && d > 0.0)
            facRow->editor()->setText (formatWeightText (refDur / d));
    };
    facRow->editor()->onTextChange = [durRow, facRow, refDur]
    {
        double f;
        if (parseGainDbText (facRow->editor()->getText (), f) && f > 0.0)
            durRow->editor()->setText (formatWeightText (refDur / f));
    };

    durRow->setSize (kInputRowW, kInputRowH);
    facRow->setSize (kInputRowW, kInputRowH);
    w->addCustomComponent (durRow);
    w->addCustomComponent (facRow);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    durRow->editor()->setWantsKeyboardFocus (true);
    durRow->editor()->selectAll();
    return w;
}

void MainWindow::showTimeStretchDialog(std::function<void(bool)> onApplied)
{
    if (!app.audioEngine().isFileLoaded())
    { if (onApplied) onApplied (false); return; }

    auto* w = createTimeStretchDialog ();

    // Enter modal state WITHOUT taking keyboard focus (see showChangeGainDialog).
    w->enterModalState (false);
    if (auto* editor = findNamedEditor (*w, "stretchDuration"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop ();

    // The factor field is authoritative; fall back to the duration when it was
    // left invalid (the two are kept in sync while typing).
    double factor = -1.0;
    if (auto* ed = findNamedEditor (*w, "stretchFactor"))
        parseGainDbText (ed->getText (), factor);
    if (!(factor > 0.0))
    {
        double d = -1.0;
        if (auto* ed = findNamedEditor (*w, "stretchDuration"))
            parseGainDbText (ed->getText (), d);
        const double refDur = std::max (1e-9, stretchReferenceDuration (app));
        if (d > 0.0) factor = refDur / d;
    }
    delete w;
    if (result != 1 || !(factor > 0.0)) { if (onApplied) onApplied (false); return; }   // invalid: ignore

    const float pct = (float) jlimit (-95.0f, 3000.0f, (float) (100.0 * (factor - 1.0)));
    app.recordActionStep (sc::actionTypes::kTimeStretch,
                          sc::makeVarObject ({ { "pct", pct } }));
    runDestructiveEdit ([this, pct] (std::string* e)
        { const bool ok = app.timeStretch (pct, e);
          if (ok) lastEffectId = ToolsTimeStretch;
          return ok; },
                       [onApplied] (bool ok) { if (onApplied) onApplied (ok); });
}

// ---------------------------------------------------------------------------
// Content Aware Time Scale (Effects menu, next to Time Stretch)
// ---------------------------------------------------------------------------

CatTimeScalePanel::CatTimeScalePanel (double refDurationSecs)
    : refDur_ (std::max (1e-9, refDurationSecs))
{
    // An explicit size is required: AlertWindow sizes the dialog from
    // getHeight() and JUCE clips children to their parent's bounds, so a
    // 0x0 panel would be invisible even though its rows exist.
    setSize (360, 115);

    modeLabel_ = new juce::Label ("", "Mode:");
    modeCombo_ = new juce::ComboBox ("catMode");
    modeCombo_->addItem ("Time Stretch", 1);
    modeCombo_->addItem ("Reduce Gaps", 2);
    modeCombo_->addItem ("Export Variation", 3);
    modeCombo_->setTooltip ("Time Stretch: change the length by cutting or repeating stable "
                            "segments. Reduce Gaps: remove the most stable gaps. Export "
                            "Variation: write the spectral change of each gap to a new table.");

    durRow_ = new LabeledField ("Time Duration:", formatWeightText (refDur_),
        "New length of the audio in seconds.");
    durRow_->editor()->setName ("catDuration");
    facRow_ = new LabeledField ("Speed Factor:", "1.0",
        "> 1 shortens (faster), < 1 lengthens (slower). The two fields are linked.");
    facRow_->editor()->setName ("catFactor");
    pctRow_ = new LabeledField ("Percentile:", "40",
        "Remove every gap whose spectral change is below this percentile (0-100) of all gaps in the range.");
    pctRow_->editor()->setName ("catPercentile");
    markOnly_ = new juce::TextButton ("Mark only");
    markOnly_->setClickingTogglesState (true);
    markOnly_->setTooltip ("Add a region marker at every removable gap instead of deleting the audio.");
    resultLabel_ = new juce::Label ("", "Analysing ...");

    addAndMakeVisible (modeLabel_);
    addAndMakeVisible (modeCombo_);
    addAndMakeVisible (durRow_);
    addAndMakeVisible (facRow_);
    addAndMakeVisible (pctRow_);
    addAndMakeVisible (markOnly_);
    addAndMakeVisible (resultLabel_);

    modeLabel_->setBounds (0, 5, 132, 18);
    modeCombo_->setBounds (138, 0, getWidth() - 138, 28);
    durRow_->setBounds (0, 34, kInputRowW, kInputRowH);
    facRow_->setBounds (0, 62, kInputRowW, kInputRowH);
    pctRow_->setBounds (0, 34, kInputRowW, kInputRowH);   // same slot as the duration row
    markOnly_->setBounds (0, 62, 190, kInputRowH);        // same slot as the factor row
    resultLabel_->setBounds (0, 92, kInputRowW, 18);

    modeCombo_->setSelectedItemIndex (0, juce::dontSendNotification);
    modeCombo_->onChange = [this]
    {
        // getSelectedItemIndex() is 0-based: 0=Time Stretch, 1=Reduce Gaps,
        // 2=Export Variation (the item IDs are 1/2/3).
        const int i = modeCombo_->getSelectedItemIndex ();
        setMode (i == 1 ? Mode::ReduceGaps : i == 2 ? Mode::ExportVariation : Mode::TimeStretch);
    };
    durRow_->editor()->onTextChange = [this] { syncFromDuration (); };
    facRow_->editor()->onTextChange = [this] { syncFromFactor (); };
    pctRow_->editor()->onTextChange = [this] { refreshResult (); };

    setMode (Mode::TimeStretch);
}

CatTimeScalePanel::~CatTimeScalePanel()
{
    // Clear alive_ first so a delivery already queued on the message thread is
    // skipped, then wait for the worker: fn() reads the active document's
    // engine and selection, which the caller may edit or close as soon as the
    // dialog is deleted. The wait is bounded by one analysis of the range.
    alive_->store (false);
    if (worker_.joinable ())
        worker_.join ();
    deleteAllChildren ();   // rows, combo, labels created with new
}

void CatTimeScalePanel::startAnalysis (std::function<void(sc::cat::Analysis&, bool&, juce::String&)> fn)
{
    if (worker_.joinable ()) return;   // one analysis per panel

    // The flag is captured by value: the queued delivery may run after this
    // panel was deleted and must not reach it through `this`.
    worker_ = std::thread ([this, fn, alive = alive_]
    {
        sc::cat::Analysis a;
        bool ok = false;
        juce::String err;
        fn (a, ok, err);

        if (!alive->load ()) return;
        juce::MessageManager::callAsync ([this, alive, a = std::move (a), ok, err]
            { if (alive->load ()) setAnalysis (std::move (a), ok, err); });
    });
}

void CatTimeScalePanel::setAnalysis (sc::cat::Analysis a, bool ok, const juce::String& err)
{
    analysis_ = std::move (a);
    analysisOk_ = ok;
    if (!ok)
        resultLabel_->setText (err.isEmpty () ? "Analysis unavailable." : err,
                               juce::dontSendNotification);
    else
        refreshResult ();
}

void CatTimeScalePanel::setMode (Mode m)
{
    mode_ = m;
    const bool reduce = (m == Mode::ReduceGaps);
    const bool exportVar = (m == Mode::ExportVariation);
    durRow_->setVisible (!reduce && !exportVar);
    facRow_->setVisible (!reduce && !exportVar);
    pctRow_->setVisible (reduce);
    markOnly_->setVisible (reduce);   // "mark instead of delete" only applies to gap removals
    refreshResult ();
}

void CatTimeScalePanel::syncFromDuration ()
{
    double d;
    if (parseGainDbText (durRow_->editor()->getText (), d) && d > 0.0)
        facRow_->editor()->setText (formatWeightText (refDur_ / d));
}

void CatTimeScalePanel::syncFromFactor ()
{
    double f;
    if (parseGainDbText (facRow_->editor()->getText (), f) && f > 0.0)
        durRow_->editor()->setText (formatWeightText (refDur_ / f));
}

void CatTimeScalePanel::refreshResult ()
{
    if (!analysisOk_)
        return;   // keep the "Analysing ..." / error text as-is

    if (mode_ == Mode::ExportVariation)
    {
        resultLabel_->setText ("Will write " + juce::String (analysis_.numGaps ())
                               + " variation samples to a new table.",
                               juce::dontSendNotification);
        return;
    }

    if (mode_ != Mode::ReduceGaps)
    {
        double d = refDur_;
        parseGainDbText (durRow_->editor()->getText (), d);
        resultLabel_->setText ("Target duration: " + formatWeightText (std::max (1e-9, d)) + " s",
                               juce::dontSendNotification);
        return;
    }

    double p = 40.0;
    parseGainDbText (pctRow_->editor()->getText (), p);
    const int64_t removed = sc::cat::removableSamplesAtPercentile (analysis_, p, 3);
    const double newDur = std::max (0.0, refDur_ - (double) removed / analysis_.rate);
    resultLabel_->setText ("Resulting duration: " + formatWeightText (newDur)
                           + " s  (removes " + formatWeightText ((double) removed / analysis_.rate) + " s)",
                           juce::dontSendNotification);
}

double CatTimeScalePanel::getSpeedFactor () const
{
    double f = -1.0;
    if (parseGainDbText (facRow_->editor()->getText (), f) && f > 0.0)
        return f;
    double d = -1.0;
    if (parseGainDbText (durRow_->editor()->getText (), d) && d > 0.0)
        return refDur_ / d;
    return 1.0;   // invalid input: no change
}

double CatTimeScalePanel::getPercentile () const
{
    double p = -1.0;
    if (parseGainDbText (pctRow_->editor()->getText (), p))
        return jlimit (0.0, 100.0, p);
    return 40.0;   // invalid input: default
}

bool CatTimeScalePanel::durationRowVisible()   const { return durRow_ != nullptr && durRow_->isVisible(); }
bool CatTimeScalePanel::factorRowVisible()     const { return facRow_ != nullptr && facRow_->isVisible(); }
bool CatTimeScalePanel::percentileRowVisible() const { return pctRow_ != nullptr && pctRow_->isVisible(); }
bool CatTimeScalePanel::markOnlyVisible()      const { return markOnly_ != nullptr && markOnly_->isVisible(); }

juce::AlertWindow* MainWindow::createContentAwareTimeScaleDialog()
{
    auto& eng = app.audioEngine();
    const double rate = std::max (1.0, eng.sampleRate());

    juce::String msg = "Content-aware time scaling of ";
    if (!app.selection.isEmpty())
        msg += juce::String (app.selection.getLength (), 3) + "s selection";
    else
        msg += "the whole file (" + juce::String (eng.numSamples () / rate, 3) + "s).";
    msg += "\nThe audio is analysed with an STFT: the most stable gaps (least change between consecutive frames) are removed or duplicated in the time domain. Each cut is joined with a 10 ms equal-power crossfade placed where the audio resembles its surroundings, and onsets and transients are protected.";

    auto* w = new OwningAlertWindow ("Content Aware Time Scale", msg,
                                     juce::AlertWindow::QuestionIcon, this);

    const double refDur = std::max (1e-9, stretchReferenceDuration (app));
    auto* panel = new CatTimeScalePanel (refDur);
    w->addCustomComponent (panel);

    // Live preview: analyse the active range off the UI thread.
    panel->startAnalysis ([this] (sc::cat::Analysis& a, bool& ok, juce::String& err)
        {
            if (!app.catAnalyzeActiveRange (a))
                { ok = false; err = "Analysis unavailable for this range."; return; }
            ok = true;
        });

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    return w;
}

void MainWindow::showContentAwareTimeScaleDialog(std::function<void(bool)> onApplied)
{
    if (!app.audioEngine().isFileLoaded())
    { if (onApplied) onApplied (false); return; }

    auto* w = createContentAwareTimeScaleDialog ();

    // Enter modal state WITHOUT taking keyboard focus (see showChangeGainDialog).
    w->enterModalState (false);
    if (auto* ed = findNamedEditor (*w, "catDuration"))
        ed->grabKeyboardFocus();

    const int result = w->runModalLoop ();

    bool reduce = false, markOnly = false, exportVar = false;
    double factor = 1.0, pct = 40.0;
    if (auto* p = dynamic_cast<CatTimeScalePanel*> (w->getCustomComponent (0)))
    {
        reduce = (p->getMode() == CatTimeScalePanel::Mode::ReduceGaps);
        factor = p->getSpeedFactor ();
        pct = p->getPercentile ();
        markOnly = p->markOnly ();
        exportVar = p->exportVariation ();
    }
    delete w;
    if (result != 1) { if (onApplied) onApplied (false); return; }   // cancelled

    if (exportVar)
    {
        // Write the per-gap amount of spectral variation to a new table
        // parameter. No audio is modified and, like mark-only, no action step
        // is recorded.
        std::string err;
        if (app.contentAwareExportVariation (&err))
            if (onApplied) onApplied (true);
        else
            showEditError (err);
        return;
    }

    if (reduce && markOnly)
    {
        // Mark the removable gaps with region markers instead of deleting audio.
        // No action step is recorded: replaying a CAT step would delete, and
        // like manual marker insertion this leaves no undo entry either.
        std::string err;
        const int added = app.contentAwareMarkRemovals (pct, &err);
        if (added >= 0)
        {
            if (markersPanel != nullptr) markersPanel->refresh ();
            syncSharedState ();
            if (onApplied) onApplied (true);
        }
        else
            showEditError (err);
        return;
    }

    app.recordActionStep (sc::actionTypes::kCatTimeScale,
        sc::makeVarObject ({
            { "reduceGaps", reduce },
            { "percentile", pct },
            { "speedFactor", factor } }));
    runDestructiveEdit ([this, reduce, pct, factor] (std::string* e)
        { const bool ok = app.contentAwareTimeScale (reduce, pct, factor, e);
          if (ok) lastEffectId = ToolsCatTimeScale;
          return ok; },
                       [onApplied] (bool ok) { if (onApplied) onApplied (ok); });
}

// Effects > Filter EQ... — Audacity's classic Filter Curve: an FFT-based
// zero-phase equaliser with an interactive control-point curve, the ten
// built-in presets from Audacity's default curve set plus user-defined
// presets (Add/Delete), and a remembered last-used curve.
juce::AlertWindow* MainWindow::createFilterEqDialog()
{
    auto* w = new OwningAlertWindow ("Filter EQ",
        "Drag points to shape the curve; click empty space to add a point, "
        "double-click a point to type its exact coordinates and right-click it "
        "to remove it. The response passes exactly through every control "
        "point and stays flat before the first and after the last one.",
        juce::AlertWindow::QuestionIcon, this);

    auto* editor = new EqCurveEditor ();
    const double nyq = app.audioEngine().sampleRate () * 0.5;
    if (nyq > 0.0)
        editor->setFreqHi (nyq);
    w->addCustomComponent (editor);

    // Preset row: the combo plus Add/Delete buttons in one container, which
    // also gives the OK button below a little more breathing room.
    auto* presetRow = new OwningRow ();
    presetRow->setSize (360, 34);
    auto* presetCombo = new juce::ComboBox ();
    presetCombo->setName ("Preset");
    presetCombo->setBounds (0, 0, 218, 26);
    presetCombo->setTooltip ("Load a built-in or saved curve.");
    auto* addBtn = new juce::TextButton ("Add...");
    addBtn->setBounds (224, 0, 62, 26);
    addBtn->setTooltip ("Save the current curve as a named preset.");
    auto* delBtn = new juce::TextButton ("Delete");
    delBtn->setBounds (292, 0, 64, 26);
    delBtn->setTooltip ("Remove the selected user preset (built-ins cannot be deleted).");
    delBtn->setEnabled (false);
    presetRow->addAndMakeVisible (presetCombo);
    presetRow->addAndMakeVisible (addBtn);
    presetRow->addAndMakeVisible (delBtn);

    auto rebuildPresets = [this, presetCombo]
    {
        const int oldId = presetCombo->getSelectedId ();
        const juce::String oldName = oldId > 0 ? presetCombo->getItemText ((int) (oldId - 1)) : juce::String ();
        presetCombo->clear (false);
        const auto& builtins = sc::eq::presets ();
        for (size_t i = 0; i < builtins.size (); ++i)
            presetCombo->addItem (builtins[i].name, (int) i + 1);
        const auto users = sc::eq::parseUserPresets (app.settings.eqUserPresets);
        for (size_t i = 0; i < users.size (); ++i)
            presetCombo->addItem (users[i].name, (int) builtins.size () + (int) i + 1);
        // Restore the previous selection by name when it still exists.
        if (!oldName.isEmpty ())
            for (int i = 0; i < presetCombo->getNumItems (); ++i)
                if (presetCombo->getItemText (i) == oldName)
                    { presetCombo->setSelectedItemIndex (i, juce::dontSendNotification); break; }
    };

    rebuildPresets ();

    auto loadSelectedPreset = [this, editor, presetCombo]
    {
        const int id = presetCombo->getSelectedId ();
        if (id <= 0) return;
        std::vector<sc::eq::EqPoint> pts;
        const auto& builtins = sc::eq::presets ();
        if ((size_t) id <= builtins.size ())
            pts = builtins[(size_t) (id - 1)].points;
        else
        {
            const auto users = sc::eq::parseUserPresets (app.settings.eqUserPresets);
            const size_t ui = (size_t) id - builtins.size () - 1;
            if (ui < users.size ()) pts = users[ui].points;
        }
        editor->setPoints (pts);
    };

    presetCombo->onChange = [loadSelectedPreset, presetCombo, delBtn]
    {
        loadSelectedPreset ();
        const auto& builtins = sc::eq::presets ();
        const int id = presetCombo->getSelectedId ();
        delBtn->setEnabled (id > 0 && (size_t) id > builtins.size ());
    };

    addBtn->onClick = [this, editor, presetCombo, rebuildPresets]
    {
        // Prompt for a name; default to the first free "My Preset N".
        const auto existing = sc::eq::parseUserPresets (app.settings.eqUserPresets);
        juce::String defaultName;
        for (int n = 1; n < 1000; ++n)
        {
            defaultName = "My Preset " + juce::String (n);
            bool taken = false;
            for (const auto& p : existing)
                if (p.name == defaultName) { taken = true; break; }
            if (!taken) break;
        };

        auto* dlg = new OwningAlertWindow ("Add Preset",
            "Name for the new preset (the current curve will be saved):",
            juce::AlertWindow::QuestionIcon, this);
        dlg->addTextEditor ("presetName", defaultName);
        dlg->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        dlg->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
        if (auto* ed = dlg->getTextEditor ("presetName"))
            { ed->setWantsKeyboardFocus (true); ed->selectAll(); }

        const int r = dlg->runModalLoop ();
        juce::String name;
        if (auto* ed = dlg->getTextEditor ("presetName"))
            name = ed->getText ().trim ();
        delete dlg;
        if (r != 1) return;

        // Sanitise: one line, no '|' separator character (same rules as
        // sc::eq::serializeUserPresets).
        name = name.replace ("\r\n", " ").replace ("\n", " ")
                   .replace ("\r", " ").replace ("|", " ");
        if (name.isEmpty ()) return;

        auto users = sc::eq::parseUserPresets (app.settings.eqUserPresets);
        bool replaced = false;
        for (auto& p : users)
            if (p.name == name) { p.points = editor->getPoints (); replaced = true; break; }
        if (!replaced) users.push_back ({ name, editor->getPoints () });
        app.settings.eqUserPresets = sc::eq::serializeUserPresets (users);
        app.saveSettings ();

        rebuildPresets ();
        // Select the new preset so it is visible in the combo.
        for (int i = 0; i < presetCombo->getNumItems (); ++i)
            if (presetCombo->getItemText (i) == name)
                { presetCombo->setSelectedItemIndex (i); break; }   // fires onChange
    };

    delBtn->onClick = [this, presetCombo, rebuildPresets]
    {
        const auto& builtins = sc::eq::presets ();
        const int id = presetCombo->getSelectedId ();
        if (id <= 0 || (size_t) id <= builtins.size ()) return;   // built-ins are protected
        auto users = sc::eq::parseUserPresets (app.settings.eqUserPresets);
        const size_t ui = (size_t) id - builtins.size () - 1;
        if (ui >= users.size ()) return;
        users.erase (users.begin () + (long) ui);
        app.settings.eqUserPresets = sc::eq::serializeUserPresets (users);
        app.saveSettings ();

        presetCombo->setSelectedId (0, juce::dontSendNotification);
        rebuildPresets ();
    };

    // Double-click a point: type its exact coordinates.
    editor->onEditPoint = [this, editor] (int idx) { showEqPointDialog (editor, idx); };

    w->addCustomComponent (presetRow);

    // Restore the last used curve when there is one; otherwise start flat.
    const auto lastPts = sc::eq::parsePoints (app.settings.eqLastCurve);
    editor->setPoints (lastPts.empty () ? std::vector<sc::eq::EqPoint> { { 1000.0, 0.0 } }
                                        : lastPts);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    return w;
}

void MainWindow::showEqPointDialog (EqCurveEditor* editor, int idx)
{
    const auto pts = editor->getPoints ();
    if (idx < 0 || idx >= (int) pts.size ()) return;

    auto* w = new OwningAlertWindow ("Edit Point",
        "Type the exact coordinates of this point:",
        juce::AlertWindow::QuestionIcon, this);
    auto* fRow = new LabeledField ("Frequency (Hz):", formatWeightText (pts[idx].freqHz), "");
    fRow->editor()->setName ("ptFreq");
    auto* gRow = new LabeledField ("Gain (dB):", formatWeightText (pts[idx].gainDb), "");
    gRow->editor()->setName ("ptGain");
    fRow->setSize (kInputRowW, kInputRowH);
    gRow->setSize (kInputRowW, kInputRowH);
    w->addCustomComponent (fRow);
    w->addCustomComponent (gRow);

    fRow->editor()->setEscapeAndReturnKeysConsumed (false);
    gRow->editor()->setEscapeAndReturnKeysConsumed (false);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    fRow->editor()->setWantsKeyboardFocus (true);
    fRow->editor()->selectAll();

    w->enterModalState (false);
    fRow->editor()->grabKeyboardFocus();
    const int result = w->runModalLoop ();
    double f = -1.0, g = -1.0;
    if (auto* ed = findNamedEditor (*w, "ptFreq")) parseGainDbText (ed->getText (), f);
    if (auto* ed = findNamedEditor (*w, "ptGain")) parseGainDbText (ed->getText (), g);
    delete w;
    if (result != 1) return;

    // Only the fields that parsed are applied; the other keeps its value.
    const auto cur = editor->getPoints ();
    if (idx < 0 || idx >= (int) cur.size ()) return;
    const sc::eq::EqPoint cp = cur[(size_t) idx];
    editor->setPoint (idx, f > 0.0 ? f : cp.freqHz,
                          std::isfinite (g) ? g : cp.gainDb);
}

void MainWindow::showFilterEqDialog(std::function<void(bool)> onApplied)
{
    if (!app.audioEngine().isFileLoaded())
    { if (onApplied) onApplied (false); return; }

    auto* w = createFilterEqDialog ();

    // Locate the curve editor so its points can be read after the modal loop.
    EqCurveEditor* editor = nullptr;
    for (auto* c : w->getChildren ())
        if (auto* e = dynamic_cast<EqCurveEditor*> (c)) { editor = e; break; }

    w->enterModalState (false);
    const int result = w->runModalLoop ();
    std::vector<sc::eq::EqPoint> pts;
    if (editor != nullptr)
        pts = editor->getPoints ();
    delete w;

    if (result == 1)
        // Remember the curve for next time (a flat one is the default state).
        app.settings.eqLastCurve = sc::eq::isFlatCurve (pts) ? juce::String ()
                                                            : sc::eq::serializePoints (pts);
    if (result != 1 || sc::eq::isFlatCurve (pts))
    { if (onApplied) onApplied (false); return; }   // flat curve: no-op

    app.recordActionStep (sc::actionTypes::kFilterEq,
                          sc::makeVarObject ({ { "points", sc::eq::serializePoints (pts) } }));
    runDestructiveEdit ([this, pts] (std::string* e)
        { const bool ok = app.applyFilterEq (pts, e); if (ok) lastEffectId = ToolsFilterEq; return ok; },
                       [onApplied] (bool ok) { if (onApplied) onApplied (ok); });
}

/** Duration in seconds of the range the envelope will be drawn for:
    the time selection if there is one, else the whole file. */
static double envelopeTargetSeconds (Application& app)
{
    auto& eng = app.audioEngine ();
    const double dur = (double) eng.numSamples () / eng.sampleRate ();
    juce::Range<double> sel { 0.0, dur };
    if (!app.selection.isEmpty ())
        sel = app.selection.getIntersectionWith ({ 0.0, dur });
    return std::max ((double) sel.getLength (), 1e-3);
}

juce::AlertWindow* MainWindow::createEnvelopeDialog()
{
    auto* w = new OwningAlertWindow ("Envelope",
        "Drag points to shape the amplitude over time; click empty space to add a point, "
        "double-click a point to type its exact coordinates and right-click it to remove it. "
        "The curve is applied as a per-sample gain: to the active spectral selection if there "
        "is one, else to the time selection, or over the whole file when nothing is selected.",
        juce::AlertWindow::QuestionIcon, this);

    auto* editor = new EnvCurveEditor ();
    const double targetSecs = envelopeTargetSeconds (app);
    editor->setTimeHi (targetSecs);
    w->addCustomComponent (editor);

    // Preset row: the combo plus Add/Delete buttons in one container, which
    // also gives the OK button below a little more breathing room.
    auto* presetRow = new OwningRow ();
    presetRow->setSize (360, 34);
    auto* presetCombo = new juce::ComboBox ();
    presetCombo->setName ("Preset");
    presetCombo->setBounds (0, 0, 218, 26);
    presetCombo->setTooltip ("Load a built-in or saved curve.");
    auto* addBtn = new juce::TextButton ("Add...");
    addBtn->setBounds (224, 0, 62, 26);
    addBtn->setTooltip ("Save the current curve as a named preset.");
    auto* delBtn = new juce::TextButton ("Delete");
    delBtn->setBounds (292, 0, 64, 26);
    delBtn->setTooltip ("Remove the selected user preset (built-ins cannot be deleted).");
    delBtn->setEnabled (false);
    presetRow->addAndMakeVisible (presetCombo);
    presetRow->addAndMakeVisible (addBtn);
    presetRow->addAndMakeVisible (delBtn);

    auto rebuildPresets = [this, presetCombo]
    {
        const int oldId = presetCombo->getSelectedId ();
        const juce::String oldName = oldId > 0 ? presetCombo->getItemText ((int) (oldId - 1)) : juce::String ();
        presetCombo->clear (false);
        const auto& builtins = sc::env::presets ();
        for (size_t i = 0; i < builtins.size (); ++i)
            presetCombo->addItem (builtins[i].name, (int) i + 1);
        const auto users = sc::env::parseUserPresets (app.settings.envUserPresets);
        for (size_t i = 0; i < users.size (); ++i)
            presetCombo->addItem (users[i].name, (int) builtins.size () + (int) i + 1);
        // Restore the previous selection by name when it still exists.
        if (!oldName.isEmpty ())
            for (int i = 0; i < presetCombo->getNumItems (); ++i)
                if (presetCombo->getItemText (i) == oldName)
                    { presetCombo->setSelectedItemIndex (i, juce::dontSendNotification); break; }
    };

    rebuildPresets ();

    auto loadSelectedPreset = [this, editor, presetCombo]
    {
        const int id = presetCombo->getSelectedId ();
        if (id <= 0) return;
        std::vector<sc::env::EnvPoint> pts;
        const auto& builtins = sc::env::presets ();
        if ((size_t) id <= builtins.size ())
            pts = builtins[(size_t) (id - 1)].points;
        else
        {
            const auto users = sc::env::parseUserPresets (app.settings.envUserPresets);
            const size_t ui = (size_t) id - builtins.size () - 1;
            if (ui < users.size ()) pts = users[ui].points;
        }
        // Preset times are absolute: stretch them to the current target range.
        editor->setPoints (sc::env::scaleToDuration (pts, envelopeTargetSeconds (app)));
    };

    presetCombo->onChange = [loadSelectedPreset, presetCombo, delBtn]
    {
        loadSelectedPreset ();
        const auto& builtins = sc::env::presets ();
        const int id = presetCombo->getSelectedId ();
        delBtn->setEnabled (id > 0 && (size_t) id > builtins.size ());
    };

    addBtn->onClick = [this, editor, presetCombo, rebuildPresets]
    {
        // Prompt for a name; default to the first free "My Preset N".
        const auto existing = sc::env::parseUserPresets (app.settings.envUserPresets);
        juce::String defaultName;
        for (int n = 1; n < 1000; ++n)
        {
            defaultName = "My Preset " + juce::String (n);
            bool taken = false;
            for (const auto& p : existing)
                if (p.name == defaultName) { taken = true; break; }
            if (!taken) break;
        };

        auto* dlg = new OwningAlertWindow ("Add Preset",
            "Name for the new preset (the current curve will be saved):",
            juce::AlertWindow::QuestionIcon, this);
        dlg->addTextEditor ("presetName", defaultName);
        dlg->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        dlg->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
        if (auto* ed = dlg->getTextEditor ("presetName"))
            { ed->setWantsKeyboardFocus (true); ed->selectAll(); }

        const int r = dlg->runModalLoop ();
        juce::String name;
        if (auto* ed = dlg->getTextEditor ("presetName"))
            name = ed->getText ().trim ();
        delete dlg;
        if (r != 1) return;

        // Sanitise: one line, no '|' separator character (same rules as
        // sc::env::serializeUserPresets).
        name = name.replace ("\r\n", " ").replace ("\n", " ")
                   .replace ("\r", " ").replace ("|", " ");
        if (name.isEmpty ()) return;

        auto users = sc::env::parseUserPresets (app.settings.envUserPresets);
        bool replaced = false;
        for (auto& p : users)
            if (p.name == name) { p.points = editor->getPoints (); replaced = true; break; }
        if (!replaced) users.push_back ({ name, editor->getPoints () });
        app.settings.envUserPresets = sc::env::serializeUserPresets (users);
        app.saveSettings ();

        rebuildPresets ();
        // Select the new preset so it is visible in the combo.
        for (int i = 0; i < presetCombo->getNumItems (); ++i)
            if (presetCombo->getItemText (i) == name)
                { presetCombo->setSelectedItemIndex (i); break; }   // fires onChange
    };

    delBtn->onClick = [this, presetCombo, rebuildPresets]
    {
        const auto& builtins = sc::env::presets ();
        const int id = presetCombo->getSelectedId ();
        if (id <= 0 || (size_t) id <= builtins.size ()) return;   // built-ins are protected
        auto users = sc::env::parseUserPresets (app.settings.envUserPresets);
        const size_t ui = (size_t) id - builtins.size () - 1;
        if (ui >= users.size ()) return;
        users.erase (users.begin () + (long) ui);
        app.settings.envUserPresets = sc::env::serializeUserPresets (users);
        app.saveSettings ();

        presetCombo->setSelectedId (0, juce::dontSendNotification);
        rebuildPresets ();
    };

    // Double-click a point: type its exact coordinates.
    editor->onEditPoint = [this, editor] (int idx) { showEnvPointDialog (editor, idx); };

    w->addCustomComponent (presetRow);

    // Restore the last used curve when there is one; otherwise start flat.
    const auto lastPts = sc::env::parsePoints (app.settings.envLastCurve);
    editor->setPoints (lastPts.empty () ? std::vector<sc::env::EnvPoint> { { 0.0, 0.0 } }
                                        : sc::env::scaleToDuration (lastPts, targetSecs));

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    return w;
}

void MainWindow::showEnvPointDialog (EnvCurveEditor* editor, int idx)
{
    const auto pts = editor->getPoints ();
    if (idx < 0 || idx >= (int) pts.size ()) return;

    auto* w = new OwningAlertWindow ("Edit Point",
        "Type the exact coordinates of this point:",
        juce::AlertWindow::QuestionIcon, this);
    auto* tRow = new LabeledField ("Time (s):", formatWeightText (pts[idx].tSec), "");
    tRow->editor()->setName ("ptTime");
    auto* gRow = new LabeledField ("Gain (dB):", formatWeightText (pts[idx].gainDb), "");
    gRow->editor()->setName ("ptGain");
    tRow->setSize (kInputRowW, kInputRowH);
    gRow->setSize (kInputRowW, kInputRowH);
    w->addCustomComponent (tRow);
    w->addCustomComponent (gRow);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    tRow->editor()->setWantsKeyboardFocus (true);
    tRow->editor()->selectAll ();

    w->enterModalState (false);
    tRow->editor()->grabKeyboardFocus();
    const int result = w->runModalLoop ();
    double t = -1.0, g = -1.0;
    if (auto* ed = findNamedEditor (*w, "ptTime")) parseGainDbText (ed->getText (), t);
    if (auto* ed = findNamedEditor (*w, "ptGain")) parseGainDbText (ed->getText (), g);
    delete w;
    if (result != 1) return;

    // Only the fields that parsed are applied; the other keeps its value.
    const auto cur = editor->getPoints ();
    if (idx < 0 || idx >= (int) cur.size ()) return;
    const sc::env::EnvPoint cp = cur[(size_t) idx];
    editor->setPoint (idx, t >= 0.0 ? t : cp.tSec,
                          std::isfinite (g) ? g : cp.gainDb);
}

void MainWindow::showEnvelopeDialog(std::function<void(bool)> onApplied)
{
    if (!app.audioEngine().isFileLoaded())
    { if (onApplied) onApplied (false); return; }

    auto* w = createEnvelopeDialog ();

    // Locate the curve editor so its points can be read after the modal loop.
    EnvCurveEditor* editor = nullptr;
    for (auto* c : w->getChildren ())
        if (auto* e = dynamic_cast<EnvCurveEditor*> (c)) { editor = e; break; }

    w->enterModalState (false);
    const int result = w->runModalLoop ();
    std::vector<sc::env::EnvPoint> pts;
    if (editor != nullptr)
        pts = editor->getPoints ();
    delete w;

    if (result == 1)
        // Remember the curve for next time (a flat one is the default state).
        app.settings.envLastCurve = sc::env::isFlatCurve (pts) ? juce::String ()
                                                              : sc::env::serializePoints (pts);
    if (result != 1 || sc::env::isFlatCurve (pts))
    { if (onApplied) onApplied (false); return; }   // flat curve: no-op

    app.recordActionStep (sc::actionTypes::kEnvelope,
                          sc::makeVarObject ({ { "points", sc::env::serializePoints (pts) } }));
    runDestructiveEdit ([this, pts] (std::string* e)
        { const bool ok = app.applyEnvelope (pts, e); if (ok) lastEffectId = ToolsEnvelope; return ok; },
                       [onApplied] (bool ok) { if (onApplied) onApplied (ok); });
}

juce::AlertWindow* MainWindow::createResampleDialog()
{
    auto& eng = app.audioEngine();
    const double cur = eng.sampleRate();
    const int curChans = eng.numChannels();

    auto* w = new OwningAlertWindow ("Resample",
                                     "Current rate: " + juce::String (cur, 3) + " Hz, "
                                     + juce::String (curChans) + " channel(s)\n"
                                     "(whole file; cannot be undone)",
                                     juce::AlertWindow::QuestionIcon, this);

    // Captioned input rows: the label sits in front of (left of) its field.
    // AlertWindow stacks custom components by their current height, so each
    // one must be sized before it is added.
    auto* rateRow = new LabeledField ("Sample Rate:", juce::String (cur, 3),
        "New sample rate in Hz - the whole file is resampled with SoXR.");
    rateRow->setSize (kInputRowW, kInputRowH);
    w->addCustomComponent (rateRow);
    auto* chansRow = new LabeledField ("Number of Channels:", juce::String (curChans),
        "Number of output channels (1-" + juce::String (kResampleMaxChans)
        + "). Changing it rebuilds the mix matrix below.");
    chansRow->setSize (kInputRowW, kInputRowH);
    w->addCustomComponent (chansRow);

    auto* grid = new RemixGrid (curChans);
    grid->rebuild (curChans, sc::defaultRemixWeights (curChans, curChans));
    // AlertWindow draws a named custom component's name above it.
    grid->setName ("Mix weights per output channel (each row is normalised to sum to 1):");

    // Fill buttons between the input rows and the matrix. Add order defines
    // the vertical stacking, so this goes in before the grid.
    auto* fillRow = new RemixFillButtons();
    fillRow->onFill = [grid, curChans] (int mode, bool shift, bool ctrl)
    {
        grid->setWeights (sc::remixFillWeights ((sc::RemixFill) mode,
                                                 grid->numRows(), curChans,
                                                 shift, ctrl));
    };
    fillRow->setSize (kFillRowW, kFillRowH);
    w->addCustomComponent (fillRow);
    w->addCustomComponent (grid);

    auto* rateEditor = rateRow->editor();
    auto* chansEditor = chansRow->editor();
    rateEditor->setName ("newRate");
    chansEditor->setName ("newChans");
    for (auto* editor : { rateEditor, chansEditor })
        // Let Enter/Escape reach the OK/Cancel shortcuts even while a
        // field has keyboard focus.
        editor->setEscapeAndReturnKeysConsumed (false);

    // Rebuild the weight grid whenever the requested channel count changes.
    auto* chansListener = new NewChansListener();
    chansListener->window = w;
    chansListener->grid = grid;
    chansListener->inChans = curChans;
    w->addOwnedChildComponent (chansListener);   // owned (and deleted) by the dialog
    chansEditor->addListener (chansListener);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // AlertWindow disables keyboard focus on its editors, so re-enable it and
    // pre-select the text. The actual grabKeyboardFocus() happens in
    // showResampleDialog() once the window is showing.
    rateEditor->setWantsKeyboardFocus (true);
    chansEditor->setWantsKeyboardFocus (true);
    rateEditor->selectAll();

    return w;
}

// Effects > Super Resolution... - non-modal window driving the external
// super-resolution CLI (FlashSR / AudioSR). Same ownership pattern as the
// stem-separation window: at most one open, released via onCloseRequested.
void MainWindow::showSuperResolutionWindow()
{
    if (superResWin_ != nullptr)
    {
        superResWin_->toFront (true);
        return;
    }
    superResWin_ = std::make_unique<SuperResolutionWindow> (app);
    // The X button routes to userTriedToCloseWindow(); let the window ask us
    // to release ownership so it can be destroyed safely.
    superResWin_->onCloseRequested = [this] { superResWin_.reset (); };
}

// Effects > Stem Separation... - non-modal window driving the external
// audio.cpp CLI tool. At most one instance is kept open; re-opening brings
// it to front.
void MainWindow::showStemSeparationWindow()
{
    if (stemWin_ != nullptr)
    {
        stemWin_->toFront (true);
        return;
    }
    stemWin_ = std::make_unique<StemSeparationWindow> (app);
    // The X button routes to userTriedToCloseWindow(); let the window ask us
    // to release ownership so it can be destroyed safely.
    stemWin_->onCloseRequested = [this] { stemWin_.reset (); };
}

// Show every channel in both views (View > All Channels, and the solo-off
// half of the Ctrl/Shift+click / Ctrl+Shift+N toggle).
void MainWindow::showEveryChannel()
{
    app.visibleChannels.clear();
    for (int i = 0; i < app.audioEngine().numChannels(); ++i)
        app.visibleChannels.push_back (i);
    app.syncActiveDoc ();   // keep the active document's copy in step
    syncSharedState();
}

void MainWindow::showResampleDialog(std::function<void(bool)> onApplied)
{
    // Streamed files are fine: resampleTo materialises them into memory first
    // (with a clear error if that is impossible).
    auto& eng = app.audioEngine();
    if (!eng.isFileLoaded()) { if (onApplied) onApplied (false); return; }

    auto* w = createResampleDialog();

    // Enter modal state WITHOUT taking keyboard focus (see showChangeGainDialog).
    w->enterModalState (false);

    if (auto* editor = findNamedEditor (*w, "newRate"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop();
    juce::String rateText, chansText;
    if (auto* ed = findNamedEditor (*w, "newRate"))   rateText = ed->getText().trim();
    if (auto* ed = findNamedEditor (*w, "newChans"))  chansText = ed->getText().trim();
    std::vector<std::vector<double>> weights;
    bool weightsOk = false;
    for (int i = 0; i < w->getNumChildComponents() && !weightsOk; ++i)
        if (auto* grid = dynamic_cast<RemixGrid*> (w->getChildComponent (i)))
            weightsOk = grid->getWeights (weights);
    delete w;
    if (result != 1) { if (onApplied) onApplied (false); return; }

    const double newRate = rateText.trim().getDoubleValue();
    if (!std::isfinite (newRate) || newRate <= 0.0)
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Invalid rate",
                                            "Please enter a positive sample rate in Hz.", "", this);
        if (onApplied) onApplied (false);
        return;
    }

    const int newChans = chansText.trim().getIntValue();
    if (newChans < 1 || newChans > kResampleMaxChans)
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Invalid channel count",
                                            "Please enter a channel count between 1 and "
                                            + juce::String (kResampleMaxChans) + ".", "", this);
        if (onApplied) onApplied (false);
        return;
    }

    if (!weightsOk || (int) weights.size() != newChans)
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Invalid mix weight",
                                            "Every weight field must be a number "
                                            "(empty means 0).", "", this);
        if (onApplied) onApplied (false);
        return;
    }

    weights = normaliseRemixWeights (weights);
    const bool sameRate = std::abs (newRate - eng.sampleRate())
                          < 1e-9 * std::max (eng.sampleRate(), newRate);
    if (sameRate && newChans == eng.numChannels() && isIdentityRemix (weights, newChans))
    { if (onApplied) onApplied (false); return; }   // nothing to do

    juce::Array<juce::var> wArr;
    for (const auto& row : weights)
    {
        juce::Array<juce::var> r;
        for (const double v : row) r.add (v);
        wArr.add (r);
    }
    app.recordActionStep (sc::actionTypes::kResample,
        sc::makeVarObject ({ { "rate", newRate }, { "weights", wArr } }));
    runResampleAsync (newRate, weights, onApplied);
}

// Effects > Split Spectral Selection Configuration / Shift+F12 - modal
// window editing settings.splitConfig. Closing it (OK or X) saves the
// configuration and marks the active document as configured for this file
// open; when opened from F12 the split is then executed on the current
// selection.
juce::AlertWindow* MainWindow::createSplitConfigDialog (bool willExecuteAfter)
{
    auto& eng = app.audioEngine();
    const int nChans = std::max (1, eng.numChannels());

    auto* w = new OwningAlertWindow ("Split Spectral Selection Configuration",
                                     "Tick the channels to extract the selected band from. "
                                     "Each destination is a channel number: an existing "
                                     "channel mixes in the extracted content; a number above "
                                     "the current count creates a new channel on first use.\n\n"
                                     + (willExecuteAfter
                                        ? juce::String (
                                            "Closing saves the configuration and then runs "
                                            "the split on the current selection.")
                                        : juce::String (
                                            "Closing saves the configuration without "
                                            "performing any split.")),
                                     juce::AlertWindow::InfoIcon, this);

    auto* grid = new SplitConfigGrid (nChans);
    // Prefill from the saved configuration; fall back to "last channel into
    // new" when nothing usable was stored for this file's channel count.
    bool prefilled = false;
    for (const auto& p : app.settings.splitConfig)
        if (p.first >= 0 && p.first < nChans)
        {
            grid->setRow (p.first, true, p.second + 1);
            prefilled = true;
        }
    if (!prefilled) grid->applyPresetLastIntoNew();

    w->addCustomComponent (grid);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    return w;
}

void MainWindow::showSplitConfigDialog (bool willExecuteAfter)
{
    auto& eng = app.audioEngine();
    if (!eng.isFileLoaded()) return;
    FileDoc* d = app.doc();
    if (d == nullptr) return;

    auto* w = createSplitConfigDialog (willExecuteAfter);
    // Enter modal state WITHOUT taking keyboard focus (see showResampleDialog).
    w->enterModalState (false);
    w->runModalLoop ();
    std::vector<std::pair<int,int>> pairs;
    for (int i = 0; i < w->getNumChildComponents (); ++i)
        if (auto* g = dynamic_cast<SplitConfigGrid*> (w->getChildComponent (i)))
        {
            pairs = g->getPairs ();
            break;
        }
    delete w;

    // Closing the window saves the configuration without performing any split.
    app.settings.splitConfig = std::move (pairs);
    d->splitConfigured = true;
    syncSharedState ();   // persists settings via samePersistent()
}

// Effects > Split Spectral Selection Execute / F12 - runs the configured
// split of the active spectral selection. The first time per file open it
// opens the configuration window; closing that window saves the
// configuration and then executes the split below on the current selection.
void MainWindow::runSplitExecute()
{
    auto& eng = app.audioEngine();
    if (!eng.isFileLoaded()) return;
    FileDoc* d = app.doc();
    if (d == nullptr) return;

    if (!d->splitConfigured)
        showSplitConfigDialog (true);   // closing it saves the configuration

    runDestructiveEdit ([&] (std::string* e)
        { return app.splitSpectralSelectionConfigured (e); });
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

    auto* w = new OwningAlertWindow ("New File",
                                     "Create a new empty audio file.\n"
                                     "Number of channels:\n"
                                     "Sample rate (Hz):",
                                     juce::AlertWindow::QuestionIcon, this);
    w->addTextEditor ("channels", juce::String (defChans));
    w->addTextEditor ("rate", juce::String (defRate, 3));
    if (auto* e = w->getTextEditor ("channels"))
        e->setTooltip ("Number of channels of the new file.");
    if (auto* e = w->getTextEditor ("rate"))
        e->setTooltip ("Sample rate of the new file, in Hz.");
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
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Invalid channel count",
                                            "Please enter a channel count between 1 and 512.",
                                            "", this);
        return;
    }
    if (!std::isfinite (rate) || rate <= 0.0)
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
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
                    sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
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
                        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
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
                    sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
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
                                   const std::vector<std::vector<double>>& remixWeights,
                                   std::function<void(bool)> onDone)
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

    AudioEngine* engPtr = &eng;   // resolved on the message thread (see runDestructiveEdit)
    std::thread ([this, newRate, remixWeights, dlg, onDone, engPtr] {
        double lastSent = -1.0;
        auto sendProgress = [dlg, &lastSent] (double v) {
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([dlg, v] { dlg->setProgress (v); });
        };

        // Cancellation stops at a SoXR chunk boundary; the buffer swap only
        // happens on success, so a cancelled resample changes nothing.
        std::string err;
        const bool ok = engPtr->resampleTo (
            newRate,
            [sendProgress] (float f) { sendProgress (0.85 * (double) f); },
            &dlg->cancelled, &err,
            remixWeights.empty() ? nullptr : &remixWeights);
        if (ok)
            engPtr->rebuildMipmaps ([sendProgress] (float f) {
                sendProgress (0.85 + 0.15 * (double) f);
            });

        juce::MessageManager::callAsync ([this, dlg, ok, err, onDone] {
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
                sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                                    "Resample failed",
                                                    "The file could not be resampled." + detail, "", this);
            }
            if (onDone) onDone (ok && !wasCancelled);
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
    auto* w = new OwningAlertWindow (feature + " Failed",
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
    auto* w = new OwningAlertWindow ("Noise Suppression",
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
                                     "For little noise reduction, set to 6 - 12. For medium, 18 - 24.\n"
                                     "\n"
                                     "Copy noise on new track adds one extra track per processed channel\n"
                                     "containing only the estimated noise (original minus enhanced).",
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
    if (auto* e = w->getTextEditor ("attenLim"))
        e->setTooltip ("Most the noise may be reduced, in dB. A lower value keeps more of the "
                       "original sound; 100 = no limit.");

    w->addComboBox ("autoDl", juce::StringArray { "Yes", "No" },
                    "Auto-download model if missing");
    // The persisted choice is preselected.
    if (auto* c = w->getComboBoxComponent ("autoDl"))
        c->setSelectedItemIndex (app.aiAutoDownload ? 0 : 1);

    // "Copy noise on new track": one appended channel per processed channel
    // holding only the estimated noise (original - enhanced).
    w->addComboBox ("copyNoise", juce::StringArray { "No", "Yes" },
                    "Copy noise on new track");
    if (auto* c = w->getComboBoxComponent ("copyNoise"))
        c->setSelectedItemIndex (0);   // off by default

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
    return w;
}

void MainWindow::showNoiseSuppressDialog(std::function<void(bool)> onApplied)
{
    if (!app.audioEngine().isFileLoaded() || app.selection.isEmpty())
    { if (onApplied) onApplied (false); return; }

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
    const bool copyNoise = w->getComboBoxComponent ("copyNoise") == nullptr
                             || w->getComboBoxComponent ("copyNoise")->getText () == "Yes";
    const std::string devicePref = sc::aiDevicePrefFromText (w->getComboBoxComponent ("device") != nullptr
                                                               ? w->getComboBoxComponent ("device")->getText ()
                                                               : juce::String ());
    delete w;
    if (result != 1) { if (onApplied) onApplied (false); return; }

    const std::string modelId = modelText == "DeepFilterNet2" ? "deepfilternet2"
                                 : modelText == "DeepFilterNet3" ? "deepfilternet3"
                                                                 : "denseunet";
    // Remember the choices for next time.
    app.lastNsModelId  = modelId;
    app.aiAutoDownload = autoDownload;
    app.aiDevicePref   = devicePref;
    app.saveAiSettings();

    app.recordActionStep (sc::actionTypes::kNoiseSuppress,
        sc::makeVarObject ({
            { "modelId", juce::String (modelId) },
            { "attenLimDb", attenLimDb } }));

    // v1: the DF3 post-filter option is not exposed in the dialog yet.
    runNoiseSuppressAsync (modelId, devicePref, attenLimDb, false, autoDownload, copyNoise,
                           onApplied);
}

// Noise suppression on a background thread with a progress dialog. Progress:
// model download/compile/inference 0..95%, mipmap rebuild 95..100%.
void MainWindow::runNoiseSuppressAsync (const std::string& modelId, const std::string& devicePref,
                                        float attenLimDb, bool df3PostFilter, bool autoDownload,
                                        bool copyNoise, std::function<void(bool)> onDone)
{
    auto* dlg = new WorkProgressDialog ("Noise Suppression",
                                        "Applying " + juce::String (modelId) + " ...",
                                        true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, modelId, devicePref, attenLimDb, df3PostFilter, autoDownload, copyNoise, dlg, onDone] {
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
        bool ok = false;
        // An exception escaping this detached thread would call std::terminate
        // (e.g. std::bad_alloc copying a very long selection): turn it into an
        // error so the final callAsync always runs and the dialog closes.
        try
        {
            // A false progress return cancels: the pipeline stops at its next
            // stage boundary and no audio is committed.
            ok = app.noiseSuppress (modelId, device, attenLimDb, df3PostFilter,
                [sendProgress, dlg] (float f) {
                    sendProgress (0.95 * (double) f);
                    return !dlg->cancelled.load ();
                }, &err,
                autoDownload, copyNoise);
            if (ok)
                app.audioEngine().rebuildMipmaps ([sendProgress] (float f) {
                    sendProgress (0.95 + 0.05 * (double) f);
                });
        }
        catch (const std::exception& e) { ok = false; err = e.what (); }
        catch (...)                     { ok = false; err = "Unexpected error during noise suppression."; }

        juce::MessageManager::callAsync ([this, modelId, dlg, ok, err, device, onDone] {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (ok)
                refreshAfterEdit();

            // A user cancel is not a failure: just close quietly.
            else if (!wasCancelled)
                showAiFailureDialog ("Noise Suppression", err, modelId, this, device);

            if (onDone) onDone (ok && !wasCancelled);
        });
    }).detach();
}

#endif // SC_ENABLE_AI (noise suppression)

// The Python bridge is not an AI feature: keep it in lean builds too.
// ---------------------------------------------------------------------------
// Python panel: run a user script against the selection (or whole file).
// Progress: materialise + extract 0..30%, interpreter run 30..95% (the bar
// holds steady while pure-Python processing runs), re-import 95..100%.

void MainWindow::runPythonScriptAsync (const juce::File& script, const juce::String& scriptText,
                                       std::function<void(bool)> onDone)
{
    std::string pyCmd;
    if (!sc::py::findPython (&pyCmd))
    {
        sc::alert::showMessageBox (juce::AlertWindow::InfoIcon,
                                           "Python not found",
                                           sc::py::installHelpText (),
                                           juce::String(), this);
        if (onDone) onDone (false);
        return;
    }

    auto* dlg = new WorkProgressDialog ("Python",
                                        "Running " + script.getFileName () + " ...",
                                        true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, script, scriptText, pyCmd, dlg, onDone] {
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

        juce::MessageManager::callAsync ([this, dlg, ok, err, onDone] {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (ok)
                refreshAfterEdit();
            else if (!wasCancelled)   // a user cancel is not a failure
                showEditError (err);
            if (onDone) onDone (ok && !wasCancelled);
        });
    }).detach();
}

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI

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
    auto* w = new OwningAlertWindow ("Transcription",
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

void MainWindow::showTranscribeDialog(std::function<void(bool)> onApplied)
{
    if (!app.audioEngine ().isFileLoaded ())
    { if (onApplied) onApplied (false); return; }

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
    if (result != 1) { if (onApplied) onApplied (false); return; }

    // Map the display texts back to registry ids / language codes.
    const auto names = whisperModelNames ();
    const int mi = names.indexOf (modelText);
    if (mi < 0) { if (onApplied) onApplied (false); return; }
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

    app.recordActionStep (sc::actionTypes::kTranscribe,
        sc::makeVarObject ({
            { "modelId", juce::String (modelId) },
            { "language", juce::String (language) },
            { "translate", translate },
            { "fast", fast } }));
    runTranscriptionAsync (modelId, language, translate, fast, autoDownload, onApplied);
}

// Transcription on a background thread with a progress dialog. Progress:
// download [0, 15%), mix+resample [15, 25%), inference [25, 100%].
void MainWindow::runTranscriptionAsync (const std::string& modelId, const std::string& language,
                                        bool translate, bool fast, bool autoDownload,
                                        std::function<void(bool)> onDone)
{
    auto* dlg = new WorkProgressDialog ("Transcription",
                                        "Transcribing with " + juce::String (modelId) + " ...\n"
                                        "(whole file - this can take a while)\n"
                                        "(Cancel stops after the current phase)",
                                        true, this);
    dlg->setProgress (0.0);
    dlg->beginModal ();

    std::thread ([this, modelId, language, translate, fast, autoDownload, dlg, onDone] {
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
        bool ok = false;
        // Never let an exception escape the detached thread (std::terminate):
        // report it so the final callAsync always closes the dialog.
        try
        {
            // A false progress return cancels: download and mix/resample stop at
            // their next checkpoint; inference itself cannot be interrupted
            // mid-whisper_full, so a cancel during it finishes that phase first.
            ok = app.transcribe (modelId, language, translate, fast,
                [sendProgress, dlg] (float f) {
                    sendProgress ((double) f);
                    return !dlg->cancelled.load ();
                }, &err,
                autoDownload);
        }
        catch (const std::exception& e) { ok = false; err = e.what (); }
        catch (...)                     { ok = false; err = "Unexpected error during transcription."; }

        juce::MessageManager::callAsync ([this, dlg, ok, err, modelId, onDone] {
            const bool wasCancelled = dlg->cancelled.load ();
            delete dlg;
            if (ok)
            {
                // Show the results: refresh the panel and switch to its tab.
                if (transcriptionPanel)
                {
                    transcriptionPanel->refresh ();
                    if (dock_ != nullptr && dock_->isOpen ("transcription"))
                        dock_->showPanel ("transcription");
                }
            }

            // A user cancel is not a failure: just close quietly.
            else if (!wasCancelled)
                showAiFailureDialog ("Transcription", err, modelId, this, "CPU");

            if (onDone) onDone (ok && !wasCancelled);
        });
    }).detach ();
}

// ---------------------------------------------------------------------------
// Effects > Transcribe Folder... — batch Whisper transcription of every WAV
// in a folder tree. Files are processed headlessly and their SCTT chunk is
// written back in place; nothing opens in the editor.
juce::AlertWindow* MainWindow::createTranscribeFolderDialog (const juce::File& folder)
{
    auto* w = new OwningAlertWindow ("Transcribe Folder",
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

            auto* w = new OwningAlertWindow ("Transcription Finished",
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
    // around the view's vertical offset (moved by the amplitude-axis pan and
    // the box zoom). The track grows to include a window panned past +/-1.
    if (waveVScroll && waveformView && app.audioEngine().isFileLoaded())
    {
        const double h = waveformView->getVisibleAmpHalf();
        const double c = waveformView->getVerticalOffset();
        waveVScroll->setRange (std::min (-1.0, c - h), std::max (1.0, c + h));
        waveVScroll->setWindow (c - h, c + h);
    }

    // Spectrogram: track is [1 Hz, Nyquist]; the thumb shows the visible range.
    if (specVScroll && spectralView && app.audioEngine().isFileLoaded())
    {
        const double nyq = 0.5 * app.audioEngine().sampleRate();
        specVScroll->setLogarithmic (spectralView->isLogFrequency());   // match the axis mapping
        specVScroll->setRange (1.0, nyq);
        specVScroll->setWindow (spectralView->getFreqMin(), spectralView->getFreqMax());
    }

    // Dedicated tables track: the thumb shows its visible value window.
    if (tablesVScroll && tablesTrack && app.audioEngine().isFileLoaded())
    {
        const auto full = tablesTrack->fullRange();
        const auto win  = tablesTrack->visibleRange();
        tablesVScroll->setRange (full.first, full.second);
        tablesVScroll->setWindow (win.first, win.second);
    }
}

juce::PopupMenu MainWindow::getMenuForIndex (int menuIndex, const juce::String& menuName)
{
    juce::PopupMenu menu;
    using enum MenuIDs;

    if (menuName == "File")
    {
        menu.addItem (FileNew, "New...", true, false);
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

        // Loaded Files: the documents open in this session. This replaces the
        // Files panel: pick one to make it active, close it, open its folder,
        // or open its Scale form.
        if (app.numDocs() > 0)
        {
            juce::PopupMenu docsMenu;
            FileDoc* active = app.doc();
            for (int i = 0; i < app.numDocs(); ++i)
            {
                const auto& d = *app.docs[(size_t) i];
                const juce::String name = d.file.getFileName().isEmpty()
                                        ? "Untitled" : d.file.getFileName();
                docsMenu.addItem (DocBase + i, name, true, active == &d);
            }
            docsMenu.addSeparator();
            docsMenu.addItem (DocScaleForm, "Scale...", active != nullptr);
            docsMenu.addItem (DocFolder, "Open Containing Folder", active != nullptr);
            docsMenu.addItem (DocClose, "Close Active Document", active != nullptr);
            menu.addSubMenu ("Loaded Files", docsMenu);
        }

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

        // Spectrogram export: the plain item plus a submenu with the snapshot
        // action and the persistent overlay options.
        {
            const bool canExport = app.audioEngine().isFileLoaded() && showSpectralView;
            juce::PopupMenu specMenu;
            // Ctrl+Shift+E / Ctrl+Shift+F now belong to the effect shortcuts
            // (Filter EQ / repeat last effect), so no hints here.
            specMenu.addItem (FileExportSpectral, "Save Spectrogram As...", canExport);
            specMenu.addItem (FileSnapshot, "Take Snapshot", canExport);
            specMenu.addSeparator ();
            specMenu.addItem (ExpAxes, "Save Axes", true, app.settings.exportAxes);
            specMenu.addItem (ExpMarkers, "Save Markers", true, app.settings.exportMarkers);
            specMenu.addItem (ExpSelections, "Save Selections", true, app.settings.exportSelections);
            specMenu.addItem (ExpDrawings, "Save Drawing", true, app.settings.exportDrawings);
            menu.addSubMenu ("Spectrogram", specMenu);
        }
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
        menu.addItem (EditTrimSelection, "Trim Selection\tCtrl+T", !app.selection.isEmpty(), false);
        menu.addItem (EditSelectAll, "Select All\tCtrl+A", app.audioEngine().isFileLoaded(), false);
        menu.addSeparator();
        menu.addItem (EditSnapToZeroCrossings, "Snap to Zero Crossings", true, app.settings.snapToZeroCrossings);
        menu.addSeparator();
        // Whole-file operation. Streamed files are materialised into memory
        // first when the resample actually runs.
        menu.addItem (ToolsResample, "Resample...\tF11",
                      app.audioEngine().isFileLoaded(), false);
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
                          juce::String (app.brushPctValue ()) + "%\tP",
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
        menu.addItem (ViewZoomIn, "Zoom In\tCtrl+=", true, false);   // the handler tests '='
        menu.addItem (ViewZoomOut, "Zoom Out\tCtrl+-", true, false);
        menu.addItem (ViewZoomFit, "Zoom Fit", app.audioEngine().isFileLoaded(), false);
        menu.addItem (ViewZoomToSelection, "Zoom to Selection", !app.selection.isEmpty(), false);
        menu.addItem (ViewZoomOutFull, "Zoom Out Full\tCtrl+0", app.audioEngine().isFileLoaded(), false);
        menu.addSeparator();
        menu.addItem (ViewVerticalZoomIn, "Vertical Zoom In\tCtrl+Shift+=", true, false);
        menu.addItem (ViewVerticalZoomOut, "Vertical Zoom Out\tCtrl+Shift+-", true, false);
        menu.addSeparator();
        menu.addItem (ViewShowWaveform, "Waveform View", true, showWaveformView);
        menu.addItem (ViewShowSpectral, "Spectrogram View", true, showSpectralView);
        menu.addItem (ViewShowAllChannels, "Show All Channels", app.audioEngine().isFileLoaded(), false);
        menu.addItem (ViewShowLeftPanels, "Panels\tCtrl+Shift+Enter", true, areLeftPanelsVisible ());
        menu.addSeparator();
        // Track axis placement: the vertical axis can sit on the right and
        // the time ruler at the bottom of both track views. The choices are
        // remembered between sessions.
        menu.addItem (ViewAxisOnRight, "Vertical Axis on Right", true,
                      app.settings.trackAxisOnRight);
        menu.addItem (ViewRulerAtBottom, "Time Ruler at Bottom", true,
                      app.settings.trackRulerAtBottom);
        // Horizontal grid lines inside the data tracks: amplitude levels in
        // the waveform, frequency ticks in the spectrogram. Remembered.
        menu.addItem (ViewShowAmpGrid, "Show Amplitude Grid", true,
                      app.settings.showAmpGrid);
        menu.addItem (ViewShowFreqGrid, "Show Frequency Grid", true,
                      app.settings.showFreqGrid);
        menu.addItem (ViewShowThirdOctGrid, "Show 1/3 Octave Grid", true,
                      app.settings.showThirdOctaveGrid);
        menu.addItem (ViewShowDedTrackGrid, "Show Dedicated Track Grid", true,
                      app.settings.showDedicatedTrackGrid);

        // Per-channel visibility toggles (applies to both views).
        // Ctrl/Shift+click shows only that channel; the same is available from
        // the keyboard: Ctrl+1..9 toggles a channel, Ctrl+Shift+1..9 solos it.
        const int nch = app.audioEngine().numChannels();
        if (nch > 0)
        {
            menu.addSeparator();
            for (int i = 0; i < nch; ++i)
            {
                juce::String label = "Channel " + juce::String (i + 1);
                if (i < 9)
                    label += "\tCtrl+(Shift)+" + juce::String (i + 1);
                menu.addItem (ChannelBase + i, label, true,
                              app.isChannelVisible (i));
            }
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

        // Recording: start/stop toggle (Shift+F10) + mode options + device
        // selection via plugins.
        menu.addSeparator ();
        menu.addItem (TransportRecord,
                      app.recording.active ? "Stop Recording\tShift+F10" : "Record\tShift+F10",
                      true, false);
        {
            juce::PopupMenu opts;
            for (const auto& item : recordingOptionItems (app))
                opts.addItem (item.id, item.text, true, item.checked);
            menu.addSubMenu ("Recording Options", opts);
        }
        menu.addSubMenu ("Recording Device", buildRecordingDeviceMenu (app));
    }
    else if (menuName == "Effects")
    {
        const bool hasSel = !app.selection.isEmpty();
        menu.addItem (ToolsSilence, "Silence Selection", hasSel, false);
        menu.addItem (ToolsFadeIn, "Fade In\tCtrl+Shift+I", hasSel, false);
        menu.addItem (ToolsFadeOut, "Fade Out\tCtrl+Shift+O", hasSel, false);
        menu.addItem (ToolsNormalize, "Normalize\tCtrl+Shift+N", hasSel, false);
        menu.addItem (ToolsChangeGain, "Change Gain...\tCtrl+Shift+G", hasSel, false);
        menu.addItem (ToolsReverse, "Reverse", hasSel, false);

        // Whole-file-capable effects: they act on the active spectral
        // selection if there is one, else on the time selection, else on the
        // whole file - so only a loaded file is required.
        menu.addSeparator();
        menu.addItem (ToolsInvert,      "Invert",          app.audioEngine().isFileLoaded(), false);
        menu.addItem (ToolsTimeStretch, "Time Stretch...\tCtrl+Shift+T",
                      app.audioEngine().isFileLoaded(), false);
        menu.addItem (ToolsCatTimeScale, "Content Aware Time Scale...",
                      app.audioEngine().isFileLoaded(), false);
        menu.addItem (ToolsFilterEq,    "Filter EQ...\tCtrl+Shift+E",
                      app.audioEngine().isFileLoaded(), false);
        menu.addItem (ToolsEnvelope,    "Envelope...",     app.audioEngine().isFileLoaded(), false);
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        // AI effects: first use downloads the model weights. Transcription is
        // launched from the buttons in the Transcription tab instead.
        menu.addSeparator();
        menu.addItem (ToolsNoiseSuppress, "Noise Suppression...", hasSel, false);
#endif
        // AI super resolution via an external CLI tool: enhances a range
        // into a single mono 48 kHz track (FlashSR or AudioSR). Sits under
        // Noise Suppression in the menu.
        menu.addItem (ToolsSuperResolve, "Super Resolution...",
                      app.audioEngine().isFileLoaded(), false);
        menu.addSeparator();
        // AI stem separation via an external CLI tool; first use downloads
        // the binary + model weights. A selection is optional - without one
        // the whole file becomes the input.
        menu.addItem (ToolsStemSeparate, "Stem Separation...",
                      app.audioEngine().isFileLoaded(), false);
        // Configure which channels the spectral selection is extracted from
        // and where each extraction goes. Closing saves the configuration
        // without performing any split.
        menu.addItem (ToolsSplitSpectralSelCfg, "Split Spectral Selection Configuration\tShift+F12",
                      app.audioEngine().isFileLoaded(), false);
        // Executes the configured split: each input channel's selected band is
        // extracted with a soft 25%/50%/25% frequency blend and noise-floor
        // subtraction, then mixed into its destination channel (existing ones
        // mix in place; numbers above the current count create new channels on
        // first use). The first time per file open it opens the configuration
        // window instead.
        menu.addItem (ToolsSplitSpectralSelExec, "Split Spectral Selection Execute\tF12",
                      app.hasSpectralSelection(), false);

        // Generation: replaces the spectral selection region if one is active,
        // else the time selection; with neither present a dialog asks for a
        // duration and inserts at the playhead.
        menu.addSeparator();
        menu.addItem (ToolsGenSilence, "Generate Silence...", app.audioEngine().isFileLoaded(), false);
        menu.addItem (ToolsGenNoise,   "Generate Noise...",   app.audioEngine().isFileLoaded(), false);
        menu.addItem (ToolsGenTone,    "Generate Tone...",    app.audioEngine().isFileLoaded(), false);
    }
    else if (menuName == "Window")
    {
        // Workspaces (also offered by the bar right of the menu) and the
        // maximised panel group (` key, or double-click a tab).
        juce::PopupMenu ws;
        buildWorkspaceMenu (ws, {});
        menu.addSubMenu ("Workspaces", ws);
        // The shortcut is the backquote key (left of 1 on US keyboards);
        // the bare ` character read as a stray mark in the menu.
        menu.addItem (WindowMaximizeGroup, "Maximize Panel Group\tBackquote", true, false);
        menu.addSeparator ();

        // One entry per panel, in canonical order; a checkmark shows which
        // panels are currently open (wherever they are docked or floating).
        const auto entries = leftPanelEntries ();
        for (size_t i = 0; i < entries.size (); ++i)
        {
            // Video (the picture of a video document, following the playhead)
            // has its own handler: checked while the picture shows anywhere,
            // disabled where pictures cannot be decoded.
            if (juce::String (entries[i].id) == "video")
            {
                menu.addItem (WindowVideo, entries[i].name, sc::videoDecodingAvailable (),
                              isPanelOpen ("video") || isVideoWindowVisible ());
                continue;
            }
            menu.addItem ((int) (WindowPanelBase + i), entries[i].name, true,
                          isPanelOpen (entries[i].id));
        }

        // Instance behaviour: when unchecked, launching SpectroCraft again
        // hands its command-line files to this running instance and quits the
        // second one. Takes effect from the next launch.
        menu.addSeparator ();
        menu.addItem (WindowAllowMultiple, "Allow multiple instances", true,
                      app.settings.allowMultipleInstances);
        // AI control (Claude Code in VS Code, scripts): loopback-only server,
        // token in <app data>/SpectroCraft/control/<pid>.json. Off by default.
#if defined(SC_ENABLE_CONTROL) && SC_ENABLE_CONTROL
        menu.addItem (WindowAllowControl, "Allow AI control (localhost)", true,
                      app.settings.controlEnabled);
#endif
    }
    else if (menuName == "Help")
    {
        menu.addItem (HelpAbout, "About SpectroCraft", true, false);
        menu.addItem (HelpShowTooltips, "Show Tooltips", true, app.settings.showTooltips);
        // Diagnostic log: local file of errors, freezes and slow operations
        // (docs/diagnostics.md), off by default, and what to give support.
        // Nothing is ever sent.
        menu.addSeparator ();
        menu.addItem (HelpDiagLog, "Diagnostic Log", true, sc::diag::enabled ());
        menu.addItem (HelpOpenDiagFolder, "Open Diagnostic Log Folder", true, false);
        menu.addItem (HelpCopyDiagSummary, "Copy Diagnostic Summary", true, false);
        menu.addItem (HelpSupportBundle, "Create Support Bundle...", true, false);
#if JUCE_WINDOWS
        // Automatic updates (docs/updates.md): the Windows archive layout, the
        // updater program and this menu item are Windows-only for now.
        menu.addSeparator ();
        menu.addItem (HelpCheckUpdates, "Check for Updates...", true, false);
        menu.addItem (HelpCheckUpdatesAtStartup, "Check for Updates at Startup", true,
                     app.settings.checkUpdatesAtStartup);
#endif
    }

    return menu;
}

juce::StringArray MainWindow::getMenuBarNames()
{
    // The Actions panel is reachable from the left-panel tabs; its menu was
    // removed (Ctrl+Shift+P still runs the selected action).
    // View sits between Effects and Window (classic DAW menu order).
    return { "File", "Edit", "Selection", "Transport", "Effects",
             "View", "Window", "Help" };
}

// The list panel (Markers or Transcription) currently shown in a left-panel
// tab, or nullptr when another tab is active. Panels can live in either of
// the two tab bars (they are draggable between them).
juce::Component* MainWindow::activeListPanel() const
{
    // Any group may show a searchable panel (Markers, Transcription): prefer
    // the one holding the keyboard focus, then any one shown as its group's
    // active tab (main window first, then floating windows).
    const auto searchable = [this] (const juce::Component* c)
    {
        if (c == nullptr) return false;
        if (c == markersPanel.get ()) return true;
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        if (c == transcriptionPanel.get ()) return true;
#endif
        return false;
    };
    std::vector<juce::Component*> shown;
    if (dock_ != nullptr)
        for (auto* g : dock_->allGroups ())
            if (const auto* d = dock_->panel (g->activeTab ()))
                if (d->content != nullptr)
                    shown.push_back (d->content);
    for (auto* c : shown)
        if (searchable (c) && c->hasKeyboardFocus (true)) return c;
    for (auto* c : shown)
        if (searchable (c)) return c;
    return nullptr;
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
    // Esc while a panel is being dragged cancels the drag (and nothing else).
    if (key.getKeyCode() == juce::KeyPress::escapeKey && dock_ != nullptr && dock_->isDragging ())
    {
        dock_->cancelDrag ();
        return true;
    }
    // Esc cancels a pending Tables-panel click alignment.
    if (key.getKeyCode() == juce::KeyPress::escapeKey
        && tablesPanel != nullptr && tablesPanel->hasPendingAlignment())
    {
        tablesPanel->cancelPendingAlignment();
        return true;
    }
    // ` (backtick): maximise / restore the panel group under the mouse, as
    // in Adobe applications. Text fields consume the key before it gets here.
    if (key.getTextCharacter() == '`' && ! key.getModifiers().isCommandDown()
        && ! key.getModifiers().isAltDown())
    {
        toggleMaximizePanelGroup ();
        return true;
    }
    // Ctrl+Tab / Ctrl+Shift+Tab cycles through the loaded files.
    if (key.getKeyCode() == juce::KeyPress::tabKey && key.getModifiers().isCommandDown())
    {
        if (isBackgroundJobRunning ()) return true;   // a worker still uses the active document
        app.cycleActiveDoc (!key.getModifiers().isShiftDown());
        return true;
    }
    // Ctrl+Shift+N is the Normalize effect shortcut (see below); File > New...
    // no longer has a keyboard shortcut.
    if (key == juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0)) { handleFileOpen(); return true; }
    if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0)) { handleFileSave(); return true; }
    if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { handleFileSaveAs(); return true; }
    // Effect shortcuts (Effects menu): G = Change Gain..., N = Normalize,
    // I/O = Fade In / Fade Out, E = Filter EQ..., T = Time Stretch... and
    // F repeats the previously run effect.
    if (key == juce::KeyPress ('g', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { showChangeGainDialog(); return true; }
    if (key == juce::KeyPress ('n', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
    {
        app.recordActionStep (sc::actionTypes::kNormalize);
        runDestructiveEdit ([&] (std::string* e)
        { const bool ok = app.normalize (e); if (ok) lastEffectId = ToolsNormalize; return ok; });
        return true;
    }
    if (key == juce::KeyPress ('i', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
    {
        app.recordActionStep (sc::actionTypes::kFadeIn);
        runDestructiveEdit ([&] (std::string* e)
        { const bool ok = app.fadeIn (e); if (ok) lastEffectId = ToolsFadeIn; return ok; });
        return true;
    }
    if (key == juce::KeyPress ('o', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
    {
        app.recordActionStep (sc::actionTypes::kFadeOut);
        runDestructiveEdit ([&] (std::string* e)
        { const bool ok = app.fadeOut (e); if (ok) lastEffectId = ToolsFadeOut; return ok; });
        return true;
    }
    if (key == juce::KeyPress ('e', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { showFilterEqDialog(); return true; }
    if (key == juce::KeyPress ('t', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { showTimeStretchDialog(); return true; }
    // Ctrl+Shift+F repeats the previously run effect.
    if (key == juce::KeyPress ('f', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { repeatLastEffect(); return true; }

    // Ctrl+Shift+P runs the selected action on the active document.
    if (key == juce::KeyPress ('p', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
    {
        runAction (actionsPanel ? actionsPanel->primaryIndex() : -1);
        return true;
    }

    // Alt+Shift+= / '+' increases the spectrogram's FFT size (Auto mode
    // switches to manual at one step up); Alt+Shift+- decreases it.
    if (key.getModifiers().isAltDown() && key.getModifiers().isShiftDown())
    {
        const int kc = key.getKeyCode();
        if (kc == '=' || kc == '+') { stepFftSize (true);  return true; }
        if (kc == '-')              { stepFftSize (false); return true; }
    }
    // Plain + / - raise or lower the current selection by 6 dB; Shift + / -
    // by 1 dB. The change is shaped like an envelope - it eases in over the
    // first 25% of the selection, holds, and eases back to 0 over the last 25%
    // (Application::boostSelectionGain), so a boost never clicks at the edges.
    // Works on a spectral selection too (only the masked content moves).
    {
        const int kc = key.getKeyCode();
        const bool plus  = kc == '+' || kc == '=' || key.getTextCharacter() == '+';
        const bool minus = kc == '-' || key.getTextCharacter() == '-';
        const auto m = key.getModifiers();
        if ((plus || minus) && ! m.isAltDown() && ! m.isCommandDown() && ! m.isCtrlDown())
        {
            // Nothing to shape: leave the key to whatever has focus.
            if (! app.audioEngine().isFileLoaded ()) return false;
            if (! app.hasSpectralSelection () && app.selection.getLength () <= 0)
                return false;
            const double db = m.isShiftDown () ? 1.0 : 6.0;
            runDestructiveEdit ([&] (std::string* e)
            { return app.boostSelectionGain (plus ? db : -db, e); });
            return true;
        }
    }
    // Shift+F10 starts/stops recording (Transport > Record). Plain F10 is
    // left free for the OS / other tools.
    if (key.getKeyCode() == juce::KeyPress::F10Key && key.getModifiers().isShiftDown ()) { handleStartStopRecording (); return true; }
    // F11 opens Edit > Resample... (no-op when no file is loaded).
    if (key.getKeyCode() == juce::KeyPress::F11Key) { showResampleDialog(); return true; }
    // Shift+F12 opens the split-configuration window (no split is performed).
    if (key.getKeyCode() == juce::KeyPress::F12Key && key.getModifiers().isShiftDown ())
    {
        if (app.audioEngine ().isFileLoaded ()) showSplitConfigDialog ();
        return true;
    }
    // F12 executes the configured split of the active spectral selection.
    if (key.getKeyCode() == juce::KeyPress::F12Key && app.hasSpectralSelection ())
    {
        runSplitExecute ();
        return true;
    }
    // F5 refreshes the spectrogram (Spectral Controls > Refresh): clears the
    // tile cache and forces a full re-render. Global, like the other shortcuts.
    if (key.getKeyCode() == juce::KeyPress::F5Key) { refreshSpectrogram (); return true; }
    // Ctrl+H toggles the spectrogram's harmonic markers (Spectral Controls >
    // "Show harmonics CTRL+H"). Global, like the other shortcuts.
    if (key == juce::KeyPress ('h', juce::ModifierKeys::commandModifier, 0)) { toggleHarmonicMarkers (); return true; }
    // Ctrl+Shift+Enter toggles the left panel column.
    if (key == juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { setLeftPanelsVisible (!areLeftPanelsVisible ()); return true; }
    if (key == juce::KeyPress ('w', juce::ModifierKeys::commandModifier, 0)) { handleCloseFile(); return true; }
    // Shortcuts announced by the File / Transport menus.
    if (key == juce::KeyPress ('q', juce::ModifierKeys::commandModifier, 0)) { closeButtonPressed(); return true; }
    if (key == juce::KeyPress ('.', juce::ModifierKeys::commandModifier, 0)) { app.stop(); return true; }
    // Ctrl+F4 also closes the active file (browser-style tab close).
    if (key == juce::KeyPress (juce::KeyPress::F4Key, juce::ModifierKeys::commandModifier, 0)) { handleCloseFile(); return true; }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0)) { app.undoStep(); refreshAfterEdit(); return true; }
    if (key == juce::KeyPress ('y', juce::ModifierKeys::commandModifier, 0)) { app.redoStep(); refreshAfterEdit(); return true; }
    if (key == juce::KeyPress ('x', juce::ModifierKeys::commandModifier, 0)) { runDestructiveEdit ([&] (std::string* e) { return app.cut (e); }); return true; }
    if (key == juce::KeyPress ('t', juce::ModifierKeys::commandModifier, 0))
    {
        app.recordActionStep (sc::actionTypes::kTrim);
        runDestructiveEdit ([&] (std::string* e) { return app.trimSelection (e); });
        return true;
    }
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
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        app.recordActionStep (sc::actionTypes::kDelete);
        runDestructiveEdit ([&] (std::string* e) { return app.del (e); });
        return true;
    }
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
    // channel in both views; Ctrl+Shift+1..9 shows ONLY it (solo) - pressing
    // it again while solo on that channel restores every channel.
    if (mods.isCommandDown() && !mods.isAltDown() && keyCode >= '1' && keyCode <= '9')
    {
        const int ch = keyCode - '1';   // 0-based channel index
        if (app.audioEngine().isFileLoaded() && ch < app.audioEngine().numChannels())
        {
            if (mods.isShiftDown())
            {
                if (app.visibleChannels.size() == 1 && app.visibleChannels[0] == ch)
                    showEveryChannel();   // solo off: back to all channels
                else
                {
                    app.visibleChannels = { ch };
                    app.syncActiveDoc ();   // keep the active document's copy in step
                    syncSharedState();
                }
            }
            else
            {
                app.toggleChannelVisible (ch);
                syncSharedState();   // views and footer channel numbers follow
            }
            return true;
        }
    }

    if (app.audioEngine().isFileLoaded() && !mods.isAltDown()
            && (keyCode == juce::KeyPress::leftKey || keyCode == juce::KeyPress::rightKey))
    {
        // Plain / Shift arrows step by one percent of the visible time span
        // per press, so zooming in makes the steps smaller until deep zoom
        // reaches exactly one sample. Ctrl+arrow steps by one video frame
        // (1/fps of the active document's picture; 1/60 s without a video);
        // Ctrl+Shift keeps the fine one-sample step for selection.
        const bool ctrl = mods.isCommandDown() || mods.isCtrlDown();
        const double minStep = 1.0 / app.audioEngine().sampleRate();
        double step;
        if (ctrl && !mods.isShiftDown())
        {
            const double fps = (videoWindow != nullptr && videoWindow->pump().isOpen ())
                                   ? videoWindow->pump().info().frameRate : 0.0;
            step = (fps > 1e-6) ? 1.0 / fps : 1.0 / 60.0;
        }
        else if (ctrl)
            step = minStep;
        else
            step = std::max (minStep, (app.viewEnd - app.viewStart) / 100.0);
        const bool right = (keyCode == juce::KeyPress::rightKey);

        if (mods.isShiftDown())
        {
            // Anchor on the first Shift+arrow press; keep it while Shift is held.
            if (shiftAnchorTime < 0.0)
                shiftAnchorTime = app.playhead;

            const double newPlayhead = app.playhead + (right ? step : -step);
            app.setPlayhead (newPlayhead);
            app.selectionRow = -1;   // keyboard selection spans all rows
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
    // Ctrl+0 = Zoom Out Full: resets the shared time window AND both views'
    // vertical ranges (amplitude / frequency) back to their defaults.
    if (key == juce::KeyPress ('0', juce::ModifierKeys::commandModifier, 0)) { zoomOutFull(); return true; }
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

// ---------------------------------------------------------------------------
// sc::control::UiBridge: what AI-control commands may ask of this window.
// Every method is non-blocking (no modal loop, no FileChooser, no message box).
// ---------------------------------------------------------------------------

void MainWindow::zoomVertical (double factor)
{
    zoomVerticalBoth (factor);
}

bool MainWindow::setViewVisible (const juce::String& what, bool visible)
{
    if (what == "waveform" || what == "spectral")
    {
        const bool wave = what == "waveform";
        // Hiding both views is not allowed (same rule as the View menu).
        if (!visible && !(wave ? showSpectralView : showWaveformView))
            return false;
        (wave ? showWaveformView : showSpectralView) = visible;
        resized();
        return true;
    }
    if (what == "leftPanels")
    {
        setLeftPanelsVisible (visible);   // false: the Editor's group is maximised
        return true;
    }
    return false;
}

bool MainWindow::isViewVisible (const juce::String& what) const
{
    if (what == "waveform")   return showWaveformView;
    if (what == "spectral")   return showSpectralView;
    if (what == "leftPanels") return areLeftPanelsVisible ();
    return false;
}

juce::StringArray MainWindow::panelNames() const
{
    juce::StringArray names;
    for (const auto& e : leftPanelEntries())
        names.add (e.name);
    return names;
}

bool MainWindow::setPanelVisibleAt (int index, bool visible)
{
    const auto entries = leftPanelEntries();
    if (index < 0 || index >= (int) entries.size()) return false;
    setPanelVisible (entries[(size_t) index].id, visible);
    return true;
}

bool MainWindow::isPanelOpenAt (int index) const
{
    const auto entries = leftPanelEntries();
    if (index < 0 || index >= (int) entries.size()) return false;
    return isPanelOpen (entries[(size_t) index].id);
}

void* MainWindow::beginJobDialog (const juce::String& title, const juce::String& status)
{
    // The same non-blocking progress dialog as the menus' background jobs:
    // isBackgroundJobRunning() and systemRequestedQuit() see it unchanged.
    auto* dlg = new WorkProgressDialog (title, status, true, this);
    dlg->setProgress (0.0);
    dlg->beginModal();
    return dlg;
}

void MainWindow::setJobProgress (void* handle, double progress, const juce::String& status)
{
    auto* dlg = static_cast<WorkProgressDialog*> (handle);
    if (dlg == nullptr) return;
    dlg->setProgress (progress < 0.0 ? -1.0 : juce::jlimit (0.0, 1.0, progress));
    if (status.isNotEmpty()) dlg->setStatusText (status);
}

const std::atomic<bool>* MainWindow::jobCancelFlag (void* handle) const
{
    auto* dlg = static_cast<WorkProgressDialog*> (handle);
    return dlg != nullptr ? &dlg->cancelled : nullptr;
}

void MainWindow::endJobDialog (void* handle)
{
    delete static_cast<WorkProgressDialog*> (handle);   // deleting exits the modal state
    // The job may have edited, opened or saved a document.
    refreshAfterEdit();
    updateTitle();
    refreshPanels();
}

void MainWindow::setControlClients (int count)
{
    controlClients_ = count;
    if (statusBar != nullptr)
        statusBar->setControlText (count > 0 ? "AI control: " + juce::String (count)
                                                   + (count == 1 ? " client" : " clients")
                                             : juce::String());
}

void MainWindow::requestQuit (bool discardUnsaved)
{
    // Same refusals as systemRequestedQuit(), checked again now: a job or an
    // edit may have started since app.quit was accepted.
    if (isBackgroundJobRunning() || app.control().jobs.numRunning() > 0)
        return;
    if (!discardUnsaved && !app.unsavedDocNames().empty())
        return;
    juce::JUCEApplicationBase::quit();
}

// ---------------------------------------------------------------------------
// Menu actions through the command registry (AI control lot 6).
// ---------------------------------------------------------------------------
bool MainWindow::runMenuCommand (const juce::String& cmd, const juce::var& params, int effectId, bool quiet)
{
    sc::control::InvokeOptions io;
    io.source = "menu";
    io.ui = this;
    io.fromMenu = true;
    const auto r = sc::control::CommandRegistry::instance().invoke (app, cmd, params, {}, io);
    const bool ok = (bool) r.getProperty ("ok", false);
    const auto res = r.getProperty ("result", {});
    if (ok && res.hasProperty ("job") && res.getProperty ("state", {}).toString() == "running")
    {
        // Streamed file: the registry loads it in a job behind the progress
        // dialog; report a failure (and the repeat-effect id) when it ends.
        const juce::String jobId = res.getProperty ("job", {}).toString();
        juce::Component::SafePointer<MainWindow> self (this);
        auto check = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weak = check;
        *check = [self, jobId, effectId, quiet, weak] {
            if (self == nullptr) return;
            auto& jobs = self->app.control().jobs;
            if (!jobs.isFinished (jobId))
            {
                if (auto again = weak.lock()) juce::Timer::callAfterDelay (100, [again] { (*again)(); });
                return;
            }
            const auto o = jobs.outcome (jobId);
            if (o && o->ok) { if (effectId != 0) self->lastEffectId = effectId; }
            else if (o && !quiet && o->error.code != sc::control::err::cancelled)
                self->showEditError (o->error.message.toStdString());
        };
        juce::Timer::callAfterDelay (100, [check] { (*check)(); });
        return true;
    }
    if (ok)
    {
        if (effectId != 0) lastEffectId = effectId;
        return true;
    }
    const auto e = r.getProperty ("error", {});
    if (!quiet && e.getProperty ("code", {}).toString() != sc::control::err::cancelled)
        showEditError (e.getProperty ("message", {}).toString().toStdString());
    return false;
}
