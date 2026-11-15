// Regression tests for the 2026-11 audit fixes (panels). Included by tests/UnitTests.cpp;
// uses its helpers (CHECK, progress, writeWav, makeTempFile, ...).

// ---------------------------------------------------------------- helpers

// Peak bin of a normalised magnitude curve (DC excluded).
static int regrPanels_peakBin (const std::vector<float>& mag)
{
    int peak = 1;
    for (int k = 2; k < (int) mag.size(); ++k)
        if (mag[(size_t) k] > mag[(size_t) peak]) peak = k;
    return peak;
}

static double regrPanels_binDb (const std::vector<float>& mag, int k)
{
    return -FFTAnalysisView::kNormFloorDb * (1.0 - (double) mag[(size_t) k]);
}

static int regrPanels_bandIndex (double centre)
{
    const double* c = FFTAnalysisView::thirdOctCentres();
    for (int b = 0; b < FFTAnalysisView::numThirdOctBands(); ++b)
        if (std::abs (c[(size_t) b] - centre) < 1e-6) return b;
    return -1;
}

static juce::File regrPanels_freshDir (const juce::String& name)
{
    const juce::File dir = makeTempFile (name);
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

// ------------------------------------------- PythonPanel (panels-other#W0/W3/W5)

// Add: the typed name used to be read from the dialog's TextEditor AFTER
// `delete w` (use-after-free). The current script's edits must also be saved
// before the editor switches to the new script.
static void regrPanels_pythonAddDialog()
{
    progress ("regression panels: Python Add dialog name + save before switch");
    const juce::File dir = regrPanels_freshDir ("py_regr_add");
    Application app;
    app.recentStore->removeValue ("python_last_script");
    PythonPanel panel (app, dir);
    panel.setSize (320, 400);

    juce::TextEditor* ed = nullptr;
    juce::ComboBox* box = nullptr;
    juce::TextButton* addBtn = nullptr;
    for (auto* c : panel.getChildren())
    {
        if (ed == nullptr) if (auto* e = dynamic_cast<juce::TextEditor*> (c)) { ed = e; continue; }
        if (box == nullptr) if (auto* b = dynamic_cast<juce::ComboBox*> (c)) { box = b; continue; }
        if (auto* tb = dynamic_cast<juce::TextButton*> (c))
            if (tb->getButtonText() == "Add") addBtn = tb;
    }
    CHECK (ed != nullptr && box != nullptr && addBtn != nullptr && addBtn->onClick,
           "regr python add: panel widgets found");
    CHECK (box == nullptr || box->getNumItems() >= 1,
           "regr python add: the example scripts are listed");
    if (ed == nullptr || box == nullptr || addBtn == nullptr || !addBtn->onClick
        || box->getNumItems() < 1) { dir.deleteRecursively(); return; }

    const juce::File first = dir.getChildFile (box->getItemText (box->getSelectedItemIndex()));
    ed->setText ("# regr unsaved edit", false);

    ScModalDriver driver ([] (juce::Component& m)
    {
        auto* aw = dynamic_cast<juce::AlertWindow*> (&m);
        if (aw == nullptr) return false;
        auto* te = aw->getTextEditor ("name");
        if (te == nullptr) return false;
        te->setText ("RegrTyped", false);
        aw->exitModalState (1);
        return true;
    });
    addBtn->onClick();

    CHECK (driver.acted(), "regr python add: the name dialog was driven");
    CHECK (dir.getChildFile ("RegrTyped.py").existsAsFile(),
           "regr python add: script created with the name typed in the dialog");
    CHECK (first.loadFileAsString() == "# regr unsaved edit",
           "regr python add: edits of the previous script saved before switching");
    dir.deleteRecursively();
}

// Switching scripts in the combo used to drop the editor's unsaved text.
static void regrPanels_pythonSwitchKeepsEdits()
{
    progress ("regression panels: Python script switch keeps edits");
    const juce::File dir = regrPanels_freshDir ("py_regr_switch");
    Application app;
    app.recentStore->removeValue ("python_last_script");
    PythonPanel panel (app, dir);
    panel.setSize (320, 400);

    juce::TextEditor* ed = nullptr;
    juce::ComboBox* box = nullptr;
    for (auto* c : panel.getChildren())
    {
        if (ed == nullptr) ed = dynamic_cast<juce::TextEditor*> (c);
        if (box == nullptr) box = dynamic_cast<juce::ComboBox*> (c);
    }
    CHECK (ed != nullptr && box != nullptr && box->getNumItems() >= 2,
           "regr python switch: combo + editor with two scripts");
    if (ed == nullptr || box == nullptr || box->getNumItems() < 2) { dir.deleteRecursively(); return; }

    box->setSelectedId (1, juce::sendNotificationSync);
    const juce::File first = dir.getChildFile (box->getItemText (0));
    ed->setText ("# regr edit kept", false);
    box->setSelectedId (2, juce::sendNotificationSync);
    CHECK (first.loadFileAsString() == "# regr edit kept",
           "regr python switch: leaving a script writes its edits to disk");

    box->setSelectedId (1, juce::sendNotificationSync);
    CHECK (ed->getText() == "# regr edit kept",
           "regr python switch: coming back shows the kept edits");

    // An untouched script is not rewritten.
    const juce::File second = dir.getChildFile (box->getItemText (1));
    const juce::String secondText = second.loadFileAsString();
    box->setSelectedId (2, juce::sendNotificationSync);
    box->setSelectedId (1, juce::sendNotificationSync);
    CHECK (second.loadFileAsString() == secondText,
           "regr python switch: unmodified scripts stay byte-identical");
    dir.deleteRecursively();
}

// ------------------------------------------- FFT levels (panels-analysis#W0/W1/W3/W4)

// A sine of peak A must read its RMS level A/sqrt(2) whatever the window
// (the old |X|/N read -12 dB (rect) / -18 dB (Hann) for A = 0.5).
static void regrPanels_fftRmsNormalisation()
{
    progress ("regression panels: FFT RMS normalisation");
    const juce::File wav = makeTempFile ("regr_fft_norm.wav");
    // 500 Hz = bin 64 of a 1024-point FFT at 8 kHz: no scalloping.
    CHECK (writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
        { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                          * 500.0 * (double) f / rate); }),
          "regr fft norm: wrote tone");
    Application app;
    CHECK (app.openFile (wav), "regr fft norm: open");
    app.settings.fftLocalN = 1024;
    FFTAnalysisView view (app);
    view.setSize (320, 200);
    view.setAxisMode (0);
    app.setSelection ({ 0.0, 0.0 });
    app.setPlayhead (1.0);

    const double expect = 20.0 * std::log10 (0.5 / std::sqrt (2.0));   // -9.03 dB
    for (int wt : { 0, 1, 2 })
    {
        view.setWindowType (wt);
        view.recomputeIfNeeded();
        CHECK (!view.getSpectrum().empty(), "regr fft norm: spectrum computed");
        if (view.getSpectrum().empty()) continue;
        const auto& mag = view.getSpectrum()[0];
        CHECK (regrPanels_peakBin (mag) == 64, "regr fft norm: tone on bin 64");
        const double db = regrPanels_binDb (mag, 64);
        CHECK (std::abs (db - expect) < 0.2,
               "regr fft norm: 0.5 sine reads its RMS level (window " + juce::String (wt)
               + "): " + juce::String (db, 2) + " dB");
    }
    wav.deleteFile();
}

// Non-Pascal 1/3-octave bands: band level = SUM of the bin powers (ENBW
// corrected), so a tone reads its RMS level and broadband noise reads its
// band power independently of the FFT size; no band is empty.
static void regrPanels_thirdOctaveBandSum()
{
    progress ("regression panels: 1/3-octave band sum");
    // --- tone ---
    {
        const juce::File wav = makeTempFile ("regr_toct_tone.wav");
        CHECK (writeWav (wav, 1, 8000.0, 32000, [] (int, int64_t f, double rate)
            { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                              * 440.0 * (double) f / rate); }),
              "regr 1/3 oct tone: wrote");
        Application app;
        CHECK (app.openFile (wav), "regr 1/3 oct tone: open");
        app.settings.fftLocalN = 2048;
        FFTAnalysisView view (app);
        view.setSize (320, 200);
        view.setWindowType (1);
        view.setAxisMode (2);
        app.setSelection ({ 0.0, 0.0 });
        app.setPlayhead (2.0);
        view.recomputeIfNeeded();
        const int b400 = regrPanels_bandIndex (400.0);
        const auto& bands = view.getThirdOctMags();
        CHECK (!bands.empty() && b400 >= 0 && b400 < (int) bands[0].size(),
               "regr 1/3 oct tone: bands computed");
        if (!bands.empty() && b400 >= 0 && b400 < (int) bands[0].size())
        {
            const double expect = 20.0 * std::log10 (0.5 / std::sqrt (2.0));
            CHECK (std::abs ((double) bands[0][(size_t) b400] - expect) < 0.6,
                   "regr 1/3 oct tone: 400 Hz band reads the tone's RMS level: "
                   + juce::String (bands[0][(size_t) b400], 2));
        }
        wav.deleteFile();
    }

    // --- white noise: level independent of N, equal to its band power ---
    {
        const juce::File wav = makeTempFile ("regr_toct_noise.wav");
        juce::Random rng (12345);
        CHECK (writeWav (wav, 1, 8000.0, 32000, [&rng] (int, int64_t, double)
            { return rng.nextFloat() - 0.5f; }),   // uniform, variance 1/12
              "regr 1/3 oct noise: wrote");
        Application app;
        CHECK (app.openFile (wav), "regr 1/3 oct noise: open");
        app.settings.fftLocalN = 1024;
        FFTAnalysisView view (app);
        view.setSize (320, 200);
        view.setWindowType (1);
        view.setAxisMode (2);
        app.setSelection ({ 0.0, app.audioEngine().duration() });
        const int b1k = regrPanels_bandIndex (1000.0);

        auto band1k = [&] (int n) -> double
        {
            // The panel skips an expensive recompute requested within 60 ms
            // of the previous one. Instead of sleeping a fixed margin past it
            // (a 70 ms sleep was flaky on a loaded machine), retry until the
            // spectrum really has the new size's bin count.
            const size_t before = view.getSpectrum().empty() ? 0 : view.getSpectrum()[0].size();
            view.setFftSize (n);
            const auto deadline = juce::Time::getMillisecondCounter() + 5000;
            for (;;)
            {
                view.recomputeIfNeeded();
                const size_t now = view.getSpectrum().empty() ? 0 : view.getSpectrum()[0].size();
                if (now != 0 && now != before) break;
                if (juce::Time::getMillisecondCounter() > deadline)
                {
                    CHECK (false, "regr 1/3 oct noise: FFT size " + juce::String (n) + " never recomputed");
                    return -999.0;
                }
                juce::Thread::sleep (5);
            }
            const auto& bands = view.getThirdOctMags();
            if (bands.empty() || b1k < 0 || b1k >= (int) bands[0].size()) return -999.0;
            return (double) bands[0][(size_t) b1k];
        };
        const double l1024 = band1k (1024);
        const double l8192 = band1k (8192);
        const double width = 1000.0 * (std::pow (10.0, 0.05) - std::pow (10.0, -0.05));
        const double expect = 10.0 * std::log10 ((1.0 / 12.0) * width / 4000.0);   // -23.2 dB
        CHECK (std::abs (l1024 - l8192) < 1.0,
               "regr 1/3 oct noise: band level does not depend on the FFT size: "
               + juce::String (l1024, 2) + " vs " + juce::String (l8192, 2));
        CHECK (std::abs (l1024 - expect) < 1.5,
               "regr 1/3 oct noise: 1 kHz band reads the noise band power: "
               + juce::String (l1024, 2) + " (expected " + juce::String (expect, 2) + ")");
        wav.deleteFile();
    }

    // --- 48 kHz / N = 1024: low bands narrower than a bin are not empty ---
    {
        const juce::File wav = makeTempFile ("regr_toct_lowband.wav");
        juce::Random rng (777);
        CHECK (writeWav (wav, 1, 48000.0, 48000, [&rng] (int, int64_t, double)
            { return rng.nextFloat() - 0.5f; }),
              "regr 1/3 oct low bands: wrote");
        Application app;
        CHECK (app.openFile (wav), "regr 1/3 oct low bands: open");
        app.settings.fftLocalN = 1024;
        FFTAnalysisView view (app);
        view.setSize (320, 200);
        view.setAxisMode (2);
        app.setSelection ({ 0.0, app.audioEngine().duration() });
        view.recomputeIfNeeded();
        const auto& bands = view.getThirdOctMags();
        CHECK (!bands.empty() && !bands[0].empty(), "regr 1/3 oct low bands: computed");
        if (!bands.empty())
        {
            double lowest = 1e9;
            for (float v : bands[0]) lowest = std::min (lowest, (double) v);
            CHECK (lowest > -100.0,
                   "regr 1/3 oct low bands: no band falls to the -160 dB floor: "
                   + juce::String (lowest, 1));
        }
        wav.deleteFile();
    }
}

// ------------------------------------------- live cache keys (panels-analysis#W2)

// Solo switch (same channel count, other channel) and an in-place content
// change (same region) must both recompute the live spectrum.
static void regrPanels_liveCacheKeys()
{
    progress ("regression panels: FFT live cache keys");
    const juce::File wav = makeTempFile ("regr_cache_solo.wav");
    CHECK (writeWav (wav, 2, 8000.0, 16000, [] (int ch, int64_t f, double rate)
        { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                          * (ch == 0 ? 500.0 : 1000.0) * (double) f / rate); }),
          "regr cache: wrote two-tone stereo");
    {
        Application app;
        CHECK (app.openFile (wav), "regr cache: open");
        app.settings.fftLocalN = 1024;
        FFTAnalysisView view (app);
        view.setSize (320, 200);
        view.setAxisMode (0);
        app.setSelection ({ 0.0, 0.0 });
        app.setPlayhead (1.0);

        app.visibleChannels = { 0 };
        view.recomputeIfNeeded();
        CHECK (!view.getSpectrum().empty() && regrPanels_peakBin (view.getSpectrum()[0]) == 64,
               "regr cache: solo ch1 shows its 500 Hz tone");
        app.visibleChannels = { 1 };
        view.recomputeIfNeeded();
        CHECK (!view.getSpectrum().empty() && regrPanels_peakBin (view.getSpectrum()[0]) == 128,
               "regr cache: soloing ch2 recomputes (1000 Hz), not the cached ch1 curve");

        // Curve mode (Amp vs Freq) has its own cache with the same key problem.
        view.setAxisMode (5);
        app.visibleChannels = { 0 };
        view.recomputeIfNeeded();
        auto peakX = [&view] () -> double
        {
            const auto& cv = view.getLiveCurves();
            if (cv.empty() || cv[0].ampDb.empty()) return -1.0;
            size_t p = 0;
            for (size_t i = 1; i < cv[0].ampDb.size(); ++i)
                if (cv[0].ampDb[i] > cv[0].ampDb[p]) p = i;
            return cv[0].x[p];
        };
        CHECK (std::abs (peakX() - 500.0) < 8.0, "regr cache: curve mode solo ch1 at 500 Hz");
        app.visibleChannels = { 1 };
        view.recomputeIfNeeded();
        CHECK (std::abs (peakX() - 1000.0) < 8.0,
               "regr cache: curve mode recomputes after the solo switch");
    }
    wav.deleteFile();

    // Content change in place (loop recording / edits bump contentVersion).
    {
        Application app;
        CHECK (app.newFile (1, 8000.0), "regr cache content: new document");
        auto& eng = app.audioEngine();
        const int64_t N = 16000;
        std::vector<float> a ((size_t) N), b ((size_t) N);
        for (int64_t i = 0; i < N; ++i)
        {
            a[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 500.0 * (double) i / 8000.0);
            b[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) i / 8000.0);
        }
        CHECK (eng.appendSamples (a.data(), N), "regr cache content: appended 500 Hz");
        app.settings.fftLocalN = 1024;
        app.visibleChannels = { 0 };
        FFTAnalysisView view (app);
        view.setSize (320, 200);
        view.setAxisMode (0);
        app.setSelection ({ 0.0, 0.0 });
        app.setPlayhead (1.0);
        view.recomputeIfNeeded();
        CHECK (!view.getSpectrum().empty() && regrPanels_peakBin (view.getSpectrum()[0]) == 64,
               "regr cache content: 500 Hz before the overwrite");
        CHECK (eng.overwriteSamples (b.data(), 0, N), "regr cache content: overwrote with 1000 Hz");
        view.recomputeIfNeeded();
        CHECK (!view.getSpectrum().empty() && regrPanels_peakBin (view.getSpectrum()[0]) == 128,
               "regr cache content: same region recomputed after the content changed");
    }
}

// ------------------------------------------- per-channel calibration (NEW bkdk)

static void regrPanels_perChannelCalibration()
{
    progress ("regression panels: per-channel calibration (clipboard)");
    const juce::File wav = makeTempFile ("regr_bkdk_fft.wav");
    CHECK (writeWav (wav, 2, 8000.0, 16000, [] (int, int64_t f, double rate)
        { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                          * 500.0 * (double) f / rate); }),
          "regr bkdk: wrote identical stereo tone");
    Application app;
    CHECK (app.openFile (wav), "regr bkdk: open");
    FileDoc& d = *app.docs[0];
    d.scale.unit = ScaleCalibration::Unit::Volts;
    d.scale.fullScaleValue = 1.0;
    d.scale.channelFullScale = { 1.0, 10.0 };   // ch2 is 10x more sensitive
    app.altFreqRefs.clear();
    app.settings.fftLocalN = 1024;

    FFTAnalysisView view (app);
    view.setSize (320, 200);
    view.setAxisMode (0);
    app.setSelection ({ 0.0, 0.0 });
    app.setPlayhead (1.0);

    auto harmonicAmp = [&] () -> double
    {
        view.recomputeIfNeeded();
        view.setHarmonicReference (1, 500.0);
        const auto lines = juce::StringArray::fromLines (view.buildHarmonicsTable());
        if (lines.size() < 2) return -1.0;
        const auto f = juce::StringArray::fromTokens (lines[1], "\t", "");
        return f.size() >= 3 ? f[2].getDoubleValue() : -1.0;
    };
    app.visibleChannels = { 0 };
    const double a0 = harmonicAmp();
    CHECK (view.levelAxisChannel() == 0, "regr bkdk: level axis follows ch1 when it is shown");
    app.visibleChannels = { 1 };
    const double a1 = harmonicAmp();
    CHECK (view.levelAxisChannel() == 1, "regr bkdk: level axis follows the soloed ch2");
    CHECK (a0 > 0.0 && std::abs (a1 / a0 - 10.0) < 0.1,
           "regr bkdk: Copy Harmonics uses the channel's own full scale: "
           + juce::String (a0, 4) + " / " + juce::String (a1, 4));

    // Copy Spectrum: one column per channel, each with its own full scale.
    app.visibleChannels = { 0, 1 };
    view.resetHarmonicReference();
    view.recomputeIfNeeded();
    double ratio = -1.0;
    CHECK (clipboardRetry ([&] {
              if (!clipboardWriteVerified ("")) return false;
              view.copySpectrumToClipboard();
              const auto lines = juce::StringArray::fromLines (
                  juce::SystemClipboard::getTextFromClipboard());
              for (int i = 1; i < lines.size(); ++i)
              {
                  const auto c = juce::StringArray::fromTokens (lines[i], "\t", "");
                  if (c.size() >= 3 && std::abs (c[0].getDoubleValue() - 500.0) < 0.01)
                  {
                      const double v0 = c[1].getDoubleValue(), v1 = c[2].getDoubleValue();
                      ratio = v0 > 0.0 ? v1 / v0 : -1.0;
                      return true;
                  }
              }
              return false;
          }), "regr bkdk: spectrum copy has the 500 Hz row");
    CHECK (std::abs (ratio - 10.0) < 0.1,
           "regr bkdk: Copy Spectrum converts each column with its channel's full scale: "
           + juce::String (ratio, 3));
    wav.deleteFile();
}

// ------------------------------------------- marker detail editors (panels-other#W1/W4/W6/W7/W8/W14)

static void regrPanels_markerEditors()
{
    progress ("regression panels: marker detail editors");
    const juce::File wav = makeTempFile ("regr_mk_edit.wav");
    CHECK (writeWav (wav, 1, 8000.0, 40000, [] (int, int64_t f, double rate)   // 5 s
        { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                          * 440.0 * (double) f / rate); }),
          "regr markers: wrote");
    Application app;
    CHECK (app.openFile (wav), "regr markers: open");
    app.markers().clear();
    const int rid = app.markers().addRegionMarker (1.0, 3.0, "RegionA");
    app.markers().addPointMarker (2.5, "PointB");

    MarkerListPanel panel (app);
    panel.setSize (300, 420);
    panel.refresh();

    auto editorNamed = [&] (const juce::String& nm) -> juce::TextEditor*
    {
        for (int i = 0; i < panel.getNumChildComponents(); ++i)
            if (auto* te = dynamic_cast<juce::TextEditor*> (panel.getChildComponent (i)))
                if (te->getName() == nm) return te;
        return nullptr;
    };
    auto* beginEd = editorNamed ("marker-begin-edit");
    auto* lenEd   = editorNamed ("marker-length-edit");
    auto* titleEd = editorNamed ("marker-title-edit");
    CHECK (beginEd && lenEd && titleEd, "regr markers: editors found");
    if (!beginEd || !lenEd || !titleEd) { wav.deleteFile(); return; }

    auto markerById = [&] (int id) -> const Marker*
    {
        for (const auto& mk : app.markers().markers())
            if (mk.id == id) return &mk;
        return nullptr;
    };
    auto type = [] (juce::TextEditor& ed, const juce::String& t)
    {
        // setText (.., true) posts the change notification as a message:
        // run the queue up to a sentinel posted behind it (not a fixed number
        // of 1 ms pumps, which a loaded machine may not reach).
        ed.setText (t, true);
        CHECK (regDrainMessages (1), "regr markers: text change delivered");
    };
    auto pressReturn = [] (juce::TextEditor& ed) { if (ed.onReturnKey) ed.onReturnKey(); };

    panel.selectRow (0);   // RegionA [1, 3]

    // Clearing Length to retype it must not turn the region into a point.
    type (*lenEd, "");
    const Marker* m = markerById (rid);
    CHECK (m != nullptr && m->end > 0.0, "regr markers: clearing Length alone keeps the region");
    type (*lenEd, "1.9");
    m = markerById (rid);
    CHECK (m != nullptr && std::abs (m->end - 3.0) < 1e-9, "regr markers: typing does not commit yet");
    CHECK (lenEd->getText() == "1.9", "regr markers: the typed text is not reformatted");
    panel.refresh();   // a state sync from elsewhere while typing
    CHECK (lenEd->getText() == "1.9", "regr markers: a refresh keeps the uncommitted text");
    pressReturn (*lenEd);
    m = markerById (rid);
    CHECK (m != nullptr && std::abs (m->end - 2.9) < 1e-9, "regr markers: Return commits Length");

    // Invalid Begin text leaves the marker where it is (used to jump to 0 s).
    type (*beginEd, "abc");
    pressReturn (*beginEd);
    m = markerById (rid);
    CHECK (m != nullptr && std::abs (m->start - 1.0) < 1e-9, "regr markers: invalid Begin is ignored");

    // Moving the marker past PointB re-sorts the list: the editors must keep
    // following RegionA.
    type (*beginEd, "2.6");   // RegionA [2.6, 2.9] now sorts after PointB (2.5)
    pressReturn (*beginEd);
    m = markerById (rid);
    CHECK (m != nullptr && std::abs (m->start - 2.6) < 1e-9, "regr markers: Begin committed");
    const auto rows = panel.selectedRowIndices();
    CHECK (rows.size() == 1 && rows[0] == 1, "regr markers: selection follows the re-sorted marker");
    CHECK (titleEd->getText() == "RegionA", "regr markers: details still show the edited marker");

    // DEL (no MainWindow hook): deletes the audio and notifies the views.
    int notified = 0;
    app.onStateChange = [&notified] { ++notified; };
    panel.selectRow (1);   // RegionA [2.6, 2.9] -> time selection
    const double durBefore = app.audioEngine().duration();
    notified = 0;
    panel.deleteAudioData();
    CHECK (app.audioEngine().duration() < durBefore - 0.1, "regr markers: DEL deleted the audio range");
    CHECK (notified > 0, "regr markers: DEL refreshes the views (onStateChange)");

    // With the MainWindow hook set, DEL is routed through it.
    int hooked = 0;
    panel.onDeleteAudioRequested = [&hooked] { ++hooked; };
    const double durHook = app.audioEngine().duration();
    panel.deleteAudioData();
    CHECK (hooked == 1 && std::abs (app.audioEngine().duration() - durHook) < 1e-9,
           "regr markers: DEL goes through onDeleteAudioRequested when set");
    app.onStateChange = nullptr;
    wav.deleteFile();
}

// Switching documents must drop the previous file's row selection.
static void regrPanels_markerDocSwitch()
{
    progress ("regression panels: marker selection cleared on document switch");
    const juce::File wa = makeTempFile ("regr_mk_docA.wav");
    const juce::File wb = makeTempFile ("regr_mk_docB.wav");
    auto tone = [] (int, int64_t f, double rate)
        { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) f / rate); };
    CHECK (writeWav (wa, 1, 8000.0, 16000, tone) && writeWav (wb, 1, 8000.0, 16000, tone),
           "regr markers doc: wrote two files");
    Application app;
    CHECK (app.openFile (wa) && app.openFile (wb) && app.numDocs() == 2, "regr markers doc: two docs");
    app.activate (0);
    app.markers().clear();
    app.markers().addRegionMarker (0.2, 0.4, "A0");
    app.activate (1);
    app.markers().clear();
    app.markers().addRegionMarker (1.0, 1.5, "B0");
    app.activate (0);

    MarkerListPanel panel (app);
    panel.setSize (300, 420);
    panel.refresh();
    panel.selectRow (0);
    CHECK (panel.numSelectedRows() == 1, "regr markers doc: row selected in doc A");
    app.activate (1);
    panel.refresh();
    CHECK (panel.numSelectedRows() == 0, "regr markers doc: switching documents clears the row selection");
    wa.deleteFile();
    wb.deleteFile();
}

// Marker paste: h:mm:ss is read as hours, header/stray rows are skipped.
static void regrPanels_markerPasteTimes()
{
    progress ("regression panels: marker paste h:mm:ss (clipboard)");
    const juce::File wav = makeTempFile ("regr_mk_paste.wav");
    CHECK (writeWav (wav, 1, 8000.0, 40000, [] (int, int64_t, double) { return 0.0f; }),
           "regr paste: wrote");
    Application app;
    CHECK (app.openFile (wav), "regr paste: open");
    app.markers().clear();
    MarkerListPanel panel (app);
    panel.setSize (300, 420);
    CHECK (clipboardRetry ([&] { return clipboardWriteVerified (
              "Title\tBegin\tEnd\tComment\r\nX\t0:00:02\t-\t"); }),
           "regr paste: clipboard holds the payload");
    const int added = panel.pasteMarkersFromClipboard();
    CHECK (added == 1, "regr paste: the header row is not pasted as a marker");
    const auto& ms = app.markers().markers();
    CHECK (ms.size() == 1 && ms[0].name == "X" && std::abs (ms[0].start - 2.0) < 1e-9,
           "regr paste: 0:00:02 reads as 2 s");
    wav.deleteFile();
}


// integration (mainwindow#3): the Files panel's Close (button / row menu) asks
// before discarding unsaved changes, like File > Close; Cancel keeps the file.
static void regrPanels_fileListCloseAsks()
{
    progress ("regression panels (integration): Files panel close asks before discarding unsaved changes");
    const juce::File wav = makeTempFile ("reg_panels_close_ask.wav");
    writeWav (wav, 1, 8000.0, 800, [] (int, int64_t, double) { return 0.1f; });
    Application app;
    CHECK (app.openFile (wav), "reg panels close: open");
    if (app.doc () == nullptr) return;
    app.doc ()->metaDirty = true;   // unsaved metadata
    FileListPanel panel (app);
    panel.setSize (300, 200);

    bool asked = false;
    {
        ScModalDriver driver ([&] (juce::Component& m)
        {
            if (auto* aw = dynamic_cast<juce::AlertWindow*> (&m))
            {
                asked = true;
                aw->exitModalState (0);   // Cancel
                return true;
            }
            return false;
        });
        panel.closeDoc (0);
    }
    CHECK (asked, "reg panels close: unsaved document -> confirmation prompt");
    CHECK (app.numDocs () == 1, "reg panels close: Cancel keeps the document open");

    app.doc ()->metaDirty = false;
    CHECK (!app.audioEngine ().hasUnsavedChanges (),
           "reg panels close: a freshly opened file has no unsaved audio changes");
    bool askedAgain = false;
    {
        // Any prompt here is a failure; the driver only keeps it from blocking.
        ScModalDriver driver ([&] (juce::Component& m)
        {
            if (auto* aw = dynamic_cast<juce::AlertWindow*> (&m))
            {
                askedAgain = true;
                aw->exitModalState (0);
                return true;
            }
            return false;
        }, 2000);
        panel.closeDoc (0);
    }
    CHECK (!askedAgain, "reg panels close: no prompt for a saved document");
    CHECK (app.numDocs () == 0, "reg panels close: a saved document closes without a prompt");
    wav.deleteFile ();
}


// ------------------------------------------------- Tables plot + footer

// The Tables preview plot: the playhead as a red line in the table's own
// time, a click / drag moves the playhead (unless a click-based alignment
// still needs plot points), and the list/plot splitter.
static void regrPanels_tablesPlotPlayheadAndSplitter()
{
    progress ("regression panels: Tables plot playhead, click/drag to seek, splitter");
    const juce::File store = makeTempFile ("tables_plot.recent");
    store.deleteFile ();
    const juce::File wav = makeTempFile ("tables_plot.wav");
    writeWav (wav, 1, 8000.0, 20 * 8000, [] (int, int64_t, double) { return 0.0f; });
    {
        Application app (store);
        CHECK (app.openFile (wav), "tables plot: the wav opens");
        FileDoc* d = app.doc ();
        if (d == nullptr) { wav.deleteFile (); return; }
        sc::tables::TableData t; std::string err;
        juce::String csv ("time_s,v\n");
        for (int i = 0; i <= 10; ++i) csv << i << "," << (i * i) << "\n";   // 1 row/s, 10 s
        CHECK (sc::tables::importTable ("T", csv, t, &err), "tables plot: the table imports");
        t.offsetSecs = 2.0;   // the table starts 2 s into the document
        d->tables.push_back (std::move (t));

        TablesPanel panel (app);
        panel.setSize (300, 400);
        panel.refresh ();
        auto& plot = panel.plotView ();
        CHECK (plot.focusTableIndex () == 0 && std::abs (plot.tableDuration () - 10.0) < 1e-9,
               "tables plot: the plot shows the table");
        CHECK (std::abs (plot.docToTableTime (5.0) - 3.0) < 1e-9
               && std::abs (plot.tableToDocTime (3.0) - 5.0) < 1e-9,
               "tables plot: document <-> table time through the table's offset");
        const int x6 = (int) std::floor (plot.tableTimeToX (6.0));
        CHECK (std::abs (plot.xToTableTime (x6) - 6.0) < 10.0 / plot.getWidth (),
               "tables plot: column <-> table time round trip");

        // The playhead line.
        plot.setPlayhead (5.0);
        const int px = plot.playheadX ();
        CHECK (px == (int) std::floor (plot.tableTimeToX (3.0)), "tables plot: the playhead sits at table time 3 s");
        {
            juce::Image img (juce::Image::RGB, plot.getWidth (), plot.getHeight (), true);
            juce::Graphics g (img);
            plot.paint (g);
            const auto c = img.getPixelAt (px, plot.getHeight () / 3);
            CHECK (c.getRed () > 200 && c.getGreen () < 80 && c.getBlue () < 80,
                   "tables plot: the playhead is drawn as a red line");
        }
        plot.setPlayhead (1.0);   // before the table starts
        CHECK (plot.playheadX () == -1, "tables plot: no line while the playhead is outside the table");

        // A click moves the playhead; dragging keeps moving it.
        const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);
        plot.mouseDown (mkMouse (plot, x6, 30, lmb));
        CHECK (std::abs (app.playhead - plot.tableToDocTime (plot.xToTableTime (x6))) < 1e-9
               && std::abs (app.playhead - 8.0) < 0.1,
               "tables plot: a click moves the playhead there (table 6 s = document 8 s)");
        const int x4 = (int) std::floor (plot.tableTimeToX (4.0));
        plot.mouseDrag (mkMouse (plot, x4, 30, lmb));
        // A drag past the threshold now selects that span instead of scrubbing
        // (the waveform's behaviour): the playhead stays where the press put it
        // and the document gets the range, in document seconds.
        CHECK (std::abs (app.playhead - 8.0) < 0.1,
               "tables plot: a drag leaves the playhead where the click put it");
        CHECK (std::abs (app.selection.getStart() - 6.0) < 0.1
            && std::abs (app.selection.getEnd() - 8.0) < 0.1,
               "tables plot: dragging selects the dragged span (table 4..6 s = document 6..8 s)");
        plot.mouseUp (mkMouse (plot, x4, 30, lmb));
        app.setSelection ({});

        // A click-based alignment that needs plot points takes the click.
        panel.startPointAlign (1, 0, -1, 0);
        const double before = app.playhead;
        plot.mouseDown (mkMouse (plot, x6, 30, lmb));
        plot.mouseDrag (mkMouse (plot, x4, 30, lmb));
        CHECK (std::abs (app.playhead - before) < 1e-12 && panel.hasPendingAlignment (),
               "tables plot: a pending alignment takes the click, the playhead stays");
        plot.mouseUp (mkMouse (plot, x4, 30, lmb));
        // Its plot point taken, the alignment waits for the waveform click:
        // the plot is free again, click and drag alike.
        plot.mouseDown (mkMouse (plot, x6, 30, lmb));
        CHECK (std::abs (app.playhead - 8.0) < 0.1 && panel.hasPendingAlignment (),
               "tables plot: once the plot point is taken, a click seeks again");
        plot.mouseDrag (mkMouse (plot, x4, 30, lmb));
        CHECK (std::abs (app.playhead - 6.0) < 0.1, "tables plot: ... and so does the drag that follows");
        plot.mouseUp (mkMouse (plot, x4, 30, lmb));
        panel.cancelPendingAlignment ();

        // A focused parameter's own offset composes with the table's.
        app.doc ()->tables[0].params[0].offsetSecs = 1.0;
        plot.setFocusParamIndex (0);
        CHECK (std::abs (plot.docToTableTime (5.0) - 2.0) < 1e-9,
               "tables plot: the focused parameter's own alignment maps the playhead");
        plot.setFocusParamIndex (-1);

        // The splitter between the list and the plot.
        auto& sp = panel.plotSplitter ();
        auto& list = panel.listTable ();
        CHECK (sp.getY () == list.getBottom () && sp.getBottom () == plot.getY (),
               "tables splitter: sits between the list and the plot");
        const int plotH0 = plot.getHeight (), listH0 = list.getHeight ();
        sp.mouseDown (mkMouse (sp, 10, 3, lmb));
        sp.mouseDrag (mkMouse (sp, 10, 3 - 40, lmb));   // 40 px up
        sp.mouseUp (mkMouse (sp, 10, 3 - 40, lmb));
        CHECK (plot.getHeight () == plotH0 + 40 && list.getHeight () == listH0 - 40
               && sp.getBottom () == plot.getY (),
               "tables splitter: dragging up gives the plot 40 px");
        CHECK (app.recentStore->getIntValue ("tables_plot_height", -1) == plotH0 + 40,
               "tables splitter: the height is saved");
        sp.mouseDown (mkMouse (sp, 10, 3, lmb));
        sp.mouseDrag (mkMouse (sp, 10, 3 + 1000, lmb));   // far down
        sp.mouseUp (mkMouse (sp, 10, 3 + 1000, lmb));
        CHECK (plot.getHeight () == 50, "tables splitter: the plot keeps a minimum height");
        sp.mouseDown (mkMouse (sp, 10, 3, lmb));
        sp.mouseDrag (mkMouse (sp, 10, 3 - 1000, lmb));   // far up
        sp.mouseUp (mkMouse (sp, 10, 3 - 1000, lmb));
        CHECK (list.getHeight () >= 40, "tables splitter: the list keeps a minimum height");
        const int saved = plot.getHeight ();

        TablesPanel again (app);
        again.setSize (300, 400);
        CHECK (again.plotView ().getHeight () == saved, "tables splitter: a new panel restores the height");
    }
    wav.deleteFile ();
    store.deleteFile ();
}

// The footer's channel numbers: one per channel, dim when hidden, red when
// muted; a click reports the channel (Ctrl/Cmd included).
static void regrPanels_statusBarChannelNumbers()
{
    progress ("regression panels: footer channel numbers");
    StatusBar sb;
    sb.setSize (900, 22);
    sb.setFileName ("take.wav");
    sb.setSampleRate (48000.0);
    sb.setChannels (4, { 0, 2, 3 }, { false, false, true });
    const auto chips = sb.channelChipRects ();
    CHECK (chips.size () == 4 && sb.channelChipLabel (0) == "1" && sb.channelChipLabel (3) == "4",
           "footer channels: one number per channel, from 1");
    CHECK (sb.channelChipShown (0) && !sb.channelChipShown (1) && sb.channelChipMuted (2)
           && !sb.channelChipMuted (0), "footer channels: shown / muted flags");

    {
        juce::Image img (juce::Image::RGB, 900, 22, true);
        juce::Graphics g (img);
        sb.paint (g);
        // Brightest text pixel inside each number.
        auto brightest = [&] (int i)
        {
            juce::Colour best (0, 0, 0);
            for (int y = chips[(size_t) i].getY (); y < chips[(size_t) i].getBottom (); ++y)
                for (int x = chips[(size_t) i].getX (); x < chips[(size_t) i].getRight (); ++x)
                {
                    const auto c = img.getPixelAt (x, y);
                    if (c.getRed () + c.getGreen () + c.getBlue () > best.getRed () + best.getGreen () + best.getBlue ())
                        best = c;
                }
            return best;
        };
        const auto shown = brightest (0), hidden = brightest (1), muted = brightest (2);
        CHECK (shown.getBrightness () > 0.75f && hidden.getBrightness () < 0.6f,
               "footer channels: a hidden channel's number is darker");
        CHECK (muted.getRed () > 200 && muted.getGreen () < 130, "footer channels: a muted channel's number is red");
    }

    int clicked = -1;
    bool withCmd = false;
    sb.onChannelClicked = [&] (int ch, bool solo) { clicked = ch; withCmd = solo; };
    const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);
    const juce::ModifierKeys cmdLmb (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::commandModifier);
    const juce::ModifierKeys shiftLmb (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
    sb.mouseDown (mkMouse (sb, chips[1].getCentreX (), chips[1].getCentreY (), lmb));
    CHECK (clicked == 1 && !withCmd, "footer channels: a click reports the channel");
    sb.mouseDown (mkMouse (sb, chips[3].getCentreX (), chips[3].getCentreY (), cmdLmb));
    CHECK (clicked == 3 && withCmd, "footer channels: Ctrl/Cmd+click is reported as a solo");
    clicked = -1;
    sb.mouseDown (mkMouse (sb, chips[2].getCentreX (), chips[2].getCentreY (), shiftLmb));
    CHECK (clicked == 2 && withCmd, "footer channels: Shift+click is reported as a solo too");
    clicked = -1;
    sb.mouseDown (mkMouse (sb, 3, 11, lmb));
    CHECK (clicked == -1, "footer channels: a click elsewhere reports nothing");

    sb.setChannels (2, { 0, 1 }, {});
    CHECK (sb.channelChipRects ().size () == 2 && sb.channelChipLabel (0) == "L" && sb.channelChipLabel (1) == "R",
           "footer channels: a stereo file shows L and R, like the tracks");
    sb.setChannels (0, {}, {});
    CHECK (sb.channelChipRects ().empty (), "footer channels: none without a file");
    sb.setChannels (64, {}, {});
    CHECK (!sb.channelChipRects ().empty () && sb.channelChipRects ().size () < 64
           && sb.channelChipRects ().back ().getRight () < sb.getWidth (),
           "footer channels: only the numbers that fit are drawn");
}

// Issue #15: Tables > "Table from Spectral Selection" = Frequency Analysis >
// Freq vs Time (Absolute) of the spectral selection, imported as a new table
// without the panel or the clipboard.
static void regrPanels_tableFromSpectralSelection()
{
    progress ("regression panels: Table from Spectral Selection");
    const juce::File store = makeTempFile ("tables_from_sel.recent");
    store.deleteFile ();
    const juce::File wav = makeTempFile ("tables_from_sel.wav");
    // 1 kHz from 0.5 s to 1.5 s, a stronger 2.5 kHz tone throughout: the
    // selection's frequency band must mask the 2.5 kHz one out. 48 kHz: the
    // fixed 2048-point frames of Freq vs Time last 43 ms, so the 0.8 s
    // selection gets ~70 rows centred from ~0.62 to ~1.38 s.
    writeWav (wav, 1, 48000.0, 2 * 48000, [] (int, int64_t f, double)
    {
        const double t = (double) f / 48000.0;
        double v = 0.5 * std::sin (juce::MathConstants<double>::twoPi * 2500.0 * t);
        if (t >= 0.5 && t < 1.5) v += 0.2 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * t);
        return (float) v;
    });
    {
        Application app (store);
        app.settings.fftTrack = 0;    // Max: the strongest bin inside the mask
        app.settings.fftSmooth = 0;
        CHECK (app.openFile (wav), "table from sel: the wav opens");
        FileDoc* d = app.doc ();
        if (d == nullptr) { wav.deleteFile (); return; }

        std::string err;
        CHECK (! app.addTableFromSpectralSelection (&err) && d->tables.empty () && ! err.empty (),
               "table from sel: refused without a spectral selection (" + juce::String (err) + ")");

        d->spectralSel.addRect (0.6, 1.4, 800.0, 1200.0, false, true);
        const juce::String csv = app.freqVsTimeCsv ();
        const auto lines = juce::StringArray::fromLines (csv.trim ());
        CHECK (lines.size () > 10 && lines[0].startsWith ("time_s\tch1_Hz\t"),
               "table from sel: Freq vs Time columns (" + lines[0] + ")");

        TablesPanel panel (app);
        panel.setSize (300, 400);
        panel.refresh ();
        panel.tableFromSpectralSelection ();
        CHECK (d->tables.size () == 1 && d->tables[0].name.startsWith ("Freq vs Time"),
               "table from sel: one new table, named after the analysis");
        if (d->tables.size () == 1)
        {
            const auto& t = d->tables[0];
            CHECK (t.params.size () == 2 && t.params[0].name == "ch1_Hz" && t.params[1].name.startsWith ("ch1_"),
                   "table from sel: tracked frequency and level per channel");
            CHECK (! t.params.empty () && (int) t.params[0].values.size () == lines.size () - 1,
                   "table from sel: one row per analysis frame");
            if (! t.params.empty () && ! t.params[0].values.empty ())
            {
                const auto& hz = t.params[0];
                const int mid = (int) hz.values.size () / 2;
                CHECK (std::abs (hz.values[(size_t) mid] - 1000.0) < 10.0,
                       "table from sel: the masked band tracks the 1 kHz tone, not 2.5 kHz ("
                       + juce::String (hz.values[(size_t) mid]) + " Hz)");
                // Absolute time: the rows sit on the selection in the document.
                const double tFirst = t.docTimeOfSample (hz, 0);
                const double tLast  = t.docTimeOfSample (hz, (int) hz.values.size () - 1);
                CHECK (tFirst >= 0.55 && tFirst < 0.75 && tLast > 1.25 && tLast <= 1.45,
                       "table from sel: rows placed at the selection's document time ("
                       + juce::String (tFirst, 3) + " .. " + juce::String (tLast, 3) + " s)");
            }
        }
        CHECK (panel.plotView ().focusTableIndex () == 0, "table from sel: the new table is selected");
    }
    wav.deleteFile ();
    store.deleteFile ();
}

// Issue #12: every control a user can see carries a tooltip. Walks the main
// window, every dockable panel and the tool windows; a control counts when it
// and its parents are visible (state-hidden controls are skipped), and JUCE's
// internal parts (scroll bar buttons, a combo's label, a slider's text box,
// table headers) are not controls of their own.
static void collectControlsWithoutTooltip (juce::Component& c, const juce::String& path,
                                           juce::StringArray& out, int& numControls)
{
    if (! c.isVisible ())
        return;
    if (dynamic_cast<juce::ScrollBar*> (&c) != nullptr
        || dynamic_cast<juce::TableHeaderComponent*> (&c) != nullptr)
        return;

    const bool isControl = dynamic_cast<juce::Button*> (&c) != nullptr
                        || dynamic_cast<juce::ComboBox*> (&c) != nullptr
                        || dynamic_cast<juce::Slider*> (&c) != nullptr
                        || (dynamic_cast<juce::TextEditor*> (&c) != nullptr
                            && ! static_cast<juce::TextEditor&> (c).isReadOnly ());
    if (isControl)
    {
        ++numControls;
        auto* tc = dynamic_cast<juce::TooltipClient*> (&c);
        if (tc == nullptr || tc->getTooltip ().trim ().isEmpty ())
        {
            juce::String what = c.getName ();
            if (auto* b = dynamic_cast<juce::Button*> (&c)) what = b->getButtonText ();
            out.add (path + " > " + juce::String (typeid (c).name ()) + " '" + what + "'");
        }
        return;   // a control's own children (combo label, slider text box) are its parts
    }
    for (auto* child : c.getChildren ())
        collectControlsWithoutTooltip (*child, path, out, numControls);
}

static void regrPanels_everyControlHasATooltip()
{
    progress ("regression panels: every visible control has a tooltip (#12)");
    const juce::File store = makeTempFile ("tooltips_all.recent");
    store.deleteFile ();
    const juce::File wav = makeTempFile ("tooltips_all.wav");
    writeWav (wav, 2, 8000.0, 8000, [] (int, int64_t f, double)
    {
        return (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * 440.0 * (double) f / 8000.0));
    });
    {
        Application app (store);
        CHECK (app.openFile (wav), "tooltips: the wav opens");
        std::unique_ptr<MainWindow> win (new MainWindow (app, "tooltips test"));
        win->setSize (1400, 900);
        app.setSelection ({ 0.2, 0.6 });   // shows the status bar time fields

        juce::StringArray missing;
        int numControls = 0;
        collectControlsWithoutTooltip (*win, "MainWindow", missing, numControls);
        for (const char* id : { "editor", "files", "fft", "history", "python", "controls", "markers",
                                "transcription", "selection", "drawing", "tables", "actions", "video" })
            if (auto* panel = win->panelForId (id))
            {
                panel->setSize (std::max (panel->getWidth (), 360), std::max (panel->getHeight (), 500));
                collectControlsWithoutTooltip (*panel, id, missing, numControls);
            }

        win->showStemSeparationWindow ();
        if (auto* w = win->stemSepWindow ())
            collectControlsWithoutTooltip (*w, "Stem Separation", missing, numControls);
        win->showSuperResolutionWindow ();
        if (auto* w = win->superResWindow ())
            collectControlsWithoutTooltip (*w, "Super Resolution", missing, numControls);

        {
            ActionEditWindow w (app, -1);
            collectControlsWithoutTooltip (w, "Edit Action", missing, numControls);
        }
        {
            ActionBatchWindow w (app, -1);
            collectControlsWithoutTooltip (w, "Action Batch", missing, numControls);
        }
        {
            TablesColourDialog w ("Colour", 0, {});
            collectControlsWithoutTooltip (w, "Table colour", missing, numControls);
        }

        CHECK (numControls > 60, "tooltips: the walk reaches the controls (" + juce::String (numControls) + ")");
        CHECK (missing.isEmpty (), "tooltips: controls without a tooltip:\n  " + missing.joinIntoString ("\n  "));
    }
    wav.deleteFile ();
    store.deleteFile ();
}

static void runRegression_panels()
{
    regrPanels_pythonAddDialog();
    regrPanels_pythonSwitchKeepsEdits();
    regrPanels_fftRmsNormalisation();
    regrPanels_thirdOctaveBandSum();
    regrPanels_liveCacheKeys();
    regrPanels_perChannelCalibration();
    regrPanels_markerEditors();
    regrPanels_markerDocSwitch();
    regrPanels_markerPasteTimes();
    regrPanels_fileListCloseAsks();
    regrPanels_tablesPlotPlayheadAndSplitter();
    regrPanels_statusBarChannelNumbers();
    regrPanels_tableFromSpectralSelection();
    regrPanels_everyControlHasATooltip();
}
