// SpectroCraft Mobile - the recordings: list, open, save, share, delete, and
// opening a WAV from elsewhere (the Files app / the Android file picker).
#pragma once
#include <JuceHeader.h>
#include "AppContext.h"
#include "IconButton.h"

namespace scm
{
class FilesPane : public juce::Component,
                  private juce::ListBoxModel,
                  private juce::ChangeListener
{
public:
    explicit FilesPane (AppContext&);
    ~FilesPane() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

    /** Opens a file, asking first when the open one has unsaved changes. */
    void openFile (const juce::File& f, const char* source = "list");
    /** When the open document has unsaved changes (an unsaved take, or edits):
        Save / Discard / Cancel, then `then` unless cancelled. */
    void confirmDiscard (std::function<void()> then);

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void updateStatus();   // the current file's name, Save and Share
    void openFromElsewhere();
    void share();
    /** The file Share and Delete act on: the selected recording, else the open one. */
    juce::File targetFile() const;
    void saveCurrent();
    void deleteSelected();

    AppContext& ctx;
    juce::Label current;
    IconButton openButton { "Open a WAV file", IconButton::Icon::Open },
               saveButton { "Save", IconButton::Icon::Save },
               shareButton { "Share", IconButton::Icon::Share },
               deleteButton { "Delete", IconButton::Icon::Delete };
    juce::ListBox list;
    juce::Array<juce::File> files;
    juce::File listedFor;   // the document file the folder was last listed for
    std::unique_ptr<juce::FileChooser> chooser;
    juce::ScopedMessageBox shareBox;
};
} // namespace scm
