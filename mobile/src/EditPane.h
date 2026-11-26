// SpectroCraft Mobile - editing the selection (delete, change gain, undo) and
// the display settings (view layout, spectrogram range, channel).
#pragma once
#include <JuceHeader.h>
#include "TouchControls.h"
#include "AppContext.h"

namespace scm
{
class EditPane : public juce::Component, private juce::ChangeListener
{
public:
    explicit EditPane (AppContext&);
    ~EditPane() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void (int channel)> onChannelChanged;
    std::function<void (bool log)> onLogFrequencyChanged;
    std::function<void()> onDisplayChanged;   // dB range / colour map
    std::function<void()> onZoomToSelection;
    /** A crop was written to a new file: open it (the screen saves pending
        edits of the current file first). */
    std::function<void (const juce::File&)> onOpenFile;

    /** The spectrogram's display options (log / linear, dB range). On the
        phone they are in the top bar's menu (issue #79) and Edit shows only
        the channel; the tablet keeps them here. */
    void showSpectrogramOptions (bool show);
    bool logFrequency() const { return axisBox.getSelectedId() == 1; }
    int displayRangeDb() const { return rangeBox.getSelectedId(); }
    /** As if chosen in Edit (the callbacks run). */
    void setLogFrequency (bool log) { axisBox.setSelectedId (log ? 1 : 2, juce::sendNotificationSync); }
    void setDisplayRangeDb (int db) { rangeBox.setSelectedId (db, juce::sendNotificationSync); }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refresh();

    AppContext& ctx;
    juce::Label selLabel;
    juce::TextButton selectAll { "Select all" }, zoomSel { "Zoom to selection" },
                     deleteButton { "Delete" }, undoButton { "Undo" }, redoButton { "Redo" },
                     cropButton { "Crop" }, cropNewButton { "Crop to new recording" },
                     gainButton { "Apply gain" };
    TouchSlider gain;
    juce::Label displayTitle { {}, "Display" };
    juce::ComboBox channelBox, axisBox, rangeBox;
    juce::Viewport viewport;
    juce::Component content;
};
} // namespace scm
