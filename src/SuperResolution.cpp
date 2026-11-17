// SpectroCraft - Effects > Super Resolution (external audio.cpp CLI tool).
#include "SuperResolution.h"
#include "AI/SuperResService.h"   // the headless super-resolution run (no UI)
#include "AlertGate.h"
#include "AudioCpp.h"
#include "OwningAlertWindow.h"   // dialogs with new-ed custom components
#include "WorkProgress.h"
#include "AcppSetup.h"           // Run downloads what is missing, after asking (issue #38)

namespace {

constexpr int kWinW   = 620;
constexpr int kMargin = 12;

// Fixed layout (no dynamic rows): range, mode, model, backend, status, buttons.
struct LayoutYs
{
    int sourceRow   = kMargin;
    int modeRow     = sourceRow + 24 + 8;
    int modelRow    = modeRow + 28 + 8;
    int backendRow  = modelRow + 28 + 8;
    int statusTop   = backendRow + 28 + 8;
    int bottomRow   = statusTop + 40 + 8;
    int totalH      = bottomRow + 28 + kMargin;
};

} // namespace

//==============================================================================
// Window
//==============================================================================

struct SuperResolutionWindow::RunJob
{
    juce::File model;
    juce::String backendCli;          // "cpu" / "cuda" / "vulkan" / "metal"
    double extStartSecs = 0.0;        // extraction range in source seconds
    double extStopSecs = 0.0;
    double rate = 48000.0;            // source document rate
    juce::String resultName;          // suggested name for the new document
    int mode = 1;                     // 1=FlashSR 2=AudioSR
    int outputMode = 1;               // 1=new file 2=overwrite current 3=append to current
    FileDoc* docAtStart = nullptr;    // the active document when Run was clicked
};

SuperResolutionWindow::SuperResolutionWindow(Application& app)
    : juce::DocumentWindow ("Super Resolution", juce::Colour (0xff3a3f4b),
                            juce::DocumentWindow::minimiseButton | juce::DocumentWindow::closeButton),
      app_ (app),
      alive_ (std::make_shared<std::atomic<bool>> (true))
{
    // Use the OS title bar (like MainWindow and the other dialogs): it shows
    // the "Super Resolution" title properly, while the JUCE-drawn fallback
    // renders broken glyphs on some platforms.
    setUsingNativeTitleBar (true);

    startLbl_.setFont (juce::FontOptions (13.0f));
    startLbl_.setText ("Start (s)", juce::dontSendNotification);
    endLbl_.setFont (juce::FontOptions (13.0f));
    endLbl_.setText ("End (s)", juce::dontSendNotification);
    modeLbl_.setFont (juce::FontOptions (13.0f));
    modeLbl_.setText ("Mode:", juce::dontSendNotification);
    modelLbl_.setFont (juce::FontOptions (13.0f));
    modelLbl_.setText ("Model:", juce::dontSendNotification);
    backendLbl_.setFont (juce::FontOptions (13.0f));
    backendLbl_.setText ("Backend:", juce::dontSendNotification);
    durLbl_.setFont (juce::FontOptions (12.5f));

    for (auto* e : { &startEdit_, &endEdit_ })
    {
        e->setMultiLine (false);
        e->setColour (juce::CaretComponent::caretColourId, juce::Colours::white);
        e->addListener (this);   // keep the duration label in sync
    }
    startEdit_.setTooltip ("Start of the range to enhance, in seconds.");
    endEdit_.setTooltip ("End of the range to enhance, in seconds.");

    // Seed the range from the current track selection ONCE. Later changes to
    // that selection must not update these fields: the run works on a stable
    // per-run extraction range.
    {
        const double dur = app_.audioEngine().isFileLoaded () ? app_.audioEngine().duration () : 0.0;
        auto sel = app_.selection;
        double s0 = 0.0, s1 = dur;
        if (sel.getLength () > 0.0) { s0 = sel.getStart (); s1 = sel.getEnd (); }
        startEdit_.setText (juce::String (s0, 3), juce::dontSendNotification);
        endEdit_.setText (juce::String (s1, 3), juce::dontSendNotification);
    }

    refreshDurationLabel ();

    selectAllBtn_.setButtonText ("Select All");
    selectAllBtn_.onClick = [this] { doSelectAll (); };
    selectAllBtn_.setTooltip ("Set Start and End to the whole file.");

    // Mode row: which super-resolution family to run. Both emit a single
    // enhanced mono 48 kHz track regardless of the input.
    modeCombo_.addItem ("FlashSR", 1);
    modeCombo_.addItem ("AudioSR", 2);
    modeCombo_.setSelectedItemIndex (std::clamp (app_.settings.superResMode, 1, 2) - 1,
                                     juce::dontSendNotification);
    modeCombo_.setTooltip ("FlashSR is small and fast; AudioSR uses a large model. Both give "
                           "one enhanced mono 48 kHz track.");
    modeCombo_.onChange = [this]
    {
        app_.settings.superResMode = modeCombo_.getSelectedItemIndex () + 1;
        app_.saveSettings ();
        refreshModelList ();   // the model list depends on the mode
        refreshStatusIdle ();
    };

    // Output handling: where does the enhanced track go?
    outputCombo_.addItem ("Copy result to new file", 1);
    outputCombo_.addItem ("Overwrite current file", 2);
    outputCombo_.addItem ("Append to current file", 3);
    outputCombo_.setSelectedItemIndex (std::clamp (app_.settings.superResOutputMode, 1, 3) - 1,
                                       juce::dontSendNotification);
    outputCombo_.setTooltip ("Where the enhanced track goes: a new file, in place of the "
                             "current file, or as a new track of the current file.");
    outputCombo_.onChange = [this]
    {
        app_.settings.superResOutputMode = outputCombo_.getSelectedItemIndex () + 1;
        app_.saveSettings ();
    };

    modelCombo_.setTooltip ("Model weights used by the selected mode: every version, one "
                            "marked (Download Required) is downloaded when you press Run.");
    modelCombo_.onChange = [this]
    {
        if (const auto* o = selectedOption ())
        {
            app_.settings.superResModel = o->name;
            app_.saveSettings ();
        }
        refreshStatusIdle ();
    };

    // Cpu, Cuda, Vulkan (+ Metal on macOS): ids 1.. in sc::acpp::backendChoices() order.
    const auto backends = sc::acpp::backendChoices ();
    for (size_t i = 0; i < backends.size (); ++i)
        backendCombo_.addItem (backends[i].label, (int) i + 1);
    // This JUCE build uses 0-based combo positions; the stored setting is a
    // 1-based id (1=Cpu 2=Cuda 3=Vulkan 4=Metal), so convert in both directions. The
    // backend choice is shared with Stem Separation - same CLI binary.
    backendCombo_.setSelectedItemIndex (std::clamp (app_.settings.acppBackend, 1, (int) backends.size ()) - 1,
                                        juce::dontSendNotification);
    backendCombo_.setTooltip ("Processor the enhancement runs on. It should match the "
                              "installed audio.cpp variant (shared with Stem Separation).");
    backendCombo_.onChange = [this]
    {
        app_.settings.acppBackend = backendCombo_.getSelectedItemIndex () + 1;
        app_.saveSettings ();
        refreshStatusIdle ();
    };

    statusLabel_.setJustificationType (juce::Justification (juce::Justification::left));
    statusLabel_.setFont (juce::FontOptions (12.0f));

    runBtn_.setButtonText ("Run");
    runBtn_.onClick = [this] { startRun (); };
    runBtn_.setTooltip ("Enhance the range with the chosen mode and model. A missing model "
                        "or audio.cpp tool is downloaded first, after you confirm.");

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
    juce::Component::addAndMakeVisible (&statusLabel_);
    juce::Component::addAndMakeVisible (&outputCombo_);
    juce::Component::addAndMakeVisible (&cancelBtn_);
    juce::Component::addAndMakeVisible (&runBtn_);

    refreshModelList ();
    centreWithSize (kWinW, LayoutYs{}.totalH);
    // Stay above the main window while open: JUCE has no per-window z-order,
    // so use global topmost for the lifetime of the dialog.
    setAlwaysOnTop (true);
    setVisible (true);
}

SuperResolutionWindow::~SuperResolutionWindow()
{
    if (alive_) alive_->store (false);   // stop any pending worker callbacks
}

void SuperResolutionWindow::userTriedToCloseWindow()
{
    // The owner's callback releases the unique_ptr that owns this window, so
    // `this` is destroyed while we are still inside this call. JUCE's WndProc
    // does not touch the component after handleUserClosingWindow() returns,
    // which makes this safe (same idiom as a self-deleting popup).
    if (onCloseRequested) onCloseRequested ();
}

void SuperResolutionWindow::closeButtonPressed()
{
    userTriedToCloseWindow ();
}

//==============================================================================
// Layout / status
//==============================================================================

juce::File SuperResolutionWindow::modelsRoot() const
{
    // A default-constructed File is the "no override" sentinel.
    return (modelsRoot_ != juce::File ()) ? modelsRoot_ : sc::acpp::dir ();
}

void SuperResolutionWindow::resized()
{
    const auto y = LayoutYs {};
    const int x = kMargin, w = getWidth () - 2 * kMargin;

    // Range row: editable start/end seconds + duration + Select All.
    startLbl_.setBounds (x, y.sourceRow + 3, 56, 18);
    startEdit_.setBounds (x + 58, y.sourceRow, 70, 24);
    endLbl_.setBounds (x + 134, y.sourceRow + 3, 48, 18);
    endEdit_.setBounds (x + 186, y.sourceRow, 70, 24);
    durLbl_.setBounds (x + 264, y.sourceRow + 3, 150, 18);
    selectAllBtn_.setBounds (getWidth () - kMargin - 96, y.sourceRow, 96, 24);

    // Mode row: which super-resolution family to run.
    modeLbl_.setBounds (x, y.modeRow + 5, 92, 18);
    modeCombo_.setBounds (x + 96, y.modeRow, 170, 28);

    // Model / backend rows: the labels share one column and every field starts
    // at the same left coordinate.
    modelLbl_.setBounds (x, y.modelRow + 5, 92, 18);
    modelCombo_.setBounds (x + 96, y.modelRow, w - 96, 28);

    backendLbl_.setBounds (x, y.backendRow + 5, 92, 18);
    backendCombo_.setBounds (x + 96, y.backendRow, 110, 28);

    statusLabel_.setBounds (x, y.statusTop, w, 40);
    outputCombo_.setBounds (x, y.bottomRow, 210, 28);   // bottom-left, clear of Cancel/Download/Run
    runBtn_.setBounds (getWidth () - kMargin - 90, y.bottomRow, 90, 28);
    cancelBtn_.setBounds (runBtn_.getX () - 96, y.bottomRow, 88, 28);
}

void SuperResolutionWindow::textEditorTextChanged(juce::TextEditor& editor)
{
    if (&editor == &startEdit_ || &editor == &endEdit_)
        refreshDurationLabel ();
}

const sc::acpp::ModelOption* SuperResolutionWindow::selectedOption() const
{
    const int i = modelCombo_.getSelectedItemIndex ();   // 0-based position
    return i >= 0 && i < (int) modelOptions_.size () ? &modelOptions_[(size_t) i] : nullptr;
}

void SuperResolutionWindow::refreshModelList()
{
    // Every version of the mode's model (issue #38): the installed ones as
    // they are, the others marked "(Download Required)" - Run downloads them.
    // This JUCE build uses 0-based combo positions.
    modelCombo_.clear ();
    modelOptions_ = sc::acpp::modelOptions (selectedModeId () == 2 ? "audiosr" : "flashsr", modelsRoot ());
    if (modelOptions_.empty ())
        modelCombo_.addItem ("(no models installed)", 1);
    else
    {
        for (size_t i = 0; i < modelOptions_.size (); ++i)
            modelCombo_.addItem (sc::aidl::label (modelOptions_[i].name, modelOptions_[i].installed ()), (int) i + 1);
        // The last used version, else the first installed one, else the first.
        const juce::String saved = app_.settings.superResModel;
        int pos = -1;
        for (size_t i = 0; i < modelOptions_.size () && pos < 0; ++i)
            if (modelOptions_[i].name == saved) pos = (int) i;
        for (size_t i = 0; i < modelOptions_.size () && pos < 0; ++i)
            if (modelOptions_[i].installed ()) pos = (int) i;
        modelCombo_.setSelectedItemIndex (std::max (0, pos), juce::dontSendNotification);
    }
    refreshStatusIdle ();
}

void SuperResolutionWindow::refreshStatusIdle()
{
    if (running_.load ()) return;

    const bool hasBin = sc::acpp::hasBinary (modelsRoot ());
    const auto* opt = selectedOption ();

    juce::String s;
    if (opt == nullptr)
        s = "No model is known for this mode.";
    else if (!hasBin || !opt->installed ())
    {
        juce::StringArray missing;
        if (!hasBin) missing.add ("the audio.cpp tool");
        if (!opt->installed ()) missing.add (opt->name);
        s = "Run downloads " + missing.joinIntoString (" and ") + " first (you are asked before).";
    }
    else
    {
        const juce::String variant = sc::acpp::installedVariant (modelsRoot ());
        // Convert the 0-based position back to the stored 1-based id.
        const int backendIdx = backendCombo_.getSelectedItemIndex () + 1;
        const bool mismatch = !sc::acpp::variantSupportsBackend (variant, sc::acpp::backendForChoiceId (backendIdx));
        s = juce::String ("Ready: ") + opt->name
          + (variant.isEmpty () ? juce::String ("") : juce::String ("  [") + variant + "]");
        if (mismatch)
            s += "  - the installed binary may not support this backend";
    }

    statusLabel_.setText (s, juce::dontSendNotification);
}

void SuperResolutionWindow::refreshDurationLabel()
{
    const double a = startEdit_.getText ().getDoubleValue ();
    const double b = endEdit_.getText ().getDoubleValue ();
    durLbl_.setText ("Duration: " + juce::String (std::max (0.0, b - a), 3) + " s",
                     juce::dontSendNotification);
}

void SuperResolutionWindow::doSelectAll()
{
    const double dur = app_.audioEngine().isFileLoaded () ? app_.audioEngine().duration () : 0.0;
    startEdit_.setText (juce::String (0.0, 3), juce::dontSendNotification);
    endEdit_.setText (juce::String (dur, 3), juce::dontSendNotification);
    refreshDurationLabel ();
}

//==============================================================================
// Run
//==============================================================================

void SuperResolutionWindow::startRun()
{
    if (running_.load ()) return;

    auto& eng = app_.audioEngine();
    if (!eng.isFileLoaded())
    { statusLabel_.setText ("Load a file first.", juce::dontSendNotification); return; }

    const int modeId = selectedModeId ();   // 1=FlashSR, 2=AudioSR

    // Resolve the model for the selected mode.
    const auto* opt = selectedOption ();
    if (opt == nullptr)
    { statusLabel_.setText ("Pick a model first.", juce::dontSendNotification); return; }

    // The audio.cpp tool or the chosen model missing (issue #38): ask once,
    // download, then run.
    if (!sc::acpp::hasBinary (modelsRoot ()) || !opt->installed ())
    {
        const int bi = backendCombo_.getSelectedItemIndex () + 1;   // 0-based position -> 1-based id
        int binaryChoice = -1;
        if (!sc::acpp::confirmMissing ("Super Resolution", modelsRoot (), sc::acpp::backendForChoiceId (bi),
                                       opt, this, &binaryChoice))
        {
            statusLabel_.setText ("Nothing was downloaded or run.", juce::dontSendNotification);
            return;
        }
        app_.settings.superResModel = opt->name;   // the list selects it again once installed
        app_.saveSettings ();
        std::optional<sc::acpp::ModelOption> want;
        if (!opt->installed ()) want = *opt;
        statusLabel_.setText ("Downloading...", juce::dontSendNotification);
        sc::acpp::downloadMissing (this, modelsRoot (), binaryChoice, want, alive_,
            [this] (bool ok, bool cancelled, juce::String err)
            {
                refreshModelList ();
                if (ok)
                    startRun ();
                else
                    statusLabel_.setText (cancelled ? juce::String ("Download cancelled - nothing was run.")
                                                    : "Download failed: " + err, juce::dontSendNotification);
            });
        return;
    }
    const juce::File model = opt->file;

    // Capture the extraction range from the editable fields on the message
    // thread (they were seeded from the track selection when the window
    // opened and are independent of later changes to it).
    const double dur = eng.duration ();
    double s0 = jlimit (0.0, dur, startEdit_.getText ().getDoubleValue ());
    double s1 = jlimit (0.0, dur, endEdit_.getText ().getDoubleValue ());
    if (s1 <= s0)
    { statusLabel_.setText ("The end time must be after the start time.", juce::dontSendNotification); return; }

    RunJob job;
    job.model = model;
    // Convert the 0-based position back to the stored 1-based id.
    const int bi = backendCombo_.getSelectedItemIndex () + 1;
    job.backendCli = sc::acpp::backendForChoiceId (bi);
    job.extStartSecs = s0;
    job.extStopSecs  = s1;
    job.rate         = eng.sampleRate ();
    job.resultName   = (app_.doc () != nullptr ? app_.doc()->name : juce::String ("superres")) + "_superres";
    job.mode         = modeId;
    job.outputMode   = outputCombo_.getSelectedItemIndex () + 1;   // 0-based position -> 1-based id
    job.docAtStart   = app_.doc ();

    running_ = true;
    statusLabel_.setText ("Starting...", juce::dontSendNotification);
    runWorker (std::make_unique<RunJob> (std::move (job)));
}

void SuperResolutionWindow::runWorker(std::unique_ptr<RunJob> job)
{
    auto* prog = new sc::WorkProgressDialog ("Super Resolution", "Preparing input...", true, nullptr);
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
                                                      "Super Resolution failed",
                                                      msg + "\n\n" + detail, "", this);
            });
        };

        // 1. Read the input range at the source rate/channel count (the run
        //    itself is headless: sc::SuperResService).
        auto& eng = app_.audioEngine ();
        const int64_t startFrame = (int64_t) std::llround (j.extStartSecs * j.rate);
        const int64_t nFrames    = (int64_t) std::llround ((j.extStopSecs - j.extStartSecs) * j.rate);
        if (nFrames <= 0) { fail ("The selected range is empty."); return; }

        sc::SuperResService::Request req;
        if (!eng.readRange (startFrame, nFrames, req.input) || req.input.empty () || req.input[0].empty ())
        { fail ("Could not read the selected range."); return; }

        req.mode = (j.mode == 2 ? sc::SuperResService::Mode::AudioSR : sc::SuperResService::Mode::FlashSR);
        req.binary = sc::acpp::binaryPath (modelsRoot ());
        req.model = j.model;
        req.backend = j.backendCli;
        req.sourceRate = j.rate;
        // The append output mode needs the result at the source document's
        // rate so it aligns with the range.
        req.resampleToSourceRate = (j.outputMode == 3);

        // 2-3. Run the CLI and read the enhanced result back.
        auto res = sc::SuperResService::run (req,
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

        // 4. Apply the result on the message thread. The result is heap-held
        //    because this MSVC build rejects `mutable` lambdas with
        //    init-captures, and applySuperResolutionResult takes a non-const
        //    reference.
        auto* resPtr = new sc::MergeResult (std::move (result));
        juce::MessageManager::callAsync ([this, guard, prog, resPtr,
                                          name = j.resultName,
                                          outMode = j.outputMode, extStart = j.extStartSecs,
                                          docAtStart = j.docAtStart] {
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
                    // Wrap the ternary in juce::String (pointer-plus-pointer
                    // would not compile against JUCE's free operator+).
                    statusLabel_.setText (juce::String (sourceStillActive ? "Created '" : "The source document changed - created '")
                        + app_.docs.back()->name + "' with the enhanced track.",
                                          juce::dontSendNotification);
                    if (onCloseRequested) onCloseRequested ();   // close on success
                }
            }
            else if (!app_.applySuperResolutionResult (*resPtr, outMode, extStart, name, &err))
            {
                statusLabel_.setText ("Could not apply the result: " + juce::String (err),
                                      juce::dontSendNotification);
            }
            else
            {
                statusLabel_.setText ((outMode == 2 ? "Replaced the current file with the enhanced track."
                                                    : "Appended the enhanced track as a new channel."),
                                      juce::dontSendNotification);
                if (onCloseRequested) onCloseRequested ();   // close on success
            }

            delete resPtr;
        });
    }).detach ();
}
