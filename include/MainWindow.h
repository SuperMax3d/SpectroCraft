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

class MainWindow : public juce::DocumentWindow,
                   private juce::KeyListener,
                   public juce::MenuBarModel,
                   public juce::Timer,
                   public juce::FileDragAndDropTarget
{
public:
    MainWindow (Application& app, const juce::String& name);
    ~MainWindow();

    void resized() override;
    void closeButtonPressed() override;
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

    void openFile (const juce::File& file);   // routes large files to the async path
    void openFileWithProgress (const juce::File& file);  // background load + progress dialog
    void runSaveAsync (const juce::File& file, bool saveAs);      // background save + dialog
    void exportSpectralAsync (const juce::File& file);            // background PNG render + dialog
    void handleFileOpen();
    void handleFileSave();
    void handleFileSaveAs();
    void updateTitle();
    void setupViews();
    void activateDocUI();   // reset both views + sync panels after a doc switch
    juce::String statusBarFileName() const;   // name currently shown in the footer
    bool waveformVisible() const { return showWaveformView; }      // View menu state
    bool spectrogramVisible() const { return showSpectralView; }
    juce::String waveformTooltip() const { return waveformView->getTooltip(); }
    juce::String spectralTooltip() const { return spectralView->getTooltip(); }
    void applyTimeWindow (double t0, double t1); // push one time window to BOTH views
    void refreshAfterEdit();   // clamp ranges, repaint waveform, re-render spectral image
    void refreshPanels();
    void applyColourMap (ColourMap m);
    void zoomHorizontal (double factor);   // both views
    void updateScrollBars();               // keep scrollbars in sync with the views
    void zoomVerticalBoth (double factor); // waveform amplitude + spectral frequency
    void zoomToFit();
    void zoomToSelection();   // fit the current selection in both views
    void zoomOutFull();       // show the whole file in both views
    void cycleViewVisibility (bool forward);  // F9: both -> waveform -> spectrogram
    void handleContextMenuAction (int id);  // right-click menu actions from either view
    void showChangeGainDialog();
    /** Builds (but does not run modally) the Change Gain dialog. The caller
        owns and must delete it; used by showChangeGainDialog() and tests. */
    juce::AlertWindow* createGainDialog();
    void showResampleDialog();
    /** Builds (but does not run modally) the Resample dialog. The caller owns
        and must delete it; used by showResampleDialog() and tests. */
    juce::AlertWindow* createResampleDialog();
    void runResampleAsync (double newRate);   // background resample + progress dialog
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    /** Builds (but does not run modally) the Noise Suppression dialog. The caller
        owns and must delete it; used by showNoiseSuppressDialog() and tests. */
    juce::AlertWindow* createNoiseSuppressDialog();
    void showNoiseSuppressDialog();
    void runNoiseSuppressAsync (const std::string& modelId, const std::string& device,
                                float attenLimDb = 0.f, bool df3PostFilter = false);
#endif
    void handleSplitterDrag (const juce::MouseEvent& e);      // waveform/spectral divider
    void handleLeftSplitterDrag (const juce::MouseEvent& e);  // left column / main area
    void handleTopSplitterDrag (const juce::MouseEvent& e);   // top tabs / bottom tabs
    void syncSharedState();   // push app state into views + panels

    enum MenuIDs
    {
        FileOpen = 1000, FileClose, FileSave, FileSaveAs, FileExportSpectral, FileQuit,
        EditUndo = 2000, EditRedo, EditCut, EditCopy, EditPaste, EditDelete,
        EditSelectAll, EditSnapToZeroCrossings,
        ViewZoomIn = 3000, ViewZoomOut, ViewZoomFit, ViewZoomToSelection, ViewZoomOutFull,
        ViewVerticalZoomIn,
        ViewVerticalZoomOut, ViewShowWaveform, ViewShowSpectral, ViewShowAllChannels, ViewColourMap,
        TransportPlay = 4000, TransportStop, TransportPause, TransportLoop,
        ToolsSilence = 5000, ToolsFadeIn, ToolsFadeOut, ToolsNormalize, ToolsChangeGain, ToolsReverse,
        ToolsResample,
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        ToolsNoiseSuppress,   // AI noise suppression (built with -DSPECTRO_ENABLE_AI=ON)
#endif
        HelpAbout = 6000,
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

    // Left-side panels
    std::unique_ptr<FileListPanel> fileListPanel;
    std::unique_ptr<FFTAnalysisView> fftPanel;
    std::unique_ptr<SpectralControlsPanel> controlsPanel;
    std::unique_ptr<MarkerListPanel> markersPanel;
    std::unique_ptr<HistoryPanel> historyPanel;

    // Zoom/pan scrollbars: time bar at the bottom, one vertical
    // scrollbar per view on its right edge.
    std::unique_ptr<TimeScrollBar> timeBar;
    std::unique_ptr<ValueScrollBar> waveVScroll, specVScroll;

    // Status bar at the very bottom: mouse probe + selection/playhead times.
    std::unique_ptr<StatusBar> statusBar;

    // Single app-wide tooltip window (JUCE shows tooltips for any component
    // implementing juce::TooltipClient once this exists).
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    juce::TabbedComponent topTabs { juce::TabbedButtonBar::TabsAtTop },
                            bottomTabs { juce::TabbedButtonBar::TabsAtTop };


    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
