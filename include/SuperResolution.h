#pragma once
// SpectroCraft - Effects > Super Resolution (external audio.cpp CLI tool).
#include <JuceHeader.h>
#include "Application.h"

/** Non-modal window driving the external super-resolution CLI: pick a range,
    choose FlashSR or AudioSR + model + backend, run it on a worker thread and
    apply the enhanced mono 48 kHz result (new file / overwrite / append). */
class SuperResolutionWindow : public juce::DocumentWindow,
                              private juce::TextEditor::Listener
{
public:
    explicit SuperResolutionWindow(Application& app);
    ~SuperResolutionWindow() override;

    void userTriedToCloseWindow() override;
    void closeButtonPressed() override;

    // The owner (MainWindow) releases the unique_ptr that owns this window.
    std::function<void()> onCloseRequested;

    // Test accessors.
    juce::ComboBox* modeComboForTest() { return &modeCombo_; }
    juce::ComboBox* modelComboForTest() { return &modelCombo_; }
    juce::ComboBox* outputComboForTest() { return &outputCombo_; }
    int selectedModeId() const { return modeCombo_.getSelectedItemIndex () + 1; }   // 1=FlashSR 2=AudioSR
    /** Test-only: redirect model discovery away from %APPDATA%. */
    void setModelsRootForTest(const juce::File& root) { modelsRoot_ = root; refreshModelList (); }
    juce::String startTextForTest() const { return startEdit_.getText (); }
    juce::String endTextForTest() const { return endEdit_.getText (); }

private:
    // Empty override = the default %APPDATA%/SpectroCraft/models/audiocpp location.
    juce::File modelsRoot() const;

private:
    void resized() override;
    void textEditorTextChanged (juce::TextEditor&) override;
    void refreshModelList();
    void refreshStatusIdle();
    void refreshDurationLabel();
    void doSelectAll();
    void openDownloadDialog();
    void startRun();

    struct RunJob;   // defined in the .cpp
    void runWorker(std::unique_ptr<RunJob> job);

    Application& app_;

    // Test-only override of the models root directory.
    juce::File modelsRoot_;

    // Top section: the extraction range in seconds. Seeded from the track
    // selection when the window opens, then editable and independent of later
    // changes to that selection.
    juce::Label startLbl_, endLbl_, durLbl_;
    juce::TextEditor startEdit_, endEdit_;
    juce::TextButton selectAllBtn_;

    juce::Label modeLbl_;
    juce::ComboBox modeCombo_;           // 1=FlashSR 2=AudioSR
    juce::Label modelLbl_, backendLbl_;
    juce::ComboBox modelCombo_;
    juce::ComboBox backendCombo_;

    juce::Label statusLabel_;
    juce::ComboBox outputCombo_;         // 1=new file 2=overwrite current 3=append to current
    juce::TextButton cancelBtn_;         // closes the window without running
    juce::TextButton downloadBtn_;
    juce::TextButton runBtn_;

    // Guards the detached worker threads (download / run) against this
    // window being closed while they are still active: workers capture a copy
    // of the shared flag and refuse to touch `this` once it is cleared.
    std::shared_ptr<std::atomic<bool>> alive_;

    std::atomic<bool> running_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SuperResolutionWindow);
};
