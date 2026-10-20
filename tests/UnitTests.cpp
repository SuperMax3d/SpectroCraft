/**
 * SpectroCraft - Unit tests (simple built-in framework, no external deps)
 * Covers: FFT correctness, marker add/remove/edit, undo/redo,
 *         selection + zero-crossing logic, WAV generation.
 */

#include <JuceHeader.h>
#include <cstring>
#include <ctime>
#include "Application.h"
#include "AudioEngine.h"
#include "SpectralEngine.h"
#include "MarkerManager.h"
#include "UndoManager.h"
#include "Settings.h"
#include "PngWriter.h"
#include "SpectralView.h"
#include "WaveformView.h"
#include "Panels.h"
#include "ScrollBars.h"
#include "MainWindow.h"
#include "Resampler.h"
#include "FFTHelpers.h"
#include "AI/DfStft.h"   // pure DSP, no OpenVINO dependency
#include "ScttChunk.h"   // SCTT RIFF chunk + JSON (no AI dependency)
#if defined(SC_ENABLE_AI)
#include "AI/ModelManager.h"
#include "AI/WhisperPipeline.h"
#include "AI/OvRuntime.h"
#include "AI/Sha256.h"
#include "NoiseSuppression.h"
#endif
#include "Version.h"   // auto-generated: SC_VERSION_STRING (bumped on every build)
#include "CrashTrace.h"  // crash stack traces in the test binary
#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef max
#undef min
#endif
#include <cmath>
#include <vector>
#include <sstream>
#include <mutex>

static int failures = 0;
static int total = 0;

#define CHECK(cond, name) \
    do { ++total; if (!(cond)) { ++failures; std::cout << "FAIL: " << name << "\n"; } } while (0)

static void progress (const char* name)
{
    std::cerr << "[test] " << name << "\n";
}

static juce::File makeTempFile (const juce::String& name)
{
    static int counter = 0;
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("sc_test_" + juce::String (++counter) + "_" + name);
}

static float readOneSample (const AudioEngine& eng, int ch, int64_t pos)
{
    float v = 0.0f;
    eng.readSamples (ch, pos, 1, &v);
    return v;
}

// ---------------- WAV generation ----------------
static bool writeWav (const juce::File& path, int nchans, double rate, int64_t frames,
                      const std::function<float (int ch, int64_t f, double rate)>& fn)
{
    AudioEngine eng; // unused; just to have formats available
    // Write a minimal WAV by hand (PCM float32)
    const int bitsPerSample = 32;
    const int bytesPerSample = 4;
    const int blockAlign = nchans * bytesPerSample;
    const int64_t dataSize = frames * (int64_t) blockAlign;
    const int32_t byteRate = (int32_t) (rate * blockAlign);

    auto put32 = [] (std::vector<uint8_t>& v, uint32_t x)
    {
        v.push_back ((uint8_t) (x & 0xff)); v.push_back ((uint8_t) ((x >> 8) & 0xff));
        v.push_back ((uint8_t) ((x >> 16) & 0xff)); v.push_back ((uint8_t) ((x >> 24) & 0xff));
    };
    auto put16 = [] (std::vector<uint8_t>& v, uint16_t x)
    {
        v.push_back ((uint8_t) (x & 0xff)); v.push_back ((uint8_t) ((x >> 8) & 0xff));
    };
    auto putFloat = [&put32] (std::vector<uint8_t>& v, float f)
    {
        uint32_t u; std::memcpy (&u, &f, 4); put32 (v, u);
    };

    std::vector<uint8_t> d;
    d.insert (d.end(), { 'R', 'I', 'F', 'F' });
    put32 (d, (uint32_t) (36 + dataSize));
    d.insert (d.end(), { 'W', 'A', 'V', 'E' });
    d.insert (d.end(), { 'f', 'm', 't', ' ' });
    put32 (d, 16);
    put16 (d, 3); // format: IEEE float
    put16 (d, (uint16_t) nchans);
    put32 (d, (uint32_t) rate);
    put32 (d, (uint32_t) byteRate);
    put16 (d, (uint16_t) blockAlign);
    put16 (d, (uint16_t) bitsPerSample);
    d.insert (d.end(), { 'd', 'a', 't', 'a' });
    put32 (d, (uint32_t) dataSize);

    for (int64_t f = 0; f < frames; ++f)
        for (int ch = 0; ch < nchans; ++ch)
            putFloat (d, fn (ch, f, rate));

    path.deleteFile(); // FileOutputStream appends to existing files!
    juce::FileOutputStream out (path);
    if (out.failedToOpen())
        return false;
    return out.write (d.data(), (int) d.size());
}

// ---------------- FFT correctness ----------------
static void testFFT()
{
    const int N = 1024;
    // NOTE: FFT constructor takes the ORDER (log2 of size), not the size!
    juce::dsp::FFT fft ((int) std::log2 ((double) N));

    // sine at bin k should peak at bin k
    // JUCE needs a 2*N buffer: input in first half, output written from index 0
    const int k = 64;
    std::vector<float> data ((size_t) N * 2, 0.0f);
    for (int i = 0; i < N; ++i)
        data[i] = (float) std::sin (2.0 * juce::MathConstants<double>::pi * k * i / N);

    fft.performRealOnlyForwardTransform (data.data(), true);

    // find max bin
    int best = 0; double bestMag = -1;
    for (int b = 1; b < N / 2; ++b)
    {
        const double re = data[b * 2], im = data[b * 2 + 1];
        const double mag = re * re + im * im;
        if (mag > bestMag) { bestMag = mag; best = b; }
    }
    CHECK (best == k, "FFT peak bin matches sine frequency");
}

// ---------------- scfft (AVX2 dispatch) vs JUCE scalar FFT ----------------
static void testScFftVsJuce()
{
    const int N = 2048;
    const int order = (int) std::log2 ((double) N);

    // Deterministic pseudo-random input in [-1, 1)
    std::vector<float> a ((size_t) N * 2), b ((size_t) N * 2);
    uint32_t seed = 0x12345678;
    for (int i = 0; i < N; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        const float v = (float) ((seed >> 8) / 8388608.0 - 1.0);
        a[(size_t) i] = b[(size_t) i] = v;
    }

    scfft::realForward (a.data(), order);
    juce::dsp::FFT fft (order);
    fft.performRealOnlyForwardTransform (b.data(), true);

    // Magnitudes must match bin-for-bin (catches inverted transform direction,
    // wrong packing, or twiddle errors).
    double maxRelErr = 0.0;
    int worstBin = -1;
    for (int k = 0; k <= N / 2; ++k)
    {
        const double ma = std::hypot ((double) a[k * 2], (double) a[k * 2 + 1]);
        const double mb = std::hypot ((double) b[k * 2], (double) b[k * 2 + 1]);
        const double rel = std::abs (ma - mb) / std::max (1e-6, mb);
        if (rel > maxRelErr) { maxRelErr = rel; worstBin = k; }
    }
    if (maxRelErr >= 1e-3)
        std::cerr << "  scfft diag: worst bin=" << worstBin
                  << " a=({" << a[worstBin * 2] << "," << a[worstBin * 2 + 1] << "})"
                  << " b=({" << b[worstBin * 2] << "," << b[worstBin * 2 + 1] << "})\n";
    CHECK (maxRelErr < 1e-3, "scfft magnitudes match JUCE FFT bin-for-bin");

    // A pure tone must land on its own bin in the scfft output.
    const int kTone = 102; // 400 Hz at 8 kHz with N=2048 -> bin 102.4
    std::fill (a.begin(), a.end(), 0.0f);
    for (int i = 0; i < N; ++i)
        a[(size_t) i] = (float) std::sin (2.0 * juce::MathConstants<double>::pi * kTone * i / N);
    scfft::realForward (a.data(), order);

    int best = 0;
    double bestMag = -1.0;
    for (int b2 = 1; b2 < N / 2; ++b2)
    {
        const double m = std::hypot ((double) a[b2 * 2], (double) a[b2 * 2 + 1]);
        if (m > bestMag) { bestMag = m; best = b2; }
    }
    CHECK (std::abs (best - kTone) <= 1, "scfft tone peak on correct bin");
}

// ---------------- Markers ----------------
static void testMarkers()
{
    MarkerManager mm;
    const int a = mm.addPointMarker (5.0, "A");
    const int b = mm.addRegionMarker (10.0, 12.0, "B");
    CHECK (a == 1 && b == 2, "marker ids assigned");
    CHECK (mm.count() == 2, "two markers");
    CHECK (mm.markers()[0].start == 5.0, "sorted by start time");

    CHECK (mm.renameMarker (a, "Renamed"), "rename ok");
    CHECK (mm.moveMarker (b, 20.0), "move ok");
    const auto* m = mm.findNear (19.9, 0.5);
    CHECK (m && m->id == b, "findNear finds region");
    CHECK (mm.removeMarker (a), "remove ok");
    CHECK (mm.count() == 1, "count after remove");
    CHECK (!mm.removeMarker (999), "remove missing id fails");

    // sidecar round-trip
    const juce::File f = makeTempFile ("markers.wav");
    mm.saveSidecar (f);
    MarkerManager mm2;
    mm2.loadSidecar (f);
    CHECK (mm2.count() == 1 && mm2.markers()[0].id == b, "sidecar round-trip");
    f.deleteFile();
}

// ---------------- Undo/redo ----------------
static void testUndo()
{
    AudioEngine eng;
    ::UndoManager um;
    eng.setUndoManager (&um);

    // Build a small in-memory buffer via WAV generation
    const juce::File wav = makeTempFile ("undo.wav");
    writeWav (wav, 1, 1000.0, 1000, [] (int, int64_t f, double) { return (float) (f % 10) / 10.0f - 0.45f; });
    CHECK (eng.loadFile (wav), "load test wav");

    // silence [100,200)
    eng.silenceSamples ({ 100, 200 });
    CHECK (readOneSample (eng, 0, 150) == 0.0f, "silenced region is zero");
    CHECK (um.depth() == 1, "one undo level");

    eng.silenceSamples ({ 300, 400 });
    CHECK (um.depth() == 2, "two undo levels");

    Patch p;
    CHECK (um.popUndo (p), "pop undo");
    CHECK (eng.applyPatch (p, false), "apply undo restores data");
    CHECK (readOneSample (eng, 0, 350) != 0.0f, "restored sample non-zero");

    Patch p2;
    CHECK (um.popRedo (p2), "pop redo");
    CHECK (eng.applyPatch (p2, true), "apply redo");
    CHECK (readOneSample (eng, 0, 350) == 0.0f, "re-silenced");

    // Regression: per-sample edits (fade/gain/normalize/reverse) must capture
    // oldData BEFORE modifying the buffer in place, or undo is a no-op.
    const float pre = readOneSample (eng, 0, 450);   // untouched region
    CHECK (pre != 0.0f, "region has data before fade");
    eng.fadeInSamples ({ 400, 500 });
    const float faded = readOneSample (eng, 0, 450);
    CHECK (std::abs (faded - pre * (50.0f / 99.0f)) < 1e-4, "fade applied");

    Patch p3;
    CHECK (um.popUndo (p3), "pop fade undo");
    CHECK (eng.applyPatch (p3, false), "apply fade undo");
    CHECK (std::abs (readOneSample (eng, 0, 450) - pre) < 1e-4,
           "fade undo restores the original data");

    wav.deleteFile();
}

// ---------------- Selection + zero crossings ----------------
static void testZeroCrossing()
{
    AudioEngine eng;
    const juce::File wav = makeTempFile ("ramp.wav");
    // ramp crossing zero between samples 500 and 501
    writeWav (wav, 1, 1000.0, 1000, [] (int, int64_t f, double)
    {
        return (float) ((int) f * 2 - 1001) / 2000.0f;
    });
    CHECK (eng.loadFile (wav), "load ramp wav");

    const double snapped = eng.snapToZeroCrossing (0.52, 0.1);
    CHECK (std::abs (snapped - 0.5) < 0.002, "zero crossing snapped near 0.5s");

    // selection intersection (times in seconds; file is 1.0s long)
    juce::Range<double> sel (0.9, 1.5);
    const auto clipped = sel.getIntersectionWith ({ 0.0, eng.duration() });
    CHECK (!clipped.isEmpty(), "selection intersects duration");
    CHECK (std::abs (clipped.getEnd() - eng.duration()) < 1e-9, "selection clipped to duration");

    wav.deleteFile();
}

// ---------------- Spectral offline render ----------------
static void testSpectral()
{
    const juce::File wav = makeTempFile ("tone.wav");
    // 2s tone at 400 Hz, stereo
    writeWav (wav, 2, 4000.0, 8000, [] (int ch, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    AudioEngine eng;
    CHECK (eng.loadFile (wav), "load tone wav");
    CHECK (eng.numChannels() == 2, "stereo channels");

    SpectralEngine sp;
    Settings s;
    sp.setColourMap (ColourMap::Classic);

    const juce::File png = makeTempFile ("spectral.png");
    CHECK (sp.renderToPng (eng, s, wav, png, 256, 128, 0.0, 2.0), "render png");

    // Verify the PNG file exists and has a non-trivial size
    CHECK (png.existsAsFile() && png.getSize() > 1000, "png non-trivial size");

    // Verify a bright row exists near the 400 Hz band (log scale 20Hz..20kHz)
    // Just sanity: cache got populated
    CHECK (sp.cacheSize() > 0, "tile cache populated");

    sp.clearCache();
    CHECK (sp.cacheSize() == 0, "cache cleared");

    wav.deleteFile();
    png.deleteFile();
}

// ---------------- Spectral view headless paint ----------------
static void testSpectralViewPaint()
{
    const juce::File wav = makeTempFile ("view_tone.wav");
    // 2s mono tone at 400 Hz, 8 kHz
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.audioEngine().loadFile (wav), "load wav for view paint test");

    SpectralView view (app);
    view.setSize (256, 128);
    view.setViewRange (0.0, 2.0);
    view.setVisibleChannels ({ 0 });
    view.setSettings (app.settings);

    juce::Image img (juce::Image::ARGB, 256, 128, true);
    {
        juce::Graphics g (img);
        view.paint (g);
    }

    // The 400 Hz tone should produce a bright row at its log-scale position.
    // Note: the engine clamps fMax to Nyquist (rate/2), so for this 8 kHz file
    // the axis is 20 Hz .. 4000 Hz:
    const double rate = app.audioEngine().sampleRate();
    const double fMin = 20.0, fMax = std::min (rate * 0.5, 20000.0);
    const int expectedRow = (int) std::lround (128.0 * (1.0 - std::log (400.0 / fMin)
                                                        / std::log (fMax / fMin)));

    int brightestRow = -1, bestVal = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 70; x < 256; ++x) // skip the frequency-ruler text area
        {
            const auto c = img.getPixelAt (x, y);
            const int v = c.getRed() + c.getGreen() + c.getBlue();
            if (v > bestVal)
            {
                bestVal = v;
                brightestRow = y;
            }
        }

    CHECK (std::abs (brightestRow - expectedRow) <= 3,
           "400 Hz tone renders near expected log-scale row");
    CHECK (bestVal > 300, "tone row is bright (not just grid lines)");

    wav.deleteFile();
}

// ---------------- Waveform view headless paint (envelope regression) ----------------
static void testWaveformViewPaint()
{
    const juce::File wav = makeTempFile ("wave_view.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.audioEngine().loadFile (wav), "load wav for waveform view test");

    WaveformView view (app);
    view.setSize (256, 128);
    view.setViewRange (0.0, 2.0);
    view.setVisibleChannels ({ 0 });

    juce::Image img (juce::Image::ARGB, 256, 128, true);
    {
        juce::Graphics g (img);
        view.paint (g);
    }

    // Layout: ruler 24px; one channel row y=24..127, centre ~76.
    // Tone amplitude 0.5 with amp = chanH*0.45 => envelope spans roughly rows 53..99.
    auto isWave = [] (const juce::Colour& p) { return p.getGreen() > 150 && p.getRed() < 120; };

    const int cx = 128;
    bool aboveCentre = false, belowCentre = false;
    for (int y = 30; y < 76; ++y)   if (isWave (img.getPixelAt (cx, y))) aboveCentre = true;
    for (int y = 77; y < 124; ++y)  if (isWave (img.getPixelAt (cx, y))) belowCentre = true;

    CHECK (aboveCentre && belowCentre,
           "waveform envelope extends both above and below centre");

    // Near the top of the channel row there must be no waveform pixels
    bool topClear = true;
    for (int x = 0; x < 256; ++x)
        if (isWave (img.getPixelAt (x, 28))) { topClear = false; break; }
    CHECK (topClear, "no waveform pixels near channel-row top edge");

    wav.deleteFile();
}

// ---------------- Waveform per-sample zoom (high-zoom sample-level rendering) ----------------
// When zoomed in to <= ~4 samples per pixel column the view must draw the
// individual samples as a thin polyline instead of blocky min/max columns.
static void testWaveformPerSampleZoom()
{
    // Square wave: with a min/max envelope every column would be filled from
    // -0.5 to +0.5 (blocky); per-sample mode draws a thin line instead.
    const juce::File wav = makeTempFile ("wave_persample.wav");
    writeWav (wav, 1, 8000.0, 32000, [] (int, int64_t f, double)
    {
        return (f % 20 < 10) ? 0.5f : -0.5f;   // 400 Hz square wave at 8 kHz
    });

    Application app;
    CHECK (app.audioEngine().loadFile (wav), "load wav for per-sample zoom test");

    WaveformView view (app);
    view.setSize (256, 128);
    // Plot width = 256 - 60 (left axis) = 196 px; a 400-sample window gives
    // ~2 samples/px, below the per-sample threshold of 4.
    view.setViewRange (0.0, 0.05);
    view.setVisibleChannels ({ 0 });

    juce::Image img (juce::Image::ARGB, 256, 128, true);
    { juce::Graphics g (img); view.paint (g); }

    auto isWave = [] (const juce::Colour& p) { return p.getGreen() > 150 && p.getRed() < 120; };

    int wavePx = 0, totalPx = 0;
    for (int y = 24; y < 128; ++y)
        for (int x = 60; x < 256; ++x)
        {
            ++totalPx;
            if (isWave (img.getPixelAt (x, y))) ++wavePx;
        }

    CHECK (wavePx > 50, "per-sample zoom draws a waveform line");
    CHECK ((double) wavePx / totalPx < 0.10,
           "per-sample zoom is a thin line, not filled min/max columns");

    // The curve must pass through the expected sample position: window sample
    // 5 is +0.5 => y = centre - 0.5*amp = 76 - 23.4 ~= row 52/53 and
    // x = 60 + (5.5/400)*196 ~= column 63.
    bool foundPeak = false;
    for (int y = 48; y <= 58 && !foundPeak; ++y)
        for (int x = 60; x <= 67; ++x)
            if (isWave (img.getPixelAt (x, y))) { foundPeak = true; break; }
    CHECK (foundPeak, "per-sample curve passes through the expected sample point");

    wav.deleteFile();
}

// ---------------- FFT analysis panel headless paint (crash regression) ----------------
static void testFFTAnalysisViewPaint()
{
    const juce::File wav = makeTempFile ("fft_view.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.audioEngine().loadFile (wav), "load wav for fft view test");

    FFTAnalysisView view (app);
    view.setSize (320, 300);

    // Normal paint: must not crash and should produce a spectrum
    {
        juce::Image img (juce::Image::ARGB, 320, 300, true);
        juce::Graphics g (img);
        view.paint (g);
    }
    CHECK (true, "fft analysis view paints without crashing");

    // Tiny selection (< 64 samples): recompute bails out; paint must not crash
    app.setSelection ({ 0.0, 0.001 });
    {
        juce::Image img (juce::Image::ARGB, 320, 300, true);
        juce::Graphics g (img);
        view.paint (g);
    }
    CHECK (true, "fft analysis view paints with tiny selection");

    // Back to full range: must recover and paint again
    app.setSelection ({ 0.0, 2.0 });
    {
        juce::Image img (juce::Image::ARGB, 320, 300, true);
        juce::Graphics g (img);
        view.paint (g);
    }
    CHECK (true, "fft analysis view recovers after tiny selection");

    // Large FFT size: the curve loop and cursor readout must never index the
    // magnitude vector beyond n/2+1 entries (this used to read out of bounds
    // for x > half width and crash with large FFT sizes).
    app.settings.fftSize = 65536;
    app.settings.fftLocalN = 65536;   // analysis view uses its own persisted size
    {
        juce::Image img (juce::Image::ARGB, 320, 300, true);
        juce::Graphics g (img);
        view.paint (g); // draws the full curve across all bins
    }
    CHECK (true, "fft analysis view paints with 65536-point FFT");

    wav.deleteFile();
}

// ---------------- Markers: region from selection + channel toggles ----------------
static void testMarkersAndChannels()
{
    const juce::File wav = makeTempFile ("mk.wav");
    writeWav (wav, 2, 8000.0, 40000, [] (int ch, int64_t f, double rate) // 5s stereo
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * (400.0 + 200.0 * ch) * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open stereo wav for marker test");

    // With a selection: region marker spanning exactly the selection
    app.setSelection ({ 1.0, 3.5 });
    const int id = app.addMarkerAtCursor();
    const Marker* m = nullptr;
    for (const auto& mk : app.markers().markers())
        if (mk.id == id) { m = &mk; break; }
    CHECK (m != nullptr, "marker created at cursor");
    if (m)
    {
        CHECK (std::abs (m->start - 1.0) < 1e-9 && std::abs (m->end - 3.5) < 1e-9,
               "region marker spans the current selection");
    }

    // Without a selection: point marker at the playhead
    app.setSelection ({ 0, 0 });
    app.playhead = 2.25;
    const int id2 = app.addMarkerAtCursor();
    m = nullptr;
    for (const auto& mk : app.markers().markers())
        if (mk.id == id2) { m = &mk; break; }
    CHECK (m != nullptr && std::abs (m->start - 2.25) < 1e-9 && m->end < 0,
           "point marker at playhead when no selection");

    // Channel visibility: arbitrary subsets, not just first-N
    CHECK (app.isChannelVisible (0) && app.isChannelVisible (1), "both channels visible initially");
    app.toggleChannelVisible (0);
    CHECK (!app.isChannelVisible (0) && app.isChannelVisible (1), "hide channel 1 only");
    app.toggleChannelVisible (1);
    CHECK (!app.isChannelVisible (0) && !app.isChannelVisible (1), "all hidden");
    app.toggleChannelVisible (0);
    CHECK (app.isChannelVisible (0) && !app.isChannelVisible (1), "show channel 1 only");

    wav.deleteFile();
}

// ---------------- Marker panel: length<->end sync, TSV copy/paste ----------------
static void testMarkerPanelDetails()
{
    const juce::File wav = makeTempFile ("mkpanel.wav");
    // Start clean: openFile() auto-loads a sidecar if one exists, and the
    // temp-file counter can collide with leftovers from earlier runs.
    wav.deleteFile();
    juce::File (wav.getFullPathName() + ".markers.json").deleteFile();

    writeWav (wav, 1, 8000.0, 40000, [] (int, int64_t f, double rate) // 5s mono
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 440.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for marker panel test");

    const int rid = app.markers().addRegionMarker (1.0, 3.0, "RegionA");
    app.markers().addPointMarker (2.5, "PointB");

    MarkerListPanel panel (app);
    panel.setSize (300, 420);
    panel.refresh();

    // JUCE 9 has no getChildComponentWithName; scan the children by name.
    auto editorNamed = [&] (const juce::String& nm) -> juce::TextEditor*
    {
        for (int i = 0; i < panel.getNumChildComponents(); ++i)
            if (auto* te = dynamic_cast<juce::TextEditor*> (panel.getChildComponent (i)))
                if (te->getName() == nm) return te;
        return nullptr;
    };

    auto* lenEdit   = editorNamed ("marker-length-edit");
    auto* endEdit   = editorNamed ("marker-end-edit");
    CHECK (lenEdit != nullptr && endEdit != nullptr, "detail editors found by name");
    if (! lenEdit || ! endEdit) { wav.deleteFile(); return; }

    auto markerById = [&] (int id) -> const Marker*
    {
        for (const auto& mk : app.markers().markers())
            if (mk.id == id) return &mk;
        return nullptr;
    };

    // TextEditor::setText posts its change message asynchronously (and drops
    // it if no MessageManager exists yet), so create one up front and pump the
    // queue after each programmatic edit to run onTextChange synchronously.
    juce::MessageManager::getInstance();
    auto setAndPump = [] (juce::TextEditor& ed, const juce::String& t)
    {
        ed.setText (t, true);
        // Pump until the queue is quiescent; a single runDispatchLoopUntil(0)
        // pass can miss messages that only become visible on the next cycle.
        for (int i = 0; i < 8; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
    };

    // --- Length box shows the region length and drives the End time ---
    panel.selectRow (0); // sorted by start: RegionA(1.0), PointB(2.5)
    CHECK (lenEdit->getText() == "0:02.000", "length box shows 2s for [1,3] region");

    setAndPump (*lenEdit, "1.5");
    const Marker* m = markerById (rid);
    CHECK (m != nullptr && std::abs (m->end - 2.5) < 1e-9, "editing length moves the end");
    CHECK (endEdit->getText() == "0:02.500", "end box follows the new length");

    // --- Editing the End time updates the Length box (vice versa) ---
    setAndPump (*endEdit, "4");
    m = markerById (rid);
    CHECK (m != nullptr && std::abs (m->end - 4.0) < 1e-9, "editing the end keeps length in sync");
    CHECK (lenEdit->getText() == "0:03.000", "length box follows the new end");

    // --- Clearing the length turns a region marker into a point marker ---
    setAndPump (*lenEdit, "");
    m = markerById (rid);
    CHECK (m != nullptr && m->end < 0, "clearing length makes it a point marker");

    // --- Copy all markers: tab-separated fields, CRLF between rows ---
    app.markers().clear();
    const int r1 = app.markers().addRegionMarker (0.5, 2.5, "Alpha");
    app.markers().setComment (r1, "first line\nsecond line"); // flattened on copy
    app.markers().addPointMarker (3.0, "Beta");
    panel.refresh();

    panel.copyAllMarkers();
    const juce::String clip = juce::SystemClipboard::getTextFromClipboard();
    CHECK (clip == "Alpha\t0:00.500\t0:02.500\tfirst line second line\r\n"
                  "Beta\t0:03.000\t-\t",
           "copy produces tab/CRLF separated rows");

    // --- Paste markers from the clipboard ---
    const int before = (int) app.markers().count();
    juce::SystemClipboard::copyTextToClipboard (
        "Gamma\t0:01.250\t0:02.750\tpasted comment\r\nDelta\t0:03.500\t-\t");

    const int added = panel.pasteMarkersFromClipboard();
    CHECK (added == 2, "paste adds two markers");
    CHECK ((int) app.markers().count() == before + 2, "marker count grew by two");

    const Marker* g = nullptr;
    const Marker* d = nullptr;
    for (const auto& mk : app.markers().markers())
    {
        if (mk.name == "Gamma") g = &mk;
        if (mk.name == "Delta") d = &mk;
    }
    CHECK (g != nullptr && std::abs (g->start - 1.25) < 1e-9 && std::abs (g->end - 2.75) < 1e-9,
           "pasted region marker times");
    CHECK (g != nullptr && g->comment == "pasted comment", "pasted comment stored");
    CHECK (d != nullptr && d->end < 0 && std::abs (d->start - 3.5) < 1e-9, "pasted point marker");

    wav.deleteFile();
    juce::File (wav.getFullPathName() + ".markers.json").deleteFile();

    // Tear down the message manager created above so it doesn't leak at exit.
    delete juce::MessageManager::getInstance();
}

// ---------------- Marker panel layout: labels, aligned editors, multi-line comment ----------------
static void testMarkerPanelLayout()
{
    const juce::File wav = makeTempFile ("mklayout.wav");
    wav.deleteFile();
    juce::File (wav.getFullPathName() + ".markers.json").deleteFile();

    writeWav (wav, 1, 8000.0, 40000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 440.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for marker layout test");
    const int rid = app.markers().addRegionMarker (1.0, 3.0, "Alpha");
    app.markers().setComment (rid, "a comment\nwith two lines");

    MarkerListPanel panel (app);
    panel.setSize (280, 420);
    panel.refresh();

    auto editorNamed = [&] (const juce::String& nm) -> juce::TextEditor*
    {
        for (int i = 0; i < panel.getNumChildComponents(); ++i)
            if (auto* te = dynamic_cast<juce::TextEditor*> (panel.getChildComponent (i)))
                if (te->getName() == nm) return te;
        return nullptr;
    };

    auto* beginEdit   = editorNamed ("marker-begin-edit");
    auto* endEdit     = editorNamed ("marker-end-edit");
    auto* lengthEdit  = editorNamed ("marker-length-edit");
    auto* titleEdit   = editorNamed ("marker-title-edit");
    auto* commentEdit = editorNamed ("marker-comment-edit");

    CHECK (beginEdit && endEdit && lengthEdit && titleEdit && commentEdit,
           "all five detail editors exist");
    if (! beginEdit || ! endEdit || ! lengthEdit || ! titleEdit || ! commentEdit)
    {
        wav.deleteFile();
        juce::File (wav.getFullPathName() + ".markers.json").deleteFile();
        return;
    }

    // Begin/End/Length/Title editors share the same x and width.
    CHECK (beginEdit->getX() == endEdit->getX() && beginEdit->getX() == lengthEdit->getX()
           && beginEdit->getX() == titleEdit->getX(), "detail editors left-aligned");
    CHECK (beginEdit->getWidth() == endEdit->getWidth()
           && beginEdit->getWidth() == lengthEdit->getWidth()
           && beginEdit->getWidth() == titleEdit->getWidth(), "detail editors same width");

    // The comment editor is multi-line and stretches to the bottom of the panel.
    CHECK (commentEdit->isMultiLine(), "comment editor is multi-line");
    CHECK (commentEdit->getBottom() >= panel.getHeight() - 8,
          "comment editor extends to the panel bottom");
    CHECK (commentEdit->getHeight() > 60, "comment editor has room for several lines");

    // Rows are stacked in order: begin < end < length < title < comment.
    CHECK (beginEdit->getY() < endEdit->getY() && endEdit->getY() < lengthEdit->getY()
           && lengthEdit->getY() < titleEdit->getY() && titleEdit->getY() < commentEdit->getY(),
          "detail rows stacked in order");

    // Paint check: the label column (left of the editors) must contain bright text.
    const juce::Image img = panel.createComponentSnapshot (panel.getLocalBounds());
    int brightLabelPixels = 0;
    for (int y = beginEdit->getY() - 2; y < commentEdit->getY() + 16 && y < img.getHeight(); ++y)
        for (int x = 4; x < beginEdit->getX() - 2 && x < img.getWidth(); ++x)
            if (img.getPixelAt (x, y).getBrightness () > 0.35f)
                ++brightLabelPixels;
    CHECK (brightLabelPixels > 100,
           "Begin/End/Length/Title/Comment labels are painted");

    wav.deleteFile();
    juce::File (wav.getFullPathName() + ".markers.json").deleteFile();
}

// ---------------- History panel: highlight only covers the current row ----------------
static void testHistoryPanelPaint()
{
    const juce::File wav = makeTempFile ("histpaint.wav");
    wav.deleteFile();
    writeWav (wav, 1, 8000.0, 40000, [] (int, int64_t f, double rate) // 5s mono
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 440.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for history paint test");

    // Two recorded actions -> rows: 0 = Loaded, 1 = gain #1, 2 = gain #2 (current).
    app.setSelection ({ 0.5, 4.5 });
    app.changeGain (3.0f);
    app.changeGain (-1.0f);
    CHECK (app.undo().undoSize() == 2, "two actions recorded");

    HistoryPanel panel (app);
    panel.setSize (300, 200);
    panel.refresh();
    CHECK (panel.numRows() == 3, "three history rows");
    CHECK (panel.currentState() == 2, "current state is the last row");

    const juce::Image img = panel.createComponentSnapshot (panel.getLocalBounds());

    // Row geometry: header 24 px, then 20 px rows. Current row band = y 64..84.
    // NB: not named 'near' — windows.h defines it as an empty macro.
    auto colNear = [&] (int x, int y, int r, int g2, int b)
    {
        const juce::Colour c = img.getPixelAt (x, y);
        return std::abs ((int) c.getRed() - r) <= 16
            && std::abs ((int) c.getGreen() - g2) <= 16
            && std::abs ((int) c.getBlue() - b) <= 16;
    };

    // Regression: the current row's highlight used to be drawn with fillAll(),
    // which painted over the whole panel and hid every earlier row + the header.
    CHECK (colNear (290, 34, 45, 45, 48), "rows above the current one keep the plain background");
    CHECK (colNear (150, 12, 45, 45, 48), "header area is not covered by the highlight");
    CHECK (colNear (290, 74, 58, 64, 80), "current row band carries the dark grey-blue highlight");

    // Past rows must still show bright text.
    int bright = 0;
    for (int y = 25; y < 43; ++y)
        for (int x = 8; x < 292; ++x)
            if (img.getPixelAt (x, y).getBrightness () > 0.6f) ++bright;
    CHECK (bright > 100, "past row text is visible");

    wav.deleteFile();
}

// ---------------- Independent view windows / zoom levels ----------------
static void testIndependentViews()
{
    const juce::File wav = makeTempFile ("ind.wav");
    writeWav (wav, 1, 8000.0, 32000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    progress ("ind: openFile");
    Application app;
    CHECK (app.openFile (wav), "open wav for independent view test"); // 4s file

    progress ("ind: construct views");
    WaveformView wave (app);
    SpectralView spec (app);
    wave.setSize (300, 200);
    spec.setSize (300, 200);
    wave.setVisibleChannels ({ 0 });
    spec.setVisibleChannels ({ 0 });

    // Give each view a different time window: they must not interfere.
    wave.setViewRange (0.0, 1.0);
    spec.setViewRange (2.0, 4.0);
    CHECK (std::abs (wave.getViewStart() - 0.0) < 1e-9 && std::abs (wave.getViewEnd() - 1.0) < 1e-9,
           "waveform keeps its own window");
    CHECK (std::abs (spec.getViewStart() - 2.0) < 1e-9 && std::abs (spec.getViewEnd() - 4.0) < 1e-9,
           "spectral keeps its own window");

    // Zoom the waveform only: spectral window must stay put.
    wave.zoomHorizontalAtCenter (2.0); // zoom in by 2x around centre (0.5s)
    CHECK (wave.getViewEnd() - wave.getViewStart() < 1.0,
           "waveform zoomed in");
    CHECK (std::abs (spec.getViewStart() - 2.0) < 1e-9 && std::abs (spec.getViewEnd() - 4.0) < 1e-9,
           "spectral window unaffected by waveform zoom");

    // Vertical zoom is independent per view: amplitude for the waveform...
    wave.zoomVerticalBy (2.0);
    CHECK (true, "waveform vertical zoom applied without crash");
    // ...and frequency range for the spectral view.
    spec.resetFrequencyRange();
    const double fMinBefore = 20.0;
    spec.zoomFrequencyBy (1.5); // zoom in on frequency axis
    {
        // After zooming, the top row must correspond to a lower max frequency:
        const double rate = app.audioEngine().sampleRate();
        const double fTop = spec.yToFreq (0.0f, 200);
        CHECK (fTop < std::min (rate * 0.5, 20000.0) - 1.0,
               "spectral frequency axis zoomed in");
        (void) fMinBefore;
    }

    // Both views still paint headlessly with their independent state.
    progress ("ind: wave paint");
    {
        juce::Image img (juce::Image::ARGB, 300, 200, true);
        juce::Graphics g (img);
        wave.paint (g);
    }
    CHECK (true, "waveform paints with independent state");
    progress ("ind: spec paint");
    {
        juce::Image img (juce::Image::ARGB, 300, 200, true);
        juce::Graphics g (img);
        spec.paint (g);
    }
    CHECK (true, "spectral paints with independent state");

    progress ("ind: done");
    wav.deleteFile();
}

// ---------------- WAV marker chunks (RIFF cue + LIST/adtl) ----------------
// Locates a file inside tests/data/ by walking up from the executable.
static juce::File findTestDataFile (const juce::String& name)
{
    juce::File dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    for (int i = 0; i < 8 && dir != juce::File(); ++i)
    {
        const juce::File f = dir.getChildFile ("tests/data/" + name);
        if (f.existsAsFile())
            return f;
        dir = dir.getParentDirectory();
    }
    const juce::File cwd = juce::File::getCurrentWorkingDirectory().getChildFile ("tests/data/" + name);
    return cwd.existsAsFile() ? cwd : juce::File();
}

static void testScrollBars()
{
    // ---- Time scrollbar: track = whole file, thumb = view window.
    TimeScrollBar bar;
    bar.setSize (400, 18);
    bar.setDuration (10.0);
    double lastT0 = -1, lastT1 = -1;
    bool fired = false;
    bar.onChange = [&] (double t0, double t1) { lastT0 = t0; lastT1 = t1; fired = true; };

    // Window [2.5, 7.5] of a 10s file in a 400px track -> thumb x=100..300.
    bar.setWindow (2.5, 7.5);
    {
        const auto tr = bar.thumbRect();
        CHECK (tr.getX() == 100 && tr.getRight() == 300,
               "time bar: thumb spans the window");

        // Hit-test modes.
        CHECK (TimeScrollBar::hitTest (tr, 200, 9) == TimeScrollBar::Mode::Pan,
               "time bar: centre hit = pan");
        CHECK (TimeScrollBar::hitTest (tr, 101, 9) == TimeScrollBar::Mode::LeftEdge,
               "time bar: left-edge hit");
        CHECK (TimeScrollBar::hitTest (tr, 299, 9) == TimeScrollBar::Mode::RightEdge,
               "time bar: right-edge hit");
        CHECK (TimeScrollBar::hitTest (tr, 50, 9) == TimeScrollBar::Mode::None,
               "time bar: outside hit = jump");
    }

    // applyWindow pans and notifies.
    fired = false;
    bar.applyWindow (3.5, 8.5);
    CHECK (fired && std::abs (lastT0 - 3.5) < 1e-9 && std::abs (lastT1 - 8.5) < 1e-9,
           "time bar: applyWindow pans");

    // Out-of-range and too-small windows are rejected.
    fired = false;
    bar.applyWindow (-2.0, 3.0);
    CHECK (!fired, "time bar: out-of-range window rejected");
    const double minSpan = 8.0 / 400 * 10.0; // ~8px in time units
    fired = false;
    bar.applyWindow (5.0, 5.0 + minSpan * 0.5);
    CHECK (!fired, "time bar: too-small window rejected");

    // Jump centres the window on the target time and clamps at the start.
    fired = false;
    bar.jumpTo (0.5); // span=5 -> would be [-2,3] -> clamped to [0,5]
    CHECK (fired && std::abs (lastT0 - 0.0) < 1e-9 && std::abs (lastT1 - 5.0) < 1e-9,
           "time bar: jump clamps at the start");

    // ---- Value scrollbar: track = [rangeMin, rangeMax], max value at top.
    ValueScrollBar vbar;
    vbar.setSize (14, 400);
    vbar.setRange (0.0, 22050.0);
    double lastLo = -1, lastHi = -1;
    fired = false;
    vbar.onChange = [&] (double lo, double hi) { lastLo = lo; lastHi = hi; fired = true; };

    // Window [2000, 8000] -> thumb y ≈ 254.9..363.7.
    vbar.setWindow (2000.0, 8000.0);
    {
        const auto tr = vbar.thumbRect();
        CHECK (std::abs ((double) tr.getY() - 254.0) <= 1.0
                 && std::abs ((double) tr.getBottom() - 364.0) <= 1.0,
               "value bar: thumb spans the window");

        // Hit-test modes.
        CHECK (ValueScrollBar::hitTest (tr, 7, 256) == ValueScrollBar::Mode::TopEdge,
               "value bar: top-edge hit");
        CHECK (ValueScrollBar::hitTest (tr, 7, 361) == ValueScrollBar::Mode::BottomEdge,
               "value bar: bottom-edge hit");
        CHECK (ValueScrollBar::hitTest (tr, 7, 300) == ValueScrollBar::Mode::Pan,
               "value bar: centre hit = pan");
        CHECK (ValueScrollBar::hitTest (tr, 7, 10) == ValueScrollBar::Mode::None,
               "value bar: outside hit = jump");
    }

    // Jump near the top clamps at rangeMax.
    fired = false;
    vbar.jumpToValue (21498.75); // span=6000 -> would exceed rangeMax
    CHECK (fired && std::abs (lastHi - 22050.0) < 1e-9 && lastLo > 15000.0,
           "value bar: jump clamps at the top");

    // Out-of-range windows are rejected.
    fired = false;
    vbar.applyWindow (-100.0, 5000.0);
    CHECK (!fired, "value bar: out-of-range window rejected");
}

static void testWavMarkerChunks()
{
    const juce::File wav = makeTempFile ("markers_src.wav");
    writeWav (wav, 1, 8000.0, 8000, [] (int, int64_t f, double rate)
             { return (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * f / rate); });

    AudioEngine eng;
    MarkerManager mm;
    if (!eng.loadFile (wav)) { CHECK (false, "marker wav load"); return; }
    eng.setMarkersForSave (&mm.markers());

    const int id1 = mm.addPointMarker (0.25, "Alpha");            // 5 chars (odd)
    mm.setComment (id1, "first marker comment");                 // 20 chars (even)
    const int id2 = mm.addRegionMarker (1.0, 1.5, "Beta Region"); // 11 chars (odd)
    mm.setComment (id2, "region note");                           // 11 chars (odd)

    const juce::File outWav = makeTempFile ("markers_out.wav");
    CHECK (eng.saveFileAs (outWav), "wav save with markers");

    // Parse the RIFF structure back.
    std::vector<uint8_t> bytes;
    { juce::FileInputStream fs (outWav);
      if (!fs.failedToOpen())
      {
          bytes.resize ((size_t) fs.getTotalLength());
          fs.read (bytes.data(), (int) bytes.size());
      } }
    CHECK (bytes.size() > 44, "wav file readable");

    struct Chunk { const char* id; uint32_t size; size_t payload; };
    std::vector<Chunk> chunks;
    for (size_t off = 12; off + 8 <= bytes.size(); )
    {
        Chunk c { (const char*) &bytes[off], 0, off + 8 };
        std::memcpy (&c.size, &bytes[off + 4], 4);
        chunks.push_back (c);
        off += 8 + (((size_t) c.size + 1) & ~(size_t) 1); // pad to even boundary
    }

    auto findChunk = [&] (const char id[4]) -> const Chunk*
    {
        for (const auto& c : chunks)
            if (std::memcmp (c.id, id, 4) == 0) return &c;
        return nullptr;
    };

    // Legacy DAW layout: exactly fmt, data, cue, LIST in that order.
    CHECK (chunks.size() == 4, "exactly four chunks");
    if (chunks.size() == 4)
    {
        CHECK (std::memcmp (chunks[0].id, "fmt ", 4) == 0, "chunk order: fmt first");
        CHECK (std::memcmp (chunks[1].id, "data", 4) == 0, "chunk order: data second");
        CHECK (std::memcmp (chunks[2].id, "cue ", 4) == 0, "chunk order: cue third");
        CHECK (std::memcmp (chunks[3].id, "LIST", 4) == 0, "chunk order: LIST last");
    }

    uint32_t riffSize = 0;
    std::memcpy (&riffSize, &bytes[4], 4);
    CHECK ((uint64_t) riffSize + 8 == bytes.size(), "RIFF size matches file length");

    // cue chunk: u32 count + one 24-byte entry per marker.
    const Chunk* cue = findChunk ("cue ");
    CHECK (cue != nullptr, "cue chunk present");
    if (cue)
    {
        uint32_t numCues = 0;
        std::memcpy (&numCues, &bytes[cue->payload], 4);
        CHECK (numCues == 2, "two cue entries");
        CHECK (cue->size == 4 + 24 * 2, "cue chunk size = 4 + 24*N");

        uint32_t pos1 = 0, pos2 = 0;
        for (int i = 0; i < 2 && cue->payload + 4 + 24 * (i + 1) <= bytes.size(); ++i)
        {
            const uint8_t* e = &bytes[cue->payload + 4 + 24 * i];
            uint32_t idv = 0, posv = 0, soff = 0;
            std::memcpy (&idv, e, 4);
            std::memcpy (&posv, e + 4, 4);
            std::memcpy (&soff, e + 20, 4);
            CHECK (std::memcmp (e + 8, "data", 4) == 0, "cue fccChunk = 'data'");
            uint32_t cs = 0, bs = 0;
            std::memcpy (&cs, e + 12, 4); std::memcpy (&bs, e + 16, 4);
            CHECK (cs == 0 && bs == 0, "cue chunkStart/blockStart zero");
            CHECK (idv == (uint32_t) (i + 1), "cue id is the 1-based marker id");
            CHECK (soff == posv, "sampleOffset duplicates position");
            if (i == 0) pos1 = posv; else pos2 = posv;
        }
        CHECK (pos1 == 2000, "first cue at sample 2000 (0.25s @8k)");
        CHECK (pos2 == 8000, "second cue at sample 8000 (1.0s @8k)");
    }

    // LIST chunk: "adtl" then ltxt entries, labl titles, note comments.
    const Chunk* list = findChunk ("LIST");
    CHECK (list != nullptr, "LIST chunk present");
    if (list && bytes.size() >= list->payload + 4)
    {
        CHECK (std::memcmp (&bytes[list->payload], "adtl", 4) == 0, "adtl form type");

        size_t off = list->payload + 4;
        auto readLtxt = [&] (uint32_t expectId, uint32_t expectLen) -> bool
        {
            if (off + 28 > bytes.size()) return false;
            if (std::memcmp (&bytes[off], "ltxt", 4) != 0)
            { std::cout << "  expected ltxt at offset, got '" << juce::String ((const char*) &bytes[off]).toRawUTF8() << "'\n"; return false; }
            uint32_t sz = 0, idv = 0, len = 0;
            std::memcpy (&sz, &bytes[off + 4], 4);
            std::memcpy (&idv, &bytes[off + 8], 4);
            std::memcpy (&len, &bytes[off + 12], 4);
            if (sz != 20 || idv != expectId || len != expectLen
                || std::memcmp (&bytes[off + 16], "rgn ", 4) != 0)
            { std::cout << "  ltxt mismatch: sz=" << sz << " id=" << idv << " len=" << len << "\n"; return false; }
            off += 28;
            return true;
        };

        auto readText = [&] (const char expectId[4], uint32_t expectIdNum, const juce::String& expectText) -> bool
        {
            if (off + 16 > bytes.size()) return false;
            if (std::memcmp (&bytes[off], expectId, 4) != 0)
            { std::cout << "  expected '" << expectId << "', got '" << juce::String ((const char*) &bytes[off]).toRawUTF8() << "'\n"; return false; }
            uint32_t sz = 0, idv = 0;
            std::memcpy (&sz, &bytes[off + 4], 4);
            std::memcpy (&idv, &bytes[off + 8], 4);
            const int textLen = expectText.getNumBytesAsUTF8();
            if (sz != (uint32_t) (5 + textLen) || idv != expectIdNum)
            { std::cout << "  size/id mismatch: sz=" << sz << " (want " << 5 + textLen << ") id=" << idv << "\n"; return false; }
            const juce::String text ((const char*) &bytes[off + 12], textLen);
            if (text != expectText)
            { std::cout << "  text mismatch: '" << text.toRawUTF8() << "' vs '" << expectText.toRawUTF8() << "'\n"; return false; }
            off += 8 + 4 + (size_t) textLen + ((textLen % 2 == 0) ? 1 : 0) + 1;
            return true;
        };

        CHECK (readLtxt (1, 0), "ltxt #1: point marker, length 0");
        CHECK (readLtxt (2, 4000), "ltxt #2: region length 4000 samples (0.5s @8k)");
        CHECK (readText ("labl", 1, "Alpha"), "labl #1 = 'Alpha'");
        CHECK (readText ("labl", 2, "Beta Region"), "labl #2 = 'Beta Region'");
        CHECK (readText ("note", 1, "first marker comment"), "note #1 = comment text");
        CHECK (readText ("note", 2, "region note"), "note #2 = region comment");

        // The walk must land exactly at the end of the LIST payload.
        CHECK (off == list->payload + list->size, "adtl entries fill the LIST chunk exactly");
    }

    // The data chunk must still be intact and loadable.
    const Chunk* data = findChunk ("data");
    CHECK (data != nullptr && data->size == 8000 * 4, "data chunk size correct");

    // Round-trip: the saved file must reload cleanly with the extra chunks present.
    AudioEngine eng2;
    CHECK (eng2.loadFile (outWav), "saved wav reloads via JUCE");
    if (eng2.isFileLoaded())
        CHECK (eng2.numChannels() == 1 && eng2.numSamples() == 8000, "reloaded dimensions correct");

    wav.deleteFile();
    outWav.deleteFile();
}

// ---------------- Loading markers from WAV RIFF chunks ----------------
static void testLoadMarkersFromWav()
{
    // 1) A reference WAV exported by a classic DAW with one embedded region marker.
    const juce::File refWav = findTestDataFile ("tone_1s_mono_marker_correct.wav");
    if (refWav.existsAsFile())
    {
        MarkerManager mm;
        CHECK (mm.loadFromWavFile (refWav, 44100.0), "reference wav: markers recovered");
        CHECK (mm.count() == 1, "reference wav: one marker");
        if (mm.count() == 1)
        {
            const Marker& m = mm.markers().front();
            CHECK (m.name == "test", "reference wav: title 'test'");
            CHECK (m.comment == "comment", "reference wav: comment 'comment'");
            CHECK (std::abs (m.start - 8864.0 / 44100.0) < 1e-9, "reference wav: start sample exact");
            CHECK (std::abs (m.end - (8864.0 + 9085.0) / 44100.0) < 1e-9, "reference wav: region end exact");
        }
    }

    // A malformed RIFF must fail cleanly without crashing or inventing markers.
    const juce::File bad = findTestDataFile ("tone_1s_mono_marker_incorrect.wav");
    if (bad.existsAsFile())
    {
        MarkerManager mm;
        CHECK (!mm.loadFromWavFile (bad, 44100.0), "malformed wav: no markers recovered");
        CHECK (mm.count() == 0, "malformed wav: list stays empty");
    }

    // 2) Round-trip through our own writer: point + region markers with odd- and
    // even-length titles/comments.
    const juce::File wav = makeTempFile ("load_markers_src.wav");
    writeWav (wav, 1, 8000.0, 8000, [] (int, int64_t f, double rate)
             { return (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * f / rate); });

    AudioEngine eng;
    MarkerManager mm;
    if (!eng.loadFile (wav)) { CHECK (false, "round-trip wav load"); return; }
    eng.setMarkersForSave (&mm.markers());

    const int id1 = mm.addPointMarker (0.25, "Alpha");            // 5 chars (odd)
    mm.setComment (id1, "first marker comment");                 // 20 chars (even)
    const int id2 = mm.addRegionMarker (1.0, 1.5, "Beta Region"); // 11 chars (odd)
    mm.setComment (id2, "region note");                           // 11 chars (odd)

    const juce::File outWav = makeTempFile ("load_markers_out.wav");
    CHECK (eng.saveFileAs (outWav), "round-trip: wav save with markers");

    MarkerManager mm2;
    CHECK (mm2.loadFromWavFile (outWav, 8000.0), "round-trip: markers recovered");
    CHECK (mm2.count() == 2, "round-trip: two markers");
    if (mm2.count() == 2)
    {
        const auto& ms = mm2.markers(); // sorted by start time
        const Marker& a = ms[0], b = ms[1];
        CHECK (a.name == "Alpha" && a.comment == "first marker comment",
               "round-trip: point title+comment");
        CHECK (std::abs (a.start - 0.25) < 1e-9 && a.end < 0,
               "round-trip: point time + end<0");
        CHECK (b.name == "Beta Region" && b.comment == "region note",
               "round-trip: region title+comment");
        CHECK (std::abs (b.start - 1.0) < 1e-9 && std::abs (b.end - 1.5) < 1e-9,
               "round-trip: region times");
    }

    wav.deleteFile();
    outWav.deleteFile();
}

// Byte-exact check against a reference export from a classic DAW: load the
// reference file's audio, re-save it with our writer using identical markers,
// and verify every byte matches (the exporter appends extra private chunks
// after the LIST chunk, which we do not emit).
static void testMarkerWavByteExact()
{
    const juce::File ref = findTestDataFile ("tone_10s_stereo_correct.wav");
    if (!ref.existsAsFile())
    { std::cout << "  (skipped: reference file not found)\n"; return; }

    AudioEngine eng;
    MarkerManager mm;
    if (!eng.loadFile (ref)) { CHECK (false, "reference wav load"); return; }
    const double rate = eng.sampleRate();
    CHECK (rate == 44100.0 && eng.numChannels() == 2, "reference is 44.1k stereo");

    // Markers as stored in the reference file.
    struct RefMarker { int64_t start; int64_t length; juce::String name; juce::String comment; };
    const RefMarker refs[] = {
        { 24387,  39734, "Marker 01", "" },
        { 153203, 60903, "Marker 02", "" },
        { 273288, 52258, "Marker 033", "Comment" },
    };
    for (const auto& r : refs)
        mm.addRegionMarker ((double) r.start / rate, (double) (r.start + r.length) / rate, r.name);
    mm.setComment (3, refs[2].comment);
    eng.setMarkersForSave (&mm.markers());

    const juce::File outWav = makeTempFile ("marker_repro.wav");
    CHECK (eng.saveFileAs (outWav), "re-save with markers");

    std::vector<uint8_t> refBytes, ourBytes;
    auto readAll = [&] (const juce::File& f) -> std::vector<uint8_t>
    {
        std::vector<uint8_t> v;
        juce::FileInputStream fs (f);
        if (!fs.failedToOpen())
        {
            v.resize ((size_t) fs.getTotalLength());
            fs.read (v.data(), (int) v.size());
        }
        return v;
    };
    refBytes = readAll (ref);
    ourBytes = readAll (outWav);

    // Our file must end exactly where the reference's LIST chunk ends.
    size_t refListEnd = 0;
    for (size_t off = 12; off + 8 <= refBytes.size(); )
    {
        if (std::memcmp (&refBytes[off], "LIST", 4) == 0)
        {
            uint32_t sz = 0;
            std::memcpy (&sz, &refBytes[off + 4], 4);
            refListEnd = off + 8 + sz;
            break;
        }
        uint32_t sz = 0;
        std::memcpy (&sz, &refBytes[off + 4], 4);
        off += 8 + (((size_t) sz + 1) & ~(size_t) 1);
    }
    CHECK (refListEnd > 0, "reference LIST chunk located");
    if (refListEnd > 0)
    {
        CHECK (ourBytes.size() == refListEnd,
              "our file length equals reference up to end of LIST (ours=" + juce::String ((int) ourBytes.size())
              + " ref=" + juce::String ((int) refListEnd) + ")");
        if (ourBytes.size() == refListEnd)
        {
            // Skip bytes 4..7: the RIFF total size legitimately differs because
            // the exporter counts its extra private chunks in the total.
            size_t firstDiff = (size_t) -1;
            for (size_t i = 8; i < ourBytes.size(); ++i)
                if (ourBytes[i] != refBytes[i]) { firstDiff = i; break; }
            CHECK (firstDiff == (size_t) -1,
                  "byte-exact match with reference export" + (firstDiff == (size_t) -1 ? juce::String() : juce::String (" (first diff at byte " + juce::String ((int) firstDiff) + ")")));
        }
    }

    outWav.deleteFile();
}

// ---------------- Effects scoped to visible channels ----------------
static void testEffectChannelScope()
{
    const juce::File wav = makeTempFile ("scope_src.wav");
    // ch0: ascending ramp 0..1, ch1: constant 0.5
    writeWav (wav, 2, 8000.0, 8000, [] (int ch, int64_t f, double)
              { return ch == 0 ? (float) f / 7999.0f : 0.5f; });

    AudioEngine eng;
    ::UndoManager um;
    if (!eng.loadFile (wav)) { CHECK (false, "scope wav load"); return; }
    eng.setUndoManager (&um);

    // Silence only channel 0 in [2000, 4000).
    const std::vector<int> onlyCh0 = { 0 };
    eng.silenceSamples ({ 2000, 4000 }, &onlyCh0);
    CHECK (std::abs (readOneSample (eng, 0, 3000)) < 1e-6f, "silence zeroes selected channel");
    CHECK (std::abs (readOneSample (eng, 1, 3000) - 0.5f) < 1e-6f, "silence leaves other channel untouched");

    // Undo restores the silenced channel exactly.
    CHECK (um.canUndo(), "silence recorded in undo stack");
    Patch p;
    if (um.popUndo (p)) eng.applyPatch (p, false);
    CHECK (std::abs (readOneSample (eng, 0, 3000) - 3000.0f / 7999.0f) < 1e-5f, "undo restores silenced channel");

    // Normalize only channel 1: its peak (0.5) goes to 1.0; ch0 must be unchanged.
    const std::vector<int> onlyCh1 = { 1 };
    eng.normalizeSamples ({ 0, 8000 }, 1.0f, &onlyCh1);
    CHECK (std::abs (readOneSample (eng, 1, 100) - 1.0f) < 1e-5f, "normalize scales selected channel to peak");
    CHECK (std::abs (readOneSample (eng, 0, 100) - 100.0f / 7999.0f) < 1e-6f, "normalize leaves other channel untouched");

    // Reverse only channel 0: ramp becomes a descending ramp; ch1 unchanged.
    eng.reverseSamples ({ 0, 8000 }, &onlyCh0);
    CHECK (std::abs (readOneSample (eng, 0, 0) - 7999.0f / 7999.0f) < 1e-5f, "reverse flips selected channel");
    CHECK (std::abs (readOneSample (eng, 0, 7999)) < 1e-6f, "reversed ramp ends at zero");
    CHECK (std::abs (readOneSample (eng, 1, 4000) - 1.0f) < 1e-5f, "reverse leaves other channel untouched");

    wav.deleteFile();
}

// ---------------- Test file generation ----------------
bool scGenerateTestFiles (const juce::String& dir)
{
    const juce::File base (dir);
    if (!base.exists())
        base.createDirectory();
    struct Spec { const char* name; int nchans; double secs; double rate; int kind; };
    const Spec specs[] =
    {
        { "tone_1s_mono.wav", 1, 1.0, 44100, 0 },
        { "tone_10s_stereo.wav", 2, 10.0, 44100, 0 },
        { "sweep_10s_mono.wav", 1, 10.0, 44100, 1 },
        { "noise_10s_8ch.wav", 8, 10.0, 44100, 2 },
        { "tone_1h_32ch.wav", 32, 3600.0, 96000, 0 },
    };

    for (const auto& sp : specs)
    {
        const juce::File out = base.getChildFile (sp.name);
        const int64_t frames = (int64_t) (sp.secs * sp.rate);
        // For the 1h/32ch/96k file, generate a sparse header-only stub (data size
        // declared but not written) so tests can exercise the streaming path
        // without multi-GB disk usage.
        const bool stub = (sp.nchans == 32 && sp.secs >= 3600.0);

        auto fn = [&] (int ch, int64_t f, double rate) -> float
        {
            if (sp.kind == 0) // tone: different pitch per channel
                return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * (400.0 + 200.0 * ch) * f / rate);
            if (sp.kind == 1) // frequency sweep
            {
                const double freq = 100.0 + (double) f / (double) frames * 5000.0;
                return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * f / rate);
            }
            // noise
            const uint32_t x = (uint32_t) (f * 2654435761u + ch * 97u);
            return ((float) (x % 1000) / 500.0f) - 1.0f;
        };

        if (stub)
        {
            // Write header with correct sizes, then truncate data (sparse)
            const int blockAlign = sp.nchans * 4;
            const int64_t dataSize = frames * (int64_t) blockAlign;
            std::vector<uint8_t> d;
            auto put32 = [] (std::vector<uint8_t>& v, uint32_t x)
            {
                v.push_back (x & 0xff); v.push_back ((x >> 8) & 0xff);
                v.push_back ((x >> 16) & 0xff); v.push_back ((x >> 24) & 0xff);
            };
            d.insert (d.end(), { 'R', 'I', 'F', 'F' });
            put32 (d, (uint32_t) (36 + dataSize));
            d.insert (d.end(), { 'W', 'A', 'V', 'E' });
            auto put16 = [] (std::vector<uint8_t>& v, uint16_t x)
            {
                v.push_back ((uint8_t) (x & 0xff));
                v.push_back ((uint8_t) ((x >> 8) & 0xff));
            };
            d.insert (d.end(), { 'f', 'm', 't', ' ' });
            put32 (d, 16);
            put16 (d, 1);                       // wFormatTag
            put16 (d, (uint16_t) sp.nchans);   // nChannels
            put32 (d, (uint32_t) sp.rate);
            put32 (d, (uint32_t) (sp.rate * blockAlign));
            put16 (d, (uint16_t) blockAlign);  // nBlockAlign
            put16 (d, 32);                     // wBitsPerSample
            d.insert (d.end(), { 'd', 'a', 't', 'a' });
            put32 (d, (uint32_t) dataSize);
            // write only first 1MB of data (file shorter than declared)
            const size_t writeBytes = std::min (d.size() + (size_t) 1024 * 1024, d.size() + (size_t) dataSize);
            std::vector<uint8_t> all = d;
            all.resize (writeBytes, 0);
            out.deleteFile(); // FileOutputStream appends to existing files!
            { juce::FileOutputStream os { out };
              if (os.failedToOpen() || !os.write (all.data(), all.size()))
                  return false; }
        }
        else
        {
            if (!writeWav (out, sp.nchans, sp.rate, frames, fn))
                return false;
        }
    }
    return true;
}

// ---------------- Runner ----------------
extern int scRunUnitTests();

#ifdef SC_TESTS_EXE
int main (int argc, char** argv)
{
#ifdef _WIN32
    scCrash::install();
#endif
    if (argc > 1 && juce::String (argv[1]) == "--generate-test-files")
    {
        const juce::String dir = argc > 2 ? argv[2] : ".";
        return scGenerateTestFiles (dir) ? 0 : 1;
    }
    return scRunUnitTests();
}
#endif

// ------------------------------------------------------- rendering backend
// SpectroCraft patches JUCE's createNewPeer() to always use the GDI "Software
// Renderer" (index 0) instead of Direct2D/D3D11, which crashes with an
// unhandled C++ exception on some Intel GPU drivers. Verify that top-level
// windows really get the GDI backend.
static void testGdiRendererForced()
{
#if JUCE_WINDOWS
    juce::Component c;
    c.setSize (64, 64);
    c.addToDesktop (0); // creates the native window + peer via createNewPeer()

    if (juce::ComponentPeer* peer = c.getPeer())
        CHECK (peer->getCurrentRenderingEngine() == 0,
               "top-level windows use the GDI software renderer (not Direct2D)");
    else
        std::cerr << "  [skip] no peer created for test component\n";
#else
    std::cerr << "  [skip] rendering backend test is Windows-only\n";
#endif
}

// ------------------------------------------------------------------ playhead sync
// Clicking either view moves the app cursor; MainWindow wires
// Application::onPlayheadChanged to push that value into both views, so a
// click in one track must move the playhead in both tracks.
static void testPlayheadSync()
{
    const juce::File wav = makeTempFile ("playhead_sync.wav");
    writeWav (wav, 1, 8000.0, 40000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.audioEngine().loadFile (wav), "load wav for playhead sync test");

    WaveformView wave (app);
    SpectralView spec (app);
    wave.setSize (256, 128);
    spec.setSize (256, 128);

    // Same wiring as MainWindow.
    app.onPlayheadChanged = [&] (double t)
    {
        wave.setPlayhead (t);
        spec.setPlayhead (t);
    };

    app.setPlayhead (0.5);
    CHECK (wave.getPlayhead() == 0.5, "waveform playhead follows the app cursor");
    CHECK (spec.getPlayhead() == 0.5, "spectral playhead follows the app cursor");

    app.setPlayhead (1.25);
    CHECK (wave.getPlayhead() == 1.25 && spec.getPlayhead() == 1.25,
           "both views track subsequent cursor moves");
}

// ------------------------------------------------------------------ FFT follow-playhead window
// With a very narrow selection, following the playhead must still analyse a
// full-size window centred on the cursor (not shrink to the selection), so a
// pure tone keeps a sharp peak instead of smearing into a wide low hump.
static void testFFTFollowPlayhead()
{
    const juce::File wav = makeTempFile ("fft_follow.wav");
    writeWav (wav, 1, 8000.0, 40000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for fft follow test");
    app.settings.fftSize = 1024;
    app.settings.fftLocalN = 1024;   // picked up as the panel's localN at construction

    FFTAnalysisView view (app);
    view.setSize (300, 200);
    view.setFollowPlayhead (true);

    // Very narrow selection: the old behaviour shrank the FFT to fit it.
    app.setSelection ({ 1.0, 1.0 + 5.0 / 8000.0 });   // 40 samples
    app.playhead = 2.0;

    juce::Image img (juce::Image::ARGB, 300, 200, true);
    { juce::Graphics g (img); view.paint (g); }

    CHECK (view.getCachedN() == 1024,
           "follow mode keeps the full FFT size despite a narrow selection");

    // The pure tone must peak near its true frequency, not smear low.
    if (!view.getSpectrum().empty())
    {
        const auto& mag = view.getSpectrum()[0];
        int peakBin = 1;
        for (int k = 2; k < (int) mag.size(); ++k)
            if (mag[k] > mag[peakBin]) peakBin = k;
        const double peakHz = (double) peakBin * 8000.0 / view.getCachedN();
        CHECK (std::abs (peakHz - 440.0) < 32.0,
               "pure tone peaks near its true frequency in follow mode");
    }
}

// ------------------------------------------------------------------ 1/3-octave band plot
// In "1/3 Octave" mode the panel power-averages the FFT into IEC 61260-1 bands
// (50 Hz .. 16 kHz); a pure tone must peak in the band containing its
// frequency, and far-away bands must be much quieter.
static void testThirdOctave()
{
    const juce::File wav = makeTempFile ("third_oct.wav");
    writeWav (wav, 1, 8000.0, 40000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for third-octave test");
    app.settings.fftSize = 2048;

    FFTAnalysisView view (app);
    view.setSize (300, 200);
    view.setAxisMode (2);   // "1/3 Octave"

    juce::Image img (juce::Image::ARGB, 300, 200, true);
    { juce::Graphics g (img); view.paint (g); }

    CHECK (view.getAxisMode() == 2, "axis mode switched to 1/3 octave");
    const auto& bands = view.getThirdOctMags();
    CHECK (!bands.empty(), "band values were computed");
    if (!bands.empty())
    {
        const double* centres = FFTAnalysisView::thirdOctCentres();

        // 440 Hz lies inside the 400 Hz band (edges ~383.5 .. 449.0 Hz).
        int peak = 0;
        for (int b = 1; b < (int) bands[0].size(); ++b)
            if (bands[0][(size_t) b] > bands[0][(size_t) peak]) peak = b;
        CHECK (std::abs (centres[(size_t) peak] - 400.0) < 1e-6,
               "440 Hz tone peaks in the 400 Hz band");

        // The 1 kHz band must be far below the peak.
        int k1k = -1;
        for (int b = 0; b < FFTAnalysisView::numThirdOctBands(); ++b)
            if (std::abs (centres[(size_t) b] - 1000.0) < 1e-6) { k1k = b; break; }
        CHECK (k1k >= 0 && k1k < (int) bands[0].size(), "1 kHz band exists");
        if (k1k >= 0)
            CHECK (bands[0][(size_t) k1k] < bands[0][(size_t) peak] - 0.25,
                   "far band is much quieter than the tone's band");

        // The live band curve (channel colour = pink) must be drawn near the
        // top of the plot at the log-mapped x position of the 400 Hz centre.
        {
            const double t = std::log (400.0 / 50.0) / std::log (4000.0 / 50.0);
            const int x = 2 + (int) std::lround (t * (300 - 34 - 4));
            bool foundPink = false;
            for (int yy = 54; yy < 146 && !foundPink; ++yy)
                for (int xx = x - 4; xx <= x + 4; ++xx)
                {
                    const auto px = img.getPixelAt (xx, yy);
                    // The anti-aliased stroke blends with the black background,
                    // so allow partially covered pixels.
                    if (px.getRed() > 150 && px.getGreen() < 120
                        && px.getBlue() > 60) { foundPink = true; break; }
                }
            CHECK (foundPink, "band curve drawn at the 400 Hz position");
        }

        // Switching back to log mode must clear the band cache.
        view.setAxisMode (1);
        CHECK (view.getThirdOctMags().empty(), "band cache cleared when leaving 1/3 octave mode");
    }
}

// ------------------------------------------------------------------ 1/3-octave click update (regression)
// In "1/3 Octave" mode every click must replace the band cache with exactly one
// curve per visible channel — never accumulate stale curves from earlier clicks
// (the old bug drew a new coloured curve on each click and then froze).
// The snapshot buttons must wear their curve colour in all three states:
// ON (full colour + white border, most obvious), OFF+stored (dimmed),
// OFF+empty (dark tint).
static void testSnapshotButtonPaint()
{
    const juce::Colour cyan = juce::Colour::fromRGB (48, 192, 255);

    auto renderPixels = [] (juce::Colour col, bool on, bool stored)
        -> std::pair<juce::Colour, juce::Colour>
    {
        SnapshotButton b;
        b.setCurveColour (col);
        b.setStored (stored);
        b.setToggleState (on, juce::dontSendNotification);
        b.setSize (23, 20);
        const auto img = b.createComponentSnapshot (b.getLocalBounds());
        // (4,4) is inside the fill but clear of the centred digit; (1,1) sits
        // on the border.
        return std::make_pair (img.getPixelAt (4, 4), img.getPixelAt (1, 1));
    };

    auto colNear = [] (juce::Colour px, int r, int g, int bl)
    {
        return std::abs ((int) px.getRed() - r) <= 3
            && std::abs ((int) px.getGreen() - g) <= 3
            && std::abs ((int) px.getBlue() - bl) <= 3;
    };

    // ON: full curve colour fill + white border (the most obvious state).
    {
        auto [fill, border] = renderPixels (cyan, true, false);
        CHECK (colNear (fill, 48, 192, 255), "ON button fills with the full curve colour");
        CHECK (border.getRed() > 200 && border.getGreen() > 200 && border.getBlue() > 200,
              "ON button has a white border");
    }

    // OFF + stored: dimmed curve colour (darker(0.45) -> factor 1/1.45).
    {
        auto [fill, border] = renderPixels (cyan, false, true);
        CHECK (colNear (fill, (int) (48 / 1.45f), (int) (192 / 1.45f), (int) (255 / 1.45f)),
              "OFF+stored button shows a dimmed curve colour");
    }

    // OFF + empty: dark tint of the curve colour (darker(0.72) -> factor 1/1.72).
    {
        auto [fill, border] = renderPixels (cyan, false, false);
        CHECK (colNear (fill, (int) (48 / 1.72f), (int) (192 / 1.72f), (int) (255 / 1.72f)),
              "OFF+empty button shows a dark tint of the curve colour");
    }

    // Prominence order: empty < stored < ON.
    const auto onFill = renderPixels (cyan, true, false).first;
    const auto stFill = renderPixels (cyan, false, true).first;
    const auto emFill = renderPixels (cyan, false, false).first;
    CHECK (onFill.getRed() > stFill.getRed() && stFill.getRed() > emFill.getRed(),
          "button prominence increases: empty < stored < ON");
}

static void testThirdOctaveClickUpdates()
{
    // First half: 440 Hz tone, second half: 880 Hz tone (mono, 8 kHz).
    const juce::File wav = makeTempFile ("third_oct_click.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        const bool secondHalf = f >= 8000;
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * (secondHalf ? 880.0 : 440.0) * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for third-octave click test");   // openFile sets visible channels
    app.settings.fftSize = 1024;
    app.settings.fftLocalN = 1024;

    FFTAnalysisView view (app);
    view.setSize (300, 200);
    view.setAxisMode (2);   // "1/3 Octave"

    auto bandPeakCentre = [&] () -> double
    {
        const auto& bands = view.getThirdOctMags();
        if (bands.empty() || bands[0].empty()) return -1.0;
        int peak = 0;
        for (int b = 1; b < (int) bands[0].size(); ++b)
            if (bands[0][(size_t) b] > bands[0][(size_t) peak]) peak = b;
        return FFTAnalysisView::thirdOctCentres()[(size_t) peak];
    };

    juce::Image img (juce::Image::ARGB, 300, 200, true);

    // Click in the first half: one curve peaking at the 400 Hz band.
    app.setSelection ({ 0.0, 0.0 });
    app.setPlayhead (0.5);
    { juce::Graphics g (img); view.paint (g); }
    CHECK (view.getThirdOctMags().size() == 1, "mono file yields exactly one band curve");
    CHECK (std::abs (bandPeakCentre() - 400.0) < 1e-6, "first click peaks in the 400 Hz band");

    // Click in the second half: the single curve must move to the 800 Hz band,
    // and no stale curves may accumulate from the previous click.
    app.setPlayhead (1.5);
    { juce::Graphics g (img); view.paint (g); }
    CHECK (view.getThirdOctMags().size() == 1, "band cache replaced, not accumulated");
    CHECK (std::abs (bandPeakCentre() - 800.0) < 1e-6, "second click peaks in the 800 Hz band");

    // A third click must still update (the old bug froze the display after a few clicks).
    app.setPlayhead (0.5);
    { juce::Graphics g (img); view.paint (g); }
    CHECK (view.getThirdOctMags().size() == 1, "band cache still single-curve on third click");
    CHECK (std::abs (bandPeakCentre() - 400.0) < 1e-6, "third click updates back to the 400 Hz band");
}

// ------------------------------------------------------------------ click updates + selection averaging
// A single click (playhead move without a selection) must re-analyse the
// spectrum centred on the cursor at the selected FFT size; a selection wider
// than one window is covered by several averaged windows, so both tones of a
// two-part file appear in the result.
static void testFFTClickAndAverage()
{
    // First half: 440 Hz tone, second half: 880 Hz tone (mono, 8 kHz).
    const juce::File wav = makeTempFile ("fft_click.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        const bool secondHalf = f >= 8000;
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * (secondHalf ? 880.0 : 440.0) * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for click/average test");   // openFile sets visible channels
    app.settings.fftSize = 1024;
    app.settings.fftLocalN = 1024;

    FFTAnalysisView view (app);
    view.setSize (300, 200);

    auto peakHz = [&] () -> double
    {
        juce::Image img (juce::Image::ARGB, 300, 200, true);
        { juce::Graphics g (img); view.paint (g); }
        const auto& spec = view.getSpectrum();
        if (spec.empty()) return -1.0;
        int peakBin = 1;
        for (int k = 2; k < (int) spec[0].size(); ++k)
            if (spec[0][(size_t) k] > spec[0][(size_t) peakBin]) peakBin = k;
        return (double) peakBin * 8000.0 / view.getCachedN();
    };

    // No selection: clicking in the first half analyses a window centred there.
    app.setSelection ({ 0.0, 0.0 });   // clear any selection
    app.setPlayhead (1.0);
    CHECK (std::abs (peakHz() - 440.0) < 32.0, "click in first half peaks at 440 Hz");

    // Clicking in the second half must move the analysis with the cursor.
    app.setPlayhead (3.0);
    CHECK (std::abs (peakHz() - 880.0) < 32.0, "click in second half peaks at 880 Hz");

    // A selection spanning both halves is averaged over many windows: both
    // tones must be present and a far bin must stay quiet.
    app.setSelection ({ 0.5, 3.5 });
    juce::Image img (juce::Image::ARGB, 300, 200, true);
    { juce::Graphics g (img); view.paint (g); }
    const auto& spec = view.getSpectrum();
    CHECK (!spec.empty(), "averaged spectrum computed for wide selection");
    if (!spec.empty())
    {
        const int bin440 = (int) std::lround (440.0 * 1024 / 8000.0);
        const int bin880 = (int) std::lround (880.0 * 1024 / 8000.0);
        CHECK (spec[0][(size_t) bin440] > 0.3, "averaged selection keeps the 440 Hz tone");
        CHECK (spec[0][(size_t) bin880] > 0.3, "averaged selection keeps the 880 Hz tone");
        // Magnitudes are mapped against a fixed -160 dB floor: 0.3 ~= -112 dB,
        // which is still far below the ~-40 dB tone peaks.
        CHECK (spec[0][25] < 0.3, "far bin stays quiet in the averaged spectrum");
    }
}

// ------------------------------------------------------------------ 1/3-octave step drawing
// In "1/3 Octave" mode a single tone must render as a flat-topped step across
// its whole band cell (the classic 1/3-octave band-plot look), not a smooth curve.
static void testThirdOctaveStepDrawing()
{
    const juce::File wav = makeTempFile ("third_oct_step.wav");
    writeWav (wav, 1, 8000.0, 40000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for step-drawing test");
    app.settings.fftSize = 2048;

    FFTAnalysisView view (app);
    view.setSize (300, 200);
    view.setAxisMode (2);   // "1/3 Octave"

    juce::Image img (juce::Image::ARGB, 300, 200, true);
    { juce::Graphics g (img); view.paint (g); }

    // The 400 Hz band cell spans ~383.5 .. 449.0 Hz on the log axis
    // (fLo = 50, fHi = min(Nyquist, 16 kHz) = 4000 for an 8 kHz file).
    const double fLo = 50.0, fHi = std::min (8000.0 * 0.5, 16000.0);
    auto freqToX = [fLo, fHi] (double f)
    {
        return 2 + (int) std::lround ((std::log (f / fLo) / std::log (fHi / fLo)) * (300 - 34 - 4));
    };
    const int xLo = freqToX (400.0 * std::pow (2.0, -1.0 / 6.0));
    const int xHi = freqToX (400.0 * std::pow (2.0,  1.0 / 6.0));

    // Collect the topmost pink pixel in each column of the band cell.
    int minY = 9999, maxY = -1;
    int cols = 0;
    for (int x = xLo + 1; x <= xHi - 1 && x < 300; ++x)
    {
        for (int y = 54; y < 164; ++y)
        {
            const auto px = img.getPixelAt (x, y);
            // The anti-aliased stroke blends with the black background,
            // so allow partially covered pixels.
            if (px.getRed() > 150 && px.getGreen() < 120
                && px.getBlue() > 60)
            {
                minY = std::min (minY, y); maxY = std::max (maxY, y);
                ++cols;
                break;   // topmost pink in this column
            }
        }
    }

    CHECK (cols >= 5, "flat top spans most of the band cell");
    if (cols >= 5)
        CHECK (maxY - minY <= 2, "band top is flat (step function), not a curve");
}

// ------------------------------------------------------------------ change gain
// Tools > Change Gain multiplies the selection by 10^(dB/20) on the visible
// channels and must be undoable.
static void testChangeGain()
{
    const juce::File wav = makeTempFile ("gain.wav");
    writeWav (wav, 1, 8000.0, 8000, [] (int, int64_t, double) { return 0.5f; });

    Application app;
    CHECK (app.openFile (wav), "open wav for gain test");   // openFile sets visible channels
    app.setSelection ({ 0.25, 0.75 });   // samples 2000..6000

    app.changeGain (6.0f);   // +6 dB = x10^(6/20)

    float inSel = -1.0f, outSel = -1.0f;
    CHECK (app.audioEngine().readSamples (0, 4000, 1, &inSel), "read inside selection");
    CHECK (app.audioEngine().readSamples (0, 1000, 1, &outSel), "read outside selection");
    const float expected = 0.5f * std::pow (10.0f, 6.0f / 20.0f);
    CHECK (std::abs (inSel - expected) < 1e-4,
           "+6 dB scales the selected samples by 10^(dB/20)");
    CHECK (std::abs (outSel - 0.5f) < 1e-4, "samples outside the selection are untouched");

    app.undoStep();
    CHECK (app.audioEngine().readSamples (0, 4000, 1, &inSel), "read after undo");
    CHECK (std::abs (inSel - 0.5f) < 1e-4, "undo restores the original level");
}

// ------------------------------------------------------------------ playback chain
// Verifies the real-time output path end-to-end (AudioDeviceManager ->
// AudioSourcePlayer -> AudioEngine): pressing play must advance the playhead,
// pausing must freeze it. Skipped on machines without a default output device.
// Plays at very low gain so it is barely audible.
static void testPlaybackChain()
{
#if JUCE_WINDOWS
    ::CoInitializeEx (nullptr, COINIT_MULTITHREADED);

    juce::AudioDeviceManager device;
    const juce::String err = device.initialiseWithDefaultDevices (0, 2);
    if (!err.isEmpty() || device.getCurrentAudioDevice() == nullptr)
    {
        std::cerr << "  [skip] no default audio output device"
                  << (err.isEmpty() ? String() : (" (" + err + ")")) << "\n";
        ::CoUninitialize();
        return;
    }

    AudioEngine engine;
    const juce::File wav = findTestDataFile ("tone_10s_stereo.wav");
    if (!wav.existsAsFile() || !engine.loadFile (wav))
    {
        std::cerr << "  [skip] tone_10s_stereo.wav not found\n";
        ::CoUninitialize();
        return;
    }

    juce::AudioSourcePlayer player;
    player.setGain (0.03f); // keep the test quiet
    player.setSource (&engine);
    device.addAudioCallback (&player);

    engine.play();
    for (int i = 0; i < 30 && engine.position() < 0.15; ++i)
        juce::Thread::sleep (50);

    const double pos = engine.position();
    CHECK (pos > 0.05, "playback advances playhead");
    std::cerr << "  playback position after wait: " << pos << " s\n";

    // Pause freezes the playhead.
    engine.pause();
    const double p1 = engine.position();
    juce::Thread::sleep (200);
    CHECK (std::abs (engine.position() - p1) < 0.01, "pause freezes playhead");

    device.removeAudioCallback (&player);
    player.setSource (nullptr);
    ::CoUninitialize();
#else
    std::cerr << "  [skip] playback chain test is Windows-only\n";
#endif
}

// ------------------------------------------------------- snapshot store/recompute
// Snapshot buttons must remember the time segment they were stored from and
// recompute that exact segment when the FFT size or window type changes.
static void testSnapshotStoreAndRecompute()
{
    // First half: 440 Hz tone, second half: 880 Hz tone (mono, 8 kHz, 4 s).
    const juce::File wav = makeTempFile ("snap_store.wav");
    writeWav (wav, 1, 8000.0, 32000, [] (int, int64_t f, double rate)
    {
        const bool secondHalf = f >= 16000;
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * (secondHalf ? 880.0 : 440.0) * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for snapshot test");
    app.settings.fftSize = 1024;
    app.settings.fftLocalN = 1024;

    FFTAnalysisView view (app);
    view.setSize (300, 200);

    auto peakHzOf = [&] (const std::vector<std::vector<float>>& spec, int n) -> double
    {
        if (spec.empty() || n <= 1) return -1.0;
        int peakBin = 1;
        for (int k = 2; k < (int) spec[0].size(); ++k)
            if (spec[0][(size_t) k] > spec[0][(size_t) peakBin]) peakBin = k;
        return (double) peakBin * 8000.0 / n;
    };

    // Store the first region: selection [0.5, 1.5] lies in the 440 Hz half.
    app.setSelection ({ 0.5, 1.5 });
    view.recomputeIfNeeded();
    CHECK (!view.getSpectrum().empty(), "live spectrum ready before storing");

    view.handleSnapshotClick (0);
    CHECK (view.snapshotShown (0), "snapshot button stores and shows the curve");
    CHECK (std::abs (view.snapshotT0 (0) - 0.5) < 1e-9 && std::abs (view.snapshotT1 (0) - 1.5) < 1e-9,
          "snapshot remembers its time segment");
    CHECK (std::abs (peakHzOf (view.snapshotMags (0), view.snapshotN (0)) - 440.0) < 32.0,
          "stored snapshot peaks at the tone of its own region");

    // Store a second region: selection [2.5, 3.5] lies in the 880 Hz half.
    // (Wait out the interactive drag throttle before requesting the new
    // wide region.)
    std::this_thread::sleep_for (std::chrono::milliseconds (80));
    app.setSelection ({ 2.5, 3.5 });
    view.recomputeIfNeeded();
    view.handleSnapshotClick (1);
    CHECK (view.snapshotShown (1), "second snapshot stored");
    CHECK (std::abs (peakHzOf (view.snapshotMags (1), view.snapshotN (1)) - 880.0) < 32.0,
          "second snapshot peaks at its own tone");

    // Changing the FFT size must recompute every stored snapshot from its own
    // time segment with the new settings; both tones must survive.
    view.setFftSize (4096);
    CHECK (view.snapshotN (0) == 4096 && view.snapshotN (1) == 4096,
          "snapshots recomputed at the new FFT size");
    CHECK (std::abs (peakHzOf (view.snapshotMags (0), 4096) - 440.0) < 8.0,
          "snapshot 1 still peaks at 440 Hz after recompute");
    CHECK (std::abs (peakHzOf (view.snapshotMags (1), 4096) - 880.0) < 8.0,
          "snapshot 2 still peaks at 880 Hz after recompute");

    // Changing the window type must also trigger a full snapshot recompute.
    view.setWindowType (2);   // Hamming
    CHECK (view.snapshotN (0) == 4096, "snapshots stay valid after window change");
    CHECK (std::abs (peakHzOf (view.snapshotMags (0), 4096) - 440.0) < 8.0,
          "snapshot still peaks at its tone with the new window");

    // Clicking a button whose slot already holds the live region toggles it off.
    app.setSelection ({ 2.5, 3.5 });   // same region as snapshot 1
    view.recomputeIfNeeded();
    CHECK (view.snapshotShown (1), "snapshot visible before toggle");
    view.handleSnapshotClick (1);
    CHECK (!view.snapshotShown (1), "clicking the same region toggles visibility off");

    // A plain click on an already-stored slot must NOT update it - only
    // toggle its visibility. First toggle the curve back on...
    view.handleSnapshotClick (1);
    CHECK (view.snapshotShown (1), "plain click toggles the stored curve back on");

    // ...then analyse a different region and plain-click again.
    std::this_thread::sleep_for (std::chrono::milliseconds (80));
    app.setSelection ({ 0.5, 1.5 });   // different from snapshot 1's [2.5, 3.5]
    view.recomputeIfNeeded();
    CHECK (view.snapshotShown (1), "snapshot still visible before next plain click");
    view.handleSnapshotClick (1);      // no modifiers in tests = plain click
    CHECK (!view.snapshotShown (1), "plain click on stored slot toggles visibility off");
    CHECK (std::abs (view.snapshotT0 (1) - 2.5) < 1e-9 && std::abs (view.snapshotT1 (1) - 3.5) < 1e-9,
          "plain click did not update the stored segment");

    // Ctrl+click re-stores from the current live region ([0.5, 1.5]).
    view.handleSnapshotClick (1, juce::ModifierKeys (juce::ModifierKeys::commandModifier));
    CHECK (view.snapshotShown (1), "Ctrl+click shows the updated curve");
    CHECK (std::abs (view.snapshotT0 (1) - 0.5) < 1e-9 && std::abs (view.snapshotT1 (1) - 1.5) < 1e-9,
          "Ctrl+click updated the stored segment to the live region");

    // Shift+click clears the slot.
    view.handleSnapshotClick (1, juce::ModifierKeys (juce::ModifierKeys::shiftModifier));
    CHECK (view.snapshotN (1) == 0 && !view.snapshotShown (1), "Shift+click cleared the snapshot slot");

    // Multi-file: opening another file does NOT clear stored snapshots - they
    // are shared across all open files and each one remembers its source doc.
    const juce::File wav2 = makeTempFile ("snap_other.wav");
    writeWav (wav2, 1, 8000.0, 4000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 300.0 * (double) f / rate);
    });
    CHECK (app.openFile (wav2), "open second file");
    view.recomputeIfNeeded();   // live spectrum now follows the new active file...
    CHECK (view.snapshotN (0) == 4096 && !view.snapshotMags (0).empty(),
          "snapshots survive opening another file (shared across files)");
    CHECK (std::abs (peakHzOf (view.snapshotMags (0), view.snapshotN (0)) - 440.0) < 8.0,
          "snapshot still peaks at its source file's tone after switching");
}

// ------------------------------------------------- snapshot overlay paint
// Regression: stored snapshot overlays must be painted over the live curve in
// both linear and log axis modes. A dangling else in the overlay loop used to
// bind to the inner 1/3-octave if, so snapshot curves were silently never
// drawn whenever the axis mode was not "1/3 Octave".
static void testSnapshotOverlayPaint()
{
    // First half: 440 Hz tone, second half: 880 Hz tone (mono, 8 kHz).
    const juce::File wav = makeTempFile ("snap_overlay.wav");
    writeWav (wav, 1, 8000.0, 32000, [] (int, int64_t f, double rate)
    {
        const bool secondHalf = f >= 16000;
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * (secondHalf ? 880.0 : 440.0) * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for snapshot overlay paint test");

    FFTAnalysisView view (app);
    view.setSize (320, 300);

    // Analyse the first half (440 Hz) and store it in slot 1.
    app.setSelection ({ 0.0, 2.0 });
    view.recomputeIfNeeded();
    view.handleSnapshotClick (0);
    CHECK (view.snapshotShown (0), "snapshot 1 stored and shown");

    // Move the live analysis to the second half so the live curve peaks at
    // 880 Hz while the stored overlay still peaks at 440 Hz. The sleep keeps
    // this recompute outside the 60 ms multi-window throttle, which would
    // otherwise leave the live cache on the first-half data.
    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    app.setSelection ({ 2.0, 4.0 });
    view.recomputeIfNeeded();

    auto countDimCyan = [] (const juce::Image& img) -> int
    {
        // Slot 1's curve colour is cyan (48,192,255); the overlay is drawn with
        // alpha 90 over the black plot background. Count pixels whose blue
        // channel clearly dominates red - the live magenta curve and the grey
        // grid lines never satisfy this.
        int count = 0;
        for (int y = 54; y < 262; ++y)
            for (int x = 2; x < 282; ++x)
            {
                const auto px = img.getPixelAt (x, y);
                if (px.getBlue() > 60 && px.getBlue() > px.getRed() + 25) ++count;
            }
        return count;
    };

    // Linear axis mode.
    view.setAxisMode (0);
    {
        juce::Image img (juce::Image::ARGB, 320, 300, true);
        juce::Graphics g (img);
        view.paint (g);
        CHECK (countDimCyan (img) > 20,
              "stored snapshot overlay is painted in linear axis mode");
    }

    // Log axis mode (the default).
    view.setAxisMode (1);
    {
        juce::Image img (juce::Image::ARGB, 320, 300, true);
        juce::Graphics g (img);
        view.paint (g);
        CHECK (countDimCyan (img) > 20,
              "stored snapshot overlay is painted in log axis mode");
    }
}

// ------------------------------------------------------------------ status bar
// The bottom status bar shows the mouse probe on the left and the selection +
// playhead times on the right.
static void testStatusBar()
{
    StatusBar bar;
    bar.setSize (600, 20);
    bar.setSelection (1.0, 2.5);
    bar.setPlayhead (0.75);
    bar.setMouseInfo ("t=1.234 s   ch L: +0.72");

    auto brightIn = [] (const juce::Image& im, int x0, int x1) -> bool
    {
        for (int y = 0; y < im.getHeight(); ++y)
            for (int x = std::max (0, x0); x < std::min (im.getWidth(), x1); ++x)
                if (im.getPixelAt (x, y).getRed() > 150) return true;
        return false;
    };

    juce::Image img (juce::Image::ARGB, 600, 20, true);
    { juce::Graphics g (img); bar.paint (g); }
    CHECK (brightIn (img, 6, 300), "status bar shows the mouse probe on the left");
    CHECK (brightIn (img, 300, 594), "status bar shows selection/playhead on the right");

    // Clearing the mouse info leaves only the right side.
    bar.setMouseInfo ("");
    juce::Image img2 (juce::Image::ARGB, 600, 20, true);
    { juce::Graphics g (img2); bar.paint (g); }
    CHECK (!brightIn (img2, 6, 300), "cleared mouse info leaves the left side empty");
    CHECK (brightIn (img2, 300, 594), "right side still shows selection/playhead");

    // The active file name sits at the far left of the footer.
    bar.setFileName ("tone.wav");
    juce::Image img3 (juce::Image::ARGB, 600, 20, true);
    { juce::Graphics g (img3); bar.paint (g); }
    CHECK (brightIn (img3, 6, 300), "file name is shown on the left");

    // With both file name and mouse probe present, the probe starts to the
    // right of the name instead of overlapping it.
    const juce::Font f (12.0f);
    const int nameW = std::min (600 / 2 - 6,
                                (int) juce::GlyphArrangement::getStringWidth (f, "tone.wav") + 8);
    const int probeX = 6 + nameW + 12;
    bar.setMouseInfo ("t=1.234 s   ch L: +0.72");
    juce::Image img4 (juce::Image::ARGB, 600, 20, true);
    { juce::Graphics g (img4); bar.paint (g); }
    CHECK (brightIn (img4, 6, probeX), "file name still at the far left");
    CHECK (brightIn (img4, probeX + 2, 300), "mouse probe starts after the file name");

    // Clearing the name sends the mouse probe back to its original spot.
    bar.setFileName ("");
    juce::Image img5 (juce::Image::ARGB, 600, 20, true);
    { juce::Graphics g (img5); bar.paint (g); }
    CHECK (brightIn (img5, 6, probeX), "mouse probe returns to the far left");
}

// ------------------------------------------------- footer file name wiring
// The status bar must always show the name of the file currently in the
// views: on open, when switching between loaded files and after closing.
static void testStatusBarFileName()
{
    const juce::File wavA = makeTempFile ("footer_a.wav");
    const juce::File wavB = makeTempFile ("footer_b.wav");
    writeWav (wavA, 1, 8000.0, 2000, [] (int, int64_t f, double rate)
             { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 300.0 * (double) f / rate); });
    writeWav (wavB, 1, 8000.0, 2000, [] (int, int64_t f, double rate)
             { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 500.0 * (double) f / rate); });

    Application app;
    juce::MessageManager* mm = juce::MessageManager::getInstance();
    std::unique_ptr<MainWindow> win (new MainWindow (app, "footer name test"));

    CHECK (win->statusBarFileName().isEmpty(), "no file loaded: footer name is empty");

    win->openFile (wavA);
    CHECK (win->statusBarFileName() == wavA.getFileName(),
           "opening a file shows its name in the footer");

    win->openFile (wavB);
    CHECK (win->statusBarFileName() == wavB.getFileName(),
           "switching to another loaded file updates the footer");

    app.closeFile();   // closes the active B, activates A again
    CHECK (win->statusBarFileName() == wavA.getFileName(),
           "closing the active file falls back to the previous one");

    app.closeFile();   // no documents left
    CHECK (win->statusBarFileName().isEmpty(), "closing every file clears the footer name");

    win.reset();   // destroy the window while its MessageManager is still alive
    delete mm;
    wavA.deleteFile();
    wavB.deleteFile();
}

// ---------------- About dialog licence credits ----------------
// Help > About must credit the project licence plus every third-party
// component: JUCE, SoXR (the engine Audacity uses), and - in AI builds - the
// OpenVINO runtime, Intel's Audacity-plugin DSP port (GPL-3.0) with its model
// weights, whisper.cpp (MIT) and the Whisper checkpoints.
static void testAboutText()
{
    const juce::String about = MainWindow::buildAboutText();
    CHECK (about.contains ("SpectroCraft v"), "version line present");
    CHECK (about.contains ("GNU Affero General Public License v3.0"), "project AGPL licence stated");
    CHECK (about.contains ("JUCE 9.0.4") && about.contains ("AGPL-3.0"), "JUCE licence credited");
    CHECK (about.contains ("SoXR resampler - LGPL-2.1"), "SoXR licence credited");

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    CHECK (about.contains ("OpenVINO runtime 2024.6") && about.contains ("Apache-2.0, Intel Corporation"),
           "OpenVINO runtime licence credited");
    CHECK (about.contains ("Intel's OpenVINO plugin for Audacity"),
           "noise-suppression code source: Intel's Audacity plugin");
    CHECK (about.contains ("GPL-3.0-only") && about.contains ("Copyright (C) 2024 Intel Corporation"),
           "DeepFilterNet port GPL licence credited");
    CHECK (about.contains ("DenseUNet model weights - Apache-2.0, Intel Open Source Model Zoo"),
           "DenseUNet model licence credited");
    CHECK (about.contains ("intel/deepfilternet-openvino"), "DeepFilterNet 2/3 model source cited");
    CHECK (about.contains ("whisper.cpp v1.6.2") && about.contains ("MIT, ggerganov"),
           "whisper.cpp MIT licence credited");
    CHECK (about.contains ("OpenAI's Whisper checkpoints"), "Whisper weights attributed to OpenAI");
#else
    CHECK (!about.contains ("Noise suppression:"), "non-AI build: no AI section in about text");
#endif
}

// ------------------------------------------------------- time scrollbar on load
// The horizontal time scrollbar must appear as soon as a file is loaded.
// Previously its visibility/bounds were only set in resized(), and loading a
// file never resizes the window - so the bar stayed hidden until the user
// maximised/restored (any resize) happened to re-run the layout.
static void testTimeScrollBarOnLoad()
{
    const juce::File wav = makeTempFile ("timebar_load.wav");
    writeWav (wav, 1, 8000.0, 2000, [] (int, int64_t f, double rate)
             { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 300.0 * (double) f / rate); });

    Application app;
    juce::MessageManager* mm = juce::MessageManager::getInstance();
    std::unique_ptr<MainWindow> win (new MainWindow (app, "time bar test"));

    CHECK (!win->timeScrollBarVisible(), "no file: time scrollbar hidden");

    win->openFile (wav);
    CHECK (win->timeScrollBarVisible(),
           "loading a file shows the time scrollbar without any window resize");

    app.closeFile();   // no documents left
    CHECK (!win->timeScrollBarVisible(), "closing every file hides the time scrollbar again");

    win.reset();
    delete mm;
    wav.deleteFile();
}

// ------------------------------------------------------- left panel layout
// Panel tabs can be reordered within a group or moved between the two groups;
// the arrangement must survive an app restart. Also covers View > Left Panels.
static void testLeftPanelLayout()
{
    const juce::File store = makeTempFile ("left_layout.recent");
    store.deleteFile ();   // deterministic per-run name: never trust leftovers

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    const juce::StringArray defaultBottom { "Spectral Controls", "Markers", "Transcription" };
#else
    const juce::StringArray defaultBottom { "Spectral Controls", "Markers" };
#endif
    const juce::StringArray defaultTop { "Files", "Frequency Analysis", "History" };
    juce::StringArray expectBottom = defaultBottom;
    for (int i = 0; i < expectBottom.size (); ++i)
        if (expectBottom[i] == "Markers") { expectBottom.remove (i); break; }

    // 1) Fresh store: the default arrangement.
    {
        Application app (store);
        juce::MessageManager* mm = juce::MessageManager::getInstance();
        std::unique_ptr<MainWindow> win (new MainWindow (app, "left layout test"));

        CHECK (win->topPanelTabNames () == defaultTop, "default top group order");
        CHECK (win->bottomPanelTabNames () == defaultBottom, "default bottom group order");

        // 2) Move Markers from the bottom group to position 1 in the top group.
        win->moveLeftPanelTab (win->bottomPanelTabs (), 1, win->topPanelTabs (), 1);
        const juce::StringArray expectTopAfterMove { "Files", "Markers",
                                                     "Frequency Analysis", "History" };
        CHECK (win->topPanelTabNames () == expectTopAfterMove, "tab moved across groups");
        CHECK (win->bottomPanelTabNames () == expectBottom, "source group lost the tab");

        // 3) Same-bar move: Files to just before Frequency Analysis.
        win->moveLeftPanelTab (win->topPanelTabs (), 0, win->topPanelTabs (), 2);
        const juce::StringArray expectTopFinal { "Markers", "Files",
                                                 "Frequency Analysis", "History" };
        CHECK (win->topPanelTabNames () == expectTopFinal, "same-bar move");

        // 4) View > Left Panels toggle hides/shows the whole column.
        win->setLeftPanelsVisible (false);
        CHECK (!win->topPanelTabs ()->isVisible () && !win->bottomPanelTabs ()->isVisible (),
               "left panels hidden by the menu toggle");
        win->setLeftPanelsVisible (true);
        CHECK (win->topPanelTabs ()->isVisible (), "left panels shown again");

        win.reset();
        delete mm;
    }

    // 5) A fresh app instance must restore the saved arrangement.
    {
        const juce::StringArray expectTopFinal { "Markers", "Files",
                                                 "Frequency Analysis", "History" };
        Application app (store);
        juce::MessageManager* mm = juce::MessageManager::getInstance();
        std::unique_ptr<MainWindow> win (new MainWindow (app, "left layout test 2"));

        CHECK (win->topPanelTabNames () == expectTopFinal,
              "tab order persisted across restarts");
        CHECK (win->bottomPanelTabNames () == expectBottom, "group membership persisted");

        win.reset();
        delete mm;
    }

    store.deleteFile ();
}

// ------------------------------------------------------- Home / End panning
// Home and End must move the playhead AND pan both views to the start/end of
// the file while keeping the current zoom span.
static void testHomeEndPans()
{
    const juce::File wav = makeTempFile ("homeend_pan.wav");
    writeWav (wav, 1, 8000.0, 40000, [] (int, int64_t f, double rate)
             { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 300.0 * (double) f / rate); });

    Application app;
    juce::MessageManager* mm = juce::MessageManager::getInstance();
    std::unique_ptr<MainWindow> win (new MainWindow (app, "home/end pan test"));
    win->openFile (wav);
    CHECK (app.audioEngine ().isFileLoaded (), "file loaded");

    const double dur = app.audioEngine ().duration ();   // 5 s
    CHECK (dur > 4.9 && dur < 5.1, "expected a ~5 s file");

    win->applyTimeWindow (2.0, 4.0);
    CHECK (std::abs (app.viewStart - 2.0) < 1e-6 && std::abs (app.viewEnd - 4.0) < 1e-6,
           "zoomed to [2,4] first");

    win->keyPressed (juce::KeyPress (juce::KeyPress::homeKey), nullptr);
    CHECK (std::abs (app.viewStart) < 1e-6 && std::abs (app.viewEnd - 2.0) < 1e-6,
           "Home pans the view to the start");
    CHECK (std::abs (app.playhead) < 1e-9, "Home moves the playhead to zero");

    win->keyPressed (juce::KeyPress (juce::KeyPress::endKey), nullptr);
    CHECK (std::abs (app.viewEnd - dur) < 1e-6 && std::abs (app.viewStart - (dur - 2.0)) < 1e-6,
           "End pans the view to the end");
    CHECK (std::abs (app.playhead - dur) < 1e-9, "End moves the playhead to the end");

    win.reset();
    delete mm;
    wav.deleteFile ();
}

// ------------------------------------------------------- spectral point probe
// SpectralView's cursor readout probes the tile cache for the dB level at an
// exact time/frequency; a steady tone must give a strong peak on its own
// frequency and be much quieter elsewhere.
static void testSpectralProbe()
{
    const juce::File wav = makeTempFile ("probe.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for probe test");

    SpectralEngine engine;
    const Settings s = app.settings;   // default: 2048-point Hann, hop 512
    const float peak = engine.probeMagnitude (app.audioEngine(), 0, 1.0, 400.0, s, 0);
    const float off  = engine.probeMagnitude (app.audioEngine(), 0, 1.0, 300.0, s, 0);

    CHECK (peak > -35.0f && peak < -5.0f, "probe finds the 400 Hz tone");
    CHECK (off < peak - 15.0f, "probe is much quieter off the tone frequency");
}

// ------------------------------------------------------------------ arrow keys / Shift+arrow selection
// Plain Left/Right moves the playhead in 10 ms steps; Ctrl+Left/Right moves it
// one sample at a time. Holding Shift extends the selection from where the
// shift-drag started to the current playhead position (in either step size).
static void testArrowKeySelection()
{
    const juce::File wav = makeTempFile ("arrow_keys.wav");
    writeWav (wav, 1, 8000.0, 4000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for arrow key test");

    // A MessageManager is required before constructing the top-level window;
    // it must still be alive when the window is destroyed.
    juce::MessageManager* mm = juce::MessageManager::getInstance();
    std::unique_ptr<MainWindow> win (new MainWindow (app, "arrow key test"));

    const double fine = 1.0 / app.audioEngine().sampleRate();   // one sample
    const double coarse = 0.010;                                // 10 ms step
    auto pressRight = [&] (juce::ModifierKeys mods)
    { return win->keyPressed (juce::KeyPress (juce::KeyPress::rightKey, mods, 0), nullptr); };
    auto pressLeft  = [&] (juce::ModifierKeys mods)
    { return win->keyPressed (juce::KeyPress (juce::KeyPress::leftKey, mods, 0), nullptr); };

    // Tolerance-based selection comparison: repeated +/- steps accumulate
    // floating-point rounding that exact Range equality would reject.
    auto selClose = [&] (double lo, double hi)
    { return std::abs (app.selection.getStart () - lo) < 1e-9
          && std::abs (app.selection.getEnd ()   - hi) < 1e-9; };

    app.setPlayhead (0.1);
    app.setSelection ({ 0.25, 0.3 });   // pre-existing selection must survive plain moves

    CHECK (pressRight (juce::ModifierKeys::noModifiers), "plain Right is handled");
    CHECK (std::abs (app.playhead - (0.1 + coarse)) < 1e-9,
           "plain Right moves the playhead one 10 ms step to the right");
    CHECK (app.selection == juce::Range<double> (0.25, 0.3),
           "plain movement does not touch the selection");

    CHECK (pressRight (juce::ModifierKeys::commandModifier), "Ctrl+Right is handled");
    CHECK (std::abs (app.playhead - (0.1 + coarse + fine)) < 1e-9,
           "Ctrl+Right moves the playhead one sample to the right");

    // Shift+Right: anchor at the current position, extend rightwards in 10 ms steps.
    const double anchor = app.playhead;
    CHECK (pressRight (juce::ModifierKeys::shiftModifier), "Shift+Right is handled");
    CHECK (std::abs (app.playhead - (anchor + coarse)) < 1e-9,
           "Shift+Right moves the playhead one 10 ms step to the right");
    CHECK (selClose (anchor, anchor + coarse),
           "selection spans from the anchor to the new position");

    pressRight (juce::ModifierKeys::shiftModifier);
    pressRight (juce::ModifierKeys::shiftModifier);
    CHECK (std::abs (app.playhead - (anchor + 3.0 * coarse)) < 1e-9, "repeated Shift+Right keeps moving");
    CHECK (selClose (anchor, anchor + 3.0 * coarse),
           "selection follows the playhead to the right");

    // Release Shift between the two shift-drag sequences so the next one
    // anchors at a fresh position (the 30 Hz timer clears the anchor once it
    // sees no physical Shift key held).
    for (int i = 0; i < 12; ++i)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        mm->runDispatchLoopUntil (1);
    }

    // Ctrl+Shift+Right: fine one-sample selection extension.
    const double anchor2 = app.playhead;
    CHECK (pressRight (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier),
           "Ctrl+Shift+Right is handled");
    CHECK (std::abs (app.playhead - (anchor2 + fine)) < 1e-9,
           "Ctrl+Shift+Right moves the playhead one sample to the right");
    CHECK (selClose (anchor2, anchor2 + fine),
           "fine selection spans from the anchor to the new position");

    // Shift+Left: shrink back across the anchor and flip direction.
    pressLeft (juce::ModifierKeys::shiftModifier);
    CHECK (std::abs (app.playhead - (anchor2 + fine - coarse)) < 1e-9,
           "Shift+Left moves one 10 ms step left");
    CHECK (selClose (anchor2 + fine - coarse, anchor2),
           "selection flips to [playhead, anchor] when crossing");
    for (int i = 0; i < 3; ++i)
        pressLeft (juce::ModifierKeys::shiftModifier);
    CHECK (std::abs (app.playhead - (anchor2 + fine - 4.0 * coarse)) < 1e-9, "repeated Shift+Left keeps moving");
    CHECK (selClose (anchor2 + fine - 4.0 * coarse, anchor2),
           "selection follows the playhead to the left");

    // Release Shift: no physical key is held in this process, so the 30 Hz timer
    // clears the anchor within a few ticks; plain moves must then stop touching
    // the selection. Pump the message loop while sleeping so the timer fires.
    for (int i = 0; i < 12; ++i)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        mm->runDispatchLoopUntil (1);
    }
    const juce::Range<double> frozen = app.selection;
    pressRight (juce::ModifierKeys::noModifiers);
    CHECK (app.selection == frozen, "after Shift release plain moves no longer change the selection");

    win.reset();   // destroy the window while its MessageManager is still alive
    delete mm;
    wav.deleteFile();
}

// ------------------------------------------------- Ctrl+number channel toggle
// Ctrl+1..9 toggles a channel's visibility like the View menu items;
// Ctrl+Shift+1..9 shows ONLY that channel (solo), matching the menu's
// Ctrl/Shift-click behaviour.
static void testCtrlNumberChannelToggle()
{
    const juce::File wav = makeTempFile ("ctrl_num_chans.wav");
    writeWav (wav, 2, 8000.0, 4000, [] (int ch, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * (ch == 0 ? 300.0 : 500.0) * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for ctrl+number test");

    // A MessageManager is required before constructing the top-level window;
    // it must still be alive when the window is destroyed.
    juce::MessageManager* mm = juce::MessageManager::getInstance();
    std::unique_ptr<MainWindow> win (new MainWindow (app, "ctrl number test"));

    auto press = [&] (char c, juce::ModifierKeys mods)
    { return win->keyPressed (juce::KeyPress (c, mods, 0), nullptr); };

    CHECK (app.isChannelVisible (0) && app.isChannelVisible (1), "both channels visible initially");

    // A plain number key does nothing.
    CHECK (!press ('1', juce::ModifierKeys::noModifiers), "plain 1 is left unhandled");
    CHECK (app.isChannelVisible (0) && app.isChannelVisible (1), "plain key leaves visibility alone");

    // Ctrl+1 hides channel 1; Ctrl+2 hides channel 2.
    CHECK (press ('1', juce::ModifierKeys::commandModifier), "Ctrl+1 is handled");
    CHECK (!app.isChannelVisible (0) && app.isChannelVisible (1), "Ctrl+1 toggles channel 1 off");
    CHECK (press ('2', juce::ModifierKeys::commandModifier), "Ctrl+2 is handled");
    CHECK (!app.isChannelVisible (0) && !app.isChannelVisible (1), "Ctrl+2 toggles channel 2 off");

    // Ctrl+Shift+1 shows ONLY channel 1 (solo, like the menu).
    CHECK (press ('1', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier),
           "Ctrl+Shift+1 is handled");
    CHECK (app.isChannelVisible (0) && !app.isChannelVisible (1), "Ctrl+Shift+1 solos channel 1");

    // Ctrl+9: a stereo file has no channel 9 -> unhandled, state untouched.
    CHECK (!press ('9', juce::ModifierKeys::commandModifier), "Ctrl+9 is left unhandled for a stereo file");
    CHECK (app.isChannelVisible (0) && !app.isChannelVisible (1), "unhandled key leaves visibility alone");

    // Ctrl+2 toggles channel 2 back on.
    CHECK (press ('2', juce::ModifierKeys::commandModifier), "Ctrl+2 is handled again");
    CHECK (app.isChannelVisible (0) && app.isChannelVisible (1), "both channels visible again");

    win.reset();   // destroy the window while its MessageManager is still alive
    delete mm;
    wav.deleteFile();
}

// ------------------------------------------------- F9 view-layout cycle
// F9 cycles both -> waveform only -> spectrogram only -> both; Shift+F9 runs
// the same cycle backwards. At least one view is always visible.
static void testF9ViewCycle()
{
    const juce::File wav = makeTempFile ("f9_cycle.wav");
    writeWav (wav, 1, 8000.0, 4000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for F9 cycle test");

    juce::MessageManager* mm = juce::MessageManager::getInstance();
    std::unique_ptr<MainWindow> win (new MainWindow (app, "F9 cycle test"));

    auto pressF9 = [&] (juce::ModifierKeys mods)
    { return win->keyPressed (juce::KeyPress (juce::KeyPress::F9Key, mods, 0), nullptr); };

    // Starts with both views visible.
    CHECK (win->waveformVisible() && win->spectrogramVisible(), "starts with both views");

    // F9: both -> waveform only.
    CHECK (pressF9 (juce::ModifierKeys::noModifiers), "F9 handled");
    CHECK (win->waveformVisible() && !win->spectrogramVisible(), "first F9 -> waveform only");

    // Second F9: waveform only -> spectrogram only.
    CHECK (pressF9 (juce::ModifierKeys::noModifiers), "second F9 handled");
    CHECK (!win->waveformVisible() && win->spectrogramVisible(), "second F9 -> spectrogram only");

    // Third F9 wraps back to both.
    CHECK (pressF9 (juce::ModifierKeys::noModifiers), "third F9 handled");
    CHECK (win->waveformVisible() && win->spectrogramVisible(), "third F9 wraps to both");

    // Shift+F9 from both goes backwards: -> spectrogram only.
    CHECK (pressF9 (juce::ModifierKeys::shiftModifier), "Shift+F9 handled");
    CHECK (!win->waveformVisible() && win->spectrogramVisible(), "Shift+F9 -> spectrogram only");

    // Shift+F9 again: -> waveform only; once more wraps back to both.
    CHECK (pressF9 (juce::ModifierKeys::shiftModifier), "second Shift+F9 handled");
    CHECK (win->waveformVisible() && !win->spectrogramVisible(), "second Shift+F9 -> waveform only");
    CHECK (pressF9 (juce::ModifierKeys::shiftModifier), "third Shift+F9 handled");
    CHECK (win->waveformVisible() && win->spectrogramVisible(), "Shift+F9 wraps to both");

    // F9 with other modifiers is ignored.
    CHECK (!pressF9 (juce::ModifierKeys::commandModifier), "Ctrl+F9 not handled");
    CHECK (win->waveformVisible() && win->spectrogramVisible(), "state unchanged by Ctrl+F9");

    win.reset();   // destroy the window while its MessageManager is still alive
    delete mm;
    wav.deleteFile();
}

// ------------------------------------------------- no help tooltips over the views
// The waveform and spectrogram used to carry long mouse-help tooltips;
// they were removed so hovering only shows the status-bar probe.
static void testNoViewTooltips()
{
    const juce::File wav = makeTempFile ("no_tooltip.wav");
    writeWav (wav, 1, 8000.0, 4000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for tooltip test");

    juce::MessageManager* mm = juce::MessageManager::getInstance();
    std::unique_ptr<MainWindow> win (new MainWindow (app, "tooltip test"));

    CHECK (win->waveformTooltip().isEmpty(), "no help tooltip over the waveform view");
    CHECK (win->spectralTooltip().isEmpty(), "no help tooltip over the spectrogram view");

    win.reset();   // destroy the window while its MessageManager is still alive
    delete mm;
    wav.deleteFile ();
}

// ------------------------------------------------- File > Open Recent
// The File menu keeps up to 10 recently opened files, most recent first.
// The list is persisted across sessions in a PropertiesFile; stale entries
// (file deleted/moved) are skipped on load and dropped when clicked.
static void testOpenRecentFiles()
{
    const juce::File store = makeTempFile ("recent_store.properties");
    store.deleteFile();   // start clean (leftovers from earlier runs)

    auto makeWav = [] (const juce::String& name, double freq) -> juce::File
    {
        const juce::File f = makeTempFile (name);
        writeWav (f, 1, 8000.0, 2000, [freq] (int, int64_t smp, double rate)
                  { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                   * freq * (double) smp / rate); });
        return f;
    };

    const juce::File a = makeWav ("recent_a.wav", 100.0);
    const juce::File b = makeWav ("recent_b.wav", 200.0);

    // --- core list behaviour ------------------------------------------------
    std::vector<juce::File> extra;
    {
        Application app (store);
        CHECK (app.recentFiles.empty(), "fresh store: no recent files");

        CHECK (app.openFile (a), "open A");
        CHECK ((int) app.recentFiles.size() == 1 && app.recentFiles[0] == a,
              "A is the only recent file");

        CHECK (app.openFile (b), "open B");
        CHECK ((int) app.recentFiles.size() == 2 && app.recentFiles[0] == b
              && app.recentFiles[1] == a, "B is most recent, then A");

        // Re-opening an already-loaded file moves it to the front (no duplicate).
        CHECK (app.openFile (a), "re-open A (already loaded)");
        CHECK ((int) app.recentFiles.size() == 2 && app.recentFiles[0] == a
              && app.recentFiles[1] == b, "re-opening A moves it to the front");

        // Fill past the cap: nine more opens push the oldest entries off.
        for (int i = 0; i < 9; ++i)
            extra.push_back (makeWav ("recent_x" + juce::String (i) + ".wav", 300.0 + i * 10));
        for (auto& f : extra)
            CHECK (app.openFile (f), "open filler file");

        CHECK ((int) app.recentFiles.size() == Application::kMaxRecentFiles,
              "recent list is capped at 10 entries");
        CHECK (app.recentFiles[0] == extra.back(), "most recently opened file is first");
        bool bGone = true;
        for (auto& f : app.recentFiles)
            if (f == b) bGone = false;
        CHECK (bGone, "oldest entry fell off the list");
    }

    // --- persistence across instances ----------------------------------------
    {
        Application app2 (store);
        CHECK ((int) app2.recentFiles.size() == Application::kMaxRecentFiles,
              "recent list survives a restart");
        // Expected: x8..x0 then a (a was the oldest survivor).
        bool orderOk = app2.recentFiles.size() == 10 && app2.recentFiles[9] == a;
        for (size_t i = 0; orderOk && i < extra.size(); ++i)
            if (!(app2.recentFiles[i] == extra[extra.size() - 1 - i]))
                orderOk = false;
        CHECK (orderOk, "persisted list keeps most-recent-first order");

        app2.clearRecentFiles ();
        CHECK (app2.recentFiles.empty(), "clear empties the list");
    }
    {
        Application app3 (store);
        CHECK (app3.recentFiles.empty(), "cleared list stays cleared after restart");
    }

    // --- stale entries are skipped on load ------------------------------------
    {
        juce::PropertiesFile pf (store, juce::PropertiesFile::Options());
        pf.setValue ("recent_1", "/nonexistent/definitely_missing.wav");
        pf.save ();

        Application app4 (store);
        CHECK (app4.recentFiles.empty(), "stale recent entries are skipped on load");
    }

    // --- menu handler: re-open a closed file, drop stale entries --------------
    {
        const juce::File c = makeWav ("recent_c.wav", 900.0);
        Application app5 (store);   // store is empty at this point
        CHECK (app5.openFile (c), "open C");
        app5.closeFile ();          // close it again so the menu item re-opens it

        juce::MessageManager* mm = juce::MessageManager::getInstance();
        std::unique_ptr<MainWindow> win (new MainWindow (app5, "recent files test"));

        win->menuItemSelected (MainWindow::MenuIDs::RecentBase + 0, 0);
        CHECK (app5.numDocs() == 1 && app5.audioEngine().getFile() == c,
              "clicking a recent file re-opens it");

        // Stale entry: open D, delete the file from disk, then click its item.
        const juce::File d = makeWav ("recent_d.wav", 950.0);
        CHECK (app5.openFile (d), "open D");
        d.deleteFile ();            // simulate the file being moved or deleted
        win->menuItemSelected (MainWindow::MenuIDs::RecentBase + 0, 0);   // D is first now
        CHECK ((int) app5.recentFiles.size() == 1 && app5.recentFiles[0] == c,
              "stale recent entry is dropped when clicked");

        win.reset();   // destroy the window while its MessageManager is still alive
        delete mm;
    }

    a.deleteFile (); b.deleteFile (); store.deleteFile ();
}

// ------------------------------------------------- change gain dialog
// When the Effects > Change Gain... dialog opens, its field must already be
// focused with "0.0" selected so that a new value can be typed in right away.
static void testChangeGainDialog()
{
    const juce::File wav = makeTempFile ("gain_dialog.wav");
    writeWav (wav, 1, 8000.0, 4000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for gain dialog test");
    app.setSelection ({ 0.1, 0.2 });

    // A MessageManager is required before constructing the top-level window;
    // it must still be alive when the window is destroyed.
    juce::MessageManager* mm = juce::MessageManager::getInstance();
    std::unique_ptr<MainWindow> win (new MainWindow (app, "gain dialog test"));

    // Build the dialog without running it modally: the field must be
    // keyboard-focusable with its initial text fully selected. (Actual focus
    // acquisition needs OS foreground, which a background test process does
    // not have; that path is verified in the live GUI.)
    juce::AlertWindow* dlg = win->createGainDialog ();
    CHECK (dlg != nullptr, "gain dialog created");
    if (dlg != nullptr)
    {
        auto* editor = dlg->getTextEditor ("gainDb");
        CHECK (editor != nullptr, "gain field exists");
        if (editor != nullptr)
        {
            CHECK (editor->getWantsKeyboardFocus (), "gain field is keyboard-focusable on open");
            CHECK (editor->getHighlightedRegion () == juce::Range<int> (0, 3),
                  "initial value is fully pre-selected");

            // Replicate showChangeGainDialog(): enter modal state WITHOUT taking
            // focus, then grab the field while the window is showing. JUCE
            // ignores focus grabs for components that are not visible yet,
            // so this order is what makes typing work immediately.
            dlg->enterModalState (false);
            editor->grabKeyboardFocus();
            CHECK (editor->hasKeyboardFocus (false),
                   "field has keyboard focus once the dialog is showing");
            dlg->exitModalState (0);
        }
        delete dlg;
    }

    // Full user flow: open the dialog, type "6", press Enter -> OK. A helper
    // thread injects the keystrokes into the modal loop exactly like a user:
    // '6' must replace the pre-selected "0.0" and Return must trigger OK.
    {
        juce::AlertWindow* w = win->createGainDialog ();
        CHECK (w != nullptr, "gain dialog created for full-flow test");
        if (w != nullptr)
        {
            auto* ed = w->getTextEditor ("gainDb");

            // Same sequence as showChangeGainDialog(): modal state without a
            // focus steal, then grab the field while the window is showing.
            w->enterModalState (false);
            if (ed != nullptr) ed->grabKeyboardFocus ();

            std::atomic<bool> injected { false };
            std::thread helper ([&]
            {
                // Wait until the field actually has keyboard focus...
                for (int i = 0; i < 500 && !injected.load(); ++i)
                {
                    auto* ed2 = dynamic_cast<juce::TextEditor*> (
                        juce::Component::getCurrentlyFocusedComponent ());
                    if (ed2 != nullptr)
                    {
                        // ...then type '6' and press Enter on the message thread.
                        juce::MessageManager::callAsync ([&]
                        {
                            auto* e2 = static_cast<juce::TextEditor*> (
                                juce::Component::getCurrentlyFocusedComponent ());
                            if (e2 != nullptr)
                            {
                                // keyCode '6' with text character '6'.
                                e2->keyPressed (juce::KeyPress ('6', {}, '6'));
                                // Return is not consumed by the field, so in a
                                // real event it bubbles to the window, whose
                                // keyPressed() triggers the OK button; do that
                                // same trigger here.
                                if (auto* ok = w->getButton ("OK")) ok->triggerClick ();
                            }
                        });
                        injected.store (true);
                    }
                    std::this_thread::sleep_for (std::chrono::milliseconds (20));
                }
            });

            const int result = w->runModalLoop ();
            helper.join ();
            const juce::String text = w->getTextEditorContents ("gainDb");
            delete w;

            CHECK (injected.load(), "helper thread found the focused field");
            CHECK (result == 1, "Enter submits the dialog via OK");
            CHECK (text.trim() == "6",
                   "typing replaced the pre-selected value instead of appending");
        }
    }

    win.reset();   // destroy the window while its MessageManager is still alive
    delete mm;
    wav.deleteFile ();
}

// ------------------------------------------------------------------ scale calibration
// Per-file physical-unit calibration: "2 Pascal" means full-scale (|1.0|) is
// 2 Pa, so relative-dB readouts become absolute dB-SPL (ref 20 uPa). Volts ->
// dBV (ref 1 V), g -> dB/g (ref 1 g); None leaves plain relative dB.
static void testScaleCalibration()
{
    using U = ScaleCalibration::Unit;

    // Pascal: offset = 20*log10(2 / 2e-5) = exactly 100 dB.
    ScaleCalibration pa;
    pa.unit = U::Pascal;
    pa.fullScaleValue = 2.0;
    CHECK (pa.isCalibrated(), "pascal calibration is active");
    CHECK (std::abs (pa.levelOffsetDb() - 100.0) < 0.01, "2 Pa full scale is +100 dB-SPL offset");
    CHECK (pa.levelUnitLabel () == "dB-SPL", "pascal level label is dB-SPL");
    CHECK (pa.unitTitle () == "Pa", "pascal axis title is Pa");
    CHECK (pa.shortDescription () == "2 Pa", "file list shows '2 Pa'");

    // Relative -60 dB -> absolute 40.0 dB-SPL.
    CHECK (ScaleCalibration::formatLevelDb (-60.0, pa) == "40.0 dB-SPL",
           "-60 dB relative reads as ~40 dB-SPL");

    // Volts: ref 1 V -> offset = 20*log10(scale).
    ScaleCalibration v;
    v.unit = U::Volts;
    v.fullScaleValue = 1.0;
    CHECK (std::abs (v.levelOffsetDb ()) < 1e-9, "1 V full scale has zero dBV offset");
    CHECK (v.levelUnitLabel () == "dBV", "volts level label is dBV");
    CHECK (ScaleCalibration::formatLevelDb (-60.0, v) == "-60.0 dBV",
           "relative readout unchanged for a 1 V scale");

    // Grams: ref 1 g -> offset = 20*log10(0.1) = -20 dB/g.
    ScaleCalibration g;
    g.unit = U::Grams;
    g.fullScaleValue = 0.1;
    CHECK (std::abs (g.levelOffsetDb () + 20.0) < 1e-9, "0.1 g full scale is -20 dB/g");
    CHECK (g.shortDescription () == "0.1 g", "file list shows '0.1 g'");

    // None: plain relative dB everywhere.
    ScaleCalibration none;
    CHECK (!none.isCalibrated (), "default calibration is off");
    CHECK (std::abs (none.levelOffsetDb ()) < 1e-9, "no offset without a unit");
    CHECK (none.levelUnitLabel () == "dB", "plain dB label when uncalibrated");
    CHECK (none.shortDescription ().isEmpty (), "no scale text in the file list");
    CHECK (ScaleCalibration::formatLevelDb (-60.0, none) == "-60.0 dB",
           "relative readout unchanged when uncalibrated");

    // Waveform axis: grid line at -12 dB with 2 Pa full scale -> 2*10^-0.6 ~= 0.502 Pa.
    CHECK (ScaleCalibration::formatWaveformAxisValue (-12.0, pa) == "0.502",
           "-12 dB grid line shows the physical value");
    CHECK (ScaleCalibration::formatWaveformAxisValue (-12.0, none) == "-12",
           "uncalibrated axis keeps integer dB labels");

    // Linear probe: 0.84 of full scale with 2 Pa -> 1.68 Pa.
    CHECK (ScaleCalibration::formatLinearValue (0.84, pa) == "1.68 Pa",
           "probe shows physical value + unit");
    CHECK (ScaleCalibration::formatLinearValue (0.84, none) == "0.84",
           "uncalibrated probe stays normalized");

    // FFT axis labels: grid line at -12 dB with the pascal offset -> 88.
    CHECK (ScaleCalibration::formatAxisLabel (-12, pa) == "88", "FFT axis shows absolute level");
    CHECK (ScaleCalibration::formatAxisLabel (-12, none) == "-12",
           "uncalibrated FFT axis unchanged");

    // Volts/grams are linear units: spectral readouts show plain V/g values.
    CHECK (v.usesLinearLevels (), "volts use linear level readouts");
    CHECK (g.usesLinearLevels (), "grams use linear level readouts");
    CHECK (!pa.usesLinearLevels (), "pascal keeps dB-SPL readouts");
    CHECK (!none.usesLinearLevels (), "uncalibrated keeps plain dB");

    CHECK (v.spectralUnitLabel () == "V", "volts spectral unit label is V");
    CHECK (g.spectralUnitLabel () == "g", "grams spectral unit label is g");
    CHECK (pa.spectralUnitLabel () == "dB-SPL", "pascal spectral unit label unchanged");
    CHECK (none.spectralUnitLabel () == "dB", "uncalibrated spectral unit label unchanged");

    // Linear level values: ~3 significant digits, trimmed.
    CHECK (ScaleCalibration::formatLevelValue (1.0) == "1", "full scale formats as 1");
    CHECK (ScaleCalibration::formatLevelValue (0.251) == "0.251", "mid value keeps 3 digits");
    CHECK (ScaleCalibration::formatLevelValue (9.76e-4) == "0.000976", "small value fixed notation");
    CHECK (ScaleCalibration::formatLevelValue (3.16e-5) == "0.0000316", "tiny value keeps 3 digits");
    CHECK (ScaleCalibration::formatLevelValue (12345.0) == "12300", "large value rounded to sig figs");
    CHECK (ScaleCalibration::formatLevelValue (0.0) == "0", "zero formats as 0");

    // Spectral level readouts: volts/grams -> linear, pascal -> dB-SPL.
    CHECK (ScaleCalibration::formatSpectralLevel (-20.0, v) == "0.1 V",
           "-20 dB relative reads as 0.1 V for a 1 V scale");
    CHECK (ScaleCalibration::formatSpectralLevel (-80.0, v) == "0.0001 V",
           "-80 dB relative reads as 0.0001 V for a 1 V scale");
    CHECK (ScaleCalibration::formatSpectralLevel (0.0, g) == "0.1 g",
           "full scale reads as the gram value");
    CHECK (ScaleCalibration::formatSpectralLevel (-20.0, pa) == "80.0 dB-SPL",
           "pascal spectral readout stays in dB-SPL");
    CHECK (ScaleCalibration::formatSpectralLevel (-43.5, none) == "-43.5 dB",
           "uncalibrated spectral readout stays plain dB");

    // FFT axis labels for linear units carry the unit suffix.
    CHECK (ScaleCalibration::formatAxisLabel (0, v) == "1 V", "volts full-scale grid label");
    CHECK (ScaleCalibration::formatAxisLabel (-24, v) == "0.0631 V",
           "-24 dB grid line shows the linear value with unit");

    // Value formatting trims trailing zeros.
    CHECK (ScaleCalibration::formatPhysicalValue (2.0) == "2", "trailing zeros trimmed (2)");
    CHECK (ScaleCalibration::formatPhysicalValue (0.5) == "0.5", "trailing zeros trimmed (0.5)");
    CHECK (ScaleCalibration::formatPhysicalValue (123.456) == "123",
           "large values get no decimals");
}

// ------------------------------------------------- set scale dialog
static void testSetScaleDialog()
{
    const juce::File wav = makeTempFile ("scale_dialog.wav");
    writeWav (wav, 1, 8000.0, 4000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for scale dialog test");

    // A MessageManager is required before grabbing keyboard focus in the dialog.
    juce::MessageManager* mm = juce::MessageManager::getInstance();
    std::unique_ptr<FileListPanel> panel (new FileListPanel (app));

    FileDoc& d = *app.docs[0];
    juce::AlertWindow* dlg = panel->createScaleDialog (d);
    CHECK (dlg != nullptr, "scale dialog created");
    if (dlg != nullptr)
    {
        auto* combo = dlg->getComboBoxComponent ("unit");
        CHECK (combo != nullptr, "units combo exists");
        if (combo != nullptr)
        {
            CHECK (combo->getNumItems () == 4, "four unit choices");
            // getItemText() takes a 0-based index in JUCE 9.
            CHECK (combo->getItemText (0) == "None" && combo->getItemText (1) == "Volts"
                  && combo->getItemText (2) == "Pascal" && combo->getItemText (3) == "g",
                  "unit items in the expected order");
            CHECK (combo->getSelectedId () == 1, "defaults to None for an uncalibrated file");
        }

        auto* editor = dlg->getTextEditor ("value");
        CHECK (editor != nullptr, "scale value field exists");
        if (editor != nullptr)
        {
            CHECK (editor->getHighlightedRegion () == juce::Range<int> (0, 3),
                  "initial '1.0' value is fully pre-selected");
            CHECK (!editor->isEnabled (), "value field disabled while the unit is None");
        }

        // Picking a unit re-enables the value field; getWantsKeyboardFocus()
        // only reports true for enabled components, so check it here.
        if (combo != nullptr && editor != nullptr)
        {
            combo->setSelectedId (3, juce::NotificationType::sendNotificationSync);
            CHECK (editor->isEnabled (), "value field enabled once a unit is picked");
            CHECK (editor->getWantsKeyboardFocus (),
                  "value field is keyboard-focusable when enabled");
        }

        delete dlg;
    }

    // The document's calibration is untouched until the dialog is confirmed.
    CHECK (!d.scale.isCalibrated (), "dialog does not modify the doc before OK");

    panel.reset();
    delete mm;
    wav.deleteFile ();
}

// ------------------------------------- spectral controls level scale labels
// The gradient preview in the Spectral Controls tab shows the absolute level
// of each end of the colour map above it, in the active file's unit. The
// labels must follow both the dB-range slider and Set Scale calibration.
static void testSpectralScaleLabels()
{
    const juce::File wav = makeTempFile ("scale_labels.wav");
    writeWav (wav, 1, 8000.0, 4000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for scale label test");

    SpectralControlsPanel panel (app);
    panel.setSize (240, 170);

    // The preview sits at y 32..88 inside the panel (6 px padding plus a 28 px
    // colour-map row); its top 18 px are the level label row.
    const juce::Rectangle<int> labelRow (6, 32, 240 - 12, 18);

    auto snapshotLabels = [&] () -> std::vector<juce::Colour>
    {
        const juce::Image img = panel.createComponentSnapshot (panel.getLocalBounds());
        std::vector<juce::Colour> px;
        px.reserve ((size_t) labelRow.getWidth() * labelRow.getHeight());
        for (int y = labelRow.getY(); y < labelRow.getBottom(); ++y)
            for (int x = labelRow.getX(); x < labelRow.getRight(); ++x)
                px.push_back (img.getPixelAt (x, y));
        return px;
    };

    auto countTextPixels = [] (const std::vector<juce::Colour>& px) -> int
    {
        int n = 0;
        for (auto c : px)
            if (c.getBrightness () > 0.5f) ++n;   // lightgrey text on the dark panel
        return n;
    };

    auto labelsDiffer = [] (const std::vector<juce::Colour>& a,
                            const std::vector<juce::Colour>& b) -> int
    {
        if (a.size() != b.size())
            return (int) std::max (a.size(), b.size());
        int n = 0;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) ++n;
        return n;
    };

    const auto base = snapshotLabels();
    CHECK (countTextPixels (base) > 20, "level labels drawn above the gradient");

    // Changing the level window must change the label values.
    app.settings.dbMinDb = -100.0f;
    app.settings.dbMaxDb = 0.0f;
    panel.refresh();
    const auto afterRange = snapshotLabels();
    CHECK (countTextPixels (afterRange) > 20, "labels still drawn after dB range change");
    CHECK (labelsDiffer (base, afterRange) > 5,
          "dB range change updates the labels");

    // Calibrating the file switches to absolute units (dB-SPL).
    FileDoc& d = *app.docs[0];
    d.scale.unit = ScaleCalibration::Unit::Pascal;
    d.scale.fullScaleValue = 2.0;
    panel.refresh();
    const auto afterScale = snapshotLabels();
    CHECK (countTextPixels (afterScale) > 20, "labels still drawn after calibration");
    CHECK (labelsDiffer (afterRange, afterScale) > 5,
          "calibration updates the labels to absolute units");

    // Exact label values: uncalibrated files show plain relative dB.
    {
        ScaleCalibration none;
        const auto l = SpectralControlsPanel::levelScaleLabels (-80.0, 0.0, none);
        CHECK (l[0] == "-80.0 dB", "uncalibrated bottom label");
        CHECK (l[1] == "-40.0 dB", "uncalibrated middle label");
        CHECK (l[2] == "0.0 dB",   "uncalibrated full-scale label");
    }

    // 2 Pa calibration: +100 dB offset, labels in dB-SPL.
    {
        ScaleCalibration sc;
        sc.unit = ScaleCalibration::Unit::Pascal;
        sc.fullScaleValue = 2.0;
        const auto l = SpectralControlsPanel::levelScaleLabels (-80.0, 0.0, sc);
        CHECK (l[0] == "20.0 dB-SPL",  "calibrated bottom label");
        CHECK (l[1] == "60.0 dB-SPL",  "calibrated middle label");
        CHECK (l[2] == "100.0 dB-SPL", "calibrated full-scale label");
    }

    // 1 Pa calibration: +93.98 dB offset, rounded to one decimal.
    {
        ScaleCalibration sc;
        sc.unit = ScaleCalibration::Unit::Pascal;
        sc.fullScaleValue = 1.0;
        const auto l = SpectralControlsPanel::levelScaleLabels (-80.0, 0.0, sc);
        CHECK (l[0] == "14.0 dB-SPL",  "1 Pa bottom label");
        CHECK (l[1] == "54.0 dB-SPL",  "1 Pa middle label");
        CHECK (l[2] == "94.0 dB-SPL",  "1 Pa full-scale label");
    }

    // Volts/grams are linear units: labels show plain V/g values, not dB.
    {
        ScaleCalibration sc;
        sc.unit = ScaleCalibration::Unit::Volts;
        sc.fullScaleValue = 1.0;
        const auto l = SpectralControlsPanel::levelScaleLabels (-80.0, 0.0, sc);
        CHECK (l[0] == "0.0001 V", "volts bottom label is linear");
        CHECK (l[1] == "0.01 V",   "volts middle label is linear");
        CHECK (l[2] == "1 V",      "volts full-scale label is the scale value");
    }

    {
        ScaleCalibration sc;
        sc.unit = ScaleCalibration::Unit::Grams;
        sc.fullScaleValue = 2.0;
        const auto l = SpectralControlsPanel::levelScaleLabels (-80.0, 0.0, sc);
        CHECK (l[0] == "0.0002 g", "grams bottom label is linear");
        CHECK (l[1] == "0.02 g",   "grams middle label is linear");
        CHECK (l[2] == "2 g",      "grams full-scale label is the scale value");
    }

    // The labels must follow the level window edges.
    {
        ScaleCalibration none;
        const auto l = SpectralControlsPanel::levelScaleLabels (-120.0, 0.0, none);
        CHECK (l[0] == "-120.0 dB", "wider range bottom label");
        CHECK (l[2] == "0.0 dB",    "full-scale label independent of range");
    }

    wav.deleteFile ();
}

// ------------------------------------- spectral dB window controls
// The Spectral Controls tab exposes the colour-window edges directly: the
// slider/text field set the range (top edge fixed, slider reaches 160 dB,
// text accepts any value > 1), and the two labels over the gradient preview
// are editable min/max levels in the file's unit.
static void testSpectralDbWindow()
{
    const juce::File wav = makeTempFile ("db_window.wav");
    writeWav (wav, 1, 8000.0, 4000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for dB window test");

    SpectralControlsPanel panel (app);
    panel.setSize (260, 170);

    // Defaults: window [-80, 0], range 80.
    CHECK (app.settings.dbMinDb == -80.0f && app.settings.dbMaxDb == 0.0f, "default window");
    CHECK (panel.getDbRangeSliderValue() == 80.0, "slider shows default range");
    CHECK (panel.getDbMinText() == "-80.0 dB", "default min label");
    CHECK (panel.getDbMaxText() == "0.0 dB",   "default max label");

    // The slider reaches 160 dB: the top edge stays fixed, the bottom moves down.
    CHECK (panel.applyDbRangeValue (160.0), "range 160 accepted");
    CHECK (app.settings.dbMinDb == -160.0f && app.settings.dbMaxDb == 0.0f, "range 160 window");
    CHECK (panel.getDbRangeSliderValue() == 160.0, "slider at its maximum");
    CHECK (panel.getDbMinText() == "-160.0 dB", "min label follows the slider");

    // Typed range: any value greater than 1 dB is accepted...
    CHECK (panel.applyDbRangeText ("5"), "small typed range accepted");
    CHECK (app.settings.dbMinDb == -5.0f && app.settings.dbMaxDb == 0.0f, "range 5 window");

    // ...but values <= 1 are rejected and the settings stay untouched.
    const float minBefore = app.settings.dbMinDb;
    CHECK (!panel.applyDbRangeText ("1"),   "range of exactly 1 dB rejected");
    CHECK (!panel.applyDbRangeText ("0.5"), "sub-1 dB range rejected");
    CHECK (!panel.applyDbRangeText ("abc"), "non-numeric range rejected");
    CHECK (app.settings.dbMinDb == minBefore, "rejected ranges leave settings untouched");

    // Ranges beyond the slider's reach still apply to the settings.
    CHECK (panel.applyDbRangeText ("300"), "range above 160 accepted via text");
    CHECK (app.settings.dbMinDb == -300.0f && app.settings.dbMaxDb == 0.0f, "range 300 window");
    CHECK (panel.getDbRangeSliderValue() == 160.0, "slider clamps at its maximum");

    // Editable bottom level: uncalibrated files take plain relative dB.
    CHECK (panel.applyDbMinText ("-90"), "min text accepted");
    CHECK (app.settings.dbMinDb == -90.0f && app.settings.dbMaxDb == 0.0f, "min edit window");

    // Editable top level: may sit above full scale for headroom.
    CHECK (panel.applyDbMaxText ("6"), "max text accepted");
    CHECK (app.settings.dbMinDb == -90.0f && app.settings.dbMaxDb == 6.0f, "max edit window");

    // The range follows the edges: 96 dB here.
    CHECK (std::abs ((double) app.settings.dbRange() - 96.0) < 1e-3, "range derived from edges");

    // Edits that would collapse the window to <= 1 dB are rejected.
    CHECK (!panel.applyDbMinText ("5"),    "min leaving only a 1 dB window rejected");
    CHECK (!panel.applyDbMaxText ("-89"),  "max leaving only a 1 dB window rejected");
    CHECK (app.settings.dbMinDb == -90.0f && app.settings.dbMaxDb == 6.0f,
          "rejected edge edits leave settings untouched");

    // Calibrated volts file: the editors speak in linear V values.
    FileDoc& d = *app.docs[0];
    d.scale.unit = ScaleCalibration::Unit::Volts;
    d.scale.fullScaleValue = 1.0;
    panel.refresh();

    CHECK (panel.applyDbMinText ("0.01"), "volts min accepted");
    CHECK (std::abs ((double) app.settings.dbMinDb + 40.0) < 1e-3, "min converted to -40 dB");
    CHECK (panel.getDbMinText() == "0.01 V", "min label reformatted in volts");

    // Zero/negative linear values are not valid levels.
    CHECK (!panel.applyDbMinText ("0"),  "zero volts rejected");
    CHECK (!panel.applyDbMinText ("-2"), "negative volts rejected");
    CHECK (std::abs ((double) app.settings.dbMinDb + 40.0) < 1e-3, "rejected volts edits ignored");

    // Pascal calibration: editors take absolute dB-SPL (+100 dB offset for 2 Pa).
    d.scale.unit = ScaleCalibration::Unit::Pascal;
    d.scale.fullScaleValue = 2.0;
    panel.refresh();
    CHECK (panel.applyDbMaxText ("106 dB-SPL"), "dB-SPL max accepted");
    CHECK (std::abs ((double) app.settings.dbMaxDb - 6.0) < 1e-3, "max converted to relative dB");

    wav.deleteFile ();
}

// ------------------------------------------------- multi-file support
// Multiple files can be open at once. Each file remembers its own playhead,
// selection, markers and undo history; switching restores them. Snapshots are
// shared across all files but each one tracks the document it was stored
// from; closing a file clears only that file's snapshot entries.
// ---------------- Right-drag box zoom (waveform + spectral views) ----------------
// Horizontal right-drag: both views zoom to the dragged time range.
// Vertical right-drag: waveform amplitude axis / spectral frequency axis is
// scaled so the dragged span fills the view. Plain right-click = context menu
// (no state change when no action handler is attached).
// The view handlers only read e.x/e.y/e.mods; the Desktop's default mouse
// source is valid headlessly (MouseInputSourceList adds one at construction).
static juce::MouseEvent mkMouse (juce::Component& c, int x, int y, juce::ModifierKeys mods)
{
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
                             { (float) x, (float) y }, mods,
                             0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                             &c, &c, juce::Time(), { (float) x, (float) y },
                             juce::Time(), 1, false);
}

static void testRightDragZoom()
{
    const juce::File wav = makeTempFile ("rightdrag_zoom.wav");
    writeWav (wav, 2, 8000.0, 44100, [] (int ch, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * (ch == 0 ? 300.0 : 600.0) * (double) f / rate);
    });

    Application app;
    CHECK (app.audioEngine().loadFile (wav), "load wav for right-drag zoom test");

    const juce::ModifierKeys rmb (juce::ModifierKeys::rightButtonModifier);

    // --- Waveform: horizontal drag -> time window ---
    {
        WaveformView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        const double e0 = view.xToTime (100), e1 = view.xToTime (300);
        CHECK (e1 > e0, "expected time range is non-degenerate");

        view.mouseDown (mkMouse (view, 100, 60, rmb));
        view.mouseDrag (mkMouse (view, 300, 70, rmb));
        view.mouseUp   (mkMouse (view, 300, 70, rmb));

        CHECK (std::abs (view.getViewStart() - e0) < 1e-9
              && std::abs (view.getViewEnd() - e1) < 1e-9,
              "horizontal right-drag zooms to the dragged time range");
    }

    // --- Waveform: vertical drag -> amplitude zoom in ---
    {
        WaveformView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        const double before = view.getVisibleAmpHalf();
        // Plot is y=24..199 (height 176); a 44 px span must zoom in by x4.
        view.mouseDown (mkMouse (view, 200, 50, rmb));
        view.mouseDrag (mkMouse (view, 203, 94, rmb));   // dx=3 < dy=44 -> vertical
        view.mouseUp   (mkMouse (view, 203, 94, rmb));

        CHECK (std::abs (view.getVisibleAmpHalf() - before / 4.0) < 1e-9,
               "vertical right-drag scales the amplitude axis to the dragged span");
    }

    // --- Spectral: horizontal drag -> time window; vertical drag -> frequency range ---
    {
        SpectralView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        const double e0 = view.xToTime (100), e1 = view.xToTime (300);
        view.mouseDown (mkMouse (view, 100, 60, rmb));
        view.mouseDrag (mkMouse (view, 300, 70, rmb));
        view.mouseUp   (mkMouse (view, 300, 70, rmb));

        CHECK (std::abs (view.getViewStart() - e0) < 1e-9
              && std::abs (view.getViewEnd() - e1) < 1e-9,
              "spectral horizontal right-drag zooms to the dragged time range");
    }

    {
        SpectralView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        // Default axis: [20 Hz .. min(20 kHz, Nyquist=4 kHz)] on a log scale;
        // one visible channel row spans the full 200 px height.
        const double fmin = 20.0, fmax = 4000.0;
        const double lmin = std::log (fmin), lmax = std::log (fmax);
        auto freqAt = [lmin, lmax] (double y) { return std::exp (lmin + (1.0 - y / 200.0) * (lmax - lmin)); };
        const double fTop = freqAt (50.0), fBot = freqAt (150.0);

        view.mouseDown (mkMouse (view, 200, 50, rmb));
        view.mouseDrag (mkMouse (view, 203, 150, rmb));   // dx=3 < dy=100 -> vertical
        view.mouseUp   (mkMouse (view, 203, 150, rmb));

        CHECK (std::abs (view.getFreqMin() - fBot) / fBot < 1e-9
              && std::abs (view.getFreqMax() - fTop) / fTop < 1e-9,
               "spectral vertical right-drag sets the frequency axis to the dragged span");
    }

    // --- Plain right-click: no state change, no playhead move ---
    {
        WaveformView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        view.mouseDown (mkMouse (view, 150, 80, rmb));
        view.mouseUp   (mkMouse (view, 150, 80, rmb));

        CHECK (view.getViewStart() == 0.0 && view.getViewEnd() == 4.0,
               "plain right-click leaves the time window unchanged");
        CHECK (app.playhead == 0.0, "right-button does not move the playhead");
    }

    wav.deleteFile();
}

// ---------------- Time-pan speed + reversal responsiveness ----------------
// Middle-drag (and left-drag on the time ruler) pans the shared time window
// at half cursor speed, incrementally: deltas absorbed by an edge clamp do
// not accumulate, so reversing direction responds immediately instead of
// first unwinding a dead zone.
static void testTimePanSpeedAndReversal()
{
    const juce::File wav = makeTempFile ("time_pan.wav");
    writeWav (wav, 1, 8000.0, 44100, [] (int, int64_t f, double rate)
    { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                      * 440.0 * (double) f / rate); });

    Application app;
    CHECK (app.audioEngine().loadFile (wav), "load wav for time-pan test");

    const juce::ModifierKeys mmb (juce::ModifierKeys::middleButtonModifier);
    const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);

    // Plot: x in [60, 400) -> width 340 px. Window [2, 6] s (w = 4 s).

    // --- Waveform middle-drag: half speed, edge pin, immediate reversal ---
    {
        WaveformView view (app);
        view.setSize (400, 200);
        view.setViewRange (2.0, 6.0);
        view.setVisibleChannels ({ 0 });

        // Cursor starts at the plot centre: x = 230 -> t = 4 s.
        view.mouseDown (mkMouse (view, 230, 100, mmb));

        // Drag right to the plot edge (x = 400): dx = +170 px. At half speed
        // the content moves 85 px = 1.0 s -> window [1, 5]. (At 1:1 it would
        // already be pinned at [0, 4].)
        view.mouseDrag (mkMouse (view, 400, 100, mmb));
        CHECK (std::abs (view.getViewStart() - 1.0) < 1e-9,
               "middle-drag pans at half cursor speed");

        // Keep dragging right past the view edge: another dx = +170 px ->
        // another 1.0 s -> pinned at [0, 4].
        view.mouseDrag (mkMouse (view, 570, 100, mmb));
        CHECK (std::abs (view.getViewStart() - 0.0) < 1e-9,
               "panning past the edge pins at file start");

        // Reverse direction: a small left drag must move the window back
        // immediately (no dead-zone unwinding). dx = -20 px -> +20*0.5*4/340 s.
        view.mouseDrag (mkMouse (view, 550, 100, mmb));
        CHECK (std::abs (view.getViewStart() - 20.0 * 0.5 * 4.0 / 340.0) < 1e-9,
               "reversing direction responds immediately after edge pin");

        view.mouseUp (mkMouse (view, 550, 100, mmb));
    }

    // --- Waveform ruler drag: same half speed ---
    {
        WaveformView view (app);
        view.setSize (400, 200);
        view.setViewRange (2.0, 6.0);
        view.setVisibleChannels ({ 0 });

        // Left-drag on the time ruler (y < 24) pans at half speed too.
        view.mouseDown (mkMouse (view, 230, 5, lmb));
        view.mouseDrag (mkMouse (view, 315, 5, lmb));   // dx = +85 px -> -0.5 s
        CHECK (std::abs (view.getViewStart() - 1.5) < 1e-9,
               "ruler drag pans at half cursor speed");

        view.mouseUp (mkMouse (view, 315, 5, lmb));
    }

    // --- Spectral middle-drag: same half speed ---
    {
        SpectralView view (app);
        view.setSize (400, 200);
        view.setViewRange (2.0, 6.0);
        view.setVisibleChannels ({ 0 });

        view.mouseDown (mkMouse (view, 230, 100, mmb));
        view.mouseDrag (mkMouse (view, 400, 100, mmb));   // dx = +170 px -> -1.0 s
        CHECK (std::abs (view.getViewStart() - 1.0) < 1e-9,
               "spectral middle-drag pans at half cursor speed");

        view.mouseUp (mkMouse (view, 400, 100, mmb));
    }

    wav.deleteFile();
}

// ---------------- Time axis adaptive ticks ----------------
// The time ruler picks a "nice" step at every zoom level: sub-second / ms /
// us values when zoomed in, and compact minute/hour labels when zoomed out on
// long files. Major ticks are subdivided into five unlabeled minor ticks.

static void testTimeAxisTicks()
{
    auto approx = [] (double a, double b) { return std::abs (a - b) <= b * 1e-9; };

    // --- Step selection at representative zoom levels ---
    CHECK (approx (niceTimeStep (45.0), 2.0), "20 s span @900 px: 2 s steps");
    CHECK (approx (niceTimeStep (900.0 / 10800.0), 1800.0), "3 h file @900 px: 30 m steps");
    CHECK (approx (niceTimeStep (4500.0), 0.02), "0.2 s span: 20 ms steps");
    CHECK (approx (niceTimeStep (45000.0), 0.002), "20 ms span: 2 ms steps");
    CHECK (approx (niceTimeStep (1e6), 1e-4), "~1 ms span: 100 us steps");

    // --- Label formatting ---
    CHECK (formatTimeTickLabel (45.0, 1.0) == "45", "whole seconds label");
    CHECK (formatTimeTickLabel (300.0, 60.0) == "5m", "minutes compact label");
    CHECK (formatTimeTickLabel (900.0, 900.0) == "15m", "odd minutes compact label");
    CHECK (formatTimeTickLabel (3600.0, 3600.0) == "1h", "hours compact label");
    CHECK (formatTimeTickLabel (7200.0, 1800.0) == "2h", "multiple hours label");
    CHECK (formatTimeTickLabel (0.2, 0.2) == "0.2", "tenths of a second label");
    CHECK (formatTimeTickLabel (0.05, 0.05) == "0.05", "hundredths of a second label");
    CHECK (formatTimeTickLabel (0.002, 0.002) == "2ms", "millisecond label");
    CHECK (formatTimeTickLabel (0.0005, 0.0005) == "500\u00b5s", "microsecond label");
    CHECK (formatTimeTickLabel (9.242, 0.002) == "9.242", "ms step away from start: decimal seconds");
    CHECK (formatTimeTickLabel (12.5, 0.0005) == "12.5000", "sub-ms step away from start: 4 digits");
    CHECK (formatTimeTickLabel (0.0, 1.0) == "0", "origin label is plain 0");

    // --- Rendered ruler: majors + minors at coarse and fine zooms ---
    {
        const juce::File sil = makeTempFile ("time_axis_silence.wav");
        writeWav (sil, 1, 100.0, 10800 * 100, [] (int, int64_t, double) { return 0.0f; });

        Application app2;
        CHECK (app2.openFile (sil), "open long silence for time-axis test");

        WaveformView view (app2);
        view.setSize (960, 300);   // plot width = 960 - kLeftAxisWidth(60) = 900 px
        view.setVisibleChannels ({ 0 });
        view.setViewRange (0.0, 10800.0);   // full 3 h file

        juce::Image img (juce::Image::ARGB, 960, 300, true);
        { juce::Graphics g (img); view.paint (g); }

        auto xOf = [&] (double t) -> int { return (int) std::floor ((float) view.timeToX (t)); };
        // Max brightness in a small window around the expected tick column.
        auto brightAt = [&] (const juce::Image& im, double t, int y0, int y1) -> int
        {
            int best = 0;
            for (int dx = -2; dx <= 2; ++dx)
                for (int y = y0; y <= y1; ++y)
                {
                    const auto c = im.getPixelAt (xOf (t) + dx, y);
                    best = std::max (best, (c.getRed() + c.getGreen() + c.getBlue()) / 3);
                }
            return best;
        };

        // Step is 1800 s: major ticks at 0, 1800 ("30m"), 3600 ("1h"), ...
        CHECK (brightAt (img, 1800.0, 16, 23) > 45, "major tick drawn at 30 m");
        CHECK (brightAt (img, 3600.0, 16, 23) > 45, "major tick drawn at 1 h");
        // Rows y=16..19 are above the minor-tick band (y>=20): no major mid-interval.
        CHECK (brightAt (img, 2700.0, 16, 19) < 45, "no major tick mid-interval");
        // Minors subdivide each interval into five: first minor at 1800+360=2160.
        CHECK (brightAt (img, 2160.0, 20, 23) > 45, "intermediate tick between majors");

        // The "1h" label text sits just right of its tick: bright pixels in rows 4..18.
        int labelBright = 0;
        for (int dx = 3; dx < 60; ++dx)
            for (int y = 4; y <= 18; ++y)
            {
                const auto c = img.getPixelAt (xOf (3600.0) + dx, y);
                labelBright = std::max (labelBright, (c.getRed() + c.getGreen() + c.getBlue()) / 3);
            }
        CHECK (labelBright > 120, "hour label text is drawn");

        // Deep zoom: the same ruler must produce ms-scale divisions.
        view.setViewRange (0.0, 0.02);   // 20 ms span -> 2 ms steps
        juce::Image img2 (juce::Image::ARGB, 960, 300, true);
        { juce::Graphics g (img2); view.paint (g); }
        CHECK (brightAt (img2, 0.004, 16, 23) > 45, "deep zoom: major tick at 4 ms");
        CHECK (brightAt (img2, 0.008, 16, 23) > 45, "deep zoom: major tick at 8 ms");

        sil.deleteFile();
    }
}

// ---------------- Marker title clipping ----------------
// A marker's yellow title may run past its own range, but it must stop
// before the next marker starts so that marker's title has room.
static void testMarkerTitleClipping()
{
    const juce::File wav = makeTempFile ("marker_title_clip.wav");
    writeWav (wav, 1, 48000.0, 4 * 48000, [] (int, int64_t, double) { return 0.0f; });

    Application app;
    CHECK (app.openFile (wav), "open wav for marker title clip test");

    MarkerManager mm;
    // Long name: ~132 px wide at font size 10, wider than the gap to B.
    mm.addPointMarker (1.0, juce::String ("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    mm.addPointMarker (1.5, juce::String ("BBBB"));

    WaveformView view (app);
    view.setSize (960, 300);   // plot width = 900 px -> 225 px/s
    view.setVisibleChannels ({ 0 });
    view.setViewRange (0.0, 4.0);
    view.setMarkers (&mm.markers());

    juce::Image img (juce::Image::ARGB, 960, 300, true);
    { juce::Graphics g (img); view.paint (g); }

    const int xA = (int) std::floor ((float) view.timeToX (1.0));   // 285
    const int xB = (int) std::floor ((float) view.timeToX (1.5));   // 397
    CHECK (xB - xA > 60, "markers are close enough to overlap at this zoom");

    // Title row: y = kRulerH+8 .. +21.
    auto brightAt = [&] (int cx, int halfW) -> int
    {
        int best = 0;
        for (int dx = -halfW; dx <= halfW; ++dx)
            for (int y = WaveformView::kRulerH + 8; y <= WaveformView::kRulerH + 21; ++y)
            {
                const auto c = img.getPixelAt (cx + dx, y);
                best = std::max (best, (c.getRed() + c.getGreen() + c.getBlue()) / 3);
            }
        return best;
    };

    CHECK (brightAt (xA + 8, 4) > 60, "marker A title is drawn");
    // The gap between B's line and the start of B's title must be free of
    // A's text: with the old unclipped 120 px box it overflowed into here.
    // (Threshold above the ~48 plot background, below glyph pixels >= 71.)
    CHECK (brightAt (xB + 2, 1) < 60, "A's title stops before marker B starts");
    CHECK (brightAt (xB + 8, 3) > 60, "marker B title is drawn in full");

    wav.deleteFile();
}

// ---------------- Axis drag pan + log minor ticks ----------------
// Left-dragging the left axis of either view pans its vertical range
// (amplitude for the waveform, frequency for the spectrogram) without moving
// the playhead. In log mode the spectral ruler also draws intermediate 2x/5x
// decade ticks in a darker colour than the major decade lines.

static void testAxisPanAndLogTicks()
{
    const juce::File wav = makeTempFile ("axis_pan.wav");
    writeWav (wav, 1, 8000.0, 32000, [] (int, int64_t f, double rate)
    { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                      * 440.0 * (double) f / rate); });

    Application app;
    CHECK (app.audioEngine().loadFile (wav), "load wav for axis pan test");

    const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);

    // --- Waveform: left-drag on the amplitude axis pans vertically ---
    {
        WaveformView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        const double ampPx = view.rowAmpPx();
        CHECK (ampPx > 1.0, "row amplitude scale is positive");

        // x < kLeftAxisWidth selects the vertical-pan drag mode.
        view.mouseDown (mkMouse (view, 20, 50, lmb));
        view.mouseDrag (mkMouse (view, 20, 90, lmb));

        // Dragging down moves the content down with the cursor; panning runs
        // at half speed so a full-scale view does not saturate the +/-2 clamp.
        CHECK (std::abs (view.getVerticalOffset() - (20.0 / ampPx)) < 1e-9,
               "axis drag shifts the vertical offset with the cursor (half speed)");
        CHECK (app.playhead == 0.0, "waveform axis drag does not move the playhead");

        // Dragging back restores the centre.
        view.mouseDown (mkMouse (view, 20, 90, lmb));
        view.mouseDrag (mkMouse (view, 20, 50, lmb));
        CHECK (std::abs (view.getVerticalOffset()) < 1e-9,
               "dragging back restores the vertical offset");
    }

    // --- Spectral: left-drag on the frequency axis pans in log space ---
    {
        SpectralView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        // Zoom to a sub-range first so the pan cannot hit the Nyquist clamp.
        view.setFrequencyRange (100.0, 2000.0);
        const double fmin = view.getFreqMin(), fmax = view.getFreqMax();

        // One visible channel: the row spans the full height.
        const int H = 200;
        const double f0 = view.yToFreq (60.0f, H);   // freq under cursor at drag start
        const double f1 = view.yToFreq (80.0f, H);

        view.mouseDown (mkMouse (view, 20, 60, lmb));
        view.mouseDrag (mkMouse (view, 20, 80, lmb));

        CHECK (std::abs (view.getFreqMin() - fmin * (f0 / f1)) / fmin < 1e-9
              && std::abs (view.getFreqMax() - fmax * (f0 / f1)) / fmax < 1e-9,
               "log axis drag multiplies the range by the cursor frequency ratio");
        CHECK (app.playhead == 0.0, "spectral axis drag does not move the playhead");
    }

    // --- Spectral: linear mode pans by a fixed Hz delta ---
    {
        SpectralView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        Settings s = app.settings;
        s.logFrequency = false;
        view.setSettings (s);
        view.setFrequencyRange (500.0, 3000.0);

        const double fmin = view.getFreqMin(), fmax = view.getFreqMax();
        const int H = 200;
        const double d = view.yToFreq (80.0f, H) - view.yToFreq (100.0f, H);

        view.mouseDown (mkMouse (view, 20, 80, lmb));
        view.mouseDrag (mkMouse (view, 20, 100, lmb));

        CHECK (std::abs (view.getFreqMin() - (fmin + d)) < 1e-6
              && std::abs (view.getFreqMax() - (fmax + d)) < 1e-6,
               "linear axis drag shifts the range by the cursor frequency delta");
    }

    // --- Log ruler: intermediate 2x/5x ticks are drawn darker than decades ---
    // A silent file keeps the background exactly black, so the anti-aliased
    // tick lines can be measured without spectral content interfering.
    {
        const juce::File sil = makeTempFile ("axis_ticks_silence.wav");
        writeWav (sil, 1, 8000.0, 32000, [] (int, int64_t, double) { return 0.0f; });

        Application app2;
        CHECK (app2.openFile (sil), "open silence for tick test");

        SpectralView view (app2);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        // Range [80, 900] Hz: decade tick at 100 Hz plus minor ticks at every
        // other digit per decade (200 ... 900).
        view.setFrequencyRange (80.0, 900.0);
        juce::Image img (juce::Image::ARGB, 400, 200, true);
        { juce::Graphics g (img); view.paint (g); }

        // GDI rasterises a 1 px line at fractional y onto the row below it.
        auto yOf = [&] (double f) -> int
        { return (int) std::floor ((float) view.freqToY (f, 200)); };

        auto brightness = [&] (int yy) -> int
        { const auto c = img.getPixelAt (100, yy); return (c.getRed() + c.getGreen() + c.getBlue()) / 3; };

        // All three tick rows must be visible grey lines on the black ground.
        CHECK (brightness (yOf (100.0)) > 25,
               "decade tick line is drawn");
        CHECK (brightness (yOf (200.0)) > 20,
               "2x intermediate tick is drawn");
        CHECK (brightness (yOf (500.0)) > 20,
               "5x intermediate tick is drawn");

        // The major decade line uses a brighter colour than the minor ones.
        CHECK (brightness (yOf (100.0)) > brightness (yOf (200.0)) + 8
              && brightness (yOf (100.0)) > brightness (yOf (500.0)) + 8,
               "decade tick is brighter than the intermediate ticks");

        // A position without a tick stays dark.
        CHECK (brightness (yOf (150.0)) < 12, "no tick line where none is expected");

        // Sub-decade zoom [1k, 2k]: a nice linear step must provide divisions
        // inside the range instead of an empty axis.
        view.setFrequencyRange (1000.0, 2000.0);
        juce::Image img2 (juce::Image::ARGB, 400, 200, true);
        { juce::Graphics g (img2); view.paint (g); }
        auto yOf2 = [&] (double f) -> int
        { return (int) std::floor ((float) view.freqToY (f, 200)); };
        auto brightness2 = [&] (int yy) -> int
        { const auto c = img2.getPixelAt (100, yy); return (c.getRed() + c.getGreen() + c.getBlue()) / 3; };
        CHECK (brightness2 (yOf2 (1400.0)) > 25,
               "sub-decade zoom draws division ticks inside the range");
        CHECK (brightness2 (yOf2 (1500.0)) < 12,
               "no tick between two sub-decade divisions");

        sil.deleteFile();
    }

    wav.deleteFile();
}

// ---------------- FFT analysis view: decoupled zoom/pan/wheel + cursor snap ----------------
// The Frequency Analysis tab keeps its own frequency and level windows:
// wheel zooms around the cursor, right-drag box-zooms both axes, left-drag on
// an axis pans it. The cursor readout snaps to the nearest curve within a few
// pixels and reports that channel's exact bin value.
static void testFFTViewInteractions()
{
    const juce::File wav = makeTempFile ("fft_interact.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                      * 440.0 * (double) f / rate); });

    Application app;
    CHECK (app.openFile (wav), "open wav for FFT interaction test");   // openFile sets visible channels
    app.settings.fftSize = 1024;
    app.settings.fftLocalN = 1024;   // bin width 7.8125 Hz -> bins around 440 Hz

    const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);
    const juce::ModifierKeys rmb (juce::ModifierKeys::rightButtonModifier);

    // --- Wheel zoom: factor = pow(1.25, -deltaY) around the cursor frequency ---
    {
        FFTAnalysisView view (app);
        view.setSize (300, 200);
        const auto plot = view.plotBounds();
        CHECK (plot.getWidth() > 50 && plot.getHeight() > 50, "FFT plot area is sane");

        // Default log axis for an 8 kHz file: [20, Nyquist].
        const double fLo0 = view.getFreqLo(), fHi0 = view.getFreqHi();
        CHECK (std::abs (fLo0 - 20.0) < 1e-9 && std::abs (fHi0 - 4000.0) < 1e-9,
               "default log axis is [20, Nyquist]");

        const int cx = plot.getX() + plot.getWidth() / 2;
        const double fC = view.xToFreq (cx);

        auto wheel = [&] (int x, int y, float dy)
        {
            juce::MouseWheelDetails wd; wd.deltaY = dy;
            view.mouseWheelMove (mkMouse (view, x, y, {}), wd);
        };

        // Wheel down (deltaY=-1) zooms in by 1.25x around the cursor.
        wheel (cx, plot.getY() + plot.getHeight() / 2, -1.0f);
        const double expLo = std::exp (std::log (fC) + (std::log (fLo0) - std::log (fC)) / 1.25);
        const double expHi = std::exp (std::log (fC) + (std::log (fHi0) - std::log (fC)) / 1.25);
        CHECK (std::abs (view.getFreqLo() - expLo) / expLo < 1e-6
              && std::abs (view.getFreqHi() - expHi) / expHi < 1e-6,
               "wheel down zooms the frequency window in around the cursor");

        // Wheel up again restores the original range.
        wheel (cx, plot.getY() + plot.getHeight() / 2, +1.0f);
        CHECK (std::abs (view.getFreqLo() - fLo0) / fLo0 < 1e-6
              && std::abs (view.getFreqHi() - fHi0) / fHi0 < 1e-6,
               "wheel up restores the original frequency window");
    }

    // --- Right-drag box zoom: horizontal = frequency, vertical = level ---
    {
        FFTAnalysisView view (app);
        view.setSize (300, 200);
        const auto plot = view.plotBounds();

        const int x1 = plot.getX() + 20, y1 = plot.getY() + 16;
        const int x2 = plot.getX() + plot.getWidth() - 40, y2 = plot.getY() + plot.getHeight() - 30;

        // Expected values must be computed BEFORE the drag changes the ranges.
        // The zoom rect is inclusive of both endpoints.
        const double fA = view.xToFreq (x1), fB = view.xToFreq (x2);
        const double dbA = view.yToLevel ((float) y1), dbB = view.yToLevel ((float) y2);

        view.mouseDown (mkMouse (view, x1, y1, rmb));
        view.mouseDrag (mkMouse (view, x2, y2, rmb));
        view.mouseUp   (mkMouse (view, x2, y2, rmb));

        CHECK (std::abs (view.getFreqLo() - fA) / fA < 1e-6
              && std::abs (view.getFreqHi() - fB) / fB < 1e-6,
               "box zoom sets the frequency window to the dragged x span");
        CHECK (std::abs (view.getDbTop() - dbA) < 1e-6
              && std::abs (view.getDbBottom() - dbB) < 1e-6,
               "box zoom sets the level window to the dragged y span");
    }

    // --- Left-drag on the bottom axis pans frequency; right axis pans level ---
    {
        FFTAnalysisView view (app);
        view.setSize (300, 200);
        const auto plot = view.plotBounds();

        const double fLo0 = view.getFreqLo(), fHi0 = view.getFreqHi();
        const int ax1 = plot.getX() + 40, ax2 = plot.getX() + 90;
        const double f0 = view.xToFreq (ax1), f1 = view.xToFreq (ax2);

        // Bottom strip: y below the plot.
        const int ay = plot.getBottom() + 8;
        CHECK (ay < view.getHeight(), "bottom axis strip is inside the component");
        view.mouseDown (mkMouse (view, ax1, ay, lmb));
        view.mouseDrag (mkMouse (view, ax2, ay, lmb));

        const double r = f0 / f1;   // log mode: multiply by the ratio
        CHECK (std::abs (view.getFreqLo() - fLo0 * r) / fLo0 < 1e-6
              && std::abs (view.getFreqHi() - fHi0 * r) / fHi0 < 1e-6,
               "bottom-axis drag pans the frequency window");

        // Right strip: x beyond the plot.
        const double dbTop0 = view.getDbTop(), dbBot0 = view.getDbBottom();
        const int lx = plot.getRight() + 8;
        CHECK (lx < view.getWidth(), "level axis strip is inside the component");
        const int ly1 = plot.getY() + 26, ly2 = plot.getY() + 66;
        const double db0 = view.yToLevel ((float) ly1), db1 = view.yToLevel ((float) ly2);

        view.mouseDown (mkMouse (view, lx, ly1, lmb));
        view.mouseDrag (mkMouse (view, lx, ly2, lmb));

        CHECK (std::abs (view.getDbTop() - (dbTop0 + (db0 - db1))) < 1e-6
              && std::abs (view.getDbBottom() - (dbBot0 + (db0 - db1))) < 1e-6,
               "level-axis drag pans the level window");
    }

    // --- Cursor readout: snaps to the curve within a few pixels ---
    {
        FFTAnalysisView view (app);
        view.setSize (300, 200);
        const auto plot = view.plotBounds();

        app.setPlayhead (1.0);   // mid-file, well inside the tone
        { juce::Image img (juce::Image::ARGB, 300, 200, true);
          juce::Graphics g (img); view.paint (g); }

        const auto& mags = view.getSpectrum();
        CHECK (!mags.empty() && !mags[0].empty(), "spectrum was computed on paint");

        if (!mags.empty())
        {
            const int n = view.getCachedN();
            const double rate = app.audioEngine().sampleRate();
            const int bin440 = (int) std::lround (440.0 * n / rate);
            CHECK (bin440 > 0 && bin440 < n / 2, "tone bin is inside the spectrum");

            // Find the actual peak bin around the expected one.
            int pb = 0;
            for (int b = std::max (1, bin440 - 3); b <= std::min (n / 2 - 1, bin440 + 3); ++b)
                if (mags[0][(size_t) b] > mags[0][(size_t) pb]) pb = b;

            const double fPeak = (double) pb * rate / n;
            const int xAtPeak = (int) std::lround ((float) view.freqToX (fPeak));
            const float yAtPeak = view.levelToY (-160.0 * (1.0 - (double) mags[0][(size_t) pb]));

            bool snapped = false;
            float dotX = 0, dotY = 0;
            const juce::String info = view.buildCursorInfo (xAtPeak, (int) std::lround ((float) yAtPeak),
                                                           snapped, dotX, dotY);
            CHECK (snapped, "cursor on the curve snaps to it");
            CHECK (info.contains ("[snap Ch1]"), "readout names the snapped channel");
            CHECK (info.contains (juce::String (fPeak >= 1000.0 ? fPeak / 1000.0 : fPeak, 2)),
                   "readout shows the exact bin centre frequency");

            // Far from any curve: no snap, plain per-channel readout.
            const int xQuiet = (int) std::lround ((float) view.freqToX (50.0));
            bool snapped2 = true;
            const juce::String info2 = view.buildCursorInfo (xQuiet, plot.getY() + 3,
                                                            snapped2, dotX, dotY);
            CHECK (!snapped2, "cursor away from the curves does not snap");
            CHECK (info2.startsWith ("Cursor:") && info2.contains ("Ch1="),
                   "quiet readout still lists the channel value");
        }
    }

    wav.deleteFile();
}

// ---------------- FFT analysis view: deep zoom keeps the axis, rubber band visible ----------------
// Zooming far into the frequency window must keep producing on-plot divisions:
// the old code drew decade ticks only in log mode and relative-to-zero ticks
// in linear mode, so the axis disappeared at certain zooms. A right-drag box
// zoom must also show its rubber band while the drag is in progress.
static void testFFTViewExtremeZoom()
{
    const juce::File wav = makeTempFile ("fft_extreme_zoom.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                      * 440.0 * (double) f / rate); });

    const juce::ModifierKeys rmb (juce::ModifierKeys::rightButtonModifier);

    // Counts bright label pixels in the bottom Hz strip of a rendered view.
    auto axisLabelPixels = [] (const juce::Image& img, int x0, int x1, int y0, int y1)
    {
        int n = 0;
        for (int y = y0; y < y1 && y < img.getHeight(); ++y)
            for (int x = x0; x < x1 && x < img.getWidth(); ++x)
                if (img.getPixelAt (x, y).getRed() > 96) ++n;
        return n;
    };

    // --- Log mode: deep box zoom into a sub-decade range keeps labels ---
    {
        Application app;
        CHECK (app.openFile (wav), "open wav for extreme zoom test");
        FFTAnalysisView view (app);
        view.setSize (300, 200);
        const auto plot = view.plotBounds();

        // Drag a narrow box whose x span maps to roughly [900 Hz, 2100 Hz].
        int xa = -1, xb = -1;
        for (int x = plot.getX() + 4; x < plot.getRight() - 4 && xa < 0; ++x)
            if (view.xToFreq ((float) x) >= 900.0) xa = x;
        for (int x = plot.getRight() - 4; x > plot.getX() + 4 && xb < 0; --x)
            if (view.xToFreq ((float) x) <= 2100.0) xb = x;
        CHECK (xa >= 0 && xb > xa, "drag endpoints found around [900 Hz, 2100 Hz]");

        view.mouseDown (mkMouse (view, xa, plot.getY() + 30, rmb));
        view.mouseDrag (mkMouse (view, xb, plot.getY() + 60, rmb));

        // The rubber band must be visible while the drag is in progress:
        // a translucent yellow fill over the dark background.
        juce::Image img (juce::Image::ARGB, 300, 200, true);
        { juce::Graphics g (img); view.paint (g); }
        int yellow = 0;
        for (int y = plot.getY() + 40; y < plot.getY() + 50; ++y)
            for (int x = xa + 8; x < xb - 8; x += 2)
            {
                const auto c = img.getPixelAt (x, y);
                if (c.getRed() > 24 && c.getGreen() > 24 && c.getBlue() < 24) ++yellow;
            }
        CHECK (yellow >= 3, "rubber-band box is painted during the right-drag");

        view.mouseUp (mkMouse (view, xb, plot.getY() + 60, rmb));
        const double fLo = view.getFreqLo(), fHi = view.getFreqHi();
        CHECK (fLo > 800.0 && fHi < 2400.0 && std::log10 (fHi / fLo) < 0.7,
               "deep box zoom lands inside a sub-decade range");

        juce::Image img2 (juce::Image::ARGB, 300, 200, true);
        { juce::Graphics g (img2); view.paint (g); }
        CHECK (axisLabelPixels (img2, plot.getX(), plot.getRight(),
                                plot.getBottom() + 1, plot.getBottom() + 17) >= 8,
               "frequency axis keeps labels after a deep log-mode zoom");
    }

    // --- Linear mode: the old tick code placed every division below fLo ---
    {
        Application app2;
        CHECK (app2.openFile (wav), "open wav for linear extreme zoom test");
        app2.settings.fftAxisMode = 0;   // FFT Linear
        FFTAnalysisView view (app2);
        view.setSize (300, 200);
        const auto plot = view.plotBounds();

        int xa = -1, xb = -1;
        for (int x = plot.getX() + 4; x < plot.getRight() - 4 && xa < 0; ++x)
            if (view.xToFreq ((float) x) >= 640.0) xa = x;
        for (int x = plot.getRight() - 4; x > plot.getX() + 4 && xb < 0; --x)
            if (view.xToFreq ((float) x) <= 760.0) xb = x;
        CHECK (xa >= 0 && xb > xa, "linear drag endpoints found around [640 Hz, 760 Hz]");

        view.mouseDown (mkMouse (view, xa, plot.getY() + 30, rmb));
        view.mouseDrag (mkMouse (view, xb, plot.getY() + 60, rmb));
        view.mouseUp   (mkMouse (view, xb, plot.getY() + 60, rmb));

        const double fLo = view.getFreqLo(), fHi = view.getFreqHi();
        CHECK (fLo > 400.0 && fHi < 1200.0 && fHi - fLo < 300.0,
               "linear deep zoom lands in a narrow range");

        juce::Image img (juce::Image::ARGB, 300, 200, true);
        { juce::Graphics g (img); view.paint (g); }
        CHECK (axisLabelPixels (img, plot.getX(), plot.getRight(),
                                plot.getBottom() + 1, plot.getBottom() + 17) >= 8,
               "frequency axis keeps labels after a deep linear-mode zoom");
    }

    // --- Spectrogram ruler: multi-decade range gets subdivisions inside the
    // partial decade between 1k and 2k (the old digit-only scheme left it empty)
    {
        const juce::File sil = makeTempFile ("gapfill_silence.wav");
        writeWav (sil, 1, 8000.0, 32000, [] (int, int64_t, double) { return 0.0f; });

        Application appS;
        CHECK (appS.openFile (sil), "open silence for gap-fill test");

        SpectralView view (appS);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });
        view.setFrequencyRange (100.0, 2000.0);

        juce::Image img (juce::Image::ARGB, 400, 200, true);
        { juce::Graphics g (img); view.paint (g); }

        auto yOf = [&] (double f) -> int
        { return (int) std::floor ((float) view.freqToY (f, 200)); };
        auto brightness = [&] (int yy) -> int
        { const auto c = img.getPixelAt (100, yy); return (c.getRed() + c.getGreen() + c.getBlue()) / 3; };

        // Structural decade/digit ticks are still there...
        CHECK (brightness (yOf (200.0)) > 20, "digit tick at 200 Hz is drawn");
        CHECK (brightness (yOf (1000.0)) > 25, "decade tick at 1 kHz is drawn");

        // ...and the partial decade between 1k and 2k now has subdivisions.
        CHECK (brightness (yOf (1400.0)) > 20,
               "subdivision ticks are drawn between 1k and 2k");
        CHECK (brightness (yOf (1600.0)) > 20,
               "second subdivision tick is drawn between 1k and 2k");
        CHECK (brightness (yOf (1500.0)) < 12,
               "no stray tick between two subdivisions");

        // The wide gap inside the lower decade (200 -> 500) is filled too.
        CHECK (brightness (yOf (300.0)) > 20, "subdivision ticks fill the 200-500 Hz gap");
        CHECK (brightness (yOf (320.0)) < 12, "no stray tick inside the lower decade");

        sil.deleteFile ();
    }

    // --- FFT view: same multi-decade range gets subdivisions between 1k and 2k ---
    {
        Application appF;
        CHECK (appF.openFile (wav), "open wav for FFT gap-fill test");
        FFTAnalysisView view (appF);
        view.setSize (300, 200);
        const auto plot = view.plotBounds();

        // Box-zoom to roughly [100 Hz, 2 kHz].
        int xa = -1, xb = -1;
        for (int x = plot.getX() + 4; x < plot.getRight() - 4 && xa < 0; ++x)
            if (view.xToFreq ((float) x) >= 95.0) xa = x;
        for (int x = plot.getRight() - 4; x > plot.getX() + 4 && xb < 0; --x)
            if (view.xToFreq ((float) x) <= 2050.0) xb = x;
        CHECK (xa >= 0 && xb > xa, "FFT gap-fill drag endpoints found");

        view.mouseDown (mkMouse (view, xa, plot.getY() + 30, rmb));
        view.mouseDrag (mkMouse (view, xb, plot.getY() + 60, rmb));
        view.mouseUp   (mkMouse (view, xb, plot.getY() + 60, rmb));

        const double fLo = view.getFreqLo(), fHi = view.getFreqHi();
        CHECK (fLo < 120.0 && fHi > 1900.0,
               "FFT gap-fill zoom lands around [100 Hz, 2 kHz]");

        juce::Image img (juce::Image::ARGB, 300, 200, true);
        { juce::Graphics g (img); view.paint (g); }

        // Count vertical grid-line columns between x(1.05k) and x(1.95k):
        // the old digit-only scheme drew none there. A column counts only if
        // it is bright in most sample rows, so horizontal level lines don't
        // produce false positives.
        int lines = 0;
        for (int x = (int) view.freqToX (1050.0); x <= (int) view.freqToX (1950.0); ++x)
        {
            int brightRows = 0;
            for (int i = 1; i <= 5; ++i)
            {
                const int y = plot.getY() + plot.getHeight() * i / 6;
                const auto c = img.getPixelAt (x, y);
                if ((c.getRed() + c.getGreen() + c.getBlue()) / 3 > 20) ++brightRows;
            }
            if (brightRows >= 4) ++lines;
        }
        CHECK (lines >= 3,
               "FFT view draws subdivision grid lines between 1k and 2k");
    }

    // --- 1/3-octave mode: wheel-zoom deep; band centres or fallback ticks ---
    {
        Application app3;
        CHECK (app3.openFile (wav), "open wav for third-octave extreme zoom test");
        app3.settings.fftAxisMode = 2;   // 1/3 Octave
        FFTAnalysisView view (app3);
        view.setSize (300, 200);
        const auto plot = view.plotBounds();

        const int cx = plot.getX() + plot.getWidth() / 2;
        for (int i = 0; i < 40; ++i)
        {
            juce::MouseWheelDetails wd; wd.deltaY = -1.0f;
            view.mouseWheelMove (mkMouse (view, cx, plot.getY() + plot.getHeight() / 2, {}), wd);
        }

        const double fLo = view.getFreqLo(), fHi = view.getFreqHi();
        CHECK (fHi - fLo < 50.0, "wheel zoom reaches a very narrow range");

        juce::Image img (juce::Image::ARGB, 300, 200, true);
        { juce::Graphics g (img); view.paint (g); }
        CHECK (axisLabelPixels (img, plot.getX(), plot.getRight(),
                                plot.getBottom() + 1, plot.getBottom() + 17) >= 8,
               "frequency axis keeps labels after a deep third-octave zoom");
    }

    wav.deleteFile ();
}

// ---------------- Spectral controls: 512 FFT size option + async load progress ----------------
static void testSpectralControlsAndAsyncLoad()
{
    // The spectrogram's FFT-size combo offers 512 as the finest resolution.
    {
        Application app;
        SpectralControlsPanel panel (app);
        CHECK (panel.hasFftSizeOption ("512"), "FFT size combo offers a 512 option");
        CHECK (panel.hasFftSizeOption ("4096"), "FFT size combo still offers the usual sizes");
    }

    // Large-file loading reports progress and activates the document on the
    // message thread.
    {
        const juce::File wav = makeTempFile ("async_load.wav");
        writeWav (wav, 1, 8000.0, 32000, [] (int, int64_t f, double rate)
        { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                          * 440.0 * (double) f / rate); });

        Application app;
        std::mutex mtx;
        int progressCount = 0, lastProgressX100 = -1;
        bool done = false;

        app.loadFileAsync (wav,
            [&] (float p)
            {
                std::lock_guard<std::mutex> l (mtx);
                ++progressCount;
                lastProgressX100 = (int) std::lround ((double) p * 100.0);
            },
            [&] (bool ok) { done = ok; });

        for (int i = 0; i < 500 && !done; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

        CHECK (done, "async load completed successfully");
        { std::lock_guard<std::mutex> l (mtx);
          CHECK (progressCount > 0, "loading reported progress");
          CHECK (lastProgressX100 >= 95, "progress reached the end of the file"); }
        CHECK (app.audioEngine().isFileLoaded(), "document is active after async load");

        wav.deleteFile();
    }
}

// ---------------- History tab + batched undo/redo ----------------

static bool vecClose (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs (a[i] - b[i]) > 2e-3f) return false;
    return true;
}

static void testHistoryPanel()
{
    const juce::File wav = makeTempFile ("history.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 300.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open file for history test");
    AudioEngine& eng = app.audioEngine();

    const auto readRegion = [&] (int64_t a, int64_t b) {
        std::vector<float> v ((size_t) (b - a));
        eng.readSamples (0, a, b - a, v.data());
        return v;
    };
    const auto r1 = readRegion (8000, 9600);   // gain region [1.0s, 1.2s)
    const auto r2 = readRegion (10400, 12000); // silence region [1.3s, 1.5s)

    HistoryPanel panel (app);
    CHECK (panel.numRows() == 1, "history starts with the load row only");
    CHECK (panel.rowText (0) == juce::String ("Loaded ") + wav.getFileName(),
          "load row names the file");
    CHECK (panel.currentState() == 0, "current state is right after load");

    // Three edits -> three named rows.
    app.setSelection ({ 1.0, 1.2 });
    app.changeGain (6.0f);
    app.setSelection ({ 1.3, 1.5 });
    app.silence();
    app.setSelection ({ 1.6, 1.8 });
    app.reverse();

    panel.refresh();
    CHECK (panel.numRows() == 4, "one row per edit plus the load row");
    CHECK (panel.rowText (1) == "Changed gain by +6.00 dB", "gain action is named");
    CHECK (panel.rowText (2) == "Silenced selection", "silence action is named");
    CHECK (panel.rowText (3) == "Reversed selection", "reverse action is named");
    CHECK (panel.currentState() == 3, "all three actions applied");
    CHECK (!panel.rowIsFuture (0) && !panel.rowIsFuture (1)
          && !panel.rowIsFuture (2) && !panel.rowIsFuture (3), "no future rows yet");

    // Undo one step: the last row becomes a dimmed, redoable entry.
    app.undoStep();
    panel.refresh();
    CHECK (panel.numRows() == 4, "undo keeps all rows listed");
    CHECK (panel.currentState() == 2, "current state moved back one action");
    CHECK (panel.rowIsFuture (3), "undone action is marked future");

    // Jump straight to the load state (two undos in one go).
    auto jump = [&] (int target) {
        const int cur = (int) app.undo().undoSize();
        if (target < cur)
            app.undoSteps (cur - target);
        else if (target > cur)
            app.redoSteps (target - cur);
    };

    jump (0);
    panel.refresh();
    CHECK (panel.currentState() == 0, "jump landed on the load state");
    CHECK (vecClose (readRegion (8000, 9600), r1), "batched undo restored the gain region");
    CHECK (vecClose (readRegion (10400, 12000), r2), "batched undo restored the silence region");

    // Jump forward to state 1: only the gain is re-applied.
    jump (1);
    panel.refresh();
    CHECK (panel.currentState() == 1, "jump landed after the first action");
    const float g = std::pow (10.0f, 6.0f / 20.0f);
    std::vector<float> r1g ((size_t) r1.size());
    for (size_t i = 0; i < r1.size(); ++i) r1g[i] = r1[i] * g;
    CHECK (vecClose (readRegion (8000, 9600), r1g), "gain re-applied after jump");
    CHECK (vecClose (readRegion (10400, 12000), r2), "silence NOT re-applied at state 1");

    // Jump to the end: everything is applied again.
    jump (3);
    panel.refresh();
    CHECK (panel.currentState() == 3, "jump landed at the end of history");
    auto r2now = readRegion (10400, 12000);
    bool allZero = true;
    for (float v : r2now) if (v != 0.0f) { allZero = false; break; }
    CHECK (allZero, "silence re-applied after jump to the end");

    wav.deleteFile();
}

// ---------------- Settings persistence between sessions ----------------
static void testSettingsPersistence()
{
    const juce::File store = makeTempFile ("session.props");
    if (store.existsAsFile()) store.deleteFile();

    {
        Application a (store);
        // Defaults with an empty store.
        CHECK (!a.settings.snapToZeroCrossings, "zero-crossing snap is OFF by default");
        CHECK (a.settings.logFrequency, "spectral log mode on by default");
        CHECK (a.settings.fftSize == 2048, "default spectral fft size");
        CHECK (a.settings.fftAxisMode == 1, "default analysis axis is log");
        CHECK (a.settings.fftLocalN == 2048, "default analysis fft size");

        a.settings.logFrequency = false;
        a.settings.fftSize = 4096;
        a.settings.snapToZeroCrossings = true;
        a.settings.fftAxisMode = 2;
        a.settings.fftLocalN = 8192;
        a.saveSettings();
    }

    {
        Application b (store);
        CHECK (!b.settings.logFrequency, "spectral log/linear state restored");
        CHECK (b.settings.fftSize == 4096, "spectral fft size restored");
        CHECK (b.settings.snapToZeroCrossings, "zero-crossing snap setting restored");
        CHECK (b.settings.fftAxisMode == 2, "analysis axis mode restored");
        CHECK (b.settings.fftLocalN == 8192, "analysis fft size restored");
    }

    // Non-power-of-two stored values are rounded up to the next power of two
    // on load (simulated here by saving raw values directly).
    {
        Application d (store);
        d.settings.fftSize = 3000;
        d.settings.fftLocalN = 700;
        d.saveSettings();
    }
    {
        Application c (store);
        CHECK (c.settings.fftSize == 4096, "stored 3000 rounds up to 4096");
        CHECK (c.settings.fftLocalN == 1024, "stored 700 clamps to the minimum 1024");
    }

    store.deleteFile();
}

// ---------------- Zero-crossing snap in setSelection ----------------
static void testZeroCrossingSnapSelection()
{
    const juce::File wav = makeTempFile ("snap.wav");
    writeWav (wav, 1, 44100.0, 88200, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open tone for snap test");
    AudioEngine& eng = app.audioEngine();

    // OFF by default: the selection is untouched even with snapEdges requested.
    CHECK (!app.settings.snapToZeroCrossings, "snap off by default");
    app.setSelection ({ 1.001, 1.903 }, true);
    CHECK (std::abs (app.selection.getStart() - 1.001) < 1e-9, "no snap when disabled (start)");
    CHECK (std::abs (app.selection.getEnd() - 1.903) < 1e-9, "no snap when disabled (end)");

    // ON: both edges land on zero crossings of the tone.
    app.settings.snapToZeroCrossings = true;
    app.setSelection ({ 1.001, 1.903 }, true);
    const double s = app.selection.getStart(), e = app.selection.getEnd();
    CHECK (std::abs (s - 1.001) < 0.0015, "snapped start within half a period of the raw edge");
    CHECK (std::abs (e - 1.903) < 0.0015, "snapped end within half a period of the raw edge");

    auto valAt = [&] (double t) {
        float v;
        eng.readSamples (0, (int64_t) std::llround (t * 44100.0), 1, &v);
        return v;
    };
    CHECK (std::abs (valAt (s)) < 0.05f, "snapped start sits on a zero crossing");
    CHECK (std::abs (valAt (e)) < 0.05f, "snapped end sits on a zero crossing");

    // A degenerate (zero-length) selection must survive snapping unchanged.
    app.setSelection ({ 1.234, 1.234 }, true);
    CHECK (app.selection.getStart() == 1.234 && app.selection.getEnd() == 1.234,
          "zero-length selection untouched by snap");

    // Programmatic selections (no snap flag) are never snapped, even with the setting ON.
    app.setSelection ({ 1.001, 1.903 });
    CHECK (std::abs (app.selection.getStart() - 1.001) < 1e-9,
          "programmatic selection not snapped");

    wav.deleteFile();
}

// ---------------- Frequency-analysis view persistence wiring ----------------
static void testFFTViewPersistence()
{
    const juce::File store = makeTempFile ("fftview.props");
    if (store.existsAsFile()) store.deleteFile();

    Application app (store);
    app.settings.fftAxisMode = 2;   // 1/3 octave
    app.settings.fftLocalN = 8192;

    FFTAnalysisView view (app);
    CHECK (view.axisMode() == 2, "axis mode restored from settings");
    CHECK (view.fftSize() == 8192, "fft size restored from settings");

    // Changing the combos writes back to the shared (persisted) settings.
    view.fftSizeComboRef().setSelectedId (4096, juce::NotificationType::sendNotificationSync);
    CHECK (app.settings.fftLocalN == 4096, "combo change updates settings.fftLocalN");
    CHECK (view.fftSize() == 4096, "combo change updates the local size");

    view.axisComboRef().setSelectedId (1, juce::NotificationType::sendNotificationSync);   // FFT Linear
    CHECK (app.settings.fftAxisMode == 0, "axis combo writes back to settings");
    CHECK (view.axisMode() == 0, "axis mode follows the combo");

    store.deleteFile();
}

// ---------------- Batched undo/redo jumps ----------------
static void testBatchedUndoRedo()
{
    const juce::File wav = makeTempFile ("batch.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 300.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open file for batched undo test");
    AudioEngine& eng = app.audioEngine();

    const auto readRegion = [&] (int64_t a, int64_t b) {
        std::vector<float> v ((size_t) (b - a));
        eng.readSamples (0, a, b - a, v.data());
        return v;
    };
    const auto r1 = readRegion (8000, 9600);
    const auto r2 = readRegion (10400, 12000);

    app.setSelection ({ 1.0, 1.2 });
    app.changeGain (6.0f);
    app.setSelection ({ 1.3, 1.5 });
    app.silence();
    CHECK (app.undo().undoSize() == 2, "two actions recorded");

    // Undo both in one batched call: data fully restored.
    app.undoSteps (2);
    CHECK (app.undo().undoSize() == 0 && app.undo().redoSize() == 2,
          "stacks moved as a pair");
    CHECK (vecClose (readRegion (8000, 9600), r1), "batched undo restored the gain region");
    CHECK (vecClose (readRegion (10400, 12000), r2), "batched undo restored the silence region");

    // Redo one step, then ask for more than is available: it must clamp.
    app.redoStep();
    const float g = std::pow (10.0f, 6.0f / 20.0f);
    std::vector<float> r1g ((size_t) r1.size());
    for (size_t i = 0; i < r1.size(); ++i) r1g[i] = r1[i] * g;
    CHECK (vecClose (readRegion (8000, 9600), r1g), "single redo re-applies the gain");

    app.redoSteps (5);
    CHECK (app.undo().undoSize() == 2 && app.undo().redoSize() == 0,
          "batched redo clamps to the available steps");
    auto r2now = readRegion (10400, 12000);
    bool allZero = true;
    for (float v : r2now) if (v != 0.0f) { allZero = false; break; }
    CHECK (allZero, "silence re-applied after batched redo");

    wav.deleteFile();
}

static void testMultiFileSupport()
{
    // Two clearly distinct files: A = 8 kHz mono 2 s (440 Hz),
    //                          B = 16 kHz stereo 1 s (300/600 Hz).
    const juce::File wavA = makeTempFile ("multi_a.wav");
    writeWav (wavA, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                      * 440.0 * (double) f / rate); });

    const juce::File wavB = makeTempFile ("multi_b.wav");
    writeWav (wavB, 2, 16000.0, 16000, [] (int ch, int64_t f, double rate)
    { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                      * (ch == 0 ? 300.0 : 600.0) * (double) f / rate); });

    Application app;
    CHECK (app.openFile (wavA), "open file A");
    CHECK (app.numDocs() == 1 && app.activeDocIndex == 0, "one doc after first open");

    // Remember per-file state on A.
    app.setPlayhead (0.25);
    app.setSelection ({ 0.1, 0.4 });

    CHECK (app.openFile (wavB), "open file B");
    CHECK (app.numDocs() == 2 && app.activeDocIndex == 1, "two docs; new one is active");
    CHECK (std::abs (app.audioEngine().sampleRate() - 16000.0) < 1e-9,
          "active engine is B's");
    CHECK (app.playhead == 0.0 && app.selection.isEmpty(), "new doc starts with empty state");

    // Give B its own state too...
    app.setPlayhead (0.5);
    app.setSelection ({ 0.2, 0.7 });

    // ...switch back to A: the remembered playhead/selection must be restored.
    app.activate (0);
    CHECK (std::abs (app.playhead - 0.25) < 1e-9, "A's playhead restored on switch");
    CHECK (std::abs (app.selection.getStart() - 0.1) < 1e-9
          && std::abs (app.selection.getEnd() - 0.4) < 1e-9,
          "A's selection restored on switch");
    CHECK (std::abs (app.audioEngine().sampleRate() - 8000.0) < 1e-9,
          "active engine is A's again");

    // ...and B must still hold its own state while inactive.
    app.activate (1);
    CHECK (std::abs (app.playhead - 0.5) < 1e-9
          && std::abs (app.selection.getStart() - 0.2) < 1e-9,
          "B's state kept while A was active");

    // Ctrl+Tab cycling: forward wraps around the doc list.
    app.cycleActiveDoc (true);   // B -> A
    CHECK (app.activeDocIndex == 0, "cycle forward moves to previous doc");
    app.cycleActiveDoc (true);   // A -> B (wrap)
    CHECK (app.activeDocIndex == 1, "cycle wraps around the list");

    // Markers are per-file.
    app.activate (0);
    app.markers().addRegionMarker (0.1, 0.2, "A marker");
    app.activate (1);
    CHECK (app.markers().count() == 0, "B has no markers of its own");
    app.activate (0);
    CHECK (app.markers().count() == 1 && app.markers().markers()[0].name == "A marker",
          "A keeps its own marker list");

    // Undo history is per-file: an edit on A must not be undoable from B.
    app.setSelection ({ 0.5, 1.0 });
    app.silence();
    CHECK (app.undo().canUndo(), "edit recorded in A's undo history");
    app.activate (1);
    CHECK (!app.undo().canUndo(), "B has an empty undo history");
    app.activate (0);
    CHECK (app.undo().canUndo(), "A still remembers its edit");

    // Closing a non-active doc leaves the active one and its state untouched.
    app.activate (1);
    const double bPlay = app.playhead;   // 0.5
    app.closeDoc (0);                    // close A while B is active
    CHECK (app.numDocs() == 1 && app.activeDocIndex == 0, "closing non-active doc keeps B");
    CHECK (std::abs (app.playhead - bPlay) < 1e-9, "B's playhead untouched by closing A");

    // Closing the last doc resets to the empty state.
    app.closeFile();
    CHECK (!app.audioEngine().isFileLoaded() && app.numDocs() == 0,
          "no docs after closing all");
    CHECK (app.playhead == 0.0 && app.selection.isEmpty(), "state reset when no file loaded");

    // Re-opening an already-loaded file just activates it (no duplicate doc).
    CHECK (app.openFile (wavA), "re-open A");
    CHECK (app.numDocs() == 1, "single doc after re-open");
    app.setPlayhead (0.3);
    CHECK (app.openFile (wavB), "open B again");
    CHECK (app.openFile (wavA), "opening an already-loaded file activates it");
    CHECK (app.numDocs() == 2, "no duplicate doc created");
    CHECK (app.activeDocIndex == 0 && std::abs (app.playhead - 0.3) < 1e-9,
          "re-opened A is active with its remembered playhead");

    // Shared snapshots: store one from each file; closing a file clears only
    // that file's snapshot entries. (The panel clamps the FFT size to >=1024.)
    app.settings.fftSize = 1024;
    app.settings.fftLocalN = 1024;
    FFTAnalysisView view (app);
    view.setSize (300, 200);

    // Wire the close-time snapshot cleanup exactly like MainWindow does.
    app.onFileClosed = [&view] (const FileDoc& d) { view.clearSnapshotsFor (&d); };

    app.activate (0);                    // A active
    app.setSelection ({ 0.2, 0.8 });
    view.recomputeIfNeeded();
    view.handleSnapshotClick (0);        // snapshot 1 <- from A
    CHECK (view.snapshotN (0) == 1024 && view.snapshotShown (0), "snapshot stored from A");

    app.activate (1);                    // B active
    app.setSelection ({ 0.1, 0.4 });
    std::this_thread::sleep_for (std::chrono::milliseconds (80));
    view.recomputeIfNeeded();
    view.handleSnapshotClick (1);        // snapshot 2 <- from B
    CHECK (view.snapshotN (1) == 1024 && view.snapshotShown (1), "snapshot stored from B");

    // Both snapshots survive the switch and keep their own source.
    app.activate (0);
    CHECK (view.snapshotN (0) == 1024 && view.snapshotN (1) == 1024,
          "both snapshots alive across files");

    // Convert snapshot 2 to a marker: it must land in B's marker list even
    // though A is active.
    CHECK (view.convertSnapshotToMarker (1), "convert snapshot 2 to marker");
    app.activate (1);
    const auto& bMarkers = app.markers().markers();
    CHECK (bMarkers.size() == 1 && bMarkers[0].name == "Snapshot 2",
          "marker created in the snapshot's source file (B)");
    CHECK (std::abs (bMarkers[0].start - 0.1) < 1e-9
          && std::abs (bMarkers[0].end - 0.4) < 1e-9,
          "marker spans the snapshot's time range");

    // Closing B clears only B's snapshots; A's stays.
    app.closeDoc (app.activeDocIndex);   // close B (active)
    CHECK (view.snapshotN (1) == 0 && !view.snapshotShown (1), "B's snapshot cleared on close");
    CHECK (view.snapshotN (0) == 1024, "A's snapshot survives closing B");

    wavA.deleteFile();
    wavB.deleteFile();
}

// ------------------------------------------------- Version auto-increment
// Every build bumps VERSION.txt and regenerates generated/Version.h (About box)
// plus sc_version.rc (exe file properties). This pins down that the baked-in
// version string has a valid x.y.z form.
static bool isXyzVersion (const char* s)
{
    int dots = 0, digits = 0;
    for (const char* p = s; *p != '\0'; ++p)
    {
        if (*p == '.')
        {
            if (++dots > 2 || digits == 0) return false;
            digits = 0;
        }
        else if (*p >= '0' && *p <= '9')
            ++digits;
        else
            return false;
    }
    return dots == 2 && digits > 0;
}

static void testVersionString()
{
    CHECK (isXyzVersion (SC_VERSION_STRING),
           "version is x.y.z: " + juce::String (SC_VERSION_STRING));
}

// ------------------------------------------------------------------ resampling
// SoXR is the same engine Audacity uses. Verify length accuracy, tone
// preservation, anti-aliasing on downsample and extreme ratios.
static double goertzelAmp (const float* data, int n, double freqHz, double rate)
{
    const double w = 2.0 * juce::MathConstants<double>::pi * freqHz / rate;
    const double c = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double s0 = data[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    // |DFT(w)| / N * 2 == sine amplitude for an exact-bin full-window tone.
    return std::sqrt (s1 * s1 + s2 * s2 - c * s1 * s2) / n * 2.0;
}

static void testResampler()
{
    // 1. Upsample stereo 48 kHz -> 96 kHz: length + tone preservation.
    {
        const int N = 48000;   // 1 s @ 48 kHz, 1 kHz sine at 0.8 amplitude
        juce::AudioBuffer<float> in (2, N);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < N; ++i)
                in.setSample (c, i,
                             (float) (0.8 * std::sin (2.0 * juce::MathConstants<double>::pi
                                                      * 1000.0 * i / 48000.0)));
        juce::AudioBuffer<float> out;
        float lastProgress = -1.0f;
        CHECK (sc::Resampler::resample (in, 48000.0, 96000.0, out,
                                        [&] (float f) { lastProgress = f; }),
               "soxr upsampling succeeds");
        const int len = out.getNumSamples();
        CHECK (std::abs ((double) len - 96000.0) <= 8.0,
               "upsampled length is ~2x the input (96000 +/- 8)");
        const double a0 = goertzelAmp (out.getReadPointer (0), len, 1000.0, 96000.0);
        const double a1 = goertzelAmp (out.getReadPointer (1), len, 1000.0, 96000.0);
        CHECK (std::abs (a0 - 0.8) < 0.02 && std::abs (a1 - 0.8) < 0.02,
               "1 kHz tone amplitude preserved on both channels after upsampling");
        CHECK (lastProgress >= 0.99f, "progress callback reaches ~1.0");
    }

    // 2. Downsample 48 kHz -> 22.05 kHz: the out-of-band 20 kHz tone must be
    //    attenuated by > 60 dB (anti-aliasing); in-band tones survive.
    {
        const int N = 48000;
        auto makeIn = [] (bool withHigh) { return [withHigh] (int, int64_t f, double rate)
            {
                float v = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                  * 1000.0 * (double) f / rate);
                if (withHigh)
                    v += 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                 * 20000.0 * (double) f / rate);
                return v;
            };
        };
        juce::AudioBuffer<float> inRef (1, N), inMix (1, N);
        for (int i = 0; i < N; ++i)
        {
            inRef.setSample (0, i, makeIn (false) (0, i, 48000.0));
            inMix.setSample (0, i, makeIn (true) (0, i, 48000.0));
        }
        juce::AudioBuffer<float> outRef, outMix;
        CHECK (sc::Resampler::resample (inRef, 48000.0, 22050.0, outRef),
               "soxr downsampling (reference) succeeds");
        CHECK (sc::Resampler::resample (inMix, 48000.0, 22050.0, outMix),
               "soxr downsampling (mixed) succeeds");
        const int len = outRef.getNumSamples();
        CHECK (std::abs ((double) len - 22050.0) <= 8.0, "downsampled length ~ 22050");
        const double a1k = goertzelAmp (outMix.getReadPointer (0), len, 1000.0, 22050.0);
        CHECK (std::abs (a1k - 0.5) < 0.03, "in-band 1 kHz tone survives downsampling");
        // The difference between the two outputs is exactly the aliased residue
        // of the 20 kHz component: it must be > 60 dB below its original level.
        double sumSq = 0.0;
        for (int i = 0; i < len && i < outMix.getNumSamples(); ++i)
        {
            const double d = (double) outMix.getSample (0, i) - (double) outRef.getSample (0, i);
            sumSq += d * d;
        }
        const double rmsResidue = std::sqrt (sumSq / len);
        CHECK (rmsResidue < 0.5 * std::pow (10.0, -60.0 / 20.0),
               "out-of-band 20 kHz component attenuated > 60 dB by downsampling");
    }

    // 3. Round trip 44.1 kHz -> 48 kHz -> 44.1 kHz.
    {
        const int N = 44100;
        juce::AudioBuffer<float> in (1, N);
        for (int i = 0; i < N; ++i)
            in.setSample (0, i,
                         (float) (0.7 * std::sin (2.0 * juce::MathConstants<double>::pi
                                                  * 440.0 * i / 44100.0)));
        juce::AudioBuffer<float> mid, back;
        CHECK (sc::Resampler::resample (in, 44100.0, 48000.0, mid), "round trip stage 1");
        CHECK (sc::Resampler::resample (mid, 48000.0, 44100.0, back), "round trip stage 2");
        const int len = back.getNumSamples();
        CHECK (std::abs ((double) len - 44100.0) <= 16.0, "round-trip length ~ original");
        const double a = goertzelAmp (back.getReadPointer (0), len, 440.0, 44100.0);
        CHECK (std::abs (a - 0.7) < 0.02, "tone amplitude preserved through round trip");
    }

    // 4. Extreme ratio: 8 kHz -> 352.8 kHz (x44.1).
    {
        const int N = 8000;
        juce::AudioBuffer<float> in (1, N);
        for (int i = 0; i < N; ++i)
            in.setSample (0, i,
                         (float) (0.6 * std::sin (2.0 * juce::MathConstants<double>::pi
                                                  * 1000.0 * i / 8000.0)));
        juce::AudioBuffer<float> out;
        CHECK (sc::Resampler::resample (in, 8000.0, 352800.0, out),
               "extreme x44.1 upsampling succeeds");
        const int len = out.getNumSamples();
        CHECK (std::abs ((double) len - 352800.0) <= 64.0,
               "extreme-ratio output length ~ 352800");
        const double a = goertzelAmp (out.getReadPointer (0), len, 1000.0, 352800.0);
        CHECK (std::abs (a - 0.6) < 0.03, "tone preserved at extreme ratio");
    }

    // 5. Same-rate resample is an exact copy.
    {
        const int N = 1234;
        juce::AudioBuffer<float> in (2, N);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < N; ++i)
                in.setSample (c, i, (float) std::sin (i * 1.7 + c));
        juce::AudioBuffer<float> out;
        CHECK (sc::Resampler::resample (in, 48000.0, 48000.0, out), "same-rate resample succeeds");
        bool identical = (out.getNumSamples() == N);
        for (int c = 0; c < 2 && identical; ++c)
            for (int i = 0; i < N; ++i)
                if (out.getSample (c, i) != in.getSample (c, i)) { identical = false; break; }
        CHECK (identical, "same-rate resample returns an exact copy");
    }

    // 6. AudioEngine::resampleTo integration: rate/length/duration/playhead.
    {
        const juce::File wav = makeTempFile ("resample_engine.wav");
        writeWav (wav, 2, 44100.0, 44100,
                 [] (int ch, int64_t f, double rate)
                 { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                  * (ch == 0 ? 300.0 : 900.0) * (double) f / rate); });
        Application app;
        CHECK (app.openFile (wav), "open wav for engine resample test");
        CHECK (app.audioEngine().fullyInMemory(), "small file is fully in memory");

        const double durBefore = app.audioEngine().duration();
        app.audioEngine().setPosition (1.0);
        CHECK (app.audioEngine().resampleTo (48000.0), "engine resample to 48 kHz succeeds");
        CHECK (std::abs (app.audioEngine().sampleRate() - 48000.0) < 1e-6, "rate updated");
        const int64_t n = app.audioEngine().numSamples();
        CHECK (std::abs ((double) n / 48000.0 - durBefore) < 0.01,
               "duration preserved after resample");
        CHECK (app.audioEngine().mipmaps().empty(), "mipmaps cleared after resample");
        app.audioEngine().rebuildMipmaps();
        CHECK (!app.audioEngine().mipmaps().empty(), "mipmaps rebuilt for the new rate");
        CHECK (std::abs (app.audioEngine().position() - 1.0) < 0.01,
               "playhead position preserved in seconds after resample");
    }
}

// Regression: after a resample the Frequency Analysis panel's cached spectrum
// must be invalidated by the sample-rate change alone — same file pointer and
// same time region, so without the rate key recomputeIfNeeded() would keep
// drawing stale curves until some other setting happened to change.
static void testFFTAnalysisRateChange()
{
    const juce::File wav = makeTempFile ("fft_rate.wav");
    writeWav (wav, 1, 8000.0, 8000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 400.0 * (double) f / rate);
    });

    Application app;
    // openFile (not loadFile): it creates the FileDoc and populates
    // visibleChannels, which the analysis view iterates over.
    CHECK (app.openFile (wav), "open wav for fft rate-change test");
    app.setSelection ({ 0.0, 1.0 });   // the whole second of audio

    // Pin the FFT size so the expected peak bins are deterministic regardless
    // of whatever a persisted settings file may hold.
    app.settings.fftLocalN = 2048;

    FFTAnalysisView view (app);
    view.setSize (320, 300);
    view.recomputeIfNeeded();

    auto peakBin = [] (const std::vector<std::vector<float>>& m) -> int
    {
        if (m.empty() || m[0].empty()) return -1;
        size_t p = 0;
        for (size_t i = 1; i < m[0].size(); ++i)
            if (m[0][i] > m[0][p]) p = i;
        return (int) p;
    };

    // Copy by value: getSpectrum() returns a reference to the internal cache,
    // which is replaced on every recompute.
    auto before = view.getSpectrum();
    CHECK (!before.empty() && !before[0].empty(), "spectrum computed at original rate");
    const int peakBefore = peakBin (before);
    // 400 Hz @ 8 kHz with N=2048 -> bin ~102.
    CHECK (std::abs (peakBefore - 102) <= 1, "pre-resample spectrum peaks at the tone's bin");

    // Resample to a different rate: the analysed time region and file pointer
    // are unchanged, so only the sample-rate cache key can invalidate.
    CHECK (app.audioEngine().resampleTo (16000.0), "resample 8 kHz -> 16 kHz");
    // The multi-window averaging path is throttled to one expensive compute
    // per 60 ms; wait it out so this recompute actually runs.
    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    view.recomputeIfNeeded();

    auto after = view.getSpectrum();
    const int peakAfter = peakBin (after);
    // 400 Hz @ 16 kHz with N=2048 -> bin ~51: the curve must have moved.
    CHECK (std::abs (peakAfter - 51) <= 1, "post-resample spectrum peaks at the new rate's bin");
    CHECK (peakBefore != peakAfter, "spectrum recomputed after rate change (cache invalidated)");

    // The display window must follow the new Nyquist: otherwise the curve is
    // squashed into part of the plot (the "shifted analysis" bug).
    view.onSampleRateChanged();
    CHECK (view.getFreqHi() <= 16000.0 * 0.5 + 1e-6,
           "post-resample freq window clamped to new Nyquist");
    CHECK (view.getFreqHi() >= 2000.0, "post-resample freq window not degenerate");
    CHECK (view.getFreqLo() >= 0.0, "freq window lower bound sane");

    // With nothing changed the cache must be stable again.
    view.recomputeIfNeeded();
    auto again = view.getSpectrum();
    bool same = (again.size() == after.size()) && (!after.empty() || again.empty());
    if (same && !after.empty())
        for (size_t i = 0; i < after[0].size () && same; ++i)
            if (again[0][i] != after[0][i]) same = false;
    CHECK (same, "cache stable when nothing changed");
}

// The footer bar shows the active file's actual sample rate next to its name.
static void testStatusBarSampleRate()
{
    StatusBar bar;
    bar.setSize (600, 24);
    bar.setFileName ("tone.wav");
    CHECK (bar.getSampleRateText().isEmpty(), "no rate text before setSampleRate");

    bar.setSampleRate (48000.0);
    CHECK (bar.getSampleRateText() == "48000 Hz", "integer rate formatted as '48000 Hz'");

    bar.setSampleRate (47952.123);
    CHECK (bar.getSampleRateText() == "47952.123 Hz", "fractional rate keeps 3 decimals");

    bar.setSampleRate (0.0);
    CHECK (bar.getSampleRateText().isEmpty(), "zero rate clears the text");

    // Painting with every part set must not crash.
    bar.setSampleRate (44100.0);
    bar.setSelection (0.5, 1.25);
    bar.setPlayhead (0.75);
    juce::Image img (juce::Image::ARGB, 600, 24, true);
    juce::Graphics g (img);
    bar.paint (g);
    CHECK (true, "status bar paints with sample rate");
}

// Phase 2: streaming STFT/ISTFT used by DeepFilterNet. Pure DSP — exercised in
// every build (no model weights or OpenVINO required).
static void testDfStft()
{
    using sc::ai::DfStft;
    constexpr double kPi = 3.14159265358979323846;

    // Deterministic signal: two tones + low-level noise, exactly 34 frames.
    const int N = 34 * DfStft::kHop;   // 16320 samples
    std::vector<float> x ((size_t) N);
    uint32_t s = 987654u;
    for (int i = 0; i < N; ++i)
    {
        s = s * 1664525u + 1013904223u;
        const double t = (double) i / 8000.0;
        x[(size_t) i] = (float) (0.4 * std::sin (2.0 * kPi * 37.0 * t)
                                + 0.25 * std::sin (2.0 * kPi * 191.0 * t + 0.7)
                                + 0.01 * ((double) (s >> 8) / 16777216.0 - 1.0));
    }

    DfStft stft;
    std::vector<float> spec ((size_t) DfStft::kFreqs * 2);
    std::vector<float> y ((size_t) N, 0.f);

    for (int t = 0; t < N / DfStft::kHop; ++t)
    {
        stft.analyze (x.data () + (size_t) t * DfStft::kHop, spec.data ());
        float frame[DfStft::kHop];
        stft.synthesize (spec.data (), frame);
        for (int n = 0; n < DfStft::kHop; ++n)
            y[(size_t) t * DfStft::kHop + (size_t) n] += frame[n];
    }

    // The streaming OLA has an inherent one-hop latency: output chunk t
    // (absolute [t*hop, (t+1)*hop)) carries input samples [(t-1)*hop, t*hop).
    // DeepFilter::forwardChunk compensates by trimming the first hop of the
    // synthesised stream. Here we compare against the delayed reference.
    double maxErr = 0.0;
    for (int p = DfStft::kFftSize; p < N - DfStft::kFftSize; ++p)
        maxErr = std::max (maxErr, std::abs ((double) y[(size_t) p] - x[(size_t) (p - DfStft::kHop)]));
    CHECK (maxErr < 1e-3,
           "STFT/ISTFT round trip reconstructs the signal in the interior");

    // DC passthrough: a constant must come back at exactly the same level.
    {
        DfStft st;
        std::vector<float> dc ((size_t) N, 0.5f);
        std::vector<float> y2 ((size_t) N, 0.f);
        for (int t = 0; t < N / DfStft::kHop; ++t)
        {
            st.analyze (dc.data () + (size_t) t * DfStft::kHop, spec.data ());
            float frame[DfStft::kHop];
            st.synthesize (spec.data (), frame);
            for (int n = 0; n < DfStft::kHop; ++n)
                y2[(size_t) t * DfStft::kHop + (size_t) n] += frame[n];
        }
        double dcErr = 0.0;
        for (int p = DfStft::kFftSize; p < N - DfStft::kFftSize; ++p)
            dcErr = std::max (dcErr, std::abs ((double) y2[(size_t) p] - 0.5));
        CHECK (dcErr < 1e-4, "DC level preserved through the round trip");
    }

    // reset() must clear the streaming state: analysing silence after a loud
    // frame yields an all-zero spectrum.
    {
        DfStft st;
        std::vector<float> loud ((size_t) DfStft::kHop, 0.9f);
        std::vector<float> zero ((size_t) DfStft::kHop, 0.f);
        float spec2[DfStft::kFreqs * 2];
        st.analyze (loud.data (), spec2);
        st.reset ();
        st.analyze (zero.data (), spec2);
        double m = 0.0;
        for (float v : spec2) m = std::max (m, std::abs ((double) v));
        CHECK (m < 1e-6, "reset clears the analysis memory");
    }
}

// ---------------------------------------------------------------------------
// SCTT chunk (Whisper transcription storage): JSON + RIFF I/O

static void testScttJson()
{
    ScttData d;
    d.version = 1;
    d.segments.push_back ({ 0.5, 2.75, "hello world" });
    // Latin-1 chars, an emoji (U+1F600) and JSON special characters that
    // must be escaped on write. Built from code points so the test does not
    // depend on the source-file encoding.
    auto pushCp = [] (std::string& s, unsigned cp)
    {
        if (cp < 0x80) { s += (char) cp; return; }
        if (cp < 0x800) { s += (char) (0xC0 | (cp >> 6)); s += (char) (0x80 | (cp & 0x3F)); return; }
        if (cp < 0x10000) { s += (char) (0xE0 | (cp >> 12)); s += (char) (0x80 | ((cp >> 6) & 0x3F)); s += (char) (0x80 | (cp & 0x3F)); return; }
        s += (char) (0xF0 | (cp >> 18));
        s += (char) (0x80 | ((cp >> 12) & 0x3F));
        s += (char) (0x80 | ((cp >> 6) & 0x3F));
        s += (char) (0x80 | (cp & 0x3F));
    };
    std::string tricky = "\"quoted\" \\ backslash";   // JSON specials first
    pushCp (tricky, 0xE9);      // é
    pushCp (tricky, 0xFC);      // ü
    pushCp (tricky, 0x1F600);   // grinning-face emoji
    d.segments.push_back ({ 3.0, 4.25, tricky });

    const std::string json = scttToJson (d);
    ScttData d2;
    CHECK (parseScttJson (json, d2), "sctt json parses");
    if (d2.segments.size() == 2)
    {
        CHECK (d2.version == 1, "version round-trip");
        CHECK (std::abs (d2.segments[0].startSec - 0.5) < 1e-9, "t0 round-trip");
        CHECK (std::abs (d2.segments[0].endSec - 2.75) < 1e-9, "t1 round-trip");
        CHECK (d2.segments[0].text == "hello world", "plain text round-trip");
        CHECK (d2.segments[1].text == tricky, "unicode/escape round-trip");
    }

    ScttData bad;
    CHECK (!parseScttJson ("not json at all", bad), "garbage rejected");
    CHECK (!parseScttJson ("", bad), "empty rejected");
    CHECK (!parseScttJson ("{\"version\":1,\"segments\":[\"oops\"]}", bad), "wrong segment type rejected");
}

static void testScttChunkWav()
{
    const juce::File wav = makeTempFile ("sctt.wav");
    CHECK (writeWav (wav, 2, 44100.0, 4410 * 2,
        [] (int, int64_t f, double rate) {
            return (float) (0.1 * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) f / rate));
        }), "write test wav");

    // Save with an SCTT chunk through the engine hook.
    AudioEngine eng;
    CHECK (eng.loadFile (wav), "load sctt wav");
    ScttData want;
    want.version = 1;
    want.segments.push_back ({ 0.25, 1.5, "first line" });
    // "second é" with the é spelled out as UTF-8 bytes (encoding-independent).
    std::string uniBytes = "second \xC3\xA9";
    want.segments.push_back ({ 1.75, 3.9, uniBytes });
    const std::string json = scttToJson (want);
    eng.setScttForSave (&json);

    const juce::File outWav = makeTempFile ("sctt_out.wav");
    CHECK (eng.saveFileAs (outWav), "save with sctt chunk");

    // Read it back through the public RIFF reader.
    ScttData got;
    CHECK (readScttChunk (outWav, got), "sctt chunk read back");
    if (got.segments.size() == 2)
    {
        CHECK (std::abs (got.segments[0].startSec - 0.25) < 1e-9, "chunk t0");
        CHECK (std::abs (got.segments[0].endSec - 1.5) < 1e-9, "chunk t1");
        CHECK (got.segments[1].text == want.segments[1].text, "chunk text unicode");
    }

    // A plain WAV without the chunk must return false cleanly.
    ScttData none;
    CHECK (!readScttChunk (wav, none), "no sctt in plain wav");

    // FileDoc load path: opening through Application picks up the chunk.
    { Application app;
      CHECK (app.openFile (outWav), "open sctt wav via app");
      if (app.doc () != nullptr)
          CHECK (app.doc ()->sctt.segments.size() == 2, "filedoc picked up sctt");
    }
}

// RIFF scan helpers for the surgical chunk-replace test.
static int countRiffChunks (const juce::File& f, const char* fourcc)
{
    juce::FileInputStream in (f);
    if (in.failedToOpen ()) return -1;
    char hdr [12];
    if (in.read (hdr, 12) != 12 || std::memcmp (hdr, "RIFF", 4) != 0)
        return -1;
    const int64_t total = in.getTotalLength ();
    int count = 0;
    while (in.getPosition () + 8 <= total)
    {
        char id [4];
        unsigned char b [4];
        if (!in.read (id, 4) || in.read (b, 4) != 4) break;
        const uint32_t sz = (uint32_t) b[0] | ((uint32_t) b[1] << 8)
                          | ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24);
        if (std::memcmp (id, fourcc, 4) == 0) count++;
        const int64_t skip = (int64_t) sz + ((sz & 1) ? 1 : 0);
        if (!in.setPosition (in.getPosition () + skip)) break;
    }
    return count;
}

static std::vector<uint8_t> riffChunkPayload (const juce::File& f, const char* fourcc)
{
    std::vector<uint8_t> out;
    juce::FileInputStream in (f);
    if (in.failedToOpen ()) return out;
    char hdr [12];
    if (in.read (hdr, 12) != 12 || std::memcmp (hdr, "RIFF", 4) != 0)
        return out;
    const int64_t total = in.getTotalLength ();
    while (in.getPosition () + 8 <= total)
    {
        char id [4];
        unsigned char b [4];
        if (!in.read (id, 4) || in.read (b, 4) != 4) break;
        const uint32_t sz = (uint32_t) b[0] | ((uint32_t) b[1] << 8)
                          | ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24);
        if (std::memcmp (id, fourcc, 4) == 0 && sz > 0)
        {
            out.resize (sz);
            if (in.read (out.data (), (int) sz) != (int) sz) { out.clear (); break; }
            break;
        }
        const int64_t skip = (int64_t) sz + ((sz & 1) ? 1 : 0);
        if (!in.setPosition (in.getPosition () + skip)) break;
    }
    return out;
}

static void testScttChunkReplace()
{
    const juce::File wav = makeTempFile ("sctt_replace.wav");
    CHECK (writeWav (wav, 1, 8000.0, 4000,
        [] (int, int64_t f, double rate) {
            return (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                     * 440.0 * (double) f / rate);
        }), "write base wav");

    // Append a custom chunk that must survive the surgical rewrite.
    {
        const char* payload = "extra-chunk-payload";
        juce::FileOutputStream out (wav);   // default ctor opens in append mode
        CHECK (!out.failedToOpen (), "open wav for append");
        out.write ("TEST", 4);
        out.writeInt ((int) std::strlen (payload));
        out.write (payload, std::strlen (payload));
        out.writeByte (0);   // RIFF chunks are word-aligned: pad odd payloads
    }

    const std::vector<uint8_t> dataBefore = riffChunkPayload (wav, "data");
    CHECK (!dataBefore.empty (), "base wav has a data chunk");

    // 1. Append an SCTT to a file that has none.
    ScttData d1;
    d1.segments.push_back ({ 0.5, 2.0, "hello world" });
    std::string err;
    CHECK (replaceScttChunkInWav (wav, scttToJson (d1), &err),
          juce::String ("append sctt: ") + err);

    ScttData got;
    CHECK (readScttChunk (wav, got), "sctt read back after append");
    if (!got.empty ())
        CHECK (got.segments.size() == 1 && got.segments[0].text == "hello world",
              "appended sctt content");

    // The audio data must be byte-identical and the custom chunk preserved.
    CHECK (riffChunkPayload (wav, "data") == dataBefore, "audio bytes unchanged");
    const std::vector<uint8_t> extra = riffChunkPayload (wav, "TEST");
    CHECK (extra.size() == 19 && std::memcmp (extra.data(), "extra-chunk-payload", 19) == 0,
          "custom chunk preserved");

    // 2. Replace the existing SCTT: content updates, no duplicate chunks.
    ScttData d2;
    d2.segments.push_back ({ 1.0, 3.0, "second line" });
    CHECK (replaceScttChunkInWav (wav, scttToJson (d2), &err),
          juce::String ("replace sctt: ") + err);

    got = ScttData ();
    CHECK (readScttChunk (wav, got), "sctt read back after replace");
    if (!got.empty ())
        CHECK (got.segments.size() == 1 && got.segments[0].text == "second line",
              "replaced sctt content");
    CHECK (countRiffChunks (wav, "SCTT") == 1, "exactly one sctt chunk after replace");
    CHECK (riffChunkPayload (wav, "data") == dataBefore, "audio bytes unchanged after replace");

    // 3. Error cases: non-WAVE content and a missing file.
    const juce::File notWav = makeTempFile ("notwave.bin");
    {
        juce::FileOutputStream out (notWav);
        out.write ("GARBAGE", 7);
    }
    CHECK (!replaceScttChunkInWav (notWav, "{}", &err), "rejects non-wave file");
    CHECK (!replaceScttChunkInWav (makeTempFile ("missing_xyz.wav"), "{}", &err),
          "rejects missing file");
}

static void testAiSettingsPersistence()
{
    const juce::File store = makeTempFile ("ai_settings.recent");
    store.deleteFile ();   // names are deterministic per run position: never trust leftovers
    {
        Application app (store);
        CHECK (app.aiAutoDownload, "default auto-download is on");
        app.aiAutoDownload        = false;
        app.lastNsModelId         = "deepfilternet2";
        app.lastTranscribeModelId = "whisper-small";
        app.lastTranscribeLanguage= "de";
        app.lastTranscribeTranslate = true;
        app.saveAiSettings ();
    }
    {
        Application app (store);
        CHECK (!app.aiAutoDownload, "auto-download persisted");
        CHECK (app.lastNsModelId == "deepfilternet2", "ns model persisted");
        CHECK (app.lastTranscribeModelId == "whisper-small", "transcribe model persisted");
        CHECK (app.lastTranscribeLanguage == "de", "language persisted");
        CHECK (app.lastTranscribeTranslate, "translate flag persisted");
    }
}

#if defined(SC_ENABLE_AI)
// Phase 0: OpenVINO runtime smoke test + generic model manager logic.
// FIPS 180-4 test vectors for our self-contained SHA-256 (this JUCE build has
// no juce::MessageDigest). Digests verified against Python's hashlib.
static void testSha256()
{
    CHECK (sc::ai::sha256Hex ("", 0) ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256 of empty string");
    CHECK (sc::ai::sha256Hex ("abc", 3) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 of 'abc'");

    std::string a64 (64, 'a');   // exact block boundary: padding needs a 2nd block
    CHECK (sc::ai::sha256Hex (a64.data (), a64.size ()) ==
           "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
          "sha256 of 64 x 'a' (block boundary)");

    std::string a1000 (1000, 'a');   // multi-block
    CHECK (sc::ai::sha256Hex (a1000.data (), a1000.size ()) ==
           "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
          "sha256 of 1000 x 'a' (multi-block)");

    // Streaming API must match one-shot.
    sc::ai::Sha256 h;
    h.update ((const uint8*) a1000.data (), 37);
    h.update ((const uint8*) a1000.data () + 37, a1000.size () - 37);
    CHECK (h.hexDigest () == sc::ai::sha256Hex (a1000.data (), a1000.size ()),
          "streaming sha256 matches one-shot");
}

static void testAIPlumbing()
{
    // --- Model registry sanity -------------------------------------------
    auto& mm = sc::ai::ModelManager::instance ();
    // 3 noise-suppression models + 7 Whisper transcription models.
    CHECK (mm.registry().size() == 10, "registry lists 10 models");

    const auto* df2 = mm.find ("deepfilternet2");
    CHECK (df2 != nullptr, "find deepfilternet2");
    if (df2)
    {
        CHECK (df2->files.size() == 6, "df2 has 6 files");
        bool ok = true;
        for (const auto& f : df2->files)
            if (f.sha256.size() != 64 || f.sizeBytes <= 0 || f.url.empty()) ok = false;
        CHECK (ok, "df2 file specs complete");
    }

    // --- Install state + checksum logic on a synthetic model -------------
    juce::File root = mm.modelRoot ();
    if (!root.exists ()) root.createDirectory ();
    const juce::File dir = root.getChildFile ("synthetic-test-model");
    dir.deleteRecursively ();
    dir.createDirectory ();

    auto shaOf = [] (const juce::String& s) {
        const std::string utf8 = s.toStdString ();
        return sc::ai::sha256Hex (utf8.data (), utf8.size ());
    };

    sc::ai::ModelSpec spec;
    spec.id = "synthetic-test-model";
    spec.displayName = "Synthetic";
    const juce::String contentA = "SpectroCraft synthetic model file A\n";
    const juce::String contentB = "SpectroCraft synthetic model file B\n";
    spec.files.push_back ({ "https://example.invalid/a.bin", shaOf (contentA),
                            (int64_t) contentA.toStdString ().size () });
    spec.files.push_back ({ "https://example.invalid/b.bin", shaOf (contentB),
                            (int64_t) contentB.toStdString ().size () });

    std::vector<std::string> missing;
    CHECK (!mm.isInstalled (spec, &missing), "synthetic model not installed before files exist");
    CHECK (missing.size() == 2, "both files reported missing");

    auto writeFile = [] (const juce::File& p, const juce::String& s) {
        p.deleteFile ();   // FileOutputStream opens in append mode
        juce::FileOutputStream o (p);
        const std::string utf8 = s.toStdString ();
        o.write ((const uint8*) utf8.data (), utf8.size ());
    };

    writeFile (dir.getChildFile ("a.bin"), contentA);
    CHECK (!mm.isInstalled (spec, &missing), "still not installed with one file missing");
    CHECK (missing.size() == 1 && missing[0] == "b.bin", "only b.bin reported missing");

    writeFile (dir.getChildFile ("b.bin"), contentB);
    CHECK (mm.isInstalled (spec, &missing), "installed once all files present with right sizes");

    std::string bad;
    CHECK (mm.verifyInstalled (spec, &bad), "checksums verify for intact synthetic model");

    // Corrupt b.bin: size changes -> isInstalled false; sha also fails.
    {
        juce::FileOutputStream o (dir.getChildFile ("b.bin"));
        const char extra = 'x';
        o.setPosition ((int64_t) contentB.toStdString ().size ());
        o.write (&extra, 1);
    }
    CHECK (!mm.isInstalled (spec, nullptr), "corrupted file detected by size check");
    CHECK (!mm.verifyInstalled (spec, &bad), "checksum mismatch detected");
    CHECK (bad == "b.bin", "mismatching file identified");

    // Restore and exercise the ensureInstalled fast path: everything present,
    // so no download is attempted even though the URLs are fake.
    writeFile (dir.getChildFile ("b.bin"), contentB);
    std::string err;
    bool cancelled = false;
    CHECK (mm.ensureInstalled (spec, [&cancelled] (float, const std::string&) { return !cancelled; }, &err),
          "ensureInstalled fast path when already installed");

    // --- OpenVINO runtime smoke test --------------------------------------
#if !defined(_DEBUG)
    auto& rt = sc::ai::OvRuntime::instance ();
    const bool initOk = rt.init (&err);
    // Graceful skip when the OpenVINO runtime DLLs are not installed (the
    // executable is delay-loaded and runs fine without them). A genuine
    // initialisation failure in an environment that does have the DLLs still
    // fails the check below.
#if defined(_WIN32)
    if (!initOk && !LoadLibraryA ("openvino.dll"))
    {
        std::cerr << "  [OpenVINO runtime smoke test skipped: openvino.dll not installed]\n";
        return;
    }
#endif
    CHECK (initOk, "OpenVINO runtime initialised");
    if (rt.ready ())
    {
        const auto devs = rt.devices ();
        bool hasCpu = false;
        for (const auto& d : devs) if (d == "CPU") hasCpu = true;
        CHECK (hasCpu, "CPU device available");
    }
    else
    {
        std::cerr << "  [ov init failed: " << err << "]\n";
        CHECK (false, "OpenVINO runtime initialised (see message)");
    }
#else
    // OpenVINO 2024.6 binaries fail to initialise under the MSVC debug CRT
    // (std::bad_alloc inside plugin discovery; verified with a standalone
    // repro). The AI plumbing is therefore smoke-tested in Release builds;
    // Debug builds exercise everything up to this point.
    std::cerr << "  [OpenVINO runtime smoke test skipped: MSVC debug CRT]\n";
#endif
}

// The failure-dialog text builder must point users at the right fix:
// missing OpenVINO DLLs -> DLL instructions (NOT model downloads);
// model problems -> manual-install folder + per-file URLs.
static void testNsFailureDetails()
{
    const std::string rtErr = "OpenVINO runtime unavailable: OpenVINO runtime could not be loaded (Windows error 126). The OpenVINO DLLs are missing - install them next to SpectroCraft.exe.";

    const std::string d1 = sc::buildNsFailureDetails (rtErr, "deepfilternet2");
    CHECK (d1.find ("openvino.dll") != std::string::npos, "runtime-missing: lists openvino.dll");
    CHECK (d1.find ("tbb12.dll") != std::string::npos, "runtime-missing: lists tbb12.dll");
    CHECK (d1.find ("extern/OpenVINO/bin/") != std::string::npos, "runtime-missing: points at repo DLL folder");
    CHECK (d1.find ("openvinotoolkit/openvino/releases") != std::string::npos, "runtime-missing: gives download source");
    CHECK (d1.find ("huggingface.co/Intel") == std::string::npos, "runtime-missing: no model URLs");

    const std::string d2 = sc::buildNsFailureDetails ("download failed: HTTP 404", "deepfilternet2");
    CHECK (d2.find ("Manual installation") != std::string::npos, "model error: has manual-install section");
    CHECK (d2.find ("huggingface.co/Intel/deepfilternet-openvino") != std::string::npos, "model error: lists model URLs");
    CHECK (d2.find ("OpenVINO runtime DLLs must sit") == std::string::npos, "model error: no DLL list");

    const std::string d3 = sc::buildNsFailureDetails ("boom", "no-such-model");
    CHECK (d3.find ("Manual installation") == std::string::npos, "unknown model: no extra section");
    CHECK (d3.find ("boom") != std::string::npos, "unknown model: raw error preserved");

    const std::string d4 = sc::buildNsFailureDetails ("", "denseunet");
    CHECK (d4.find ("could not be processed") != std::string::npos, "empty error: generic message");

    // Missing companion DLLs surface as OpenVINO exceptions at model-read /
    // compile time (openvino.dll itself loads fine) - they must also get the
    // four-DLL instructions, not model-download guidance.
    const std::string feErr = "failed to load model: Exception from src\\inference\\src\\model_reader.cpp:160:\n"
        "Unable to read the model: C:\\x\\enc.xml Please check that model format: xml is supported and the model is correct. Available frontends: \n";
    const std::string d5 = sc::buildNsFailureDetails (feErr, "deepfilternet2");
    CHECK (d5.find ("openvino_ir_frontend.dll") != std::string::npos, "missing frontend DLL: lists the four DLLs");
    CHECK (d5.find ("tbb12.dll") != std::string::npos, "missing frontend DLL: lists tbb12.dll");
    CHECK (d5.find ("Manual installation") == std::string::npos, "missing frontend DLL: no model-install section");

    const std::string cpuErr = "failed to load model: Exception from src\\inference\\src\\dev\\core_impl.cpp:567:\n"
        "Device with \"CPU\" name is not registered in the OpenVINO Runtime\n";
    const std::string d6 = sc::buildNsFailureDetails (cpuErr, "deepfilternet2");
    CHECK (d6.find ("openvino_intel_cpu_plugin.dll") != std::string::npos, "missing CPU plugin DLL: lists the four DLLs");
    CHECK (d6.find ("Manual installation") == std::string::npos, "missing CPU plugin DLL: no model-install section");

    // A genuinely broken model file (frontends present -> non-empty list) must
    // NOT be treated as a missing-DLL case.
    const std::string badModelErr = "failed to load model: Unable to read the model: C:\\x\\enc.xml Please check that model format: xml is supported and the model is correct. Available frontends: xml, onnx";
    const std::string d7 = sc::buildNsFailureDetails (badModelErr, "deepfilternet2");
    CHECK (d7.find ("Manual installation") != std::string::npos, "corrupt model with frontends present: model-install section");
    CHECK (d7.find ("All four must sit in the same folder") == std::string::npos, "corrupt model with frontends present: no DLL list");
}

static double rmsOf (const std::vector<float>& v)
{
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (float x : v) s += (double) x * x;
    return std::sqrt (s / (double) v.size());
}

static void testDenseUnetPipeline()
{
    auto& mm = sc::ai::ModelManager::instance ();

    const auto* spec = mm.find ("denseunet");
    CHECK (spec != nullptr, "denseunet registered in the model registry");
    if (!spec) return;

#if defined(_DEBUG)
    // OpenVINO 2024.6 cannot be initialised under the MSVC debug CRT (see
    // testAIPlumbing); inference is therefore only exercised in Release.
    std::cerr << "  [denseunet pipeline test skipped: MSVC debug CRT]\n";
    return;
#endif

#if defined(_WIN32)
    // Graceful skip when the OpenVINO runtime DLLs are not installed (the
    // executable is delay-loaded and runs fine without them).
    if (!LoadLibraryA ("openvino.dll"))
    {
        std::cerr << "  [denseunet pipeline test skipped: openvino.dll not installed]\n";
        return;
    }
#endif

    // Attempt to install the weights (~9 MB download into the isolated temp
    // model root on first run).
    std::string err;
    bool cancelled = false;
    const bool installed = mm.ensureInstalled (*spec,
        [&cancelled] (float, const std::string&) { return !cancelled; }, &err);
    if (!installed)
    {
        // Graceful skip: no network / non-Windows. The pipeline is still
        // compiled and linked; only the model weights are missing.
        std::cerr << "  [denseunet pipeline test skipped: model unavailable (" << err << ")]\n";
        return;
    }

    // Synthetic signal: 2 s @ 48 kHz stereo white noise. The denseunet model
    // is a speech-enhancement network trained on speech+noise; stationary
    // noise is its in-distribution target, so we assert strong suppression.
    // (Pure tones are out-of-distribution and get suppressed as well — the
    // same behaviour Intel's Audacity plugin exhibits.)
    const int N = 96000;
    const double rate = 48000.0;
    std::vector<std::vector<float>> data (2);
    juce::Random rng (12345);
    for (int ch = 0; ch < 2; ++ch)
    {
        data[ch].resize ((size_t) N);
        const double noiseScale = 0.05 * (ch == 0 ? 1.0 : 1.3);   // differs per channel
        for (int i = 0; i < N; ++i)
            data[ch][(size_t) i] = (float) (noiseScale * (rng.nextFloat () * 2.0 - 1.0));
    }

    const double inRms[2] = { rmsOf (data[0]), rmsOf (data[1]) };
    // uniform [-1,1] noise has RMS = scale / sqrt(3) ~ 0.029 / 0.038 here
    CHECK (inRms[0] > 0.02 && inRms[1] > 0.02, "synthetic noise present before processing");

    const bool ok = sc::runDenseUnetPipeline ("denseunet", "CPU", rate, 2, data,
                                              nullptr, &err);
    CHECK (ok, "pipeline completed");
    if (!ok)
    {
        std::cerr << "  [pipeline failed: " << err << "]\n";
        return;
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        CHECK (data[ch].size () == (size_t) N, "length preserved");

        bool finite = true;
        for (float v : data[ch]) if (!std::isfinite (v)) { finite = false; break; }
        CHECK (finite, "output is finite");

        const double outRms = rmsOf (data[ch]);
        const double dbReduction = 20.0 * std::log10 (
            std::max (outRms, 1e-9) / inRms[ch]);
        CHECK (dbReduction <= -10.0, "noise reduced by at least 10 dB");
    }

    // Silence passes through as silence.
    {
        std::vector<std::vector<float>> sil (2);
        for (auto& c : sil) c.assign ((size_t) N, 0.0f);
        std::string sErr;
        const bool okS = sc::runDenseUnetPipeline ("denseunet", "CPU", rate, 2, sil,
                                                   nullptr, &sErr);
        CHECK (okS, "silence: pipeline completed");
        if (okS)
            CHECK (rmsOf (sil[0]) < 1e-4, "silence stays silent");
    }

    // Integration via Application: selection + undo/redo around the effect.
    {
        const juce::File wav = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("SpectroCraftTests-ns-integration.wav");
        writeWav (wav, 2, rate, N,
                  [] (int ch, int64_t f, double) {
                      // deterministic per-channel white noise (LCG)
                      uint32_t s = 12345u + (uint32_t) (ch * 7919u) + (uint32_t) f;
                      s = s * 1664525u + 1013904223u;
                      const double scale = 0.05 * (ch == 0 ? 1.0 : 1.3);
                      return (float) (scale * ((double) (s >> 8) / 16777216.0 - 1.0));
                  });

        Application app;   // isolated settings store via SC_TESTS_EXE
        CHECK (app.openFile (wav), "integration: file loaded");
        if (!app.audioEngine().isFileLoaded()) return;

        const double dur = app.audioEngine().duration ();
        app.setSelection ({ 0.0, dur });

        std::vector<float> before ((size_t) N), suppressed ((size_t) N);
        CHECK (app.audioEngine().readSamples (0, 0, N, before.data()), "integration: read pre-edit data");

        std::string iErr;
        const bool ok2 = app.noiseSuppress ("denseunet", "CPU", 0.f, false,
                                             nullptr, &iErr);
        CHECK (ok2, "integration: noiseSuppress succeeded");
        if (!ok2)
        {
            std::cerr << "  [integration failed: " << iErr << "]\n";
            return;
        }

        CHECK (app.audioEngine().readSamples (0, 0, N, suppressed.data()), "integration: read post-edit data");

        bool changed = false;
        for (int i = 0; i < N && !changed; ++i)
            if (suppressed[(size_t) i] != before[(size_t) i]) changed = true;
        CHECK (changed, "effect modified the samples");

        app.undoStep ();
        std::vector<float> afterUndo ((size_t) N);
        app.audioEngine().readSamples (0, 0, N, afterUndo.data());
        bool same = true;
        for (int i = 0; i < N && same; ++i)
            if (afterUndo[(size_t) i] != before[(size_t) i]) same = false;
        CHECK (same, "undo restores the original samples exactly");

        app.redoStep ();
        std::vector<float> afterRedo ((size_t) N);
        app.audioEngine().readSamples (0, 0, N, afterRedo.data());
        bool same2 = true;
        for (int i = 0; i < N && same2; ++i)
            if (afterRedo[(size_t) i] != suppressed[(size_t) i]) same2 = false;
        CHECK (same2, "redo restores the suppressed samples exactly");
    }
}

// Phase 2: DeepFilterNet2/3 end-to-end. Same contract as the denseunet test:
// stationary white noise must be strongly suppressed; silence stays silent;
// length is preserved; undo/redo work through Application.
static void testDeepFilterPipeline()
{
    auto& mm = sc::ai::ModelManager::instance ();

    const auto* spec2 = mm.find ("deepfilternet2");
    const auto* spec3 = mm.find ("deepfilternet3");
    CHECK (spec2 != nullptr, "deepfilternet2 registered in the model registry");
    CHECK (spec3 != nullptr, "deepfilternet3 registered in the model registry");
    if (!spec2 || !spec3) return;

#if defined(_DEBUG)
    // OpenVINO 2024.6 cannot be initialised under the MSVC debug CRT.
    std::cerr << "  [deepfilter pipeline test skipped: MSVC debug CRT]\n";
    return;
#endif

#if defined(_WIN32)
    // Graceful skip when the OpenVINO runtime DLLs are not installed (the
    // executable is delay-loaded and runs fine without them).
    if (!LoadLibraryA ("openvino.dll"))
    {
        std::cerr << "  [deepfilter pipeline test skipped: openvino.dll not installed]\n";
        return;
    }
#endif

    // Attempt to install the weights (~15 MB download on first run).
    std::string err;
    bool cancelled = false;
    const bool installed = mm.ensureInstalled (*spec2,
        [&cancelled] (float, const std::string&) { return !cancelled; }, &err);
    if (!installed)
    {
        std::cerr << "  [deepfilter pipeline test skipped: model unavailable (" << err << ")]\n";
        return;
    }

    // Synthetic signal: 2 s @ 48 kHz stereo white noise — the in-distribution
    // target for speech-enhancement models.
    const int N = 96000;
    const double rate = 48000.0;
    std::vector<std::vector<float>> data (2);
    juce::Random rng (54321);
    for (int ch = 0; ch < 2; ++ch)
    {
        data[ch].resize ((size_t) N);
        const double noiseScale = 0.05 * (ch == 0 ? 1.0 : 1.3);
        for (int i = 0; i < N; ++i)
            data[ch][(size_t) i] = (float) (noiseScale * (rng.nextFloat () * 2.0 - 1.0));
    }

    const double inRms[2] = { rmsOf (data[0]), rmsOf (data[1]) };
    CHECK (inRms[0] > 0.02 && inRms[1] > 0.02, "synthetic noise present before processing");

    const bool ok = sc::runDeepFilterPipeline ("deepfilternet2", "CPU", rate, 2, data,
                                               0.f, false, nullptr, &err);
    CHECK (ok, "pipeline completed");
    if (!ok)
    {
        std::cerr << "  [pipeline failed: " << err << "]\n";
        return;
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        CHECK (data[ch].size () == (size_t) N, "length preserved");

        bool finite = true;
        for (float v : data[ch]) if (!std::isfinite (v)) { finite = false; break; }
        CHECK (finite, "output is finite");

        const double outRms = rmsOf (data[ch]);
        const double dbReduction = 20.0 * std::log10 (
            std::max (outRms, 1e-9) / inRms[ch]);
        CHECK (dbReduction <= -10.0, "noise reduced by at least 10 dB");
    }

    // Silence passes through as silence.
    {
        std::vector<std::vector<float>> sil (2);
        for (auto& c : sil) c.assign ((size_t) N, 0.0f);
        std::string sErr;
        const bool okS = sc::runDeepFilterPipeline ("deepfilternet2", "CPU", rate, 2, sil,
                                                    0.f, false, nullptr, &sErr);
        CHECK (okS, "silence: pipeline completed");
        if (okS)
            CHECK (rmsOf (sil[0]) < 1e-4, "silence stays silent");
    }

    // Integration via Application: selection + undo/redo around the effect.
    {
        const juce::File wav = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("SpectroCraftTests-df-integration.wav");
        writeWav (wav, 2, rate, N,
                  [] (int ch, int64_t f, double) {
                      // deterministic per-channel white noise (LCG)
                      uint32_t s = 54321u + (uint32_t) (ch * 7919u) + (uint32_t) f;
                      s = s * 1664525u + 1013904223u;
                      const double scale = 0.05 * (ch == 0 ? 1.0 : 1.3);
                      return (float) (scale * ((double) (s >> 8) / 16777216.0 - 1.0));
                  });

        Application app;   // isolated settings store via SC_TESTS_EXE
        CHECK (app.openFile (wav), "integration: file loaded");
        if (!app.audioEngine().isFileLoaded()) return;

        const double dur = app.audioEngine().duration ();
        app.setSelection ({ 0.0, dur });

        std::vector<float> before ((size_t) N), suppressed ((size_t) N);
        CHECK (app.audioEngine().readSamples (0, 0, N, before.data()), "integration: read pre-edit data");

        std::string iErr;
        const bool ok2 = app.noiseSuppress ("deepfilternet2", "CPU", 0.f, false,
                                             nullptr, &iErr);
        CHECK (ok2, "integration: noiseSuppress succeeded");
        if (!ok2)
        {
            std::cerr << "  [integration failed: " << iErr << "]\n";
            return;
        }

        CHECK (app.audioEngine().readSamples (0, 0, N, suppressed.data()), "integration: read post-edit data");

        bool changed = false;
        for (int i = 0; i < N && !changed; ++i)
            if (suppressed[(size_t) i] != before[(size_t) i]) changed = true;
        CHECK (changed, "effect modified the samples");

        app.undoStep ();
        std::vector<float> afterUndo ((size_t) N);
        app.audioEngine().readSamples (0, 0, N, afterUndo.data());
        bool same = true;
        for (int i = 0; i < N && same; ++i)
            if (afterUndo[(size_t) i] != before[(size_t) i]) same = false;
        CHECK (same, "undo restores the original samples exactly");

        app.redoStep ();
        std::vector<float> afterRedo ((size_t) N);
        app.audioEngine().readSamples (0, 0, N, afterRedo.data());
        bool same2 = true;
        for (int i = 0; i < N && same2; ++i)
            if (afterRedo[(size_t) i] != suppressed[(size_t) i]) same2 = false;
        CHECK (same2, "redo restores the suppressed samples exactly");
    }
}

// Phase 3: Whisper transcription smoke test. Runs the full pipeline on a short
// synthetic signal; no assertion on segment content (a pure tone is not
// speech - whisper may return zero segments or hallucinate), only that the
// whole chain (download, ggml load, OV encoder init, inference) works.
static void testWhisperPipeline()
{
    auto& mm = sc::ai::ModelManager::instance ();

    // All seven Whisper models must be registered with a complete per-file
    // spec: one ggml .bin from HuggingFace. The optional OpenVINO encoder pair
    // is not part of the download (users can add it manually for speed).
    const char* ids[] = { "whisper-tiny", "whisper-base", "whisper-small",
                          "whisper-medium", "whisper-large-v1", "whisper-large-v2",
                          "whisper-large-v3" };
    for (const char* id : ids)
    {
        const auto* spec = mm.find (id);
        CHECK (spec != nullptr, juce::String (id) + " registered");
        if (!spec) continue;
        bool ok = spec->files.size() == 1 && spec->zip.url.empty ()
                  && !spec->files[0].url.empty ()
                  && spec->files[0].sha256.size() == 64
                  && spec->files[0].sizeBytes > 0
                  && !spec->files[0].name.empty ();
        CHECK (ok, juce::String (id) + " file spec complete");
    }

#if defined(_DEBUG)
    // OpenVINO cannot be initialised under the MSVC debug CRT.
    std::cerr << "  [whisper pipeline test skipped: MSVC debug CRT]\n";
    return;
#endif

#if defined(_WIN32)
    if (!LoadLibraryA ("openvino.dll"))
    {
        std::cerr << "  [whisper pipeline test skipped: openvino.dll not installed]\n";
        return;
    }
#endif

    const auto* spec = mm.find ("whisper-base");
    if (!spec) return;

    std::string err;
    bool cancelled = false;
    const bool installed = mm.ensureInstalled (*spec,
        [&cancelled] (float, const std::string&) { return !cancelled; }, &err);
    if (!installed)
    {
        // Graceful skip: no network / non-Windows.
        std::cerr << "  [whisper pipeline test skipped: model unavailable (" << err << ")]\n";
        return;
    }

    const juce::File dir = mm.dirFor ("whisper-base");
    const juce::File ggmlModel = dir.getChildFile ("ggml-base.bin");
    const juce::File irXml     = dir.getChildFile ("ggml-base-encoder-openvino.xml");

    // 2 s of a 440 Hz tone @ 16 kHz mono.
    AudioEngine eng;
    const int64_t frames = 32000;
    const juce::File wav = makeTempFile ("whisper_smoke.wav");
    CHECK (writeWav (wav, 1, 16000.0, frames,
        [] (int, int64_t f, double rate) {
            return 0.2f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                            * 440.0 * (double) f / rate);
        }), "write whisper smoke wav");
    CHECK (eng.loadFile (wav), "load whisper smoke wav");

    sc::ai::WhisperTranscriber::Options opts;
    opts.language = "en";

    std::vector<ScttSegment> segs;
    int lastPct = -1;
    const bool ok = sc::ai::runWhisperPipeline (eng, { 0 }, ggmlModel, irXml, opts, segs,
        [&lastPct] (float f) {
            const int pct = (int) (f * 100.0);
            if (pct >= lastPct + 25 || pct == 100)
                std::cerr << "    whisper " << std::max (lastPct, pct) << "%\n";
            lastPct = std::max (lastPct, pct);
            return true;
        }, &err);

    CHECK (ok, juce::String ("whisper pipeline ran: ") + err);
    if (ok)
        std::cerr << "    whisper segments on tone: " << segs.size() << "\n";
}

// Phase 4: batch folder transcription — headless WAV scan, SCTT written back
// in place, skip-existing mode. Real inference with the cached whisper-base.
static void testBatchTranscribe()
{
    auto& mm = sc::ai::ModelManager::instance ();

#if defined(_DEBUG)
    // OpenVINO cannot be initialised under the MSVC debug CRT.
    std::cerr << "  [batch transcribe skipped: MSVC debug CRT]\n";
    return;
#endif

#if defined(_WIN32)
    if (!LoadLibraryA ("openvino.dll"))
    {
        std::cerr << "  [batch transcribe skipped: openvino.dll not installed]\n";
        return;
    }
#endif

    const auto* spec = mm.find ("whisper-base");
    if (!spec) return;

    std::string err;
    bool cancelled = false;
    const bool installed = mm.ensureInstalled (*spec,
        [&cancelled] (float, const std::string&) { return !cancelled; }, &err);
    if (!installed)
    {
        std::cerr << "  [batch transcribe skipped: model unavailable (" << err << ")]\n";
        return;
    }

    // Two short WAVs in a temp folder tree, one nested in a subfolder.
    const juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("sc_test_batch_" + juce::String ((int) std::time (nullptr)));
    folder.createDirectory ();
    const juce::File sub = folder.getChildFile ("sub");
    sub.createDirectory ();

    auto writeTone = [] (const juce::File& f, double hz) {
        return writeWav (f, 1, 16000.0, 24000,
            [hz] (int, int64_t i, double rate) {
                return 0.2f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                * hz * (double) i / rate);
            });
    };
    const juce::File wavA = folder.getChildFile ("tone_a.wav");
    const juce::File wavB = sub.getChildFile ("tone_b.wav");
    if (!writeTone (wavA, 440.0) || !writeTone (wavB, 880.0)) return;

    Application app;   // isolated store under SC_TESTS_EXE
    BatchTranscribeSummary sum;
    const bool ok = app.batchTranscribeFolder (folder, "whisper-base", "en", false,
                                               /*skipExisting=*/false,
                                               nullptr, nullptr, nullptr, &sum, &err);
    CHECK (ok, juce::String ("batch transcribe ran: ") + err);
    if (ok)
    {
        CHECK (sum.total == 2, "found both wavs (recursive scan)");
        CHECK (sum.done == 2 && sum.failed == 0,
              juce::String ("both files transcribed (") + juce::String (sum.failed) + " failed)");

        ScttData a, b;
        CHECK (readScttChunk (wavA, a), "sctt chunk written to file A");
        CHECK (readScttChunk (wavB, b), "sctt chunk written to file B");

        // Second run with skip-existing: nothing new is transcribed.
        BatchTranscribeSummary sum2;
        const bool ok2 = app.batchTranscribeFolder (folder, "whisper-base", "en", false,
                                                    /*skipExisting=*/true,
                                                    nullptr, nullptr, nullptr, &sum2, &err);
        CHECK (ok2, juce::String ("batch skip run: ") + err);
        if (ok2)
            CHECK (sum2.skipped == 2 && sum2.done == 0 && sum2.failed == 0,
                  "skip-existing honoured");
    }

    // Clean up the temp tree.
    folder.deleteRecursively ();
}
#endif   // SC_ENABLE_AI

static int scRunUnitTestsImpl()
{
    std::setvbuf (stderr, nullptr, _IONBF, 0);   // keep stderr unbuffered so crash diagnostics survive
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    progress ("FFT");            testFFT();
    progress ("scfft vs juce");  testScFftVsJuce();
    progress ("markers");        testMarkers();
    progress ("undo/redo");      testUndo();
    progress ("zero-crossing");  testZeroCrossing();
    progress ("spectral engine");testSpectral();
    progress ("spectral view paint"); testSpectralViewPaint();
    progress ("waveform view paint"); testWaveformViewPaint();
    progress ("per-sample zoom");     testWaveformPerSampleZoom();
    progress ("FFT panel paint");testFFTAnalysisViewPaint();
    progress ("markers+channels");testMarkersAndChannels();
    progress ("marker panel details"); testMarkerPanelDetails();
    progress ("history panel paint");  testHistoryPanelPaint();
    progress ("marker panel layout");  testMarkerPanelLayout();
    progress ("independent views");testIndependentViews();
    progress ("scroll bars");     testScrollBars();
    progress ("wav marker chunks"); testWavMarkerChunks();
    progress ("load markers from wav"); testLoadMarkersFromWav();
    progress ("marker wav byte-exact"); testMarkerWavByteExact();
    progress ("effect channel scope"); testEffectChannelScope();
    progress ("rendering backend");   testGdiRendererForced();
    progress ("playhead sync");       testPlayheadSync();
    progress ("fft follow window");   testFFTFollowPlayhead();
    progress ("1/3 octave bands");    testThirdOctave();
    progress ("fft click+average");   testFFTClickAndAverage();
    progress ("1/3 octave step draw"); testThirdOctaveStepDrawing();
    progress ("snapshot button paint"); testSnapshotButtonPaint();
    progress ("1/3 octave click update"); testThirdOctaveClickUpdates();
    progress ("change gain");         testChangeGain();
    progress ("playback chain");      testPlaybackChain();
    progress ("snapshot store/recompute"); testSnapshotStoreAndRecompute();
    progress ("snapshot overlay paint");  testSnapshotOverlayPaint();
    progress ("status bar");          testStatusBar();
    progress ("footer file name");    testStatusBarFileName();
    progress ("about text licences"); testAboutText();
    progress ("time scrollbar on load"); testTimeScrollBarOnLoad();
    progress ("left panel layout"); testLeftPanelLayout();
    progress ("home/end panning"); testHomeEndPans();
    progress ("spectral probe");      testSpectralProbe();
    progress ("arrow key selection"); testArrowKeySelection();
    progress ("ctrl+number channels"); testCtrlNumberChannelToggle();
    progress ("F9 view cycle");       testF9ViewCycle();
    progress ("no view tooltips");    testNoViewTooltips();
    progress ("open recent files");   testOpenRecentFiles();
    progress ("change gain dialog");  testChangeGainDialog();
    progress ("scale calibration");   testScaleCalibration();
    progress ("set scale dialog");    testSetScaleDialog();
    progress ("spectral scale labels"); testSpectralScaleLabels();
    progress ("spectral dB window"); testSpectralDbWindow();
    progress ("right-drag box zoom"); testRightDragZoom();
    progress ("time-pan speed + reversal"); testTimePanSpeedAndReversal();
    progress ("axis pan + log ticks"); testAxisPanAndLogTicks();
    progress ("time axis ticks");     testTimeAxisTicks();
    progress ("marker title clip");   testMarkerTitleClipping();
    progress ("FFT view interactions"); testFFTViewInteractions();
    progress ("FFT extreme zoom axis + rubber band"); testFFTViewExtremeZoom();
    progress ("spectral controls + async load"); testSpectralControlsAndAsyncLoad();
    progress ("history tab");             testHistoryPanel();
    progress ("settings persistence");    testSettingsPersistence();
    progress ("zero-crossing snap sel");  testZeroCrossingSnapSelection();
    progress ("fft view persistence");    testFFTViewPersistence();
    progress ("batched undo/redo");       testBatchedUndoRedo();
    progress ("multi-file support");  testMultiFileSupport();
    progress ("resampler (soxr)");    testResampler();
    progress ("fft view rate change"); testFFTAnalysisRateChange();
    progress ("status bar sample rate"); testStatusBarSampleRate();
    progress ("sctt json");             testScttJson();
    progress ("sctt chunk wav");        testScttChunkWav();
    progress ("sctt chunk replace");    testScttChunkReplace();
    progress ("ai settings persist");   testAiSettingsPersistence();
    progress ("df stft round-trip");  testDfStft();
#if defined(SC_ENABLE_AI)
    progress ("sha256 (self-contained)"); testSha256();
    progress ("AI plumbing");         testAIPlumbing();
    progress ("NS failure details");  testNsFailureDetails();
    progress ("denseunet pipeline");  testDenseUnetPipeline();
    progress ("deepfilter pipeline"); testDeepFilterPipeline();
    progress ("whisper pipeline");    testWhisperPipeline();
    progress ("batch transcribe");    testBatchTranscribe();
#endif
    progress ("version string");      testVersionString();

    std::cerr << (total - failures) << "/" << total << " checks passed\n";
    return failures == 0 ? 0 : 1;
}

int scRunUnitTests()
{
    return scRunUnitTestsImpl();
}
