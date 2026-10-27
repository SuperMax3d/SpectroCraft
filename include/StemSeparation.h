// SpectroCraft - Stem Separation window (drives the audio.cpp CLI tool).
//
// Non-modal document window so the user can keep working in the main window
// while configuring runs. The current track selection is shown live at the
// top; "Include/Exclude Selection" captures it as a time range relative to
// the input file that will be extracted (the current selection, or the whole
// file when nothing is selected).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "AudioCpp.h"

struct Application;   // defined in Application.h (kept out of this header)

/** One description entry. In simple mode the window holds a single one. */
struct StemDescEntry
{
    juce::String text;
    std::vector<sc::acpp::Anchor> anchors;   // times relative to extracted input
};

class StemSeparationWindow : public juce::DocumentWindow,
                             private juce::Timer
{
public:
    /** @param app  the owning application (must outlive this window). */
    explicit StemSeparationWindow(Application& app);
    ~StemSeparationWindow() override;

    void timerCallback() override;

public:
    // Called by the DescRow buttons (a separate class), so these are public.
    void addAnchorFromSelection(int descIndex, bool include);
    void removeAnchor(int descIndex, int anchorIndex);
    void removeDesc(int index);

private:
    // --- layout -----------------------------------------------------------
    void resized() override;
    int computeHeight() const;
    void refreshModelList();
    void refreshStatusIdle();
    void rebuildRows();
    void setAdvanced(bool on);
    juce::String selectionRangeText() const;

    // --- actions ------------------------------------------------------------
    void doSelectAll();
    void openDownloadDialog();
    void startRun();

    // --- worker -------------------------------------------------------------
    struct RunJob;   // defined in the .cpp
    void runWorker(std::unique_ptr<RunJob> job);

    Application& app_;

    // top section
    juce::Label selRangeLabel_;
    juce::TextButton selectAllBtn_;
    juce::ComboBox modelCombo_;
    juce::ComboBox backendCombo_;
    juce::ToggleButton extractOrigToggle_;
    juce::TextEditor descEdit_;          // simple mode
    juce::TextButton advancedBtn_;       // expands the multi-description area

    // advanced section
    bool advanced_ = false;
    std::vector<StemDescEntry> descs_;
    juce::Component rowsHost_;           // owns one DescRow per entry
    juce::Label advancedHint_;

    juce::Label statusLabel_;
    juce::TextButton downloadBtn_;
    juce::TextButton runBtn_;

    // Guards the detached worker threads (download / run) against this
    // window being closed while they are still active: workers capture a copy
    // of the shared flag and refuse to touch `this` once it is cleared.
    std::shared_ptr<std::atomic<bool>> alive_;

    std::atomic<bool> running_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparationWindow);
};
