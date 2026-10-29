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
constexpr int kRowBaseH    = 30;   // first row of a description (editor + buttons)
constexpr int kAnchorLineH = 20;   // one line of the anchor list under it

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

/** One row of the advanced section: description text + its anchor list
    (one "Included/Excluded: t0 - t1 s" line per anchor, each with its own
    Remove button). */
class DescRow : public juce::Component, private juce::TextEditor::Listener
{
public:
    static int heightFor (int numAnchors, int)
    {
        return kRowBaseH + std::max (0, numAnchors) * kAnchorLineH;
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
        edit_.setTooltip ("Description of the stem to extract.");

        includeBtn_.setButtonText ("Include Selection");
        includeBtn_.onClick = [this] { owner_->addAnchorFromSelection (index_, true); };
        excludeBtn_.setButtonText ("Exclude Selection");
        excludeBtn_.onClick = [this] { owner_->addAnchorFromSelection (index_, false); };
        removeBtn_.setButtonText ("Remove");
        removeBtn_.onClick = [this] { owner_->removeDesc (index_); };

        addAndMakeVisible (numLabel_);
        addAndMakeVisible (edit_);
        addAndMakeVisible (includeBtn_);
        addAndMakeVisible (excludeBtn_);
        addAndMakeVisible (removeBtn_);

        rebuildAnchorList ();   // show any anchors this entry already carries
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

        // Anchor list: one line per anchor - a label on the left and its own
        // Remove button in the same right-hand column as the row's Remove.
        for (size_t i = 0; i + 1 < anchorWidgets_.size (); i += 2)
        {
            const int ay = kRowBaseH + 2 + (int) (i / 2) * kAnchorLineH;
            auto* lbl = dynamic_cast<juce::Label*> (anchorWidgets_[i]);
            auto* btn = dynamic_cast<juce::TextButton*> (anchorWidgets_[i + 1]);
            if (lbl == nullptr || btn == nullptr) continue;
            lbl->setBounds (kMargin + 26, ay, std::max (40, w - kMargin * 2 - 26 - 70), kAnchorLineH);
            btn->setBounds (w - kMargin - 64, ay, 64, kAnchorLineH - 2);
        }
    }

private:
    void rebuildAnchorList()
    {
        for (auto* c : anchorWidgets_) delete c;
        anchorWidgets_.clear ();

        for (size_t i = 0; i < entry_.anchors.size (); ++i)
        {
            const auto& a = entry_.anchors[i];
            auto* lbl = new juce::Label();
            lbl->setFont (juce::FontOptions (12.5f));
            lbl->setText ((a.include ? "Included: " : "Excluded: ")
                          + sc::acpp::formatAnchorTime (a.startSecs) + " - "
                          + sc::acpp::formatAnchorTime (a.stopSecs) + " s",
                         juce::dontSendNotification);
            auto* btn = new juce::TextButton();
            btn->setButtonText ("Remove");
            const size_t idx = i;
            btn->onClick = [this, idx] { owner_->removeAnchor (index_, (int) idx); };
            anchorWidgets_.push_back (lbl);
            anchorWidgets_.push_back (btn);
            addAndMakeVisible (lbl);
            addAndMakeVisible (btn);
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
    std::vector<juce::Component*> anchorWidgets_;   // label + Remove button per anchor
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
    // Use the OS title bar (like MainWindow and the other dialogs): it shows
    // the "Stem Separation" title properly, while the JUCE-drawn fallback
    // renders broken glyphs on some platforms.
    setUsingNativeTitleBar (true);

    startLbl_.setFont (juce::FontOptions (13.0f));
    startLbl_.setText ("Start (s)", juce::dontSendNotification);
    endLbl_.setFont (juce::FontOptions (13.0f));
    endLbl_.setText ("End (s)", juce::dontSendNotification);
    modelLbl_.setFont (juce::FontOptions (13.0f));
    modelLbl_.setText ("Model:", juce::dontSendNotification);
    backendLbl_.setFont (juce::FontOptions (13.0f));
    backendLbl_.setText ("Backend:", juce::dontSendNotification);
    descLbl_.setFont (juce::FontOptions (13.0f));
    descLbl_.setText ("Description:", juce::dontSendNotification);
    durLbl_.setFont (juce::FontOptions (12.5f));

    for (auto* e : { &startEdit_, &endEdit_ })
    {
        e->setMultiLine (false);
        e->setColour (juce::CaretComponent::caretColourId, juce::Colours::white);
        e->addListener (this);   // keep the duration label in sync
    }

    // Seed the range from the current track selection ONCE. Later changes to
    // that selection must not update these fields: advanced mode needs stable
    // per-run ranges for its include/exclude anchors.
    {
        const double dur = app_.audioEngine().isFileLoaded () ? app_.audioEngine().duration () : 0.0;
        auto sel = app_.selection;
        double s0 = 0.0, s1 = dur;
        if (sel.getLength () > 0.0) { s0 = sel.getStart (); s1 = sel.getEnd (); }
        startEdit_.setText (juce::String (s0, 3), juce::dontSendNotification);
        endEdit_.setText (juce::String (s1, 3), juce::dontSendNotification);
    }

    // Restore descriptions remembered from a previous open of this window
    // (the Application keeps them between close/reopen). Anchors are absolute
    // document times; keep only those that still fit inside the current file.
    {
        const double dur = app_.audioEngine().isFileLoaded () ? app_.audioEngine().duration () : 0.0;
        for (const auto& s : app_.stemSepSavedDescs)
        {
            StemDescEntry e;
            e.text = s.text;
            for (const auto& a : s.anchors)
                if (a.startSecs >= -1e-6 && a.stopSecs > a.startSecs + 1e-9
                    && a.stopSecs <= dur + 1e-6)
                    e.anchors.push_back (a);
            descs_.push_back (e);
        }

        if (!descs_.empty ())
        {
            const bool anyAnchors = std::any_of (descs_.begin (), descs_.end (),
                [] (const StemDescEntry& d) { return !d.anchors.empty (); });
            // Multiple descriptions - or a single one carrying anchors - need
            // the advanced section to be visible; plain text goes back into
            // the simple-mode editor.
            if (descs_.size () > 1 || anyAnchors)
            {
                advanced_ = true;
                advancedBtn_.setButtonText ("Simple view");
            }
            else
            {
                descEdit_.setText (descs_[0].text, juce::dontSendNotification);
                descs_.clear ();
            }
        }
    }

    refreshDurationLabel ();

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
    // This JUCE build uses 0-based combo positions; the stored setting is a
    // 1-based id (1=Cpu 2=Cuda 3=Vulkan), so convert in both directions.
    backendCombo_.setSelectedItemIndex (std::clamp (app_.settings.acppBackend, 1, 3) - 1,
                                       juce::dontSendNotification);
    backendCombo_.onChange = [this]
    {
        app_.settings.acppBackend = backendCombo_.getSelectedItemIndex () + 1;
        app_.saveSettings ();
        refreshStatusIdle ();
    };

    extractOrigToggle_.setButtonText ("Extract subsequent stem from original");
    extractOrigToggle_.setToggleState (app_.settings.acppExtractOriginal, juce::dontSendNotification);
    extractOrigToggle_.onClick = [this]
    {
        app_.settings.acppExtractOriginal = extractOrigToggle_.getToggleState ();
        app_.saveSettings ();
    };

    descEdit_.setMultiLine (false);
    descEdit_.setColour (juce::CaretComponent::caretColourId, juce::Colours::white);

    // The constructor's restore block may have entered advanced mode already.
    advancedBtn_.setButtonText (advanced_ ? "Simple view" : "Advanced...");
    advancedBtn_.onClick = [this] { setAdvanced (!advanced_); };

    // Sits below the last description row in advanced mode (see resized()).
    addDescBtn_.setButtonText ("Add Stem");
    addDescBtn_.onClick = [this]
    {
        descs_.push_back (StemDescEntry {});
        rebuildRows ();
        setSize (kWinW, computeHeight ());
    };

    advancedHint_.setFont (juce::FontOptions (11.5f));
    advancedHint_.setText ("Anchor times come from the track selection and are converted to the "
                           "extracted input's timeline when the run starts.", juce::dontSendNotification);

    statusLabel_.setJustificationType (juce::Justification (juce::Justification::left));
    statusLabel_.setFont (juce::FontOptions (12.0f));

    downloadBtn_.setButtonText ("Download...");
    downloadBtn_.onClick = [this] { openDownloadDialog (); };
    runBtn_.setButtonText ("Run");
    runBtn_.onClick = [this] { startRun (); };

    cancelBtn_.setButtonText ("Cancel");
    cancelBtn_.onClick = [this] { userTriedToCloseWindow (); };

    // DocumentWindow inherits ResizableWindow, whose addAndMakeVisible() asserts;
    // call the base-class method explicitly to side-step it. (Must use
    // addAndMakeVisible - plain addChildComponent leaves the children invisible.)
    juce::Component::addAndMakeVisible (&startLbl_);
    juce::Component::addAndMakeVisible (&startEdit_);
    juce::Component::addAndMakeVisible (&endLbl_);
    juce::Component::addAndMakeVisible (&endEdit_);
    juce::Component::addAndMakeVisible (&durLbl_);
    juce::Component::addAndMakeVisible (&selectAllBtn_);
    juce::Component::addAndMakeVisible (&modelLbl_);
    juce::Component::addAndMakeVisible (&modelCombo_);
    juce::Component::addAndMakeVisible (&backendLbl_);
    juce::Component::addAndMakeVisible (&backendCombo_);
    juce::Component::addAndMakeVisible (&extractOrigToggle_);
    juce::Component::addAndMakeVisible (&descLbl_);
    juce::Component::addAndMakeVisible (&descEdit_);
    juce::Component::addAndMakeVisible (&advancedBtn_);
    juce::Component::addAndMakeVisible (&addDescBtn_);
    juce::Component::addAndMakeVisible (&rowsHost_);
    juce::Component::addAndMakeVisible (&advancedHint_);
    juce::Component::addAndMakeVisible (&statusLabel_);
    juce::Component::addAndMakeVisible (&cancelBtn_);
    juce::Component::addAndMakeVisible (&downloadBtn_);
    juce::Component::addAndMakeVisible (&runBtn_);

    refreshModelList ();
    centreWithSize (kWinW, computeHeight ());
    // Stay above the main window while open: making include/exclude
    // selections requires clicking into the track view, which would otherwise
    // push this non-modal window behind it. JUCE has no per-window z-order,
    // so use global topmost for the lifetime of the dialog.
    setAlwaysOnTop (true);
    setVisible (true);
}

StemSeparationWindow::~StemSeparationWindow()
{
    if (alive_) alive_->store (false);   // stop any pending worker callbacks
}

void StemSeparationWindow::userTriedToCloseWindow()
{
    rememberDescs ();   // keep the typed descriptions for the next open

    // The owner's callback releases the unique_ptr that owns this window, so
    // `this` is destroyed while we are still inside this call. JUCE's WndProc
    // does not touch the component after handleUserClosingWindow() returns,
    // which makes this safe (same idiom as a self-deleting popup).
    if (onCloseRequested) onCloseRequested ();
}

void StemSeparationWindow::closeButtonPressed()
{
    userTriedToCloseWindow ();
}

void StemSeparationWindow::refreshModelList()
{
    const auto models = sc::acpp::listModels ();
    modelCombo_.clear ();
    if (models.empty ())
    {
        modelCombo_.addItem ("(no models installed)", 1);
        // This JUCE build uses 0-based combo positions.
        modelCombo_.setSelectedItemIndex (0, juce::dontSendNotification);
    }
    else
    {
        for (size_t i = 0; i < models.size (); ++i)
            modelCombo_.addItem (models[i].getFileName (), (int) i + 1);
        // Restore the last used model when it is still installed.
        int pos = 0;
        const juce::String saved = app_.settings.acppModel;
        for (size_t i = 0; i < models.size (); ++i)
            if (models[i].getFileName() == saved) { pos = (int) i; break; }
        modelCombo_.setSelectedItemIndex (pos, juce::dontSendNotification);
    }
    refreshStatusIdle ();
}

void StemSeparationWindow::refreshStatusIdle()
{
    if (running_.load ()) return;

    const bool hasBin = sc::acpp::hasBinary ();
    const int modelIdx = modelCombo_.getSelectedItemIndex ();   // 0-based position
    const bool hasModel = !sc::acpp::listModels ().empty () && modelIdx >= 0;

    juce::String s;
    if (!hasBin)
        s = "The audiocpp_cli.exe binary is not installed yet - use Download...";
    else if (!hasModel)
        s = "No SAM-Audio model found in the models folder - use Download...";
    else
    {
        const juce::String variant = sc::acpp::installedVariant ();
        // Convert the 0-based position back to the stored 1-based id.
        const int backendIdx = backendCombo_.getSelectedItemIndex () + 1;
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
        rowsHost_.addAndMakeVisible (row);
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
    int addDescRow = 0;       // "Add Stem" button, below the last row (advanced)
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
            y.rowsTop = y.descAreaTop + 26 + 8;           // below the header buttons
            y.rowsTop += 16 + 8;                          // hint line + gap
            y.addDescRow = y.rowsTop + rowsH + 4;         // "Add Stem" under the last row
        }

        y.statusTop   = advanced ? y.addDescRow + 24 + 8 : y.rowsTop + 8;
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

    // Range row: editable start/end seconds + duration + Select All.
    startLbl_.setBounds (x, y.sourceRow + 3, 56, 18);
    startEdit_.setBounds (x + 58, y.sourceRow, 70, 24);
    endLbl_.setBounds (x + 134, y.sourceRow + 3, 48, 18);
    endEdit_.setBounds (x + 186, y.sourceRow, 70, 24);
    durLbl_.setBounds (x + 264, y.sourceRow + 3, 150, 18);
    selectAllBtn_.setBounds (getWidth () - kMargin - 96, y.sourceRow, 96, 24);

    // Model / backend rows: the labels share one column and every field starts
    // at the same left coordinate.
    modelLbl_.setBounds (x, y.modelRow + 5, 92, 18);
    modelCombo_.setBounds (x + 96, y.modelRow, w - 96, 28);

    backendLbl_.setBounds (x, y.backendRow + 5, 92, 18);
    backendCombo_.setBounds (x + 96, y.backendRow, 110, 28);

    if (!advanced_)
    {
        descLbl_.setBounds (x, y.descAreaTop + 5, 92, 18);
        descEdit_.setBounds (x + 96, y.descAreaTop, std::max (80, w - 96), 28);
        advancedBtn_.setBounds (x, y.descAreaTop + 34, 110, 24);
        // The advanced-only controls must not keep their old bounds here:
        // after a round-trip through advanced mode they would overlap the
        // description editor.
        addDescBtn_.setBounds (0, 0, 0, 0);
        extractOrigToggle_.setBounds (0, 0, 0, 0);
        advancedHint_.setBounds (0, 0, 0, 0);
    }
    else
    {
        // Header row: "Simple view" toggle; the hint sits on its own line below
        // so it has the full width. The regular description editor is hidden in
        // advanced mode (each DescRow carries its own field), which also keeps
        // the header from overlapping it.
        descLbl_.setBounds (0, 0, 0, 0);
        descEdit_.setBounds (0, 0, 0, 0);
        advancedBtn_.setBounds (x, y.descAreaTop + 2, 110, 24);
        // "Add Stem" follows the last description row dynamically and sits
        // just above the status line.
        addDescBtn_.setBounds (x, y.addDescRow, 130, 24);
        // Only meaningful with more than one description: it switches the run
        // from chained residuals back to extracting every stem from the original.
        if (descs_.size () > 1)
            extractOrigToggle_.setBounds (x + w - 300, y.descAreaTop + 5, 300, 20);
        else
            extractOrigToggle_.setBounds (0, 0, 0, 0);
        advancedHint_.setBounds (x, y.descAreaTop + 34, w, 16);
        rowsHost_.setBounds (x, y.rowsTop - rowsH, kWinW, std::max (0, rowsH));
    }

    statusLabel_.setBounds (x, y.statusTop, w, 40);
    runBtn_.setBounds (getWidth () - kMargin - 90, y.bottomRow, 90, 28);
    downloadBtn_.setBounds (runBtn_.getX () - 110, y.bottomRow, 104, 28);
    cancelBtn_.setBounds (downloadBtn_.getX () - 96, y.bottomRow, 88, 28);

    if (advanced_) rebuildRows ();   // re-lay out rows for the current width
}

void StemSeparationWindow::doSelectAll()
{
    const double dur = app_.audioEngine().isFileLoaded () ? app_.audioEngine().duration () : 0.0;
    startEdit_.setText (juce::String (0.0, 3), juce::dontSendNotification);
    endEdit_.setText (juce::String (dur, 3), juce::dontSendNotification);
    refreshDurationLabel ();
}

void StemSeparationWindow::refreshDurationLabel()
{
    const double dur = app_.audioEngine().isFileLoaded () ? app_.audioEngine().duration () : 0.0;
    const double s0 = jlimit (0.0, dur, startEdit_.getText ().getDoubleValue ());
    const double s1 = jlimit (0.0, dur, endEdit_.getText ().getDoubleValue ());
    durLbl_.setText ("Duration: " + juce::String (std::max (0.0, s1 - s0), 3) + " s",
                     juce::dontSendNotification);
}

void StemSeparationWindow::textEditorTextChanged(juce::TextEditor& editor)
{
    if (&editor == &startEdit_ || &editor == &endEdit_)
        refreshDurationLabel ();
}

/** Copies the currently typed descriptions into the Application so a later
    open of this window can restore them (see the constructor). */
void StemSeparationWindow::rememberDescs()
{
    app_.stemSepSavedDescs.clear ();
    if (advanced_)
    {
        for (const auto& d : descs_)
            if (!d.text.trim ().isEmpty ())
                app_.stemSepSavedDescs.push_back (d);
    }
    else
    {
        const juce::String t = descEdit_.getText ().trim ();
        if (!t.isEmpty ())
            app_.stemSepSavedDescs.push_back ({ t, {} });
    }
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
        // This JUCE build uses 0-based combo positions.
        cb->setSelectedItemIndex (0, juce::dontSendNotification);
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
        // This JUCE build uses 0-based combo positions.
        cb->setSelectedItemIndex (0, juce::dontSendNotification);
        dlg.addCustomComponent (cb);   // slot customSlot + 1
        modelComboSlot = customSlot + 1;
    }

    if (dlg.runModalLoop () != 1) return;   // cancelled

    int binaryChoice = 1, modelChoice = 1;
    if (binaryComboSlot >= 0)
        if (auto* cb = dynamic_cast<juce::ComboBox*> (dlg.getCustomComponent (binaryComboSlot)))
            if (const int p = cb->getSelectedItemIndex (); p >= 0)   // 0-based position -> 1-based id
                binaryChoice = p + 1;
    if (modelComboSlot >= 0)
        if (auto* cb = dynamic_cast<juce::ComboBox*> (dlg.getCustomComponent (modelComboSlot)))
            if (const int p = cb->getSelectedItemIndex (); p >= 0)   // 0-based position -> 1-based id
                modelChoice = p + 1;

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
    {
        for (auto& d : descs_)
            if (!d.text.trim().isEmpty()) descs.push_back (d);
    }
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

    const int mi = modelCombo_.getSelectedItemIndex ();   // 0-based position
    if (mi < 0)
    { statusLabel_.setText ("Pick a model first.", juce::dontSendNotification); return; }

    // Capture the extraction range from the editable fields on the message
    // thread (they were seeded from the track selection when the window
    // opened and are independent of later changes to it).
    const double dur = eng.duration ();
    double s0 = jlimit (0.0, dur, startEdit_.getText ().getDoubleValue ());
    double s1 = jlimit (0.0, dur, endEdit_.getText ().getDoubleValue ());
    if (s1 <= s0)
    { statusLabel_.setText ("The end time must be after the start time.", juce::dontSendNotification); return; }

    RunJob job;
    job.descs = std::move (descs);
    job.model = models[(size_t) mi];
    // Convert the 0-based position back to the stored 1-based id.
    const int bi = backendCombo_.getSelectedItemIndex () + 1;
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
        if (!cleaner.dir_.createDirectory ())
        { fail ("Could not create the temporary working folder."); return; }

        std::vector<std::vector<float>> inData;
        if (!eng.readRange (startFrame, nFrames, inData) || inData.empty () || inData[0].empty ())
        { fail ("Could not read the selected range."); return; }
        if (!sc::py::writeFloatWav (tmpIn, inData, j.rate))
        { fail ("Could not write the temporary input file."); return; }

        // Subsequent descriptions must work on a single mono track rather than
        // the whole multichannel extraction. Write a downmixed copy once when
        // the source has more than one channel (mono sources need none).
        const juce::File tmpInMono = cleaner.dir_.getChildFile ("input_mono.wav");
        bool haveMonoCopy = false;
        if (inData.size () > 1)
        {
            std::vector<std::vector<float>> mono;
            mono.push_back (sc::acpp::downmixToMono (inData));
            if (!sc::py::writeFloatWav (tmpInMono, mono, j.rate))
                { fail ("Could not write the temporary mono input file."); return; }
            haveMonoCopy = true;
        }

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
            // previous residual. From-original mode: subsequent descriptions
            // work on a single mono track of the original extraction - never
            // the whole multichannel file (the first keeps the full input).
            juce::File inWav;
            if (!j.extractFromOriginal && i > 0)   inWav = prevResidual;
            else if (i > 0 && haveMonoCopy)        inWav = tmpInMono;
            else                                   inWav = tmpIn;
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
                if (rr.cancelled)
                { fail ("Run cancelled."); return; }

                // The CLI reports errors on stdout as well as stderr, so fall
                // back to the output tail when stderr is empty.
                const juce::String detail = rr.stderrText.isNotEmpty () ? tail (rr.stderrText)
                                : rr.stdoutText.isNotEmpty ()          ? tail (rr.stdoutText)
                                                                        : juce::String (err);
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

            // Every description contributes its target; only the LAST one
            // also keeps its residual, so a run with N descriptions yields
            // N targets plus a single final "everything else" track instead of
            // an intermediate residual after every step.
            for (auto& c : tData)   { tracks.push_back (std::move (c)); trackRates.push_back (tRate); }
            if (i + 1 == N)
                for (auto& c : rData) { tracks.push_back (std::move (c)); trackRates.push_back (rRate); }
            prevResidual = residualF;   // still needed as the next step's input
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
            {
                statusLabel_.setText ("Created '" + app_.docs.back()->name + "' with "
                                      + juce::String (nTracks)
                                      + " tracks - one target per description plus the final residual.",
                                      juce::dontSendNotification);
                rememberDescs ();   // keep the typed descriptions for next time
                if (onCloseRequested) onCloseRequested ();   // close on success
            }
            delete resPtr;
        });
    }).detach ();
}
