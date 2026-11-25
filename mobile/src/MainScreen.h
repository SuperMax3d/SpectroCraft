// SpectroCraft Mobile - the main screen.
//
//   portrait                         landscape
//   +---------------------------+   +-------------------+-----------+
//   | file name     W S W+S Save|   | file name       W S W+S Save  |
//   | waveform                  |   | waveform          | panel     |
//   | spectrogram               |   | spectrogram       |           |
//   | REC  >  []  Loop  Video  |   | REC > [] ...      | tabs      |
//   | panel (Analysis, ...)     |   +-------------------+-----------+
//   | Files Analyse Gen Cal ... |
//   +---------------------------+
#pragma once
#include <JuceHeader.h>
#include "AppContext.h"
#include "AnalysisPane.h"
#include "CalibrationPane.h"
#include "EditPane.h"
#include "FilesPane.h"
#include "GeneratorPane.h"
#include "InputPane.h"
#include "SpectrogramPane.h"
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

    /** The audio device came up (or changed): refresh the input-dependent panes. */
    void audioReady (const juce::String& error);
    /** App going to the background (iOS / Android). */
    void suspended();
    /** Opens a panel (0 Files, 1 Analyse, 2 Generate, 3 Calibrate, 4 Input, 5 Edit). */
    void showTab (int t) { panelOpen = false; currentTab = -1; selectTab (t); }

private:
    enum class Layout { Both, Waveform, Spectrogram };
    enum Tab { Files = 0, Analysis, Generator, Calibrate, Input, Edit, NumTabs };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void toggleRecord();
    void startRecording();
    void stopRecording();
    void togglePlay();
    void stopPlayback();
    void toggleVideo();
    void selectTab (int t);
    void setLayout (Layout l);
    void updateTransport();
    void documentReplaced();
    void zoomToSelection();
    juce::Component* tabContent (int t);

    AppContext& ctx;

    // top bar
    juce::Label title;
    juce::TextButton layoutW { "W" }, layoutS { "S" }, layoutWS { "W+S" }, saveButton { "Save" };

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
    int currentTab = Analysis;
    bool panelOpen = true;
    juce::Component* videoPreview = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainScreen)
};
} // namespace scm
