// Blind source separation (issue #68): STFT round trip, block order
// matching, and each method on synthetic convolutive mixtures, scored by the
// gain in signal-to-distortion ratio (best permutation) over the mixture.

namespace bsstest
{
// Three independent, non-stationary sources at 16 kHz.
static std::vector<std::vector<float>> sources (int n, double rate, double seconds, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> g (0.0, 1.0);
    std::uniform_real_distribution<double> u (0.0, 1.0);
    const int64_t L = (int64_t) (rate * seconds);
    std::vector<std::vector<float>> s ((size_t) n, std::vector<float> ((size_t) L));
    // 0: "speech": low-passed noise in random bursts.
    {
        double lp = 0.0, env = 0.0, target = 0.0;
        for (int64_t i = 0; i < L; ++i)
        {
            if (i % (int64_t) (rate * 0.12) == 0) target = u (rng) < 0.55 ? 0.3 + u (rng) : 0.0;
            env += (target - env) * 0.004;
            lp += 0.25 * (g (rng) - lp);
            s[0][(size_t) i] = (float) (lp * env);
        }
    }
    // 1: "motor": harmonics of 120 Hz with a slow beat.
    if (n > 1)
        for (int64_t i = 0; i < L; ++i)
        {
            const double t = (double) i / rate;
            double v = 0.0;
            for (int k = 1; k <= 24; ++k)
                v += std::sin (2.0 * juce::MathConstants<double>::pi * 120.0 * k * t + 0.3 * k) / k;
            s[1][(size_t) i] = (float) (0.35 * v * (0.6 + 0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 0.7 * t))
                                        + 0.02 * g (rng));
        }
    // 2: "airflow": high-passed noise, slowly swelling.
    if (n > 2)
    {
        double prev = 0.0;
        for (int64_t i = 0; i < L; ++i)
        {
            const double t = (double) i / rate;
            const double w = g (rng);
            s[2][(size_t) i] = (float) (0.5 * (w - prev) * (0.5 + 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 0.3 * t)));
            prev = w;
        }
    }
    return s;
}

// Convolutive mixture: a direct path and a few short echoes per pair.
static std::vector<std::vector<float>> mix (const std::vector<std::vector<float>>& s, int mics, unsigned seed,
                                            std::vector<std::vector<float>>& refImages)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> u (-1.0, 1.0);
    const size_t L = s[0].size();
    std::vector<std::vector<float>> x ((size_t) mics, std::vector<float> (L, 0.0f));
    refImages.assign (s.size(), std::vector<float> (L, 0.0f));
    for (int m = 0; m < mics; ++m)
        for (size_t i = 0; i < s.size(); ++i)
        {
            std::vector<double> h (12, 0.0);
            h[(size_t) ((m + 2 * (int) i) % 5)] = 0.6 + 0.4 * std::abs (u (rng));   // a direct path per pair
            for (int e = 0; e < 3; ++e)
                h[(size_t) (5 + (std::abs ((int) (u (rng) * 6))) % 7)] += 0.25 * u (rng);
            for (size_t k = 0; k < L; ++k)
            {
                double acc = 0.0;
                for (size_t d = 0; d < h.size() && d <= k; ++d)
                    acc += h[d] * s[i][k - d];
                x[(size_t) m][k] += (float) acc;
                if (m == 0) refImages[i][k] = (float) acc;
            }
        }
    return x;
}

static double sdr (const std::vector<float>& est, const std::vector<float>& ref)
{
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (size_t k = 0; k < std::min (est.size(), ref.size()); ++k)
    {
        sab += (double) est[k] * ref[k];
        saa += (double) est[k] * est[k];
        sbb += (double) ref[k] * ref[k];
    }
    const double rho2 = (sab * sab) / std::max (saa * sbb, 1e-30);
    return 10.0 * std::log10 (std::max (rho2, 1e-12) / std::max (1.0 - rho2, 1e-12));
}

// Mean SDR gain over the mixture at the reference mic, best permutation.
static double gain (const std::vector<std::vector<float>>& out, const std::vector<std::vector<float>>& images,
                    const std::vector<float>& mixture)
{
    const int N = (int) images.size();
    std::vector<int> perm ((size_t) N);
    std::iota (perm.begin(), perm.end(), 0);
    double best = -1e9;
    do
    {
        double s = 0.0;
        for (int i = 0; i < N; ++i) s += sdr (out[(size_t) perm[(size_t) i]], images[(size_t) i]);
        best = std::max (best, s / N);
    } while (std::next_permutation (perm.begin(), perm.end()));
    double base = 0.0;
    for (int i = 0; i < N; ++i) base += sdr (mixture, images[(size_t) i]);
    return best - base / N;
}
} // namespace bsstest

static void testBlindSeparation()
{
    namespace bss = sc::bss;
    using namespace bsstest;
    const double rate = 16000.0;

    // STFT round trip.
    {
        std::mt19937 rng (3);
        std::normal_distribution<double> g (0.0, 1.0);
        std::vector<float> x (20000);
        for (auto& v : x) v = (float) g (rng);
        const auto y = bss::stftRoundTrip (x, 1024);
        double err = 0.0, pw = 0.0;
        for (size_t i = 0; i < x.size(); ++i) { err += (y[i] - x[i]) * (double) (y[i] - x[i]); pw += (double) x[i] * x[i]; }
        CHECK (y.size() == x.size() && err / pw < 1e-8, "bss #68: STFT round trip (" + juce::String (err / pw) + ")");
    }
    // Block order.
    {
        std::vector<std::vector<float>> prev { { 1, 0, -1, 0, 2 }, { 0, 3, 0, -3, 0 } };
        std::vector<std::vector<float>> cur { { 0, 1.5f, 0, -1.5f, 0.1f }, { -1, 0, 1, 0, -2 } };
        const auto perm = bss::matchOrder (prev, cur);
        CHECK (perm.size() == 2 && perm[0] == 1 && perm[1] == 0, "bss #68: the next block keeps the order (sign ignored)");
    }
    // Invalid input.
    {
        std::vector<std::vector<float>> out;
        std::string err;
        CHECK (! bss::separate ({ std::vector<float> (50000) }, rate, {}, out, &err) && err.find ("two channels") != std::string::npos,
               "bss #68: one channel is refused");
        bss::Params p;
        p.sources = 5;
        CHECK (bss::sourcesFor (p, 3) == 3 && bss::sourcesFor ({}, 4) == 4, "bss #68: sources clamped to the channels");
    }

    // Two sources, two mics: every method.
    const auto s2 = sources (2, rate, 4.0, 11);
    std::vector<std::vector<float>> img2;
    const auto x2 = mix (s2, 2, 21, img2);
    struct Case { bss::Method m; int iters; double minGain; };
    const Case cases[] = {
        { bss::Method::auxIva,    40, 12.0 },   // +26 dB measured (macOS)
        { bss::Method::ilrma,     40, 12.0 },   // +28 dB
        { bss::Method::cacgmm,    30, 8.0 },    // +19 dB
        { bss::Method::fastMnmf2, 60, 8.0 },    // +20 dB
    };
    for (const auto& c : cases)
    {
        bss::Params p;
        p.method = c.m;
        p.fftSize = 1024;
        p.iterations = c.iters;
        p.nmfBases = 6;
        std::vector<std::vector<float>> out;
        std::string err;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const bool ok = bss::separate (x2, rate, p, out, &err);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        const double gdb = ok && out.size() == 2 ? gain (out, img2, x2[0]) : -99.0;
        CHECK (ok && out.size() == 2 && out[0].size() == x2[0].size() && gdb >= c.minGain,
               juce::String ("bss #68: ") + bss::infoOf (c.m).name + " separates 2 sources from 2 mics: +"
                   + juce::String (gdb, 1) + " dB (" + juce::String (ms / 1000.0, 2) + " s)" + (ok ? "" : " " + juce::String (err)));
    }

    // Long input in blocks: the order holds across them.
    {
        const auto s = sources (2, rate, 8.0, 5);
        std::vector<std::vector<float>> img;
        const auto x = mix (s, 2, 7, img);
        bss::Params p;
        p.fftSize = 1024;
        p.iterations = 30;
        p.blockSeconds = 3.0;
        p.overlapSeconds = 1.0;
        std::vector<std::vector<float>> out;
        const bool ok = bss::separate (x, rate, p, out);
        const double gdb = ok ? gain (out, img, x[0]) : -99.0;
        CHECK (ok && gdb >= 12.0, "bss #68: AuxIVA in 3 s blocks keeps the sources apart: +" + juce::String (gdb, 1) + " dB");
    }

    // Three mics, two sources: the reduced (PCA) path.
    {
        std::vector<std::vector<float>> img;
        const auto x = mix (s2, 3, 31, img);
        for (auto m : { bss::Method::auxIva, bss::Method::cacgmm })
        {
            bss::Params p;
            p.method = m;
            p.sources = 2;
            p.fftSize = 1024;
            p.iterations = 40;
            std::vector<std::vector<float>> out;
            const bool ok = bss::separate (x, rate, p, out);
            const double gdb = ok && out.size() == 2 ? gain (out, img, x[0]) : -99.0;
            CHECK (ok && out.size() == 2 && gdb >= (m == bss::Method::auxIva ? 12.0 : 5.0),   // +24 / +11 dB
                   juce::String ("bss #68: ") + bss::infoOf (m).name + ", 2 sources from 3 mics: +" + juce::String (gdb, 1) + " dB");
        }
    }

    // Three sources, three mics (ILRMA).
    {
        const auto s3 = sources (3, rate, 4.0, 17);
        std::vector<std::vector<float>> img;
        const auto x = mix (s3, 3, 41, img);
        bss::Params p;
        p.method = bss::Method::ilrma;
        p.fftSize = 1024;
        p.iterations = 50;
        std::vector<std::vector<float>> out;
        const bool ok = bss::separate (x, rate, p, out);
        const double gdb = ok && out.size() == 3 ? gain (out, img, x[0]) : -99.0;
        CHECK (ok && out.size() == 3 && gdb >= 12.0, "bss #68: ILRMA, 3 sources from 3 mics: +" + juce::String (gdb, 1) + " dB");
    }

    // Cancel.
    {
        bss::Params p;
        p.fftSize = 1024;
        int calls = 0;
        std::vector<std::vector<float>> out;
        std::string err;
        CHECK (! bss::separate (x2, rate, p, out, &err, [&calls] (double) { return ++calls < 3; })
                   && err == "cancelled" && out.empty(),
               "bss #68: cancel stops it");
    }
    // Same result on one thread and on many.
    {
        bss::Params p;
        p.fftSize = 1024;
        p.iterations = 10;
        std::vector<std::vector<float>> a, b;
        {
            const sc::par::ScopedWorkers one (1);
            bss::separate (x2, rate, p, a);
        }
        bss::separate (x2, rate, p, b);
        double d = 0.0;
        for (size_t n = 0; n < a.size() && n < b.size(); ++n)
            for (size_t k = 0; k < a[n].size(); ++k)
                d = std::max (d, (double) std::abs (a[n][k] - b[n][k]));
        CHECK (a.size() == 2 && b.size() == 2 && d < 1e-4, "bss #68: same result with 1 thread or many (" + juce::String (d) + ")");
    }
}

// The effect: Effects menu (multi-channel only), the window's rows per
// method, and a run that creates a new file with the sources.
static void testBlindSeparationWindow()
{
    using namespace bsstest;
    const double rate = 16000.0;
    std::vector<std::vector<float>> img;
    const auto x = mix (sources (2, rate, 3.0, 9), 2, 19, img);
    const juce::File wav = makeTempFile ("bss68.wav");
    writeWav (wav, 2, rate, (int64_t) x[0].size(), [&x] (int ch, int64_t i, double) { return x[(size_t) ch][(size_t) i]; });
    const juce::File mono = makeTempFile ("bss68_mono.wav");
    writeWav (mono, 1, rate, 16000, [] (int, int64_t, double) { return 0.0f; });
    const juce::File store = makeTempFile ("bss68.recent");
    store.deleteFile ();
    {
        Application app (store);
        CHECK (app.openFile (mono), "bss #68: open mono");
        std::unique_ptr<MainWindow> win (new MainWindow (app, "bss 68"));
        auto enabledInEffects = [&win]
        {
            const int idx = win->getMenuBarNames ().indexOf ("Effects");
            const auto m = win->getMenuForIndex (idx, "Effects");
            for (juce::PopupMenu::MenuItemIterator it (m, true); it.next ();)
                if (it.getItem ().itemID == MainWindow::ToolsBlindSeparation)
                    return it.getItem ().isEnabled ? 1 : 0;
            return -1;
        };
        CHECK (enabledInEffects () == 0, "bss #68: Effects > Blind Source Separation is off for a mono file");
        CHECK (app.openFile (wav), "bss #68: open stereo");
        CHECK (enabledInEffects () == 1, "bss #68: and on for a multi-channel one");

        win->showBlindSeparationWindow ();
        auto* w = win->blindSeparationWindow ();
        CHECK (w != nullptr, "bss #68: the window opens");
        if (w == nullptr) { win.reset (); return; }
        auto& v = w->view ();
        v.setMethod (sc::bss::Method::ilrma);
        CHECK (v.isRowShown ("bases") && ! v.isRowShown ("noise") && v.descriptionText ().contains ("Pros:")
                   && v.descriptionText ().contains ("Cons:"),
               "bss #68: ILRMA shows its NMF bases, and the pros and cons");
        v.setMethod (sc::bss::Method::cacgmm);
        CHECK (v.isRowShown ("noise") && v.isRowShown ("extract") && ! v.isRowShown ("bases"),
               "bss #68: the cluster method shows its own settings");
        v.setMethod (sc::bss::Method::auxIva);
        const auto p = v.params ();
        CHECK (p.method == sc::bss::Method::auxIva && p.sources == 2 && p.fftSize == 2048 && p.iterations == 50
                   && v.outputMode () == 1 && v.rangeText ().contains ("Whole file") && v.rangeText ().contains ("2 channels"),
               "bss #68: the defaults (2 sources, 2048 at 16 kHz, 50 iterations, a new file): " + v.rangeText ());

        const int docs = app.numDocs ();
        w->start ();
        CHECK (scPumpUntil ([&win] { return win->blindSeparationWindow () == nullptr; }, 60000),
               "bss #68: the separation finishes and the window closes");
        CHECK (app.numDocs () == docs + 1 && app.doc () != nullptr && app.doc ()->engine.numChannels () == 2,
               "bss #68: a new file with the 2 sources");
        if (app.doc () != nullptr && app.doc ()->engine.numChannels () == 2)
        {
            std::vector<std::vector<float>> out;
            app.doc ()->engine.readRange (0, app.doc ()->engine.numSamples (), out);
            const double g = gain (out, img, x[0]);
            CHECK (g >= 10.0, "bss #68: and they are separated: +" + juce::String (g, 1) + " dB");
        }
        CHECK (app.recentStore->getValue ("bss_params").contains ("\"method\""), "bss #68: the settings are remembered");
        win.reset ();
    }
    store.deleteFile ();
    wav.deleteFile ();
    mono.deleteFile ();
}

// Timings of each method on a long multi-channel recording (not a test:
// SC_BENCH=1 SC_TEST_FILTER=bench; SC_BENCH_BSS_SECONDS sets the length,
// default 60 s; 4 channels, 4 sources, 48 kHz, the window's defaults).
static void testBlindSeparationBench()
{
    if (juce::SystemStats::getEnvironmentVariable ("SC_BENCH", {}) != "1")
        return;
    using namespace bsstest;
    const double rate = 48000.0;
    const double seconds = juce::SystemStats::getEnvironmentVariable ("SC_BENCH_BSS_SECONDS", "60").getDoubleValue();
    auto s = sources (3, rate, seconds, 23);
    s.push_back (sources (1, rate, seconds, 29)[0]);   // a fourth source
    std::vector<std::vector<float>> img;
    const auto x = mix (s, 4, 37, img);
    std::printf ("[bench] blind separation: %.0f s, 4 channels, 4 sources, 48 kHz, %d cores\n", seconds,
                 juce::SystemStats::getNumCpus());
    for (const auto& info : sc::bss::methods())
    {
        sc::bss::Params p;
        p.method = info.method;
        p.fftSize = 4096;
        p.iterations = info.method == sc::bss::Method::auxIva ? 50 : info.method == sc::bss::Method::cacgmm ? 40 : 100;
        p.nmfBases = info.method == sc::bss::Method::fastMnmf2 ? 16 : 8;
        std::vector<std::vector<float>> out;
        const double rss0 = benchRssMb();
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const bool ok = sc::bss::separate (x, rate, p, out);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf ("[bench] %-18s %3d it  %8.1f s  (%.2fx real time)  RSS %+.0f MB  gain %+.1f dB%s\n", info.name,
                     p.iterations, ms / 1000.0, ms / 1000.0 / seconds, benchRssMb() - rss0,
                     ok ? gain (out, img, x[0]) : -99.0, ok ? "" : "  FAILED");
        std::fflush (stdout);
    }
}
