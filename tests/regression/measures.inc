// Tests of the headless measurement module (include/Measures.h, lot 4 core of
// the AI-control plan). Included by tests/UnitTests.cpp; uses its helpers
// (CHECK, progress, writeWav, makeTempFile, scRecomputeUntil, ...).
//
// Two families:
//  - EQUALITY: the FFT Analysis panel's caches (spectrum, 1/3-octave bands,
//    curves) must be bit-identical to the module's output for the same
//    region/settings - the panel and measure.* share one implementation;
//  - KNOWN SIGNALS: levels (sine RMS = peak - 3.01 dB, dB-SPL), peaks (two
//    tones within half a bin), harmonics (synthetic series), cancellation,
//    worker-thread use.
#include "Measures.h"
#include <thread>

namespace scm = sc::measures;

// In-memory document (no file, no I/O); one vector per channel.
static bool measInitEngine (AudioEngine& eng, double rate, const std::vector<std::vector<float>>& data)
{
    return eng.initNew ((int) data.size(), rate, &data);
}

static std::vector<float> measSine (double rate, int64_t frames, double f, double amp, double phase = 0.0)
{
    std::vector<float> v ((size_t) frames);
    for (int64_t i = 0; i < frames; ++i)
        v[(size_t) i] = (float) (amp * std::sin (2.0 * juce::MathConstants<double>::pi * f * (double) i / rate + phase));
    return v;
}

static bool measSameCurves (const std::vector<scm::CurveData>& a, const std::vector<scm::CurveData>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].ampDb != b[i].ampDb || a[i].freqHz != b[i].freqHz)
            return false;
    return true;
}

// Stereo fixture: ch0 = 0.5 sin(1 kHz) + 0.1 sin(3.3 kHz), ch1 = uniform noise.
static juce::File measWriteStereoFixture (const juce::String& name)
{
    const juce::File wav = makeTempFile (name);
    juce::Random rng (4242);
    writeWav (wav, 2, 48000.0, 96000, [&rng] (int ch, int64_t f, double r)
    {
        if (ch == 1) return 0.5f * (rng.nextFloat() - 0.5f);
        const double t = (double) f / r;
        return (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * t)
                        + 0.1 * std::sin (2.0 * juce::MathConstants<double>::pi * 3300.0 * t));
    });
    return wav;
}

// ---------------------------------------------------------------- equality

static void testMeasSpectrumMatchesPanel()
{
    progress ("measures: spectrum == FFT panel cache");
    const juce::File wav = measWriteStereoFixture ("meas_spec.wav");
    Application app;
    CHECK (app.openFile (wav), "measures spectrum: open fixture");
    app.visibleChannels = { 0, 1 };

    struct Case { int wt; int n; double t0, t1; };
    const Case cases[] = {
        { 0, 1024, 0.20, 1.70 },   // multi-window average, rectangular
        { 1, 2048, 0.20, 1.70 },   // Hann
        { 2, 4096, 0.50, 0.50 },   // single window centred on the playhead, Hamming
        { 1, 8192, 0.00, 2.00 },   // whole file
        { 1, 1024, 1.99, 2.00 },   // short region near the end (clamped window)
    };
    for (const auto& c : cases)
    {
        const bool point = (c.t0 == c.t1);
        if (point) { app.setSelection ({ 0.0, 0.0 }); app.setPlayhead (c.t0); }
        else         app.setSelection ({ c.t0, c.t1 });

        FFTAnalysisView view (app);
        view.setSize (320, 200);
        view.setAxisMode (1);
        view.setFftSize (c.n);
        view.setWindowType (c.wt);

        scm::SpectrumParams p;
        p.t0 = point ? app.playhead : app.selection.getStart();
        p.t1 = point ? app.playhead : app.selection.getEnd();
        p.channels = { 0, 1 };
        p.fftSize = c.n;
        p.windowType = c.wt;
        const auto res = scm::spectrum (app.audioEngine(), p, &app.scale());
        const juce::String tag = " (win " + juce::String (c.wt) + ", N " + juce::String (c.n)
                               + ", " + juce::String (c.t0, 2) + ".." + juce::String (c.t1, 2) + ")";
        CHECK (res.channels.size() == 2 && !res.cancelled && res.channels[0].norm.size() == (size_t) (c.n / 2 + 1),
               "measures spectrum: module computed both channels" + tag);
        if (res.channels.size() != 2) continue;

        const bool same = scRecomputeUntil (view, [&]
        {
            const auto& m = view.getSpectrum();
            return view.getCachedN() == c.n && m.size() == 2
                && m[0] == res.channels[0].norm && m[1] == res.channels[1].norm;
        });
        CHECK (same, "measures spectrum: panel cache is bit-identical to the module" + tag);

        bool dbOk = true;
        for (size_t k = 0; k < res.channels[0].norm.size(); ++k)
            if (res.channels[0].relDb[k] != -FFTAnalysisView::kNormFloorDb * (1.0 - (double) res.channels[0].norm[k]))
                dbOk = false;
        CHECK (dbOk, "measures spectrum: relDb uses the panel's dB conversion" + tag);
        CHECK (res.binHz == 48000.0 / c.n && res.framesAveraged >= 1,
               "measures spectrum: analysis metadata filled" + tag);
    }
    wav.deleteFile();
}

static void testMeasThirdOctaveMatchesPanel()
{
    progress ("measures: 1/3 octave == FFT panel cache");
    const juce::File wav = measWriteStereoFixture ("meas_toct.wav");
    Application app;
    CHECK (app.openFile (wav), "measures 1/3 oct: open fixture");
    app.visibleChannels = { 0, 1 };
    app.setSelection ({ 0.1, 1.9 });

    // IEC pipeline (uncalibrated file).
    for (int wt : { 1, 2 })
        for (int n : { 1024, 4096 })
        {
            FFTAnalysisView view (app);
            view.setSize (320, 200);
            view.setFftSize (n);
            view.setWindowType (wt);
            view.setAxisMode (2);
            CHECK (!view.pascalThirdOct(), "measures 1/3 oct: IEC pipeline for an uncalibrated file");

            scm::ThirdOctaveParams p;
            p.t0 = 0.1; p.t1 = 1.9; p.channels = { 0, 1 };
            p.mode = scm::ThirdOctaveParams::Mode::Iec;
            p.fftSize = n; p.windowType = wt;
            const auto res = scm::thirdOctave (app.audioEngine(), p, &app.scale());
            CHECK (res.channels.size() == 2 && res.centresHz.size() == res.channels[0].relDb.size()
                   && res.centresHz.size() == 26,
                   "measures 1/3 oct: 26 IEC bands for both channels");
            if (res.channels.size() != 2) continue;
            const bool same = scRecomputeUntil (view, [&]
            {
                const auto& b = view.getThirdOctMags();
                return b.size() == 2 && b[0] == res.channels[0].relDb && b[1] == res.channels[1].relDb;
            });
            CHECK (same, "measures 1/3 oct: IEC bands bit-identical to the panel (win "
                   + juce::String (wt) + ", N " + juce::String (n) + ")");
        }

    // Reference (dB-SPL) pipeline: Pascal file, order 40 then 10.
    std::vector<double> one { 1.0 };
    CHECK (FileListPanel::applyScaleValues (*app.docs[0], ScaleCalibration::Unit::Pascal, one),
           "measures 1/3 oct: pascal calibration applied");
    for (int order : { 40, 10 })
    {
        app.settings.fftThirdOctOrder = order;
        FFTAnalysisView view (app);
        view.setSize (320, 200);
        view.setAxisMode (2);
        CHECK (view.pascalThirdOct(), "measures 1/3 oct: dB-SPL pipeline for a Pascal file");

        scm::ThirdOctaveParams p;
        p.t0 = 0.1; p.t1 = 1.9; p.channels = { 0, 1 };
        p.mode = scm::ThirdOctaveParams::Mode::Reference;
        p.order = order;
        const auto res = scm::thirdOctave (app.audioEngine(), p, &app.scale());
        CHECK (res.channels.size() == 2 && res.channels[0].relDb.size() == 27 && res.centresHz.size() == 27
               && res.fftSize == 65536 && res.framesUsed >= 1 && res.absUnit == "dB-SPL",
               "measures 1/3 oct: 27 reference bands with metadata");
        if (res.channels.size() != 2) continue;
        CHECK (res.channels[0].cal.calibrated
               && std::abs (res.channels[0].cal.levelOffsetDb - 20.0 * std::log10 (1.0 / 2e-5)) < 1e-9,
               "measures 1/3 oct: per-channel calibration attached (93.98 dB at 1 Pa)");
        const bool same = scRecomputeUntil (view, [&]
        {
            const auto& b = view.getThirdOctMags();
            return b.size() == 2 && b[0] == res.channels[0].relDb && b[1] == res.channels[1].relDb;
        });
        CHECK (same, "measures 1/3 oct: reference bands bit-identical to the panel (order " + juce::String (order) + ")");
        CHECK (FFTAnalysisView::thirdOctBandsRef (app.audioEngine(), 0, 0.1, 1.9, order) == res.channels[0].relDb,
               "measures 1/3 oct: FFTAnalysisView::thirdOctBandsRef forwards to the module");
    }
    wav.deleteFile();
}

// Same reference as the existing dB-SPL test: a full-scale bin-exact tone at
// 1 Pa reads ~91 dB-SPL in the 1 kHz band through the module.
static void testMeasThirdOctaveDbSpl()
{
    progress ("measures: 1/3 octave dB-SPL reference value");
    const double rate = 48000.0;
    const double fTone = 1365.0 * rate / 65536.0;
    AudioEngine eng;
    CHECK (measInitEngine (eng, rate, { measSine (rate, 96000, fTone, 1.0) }), "measures dB-SPL: engine");
    ScaleCalibration sc;
    sc.unit = ScaleCalibration::Unit::Pascal;
    sc.fullScaleValue = 1.0;
    scm::ThirdOctaveParams p;
    p.t0 = 0.0; p.t1 = 2.0; p.channels = { 0 };
    const auto res = scm::thirdOctave (eng, p, &sc);
    int k1k = -1;
    for (int b = 0; b < (int) res.centresHz.size(); ++b)
        if (std::abs (res.centresHz[(size_t) b] - 1000.0) < 1e-9) k1k = b;
    CHECK (res.channels.size() == 1 && k1k >= 0, "measures dB-SPL: 1 kHz band present");
    if (res.channels.size() == 1 && k1k >= 0)
    {
        const double absDb = (double) res.channels[0].relDb[(size_t) k1k] + res.channels[0].cal.levelOffsetDb;
        CHECK (std::abs (absDb - 91.0) < 1.0, "measures dB-SPL: full-scale tone reads ~91 dB-SPL, got " + juce::String (absDb, 2));
        CHECK (!res.channels[0].noData[(size_t) k1k], "measures dB-SPL: tone band has data");
    }

    // Spectrum convention: the full-scale tone reads -3.01 dB = 90.97 dB-SPL on its bin.
    scm::SpectrumParams sp;
    sp.t0 = 0.0; sp.t1 = 2.0; sp.channels = { 0 }; sp.fftSize = 65536; sp.windowType = scm::Hann;
    const auto spec = scm::spectrum (eng, sp, &sc);
    CHECK (spec.channels.size() == 1 && spec.absUnit == "dB-SPL", "measures dB-SPL: spectrum computed");
    if (spec.channels.size() == 1)
    {
        const double rel = spec.channels[0].relDb[1365];
        CHECK (std::abs (rel - (-3.0103)) < 0.05, "measures dB-SPL: full-scale sine bin reads -3.01 dB, got " + juce::String (rel, 3));
        CHECK (std::abs (rel + spec.channels[0].cal.levelOffsetDb - 90.97) < 0.05,
               "measures dB-SPL: and 90.97 dB-SPL at 1 Pa");
    }
}

static void testMeasCurvesMatchPanel()
{
    progress ("measures: curves == FFT panel cache");
    const juce::File wav = measWriteStereoFixture ("meas_curves.wav");
    Application app;
    CHECK (app.openFile (wav), "measures curves: open fixture");
    app.visibleChannels = { 0, 1 };
    app.setSelection ({ 0.25, 1.75 });
    const std::vector<int> chans { 0, 1 };

    for (int mode : { 3, 4, 5 })
        for (int track : { 0, 1, 2 })
        {
            FFTAnalysisView view (app);
            view.setSize (320, 200);
            view.setAxisMode (mode);
            view.smoothComboRef().setSelectedId (1, juce::sendNotificationSync);   // no smoothing
            view.trackComboRef().setSelectedId (track + 1, juce::sendNotificationSync);
            CHECK (view.trackMode() == track && view.smoothingWindow() == 0, "measures curves: options set");

            std::vector<scm::CurveData> out;
            const bool ok = (mode == 5)
                ? scm::spectrumCurve (app.audioEngine(), chans, 0.25, 1.75, scm::kTimeModeFFT, scm::Hann,
                                      track, nullptr, out)
                : scm::timeCurve (app.audioEngine(), chans, 0.25, 1.75, track, nullptr, out);
            CHECK (ok && out.size() == 2 && !out[0].x.empty(), "measures curves: module computed");
            const bool same = scRecomputeUntil (view, [&]
            {
                return view.curveCacheValid() && measSameCurves (view.getLiveCurves(), out);
            });
            CHECK (same, "measures curves: live curves bit-identical to the module (mode "
                   + juce::String (mode) + ", track " + juce::String (track) + ")");
        }

    // Smoothing goes through the same helper.
    std::vector<float> a { 0, 0, 3, 0, 0, 6, 0 }, b = a;
    scm::smoothSeries (a, 3);
    FFTAnalysisView::smoothSeries (b, 3);
    CHECK (a == b && std::abs (a[2] - 1.0f) < 1e-6f, "measures curves: smoothSeries shared");
    wav.deleteFile();
}

// Harmonics table of the panel (Copy Harmonics) now reads through the module.
static void testMeasHarmonicsTable()
{
    progress ("measures: harmonics table");
    const double rate = 48000.0;
    const int n = 4096;
    const double f0 = 10.0 * rate / n;   // bin-exact fundamental (117.19 Hz)
    const juce::File wav = makeTempFile ("meas_harm_table.wav");
    writeWav (wav, 1, rate, 48000, [f0] (int, int64_t f, double r)
    {
        double v = 0.0;
        for (int k = 1; k <= 4; ++k)
            v += (0.4 / k) * std::sin (2.0 * juce::MathConstants<double>::pi * k * f0 * (double) f / r);
        return (float) v;
    });
    Application app;
    CHECK (app.openFile (wav), "measures harmonics table: open");
    app.setSelection ({ 0.1, 0.9 });
    FFTAnalysisView view (app);
    view.setSize (320, 200);
    view.setAxisMode (1);
    view.setFftSize (n);
    view.setWindowType (1);
    CHECK (scRecomputeUntil (view, [&] { return view.getCachedN() == n && !view.getSpectrum().empty(); }),
           "measures harmonics table: spectrum computed");
    view.setHarmonicReference (2, 2.0 * f0, 10);   // anchor = harmonic #2
    const auto lines = juce::StringArray::fromLines (view.buildHarmonicsTable());
    CHECK (lines.size() == 3 && lines[0] == "Harmonic\tFrequency\tAmplitude", "measures harmonics table: header + N rows");
    for (int k = 1; k <= 2 && k < lines.size(); ++k)
    {
        juce::StringArray cells;
        cells.addTokens (lines[k], "\t", "");
        const double expect = 20.0 * std::log10 ((0.4 / k) / std::sqrt (2.0));
        CHECK (cells.size() == 3 && std::abs (cells[1].getDoubleValue() - k * f0) < 1e-3
               && std::abs (cells[2].getDoubleValue() - expect) < 0.05,
               "measures harmonics table: row " + juce::String (k) + " = " + lines[k]);
    }
    wav.deleteFile();
}

// ------------------------------------------------------------ known signals

static void testMeasLevels()
{
    progress ("measures: levels (RMS / peak / crest / dB-SPL)");
    const double rate = 48000.0;
    AudioEngine eng;
    // ch0: full-scale 1 kHz sine; ch1: 0.5 sine + 0.1 DC; ch2: silence.
    auto s0 = measSine (rate, 48000, 1000.0, 1.0);
    auto s1 = measSine (rate, 48000, 1000.0, 0.5);
    for (auto& v : s1) v += 0.1f;
    CHECK (measInitEngine (eng, rate, { s0, s1, std::vector<float> (48000, 0.0f) }), "measures levels: engine");

    ScaleCalibration sc;
    sc.unit = ScaleCalibration::Unit::Pascal;
    sc.channelFullScale = { 1.0, 2.0, 1.0 };   // per-channel ("bkdk") full scales
    scm::LevelsParams p;
    p.t0 = 0.0; p.t1 = 1.0;
    const auto r = scm::levels (eng, p, &sc);
    CHECK (r.channels.size() == 3 && r.s0 == 0 && r.s1 == 48000 && r.unit == "dBFS" && r.absUnit == "dB-SPL"
           && r.physicalUnit == "Pa", "measures levels: three channels, units");
    if (r.channels.size() == 3)
    {
        const auto& a = r.channels[0];
        CHECK (std::abs (a.peakDb - 0.0) < 1e-6 && std::abs (a.rmsDb - (-3.0103)) < 0.01,
               "measures levels: full-scale sine peak 0 dBFS, RMS -3.01 dBFS (" + juce::String (a.rmsDb, 3) + ")");
        CHECK (std::abs (a.crestDb - 3.0103) < 0.01, "measures levels: sine crest factor 3.01 dB");
        CHECK (std::abs (a.rmsDb - (a.peakDb - 3.0103)) < 0.01, "measures levels: sine RMS = peak - 3.01 dB");
        CHECK (std::abs (a.rmsAbsDb - 90.97) < 0.02, "measures levels: 1 Pa full-scale sine = 90.97 dB-SPL, got "
               + juce::String (a.rmsAbsDb, 3));
        CHECK (a.clipped > 0 && std::abs (a.dcOffset) < 1e-6 && a.samples == 48000,
               "measures levels: full-scale peaks counted as clipped, no DC");
        CHECK (std::abs (a.peakAtSec - 0.25 / 1000.0) < 1.0 / rate, "measures levels: first peak at 0.25 ms");

        const auto& b = r.channels[1];
        const double rmsB = std::sqrt (0.125 + 0.01);   // sine power + DC power
        CHECK (std::abs (b.rms - rmsB) < 1e-4 && std::abs (b.dcOffset - 0.1) < 1e-4 && b.clipped == 0,
               "measures levels: RMS includes the DC offset, reported separately");
        CHECK (std::abs (b.peak - 0.6) < 1e-5, "measures levels: peak = 0.5 + 0.1");
        CHECK (b.cal.calibrated && std::abs (b.cal.fullScale - 2.0) < 1e-12
               && std::abs (b.rmsAbsDb - (b.rmsDb + 20.0 * std::log10 (2.0 / 2e-5))) < 1e-9
               && std::abs (b.rmsPhysical - b.rms * 2.0) < 1e-12,
               "measures levels: per-channel full scale used for absolute levels");

        const auto& c = r.channels[2];
        CHECK (c.rmsDb == -300.0 && c.peakDb == -300.0 && c.crestDb == 0.0 && c.rmsAbsDb == -300.0,
               "measures levels: digital silence reads the -300 floor");
    }

    // Sub-range, explicit channel list, uncalibrated.
    scm::LevelsParams q;
    q.t0 = 0.25; q.t1 = 0.5; q.channels = { 1, 7 };
    const auto r2 = scm::levels (eng, q, nullptr);
    CHECK (r2.channels.size() == 1 && r2.channels[0].ch == 1 && r2.s0 == 12000 && r2.s1 == 24000
           && r2.absUnit.empty() && !r2.channels[0].cal.calibrated && r2.channels[0].rmsAbsDb == -300.0,
           "measures levels: sub-range, invalid channel skipped, uncalibrated");

    // Level curve: 0.5 sine then 0.1 sine.
    AudioEngine eng2;
    auto step = measSine (rate, 48000, 1000.0, 0.5);
    for (size_t i = 24000; i < step.size(); ++i) step[i] *= 0.2f;
    CHECK (measInitEngine (eng2, rate, { step }), "measures level curve: engine");
    const auto lc = scm::levelCurve (eng2, 0, 0.0, 1.0, 0.05, 0.01, scm::LevelKind::Rms);
    CHECK (lc.valuesDb.size() == 96 && std::abs (lc.hopSec - 0.01) < 1e-12 && std::abs (lc.t0 - 0.025) < 1e-12,
           "measures level curve: 96 points, hop 10 ms, first centre 25 ms (" + juce::String ((int) lc.valuesDb.size()) + ")");
    if (lc.valuesDb.size() == 96)
        CHECK (std::abs (lc.valuesDb[5] - (-9.0309)) < 0.02 && std::abs (lc.valuesDb[90] - (-23.0103)) < 0.02,
               "measures level curve: RMS follows the step (-9.03 then -23.01 dBFS)");
    const auto pc = scm::levelCurve (eng2, 0, 0.0, 1.0, 0.05, 0.01, scm::LevelKind::Peak);
    CHECK (pc.valuesDb.size() == 96 && std::abs (pc.valuesDb[5] - 20.0 * std::log10 (0.5)) < 0.01,
           "measures level curve: peak kind");
}

static void testMeasPeaks()
{
    progress ("measures: spectral peaks");
    const double rate = 48000.0;
    const double fA = 1000.0, fB = 2750.3;   // off-bin tones
    auto x = measSine (rate, 48000, fA, 0.5);
    const auto y = measSine (rate, 48000, fB, 0.1, 0.7);
    juce::Random rng (99);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] += y[i] + 1e-4f * (rng.nextFloat() - 0.5f);
    AudioEngine eng;
    CHECK (measInitEngine (eng, rate, { x }), "measures peaks: engine");

    scm::SpectrumParams sp;
    sp.t0 = 0.0; sp.t1 = 1.0; sp.channels = { 0 }; sp.fftSize = 4096; sp.windowType = scm::Hann;
    const auto spec = scm::spectrum (eng, sp);
    scm::PeakParams pp;
    pp.maxCount = 5;
    pp.minProminenceDb = 10.0;
    const auto all = scm::peaks (spec, pp);
    CHECK (all.size() == 1 && all[0].peaks.size() >= 2, "measures peaks: two tones found");
    if (all.size() == 1 && all[0].peaks.size() >= 2)
    {
        const auto& pk = all[0].peaks;
        const double halfBin = 0.5 * spec.binHz;
        CHECK (std::abs (pk[0].freqHz - fA) < halfBin && std::abs (pk[1].freqHz - fB) < halfBin,
               "measures peaks: strongest two at " + juce::String (pk[0].freqHz, 2) + " / "
               + juce::String (pk[1].freqHz, 2) + " Hz (within half a bin)");
        CHECK (std::abs (pk[0].relDb - 20.0 * std::log10 (0.5 / std::sqrt (2.0))) < 0.5
               && std::abs (pk[1].relDb - 20.0 * std::log10 (0.1 / std::sqrt (2.0))) < 0.5,
               "measures peaks: interpolated levels within 0.5 dB of the tones' RMS");
        CHECK (pk[0].prominenceDb >= pk[1].prominenceDb && pk[1].prominenceDb > 40.0,
               "measures peaks: sorted by prominence");
        CHECK (pk[0].absDb == pk[0].relDb, "measures peaks: uncalibrated absDb = relDb");
    }

    // Frequency window and maxCount.
    pp.minFreqHz = 2000.0; pp.maxCount = 1;
    const auto win = scm::peaks (spec, pp);
    CHECK (win.size() == 1 && win[0].peaks.size() == 1 && std::abs (win[0].peaks[0].freqHz - fB) < spec.binHz,
           "measures peaks: minFreqHz / maxCount respected");

    // Pure helper on a hand-made spectrum: plateau and prominence.
    const std::vector<double> v { -100, -50, -80, -60, -60, -90, -10, -100, -100 };
    scm::PeakParams all0;
    all0.minProminenceDb = 0.0; all0.maxCount = 0;
    const auto hp = scm::findPeaks (v, 16.0, 16, all0);
    CHECK (hp.size() == 3 && hp[0].bin == 6 && std::abs (hp[0].prominenceDb - 90.0) < 1e-9
           && hp[1].bin == 1 && std::abs (hp[1].prominenceDb - 40.0) < 1e-9
           && hp[2].bin == 3 && std::abs (hp[2].prominenceDb - 20.0) < 1e-9,
           "measures peaks: prominence and plateau handling on a synthetic spectrum");
}

static void testMeasHarmonics()
{
    progress ("measures: harmonics");
    const double rate = 48000.0;
    const double f0 = 441.7;
    std::vector<float> x ((size_t) 48000, 0.0f);
    for (int k = 1; k <= 6; ++k)
    {
        const auto h = measSine (rate, 48000, k * f0, 0.5 / k, 0.3 * k);
        for (size_t i = 0; i < x.size(); ++i) x[i] += h[i];
    }
    AudioEngine eng;
    CHECK (measInitEngine (eng, rate, { x }), "measures harmonics: engine");
    ScaleCalibration sc;
    sc.unit = ScaleCalibration::Unit::Volts;
    sc.fullScaleValue = 10.0;
    scm::SpectrumParams sp;
    sp.t0 = 0.0; sp.t1 = 1.0; sp.channels = { 0 }; sp.fftSize = 8192; sp.windowType = scm::Hann;
    const auto spec = scm::spectrum (eng, sp, &sc);
    const double halfBin = 0.5 * spec.binHz;

    scm::HarmonicsParams hp;
    hp.maxK = 8;
    const auto r = scm::harmonics (spec, 0, hp);
    CHECK (r.autoDetected && std::abs (r.fundamentalHz - f0) < halfBin,
           "measures harmonics: fundamental auto-detected at " + juce::String (r.fundamentalHz, 2) + " Hz");
    CHECK (r.harmonics.size() == 8, "measures harmonics: 8 harmonics reported");
    bool levelsOk = true, freqOk = true;
    for (int k = 1; k <= 6 && k <= (int) r.harmonics.size(); ++k)
    {
        const auto& h = r.harmonics[(size_t) k - 1];
        if (!h.valid || std::abs (h.freqHz - k * f0) > halfBin) freqOk = false;
        if (std::abs (h.relDb - 20.0 * std::log10 ((0.5 / k) / std::sqrt (2.0))) > 0.5) levelsOk = false;
        if (std::abs (h.absDb - (h.relDb + 20.0)) > 1e-9) levelsOk = false;   // 10 V full scale = +20 dBV
    }
    CHECK (freqOk, "measures harmonics: harmonics 1..6 found within half a bin");
    CHECK (levelsOk, "measures harmonics: harmonic levels within 0.5 dB, dBV offset applied");
    if (r.harmonics.size() == 8)
        CHECK (r.harmonics[6].relDb < r.harmonics[5].relDb - 40.0, "measures harmonics: absent 7th harmonic far below");
    const double thd = 100.0 * std::sqrt (1.0 / 4 + 1.0 / 9 + 1.0 / 16 + 1.0 / 25 + 1.0 / 36);
    CHECK (std::abs (r.thdPercent - thd) < 2.0, "measures harmonics: THD ~" + juce::String (thd, 1)
           + " %, got " + juce::String (r.thdPercent, 1));

    // Anchor form (the panel's "anchor is harmonic #N") and above-Nyquist handling.
    scm::HarmonicsParams ha;
    ha.anchorHz = 3.0 * f0; ha.anchorN = 3; ha.maxK = 60;
    const auto ra = scm::harmonics (spec, 0, ha);
    CHECK (!ra.autoDetected && std::abs (ra.fundamentalHz - f0) < 1e-9 && ra.harmonics.size() == 60
           && ra.harmonics[53].valid && !ra.harmonics[54].valid,
           "measures harmonics: anchor/N fundamental, harmonics above Nyquist flagged invalid");

    // NearestBin: the bin nearest to k * f0, level of that bin.
    scm::HarmonicsParams hn;
    hn.fundamentalHz = f0; hn.maxK = 2; hn.search = scm::HarmonicsParams::Search::NearestBin;
    const auto rn = scm::harmonics (spec, 0, hn);
    const int nb = (int) std::lround (2.0 * f0 * 8192 / rate);
    CHECK (rn.harmonics.size() == 2 && rn.harmonics[1].bin == nb
           && rn.harmonics[1].relDb == spec.channels[0].relDb[(size_t) nb] && rn.harmonics[1].freqHz == 2.0 * f0,
           "measures harmonics: NearestBin reads the nearest bin");
}

static void testMeasCancelAndThreads()
{
    progress ("measures: cancellation, progress, worker thread");
    const double rate = 48000.0;
    AudioEngine eng;
    CHECK (measInitEngine (eng, rate, { measSine (rate, 480000, 1000.0, 0.5), measSine (rate, 480000, 500.0, 0.25) }),
           "measures cancel: engine");

    int calls = 0;
    double last = -1.0;
    bool monotonic = true;
    scm::SpectrumParams sp;
    sp.t0 = 0.0; sp.t1 = 10.0; sp.channels = { 0, 1 }; sp.fftSize = 128;   // 3750 windows > cap
    const auto full = scm::spectrum (eng, sp, nullptr, [&] (double f)
    {
        if (f < last) monotonic = false;
        last = f; ++calls;
        return true;
    });
    CHECK (!full.cancelled && full.channels.size() == 2 && calls > 10 && monotonic && last == 1.0,
           "measures cancel: progress reported monotonically up to 1");
    CHECK (full.framesAveraged == 2048 && full.framesCapped, "measures cancel: window cap reported");

    int n = 0;
    const auto cancelled = scm::spectrum (eng, sp, nullptr, [&n] (double) { return ++n < 5; });
    CHECK (cancelled.cancelled && cancelled.channels.empty(), "measures cancel: spectrum cancelled");

    scm::LevelsParams lp;
    const auto lc = scm::levels (eng, lp, nullptr, [] (double f) { return f < 0.3; });
    CHECK (lc.cancelled && lc.channels.empty(), "measures cancel: levels cancelled");

    CHECK (scm::thirdOctaveRefBands (eng, 0, 0.0, 10.0, 40, [] (double f) { return f < 0.5; }).empty(),
           "measures cancel: reference bands cancelled");
    scm::ThirdOctaveParams tp;
    tp.t0 = 0.0; tp.t1 = 10.0; tp.channels = { 0, 1 };
    int m = 0;
    const auto tc = scm::thirdOctave (eng, tp, nullptr, [&m] (double) { return ++m < 3; });
    CHECK (tc.cancelled && tc.channels.empty(), "measures cancel: third octave cancelled");
    std::vector<scm::CurveData> curves;
    CHECK (!scm::timeCurve (eng, { 0 }, 0.0, 10.0, 0, nullptr, curves, [] (double f) { return f < 0.5; })
           && curves.empty(), "measures cancel: time curve cancelled");

    // Same numbers from worker threads (readSamples + scfft are thread-safe).
    sp.t1 = 2.0;
    const auto ref = scm::spectrum (eng, sp);
    scm::SpectrumResult a, b;
    std::thread ta ([&] { a = scm::spectrum (eng, sp); });
    std::thread tb ([&] { b = scm::spectrum (eng, sp); });
    ta.join(); tb.join();
    CHECK (a.channels.size() == 2 && b.channels.size() == 2 && a.channels[0].norm == ref.channels[0].norm
           && b.channels[1].norm == ref.channels[1].norm, "measures threads: worker results identical");

    // Fixed-overlap placement.
    scm::SpectrumParams so;
    so.t0 = 0.0; so.t1 = 1.0; so.channels = { 0 }; so.fftSize = 1024; so.overlap = 0.5;
    const auto ov = scm::spectrum (eng, so);
    CHECK (ov.framesAveraged == 1 + (48000 - 1024) / 512 && !ov.framesCapped,
           "measures overlap: 50% overlap gives " + juce::String (ov.framesAveraged) + " windows");
    if (ov.channels.size() == 1)
    {
        const int bin = (int) std::lround (1000.0 * 1024 / rate);
        double best = -300.0;
        for (int k = bin - 2; k <= bin + 2; ++k) best = std::max (best, ov.channels[0].relDb[(size_t) k]);
        CHECK (best > 20.0 * std::log10 (0.5 / std::sqrt (2.0)) - 1.5, "measures overlap: tone level preserved");
    }

    CHECK (scm::windowTypeFromName ("Hanning") == scm::Hann && scm::windowTypeFromName ("rect") == scm::Rectangular
           && scm::windowTypeFromName ("hamming") == scm::Hamming && scm::windowTypeFromName ("kaiser") == -1
           && juce::String (scm::windowName (scm::Hamming)) == "hamming",
           "measures: window names");
    CHECK (std::abs (scm::enbwBins (scm::Hann, 4096) - 1.5) < 1e-3 && std::abs (scm::enbwBins (scm::Rectangular, 64) - 1.0) < 1e-12,
           "measures: ENBW helper");
}

static void runRegression_measures()
{
    testMeasSpectrumMatchesPanel();
    testMeasThirdOctaveMatchesPanel();
    testMeasThirdOctaveDbSpl();
    testMeasCurvesMatchPanel();
    testMeasHarmonicsTable();
    testMeasLevels();
    testMeasPeaks();
    testMeasHarmonics();
    testMeasCancelAndThreads();
}
