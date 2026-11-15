// SpectroCraft - Stem Separation window (drives the audio.cpp CLI tool).
#include "StemSeparation.h"
#include "AI/StemService.h"      // the headless separation run (no UI)
#include "AlertGate.h"
#include "Application.h"
#include "MergeFiles.h"
#include "OwningAlertWindow.h"   // dialogs with new-ed custom components
#include "WorkProgress.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace {

constexpr int kWinW     = 620;
constexpr int kMargin   = 12;
constexpr int kRowBaseH    = 30;   // first row of a description (editor + buttons)
constexpr int kAnchorLineH = 20;   // one line of the anchor list under it

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
        includeBtn_.setTooltip ("Add the current track selection as a range where this "
                                "element is heard.");
        excludeBtn_.setTooltip ("Add the current track selection as a range where this "
                                "element is absent.");
        removeBtn_.setTooltip ("Remove this description and its ranges.");

        addAndMakeVisible (numLabel_);
        addAndMakeVisible (edit_);
        addAndMakeVisible (includeBtn_);
        addAndMakeVisible (excludeBtn_);
        addAndMakeVisible (removeBtn_);

        rebuildAnchorList ();   // show any anchors this entry already carries
    }

    // The anchor widgets are raw children: a JUCE Component never deletes its
    // children, so without this every anchor line leaked a Label and a
    // TextButton when the row went away.
    ~DescRow() override
    {
        for (auto* c : anchorWidgets_) delete c;
        anchorWidgets_.clear ();
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
            btn->setTooltip ("Remove this range from the description.");
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

} // namespace

//==============================================================================
// StemSeparationWindow
//==============================================================================

struct StemSeparationWindow::RunJob
{
    std::vector<StemDescEntry> descs;
    juce::File model;
    juce::String backendCli;          // "cpu" / "cuda" / "vulkan" / "metal"
    bool extractFromOriginal = true;
    double extStartSecs = 0.0;        // extraction range in source seconds
    double extStopSecs = 0.0;
    double rate = 48000.0;
    juce::String resultName;          // suggested name for the new document
    int mode = 1;                     // 1=Sam Audio 2-5=htdemucs families
    int outputMode = 1;               // 1=new file 2=overwrite current 3=append to current
    FileDoc* docAtStart = nullptr;    // the active document when Run was clicked
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
    startEdit_.setTooltip ("Start of the range to separate, in seconds.");
    endEdit_.setTooltip ("End of the range to separate, in seconds.");

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
    selectAllBtn_.setTooltip ("Set Start and End to the whole file.");

    // Mode row: which separation family to run. Sam Audio keeps the full
    // description UI; the htdemucs modes only need model + backend.
    modeLbl_.setFont (juce::FontOptions (13.0f));
    modeLbl_.setText ("Mode:", juce::dontSendNotification);
    modeCombo_.addItem ("Sam Audio", 1);
    modeCombo_.addItem ("htdemucs_6stems", 2);
    modeCombo_.addItem ("htdemucs_ft", 3);
    modeCombo_.addItem ("htdemucs-whine-hiss_v1", 4);
    modeCombo_.addItem ("htdemucs-whine-hiss-click-bump_v1", 5);
    modeCombo_.setSelectedItemIndex (std::clamp (app_.settings.stemSepMode, 1, 5) - 1,
                                     juce::dontSendNotification);
    modeCombo_.setTooltip ("Sam Audio extracts what you describe in words; the htdemucs "
                           "modes split the sound into fixed stems.");
    modeCombo_.onChange = [this]
    {
        app_.settings.stemSepMode = modeCombo_.getSelectedItemIndex () + 1;
        app_.saveSettings ();
        refreshModelList ();   // the model list depends on the mode
        setSize (kWinW, computeHeight ());   // show/hide the description area
    };

    // Output handling for every mode: where do the separated stems go?
    outputCombo_.addItem ("Copy stems to new file", 1);
    outputCombo_.addItem ("Overwrite current file", 2);
    outputCombo_.addItem ("Append to current file", 3);
    outputCombo_.setSelectedItemIndex (std::clamp (app_.settings.stemSepOutputMode, 1, 3) - 1,
                                       juce::dontSendNotification);
    outputCombo_.setTooltip ("Where the separated stems go: a new file, in place of the "
                             "current file, or as new tracks of the current file.");
    outputCombo_.onChange = [this]
    {
        app_.settings.stemSepOutputMode = outputCombo_.getSelectedItemIndex () + 1;
        app_.saveSettings ();
    };

    modelCombo_.setTooltip ("Model weights used by the selected mode (the installed "
                            ".gguf files). Download... fetches more.");
    modelCombo_.onChange = [this]
    {
        const juce::String t = modelCombo_.getText ();
        if (!demucsMode ())
            app_.settings.acppModel = (t == "(no models installed)") ? juce::String () : t;
        else if (selectedModeId () == 2)
            app_.settings.acppModel6 = (t == "(no models installed)") ? juce::String () : t;
        else if (selectedModeId () == 4)
            app_.settings.acppModelWH = (t == "(no models installed)") ? juce::String () : t;
        else if (selectedModeId () == 5)
            app_.settings.acppModelWHCB = (t == "(no models installed)") ? juce::String () : t;
        app_.saveSettings ();
        refreshStatusIdle ();
    };
    // Cpu, Cuda, Vulkan (+ Metal on macOS): ids 1.. in sc::acpp::backendChoices() order.
    const auto backends = sc::acpp::backendChoices ();
    for (size_t i = 0; i < backends.size (); ++i)
        backendCombo_.addItem (backends[i].label, (int) i + 1);
    // This JUCE build uses 0-based combo positions; the stored setting is a
    // 1-based id (1=Cpu 2=Cuda 3=Vulkan 4=Metal), so convert in both directions.
    backendCombo_.setSelectedItemIndex (std::clamp (app_.settings.acppBackend, 1, (int) backends.size ()) - 1,
                                       juce::dontSendNotification);
    backendCombo_.setTooltip ("Processor the separation runs on. It should match the "
                              "installed audio.cpp variant.");
    backendCombo_.onChange = [this]
    {
        app_.settings.acppBackend = backendCombo_.getSelectedItemIndex () + 1;
        app_.saveSettings ();
        refreshStatusIdle ();
    };

    extractOrigToggle_.setButtonText ("Extract subsequent stem from original");
    extractOrigToggle_.setToggleState (app_.settings.acppExtractOriginal, juce::dontSendNotification);
    extractOrigToggle_.setTooltip ("On: every description works on the original audio. Off: each "
                                   "description after the first works on what the previous "
                                   "one left over.");
    extractOrigToggle_.onClick = [this]
    {
        app_.settings.acppExtractOriginal = extractOrigToggle_.getToggleState ();
        app_.saveSettings ();
    };

    descEdit_.setMultiLine (false);
    descEdit_.setColour (juce::CaretComponent::caretColourId, juce::Colours::white);
    descEdit_.setTooltip ("Describe the sound to extract, e.g. \"dog barking\" or "
                          "\"engine hum\".");

    // The constructor's restore block may have entered advanced mode already.
    advancedBtn_.setButtonText (advanced_ ? "Simple view" : "Advanced...");
    advancedBtn_.onClick = [this] { setAdvanced (!advanced_); };
    advancedBtn_.setTooltip ("Switch between one description and several, each with "
                             "include / exclude ranges.");

    // Sits below the last description row in advanced mode (see resized()).
    addDescBtn_.setButtonText ("Add Stem");
    addDescBtn_.setTooltip ("Add another description to extract in the same run.");
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
    downloadBtn_.setTooltip ("Download the audio.cpp tool and separation models.");
    runBtn_.setButtonText ("Run");
    runBtn_.onClick = [this] { startRun (); };
    runBtn_.setTooltip ("Separate the range with the chosen mode and model.");

    cancelBtn_.setButtonText ("Cancel");
    cancelBtn_.onClick = [this] { userTriedToCloseWindow (); };
    cancelBtn_.setTooltip ("Close this window without running.");

    // DocumentWindow inherits ResizableWindow, whose addAndMakeVisible() asserts;
    // call the base-class method explicitly to side-step it. (Must use
    // addAndMakeVisible - plain addChildComponent leaves the children invisible.)
    juce::Component::addAndMakeVisible (&startLbl_);
    juce::Component::addAndMakeVisible (&startEdit_);
    juce::Component::addAndMakeVisible (&endLbl_);
    juce::Component::addAndMakeVisible (&endEdit_);
    juce::Component::addAndMakeVisible (&durLbl_);
    juce::Component::addAndMakeVisible (&selectAllBtn_);
    juce::Component::addAndMakeVisible (&modeLbl_);
    juce::Component::addAndMakeVisible (&modeCombo_);
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
    juce::Component::addAndMakeVisible (&outputCombo_);
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

    // rowsHost_ holds the DescRows created with new in rebuildRows(); a JUCE
    // Component does not own its children, so closing the window in advanced
    // mode leaked every row (TextEditor, Label, buttons). Delete them while
    // descs_ (which the rows reference) is still alive.
    for (int i = rowsHost_.getNumChildComponents () - 1; i >= 0; --i)
        delete rowsHost_.getChildComponent (i);
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
    // This JUCE build uses 0-based combo positions.
    auto addPlaceholder = [this] { modelCombo_.addItem ("(no models installed)", 1); };

    modelCombo_.clear ();
    if (!demucsMode ())
    {
        const auto models = sc::acpp::listModels ();
        if (models.empty ())
            addPlaceholder ();
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
    }
    else if (selectedModeId () == 2)
    {
        const auto models = sc::acpp::listDemucs6Models ();
        if (models.empty ())
            addPlaceholder ();
        else
        {
            for (size_t i = 0; i < models.size (); ++i)
                modelCombo_.addItem (models[i].getFileName (), (int) i + 1);
            int pos = 0;
            const juce::String saved = app_.settings.acppModel6;
            for (size_t i = 0; i < models.size (); ++i)
                if (models[i].getFileName() == saved) { pos = (int) i; break; }
            modelCombo_.setSelectedItemIndex (pos, juce::dontSendNotification);
        }
    }
    else if (selectedModeId () == 3)   // htdemucs_ft: the whole folder is one "model".
    {
        if (sc::acpp::demucsFtModelDir () == juce::File ())
            addPlaceholder ();
        else
            modelCombo_.addItem ("htdemucs_ft", 1);
    }
    else   // htdemucs-whine-hiss_v1 / -click-bump_v1: one .gguf file each.
    {
        const auto models = (selectedModeId () == 4 ? sc::acpp::listWhineHissModels ()
                                                    : sc::acpp::listWhineHissClickBumpModels ());
        if (models.empty ())
            addPlaceholder ();
        else
        {
            for (size_t i = 0; i < models.size (); ++i)
                modelCombo_.addItem (models[i].getFileName (), (int) i + 1);
            int pos = 0;
            const juce::String saved = (selectedModeId () == 4 ? app_.settings.acppModelWH
                                                               : app_.settings.acppModelWHCB);
            for (size_t i = 0; i < models.size (); ++i)
                if (models[i].getFileName() == saved) { pos = (int) i; break; }
            modelCombo_.setSelectedItemIndex (pos, juce::dontSendNotification);
        }
    }

    if (modelCombo_.getSelectedItemIndex () < 0)
        modelCombo_.setSelectedItemIndex (0, juce::dontSendNotification);
    refreshStatusIdle ();
}

void StemSeparationWindow::refreshStatusIdle()
{
    if (running_.load ()) return;

    const bool hasBin = sc::acpp::hasBinary ();
    const int modelIdx = modelCombo_.getSelectedItemIndex ();   // 0-based position
    const bool demucs = demucsMode ();
    const bool hasModel = modelIdx >= 0 && modelCombo_.getText () != "(no models installed)";

    juce::String s;
    if (!hasBin)
        s = "The " + sc::acpp::binaryFileName () + " binary is not installed yet - use Download...";
    else if (demucs && !hasModel)
        s = (selectedModeId () == 2
             ? "No htdemucs_6stems model found - place a .gguf file in the HTDemucs-6stems-GGUF folder."
             : selectedModeId () == 4
               ? "No htdemucs-whine-hiss_v1 model found - use Download... or place a .gguf file in the HTDemucs-WhineHiss-v1-GGUF folder."
               : selectedModeId () == 5
                 ? "No htdemucs-whine-hiss-click-bump_v1 model found - use Download... or place a .gguf file in the HTDemucs-WhineHiss-ClickBump-v1-GGUF folder."
                 : "The htdemucs_ft model folder is not installed - create it with at least one .gguf file.");
    else if (!demucs && !hasModel)
        s = "No SAM-Audio model found in the models folder - use Download...";
    else
    {
        const juce::String variant = sc::acpp::installedVariant ();
        // Convert the 0-based position back to the stored 1-based id.
        const int backendIdx = backendCombo_.getSelectedItemIndex () + 1;
        const bool mismatch = !sc::acpp::variantSupportsBackend (variant, sc::acpp::backendForChoiceId (backendIdx));
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
    // Delete from the back: deleting a child removes it from rowsHost_'s
    // internal array, which invalidates forward pointer iteration and would
    // double-free the last row once three or more exist.
    for (int i = rowsHost_.getNumChildComponents () - 1; i >= 0; --i)
        delete rowsHost_.getChildComponent (i);

    if (!advanced_) return;

    const int w = kWinW - 2 * kMargin;
    int y = 0;
    for (size_t i = 0; i < descs_.size (); ++i)
    {
        auto* row = new DescRow ((int) i, descs_[i], this);
        const int h = DescRow::heightFor ((int) descs_[i].anchors.size (), w);
        row->setBounds (0, y, w, h);
        row->layoutChildren (w);
        rowsHost_.addAndMakeVisible (row);
        y += h + 4;
    }
}

namespace {
// Vertical layout shared by metrics() and resized().
struct LayoutYs
{
    int sourceRow = 0, modeRow = 0, modelRow = 0, backendRow = 0;
    int descAreaTop = 0;      // description editor (simple) or header row (advanced)
    int rowsTop = 0;          // top of the DescRow stack (advanced only)
    int addDescRow = 0;       // "Add Stem" button, below the last row (advanced)
    int statusTop = 0;
    int bottomRow = 0;
    int totalH = 0;

    static LayoutYs compute (bool advanced, int rowsH, bool demucs)
    {
        LayoutYs y;
        y.sourceRow   = kMargin;
        y.modeRow     = y.sourceRow + 24 + 8;             // mode dropdown row
        y.modelRow    = y.modeRow + 28 + 8;
        y.backendRow  = y.modelRow + 28 + 8;

        if (demucs)
            // No description area at all in the htdemucs modes.
            y.statusTop = y.backendRow + 28 + 8;
        else
        {
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
        }

        y.bottomRow   = y.statusTop + 40 + 8;
        y.totalH      = y.bottomRow + 28 + kMargin;
        return y;
    }
};
} // namespace

int StemSeparationWindow::computeHeight() const
{
    int rowsH = 0;
    if (advanced_ && !demucsMode ())
    {
        const int w = kWinW - 2 * kMargin;
        for (const auto& d : descs_)
            rowsH += DescRow::heightFor ((int) d.anchors.size (), w) + 4;
    }
    return LayoutYs::compute (advanced_, rowsH, demucsMode ()).totalH;
}

void StemSeparationWindow::resized()
{
    const bool demucs = demucsMode ();
    int rowsH = 0;
    if (advanced_ && !demucs)
    {
        const int w = kWinW - 2 * kMargin;
        for (const auto& d : descs_)
            rowsH += DescRow::heightFor ((int) d.anchors.size (), w) + 4;
    }
    const auto y = LayoutYs::compute (advanced_, rowsH, demucs);
    const int x = kMargin, w = getWidth () - 2 * kMargin;

    // Range row: editable start/end seconds + duration + Select All.
    startLbl_.setBounds (x, y.sourceRow + 3, 56, 18);
    startEdit_.setBounds (x + 58, y.sourceRow, 70, 24);
    endLbl_.setBounds (x + 134, y.sourceRow + 3, 48, 18);
    endEdit_.setBounds (x + 186, y.sourceRow, 70, 24);
    durLbl_.setBounds (x + 264, y.sourceRow + 3, 150, 18);
    selectAllBtn_.setBounds (getWidth () - kMargin - 96, y.sourceRow, 96, 24);

    // Mode row: which separation family to run.
    modeLbl_.setBounds (x, y.modeRow + 5, 92, 18);
    modeCombo_.setBounds (x + 96, y.modeRow, 170, 28);

    // Model / backend rows: the labels share one column and every field starts
    // at the same left coordinate.
    modelLbl_.setBounds (x, y.modelRow + 5, 92, 18);
    modelCombo_.setBounds (x + 96, y.modelRow, w - 96, 28);

    backendLbl_.setBounds (x, y.backendRow + 5, 92, 18);
    backendCombo_.setBounds (x + 96, y.backendRow, 110, 28);

    if (demucs)
    {
        // The htdemucs modes have no description area at all.
        descLbl_.setBounds (0, 0, 0, 0);
        descEdit_.setBounds (0, 0, 0, 0);
        advancedBtn_.setBounds (0, 0, 0, 0);
        addDescBtn_.setBounds (0, 0, 0, 0);
        extractOrigToggle_.setBounds (0, 0, 0, 0);
        advancedHint_.setBounds (0, 0, 0, 0);
        rowsHost_.setBounds (0, 0, 0, 0);
    }
    else if (!advanced_)
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
        rowsHost_.setBounds (0, 0, 0, 0);   // no rows in simple mode
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
        // The row stack grows DOWNWARD from rowsTop: the "Add Stem" button and
        // the status line below it are positioned relative to its bottom.
        rowsHost_.setBounds (x, y.rowsTop, w, std::max (0, rowsH));
    }

    statusLabel_.setBounds (x, y.statusTop, w, 40);
    outputCombo_.setBounds (x, y.bottomRow, 210, 28);   // bottom-left, clear of Cancel/Download/Run
    runBtn_.setBounds (getWidth () - kMargin - 90, y.bottomRow, 90, 28);
    downloadBtn_.setBounds (runBtn_.getX () - 110, y.bottomRow, 104, 28);
    cancelBtn_.setBounds (downloadBtn_.getX () - 96, y.bottomRow, 88, 28);

    if (advanced_ && !demucs) rebuildRows ();   // re-lay out rows for the current width
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

    // Downloadable models of the currently selected mode. The htdemucs_6stems
    // and htdemucs_ft families have no download catalogue - their .gguf files
    // are placed in their folders manually (the status line says where).
    struct ModelRow { std::string file; juce::String folder; int64_t sizeBytes = 0; juce::String url; };
    std::vector<ModelRow> modelRows;
    const int modeId = selectedModeId ();
    if (modeId == 1)
    {
        for (const auto& m : sc::acpp::modelChoices ())
            modelRows.push_back ({ m.file, {}, m.sizeBytes, sc::acpp::modelUrl (m.file) });
    }
    else if (modeId == 4 || modeId == 5)
    {
        for (const auto& m : sc::acpp::demucsEngineChoices ())
            if (m.mode == modeId)
                modelRows.push_back ({ m.file,
                                       modeId == 4 ? juce::String ("HTDemucs-WhineHiss-v1-GGUF")
                                                   : juce::String ("HTDemucs-WhineHiss-ClickBump-v1-GGUF"),
                                       m.sizeBytes, sc::acpp::demucsEngineUrl (m.file) });
    }

    // Owning: the Label/ComboBox rows below are added with new.
    const bool offerDownload = needBinary || !modelRows.empty ();
    sc::OwningAlertWindow dlg ("Stem Separation setup",
        offerDownload ? juce::String ()
                      : "Nothing to download for the current mode.\nThe htdemucs_6stems / htdemucs_ft models are placed manually in their folders.",
        juce::AlertWindow::NoIcon, this);
    if (offerDownload)
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
        cb->setTooltip ("Build of the audio.cpp tool to install. The Backend setting must "
                        "match it.");
        dlg.addCustomComponent (cb);   // slot customSlot + 1
        binaryComboSlot = customSlot + 1;
        customSlot += 2;
    }

    if (!modelRows.empty ())
    {
        auto* lbl = new juce::Label();
        lbl->setText (modeId == 1 ? "SAM-Audio model:"
                                  : modeId == 4 ? "htdemucs-whine-hiss_v1 model:"
                                                : "htdemucs-whine-hiss-click-bump_v1 model:",
                     juce::dontSendNotification);
        dlg.addCustomComponent (lbl);   // slot customSlot
        auto* cb = new juce::ComboBox ();
        for (size_t i = 0; i < modelRows.size (); ++i)
            cb->addItem (juce::String (modelRows[i].file) + "   ("
                         + juce::String ((double) modelRows[i].sizeBytes / 1.0e6, 1) + " MB)",
                         (int) i + 1);
        // This JUCE build uses 0-based combo positions.
        cb->setSelectedItemIndex (0, juce::dontSendNotification);
        cb->setTooltip ("Model file to download, with its size.");
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
    std::thread ([this, guard = alive_, prog, needBinary, modelRows,
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
            const juce::File zip = root.getChildFile (sc::acpp::archiveFileName ());   // .zip or .tar.gz

            setStatus ("Downloading " + choices[i].label + "...");
            ok = sc::acpp::downloadFile (choices[i].url, zip,
                [&] (float f) { sendProgress (f); return !prog->cancelled.load (); }, &err,
                choices[i].sizeBytes);
            if (!ok && !prog->cancelled.load ())
                setStatus ("Download failed: " + juce::String (err));

            if (ok)
            {
                setStatus ("Extracting the binary...");
                ok = sc::acpp::installBinaryFromArchive (zip, choices[i].id, root, &err);
                if (!ok && !prog->cancelled.load ())
                    setStatus ("Extraction failed: " + juce::String (err));
            }
        }

        if (ok && !modelRows.empty ())
        {
            const size_t i = (size_t) std::min (std::max (1, modelChoice), (int) modelRows.size ()) - 1;
            juce::File destDir = root;
            if (!modelRows[i].folder.isEmpty ())
                destDir = destDir.getChildFile (modelRows[i].folder);
            const juce::File dest = destDir.getChildFile (modelRows[i].file);

            lastSent = -1.0;
            setStatus ("Downloading " + modelRows[i].file + "...");
            ok = sc::acpp::downloadFile (modelRows[i].url.toStdString (),
                                         dest,
                [&] (float f) { sendProgress (f); return !prog->cancelled.load (); }, &err,
                modelRows[i].sizeBytes);
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

    const int modeId = selectedModeId ();   // 1=Sam Audio, 2-5=htdemucs families
    const bool demucs = modeId >= 2;

    // Collect the descriptions to run (Sam Audio only - the htdemucs modes
    // need no description).
    std::vector<StemDescEntry> descs;
    if (!demucs)
    {
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
    }

    if (!sc::acpp::hasBinary ())
    { openDownloadDialog (); return; }

    // Resolve the model for the selected mode.
    juce::File model;
    if (demucs && modeId != 3)
    {
        const auto models = (modeId == 2 ? sc::acpp::listDemucs6Models ()
                              : modeId == 4 ? sc::acpp::listWhineHissModels ()
                                            : sc::acpp::listWhineHissClickBumpModels ());
        const juce::String missingMsg = (modeId == 2
            ? "The htdemucs_6stems model is not installed - place a .gguf file in the HTDemucs-6stems-GGUF folder."
            : modeId == 4
              ? "The htdemucs-whine-hiss_v1 model is not installed - use Download... or place a .gguf file in the HTDemucs-WhineHiss-v1-GGUF folder."
              : "The htdemucs-whine-hiss-click-bump_v1 model is not installed - use Download... or place a .gguf file in the HTDemucs-WhineHiss-ClickBump-v1-GGUF folder.");
        const int mi = modelCombo_.getSelectedItemIndex ();   // 0-based position
        if (models.empty () || mi < 0 || mi >= (int) models.size ())
        { statusLabel_.setText (missingMsg, juce::dontSendNotification); return; }
        model = models[(size_t) mi];
    }
    else if (demucs && modeId == 3)
    {
        model = sc::acpp::demucsFtModelDir ();
        if (model == juce::File ())
        { statusLabel_.setText ("The htdemucs_ft model folder is not installed - create it with at least one .gguf file.", juce::dontSendNotification); return; }
    }
    else
    {
        const auto models = sc::acpp::listModels ();
        if (models.empty ())
        { openDownloadDialog (); return; }
        const int mi = modelCombo_.getSelectedItemIndex ();   // 0-based position
        if (mi < 0)
        { statusLabel_.setText ("Pick a model first.", juce::dontSendNotification); return; }
        model = models[(size_t) mi];
    }

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
    job.model = model;
    // Convert the 0-based position back to the stored 1-based id.
    const int bi = backendCombo_.getSelectedItemIndex () + 1;
    job.backendCli = sc::acpp::backendForChoiceId (bi);
    job.extractFromOriginal = extractOrigToggle_.getToggleState ();
    job.extStartSecs = s0;
    job.extStopSecs  = s1;
    job.rate         = eng.sampleRate ();
    job.resultName   = (app_.doc () != nullptr ? app_.doc()->name : juce::String ("stem")) + "_stems";
    job.mode         = modeId;
    job.outputMode   = outputCombo_.getSelectedItemIndex () + 1;   // 0-based position -> 1-based id
    job.docAtStart   = app_.doc ();

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

        auto fail = [this, guard, prog] (const juce::String& msg, const juce::String& detail = {})
        {
            juce::MessageManager::callAsync ([this, guard, prog, msg, detail] {
                delete prog;
                if (!guard || !guard->load ()) return;   // window closed meanwhile
                running_ = false;
                statusLabel_.setText (msg, juce::dontSendNotification);
                if (!detail.isEmpty ())
                    sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                                      "Stem Separation failed",
                                                      msg + "\n\n" + detail, "", this);
            });
        };

        // 1. Read the input range (the separation itself is headless: sc::StemService).
        auto& eng = app_.audioEngine ();
        const int64_t startFrame = (int64_t) std::llround (j.extStartSecs * j.rate);
        const int64_t nFrames    = (int64_t) std::llround ((j.extStopSecs - j.extStartSecs) * j.rate);
        if (nFrames <= 0) { fail ("The selected range is empty."); return; }

        sc::StemService::Request req;
        if (!eng.readRange (startFrame, nFrames, req.input) || req.input.empty () || req.input[0].empty ())
        { fail ("Could not read the selected range."); return; }

        req.mode = (j.mode == 2 ? sc::StemService::Mode::HtDemucs6
                    : j.mode == 3 ? sc::StemService::Mode::HtDemucsFt
                    : j.mode == 4 ? sc::StemService::Mode::HtWhineHiss
                                  : j.mode == 5 ? sc::StemService::Mode::HtWhineHissClickBump
                                                : sc::StemService::Mode::SamAudio);
        for (const auto& d : j.descs)
            req.descriptions.push_back ({ d.text, d.anchors });
        req.model = j.model;
        req.backend = j.backendCli;
        req.extractFromOriginal = j.extractFromOriginal;
        req.extStartSecs = j.extStartSecs;
        req.sourceRate = j.rate;

        // 2-3. Run the CLI and bring every track to the source rate.
        auto res = sc::StemService::run (req,
            [prog] (double fraction, const juce::String& status) {
                if (status.isNotEmpty ())
                    juce::MessageManager::callAsync ([prog, status] { prog->setStatusText (status); });
                if (fraction >= 0.0)
                    juce::MessageManager::callAsync ([prog, fraction] { prog->setProgress (fraction); });
            },
            [prog] { return prog->cancelled.load (); });

        if (!res.ok)
        {
            fail (res.error, res.detail);
            return;
        }

        sc::MergeResult result = std::move (res.merged);
        const int nTracks = result.channels;

        // 4. Create the new document on the message thread. The result is
        // heap-held because this MSVC build rejects `mutable` lambdas with
        // init-captures, and createDocFromMerged takes a non-const reference.
        auto* resPtr = new sc::MergeResult (std::move (result));
        const juce::String kindMsg = (j.mode >= 2)
            ? "named stem tracks."
            : "tracks - one target per description plus the final residual.";
        juce::MessageManager::callAsync ([this, guard, prog, resPtr,
                                          name = j.resultName, nTracks,
                                          outMode = j.outputMode, extStart = j.extStartSecs,
                                          docAtStart = j.docAtStart, kindMsg] {
            delete prog;
            if (!guard || !guard->load ()) { delete resPtr; return; }
            running_ = false;
            std::string err;

            // If the user switched documents while the run was in flight, an
            // overwrite/append would hit the wrong file: fall back to creating
            // a new document instead of losing the result.
            const bool sourceStillActive = (docAtStart != nullptr && app_.doc () == docAtStart);

            if (outMode == 1 || !sourceStillActive)
            {
                if (!app_.createDocFromMerged (*resPtr, &err, name))
                    statusLabel_.setText ("Could not create the result document: " + juce::String (err),
                                          juce::dontSendNotification);
                else
                {
                    statusLabel_.setText ((sourceStillActive ? "Created '" : "The source document changed - created '")
                        + app_.docs.back()->name + "' with "
                        + juce::String (nTracks) + " " + kindMsg,
                                          juce::dontSendNotification);
                    rememberDescs ();   // keep the typed descriptions for next time
                    if (onCloseRequested) onCloseRequested ();   // close on success
                }
            }
            else if (!app_.applyStemSeparationResult (*resPtr, outMode, extStart, name, &err))
            {
                statusLabel_.setText ("Could not apply the result to the current file: " + juce::String (err),
                                      juce::dontSendNotification);
            }
            else
            {
                statusLabel_.setText ((outMode == 2 ? "Replaced the current file with " : "Appended ")
                    + juce::String (nTracks) + " stem track(s).",
                                      juce::dontSendNotification);
                rememberDescs ();   // keep the typed descriptions for next time
                if (onCloseRequested) onCloseRequested ();   // close on success
            }
            delete resPtr;
        });
    }).detach ();
}
