// SpectroCraft - Stem Separation window (drives the audio.cpp CLI tool).
#include "StemSeparation.h"
#include "Application.h"
#include "MergeFiles.h"
#include "PythonScripts.h"
#include "Resampler.h"
#include "WorkProgress.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace {

constexpr int kWinW     = 620;
constexpr int kMargin   = 12;
constexpr int kRowBaseH = 30;   // first row of a description (editor + buttons)
constexpr int kChipH    = 24;   // one wrapped line of anchor chips
constexpr int kChipW    = 132;  // chip width used for the wrap calculation

/** Converts absolute document-time anchors to input-file-relative ones.
    Ranges that fall outside the extracted input are clamped; empty results
    are dropped. Pure function so it can be unit-tested without a process. */
std::vector<sc::acpp::Anchor> anchorsRelativeToInput(const std::vector<sc::acpp::Anchor>& absAnchors,
                                                     double extStartSecs, double inputDurSecs)
{
    std::vector<sc::acpp::Anchor> out;
    for (const auto& a : absAnchors)
    {
        const double s = std::max (0.0, a.startSecs - extStartSecs);
        const double e = std::min (inputDurSecs, a.stopSecs - extStartSecs);
        if (e > s) out.push_back ({ a.include, s, e });
    }
    return out;
}

/** One row of the advanced section: description text + anchor chips. */
class DescRow : public juce::Component, private juce::TextEditor::Listener
{
public:
    static int heightFor (int numAnchors, int width)
    {
        const int cols = std::max (1, (width - 2 * kMargin) / (kChipW + 6));
        const int lines = numAnchors == 0 ? 0 : (numAnchors + cols - 1) / cols;
        return kRowBaseH + lines * kChipH;
    }

    DescRow (int index, StemDescEntry& entry, StemSeparationWindow* owner)
        : index_ (index), entry_ (entry), owner_ (owner)
    {
        numLabel_.setFont (juce::FontOptions (13.0f));
        numLabel_.setText (juce::String (index + 1), juce::dontSendNotification);

        edit_.setMultiLine (false);
        edit_.setColour (juce::CaretComponent::caretColourId, juce::Colours::white);
        edit_.addListener (this);
        edit_.setText (entry.text, juce::dontSendNotification);

        includeBtn_.setButtonText ("Include Selection");
        includeBtn_.onClick = [this] { owner_->addAnchorFromSelection (index_, true); };
        excludeBtn_.setButtonText ("Exclude Selection");
        excludeBtn_.onClick = [this] { owner_->addAnchorFromSelection (index_, false); };
        removeBtn_.setButtonText ("Remove");
        removeBtn_.onClick = [this] { owner_->removeDesc (index_); };

        addChildComponent (numLabel_);
        addChildComponent (edit_);
        addChildComponent (includeBtn_);
        addChildComponent (excludeBtn_);
        addChildComponent (removeBtn_);
    }

    void layoutChildren (int w)
    {
        const int rightW = 92 + 6 + 92 + 6 + 64;   // include + exclude + remove
        numLabel_.setBounds (kMargin, 5, 20, 18);
        edit_.setBounds (kMargin + 26, 4, std::max (60, w - kMargin * 2 - 26 - rightW), 22);

        int bx = w - kMargin - 64;
        removeBtn_.setBounds (bx, 4, 64, 22);
        bx -= 92 + 6;
        excludeBtn_.setBounds (bx, 4, 92, 22);
        bx -= 92 + 6;
        includeBtn_.setBounds (bx, 4, 92, 22);

        const int cols = std::max (1, (w - 2 * kMargin) / (kChipW + 6));
        for (size_t i = 0; i < chips_.size (); ++i)
            chips_[i]->setBounds (kMargin + 26 + (int) (i % cols) * (kChipW + 6),
                                kRowBaseH + 2 + (int) (i / cols) * kChipH,
                                kChipW, kChipH - 4);
    }

private:
    void rebuildChips()
    {
        for (auto* c : chips_) delete c;
        chips_.clear ();

        for (size_t i = 0; i < entry_.anchors.size (); ++i)
        {
            const auto& a = entry_.anchors[i];
            auto* b = new juce::TextButton();
            b->setButtonText (juce::String (a.include ? "+" : "-") + " "
                              + sc::acpp::formatAnchorTime (a.startSecs) + " - "
                              + sc::acpp::formatAnchorTime (a.stopSecs) + " s");
            b->setTooltip ("Click to remove this range");
            const size_t idx = i;
            b->onClick = [this, idx] { owner_->removeAnchor (index_, (int) idx); };
            chips_.push_back (b);
            addChildComponent (b);
        }
    }

    void textEditorTextChanged (juce::TextEditor&) override
    {
        entry_.text = edit_.getText ();
    }

    int index_;
    StemDescEntry& entry_;
    StemSeparationWindow* owner_;
    juce::Label numLabel_;
    juce::TextEditor edit_;
    juce::TextButton includeBtn_, excludeBtn_, removeBtn_;
    std::vector<juce::TextButton*> chips_;
};

/** RAII cleanup for the per-run temporary folder. */
struct TempCleaner
{
    explicit TempCleaner (const juce::File& f) : dir_ (f) {}
    ~TempCleaner() { if (!dir_.getFullPathName().isEmpty ()) dir_.deleteRecursively (); }
    juce::File dir_;
};

} // namespace

//==============================================================================
// StemSeparationWindow
//==============================================================================

struct StemSeparationWindow::RunJob
{
    std::vector<StemDescEntry> descs;
    juce::File model;
    juce::String backendCli;          // "cpu" / "cuda" / "vulkan"
    bool extractFromOriginal = true;
    double extStartSecs = 0.0;        // extraction range in source seconds
    double extStopSecs = 0.0;
    double rate = 48000.0;
    juce::String resultName;          // suggested name for the new document
};

StemSeparationWindow::StemSeparationWindow(Application& app)
    : juce::DocumentWindow ("Stem Separation", juce::Colour (0xff3a3f4b),
                            juce::DocumentWindow::minimiseButton | juce::DocumentWindow::closeButton),
      app_ (app),
      alive_ (std::make_shared<std::atomic<bool>> (true))
{
    selRangeLabel_.setFont (juce::FontOptions (13.0f));

    selectAllBtn_.setButtonText ("Select All");
    selectAllBtn_.onClick = [this] { doSelectAll (); };

    modelCombo_.onChange = [this]
    {
        const juce::String t = modelCombo_.getText ();
        app_.settings.acppModel = (t == "(no models installed)") ? juce::String () : t;
        app_.saveSettings ();
        refreshStatusIdle ();
    };
    backendCombo_.addItem ("Cpu", 1);
    backendCombo_.addItem ("Cuda", 2);
    backendCombo_.addItem ("Vulkan", 3);
    backendCombo_.setSelectedItemIndex (std::clamp (app_.settings.acppBackend, 1, 3),
                                       juce::dontSendNotification);
    backendCombo_.onChange = [this]
    {
        app_.settings.acppBackend = backendCombo_.getSelectedItemIndex ();
        app_.saveSettings ();
        refreshStatusIdle ();
    };

    extractOrigToggle_.setButtonText ("Extract from original");
    extractOrigToggle_.setToggleState (app_.settings.acppExtractOriginal, juce::dontSendNotification);
    extractOrigToggle_.onClick = [this]
    {
        app_.settings.acppExtractOriginal = extractOrigToggle_.getToggleState ();
        app_.saveSettings ();
    };

    descEdit_.setMultiLine (false);
    descEdit_.setColour (juce::CaretComponent::caretColourId, juce::Colours::white);

    advancedBtn_.setButtonText ("Advanced...");
    advancedBtn_.onClick = [this] { setAdvanced (!advanced_); };

    advancedHint_.setFont (juce::FontOptions (11.5f));
    advancedHint_.setText ("Anchor times come from the track selection and are converted to the "
                           "extracted input's timeline when the run starts.", juce::dontSendNotification);

    statusLabel_.setJustificationType (juce::Justification (juce::Justification::left));
    statusLabel_.setFont (juce::FontOptions (12.0f));

    downloadBtn_.setButtonText ("Download...");
    downloadBtn_.onClick = [this] { openDownloadDialog (); };
    runBtn_.setButtonText ("Run");
    runBtn_.onClick = [this] { startRun (); };

    // DocumentWindow inherits ResizableWindow, whose addAndMakeVisible() asserts;
    // call the base-class method explicitly to side-step it.
    juce::Component::addChildComponent (&selRangeLabel_);
    juce::Component::addChildComponent (&selectAllBtn_);
    juce::Component::addChildComponent (&modelCombo_);
    juce::Component::addChildComponent (&backendCombo_);
    juce::Component::addChildComponent (&extractOrigToggle_);
    juce::Component::addChildComponent (&descEdit_);
    juce::Component::addChildComponent (&advancedBtn_);
    juce::Component::addChildComponent (&rowsHost_);
    juce::Component::addChildComponent (&advancedHint_);
    juce::Component::addChildComponent (&statusLabel_);
    juce::Component::addChildComponent (&downloadBtn_);
    juce::Component::addChildComponent (&runBtn_);

    refreshModelList ();
    centreWithSize (kWinW, computeHeight ());
    setVisible (true);
    startTimerHz (3);
}

StemSeparationWindow::~StemSeparationWindow()
{
    if (alive_) alive_->store (false);   // stop any pending worker callbacks
}

void StemSeparationWindow::timerCallback()
{
    selRangeLabel_.setText (selectionRangeText (), juce::dontSendNotification);
}

juce::String StemSeparationWindow::selectionRangeText() const
{
    if (!app_.audioEngine().isFileLoaded ())
        return "No file loaded";

    const auto dur = app_.audioEngine().duration ();
    const auto sel = app_.selection;
    if (sel.getLength () > 0.0)
        return juce::String ("Selection: ") + sc::acpp::formatAnchorTime (sel.getStart ())
             + " - " + sc::acpp::formatAnchorTime (sel.getEnd ()) + " s"
             + "  (" + sc::acpp::formatAnchorTime (sel.getLength ()) + " s)";
    return juce::String ("Whole file: 0 - ") + sc::acpp::formatAnchorTime (dur) + " s";
}

void StemSeparationWindow::refreshModelList()
{
    const auto models = sc::acpp::listModels ();
    modelCombo_.clear ();
    if (models.empty ())
    {
        modelCombo_.addItem ("(no models installed)", 1);
        modelCombo_.setSelectedItemIndex (1, juce::dontSendNotification);
    }
    else
    {
        for (size_t i = 0; i < models.size (); ++i)
            modelCombo_.addItem (models[i].getFileName (), (int) i + 1);
        // Restore the last used model when it is still installed.
        int sel = 1;
        const juce::String saved = app_.settings.acppModel;
        for (size_t i = 0; i < models.size (); ++i)
            if (models[i].getFileName() == saved) { sel = (int) i + 1; break; }
        modelCombo_.setSelectedItemIndex (sel, juce::dontSendNotification);
    }
    refreshStatusIdle ();
}

void StemSeparationWindow::refreshStatusIdle()
{
    if (running_.load ()) return;

    const bool hasBin = sc::acpp::hasBinary ();
    const int modelIdx = modelCombo_.getSelectedItemIndex ();
    const bool hasModel = !sc::acpp::listModels ().empty () && modelIdx > 0;

    juce::String s;
    if (!hasBin)
        s = "The audiocpp_cli.exe binary is not installed yet - use Download...";
    else if (!hasModel)
        s = "No SAM-Audio model found in the models folder - use Download...";
    else
    {
        const juce::String variant = sc::acpp::installedVariant ();
        const int backendIdx = backendCombo_.getSelectedItemIndex ();
        const bool mismatch = (variant == "cpu" && backendIdx != 1)
                          || ((variant == "cuda12.4" || variant == "cuda13.3") && backendIdx != 2)
                          || (variant == "vulkan" && backendIdx != 3);
        s = juce::String ("Ready: ") + modelCombo_.getText ()
          + (variant.isEmpty () ? juce::String ("") : juce::String ("  [") + variant + "]");
        if (mismatch)
            s += "  - the installed binary may not support this backend";
    }
    statusLabel_.setText (s, juce::dontSendNotification);
}

void StemSeparationWindow::setAdvanced(bool on)
{
    if (on == advanced_) return;
    advanced_ = on;
    advancedBtn_.setButtonText (advanced_ ? "Simple view" : "Advanced...");

    if (advanced_)
    {
        // Carry the simple-mode description into the first entry.
        descs_.clear ();
        StemDescEntry e;
        e.text = descEdit_.getText ();
        descs_.push_back (e);
    }
    else
    {
        if (!descs_.empty ())
            descEdit_.setText (descs_[0].text, juce::dontSendNotification);
        descs_.clear ();
    }

    rebuildRows ();
    setSize (kWinW, computeHeight ());
}

void StemSeparationWindow::rebuildRows()
{
    for (auto* c : rowsHost_.getChildren()) delete c;

    if (!advanced_) return;

    const int w = kWinW - 2 * kMargin;
    int y = 0;
    for (size_t i = 0; i < descs_.size (); ++i)
    {
        auto* row = new DescRow ((int) i, descs_[i], this);
        const int h = DescRow::heightFor ((int) descs_[i].anchors.size (), w);
        row->setBounds (0, y, kWinW, h);
        row->layoutChildren (kWinW);
        rowsHost_.addChildComponent (row);
        y += h + 4;
    }
}

namespace {
// Vertical layout shared by metrics() and resized().
struct LayoutYs
{
    int sourceRow = 0, modelRow = 0, backendRow = 0;
    int descAreaTop = 0;      // description editor (simple) or header row (advanced)
    int rowsTop = 0;          // top of the DescRow stack (advanced only)
    int statusTop = 0;
    int bottomRow = 0;
    int totalH = 0;

    static LayoutYs compute (bool advanced, int rowsH)
    {
        LayoutYs y;
        y.sourceRow   = kMargin;
        y.modelRow    = y.sourceRow + 24 + 8;
        y.backendRow  = y.modelRow + 28 + 8;
        y.descAreaTop = y.backendRow + 28 + 8;

        if (!advanced)
            y.rowsTop = y.descAreaTop + 28 + 8 + 24 + 8;   // editor row + advanced button
        else
        {
            y.rowsTop = y.descAreaTop + 26 + 8;           // header row (add description)
            y.rowsTop += rowsH;                           // the DescRow stack itself
        }

        y.statusTop   = y.rowsTop + 8;
        y.bottomRow   = y.statusTop + 40 + 8;
        y.totalH      = y.bottomRow + 28 + kMargin;
        return y;
    }
};
} // namespace

int StemSeparationWindow::computeHeight() const
{
    int rowsH = 0;
    if (advanced_)
    {
        const int w = kWinW - 2 * kMargin;
        for (const auto& d : descs_)
            rowsH += DescRow::heightFor ((int) d.anchors.size (), w) + 4;
    }
    return LayoutYs::compute (advanced_, rowsH).totalH;
}

void StemSeparationWindow::resized()
{
    int rowsH = 0;
    if (advanced_)
    {
        const int w = kWinW - 2 * kMargin;
        for (const auto& d : descs_)
            rowsH += DescRow::heightFor ((int) d.anchors.size (), w) + 4;
    }
    const auto y = LayoutYs::compute (advanced_, rowsH);
    const int x = kMargin, w = getWidth () - 2 * kMargin;

    // Source row: live selection range + Select All.
    selRangeLabel_.setBounds (x + 64, y.sourceRow + 3, std::max (80, w - 64 - 100), 18);
    selectAllBtn_.setBounds (getWidth () - kMargin - 96, y.sourceRow, 96, 24);

    // Model row.
    modelCombo_.setBounds (x + 56, y.modelRow, w - 56, 28);

    // Backend row: combo + extract-from-original toggle.
    backendCombo_.setBounds (x + 70, y.backendRow, 110, 28);
    extractOrigToggle_.setBounds (x + 190, y.backendRow + 4, w - 190, 20);

    if (!advanced_)
    {
        descEdit_.setBounds (x + 96, y.descAreaTop, std::max (80, w - 96), 28);
        advancedBtn_.setBounds (x, y.descAreaTop + 34, 110, 24);
    }
    else
    {
        // Header row: "Simple view" toggle on the left, hint text right of it.
        advancedBtn_.setBounds (x, y.descAreaTop + 2, 110, 24);
        rowsHost_.setBounds (x, y.rowsTop - rowsH, kWinW, std::max (0, rowsH));
        advancedHint_.setBounds (x + 130, y.descAreaTop + 6, w - 130, 18);
    }

    statusLabel_.setBounds (x, y.statusTop, w, 40);
    runBtn_.setBounds (getWidth () - kMargin - 90, y.bottomRow, 90, 28);
    downloadBtn_.setBounds (runBtn_.getX () - 110, y.bottomRow, 104, 28);

    if (advanced_) rebuildRows ();   // re-lay out rows for the current width
}

void StemSeparationWindow::doSelectAll()
{
    app_.selectAll ();
    selRangeLabel_.setText (selectionRangeText (), juce::dontSendNotification);
}

void StemSeparationWindow::addAnchorFromSelection(int descIndex, bool include)
{
    if (!app_.audioEngine().isFileLoaded ())
    { statusLabel_.setText ("No file is loaded.", juce::dontSendNotification); return; }

    const auto sel = app_.selection;
    if (sel.getLength () <= 0.0)
    {
        statusLabel_.setText ("Make a selection in the track first, then click "
                              + juce::String (include ? "Include" : "Exclude") + " Selection.",
                              juce::dontSendNotification);
        return;
    }

    if (descIndex < 0 || descIndex >= (int) descs_.size ()) return;
    // Stored as absolute document time; converted to input-relative at run time.
    sc::acpp::Anchor a { include, sel.getStart (), sel.getEnd () };
    descs_[descIndex].anchors.push_back (a);

    rebuildRows ();
    setSize (kWinW, computeHeight ());
}

void StemSeparationWindow::removeAnchor(int descIndex, int anchorIndex)
{
    if (descIndex < 0 || descIndex >= (int) descs_.size ()) return;
    auto& anchors = descs_[descIndex].anchors;
    if (anchorIndex < 0 || anchorIndex >= (int) anchors.size ()) return;
    anchors.erase (anchors.begin () + anchorIndex);

    rebuildRows ();
    setSize (kWinW, computeHeight ());
}

void StemSeparationWindow::removeDesc(int index)
{
    if (index < 0 || index >= (int) descs_.size ()) return;
    descs_.erase (descs_.begin () + index);
    rebuildRows ();
    setSize (kWinW, computeHeight ());
}


//==============================================================================
// Download dialog
//==============================================================================

void StemSeparationWindow::openDownloadDialog()
{
    const bool needBinary = !sc::acpp::hasBinary ();
    bool needModel  = sc::acpp::listModels ().empty ();
    if (!needBinary && !needModel)
        needModel = true;   // everything installed: offer downloading another model

    juce::AlertWindow dlg ("Stem Separation setup", "", juce::AlertWindow::NoIcon, this);
    dlg.addButton ("Download", 1, {});
    dlg.addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // getCustomComponent() indexes only addCustomComponent() calls, in order.
    int customSlot = 0;
    int binaryComboSlot = -1, modelComboSlot = -1;
    if (needBinary)
    {
        auto* lbl = new juce::Label();
        lbl->setText ("CLI binary:", juce::dontSendNotification);
        dlg.addCustomComponent (lbl);   // slot customSlot
        auto* cb = new juce::ComboBox ();
        const auto choices = sc::acpp::binaryChoices ();
        for (size_t i = 0; i < choices.size (); ++i)
            cb->addItem (choices[i].label + "   (" + juce::String ((double) choices[i].sizeBytes / 1.0e6, 1) + " MB)", (int) i + 1);
        cb->setSelectedItemIndex (1, juce::dontSendNotification);
        dlg.addCustomComponent (cb);   // slot customSlot + 1
        binaryComboSlot = customSlot + 1;
        customSlot += 2;
    }

    if (needModel)
    {
        auto* lbl = new juce::Label();
        lbl->setText ("SAM-Audio model:", juce::dontSendNotification);
        dlg.addCustomComponent (lbl);   // slot customSlot
        auto* cb = new juce::ComboBox ();
        const auto choices = sc::acpp::modelChoices ();
        for (size_t i = 0; i < choices.size (); ++i)
            cb->addItem (juce::String (choices[i].file) + "   ("
                         + juce::String ((double) choices[i].sizeBytes / 1.0e9, 2) + " GB)",
                         (int) i + 1);
        cb->setSelectedItemIndex (1, juce::dontSendNotification);
        dlg.addCustomComponent (cb);   // slot customSlot + 1
        modelComboSlot = customSlot + 1;
    }

    if (dlg.runModalLoop () != 1) return;   // cancelled

    int binaryChoice = 1, modelChoice = 1;
    if (binaryComboSlot >= 0)
        if (auto* cb = dynamic_cast<juce::ComboBox*> (dlg.getCustomComponent (binaryComboSlot)))
            binaryChoice = std::max (1, cb->getSelectedItemIndex ());
    if (modelComboSlot >= 0)
        if (auto* cb = dynamic_cast<juce::ComboBox*> (dlg.getCustomComponent (modelComboSlot)))
            modelChoice = std::max (1, cb->getSelectedItemIndex ());

    // Download on a worker thread with a cancellable progress dialog. The
    // binary is installed first; then the model (when both were missing).
    auto* prog = new sc::WorkProgressDialog ("Downloading", "Starting...", true, this);
    prog->setProgress (0.0);
    prog->beginModal ();

    const juce::File root = sc::acpp::dir ();
    std::thread ([this, guard = alive_, prog, needBinary, needModel,
                  binaryChoice, modelChoice, root] {
        auto setStatus = [prog] (const juce::String& s)
            { juce::MessageManager::callAsync ([prog, s] { prog->setStatusText (s); }); };

        double lastSent = -1.0;
        auto sendProgress = [prog, &lastSent] (double v)
        {
            if (v == lastSent) return;   // e.g. repeated 1.0 on chunked responses
            if (v - lastSent < 0.02 && v != 1.0) return;
            lastSent = v;
            juce::MessageManager::callAsync ([prog, v] { prog->setProgress (v); });
        };

        std::string err;
        bool ok = true;

        if (needBinary)
        {
            const auto choices = sc::acpp::binaryChoices ();
            const size_t i = (size_t) std::min (std::max (1, binaryChoice), (int) choices.size ()) - 1;
            const juce::File zip = root.getChildFile ("audiocpp_bin.zip");

            setStatus ("Downloading " + choices[i].label + "...");
            ok = sc::acpp::downloadFile (choices[i].url, zip,
                [&] (float f) { sendProgress (f); return !prog->cancelled.load (); }, &err);
            if (!ok && !prog->cancelled.load ())
                setStatus ("Download failed: " + juce::String (err));

            if (ok)
            {
                setStatus ("Extracting the binary...");
                ok = sc::acpp::installBinaryFromZip (zip, choices[i].id, root, &err);
                if (!ok && !prog->cancelled.load ())
                    setStatus ("Extraction failed: " + juce::String (err));
            }
        }

        if (ok && needModel)
        {
            const auto choices = sc::acpp::modelChoices ();
            const size_t i = (size_t) std::min (std::max (1, modelChoice), (int) choices.size ()) - 1;
            const juce::File dest = root.getChildFile (choices[i].file);

            lastSent = -1.0;
            setStatus ("Downloading " + choices[i].file + "...");
            ok = sc::acpp::downloadFile (sc::acpp::modelUrl (choices[i].file).toStdString (),
                                         dest,
                [&] (float f) { sendProgress (f); return !prog->cancelled.load (); }, &err);
            if (!ok && !prog->cancelled.load ())
                setStatus ("Download failed: " + juce::String (err));
        }

        const bool cancelled = prog->cancelled.load ();
        juce::MessageManager::callAsync ([this, guard, prog, ok, cancelled] {
            delete prog;
            if (!guard || !guard->load ()) return;   // window closed meanwhile
            refreshModelList ();   // updates the model combo + status line
            if (!ok && !cancelled)
                statusLabel_.setText ("Download failed - see the progress log above.",
                                      juce::dontSendNotification);
        });
    }).detach ();
}

//==============================================================================
// Run
//==============================================================================

void StemSeparationWindow::startRun()
{
    if (running_.load ()) return;

    auto& eng = app_.audioEngine();
    if (!eng.isFileLoaded())
    { statusLabel_.setText ("Load a file first.", juce::dontSendNotification); return; }

    // Collect the descriptions to run.
    std::vector<StemDescEntry> descs;
    if (advanced_)
        for (auto& d : descs_)
            if (!d.text.trim().isEmpty()) descs.push_back (d);
    else
    {
        StemDescEntry e;
        e.text = descEdit_.getText ();
        if (!e.text.trim().isEmpty()) descs.push_back (e);
    }
    if (descs.empty ())
    { statusLabel_.setText ("Enter a description first.", juce::dontSendNotification); return; }

    const auto models = sc::acpp::listModels ();
    if (!sc::acpp::hasBinary () || models.empty ())
    { openDownloadDialog (); return; }

    const int mi = modelCombo_.getSelectedItemIndex ();
    if (mi <= 0)
    { statusLabel_.setText ("Pick a model first.", juce::dontSendNotification); return; }

    // Capture the extraction range on the message thread.
    const double dur = eng.duration ();
    auto sel = app_.selection;
    double s0 = 0.0, s1 = dur;
    if (sel.getLength () > 0.0) { s0 = sel.getStart (); s1 = sel.getEnd (); }

    RunJob job;
    job.descs = std::move (descs);
    job.model = models[(size_t) mi - 1];
    const int bi = backendCombo_.getSelectedItemIndex ();
    job.backendCli = (bi == 2 ? "cuda" : bi == 3 ? "vulkan" : "cpu");
    job.extractFromOriginal = extractOrigToggle_.getToggleState ();
    job.extStartSecs = s0;
    job.extStopSecs  = s1;
    job.rate         = eng.sampleRate ();
    job.resultName   = (app_.doc () != nullptr ? app_.doc()->name : juce::String ("stem")) + "_stems";

    running_ = true;
    statusLabel_.setText ("Starting...", juce::dontSendNotification);
    runWorker (std::make_unique<RunJob> (std::move (job)));
}

void StemSeparationWindow::runWorker(std::unique_ptr<RunJob> job)
{
    auto* prog = new sc::WorkProgressDialog ("Stem Separation", "Preparing input...", true, nullptr);
    prog->setProgress (0.0);
    prog->beginModal ();

    std::thread ([this, guard = alive_, jobPtr = std::move (job), prog] {
        RunJob& j = *jobPtr;

        auto setStatus = [prog] (const juce::String& s)
            { juce::MessageManager::callAsync ([prog, s] { prog->setStatusText (s); }); };
        auto setProgress = [prog] (double v)
            { juce::MessageManager::callAsync ([prog, v] { prog->setProgress (v); }); };

        // Tail of a long log for the error dialog.
        auto tail = [] (const juce::String& s, int maxChars = 2000) -> juce::String
        {
            if (s.length () <= maxChars) return s;
            return "..." + s.substring (s.length () - maxChars, s.length ());
        };

        auto fail = [this, guard, prog] (const juce::String& msg, const juce::String& detail = {})
        {
            juce::MessageManager::callAsync ([this, guard, prog, msg, detail] {
                delete prog;
                if (!guard || !guard->load ()) return;   // window closed meanwhile
                running_ = false;
                statusLabel_.setText (msg, juce::dontSendNotification);
                if (!detail.isEmpty ())
                    juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                                      "Stem Separation failed",
                                                      msg + "\n\n" + detail, "", this);
            });
        };

        // 1. Extract the input range into a temporary float WAV.
        auto& eng = app_.audioEngine ();
        const int64_t startFrame = (int64_t) std::llround (j.extStartSecs * j.rate);
        const int64_t nFrames    = (int64_t) std::llround ((j.extStopSecs - j.extStartSecs) * j.rate);
        if (nFrames <= 0) { fail ("The selected range is empty."); return; }

        TempCleaner cleaner (juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("SpectroCraft_stem_"
                           + juce::String (juce::Random::getSystemRandom().nextInt (999998) + 1)));
        const juce::File tmpIn = cleaner.dir_.getChildFile ("input.wav");

        std::vector<std::vector<float>> inData;
        if (!eng.readRange (startFrame, nFrames, inData) || inData.empty () || inData[0].empty ())
        { fail ("Could not read the selected range."); return; }
        if (!sc::py::writeFloatWav (tmpIn, inData, j.rate))
        { fail ("Could not write the temporary input file."); return; }

        const double inputDur = (double) nFrames / j.rate;
        const int N = (int) j.descs.size ();

        // 2. Run each description through the CLI.
        std::vector<std::vector<float>> tracks;      // one per output track
        std::vector<double> trackRates;
        juce::File prevResidual;

        for (int i = 0; i < N; ++i)
        {
            if (prog->cancelled.load ())
            { fail ("Run cancelled."); return; }

            const auto& d = j.descs[(size_t) i];
            const juce::String what = "Description " + juce::String (i + 1) + "/" + juce::String (N)
                                     + ": " + d.text;
            setStatus (what);
            setProgress ((double) i / (double) N);

            // Chained mode: every description after the first works on the
            // previous residual instead of the original input.
            const juce::File inWav = (!j.extractFromOriginal && i > 0) ? prevResidual : tmpIn;
            const juce::File outDir = cleaner.dir_.getChildFile ("d" + juce::String (i + 1));

            auto relAnchors = anchorsRelativeToInput (d.anchors, j.extStartSecs, inputDur);

            sc::acpp::RunResult rr;
            std::string err;
            auto lastUi = std::chrono::steady_clock::now () - std::chrono::milliseconds (500);
            const bool ok = sc::acpp::runStemSeparation (
                sc::acpp::binaryPath (), j.model, j.backendCli, inWav, d.text, relAnchors, outDir,
                [&] { return !prog->cancelled.load (); },
                [&] (const juce::String& line) {
                    const auto now = std::chrono::steady_clock::now ();
                    if (now - lastUi < std::chrono::milliseconds (300)) return;
                    lastUi = now;
                    setStatus (what + "  |  " + line);
                },
                &rr, &err);

            if (!ok)
            {
                const juce::String detail = rr.stderrText.isEmpty () ? juce::String (err)
                                                                    : tail (rr.stderrText);
                fail ("Stem separation failed for '" + d.text + "'.", detail);
                return;
            }

            const juce::File targetF   = outDir.getChildFile ("target.wav");
            const juce::File residualF = outDir.getChildFile ("residual.wav");
            if (!targetF.existsAsFile () || !residualF.existsAsFile ())
            { fail ("The tool did not produce target.wav / residual.wav in " + outDir.getFullPathName ()); return; }

            double tRate = 0.0, rRate = 0.0;
            int nchT = 0, nchR = 0;
            std::vector<std::vector<float>> tData, rData;
            if (!sc::py::readFloatWav (targetF, &tRate, &nchT, tData)
                || !sc::py::readFloatWav (residualF, &rRate, &nchR, rData))
            { fail ("Could not read the tool's output files."); return; }

            for (auto& c : tData)   { tracks.push_back (std::move (c)); trackRates.push_back (tRate); }
            for (auto& c : rData)   { tracks.push_back (std::move (c)); trackRates.push_back (rRate); }
            prevResidual = residualF;
            setProgress ((double) (i + 1) / (double) N);
        }

        // 3. Normalise every track to the source rate and pad to equal length.
        int64_t maxLen = 0;
        for (const auto& t : tracks) maxLen = std::max (maxLen, (int64_t) t.size ());
        if (maxLen <= 0) { fail ("The tool produced empty output."); return; }

        sc::MergeResult result;
        result.rate = j.rate;
        for (size_t i = 0; i < tracks.size (); ++i)
        {
            auto& t = tracks[i];
            if (std::abs (trackRates[i] - j.rate) > 1e-6 * j.rate && !t.empty ())
            {
                juce::AudioBuffer<float> in (1, (int) t.size ());
                std::copy (t.begin (), t.end (), in.getWritePointer (0));
                juce::AudioBuffer<float> out (1, (int) sc::Resampler::estimateOutputFrames (
                    (int64_t) t.size (), trackRates[i], j.rate));
                if (!sc::Resampler::resample (in, trackRates[i], j.rate, out))
                { fail ("Could not resample the tool's output to " + juce::String (j.rate, 0) + " Hz."); return; }
                t.assign (out.getReadPointer (0), out.getReadPointer (0) + out.getNumSamples ());
            }
            t.resize ((size_t) maxLen, 0.0f);
        }

        result.channels = (int) tracks.size ();
        result.data     = std::move (tracks);
        const int nTracks = result.channels;

        // 4. Create the new document on the message thread. The result is
        // heap-held because this MSVC build rejects `mutable` lambdas with
        // init-captures, and createDocFromMerged takes a non-const reference.
        auto* resPtr = new sc::MergeResult (std::move (result));
        juce::MessageManager::callAsync ([this, guard, prog, resPtr,
                                          name = j.resultName, nTracks] {
            delete prog;
            if (!guard || !guard->load ()) { delete resPtr; return; }
            running_ = false;
            std::string err;
            if (!app_.createDocFromMerged (*resPtr, &err, name))
                statusLabel_.setText ("Could not create the result document: " + juce::String (err),
                                      juce::dontSendNotification);
            else
                statusLabel_.setText ("Created '" + app_.docs.back()->name + "' with "
                                      + juce::String (nTracks)
                                      + " tracks - target/residual per description.",
                                      juce::dontSendNotification);
            delete resPtr;
        });
    }).detach ();
}
