#pragma once
// SpectroCraft - Main window
#include <JuceHeader.h>
#include "Application.h"
#include "Recorder.h"   // sc::rec::DeviceInfo for the recording dialog helpers
#include "WaveformView.h"
#include "SpectralView.h"
#include "ScrollBars.h"
#include "TablesTrack.h"
#include "Panels.h"
#include "ScaleForm.h"   // ScaleFormWindow::PickAction in handleScalePick
#include "Docking.h"   // Adobe-style docking workspace (panel groups, floating windows)
#include <memory>
#include <algorithm>
#include <atomic>
#include <thread>

#include "WorkProgress.h"
#include "ContentAwareTimeScale.h"
#include "control/UiBridge.h"   // AI control: what commands may ask of this window

class StemSeparationWindow;   // defined in StemSeparation.h
class VideoWindow;            // defined in VideoWindow.h
class VideoDockHost;          // defined in VideoWindow.h
class SuperResolutionWindow;  // defined in SuperResolution.h

namespace sc { namespace update { struct ReleaseInfo; struct AssetInfo; } }   // docs/updates.md

/** Content of the "Content Aware Time Scale" dialog: a mode drop-down
    (Time Stretch / Reduce Gaps) plus the fields for the selected mode. The
    whole block is one custom component so rows can be shown/hidden per mode.
    A background analysis thread feeds setAnalysis() for the live preview. */
class CatTimeScalePanel : public juce::Component
{
public:
    enum class Mode { TimeStretch, ReduceGaps, ExportVariation };

    explicit CatTimeScalePanel (double refDurationSecs);
    ~CatTimeScalePanel() override;   // joins the analysis worker, deletes the rows

    /** Starts a worker that runs fn() off the UI thread and then delivers its
        result via setAnalysis(). Only the first call starts a worker. The
        destructor waits for fn() to return, so fn() never reads application
        state after the dialog is gone (edit applied, document closed); a
        delivery still queued then is skipped. */
    void startAnalysis (std::function<void(sc::cat::Analysis&, bool&, juce::String&)> fn);

    /** Delivers a finished analysis (call on the message thread). ok=false
        shows err in the status line instead of a preview. */
    void setAnalysis (sc::cat::Analysis a, bool ok, const juce::String& err = {});

    Mode getMode() const { return mode_; }
    double getSpeedFactor () const;   // parsed from the visible fields (> 0)
    double getPercentile () const;    // clamped to [0, 100]
    bool markOnly() const { return markOnly_ != nullptr && markOnly_->getToggleState(); }
    bool exportVariation() const { return mode_ == Mode::ExportVariation; }

    juce::ComboBox* modeCombo() const { return modeCombo_; }   // unit tests
    // Row visibility (unit tests): per-mode visibility is tracked on the rows.
    bool durationRowVisible()   const;
    bool factorRowVisible()     const;
    bool percentileRowVisible() const;
    bool markOnlyVisible()      const;
    juce::String getResultText() const { return resultLabel_->getText(); }   // unit tests

private:
    void setMode (Mode m);
    void syncFromDuration ();
    void syncFromFactor ();
    void refreshResult ();

    Mode mode_ = Mode::TimeStretch;
    double refDur_ = 1.0;
    sc::cat::Analysis analysis_;
    bool analysisOk_ = false;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>> (true);
    std::thread worker_;   // analysis thread, joined by the destructor

    juce::Label* modeLabel_ = nullptr;
    juce::ComboBox* modeCombo_ = nullptr;
    class LabeledField* durRow_ = nullptr;   // defined in MainWindow.cpp
    class LabeledField* facRow_ = nullptr;
    class LabeledField* pctRow_ = nullptr;
    juce::TextButton* markOnly_ = nullptr;
    juce::Label* resultLabel_ = nullptr;
};

class ScaleFormWindow;   // the Scale form (include/ScaleForm.h)

class MainWindow : public juce::DocumentWindow,
                   private juce::KeyListener,
                   public juce::MenuBarModel,
                   public juce::Timer,
                   public juce::FileDragAndDropTarget,
                   public sc::control::UiBridge
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

    // The open Super Resolution window, if any (null when closed).
    SuperResolutionWindow* superResWindow() const { return superResWin_.get (); }

    void openFile (const juce::File& file);   // routes large files to the async path
    void openFileWithProgress (const juce::File& file);  // background load + progress dialog
    /** CSV files dropped on the window become tables of the active document,
        one after the other. A large one (kCsvProgressBytes and up) is read
        and parsed on a worker thread behind a progress dialog with Cancel
        (issue #14); a small one imports at once. */
    void importCsvTables (std::vector<juce::File> files);
    static constexpr juce::int64 kCsvProgressBytes = 2 * 1024 * 1024;
    void runSaveAsync (const juce::File& file, bool saveAs);      // background save + dialog
    void exportSpectralAsync (const juce::File& file,
                              const SpectralView::SpectralExportOptions& opts = {});
                                                          // background PNG render + dialog
    void handleExportSpectral();                            // File > Spectrogram > Save As... / Ctrl+Shift+E
    void takeSpectrogramSnapshot();                         // File > Spectrogram > Take snapshot (Ctrl+Shift+F)
    void refreshSpectrogram();                              // Spectral Controls > Refresh / F5: clear the tile cache and force a full re-render
    void toggleHarmonicMarkers();                           // Spectral Controls > "Show harmonics" / Ctrl+H
    /** Timestamped snapshot path next to the WAV: <name>_snapshot_<yyyy-mm-dd>_<hh-mm-ss>.png,
        with a numeric suffix if that exact file already exists. `when` is
        injectable for unit tests (defaults to now). */
    static juce::File makeSnapshotPath (const juce::File& wav, const juce::Time* when = nullptr);
    void handleFileOpen();
    void handleFileSave();
    void handleFileSaveAs();
    void handleSaveMetadataOnly();
    void handleCloseAll();
    /** Ctrl+W / Ctrl+F4 / File > Close: closes the active document, asking
        first when it has unsaved changes (same prompt style as Close All /
        Quit). Returns true if a document was closed. */
    bool handleCloseFile();
    /** True while a background job is running (a progress dialog other than
        the "Preparing audio" wait is alive; its worker deletes it when done).
        Documents must not be closed or switched, nor the app quit, until then. */
    bool isBackgroundJobRunning() const;
    void showOpenFailed (const juce::File& file);   // "Open failed" message box
    void updateTitle();
    void setupViews();
    void activateDocUI();   // reset both views + sync panels after a doc switch

    // ---- Video window (include/VideoWindow.h) ----
    /** Points the Video window at the active document's video (creating
        the window the first time a document with a video is active) and
        refreshes the audio length it compares against. */
    void syncVideoWindow();
    /** Shows / hides the Video window (Window > Video). */
    void setVideoWindowVisible (bool visible);
    bool isVideoWindowVisible() const;
    VideoWindow* getVideoWindow() const noexcept { return videoWindow.get(); }
    /** Docks the picture into the left panel (a tab like Tables) or back
        into its own window. Docking keeps one decoder and one zoom/pan state. */
    void setVideoDocked (bool docked);
    bool isVideoDocked() const;
    VideoDockHost* getVideoDockHost() const noexcept { return videoDockHost.get(); }
    /** Document time the Video window shows now. */
    double videoTimeNow() const;
    /** Time under the mouse over a view, -1 when outside (tests). */
    double videoHoverTimeNow() const noexcept { return videoHoverTime; }
    juce::String statusBarFileName() const;   // name currently shown in the footer
    bool statusBarShowsDiagLog() const;        // footer "Diagnostic log" indicator
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
    SpectralControlsPanel* controlsPanelComp() { return controlsPanel.get (); }   // unit tests
    juce::Component* waveVScrollBarComp() { return waveVScroll.get (); }   // unit tests
    juce::Component* specVScrollBarComp() { return specVScroll.get (); }   // unit tests
    juce::Component* tablesTrackComp() { return tablesTrack.get (); }      // unit tests
    TablesPanel* tablesPanelComp() { return tablesPanel.get (); }          // unit tests
    StatusBar* statusBarComp() { return statusBar.get (); }                // unit tests
    /** The Editor tab (dock panel id) showing a document, empty when none
        (unit tests). */
    juce::String editorTabOfDoc (int docId) const
    {
        const auto it = editorTabOfDoc_.find (docId);
        return it != editorTabOfDoc_.end() ? it->second : juce::String();
    }
    /** Whether an Editor tab (dock panel id) holds a picture of its document,
        shown while the tab is visible without the views (unit tests). */
    bool editorTabHasPicture (const juce::String& panelId) const;
    /** Whether tooltips can show (Help > Show Tooltips) (unit tests). */
    bool tooltipsActive() const noexcept { return tooltipWindow != nullptr; }
    juce::Component* tablesSepComp()   { return tablesSep.get (); }        // unit tests
    /** Top/bottom insets (px) that keep a view's vertical scrollbar clear of its own time ruler strip. */
    static void vScrollInsetsForView (Application& app, juce::Component* view,
                                      int& topInset, int& botInset);
    void applyTimeWindow (double t0, double t1) override; // push one time window to BOTH views
    void refreshAfterEdit() override;   // clamp ranges, repaint waveform, re-render spectral image
    /** Applies a destructive edit (`edit` returns false and fills errOut on
        failure). If the active file is still being streamed from disk it must
        first be loaded into memory; that can take seconds for multi-GB files,
        so in that case the whole thing runs on a worker thread behind a
        progress dialog. On success refreshAfterEdit() is called; on failure an
        error message box is shown (unless the user cancelled). */
    /** onDone (optional) is called on the message thread with the success
        flag once the edit has finished - used by action replay to chain
        steps. */
    void runDestructiveEdit(std::function<bool(std::string*)> edit,
                            std::function<void(bool)> onDone = nullptr);
    void showEditError (const std::string& err);
    /** Menu/shortcut actions that have a control command run THROUGH the
        command registry (source "menu"): same code, refresh, recording,
        journal and history provenance as the API (lot 6). Shows the error
        (sync or when the job ends) unless `quiet`. effectId, when non-zero,
        becomes the "repeat last effect" id on success. Returns ok. */
    bool runMenuCommand (const juce::String& cmd, const juce::var& params = {}, int effectId = 0,
                         bool quiet = false);

    // Play request that first guarantees the lazy audio device init has
    // completed; shows a short progress dialog if it still needs to run.
    void playWithAudioInit();
    void refreshPanels();
    void zoomHorizontal (double factor);   // both views
    void updateScrollBars();               // keep scrollbars in sync with the views
    void zoomVerticalBoth (double factor); // waveform amplitude + spectral frequency
    void zoomToFit() override;
    void zoomToSelection() override;   // fit the current selection in both views
    void zoomOutFull() override;       // whole file + default vertical zooms in both views

    // ---- sc::control::UiBridge (AI control; never opens a modal dialog) ----
    void zoomVertical (double factor) override;
    bool setViewVisible (const juce::String& what, bool visible) override;
    bool isViewVisible (const juce::String& what) const override;
    juce::StringArray panelNames() const override;
    bool setPanelVisibleAt (int index, bool visible) override;
    bool isPanelOpenAt (int index) const override;
    void* beginJobDialog (const juce::String& title, const juce::String& status) override;
    void setJobProgress (void* handle, double progress, const juce::String& status) override;
    const std::atomic<bool>* jobCancelFlag (void* handle) const override;
    void endJobDialog (void* handle) override;
    void setControlClients (int count) override;
    void requestQuit (bool discardUnsaved) override;
    /** Window > Allow AI control toggled: Main.cpp starts/stops the server. */
    std::function<void (bool)> onControlToggled;
    /** Help > Diagnostic Log toggled (after the setting was
        saved): Main.cpp starts/stops the log (src/Diagnostics.cpp). */
    std::function<void (bool)> onDiagToggled;
    /** Turns the diagnostic log on (after explaining what it records, unless
        headless) or off, and persists the choice. */
    void toggleDiagnosticLog();
    /** A link showing the folder's path; a click opens the folder (created
        first) in the Finder / Explorer. */
    static std::unique_ptr<juce::HyperlinkButton> makeDiagFolderLink (const juce::File& folder);
    /** The dialog toggleDiagnosticLog() shows first: what is recorded, the
        folder link, Turn On / Cancel. The link outlives the window (the
        AlertWindow does not own its custom components). */
    struct DiagLogDialog
    {
        std::unique_ptr<juce::HyperlinkButton> link;   // declared first: destroyed last
        std::unique_ptr<juce::AlertWindow> window;
    };
    static DiagLogDialog makeDiagLogDialog (juce::Component* centreAround);
    static const juce::String kDiagLogDialogText;
    /** Help > Create Support Bundle...: lists the files, asks where to save
        the zip. writeSupportBundleTo() is the part without dialogs. */
    void showSupportBundleDialog();
    bool writeSupportBundleTo (const juce::File& zip, juce::String* error = nullptr);
    /** Help > Copy Diagnostic Summary: markdown (<= 30 KB) to the clipboard. */
    void copyDiagnosticSummary();
    /** "<Menu> > <item text>" of a menu bar item, for the diagnostic log. */
    juce::String menuItemDescription (int menuItemID, int topLevelMenuIndex);
    /** Status text of the control server for the menu tooltip / status bar. */
    int controlClients() const { return controlClients_; }
    /** Set by Main.cpp while the control server runs (journal polling). */
    bool controlServerRunning = false;

    // ------------------------------------------------------------------ updates
    // Help > Check for Updates (docs/updates.md). Windows only: the archive
    // layout, the updater and the menu item are Windows-specific for now.
    /** Fetch the latest GitHub release and compare it with this build. With
        automatic = true (the startup check) nothing newer, an unreachable
        GitHub or a declined version stays silent; only the diagnostic log may
        mention it. */
    void checkForUpdates (bool automatic = false);
    /** The startup check, when Help > Check for Updates at Startup is on:
        at most once a day, and never about a version already declined. */
    void maybeCheckForUpdatesAtStartup();
    /** Called after a start the updater asked for (--finish-update): reports
        what it did (update-result.json) and removes the files it left behind. */
    void reportFinishedUpdate();
    /** True while a release lookup or an update job is in flight. */
    std::atomic<bool> updateCheckRunning { false };

private:
    /** The "a new version is available" dialog: [Update Now] starts the
        download, [Later] remembers the version so the startup check does not
        offer the same one again. */
    void offerUpdate (const sc::update::ReleaseInfo& rel, const juce::String& current,
                      bool automatic);
    /** Download, verify and stage the release, then start the updater and
        quit (docs/updates.md). */
    void startUpdate (const sc::update::ReleaseInfo& rel, const sc::update::AssetInfo& asset,
                      const sc::update::AssetInfo& manifestAsset,
                      const sc::update::AssetInfo& sigAsset);

public:
    void cycleViewVisibility (bool forward);  // F9: both -> waveform -> spectrogram
    juce::Component* activeListPanel() const;  // Markers/Transcription tab content, or nullptr
    void handleFind (bool openDialog);     // Ctrl+F / F3 in the active list panel
    void handleFindPrevious ();            // Ctrl+F3 in the active list panel
    void handleContextMenuAction (int id);  // right-click menu actions from either view
    /** Arms the spectrogram's reference-click flow for a scale calibration
        (Scale > Custom / Alternative Scale menu items). */
    void armRefClick (Application::RefClickTarget target);

    /* A Scale form crosshair button: work out an axis entry's offset / scale
       from the track - the playhead, a selection's ends, or a reference point
       the user is about to click in the waveform or spectrogram. */
    void handleScalePick (int entryIndex, ScaleFormWindow::PickAction action);
    void applyScaleReference (int entryIndex, double mainValue, double typedValue);
    int pendingScaleEntry = -1;   // the axis a reference click is being placed for
    /** Opens (or raises) the Scale form for the given axis kind
        (sc::axes::Kind): the modeless, always-on-top dialog that replaces the
        old Scale / Alternative Scale submenus. */
    void openScaleForm (int axisKind);
    /** Reveal the active document's file in the file browser. */
    void openActiveDocFolder();
    /** Which kind a "Add Axis" in the Scale form defaults to: the axis of the
        track whose context menu opened it. */
    int ctxScaleKind = 1;   // sc::axes::Kind
    /** The armed flow received its reference point: prompts for the value
        and applies the resulting scale. */
    void handleRefClickPlaced (double tSecs, double fHz);
    // ---- Docking workspace (include/Docking.h) ----------------------------
    // Every panel - the Editor (waveform + spectrogram) included - lives in a
    // panel group of the dock tree; panels are dragged between groups, docked
    // on any side, undocked into floating windows. Named workspaces keep
    // whole arrangements (Window > Workspaces and the bar right of the menu).
    sc::dock::DockManager& dockManager() noexcept { return *dock_; }
    sc::dock::Workspaces& workspaces() noexcept { return workspaces_; }
    juce::Component* editorPanelComp() const noexcept { return editorPanel_.get(); }
    sc::dock::WorkspaceBar* workspaceBarComp() const noexcept { return workspaceBar_.get(); }
    /** Tabs of the group holding a panel, as shown (titles), empty if closed. */
    juce::StringArray panelGroupTitles (const juce::String& id) const;
    void saveDockLayout();                    // persist the arrangement now
    /** Applies a workspace: its current (modified) layout, else its saved one. */
    void switchWorkspace (const juce::String& id);
    void resetWorkspace();                    // Window > Workspaces > Reset to Saved Layout
    void saveWorkspaceChanges();              // ... > Save Changes to this Workspace
    juce::String saveWorkspaceAs (const juce::String& name);   // ... > Save as New Workspace (new id)
    bool deleteWorkspace (const juce::String& id);
    bool renameWorkspace (const juce::String& id, const juce::String& name);
    /** The built-in layout of a workspace id ("default", "editing", ...). */
    juce::var builtInWorkspaceLayout (const juce::String& id) const;
    /** ` (backtick): maximise / restore the panel group under the mouse
        (else the focused one). */
    void toggleMaximizePanelGroup();
    void setLeftPanelsVisible (bool visible); // View > Panels: false = Editor maximised
    bool areLeftPanelsVisible() const;

    // Window menu + per-panel "Close Panel" support: open/close state of the
    // panels. `id` is one of files/fft/history/python/controls/markers/
    // transcription/selection/drawing/tables/actions/video (and "editor",
    // which cannot be closed).
    bool isPanelOpen (const juce::String& id) const;
    void setPanelVisible (const juce::String& id, bool visible);
    juce::Component* panelForId (const juce::String& id) const;   // unit tests

    /** onApplied (optional) is called with the success flag once the effect
        has been applied - used by action replay to chain steps. */
    void showChangeGainDialog(std::function<void(bool)> onApplied = nullptr);
    /** Builds (but does not run modally) the Change Gain dialog. The caller
        owns and must delete it; used by showChangeGainDialog() and tests. */
    juce::AlertWindow* createGainDialog();

    // ---- Effects > Generate ... dialogs ----------------------------------
    void showGenerateSilenceDialog(std::function<void(bool)> onApplied = nullptr);
    void showGenerateNoiseDialog(std::function<void(bool)> onApplied = nullptr);
    void showGenerateToneDialog(std::function<void(bool)> onApplied = nullptr);
    /** Build (but do not run modally) the generation dialogs. The caller owns
        and must delete them; used by the show* functions and tests. */
    juce::AlertWindow* createGenerateSilenceDialog();
    juce::AlertWindow* createGenerateNoiseDialog();
    juce::AlertWindow* createGenerateToneDialog();

    // ---- Effects > Invert / Time Stretch... / Content Aware Time Scale... /
    //          Filter EQ... / Envelope... ---
    void showTimeStretchDialog(std::function<void(bool)> onApplied = nullptr);
    void showContentAwareTimeScaleDialog(std::function<void(bool)> onApplied = nullptr);
    void showFilterEqDialog(std::function<void(bool)> onApplied = nullptr);
    void showEnvelopeDialog(std::function<void(bool)> onApplied = nullptr);

    /** Ctrl+Shift+F: re-dispatches the previously run effect (one of the
        classic Effects-menu items). No-op until an effect has been run. */
    void repeatLastEffect();
    int getLastEffectId() const { return lastEffectId; }   // unit tests

    /** Actions panel: interactive replay of a recorded action on the current
        document. Steps run in order; prompt steps open their normal dialog,
        non-prompt steps apply the recorded values directly. */
    void runAction (int index);
    void continueReplay();                       // advance to the next step
    /** When set, action replay reports its end here (ok, message) instead of
        the modal "finished" / "stopped" message boxes. */
    std::function<void (bool, const juce::String&)> onReplayFinished;
    void stopReplay (const juce::String& why);  // abort with a status message
    bool isReplaying() const { return !replaySteps_.empty (); }
    /** Batch button: opens the batch-processing window for an action. */
    void showActionBatchWindow (int index);

    std::vector<sc::ActionStep> replaySteps_;   // active replay queue
    int replayPos_ = 0;
    juce::String replayName_;

    /** Alt+Shift+= / '+' and Alt+Shift+-: step the spectrogram's FFT size one
        power of two along the 256..65536 ladder. Auto mode switches to manual
        at one step away from its currently resolved size. */
    void stepFftSize (bool up);
    /** Build (but do not run modally) the Time Stretch, Filter EQ and
        Envelope dialogs. The caller owns and must delete them; used by the
        show* functions and tests. */
    juce::AlertWindow* createTimeStretchDialog();
    juce::AlertWindow* createContentAwareTimeScaleDialog();
    juce::AlertWindow* createFilterEqDialog();
    juce::AlertWindow* createEnvelopeDialog();

    /** Modal dialog to type the exact coordinates of one EQ curve point.
        Used by EqCurveEditor's onEditPoint (double-click). */
    void showEqPointDialog (class EqCurveEditor* editor, int index);

    /** Modal dialog to type the exact coordinates of one envelope curve point.
        Used by EnvCurveEditor's onEditPoint (double-click). */
    void showEnvPointDialog (class EnvCurveEditor* editor, int index);

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
    /** The Scale form, kept open while the tracks keep reacting. */
    std::unique_ptr<ScaleFormWindow> scaleForm;
    void showNewTextDialog (double tSecs, double fHz, int px, int py);
    /** Shows every channel in both views (View > All Channels; also the
        solo-off half of the Ctrl/Shift+click and Ctrl+Shift+N toggles). */
    void showEveryChannel();
    /** Effects > Split Spectral Selection Configuration / Shift+F12: modal
        window editing settings.splitConfig. Closing it saves the
        configuration without performing any split and marks the active
        document as configured (per-file-open state). @param willExecuteAfter
        true when closing the window is immediately followed by executing the
        split (the F12 flow), so the message can say so. */
    void showSplitConfigDialog (bool willExecuteAfter = false);

    /** Builds the split-configuration dialog; caller owns and must delete it;
        used by showSplitConfigDialog() and tests. @param willExecuteAfter as
        for showSplitConfigDialog(). */
    juce::AlertWindow* createSplitConfigDialog (bool willExecuteAfter = false);

    /** Effects > Split Spectral Selection Execute / F12: runs the configured
        split of the active spectral selection. The first time per file open
        it opens the configuration window; closing that window saves the
        configuration and then executes the split on the current selection. */
    void runSplitExecute();

    void showResampleDialog(std::function<void(bool)> onApplied = nullptr);
    /** Builds (but does not run modally) the Resample dialog. The caller owns
        and must delete it; used by showResampleDialog() and tests. */
    juce::AlertWindow* createResampleDialog();
    /** "New File" dialog: asks for channel count and sample rate, then calls
        Application::newFile(). */
    void showNewFileDialog();
    /** Effects > Stem Separation... - opens (or brings to front) the
        non-modal window that drives the external stem-separation CLI. */
    void showStemSeparationWindow();
    /** Effects > Super Resolution... - opens (or brings to front) the
        non-modal window that drives the external super-resolution CLI. */
    void showSuperResolutionWindow();
    /** Background resample (+ optional channel remix) with a progress
        dialog. Empty remixWeights keeps the current channels. */
    /** onDone (optional) is called with the success flag when the resample
        finishes - used by action replay to chain steps. */
    void runResampleAsync (double newRate,
                           const std::vector<std::vector<double>>& remixWeights,
                           std::function<void(bool)> onDone = nullptr);

    /** Shift + drag & drop: popup offering how to merge the dropped files.
        Item ids: 1 = plain open, 2/3 = sequence into current/new file,
        4/5 = parallel into current/new file. The "into current file" items
        only exist when a document is loaded (public static so headless tests
        can check the menu contents). */
    static juce::PopupMenu buildMergeMenu (bool haveActiveDoc);

    /** Transport > Recording Options menu items (Unlimited / Loop Mode /
        Timed). Public static so headless tests can check them. */
    struct RecOptionMenuItem { int id = 0; juce::String text; bool checked = false; };
    static std::vector<RecOptionMenuItem> recordingOptionItems (const Application& app);

    /** Transport > Recording Device menu: one sub-menu per recorder plugin.
        Public static so headless tests can check it. */
    static juce::PopupMenu buildRecordingDeviceMenu (const Application& app);

    /** Channel combo items for the recording device dialog: explicit
        per-channel options ("All (N)", "Channel 1", ...) for fixed-width
        devices, or every count the device offers otherwise. Public static so
        headless tests can check it. */
    static juce::StringArray recChannelComboItems (const sc::rec::DeviceInfo& dev);

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
    void showNoiseSuppressDialog(std::function<void(bool)> onApplied = nullptr);
    // devicePref: "auto" | "CPU" | "GPU" | "NPU" - resolved against the
    // available OpenVINO devices on the worker thread before processing.
    // copyNoise: append one new track per processed channel holding only the
    // estimated noise (original - enhanced) over the selection range.
    /** onDone (optional) is called with the success flag when the run
        finishes - used by action replay to chain steps. */
    void runNoiseSuppressAsync (const std::string& modelId, const std::string& devicePref,
                                float attenLimDb = 0.f, bool df3PostFilter = false,
                                bool autoDownload = true, bool copyNoise = false,
                                std::function<void(bool)> onDone = nullptr);

    /** Builds (but does not run modally) the Transcription dialog. The caller
        owns and must delete it; used by showTranscribeDialog() and tests. */
    juce::AlertWindow* createTranscribeDialog();
    void showTranscribeDialog(std::function<void(bool)> onApplied = nullptr);
    void runTranscriptionAsync (const std::string& modelId, const std::string& language,
                                bool translate, bool fast, bool autoDownload = true,
                                std::function<void(bool)> onDone = nullptr);

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
    /** onDone (optional) is called with the success flag when the script
        finishes - used by action replay to chain steps. */
    void runPythonScriptAsync (const juce::File& script, const juce::String& scriptText,
                               std::function<void(bool)> onDone = nullptr);

    void handleSplitterDrag (const juce::MouseEvent& e);      // waveform/spectral divider
    void handleTablesSepDrag (const juce::MouseEvent& e);     // dedicated tables track divider
    const char* panelIdFor (juce::Component*) const;          // stable id for a panel component
    struct LeftPanelEntry { const char* id; juce::String name; bool defaultTop; };
    std::vector<LeftPanelEntry> leftPanelEntries() const;     // canonical order, AI-conditional
    void syncSharedState();   // push app state into views + panels
    void updateAutoFftLabel();   // keep the "Auto (N)" FFT-size combo label in sync

    enum MenuIDs
    {
        FileNew = 999, FileOpen = 1000, FileClose, FileCloseAll, FileSave, FileSaveAs,
        FileExportSpectral, FileSnapshot, ExpAxes, ExpMarkers, ExpSelections, ExpDrawings, FileQuit,
        EditUndo = 2000, EditRedo, EditCut, EditCopy, EditPaste, EditDelete,
        EditTrimSelection,
        EditSelectAll, EditSnapToZeroCrossings, EditCopyToNewFile, EditPasteMix,
        EditPasteToNewChannels,
        ViewZoomIn = 3000, ViewZoomOut, ViewZoomFit, ViewZoomToSelection, ViewZoomOutFull,
        ViewVerticalZoomIn,
        ViewVerticalZoomOut, ViewShowWaveform, ViewShowSpectral, ViewShowAllChannels,
        ViewShowLeftPanels,   // hide/show the whole left panel column
        ViewAxisOnRight,      // vertical axis on the right of both track views
        ViewRulerAtBottom,    // time ruler at the bottom of both track views
        ViewShowAmpGrid,      // horizontal amplitude grid lines in the waveform tracks
        ViewShowFreqGrid,     // horizontal frequency grid lines in the spectrogram tracks
        ViewShowThirdOctGrid, // 1/3 octave band borders + centre labels in the spectrogram
        ViewShowDedTrackGrid, // horizontal value grid lines in the dedicated tables track
        ViewColourMap,
        SelectionToolTimeRange = 3500, SelectionToolMarquee, SelectionToolLasso,
        SelectionToolPaintbrush, SelectionClear, SelectionBrushSmaller,
        SelectionBrushLarger,
        TransportPlay = 4000, TransportStop, TransportPause, TransportLoop,
        TransportPlaySpectralSel,
        TransportRecord,          // start/stop recording (plugin-based capture; Shift+F10)
        RecOptUnlimited = 4100,   // Recording Options submenu
        RecOptLoop,
        RecOptTimed,
        RecNoDeviceItem = 4200,   // disabled "(no devices)" / "(no recorder plugins found)" placeholders (ID 0 is reserved by PopupMenu)
        ToolsSilence = 5000, ToolsFadeIn, ToolsFadeOut, ToolsNormalize, ToolsChangeGain, ToolsReverse,
        ToolsInvert,           // Effects > Invert (spectral sel / time sel / whole file)
        ToolsTimeStretch,      // Effects > Time Stretch... (SoundTouch tempo change)
        ToolsCatTimeScale,     // Effects > Content Aware Time Scale... (STFT gap analysis)
        ToolsFilterEq,         // Effects > Filter EQ... (FFT curve equaliser)
        ToolsEnvelope,         // Effects > Envelope... (amplitude-over-time curve)
        ToolsResample,
        ToolsStemSeparate,   // AI stem separation via the audio.cpp CLI tool
        ToolsSuperResolve,   // AI super resolution (FlashSR / AudioSR) via the audio.cpp CLI tool
        ToolsSplitSpectralSelCfg,   // configure the split: input -> destination channels (Shift+F12)
        ToolsSplitSpectralSelExec,  // execute the configured split of the active spectral selection (F12)
        ToolsGenSilence,           // Effects > Generate Silence...
        ToolsGenNoise,             // Effects > Generate Noise...
        ToolsGenTone,              // Effects > Generate Tone...
        ActionsRunSelected = 6800, // Actions > Run Selected Action (Ctrl+Shift+P)
        ActionsBatch = 6801,       // Actions > Batch...
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        ToolsNoiseSuppress,   // AI noise suppression (built with -DSPECTRO_ENABLE_AI=ON)
#endif
        WindowPanelBase = 6500,   // Window menu: per-panel open/close toggles
        WindowAllowMultiple = 6700,   // Window menu: "Allow multiple instances" toggle
        WindowAllowControl = 6701,    // Window menu: "Allow AI control (localhost)" toggle
        WindowVideo = 6702,           // Window menu: open the Video panel / hide the picture
        HelpDiagLog = 6704,           // Help menu: "Diagnostic Log" toggle
        WindowWorkspaceBase = 6600,   // Window > Workspaces: switch to workspace i (up to 48)
        WindowWsReset = 6650,         // Window > Workspaces > Reset to Saved Layout
        WindowWsSaveChanges = 6651,   // ... > Save Changes to this Workspace
        WindowWsSaveAs = 6652,        // ... > Save as New Workspace...
        WindowWsRename = 6653,        // ... > Rename Workspace...
        WindowWsDelete = 6654,        // ... > Delete Workspace
        WindowMaximizeGroup = 6655,   // Window > Maximize Panel Group (backquote key)
        HelpAbout = 6000,
        HelpOpenDiagFolder = 6010,    // Help > Open Diagnostic Log Folder
        HelpCopyDiagSummary = 6011,   // Help > Copy Diagnostic Summary
        HelpSupportBundle = 6012,     // Help > Create Support Bundle...
        HelpCheckUpdates = 6013,      // Help > Check for Updates...
        HelpCheckUpdatesAtStartup = 6014, // ... submenu: also check at startup
        HelpShowTooltips = 6015,      // Help > Show Tooltips (issue #12)
        MetaEmbedded = 7500, MetaExternal, MetaSaveOnly,   // File > Metadata submenu
        ChannelBase = 8000,    // per-channel visibility toggles: ChannelBase + index
        RecentBase = 9000,     // File > Open Recent items: RecentBase + index
        // File > Loaded Files: the documents that are open in this session
        // (what the Files panel used to be). DocBase + index, then three items
        // for the active document.
        DocBase = 9100, DocEnd = 9140,
        DocClose = 9141, DocFolder = 9142, DocScaleForm = 9143,
        // The same two actions from a panel header menu (right-click on the
        // Editor's tab or its ⋯ button): ids clear of the docking ids, which
        // are small integers.
        PanelFolderId = 9200, PanelScaleId = 9201,
        ClearRecent = 9100,    // File > Open Recent > Clear Recent Files
        // Recording Device submenu: RecDeviceBase + pluginIndex*1000 + deviceIndex.
        // Kept above every other ID: with the old base (4500) the second
        // plugin's devices (5500+) fell outside the routed range and a third
        // plugin's first device (6500) was WindowPanelBase.
        RecDeviceBase = 10000,
        RecDeviceEnd  = RecDeviceBase + 1000 * 1000   // up to 1000 plugins x 1000 devices
    };

private:
    Application& app;

    juce::MenuBarComponent menuBar;
    std::unique_ptr<WaveformView> waveformView;
    std::unique_ptr<SpectralView> spectralView;
    std::unique_ptr<ViewSplitter> splitter;   // draggable divider between the views
    double splitRatio = 0.55;                 // fraction of main area used by waveform
    int tablesSepAnchorY = 0;                 // strip top (Editor panel coords) for the drag handler
    double tablesSepRefH = 1.0;               // reference height for tablesTrackRatio
    bool showWaveformView = true, showSpectralView = true;  // View menu visibility
    ColourMap lastSyncedColourMap;            // to detect colour-map changes
    Settings lastSyncedSettings;              // baseline for the settings-save trigger
    double shiftAnchorTime = -1.0;           // Shift+arrow selection anchor (-1 = inactive)
    int lastEffectId = 0;                    // last run classic effect (Ctrl+Shift+F repeat); 0 = none yet
    juce::String autoFftLabel_;              // cached "Auto (N)" combo label text
    bool iconApplied = false;                 // window icon set once HWND is available
    bool audioInitKickedOff = false;          // lazy device init started (timer-driven)
    sc::WorkProgressDialog* audioInitDialog = nullptr;   // open while waiting for init

    // Left-side panels
    std::unique_ptr<FFTAnalysisView> fftPanel;

    // Video window: created on first use, see syncVideoWindow(). The dock host
    // is the left-panel tab that holds the view while docked.
    std::unique_ptr<VideoWindow> videoWindow;
    std::unique_ptr<VideoDockHost> videoDockHost;
    double videoHoverTime = -1.0;      // time under the mouse, -1 = not over a view
    juce::File videoDismissedFor;      // closed by the user while showing this file
    std::shared_ptr<AudioEngine::SoloAudio> videoSoloCache;   // decoded solo track audio (reused across offset changes)
    juce::File videoSoloCacheFor;                          // which file it belongs to
    void ensureVideoWindow();          // creates + wires the window once
    /** Adds a video file as a new track of the active document (or opens it
        as a document when none is open). Used for drops on the video area. */
    void addVideoTrackFromFile (const juce::File& f);

    // Video track context-menu actions (ids from VideoTracksPanel).
    void handleVideoTrackMenu (int id, int idx);
    void showVideoAlignDialog (int idx);      // manual time offset
    void showVideoAdjustDialog (int idx);     // live contrast/brightness dialog
    void startVideoAutoSync (int idx);        // background NCC job -> sets the offset
    void copyVideoAudioToChannels (int idx);  // decode video audio into new channels
    void addVideoTrackFromPicker();           // File > ... picker, adds a track at 0
    void removeVideoTrack (int idx);
    void toggleVideoSolo (int idx);
    /** Recomputes the engine's solo playback from the active document's
        video tracks (decodes the soloed track's own audio). Called on doc
        switch and after any offset/solo change. */
    void refreshVideoSolo();
    std::unique_ptr<SpectralControlsPanel> controlsPanel;
    std::unique_ptr<MarkerListPanel> markersPanel;
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    std::unique_ptr<TranscriptionPanel> transcriptionPanel;   // Whisper segments (Phase 3)
#endif
    std::unique_ptr<SelectionPanel> selectionPanel;           // named selections + clone & multiply
    std::unique_ptr<DrawingPanel> drawingPanel;               // spectrogram drawings (bottom group)
    std::unique_ptr<TablesPanel> tablesPanel;                 // imported CSV tables (top group)
    std::unique_ptr<HistoryPanel> historyPanel;
    std::unique_ptr<ActionsPanel> actionsPanel;              // recorded actions (top group)
    std::unique_ptr<PythonPanel> pythonPanel;                 // user Python scripts (top group)

    // Effects > Stem Separation window (at most one open at a time).
    std::unique_ptr<StemSeparationWindow> stemWin_;

    // Effects > Super Resolution window (at most one open at a time).
    std::unique_ptr<SuperResolutionWindow> superResWin_;


    // Zoom/pan scrollbars: time bar at the bottom, one vertical
    // scrollbar per view on its right edge.
    std::unique_ptr<TimeScrollBar> timeBar;
    std::unique_ptr<ValueScrollBar> waveVScroll, specVScroll;

    // Dedicated tables track (Tables panel mode 2): a strip above the data
    // views with its own separator + vertical scrollbar.
    std::unique_ptr<TablesTrackView> tablesTrack;
    std::unique_ptr<ViewSplitter> tablesSep;
    std::unique_ptr<ValueScrollBar> tablesVScroll;
    double tablesTrackRatio = 0.18;   // fraction of the main area height

    // Status bar at the very bottom: mouse probe + selection/playhead times.
    std::unique_ptr<StatusBar> statusBar;

    // Single app-wide tooltip window (JUCE shows tooltips for any component
    // implementing juce::TooltipClient once this exists).
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;   // null while Help > Show Tooltips is off
    void applyTooltipsSetting();   // tooltipWindow <- app.settings.showTooltips

    int controlClients_ = 0;                  // AI control clients connected (status bar)

    // ---- Docking workspace -------------------------------------------------
    // Declared last: destroyed first, while the panel contents they show
    // (owned above) still exist.
    void layoutEditor();                      // place the views inside the Editor panel
    void handlePanelOpenChanged (const juce::String& id, bool open);
    void applyDockLayout (const juce::var& layout);
    juce::var legacyLeftLayout() const;       // arrangement saved by the old two-group column
    std::vector<sc::dock::Workspaces::Entry> builtInWorkspaces() const;
    void buildWorkspaceMenu (juce::PopupMenu& m, const juce::String& clickedId);
    void handleWorkspaceCommand (int menuId, const juce::String& clickedId = {});
    void activeWindowStatusChanged() override;   // floating panel windows stay above this one

    sc::dock::Workspaces workspaces_;
    std::unique_ptr<juce::Component> editorPanel_;          // waveform + spectrogram + scrollbars
    // One Editor tab per open document, titled with its file name. The
    // permanent "editor" panel holds one document ("Editor" when none is
    // open); every other document gets an "editor-<FileDoc::id>" panel in the
    // same group. They all share editorPanel_ (the views), which moves into
    // the active document's tab. Declared before dock_: destroyed after it.
    std::map<juce::String, std::unique_ptr<juce::Component>> editorSlots_;   // panel id -> tab content
    std::map<int, juce::String> editorTabOfDoc_;                             // FileDoc::id -> panel id
    void syncEditorTabs();             // tabs <- open documents; views -> the active one's tab
    void snapshotEditorIntoItsTab();   // the active document's tab keeps a picture of the views
    void refreshEditorTabTitles();     // tab titles <- file names (cheap: after Save As...)
    void activateDocById (int docId);  // a document's tab was clicked
    std::unique_ptr<sc::dock::WorkspaceBar> workspaceBar_;  // right of the menu bar
    std::unique_ptr<sc::dock::DockManager> dock_;
    bool dockLayoutDirty_ = false;            // save pending (debounced in the timer)
    juce::uint32 dockLayoutDirtyAt_ = 0;
    bool dockLoading_ = false;                // applying a layout: no persistence / video side effects
    bool videoHideQuietly_ = false;           // Window > Video hides the picture: closing its tab must not pop its own window
    bool floatersHiddenForMinimise_ = false;  // floating panel windows hidden with this one
    juce::String workspaceMenuTarget_;        // workspace right-clicked in the bar


    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
