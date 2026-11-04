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
    if (ed == nullptr || box == nullptr || addBtn == nullptr || !addBtn->onClick
        || box->getNumItems() < 1) return;

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
    if (ed == nullptr || box == nullptr || box->getNumItems() < 2) return;

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
            view.setFftSize (n);
            juce::Thread::sleep (70);   // defeat the expensive-recompute throttle
            view.recomputeIfNeeded();
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
    juce::MessageManager::getInstance();
    auto type = [] (juce::TextEditor& ed, const juce::String& t)
    {
        ed.setText (t, true);   // async change message: pump it
        for (int i = 0; i < 8; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
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
    if (app.audioEngine ().hasUnsavedChanges ()) return;   // nothing else to check without a prompt
    panel.closeDoc (0);
    CHECK (app.numDocs () == 0, "reg panels close: a saved document closes without a prompt");
    wav.deleteFile ();
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
}
