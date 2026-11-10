// Regression tests for application bugs revealed by the phase-7 test analysis
// (agent T4: src/ and include/ fixes). Included by tests/UnitTests.cpp; uses
// its helpers (CHECK, scSkip, progress, writeWav, makeTempFile, ScModalDriver,
// scHasForegroundFocus, ...).

#include "OwningAlertWindow.h"

#if JUCE_WINDOWS
 #include <stdlib.h>   // _putenv_s
#else
 #include <cstdlib>    // setenv / unsetenv
#endif

/** AudioEngine::readSamples() at / past the end of the file: an empty read is
    valid (the playhead reaches the end, or sits past it after a delete). It
    used to call AudioBuffer::getReadPointer (ch, totalSamples) for count 0,
    which asserts (juce_AudioSampleBuffer.h, sampleIndex < size). Both the
    in-memory and the streamed paths must return true and write nothing. */
static void testAppRegReadSamplesAtEnd()
{
    progress ("regression app-from-tests: readSamples at / past the end is an empty read");
    const juce::File wav = makeTempFile ("reg_t4_read_end.wav");
    const int64_t frames = 1000;
    writeWav (wav, 2, 8000.0, frames, [] (int ch, int64_t f, double)
             { return (float) (ch + 1) * 0.001f * (float) (f % 100); });

    for (int streamed = 0; streamed < 2; ++streamed)
    {
        const juce::String tag = streamed ? "streamed" : "in-memory";
        AudioEngine eng;
        if (streamed) eng.setMaxInMemorySampleChannels (0);   // 0 = always stream
        CHECK (eng.loadFile (wav), "read-at-end: load " + tag);
        CHECK (eng.fullyInMemory () == (streamed == 0), "read-at-end: " + tag + " engine as requested");

        float dest[16];
        std::fill (std::begin (dest), std::end (dest), 123.0f);
        CHECK (eng.readSamples (1, frames, 0, dest), "read-at-end: " + tag + " count 0 at the end succeeds");
        CHECK (eng.readSamples (1, frames, 16, dest), "read-at-end: " + tag + " read starting at the end succeeds (empty)");
        CHECK (eng.readSamples (0, frames + 500, 16, dest), "read-at-end: " + tag + " read past the end succeeds (empty)");
        bool untouched = true;
        for (float v : dest) untouched = untouched && v == 123.0f;
        CHECK (untouched, "read-at-end: " + tag + " empty reads write nothing");

        // The last real sample is still read correctly right before the end.
        float last = 0.0f;
        CHECK (eng.readSamples (1, frames - 1, 1, &last), "read-at-end: " + tag + " last sample read");
        CHECK (std::abs (last - 2.0f * 0.001f * (float) ((frames - 1) % 100)) < 1e-6f,
               "read-at-end: " + tag + " last sample value");
    }
    wav.deleteFile ();
}

/** Transport > Recording Device: the "(no devices)" / "(no recorder plugins
    found)" placeholders used ID 0, which PopupMenu reserves for "nothing
    picked" (assertion juce_PopupMenu.cpp). They are now disabled items with a
    dedicated non-zero ID. And the device IDs (RecDeviceBase + plugin*1000 +
    device) started at 4500 with a routed range ending at 5000: a second
    plugin's devices were ignored and a third plugin's first device (6500)
    was WindowPanelBase. The range now sits above every fixed menu ID. */
static void testAppRegRecordingMenuIds()
{
    progress ("regression app-from-tests: recording menu placeholder + device ID range");
    Application app;

    // A plugin index that does not exist always yields the placeholder.
    const juce::PopupMenu m = MainWindow::buildRecordingDeviceItems (app, -1);
    CHECK (m.getNumItems () == 1, "rec ids: one placeholder item for an unknown plugin");
    for (juce::PopupMenu::MenuItemIterator it (m); it.next ();)
    {
        const auto& item = it.getItem ();
        CHECK (item.itemID != 0, "rec ids: placeholder does not use the reserved ID 0");
        CHECK (item.itemID == MainWindow::RecNoDeviceItem, "rec ids: placeholder uses RecNoDeviceItem");
        CHECK (!item.isEnabled, "rec ids: placeholder is disabled");
    }

    // Every encodable device ID lies in the routed range and above all fixed IDs.
    const int fixedIds[] = { MainWindow::FileNew, MainWindow::TransportRecord, MainWindow::RecOptTimed,
                             MainWindow::RecNoDeviceItem, MainWindow::ToolsSilence, MainWindow::HelpAbout,
                             MainWindow::WindowPanelBase + 15, MainWindow::WindowAllowMultiple,
                             MainWindow::ActionsBatch, MainWindow::MetaSaveOnly, MainWindow::ChannelBase + 999,
                             MainWindow::RecentBase + 99, MainWindow::ClearRecent };
    int maxFixed = 0;
    for (int v : fixedIds) maxFixed = std::max (maxFixed, v);
    bool inRange = true, aboveFixed = true;
    for (int pi : { 0, 1, 2, 7, 999 })
        for (int dj : { 0, 1, 999 })
        {
            const int id = MainWindow::RecDeviceBase + pi * 1000 + dj;
            inRange    = inRange && id >= MainWindow::RecDeviceBase && id < MainWindow::RecDeviceEnd;
            aboveFixed = aboveFixed && id > maxFixed;
        }
    CHECK (inRange, "rec ids: device IDs of plugins 0..999 stay in [RecDeviceBase, RecDeviceEnd)");
    CHECK (aboveFixed, "rec ids: device IDs never collide with a fixed menu ID (e.g. WindowPanelBase)");
}

/** Recorder plugins are native shared libraries: the scan used to look for
    *.dll on every platform, so a .dylib / .so plugin was never found on
    macOS / Linux. */
static void testAppRegRecorderPluginPattern()
{
    progress ("regression app-from-tests: recorder plugin file pattern per platform");
    const juce::String pat = sc::rec::RecorderHost::pluginFilePattern ();
#if JUCE_WINDOWS
    CHECK (pat == "*.dll", "plugin pattern: *.dll on Windows");
#elif JUCE_MAC
    CHECK (pat == "*.dylib", "plugin pattern: *.dylib on macOS");
#else
    CHECK (pat == "*.so", "plugin pattern: *.so on Linux");
#endif
}

/** FileListPanel "Set Unit and Scale": createScaleDialog() grabbed the focus
    on a field of a window that was not showing yet (assertion
    juce_Component.cpp, and the grab was lost), and the field is disabled
    while the unit is None. showScaleDialog() now focuses once the window is
    modal and showing: the unit combo for an uncalibrated file, the first
    full-scale field (text pre-selected) otherwise. */
static void testAppRegScaleDialogFocus()
{
    progress ("regression app-from-tests: scale dialog focus after the window shows");
    if (!scHasForegroundFocus ())
    {
        scSkip ("scale dialog focus: keyboard focus needs the OS foreground");
        return;
    }

    const juce::File wav = makeTempFile ("reg_t4_scale_focus.wav");
    writeWav (wav, 1, 8000.0, 800, [] (int, int64_t, double) { return 0.25f; });
    Application app;
    CHECK (app.openFile (wav), "scale focus: open wav");
    FileListPanel panel (app);

    for (int calibrated = 0; calibrated < 2; ++calibrated)
    {
        if (calibrated)
        {
            app.docs[0]->scale.unit = ScaleCalibration::Unit::Volts;
            app.docs[0]->scale.fullScaleValue = 2.0;
        }
        bool comboFocused = false, fieldFocused = false, fieldSelected = false;
        ScModalDriver driver ([&] (juce::Component& modal)
        {
            auto* aw = dynamic_cast<juce::AlertWindow*> (&modal);
            if (aw == nullptr || aw->getTitle () != "Set Unit and Scale") return false;
            auto* combo = aw->getComboBoxComponent ("unit");
            auto* ed = aw->getTextEditor ("fs0");
            auto* focused = juce::Component::getCurrentlyFocusedComponent ();
            comboFocused = combo != nullptr && focused != nullptr
                           && (focused == combo || combo->isParentOf (focused));
            fieldFocused = ed != nullptr && focused == ed;
            fieldSelected = ed != nullptr && ed->getHighlightedRegion ().getLength () == ed->getTotalNumChars ()
                            && ed->getTotalNumChars () > 0;
            aw->exitModalState (2);   // Cancel
            return true;
        }, 3000);
        panel.showScaleDialog (0);

        CHECK (driver.acted (), "scale focus: dialog was shown and driven");
        if (calibrated)
        {
            CHECK (fieldFocused, "scale focus: calibrated file -> first full-scale field has focus");
            CHECK (fieldSelected, "scale focus: the field's text is pre-selected");
        }
        else
            CHECK (comboFocused, "scale focus: unit None -> the unit combo has focus (fields disabled)");
    }
    CHECK (app.docs[0]->scale.unit == ScaleCalibration::Unit::Volts
           && app.docs[0]->scale.fullScaleValue == 2.0, "scale focus: Cancel leaves the calibration alone");
    wav.deleteFile ();
}

/** Stem Separation window, advanced mode: rowsHost_ held DescRows created with
    new and each DescRow held its anchor Label/Remove button as raw children -
    a JUCE Component deletes neither, so every close leaked them (the
    LeakedObjectDetector reports at exit: TextEditor, Label, TextButton...). */
static void testAppRegStemSepRowsDeleted()
{
    progress ("regression app-from-tests: stem-sep rows and anchor widgets deleted on close");
    const juce::File wav = makeTempFile ("reg_t4_stem_rows.wav");
    writeWav (wav, 1, 8000.0, 32000, [] (int, int64_t f, double r)
             { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 300.0 * (double) f / r); });
    Application app;
    CHECK (app.openFile (wav), "stem rows: open wav");
    std::unique_ptr<MainWindow> win (new MainWindow (app, "reg t4 stem rows"));

    auto findButton = [] (juce::Component* w, const juce::String& text) -> juce::TextButton*
    {
        for (int i = 0; i < w->getNumChildComponents (); ++i)
            if (auto* b = dynamic_cast<juce::TextButton*> (w->getChildComponent (i)))
                if (b->getButtonText () == text) return b;
        return nullptr;
    };

    win->menuItemSelected (MainWindow::MenuIDs::ToolsStemSeparate, 5);
    StemSeparationWindow* dlg = win->stemSepWindow ();
    CHECK (dlg != nullptr, "stem rows: window opened");
    if (dlg != nullptr)
    {
        juce::Component::SafePointer<juce::Component> row, anchorLabel;
        if (auto* adv = findButton (dlg, "Advanced..."))
        {
            adv->onClick ();
            auto* host = dlg->rowsHostForTest ();
            if (host != nullptr && host->getNumChildComponents () > 0)
            {
                app.setSelection ({ 1.0, 2.0 });
                if (auto* inc = dynamic_cast<juce::TextButton*> (host->getChildComponent (0)->getChildComponent (2)))
                    inc->onClick ();   // adds an include anchor -> rows rebuilt with an anchor line
                if (host->getNumChildComponents () > 0)
                {
                    row = host->getChildComponent (0);
                    for (int i = 0; i < row->getNumChildComponents (); ++i)
                        if (auto* l = dynamic_cast<juce::Label*> (row->getChildComponent (i)))
                            if (l->getText ().startsWith ("Included:")) anchorLabel = l;
                }
            }
        }
        CHECK (row != nullptr, "stem rows: an advanced-mode row exists");
        CHECK (anchorLabel != nullptr, "stem rows: the row shows its include anchor");

        dlg->userTriedToCloseWindow ();
        CHECK (win->stemSepWindow () == nullptr, "stem rows: window closed");
        CHECK (row == nullptr, "stem rows: closing the window deleted the row");
        CHECK (anchorLabel == nullptr, "stem rows: closing the window deleted the anchor widgets");
    }
    win.reset ();
    wav.deleteFile ();
}

/** sc::OwningAlertWindow deletes the components handed to
    addCustomComponent() (plain juce::AlertWindow does not): used by the
    Stem Separation / Super Resolution download dialogs and the drawing
    colour dialogs, which all add new-ed components. */
static void testAppRegOwningAlertWindow()
{
    progress ("regression app-from-tests: OwningAlertWindow deletes its custom components");
    juce::Component::SafePointer<juce::Component> lbl;
    {
        sc::OwningAlertWindow w ("t", "m", juce::AlertWindow::NoIcon);
        auto* l = new juce::Label ();
        l->setSize (100, 20);
        w.addCustomComponent (l);
        lbl = l;
        CHECK (lbl != nullptr, "owning alert: custom component alive while the dialog is");
    }
    CHECK (lbl == nullptr, "owning alert: custom component deleted with the dialog");
}

// Sets (or clears, value == nullptr) an environment variable of this process.
static void appRegSetEnv (const char* name, const char* value)
{
#if JUCE_WINDOWS
    _putenv_s (name, value != nullptr ? value : "");
#else
    if (value != nullptr) setenv (name, value, 1);
    else                  unsetenv (name);
#endif
}

/** SC_NO_AUDIO_DEVICE=1 keeps the real output device closed (headless test
    runs: silent, no fight over the hardware between parallel processes). It
    was defined for the test build but never read: every MainWindow's first
    timer tick opened the default device. With the variable set the lazy init
    completes immediately without a device, and play() does not retry. */
static void testAppRegNoAudioDeviceSwitch()
{
    progress ("regression app-from-tests: SC_NO_AUDIO_DEVICE keeps the output device closed");
    const juce::String old = juce::SystemStats::getEnvironmentVariable ("SC_NO_AUDIO_DEVICE", "\x01");
    const bool hadOld = old != "\x01";

    appRegSetEnv ("SC_NO_AUDIO_DEVICE", "1");
    CHECK (Application::audioOutputDisabled (), "no-audio: SC_NO_AUDIO_DEVICE=1 disables the output");
    {
        Application app;
        CHECK (!app.audioDeviceReady (), "no-audio: nothing initialised at construction");
        app.startAudioInitAsync ();
        CHECK (app.audioDeviceReady (), "no-audio: init completes at once");
        CHECK (app.ensureAudioReady (0), "no-audio: ensureAudioReady does not wait");
        CHECK (app.audioDevice->getCurrentAudioDevice () == nullptr, "no-audio: no device was opened");

        const juce::File wav = makeTempFile ("reg_t4_noaudio.wav");
        writeWav (wav, 1, 8000.0, 800, [] (int, int64_t, double) { return 0.1f; });
        CHECK (app.openFile (wav), "no-audio: open wav");
        app.play ();   // must not re-open the device (self-heal path)
        CHECK (app.audioDevice->getCurrentAudioDevice () == nullptr, "no-audio: play() does not open a device");
        app.stop ();
        wav.deleteFile ();
    }

#if ! defined (SC_NO_AUDIO_DEVICE)
    appRegSetEnv ("SC_NO_AUDIO_DEVICE", "0");
    CHECK (!Application::audioOutputDisabled (), "no-audio: SC_NO_AUDIO_DEVICE=0 keeps the output enabled");
#endif

    if (hadOld) appRegSetEnv ("SC_NO_AUDIO_DEVICE", old.toRawUTF8 ());
    else        appRegSetEnv ("SC_NO_AUDIO_DEVICE", nullptr);
}

static void runRegression_app_from_tests()
{
    testAppRegReadSamplesAtEnd ();
    testAppRegRecordingMenuIds ();
    testAppRegRecorderPluginPattern ();
    testAppRegScaleDialogFocus ();
    testAppRegStemSepRowsDeleted ();
    testAppRegOwningAlertWindow ();
    testAppRegNoAudioDeviceSwitch ();
}
