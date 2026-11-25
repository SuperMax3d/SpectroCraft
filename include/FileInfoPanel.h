#pragma once
// SpectroCraft - File Info panel (issue #67): the WAV's text metadata (RIFF
// LIST/INFO fields and the DISP title, as Adobe Audition's File Info > Text
// Fields lists them), editable; Comments and Subject take basic Markdown.
#include <JuceHeader.h>
#include <memory>
#include <vector>

class Application;

/** A multi-line Markdown field: the source in a text editor, formatting
    buttons that edit it (Bold, Italic, Code, Heading, Bullet, Numbered), and
    a Preview toggle that shows it formatted. */
class MarkdownField : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit MarkdownField (const juce::String& title);
    juce::TextEditor& editor() noexcept { return editor_; }
    enum Format { bold, italic, code, heading, bullet, numbered };
    /** Applies a format to the editor's selection (its buttons call it). */
    void applyFormat (Format f);
    void setPreview (bool on);
    bool isPreview() const noexcept { return preview_; }
    void setEditable (bool on);
    /** The editor's text changed (typed or by a format). */
    std::function<void()> onChange;
    void resized() override;
    void paint (juce::Graphics& g) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;   // the preview: back to editing

private:
    juce::Label title_;
    juce::TextEditor editor_;
    std::vector<std::unique_ptr<juce::TextButton>> buttons_;
    std::unique_ptr<juce::TextButton> previewBtn_;
    juce::Rectangle<int> textArea_;
    bool preview_ = false;
};

class FileInfoPanel : public juce::Component
{
public:
    explicit FileInfoPanel (Application& app);
    ~FileInfoPanel() override;
    /** Shows the active document's fields; editable when it is a WAV file.
        An editor being typed in keeps its text. */
    void refresh();
    /** A field was edited (the document is now unsaved): MainWindow updates
        the title. */
    std::function<void()> onEdited;
    std::function<void()> onCloseRequested;

    // For the tests.
    juce::TextEditor* editorFor (const juce::String& id) const;
    MarkdownField* markdownFor (const juce::String& id) const;
    bool isEditable() const noexcept { return editable_; }
    juce::String noteText() const { return note_.getText(); }

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    Application& app;
    juce::Viewport view_;
    juce::Component content_;
    juce::Label note_;
    struct Row
    {
        juce::String id;
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::TextEditor> editor;
        std::unique_ptr<MarkdownField> markdown;
    };
    std::vector<std::unique_ptr<Row>> rows_;
    int shownDocId_ = -1;
    bool editable_ = false;
    bool loading_ = false;
    void commit (const juce::String& id, const juce::String& text);
    void layoutContent();
    JUCE_DECLARE_NON_COPYABLE (FileInfoPanel)
};
