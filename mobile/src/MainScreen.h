// SpectroCraft Mobile - the main screen.
//
// Phones (MainScreen::layoutPhone): one panel at a time under the views
// (portrait) or beside them (landscape).
//
//   portrait                         landscape
//   +---------------------------+   +-------------------+-----------+
//   | file name     W S W+S Save|   | file name       W S W+S Save  |
//   | waveform                  |   | waveform          | panel     |
//   | spectrogram               |   | spectrogram       |           |
//   | REC  >  []  Loop  Video  |   | REC > [] ...      | tabs      |
//   | panel (Analysis, ...)     |   +-------------------+-----------+
//   | Files Edit Analyse Gen ...|
//   +---------------------------+
//
// Tablets (smaller side >= 600 points, MainScreen::layoutTablet): two panel
// groups shown at once, as in the desktop's left column; see ScreenLayout.h.
#pragma once
#include <JuceHeader.h>
#include "AppContext.h"
#include "AnalysisPane.h"
#include "CalibrationPane.h"
#include "EditPane.h"
#include "FilesPane.h"
#include "GeneratorPane.h"
#include "IconButton.h"
#include "InputPane.h"
#include "ScreenLayout.h"
#include "SpectrogramPane.h"
#include "Splitter.h"
#include "VideoPane.h"
#include "WaveformPane.h"

namespace scm
{
class LevelMeter : public juce::Component, private juce::Timer
{
public:
    explicit LevelMeter (AppContext& c);
    void paint (juce::Graphics&) override;
private:
    void timerCallback() override { repaint(); }
    AppContext& ctx;
};

class MainScreen : public juce::Component,
                   private juce::ChangeListener,
                   private juce::Timer
{
public:
    explicit MainScreen (AppContext&);
    ~MainScreen() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    /** A hardware keyboard: the desktop's shortcuts (screen::commandForKey). */
    bool keyPressed (const juce::KeyPress&) override;

    /** The audio device came up (or changed): refresh the input-dependent panes. */
    void audioReady (const juce::String& error);
    /** App going to the background (iOS / Android). */
    void suspended();
    /** Opens a panel (0 Files, 1 Analyse, 2 Generate, 3 Calibrate, 4 Input, 5 Edit). */
    void showTab (int t);
    /** Development hook: the analysis mode (0 FFT Linear, 1 FFT Log, 2 1/3 octave). */
    void setAnalysisMode (int m) { analysisPane->setMode (m); }
    void showPreciseAt (float fx, float fy) { spectrogram.showPreciseAt (fx, fy); }

private:
    enum class Layout { Both, Waveform, Spectrogram };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void toggleRecord();
    void startRecording();
    void stopRecording();
    void togglePlay();
    void stopPlayback();
    void toggleVideo();
    /** The video pane follows the camera, the open file's movie and the transport. */
    void refreshVideo();
    void selectTab (int t);
    void setLayout (Layout l);
    void updateTransport();
    void documentReplaced();
    void zoomToSelection();
    juce::Component* tabContent (int t);
    /** Shows the panels the form and the state ask for, and their tabs. */
    void applyPanels();
    bool panelShown (int t) const;
    void togglePanels();
    void runCommand (screen::Command);
    void layoutPhone (juce::Rectangle<int> r);
    void layoutTablet (juce::Rectangle<int> r);
    void layoutTransport (juce::Rectangle<int> r, bool singleRow);
    void layoutViews (juce::Rectangle<int> views);
    juce::Rectangle<int> safeBounds() const { return appliedInsets.subtractedFrom (getLocalBounds()); }
    void loadTabletState();
    void saveTabletState();

    AppContext& ctx;

    // top bar
    juce::Label title;
    juce::TextButton layoutW { "W" }, layoutS { "S" }, layoutWS { "W+S" }, saveButton { "Save" };
    // tablet only: show / hide the panels, undo, redo (the desktop's toolbar)
    IconButton panelsButton { "Panels", IconButton::Icon::Panels },
               undoButton { "Undo", IconButton::Icon::Undo },
               redoButton { "Redo", IconButton::Icon::Redo };

    // views
    WaveformPane waveform;
    SpectrogramPane spectrogram;
    Layout layout = Layout::Both;

    // transport
    juce::TextButton recordButton { "REC" }, playButton { "Play" }, stopButton { "Stop" }, videoButton { "Video" };
    juce::ComboBox modeBox;
    juce::Label timeLabel;
    LevelMeter meter;

    // panels
    std::unique_ptr<FilesPane> filesPane;
    std::unique_ptr<AnalysisPane> analysisPane;
    std::unique_ptr<GeneratorPane> generatorPane;
    std::unique_ptr<CalibrationPane> calibrationPane;
    std::unique_ptr<InputPane> inputPane;
    std::unique_ptr<EditPane> editPane;
    juce::OwnedArray<juce::TextButton> tabButtons;
    int currentTab = screen::Analysis;   // phone: the panel shown
    bool panelOpen = true;               // phone: tap the shown tab again to hide it

    // tablet
    screen::Form form = screen::Form::Phone;   // of the last layout
    screen::TabletState tablet;
    screen::TabletFrame frame;                 // the last tablet layout (paint)
    Splitter panelsSplitter, groupSplitter;

    bool wasPlaying = false;   // to notice the engine stopping at the end
    juce::BorderSize<int> appliedInsets;   // the safe area the layout used
    VideoPane videoPane;
    bool reviewMovie = false;   // Play shows the take's movie instead of the live camera; Stop goes back

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainScreen)
};
} // namespace scm
