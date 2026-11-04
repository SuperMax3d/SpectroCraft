// Regression tests for the 2026-11 audit fixes (spectral_dsp). Included by tests/UnitTests.cpp;
// uses its helpers (CHECK, progress, writeWav, makeTempFile, ...).

// In-memory mono document (no file, no I/O).
static bool sdspInitEngine (AudioEngine& eng, double rate, const std::vector<float>& mono)
{
    std::vector<std::vector<float>> data { mono };
    return eng.initNew (1, rate, mono.empty() ? nullptr : &data);
}

// Largest dB value of one tile frame.
static float sdspFrameMax (const SpectralTile& t, int f)
{
    float m = -300.0f;
    for (int b = 0; b < t.numBins; ++b)
        m = std::max (m, t.get (f, b));
    return m;
}

// spectral-dsp#W0/W1: scfft::realForward (AVX2 path when compiled in and
// supported, cached JUCE FFT otherwise) must match a fresh juce::dsp::FFT on
// every size, never write past its 2*N buffer, and stay correct when the
// per-thread FFT cache is reused and used from several threads at once.
static void testSdspScFftAllSizes()
{
    progress ("spectral_dsp: scfft vs juce::dsp::FFT, orders 1..16");
    const float SENTINEL = 1.2345e30f;
    const int PAD = 64;
    auto runOrder = [&] (int order, uint32_t seed, double& relErr, bool& oob)
    {
        const int N = 1 << order;
        std::vector<float> a ((size_t) (2 * N + 2 * PAD), SENTINEL), b ((size_t) N * 2, 0.0f);
        float* d = a.data() + PAD;
        for (int i = 0; i < 2 * N; ++i)
            d[i] = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            d[i] = b[(size_t) i] = (float) ((seed >> 8) / 8388608.0 - 1.0);
        }
        scfft::realForward (d, order);
        juce::dsp::FFT ref (order);
        ref.performRealOnlyForwardTransform (b.data(), true);

        oob = false;
        for (int k = 0; k < PAD; ++k)
            if (a[(size_t) k] != SENTINEL || a[(size_t) (PAD + 2 * N + k)] != SENTINEL)
                oob = true;
        double maxErr = 0.0, maxMag = 1e-12;
        for (int k = 0; k <= N / 2; ++k)
        {
            maxErr = std::max (maxErr, std::hypot ((double) d[k * 2] - b[(size_t) k * 2],
                                                   (double) d[k * 2 + 1] - b[(size_t) k * 2 + 1]));
            maxMag = std::max (maxMag, std::hypot ((double) b[(size_t) k * 2], (double) b[(size_t) k * 2 + 1]));
        }
        relErr = maxErr / maxMag;
    };

    bool allOk = true;
    for (int order = 1; order <= 16; ++order)
        for (int rep = 0; rep < 2; ++rep)   // second call reuses the cached FFT object
        {
            double e = 0.0; bool oob = false;
            runOrder (order, 0x9e3779b9u + (uint32_t) (order * 7 + rep), e, oob);
            if (e > 1e-4 || oob)
            {
                allOk = false;
                std::cerr << "  scfft order " << order << " relErr=" << e << " oob=" << oob << "\n";
            }
        }
    CHECK (allOk, juce::String ("scfft::realForward matches juce::dsp::FFT for N=2..65536 (")
                      + (scfft::hasAvx2() ? "AVX2 path" : "scalar path") + ")");

    std::atomic<int> bad { 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back ([&, t]
        {
            for (int it = 0; it < 40; ++it)
            {
                double e = 0.0; bool oob = false;
                runOrder (3 + (t + it) % 11, (uint32_t) (t * 1000 + it), e, oob);
                if (e > 1e-4 || oob) ++bad;
            }
        });
    for (auto& th : threads)
        th.join();
    CHECK (bad.load() == 0, "scfft::realForward is correct from concurrent threads (per-thread FFT cache)");
}

// spectral-dsp#W4: a sine of peak amplitude A must read 20*log10(A/sqrt 2)
// on its bin whatever the window (coherent gain + one-sided factor), i.e. a
// full-scale sine reads -3.01 dB = 90.97 dB-SPL at 1 Pa full scale, the same
// convention as the FFT panel's 1/3-octave dB-SPL mode.
static void testSdspSpectrogramCalibration()
{
    progress ("spectral_dsp: spectrogram dB calibration");
    const double rate = 48000.0;
    const int N = 2048, bin = 100;
    const double f = rate * bin / N;   // exactly on bin 100
    std::vector<float> tone ((size_t) 48000), dc ((size_t) 48000, 0.5f);
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = (float) std::sin (2.0 * juce::MathConstants<double>::pi * f * (double) i / rate);

    AudioEngine eng, engDc;
    CHECK (sdspInitEngine (eng, rate, tone) && sdspInitEngine (engDc, rate, dc), "calibration engines created");
    SpectralEngine sp;
    const char* names[] = { "rectangular", "hann", "hamming" };
    for (int wt = 0; wt <= 2; ++wt)
    {
        auto t = sp.computeTile (eng, 0, 0, 20000, N, 512, wt);
        const float v = t->get (10, bin);
        CHECK (std::abs (v - (-3.0103f)) < 0.05f,
               juce::String ("full-scale sine reads -3.01 dB (") + names[wt] + "), got " + juce::String (v, 2));
        auto tdc = sp.computeTile (engDc, 0, 0, 20000, N, 512, wt);
        const float vdc = tdc->get (10, 0);
        CHECK (std::abs (vdc - (-6.0206f)) < 0.05f,
               juce::String ("DC of 0.5 reads -6.02 dB (") + names[wt] + "), got " + juce::String (vdc, 2));
    }

    ScaleCalibration sc;
    sc.unit = ScaleCalibration::Unit::Pascal;
    sc.fullScaleValue = 1.0;
    Settings s;
    s.fftSize = N; s.hopSize = 512; s.windowType = 1;
    const float probe = sp.probeMagnitude (eng, 0, 0.2, f, s, 0);
    const double spl = (double) probe + sc.levelOffsetDb (0);
    CHECK (std::abs (spl - 90.97) < 0.1,
           "1 Pa full-scale sine probes at 90.97 dB-SPL, got " + juce::String (spl, 2));
}

// spectral-dsp#NEW (599aed4): tiles computed while recording must not be
// served after the document grew (tail tile cut short) or was overwritten in
// place (loop mode).
static void testSdspTileCacheFollowsContent()
{
    progress ("spectral_dsp: tile cache follows appended / overwritten content");
    const double rate = 8000.0;
    Settings s;
    s.fftSize = 256; s.hopSize = 64; s.windowType = 1;
    const int toneBin = 32;   // 1 kHz at 8 kHz / 256
    auto toneBlock = [rate] (int n)
    {
        std::vector<float> v ((size_t) n);
        for (int i = 0; i < n; ++i)
            v[(size_t) i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / rate);
        return v;
    };

    // (a) Append: 4000 silent samples, render, then 8000 tone samples.
    {
        AudioEngine eng;
        CHECK (eng.initNew (1, rate), "empty recording document");
        std::vector<float> silence ((size_t) 4000, 0.0f);
        CHECK (eng.appendSamples (silence.data(), 4000), "append silence");
        SpectralEngine sp;
        auto t1 = sp.getTiles (eng, 0, 0, eng.numSamples(), 0, s);
        CHECK (t1.size() == 1 && t1[0]->numFrames == 63, "tail tile covers the 4000 recorded samples");

        const auto tone = toneBlock (8000);
        CHECK (eng.appendSamples (tone.data(), 8000), "append tone");
        auto t2 = sp.getTiles (eng, 0, 0, eng.numSamples(), 0, s);
        CHECK (t2.size() == 1 && t2[0]->numFrames >= 12000 / 64,
               "tail tile recomputed after the recording grew");
        const float v = t2.empty() ? -300.0f : t2[0]->get (10000 / 64, toneBin);
        CHECK (v > -20.0f, "newly recorded tone visible in the refreshed tail tile, got " + juce::String (v, 1));
    }

    // (b) Loop-mode overwrite: same length, new content.
    {
        AudioEngine eng;
        CHECK (sdspInitEngine (eng, rate, std::vector<float> ((size_t) 20000, 0.0f)), "silent loop buffer");
        SpectralEngine sp;
        auto t1 = sp.getTiles (eng, 0, 0, eng.numSamples(), 0, s);
        CHECK (t1.size() == 2 && t1[0]->get (100, toneBin) < -100.0f, "loop buffer starts silent");

        const auto tone = toneBlock (20000);
        CHECK (eng.overwriteSamples (tone.data(), 0, 20000), "overwrite loop buffer");
        auto t2 = sp.getTiles (eng, 0, 0, eng.numSamples(), 0, s);
        const float v = t2.empty() ? -300.0f : t2[0]->get (100, toneBin);
        CHECK (v > -20.0f, "overwritten content visible (tile not served stale), got " + juce::String (v, 1));
        const float v2 = t2.size() < 2 ? -300.0f : t2[1]->get (10, toneBin);
        CHECK (v2 > -20.0f, "second tile refreshed too");

        // Unchanged content keeps serving the cached tiles.
        auto t3 = sp.getTiles (eng, 0, 0, eng.numSamples(), 0, s);
        CHECK (t3.size() == t2.size() && !t3.empty() && t3[0] == t2[0], "unchanged content reuses cached tiles");
    }
}

// spectral-dsp#W3: concurrent getTiles on one engine (shared pool) must each
// get fully computed tiles - no -300 dB frames inside the data.
static void testSdspConcurrentGetTiles()
{
    progress ("spectral_dsp: concurrent getTiles share the pool safely");
    const double rate = 8000.0;
    const int len = 65536;
    std::vector<std::vector<float>> data (2, std::vector<float> ((size_t) len));
    uint32_t seed = 12345u;
    for (auto& ch : data)
        for (auto& x : ch)
        {
            seed = seed * 1664525u + 1013904223u;
            x = (float) ((seed >> 8) / 8388608.0 - 1.0) * 0.5f;
        }
    AudioEngine eng;
    CHECK (eng.initNew (2, rate, &data), "noise document");

    SpectralEngine sp;
    std::atomic<int> holes { 0 }, empties { 0 };
    auto runner = [&] (int ch)
    {
        for (int it = 0; it < 12; ++it)
        {
            Settings s;
            s.fftSize = 256; s.windowType = 1;
            s.hopSize = 32 + it;   // fresh tile keys every iteration -> all misses
            auto tiles = sp.getTiles (eng, ch, 0, len, 0, s);
            if (tiles.empty()) { ++empties; continue; }
            for (const auto& t : tiles)
                for (int f = 0; f < t->numFrames; ++f)
                    if (t->startSample + (int64_t) f * t->hopSize < len && sdspFrameMax (*t, f) <= -299.0f)
                        ++holes;
        }
    };
    std::thread a (runner, 0), b (runner, 1);
    a.join();
    b.join();
    CHECK (empties.load() == 0, "concurrent getTiles return tiles");
    CHECK (holes.load() == 0, "concurrent getTiles never publish uncomputed (-300 dB) frames, holes="
                                  + juce::String (holes.load()));
}

// spectral-dsp#W2: the tile cache is bounded in bytes, not only in tiles.
static void testSdspCacheByteBudget()
{
    progress ("spectral_dsp: tile cache byte budget");
    const double rate = 8000.0;
    AudioEngine eng;
    CHECK (sdspInitEngine (eng, rate, std::vector<float> ((size_t) 16384 * 10, 0.1f)), "long document");
    SpectralEngine sp;
    const size_t budget = (size_t) 4 << 20;
    sp.setMaxCacheBytes (budget);
    Settings s;
    s.fftSize = 2048; s.hopSize = 64; s.windowType = 1;   // 1 MiB per tile
    const auto tiles = sp.getTiles (eng, 0, 0, eng.numSamples(), 0, s);
    CHECK (tiles.size() == 10, "all requested tiles returned even beyond the budget");
    CHECK (sp.cacheBytes() <= budget && sp.cacheSize() <= 4,
           "cache stays within its byte budget (" + juce::String ((int64_t) sp.cacheBytes()) + " bytes)");
    CHECK (sp.cacheSize() > 0, "cache still holds the most recent tiles");
    sp.clearCache();
    CHECK (sp.cacheBytes() == 0 && sp.cacheSize() == 0, "clearCache resets the byte count");
}

// spectral-dsp#W5: the rendered row of a tone must sit at the tone's
// frequency (bin = f * N / rate, as probeMagnitude uses), not N/(N-2) higher.
static void testSdspRenderBinMapping()
{
    progress ("spectral_dsp: render frequency-to-bin mapping");
    const double rate = 8000.0;
    std::vector<float> tone ((size_t) 16000);
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 3000.0 * (double) i / rate);
    AudioEngine eng;
    CHECK (sdspInitEngine (eng, rate, tone), "tone document");

    Settings s;
    s.fftSize = 256; s.hopSize = 64; s.windowType = 1;
    s.logFrequency = false; s.minFrequency = 0.0f; s.maxFrequency = 4000.0f;
    s.dbMinDb = -40.0f; s.dbMaxDb = 0.0f; s.gain = 1.0f; s.bias = 0.0f;
    SpectralEngine sp;
    sp.setColourMap (ColourMap::Classic);
    const int W = 32, H = 400;   // 10 Hz per row
    std::vector<uint8_t> rgba;
    CHECK (sp.renderToPixels (eng, s, W, H, 0.5, 1.5, { 0 }, rgba), "render tone");

    const auto lut = buildColourLUT (ColourMap::Classic);
    auto lutIndex = [&lut] (const uint8_t* p)
    {
        int best = -1;
        for (int i = 0; i < 256; ++i)
            if (lut[(size_t) i * 4] == p[0] && lut[(size_t) i * 4 + 1] == p[1] && lut[(size_t) i * 4 + 2] == p[2])
                best = i;
        return best;
    };
    int bestIdx = -1;
    double rowSum = 0.0; int rowCount = 0;
    for (int y = 0; y < H; ++y)
    {
        const int idx = lutIndex (&rgba[((size_t) y * W + W / 2) * 4]);
        if (idx > bestIdx) { bestIdx = idx; rowSum = 0.0; rowCount = 0; }
        if (idx == bestIdx) { rowSum += y; ++rowCount; }
    }
    const double yPeak = rowCount > 0 ? rowSum / rowCount : -1.0;
    const double fPeak = (1.0 - (yPeak + 0.5) / H) * 4000.0;
    CHECK (std::abs (fPeak - 3000.0) < 8.0,
           "3 kHz tone drawn on the 3 kHz row, got " + juce::String (fPeak, 1) + " Hz");
}


// integration (persistence#3): a selection made only of subtractive shapes
// selects nothing - no STFT pass, no undo entry, no "unsaved", samples intact.
static void testSdspSubtractiveOnlyIsNoOp()
{
    progress ("regression spectral-dsp (integration): subtractive-only selection is a no-op");
    AudioEngine eng;
    ::UndoManager um;
    eng.setUndoManager (&um);
    std::vector<std::vector<float>> data (1, std::vector<float> (8000));
    for (int i = 0; i < 8000; ++i)
        data[0][(size_t) i] = 0.5f * (float) std::sin (0.3 * i);
    CHECK (eng.initNew (1, 8000.0, &data), "subtractive-only: document");
    const int64_t v0 = eng.contentVersion ();

    SpectralSelection sel;
    sel.addRect (0.1, 0.5, 100.0, 2000.0, /*subtract=*/true);
    CHECK (!sel.empty () && !sel.hasAdditiveShape (), "subtractive-only: list is non-empty but selects nothing");

    std::string err;
    CHECK (SpectralMask::applyMaskedGain (eng, { 0, 8000 }, sel, 0.0f, "mute", nullptr, &err),
           "subtractive-only: applyMaskedGain succeeds");
    CHECK (!um.canUndo (), "subtractive-only: no undo entry recorded");
    CHECK (eng.contentVersion () == v0, "subtractive-only: content unchanged (version)");
    float x = 0.0f;
    CHECK (eng.readSamples (0, 1234, 1, &x) && x == data[0][1234], "subtractive-only: samples untouched");
}

static void runRegression_spectral_dsp()
{
    testSdspScFftAllSizes();
    testSdspSpectrogramCalibration();
    testSdspTileCacheFollowsContent();
    testSdspConcurrentGetTiles();
    testSdspCacheByteBudget();
    testSdspRenderBinMapping();
    testSdspSubtractiveOnlyIsNoOp();
}
