#pragma once
// SpectroCraft - Main window
#include <JuceHeader.h>
#include "Application.h"
#include "WaveformView.h"
#include "SpectralView.h"
#include "ScrollBars.h"
#include "Panels.h"
#include <memory>
#include <algorithm>

#include "WorkProgress.h"

class StemSeparationWindow;   // defined in StemSeparation.h

class MainWindow : public juce::DocumentWindow,
                   private juce::KeyListener,
                   public juce::MenuBarModel,
                   public juce::Timer,
                   public juce::FileDragAndDropTarget
{
public:
    MainWindow (Application& app, const juce::String& name);
    ~MainWindow();

    // Text shown by Help > About SpectroCraft: version, project licence and the
    // licences of all third-party components (JUCE, SoXR, OpenVINO runtime,
    // Intel's Audacity-plugin DSP port, whisper.cpp, model weights).
    static juce::String buildAboutText();

    void resized() override;
    void closeButtonPressed() override;
    void userTriedToCloseWindow() override;   // native-title-bar X button
    void timerCallback() override; // animates the playhead while playing
    void visibilityChanged() override; // applies the window icon once the HWND exists

    // Drag & drop audio files onto the window (FileDragAndDropTarget)
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // MenuBarModel interface
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int menuIndex, const juce::String& menuName) override;
    void menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

    // KeyListener
    bool keyPressed (const juce::KeyPress& key, juce::Component* originatingComponent) override;

    // The open Stem Separation window, if any (null when closed).
    StemSeparationWindow* stemSepWindow() const { return stemWin_.get (); }

    void openFile (const juce::File& file);   // routes large files to the async path
    void openFileWithProgress (const juce::File& file);  // background load + progress dialog
    void runSaveAsync (const juce::File& file, bool saveAs);      // background save + dialog
    void exportSpectralAsync (const juce::File& file,
                              const SpectralView::SpectralExportOptions& opts = {});
                                                          // background PNG render + dialog
    void handleExportSpectral();                            // File > Spectrogram > Save As... / Ctrl+Shift+E
    void takeSpectrogramSnapshot();                         // File > Spectrogram > Take snapshot (Ctrl+Shift+F)
    /** Timestamped snapshot path next to the WAV: <name>_snapshot_<yyyy-mm-dd>_<hh-mm-ss>.png,
        with a numeric suffix if that exact file already exists. `when` is
        injectable for unit tests (defaults to now). */
    static juce::File makeSnapshotPath (const juce::File& wav, const juce::Time* when = nullptr);
    void handleFileOpen();
    void handleFileSave();
    void handleFileSaveAs();
    void handleSaveMetadataOnly();
    void handleCloseAll();
    void updateTitle();
    void setupViews();
    void activateDocUI();   // reset both views + sync panels after a doc switch
    juce::String statusBarFileName() const;   // name currently shown in the footer
    // The status-bar tool indicator: active spectral selection tool name,
    // overridden by Clone & Multiply picking and Move/Scale gesture hints;
    // while a shape-drawing tool is active, holding SHIFT appends "Add
    // Selection" and CTRL appends "Subtract Selection" (live modifier state).
    juce::String currentModeText() const;
    // Pure part of the above: append the SHIFT/CTRL selection-mode suffix.
    juce::String applyModifierHint (const juce::String& mode,
                                    bool shiftDown, bool ctrlDown) const;
    bool timeScrollBarVisible() const { return timeBar != nullptr && timeBar->isVisible(); }
    bool waveformVisible() const { return showWaveformView; }      // View menu state
    bool spectrogramVisible() const { return showSpectralView; }
    juce::String waveformTooltip() const { return waveformView->getTooltip(); }
    juce::String spectralTooltip() const { return spectralView->getTooltip(); }
    double waveformTimeStart() const { return waveformView->getViewStart(); }
    double waveformTimeEnd()   const { return waveformView->getViewEnd(); }
    double waveformAmpHalf()   const { return waveformView->getVisibleAmpHalf(); }
    double spectralFreqMin()   const { return spectralView->getFreqMin(); }
    double spectralFreqMax()   const { return spectralView->getFreqMax(); }
    SpectralView* spectralViewComp() { return spectralView.get (); }
    WaveformView* waveformViewComp() { return waveformView.get (); }
    void applyTimeWindow (double t0, double t1); // push one time window to BOTH views
    void refreshAfterEdit();   // clamp ranges, repaint waveform, re-render spectral image
    /** Applies a destructive edit (`edit` returns false and fills errOut on
        failure). If the active file is still being streamed from disk it must
        first be loaded into memory; that can take seconds for multi-GB files,
        so in that case the whole thing runs on a worker thread behind a
        progress dialog. On success refreshAfterEdit() is called; on failure an
        error message box is shown (unless the user cancelled). */
    void runDestructiveEdit(std::function<bool(std::string*)> edit);
    void showEditError (const std::string& err);

    // Play request that first guarantees the lazy audio device init has
    // completed; shows a short progress dialog if it still needs to run.
    void playWithAudioInit();
    void refreshPanels();
    void applyColourMap (ColourMap m);
    void zoomHorizontal (double factor);   // both views
    void updateScrollBars();               // keep scrollbars in sync with the views
    void zoomVerticalBoth (double factor); // waveform amplitude + spectral frequency
    void zoomToFit();
    void zoomToSelection();   // fit the current selection in both views
    void zoomOutFull();       // whole file + default vertical zooms in both views
    void cycleViewVisibility (bool forward);  // F9: both -> waveform -> spectrogram
    juce::Component* activeListPanel() const;  // Markers/Transcription tab content, or nullptr
    void handleFind (bool openDialog);     // Ctrl+F / F3 in the active list panel
    void handleFindPrevious ();            // Ctrl+F3 in the active list panel
    void handleContextMenuAction (int id);  // right-click menu actions from either view
    /** Arms the spectrogram's reference-click flow for a scale calibration
        (Scale > Custom / Alternative Scale menu items). */
    void armRefClick (Application::RefClickTarget target);
    /** The armed flow received its reference point: prompts for the value
        and applies the resulting scale. */
    void handleRefClickPlaced (double tSecs, double fHz);
    /** Moves a left-panel tab from (from, fromIndex) to position insertIndex in
        `to` - within the same bar or across bars. Persists the new layout.
    */
    void moveLeftPanelTab (juce::TabbedComponent* from, int fromIndex,
                           juce::TabbedComponent* to, int insertIndex);
    void saveLeftLayout();                    // persist tab order + column geometry
    void applySavedLeftLayout();              // restore a persisted arrangement at startup
    void setLeftPanelsVisible (bool visible); // View > Left Panels toggle
    bool areLeftPanelsVisible() const { return leftPanelsVisible; }
    juce::StringArray topPanelTabNames() const { return topTabs.getTabNames (); }
    juce::StringArray bottomPanelTabNames() const { return bottomTabs.getTabNames (); }
    juce::TabbedComponent* topPanelTabs() { return &topTabs; }
    juce::TabbedComponent* bottomPanelTabs() { return &bottomTabs; }

    // Window menu + per-panel "Close Panel" support: open/close state of the
    // left-column panels. `id` is one of files/fft/history/python/
    // controls/markers/transcription/selection.
    bool isPanelOpen (const juce::String& id) const;
    void setPanelVisible (const juce::String& id, bool visible);
    juce::Component* panelForId (const juce::String& id) const;   // unit tests

    void showChangeGainDialog();
    /** Builds (but does not run modally) the Change Gain dialog. The caller
        owns and must delete it; used by showChangeGainDialog() and tests. */
    juce::AlertWindow* createGainDialog();

    // ---- Alternative scale references + Go to Time -----------------------

    /** Builds (but does not run modally) the edit dialog for one alternative-
        scale reference: rename, edit both endpoint values, enable/disable,
        delete. The caller owns the returned window. */
    juce::AlertWindow* createAltRefEditDialog(bool isFreq, int index);

    /** Runs the alternative-reference edit dialog modally and applies its
        outcome to the session state (persisted with per-file metadata). */
    void showAltRefEditDialog(bool isFreq, int index);

    /** Applies an alternative-reference edit dialog's outcome to the session
        state. result: 1 = OK, 2 = cancel, 3 = delete. Returns true when the
        state changed (so the caller can persist + refresh). */
    static bool applyAltRefEditResult(Application& app, bool isFreq, int index,
                                      int result, const juce::String& nameText,
                                      const juce::String& mainText,
                                      const juce::String& altText, bool visible);

    /** Field layout of the Go to Time dialog: the main-axis field id plus one
        entry per enabled alternative time reference (index into
        Application::altTimeRefs + editor id), in display order. */
    struct GoToTimeFields
    {
        juce::String mainId;
        std::vector<std::pair<int, juce::String>> alts;
    };

    static GoToTimeFields computeGoToTimeFields(Application& app);

    /** Resolves which Go to Time field was filled in and converts it to
        seconds. altTexts must match the order of fields.alts. Returns false
        when no usable value was found. */
    static bool resolveGoToTime(Application& app, const GoToTimeFields& fields,
                                const juce::String& mainText,
                                const std::vector<juce::String>& altTexts,
                                double& outSecs);

    /** Builds (but does not run modally) the Go to Time dialog. The caller
        owns the returned window. */
    juce::AlertWindow* createGoToTimeDialog();

    /** Runs the Go to Time dialog modally and moves the playhead to the first
        non-empty, parseable field (main axis mode or an alternative time
        reference). Ctrl+G. */
    void showGoToTimeDialog();
    void showNewTextDialog (double tSecs, double fHz, int px, int py);
    void showResampleDialog();
    /** Builds (but does not run modally) the Resample dialog. The caller owns
        and must delete it; used by showResampleDialog() and tests. */
    juce::AlertWindow* createResampleDialog();
    /** "New File" dialog: asks for channel count and sample rate, then calls
        Application::newFile(). */
    void showNewFileDialog();
    /** Effects > Stem Separation... - opens (or brings to front) the
        non-modal window that drives the external stem-separation CLI. */
    void showStemSeparationWindow();
    /** Background resample (+ optional channel remix) with a progress
        dialog. Empty remixWeights keeps the current channels. */
    void runResampleAsync (double newRate,
                           const std::vector<std::vector<double>>& remixWeights);

    /** Shift + drag & drop: popup offering how to merge the dropped files.
        Item ids: 1 = plain open, 2/3 = sequence into current/new file,
        4/5 = parallel into current/new file. The "into current file" items
        only exist when a document is loaded (public static so headless tests
        can check the menu contents). */
    static juce::PopupMenu buildMergeMenu (bool haveActiveDoc);

    /** Transport > Recording Device menu: one sub-menu per recorder plugin.
        Public static so headless tests can check it. */
    static juce::PopupMenu buildRecordingDeviceMenu (const Application& app);

    /** One entry of a Recording Device sub-menu (plain data for tests). */
    struct RecDeviceMenuItem { int id = 0; juce::String text; bool checked = false; };

    /** The device-level items for the plugin at `pluginIndex` (same IDs and
        texts as its sub-menu). Public static so headless tests can check it. */
    static std::vector<RecDeviceMenuItem> recordingDeviceItems (const Application& app,
                                                                int pluginIndex);
    static juce::PopupMenu buildRecordingDeviceItems (const Application& app, int pluginIndex);

    void handleStartStopRecording();
    void handleRecDevicePick (int id);
    /** File Open dialog wildcard: every extension the engine can open as
        audio (native formats + video containers when built with FFmpeg).
        Public static so headless tests can check it. */
    static juce::String openAudioWildcard();
    void showMergeMenu (const std::vector<juce::File>& files);
    /** Background merge with a progress dialog. intoCurrent merges into the
        active document; otherwise a new document is created from the result. */
    void runMergeFilesAsync (const std::vector<juce::File>& files, sc::MergeMode mode,
                             bool intoCurrent);
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    /** Builds (but does not run modally) the Noise Suppression dialog. The caller
        owns and must delete it; used by showNoiseSuppressDialog() and tests. */
    juce::AlertWindow* createNoiseSuppressDialog();
    void showNoiseSuppressDialog();
    // devicePref: "auto" | "CPU" | "GPU" | "NPU" - resolved against the
    // available OpenVINO devices on the worker thread before processing.
    void runNoiseSuppressAsync (const std::string& modelId, const std::string& devicePref,
                                float attenLimDb = 0.f, bool df3PostFilter = false,
                                bool autoDownload = true);

    /** Builds (but does not run modally) the Transcription dialog. The caller
        owns and must delete it; used by showTranscribeDialog() and tests. */
    juce::AlertWindow* createTranscribeDialog();
    void showTranscribeDialog();
    void runTranscriptionAsync (const std::string& modelId, const std::string& language,
                                bool translate, bool fast, bool autoDownload = true);

    /** Builds (but does not run modally) the Transcribe Folder options dialog.
        The caller owns and must delete it; used by showTranscribeFolderDialog(). */
    juce::AlertWindow* createTranscribeFolderDialog (const juce::File& folder);
    void showTranscribeFolderDialog();   // FileChooser -> options dialog -> async run
    void runBatchTranscribeAsync (const juce::File& folder, const std::string& modelId,
                                  const std::string& language, bool translate,
                                  bool fast, bool skipExisting, int channel,
                                  bool autoDownload);
#endif

    /** Runs the Python script `script` with exactly this editor text on a
        background thread with a progress dialog. The selection is processed,
        or the whole file when nothing is selected; the result replaces that
        range in place (undoable). */
    void runPythonScriptAsync (const juce::File& script, const juce::String& scriptText);

    void handleSplitterDrag (const juce::MouseEvent& e);      // waveform/spectral divider
    void handleLeftSplitterDrag (const juce::MouseEvent& e);  // left column / main area
    void handleTopSplitterDrag (const juce::MouseEvent& e);   // top tabs / bottom tabs
    const char* panelIdFor (juce::Component*) const;          // stable id for a panel component
    struct LeftPanelEntry { const char* id; juce::String name; bool defaultTop; };
    std::vector<LeftPanelEntry> leftPanelEntries() const;     // canonical order, AI-conditional
    void syncSharedState();   // push app state into views + panels

    enum MenuIDs
    {
        FileNew = 999, FileOpen = 1000, FileClose, FileCloseAll, FileSave, FileSaveAs,
        FileExportSpectral, FileSnapshot, ExpAxes, ExpMarkers, ExpSelections, ExpDrawings, FileQuit,
        EditUndo = 2000, EditRedo, EditCut, EditCopy, EditPaste, EditDelete,
        EditSelectAll, EditSnapToZeroCrossings, EditCopyToNewFile, EditPasteMix,
        EditPasteToNewChannels,
        ViewZoomIn = 3000, ViewZoomOut, ViewZoomFit, ViewZoomToSelection, ViewZoomOutFull,
        ViewVerticalZoomIn,
        ViewVerticalZoomOut, ViewShowWaveform, ViewShowSpectral, ViewShowAllChannels,
        ViewShowLeftPanels,   // hide/show the whole left panel column
        ViewAxisOnRight,      // vertical axis on the right of both track views
        ViewRulerAtBottom,    // time ruler at the bottom of both track views
        ViewColourMap,
        SelectionToolTimeRange = 3500, SelectionToolMarquee, SelectionToolLasso,
        SelectionToolPaintbrush, SelectionClear, SelectionBrushSmaller,
        SelectionBrushLarger,
        TransportPlay = 4000, TransportStop, TransportPause, TransportLoop,
        TransportPlaySpectralSel,
        TransportRecord,          // start/stop recording (plugin-based capture)
        RecDeviceBase = 4500,     // Recording Device submenu: + pluginIndex*1000 + deviceIndex
        ToolsSilence = 5000, ToolsFadeIn, ToolsFadeOut, ToolsNormalize, ToolsChangeGain, ToolsReverse,
        ToolsResample,
        ToolsStemSeparate,   // AI stem separation via the audio.cpp CLI tool
        ToolsSplitSpectralSel,     // split active spectral selection into new channel(s) (F12)
        ToolsSplitSpectralSelT12,  // move track 1's selected band to track 2 (Shift+F12)
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        ToolsNoiseSuppress,   // AI noise suppression (built with -DSPECTRO_ENABLE_AI=ON)
        ToolsTranscribe,      // AI Whisper transcription (whole file)
        ToolsTranscribeFolder,// AI batch transcription of a folder of WAV files
#endif
        WindowPanelBase = 6500,   // Window menu: per-panel open/close toggles
        HelpAbout = 6000,
        MetaEmbedded = 7500, MetaExternal, MetaSaveOnly,   // File > Metadata submenu
        ColourMapClassic = 7000, ColourMapViridis, ColourMapPlasma, ColourMapInferno,
        ColourMapMagma, ColourMapHot, ColourMapCool, ColourMapGrayscale,
        ChannelBase = 8000,    // per-channel visibility toggles: ChannelBase + index
        RecentBase = 9000,     // File > Open Recent items: RecentBase + index
        ClearRecent = 9100     // File > Open Recent > Clear Recent Files
    };

private:
    Application& app;

    juce::MenuBarComponent menuBar;
    std::unique_ptr<WaveformView> waveformView;
    std::unique_ptr<SpectralView> spectralView;
    std::unique_ptr<ViewSplitter> splitter;   // draggable divider between the views
    double splitRatio = 0.55;                 // fraction of main area used by waveform
    std::unique_ptr<ViewSplitter> leftSplitter;  // vertical divider: left column | main area
    int leftPanelW = 340;                     // width of the left panel column (px)
    std::unique_ptr<ViewSplitter> topSplitter;   // horizontal divider between tab groups
    double topRatio = 0.52;                   // fraction of left column used by top tabs
    bool showWaveformView = true, showSpectralView = true;  // View menu visibility
    ColourMap lastSyncedColourMap;            // to detect colour-map changes
    Settings lastSyncedSettings;              // baseline for the settings-save trigger
    double shiftAnchorTime = -1.0;           // Shift+arrow selection anchor (-1 = inactive)
    bool iconApplied = false;                 // window icon set once HWND is available
    bool audioInitKickedOff = false;          // lazy device init started (timer-driven)
    sc::WorkProgressDialog* audioInitDialog = nullptr;   // open while waiting for init

    // Left-side panels
    std::unique_ptr<FileListPanel> fileListPanel;
    std::unique_ptr<FFTAnalysisView> fftPanel;
    std::unique_ptr<SpectralControlsPanel> controlsPanel;
    std::unique_ptr<MarkerListPanel> markersPanel;
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    std::unique_ptr<TranscriptionPanel> transcriptionPanel;   // Whisper segments (Phase 3)
#endif
    std::unique_ptr<SelectionPanel> selectionPanel;           // named selections + clone & multiply
    std::unique_ptr<DrawingPanel> drawingPanel;               // spectrogram drawings (bottom group)
    std::unique_ptr<HistoryPanel> historyPanel;
    std::unique_ptr<PythonPanel> pythonPanel;                 // user Python scripts (top group)

    // Effects > Stem Separation window (at most one open at a time).
    std::unique_ptr<StemSeparationWindow> stemWin_;


    // Zoom/pan scrollbars: time bar at the bottom, one vertical
    // scrollbar per view on its right edge.
    std::unique_ptr<TimeScrollBar> timeBar;
    std::unique_ptr<ValueScrollBar> waveVScroll, specVScroll;

    // Status bar at the very bottom: mouse probe + selection/playhead times.
    std::unique_ptr<StatusBar> statusBar;

    // Single app-wide tooltip window (JUCE shows tooltips for any component
    // implementing juce::TooltipClient once this exists).
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    TabDragContext tabDragCtx;                 // shared state for dragging panel tabs
    DragTabBar topTabs, bottomTabs;           // left-column tab groups (draggable tabs)
    bool leftPanelsVisible = true;            // View > Left Panels toggle


    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
