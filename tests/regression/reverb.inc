// Reverberation analysis: Effects > Reverberation Analysis... (sc::room in
// include/RoomAcoustics.h, the window in include/ReverbWindow.h). The maths has
// its own tests on synthetic rooms in mobile/tests/RoomAcousticsTests.cpp; here
// the window on a file: the menu, a recorded sweep, an impulse, what cannot be
// analysed, the table and the CSV.

static void testReverbWindow()
{
    const double rate = 48000.0, t60 = 0.6;
    sc::room::SweepSpec spec;
    spec.seconds = 3.0;
    spec.silenceSeconds = 3.0;

    // a room that decays by 60 dB in 0.6 s, and a 3 s sweep heard in it, 0.5 s into the file
    std::vector<float> room ((size_t) (1.5 * rate));
    juce::Random rng (2026);
    for (size_t i = 0; i < room.size(); ++i)
        room[i] = (float) (0.04 * (rng.nextDouble() * 2.0 - 1.0) * std::exp (-6.9078 * (double) i / rate / t60));
    room[0] = 0.5f;
    const auto sweep = sc::room::makeSweep (spec, rate);
    int order = 1;
    while (((size_t) 1 << order) < sweep.size() + room.size()) ++order;
    const size_t m = (size_t) 1 << order;
    std::vector<float> a (2 * m, 0.0f), b (2 * m, 0.0f);
    std::copy (sweep.begin(), sweep.end(), a.begin());
    std::copy (room.begin(), room.end(), b.begin());
    {
        juce::dsp::FFT fft (order);
        fft.performRealOnlyForwardTransform (a.data(), false);
        fft.performRealOnlyForwardTransform (b.data(), false);
        for (size_t k = 0; k < m; ++k)
        {
            const float re = a[2 * k] * b[2 * k] - a[2 * k + 1] * b[2 * k + 1], im = a[2 * k] * b[2 * k + 1] + a[2 * k + 1] * b[2 * k];
            a[2 * k] = re;
            a[2 * k + 1] = im;
        }
        fft.performRealOnlyInverseTransform (a.data());
    }
    const int64_t lead = (int64_t) (0.5 * rate), frames = (int64_t) (6.0 * rate);
    const juce::File wav = makeTempFile ("reverb_sweep.wav");
    writeWav (wav, 1, rate, frames, [&a, lead] (int, int64_t i, double) { return i >= lead ? a[(size_t) (i - lead)] : 0.0f; });
    const juce::File bang = makeTempFile ("reverb_impulse.wav");
    writeWav (bang, 1, rate, (int64_t) room.size() + lead,
              [&room, lead] (int, int64_t i, double) { return i >= lead ? room[(size_t) (i - lead)] : 0.0f; });
    const juce::File store = makeTempFile ("reverb.recent");
    store.deleteFile ();
    {
        Application app (store);
        std::unique_ptr<MainWindow> win (new MainWindow (app, "reverb"));
        auto enabledInEffects = [&win]
        {
            const int idx = win->getMenuBarNames ().indexOf ("Effects");
            const auto menu = win->getMenuForIndex (idx, "Effects");
            for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next ();)
                if (it.getItem ().itemID == MainWindow::ToolsReverberation)
                    return it.getItem ().isEnabled ? 1 : 0;
            return -1;
        };
        CHECK (enabledInEffects () == 0, "reverb: Effects > Reverberation Analysis is off without a file");
        CHECK (app.openFile (wav), "reverb: open the recorded sweep");
        CHECK (enabledInEffects () == 1, "reverb: and on with one");

        win->showReverbWindow ();
        auto* w = win->reverbWindow ();
        CHECK (w != nullptr, "reverb: the window opens");
        if (w == nullptr) { win.reset (); return; }
        auto& v = w->view ();
        CHECK (v.rangeText ().contains ("Whole file"), "reverb: the range is the whole file without a selection: " + v.rangeText ());

        // the sweep is not the one in the file: said, and nothing shown
        auto wrong = spec;
        wrong.seconds = 20.0;
        v.setSweep (wrong);
        CHECK (! v.analyse () && v.results ().empty () && v.statusText ().contains ("shorter"),
               "reverb: a sweep longer than the recording is refused: " + v.statusText ());

        v.setSweep (spec);
        CHECK (v.analyse (), "reverb: the recorded sweep is analysed: " + v.statusText ());
        CHECK (v.results ().size () >= 8, "reverb: the whole response and the octaves (" + juce::String ((int) v.results ().size ()) + ")");
        CHECK (std::abs ((double) v.response ().startInRecording + v.response ().peak / rate - 0.5) < 0.002,
               "reverb: the sweep is found where it starts, 0.5 s into the file");
        int row1k = -1;
        for (size_t i = 0; i < v.results ().size (); ++i)
            if (std::abs (v.results ()[i].nominalHz - 1000.0) < 1e-6) row1k = (int) i;
        CHECK (row1k > 0, "reverb: a 1 kHz octave");
        if (row1k > 0)
        {
            const auto& k1 = v.results ()[(size_t) row1k];
            CHECK (k1.t20.reliable && std::abs (k1.t20.seconds - t60) < 0.06,
                   "reverb: T20 at 1 kHz is the room's 0.6 s (" + juce::String (k1.t20.seconds, 3) + ")");
            CHECK (v.cellText (row1k, 1) == "1 kHz" && std::abs (v.cellText (row1k, 3).getDoubleValue () - t60) < 0.06
                       && ! v.cellText (row1k, 3).contains ("?"),
                   "reverb: the table shows it: " + v.cellText (row1k, 1) + " / " + v.cellText (row1k, 3));
        }
        CHECK (v.cellText (0, 1) == "All", "reverb: the first row is the whole response");
        // SC_REVERB_SNAPSHOT=<png>: the window as it is now, to look at
        if (const auto png = juce::SystemStats::getEnvironmentVariable ("SC_REVERB_SNAPSHOT", {}); png.isNotEmpty ())
        {
            juce::FileOutputStream out { juce::File (png) };
            juce::File (png).deleteFile ();
            juce::FileOutputStream fresh { juce::File (png) };
            juce::PNGImageFormat ().writeImageToStream (v.createComponentSnapshot (v.getLocalBounds (), true, 2.0f), fresh);
        }
        const auto csv = v.csv ();
        CHECK (csv.startsWith ("band_hz,EDT_s,T20_s,T30_s,C50_dB") && juce::StringArray::fromLines (csv.trim ()).size () == (int) v.results ().size () + 1,
               "reverb: the CSV has a header and one row per band");

        v.setThirdOctaves (true);
        CHECK (v.analyse () && v.results ().size () > 20, "reverb: 1/3 octaves (" + juce::String ((int) v.results ().size ()) + " rows)");
        v.setThirdOctaves (false);

        // only the selection is analysed: one without the sweep's end says so
        app.selection = { 0.0, 3.2 };
        CHECK (! v.analyse () && v.rangeText ().contains ("Selection"), "reverb: a selection that cuts the sweep is refused: " + v.statusText ());
        app.selection = { 0.0, 0.0 };

        // a sound map's reverberation take says which sweep it holds
        if (auto* d = app.doc ())
        {
            sc::room::SweepSpec told;
            told.seconds = 4.5;
            auto* o = new juce::DynamicObject ();
            o->setProperty ("sweep", told.toVar ());
            d->soundMapPoint = juce::var (o);
            v.refresh ();
            CHECK (std::abs (v.sweep ().seconds - 4.5) < 1e-9 && v.rangeText ().contains ("this take says"),
                   "reverb: the sweep comes from the take's metadata: " + v.rangeText ());
            d->soundMapPoint = juce::var ();
        }

        // an impulse (balloon, clapper) is a response as it is
        CHECK (app.openFile (bang), "reverb: open the impulse");
        v.setMethod (ReverbView::Method::Impulse);
        CHECK (v.analyse (), "reverb: the impulse is analysed: " + v.statusText ());
        for (const auto& band : v.results ())
            if (std::abs (band.nominalHz - 1000.0) < 1e-6)
                CHECK (band.t20.reliable && std::abs (band.t20.seconds - t60) < 0.06,
                       "reverb: the same room from the impulse (" + juce::String (band.t20.seconds, 3) + ")");
        win.reset ();
    }
    wav.deleteFile ();
    bang.deleteFile ();
    store.deleteFile ();
}
