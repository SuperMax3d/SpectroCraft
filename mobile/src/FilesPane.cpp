#include "FilesPane.h"
#include "MobileDocument.h"
#include "MobileStyle.h"
#include "Recorder.h"
#include "TimeFormat.h"
#include "VideoCapture.h"

namespace scm
{
FilesPane::FilesPane (AppContext& c) : ctx (c)
{
    current.setFont (style::font (15.0f, true));
    addAndMakeVisible (current);
    openButton.onClick = [this] { openFromElsewhere(); };
    saveButton.onClick = [this] { saveCurrent(); };
    shareButton.onClick = [this] { share(); };
    deleteButton.onClick = [this] { deleteSelected(); };
    for (auto* b : { &openButton, &saveButton, &shareButton, &deleteButton }) addAndMakeVisible (b);
    list.setModel (this);
    list.setRowHeight (52);
    list.setColour (juce::ListBox::backgroundColourId, style::panelBackground);
    addAndMakeVisible (list);
    ctx.document.addChangeListener (this);
    refresh();
}

FilesPane::~FilesPane()
{
    ctx.document.removeChangeListener (this);
    list.setModel (nullptr);
}

void FilesPane::changeListenerCallback (juce::ChangeBroadcaster*)
{
    current.setText (ctx.document.displayName() + (ctx.document.hasUnsavedChanges() ? "  (not saved)" : ""),
                     juce::dontSendNotification);
    saveButton.setEnabled (ctx.document.hasAudio() && ! ctx.recorder.isRecording());
    shareButton.setEnabled (ctx.document.file().existsAsFile());
    if (ctx.document.file() != juce::File() && ! files.contains (ctx.document.file())) refresh();
}

void FilesPane::refresh()
{
    files.clear();
    for (const auto& e : juce::RangedDirectoryIterator (MobileDocument::recordingsFolder(), false, "*.wav"))
        files.add (e.getFile());
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getLastModificationTime() > b.getLastModificationTime();
    });
    list.updateContent();
    list.repaint();
    changeListenerCallback (nullptr);
}

int FilesPane::getNumRows() { return files.size(); }

void FilesPane::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (! juce::isPositiveAndBelow (row, files.size())) return;
    const auto& f = files.getReference (row);
    if (selected) g.fillAll (style::accent.withAlpha (0.35f));
    if (f == ctx.document.file()) { g.setColour (style::waveform); g.fillRect (0, 6, 3, h - 12); }
    g.setColour (style::text);
    g.setFont (style::font (16.0f));
    g.drawText (f.getFileNameWithoutExtension(), 10, 4, w - 20, 24, juce::Justification::centredLeft);
    juce::String info = f.getLastModificationTime().formatted ("%Y-%m-%d %H:%M") + "   "
                      + juce::File::descriptionOfSizeInBytes (f.getSize());
    if (VideoCapture::movieFileFor (f).existsAsFile()) info << "   + video";
    g.setColour (style::textDim);
    g.setFont (style::font (13.0f));
    g.drawText (info, 10, 28, w - 20, 18, juce::Justification::centredLeft);
    g.setColour (style::border);
    g.drawHorizontalLine (h - 1, 0.0f, (float) w);
}

void FilesPane::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow (row, files.size()) && files[row] != ctx.document.file())
        openFile (files[row]);
}

void FilesPane::confirmDiscard (std::function<void()> then)
{
    if (! ctx.document.hasUnsavedChanges()) { then(); return; }
    auto options = juce::MessageBoxOptions::makeOptionsYesNoCancel (juce::MessageBoxIconType::QuestionIcon,
        "Unsaved changes", ctx.document.displayName() + " has changes that are not saved.",
        "Save", "Discard", "Cancel");
    juce::AlertWindow::showAsync (options, [this, then] (int r)
    {
        if (r == 1)
        {
            juce::String err;
            if (! ctx.document.save (err)) { showMessage ("Save", err); return; }
            then();
        }
        else if (r == 2)
            then();
    });
}

void FilesPane::openFile (const juce::File& f)
{
    if (ctx.recorder.isRecording()) return;
    confirmDiscard ([this, f]
    {
        juce::String err;
        if (! ctx.document.open (f, err)) showMessage ("Open", err);
        refresh();
    });
}

void FilesPane::openFromElsewhere()
{
    chooser = std::make_unique<juce::FileChooser> ("Open a WAV file", juce::File(), "*.wav;*.WAV");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
    {
        const auto url = fc.getURLResult();
        if (url.isEmpty()) return;
        juce::File local = url.isLocalFile() ? url.getLocalFile() : juce::File();
        // Android (content://) and iOS security-scoped files: copy into the
        // recordings folder so the engine can read them.
        if (! local.existsAsFile() || ! local.isAChildOf (MobileDocument::recordingsFolder()))
        {
            auto in = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress));
            if (in == nullptr) { showMessage ("Open", "The file could not be read."); return; }
            auto name = url.getFileName();
            if (name.isEmpty() || ! name.endsWithIgnoreCase (".wav")) name = "Imported.wav";
            auto target = MobileDocument::recordingsFolder().getNonexistentChildFile (
                juce::File::createLegalFileName (name.upToLastOccurrenceOf (".", false, false)), ".wav", false);
            juce::FileOutputStream out (target);
            if (! out.openedOk() || out.writeFromInputStream (*in, -1) <= 0)
            {
                showMessage ("Open", "The file could not be copied.");
                return;
            }
            out.flush();
            local = target;
        }
        openFile (local);
    });
}

void FilesPane::saveCurrent()
{
    juce::String err;
    if (! ctx.document.save (err)) showMessage ("Save", err);
    refresh();
}

void FilesPane::share()
{
    const auto f = ctx.document.file();
    if (! f.existsAsFile()) return;
    juce::Array<juce::URL> urls { juce::URL (f) };
    const auto movie = VideoCapture::movieFileFor (f);
    if (movie.existsAsFile()) urls.add (juce::URL (movie));
    shareBox = juce::ContentSharer::shareFilesScoped (urls, [] (bool ok, const juce::String& err)
    {
        if (! ok && err.isNotEmpty()) showMessage ("Share", err);
    }, this);
}

void FilesPane::deleteSelected()
{
    const int row = list.getSelectedRow();
    if (! juce::isPositiveAndBelow (row, files.size())) return;
    const auto f = files[row];
    auto options = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::WarningIcon,
        "Delete", "Delete " + f.getFileName() + " (and its video)?", "Delete", "Cancel");
    juce::AlertWindow::showAsync (options, [this, f] (int r)
    {
        if (r != 1) return;
        if (f == ctx.document.file()) ctx.document.close();
        VideoCapture::movieFileFor (f).deleteFile();
        f.deleteFile();
        refresh();
    });
}

void FilesPane::paint (juce::Graphics& g)
{
    g.fillAll (style::panelBackground);
}

void FilesPane::resized()
{
    auto r = getLocalBounds().reduced (style::kGap);
    current.setBounds (r.removeFromTop (26));
    auto row = r.removeFromTop (style::kTouch);
    const int w = (row.getWidth() - 12) / 4;
    for (auto* b : { &openButton, &saveButton, &shareButton, &deleteButton })
    {
        b->setBounds (row.removeFromLeft (w));
        row.removeFromLeft (4);
    }
    r.removeFromTop (style::kGap);
    list.setBounds (r);
}
} // namespace scm
