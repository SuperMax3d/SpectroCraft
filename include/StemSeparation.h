// SpectroCraft - Stem Separation window (drives the audio.cpp CLI tool).
//
// Non-modal document window so the user can keep working in the main window
// while configuring runs. The extraction range (start/end seconds) is seeded
// from the track selection once when the window opens and is then editable;
// later changes to that selection do not update it. "Include/Exclude
// Selection" captures the current selection as a time-range anchor relative
// to the input file that will be extracted.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
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

class StemSeparationWindow : public juce::DocumentWindow
{
public:
    /** @param app  the owning application (must outlive this window). */
    explicit StemSeparationWindow(Application& app);
    ~StemSeparationWindow() override;

    // The native title bar's X button routes to userTriedToCloseWindow()
    // (DocumentWindow::closeButtonPressed is never called for it in this JUCE
    // build, where both base implementations are no-op asserts). Call the
    // owner-provided callback so MainWindow can release its unique_ptr.
    void userTriedToCloseWindow() override;
    void closeButtonPressed() override;

public:
    // Set by the owner (MainWindow) to take back ownership when the window is
    // closed via X; called from within this object, so it must not touch `this`.
    std::function<void()> onCloseRequested;

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

    // --- actions ------------------------------------------------------------
    void doSelectAll();
    void openDownloadDialog();
    void startRun();

    // --- worker -------------------------------------------------------------
    struct RunJob;   // defined in the .cpp
    void runWorker(std::unique_ptr<RunJob> job);

    Application& app_;

    // top section: the extraction range in seconds. Seeded from the track
    // selection when the window opens, then editable and independent of later
    // changes to that selection (advanced mode needs stable per-run ranges).
    juce::Label startLbl_, endLbl_;
    juce::TextEditor startEdit_, endEdit_;
    juce::TextButton selectAllBtn_;
    juce::ComboBox modelCombo_;
    juce::ComboBox backendCombo_;
    juce::ToggleButton extractOrigToggle_;
    juce::TextEditor descEdit_;          // simple mode
    juce::TextButton advancedBtn_;       // expands the multi-description area
    juce::TextButton addDescBtn_;        // advanced: append another description

    // advanced section
    bool advanced_ = false;
    std::vector<StemDescEntry> descs_;
    juce::Component rowsHost_;           // owns one DescRow per entry
    juce::Label advancedHint_;

    juce::Label statusLabel_;
    juce::TextButton cancelBtn_;         // closes the window without running
    juce::TextButton downloadBtn_;
    juce::TextButton runBtn_;

    // Guards the detached worker threads (download / run) against this
    // window being closed while they are still active: workers capture a copy
    // of the shared flag and refuse to touch `this` once it is cleared.
    std::shared_ptr<std::atomic<bool>> alive_;

    std::atomic<bool> running_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparationWindow);
};
