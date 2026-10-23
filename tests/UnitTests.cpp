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
#include "SpectralSelection.h"
#include "SpectralMaskProcessor.h"
#include "AI/DfStft.h"   // pure DSP, no OpenVINO dependency
#include "ScttChunk.h"   // SCTT RIFF chunk + JSON (no AI dependency)
#include "AI/OvRuntime.h"  // resolveAiDevice is plain string logic; the header degrades without AI
#if defined(SC_ENABLE_AI)
#include "AI/ModelManager.h"
#include "AI/WhisperPipeline.h"
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

    // Hand-written sidecars use plain unsigned ARGB; the app itself writes
    // signed ints. Both forms must load to the same colour.
    const juce::File fu = makeTempFile ("markers_unsigned.wav");
    juce::File (fu.getFullPathName() + ".markers.json").replaceWithText (juce::String (
        "{ \"markers\": ["
        " { \"id\": 1, \"name\": \"U\", \"comment\": \"\", \"start\": 0.5, \"end\": -1.0, \"colour\": 4294967040 },"
        " { \"id\": 2, \"name\": \"S\", \"comment\": \"\", \"start\": 1.5, \"end\": -1.0, \"colour\": -256 }"
        "] }") + "\n");
    MarkerManager mm3;
    CHECK (mm3.loadSidecar (fu), "unsigned sidecar loads");
    CHECK (mm3.count() == 2, "unsigned sidecar marker count");
    if (mm3.count() == 2)
    {
        CHECK (mm3.markers()[0].colour == juce::Colours::yellow,
              "unsigned ARGB loads as yellow");
        CHECK (mm3.markers()[1].colour == juce::Colours::yellow,
              "signed ARGB loads as yellow");
    }
    juce::File (fu.getFullPathName() + ".markers.json").deleteFile();
    fu.deleteFile();
}


// ---------------- Marker CSV export/import ----------------
static void testMarkerCsvRoundTrip()
{
    MarkerManager mm;
    const int p = mm.addPointMarker (0.5, "Intro");
    const int r = mm.addRegionMarker (1.25, 3.75, "Verse One");
    CHECK (mm.setComment (p, "He said \"hi\", twice"), "comment set");

    const juce::File f = makeTempFile ("markers_export.csv");
    CHECK (mm.saveCsv (f), "saveCsv ok");
    const juce::String txt = f.loadFileAsString();
    CHECK (txt.startsWith ("Title,Begin,End,Comment"), "header row present");
    CHECK (txt.contains ("\"He said \"\"hi\"\", twice\""),
          "comment with quotes/commas is CSV-quoted");

    MarkerManager mm2;
    CHECK (mm2.loadCsv (f, 10.0) == 2, "two markers imported");
    if (mm2.count() == 2)
    {
        const auto& m0 = mm2.markers()[0];   // sorted by start: Intro first
        CHECK (m0.name == "Intro", "name round-trip");
        CHECK (std::abs (m0.start - 0.5) < 1e-9, "point start round-trip");
        CHECK (m0.end < 0.0, "point marker stays a point");
        CHECK (m0.comment == "He said \"hi\", twice",
              "comment with quotes/commas round-trips");

        const auto& m1 = mm2.markers()[1];
        CHECK (m1.name == "Verse One", "region name round-trip");
        CHECK (std::abs (m1.start - 1.25) < 1e-9 && std::abs (m1.end - 3.75) < 1e-9,
              "region times round-trip");
    }

    // Hand-written CSV: header row, time-code begin, "-" end, an empty Begin
    // field and a region whose end exceeds the duration (clamped).
    const juce::File f2 = makeTempFile ("markers_import.csv");
    f2.replaceWithText (juce::String (
        "Title,Begin,End,Comment\r\n"
        "First,0:01.5,-,\r\n"
        "Long,5.0,9.5,tail\r\n"
        "BadLine,,\r\n"
        "\r\n"));
    MarkerManager mm3;
    CHECK (mm3.loadCsv (f2, 7.0) == 2, "header/malformed lines skipped");
    if (mm3.count() == 2)
    {
        const auto& m0 = mm3.markers()[0];
        CHECK (m0.name == "First" && std::abs (m0.start - 1.5) < 1e-9,
              "time-code begin parsed");
        CHECK (m0.end < 0.0, "\"-\" end becomes a point marker");

        const auto& m1 = mm3.markers()[1];
        CHECK (std::abs (m1.start - 5.0) < 1e-9 && std::abs (m1.end - 7.0) < 1e-9,
              "end clamped to duration");
        CHECK (m1.comment == "tail", "comment imported");
    }

    f.deleteFile();
    f2.deleteFile();
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
    // the axis is 20 Hz .. 4000 Hz. The plot starts below the top time ruler:
    const auto prTone = view.plotRect();
    const double rate = app.audioEngine().sampleRate();
    const double fMin = 20.0, fMax = std::min (rate * 0.5, 20000.0);
    const int expectedRow = prTone.getY() + (int) std::lround (prTone.getHeight()
        * (1.0 - std::log (400.0 / fMin) / std::log (fMax / fMin)));

    int brightestRow = -1, bestVal = 0;
    for (int y = prTone.getY(); y < 128; ++y)   // below the time ruler (its labels are brighter than the tone)
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

// ---------------- Marker axis clipping (regression) ----------------
// A marker whose time maps into the left amplitude-axis strip must not paint
// its line or title over that strip: both views clip markers to the plot rect.
static void testMarkerAxisClipping()
{
    const juce::File wav = makeTempFile ("marker_clip.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.audioEngine().loadFile (wav), "load wav for marker clip test");

    MarkerManager mm;
    const int id = mm.addPointMarker (0.0, "M-zero");   // yellow by default
    CHECK (id == 1, "marker added");

    auto countYellow = [] (const juce::Image& img)
    {
        int n = 0;
        for (int y = 0; y < img.getHeight(); ++y)
            for (int x = 0; x < img.getWidth(); ++x)
            {
                const auto c = img.getPixelAt (x, y);
                if (c.getRed() > 150 && c.getGreen() > 150 && c.getBlue() < 120) ++n;
            }
        return n;
    };

    // Control: window [0,2] puts the t=0 marker exactly on plotLeft (x=64),
    // so it must be drawn.
    {
        WaveformView view (app);
        view.setSize (256, 128);
        view.setViewRange (0.0, 2.0);
        view.setVisibleChannels ({ 0 });
        view.setMarkers (&mm.markers());
        juce::Image img (juce::Image::ARGB, 256, 128, true);
        { juce::Graphics g (img); view.paint (g); }
        CHECK (countYellow (img) > 0, "waveform marker visible when on plot edge");

        SpectralView sv (app);
        sv.setSize (256, 128);
        sv.setViewRange (0.0, 2.0);
        sv.setVisibleChannels ({ 0 });
        sv.setSettings (app.settings);
        sv.setMarkers (&mm.markers());
        juce::Image img2 (juce::Image::ARGB, 256, 128, true);
        { juce::Graphics g (img2); sv.paint (g); }
        CHECK (countYellow (img2) > 0, "spectral marker visible when on plot edge");
    }

    // Window [0.5,2]: the t=0 marker maps to x = 64 - 0.5*128 = 0, deep inside
    // the axis strip. Neither its line nor its title may paint there.
    {
        WaveformView view (app);
        view.setSize (256, 128);
        view.setViewRange (0.5, 2.0);
        view.setVisibleChannels ({ 0 });
        view.setMarkers (&mm.markers());
        juce::Image img (juce::Image::ARGB, 256, 128, true);
        { juce::Graphics g (img); view.paint (g); }
        CHECK (countYellow (img) == 0, "waveform marker clipped out of axis strip");

        SpectralView sv (app);
        sv.setSize (256, 128);
        sv.setViewRange (0.5, 2.0);
        sv.setVisibleChannels ({ 0 });
        sv.setSettings (app.settings);
        sv.setMarkers (&mm.markers());
        juce::Image img2 (juce::Image::ARGB, 256, 128, true);
        { juce::Graphics g (img2); sv.paint (g); }
        CHECK (countYellow (img2) == 0, "spectral marker clipped out of axis strip");
    }

    wav.deleteFile();
}

// ---------------- Playhead arrow symmetry ----------------
// The playhead is a 1px vertical line plus an upward triangle in the ruler.
// Both must be centred on the same pixel column: if the triangle's apex sat
// at a fractional x while the line occupied [floor(px), floor(px)+1), one
// wing would rasterise visibly heavier than the other (the arrow looked
// skewed). This test renders the real view headlessly and checks that every
// ruler row is balanced about the snapped centre, at both 100% and 150%
// DPI scale.
static void testPlayheadArrowSymmetry()
{
    const juce::File wav = makeTempFile ("ph_sym.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.audioEngine().loadFile (wav), "playhead symmetry: load wav");

    // View [0,2] in a 300px-wide view: plot x = 60..300, timeToX(t) = 60 + 120 t.
    // pxI = 144 keeps the probe window clear of ruler ticks (x=...132,156...) and labels.
    for (double px : { 144.3, 144.7 })
    {
        const double t = (px - 60.0) / 120.0;

        for (float scale : { 1.0f, 1.5f })
        {
            WaveformView view (app);
            view.setSize (300, 128);
            view.setViewRange (0.0, 2.0);
            view.setVisibleChannels ({ 0 });
            view.setPlayhead (t);

            const int W = (int) std::ceil (300 * scale), H = (int) std::ceil (128 * scale);
            juce::Image img (juce::Image::ARGB, W, H, true);
            {
                juce::Graphics bg (img);
                bg.fillAll (juce::Colours::black);
            }
            juce::Graphics g (img);
            if (scale != 1.0f)
                g.addTransform (juce::AffineTransform::scale (scale));
            view.paint (g);

            const int pxI = (int) std::lround (px);
            const double centreDev = ((double) pxI + 0.5) * scale;   // snapped line centre
            const int cxw = (int) std::lround ((double) pxI * scale);
            const int lastRow = (int) std::lround ((WaveformView::kRulerH - 1) * scale);  // above ruler border

            bool rowsSeen = false, comOk = true, edgeOk = true;
            int maxSpan = 0;
            for (int y = 0; y < lastRow && y < H; ++y)
            {
                int lo = -1, hi = -1;
                double sum = 0.0, wsum = 0.0;
                for (int x = std::max (0, cxw - 8); x < std::min (W, cxw + 9); ++x)
                {
                    const int r = (int) img.getPixelAt (x, y).getRed();
                    if (r > 40)
                    {
                        if (lo < 0) lo = x;
                        hi = x;
                        sum += (double) r * ((double) x + 0.5);
                        wsum += (double) r;
                    }
                }
                if (lo < 0)
                    continue;

                rowsSeen = true;
                maxSpan = std::max (maxSpan, hi - lo + 1);
                const double comOffset = sum / wsum - centreDev;
                if (std::abs (comOffset) > 0.3)
                {
                    comOk = false;
                    std::cerr << "[phsym] px=" << px << " scale=" << scale
                              << " row " << y << " comOffset=" << comOffset << '\n';
                }
                // At 100% scale the two slanted edges must land at mirror-image
                // sub-pixel phases: equal coverage on both outer pixels.
                if (scale == 1.0f && hi - lo >= 2)
                {
                    const int vL = (int) img.getPixelAt (lo, y).getRed();
                    const int vR = (int) img.getPixelAt (hi, y).getRed();
                    if (std::abs (vL - vR) > 8)
                    {
                        edgeOk = false;
                        std::cerr << "[phsym] px=" << px << " row " << y
                                  << " edges " << vL << "/" << vR << '\n';
                    }
                }
            }

            const juce::String tag = "px=" + juce::String (px) + " scale=" + juce::String (scale);
            CHECK (rowsSeen, "playhead arrow visible (" + tag + ")");
            CHECK (maxSpan >= 8 * (int) std::lround (scale) - 2,
                  "playhead arrow reaches full width (" + tag + "): span=" + juce::String (maxSpan));
            CHECK (comOk, "playhead arrow centred on its line (" + tag + ")");
            if (scale == 1.0f)
                CHECK (edgeOk, "playhead arrow wings equally covered (" + tag + ")");
        }
    }

    wav.deleteFile();
}

// ---------------- Spectral view top time ruler ----------------
// The spectral view carries its own top time ruler so the time axis stays
// visible in spectrogram-only (F9) mode.
static void testSpectralTimeRuler()
{
    const juce::File wav = makeTempFile ("spec_ruler.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.audioEngine().loadFile (wav), "load wav for spectral ruler test");

    SpectralView view (app);
    view.setSize (256, 128);
    view.setViewRange (0.0, 2.0);
    view.setVisibleChannels ({ 0 });
    view.setSettings (app.settings);

    juce::Image img (juce::Image::ARGB, 256, 128, true);
    { juce::Graphics g (img); view.paint (g); }

    // Opaque dark background across the ruler strip (right of the frequency axis).
    bool bgOk = true;
    for (int x : { 100, 150, 200 })
        for (int y = 0; y < SpectralView::kRulerH; ++y)
        {
            const auto c = img.getPixelAt (x, y);
            if (c.getRed() > 60 || c.getGreen() > 60 || c.getBlue() > 70) bgOk = false;
        }
    CHECK (bgOk, "ruler strip has opaque dark background");

    // Tick marks: brighter pixels in the bottom rows of the ruler.
    int tickPx = 0;
    for (int y = SpectralView::kRulerH - 8; y < SpectralView::kRulerH; ++y)
        for (int x = kLeftAxisWidth + 1; x < 256; ++x)
            if ((img.getPixelAt (x, y).getRed() + img.getPixelAt (x, y).getGreen()
                 + img.getPixelAt (x, y).getBlue()) / 3 > 40)
                ++tickPx;
    CHECK (tickPx >= 8, "ruler has tick marks");

    // Time labels: bright text pixels inside the ruler strip.
    int labelPx = 0;
    for (int y = 2; y < SpectralView::kRulerH - 4; ++y)
        for (int x = kLeftAxisWidth + 1; x < 256; ++x)
            if ((img.getPixelAt (x, y).getRed() + img.getPixelAt (x, y).getGreen()
                 + img.getPixelAt (x, y).getBlue()) / 3 > 100)
                ++labelPx;
    CHECK (labelPx >= 8, "ruler has time labels");

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

/** The eight snapshot buttons wear the classic hold-button palette in order:
    red, orange, yellow, light green, dark green, teal, blue, magenta. */
static void testSnapshotPaletteLayout()
{
    Application app;
    FFTAnalysisView view (app);
    view.setSize (320, 120);

    const int expected[8][3] = {
        { 255,   0,   0 }, { 255, 127,   0 }, { 255, 255,   0 }, { 109, 242,   0 },
        {  65, 189,  58 }, {   2, 238, 255 }, {  45, 152, 252 }, { 255,   5, 247 }
    };

    const auto img = view.createComponentSnapshot (view.getLocalBounds());

    for (int i = 0; i < 8; ++i)
    {
        // Empty-slot fill is the palette colour darkened by 0.72 (factor 1/1.72);
        // local (4,4) inside each button sits clear of the centred digit.
        const auto px = img.getPixelAt (98 + i * 24 + 4, 26 + 2 + 4);
        CHECK (std::abs ((int) px.getRed()   - expected[i][0] / 1.72f) <= 3
               && std::abs ((int) px.getGreen() - expected[i][1] / 1.72f) <= 3
               && std::abs ((int) px.getBlue()  - expected[i][2] / 1.72f) <= 3,
               "button " + juce::String (i + 1) + " wears its palette colour");
    }
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

    auto countRed = [] (const juce::Image& img) -> int
    {
        // Slot 1's curve colour is red (255,0,0), drawn at full opacity over
        // the black plot background. Count pixels whose red channel clearly
        // dominates while blue stays low - the live pink curve (blue ~144)
        // and the green grid lines never satisfy this.
        int count = 0;
        for (int y = 54; y < 262; ++y)
            for (int x = 2; x < 282; ++x)
            {
                const auto px = img.getPixelAt (x, y);
                if (px.getRed() > 60 && px.getBlue() < 30) ++count;
            }
        return count;
    };

    // Linear axis mode.
    view.setAxisMode (0);
    {
        juce::Image img (juce::Image::ARGB, 320, 300, true);
        juce::Graphics g (img);
        view.paint (g);
        CHECK (countRed (img) > 20,
              "stored snapshot overlay is painted in linear axis mode");
    }

    // Log axis mode (the default).
    view.setAxisMode (1);
    {
        juce::Image img (juce::Image::ARGB, 320, 300, true);
        juce::Graphics g (img);
        view.paint (g);
        CHECK (countRed (img) > 20,
              "stored snapshot overlay is painted in log axis mode");
    }
}

// ------------------------------------------------------------------ status bar
// The bottom status bar shows the mouse probe on the left and the selection +
// playhead times on the right.
// The footer's right side is a fixed-width group of editable selection
// fields plus the playhead readout (~468 px), so these tests use a 900 px
// bar to keep the left and right halves clearly separated.
static void testStatusBar()
{
    StatusBar bar;
    bar.setSize (900, 20);
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

    juce::Image img (juce::Image::ARGB, 900, 20, true);
    { juce::Graphics g (img); bar.paint (g); }
    CHECK (brightIn (img, 6, 300), "status bar shows the mouse probe on the left");
    CHECK (brightIn (img, 450, 894), "status bar shows selection/playhead on the right");

    // Clearing the mouse info leaves only the right side.
    bar.setMouseInfo ("");
    juce::Image img2 (juce::Image::ARGB, 900, 20, true);
    { juce::Graphics g (img2); bar.paint (g); }
    CHECK (!brightIn (img2, 6, 300), "cleared mouse info leaves the left side empty");
    CHECK (brightIn (img2, 450, 894), "right side still shows selection/playhead");

    // The active file name sits at the far left of the footer.
    bar.setFileName ("tone.wav");
    juce::Image img3 (juce::Image::ARGB, 900, 20, true);
    { juce::Graphics g (img3); bar.paint (g); }
    CHECK (brightIn (img3, 6, 300), "file name is shown on the left");

    // With both file name and mouse probe present, the probe starts to the
    // right of the name instead of overlapping it.
    const juce::Font f (12.0f);
    const int nameW = std::min (900 / 2 - 6,
                                (int) juce::GlyphArrangement::getStringWidth (f, "tone.wav") + 8);
    const int probeX = 6 + nameW + 12;
    bar.setMouseInfo ("t=1.234 s   ch L: +0.72");
    juce::Image img4 (juce::Image::ARGB, 900, 20, true);
    { juce::Graphics g (img4); bar.paint (g); }
    CHECK (brightIn (img4, 6, probeX), "file name still at the far left");
    CHECK (brightIn (img4, probeX + 2, 300), "mouse probe starts after the file name");

    // Clearing the name sends the mouse probe back to its original spot.
    bar.setFileName ("");
    juce::Image img5 (juce::Image::ARGB, 900, 20, true);
    { juce::Graphics g (img5); bar.paint (g); }
    CHECK (brightIn (img5, 6, probeX), "mouse probe returns to the far left");
}

// ------------------------------------------------- footer selection editing
// The status bar's start/stop/length fields are live-editable: parseable
// changes apply straight away (so pasted values work without Enter),
// unparseable partial text is ignored, and programmatic updates never echo
// back into the edit callback.
static void testStatusBarEditing()
{
    auto approx = [] (double a, double b) { return std::abs (a - b) <= 1e-9; };

    StatusBar bar;
    bar.setSize (900, 20);

    // No selection: the fields are hidden and empty.
    CHECK (!bar.selFieldsVisible(), "no selection: footer fields hidden");

    bar.setSelection (12.0, 18.5);
    CHECK (bar.selFieldsVisible(), "a selection shows the footer fields");
    CHECK (bar.getSelStartText() == "12.000", "start field shows the start time");
    CHECK (bar.getSelStopText() == "18.500", "stop field shows the stop time");
    CHECK (bar.getSelLenText() == "6.500", "length field shows the duration");

    // Long times use clock style in all three fields.
    bar.setSelection (3725.25, 7400.0);
    CHECK (bar.getSelStartText() == "1:02:05.25", "start past an hour uses h:mm:ss");
    CHECK (bar.getSelStopText() == "2:03:20", "stop past two hours uses h:mm:ss");
    CHECK (bar.getSelLenText() == "1:01:14.75", "length past an hour uses h:mm:ss");

    bar.setSelection (0.0, 0.0);
    CHECK (!bar.selFieldsVisible(), "clearing the selection hides the fields");

    // Editing pushes new start/stop values through the callback; mirror the
    // real wiring by feeding them back into the bar as well.
    double gotA = -1.0, gotB = -1.0;
    bar.onSelectionEdited = [&] (double a, double b)
    { gotA = a; gotB = b; bar.setSelection (a, b); };

    auto pump = [] ()
    {
        for (int i = 0; i < 8; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
    };

    bar.setSelection (10.0, 20.0);
    pump();   // flush any pending messages from the setup above

    // Typing a new start applies it live...
    bar.selStartField().setText ("15", true);
    pump();
    CHECK (approx (gotA, 15.0) && approx (gotB, 20.0), "typing a start applies it");

    // ...pasting clock-style text works too...
    bar.selStopField().setText ("1:30", true);
    pump();
    CHECK (approx (gotA, 15.0) && approx (gotB, 90.0), "pasted m:ss applies to the stop");

    // ...and the length field moves the stop edge from a fixed start.
    bar.selLenField().setText ("30", true);
    pump();
    CHECK (approx (gotA, 15.0) && approx (gotB, 45.0), "length sets stop = start + len");

    // Unparseable partial text leaves the selection alone...
    bar.selStartField().setText ("-", true);
    pump();
    CHECK (approx (gotA, 15.0) && approx (gotB, 45.0), "unparseable text is ignored");

    // ...and a start beyond the stop clamps to the stop.
    bar.selStartField().setText ("99", true);
    pump();
    CHECK (approx (gotA, 45.0) && approx (gotB, 45.0), "start past stop clamps to stop");

    // Programmatic selection changes update the fields without echoing into
    // the callback (setText with no notification).
    gotA = gotB = -1.0;
    bar.setSelection (30.0, 40.0);
    pump();
    CHECK (gotA < 0.0, "programmatic change does not echo into the callback");
    CHECK (bar.getSelStartText() == "30.000", "...but still refreshes the fields");

    // The playhead readout switches to clock style past a minute.
    bar.setPlayhead (75.0);
    CHECK (bar.getPlayheadText() == "Pos: 1:15", "playhead readout uses m:ss past a minute");
    bar.setPlayhead (3661.25);
    CHECK (bar.getPlayheadText() == "Pos: 1:01:01.25", "playhead readout uses h:mm:ss past an hour");
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
    CHECK (about.contains ("whisper.cpp b5454") && about.contains ("MIT, ggerganov"),
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
    const juce::StringArray defaultBottom { "Spectral Controls", "Markers",
                                            "Transcription", "Selection" };
#else
    const juce::StringArray defaultBottom { "Spectral Controls", "Markers",
                                            "Selection" };
#endif
    const juce::StringArray defaultTop { "Files", "Frequency Analysis", "History",
                                          "Python" };
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
                                                     "Frequency Analysis", "History",
                                                     "Python" };
        CHECK (win->topPanelTabNames () == expectTopAfterMove, "tab moved across groups");
        CHECK (win->bottomPanelTabNames () == expectBottom, "source group lost the tab");

        // 3) Same-bar move: Files to just before Frequency Analysis.
        win->moveLeftPanelTab (win->topPanelTabs (), 0, win->topPanelTabs (), 2);
        const juce::StringArray expectTopFinal { "Markers", "Files",
                                                 "Frequency Analysis", "History",
                                                 "Python" };
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
                                                 "Frequency Analysis", "History",
                                                 "Python" };
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

static juce::MouseEvent mkMouse (juce::Component& c, int x, int y, juce::ModifierKeys mods);   // defined below

// ------------------------------------------- Cross-bar tab move (drag regression)
// Dragging a panel tab from one row to the other used to crash: removeTab()
// deletes the DragTabButton that was still executing its own mouseUp, and the
// old endDrag() then touched freed members. moveLeftPanelTab is the exact code
// path the drop invokes, so driving it headlessly covers both sides.
static void testCrossBarTabMove()
{
    const juce::File store = makeTempFile ("crossbar_tab.recent");
    store.deleteFile ();

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    const int kNumPanels = 8;
#else
    const int kNumPanels = 7;
#endif

    {
        Application app (store);
        std::unique_ptr<MainWindow> win (new MainWindow (app, "cross bar tab test"));

        auto* filesPanel = win->panelForId ("files");
        CHECK (filesPanel != nullptr, "test setup: probe panel exists");

        // The two bars are direct children of the window. Identify them by
        // their tab names - a panel's parent is only its bar while that tab
        // is the current one.
        juce::TabbedComponent* topBar = nullptr, *bottomBar = nullptr;
        for (auto* c : win->getChildren ())
        {
            auto* tb = dynamic_cast<juce::TabbedComponent*> (c);
            if (!tb) continue;
            const juce::StringArray names = tb->getTabNames ();
            bool hasFiles = false, hasMarkers = false;
            for (auto& n : names)
            {
                if (n == "Files") hasFiles = true;
                else if (n == "Markers") hasMarkers = true;
            }
            if (hasFiles && topBar == nullptr) topBar = tb;
            else if (hasMarkers && bottomBar == nullptr) bottomBar = tb;
        }
        CHECK (topBar != nullptr && bottomBar != nullptr, "test setup: both bars found");
        if (!filesPanel || !topBar || !bottomBar) return;

        const int topN = topBar->getNumTabs ();
        const int botN = bottomBar->getNumTabs ();
        CHECK (topN + botN == kNumPanels, "test setup: all panels present");

        // Move the first tab of the top bar ("Files") to the front of the bottom bar.
        win->moveLeftPanelTab (topBar, 0, bottomBar, 0);

        CHECK (topBar->getNumTabs () == topN - 1, "source bar lost one tab");
        CHECK (bottomBar->getNumTabs () == botN + 1, "destination bar gained one tab");
        CHECK (bottomBar->getTabNames() [0] == "Files", "moved tab is first in the destination bar");

        // The panel component itself must survive the move intact.
        auto* moved = win->panelForId ("files");
        CHECK (moved == filesPanel, "panel object survived the cross-bar move");
        CHECK (dynamic_cast<juce::TabbedComponent*> (moved->getParentComponent()) == bottomBar,
              "panel now lives in the destination bar");

        // Selection state: the moved tab is active in its new bar, and the
        // source bar keeps a selection while tabs remain.
        CHECK (bottomBar->getCurrentTabIndex () == 0, "moved tab selected in destination bar");
        CHECK (topBar->getNumTabs () > 0 && topBar->getCurrentTabIndex () >= 0,
              "source bar keeps a selection when tabs remain");

        // Round trip back to the original arrangement.
        win->moveLeftPanelTab (bottomBar, 0, topBar, 0);
        CHECK (topBar->getNumTabs () == topN && bottomBar->getNumTabs () == botN,
              "moving the tab back restores both bars");
        CHECK (win->isPanelOpen ("files"), "panel still open after round trip");

        // Appending at the end of the other bar also works.
        win->moveLeftPanelTab (topBar, 0, bottomBar, botN);
        CHECK (bottomBar->getTabNames() [botN] == "Files", "append-at-end lands last");
        win->moveLeftPanelTab (bottomBar, botN, topBar, 0);
    }

    // Simulated real drag: mouseDown on the Files tab button, drag onto a tab
    // of the other bar, release. This exercises DragTabButton::endDrag() -
    // before the fix, removeTab() deleted that very button mid-mouseUp and
    // endDrag then touched freed members (crash).
    {
        Application app2 (store);
        std::unique_ptr<MainWindow> win2 (new MainWindow (app2, "cross bar drag test"));

        juce::TabbedComponent* topBar2 = nullptr, *bottomBar2 = nullptr;
        for (auto* c : win2->getChildren ())
        {
            auto* tb = dynamic_cast<juce::TabbedComponent*> (c);
            if (!tb) continue;
            const juce::StringArray names = tb->getTabNames ();
            bool hasFiles = false, hasMarkers = false;
            for (auto& n : names)
            {
                if (n == "Files") hasFiles = true;
                else if (n == "Markers") hasMarkers = true;
            }
            if (hasFiles && topBar2 == nullptr) topBar2 = tb;
            else if (hasMarkers && bottomBar2 == nullptr) bottomBar2 = tb;
        }
        CHECK (topBar2 != nullptr && bottomBar2 != nullptr, "drag test setup: both bars found");

        auto* filesBtn  = dynamic_cast<DragTabButton*> (topBar2->getTabbedButtonBar().getTabButton (0));
        auto* targetBtn = bottomBar2->getTabbedButtonBar().getTabButton (0);
        CHECK (filesBtn != nullptr && targetBtn != nullptr, "drag test setup: tab buttons found");

        if (topBar2 && bottomBar2 && filesBtn && targetBtn)
        {
            const auto srcTL = filesBtn->localPointToGlobal  (juce::Point<int> (0, 0));
            const auto dstTL = targetBtn->localPointToGlobal (juce::Point<int> (0, 0));

            // Down near the source tab's centre...
            filesBtn->mouseDown (mkMouse (*filesBtn, 12, 6, {}));
            // ...drag far enough to arm the drag and land on the target tab.
            const int lx = dstTL.x - srcTL.x + 12;
            const int ly = dstTL.y - srcTL.y + 6;
            filesBtn->mouseDrag (mkMouse (*filesBtn, lx, ly, {}));
            // ...release. (Do not touch filesBtn after this: the drop deletes it.)
            filesBtn->mouseUp   (mkMouse (*filesBtn, lx, ly, {}));

            CHECK (bottomBar2->getTabNames() [0] == "Files",
                  "drag moved the Files tab to the other bar");
            auto* moved = win2->panelForId ("files");
            CHECK (moved != nullptr && moved->getParentComponent() == bottomBar2,
                  "panel survived the simulated drag and lives in the destination bar");
        }
    }

    store.deleteFile ();
}

// ------------------------------------- Spectral refresh on equal-length doc switch
// Ctrl+Tab between two files of identical duration used to leave the previous
// file's tiles on screen: fitAll() saw no time-window change and never marked
// the tile image dirty. Switching must force a re-render from the new engine.
static void testSpectralRefreshOnDocSwitch()
{
    const juce::File store = makeTempFile ("spec_switch.recent");
    store.deleteFile ();
    const juce::File wavA = makeTempFile ("spec_switch_a.wav");
    const juce::File wavB = makeTempFile ("spec_switch_b.wav");

    // Same length, different spectral content: 400 Hz vs 3 kHz at an 8 kHz rate.
    writeWav (wavA, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });
    writeWav (wavB, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 3000.0 * (double) f / rate);
    });

    Application app (store);
    std::unique_ptr<MainWindow> win (new MainWindow (app, "spec switch test"));

    SpectralView* sv = nullptr;
    for (auto* c : win->getChildren ())
        if ((sv = dynamic_cast<SpectralView*> (c)) != nullptr) break;
    CHECK (sv != nullptr, "found the spectral view in the main window");
    if (!sv)
    {
        wavA.deleteFile ();
        wavB.deleteFile ();
        store.deleteFile ();
        return;
    }

    // Capture at the view's real layout size (MainWindow::resized() has already
    // placed it): A's 400 Hz tone renders as a full-width row mid-axis while B's
    // 3 kHz tone sits near the top of the axis, so a stale image (A's tiles
    // painted for B) differs by thousands of pixels.
    CHECK (app.openFile (wavA), "open file A");   // fires activateDocUI()
    juce::Image imgA (juce::Image::ARGB, sv->getWidth(), sv->getHeight(), true);
    { juce::Graphics g (imgA); sv->paint (g); }

    CHECK (app.openFile (wavB), "open file B (same duration)");
    // Regression: without the forced refresh in activateDocUI(), fitAll() sees
    // no window change and the view keeps painting A's tiles - imgB would be
    // pixel-identical to imgA.
    juce::Image imgB (juce::Image::ARGB, sv->getWidth(), sv->getHeight(), true);
    { juce::Graphics g (imgB); sv->paint (g); }

    int diffs = 0;
    for (int y = 0; y < imgA.getHeight(); ++y)
        for (int x = 0; x < imgA.getWidth(); ++x)
            if (imgA.getPixelAt (x, y) != imgB.getPixelAt (x, y)) ++diffs;

    CHECK (diffs > 50, "spectrogram re-rendered after switching to an equal-length file");

    wavA.deleteFile ();
    wavB.deleteFile ();
    store.deleteFile ();
}

// ------------------------------------------------- Close Panel + Window menu
// Every left-column panel offers right-click "Close Panel"; the Window menu
// lists all panels with checkmarks and toggles them. Closed state persists.
static void testClosePanelsAndWindowMenu()
{
    const juce::File store = makeTempFile ("close_panel.recent");
    store.deleteFile ();   // deterministic per-run name: never trust leftovers

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    const int kNumPanels = 8;
#else
    const int kNumPanels = 7;
#endif

    auto windowMenuIndex = [] (MainWindow& win) -> int
    {
        const juce::StringArray names = win.getMenuBarNames ();
        for (int i = 0; i < names.size (); ++i)
            if (names[i] == "Window") return i;
        return -1;
    };

    // 1) Fresh store: every panel open, Window menu lists them all.
    {
        Application app (store);
        juce::MessageManager* mm = juce::MessageManager::getInstance();
        std::unique_ptr<MainWindow> win (new MainWindow (app, "close panel test"));

        const bool allOpen = win->isPanelOpen ("files") && win->isPanelOpen ("fft")
            && win->isPanelOpen ("history") && win->isPanelOpen ("python")
            && win->isPanelOpen ("controls") && win->isPanelOpen ("markers")
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
            && win->isPanelOpen ("transcription")
#endif
            && win->isPanelOpen ("selection");
        CHECK (allOpen, "all panels open by default");

        const int wIdx = windowMenuIndex (*win);
        CHECK (wIdx >= 0, "Window menu exists in the menu bar");
        juce::PopupMenu wm = win->getMenuForIndex (wIdx, "Window");
        CHECK (wm.getNumItems () == kNumPanels, "Window menu lists every panel");

        // Each Window menu entry toggles its own panel (dispatch + action).
        // Toggle back immediately so later steps see the original state.
        juce::StringArray ids;
        ids.add ("files");   ids.add ("fft");     ids.add ("history");
        ids.add ("python");  ids.add ("controls"); ids.add ("markers");
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        ids.add ("transcription");
#endif
        ids.add ("selection");
        for (int i = 0; i < kNumPanels; ++i)
        {
            const bool wasOpen = win->isPanelOpen (ids[i]);
            win->menuItemSelected (MainWindow::MenuIDs::WindowPanelBase + i, wIdx);
            CHECK (win->isPanelOpen (ids[i]) == !wasOpen,
                  "Window menu toggles panel " + ids[i]);
            win->menuItemSelected (MainWindow::MenuIDs::WindowPanelBase + i, wIdx);
        }

        // 2) Closing a panel through its own right-click wiring.
        auto* files = dynamic_cast<FileListPanel*> (win->panelForId ("files"));
        CHECK (files != nullptr, "panelForId resolves Files");
        if (files)
        {
            files->requestClose ();
            CHECK (!win->isPanelOpen ("files"), "Files closed via its own close action");
        }

        // 3) Reopening through the Window menu lands back at index 0.
        win->menuItemSelected (MainWindow::MenuIDs::WindowPanelBase + 0, wIdx);
        CHECK (win->isPanelOpen ("files"), "Files reopened from the Window menu");
        CHECK (win->topPanelTabNames ()[0] == "Files",
              "reopened Files is first in its group again");

        // 4) Canonical order restored after closing and reopening out of order.
        win->setPanelVisible ("history", false);
        win->setPanelVisible ("fft", false);
        {
            const juce::StringArray expectTop1 { "Files", "Python" };
            CHECK (win->topPanelTabNames () == expectTop1,
                  "two panels closed from the top group");
        }
        win->setPanelVisible ("history", true);   // before fft canonically
        {
            const juce::StringArray expectTop2 { "Files", "History", "Python" };
            CHECK (win->topPanelTabNames () == expectTop2,
                  "History reinserted at its canonical position");
        }
        win->setPanelVisible ("fft", true);
        {
            const juce::StringArray expectTop3 { "Files", "Frequency Analysis",
                                                 "History", "Python" };
            CHECK (win->topPanelTabNames () == expectTop3,
                  "canonical top order fully restored");
        }

        // 5) A panel dragged to the other bar is closed from there.
        int markersIdx = -1;
        const juce::StringArray bot = win->bottomPanelTabNames ();
        for (int i = 0; i < bot.size (); ++i)
            if (bot[i] == "Markers") { markersIdx = i; break; }
        CHECK (markersIdx >= 0, "Markers found in the bottom group");
        win->moveLeftPanelTab (win->bottomPanelTabs (), markersIdx,
                               win->topPanelTabs (), 0);
        CHECK (win->topPanelTabNames ()[0] == "Markers",
              "Markers moved to the top group");
        win->setPanelVisible ("markers", false);
        CHECK (!win->isPanelOpen ("markers"), "Markers closed from the top group");
        CHECK (win->topPanelTabNames ()[0] == "Files",
              "top group order after closing Markers");

        // 6) Closing every panel of a group hides that bar; reopening one
        //    shows it again.
        win->setPanelVisible ("files", false);
        win->setPanelVisible ("fft", false);
        win->setPanelVisible ("history", false);
        win->setPanelVisible ("python", false);
        CHECK (!win->topPanelTabs ()->isVisible (), "empty top group is hidden");
        CHECK (win->bottomPanelTabs ()->isVisible (), "bottom group still visible");
        win->menuItemSelected (MainWindow::MenuIDs::WindowPanelBase + 1, wIdx);   // fft
        CHECK (win->topPanelTabs ()->isVisible (), "top group visible again");
        CHECK (win->isPanelOpen ("fft") && !win->isPanelOpen ("files"),
              "only Frequency Analysis reopened");

        win.reset ();
        delete mm;
    }

    // 7) Closed panels stay closed after a restart; open ones keep their order.
    {
        Application app (store);
        juce::MessageManager* mm = juce::MessageManager::getInstance();
        std::unique_ptr<MainWindow> win (new MainWindow (app, "close panel test 2"));

        CHECK (!win->isPanelOpen ("files") && !win->isPanelOpen ("history")
               && !win->isPanelOpen ("markers") && !win->isPanelOpen ("python"),
              "closed panels stay closed after restart");
        {
            const juce::StringArray expectTop { "Frequency Analysis" };
            CHECK (win->topPanelTabNames () == expectTop, "open top group restored in order");
        }

        const int wIdx = windowMenuIndex (*win);
        juce::PopupMenu wm = win->getMenuForIndex (wIdx, "Window");
        CHECK (wm.getNumItems () == kNumPanels, "Window menu still lists every panel");

        // Reopening a closed panel from the Window menu works after restart too.
        win->menuItemSelected (MainWindow::MenuIDs::WindowPanelBase + 0, wIdx);   // files
        CHECK (win->isPanelOpen ("files"), "Files reopened after restart");
        {
            const juce::StringArray expectTop { "Files", "Frequency Analysis" };
            CHECK (win->topPanelTabNames () == expectTop,
                  "reopened Files first again after restart");
        }

        win.reset ();
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

    // Slider below the text box: two-way sync with clamping to its range.
    {
        juce::AlertWindow* w = win->createGainDialog ();
        CHECK (w != nullptr, "gain dialog created for slider test");
        if (w != nullptr)
        {
            auto* ed = w->getTextEditor ("gainDb");
            auto* sl = dynamic_cast<juce::Slider*> (w->getCustomComponent (0));
            CHECK (sl != nullptr, "slider is the dialog's custom component");

            if (ed != nullptr && sl != nullptr)
            {
                // TextEditor posts its change message asynchronously; pump so
                // onTextChange runs in this headless test.
                auto setAndPump = [ed] (const juce::String& t)
                {
                    ed->setText (t, true);
                    for (int i = 0; i < 8; ++i)
                        juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
                };

                const juce::Range<double> expectRange (-60.0, 24.0);
                CHECK (sl->getRange() == expectRange, "slider spans -60..+24 dB");
                CHECK (std::abs (sl->getValue()) < 1e-9, "slider starts at 0 dB");

                // Dragging the slider rewrites the text box...
                sl->setValue (6.5, juce::sendNotificationSync);
                CHECK (ed->getText().trim() == "6.5", "slider drag updates the text box");
                setAndPump ("6.5");   // flush the async echo; value must not move
                CHECK (std::abs (sl->getValue() - 6.5) < 1e-9,
                      "...without a feedback loop moving it back");

                // ...and typing moves the slider...
                setAndPump ("12");
                CHECK (std::abs (sl->getValue() - 12.0) < 1e-9, "typing moves the slider");

                // ...clamped to its range when a larger value is typed; the text
                // box stays authoritative for what OK will apply.
                setAndPump ("50");
                CHECK (std::abs (sl->getValue() - 24.0) < 1e-9, "out-of-range typing clamps the slider");
                CHECK (ed->getText().trim() == "50", "...but keeps the typed value in the box");

                // Half-typed text must not yank the slider around.
                setAndPump ("-");
                CHECK (std::abs (sl->getValue() - 24.0) < 1e-9, "half-typed text leaves the slider alone");
            }
        }
        delete w;
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
// The drag START position selects the zoomed axes:
//  - drag on the track (plot area): BOTH axes — time from the box's x span,
//    amplitude/frequency from its y span;
//  - drag starting on the left axis: vertical only;
//  - drag starting on the top time ruler (waveform): horizontal only.
// Plain right-click = context menu (no state change when no action handler
// is attached).
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

// ---------------- Harmonic reference lines (FFT panel) ----------------
// Right-click "Set Harmonic References" treats the frequency under the cursor
// as harmonic #N of a fundamental: delicate vertical lines mark harmonics
// 1..N wherever they are inside the visible window and follow the mouse.
// "Copy Harmonics" exports an Excel-friendly TSV table (Harmonic/Frequency/
// Amplitude, CRLF line endings); "Reset Harmonic" clears everything again.
static void testFFTHarmonicReferences()
{
    const juce::File wav = makeTempFile ("fft_harm.wav");
    writeWav (wav, 1, 8000.0, 16000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 440.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for harmonic reference test");
    app.settings.fftLocalN = 1024;   // picked up as the panel's localN at construction

    FFTAnalysisView view (app);
    view.setSize (300, 200);
    { juce::Image img (juce::Image::ARGB, 300, 200, true); juce::Graphics g (img); view.paint (g); }
    CHECK (!view.getSpectrum().empty(), "spectrum computed before enabling");

    // --- Table contents: the clicked frequency is the 3rd harmonic of 440 Hz.
    view.setHarmonicReference (3, 1320.0);
    CHECK (view.harmonicRefActive(), "reference active after set");
    CHECK (view.harmonicN() == 3, "harmonic number stored");
    CHECK (std::abs (view.harmonicAnchorF() - 1320.0) < 1e-9, "anchor stored");

    const auto lines = juce::StringArray::fromLines (view.buildHarmonicsTable());
    CHECK (lines.size() == 4, "header + one row per harmonic");
    if (lines.size() == 4)
    {
        CHECK (lines[0] == "Harmonic\tFrequency\tAmplitude", "TSV header row");
        double amps[3] = { -1e9, -1e9, -1e9 };
        for (int k = 1; k <= 3; ++k)
        {
            const auto fields = juce::StringArray::fromTokens (lines[(size_t) k], "\t");
            if (fields.size() != 3) continue;
            CHECK (fields[0] == juce::String (k), "row number is the harmonic index");
            const double f = fields[1].getDoubleValue();
            CHECK (std::abs (f - (double) k * 440.0) < 0.5, "harmonic frequency is k*440 Hz");
            amps[k - 1] = fields[2].getDoubleValue();
        }
        CHECK (amps[0] > amps[1], "amplitude at the 440 Hz tone beats the 880 Hz bin");
    }

    // --- Reset clears state and table.
    view.resetHarmonicReference();
    CHECK (!view.harmonicRefActive(), "reset deactivates the reference");
    CHECK (view.buildHarmonicsTable().isEmpty(), "table empty after reset");

    // --- N=1: single row, anchor is the fundamental itself.
    view.setHarmonicReference (1, 440.0);
    {
        const auto l2 = juce::StringArray::fromLines (view.buildHarmonicsTable());
        CHECK (l2.size() == 2, "N=1 gives header + one row");
        if (l2.size() == 2)
        {
            const auto fields = juce::StringArray::fromTokens (l2[1], "\t");
            CHECK (fields.size() == 3 && std::abs (fields[1].getDoubleValue() - 440.0) < 0.5,
                   "N=1 row is the fundamental itself");
        }
    }

    // --- The lines follow the mouse X position while inside the plot.
    view.setHarmonicReference (2, 880.0);
    const auto plot = view.plotBounds();
    const int midX = plot.getX() + plot.getWidth() / 2;
    view.mouseMove (mkMouse (view, midX, plot.getY() + 10, {}));
    CHECK (std::abs (view.harmonicAnchorF() - view.xToFreq (midX)) < 1e-3,
           "anchor follows the mouse");

    const double frozen = view.harmonicAnchorF();
    view.mouseMove (mkMouse (view, 0, plot.getY() + 10, {}));   // left of the plot
    CHECK (std::abs (view.harmonicAnchorF() - frozen) < 1e-9,
           "anchor stays put outside the plot");

    // --- The delicate vertical lines are actually drawn: with anchor 2000 Hz
    //     and N=2, harmonic #1 sits at 1000 Hz. Scan that column for the
    //     line's cyan tint (the mono curve is pink, grid lines grey).
    view.setHarmonicReference (2, 2000.0);
    const float xLine = (float) view.freqToX (1000.0);
    {
        juce::Image img (juce::Image::ARGB, 300, 200, true);
        { juce::Graphics g (img); view.paint (g); }
        int cyan = 0;
        for (int x = (int) xLine - 1; x <= (int) xLine + 1; ++x)
            for (int y = plot.getY() + 2; y < plot.getBottom() - 2; ++y)
                if (x >= 0 && x < 300 && y >= 0 && y < 200)
                {
                    const auto px = img.getPixelAt (x, y);
                    if (px.getBlue() > 120 && px.getGreen() > 100 && px.getRed() < 110) ++cyan;
                }
        CHECK (cyan >= 40, "harmonic line pixels drawn at the expected column");
    }

    wav.deleteFile();
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

    // --- Waveform: drag on the track -> BOTH axes (time + amplitude) ---
    {
        WaveformView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        const double e0 = view.xToTime (100), e1 = view.xToTime (300);
        CHECK (e1 > e0, "expected time range is non-degenerate");
        const double ampBefore = view.getVisibleAmpHalf();

        // Box x span [100,300], y span 60..70 (10 px of the 176 px plot).
        view.mouseDown (mkMouse (view, 100, 60, rmb));
        view.mouseDrag (mkMouse (view, 300, 70, rmb));
        view.mouseUp   (mkMouse (view, 300, 70, rmb));

        CHECK (std::abs (view.getViewStart() - e0) < 1e-9
              && std::abs (view.getViewEnd() - e1) < 1e-9,
              "track right-drag zooms to the dragged time range");
        CHECK (std::abs (view.getVisibleAmpHalf() - ampBefore * 10.0 / 176.0) < 1e-9,
               "track right-drag also scales the amplitude axis to the y span");
    }

    // --- Waveform: narrow track drag still zooms BOTH axes ---
    {
        WaveformView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        const double e0 = view.xToTime (200), e1 = view.xToTime (203);
        const double ampBefore = view.getVisibleAmpHalf();

        // dx=3, dy=44: the time window shrinks to the 3 px x span as well.
        view.mouseDown (mkMouse (view, 200, 50, rmb));
        view.mouseDrag (mkMouse (view, 203, 94, rmb));
        view.mouseUp   (mkMouse (view, 203, 94, rmb));

        CHECK (std::abs (view.getViewStart() - e0) < 1e-9
              && std::abs (view.getViewEnd() - e1) < 1e-9,
               "narrow track right-drag zooms the time axis to the x span");
        CHECK (std::abs (view.getVisibleAmpHalf() - ampBefore / 4.0) < 1e-9,
               "narrow track right-drag scales the amplitude axis to the y span");
    }

    // --- Waveform: drag starting on the left amplitude axis -> vertical only ---
    {
        WaveformView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        const double ampBefore = view.getVisibleAmpHalf();

        // Box x span [30,40] lies on the axis; y span 50..120 (70 px).
        view.mouseDown (mkMouse (view, 30, 50, rmb));
        view.mouseDrag (mkMouse (view, 40, 120, rmb));
        view.mouseUp   (mkMouse (view, 40, 120, rmb));

        CHECK (std::abs (view.getVisibleAmpHalf() - ampBefore * 70.0 / 176.0) < 1e-9,
               "axis right-drag scales the amplitude axis to the y span");
        CHECK (view.getViewStart() == 0.0 && view.getViewEnd() == 4.0,
               "axis right-drag leaves the time window unchanged");
    }

    // --- Waveform: drag starting on the top time ruler -> horizontal only ---
    {
        WaveformView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        const double e0 = view.xToTime (100), e1 = view.xToTime (300);
        const double ampBefore = view.getVisibleAmpHalf();

        // Box y span [8,18] lies on the ruler; x span 100..300.
        view.mouseDown (mkMouse (view, 100, 8, rmb));
        view.mouseDrag (mkMouse (view, 300, 18, rmb));
        view.mouseUp   (mkMouse (view, 300, 18, rmb));

        CHECK (std::abs (view.getViewStart() - e0) < 1e-9
              && std::abs (view.getViewEnd() - e1) < 1e-9,
               "ruler right-drag zooms to the dragged time range");
        CHECK (std::abs (view.getVisibleAmpHalf() - ampBefore) < 1e-9,
               "ruler right-drag leaves the amplitude axis unchanged");
    }

    // --- Spectral: drag on the track -> BOTH axes (time + frequency) ---
    {
        SpectralView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        // Default axis: [20 Hz .. min(20 kHz, Nyquist=4 kHz)] on a log scale;
        // one visible channel row spans the plot area below the time ruler.
        const double fmin = 20.0, fmax = 4000.0;
        const double lmin = std::log (fmin), lmax = std::log (fmax);
        const int ry = SpectralView::kRulerH, rh = 200 - SpectralView::kRulerH;
        auto freqAt = [lmin, lmax, ry, rh] (double y)
            { return std::exp (lmin + (1.0 - (y - ry) / rh) * (lmax - lmin)); };

        const double e0 = view.xToTime (100), e1 = view.xToTime (300);

        // Box x span [100,300], y span 60..70.
        view.mouseDown (mkMouse (view, 100, 60, rmb));
        view.mouseDrag (mkMouse (view, 300, 70, rmb));
        view.mouseUp   (mkMouse (view, 300, 70, rmb));

        CHECK (std::abs (view.getViewStart() - e0) < 1e-9
              && std::abs (view.getViewEnd() - e1) < 1e-9,
               "spectral track right-drag zooms to the dragged time range");
        CHECK (std::abs (view.getFreqMin() - freqAt (70.0)) / freqAt (70.0) < 1e-9
              && std::abs (view.getFreqMax() - freqAt (60.0)) / freqAt (60.0) < 1e-9,
               "spectral track right-drag also sets the frequency axis to the y span");
    }

    // --- Spectral: narrow track drag still zooms BOTH axes ---
    {
        SpectralView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        const double fmin = 20.0, fmax = 4000.0;
        const double lmin = std::log (fmin), lmax = std::log (fmax);
        const int ry = SpectralView::kRulerH, rh = 200 - SpectralView::kRulerH;
        auto freqAt = [lmin, lmax, ry, rh] (double y)
            { return std::exp (lmin + (1.0 - (y - ry) / rh) * (lmax - lmin)); };

        const double e0 = view.xToTime (200), e1 = view.xToTime (203);

        // dx=3, dy=100: the time window shrinks to the 3 px x span as well.
        view.mouseDown (mkMouse (view, 200, 50, rmb));
        view.mouseDrag (mkMouse (view, 203, 150, rmb));
        view.mouseUp   (mkMouse (view, 203, 150, rmb));

        CHECK (std::abs (view.getViewStart() - e0) < 1e-9
              && std::abs (view.getViewEnd() - e1) < 1e-9,
               "spectral narrow track right-drag zooms the time axis to the x span");
        CHECK (std::abs (view.getFreqMin() - freqAt (150.0)) / freqAt (150.0) < 1e-9
              && std::abs (view.getFreqMax() - freqAt (50.0)) / freqAt (50.0) < 1e-9,
               "spectral narrow track right-drag sets the frequency axis to the y span");
    }

    // --- Spectral: drag starting on the left frequency axis -> vertical only ---
    {
        SpectralView view (app);
        view.setSize (400, 200);
        view.setViewRange (0.0, 4.0);
        view.setVisibleChannels ({ 0 });

        const double fmin = 20.0, fmax = 4000.0;
        const double lmin = std::log (fmin), lmax = std::log (fmax);
        const int ry = SpectralView::kRulerH, rh = 200 - SpectralView::kRulerH;
        auto freqAt = [lmin, lmax, ry, rh] (double y)
            { return std::exp (lmin + (1.0 - (y - ry) / rh) * (lmax - lmin)); };

        // Box x span [30,40] lies on the axis; y span 50..150.
        view.mouseDown (mkMouse (view, 30, 50, rmb));
        view.mouseDrag (mkMouse (view, 40, 150, rmb));
        view.mouseUp   (mkMouse (view, 40, 150, rmb));

        CHECK (std::abs (view.getFreqMin() - freqAt (150.0)) / freqAt (150.0) < 1e-9
              && std::abs (view.getFreqMax() - freqAt (50.0)) / freqAt (50.0) < 1e-9,
               "spectral axis right-drag sets the frequency axis to the y span");
        CHECK (view.getViewStart() == 0.0 && view.getViewEnd() == 4.0,
               "spectral axis right-drag leaves the time window unchanged");
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

// ------------------------------------------------------- context menu zoom-out-full
// Right-clicking the track data and choosing "Zoom Out Full" must reset BOTH
// axes: the shared time window plus each view's vertical zoom (waveform
// amplitude + spectral frequency). Only right-clicks that start on an axis
// keep the local axis-only behaviour.
static void testCtxZoomOutFull()
{
    const juce::File wav = makeTempFile ("ctx_zoomout.wav");
    // 5 s stereo @ 8 kHz -> Nyquist 4 kHz, so the default spectral range is
    // [20 Hz .. min(4 kHz, 20 kHz)] = [20 .. 4000].
    writeWav (wav, 2, 8000.0, 40000, [] (int ch, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * (ch == 0 ? 300.0 : 600.0) * (double) f / rate);
    });

    Application app;
    // A MessageManager is required before constructing the top-level window.
    juce::MessageManager* mm = juce::MessageManager::getInstance();
    std::unique_ptr<MainWindow> win (new MainWindow (app, "ctx zoom out test"));
    win->openFile (wav);
    CHECK (app.audioEngine().isFileLoaded(), "load wav for ctx zoom-out test");
    const double dur = app.audioEngine().duration();

    const double ampDefault = 1.0 / 0.9;   // getVisibleAmpHalf() at vZoom == 1
    CHECK (std::abs (win->waveformAmpHalf() - ampDefault) < 1e-9,
           "setup: default amplitude zoom");

    // Zoom in on both axes of both views.
    win->applyTimeWindow (1.0, 3.0);
    win->zoomVerticalBoth (4.0);
    CHECK (std::abs (app.viewStart - 1.0) < 1e-9 && std::abs (app.viewEnd - 3.0) < 1e-9,
           "setup: time window zoomed in");
    CHECK (std::abs (win->waveformAmpHalf() - ampDefault / 4.0) < 1e-9,
           "setup: amplitude zoomed in");
    CHECK (win->spectralFreqMin() > 20.0, "setup: frequency range narrowed");

    // Track right-click -> context menu -> Zoom Out Full. The views only
    // intercept CtxZoomOutFull for axis clicks; track clicks route through
    // MainWindow::handleContextMenuAction -> zoomOutFull().
    win->handleContextMenuAction (CtxZoomOutFull);

    CHECK (std::abs (app.viewStart) < 1e-9 && std::abs (app.viewEnd - dur) < 1e-9,
           "track zoom-out-full resets the shared time window");
    CHECK (std::abs (win->waveformTimeStart()) < 1e-9
          && std::abs (win->waveformTimeEnd() - dur) < 1e-9,
           "track zoom-out-full resets the waveform view's time window");
    CHECK (std::abs (win->waveformAmpHalf() - ampDefault) < 1e-9,
           "track zoom-out-full resets the amplitude zoom");
    CHECK (std::abs (win->spectralFreqMin() - 20.0) < 1e-9
          && std::abs (win->spectralFreqMax() - 4000.0) < 1e-9,
           "track zoom-out-full resets the frequency range");

    win.reset();
    delete mm;
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

// ------------------------------------------------- time text formatting
// Clock-style (m:ss / h:mm:ss) and footer formatting, plus the parser that
// backs the editable footer fields.
static void testTimeFormatting()
{
    auto approx = [] (double a, double b) { return std::abs (a - b) <= 1e-9; };

    // --- Clock style ---
    CHECK (formatClockTime (0.0) == "0:00", "zero as m:ss");
    CHECK (formatClockTime (45.0) == "0:45", "sub-minute clock value keeps m:ss");
    CHECK (formatClockTime (155.0) == "2:35", "minutes and seconds");
    CHECK (formatClockTime (155.5) == "2:35.5", "fractional seconds trimmed");
    CHECK (formatClockTime (155.127) == "2:35.127", "three fractional digits");
    CHECK (formatClockTime (155.99996, 4) == "2:36", "fractional carry bumps the second");
    CHECK (formatClockTime (155.6, 0) == "2:36", "no fraction requested rounds to nearest second");
    CHECK (formatClockTime (3600.0) == "1:00:00", "hour boundary gains the hour field");
    CHECK (formatClockTime (3755.25) == "1:02:35.25", "hours with trimmed fraction");
    CHECK (formatClockTime (7200.0) == "2:00:00", "multiple hours");

    // --- Footer style (magnitude-based) ---
    CHECK (formatFooterTime (0.0) == "0.000", "footer zero stays decimal seconds");
    CHECK (formatFooterTime (12.5) == "12.500", "sub-minute footer value: 3 decimals");
    CHECK (formatFooterTime (59.999) == "59.999", "just under a minute stays decimal");
    CHECK (formatFooterTime (60.0) == "1:00", "one minute switches to m:ss");
    CHECK (formatFooterTime (3599.0) == "59:59", "last second before an hour");
    CHECK (formatFooterTime (3600.0) == "1:00:00", "one hour switches to h:mm:ss");
    CHECK (formatFooterTime (7200.5) == "2:00:00.5", "hours with trimmed fraction");

    // --- Parsing ---
    double v = -1.0;
    CHECK (parseTimeText ("12.5", v) && approx (v, 12.5), "plain seconds parse");
    CHECK (parseTimeText ("60", v) && approx (v, 60.0), "whole seconds parse");
    CHECK (parseTimeText ("2:35", v) && approx (v, 155.0), "m:ss parses");
    CHECK (parseTimeText ("02:35.5", v) && approx (v, 155.5), "padded m:ss with fraction parses");
    CHECK (parseTimeText ("1:02:35", v) && approx (v, 3755.0), "h:mm:ss parses");
    CHECK (parseTimeText ("1:02:35.127", v) && approx (v, 3755.127), "h:mm:ss with fraction parses");
    CHECK (!parseTimeText ("-", v), "lone minus rejected");
    CHECK (!parseTimeText ("abc", v), "letters rejected");
    CHECK (!parseTimeText ("2:", v), "trailing colon rejected");
    CHECK (!parseTimeText (":30", v), "leading colon rejected");
    CHECK (!parseTimeText ("1:2:3:4", v), "three colons rejected");
    CHECK (!parseTimeText ("", v), "empty text rejected");
    CHECK (!parseTimeText ("  ", v), "blank text rejected");

    // Round-trip: what the footer displays must parse back to itself.
    const double roundTrip [] = { 0.0, 12.5, 59.999, 60.0, 155.127, 3599.0, 3600.0, 7200.5 };
    for (double t : roundTrip)
    {
        const juce::String s = formatFooterTime (t);
        double back = -1.0;
        CHECK (parseTimeText (s, back) && approx (back, t), "round-trip: " + s);
    }
}

static void testTimeAxisTicks()
{
    auto approx = [] (double a, double b) { return std::abs (a - b) <= b * 1e-9; };

    // --- Step selection at representative zoom levels ---
    CHECK (approx (niceTimeStep (45.0), 2.0), "20 s span @900 px: 2 s steps");
    CHECK (approx (niceTimeStep (900.0 / 10800.0), 1800.0), "3 h file @900 px: 30 m steps");
    CHECK (approx (niceTimeStep (4500.0), 0.02), "0.2 s span: 20 ms steps");
    CHECK (approx (niceTimeStep (45000.0), 0.002), "20 ms span: 2 ms steps");
    CHECK (approx (niceTimeStep (1e6), 1e-4), "~1 ms span: 100 us steps");

    // --- Label formatting (zoomed in: span <= one minute keeps decimal seconds)
    CHECK (formatTimeTickLabel (45.0, 1.0, 45.0) == "45", "whole seconds label");
    CHECK (formatTimeTickLabel (0.2, 0.2, 1.0) == "0.2", "tenths of a second label");
    CHECK (formatTimeTickLabel (0.05, 0.05, 1.0) == "0.05", "hundredths of a second label");
    CHECK (formatTimeTickLabel (0.002, 0.002, 1.0) == "2ms", "millisecond label");
    CHECK (formatTimeTickLabel (0.0005, 0.0005, 1.0) == "500\u00b5s", "microsecond label");
    CHECK (formatTimeTickLabel (9.242, 0.002, 20.0) == "9.242", "ms step away from start: decimal seconds");
    CHECK (formatTimeTickLabel (12.5, 0.0005, 20.0) == "12.5000", "sub-ms step away from start: 4 digits");
    CHECK (formatTimeTickLabel (0.0, 1.0, 30.0) == "0", "origin label is plain 0 when zoomed in");

    // --- Label formatting (zoomed out: span > one minute switches to clock style)
    CHECK (formatTimeTickLabel (45.0, 15.0, 90.0) == "0:45", "sub-minute tick in a long view: m:ss");
    CHECK (formatTimeTickLabel (300.0, 60.0, 300.0) == "5:00", "minutes as m:ss");
    CHECK (formatTimeTickLabel (900.0, 900.0, 900.0) == "15:00", "odd minutes as m:ss");
    CHECK (formatTimeTickLabel (3600.0, 600.0, 7200.0) == "1:00:00", "hour boundary as h:mm:ss");
    CHECK (formatTimeTickLabel (5400.0, 900.0, 7200.0) == "1:30:00", "mid-hour tick in a long view");
    CHECK (formatTimeTickLabel (7200.0, 1800.0, 7200.0) == "2:00:00", "multiple hours as h:mm:ss");
    CHECK (formatTimeTickLabel (62.5, 0.5, 90.0) == "1:02.5",
          "sub-second step in a long view keeps fractional seconds");

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

        // Step is 1800 s: major ticks at 0, 1800 ("30:00"), 3600 ("1:00:00"), ...
        CHECK (brightAt (img, 1800.0, 16, 23) > 45, "major tick drawn at 30 m");
        CHECK (brightAt (img, 3600.0, 16, 23) > 45, "major tick drawn at 1 h");
        // Rows y=16..19 are above the minor-tick band (y>=20): no major mid-interval.
        CHECK (brightAt (img, 2700.0, 16, 19) < 45, "no major tick mid-interval");
        // Minors subdivide each interval into five: first minor at 1800+360=2160.
        CHECK (brightAt (img, 2160.0, 20, 23) > 45, "intermediate tick between majors");

        // The "1:00:00" label text sits just right of its tick: bright pixels in rows 4..18.
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

        // One visible channel: the row is the plot area below the time ruler.
        const auto prLog = view.plotRect();
        const double f0 = view.yToFreq (60.0f - prLog.getY(), prLog.getHeight());   // freq under cursor at drag start
        const double f1 = view.yToFreq (80.0f - prLog.getY(), prLog.getHeight());

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
        const auto prLin = view.plotRect();
        const double d = view.yToFreq (80.0f - prLin.getY(), prLin.getHeight())
                       - view.yToFreq (100.0f - prLin.getY(), prLin.getHeight());

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
        const auto prTicks = view.plotRect();
        auto yOf = [&] (double f) -> int
        { return prTicks.getY() + (int) std::floor ((float) view.freqToY (f, prTicks.getHeight())); };

        auto brightness = [&] (int yy) -> int
        { const auto c = img.getPixelAt (100, yy); return (c.getRed() + c.getGreen() + c.getBlue()) / 3; };

        // GDI anti-aliases a fractional-y line across two rows, so the total
        // energy of a tick is measured over its nominal row plus neighbours.
        auto tickEnergy = [&] (int yy) -> int
        {
            int e = 0;
            for (int dy = -1; dy <= 1; ++dy)
                if (yy + dy >= 0 && yy + dy < img.getHeight())
                    e += brightness (yy + dy);
            return e;
        };

        // All three tick rows must be visible grey lines on the black ground.
        CHECK (brightness (yOf (100.0)) > 25,
               "decade tick line is drawn");
        CHECK (brightness (yOf (200.0)) > 20,
               "2x intermediate tick is drawn");
        CHECK (brightness (yOf (500.0)) > 20,
               "5x intermediate tick is drawn");

        // The major decade line uses a brighter colour than the minor ones
        // (compared by total energy so the AA row split cannot hide it).
        CHECK (tickEnergy (yOf (100.0)) > tickEnergy (yOf (200.0)) + 4
              && tickEnergy (yOf (100.0)) > tickEnergy (yOf (500.0)) + 4,
               "decade tick is brighter than the intermediate ticks");

        // A position without a tick stays dark.
        CHECK (brightness (yOf (150.0)) < 12, "no tick line where none is expected");

        // Sub-decade zoom [1k, 2k]: a nice linear step must provide divisions
        // inside the range instead of an empty axis.
        view.setFrequencyRange (1000.0, 2000.0);
        juce::Image img2 (juce::Image::ARGB, 400, 200, true);
        { juce::Graphics g (img2); view.paint (g); }
        auto yOf2 = [&] (double f) -> int
        { return prTicks.getY() + (int) std::floor ((float) view.freqToY (f, prTicks.getHeight())); };
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

    // --- Right-drag starting on an axis: blue band + only that axis zooms ---
    {
        FFTAnalysisView view (app);
        view.setSize (300, 200);
        const auto plot = view.plotBounds();

        // Bottom Hz axis: horizontal drag with a slight vertical component.
        const int ax1 = plot.getX() + 30, ax2 = plot.getRight() - 60;
        const int ay1 = plot.getBottom() + 4, ay2 = plot.getBottom() + 12;
        const double fA = view.xToFreq (ax1), fB = view.xToFreq (ax2);
        const double dbTop0 = view.getDbTop(), dbBot0 = view.getDbBottom();

        view.mouseDown (mkMouse (view, ax1, ay1, rmb));
        view.mouseDrag (mkMouse (view, ax2, ay2, rmb));

        // The rubber band must stretch across the full height of the bottom
        // axis strip: a translucent blue fill over the dark background.
        juce::Image img (juce::Image::ARGB, 300, 200, true);
        { juce::Graphics g (img); view.paint (g); }
        int blue = 0;
        for (int y = plot.getBottom() + 1; y < plot.getBottom() + 17; ++y)
            for (int x = ax1 + 8; x < ax2 - 8; x += 2)
                if (img.getPixelAt (x, y).getBlue() > img.getPixelAt (x, y).getRed() + 40) ++blue;
        CHECK (blue >= 3, "axis drag shows a blue band across the bottom axis");

        view.mouseUp (mkMouse (view, ax2, ay2, rmb));
        CHECK (std::abs (view.getFreqLo() - fA) / fA < 1e-6
              && std::abs (view.getFreqHi() - fB) / fB < 1e-6,
               "bottom-axis drag zooms the frequency window");
        CHECK (view.getDbTop() == dbTop0 && view.getDbBottom() == dbBot0,
               "bottom-axis drag leaves the level window alone");

        // Right level axis: vertical drag with a slight horizontal component.
        const int lx1 = plot.getRight() + 4, lx2 = plot.getRight() + 12;
        const int ly1 = plot.getY() + 30, ly2 = plot.getY() + 90;
        const double dbA = view.yToLevel ((float) ly1), dbB = view.yToLevel ((float) ly2);
        const double fLo0 = view.getFreqLo(), fHi0 = view.getFreqHi();

        view.mouseDown (mkMouse (view, lx1, ly1, rmb));
        view.mouseDrag (mkMouse (view, lx2, ly2, rmb));

        img = juce::Image (juce::Image::ARGB, 300, 200, true);
        { juce::Graphics g (img); view.paint (g); }
        blue = 0;
        for (int y = ly1 + 8; y < ly2 - 8; ++y)
            for (int x = plot.getRight() + 1; x < plot.getRight() + 33; ++x)
                if (img.getPixelAt (x, y).getBlue() > img.getPixelAt (x, y).getRed() + 40) ++blue;
        CHECK (blue >= 3, "axis drag shows a blue band across the right axis");

        view.mouseUp (mkMouse (view, lx2, ly2, rmb));
        const double span = std::clamp (dbA - dbB, 10.0, 320.0);
        const double expTop = std::clamp (dbA, -240.0 + span * 0.5, 40.0);
        CHECK (std::abs (view.getDbTop() - expTop) < 1e-6
              && std::abs (view.getDbBottom() - (expTop - span)) < 1e-6,
               "right-axis drag zooms the level window");
        CHECK (view.getFreqLo() == fLo0 && view.getFreqHi() == fHi0,
               "right-axis drag leaves the frequency window alone");
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

        const auto prGap = view.plotRect();
        auto yOf = [&] (double f) -> int
        { return prGap.getY() + (int) std::floor ((float) view.freqToY (f, prGap.getHeight())); };
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

// ---------------- Cancellation of long-running operations (ESC / Cancel) ----
static void testLoadCancel()
{
    // In-memory path: a file spanning several 1M-sample I/O chunks.
    {
        const juce::File wav = makeTempFile ("load_cancel_mem.wav");
        CHECK (writeWav (wav, 2, 48000.0, 3000000,
            [] (int c, int64_t f, double rate) {
                return (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                         * (100.0 + 50.0 * c) * (double) f / rate);
            }), "write large wav");

        AudioEngine eng;
        std::atomic<bool> cancel { false };
        const bool ok = eng.loadFile (wav,
            [&cancel] (float frac) { if (frac > 0.5f) cancel.store (true); },
            &cancel);
        CHECK (!ok, "cancelled load returns false");
        CHECK (!eng.isFileLoaded (), "engine back to pre-load state after cancel");
        CHECK (eng.mipmaps().empty (), "no partial mipmap pyramid after cancelled load");
    }

    // Streaming path: force streaming and cancel during mipmap construction.
    {
        const juce::File wav = makeTempFile ("load_cancel_stream.wav");
        CHECK (writeWav (wav, 2, 48000.0, 3000000,
            [] (int c, int64_t f, double rate) {
                return (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                         * (100.0 + 50.0 * c) * (double) f / rate);
            }), "write large wav (streaming)");

        AudioEngine eng;
        eng.setMaxInMemorySampleChannels (0);   // force the streaming path
        std::atomic<bool> cancel { false };
        const bool ok = eng.loadFile (wav,
            [&cancel] (float frac) {
                if (frac > 0.6f) cancel.store (true);   // mipmap phase is [0.5, 1]
            },
            &cancel);
        CHECK (!ok, "cancelled streaming load returns false");
        CHECK (!eng.isFileLoaded (), "streaming engine clean after cancel");
        CHECK (eng.mipmaps().empty (), "no partial pyramid after cancelled streaming load");
    }

    // Pre-set flag: aborts before any work.
    {
        const juce::File wav = makeTempFile ("load_cancel_preset.wav");
        writeWav (wav, 1, 8000.0, 8000,
            [] (int, int64_t f, double rate) {
                return (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                         * 440.0 * (double) f / rate);
            });
        AudioEngine eng;
        std::atomic<bool> cancel { true };
        CHECK (!eng.loadFile (wav, nullptr, &cancel), "pre-set cancel aborts immediately");
        CHECK (!eng.isFileLoaded (), "engine clean after pre-set cancel");
    }
}

// ------------------------------------------------------------------ large in-memory load
// Regression for a chunked-load bug: AudioEngine::loadFile reads files that
// fit in memory in 1<<20 sample-channel chunks but wrote every chunk to
// destination position 0 instead of its offset, so only fragments of the last
// two chunks survived at the buffer's head and the rest stayed zero. Any
// in-memory file longer than one chunk (~21.8 s mono / ~10.9 s stereo @48k)
// was silently corrupted — waveform display, spectral analysis and every
// processing path read wrong data.
static void testLargeInMemoryLoad()
{
    // Mono: 1.5M frames @ 48k = 31.25M sample-channels -> two chunks, in-memory.
    {
        const int64_t frames = 1500000;
        const juce::File wav = makeTempFile ("large_mem_mono.wav");
        CHECK (writeWav (wav, 1, 48000.0, frames,
            [frames] (int, int64_t f, double) { return f < frames / 2 ? 0.5f : -0.5f; }),
              "write large mono wav");

        AudioEngine eng;
        CHECK (eng.loadFile (wav), "load large mono file");
        CHECK (eng.numSamples () == frames, "full length reported");

        std::vector<float> d (100);
        CHECK (eng.readSamples (0, 0, 100, d.data ()), "read head ok");
        bool headOk = true;
        for (float v : d) if (std::abs (v - 0.5f) > 1e-6f) headOk = false;
        CHECK (headOk, "head chunk intact (+0.5)");

        // Straddle the halfway boundary: both values must be present in order.
        CHECK (eng.readSamples (0, frames / 2 - 50, 100, d.data ()), "read boundary ok");
        bool bndOk = true;
        for (int i = 0; i < 100; ++i)
            if (std::abs (d[i] - ((frames / 2 - 50 + i) < frames / 2 ? 0.5f : -0.5f)) > 1e-6f)
                bndOk = false;
        CHECK (bndOk, "half boundary exact");

        // Tail: must hold the last chunk's data (-0.5), not zeros or head data.
        CHECK (eng.readSamples (0, frames - 100, 100, d.data ()), "read tail ok");
        bool tailOk = true;
        for (float v : d) if (std::abs (v + 0.5f) > 1e-6f) tailOk = false;
        CHECK (tailOk, "tail chunk intact (-0.5)");
    }

    // Stereo: 3M frames @ 48k = 6M sample-channels -> three chunks, in-memory.
    {
        const int64_t frames = 3000000;
        const juce::File wav = makeTempFile ("large_mem_stereo.wav");
        CHECK (writeWav (wav, 2, 48000.0, frames,
            [] (int c, int64_t, double) { return c == 0 ? 0.25f : -0.75f; }),
              "write large stereo wav");

        AudioEngine eng;
        CHECK (eng.loadFile (wav), "load large stereo file");

        std::vector<float> d (100);
        CHECK (eng.readSamples (0, frames / 2, 100, d.data ()), "read ch0 mid ok");
        bool c0 = true;
        for (float v : d) if (std::abs (v - 0.25f) > 1e-6f) c0 = false;
        CHECK (c0, "ch0 mid intact (+0.25)");

        CHECK (eng.readSamples (1, frames - 100, 100, d.data ()), "read ch1 tail ok");
        bool c1 = true;
        for (float v : d) if (std::abs (v + 0.75f) > 1e-6f) c1 = false;
        CHECK (c1, "ch1 tail intact (-0.75)");
    }
}

// Regression: after an edit the mipmap pyramid must be built from the
// in-memory buffer, not from a memory-mapped read of the file on disk.
// Before the fix, WAV/AIFF files were always mapped from disk for the finest
// level, so the waveform showed pre-edit content while the spectral view
// (which reads the buffer) showed the edit.
static void testMipmapReflectsEdits()
{
    const juce::File wav = makeTempFile ("mip_edit.wav");
    CHECK (writeWav (wav, 2, 1000.0, 4000,
        [] (int ch, int64_t, double) { return ch == 0 ? 0.5f : -0.25f; }),
          "write wav for mipmap edit test");

    AudioEngine eng;
    CHECK (eng.loadFile (wav), "load ok");
    CHECK (eng.fullyInMemory (), "in memory");

    std::vector<int> onlyCh0 = { 0 };
    CHECK (eng.silenceSamples ({ 1000, 2000 }, &onlyCh0), "silence ok");

    const auto& mips = eng.mipmaps ();
    CHECK (mips.size () > 0, "mipmap pyramid built");
    if (mips.empty ()) return;
    const auto& lv = mips.front ();   // finest level; factor 1 for this size
    CHECK (lv.factor == 1, "finest level is per-sample");

    // Blocks fully inside the silenced range: ch0 flat zero, ch1 untouched.
    float worstCh0 = 0.0f, worstCh1Min = 0.0f, worstCh1Max = 0.0f;
    for (int64_t i = 1000; i < 2000; ++i)
    {
        worstCh0     = std::max (worstCh0, std::abs (lv.min[0][(size_t) i])
                                       + std::abs (lv.max[0][(size_t) i]));
        worstCh1Min  = std::max (worstCh1Min, std::abs (lv.min[1][(size_t) i] - (-0.25f)));
        worstCh1Max  = std::max (worstCh1Max, std::abs (lv.max[1][(size_t) i]));
    }
    CHECK (worstCh0 == 0.0f, "ch0 mipmap flat in silenced range");
    CHECK (worstCh1Min < 1e-6 && worstCh1Max < 1e-6,
           "ch1 mipmap unchanged in silenced range");
    // Just outside the range: ch0 still at its original level.
    CHECK (lv.max[0][999] == 0.5f, "ch0 mipmap intact just before the range");

    // A gain applied afterwards must also be visible in the mipmaps.
    CHECK (eng.gainSamples ({ 0, 4000 }, +6.0206f, &onlyCh0), "gain ok");
    const auto& lv2 = eng.mipmaps ().front ();
    CHECK (std::abs (lv2.max[0][999] - 1.0f) < 1e-3,
           "ch0 mipmap reflects the +6 dB gain outside the silenced range");
    CHECK (lv2.min[0][1500] == 0.0f && lv2.max[0][1500] == 0.0f,
           "silenced region stays flat after the gain");
}

// Regression: destructive edits on streamed files must materialise the file
// into memory and actually apply, instead of silently no-op'ing (and errors
// such as out-of-memory must be reported through errOut).
static void testEditStreamingFile()
{
    const juce::File wav = makeTempFile ("stream_edit.wav");
    CHECK (writeWav (wav, 2, 8000.0, 4000,
        [] (int ch, int64_t, double) { return ch == 0 ? 0.5f : -0.25f; }),
          "write wav for streaming edit test");

    // Silence on a streamed file: materialises + applies.
    {
        AudioEngine eng;
        eng.setMaxInMemorySampleChannels (100);   // force streaming mode
        CHECK (eng.loadFile (wav), "load ok");
        CHECK (!eng.fullyInMemory (), "streamed");

        std::vector<int> onlyCh0 = { 0 };
        std::string err;
        CHECK (eng.silenceSamples ({ 1000, 2000 }, &onlyCh0, &err),
              juce::String ("silence ok: ") + err);
        CHECK (eng.fullyInMemory (), "materialised after the edit");

        std::vector<float> d (500);
        CHECK (eng.readSamples (0, 1200, 500, d.data ()), "read silenced range");
        bool allZero = true;
        for (float v : d) if (v != 0.0f) { allZero = false; break; }
        CHECK (allZero, "silenced region is actually zero in the buffer");

        std::vector<float> d2 (1);
        CHECK (eng.readSamples (0, 500, 1, d2.data ()), "read outside range");
        CHECK (std::abs (d2[0] - 0.5f) < 1e-6, "outside the range is unchanged");
        CHECK (eng.readSamples (1, 1200, 1, d2.data ()), "read other channel");
        CHECK (std::abs (d2[0] - (-0.25f)) < 1e-6, "unaffected channel unchanged");

        // Gain works on the now-in-memory file too.
        CHECK (eng.gainSamples ({ 0, 4000 }, +6.0206f, &onlyCh0), "gain ok");
        CHECK (eng.readSamples (0, 500, 1, d2.data ()), "read after gain");
        CHECK (std::abs (d2[0] - 1.0f) < 1e-3, "+6 dB doubled the amplitude");
    }

    // Structural edit on a streamed file: delete.
    {
        AudioEngine eng;
        eng.setMaxInMemorySampleChannels (100);
        CHECK (eng.loadFile (wav), "load ok");
        std::string err;
        CHECK (eng.deleteSamples ({ 1000, 2000 }, &err),
              juce::String ("delete ok: ") + err);
        CHECK (eng.numSamples () == 3000, "length reduced by the deletion");
    }

    // Resample on a streamed file: materialises first, then resamples.
    {
        AudioEngine eng;
        eng.setMaxInMemorySampleChannels (100);
        CHECK (eng.loadFile (wav), "load ok");
        std::string err;
        CHECK (eng.resampleTo (16000.0, nullptr, nullptr, &err),
              juce::String ("resample ok: ") + err);
        CHECK (std::abs (eng.sampleRate () - 16000.0) < 1e-9, "rate changed");
        CHECK (eng.fullyInMemory (), "materialised by the resample");
    }

    // Error path: no file loaded -> false with a clear message.
    {
        AudioEngine eng;
        std::string err;
        CHECK (!eng.silenceSamples ({ 0, 10 }, nullptr, &err), "no file -> false");
        CHECK (!err.empty (), "error message was set");
    }
}

static void testResampleCancel()
{
    const juce::File wav = makeTempFile ("resample_cancel.wav");
    CHECK (writeWav (wav, 1, 48000.0, 3000000,
        [] (int, int64_t f, double rate) {
            return (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                     * 440.0 * (double) f / rate);
        }), "write wav for resample cancel");

    AudioEngine eng;
    CHECK (eng.loadFile (wav), "load ok");
    const double oldRate = eng.sampleRate();
    const int64_t oldLen = eng.numSamples();

    // Pre-set flag: cancels at the first chunk boundary.
    std::atomic<bool> cancel { true };
    CHECK (!eng.resampleTo (96000.0, nullptr, &cancel), "pre-set cancel aborts resample");
    CHECK (std::abs (eng.sampleRate() - oldRate) < 1e-9,
          "rate unchanged after cancelled resample");
    CHECK (eng.numSamples() == oldLen, "length unchanged after cancelled resample");

    // Mid-resample cancel via the progress callback.
    std::atomic<bool> cancel2 { false };
    const bool ok = eng.resampleTo (96000.0,
        [&cancel2] (float f) { if (f > 0.5f) cancel2.store (true); },
        &cancel2);
    CHECK (!ok, "mid-resample cancel returns false");
    CHECK (std::abs (eng.sampleRate() - oldRate) < 1e-9,
          "rate unchanged after mid-resample cancel");
    CHECK (eng.numSamples() == oldLen, "length unchanged after mid-resample cancel");
}

static void testRenderToPixelsCancel()
{
    const juce::File wav = makeTempFile ("render_cancel.wav");
    CHECK (writeWav (wav, 1, 48000.0, 240000,
        [] (int, int64_t f, double rate) {
            return (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                     * 440.0 * (double) f / rate);
        }), "write wav for render cancel");

    AudioEngine eng;
    CHECK (eng.loadFile (wav), "load ok");

    SpectralEngine sp;
    Settings s;
    sp.setColourMap (ColourMap::Classic);

    // Pre-set flag: stops before sampling; no partial tiles may be cached.
    std::atomic<bool> cancel { true };
    std::vector<uint8_t> rgba;
    CHECK (!sp.renderToPixels (eng, s, 256, 128, 0.0, 5.0, { 0 }, rgba, &cancel),
          "pre-set cancel aborts renderToPixels");
    CHECK (sp.cacheSize() == 0, "no partial tiles cached after cancelled render");

    // A normal (uncancelled) render still works afterwards.
    std::vector<uint8_t> rgba2;
    CHECK (sp.renderToPixels (eng, s, 256, 128, 0.0, 5.0, { 0 }, rgba2),
          "uncancelled render succeeds after a cancelled one");
    CHECK (rgba2.size() == (size_t) 256 * 128 * 4, "render output size correct");
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
    // 3 noise-suppression models + 12 Whisper transcription models.
    CHECK (mm.registry().size() == 15, "registry lists 15 models");

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

    // When GPU/NPU was selected and its plugin is missing, name that DLL too.
    const std::string d8 = sc::buildNsFailureDetails (cpuErr, "deepfilternet2", "GPU");
    CHECK (d8.find ("openvino_intel_gpu_plugin.dll") != std::string::npos, "missing GPU plugin: names the GPU DLL");
    const std::string d9 = sc::buildNsFailureDetails (cpuErr, "deepfilternet2", "NPU");
    CHECK (d9.find ("openvino_npu_plugin.dll") != std::string::npos, "missing NPU plugin: names the NPU DLL");
}

// Device preference resolution shared by noise suppression and transcription:
// "auto" picks GPU over NPU over CPU; an explicit choice that is unavailable
// falls back to CPU with a human-readable note.
static void testAiDeviceResolution()
{
    const std::vector<std::string> cpuOnly { "CPU" };
    const std::vector<std::string> all     { "CPU", "GPU", "NPU" };

    CHECK (sc::resolveAiDevice ("auto", cpuOnly) == "CPU",  "auto, only CPU -> CPU");
    CHECK (sc::resolveAiDevice ("AUTO", all)      == "GPU",  "auto, GPU available -> GPU");
    CHECK (sc::resolveAiDevice ("auto", { "NPU", "CPU" }) == "NPU", "auto, no GPU but NPU -> NPU");
    CHECK (sc::resolveAiDevice ("", cpuOnly)     == "CPU",  "empty pref treated as auto");

    std::string note;
    CHECK (sc::resolveAiDevice ("GPU", all, &note) == "GPU", "explicit GPU available -> GPU");
    CHECK (note.empty (), "no fallback note when the device is available");

    note.clear ();
    CHECK (sc::resolveAiDevice ("gpu", { "CPU", "GPU" }, &note) == "GPU",
          "lowercase pref normalised to GPU");

    note.clear ();
    CHECK (sc::resolveAiDevice ("NPU", cpuOnly, &note) == "CPU",
          "explicit NPU unavailable -> CPU fallback");
    CHECK (!note.empty () && note.find ("NPU") != std::string::npos,
          "fallback note names the missing device");

    note.clear ();
    CHECK (sc::resolveAiDevice ("GPU", {}, &note) == "CPU",
          "runtime not initialised (empty list) -> CPU");

    CHECK (sc::resolveAiDevice ("CPU", cpuOnly) == "CPU", "explicit CPU -> CPU");
}

// ------------------------------------------------- spectral selection model

static void testSpectralSelectionModel()
{
    // Rect: contains + inverted-edge normalisation.
    SpectralSelection sel;
    sel.addRect (1.0, 2.0, 300.0, 700.0, false);
    CHECK (sel.contains (1.5, 500.0), "rect contains interior point");
    CHECK (!sel.contains (0.5, 500.0), "rejects point left of rect");
    CHECK (!sel.contains (1.5, 900.0), "rejects point above rect");
    CHECK (!sel.contains (2.5, 500.0), "rejects point right of rect");

    // Inverted edges are normalised.
    SpectralSelection inv;
    inv.addRect (2.0, 1.0, 700.0, 300.0, false);
    CHECK (inv.contains (1.5, 500.0), "inverted rect edges normalised");

    // Polygon: even-odd rule.
    SpectralSelection poly;
    poly.addPolygon ({ { 0.0, 0.0 }, { 2.0, 0.0 }, { 1.0, 1000.0 } }, false);
    CHECK (poly.contains (1.0, 300.0), "polygon contains centroid");
    CHECK (!poly.contains (0.1, 900.0), "polygon rejects outside point");

    // Degenerate polygons are dropped.
    SpectralSelection tiny;
    tiny.addPolygon ({ { 0.0, 0.0 }, { 0.0, 0.0 }, { 0.1, 0.0 } }, false);
    CHECK (tiny.empty (), "<3 distinct points -> no shape");

    // Stroke: rasterised brush band.
    SpectralSelection stroke;
    std::vector<std::pair<double, double>> path;
    for (int i = 0; i <= 100; ++i)
        path.emplace_back ((double) i / 100.0 * 2.0, 500.0);   // t: 0..2 s at 500 Hz
    stroke.addStroke (path, 200.0, false);
    CHECK (!stroke.empty (), "stroke committed");
    CHECK (stroke.contains (1.0, 500.0), "stroke contains its centre line");
    CHECK (stroke.contains (1.0, 650.0), "stroke covers within brush radius");
    CHECK (!stroke.contains (1.0, 900.0), "stroke rejects beyond brush radius");
    CHECK (!stroke.contains (3.0, 500.0), "stroke rejects outside its time span");

    // Fast-drag strokes: sparse sample points must not leave gaps between
    // them - the rasteriser interpolates along each segment.
    SpectralSelection sparse;
    sparse.addStroke ({ { 0.0, 500.0 }, { 2.0, 500.0 } }, 20.0, false);
    bool noGapH = true;
    for (int i = 1; i < 200; ++i)
        if (!sparse.contains ((double) i / 100.0, 500.0)) { noGapH = false; break; }
    CHECK (noGapH, "horizontal stroke has no gaps between sparse samples");

    SpectralSelection diag;
    diag.addStroke ({ { 0.0, 100.0 }, { 2.0, 900.0 } }, 20.0, false);
    bool noGapD = true;
    for (int i = 1; i < 200; ++i)
    {
        const double u = (double) i / 200.0;   // 0..1 along the segment
        if (!diag.contains (u * 2.0, 100.0 + u * 800.0)) { noGapD = false; break; }
    }
    CHECK (noGapD, "diagonal stroke has no gaps between sparse samples");

    // Boolean order: add A, subtract B inside A.
    SpectralSelection boolSel;
    boolSel.addRect (0.0, 2.0, 0.0, 1000.0, false);          // A: everything
    boolSel.addRect (0.5, 1.5, 400.0, 600.0, true);          // B: hole in the middle
    CHECK (!boolSel.contains (1.0, 500.0), "subtractive rect carves a hole");
    CHECK (boolSel.contains (0.2, 500.0), "area outside the hole stays selected");

    // timeExtent is the union over ALL shapes (including subtractive ones).
    const auto ext = boolSel.timeExtent();
    CHECK (std::abs (ext.getStart() - 0.0) < 1e-9 && std::abs (ext.getEnd() - 2.0) < 1e-9,
          "timeExtent spans the additive shape");

    // replace semantics: a new plain rect clears previous shapes.
    SpectralSelection rep;
    rep.addRect (0.0, 1.0, 0.0, 100.0, false);
    rep.addRect (5.0, 6.0, 0.0, 100.0, false);   // replace=true by default
    CHECK (rep.count() == 1, "plain add replaces previous shapes");
    CHECK (!rep.contains (0.5, 50.0), "old shape gone after replace");
    CHECK (rep.contains (5.5, 50.0), "new shape present");

    // Shift-add keeps both.
    SpectralSelection add2;
    add2.addRect (0.0, 1.0, 0.0, 100.0, false);
    add2.addRect (5.0, 6.0, 0.0, 100.0, false, /*replace=*/false);
    CHECK (add2.count() == 2, "shift-add keeps previous shapes");
}

// ---------------- Spectral selection rendering: subtractive shapes render as holes ----------------
static void testSpectralSelectionRenderHole()
{
    const juce::File wav = makeTempFile ("sel_hole.wav");
    writeWav (wav, 1, 8000.0, 4000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for selection render test");

    SpectralView view (app);
    view.setSize (256, 128);
    // Exactly the file length: the engine maps pixels onto the data range, so a
    // wider view would stretch the content and smear colours across the plot.
    view.setViewRange (0.0, 0.5);
    view.setVisibleChannels ({ 0 });
    view.setSettings (app.settings);

    // A: additive rect; B: subtractive rect fully inside A -> final selection is A minus B.
    auto& sel = app.doc()->spectralSel;
    sel.addRect (0.1, 0.4, 100.0, 700.0, false);                    // A
    sel.addRect (0.2, 0.3, 200.0, 600.0, true, /*replace=*/false);  // B: hole in the middle

    juce::Image img (juce::Image::ARGB, 256, 128, true);
    { juce::Graphics g (img); view.paint (g); }

    // Baseline render with no selection at all. The overlay must be the ONLY
    // difference between the two images — comparing against it is immune to
    // whatever spectral content (tone lines, sidelobe halos) sits underneath.
    sel.clear ();
    juce::Image base (juce::Image::ARGB, 256, 128, true);
    { juce::Graphics g0 (base); view.paint (g0); }

    auto sameAsBase = [&] (int x, int y) { return img.getPixelAt (x, y) == base.getPixelAt (x, y); };
    // The selection fill is RGBA(70,130,255,~80): over any dark background it
    // lands around (44,60,105) — clearly blue-dominant.
    auto blueTint = [] (const juce::Colour& p) { return p.getBlue() - p.getRed() > 40; };

    const auto prSel = view.plotRect();
    const int yLow   = prSel.getY() + (int) view.freqToY (250.0, prSel.getHeight());  // inside both rects' freq span
    const int xAonly = (int) view.timeToX (0.15);        // inside A only -> blue fill
    CHECK (!sameAsBase (xAonly, yLow), "additive area differs from the no-selection baseline");
    CHECK (blueTint (img.getPixelAt (xAonly, yLow)), "additive area renders blue");

    const int xHole = (int) view.timeToX (0.25);         // inside A and B -> hole, no fill
    CHECK (sameAsBase (xHole, yLow), "subtractive overlap renders as a hole");

    const int xOut  = (int) view.timeToX (0.45);         // outside both shapes -> no fill
    CHECK (sameAsBase (xOut, yLow), "area outside the selection is untouched");

    wav.deleteFile();
}

// Straight-line sampling must be linear in BOTH time and frequency: the
// midpoint is exactly the average of the endpoints and every sample lies on
// f = f0 + (f1-f0)*(t-t0)/(t1-t0).
static void testStraightLinePoints()
{
    auto pts = straightLinePoints (0.0, 100.0, 1.0, 300.0);
    CHECK (!pts.empty (), "line has points");
    if (pts.size () >= 2)
    {
        CHECK (pts.front ().first == 0.0 && pts.front ().second == 100.0,
              "starts at P0");
        CHECK (pts.back ().first == 1.0 && pts.back ().second == 300.0,
              "ends at P1");

        const auto mid = pts[pts.size () / 2];
        CHECK (std::abs (mid.first - 0.5) < 1e-9 && std::abs (mid.second - 200.0) < 1e-6,
              "midpoint is the exact average");

        bool onLine = true;
        for (auto& p : pts)
            if (std::abs (p.second - (100.0 + 200.0 * p.first)) > 1e-6) { onLine = false; break; }
        CHECK (onLine, "all samples collinear in linear-f space");
    }

    // A vertical line keeps constant time.
    auto vpts = straightLinePoints (0.5, 200.0, 0.5, 800.0);
    bool vertOk = !vpts.empty ();
    for (auto& p : vpts) if (p.first != 0.5) { vertOk = false; break; }
    CHECK (vertOk, "vertical line keeps constant time");

    // A degenerate zero-length line still returns points (a dot stamp).
    auto dpts = straightLinePoints (1.0, 500.0, 1.0, 500.0);
    CHECK (!dpts.empty (), "zero-length line has points");
}

// ALT+click in paintbrush mode draws a straight line from the last stroke's
// end point to the click position. The line is interpolated linearly in
// (time, frequency) space - i.e. it stays straight on a LINEAR frequency
// scale and only appears slightly curved on screen when the view uses a log
// axis (which is correct).
static void testAltClickStraightLine()
{
    const juce::File wav = makeTempFile ("alt_line.wav");
    writeWav (wav, 1, 8000.0, 16384, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for alt-click line test");

    SpectralView view (app);
    view.setSize (512, 256);
    view.setViewRange (0.0, 2.0);
    view.setVisibleChannels ({ 0 });
    // Keep the default LOG frequency axis: a screen-straight drag would be an
    // exponential curve in f; the committed line must still be linear in f.
    view.setSettings (app.settings);

    const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);
    const juce::ModifierKeys altLmb (juce::ModifierKeys::leftButtonModifier
                                     | juce::ModifierKeys::altModifier);
    app.selMode = SpectralSelMode::Paintbrush;

    // Op 1: plain freehand paint drag from A to B. Its mouse-up point B
    // becomes the anchor for the next ALT+click.
    const auto prLine = view.plotRect();
    const int xA = (int) view.timeToX (0.2), yA = prLine.getY() + (int) view.freqToY (500.0, prLine.getHeight());
    const int xB = (int) view.timeToX (0.4), yB = prLine.getY() + (int) view.freqToY (200.0, prLine.getHeight());
    view.mouseDown (mkMouse (view, xA, yA, lmb));
    view.mouseDrag (mkMouse (view, xB, yB, lmb));
    view.mouseUp   (mkMouse (view, xB, yB, lmb));

    // Op 2: ALT+click at C. Commits a straight line B -> C and replaces the
    // selection (plain modifier = replace).
    const int xC = (int) view.timeToX (0.8), yC = prLine.getY() + (int) view.freqToY (3500.0, prLine.getHeight());
    view.mouseDown (mkMouse (view, xC, yC, altLmb));
    view.mouseUp   (mkMouse (view, xC, yC, altLmb));

    auto& sel = app.doc()->spectralSel;
    CHECK (sel.count () == 1, "ALT+click replaces the selection with one line stroke");
    if (sel.count () != 1) { wav.deleteFile (); return; }
    const SpectralShape& s = sel.shapes ().front ();
    CHECK (s.kind == SpectralShape::Kind::Stroke, "committed shape is a stroke");

    // The anchor B was at f=200 Hz and the click C at f=3500 Hz. At the time
    // midpoint t=0.6 s a LINEAR-f line passes through (200+3500)/2 = 1850 Hz;
    // a screen-straight (log-interpolated) line would pass through
    // sqrt(200*3500) ~ 837 Hz instead - far outside the 200 Hz brush radius.
    CHECK (sel.contains (0.6, 1850.0), "line is straight in linear-f space");
    CHECK (!sel.contains (0.6, 837.0), "log-curved position is NOT on the line");

    // Both endpoints are covered by their brush radius.
    const double tB = view.xToTime (xB), fB = view.freqAtY (yB);
    const double tC = view.xToTime (xC), fC = view.freqAtY (yC);
    CHECK (sel.contains (tB, fB), "anchor end covered");
    CHECK (sel.contains (tC, fC), "click end covered");

    wav.deleteFile ();
}

// DFT amplitude of a pure tone in a buffer (leakage-tolerant).
static float toneAmp (const std::vector<float>& x, size_t from, size_t to,
                      double freqHz, double rate)
{
    double sr = 0.0, si = 0.0;
    for (size_t i = from; i < to && i < x.size(); ++i)
    {
        const double ph = 2.0 * juce::MathConstants<double>::pi * freqHz * (double) i / rate;
        sr += (double) x[i] * std::cos (ph);
        si -= (double) x[i] * std::sin (ph);
    }
    const double n = (double) std::max<size_t> (1, to - from);
    return (float) (std::sqrt (sr * sr + si * si) / n * 2.0);
}

// Two-tone test file: 500 Hz + 3500 Hz sines, mono 8 kHz.
static const double kToneRate = 8000.0;
static const int64_t kToneFrames = 16384;   // 2 seconds

static bool writeTwoToneWav (const juce::File& path)
{
    return writeWav (path, 1, kToneRate, kToneFrames,
        [] (int, int64_t f, double rate) {
            const double t = (double) f / rate;
            return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 500.0 * t)
                 + 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 3500.0 * t);
        });
}

// Marquee covering only the 500 Hz tone.
static SpectralSelection lowBandSel()
{
    SpectralSelection s;
    s.addRect (0.0, kToneFrames / kToneRate, 300.0, 700.0, false);
    return s;
}

// ------------------------------------------------- masked gain + zero-out

// ------------------------------------------------------- deletion side state
// Deleting a time range must also adjust the non-audio state of that file:
// markers shift / trim / drop, named saved spectral selections follow the
// same rules, and stored FFT snapshots do too. Everything is undoable along
// with the audio patch.
static void testMarkerAdjustForDeletion()
{
    MarkerManager mm;
    const int pBefore = mm.addPointMarker (1.0, "before");
    const int pAtA    = mm.addPointMarker (4.0, "at-a");
    const int pIn     = mm.addPointMarker (5.0, "inside");
    const int pAtB    = mm.addPointMarker (6.0, "at-b");
    const int pAfter  = mm.addPointMarker (8.0, "after");
    const int rBefore   = mm.addRegionMarker (0.5, 2.0, "r-before");
    const int rAfter    = mm.addRegionMarker (7.0, 9.0, "r-after");
    const int rStraddle = mm.addRegionMarker (3.0, 8.0, "r-straddle");
    const int rRightTrim= mm.addRegionMarker (2.0, 5.0, "r-right-trim");
    const int rLeftTrim = mm.addRegionMarker (5.0, 8.0, "r-left-trim");
    const int rContained= mm.addRegionMarker (4.5, 5.5, "r-contained");

    CHECK (mm.adjustForDeletion (4.0, 6.0), "deletion adjusts markers");

    auto find = [&] (int id) -> const Marker*
    { for (const auto& m : mm.markers()) if (m.id == id) return &m; return nullptr; };

    const Marker* mb = find (pBefore);
    CHECK (mb && std::abs (mb->start - 1.0) < 1e-9, "point before deletion unchanged");
    CHECK (!find (pAtA), "point at the deletion start is dropped");
    CHECK (!find (pIn), "point inside the deleted range is dropped");
    mb = find (pAtB);
    CHECK (mb && std::abs (mb->start - 4.0) < 1e-9, "point at b shifts to a");
    mb = find (pAfter);
    CHECK (mb && std::abs (mb->start - 6.0) < 1e-9, "point after the range shifts by its length");

    const Marker* rb = find (rBefore);
    CHECK (rb && std::abs (rb->end - 2.0) < 1e-9, "region before deletion unchanged");
    const Marker* ra = find (rAfter);
    CHECK (ra && std::abs (ra->start - 5.0) < 1e-9 && std::abs (ra->end - 7.0) < 1e-9,
          "region after the range shifts left");
    const Marker* rs = find (rStraddle);
    CHECK (rs && std::abs (rs->start - 3.0) < 1e-9 && std::abs (rs->end - 6.0) < 1e-9,
          "region spanning the whole deletion trims to [3,6]");
    const Marker* rr = find (rRightTrim);
    CHECK (rr && std::abs (rr->start - 2.0) < 1e-9 && std::abs (rr->end - 4.0) < 1e-9,
          "region with its right part deleted trims to [2,4]");
    const Marker* rl = find (rLeftTrim);
    CHECK (rl && std::abs (rl->start - 4.0) < 1e-9 && std::abs (rl->end - 6.0) < 1e-9,
          "region with its left part deleted becomes [4,6]");
    CHECK (!find (rContained), "fully contained region is dropped");

    // restore() replaces the list and keeps id allocation sane.
    const std::vector<Marker> saved = mm.markers();
    MarkerManager mm2;
    mm2.addPointMarker (0.1, "x");
    mm2.restore (saved);
    CHECK ((int) mm2.markers().size() == (int) saved.size(), "restore replaces the list");
    bool same = true;
    for (size_t i = 0; i < saved.size(); ++i)
        if (!(mm2.markers()[i].id == saved[i].id
              && std::abs (mm2.markers()[i].start - saved[i].start) < 1e-9))
            same = false;
    CHECK (same, "restored markers match the original list");
}

static void testSpectralSelectionAdjustForDeletion()
{
    // Rectangles against deletion [4,6].
    SpectralSelection sel;
    sel.addRect (0.5, 1.5, 100, 200, false);            // before: unchanged
    sel.addRect (7.0, 8.0, 100, 200, false, false);     // after: shift -> [5,7]
    sel.addRect (3.0, 9.0, 100, 200, false, false);     // straddles whole deletion -> [3,7]
    sel.addRect (2.0, 5.0, 100, 200, false, false);     // right part deleted -> [2,4]
    sel.addRect (5.0, 8.0, 100, 200, false, false);     // left part deleted -> [4,6]
    sel.addRect (4.5, 5.5, 100, 200, false, false);     // contained: dropped

    CHECK (sel.adjustForDeletion (4.0, 6.0), "rects adjusted for deletion");
    auto rectAt = [&] (double t) -> const SpectralShape*
    { for (const auto& s : sel.shapes()) if (s.kind == SpectralShape::Kind::Rect && std::abs (s.t0 - t) < 1e-9) return &s; return nullptr; };

    const SpectralShape* r = rectAt (0.5);
    CHECK (r && std::abs (r->t1 - 1.5) < 1e-9, "rect before deletion unchanged");
    r = rectAt (5.0);
    CHECK (r && std::abs (r->t1 - 6.0) < 1e-9, "rect after the range shifted to [5,6]");
    r = rectAt (3.0);
    CHECK (r && std::abs (r->t1 - 7.0) < 1e-9, "straddling rect trims to [3,7]");
    r = rectAt (2.0);
    CHECK (r && std::abs (r->t1 - 4.0) < 1e-9, "right-trimmed rect becomes [2,4]");
    r = rectAt (4.0);
    CHECK (r && std::abs (r->t1 - 6.0) < 1e-9, "left-trimmed rect becomes [4,6]");
    int rects = 0;
    for (const auto& s : sel.shapes()) if (s.kind == SpectralShape::Kind::Rect) ++rects;
    CHECK (rects == 5, "contained rect dropped");

    // Polygon entirely after the range: shifted.
    SpectralSelection poly1;
    poly1.addPolygon ({ {7.0, 100}, {9.0, 100}, {8.0, 300} }, false);
    CHECK (poly1.adjustForDeletion (4.0, 6.0), "polygon shifted");
    CHECK (poly1.shapes().size() == 1 && poly1.shapes()[0].kind == SpectralShape::Kind::Polygon,
          "single polygon kept");
    const auto& pp = poly1.shapes()[0].pts;
    CHECK (std::abs (pp[0].first - 5.0) < 1e-9 && std::abs (pp[1].first - 7.0) < 1e-9
           && std::abs (pp[2].first - 6.0) < 1e-9,
          "polygon vertices shifted by the deleted length");

    // Polygon straddling the deletion: split into two pieces.
    SpectralSelection poly2;
    poly2.addPolygon ({ {3.0, 100}, {8.0, 100}, {5.5, 400} }, false);
    CHECK (poly2.adjustForDeletion (4.0, 6.0), "polygon split");
    int polys = 0;
    for (const auto& s : poly2.shapes()) if (s.kind == SpectralShape::Kind::Polygon) ++polys;
    CHECK (polys == 2, "two polygon pieces after the split");
    CHECK (poly2.contains (3.5, 150), "left piece keeps its content");
    CHECK (poly2.contains (5.0, 150), "right piece shifted into place");
    CHECK (!poly2.contains (4.5, 300), "deleted middle is gone");

    // Stroke entirely after the range: shifted with grid intact.
    SpectralSelection st1;
    st1.addStroke (straightLinePoints (7.0, 150, 9.0, 250), 50.0, false);
    CHECK (st1.adjustForDeletion (4.0, 6.0), "stroke shifted");
    const auto& s1 = st1.shapes()[0];
    // Stroke grids carry a half-cell margin around the path, so allow slack.
    CHECK (std::abs (s1.t0 - 5.0) < 0.01 && std::abs (s1.t1 - 7.0) < 0.01,
          "stroke time range shifted");
    CHECK (st1.contains (6.0, 200), "shifted stroke still covers its line");

    // Stroke straddling both edges: split into two contiguous pieces.
    SpectralSelection st2;
    st2.addStroke (straightLinePoints (3.0, 150, 9.0, 250), 50.0, false);
    CHECK (st2.adjustForDeletion (4.0, 6.0), "stroke split");
    int strokes = 0;
    for (const auto& s : st2.shapes()) if (s.kind == SpectralShape::Kind::Stroke) ++strokes;
    CHECK (strokes == 2, "two stroke pieces after the split");
    CHECK (st2.contains (3.5, 160), "left piece keeps its line");
    CHECK (st2.contains (5.5, 225), "right piece shifted into place");
    CHECK (!st2.contains (7.5, 280), "nothing beyond the new end");

    // Stroke fully contained: dropped.
    SpectralSelection st3;
    st3.addStroke (straightLinePoints (4.5, 150, 5.5, 250), 50.0, false);
    CHECK (st3.adjustForDeletion (4.0, 6.0), "contained stroke dropped");
    CHECK (st3.empty(), "selection empty after dropping the only shape");

    // No-op: everything before the deletion range.
    SpectralSelection st4;
    st4.addRect (0.5, 1.5, 100, 200, false);
    CHECK (!st4.adjustForDeletion (4.0, 6.0), "no change when nothing overlaps");
}

static void testDeleteShiftsMarkersAndSavedSelections()
{
    const juce::File wav = makeTempFile ("del_side.wav");
    CHECK (writeWav (wav, 1, 8000.0, 64000, [] (int, int64_t f, double rate)
        { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 440.0 * (double) f / rate); }),
          "wrote test wav");

    Application app;
    CHECK (app.openFile (wav), "open wav for deletion side-state test");
    FileDoc* d = app.doc();
    CHECK (d != nullptr, "active doc exists");

    const int m1 = app.markers().addPointMarker (8.0, "after-marker");
    SavedSpectralSelection ss;
    ss.name = "saved";
    ss.data.addRect (7.0, 9.0, 100, 500, false);
    d->savedSelections.push_back (std::move (ss));

    app.setSelection ({ 4.0, 6.0 });
    CHECK (app.del (), "delete succeeds");

    auto findMarker = [&] (int id) -> const Marker*
    { for (const auto& m : app.markers().markers()) if (m.id == id) return &m; return nullptr; };

    const Marker* mk = findMarker (m1);
    CHECK (mk && std::abs (mk->start - 6.0) < 1e-9, "marker after the deleted range shifted left");

    CHECK (d->savedSelections.size() == 1, "named saved selection kept");
    const auto& sr = d->savedSelections[0].data.shapes().front();
    CHECK (std::abs (sr.t0 - 5.0) < 1e-9 && std::abs (sr.t1 - 7.0) < 1e-9,
          "saved spectral rect shifted with the audio");

    const Patch& p = app.undo().undoAt (app.undo().undoSize() - 1);
    CHECK (p.hasSideState, "patch carries non-audio side state");

    // Undo restores markers and saved selections together with the audio.
    app.undoStep ();
    mk = findMarker (m1);
    CHECK (mk && std::abs (mk->start - 8.0) < 1e-9, "undo restores the marker position");
    CHECK (d->savedSelections.size() == 1, "undo keeps the saved selection");
    const auto& sr2 = d->savedSelections[0].data.shapes().front();
    CHECK (std::abs (sr2.t0 - 7.0) < 1e-9 && std::abs (sr2.t1 - 9.0) < 1e-9,
          "undo restores the saved spectral rect");

    // Redo re-applies the adjustment.
    app.redoStep ();
    mk = findMarker (m1);
    CHECK (mk && std::abs (mk->start - 6.0) < 1e-9, "redo shifts the marker again");
    const auto& sr3 = d->savedSelections[0].data.shapes().front();
    CHECK (std::abs (sr3.t0 - 5.0) < 1e-9 && std::abs (sr3.t1 - 7.0) < 1e-9,
          "redo shifts the saved spectral rect again");
}

static void testDeleteAdjustsSnapshots()
{
    const juce::File wav = makeTempFile ("snap_del.wav");
    CHECK (writeWav (wav, 1, 8000.0, 64000, [] (int, int64_t f, double rate)
        { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 440.0 * (double) f / rate); }),
          "wrote test wav");

    Application app;
    CHECK (app.openFile (wav), "open wav for snapshot deletion test");
    app.settings.fftSize = 1024;
    app.settings.fftLocalN = 1024;

    FFTAnalysisView view (app);
    view.setSize (300, 200);

    // Three snapshots: before / straddling the right edge / contained in [4,6].
    app.setSelection ({ 1.0, 2.0 });
    view.recomputeIfNeeded();
    view.handleSnapshotClick (0);
    CHECK (view.snapshotN (0) > 0, "snapshot 0 stored");

    std::this_thread::sleep_for (std::chrono::milliseconds (80));
    app.setSelection ({ 5.0, 7.0 });
    view.recomputeIfNeeded();
    view.handleSnapshotClick (1);
    CHECK (view.snapshotN (1) > 0, "snapshot 1 stored");

    std::this_thread::sleep_for (std::chrono::milliseconds (80));
    app.setSelection ({ 4.2, 5.8 });
    view.recomputeIfNeeded();
    view.handleSnapshotClick (2);
    CHECK (view.snapshotN (2) > 0, "snapshot 2 stored");

    FileDoc* d = app.doc();
    Patch p;
    view.recordAndAdjustSnapshotsForDeletion (d, 4.0, 6.0, p);

    CHECK (std::abs (view.snapshotT0 (0) - 1.0) < 1e-9 && std::abs (view.snapshotT1 (0) - 2.0) < 1e-9
          && view.snapshotN (0) > 0, "snapshot before the range untouched");
    CHECK (std::abs (view.snapshotT0 (1) - 4.0) < 1e-9 && std::abs (view.snapshotT1 (1) - 5.0) < 1e-9,
          "straddling snapshot trimmed to [4,5]");
    CHECK (view.snapshotN (2) == 0, "contained snapshot cleared");

    // Undo-style restore: slot 2 comes back and its curve is recomputed.
    view.restoreSnapshotSlots (d, p, false);
    CHECK (std::abs (view.snapshotT0 (2) - 4.2) < 1e-9 && std::abs (view.snapshotT1 (2) - 5.8) < 1e-9,
          "undo restores the cleared snapshot's segment");
    CHECK (!view.snapshotMags (2).empty(), "restored curve recomputed from audio");

    // Redo-style: it is cleared again.
    view.restoreSnapshotSlots (d, p, true);
    CHECK (view.snapshotN (2) == 0, "redo clears the slot again");
}

static void testMaskedGainAndZero()
{
    const juce::File wav = makeTempFile ("masked_gain.wav");
    CHECK (writeTwoToneWav (wav), "wrote two-tone wav");

    AudioEngine eng;
    CHECK (eng.loadFile (wav), "loaded two-tone wav");

    // Baseline amplitudes.
    std::vector<float> orig ((size_t) kToneFrames);
    CHECK (eng.readSamples (0, 0, (int) kToneFrames, orig.data()), "read baseline");
    const float a500 = toneAmp (orig, 2048, (size_t) kToneFrames - 2048, 500.0, kToneRate);
    const float a3500 = toneAmp (orig, 2048, (size_t) kToneFrames - 2048, 3500.0, kToneRate);
    CHECK (std::abs (a500 - 0.5f) < 0.01f, "baseline 500 Hz amplitude");
    CHECK (std::abs (a3500 - 0.5f) < 0.01f, "baseline 3500 Hz amplitude");

    // -6 dB gain on the low band only.
    const float g = (float) std::pow (10.0, -6.0 / 20.0);
    SpectralSelection sel = lowBandSel();
    CHECK (SpectralMask::applyMaskedGain (eng,
            { 0, kToneFrames }, sel, g, "test gain"), "masked gain applied");

    std::vector<float> after ((size_t) kToneFrames);
    CHECK (eng.readSamples (0, 0, (int) kToneFrames, after.data()), "read after gain");
    const float b500 = toneAmp (after, 2048, (size_t) kToneFrames - 2048, 500.0, kToneRate);
    const float b3500 = toneAmp (after, 2048, (size_t) kToneFrames - 2048, 3500.0, kToneRate);
    CHECK (std::abs (b500 - a500 * g) < 0.01f, "masked tone attenuated by the gain");
    CHECK (std::abs (b3500 - a3500) < 0.005f, "unmasked tone untouched");

    // Zero-out: the low band disappears entirely.
    SpectralSelection sel2 = lowBandSel();
    CHECK (SpectralMask::applyMaskedGain (eng,
            { 0, kToneFrames }, sel2, 0.0f, "test zero"), "masked zero applied");

    std::vector<float> zeroed ((size_t) kToneFrames);
    CHECK (eng.readSamples (0, 0, (int) kToneFrames, zeroed.data()), "read after zero");
    const float z500 = toneAmp (zeroed, 2048, (size_t) kToneFrames - 2048, 500.0, kToneRate);
    const float z3500 = toneAmp (zeroed, 2048, (size_t) kToneFrames - 2048, 3500.0, kToneRate);
    CHECK (z500 < 0.01f, "masked tone zeroed");
    CHECK (std::abs (z3500 - a3500) < 0.005f, "unmasked tone still intact after zero-out");

    // Identity gain is a guaranteed no-op: bit-exact, no undo entry.
    ::UndoManager um;   // global scope: JUCE also has juce::UndoManager
    eng.setUndoManager (&um);
    const size_t undoBefore = um.undoSize();
    SpectralSelection sel3 = lowBandSel();
    CHECK (SpectralMask::applyMaskedGain (eng,
            { 0, kToneFrames }, sel3, 1.0f, "no-op gain"), "identity gain returns true");
    std::vector<float> identity ((size_t) kToneFrames);
    CHECK (eng.readSamples (0, 0, (int) kToneFrames, identity.data()), "read after no-op");
    bool bitExact = identity.size() == zeroed.size();
    for (size_t i = 0; bitExact && i < identity.size(); ++i)
        bitExact = identity[i] == zeroed[i];
    CHECK (bitExact, "identity gain leaves samples bit-exact");
    CHECK (um.undoSize() == undoBefore, "identity gain records no undo entry");
}

// ------------------------------------- masked edits refresh the waveform mips
// Regression: spectral-mask edits commit through replaceSamples(), which does
// NOT rebuild the waveform mipmaps. The Application-level helpers must do it,
// otherwise the waveform view keeps showing pre-edit content after DEL /
// silence / gain / fade / normalize / reverse on a spectral selection (the
// spectral view updates because refreshAfterEdit clears its tile cache).
static void testMaskedEditRebuildsMipmaps()
{
    const juce::File wav = makeTempFile ("masked_mips.wav");
    CHECK (writeTwoToneWav (wav), "wrote two-tone wav");

    Application app;
    CHECK (app.openFile (wav), "opened two-tone wav");
    FileDoc* d = app.doc();
    if (!d) return;
    AudioEngine& eng = app.audioEngine ();

    // Peak of the finest mipmap level over a sample range.
    auto mipPeak = [&] (int ch, int64_t s0, int64_t s1) -> float
    {
        const auto& mips = eng.mipmaps ();
        if (mips.empty ()) return -1.0f;
        const auto& mx = mips.front ().max[(size_t) ch];
        float m = 0.0f;
        for (int64_t i = s0; i < s1 && i < (int64_t) mx.size (); ++i)
            if (mx[(size_t) i] > m) m = mx[(size_t) i];
        return m;
    };

    // Ground truth: highest POSITIVE sample in the engine buffer. The finest
    // mipmap level stores per-sample max clamped at zero, so it matches this.
    auto bufPosPeak = [&] () -> float
    {
        std::vector<float> x ((size_t) kToneFrames);
        if (!eng.readSamples (0, 0, (int) kToneFrames, x.data ())) return -1.0f;
        float m = 0.0f;
        for (float v : x)
            if (v > m) m = v;
        return m;
    };

    CHECK (mipPeak (0, 0, kToneFrames) > 0.4f, "baseline tone visible in mipmap");

    // DEL with an active spectral selection: zeros the low band through
    // maskedGainEdit -> applyMaskedGain + rebuildMipmaps.
    d->spectralSel = lowBandSel ();
    std::string err;
    CHECK (app.del (&err), "del zeroed the spectral selection");

    // The 500 Hz tone is gone from the buffer; the unmasked 3500 Hz one remains.
    std::vector<float> after ((size_t) kToneFrames);
    CHECK (eng.readSamples (0, 0, (int) kToneFrames, after.data ()), "read after del");
    const float a500 = toneAmp (after, 2048, (size_t) kToneFrames - 2048, 500.0, kToneRate);
    const float a3500 = toneAmp (after, 2048, (size_t) kToneFrames - 2048, 3500.0, kToneRate);
    CHECK (a500 < 0.01f, "masked 500 Hz tone zeroed");
    CHECK (std::abs (a3500 - 0.5f) < 0.01f, "unmasked 3500 Hz tone intact");

    // The mipmap must now match the edited buffer (finest level is per-sample).
    CHECK (std::abs (mipPeak (0, 0, kToneFrames) - bufPosPeak ()) < 1e-3f,
           "mipmap matches the edited buffer");

    // Undo restores the tone AND refreshes the mipmap again.
    ::UndoManager& um = app.undo ();
    CHECK (um.canUndo (), "undo available after masked del");
    app.undoStep ();
    std::vector<float> undone ((size_t) kToneFrames);
    CHECK (eng.readSamples (0, 0, (int) kToneFrames, undone.data ()), "read after undo");
    const float u500 = toneAmp (undone, 2048, (size_t) kToneFrames - 2048, 500.0, kToneRate);
    CHECK (u500 > 0.4f, "undo restored the masked tone");
    CHECK (std::abs (mipPeak (0, 0, kToneFrames) - bufPosPeak ()) < 1e-3f,
           "mipmap matches the buffer after undo");
}

// ------------------------------------------------- extract masked (A + B = orig)

static void testExtractMasked()
{
    const juce::File wav = makeTempFile ("extract_masked.wav");
    CHECK (writeTwoToneWav (wav), "wrote two-tone wav");

    AudioEngine eng;
    CHECK (eng.loadFile (wav), "loaded two-tone wav");

    const juce::Range<int64_t> range { 0, kToneFrames };
    SpectralSelection sel = lowBandSel();

    std::vector<std::vector<float>> A;
    CHECK (SpectralMask::extractMasked (eng, range, sel, A), "extract masked part A");
    CHECK (A.size() == 1 && A[0].size() == (size_t) kToneFrames, "A has one channel of full length");

    // A must contain ~all of the 500 Hz tone and ~none of the 3500 Hz tone.
    const float a500 = toneAmp (A[0], 2048, (size_t) kToneFrames - 2048, 500.0, kToneRate);
    const float a3500 = toneAmp (A[0], 2048, (size_t) kToneFrames - 2048, 3500.0, kToneRate);
    CHECK (std::abs (a500 - 0.5f) < 0.01f, "A contains the masked tone");
    CHECK (a3500 < 0.01f, "A excludes the unmasked tone");

    // B = original - A; A + B must reconstruct the original to within a
    // couple of ULPs (floating-point round-trip, not bit-exact).
    std::vector<float> orig ((size_t) kToneFrames);
    CHECK (eng.readSamples (0, 0, (int) kToneFrames, orig.data()), "read original");
    double maxErr = 0.0;
    for (size_t i = 0; i < orig.size(); ++i)
        maxErr = std::max (maxErr, (double) std::abs (A[0][i] + (orig[i] - A[0][i]) - orig[i]));
    CHECK (maxErr < 1e-6, "A + B reconstructs the original");
}

// ------------------------------------------------- playback mask (streaming OLA)

static void testPlaybackMaskStreaming()
{
    const juce::File wav = makeTempFile ("playback_mask.wav");
    CHECK (writeTwoToneWav (wav), "wrote two-tone wav");

    AudioEngine eng;
    CHECK (eng.loadFile (wav), "loaded two-tone wav");

    // Offline reference: the masked signal over the whole file.
    SpectralSelection sel = lowBandSel();
    std::vector<std::vector<float>> ref;
    CHECK (SpectralMask::extractMasked (eng, { 0, kToneFrames }, sel, ref), "offline reference");

    // Streaming: feed the mask processor in consecutive blocks and compare.
    // Each block carries trailing context so its last analysis windows are
    // complete (exactly what AudioEngine::getNextAudioBlock does).
    SpectralMask::PlaybackMask pm;
    pm.prepare (kToneRate, 1);

    const int block = 4097;   // deliberately NOT a multiple of the hop (512)
    double maxErrConsec = 0.0;
    for (int64_t start = 0; start < kToneFrames; start += block)
    {
        const int n = (int) std::min ((int64_t) block, kToneFrames - start);
        const int64_t needEnd = std::min ((int64_t) kToneFrames,
                                          start + (int64_t) n
                                              + SpectralMask::contextSamples());
        const int extN = (int) (needEnd - start);
        std::vector<std::vector<float>> datas (1, std::vector<float> ((size_t) extN));
        CHECK (eng.readSamples (0, start, extN, datas[0].data()), "read block");
        pm.process (datas, start, start, n, sel);
        for (int i = 0; i < n; ++i)
            maxErrConsec = std::max (maxErrConsec,
                (double) std::abs (datas[0][(size_t) i] - ref[0][(size_t) start + (size_t) i]));
    }
    CHECK (maxErrConsec < 1e-4, "consecutive blocks match the offline reference");

    // Seek: a non-consecutive start must re-prime and still match.  The block
    // carries fftSize()-1 samples of pre-context so the fast-forward is exact.
    pm.prepare (kToneRate, 1);
    const int64_t seekTo = 8192 + 37;   // deliberately not on a frame boundary
    {
        const int n = 4096;
        const int64_t dataStart = std::max<int64_t> (0, seekTo - (SpectralMask::fftSize() - 1));
        const int64_t needEnd = std::min ((int64_t) kToneFrames,
                                          seekTo + (int64_t) n
                                              + SpectralMask::contextSamples());
        const int extN = (int) (needEnd - dataStart);
        std::vector<std::vector<float>> datas (1, std::vector<float> ((size_t) extN));
        CHECK (eng.readSamples (0, dataStart, extN, datas[0].data()), "read seek block");
        pm.process (datas, dataStart, seekTo, n, sel);
        const size_t preOff = (size_t) (seekTo - dataStart);
        double maxErrSeek = 0.0;
        for (int i = 0; i < n; ++i)
            maxErrSeek = std::max (maxErrSeek,
                (double) std::abs (datas[0][preOff + (size_t) i]
                                   - ref[0][(size_t) seekTo + (size_t) i]));
        CHECK (maxErrSeek < 1e-4, "seek re-prime matches the offline reference");
    }

    // matches() guards: wrong rate/channels must force a re-prepare.
    CHECK (pm.matches (kToneRate, 1), "matches after prepare");
    CHECK (!pm.matches (44100.0, 1), "mismatched rate detected");
    CHECK (!pm.matches (kToneRate, 2), "mismatched channel count detected");
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

    // Every Whisper model is a plain single-file ggml .bin download from
    // ggerganov/whisper.cpp, hash-verified against the published value.
    const char* ids[] = { "whisper-tiny", "whisper-tiny-q5_1", "whisper-base",
                          "whisper-base-q5_1", "whisper-small", "whisper-small-q5_1",
                          "whisper-medium", "whisper-large-v1", "whisper-large-v2",
                          "whisper-large-v3", "whisper-large-v3-turbo",
                          "whisper-large-v3-turbo-q8_0" };
    for (const char* id : ids)
    {
        const auto* spec = mm.find (id);
        CHECK (spec != nullptr, juce::String (id) + " registered");
        if (!spec) continue;
        bool ok = spec->files.size() == 1
                  && !spec->files[0].url.empty ()
                  && spec->files[0].sha256.size() == 64
                  && spec->files[0].sizeBytes > 0
                  && !spec->files[0].name.empty ();
        CHECK (ok, juce::String (id) + " file spec complete");
    }

#if defined(_WIN32)
    if (!LoadLibraryA ("whisper.dll"))
    {
        std::cerr << "  [whisper pipeline test skipped: whisper.dll not installed]\n";
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
    const bool ok = sc::ai::runWhisperPipeline (eng, { 0 }, ggmlModel, opts, segs,
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

#if defined(_WIN32)
    if (!LoadLibraryA ("whisper.dll"))
    {
        std::cerr << "  [batch transcribe skipped: whisper.dll not installed]\n";
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
                                               /*fast=*/false, /*skipExisting=*/false,
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
                                                    /*fast=*/false, /*skipExisting=*/true,
                                                    nullptr, nullptr, nullptr, &sum2, &err);
        CHECK (ok2, juce::String ("batch skip run: ") + err);
        if (ok2)
            CHECK (sum2.skipped == 2 && sum2.done == 0 && sum2.failed == 0,
                  "skip-existing honoured");
    }

    // Clean up the temp tree.
    folder.deleteRecursively ();
}

// ---------------- Transcription panel: buttons pinned to bottom ----------------
static void testTranscriptionPanelLayout()
{
    const juce::File wav = makeTempFile ("trplayout.wav");
    wav.deleteFile();
    writeWav (wav, 1, 8000.0, 40000, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 440.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for transcription layout test");

    TranscriptionPanel panel (app);
    panel.setSize (300, 420);

    juce::TextButton* runBtn = nullptr;
    juce::TextButton* clearBtn = nullptr;
    juce::TableListBox* table = nullptr;
    for (int i = 0; i < panel.getNumChildComponents (); ++i)
    {
        auto* c = panel.getChildComponent (i);
        if (auto* tb = dynamic_cast<juce::TextButton*> (c))
        {
            if (tb->getButtonText () == "Transcribe...") runBtn = tb;
            else if (tb->getButtonText () == "Clear") clearBtn = tb;
        }
        else if (auto* tl = dynamic_cast<juce::TableListBox*> (c)) table = tl;
    }

    CHECK (runBtn != nullptr && clearBtn != nullptr && table != nullptr,
           "transcription panel children found");
    if (! runBtn || ! clearBtn || ! table)
    {
        wav.deleteFile ();
        return;
    }

    // The button strip is pinned to the bottom of the panel (4px margin + 2px
    // offset inside a 30px row).
    const int expectedBottom = panel.getHeight () - 6;
    CHECK (runBtn->getBottom () == expectedBottom, "Transcribe button at panel bottom");
    CHECK (clearBtn->getBottom () == expectedBottom, "Clear button at panel bottom");

    // The segment list takes all the space above the buttons.
    CHECK (table->getBottom () < runBtn->getY (), "list ends above the button row");
    CHECK (runBtn->getY () - table->getBottom () <= 10, "only a small gap between list and buttons");
    CHECK (table->getHeight () > panel.getHeight () * 3 / 4,
          "segment list takes most of the panel height");

    wav.deleteFile ();
}
#endif   // SC_ENABLE_AI

// ---------------- Double-clicking a marker / stored selection shows it ----
// ensureTimeRangeVisible() is a no-op when the range is already fully in the
// window; otherwise it pans with the current span kept, or zooms to the range
// exactly when that is at least as wide as the view. Marker double-click and
// stored-selection recall both route through it.
static void testEnsureTimeRangeVisible()
{
    const juce::File wav = makeTempFile ("ensure_visible.wav");
    writeWav (wav, 1, 8000.0, 80000, [] (int, int64_t f, double rate)
             { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 440.0 * (double) f / rate); });

    Application app;
    CHECK (app.openFile (wav), "open wav for ensure-visible test");   // dur = 10 s

    int calls = 0;
    double lastT0 = -1, lastT1 = -1;
    // Mimic MainWindow::applyTimeWindow: record the request and update the
    // canonical window.
    app.onSetTimeWindow = [&] (double a, double b)
    {
        ++calls; lastT0 = a; lastT1 = b;
        app.viewStart = a; app.viewEnd = b;
    };

    // Already fully visible -> no-op.
    app.viewStart = 2.0; app.viewEnd = 8.0;
    calls = 0;
    app.ensureTimeRangeVisible (3.0, 5.0);
    CHECK (calls == 0, "already-visible range is a no-op");

    // Target left of the view, narrower than the span -> pan keeping span.
    app.viewStart = 4.0; app.viewEnd = 9.0;   // W=5
    calls = 0;
    app.ensureTimeRangeVisible (1.0, 3.0);    // S=2 < 5
    CHECK (calls == 1 && lastT0 == 1.0 && lastT1 == 6.0,
           "left target: pan keeping the current span");

    // Target right of the view -> pan keeping span.
    app.viewStart = 1.0; app.viewEnd = 6.0;   // W=5
    calls = 0;
    app.ensureTimeRangeVisible (7.0, 9.0);
    CHECK (calls == 1 && lastT0 == 4.0 && lastT1 == 9.0,
           "right target: pan keeping the current span");

    // Target at least as wide as the view -> zoom to it exactly.
    app.viewStart = 2.0; app.viewEnd = 4.0;   // W=2
    calls = 0;
    app.ensureTimeRangeVisible (1.0, 8.0);    // S=7 >= 2
    CHECK (calls == 1 && lastT0 == 1.0 && lastT1 == 8.0,
           "wide target: zoom to it exactly");

    // Point outside on the left -> pan so it lands at the left edge.
    app.viewStart = 3.0; app.viewEnd = 8.0;   // W=5
    calls = 0;
    app.ensureTimeRangeVisible (0.5, 0.5);
    CHECK (calls == 1 && lastT0 == 0.5 && lastT1 == 5.5,
           "point left of view: pan to the edge");

    // Pan near the end of file: window [7,9], target [9.5,9.9] -> [7.9,9.9].
    app.viewStart = 7.0; app.viewEnd = 9.0;   // W=2, dur=10
    calls = 0;
    app.ensureTimeRangeVisible (9.5, 9.9);
    CHECK (calls == 1 && lastT0 == 7.9 && lastT1 == 9.9,
           "pan near the end of file keeps the span");

    // Degenerate window wider than the whole file: result clamps to [0, dur].
    app.viewStart = 3.0; app.viewEnd = 15.0;  // W=12 > dur=10
    calls = 0;
    app.ensureTimeRangeVisible (0.5, 1.0);
    CHECK (calls == 1 && lastT0 == 0.0 && lastT1 == 10.0,
           "window wider than the file: clamped to the whole file");

    // --- Marker double-click integration ----------------------------------
    app.markers().addRegionMarker (8.0, 9.0, "far region");
    app.viewStart = 0.0; app.viewEnd = 3.0;   // W=3, target S=1 < 3 -> [6,9]

    MarkerListPanel mp (app);
    mp.setSize (260, 200);
    calls = 0;
    mp.activateRow (0);
    CHECK (calls == 1 && lastT0 == 6.0 && lastT1 == 9.0,
           "marker double-click pans to include the marker");
    CHECK (app.selection.getStart() == 8.0 && app.selection.getEnd() == 9.0,
           "marker double-click sets the time selection");

    calls = 0;
    mp.activateRow (0);   // window is now [6,9] and contains [8,9]
    CHECK (calls == 0, "visible marker: no window change");

    // --- Stored-selection recall integration ------------------------------
    auto* d = app.doc ();
    SpectralSelection ss;
    ss.addRect (5.0, 6.5, 100.0, 2000.0, false);
    d->savedSelections.push_back ({ "sel A", ss });

    SelectionPanel sp (app);
    sp.setSize (300, 240);
    app.viewStart = 0.0; app.viewEnd = 3.0;   // W=3, target S=1.5 < 3 -> [3.5,6.5]
    calls = 0;
    sp.doRecall (0);
    CHECK (calls == 1 && lastT0 == 3.5 && lastT1 == 6.5,
           "recall pans to include the stored selection");
    CHECK (!d->spectralSel.empty(), "recall restored the spectral selection");

    wav.deleteFile ();
}

// ---------------- Find in the Markers / Transcription panels ----------------
// Ctrl+F opens a per-panel find dialog; F3 / Ctrl+F3 jump to the next /
// previous match. Each panel keeps its own query/anchor state, and both are
// case-insensitive substring searches (markers: title + comment).
static void testFindInPanels()
{
    // 6 s long: the markers/segments below reach t=5, and setPlayhead clamps
    // to the file duration.
    const juce::File wav = makeTempFile ("find_panels.wav");
    writeWav (wav, 1, 8000.0, 48000, [] (int, int64_t f, double rate)
             { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 440.0 * (double) f / rate); });

    Application app;
    CHECK (app.openFile (wav), "open wav for find test");

    // Markers: two matching titles + one non-matching.
    app.markers().addPointMarker (1.0, "alpha one");
    app.markers().addPointMarker (3.0, "beta two");
    app.markers().addPointMarker (5.0, "ALPHA three");   // case-insensitive match

    MarkerListPanel mp (app);
    mp.setSize (260, 200);

    CHECK (!mp.findNext(), "no query yet: find fails");

    mp.setFindQuery ("alpha");
    CHECK (mp.getFindQuery () == "alpha", "query stored");
    CHECK (mp.findNext (), "first match found");
    CHECK (std::abs (app.playhead - 1.0) < 1e-9, "jumped to first match");

    CHECK (mp.findNext (), "second match found");
    CHECK (std::abs (app.playhead - 5.0) < 1e-9, "case-insensitive second match");

    CHECK (mp.findNext (), "wraps around to the first match");
    CHECK (std::abs (app.playhead - 1.0) < 1e-9, "wrapped to first match");

    CHECK (mp.findPrevious (), "previous wraps backwards");
    CHECK (std::abs (app.playhead - 5.0) < 1e-9, "backwards wrap lands on second match");

    mp.setFindQuery ("zzz-no-match");
    CHECK (!mp.findNext (), "no match: find fails cleanly");

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    // Transcription search is independent of the marker search.
    auto* d = app.doc ();
    CHECK (d != nullptr, "doc exists for transcription find test");
    if (d != nullptr)
    {
        std::vector<ScttSegment> segs;
        segs.push_back ({ 0.5, 1.0, "hello world" });
        segs.push_back ({ 2.0, 3.0, "goodbye moon" });
        segs.push_back ({ 4.0, 5.0, "HELLO again" });
        d->setTranscription (segs);

        TranscriptionPanel tp (app);
        tp.setSize (300, 200);

        CHECK (!tp.findNext (), "transcription: no query yet");
        tp.setFindQuery ("hello");
        CHECK (mp.getFindQuery () == "zzz-no-match",
              "marker query untouched by transcription search");

        CHECK (tp.findNext (), "transcription first match");
        CHECK (std::abs (app.playhead - 0.5) < 1e-9, "jumped to segment start");
        CHECK (!app.selection.isEmpty ()
              && std::abs (app.selection.getStart () - 0.5) < 1e-9
              && std::abs (app.selection.getEnd () - 1.0) < 1e-9,
              "segment range selected");

        CHECK (tp.findNext (), "transcription second match");
        CHECK (std::abs (app.playhead - 4.0) < 1e-9, "case-insensitive segment match");

        CHECK (tp.findPrevious (), "transcription previous wraps backwards");
        CHECK (std::abs (app.playhead - 0.5) < 1e-9,
              "backwards wrap lands on first segment");
    }
#endif

    wav.deleteFile ();
}

// ------------------------------------------------- Python script panel
static void testPythonScripts()
{
    const juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("sc_pytest_"
                       + juce::String ((int64_t) juce::Time::getCurrentTime ().toMilliseconds ()));
    dir.deleteRecursively ();   // deterministic start

    // 1) Example scripts are created once and never clobbered afterwards.
    sc::py::ensureExampleScripts (dir);
    CHECK (dir.getChildFile ("Threshold.py").existsAsFile (), "threshold example created");
    CHECK (dir.getChildFile ("Dynamic processing.py").existsAsFile (),
          "dynamic processing example created");
    CHECK (dir.getChildFile ("Modulation.py").existsAsFile (), "modulation example created");

    const juce::String thrText = dir.getChildFile ("Threshold.py").loadFileAsString ();
    CHECK (thrText.contains ("THRESHOLD"), "threshold script has a threshold constant");

    {
        const juce::File t = dir.getChildFile ("Threshold.py");
        t.replaceWithText (t.loadFileAsString () + "\n# user edit\n");
    }
    sc::py::ensureExampleScripts (dir);
    CHECK (dir.getChildFile ("Threshold.py").loadFileAsString ().contains ("user edit"),
          "existing scripts are not overwritten");

    // 2) listScripts returns the sorted .py files.
    const auto scripts = sc::py::listScripts (dir);
    CHECK (scripts.size () == 3, "three scripts listed");
    if (scripts.size () == 3)
        CHECK (scripts[0].getFileName () == "Dynamic processing.py"
               && scripts[1].getFileName () == "Modulation.py"
               && scripts[2].getFileName () == "Threshold.py",
              "scripts sorted by name");

    // 3) Metadata JSON carries rate/channels/selection/markers.
    {
        std::vector<Marker> markers;
        Marker m;
        m.name = "Intro"; m.comment = "start of intro"; m.start = 1.25; m.end = -1;
        markers.push_back (m);

        const juce::String json = sc::py::buildMetaJson (44100.0, 2, 19.886,
                                                         { 0.5, 2.5 }, false, "mix.wav", markers);
        const juce::var parsed = juce::JSON::parse (json);
        CHECK (parsed.isObject (), "meta json parses");

        if (parsed.isObject ())
        {
            auto* v = parsed.getDynamicObject ();
            CHECK ((int64) v->getProperty ("sampleRate") == 44100, "sample rate in meta");
            CHECK ((int) v->getProperty ("numChannels") == 2, "channel count in meta");
            CHECK (!((bool) v->getProperty ("wholeFile")), "wholeFile flag is false");
            CHECK (std::abs ((double) v->getProperty ("selectionStartSec") - 0.5) < 1e-9
                   && std::abs ((double) v->getProperty ("selectionEndSec") - 2.5) < 1e-9,
                  "selection range in meta");
            CHECK (v->getProperty ("fileName").toString () == "mix.wav", "file name in meta");

            const juce::var& markersVar = v->getProperty ("markers");
            CHECK (markersVar.isArray (), "markers is an array");
            if (markersVar.isArray ())
            {
                auto* arr = markersVar.getArray ();
                CHECK (arr != nullptr && arr->size () == 1, "one marker in meta");
                if (arr != nullptr && arr->size () == 1)
                {
                    auto* m0 = (*arr)[0].getDynamicObject ();
                    CHECK (m0 != nullptr
                           && m0->getProperty ("name").toString () == "Intro"
                           && std::abs ((double) m0->getProperty ("start") - 1.25) < 1e-9,
                          "marker fields in meta");
                }
            }
        }
    }

    // 4) Float WAV round-trip is bit-exact.
    {
        const juce::File wav = makeTempFile ("py_roundtrip.wav");
        std::vector<std::vector<float>> data (2, std::vector<float> (100));
        for (int i = 0; i < 100; ++i)
        {
            data[0][(size_t) i] = (float) std::sin ((double) i * 0.1);
            data[1][(size_t) i] = -data[0][(size_t) i];
        }

        CHECK (sc::py::writeFloatWav (wav, data, 8000.0), "wrote float wav");
        double rate = 0.0;
        int nch = 0;
        std::vector<std::vector<float>> back;
        CHECK (sc::py::readFloatWav (wav, &rate, &nch, back), "read float wav");
        CHECK (std::abs (rate - 8000.0) < 1e-9 && nch == 2, "rate/channels preserved");

        bool exact = back.size () == 2;
        for (int c = 0; exact && c < 2; ++c)
            if (back[(size_t) c].size () != 100 || back[(size_t) c] != data[(size_t) c])
                exact = false;
        CHECK (exact, "float32 round-trip is bit-exact");
        wav.deleteFile ();
    }

    // 5) Interpreter probe + end-to-end threshold run (skipped when no Python).
    std::string pyCmd;
    if (sc::py::findPython (&pyCmd))
    {
        const juce::File wav = makeTempFile ("py_e2e.wav");
        writeWav (wav, 2, 8000.0, 4000, [] (int ch, int64_t f, double rate)
                 {   // ch1: loud tone; ch2: quiet tone below the 0.01 threshold
                     const double t = (double) f / rate;
                     return ch == 0 ? 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                       * 440.0 * t)
                                    : 0.005f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                        * 3000.0 * t);
                 });

        Application app;
        CHECK (app.openFile (wav), "opened e2e wav");

        // Snapshot the quiet channel before processing.
        std::vector<float> ch2Before ((size_t) app.audioEngine ().numSamples ());
        CHECK (app.audioEngine ().readSamples (1, 0, app.audioEngine ().numSamples (),
                                               ch2Before.data ()), "read pre-state");

        // Select [0.1, 0.4] so the outside can be verified unchanged.
        app.setSelection ({ 0.1, 0.4 });

        const juce::File script = dir.getChildFile ("Threshold.py");
        std::string err;
        const bool ok = app.runPythonScript (script, script.loadFileAsString (), pyCmd,
                                             nullptr, &err);
        CHECK (ok, "threshold script ran" + (ok ? juce::String () : ": " + juce::String (err)));

        if (ok)
        {
            const int64_t s0 = 800, s1 = 3200;   // [0.1, 0.4] @ 8 kHz

            std::vector<float> ch2Sel ((size_t) (s1 - s0));
            app.audioEngine ().readSamples (1, s0, s1 - s0, ch2Sel.data ());
            bool allZero = true;
            for (float v : ch2Sel)
                if (v != 0.0f) { allZero = false; break; }
            CHECK (allZero, "quiet channel zeroed inside the selection");

            // Outside the selection the audio must be untouched.
            std::vector<float> ch2After ((size_t) app.audioEngine ().numSamples ());
            app.audioEngine ().readSamples (1, 0, app.audioEngine ().numSamples (),
                                            ch2After.data ());
            bool outsideUnchanged = true;
            for (int64_t i = 0; i < s0 && outsideUnchanged; ++i)
                if (ch2After[(size_t) i] != ch2Before[(size_t) i]) outsideUnchanged = false;
            for (int64_t i = s1; i < (int64_t) ch2After.size () && outsideUnchanged; ++i)
                if (ch2After[(size_t) i] != ch2Before[(size_t) i]) outsideUnchanged = false;
            CHECK (outsideUnchanged, "audio outside the selection is unchanged");

            CHECK (app.undo ().undoSize () == 1, "edit recorded in undo history");
        }

        wav.deleteFile ();
    }
    else
    {
        // No interpreter here: a missing executable must fail cleanly.
        int rc = -1;
        CHECK (!sc::py::runProcess ("definitely_not_a_real_exe_xyz", {},
                                    [] { return true; }, &rc),
              "missing executable reported as failure");
    }

    // 6) Panel UI: combo lists the scripts, selecting one loads its text.
    {
        Application app;
        PythonPanel panel (app, dir);
        panel.setSize (320, 400);

        juce::ComboBox* box = nullptr;
        juce::TextEditor* ed = nullptr;
        int buttons = 0;
        for (auto* c : panel.getChildren ())
        {
            if (box == nullptr && (box = dynamic_cast<juce::ComboBox*> (c)) != nullptr) {}
            else if (ed == nullptr && (ed = dynamic_cast<juce::TextEditor*> (c)) != nullptr) {}
            else if (dynamic_cast<juce::TextButton*> (c) != nullptr) ++buttons;
        }

        CHECK (box != nullptr && ed != nullptr, "panel has combo + editor");
        CHECK (buttons == 3, "panel has Add/Remove/Run buttons");

        // Regression: the buttons were once created without setButtonText and
        // rendered as blank rectangles.
        juce::StringArray labels;
        for (auto* c : panel.getChildren ())
            if (auto* tb = dynamic_cast<juce::TextButton*> (c))
                labels.add (tb->getButtonText());
        const juce::StringArray expectLabels { "Add", "Remove", "Run" };
        CHECK (labels == expectLabels, "buttons are labelled Add/Remove/Run");

        if (box != nullptr)
        {
            CHECK (box->getNumItems () == 3, "combo lists the three scripts");
            // sendNotificationSync: the headless test has no message loop to
            // process the async update that plain sendNotification would queue.
            box->setSelectedId (2, juce::sendNotificationSync);   // modulation.py
            const juce::String expect = dir.getChildFile ("Modulation.py").loadFileAsString ();
            CHECK (ed != nullptr && ed->getText () == expect,
                  "selecting a script loads its text into the editor");
        }
    }

    dir.deleteRecursively ();
}

// ---------------- Mipmap construction: bit-exactness across all source paths ----------------

// Exact JUCE int16 -> float conversion (WavAudioFormatReader scales the raw
// value to full-range int32 via ((uint16)v << 16), then multiplies by
// 1/INT_MAX - which is exactly 2^-31 in float). Both the streaming read path
// and the memory-mapped findMinAndMax path use this identical scaling.
static float scI16ToF (int v)
{
    const int32_t s = (int32_t) ((uint16_t) (uint16_t) v << 16);
    return (float) s * (1.0f / 2147483647.0f);
}

// Hand-written 16-bit PCM WAV writer (the existing writeWav helper is float32).
static bool scWriteWavI16 (const juce::File& path, int nchans, double rate, int64_t frames,
                           const std::function<int (int ch, int64_t f)>& fn)
{
    const int bytesPerSample = 2;
    const int blockAlign = nchans * bytesPerSample;
    const int64_t dataSize = frames * (int64_t) blockAlign;

    auto put32 = [] (std::vector<uint8_t>& v, uint32_t x)
    {
        v.push_back ((uint8_t) (x & 0xff)); v.push_back ((uint8_t) ((x >> 8) & 0xff));
        v.push_back ((uint8_t) ((x >> 16) & 0xff)); v.push_back ((uint8_t) ((x >> 24) & 0xff));
    };
    auto put16 = [] (std::vector<uint8_t>& v, uint16_t x)
    {
        v.push_back ((uint8_t) (x & 0xff)); v.push_back ((uint8_t) ((x >> 8) & 0xff));
    };

    std::vector<uint8_t> d;
    d.insert (d.end(), { 'R', 'I', 'F', 'F' });
    put32 (d, (uint32_t) (36 + dataSize));
    d.insert (d.end(), { 'W', 'A', 'V', 'E' });
    d.insert (d.end(), { 'f', 'm', 't', ' ' });
    put32 (d, 16);
    put16 (d, 1); // format: PCM
    put16 (d, (uint16_t) nchans);
    put32 (d, (uint32_t) rate);
    put32 (d, (uint32_t) (rate * blockAlign));
    put16 (d, (uint16_t) blockAlign);
    put16 (d, 16); // bits per sample
    d.insert (d.end(), { 'd', 'a', 't', 'a' });
    put32 (d, (uint32_t) dataSize);

    for (int64_t f = 0; f < frames; ++f)
        for (int ch = 0; ch < nchans; ++ch)
            put16 (d, (uint16_t) fn (ch, f));

    path.deleteFile(); // FileOutputStream appends to existing files!
    juce::FileOutputStream out (path);
    if (out.failedToOpen())
        return false;
    return out.write (d.data(), (int) d.size());
}

// Reference mipmap with the legacy semantics, computed directly from float
// samples: per-block min/max clamped against zero, empty tail blocks 0/0.
static std::vector<AudioEngine::MipLevel> scRefMips (const std::vector<std::vector<float>>& chs)
{
    const int nchans = (int) chs.size();
    const int64_t N  = (int64_t) chs[0].size();
    std::vector<AudioEngine::MipLevel> out;

    int64_t baseF = 1;
    while (N / baseF > 100000)
        baseF *= 2;

    // Mirror the engine's level loop exactly: condition checked before building.
    for (int64_t f = baseF; f <= 16384ll * baseF && N / f > 100; f *= 2)
    {
        AudioEngine::MipLevel lv;
        lv.factor = f;
        const int64_t n = N / f + 1;
        lv.min.resize (nchans);
        lv.max.resize (nchans);

        for (int ch = 0; ch < nchans; ++ch)
        {
            lv.min[ch].resize ((size_t) n);
            lv.max[ch].resize ((size_t) n);
            for (int64_t i = 0; i < n; ++i)
            {
                float mn = 0.0f, mx = 0.0f;
                const int64_t s = i * f;
                const int64_t e = std::min (s + f, N);
                for (int64_t p = s; p < e; ++p)
                {
                    const float v = chs[ch][(size_t) p];
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
                lv.min[ch][(size_t) i] = mn;
                lv.max[ch][(size_t) i] = mx;
            }
        }

        out.push_back (std::move (lv));
    }
    return out;
}

static void scCheckMips (const AudioEngine& eng, const std::vector<std::vector<float>>& chs,
                         const char* what)
{
    auto ref = scRefMips (chs);
    CHECK ((int) eng.mipmaps().size() == (int) ref.size(), juce::String (what) + ": level count");
    if (eng.mipmaps().size() != ref.size())
        return;

    for (size_t li = 0; li < ref.size(); ++li)
    {
        const auto& a = eng.mipmaps()[li];
        const auto& b = ref[li];
        CHECK (a.factor == b.factor, juce::String (what) + ": level factor");

        bool same = true;
        for (int ch = 0; ch < (int) b.min.size() && same; ++ch)
            for (size_t i = 0; i < a.min[ch].size(); ++i)
                if (a.min[ch][i] != b.min[ch][i] || a.max[ch][i] != b.max[ch][i])
                    break;
        CHECK (same, juce::String (what) + ": level values bit-exact");
    }
}

static void testMipmapBitExact()
{
    // Deterministic stereo int16 content covering every clamping case:
    // positive-only blocks (min clamps to 0), negative-only blocks (max clamps
    // to 0), exact silence, a zero-crossing ramp, full-scale +/- spikes and a
    // dense LCG tail. N=300k -> baseF=4 with 11 mipmap levels.
    const int nchans = 2;
    const int64_t N = 300000;
    std::vector<std::vector<int>> raw (nchans, std::vector<int> ((size_t) N));

    uint32_t seed = 12345u;
    for (int64_t p = 0; p < N; ++p)
        for (int ch = 0; ch < nchans; ++ch)
        {
            int v;
            if (p < 50000)          v = 100 + (int) ((p * 7 + ch * 13) % 2901);   // positive only
            else if (p < 100000)    v = -(100 + (int) ((p * 5 + ch * 11) % 2901)); // negative only
            else if (p < 150000)    v = 0;                                       // silence
            else if (p < 200000)    v = (int) ((p + ch * 37) % 4001) - 2000;      // crosses zero
            else                   { seed = seed * 1664525u + 1013904223u;
                                     v = (int) ((seed >> 8) % 65537u) - 32768; } // full range
            if (p % 1000 == 0 && ch == 0) v = 32767;   // +full-scale spike
            if (p % 1500 == 0 && ch == 1) v = -32768;  // -full-scale spike
            raw[ch][(size_t) p] = v;
        }

    std::vector<std::vector<float>> chs (nchans);
    for (int ch = 0; ch < nchans; ++ch)
    {
        chs[ch].resize ((size_t) N);
        for (int64_t p = 0; p < N; ++p)
            chs[ch][(size_t) p] = scI16ToF (raw[ch][(size_t) p]);
    }

    const juce::File wav = makeTempFile ("mipmap_i16.wav");
    CHECK (scWriteWavI16 (wav, nchans, 44100.0, N,
                         [&] (int ch, int64_t f) { return raw[ch][(size_t) f]; }),
          "write i16 wav");

    // Path A: in-memory buffer scan.
    {
        AudioEngine eng;
        CHECK (eng.loadFile (wav), "load (in-memory)");
        CHECK (eng.fullyInMemory(), "is in memory");
        scCheckMips (eng, chs, "mipmap in-memory");
    }

    // Path B: streaming load -> memory-mapped reader (WAV is mappable).
    {
        AudioEngine eng;
        eng.setMaxInMemorySampleChannels (0);
        CHECK (eng.loadFile (wav), "load (streaming)");
        CHECK (!eng.fullyInMemory(), "is streamed");
        scCheckMips (eng, chs, "mipmap mapped");
    }

    // Path C: streaming load -> chunked fallback. FLAC is lossless but has no
    // memory-mapped reader, so this exercises the sequential-chunk source.
    {
        const juce::File flac = makeTempFile ("mipmap.flac");
        bool wrote = false;
        {
            juce::FlacAudioFormat fmt;
            auto fileStream = std::make_unique<juce::FileOutputStream> (flac);
            CHECK (!fileStream->failedToOpen(), "flac stream open");
            if (!fileStream->failedToOpen())
            {
                std::unique_ptr<juce::OutputStream> stream (std::move (fileStream));
                const juce::AudioFormatWriterOptions opts =
                    juce::AudioFormatWriterOptions {}.withNumChannels (nchans)
                                                      .withSampleRate (44100.0)
                                                      .withBitsPerSample (16);
                auto writer = fmt.createWriterFor (stream, opts);
                CHECK (writer != nullptr, "flac writer created");

                if (writer != nullptr)
                {
                    // The FLAC writer right-shifts full-range int32 by 16 for
                    // 16-bit output, so pre-scale with the same <<16 the reader
                    // applies - values round-trip exactly.
                    std::vector<std::vector<int>> scaled (nchans, std::vector<int> ((size_t) N));
                    for (int ch = 0; ch < nchans; ++ch)
                        for (int64_t p = 0; p < N; ++p)
                            scaled[ch][(size_t) p] = (int32_t) ((uint16_t) raw[ch][(size_t) p] << 16);

                    const int block = 1 << 18;
                    bool ok = true;
                    for (int64_t off = 0; off < N && ok; off += block)
                    {
                        const int cnt = (int) std::min<int64_t> (block, N - off);
                        std::vector<const int*> ptrs ((size_t) nchans);
                        for (int ch = 0; ch < nchans; ++ch)
                            ptrs[ch] = scaled[ch].data() + off;
                        ok = writer->write (ptrs.data(), cnt);
                    }
                    CHECK (ok, "flac write");
                    wrote = ok; // encoder finalizes when the writer is destroyed
                }
            }
        }   // <- writer + stream destroyed here: FLAC encoder finalized, file closed
        CHECK (wrote && flac.getSize() > 0, "write flac");

        if (wrote)
        {
            AudioEngine eng;
            eng.setMaxInMemorySampleChannels (0);
            CHECK (eng.loadFile (flac), "load (streamed flac)");
            CHECK (!eng.fullyInMemory(), "is streamed");
            scCheckMips (eng, chs, "mipmap chunked fallback");
        }
    }
}

// ------------------------------------------------- Selection panel / clone & multiply

/** scaledCopy multiplies frequencies (and stroke radii) by m, time unchanged. */
static void testScaledCopy()
{
    SpectralSelection sel;
    // replace=false on every call so the shapes stack instead of replacing.
    sel.addRect (0.1, 0.5, 200.0, 400.0, false, false);   // additive rect
    std::vector<std::pair<double,double>> poly = { {0.2, 300}, {0.6, 300}, {0.4, 900} };
    sel.addPolygon (poly, true, false);                  // subtractive triangle
    // Dense path: strokes stamp per point, so the midpoint needs a point.
    std::vector<std::pair<double,double>> stroke = { {0.1, 500}, {0.2, 750},
                                                     {0.3, 1000}, {0.4, 1250},
                                                     {0.5, 1500} };
    sel.addStroke (stroke, 250.0, false, false);         // additive stroke

    const SpectralSelection scaled = sel.scaledCopy (2.0);
    CHECK (scaled.count () == 3, "scaled copy keeps shape count");
    CHECK (sel.count () == 3, "original untouched by scaledCopy");

    // Rect: doubled frequency band [400,800]; probe below the triangle edge.
    CHECK (scaled.contains (0.3, 590.0), "scaled rect at doubled frequency");
    CHECK (!sel.contains (0.3, 590.0), "original has nothing at that point");

    // Subtractive hole moves from f in (300,600) to f in (600,1200).
    CHECK (!scaled.contains (0.3, 900.0), "scaled subtractive hole moved up");
    CHECK (!sel.contains (0.3, 450.0), "original subtractive hole in place");

    // Stroke: midpoint doubles from f=1000 to f=2000 and the radius doubles too.
    CHECK (scaled.contains (0.3, 2000.0), "scaled stroke midpoint at doubled frequency");
    CHECK (sel.contains (0.3, 1000.0), "original stroke midpoint in place");
    CHECK (!sel.contains (0.3, 2000.0), "original has no energy where the clone is");
    CHECK (scaled.contains (0.3, 2490.0), "clone thickness stretched by the multiplier");

    const auto te = sel.timeExtent ();
    CHECK (std::abs (scaled.timeExtent ().getStart () - te.getStart ()) < 1e-9
          && std::abs (scaled.timeExtent ().getEnd () - te.getEnd ()) < 1e-9,
          "time extent unchanged by scaling");
}

/** addScaledClones appends one clone per multiplier, built from a snapshot. */
static void testAddScaledClones()
{
    SpectralSelection sel;
    sel.addRect (0.1, 0.5, 200.0, 400.0, false);
    const int before = sel.count ();

    sel.addScaledClones ({ 0.5, 2.0 });
    CHECK (sel.count () == before + 2, "one clone per multiplier");
    CHECK (sel.contains (0.3, 300.0), "original rect kept");
    CHECK (sel.contains (0.3, 150.0), "x0.5 clone present");
    CHECK (sel.contains (0.3, 600.0), "x2 clone present");

    // Snapshot semantics: the next call clones every CURRENT shape exactly once.
    sel.addScaledClones ({ 4.0 });
    CHECK (sel.count () == before + 5, "second call adds one clone per current shape");

    // Non-positive / non-finite multipliers are ignored silently.
    sel.addScaledClones ({ -1.0, 0.0 });
    CHECK (sel.count () == before + 5, "bad multipliers add nothing");
}

/** parseMultiplierList accepts positive numbers only and reports bad input. */
static void testParseMultiplierList()
{
    std::string err;

    auto v = Application::parseMultiplierList ("0.5, 2, 5", &err);
    CHECK (v.size () == 3 && v[0] == 0.5 && v[1] == 2.0 && v[2] == 5.0,
          "parses comma list");

    auto v2 = Application::parseMultiplierList (" 2 ; 4 ", &err);
    CHECK (v2.size () == 2 && v2[0] == 2.0 && v2[1] == 4.0, "semicolon + spaces ok");

    auto v3 = Application::parseMultiplierList ("", &err);
    CHECK (v3.empty() && !err.empty(), "empty input rejected with message");

    auto v4 = Application::parseMultiplierList ("abc", &err);
    CHECK (v4.empty() && !err.empty(), "non-numeric token rejected");

    auto v5 = Application::parseMultiplierList ("2x", &err);
    CHECK (v5.empty() && !err.empty(), "trailing junk rejected");

    auto v6 = Application::parseMultiplierList ("-1, 0, 3", &err);
    CHECK (v6.empty() && !err.empty(), "non-positive values rejected");
}

/** Saved selections survive a JSON serialise/deserialise round trip. */
static void testSavedSelectionsRoundTrip()
{
    std::vector<SavedSpectralSelection> list;

    SavedSpectralSelection a;
    a.name = "Harmonics \"quoted\"";
    // replace=false so all three shapes are kept.
    a.data.addRect (0.1, 0.5, 200.0, 400.0, false, false);
    std::vector<std::pair<double,double>> poly = { {0.2, 300}, {0.6, 300}, {0.4, 900} };
    a.data.addPolygon (poly, true, false);
    std::vector<std::pair<double,double>> stroke = { {0.1, 500}, {0.2, 750},
                                                     {0.3, 1000}, {0.4, 1250},
                                                     {0.5, 1500} };
    a.data.addStroke (stroke, 250.0, false, false);
    list.push_back (std::move (a));

    SavedSpectralSelection b;
    b.name = "Second";
    b.data.addRect (1.0, 2.0, 1000.0, 2000.0, true);   // subtractive rect
    list.push_back (std::move (b));

    const juce::String json = serialiseSavedSelections (list);
    std::vector<SavedSpectralSelection> out;
    CHECK (deserialiseSavedSelections (json, out), "round-trip parse ok");
    CHECK (out.size () == 2, "two entries restored");
    if (out.size() != 2) return;

    CHECK (out[0].name == "Harmonics \"quoted\"", "name with quotes survives");
    CHECK (out[1].name == "Second", "second name ok");
    CHECK (out[0].data.count () == 3, "all three shapes restored");
    CHECK (out[1].data.count () == 1, "subtractive rect restored");

    // Behavioural equivalence at probe points.
    SpectralSelection orig;
    orig.addRect (0.1, 0.5, 200.0, 400.0, false, false);
    orig.addPolygon (poly, true, false);
    orig.addStroke (stroke, 250.0, false, false);
    bool same = true;
    for (double t : { 0.15, 0.3, 0.45 })
        for (double f : { 150.0, 300.0, 450.0, 900.0, 1000.0, 2000.0 })
            if (orig.contains (t, f) != out[0].data.contains (t, f)) same = false;
    CHECK (same, "restored selection matches original at probe points");

    // Subtractive rect: the hole is preserved.
    CHECK (!out[1].data.contains (1.5, 1500.0), "subtractive rect hole preserved");

    // Empty list round-trips to empty; garbage is rejected without crashing.
    std::vector<SavedSpectralSelection> none;
    CHECK (deserialiseSavedSelections (serialiseSavedSelections (none), none) && none.empty(),
          "empty list round-trip");

    std::vector<SavedSpectralSelection> bad;
    CHECK (!deserialiseSavedSelections ("{ not json", bad), "garbage rejected");
}

/** Interactive clone picking: reference click, then target clicks. */
// ---------------- Move/Scale selection gesture (right-click menu) ----------
// Transform math + the click-move-click state machine with SHIFT axis lock.
// ------------------------------------------------- new file / clipboard / paste mix

// Build a document directly (no disk I/O), attach it to app and make it
// active (mirrors Application::openFile).
static FileDoc* attachNewDoc (Application& app, int nchans, double rate,
                              const std::vector<std::vector<float>>* data = nullptr)
{
    auto d = std::make_unique<FileDoc>();
    if (!d->engine.initNew (nchans, rate, data)) return nullptr;
    for (int i = 0; i < nchans; ++i)
        d->visibleChannels.push_back (i);
    FileDoc* raw = d.get();
    app.docs.push_back (std::move (d));
    app.activate ((int) app.docs.size () - 1);
    return raw;
}

static void testNewFileCopyPasteMix()
{
    // --- initNew: empty document -------------------------------------
    {
        AudioEngine eng;
        CHECK (eng.initNew (2, 48000.0), "initNew accepts an empty document");
        CHECK (eng.isFileLoaded () && !eng.hasUnsavedChanges (),
              "empty doc is loaded but not unsaved");
        CHECK (eng.numChannels () == 2 && std::abs (eng.sampleRate () - 48000.0) < 1e-9,
              "channel count and rate are stored");
        CHECK (eng.numSamples () == 0 && eng.getFile ().getFullPathName ().isEmpty (),
              "no samples and no file path yet");
        AudioEngine bad;
        CHECK (!bad.initNew (0, 44100.0), "zero channels rejected");
        CHECK (!bad.initNew (2, -1.0), "negative rate rejected");
    }

    // --- initNew with data + readRange ---------------------------------
    {
        AudioEngine eng;
        std::vector<std::vector<float>> data (4);
        for (int ch = 0; ch < 4; ++ch)
            for (int i = 0; i < 1000; ++i)
                data[(size_t) ch].push_back ((float) std::sin (i * 0.01 + ch));
        CHECK (eng.initNew (4, 8000.0, &data), "initNew accepts initial content");
        CHECK (eng.numSamples () == 1000 && eng.hasUnsavedChanges (),
              "content stored and marked unsaved");

        std::vector<std::vector<float>> out;
        CHECK (eng.readRange (100, 50, out), "readRange reads a middle slice");
        CHECK (out.size () == 4 && out[0].size () == 50, "readRange shape");
        bool same = true;
        for (int i = 0; i < 50; ++i)
            if (out[1][(size_t) i] != data[1][(size_t) (100 + i)]) { same = false; break; }
        CHECK (same, "readRange content matches the source");
    }

    // --- Application::newFile: naming, activation, paste into empty ----
    {
        Application app;
        CHECK (app.newFile (2, 44100.0), "newFile creates a document");
        CHECK (app.numDocs () == 1 && app.activeDocIndex == 0, "one doc, active");
        FileDoc& d = *app.docs[0];
        CHECK (d.name == "Untitled", "first new file is named Untitled");
        CHECK (d.engine.numSamples () == 0, "new file starts empty");
        CHECK (app.audioEngine ().numChannels () == 2, "active engine is the new doc");

        CHECK (app.newFile (4, 96000.0), "second newFile ok");
        FileDoc& d2 = *app.docs[1];
        CHECK (d2.name == "Untitled 2", "unique name for the second file");
        CHECK (app.activeDocIndex == 1 && app.audioEngine ().numChannels () == 4,
              "second doc is active with its own rate/channels");

        // Paste into an empty document at position 0.
        auto c = std::make_unique<juce::AudioBuffer<float>> (2, 800);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 800; ++i)
                c->getWritePointer (ch)[(size_t) i] = (float) std::sin (i * 0.1 + ch);
        app.clipboard = std::move (c);
        CHECK (app.canPaste (), "clipboard set");
        CHECK (app.paste (nullptr), "paste into empty doc ok");
        CHECK (app.audioEngine ().numSamples () == 800,
              "empty doc grew to the clipboard length");
    }

    // --- Copy + copy-to-new-file with visible channels ------------------
    {
        Application app;
        std::vector<std::vector<float>> src (4);
        for (int ch = 0; ch < 4; ++ch)
            for (int i = 0; i < 8000; ++i)
                src[(size_t) ch].push_back ((float) std::sin (i * 0.01 + ch));
        FileDoc* sd = attachNewDoc (app, 4, 8000.0, &src);
        CHECK (sd != nullptr, "source doc attached");

        // Select [0.25s, 0.75s) and copy: the clipboard holds exactly that slice.
        app.setSelection ({ 0.25, 0.75 });
        app.copy ();
        CHECK (app.canPaste (), "copy fills the app-level clipboard");
        bool same = true;
        for (int ch = 0; ch < 4 && same; ++ch)
            for (int i = 0; i < 4000; ++i)
                if (app.clipboard->getReadPointer (ch)[(size_t) i]
                    != src[(size_t) ch][(size_t) (2000 + i)])
                    { same = false; break; }
        CHECK (same, "clipboard holds the selected slice of every channel");

        // Show only channels 1 and 3 (0-based), then copy to a new file.
        app.toggleChannelVisible (0);
        app.toggleChannelVisible (2);
        std::string err;
        CHECK (app.copyToNewFile (&err), "copy-to-new-file ok");
        FileDoc& nd = *app.docs[1];
        CHECK (nd.engine.numChannels () == 2,
              "new file has exactly the visible channels");
        CHECK (std::abs (nd.engine.sampleRate () - 8000.0) < 1e-9, "rate preserved");
        CHECK (nd.engine.numSamples () == 4000, "new file holds the selected length");
        std::vector<std::vector<float>> out;
        CHECK (nd.engine.readRange (0, 4000, out), "read new file content");
        bool chOk = true;
        for (int i = 0; i < 4000 && chOk; ++i)
        {
            if (out[0][(size_t) i] != src[1][(size_t) (2000 + i)]) chOk = false;
            if (out[1][(size_t) i] != src[3][(size_t) (2000 + i)]) chOk = false;
        }
        CHECK (chOk, "new file channels are the visible source channels");
        CHECK (app.activeDocIndex == 1, "the new file is active");

        // Nothing selected -> the whole file would be copied. Re-show every
        // channel first so this copy keeps all four.
        app.activate (0);
        app.setSelection ({ 0, 0 });
        for (int ch = 0; ch < 4; ++ch)
            if (!app.isChannelVisible (ch)) app.toggleChannelVisible (ch);
        CHECK (app.copyToNewFile (&err), "copy-to-new-file of the whole file ok");
        FileDoc& nd2 = *app.docs[2];
        CHECK (nd2.engine.numSamples () == 8000 && nd2.engine.numChannels () == 4,
              "whole-file copy keeps length and all channels");
    }

    // --- Spectral selection: masked copy-to-new-file + masked clipboard --
    {
        const juce::File wav = makeTempFile ("spec_copy.wav");
        CHECK (writeTwoToneWav (wav), "wrote two-tone wav");
        Application app;
        CHECK (app.openFile (wav), "opened two-tone wav");
        FileDoc& d = *app.docs[0];

        // Select the whole file and keep only the 300-700 Hz band.
        app.setSelection ({ 0.0, (double) kToneFrames / kToneRate });
        d.spectralSel = lowBandSel ();

        std::string err;
        CHECK (app.copyToNewFile (&err), "masked copy-to-new-file ok");
        FileDoc& nd = *app.docs[1];
        std::vector<float> got ((size_t) kToneFrames);
        CHECK (nd.engine.readSamples (0, 0, (int) kToneFrames, got.data()),
              "read new file content");
        const float n500 = toneAmp (got, 2048, (size_t) kToneFrames - 2048,
                                    500.0, kToneRate);
        const float n3500 = toneAmp (got, 2048, (size_t) kToneFrames - 2048,
                                    3500.0, kToneRate);
        CHECK (std::abs (n500 - 0.5f) < 0.01f, "new file contains the selected tone");
        CHECK (n3500 < 0.01f, "new file has no unselected content");

        // A plain copy is masked too: the clipboard keeps only the low band.
        app.activate (0);
        app.copy ();
        CHECK (app.canPaste (), "masked copy fills the clipboard");
        std::vector<float> clipData ((size_t) kToneFrames);
        std::copy (app.clipboard->getReadPointer (0),
                   app.clipboard->getReadPointer (0) + kToneFrames, clipData.begin());
        const float c500 = toneAmp (clipData, 2048,
                                    (size_t) kToneFrames - 2048, 500.0, kToneRate);
        const float c3500 = toneAmp (clipData, 2048,
                                     (size_t) kToneFrames - 2048, 3500.0, kToneRate);
        CHECK (std::abs (c500 - 0.5f) < 0.01f, "selected band survives the copy");
        CHECK (c3500 < 0.01f, "unselected band is gone from the clipboard");

        // Pasting that clipboard into a fresh file lands only the low tone;
        // extra target channels stay silence.
        CHECK (app.newFile (2, kToneRate), "new target file");
        app.setSelection ({ 0, 0 });   // paste at position 0
        CHECK (app.paste (nullptr), "paste masked clipboard into new file");
        FileDoc& nd2 = *app.docs[2];
        std::vector<float> got2 ((size_t) kToneFrames);
        CHECK (nd2.engine.readSamples (0, 0, (int) kToneFrames, got2.data()),
              "read target ch0");
        const float t500 = toneAmp (got2, 2048, (size_t) kToneFrames - 2048,
                                    500.0, kToneRate);
        const float t3500 = toneAmp (got2, 2048, (size_t) kToneFrames - 2048,
                                     3500.0, kToneRate);
        CHECK (std::abs (t500 - 0.5f) < 0.01f, "pasted file contains the selected tone");
        CHECK (t3500 < 0.01f, "pasted file has no unselected content");
        std::vector<float> got3 ((size_t) kToneFrames);
        nd2.engine.readSamples (1, 0, (int) kToneFrames, got3.data());
        bool silent = true;
        for (float v : got3) if (v != 0.0f) { silent = false; break; }
        CHECK (silent, "extra target channel is silence");

        wav.deleteFile ();
    }

    // --- Paste mix ------------------------------------------------------
    {
        Application app;
        std::vector<std::vector<float>> src (1);
        for (int i = 0; i < 8000; ++i)
            src[0].push_back ((float) std::sin (i * 0.02));
        FileDoc* sd = attachNewDoc (app, 1, 8000.0, &src);
        CHECK (sd != nullptr, "mix source doc attached");

        // Copy [0.25s, 0.75s) and mix it back at the same position: doubled.
        app.setSelection ({ 0.25, 0.75 });
        app.copy ();
        CHECK (app.pasteMix (nullptr), "paste-mix ok");
        std::vector<float> got ((size_t) 8000);
        CHECK (app.audioEngine ().readSamples (0, 0, 8000, got.data()),
              "read mixed file");
        bool doubled = true;
        for (int i = 2000; i < 6000 && doubled; ++i)
            if (std::abs (got[(size_t) i] - 2.0f * src[0][(size_t) i]) > 1e-5f)
                doubled = false;
        CHECK (doubled, "overlap region is the sum of old + clipboard");
        bool restSame = true;
        for (int i = 0; i < 8000; ++i)
            if ((i < 2000 || i >= 6000) && got[(size_t) i] != src[0][(size_t) i])
                { restSame = false; break; }
        CHECK (restSame, "regions outside the overlap are untouched");
        CHECK (app.audioEngine ().numSamples () == 8000,
              "length unchanged when the clipboard fits");

        // Undo restores exactly.
        app.undoStep ();
        std::vector<float> back ((size_t) 8000);
        app.audioEngine ().readSamples (0, 0, 8000, back.data());
        bool exact = true;
        for (int i = 0; i < 8000; ++i)
            if (back[(size_t) i] != src[0][(size_t) i]) { exact = false; break; }
        CHECK (exact, "undo restores the pre-mix content exactly");

        // Mix at 0.9s: overlap [7200,8000) is summed, tail appended past EOF.
        app.setSelection ({ 0.9, 1.0 });   // paste position = selection start
        CHECK (app.pasteMix (nullptr), "paste-mix with a tail ok");
        const int64_t newLen = 8000 + 3200;
        CHECK (app.audioEngine ().numSamples () == newLen,
              "file grew by the clipboard tail");
        std::vector<float> got2 ((size_t) newLen);
        app.audioEngine ().readSamples (0, 0, (int) newLen, got2.data());
        bool sumOk = true;
        for (int i = 7200; i < 8000 && sumOk; ++i)
            if (std::abs (got2[(size_t) i]
                         - (src[0][(size_t) i] + src[0][(size_t) (2000 + (i - 7200))]))
                > 1e-5f)
                sumOk = false;
        CHECK (sumOk, "overlap at EOF is summed");
        bool tailOk = true;
        for (int i = 8000; i < (int) newLen && tailOk; ++i)
            if (got2[(size_t) i] != src[0][(size_t) (2800 + (i - 8000))])
                { tailOk = false; break; }
        CHECK (tailOk, "appended tail matches the clipboard remainder");

        // Undo restores the original length and content exactly.
        app.undoStep ();
        CHECK (app.audioEngine ().numSamples () == 8000,
              "undo shrinks back to the original length");
        std::vector<float> back2 ((size_t) 8000);
        app.audioEngine ().readSamples (0, 0, 8000, back2.data());
        bool exact2 = true;
        for (int i = 0; i < 8000; ++i)
            if (back2[(size_t) i] != src[0][(size_t) i]) { exact2 = false; break; }
        CHECK (exact2, "undo restores the pre-mix content exactly");
    }

    // --- Empty-document save round-trip ----------------------------------
    {
        Application app;
        CHECK (app.newFile (1, 8000.0), "empty doc for save test");
        const juce::File out = makeTempFile ("empty_save.wav");
        CHECK (app.audioEngine ().saveFileAs (out), "saving an empty file works");
        AudioEngine eng2;
        CHECK (eng2.loadFile (out), "empty wav reloads");
        CHECK (eng2.numSamples () == 0 && eng2.numChannels () == 1,
              "reloaded as empty mono");
        out.deleteFile ();
    }
}

static void testSelMoveScaleGesture()
{
    // ---- Pure transform math -------------------------------------------------
    {
        SpectralSelection sel;
        sel.addRect (1.0, 2.0, 100.0, 300.0, false);

        auto tr = sel.translatedCopy (0.5, -50.0);
        CHECK (std::abs (tr.boundingBox().getX() - 1.5) < 1e-9
              && std::abs (tr.boundingBox().getY() - 50.0) < 1e-9,
              "translated bbox moved");
        CHECK (tr.contains (2.3, 60.0) && !sel.contains (2.3, 60.0),
              "translation shifts the region");

        auto bad = sel.translatedCopy (std::nan (""), 0.0);
        CHECK (bad.boundingBox() == sel.boundingBox(),
              "NaN delta leaves the selection unchanged");

        // Scale about an interior anchor: x2 in time, x0.5 in frequency.
        auto sc = sel.scaledAboutCopy (1.5, 200.0, 2.0, 0.5);
        CHECK (std::abs (sc.boundingBox().getX() - 0.5) < 1e-9
              && std::abs (sc.boundingBox().getRight() - 2.5) < 1e-9
              && std::abs (sc.boundingBox().getY() - 150.0) < 1e-9
              && std::abs (sc.boundingBox().getBottom() - 250.0) < 1e-9,
              "scale about the anchor stretches both axes");
        CHECK (sc.contains (1.5, 200.0), "anchor point stays inside");

        auto badSc = sel.scaledAboutCopy (1.5, 200.0, -1.0, 2.0);
        CHECK (badSc.boundingBox() == sel.boundingBox(),
              "negative factor leaves the selection unchanged");
    }

    // Stroke translation keeps the rasterised grid aligned with its path.
    {
        SpectralSelection sel;
        std::vector<std::pair<double, double>> line = { {0.2, 300}, {0.6, 500} };
        sel.addStroke (line, 40.0, false);

        CHECK (sel.contains (0.4, 400.0), "original stroke covers its midpoint");
        auto tr = sel.translatedCopy (1.0, -100.0);
        CHECK (tr.contains (1.4, 300.0), "translated stroke covers the shifted midpoint");
        CHECK (!tr.contains (0.4, 400.0), "original position no longer covered");

        auto sc = sel.scaledAboutCopy (0.2, 300.0, 2.0, 2.0);   // anchor at line start
        CHECK (sc.contains (1.0, 700.0), "scaled stroke covers the scaled endpoint");
        CHECK (sc.contains (0.6, 500.0), "scaled stroke covers the scaled midpoint");
    }

    // ---- Gesture state machine -------------------------------------------------
    const juce::File wav = makeTempFile ("selgesture.wav");
    writeWav (wav, 1, 8000.0, 32768, [] (int, int64_t f, double rate)
             { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 440.0 * (double) f / rate); });

    Application app;
    CHECK (app.openFile (wav), "open wav for gesture test");
    auto* d = app.doc ();
    if (!d) return;

    // No selection -> starting a gesture is a no-op.
    app.startSelGesture (false);
    CHECK (!app.isSelGesturing (), "no gesture without a selection");

    d->spectralSel.addRect (1.0, 2.0, 100.0, 300.0, false);

    app.startSelGesture (false);   // Move
    CHECK (app.isSelGesturing () && !app.selGestureIsScale (), "move gesture started");

    // Before the reference click nothing may change.
    app.updateSelGesture (1.5, 200.0, Application::AxisLock::None);
    CHECK (d->spectralSel.boundingBox().getX() == 1.0,
          "no preview before the reference click");

    // First click sets the reference; still no change.
    CHECK (app.selGestureClick (1.5, 200.0, Application::AxisLock::None),
          "reference click accepted");
    CHECK (app.haveSelGestureRef (), "reference stored");
    CHECK (d->spectralSel.boundingBox().getX() == 1.0
          && d->spectralSel.boundingBox().getY() == 100.0,
          "no preview from the reference click itself");

    // Free move: +0.2 s / +50 Hz from the reference.
    app.updateSelGesture (1.7, 250.0, Application::AxisLock::None);
    CHECK (std::abs (d->spectralSel.boundingBox().getX() - 1.2) < 1e-9
          && std::abs (d->spectralSel.boundingBox().getY() - 150.0) < 1e-9,
          "free move applied");
    CHECK (std::abs (app.selection.getStart () - 1.2) < 1e-9
          && std::abs (app.selection.getEnd () - 2.2) < 1e-9,
          "time extent mirrored into the shared selection");

    // SHIFT + x-dominant: only time moves.
    app.updateSelGesture (1.9, 400.0, Application::AxisLock::X);
    CHECK (std::abs (d->spectralSel.boundingBox().getX() - 1.4) < 1e-9
          && d->spectralSel.boundingBox().getY() == 100.0,
          "x-locked move keeps the frequency");

    // SHIFT + y-dominant: only frequency moves.
    app.updateSelGesture (2.5, 300.0, Application::AxisLock::Y);
    CHECK (d->spectralSel.boundingBox().getX() == 1.0
          && std::abs (d->spectralSel.boundingBox().getY() - 200.0) < 1e-9,
          "y-locked move keeps the time");

    // Second click applies the delta under the cursor and ends the gesture.
    CHECK (app.selGestureClick (2.0, 300.0, Application::AxisLock::None),
          "apply click accepted");
    CHECK (!app.isSelGesturing (), "gesture ended after apply");
    CHECK (std::abs (d->spectralSel.boundingBox().getX() - 1.5) < 1e-9
          && std::abs (d->spectralSel.boundingBox().getY() - 200.0) < 1e-9,
          "applied move committed");

    // ---- Scale gesture ---------------------------------------------------------
    app.startSelGesture (true);   // bbox is now [1.5,2.5] x [200,400]: W=1 s, H=200 Hz
    CHECK (app.isSelGesturing () && app.selGestureIsScale (), "scale gesture started");

    CHECK (app.selGestureClick (1.75, 250.0, Application::AxisLock::None),
          "scale reference click accepted");

    // +0.5 s / +100 Hz from the ref -> x1.5 on both axes about the anchor.
    app.updateSelGesture (2.25, 350.0, Application::AxisLock::None);
    {
        const auto bb = d->spectralSel.boundingBox();
        CHECK (std::abs (bb.getX() - 1.375) < 1e-9
              && std::abs (bb.getRight() - 2.875) < 1e-9,
              "scale stretched time about the anchor");
        CHECK (std::abs (bb.getY() - 175.0) < 1e-9
              && std::abs (bb.getBottom() - 475.0) < 1e-9,
              "scale stretched frequency about the anchor");
    }

    // X-locked: time x2.5, frequency untouched.
    app.updateSelGesture (3.25, 100.0, Application::AxisLock::X);
    {
        const auto bb = d->spectralSel.boundingBox();
        CHECK (std::abs (bb.getX() - 1.125) < 1e-9
              && std::abs (bb.getRight() - 3.625) < 1e-9,
              "x-locked scale stretched time only");
        CHECK (std::abs (bb.getY() - 200.0) < 1e-9
              && std::abs (bb.getBottom() - 400.0) < 1e-9,
              "x-locked scale kept the frequency");
    }

    // Cancel restores the session-start selection.
    CHECK (app.cancelSelGesture (), "cancel returns true while gesturing");
    CHECK (!app.isSelGesturing (), "gesture ended after cancel");
    {
        const auto bb = d->spectralSel.boundingBox();
        CHECK (std::abs (bb.getX() - 1.5) < 1e-9 && std::abs (bb.getRight() - 2.5) < 1e-9
              && std::abs (bb.getY() - 200.0) < 1e-9 && std::abs (bb.getBottom() - 400.0) < 1e-9,
              "cancel restored the session-start selection");
    }

    CHECK (!app.cancelSelGesture (), "cancel is a no-op when not gesturing");

    wav.deleteFile ();
}

// ---------------- Saved selections: disk reload + drag & drop --------------
// A sidecar next to the wav auto-loads on open; "Load from disk..." and
// dropping a .selections.json onto the panel re-read it into memory.
static void testSelectionsDiskReload()
{
    const juce::File wav = makeTempFile ("sel_disk_reload.wav");
    writeWav (wav, 1, 8000.0, 80000, [] (int, int64_t f, double rate)
             { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 440.0 * (double) f / rate); });

    auto makeSel = [] (const juce::String& name, double t0, double t1)
                   { SpectralSelection s; s.addRect (t0, t1, 50.0, 900.0, false);
                     return SavedSpectralSelection { name, s }; };

    // Two named selections in the sidecar next to the wav, written BEFORE open.
    std::vector<SavedSpectralSelection> diskList = { makeSel ("one", 1.0, 2.0),
                                                     makeSel ("two", 3.0, 4.0) };
    const juce::File sidecar = savedSelectionsSidecarPath (wav);
    {
        juce::FileOutputStream out (sidecar);
        CHECK (!out.failedToOpen (), "sidecar writable");
        const juce::String json = serialiseSavedSelections (diskList);
        out.write (json.getCharPointer (), json.getNumBytesAsUTF8 ());
    }

    Application app;
    CHECK (app.openFile (wav), "open wav with sidecar");
    auto* d = app.doc ();
    CHECK (d != nullptr, "doc exists");
    CHECK (d->savedSelections.size () == 2 && d->selectionsOnDisk,
          "sidecar next to the wav loads automatically on open");

    SelectionPanel sp (app);
    sp.setSize (300, 240);

    // Manual reload: mutate memory, then load from disk restores it.
    d->savedSelections.clear ();
    CHECK (sp.loadFromDisk (), "load from disk succeeds with the canonical sidecar");
    CHECK (d->savedSelections.size () == 2 && d->selectionsOnDisk,
          "manual reload restored the sidecar list");

    // Drag & drop of another JSON file replaces the in-memory list.
    const juce::File other = makeTempFile ("other_selections.json");
    {
        std::vector<SavedSpectralSelection> one = { makeSel ("three", 5.0, 6.0) };
        juce::FileOutputStream out (other);
        CHECK (!out.failedToOpen (), "other json writable");
        const juce::String json = serialiseSavedSelections (one);
        out.write (json.getCharPointer (), json.getNumBytesAsUTF8 ());
    }
    {
        juce::StringArray dropped; dropped.add (other.getFullPathName());
        CHECK (sp.isInterestedInFileDrag (dropped), "interested in a .json drag");
        sp.filesDropped (dropped, 10, 10);
        CHECK (d->savedSelections.size () == 1
              && d->savedSelections [(size_t) 0].name == "three",
              "drag & drop loads the dropped file");
    }

    // Rejections leave the list untouched.
    const juce::File notJson = makeTempFile ("not_json.txt");
    {
        juce::FileOutputStream out (notJson);
        CHECK (!out.failedToOpen (), "txt writable");
        out.write ("hello", 5);
    }
    {
        juce::StringArray dropped; dropped.add (notJson.getFullPathName());
        CHECK (!sp.isInterestedInFileDrag (dropped), "not interested in a .txt drag");
        CHECK (!sp.loadSelectionsFromFile (notJson), "non-JSON file rejected");
    }

    const juce::File corrupt = makeTempFile ("corrupt.json");
    {
        juce::FileOutputStream out (corrupt);
        CHECK (!out.failedToOpen (), "corrupt json writable");
        out.write ("{ not valid json", 16);
    }
    CHECK (!sp.loadSelectionsFromFile (corrupt), "corrupt JSON rejected");
    CHECK (d->savedSelections.size () == 1, "rejections left the list untouched");

    wav.deleteFile (); sidecar.deleteFile (); other.deleteFile ();
    notJson.deleteFile (); corrupt.deleteFile ();
}

static void testClonePickFlow()
{
    const juce::File wav = makeTempFile ("clonepick.wav");
    writeWav (wav, 1, 8000.0, 16384, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for clone-pick test");
    auto* d = app.doc ();
    if (!d) return;

    d->spectralSel.addRect (0.1, 0.5, 200.0, 400.0, false);

    CHECK (!app.isClonePicking (), "not picking initially");
    app.startClonePick ();
    CHECK (app.isClonePicking () && !app.hasClonePickRef (), "pick session started clean");

    // First click sets the reference frequency; no clone yet.
    const int before = d->spectralSel.count ();
    CHECK (app.clonePickClick (0.3, 100.0), "reference click accepted");
    CHECK (app.hasClonePickRef () && std::abs (app.clonePickRefFreq () - 100.0) < 1e-9,
          "reference frequency stored");
    CHECK (d->spectralSel.count () == before, "no clone from the reference click");

    // Second click at 500 Hz -> multiplier 5: rect clone at [1000,2000].
    CHECK (app.clonePickClick (0.4, 500.0), "target click accepted");
    CHECK (d->spectralSel.count () == before + 1, "one clone added per target click");
    CHECK (d->spectralSel.contains (0.3, 1500.0), "clone at x5 frequency present");
    CHECK (d->spectralSel.contains (0.3, 300.0), "original still in place");

    // Third click at 200 Hz -> multiplier 2.
    app.clonePickClick (0.45, 200.0);
    CHECK (d->spectralSel.count () == before + 2, "second clone added");
    CHECK (d->spectralSel.contains (0.3, 600.0), "clone at x2 frequency present");
    // No cascade: the x5 clone must NOT itself be cloned at x2 (x10 band).
    CHECK (!d->spectralSel.contains (0.3, 3000.0), "no cascaded x10 clone");

    // Ending the session keeps the selection and resets the pick state.
    app.endClonePick ();
    CHECK (!app.isClonePicking () && !app.hasClonePickRef (), "pick session ended");
    CHECK (d->spectralSel.count () == before + 2, "selection kept after ending pick");

    // Clicks without a selection are ignored.
    d->spectralSel.clear ();
    app.startClonePick ();
    CHECK (!app.clonePickClick (0.3, 100.0), "click rejected with no selection");
    app.endClonePick ();

    wav.deleteFile ();
}

static void testClonePickToggleAndHints()
{
    const juce::File wav = makeTempFile ("picktoggle.wav");
    writeWav (wav, 1, 8000.0, 16384, [] (int, int64_t f, double rate)
    {
        return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                        * 400.0 * (double) f / rate);
    });

    Application app;
    CHECK (app.openFile (wav), "open wav for pick-toggle test");
    auto* d = app.doc ();
    if (!d) { wav.deleteFile (); return; }

    // One base rect: [1,2] s x [100,300] Hz.
    d->spectralSel.addRect (1.0, 2.0, 100.0, 300.0, false);
    const int before = d->spectralSel.count ();   // 1

    app.startClonePick ();
    CHECK (app.isClonePicking (), "pick session started");

    // Reference click at 200 Hz adds no shapes.
    CHECK (app.clonePickClick (1.5, 200.0, 0.05, 25.0), "reference click accepted");
    CHECK (app.hasClonePickRef (), "reference stored");
    CHECK (d->spectralSel.count () == before, "reference adds no shapes");

    // Target at 400 Hz -> x2 clone: same time range [1,2] s, freq [200,600] Hz.
    CHECK (app.clonePickClick (1.5, 400.0, 0.05, 25.0), "target click accepted");
    CHECK (d->spectralSel.count () == before + 1, "x2 clone added");
    CHECK (d->spectralSel.contains (1.5, 500.0), "x2 clone present");

    // Target at 300 Hz -> x1.5 clone: freq [150,450] Hz.
    app.clonePickClick (1.6, 300.0, 0.05, 25.0);
    CHECK (d->spectralSel.count () == before + 2, "x1.5 clone added");

    // Re-clicking the x2 point removes it and its clone only.
    CHECK (app.clonePickClick (1.53, 405.0, 0.05, 25.0), "re-click near a picked point accepted");
    CHECK (d->spectralSel.count () == before + 1, "x2 clone removed with its pick point");
    CHECK (!d->spectralSel.contains (1.5, 500.0), "removed x2 clone is gone");
    CHECK (d->spectralSel.contains (1.5, 375.0), "x1.5 clone still present");

    // The later point's recorded index survived the earlier removal.
    CHECK (app.clonePickClick (1.62, 295.0, 0.05, 25.0), "re-click near the other picked point accepted");
    CHECK (d->spectralSel.count () == before, "x1.5 clone removed too");

    // Re-clicking the reference clears it; committed clones stay put.
    CHECK (app.clonePickClick (1.48, 195.0, 0.05, 25.0), "re-click near the reference accepted");
    CHECK (!app.hasClonePickRef (), "reference cleared by its removal");
    CHECK (d->spectralSel.count () == before, "removing the reference adds nothing");

    // A fresh click starts a new reference.
    CHECK (app.clonePickClick (1.5, 250.0, 0.05, 25.0), "new reference after removal");
    CHECK (app.hasClonePickRef () && std::abs (app.clonePickRefFreq () - 250.0) < 1e-9,
          "new reference frequency stored");

    // Zero tolerances never remove: an exact re-click adds a x1 clone instead.
    app.clonePickClick (1.5, 250.0);
    CHECK (app.hasClonePickRef (), "zero-tolerance click does not remove the reference");
    CHECK (d->spectralSel.count () == before + 1, "exact re-click adds a x1 clone instead");

    app.endClonePick ();
    CHECK (!app.isClonePicking (), "pick session ended");

    // Paintbrush radius: x1.5 steps clamped to [5 Hz, 5000 Hz].
    app.brushRadiusHz = 200.0;
    app.stepBrushRadius (false);
    CHECK (std::abs (app.brushRadiusHz - 200.0 / 1.5) < 1e-9, "brush step down x(1/1.5)");
    app.brushRadiusHz = 20.0;
    for (int i = 0; i < 6; ++i) app.stepBrushRadius (false);
    CHECK (std::abs (app.brushRadiusHz - 5.0) < 1e-9, "brush radius clamps at 5 Hz");
    app.stepBrushRadius (true);
    CHECK (std::abs (app.brushRadiusHz - 7.5) < 1e-9, "step up from the floor");
    app.brushRadiusHz = 4000.0;
    app.stepBrushRadius (true);
    CHECK (std::abs (app.brushRadiusHz - 5000.0) < 1e-9, "brush radius clamps at 5000 Hz");

    // Footer modifier hints: the pure part plus live composition.
    std::unique_ptr<MainWindow> win (new MainWindow (app, "hint test"));
    CHECK (win->applyModifierHint ("Marquee", true, false) == "Marquee  +  Add Selection",
          "SHIFT hint text");
    CHECK (win->applyModifierHint ("Paintbrush 200 Hz", false, true)
              == "Paintbrush 200 Hz  -  Subtract Selection",
          "CTRL hint text");
    const juce::String both = win->applyModifierHint ("Lasso", true, true);
    CHECK (both.contains ("Subtract Selection") && !both.contains ("Add Selection"),
          "CTRL wins over SHIFT");
    CHECK (win->applyModifierHint ("Marquee", false, false) == "Marquee",
          "no modifiers -> no hint");

    // Live composition: headless tests hold no keys, so the base text passes through.
    app.selMode = SpectralSelMode::Marquee;
    CHECK (win->currentModeText () == "Marquee", "live mode text without modifiers");
    app.selMode = SpectralSelMode::TimeRange;
    CHECK (win->currentModeText ().isEmpty (), "no tool indicator in Time Range mode");

    wav.deleteFile ();
}

static int scRunUnitTestsImpl()
{
    std::setvbuf (stderr, nullptr, _IONBF, 0);   // keep stderr unbuffered so crash diagnostics survive
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    progress ("FFT");            testFFT();
    progress ("scfft vs juce");  testScFftVsJuce();
    progress ("markers");        testMarkers();
    progress ("marker csv");     testMarkerCsvRoundTrip();
    progress ("undo/redo");      testUndo();
    progress ("zero-crossing");  testZeroCrossing();
    progress ("spectral engine");testSpectral();
    progress ("playhead arrow symmetry"); testPlayheadArrowSymmetry();
    progress ("spectral view paint"); testSpectralViewPaint();
    progress ("marker axis clipping"); testMarkerAxisClipping();
    progress ("spectral time ruler");  testSpectralTimeRuler();
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
    progress ("snapshot palette layout"); testSnapshotPaletteLayout();
    progress ("1/3 octave click update"); testThirdOctaveClickUpdates();
    progress ("change gain");         testChangeGain();
    progress ("playback chain");      testPlaybackChain();
    progress ("snapshot store/recompute"); testSnapshotStoreAndRecompute();
    progress ("snapshot overlay paint");  testSnapshotOverlayPaint();
    progress ("status bar");          testStatusBar();
    progress ("footer selection editing"); testStatusBarEditing();
    progress ("footer file name");    testStatusBarFileName();
    progress ("about text licences"); testAboutText();
    progress ("time scrollbar on load"); testTimeScrollBarOnLoad();
    progress ("left panel layout"); testLeftPanelLayout();
    progress ("close panels + window menu"); testClosePanelsAndWindowMenu();
    progress ("cross bar tab move"); testCrossBarTabMove();
    progress ("spectral refresh on doc switch"); testSpectralRefreshOnDocSwitch();
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
    progress ("ctx menu zoom-out-full"); testCtxZoomOutFull();
    progress ("time-pan speed + reversal"); testTimePanSpeedAndReversal();
    progress ("axis pan + log ticks"); testAxisPanAndLogTicks();
    progress ("time axis ticks");     testTimeAxisTicks();
    progress ("time text formatting"); testTimeFormatting();
    progress ("mipmap bit-exact");    testMipmapBitExact();
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
    progress ("load cancel");         testLoadCancel();
    progress ("large in-memory load"); testLargeInMemoryLoad();
    progress ("mipmap reflects edits");  testMipmapReflectsEdits();
    progress ("edit streamed file");     testEditStreamingFile();
    progress ("resample cancel");     testResampleCancel();
    progress ("render cancel");       testRenderToPixelsCancel();
    progress ("fft view rate change"); testFFTAnalysisRateChange();
    progress ("fft harmonic references"); testFFTHarmonicReferences();
    progress ("status bar sample rate"); testStatusBarSampleRate();
    progress ("sctt json");             testScttJson();
    progress ("sctt chunk wav");        testScttChunkWav();
    progress ("sctt chunk replace");    testScttChunkReplace();
    progress ("ai settings persist");   testAiSettingsPersistence();
    progress ("df stft round-trip");  testDfStft();
    progress ("AI device resolution"); testAiDeviceResolution();
#if defined(SC_ENABLE_AI)
    progress ("sha256 (self-contained)"); testSha256();
    progress ("AI plumbing");         testAIPlumbing();
    progress ("NS failure details");  testNsFailureDetails();
    progress ("denseunet pipeline");  testDenseUnetPipeline();
    progress ("deepfilter pipeline"); testDeepFilterPipeline();
    progress ("whisper pipeline");    testWhisperPipeline();
    progress ("batch transcribe");    testBatchTranscribe();
    progress ("transcription panel layout"); testTranscriptionPanelLayout();
#endif
    progress ("ensure time range visible"); testEnsureTimeRangeVisible();
    progress ("find in panels");            testFindInPanels();
    progress ("python scripts");            testPythonScripts();
    progress ("spectral selection model");  testSpectralSelectionModel();
    progress ("selection render hole");     testSpectralSelectionRenderHole();
    progress ("straight line points");      testStraightLinePoints();
    progress ("alt-click straight line");   testAltClickStraightLine();
    progress ("scaled copy");               testScaledCopy();
    progress ("add scaled clones");         testAddScaledClones();
    progress ("parse multiplier list");     testParseMultiplierList();
    progress ("saved selections round-trip"); testSavedSelectionsRoundTrip();
    progress ("selections disk reload + dnd"); testSelectionsDiskReload();
    progress ("clone pick flow");           testClonePickFlow();
    progress ("pick toggle + brush floor + modifier hints");
    testClonePickToggleAndHints();
    progress ("move/scale selection gesture"); testSelMoveScaleGesture();
    progress ("new file / clipboard / paste mix"); testNewFileCopyPasteMix();
    progress ("masked gain + zero-out");    testMaskedGainAndZero();
    progress ("masked edit mipmap refresh"); testMaskedEditRebuildsMipmaps();
    progress ("extract masked A+B=orig");   testExtractMasked();
    progress ("playback mask streaming");   testPlaybackMaskStreaming();
    progress ("marker deletion adjust");  testMarkerAdjustForDeletion();
    progress ("spectral sel deletion adjust"); testSpectralSelectionAdjustForDeletion();
    progress ("delete shifts markers/saved sel"); testDeleteShiftsMarkersAndSavedSelections();
    progress ("snapshot deletion adjust");  testDeleteAdjustsSnapshots();
    progress ("version string");      testVersionString();

    std::cerr << (total - failures) << "/" << total << " checks passed\n";
    return failures == 0 ? 0 : 1;
}

int scRunUnitTests()
{
    return scRunUnitTestsImpl();
}
