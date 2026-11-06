// AI control lot 6: typed commands for every step type, clipboard/doc/
// selection/settings/recent/meta/record/actions/tables commands, and the menu
// items that now run through the command registry ("menu == command").
// Included by tests/UnitTests.cpp after control.inc.

static std::vector<float> mcRead (const AudioEngine& eng, int ch)
{
    std::vector<float> v ((size_t) eng.numSamples());
    eng.readSamples (ch, 0, eng.numSamples(), v.data());
    return v;
}

static void testMenuEqualsCommand()
{
    struct Case { int menuId; const char* cmd; };
    const Case cases[] = {
        { MainWindow::MenuIDs::ToolsSilence,   "effect.silence" },
        { MainWindow::MenuIDs::ToolsFadeIn,    "effect.fadeIn" },
        { MainWindow::MenuIDs::ToolsFadeOut,   "effect.fadeOut" },
        { MainWindow::MenuIDs::ToolsNormalize, "effect.normalize" },
        { MainWindow::MenuIDs::ToolsReverse,   "effect.reverse" },
        { MainWindow::MenuIDs::ToolsInvert,    "effect.invert" },
        { MainWindow::MenuIDs::EditDelete,     "edit.delete" },
        { MainWindow::MenuIDs::EditTrimSelection, "edit.trim" },
    };
    for (const auto& c : cases)
    {
        // Menu path.
        const juce::File wav = ctlMakeWav (juce::String ("menu_eq_") + c.cmd + ".wav", 1, 8000.0, 16000);
        Application appMenu;
        CHECK (appMenu.openFile (wav), "menu==cmd: open (menu)");
        std::unique_ptr<MainWindow> win (new MainWindow (appMenu, "menu == command"));
        appMenu.setSelection ({ 0.25, 1.25 });
        win->menuItemSelected (c.menuId, 0);
        const auto menuData = mcRead (appMenu.audioEngine(), 0);
        CHECK (appMenu.undo().undoSize() == 1, juce::String ("menu==cmd: ") + c.cmd + " menu edit recorded");
        if (appMenu.undo().canUndo())
        {
            const Patch& p = appMenu.undo().lastRecorded();
            CHECK (p.opId == c.cmd && p.source == "menu",
                   juce::String ("menu==cmd: menu edit stamped ") + c.cmd + "/menu (got " + p.opId + "/" + p.source + ")");
        }
        const bool isEffect = juce::String (c.cmd).startsWith ("effect.");
        if (isEffect)
            CHECK (win->getLastEffectId() == c.menuId, juce::String ("menu==cmd: repeat-last-effect id kept for ") + c.cmd);
        win.reset();

        // API path, same range.
        Application appApi;
        CHECK (appApi.openFile (wav), "menu==cmd: open (api)");
        const auto r = ctlCall (appApi, c.cmd, scctl::obj ({ { "select", scctl::obj ({ { "start", 0.25 }, { "end", 1.25 } }) } }));
        CHECK (ctlOk (r), juce::String ("menu==cmd: ") + c.cmd + " via the API");
        CHECK (mcRead (appApi.audioEngine(), 0) == menuData, juce::String ("menu==cmd: identical samples for ") + c.cmd);
    }

    // Clipboard edits through the menu use the registry too.
    {
        const juce::File wav = ctlMakeWav ("menu_eq_clip.wav", 1, 8000.0, 16000);
        Application app;
        app.openFile (wav);
        std::unique_ptr<MainWindow> win (new MainWindow (app, "menu clipboard"));
        app.setSelection ({ 0.5, 1.0 });
        win->menuItemSelected (MainWindow::MenuIDs::EditCut, 0);
        CHECK (app.canPaste() && std::abs (app.audioEngine().duration() - 1.5) < 1e-6, "menu cut: 0.5 s removed, clipboard filled");
        CHECK (app.undo().lastRecorded().opId == "edit.cut", "menu cut: patch credited to edit.cut");
        app.setSelection ({ 0.0, 0.0 });
        app.setPlayhead (0.0);
        win->menuItemSelected (MainWindow::MenuIDs::EditPaste, 0);
        CHECK (std::abs (app.audioEngine().duration() - 2.0) < 1e-6, "menu paste: length restored");
        win->menuItemSelected (MainWindow::MenuIDs::EditUndo, 0);
        CHECK (std::abs (app.audioEngine().duration() - 1.5) < 1e-6, "menu undo through history.undo");
        win->menuItemSelected (MainWindow::MenuIDs::EditSelectAll, 0);
        CHECK (std::abs (app.selection.getLength() - 1.5) < 1e-6, "menu select all through selection.all");
    }
}

static void testLot6Commands()
{
    auto& reg = scctl::CommandRegistry::instance();
    // Every step type has a typed command whose alias is the old type name.
    for (const char* t : { "silence", "fadeIn", "fadeOut", "normalize", "reverse", "invert", "changeGain",
                           "timeStretch", "catTimeScale", "filterEq", "envelope", "resample", "genSilence",
                           "genNoise", "genTone", "delete", "trim", "noiseSuppress", "transcribe" })
    {
        const auto* s = reg.find (t);
        CHECK (s != nullptr && s->id.containsChar ('.') && s->id != "step.run",
               juce::String ("lot 6: typed command for step type ") + t);
    }

    Application app;
    const juce::File wav = ctlMakeWav ("lot6.wav", 2, 8000.0, 16000);
    ctlCall (app, "doc.open", scctl::obj ({ { "path", wav.getFullPathName() } }));
    const auto before = mcRead (app.audioEngine(), 0);

    // Typed == generic step.
    auto a = ctlCall (app, "effect.changeGain", scctl::obj ({ { "db", -6.0 }, { "select", "all" } }));
    const auto typed = mcRead (app.audioEngine(), 0);
    ctlCall (app, "history.undo");
    auto b = ctlCall (app, "step.run", scctl::obj ({ { "select", "all" }, { "step", scctl::obj ({ { "type", "changeGain" },
                                                                                         { "params", scctl::obj ({ { "db", -6.0 } }) } }) } }));
    CHECK (ctlOk (a) && ctlOk (b) && mcRead (app.audioEngine(), 0) == typed && typed != before,
           "lot 6: effect.changeGain == step.run changeGain");
    CHECK (ctlCode (ctlCall (app, "effect.changeGain", scctl::obj ({ { "db", 500 } }))) == "invalid_params",
           "lot 6: typed params validated (db range)");
    CHECK (ctlOk (ctlCall (app, "changeGain", scctl::obj ({ { "db", 1.0 } })))
               && app.undo().lastRecorded().opId == "effect.changeGain", "lot 6: alias = old type name");
    CHECK (ctlOk (ctlCall (app, "gen.tone", scctl::obj ({ { "tones", scctl::arr ({ scctl::obj ({ { "f", 440 }, { "a", 0.2 } }) }) },
                                                          { "dur", 0.1 }, { "select", scctl::obj ({ { "start", 0.1 }, { "end", 0.2 } }) } }))),
           "lot 6: gen.tone");

    // Clipboard.
    CHECK (ctlOk (ctlCall (app, "edit.copy", scctl::obj ({ { "select", scctl::obj ({ { "start", 0.0 }, { "end", 0.5 } }) } })))
               && app.canPaste(), "edit.copy fills the clipboard");
    const int docsBefore = app.numDocs();
    const auto nf = ctlCall (app, "edit.copyToNewFile");
    CHECK (ctlOk (nf) && app.numDocs() == docsBefore + 1, "edit.copyToNewFile opens a new document");

    // Documents / selection tools.
    const auto nd = ctlCall (app, "doc.new", scctl::obj ({ { "channels", 1 }, { "sampleRate", 16000 }, { "durationSec", 1.0 } }));
    CHECK (ctlOk (nd) && (int) ctlResult (nd).getProperty ("channels", 0) == 1
               && std::abs ((double) ctlResult (nd).getProperty ("durationSec", 0) - 1.0) < 1e-3, "doc.new with duration");
    CHECK (ctlOk (ctlCall (app, "selection.tool", scctl::obj ({ { "tool", "marquee" } })))
               && app.selMode == SpectralSelMode::Marquee, "selection.tool");
    ctlCall (app, "selection.tool", scctl::obj ({ { "tool", "timeRange" } }));
    CHECK (ctlOk (ctlCall (app, "selection.brush", scctl::obj ({ { "radiusHz", 300 } }))) && app.brushRadiusHz == 300.0, "selection.brush");

    // Settings: clamps, unknown key, control_* refused.
    auto ss = ctlCall (app, "settings.set", scctl::obj ({ { "values", scctl::obj ({ { "fft_size", 99999 }, { "snap_zero", true } }) } }));
    CHECK (ctlOk (ss) && app.settings.fftSize == 65536 && app.settings.snapToZeroCrossings
               && scctl::toLine (ctlResult (ss)).contains ("fft_size"), "settings.set clamps fft_size and applies snap_zero");
    CHECK (ctlCode (ctlCall (app, "settings.set", scctl::obj ({ { "values", scctl::obj ({ { "control_enabled", true } }) } })))
               == "invalid_params" && !app.settings.controlEnabled, "settings.set refuses control_* keys");
    CHECK (ctlCode (ctlCall (app, "settings.set", scctl::obj ({ { "values", scctl::obj ({ { "nope", 1 } }) } }))) == "invalid_params",
           "settings.set refuses unknown keys");
    const auto sg = ctlResult (ctlCall (app, "settings.get", scctl::obj ({ { "keys", scctl::arr ({ "fft_size", "snap_zero" }) } })));
    CHECK ((int) sg.getProperty ("values", {}).getProperty ("fft_size", 0) == 65536, "settings.get reads back");

    // Recent files, metadata, actions, tables, record.
    CHECK (ctlResult (ctlCall (app, "recent.list")).getProperty ("recent", {}).size() >= 1, "recent.list");
    ctlCall (app, "doc.activate", {}, (int) ctlResult (ctlCall (app, "doc.list")).getProperty ("docs", {})[0].getProperty ("id", -1));
    CHECK (ctlOk (ctlCall (app, "meta.setMode", scctl::obj ({ { "mode", "external" } })))
               && app.doc()->metaMode == sc::MetaMode::External, "meta.setMode external");
    CHECK (ctlOk (ctlCall (app, "meta.save")), "meta.save");
    const int ai = app.actions.add ("API action");
    app.actions.get (ai)->steps.push_back (sc::ActionStep { "reverse", {}, false });
    app.actions.get (ai)->steps.push_back (sc::ActionStep { "invert", {}, false });
    const size_t u0 = app.undo().undoSize();
    CHECK (ctlOk (ctlCall (app, "actions.run", scctl::obj ({ { "name", "API action" }, { "select", "all" } })))
               && app.undo().undoSize() == u0 + 2, "actions.run: one history entry per step");
    CHECK (ctlResult (ctlCall (app, "actions.list")).getProperty ("actions", {}).size() == 1, "actions.list");
    CHECK (ctlOk (ctlCall (app, "tables.list")), "tables.list");
    CHECK (ctlCode (ctlCall (app, "record.stop")) == "failed", "record.stop when not recording");
    CHECK (ctlOk (ctlCall (app, "record.setMode", scctl::obj ({ { "mode", "loop" }, { "seconds", 5 } }))), "record.setMode");
}

static void runRegression_menuCommands()
{
    testMenuEqualsCommand();
    testLot6Commands();
}
