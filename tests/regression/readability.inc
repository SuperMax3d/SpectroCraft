// Spectral Controls > Readability (issue #69): SpectralEngine::balanceLevels
// on synthetic dB pictures, the panel's toggle and sliders, persistence, and
// the settings.* control keys. Display only: the audio is never touched.

static void testSpectrogramReadability()
{
    Settings s;
    s.dbMinDb = -120.0f;
    s.dbMaxDb = 0.0f;
    const int w = 100, h = 48;
    auto at = [w] (std::vector<float>& d, int x, int y) -> float& { return d[(size_t) y * w + x]; };
    auto rowAvg = [w] (const std::vector<float>& d, int y)
    {
        double sum = 0.0;
        for (int x = 0; x < w; ++x) sum += d[(size_t) y * w + x];
        return sum / w;
    };

    // A tilted spectrum: the top row (treble) at -67 dB, the bottom (bass) at -20.
    std::vector<float> tilt ((size_t) w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            at (tilt, x, y) = -67.0f + (float) y;
    at (tilt, 50, 10) += 30.0f;   // a peak 30 dB above its band

    {
        auto d = tilt;
        s.readability = false;
        SpectralEngine::balanceLevels (d, w, h, s);
        CHECK (d == tilt, "readability #69: off changes nothing");
    }
    s.readability = true;
    s.freqBalance = 1.0f;
    s.timeBalance = 0.0f;
    {
        auto d = tilt;
        SpectralEngine::balanceLevels (d, w, h, s);
        double lo = 1e9, hi = -1e9;
        for (int y = 3; y < h - 3; ++y)   // away from the smoothing's edges
        {
            lo = std::min (lo, rowAvg (d, y));
            hi = std::max (hi, rowAvg (d, y));
        }
        CHECK (hi - lo < 1.0, "readability #69: full frequency balance evens the bands ("
                                  + juce::String (hi - lo, 2) + " dB apart, was 47)");
        CHECK (std::abs ((at (d, 50, 10) - at (d, 49, 10)) - 30.0f) < 1e-3f,
               "readability #69: a peak keeps its height above its band");
        CHECK (rowAvg (d, 2) > rowAvg (tilt, 2) && rowAvg (d, h - 3) < rowAvg (tilt, h - 3),
               "readability #69: the treble rises, the bass comes down");
    }
    {
        s.freqBalance = 0.5f;
        auto d = tilt;
        SpectralEngine::balanceLevels (d, w, h, s);
        const double spread = rowAvg (d, h - 4) - rowAvg (d, 3);
        const double was = rowAvg (tilt, h - 4) - rowAvg (tilt, 3);
        CHECK (std::abs (spread - was * 0.5) < 1.0, "readability #69: half balance halves the tilt");
        s.freqBalance = 1.0f;
    }
    {
        // Rows below the window (nothing there) are never lifted; no lift
        // exceeds half the window; pixels beyond the audio stay -300.
        std::vector<float> d ((size_t) w * h, -10.0f);
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < w; ++x)
                at (d, x, y) = -150.0f;
        for (int y = 20; y <= 28; ++y)   // a quiet band among loud ones: wants ~81 dB of lift
            for (int x = 0; x < w; ++x)
                at (d, x, y) = -118.0f;
        at (d, 0, h - 1) = -300.0f;
        SpectralEngine::balanceLevels (d, w, h, s);
        bool emptyKept = true;
        for (int y = 0; y < 12; ++y)
            emptyKept = emptyKept && at (d, 50, y) <= -150.0f;
        CHECK (emptyKept, "readability #69: an empty band is not lifted into view");
        CHECK (std::abs (at (d, 50, 24) - (-118.0f + 60.0f)) < 1e-3f, "readability #69: at most half the window of lift");
        CHECK (at (d, 0, h - 1) == -300.0f, "readability #69: beyond the audio stays as is");
    }
    {
        // Time balance: a loud first half, a quiet second half, then silence.
        std::vector<float> d ((size_t) w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                at (d, x, y) = x < 45 ? -20.0f : (x < 90 ? -60.0f : -200.0f);
        Settings t = s;
        t.freqBalance = 0.0f;
        t.timeBalance = 1.0f;
        SpectralEngine::balanceLevels (d, w, h, t);
        CHECK (std::abs (at (d, 10, 5) - at (d, 70, 5)) < 1.0f, "readability #69: time balance evens loud and quiet parts");
        CHECK (at (d, 97, 5) <= -200.0f, "readability #69: silence is not lifted");
    }

    // Settings: compared (the view re-renders), saved, read back.
    {
        Settings a, b;
        b.timeBalance = 0.3f;
        CHECK (a != b && ! a.readability && a.freqBalance > 0.0f && a.timeBalance == 0.0f,
               "readability #69: off by default, part of the view's settings");
    }
    const juce::File store = makeTempFile ("readability69.recent");
    store.deleteFile ();
    {
        Application app (store);
        SpectralControlsPanel panel (app);
        auto& btn = panel.readabilityBtnRef ();
        CHECK (! btn.getToggleState () && ! panel.freqBalanceSliderRef ().isEnabled ()
                   && (int) panel.freqBalanceSliderRef ().getValue () == 70,
               "readability #69: the panel starts off, sliders disabled");
        btn.setToggleState (true, juce::dontSendNotification);
        btn.onClick ();
        CHECK (app.settings.readability && panel.freqBalanceSliderRef ().isEnabled ()
                   && panel.timeBalanceSliderRef ().isEnabled (),
               "readability #69: on enables the sliders");
        panel.freqBalanceSliderRef ().setValue (40.0, juce::sendNotificationSync);
        panel.timeBalanceSliderRef ().setValue (25.0, juce::sendNotificationSync);
        CHECK (std::abs (app.settings.freqBalance - 0.4f) < 1e-6f && std::abs (app.settings.timeBalance - 0.25f) < 1e-6f,
               "readability #69: the sliders set the amounts");

        // The control API reads and sets the same values.
        const auto sg = ctlResult (ctlCall (app, "settings.get", scctl::obj ({ { "keys",
                                       scctl::arr ({ "readability", "readability_freq", "readability_time" }) } })));
        CHECK ((bool) sg.getProperty ("values", {}).getProperty ("readability", false)
                   && std::abs ((double) sg.getProperty ("values", {}).getProperty ("readability_time", 0.0) - 0.25) < 1e-6,
               "readability #69: settings.get");
        CHECK (ctlOk (ctlCall (app, "settings.set", scctl::obj ({ { "values", scctl::obj ({ { "readability_freq", 7.0 } }) } })))
                   && app.settings.freqBalance == 1.0f,
               "readability #69: settings.set clamps to 0..1");
        app.settings.freqBalance = 0.4f;
        app.saveSettings ();
    }
    {
        Application app (store);
        CHECK (app.settings.readability && std::abs (app.settings.freqBalance - 0.4f) < 1e-6f
                   && std::abs (app.settings.timeBalance - 0.25f) < 1e-6f,
               "readability #69: remembered after a restart");
        SpectralControlsPanel panel (app);
        CHECK (panel.readabilityBtnRef ().getToggleState () && (int) panel.timeBalanceSliderRef ().getValue () == 25,
               "readability #69: the panel shows the saved state");
        // Reset puts every Spectral Controls setting back, Readability off.
        for (auto* c : panel.getChildren ())
            if (auto* b = dynamic_cast<juce::Button*> (c); b != nullptr && b->getName () == "resetFreqBtn" && b->onClick)
                b->onClick ();
        CHECK (! app.settings.readability && ! panel.readabilityBtnRef ().getToggleState (),
               "readability #69: Reset turns it off");
    }
    store.deleteFile ();
}
