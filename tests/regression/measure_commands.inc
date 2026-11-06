// AI control lot 4: measure.* / export.spectrogram / markers CSV commands.
// Included by tests/UnitTests.cpp after control.inc and measures.inc (reuses
// measSine). Reference values: full-scale sine at 1 Pa -> 90.97 dB-SPL RMS,
// 1 kHz 1/3-octave band ~91 dB-SPL (the panel's dB-SPL tests).

static void testMeasureCommands()
{
    Application app;
    const double rate = 48000.0;
    const double fTone = 1365.0 * rate / 65536.0;   // exactly on a 65536-point bin (~1000 Hz)
    const juce::File wav = makeTempFile ("meas_cmd_sine.wav");
    writeWav (wav, 1, rate, 96000, [fTone] (int, int64_t f, double r) {
        return (float) std::sin (2.0 * juce::MathConstants<double>::pi * fTone * (double) f / r);
    });
    const auto op = ctlCall (app, "doc.open", scctl::obj ({ { "path", wav.getFullPathName() } }));
    CHECK (ctlOk (op), "measure cmds: open sine");
    if (!ctlOk (op)) return;
    FileDoc& d = *app.doc();
    d.scale.unit = ScaleCalibration::Unit::Pascal;   // 1.0 full scale = 1 Pa
    d.scale.fullScaleValue = 1.0;

    // Levels (inline: short file): dBFS + dB-SPL.
    auto lv = ctlCall (app, "measure.levels");
    const auto ch0 = ctlResult (lv).getProperty ("channels", {})[0];
    CHECK (ctlOk (lv) && std::abs ((double) ch0.getProperty ("peakDb", 0) - 0.0) < 0.01
               && std::abs ((double) ch0.getProperty ("rmsDb", 0) + 3.0103) < 0.01,
           "measure.levels: full-scale sine peak 0 dBFS, RMS -3.01 dBFS " + scctl::toLine (ch0).substring (0, 200));
    CHECK (std::abs ((double) ch0.getProperty ("rmsAbsDb", 0) - 90.97) < 0.05
               && ch0.getProperty ("absUnit", {}).toString() == "dB-SPL",
           "measure.levels: 90.97 dB-SPL at 1 Pa");

    // Spectrum: identical to the module (the FFT panel's code) for the same settings.
    auto sp = ctlCall (app, "measure.spectrum", scctl::obj ({ { "fftSize", 65536 }, { "window", "hann" },
                                                              { "peaks", scctl::obj ({ { "maxCount", 3 } }) } }));
    if (ctlResult (sp).hasProperty ("job"))
        sp = scctl::obj ({ { "ok", true }, { "result", ctlWaitJob (app, ctlResult (sp).getProperty ("job", {}).toString()).getProperty ("result", {}) } });
    CHECK (ctlOk (sp), "measure.spectrum: ok");
    {
        scm::SpectrumParams p;
        p.t0 = 0.0; p.t1 = d.engine.duration(); p.channels = { 0 }; p.fftSize = 65536; p.windowType = scm::Hann;
        const auto ref = scm::spectrum (d.engine, p, &d.scale);
        const auto rel = ctlResult (sp).getProperty ("channels", {})[0].getProperty ("relDb", {});
        bool same = ref.channels.size() == 1 && rel.size() == (int) ref.channels[0].relDb.size();
        for (int i = 0; same && i < rel.size(); ++i)
            if (std::abs ((double) rel[i] - ref.channels[0].relDb[(size_t) i]) > 0.0006) same = false;   // 3-decimal JSON rounding
        CHECK (same, "measure.spectrum: same values as sc::measures::spectrum (panel code)");
        CHECK (rel.size() > 1365 && std::abs ((double) rel[1365] + 3.0103) < 0.05, "measure.spectrum: tone bin -3.01 dB");
        const auto pk = ctlResult (sp).getProperty ("peaks", {});
        CHECK (pk.size() >= 1 && std::abs ((double) pk[0].getProperty ("freqHz", 0) - fTone) < 1.0,
               "measure.spectrum: strongest peak at the tone");
    }

    // 1/3 octave: Pascal file -> reference analyzer by default, 1 kHz ~ 91 dB-SPL.
    auto to = ctlCall (app, "measure.thirdOctave");
    CHECK (ctlOk (to) && ctlResult (to).hasProperty ("job"), "measure.thirdOctave: reference mode runs as a job");
    const auto toInf = ctlWaitJob (app, ctlResult (to).getProperty ("job", {}).toString(), 60000);
    const auto toRes = toInf.getProperty ("result", {});
    CHECK (toRes.getProperty ("mode", {}).toString() == "reference" && toRes.getProperty ("unit", {}).toString() == "dB-SPL",
           "measure.thirdOctave: reference mode, dB-SPL");
    double k1k = -1000;
    const auto bands = toRes.getProperty ("channels", {})[0].getProperty ("bands", {});
    for (int b = 0; b < bands.size(); ++b)
        if (std::abs ((double) bands[b].getProperty ("fcHz", 0) - 1000.0) < 1e-6) k1k = (double) bands[b].getProperty ("absDb", -1000);
    CHECK (std::abs (k1k - 91.0) < 1.0, "measure.thirdOctave: 1 kHz band ~91 dB-SPL (" + juce::String (k1k, 2) + ")");
    CHECK (ctlCode (ctlCall (app, "measure.thirdOctave", scctl::obj ({ { "order", 7 } }))) == "invalid_params",
           "measure.thirdOctave: bad order refused");

    // Harmonics on a given fundamental.
    auto hm = ctlCall (app, "measure.harmonics", scctl::obj ({ { "fundamentalHz", fTone }, { "maxK", 3 }, { "fftSize", 8192 } }));
    if (ctlResult (hm).hasProperty ("job"))
        hm = scctl::obj ({ { "ok", true }, { "result", ctlWaitJob (app, ctlResult (hm).getProperty ("job", {}).toString()).getProperty ("result", {}) } });
    CHECK (ctlOk (hm) && ctlResult (hm).getProperty ("harmonics", {}).size() == 3, "measure.harmonics: 3 harmonics");

    // Curves.
    auto cv = ctlCall (app, "measure.curve", scctl::obj ({ { "kind", "rms" }, { "windowMs", 50 }, { "hopMs", 100 } }));
    if (ctlResult (cv).hasProperty ("job"))
        cv = scctl::obj ({ { "ok", true }, { "result", ctlWaitJob (app, ctlResult (cv).getProperty ("job", {}).toString()).getProperty ("result", {}) } });
    const auto vals = ctlResult (cv).getProperty ("values", {});
    CHECK (ctlOk (cv) && vals.size() >= 15 && std::abs ((double) vals[5] + 3.01) < 0.1, "measure.curve rms: -3.01 dBFS per window");
    auto cv2 = ctlCall (app, "measure.curve", scctl::obj ({ { "kind", "ampVsFreq" }, { "fftSize", 4096 } }));
    if (ctlResult (cv2).hasProperty ("job"))
        cv2 = scctl::obj ({ { "ok", true }, { "result", ctlWaitJob (app, ctlResult (cv2).getProperty ("job", {}).toString()).getProperty ("result", {}) } });
    CHECK (ctlOk (cv2) && ctlResult (cv2).getProperty ("xUnit", {}).toString() == "Hz", "measure.curve ampVsFreq");

    // Probe + spectrogram data.
    CHECK (ctlOk (ctlCall (app, "measure.probe", scctl::obj ({ { "t", 1.0 }, { "f", fTone } }))), "measure.probe");
    auto sg = ctlCall (app, "measure.spectrogramData", scctl::obj ({ { "start", 0.0 }, { "end", 0.5 }, { "fftSize", 1024 },
                                                                     { "hop", 512 }, { "fMax", 4000 } }));
    auto sgi = ctlWaitJob (app, ctlResult (sg).getProperty ("job", {}).toString(), 30000).getProperty ("result", {});
    CHECK ((bool) sgi.getProperty ("inline", false) && sgi.getProperty ("values", {}).size() == (int) sgi.getProperty ("frames", -1),
           "measure.spectrogramData: inline matrix " + scctl::toLine (sgi).substring (0, 120));
    sg = ctlCall (app, "measure.spectrogramData", scctl::obj ({ { "fftSize", 1024 }, { "hop", 256 }, { "format", "f32" } }));
    sgi = ctlWaitJob (app, ctlResult (sg).getProperty ("job", {}).toString(), 30000).getProperty ("result", {});
    const juce::File f32 (sgi.getProperty ("path", {}).toString());
    CHECK (f32.existsAsFile() && f32.getSize() == (juce::int64) sizeof (float) * (int) sgi.getProperty ("frames", 0)
                                                     * (int) sgi.getProperty ("bins", 0),
           "measure.spectrogramData: f32 file of frames x bins floats");
    f32.deleteFile();

    // Spectrogram PNG export (headless renderer; overlays need the window).
    const juce::File png = makeTempFile ("meas_cmd.png");
    auto ex = ctlCall (app, "export.spectrogram", scctl::obj ({ { "path", png.getFullPathName() }, { "width", 320 }, { "height", 120 } }));
    auto exi = ctlWaitJob (app, ctlResult (ex).getProperty ("job", {}).toString(), 30000);
    CHECK (exi.getProperty ("state", {}).toString() == "done" && png.getSize() > 100, "export.spectrogram: PNG written");
    CHECK (ctlCode (ctlCall (app, "export.spectrogram", scctl::obj ({ { "path", png.getFullPathName() } }))) == "confirm_required",
           "export.spectrogram: existing target needs confirm");
    CHECK (ctlCode (ctlCall (app, "export.spectrogram", scctl::obj ({ { "path", png.getFullPathName() }, { "confirm", true },
                                                                      { "overlays", scctl::obj ({ { "axes", true } }) } })))
               == "needs_ui", "export.spectrogram: overlays need the window");

    // Markers CSV round trip.
    ctlCall (app, "markers.add", scctl::obj ({ { "name", "a" }, { "start", 0.5 } }));
    ctlCall (app, "markers.add", scctl::obj ({ { "name", "b" }, { "start", 1.0 }, { "end", 1.5 } }));
    const juce::File csv = makeTempFile ("meas_cmd_markers.csv");
    CHECK (ctlOk (ctlCall (app, "markers.exportCsv", scctl::obj ({ { "path", csv.getFullPathName() } }))) && csv.getSize() > 0,
           "markers.exportCsv");
    d.markers.clear();
    auto im = ctlCall (app, "markers.importCsv", scctl::obj ({ { "path", csv.getFullPathName() } }));
    CHECK (ctlOk (im) && (int) ctlResult (im).getProperty ("count", 0) == 2, "markers.importCsv: markers back");

    // Busy: a measurement job cannot start next to a mutating job.
    const auto sl = ctlCall (app, "debug.sleep", scctl::obj ({ { "ms", 2000 }, { "mutating", true } }));
    CHECK (ctlCode (ctlCall (app, "measure.thirdOctave")) == "busy", "measure job refused while a mutating job runs");
    ctlCall (app, "job.cancel", scctl::obj ({ { "job", ctlResult (sl).getProperty ("job", {}) } }));
    ctlWaitJob (app, ctlResult (sl).getProperty ("job", {}).toString(), 5000);
}

static void runRegression_measureCommands()
{
    testMeasureCommands();
}
