// SpectroCraft - File Info panel (issue #67); see include/FileInfoPanel.h
#include "FileInfoPanel.h"
#include "Application.h"
#include "Markdown.h"
#include "RiffInfo.h"

namespace
{
const juce::Colour kBg (45, 45, 48), kField (35, 35, 38), kOutline (72, 72, 78), kInk (225, 225, 230);

void styleEditor (juce::TextEditor& e)
{
    e.setFont (juce::FontOptions (12.5f));
    e.setColour (juce::TextEditor::backgroundColourId, kField);
    e.setColour (juce::TextEditor::textColourId, kInk);
    e.setColour (juce::TextEditor::outlineColourId, kOutline);
    e.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xff4ea1f5));
}
} // namespace

// ---------------------------------------------------------------- Markdown field

MarkdownField::MarkdownField (const juce::String& title)
{
    title_.setText (title, juce::dontSendNotification);
    title_.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    title_.setColour (juce::Label::textColourId, kInk);
    addAndMakeVisible (title_);

    editor_.setMultiLine (true, true);
    editor_.setReturnKeyStartsNewLine (true);
    editor_.setScrollbarsShown (true);
    styleEditor (editor_);
    editor_.onTextChange = [this] { if (onChange) onChange(); };
    addAndMakeVisible (editor_);

    struct B { const char* text; const char* tip; Format f; };
    const B specs[] = {
        { "B",   "Bold: **text**", bold },
        { "I",   "Italic: *text*", italic },
        { "`",   "Code: `text`", code },
        { "H",   "Heading: # at the start of the line", heading },
        { "-",   "Bullet list: - at the start of each line", bullet },
        { "1.",  "Numbered list: 1. at the start of each line", numbered },
    };
    for (const auto& s : specs)
    {
        auto b = std::make_unique<juce::TextButton> (s.text);
        b->setTooltip (juce::String (s.tip) + ". Click again to remove it.");
        const Format f = s.f;
        b->onClick = [this, f] { applyFormat (f); };
        addAndMakeVisible (*b);
        buttons_.push_back (std::move (b));
    }
    previewBtn_ = std::make_unique<juce::TextButton> ("Preview");
    previewBtn_->setClickingTogglesState (true);
    previewBtn_->setTooltip ("Shows the text formatted (Markdown). Click again, or double-click the text, to edit.");
    previewBtn_->onClick = [this] { setPreview (previewBtn_->getToggleState()); };
    addAndMakeVisible (*previewBtn_);
}

void MarkdownField::applyFormat (Format f)
{
    if (preview_) setPreview (false);
    const auto text = editor_.getText();
    const auto sel = editor_.getHighlightedRegion();
    juce::Range<int> next;
    juce::String out;
    switch (f)
    {
        case bold:     out = sc::markdown::wrap (text, sel, "**", next); break;
        case italic:   out = sc::markdown::wrap (text, sel, "*", next); break;
        case code:     out = sc::markdown::wrap (text, sel, "`", next); break;
        case heading:  out = sc::markdown::prefixLines (text, sel, "# ", next); break;
        case bullet:   out = sc::markdown::prefixLines (text, sel, "- ", next); break;
        case numbered: out = sc::markdown::prefixLines (text, sel, "1. ", next); break;
    }
    editor_.setText (out, false);
    editor_.setHighlightedRegion (next);
    if (onChange) onChange();
}

void MarkdownField::setPreview (bool on)
{
    preview_ = on;
    previewBtn_->setToggleState (on, juce::dontSendNotification);
    editor_.setVisible (! on);
    repaint();
}

void MarkdownField::setEditable (bool on)
{
    editor_.setReadOnly (! on);
    for (auto& b : buttons_)
        b->setEnabled (on);
}

void MarkdownField::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (24);
    previewBtn_->setBounds (top.removeFromRight (62).reduced (1, 2));
    top.removeFromRight (6);
    for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it)
        (*it)->setBounds (top.removeFromRight (26).reduced (1, 2));
    title_.setBounds (top);
    r.removeFromTop (2);
    textArea_ = r;
    editor_.setBounds (r);
}

void MarkdownField::paint (juce::Graphics& g)
{
    if (! preview_)
        return;
    g.setColour (kField);
    g.fillRect (textArea_);
    g.setColour (kOutline);
    g.drawRect (textArea_);
    juce::TextLayout layout;
    const auto area = textArea_.reduced (6, 4).toFloat();
    layout.createLayout (sc::markdown::toAttributed (editor_.getText(), 12.5f, kInk), area.getWidth());
    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (textArea_.reduced (1));
    layout.draw (g, area.withHeight (std::max (area.getHeight(), layout.getHeight())));
}

void MarkdownField::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (preview_ && textArea_.contains (e.getPosition()))
    {
        setPreview (false);
        editor_.grabKeyboardFocus();
    }
}

// ---------------------------------------------------------------- panel

FileInfoPanel::FileInfoPanel (Application& a) : app (a)
{
    setComponentID ("file-info-panel");
    note_.setFont (juce::FontOptions (12.0f, juce::Font::italic));
    note_.setColour (juce::Label::textColourId, juce::Colour (0xffb0b0b8));
    content_.addAndMakeVisible (note_);

    for (const auto& f : sc::riffinfo::fields())
    {
        auto row = std::make_unique<Row>();
        row->id = f.id;
        const juce::String id (f.id);
        const juce::String tip = juce::String (f.label) + " (" + (id == "DISP" ? juce::String ("DISP chunk")
                                                                               : "RIFF INFO " + id) + ")";
        if (f.multiline)
        {
            row->markdown = std::make_unique<MarkdownField> (f.label);
            row->markdown->setTooltip (tip);
            auto* md = row->markdown.get();
            md->onChange = [this, id, md] { commit (id, md->editor().getText()); };
            content_.addAndMakeVisible (*row->markdown);
        }
        else
        {
            row->label = std::make_unique<juce::Label> (juce::String(), juce::String (f.label));
            row->label->setFont (juce::FontOptions (12.5f));
            row->label->setColour (juce::Label::textColourId, kInk);
            row->label->setJustificationType (juce::Justification::centredRight);
            row->editor = std::make_unique<juce::TextEditor>();
            styleEditor (*row->editor);
            row->editor->setTooltip (tip);
            auto* ed = row->editor.get();
            ed->onTextChange = [this, id, ed] { commit (id, ed->getText()); };
            content_.addAndMakeVisible (*row->label);
            content_.addAndMakeVisible (*row->editor);
        }
        rows_.push_back (std::move (row));
    }
    view_.setViewedComponent (&content_, false);
    view_.setScrollBarsShown (true, false);
    addAndMakeVisible (view_);
    refresh();
}

FileInfoPanel::~FileInfoPanel() = default;

juce::TextEditor* FileInfoPanel::editorFor (const juce::String& id) const
{
    for (const auto& r : rows_)
        if (r->id == id)
            return r->editor != nullptr ? r->editor.get() : &r->markdown->editor();
    return nullptr;
}

MarkdownField* FileInfoPanel::markdownFor (const juce::String& id) const
{
    for (const auto& r : rows_)
        if (r->id == id)
            return r->markdown.get();
    return nullptr;
}

void FileInfoPanel::refresh()
{
    const FileDoc* d = app.doc();
    const bool wav = d != nullptr && d->engine.isFileLoaded()
                     && (d->file.getFullPathName().isEmpty() || d->file.getFileExtension().equalsIgnoreCase (".wav"));
    editable_ = wav;
    note_.setText (d == nullptr ? "No file is open."
                   : wav ? "Saved inside the WAV file (RIFF INFO), with the next Save or Save Metadata."
                         : "These fields exist in WAV files only: Save As WAV to add them.",
                   juce::dontSendNotification);

    const int docId = d != nullptr ? d->id : -1;
    const bool newDoc = docId != shownDocId_;
    shownDocId_ = docId;
    const juce::ScopedValueSetter<bool> guard (loading_, true);
    for (const auto& r : rows_)
    {
        auto* ed = editorFor (r->id);
        const juce::String value = d != nullptr ? [&] {
            const auto it = d->riffInfo.find (r->id);
            return it != d->riffInfo.end() ? it->second : juce::String();
        }() : juce::String();
        // The field being typed in keeps its text (and its caret).
        if (ed->getText() != value && (newDoc || ! ed->hasKeyboardFocus (true)))
            ed->setText (value, false);
        if (r->markdown != nullptr)
            r->markdown->setEditable (wav);
        else
            ed->setReadOnly (! wav);
        ed->setAlpha (wav ? 1.0f : 0.6f);
    }
}

void FileInfoPanel::commit (const juce::String& id, const juce::String& text)
{
    if (loading_ || ! editable_)
        return;
    FileDoc* d = app.doc();
    if (d == nullptr)
        return;
    const auto it = d->riffInfo.find (id);
    const juce::String before = it != d->riffInfo.end() ? it->second : juce::String();
    if (before == text)
        return;
    if (text.isEmpty()) d->riffInfo.erase (id);
    else d->riffInfo[id] = text;
    d->metaDirty = true;   // saved with the next Save / Save Metadata
    if (onEdited) onEdited();
}

void FileInfoPanel::resized()
{
    view_.setBounds (getLocalBounds());
    layoutContent();
}

void FileInfoPanel::layoutContent()
{
    const int w = std::max (220, view_.getWidth() - view_.getScrollBarThickness() - 2);
    // Two columns when wide enough (fields | Comments + Subject), like
    // Audition's dialog; one otherwise.
    const bool twoCols = w >= 640;
    const int colW = twoCols ? (w - 30) / 2 : w - 16;
    const int labelW = juce::jlimit (90, 140, colW / 3);
    int y = 8;
    note_.setBounds (8, y, w - 16, 20);
    y += 26;
    int x = 8, ySingle = y;
    for (const auto& r : rows_)
        if (r->editor != nullptr)
        {
            r->label->setBounds (x, ySingle, labelW, 24);
            r->editor->setBounds (x + labelW + 6, ySingle + 1, colW - labelW - 6, 22);
            ySingle += 28;
        }
    int yMd = twoCols ? y : ySingle + 8;
    const int mdX = twoCols ? x + colW + 14 : x;
    const int mdH = twoCols ? std::max (140, (ySingle - y - 8) / 2) : 170;
    for (const auto& r : rows_)
        if (r->markdown != nullptr)
        {
            r->markdown->setBounds (mdX, yMd, colW, mdH);
            yMd += mdH + 8;
        }
    content_.setSize (w, std::max (ySingle, yMd) + 8);
}

void FileInfoPanel::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
}
