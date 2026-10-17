/**
 * SpectroCraft - Unit tests (simple built-in framework, no external deps)
 * Covers: FFT correctness, marker add/remove/edit, undo/redo,
 *         selection + zero-crossing logic, WAV generation.
 */

#include <JuceHeader.h>
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
#include "FFTHelpers.h"
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
    CHECK (app.audioEngine.loadFile (wav), "load wav for view paint test");

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
    const double rate = app.audioEngine.sampleRate();
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
    CHECK (app.audioEngine.loadFile (wav), "load wav for waveform view test");

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
    CHECK (app.audioEngine.loadFile (wav), "load wav for per-sample zoom test");

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
    CHECK (app.audioEngine.loadFile (wav), "load wav for fft view test");

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
    for (const auto& mk : app.markers.markers())
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
    for (const auto& mk : app.markers.markers())
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

    const int rid = app.markers.addRegionMarker (1.0, 3.0, "RegionA");
    app.markers.addPointMarker (2.5, "PointB");

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
        for (const auto& mk : app.markers.markers())
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
    app.markers.clear();
    const int r1 = app.markers.addRegionMarker (0.5, 2.5, "Alpha");
    app.markers.setComment (r1, "first line\nsecond line"); // flattened on copy
    app.markers.addPointMarker (3.0, "Beta");
    panel.refresh();

    panel.copyAllMarkers();
    const juce::String clip = juce::SystemClipboard::getTextFromClipboard();
    CHECK (clip == "Alpha\t0:00.500\t0:02.500\tfirst line second line\r\n"
                  "Beta\t0:03.000\t-\t",
           "copy produces tab/CRLF separated rows");

    // --- Paste markers from the clipboard ---
    const int before = (int) app.markers.count();
    juce::SystemClipboard::copyTextToClipboard (
        "Gamma\t0:01.250\t0:02.750\tpasted comment\r\nDelta\t0:03.500\t-\t");

    const int added = panel.pasteMarkersFromClipboard();
    CHECK (added == 2, "paste adds two markers");
    CHECK ((int) app.markers.count() == before + 2, "marker count grew by two");

    const Marker* g = nullptr;
    const Marker* d = nullptr;
    for (const auto& mk : app.markers.markers())
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
    const int rid = app.markers.addRegionMarker (1.0, 3.0, "Alpha");
    app.markers.setComment (rid, "a comment\nwith two lines");

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
        const double rate = app.audioEngine.sampleRate();
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
    CHECK (app.audioEngine.loadFile (wav), "load wav for playhead sync test");

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
    app.settings.fftSize = 1024;   // picked up as the panel's localN at construction

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
        CHECK (spec[0][25] < 0.1, "far bin stays quiet in the averaged spectrum");
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
    CHECK (app.audioEngine.readSamples (0, 4000, 1, &inSel), "read inside selection");
    CHECK (app.audioEngine.readSamples (0, 1000, 1, &outSel), "read outside selection");
    const float expected = 0.5f * std::pow (10.0f, 6.0f / 20.0f);
    CHECK (std::abs (inSel - expected) < 1e-4,
           "+6 dB scales the selected samples by 10^(dB/20)");
    CHECK (std::abs (outSel - 0.5f) < 1e-4, "samples outside the selection are untouched");

    app.undoStep();
    CHECK (app.audioEngine.readSamples (0, 4000, 1, &inSel), "read after undo");
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

    // Loading a different file invalidates all stored snapshots.
    const juce::File wav2 = makeTempFile ("snap_other.wav");
    writeWav (wav2, 1, 8000.0, 4000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 300.0 * (double) f / rate);
    });
    CHECK (app.openFile (wav2), "open second file");
    view.recomputeIfNeeded();
    CHECK (view.snapshotN (0) == 0 && view.snapshotN (1) == 0,
          "snapshots cleared when the file changes");
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
    const float peak = engine.probeMagnitude (app.audioEngine, 0, 1.0, 400.0, s, 0);
    const float off  = engine.probeMagnitude (app.audioEngine, 0, 1.0, 300.0, s, 0);

    CHECK (peak > -35.0f && peak < -5.0f, "probe finds the 400 Hz tone");
    CHECK (off < peak - 15.0f, "probe is much quieter off the tone frequency");
}

// ------------------------------------------------------------------ arrow keys / Shift+arrow selection
// Left/Right arrows move the playhead by one frame; holding Shift selects from
// where the shift-drag started to the current playhead position.
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

    const double step = 1.0 / app.audioEngine.sampleRate();
    auto pressRight = [&] (juce::ModifierKeys mods)
    { return win->keyPressed (juce::KeyPress (juce::KeyPress::rightKey, mods, 0), nullptr); };
    auto pressLeft  = [&] (juce::ModifierKeys mods)
    { return win->keyPressed (juce::KeyPress (juce::KeyPress::leftKey, mods, 0), nullptr); };

    app.setPlayhead (0.1);
    app.setSelection ({ 0.25, 0.3 });   // pre-existing selection must survive plain moves

    CHECK (pressRight (juce::ModifierKeys::noModifiers), "plain Right is handled");
    CHECK (std::abs (app.playhead - (0.1 + step)) < 1e-9,
           "plain Right moves the playhead one frame to the right");
    CHECK (app.selection == juce::Range<double> (0.25, 0.3),
           "plain movement does not touch the selection");

    CHECK (!pressRight (juce::ModifierKeys::commandModifier), "Ctrl+Right is left unhandled");
    CHECK (std::abs (app.playhead - (0.1 + step)) < 1e-9, "unhandled key leaves the playhead alone");

    // Shift+Right: anchor at the current position, extend rightwards.
    const double anchor = app.playhead;
    CHECK (pressRight (juce::ModifierKeys::shiftModifier), "Shift+Right is handled");
    CHECK (std::abs (app.playhead - (anchor + step)) < 1e-9,
           "Shift+Right moves the playhead one frame to the right");
    CHECK (app.selection == juce::Range<double> (anchor, anchor + step),
           "selection spans from the anchor to the new position");

    pressRight (juce::ModifierKeys::shiftModifier);
    pressRight (juce::ModifierKeys::shiftModifier);
    CHECK (std::abs (app.playhead - (anchor + 3.0 * step)) < 1e-9, "repeated Shift+Right keeps moving");
    CHECK (app.selection == juce::Range<double> (anchor, anchor + 3.0 * step),
           "selection follows the playhead to the right");

    // Shift+Left: shrink back across the anchor and flip direction.
    pressLeft (juce::ModifierKeys::shiftModifier);
    CHECK (app.selection == juce::Range<double> (anchor, anchor + 2.0 * step),
           "Shift+Left shrinks the selection");
    for (int i = 0; i < 5; ++i)
        pressLeft (juce::ModifierKeys::shiftModifier);
    CHECK (std::abs (app.playhead - (anchor - 3.0 * step)) < 1e-9, "crossing the anchor keeps moving left");
    CHECK (app.selection == juce::Range<double> (anchor - 3.0 * step, anchor),
           "selection flips to [playhead, anchor] when crossing");

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

static int scRunUnitTestsImpl()
{
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
    progress ("spectral probe");      testSpectralProbe();
    progress ("arrow key selection"); testArrowKeySelection();

    std::cerr << (total - failures) << "/" << total << " checks passed\n";
    return failures == 0 ? 0 : 1;
}

int scRunUnitTests()
{
    return scRunUnitTestsImpl();
}
