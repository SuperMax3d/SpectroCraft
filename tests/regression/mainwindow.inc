// Regression tests for the 2026-11 audit fixes (mainwindow). Included by tests/UnitTests.cpp;
// uses its helpers (CHECK, progress, writeWav, makeTempFile, ...).

static juce::File regMwWriteWav (const juce::String& name, float value, int64_t frames = 8000)
{
    const juce::File wav = makeTempFile (name);
    writeWav (wav, 1, 8000.0, frames, [value] (int, int64_t, double) { return value; });
    return wav;
}

// Waits for a background save started by runSaveAsync: its completion
// callback deletes the progress dialog first and then does the rest of its
// work in the same message, so "no extra dialog alive" means "callback done".
static bool regMwWaitJobsBackTo (int jobs, int timeoutMs = 10000)
{
    return scPumpUntil ([jobs] { return sc::WorkProgressDialog::numActive () <= jobs; }, timeoutMs);
}

// NEW:filtereq-dialog-uaf - juce::AlertWindow does not delete its custom
// components. The Filter EQ preset combo leaked, and its pending async change
// message later ran onChange (capturing the MainWindow) after the window was
// destroyed: SIGBUS in parseUserPresets. The dialog must own (and delete) them.
static void regMwFilterEqDialogOwnsComponents()
{
    progress ("regression mainwindow: Filter EQ dialog owns its components");
    const juce::File wav = regMwWriteWav ("reg_mw_eq_owns.wav", 0.0f);
    Application app;
    CHECK (app.openFile (wav), "reg mw: opened eq wav");
    std::unique_ptr<MainWindow> win (new MainWindow (app, "reg mw eq owns"));

    juce::AlertWindow* w = win->createFilterEqDialog ();
    juce::Component::SafePointer<juce::ComboBox> combo (findComboRecursive (*w));
    juce::Component::SafePointer<juce::Component> editor;
    for (auto* c : w->getChildren ())
        if (dynamic_cast<EqCurveEditor*> (c) != nullptr) editor = c;
    CHECK (combo != nullptr && editor != nullptr, "reg mw: eq dialog has its combo and curve editor");

    // A preset picked right before the dialog closes: the change message is
    // still queued (ComboBox notifies asynchronously) when the dialog dies.
    if (combo != nullptr)
        combo->setSelectedItemIndex (1);
    delete w;

    CHECK (combo == nullptr, "reg mw: deleting the Filter EQ dialog deletes its preset combo");
    CHECK (editor == nullptr, "reg mw: deleting the Filter EQ dialog deletes its curve editor");

    // Before the fix the combo leaked: disarm it so the pending message cannot
    // crash the remaining tests once the window is gone.
    if (combo != nullptr)
        combo->onChange = nullptr;
    win.reset ();
    regDrainMessages ();   // run the change message that was pending, if any
    wav.deleteFile ();
}

// Same ownership for the LabeledField rows (their label/editor children) used
// by every generator/effect dialog.
static void regMwLabeledFieldRowsAreDeleted()
{
    progress ("regression mainwindow: LabeledField rows deleted with the dialog");
    const juce::File wav = regMwWriteWav ("reg_mw_rows.wav", 0.0f);
    Application app;
    CHECK (app.openFile (wav), "reg mw: opened rows wav");
    app.setSelection ({});   // no selection -> the duration row is shown
    std::unique_ptr<MainWindow> win (new MainWindow (app, "reg mw rows"));

    juce::AlertWindow* w = win->createGenerateSilenceDialog ();
    CHECK (w != nullptr, "reg mw: silence dialog created");
    if (w == nullptr) { wav.deleteFile (); return; }
    juce::Component::SafePointer<juce::TextEditor> dur (findEditorRecursive (*w, "genDuration"));
    CHECK (dur != nullptr, "reg mw: silence dialog has its duration row");
    delete w;
    CHECK (dur == nullptr, "reg mw: the duration row's editor is deleted with the dialog");

    win.reset ();
    wav.deleteFile ();
}

// NEW:Apercu Content Aware Time Scale - deleting the dialog while the
// preview analysis may still run must delete the panel, wait for the worker
// and skip the queued delivery (alive_ captured by value).
static void regMwCatDialogTeardown()
{
    progress ("regression mainwindow: CAT dialog teardown");
    const juce::File wav = makeTempFile ("reg_mw_cat.wav");
    CHECK (writeWav (wav, 1, 8000.0, 4 * 8000,
            [] (int, int64_t f, double rate) {
                return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                               * 300.0 * (double) f / rate);
            }), "reg mw: wrote cat wav");
    Application app;
    CHECK (app.openFile (wav), "reg mw: opened cat wav");
    std::unique_ptr<MainWindow> win (new MainWindow (app, "reg mw cat"));

    juce::AlertWindow* w = win->createContentAwareTimeScaleDialog ();
    CHECK (w != nullptr, "reg mw: cat dialog created");
    if (w == nullptr) { wav.deleteFile (); return; }
    juce::Component::SafePointer<juce::Component> panel (w->getCustomComponent (0));
    CHECK (dynamic_cast<CatTimeScalePanel*> (panel.getComponent ()) != nullptr,
           "reg mw: cat dialog hosts its panel");
    delete w;   // analysis typically still running
    CHECK (panel == nullptr, "reg mw: deleting the cat dialog deletes its panel");

    // The worker has been joined: editing the document right away is safe, and
    // the queued preview delivery (if any) must find the panel gone.
    std::string err;
    app.setSelection ({ 0.0, app.audioEngine ().duration () });
    CHECK (app.changeGain (-6.0f, &err), "reg mw: edit right after closing the cat dialog");
    // The worker was joined by the delete, so any delivery it posted is
    // already queued: run the queue to its end (not a fixed 50 ms pump that
    // could stop before it) - the delivery must find the panel gone.
    CHECK (regDrainMessages (), "reg mw: queued cat deliveries ran");

    win.reset ();
    wav.deleteFile ();
}

// mainwindow#W0 - a progress dialog stays modal until its worker deletes it:
// ESC never dismisses it, Cancel / window close only request the stop and
// show "Cancelling...", and live dialogs are counted as running jobs.
static void regMwProgressDialogStaysModal()
{
    progress ("regression mainwindow: progress dialog stays modal");
    const int before = sc::WorkProgressDialog::numActive ();

    {
        auto* dlg = new sc::WorkProgressDialog ("reg mw job", "Working ...", false, nullptr);
        CHECK (sc::WorkProgressDialog::numActive () == before + 1, "reg mw: a live progress dialog counts as a job");
        dlg->enterModalState (false);
        juce::Component& c = *dlg;
        c.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
        CHECK (dlg->isCurrentlyModal (false), "reg mw: ESC does not end a progress dialog's modal state");
        delete dlg;
    }

    {
        auto* dlg = new sc::WorkProgressDialog ("reg mw job", "Working ...", true, nullptr);
        dlg->enterModalState (false);
        juce::Component& c = *dlg;
        c.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));   // ESC = Cancel shortcut
        scPumpUntil ([dlg] { return dlg->cancelled.load (); });     // triggerClick is async
        CHECK (dlg->cancelled.load (), "reg mw: ESC / Cancel requests the worker to stop");
        CHECK (dlg->isCurrentlyModal (false), "reg mw: the dialog stays modal after Cancel");
        auto* b = dlg->getButton ("Cancel");
        CHECK (b != nullptr && !b->isEnabled (), "reg mw: Cancel is disabled once requested");
        dlg->setStatusText ("Step 2 ...");   // late progress text from the worker
        CHECK (dlg->getDescription ().endsWith ("Cancelling..."), "reg mw: the dialog shows Cancelling...");
        delete dlg;
    }

    {
        auto* dlg = new sc::WorkProgressDialog ("reg mw job", "Working ...", true, nullptr);
        dlg->enterModalState (false);
        dlg->userTriedToCloseWindow ();   // e.g. Alt+F4 on the dialog
        CHECK (dlg->cancelled.load () && dlg->isCurrentlyModal (false),
               "reg mw: closing the progress window cancels but keeps it modal");
        delete dlg;
    }

    CHECK (sc::WorkProgressDialog::numActive () == before, "reg mw: deleted dialogs are no longer counted");
}

// mainwindow#W3 + W0 - Ctrl+W asks before discarding unsaved changes and
// refuses while a background job runs.
static void regMwCloseAsksAndRespectsJobs()
{
    progress ("regression mainwindow: close asks and respects jobs");
    const juce::File wav = regMwWriteWav ("reg_mw_close.wav", 0.25f);
    Application app;
    std::unique_ptr<MainWindow> win (new MainWindow (app, "reg mw close"));
    const juce::KeyPress ctrlW ('w', juce::ModifierKeys::commandModifier, 0);
    auto isClosePrompt = [] (juce::Component& m) -> juce::AlertWindow*
    {
        auto* aw = dynamic_cast<juce::AlertWindow*> (&m);
        return (aw != nullptr && (aw->getName () == "Close File" || aw->getTitle () == "Close File"))
                   ? aw : nullptr;
    };

    // Clean document: closes straight away.
    CHECK (app.openFile (wav), "reg mw: opened close wav");
    win->keyPressed (ctrlW, nullptr);
    CHECK (app.numDocs () == 0, "reg mw: Ctrl+W closes a clean file without asking");

    // Unsaved document: Cancel keeps it.
    CHECK (app.openFile (wav), "reg mw: re-opened close wav");
    if (app.doc () != nullptr) app.doc ()->metaDirty = true;
    {
        bool asked = false;
        ScModalDriver driver ([&] (juce::Component& m)
        {
            if (auto* aw = isClosePrompt (m)) { asked = true; aw->exitModalState (0); return true; }
            return false;
        });
        win->keyPressed (ctrlW, nullptr);
        CHECK (asked, "reg mw: Ctrl+W on an unsaved file asks first");
        CHECK (app.numDocs () == 1, "reg mw: Cancel keeps the unsaved file open");
    }
    // ... and Close discards it.
    {
        bool asked = false;
        ScModalDriver driver ([&] (juce::Component& m)
        {
            if (auto* aw = isClosePrompt (m)) { asked = true; aw->exitModalState (1); return true; }
            return false;
        });
        win->menuItemSelected (MainWindow::MenuIDs::FileClose, 0);
        CHECK (asked && app.numDocs () == 0, "reg mw: File > Close closes after confirmation");
    }

    // A running job (live progress dialog) blocks closing.
    CHECK (app.openFile (wav), "reg mw: opened close wav again");
    auto* dlg = new sc::WorkProgressDialog ("reg mw job", "Working ...", false, nullptr);
    CHECK (win->isBackgroundJobRunning (), "reg mw: a progress dialog marks a job as running");
    win->keyPressed (ctrlW, nullptr);
    CHECK (app.numDocs () == 1, "reg mw: Ctrl+W does nothing while a job runs");
    delete dlg;
    CHECK (!win->isBackgroundJobRunning (), "reg mw: job over once its dialog is gone");
    win->keyPressed (ctrlW, nullptr);
    CHECK (app.numDocs () == 0, "reg mw: Ctrl+W works again after the job");

    // mainwindow#W4: the window uninstalls its Application callbacks.
    win.reset ();
    CHECK (!app.onStateChange && !app.onActiveDocChanged && !app.onTimeRangeDeleted
           && !app.onFileClosed && !app.onPlayheadChanged,
           "reg mw: destroying the window clears the callbacks that captured it");
    wav.deleteFile ();
}

// mainwindow#W1 - on a streamed file runDestructiveEdit runs the edit on a
// worker after the dialog function returned: the gain must be captured by
// value, not read from the dead stack frame.
static void regMwStreamedGainDialog()
{
    progress ("regression mainwindow: streamed gain dialog");
    const juce::File wav = regMwWriteWav ("reg_mw_stream_gain.wav", 0.25f, 4000);
    Application app;
    CHECK (app.openFile (wav), "reg mw: opened stream wav");
    AudioEngine& eng = app.audioEngine ();
    eng.setMaxInMemorySampleChannels (100);   // force streaming mode
    CHECK (eng.loadFile (wav) && !eng.fullyInMemory (), "reg mw: active document is streamed");
    app.setSelection ({ 0.0, eng.duration () });
    std::unique_ptr<MainWindow> win (new MainWindow (app, "reg mw stream gain"));

    bool done = false, applied = false;
    {
        ScModalDriver driver ([] (juce::Component& m)
        {
            if (auto* aw = dynamic_cast<juce::AlertWindow*> (&m))
                if (auto* ed = aw->getTextEditor ("gainDb"))
                {
                    ed->setText ("6.0206");
                    aw->exitModalState (1);   // OK
                    return true;
                }
            return false;
        });
        win->showChangeGainDialog ([&] (bool ok) { done = true; applied = ok; });
        scPumpUntil ([&done] { return done; }, 10000);   // the edit finishes on a worker
    }
    CHECK (done && applied, "reg mw: streamed gain edit completed");
    float v = 0.0f;
    CHECK (eng.readSamples (0, 1000, 1, &v) && std::abs (v - 0.5f) < 1e-3,
           "reg mw: the typed +6 dB was applied to the streamed file");

    win.reset ();
    wav.deleteFile ();
}

// mainwindow#W1 (action replay) - the replayed step is captured by value.
static void regMwStreamedReplayStep()
{
    progress ("regression mainwindow: streamed replay step");
    const juce::File wav = regMwWriteWav ("reg_mw_stream_replay.wav", 0.25f, 4000);
    Application app;
    CHECK (app.openFile (wav), "reg mw: opened replay wav");
    AudioEngine& eng = app.audioEngine ();
    eng.setMaxInMemorySampleChannels (100);
    CHECK (eng.loadFile (wav) && !eng.fullyInMemory (), "reg mw: replay document is streamed");
    app.setSelection ({ 0.0, eng.duration () });
    std::unique_ptr<MainWindow> win (new MainWindow (app, "reg mw stream replay"));

    sc::ActionStep step;
    step.type = sc::actionTypes::kChangeGain;
    step.params = sc::makeVarObject ({ { "db", 6.0206 } });
    {
        // Dismiss the "Run Action" finished / stopped message box.
        bool finished = false;
        ScModalDriver driver ([&] (juce::Component& m)
        {
            if (auto* aw = dynamic_cast<juce::AlertWindow*> (&m))
                if (aw->getName () == "Run Action" || aw->getTitle () == "Run Action")
                {
                    finished = true;
                    aw->exitModalState (0);
                    return true;
                }
            return false;
        });
        // Headless runs record the box instead of showing it (sc::alert).
        sc::alert::clearSuppressedMessages ();
        win->replaySteps_ = { step };
        win->replayPos_ = 0;
        win->replayName_ = "reg mw replay";
        win->continueReplay ();
        scPumpUntil ([&finished] { return finished || regAlertShown ("Run Action"); }, 10000);
        finished = finished || regAlertShown ("Run Action");
        CHECK (finished, "reg mw: replay of the streamed step finished");
    }
    float v = 0.0f;
    CHECK (eng.readSamples (0, 1000, 1, &v) && std::abs (v - 0.5f) < 1e-3,
           "reg mw: the replayed +6 dB step was applied to the streamed file");

    win.reset ();
    wav.deleteFile ();
}

// mainwindow#W5 - Save As renames the document (Files list, footer, prompts)
// and records the new path in Open Recent.
static void regMwSaveAsRenamesDoc()
{
    progress ("regression mainwindow: Save As renames the document");
    const juce::File wav = regMwWriteWav ("reg_mw_saveas_src.wav", 0.25f);
    const juce::File dst = makeTempFile ("reg_mw_saveas_dst.wav");
    dst.deleteFile ();
    Application app;
    CHECK (app.openFile (wav), "reg mw: opened save-as wav");
    std::unique_ptr<MainWindow> win (new MainWindow (app, "reg mw save as"));

    const int jobs = sc::WorkProgressDialog::numActive ();
    win->runSaveAsync (dst, true);
    CHECK (regMwWaitJobsBackTo (jobs), "reg mw: Save As completed within 10 s");

    FileDoc* d = app.doc ();
    CHECK (d != nullptr && d->file == dst, "reg mw: Save As points the document at the new file");
    CHECK (d != nullptr && d->name == dst.getFileName (), "reg mw: Save As renames the document");
    CHECK (win->statusBarFileName () == dst.getFileName (), "reg mw: the footer shows the new name");
    CHECK (!app.recentFiles.empty () && app.recentFiles[0] == dst, "reg mw: Save As adds the new path to Open Recent");

    win.reset ();
    wav.deleteFile ();
    dst.deleteFile ();
}

// mainwindow#W6 - opening an unreadable file says so instead of doing nothing;
// the drag filter matches whole extensions only.
static void regMwOpenFailureAndDragFilter()
{
    progress ("regression mainwindow: open failure + drag filter");
    const juce::File bad = makeTempFile ("reg_mw_bad.wav");
    bad.replaceWithText ("this is not a wav file");
    Application app;
    std::unique_ptr<MainWindow> win (new MainWindow (app, "reg mw open fail"));
    {
        bool shown = false;
        ScModalDriver driver ([&] (juce::Component& m)
        {
            if (auto* aw = dynamic_cast<juce::AlertWindow*> (&m))
            {
                shown = true;
                aw->exitModalState (0);
                return true;
            }
            return false;
        });
        sc::alert::clearSuppressedMessages ();
        win->openFile (bad);
        shown = shown || regAlertShown ("Open failed");   // headless: recorded, not shown
        CHECK (shown, "reg mw: an unreadable file reports an error");
        CHECK (app.numDocs () == 0, "reg mw: nothing is opened");
    }

    juce::StringArray noExt;   noExt.add ("/tmp/README");
    juce::StringArray partial; partial.add ("/tmp/take.wa");
    juce::StringArray upper;   upper.add ("/tmp/take.WAV");
    CHECK (!win->isInterestedInFileDrag (noExt), "reg mw: drag filter rejects files without an extension");
    CHECK (!win->isInterestedInFileDrag (partial), "reg mw: drag filter rejects partial extensions");
    CHECK (win->isInterestedInFileDrag (upper), "reg mw: drag filter still accepts .WAV");

    win.reset ();
    bad.deleteFile ();
}

// views#W10 - dragging the waveform's vertical scrollbar thumb pans the view.
static void regMwWaveScrollBarPans()
{
    progress ("regression mainwindow: waveform scrollbar pans");
    const juce::File wav = regMwWriteWav ("reg_mw_vscroll.wav", 0.25f);
    Application app;
    CHECK (app.openFile (wav), "reg mw: opened vscroll wav");
    std::unique_ptr<MainWindow> win (new MainWindow (app, "reg mw vscroll"));
    auto* wave = win->waveformViewComp ();
    auto* bar = dynamic_cast<ValueScrollBar*> (win->waveVScrollBarComp ());
    CHECK (wave != nullptr && bar != nullptr, "reg mw: waveform view and its scrollbar exist");
    if (wave != nullptr && bar != nullptr)
    {
        wave->setVerticalZoom (4.0);
        win->updateScrollBars ();
        const double h = wave->getVisibleAmpHalf ();
        bar->applyWindow (0.1 - h, 0.1 + h);   // pan the thumb up by 0.1, same span
        CHECK (std::abs (wave->getVerticalOffset () - 0.1) < 1e-6,
               "reg mw: the scrollbar pan moves the waveform's vertical offset");
        CHECK (std::abs (wave->getVisibleAmpHalf () - h) < 1e-6, "reg mw: a pure pan keeps the zoom");
    }

    // mainwindow#W7: the Transport menu's "Stop  Ctrl+." has a handler.
    CHECK (win->keyPressed (juce::KeyPress ('.', juce::ModifierKeys::commandModifier, 0), nullptr),
           "reg mw: Ctrl+. (Stop) is handled");
    win.reset ();
    wav.deleteFile ();
}


// integration (handoffs persistence#1, views#2, panels#2a): a metadata edit
// made while a background save runs keeps the document dirty; the spectral
// scrollbar follows the log/linear frequency setting; DEL in the Markers list
// deletes through MainWindow's destructive-edit route.
static void regMwIntegrationHandoffs()
{
    progress ("regression mainwindow (integration): save keeps later metadata edits dirty, log scrollbar, DEL route");
    const juce::File wav = regMwWriteWav ("reg_mw_integ_src.wav", 0.25f);
    const juce::File dst = makeTempFile ("reg_mw_integ_dst.wav");
    dst.deleteFile ();
    Application app;
    CHECK (app.openFile (wav), "reg mw integ: opened wav");
    std::unique_ptr<MainWindow> win (new MainWindow (app, "reg mw integ"));
    FileDoc* d = app.doc ();
    CHECK (d != nullptr, "reg mw integ: document open");
    if (d == nullptr) { win.reset (); wav.deleteFile (); return; }

    // Save, then "edit" the metadata before the completion callback has run
    // (it only runs when the message loop is pumped below).
    d->metaDirty = true;
    const int jobs = sc::WorkProgressDialog::numActive ();
    win->runSaveAsync (dst, true);
    d->metaDirty = true;   // a marker added while the worker writes the file
    CHECK (regMwWaitJobsBackTo (jobs), "reg mw integ: first save completed within 10 s");
    CHECK (dst.existsAsFile (), "reg mw integ: file saved");
    CHECK ((bool) d->metaDirty, "reg mw integ: an edit made during the save keeps the document dirty");

    // Nothing changed during this one: it ends clean.
    win->runSaveAsync (dst, false);
    CHECK (regMwWaitJobsBackTo (jobs), "reg mw integ: second save completed within 10 s");
    CHECK (!d->metaDirty, "reg mw integ: an undisturbed save clears the metadata flag");

    // Log frequency axis -> logarithmic scrollbar, without waiting for a zoom.
    auto* bar = dynamic_cast<ValueScrollBar*> (win->specVScrollBarComp ());
    CHECK (bar != nullptr, "reg mw integ: spectral scrollbar exists");
    if (bar != nullptr)
    {
        app.settings.logFrequency = true;
        if (app.onStateChange) app.onStateChange ();
        CHECK (bar->isLogarithmic (), "reg mw integ: log frequency -> logarithmic scrollbar");
        app.settings.logFrequency = false;
        if (app.onStateChange) app.onStateChange ();
        CHECK (!bar->isLogarithmic (), "reg mw integ: linear frequency -> linear scrollbar");
    }

    // DEL hooks of the Markers and Selection panels are wired.
    auto* markers = dynamic_cast<MarkerListPanel*> (win->panelForId ("markers"));
    auto* selPanel = dynamic_cast<SelectionPanel*> (win->panelForId ("selection"));
    CHECK (markers != nullptr && markers->onDeleteAudioRequested != nullptr,
           "reg mw integ: Markers DEL routed through MainWindow");
    CHECK (selPanel != nullptr && selPanel->onDeleteAudioRequested != nullptr,
           "reg mw integ: Selection DEL routed through MainWindow");
    if (markers != nullptr && markers->onDeleteAudioRequested != nullptr)
    {
        const int64_t before = app.audioEngine ().numSamples ();
        app.setSelection ({ 0.1, 0.2 });
        markers->onDeleteAudioRequested ();
        // The destructive-edit route may finish asynchronously: wait for the
        // expected length (a wrong result fails after the timeout).
        scPumpUntil ([&app, before] { return app.audioEngine ().numSamples () == before - 800; }, 5000);
        CHECK (app.audioEngine ().numSamples () == before - 800, "reg mw integ: DEL deleted the selected 0.1 s");
    }

    win.reset ();
    wav.deleteFile ();
    dst.deleteFile ();
}

static void runRegression_mainwindow()
{
    regMwFilterEqDialogOwnsComponents ();
    regMwLabeledFieldRowsAreDeleted ();
    regMwCatDialogTeardown ();
    regMwProgressDialogStaysModal ();
    regMwCloseAsksAndRespectsJobs ();
    regMwStreamedGainDialog ();
    regMwStreamedReplayStep ();
    regMwSaveAsRenamesDoc ();
    regMwOpenFailureAndDragFilter ();
    regMwWaveScrollBarPans ();
    regMwIntegrationHandoffs ();
}
