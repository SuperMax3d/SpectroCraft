// Model panel (issue #40): the sound model's analysis, subtraction chain,
// rendering and JSON, on synthetic signals whose levels are known.
// Core: include/SoundModel.h; docs/model-panel.md.

namespace scmodeltest
{
    namespace md = sc::model;

    // The fan's parameter: 0-10 s rises 30 -> 60, 10-20 s holds 60,
    // 20-30 s falls 60 -> 30, 30-40 s holds 30.
    inline double fanSpeed (double t)
    {
        if (t < 10.0) return 30.0 + 3.0 * t;
        if (t < 20.0) return 60.0;
        if (t < 30.0) return 60.0 - 3.0 * (t - 20.0);
        return 30.0;
    }
    inline double rateOf (double t)
    {
        if (t < 10.0) return 3.0;
        if (t >= 20.0 && t < 30.0) return -3.0;
        return 0.0;
    }

    constexpr double kRate = 16000.0;
    constexpr double kLen = 40.0;

    // f0 = 2 x speed; H1 -20 dB at 60 (-0.3 dB per unit below), H2 -26 dB,
    // H3 -30 dB at 60 (-0.2 dB per unit). Optional per-motion offsets for
    // H1 (dynamics test). Noise: sigma 0.01 before 30 s, 0.02 after.
    struct Signal
    {
        std::vector<float> x;
        double h1Accel = 0.0, h1Decel = 0.0;   // dB added to H1 while rising / falling
        bool harmonics = true, noise = true;
    };

    inline double h1Db (double p) { return -20.0 + (p - 60.0) * 0.3; }
    inline double h3Db (double p) { return -30.0 + (p - 60.0) * 0.2; }

    inline void make (Signal& s)
    {
        const int64_t n = (int64_t) (kLen * kRate);
        s.x.assign ((size_t) n, 0.0f);
        juce::Random rng (1234);
        double ph1 = 0.0, ph2 = 0.3, ph3 = 1.1;
        for (int64_t i = 0; i < n; ++i)
        {
            const double t = (double) i / kRate;
            const double p = fanSpeed (t), f0 = 2.0 * p;
            double v = 0.0;
            if (s.harmonics)
            {
                const double r = rateOf (t);
                const double off = r > 0 ? s.h1Accel : r < 0 ? s.h1Decel : 0.0;
                auto amp = [] (double db) { return std::sqrt (2.0) * std::pow (10.0, db / 20.0); };
                v += amp (h1Db (p) + off) * std::sin (ph1);
                v += amp (-26.0) * std::sin (ph2);
                v += amp (h3Db (p)) * std::sin (ph3);
                ph1 += 2.0 * juce::MathConstants<double>::pi * f0 / kRate;
                ph2 += 2.0 * juce::MathConstants<double>::pi * 2.0 * f0 / kRate;
                ph3 += 2.0 * juce::MathConstants<double>::pi * 3.0 * f0 / kRate;
            }
            if (s.noise)
            {
                const double sigma = t < 30.0 ? 0.01 : 0.02;
                const double u1 = std::max (1e-12, (double) rng.nextFloat()), u2 = rng.nextFloat();
                v += sigma * std::sqrt (-2.0 * std::log (u1)) * std::cos (2.0 * juce::MathConstants<double>::pi * u2);
            }
            s.x[(size_t) i] = (float) v;
        }
    }

    inline md::Audio audioOf (const std::vector<float>& x, double rate = kRate)
    {
        md::Audio a;
        a.sampleRate = rate;
        a.numChannels = 1;
        a.numSamples = (int64_t) x.size();
        a.read = [&x] (int, int64_t start, int n, float* out)
        {
            for (int i = 0; i < n; ++i)
            {
                const int64_t s = start + i;
                out[i] = (s >= 0 && s < (int64_t) x.size()) ? x[(size_t) s] : 0.0f;
            }
        };
        return a;
    }

    inline md::ParamFn fanParam()
    {
        return [] (const juce::String& name, double t, bool* found)
        {
            if (found) *found = name == "Fan.Speed";
            return fanSpeed (t);
        };
    }

    inline md::ParamFn constParam (double v)
    {
        return [v] (const juce::String& name, double, bool* found)
        {
            if (found) *found = name == "Fan.Speed";
            return v;
        };
    }

    // Noise PSD of sigma at kRate, in dB re 1/Hz.
    inline double noiseDb (double sigma) { return 10.0 * std::log10 (2.0 * sigma * sigma / kRate); }

    inline double bandDbNear (const md::Model& m, const md::NoiseState& s, double hz)
    {
        size_t best = 0;
        for (size_t i = 1; i < m.bandHz.size(); ++i)
            if (std::abs (m.bandHz[i] - hz) < std::abs (m.bandHz[best] - hz)) best = i;
        return s.psdDb[best];
    }

    // Median band level between 200 Hz and 6 kHz (away from the edges).
    inline double medianBandDb (const md::Model& m, const md::NoiseState& s)
    {
        std::vector<double> v;
        for (size_t i = 0; i < m.bandHz.size(); ++i)
            if (m.bandHz[i] > 200.0f && m.bandHz[i] < 6000.0f) v.push_back (s.psdDb[i]);
        std::sort (v.begin(), v.end());
        return v.empty() ? -999.0 : v[v.size() / 2];
    }

    inline md::Component& fan (md::Model& m)
    {
        auto& c = m.add (md::Kind::Harmonic, "Fan");
        c.parameter = "Fan.Speed";
        c.f0Hz = 120.0;
        c.f0AtParam = 60.0;
        c.ranges = { { 0.0, kLen } };
        return c;
    }
}

static void regrModel_harmonics()
{
    progress ("regression model: harmonic lines follow the parameter (#40)");
    using namespace scmodeltest;
    Signal s;
    make (s);
    const auto audio = audioOf (s.x);

    md::Model m;
    fan (m);
    juce::String err;
    CHECK (md::analyse (m, 0, audio, fanParam(), {}, &err), "model: the fan analyses (" + err + ")");
    const auto& c = m.components[0];
    CHECK (c.analysed && c.lines.size() == 3, "model: three lines learned");
    CHECK (std::abs (c.pLo - 30.0) < 0.5 && std::abs (c.pHi - 60.0) < 0.5, "model: the parameter range seen");
    const auto& h1 = c.lines[0];
    const auto& h2 = c.lines[1];
    const auto& h3 = c.lines[2];
    CHECK (std::abs (h1.levelDb.at (60.0) - (-20.0)) < 1.0, "model: H1 at 60 is -20 dB (" + juce::String (h1.levelDb.at (60.0)) + ")");
    CHECK (std::abs (h1.levelDb.at (30.0) - (-29.0)) < 1.0, "model: H1 at 30 is -29 dB (" + juce::String (h1.levelDb.at (30.0)) + ")");
    CHECK (std::abs (h1.levelDb.at (45.0) - h1Db (45.0)) < 1.0, "model: H1 in between follows the parameter");
    CHECK (std::abs (h2.levelDb.at (40.0) - (-26.0)) < 1.0, "model: H2 is flat at -26 dB");
    CHECK (std::abs (h3.levelDb.at (50.0) - h3Db (50.0)) < 1.0, "model: H3 follows its own slope");
    CHECK (std::abs (h1.tuning - 1.0) < 0.005 && h1.framesBelowNoise == 0, "model: H1 in tune, always above the noise");

    // An unknown parameter is reported, not guessed.
    md::Model bad;
    fan (bad).parameter = "Fan.Nope";
    CHECK (! md::analyse (bad, 0, audio, fanParam(), {}, &err) && err.contains ("Fan.Nope"),
           "model: an unknown parameter is an error");

    // A second copy of the fan, analysed after it, finds nothing left: the
    // component above is taken out first.
    md::Model two;
    fan (two);
    fan (two).name = "Fan again";
    CHECK (md::analyseAll (two, audio, fanParam(), {}, &err), "model: two components analyse");
    CHECK (two.components[1].lines[0].levelDb.at (60.0) < two.components[0].lines[0].levelDb.at (60.0) - 15.0,
           "model: the component above is subtracted from the next one's peaks");

    // A track measured in a stored selection: 100-140 Hz over 10-20 s holds H1 (120 Hz).
    md::Model tr;
    auto& t = tr.add (md::Kind::Harmonic, "Tracked");
    t.parameter = "Fan.Speed";
    t.lines.clear();
    md::HarmonicLine line;
    line.selections = { "Sel A" };
    t.lines.push_back (line);
    md::SelectionFn sel = [] (const juce::String& name, md::SelectionShape& out)
    {
        if (name != "Sel A") return false;
        out.t0 = 10.0; out.t1 = 20.0;
        out.bandAt = [] (double, double& lo, double& hi) { lo = 100.0; hi = 140.0; return true; };
        return true;
    };
    CHECK (md::analyse (tr, 0, audio, fanParam(), sel, &err), "model: a track analyses (" + err + ")");
    CHECK (std::abs (tr.components[0].lines[0].freqHz.at (60.0) - 120.0) < 1.0
               && std::abs (tr.components[0].lines[0].levelDb.at (60.0) - (-20.0)) < 1.0,
           "model: the track finds 120 Hz at -20 dB");
    CHECK (std::abs (tr.components[0].lineFreqAt (tr.components[0].lines[0], 30.0) - 60.0) < 1.0,
           "model: beyond its data a track keeps its frequency / parameter ratio");
    CHECK (! md::analyse (tr, 0, audio, fanParam(), [] (const juce::String&, md::SelectionShape&) { return false; }, &err)
               && err.contains ("Sel A"), "model: a missing stored selection is an error");
}

static void regrModel_dynamics()
{
    progress ("regression model: rising, falling and steady levels (#40)");
    using namespace scmodeltest;
    Signal s;
    s.h1Accel = +3.0;
    s.h1Decel = -4.0;
    make (s);
    const auto audio = audioOf (s.x);
    md::Model m;
    auto& c = fan (m);
    c.dynamics = true;
    juce::String err;
    CHECK (md::analyse (m, 0, audio, fanParam(), {}, &err), "model dynamics: analyses (" + err + ")");
    const auto& h1 = m.components[0].lines[0];
    CHECK (! h1.accelDb.empty() && ! h1.decelDb.empty(), "model dynamics: rising and falling curves learned");
    CHECK (std::abs (h1.levelDb.at (60.0) - (-20.0)) < 1.0, "model dynamics: steady at 60 is -20 dB");
    CHECK (std::abs (h1.accelDb.at (45.0) - (h1Db (45.0) + 3.0)) < 1.2, "model dynamics: rising is 3 dB louder");
    CHECK (std::abs (h1.decelDb.at (45.0) - (h1Db (45.0) - 4.0)) < 1.2, "model dynamics: falling is 4 dB softer");
    // linePower picks the curve from the rate.
    const double thr = m.steadyRatePct / 100.0 * md::span (m.components[0]);
    CHECK (std::abs (10.0 * std::log10 (md::linePower (m.components[0], h1, 45.0, 3.0 * thr, m)) - h1.accelDb.at (45.0)) < 0.01
               && std::abs (10.0 * std::log10 (md::linePower (m.components[0], h1, 45.0, 0.0, m)) - h1.levelDb.at (45.0)) < 0.01,
           "model dynamics: the rate picks the rising or the steady level");
}

static void regrModel_noise()
{
    progress ("regression model: noise states, harmonics notched out, subtraction (#40)");
    using namespace scmodeltest;
    Signal s;
    make (s);
    const auto audio = audioOf (s.x);
    juce::String err;

    // Noise alone (nothing above): the fan's harmonics land in its spectrum.
    md::Model alone;
    auto& n0 = alone.add (md::Kind::Noise, "Hiss");
    n0.parameter = "Fan.Speed";
    n0.ranges = { { 11.0, 19.0 } };
    CHECK (md::analyse (alone, 0, audio, fanParam(), {}, &err), "model noise: analyses alone (" + err + ")");
    CHECK (bandDbNear (alone, alone.components[0].states[0], 240.0) > noiseDb (0.01) + 10.0,
           "model noise: alone, the fan's 240 Hz line raises the hiss spectrum");

    // Fan first, then the hiss: the harmonics are notched out.
    md::Model m;
    fan (m);
    auto& n = m.add (md::Kind::Noise, "Hiss");
    n.parameter = "Fan.Speed";
    n.ranges = { { 11.0, 19.0 }, { 31.0, 39.0 } };
    CHECK (md::analyseAll (m, audio, fanParam(), {}, &err), "model noise: fan then hiss (" + err + ")");
    const auto& hiss = m.components[1];
    CHECK (hiss.states.size() == 2, "model noise: one state per range");
    CHECK (std::abs (hiss.states[0].param - 30.0) < 0.01 && std::abs (hiss.states[1].param - 60.0) < 0.01,
           "model noise: states sorted by their mean parameter");
    CHECK (std::abs (medianBandDb (m, hiss.states[1]) - noiseDb (0.01)) < 1.0,
           "model noise: at 60 the hiss is sigma 0.01 (" + juce::String (medianBandDb (m, hiss.states[1])) + " vs "
               + juce::String (noiseDb (0.01)) + ")");
    CHECK (std::abs (medianBandDb (m, hiss.states[0]) - noiseDb (0.02)) < 1.0, "model noise: at 30 it is sigma 0.02");
    CHECK (std::abs (bandDbNear (m, hiss.states[1], 240.0) - noiseDb (0.01)) < 2.5,
           "model noise: the fan's 240 Hz line is notched out of the hiss");
    const double mid = 10.0 * std::log10 (md::noisePsd (hiss, m, 1000.0, 45.0));
    CHECK (std::abs (mid - 0.5 * (noiseDb (0.01) + noiseDb (0.02))) < 1.0, "model noise: halfway in dB between states");

    // A second hiss under the first: subtracting the first leaves (almost) nothing.
    md::Model sub = m;
    auto& n2 = sub.add (md::Kind::Noise, "Hiss again");
    n2.parameter = "Fan.Speed";
    n2.ranges = { { 11.0, 19.0 } };
    CHECK (md::analyseAll (sub, audio, fanParam(), {}, &err), "model noise: hiss twice (" + err + ")");
    CHECK (medianBandDb (sub, sub.components[2].states[0]) < noiseDb (0.01) - 15.0,
           "model noise: the hiss above is subtracted from the next one (" + juce::String (medianBandDb (sub, sub.components[2].states[0]) - noiseDb (0.01)) + " dB)");
    CHECK (sub.components[2].report.contains ("over-subtraction"), "model noise: the over-subtraction is reported");
}

static void regrModel_renderAndJson()
{
    progress ("regression model: render, re-analyse, JSON round trip (#40)");
    using namespace scmodeltest;
    Signal s;
    make (s);
    const auto audio = audioOf (s.x);
    md::Model m;
    fan (m);
    auto& n = m.add (md::Kind::Noise, "Hiss");
    n.parameter = "Fan.Speed";
    n.ranges = { { 11.0, 19.0 }, { 31.0, 39.0 } };
    juce::String err;
    CHECK (md::analyseAll (m, audio, fanParam(), {}, &err), "model render: analyses (" + err + ")");

    // Render 12 s at a steady 60 and analyse the result with the same definitions.
    std::vector<float> out ((size_t) (12.0 * kRate), 0.0f);
    md::render (m, 0.0, (int64_t) out.size(), kRate, constParam (60.0), out.data(), 7);
    bool finite = true;
    for (float v : out) finite = finite && std::isfinite (v);
    CHECK (finite, "model render: finite samples");
    md::Model again = m;
    for (auto& c : again.components) c.ranges = { { 1.0, 11.0 } };
    CHECK (md::analyseAll (again, audioOf (out), constParam (60.0), {}, &err), "model render: re-analyses (" + err + ")");
    CHECK (std::abs (again.components[0].lines[0].levelDb.at (60.0) - (-20.0)) < 1.0
               && std::abs (again.components[0].lines[2].levelDb.at (60.0) - (-30.0)) < 1.0,
           "model render: the rendered lines measure as learned");
    CHECK (std::abs (medianBandDb (again, again.components[1].states[0]) - noiseDb (0.01)) < 1.0,
           "model render: the rendered hiss measures as learned");

    // Below the learned range the fan fades out: silent at 0.
    std::vector<float> off ((size_t) kRate, 0.0f);
    md::render (m, 0.0, (int64_t) off.size(), kRate, constParam (0.0), off.data(), 7);
    double e = 0.0;
    for (float v : off) e += (double) v * v;
    CHECK (e == 0.0, "model render: parameter at 0 (fan off) renders silence");
    CHECK (m.components[0].rangeGain (30.0) == 1.0 && m.components[0].rangeGain (26.0) < 1.0
               && m.components[0].rangeGain (25.0) == 0.0, "model render: fade between 5 % and 15 % below the range");

    // Only one component; the same seed renders the same samples.
    std::vector<float> a ((size_t) kRate, 0.0f), b ((size_t) kRate, 0.0f), h ((size_t) kRate, 0.0f);
    md::render (m, 3.0, (int64_t) a.size(), kRate, fanParam(), a.data(), 9);
    md::render (m, 3.0, (int64_t) b.size(), kRate, fanParam(), b.data(), 9);
    md::render (m, 3.0, (int64_t) h.size(), kRate, fanParam(), h.data(), 9, 0);
    CHECK (a == b, "model render: a seed repeats");
    CHECK (a != h, "model render: one component alone differs from the whole model");

    // JSON: the model comes back and renders the same samples.
    const auto json = m.toJson();
    md::Model back;
    CHECK (md::Model::fromJson (json, back, &err), "model JSON: parses back (" + err + ")");
    std::vector<float> c2 ((size_t) kRate, 0.0f);
    md::render (back, 3.0, (int64_t) c2.size(), kRate, fanParam(), c2.data(), 9);
    double maxDiff = 0.0;
    for (size_t i = 0; i < a.size(); ++i) maxDiff = std::max (maxDiff, (double) std::abs (a[i] - c2[i]));
    CHECK (maxDiff < 2e-3, "model JSON: renders the same after the round trip (" + juce::String (maxDiff) + ")");
    CHECK (back.components.size() == 2 && back.components[1].states.size() == 2
               && back.components[0].f0Hz == 120.0 && back.components[0].parameter == "Fan.Speed",
           "model JSON: components, states and settings come back");
    CHECK (! md::Model::fromJson ("{\"format\":\"something else\"}", back, &err), "model JSON: other files are refused");
    CHECK (! md::Model::fromJson ("{\"format\":\"SpectroCraft sound model\",\"version\":99}", back, &err),
           "model JSON: a newer format is refused");

    // Variation: still about the same level.
    md::Model var = m;
    var.components[0].variation = true;
    var.components[1].enabled = false;
    std::vector<float> v ((size_t) (8.0 * kRate), 0.0f);
    md::render (var, 0.0, (int64_t) v.size(), kRate, constParam (60.0), v.data(), 3);
    md::Model chk = var;
    chk.components[0].ranges = { { 0.5, 7.5 } };
    CHECK (md::analyse (chk, 0, audioOf (v), constParam (60.0), {}, &err)
               && std::abs (chk.components[0].lines[0].levelDb.at (60.0) - (-20.0)) < 1.5,
           "model render: with variation the level stays about the same");
}

// The panel and the application: a file with a Tables parameter, components
// made in the panel, Analyse All, Compare (markers and parameters follow),
// Render Selection (a new channel, undoable), Render CSV, the model saved
// with the file, Export / Import, and the spectrogram picking.
static void regrModel_app()
{
    progress ("regression model: panel, compare, render selection / CSV, picking, persistence (#40)");
    using namespace scmodeltest;
    Signal s;
    make (s);
    const juce::File wav = makeTempFile ("model_fan.wav");
    wav.deleteFile();
    sc::fileMetaSidecarPath (wav).deleteFile();
    writeWav (wav, 1, kRate, (int64_t) s.x.size(), [&s] (int, int64_t f, double) { return s.x[(size_t) f]; });

    Application app;
    CHECK (app.openFile (wav), "model app: open the fan file");
    juce::String csv = "time,Speed\n";
    for (int i = 0; i <= 400; ++i) csv << juce::String (i * 0.1, 1) << "," << juce::String (fanSpeed (i * 0.1), 3) << "\n";
    std::string e;
    CHECK (app.addTableFromCsv ("Fan", csv, &e), "model app: the speed table imports");
    CHECK (app.modelParameterNames().contains ("Fan.Speed"), "model app: the parameter is offered as Fan.Speed");
    const double p15 = app.modelParamFn (*app.doc()) ("Fan.Speed", 15.0, nullptr);
    CHECK (std::abs (p15 - 60.0) < 0.01, "model app: the parameter at 15 s is 60");
    app.markers().addPointMarker (33.0, "late");

    ModelPanel panel (app);
    panel.setSize (380, 760);
    std::function<juce::Component* (juce::Component&, const juce::String&)> byId;
    byId = [&byId] (juce::Component& root, const juce::String& id) -> juce::Component*
    {
        if (root.getComponentID() == id) return &root;
        for (auto* c : root.getChildren())
            if (auto* f = byId (*c, id)) return f;
        return nullptr;
    };

    app.setSelection ({ 5.0, 25.0 });
    panel.addComponent (md::Kind::Harmonic);
    auto* m = app.soundModel();
    CHECK (m != nullptr && m->components.size() == 1, "model app: a harmonic component is added");
    if (m == nullptr || m->components.empty()) { wav.deleteFile(); return; }
    CHECK (m->components[0].parameter == "Fan.Speed" && m->components[0].ranges.size() == 1
               && std::abs (m->components[0].ranges[0].t0 - 5.0) < 1e-9,
           "model app: it starts on the first parameter and the selection");
    auto* f0 = dynamic_cast<juce::TextEditor*> (byId (panel, "model-f0"));
    auto* f0At = dynamic_cast<juce::TextEditor*> (byId (panel, "model-f0-at"));
    auto* orders = dynamic_cast<juce::TextEditor*> (byId (panel, "model-orders"));
    CHECK (f0 != nullptr && f0At != nullptr && orders != nullptr && f0->isVisible(), "model app: the harmonic fields show");
    if (f0 == nullptr || f0At == nullptr || orders == nullptr) { wav.deleteFile(); return; }
    f0->setText ("120");
    f0At->setText ("60");
    orders->setText ("1, 2, 3");
    panel.commitEdits();
    CHECK (m->components[0].f0Hz == 120.0 && m->components[0].f0AtParam == 60.0 && m->components[0].lines.size() == 3,
           "model app: the fundamental and the orders are applied");

    app.setSelection ({ 11.0, 19.0 });
    panel.addComponent (md::Kind::Noise);
    app.setSelection ({ 31.0, 39.0 });
    if (auto* add = dynamic_cast<juce::Button*> (byId (panel, "model-add-range"))) add->onClick();
    CHECK (m->components.size() == 2 && m->components[1].ranges.size() == 2, "model app: a noise with two ranges");
    CHECK (byId (panel, "model-f0") != nullptr && ! byId (panel, "model-f0")->isVisible(),
           "model app: the harmonic fields hide for a noise component");

    panel.analyseAll();
    CHECK (panel.statusText().contains ("analysed"), "model app: Analyse All (" + panel.statusText() + ")");
    CHECK (m->components[0].analysed && m->components[1].analysed, "model app: both components analysed");
    CHECK (std::abs (m->components[0].lines[0].levelDb.at (60.0) - (-20.0)) < 1.0, "model app: H1 learned from the file");
    CHECK (app.doc()->metaDirty, "model app: the model marks the file's metadata dirty");

    // Picking.
    const juce::String hid = m->components[0].id;
    app.startModelPick (Application::ModelPick::harmonics, hid);
    CHECK (app.modelPickOrderAt (15.0, 245.0) == 2 && app.modelPickOrderAt (15.0, 170.0) == 1,
           "model app: the nearest harmonic order at the mouse");
    CHECK (app.modelPickClick (15.0, 365.0) && m->components[0].lines.size() == 2, "model app: a click removes H3");
    CHECK (app.modelPickClick (15.0, 605.0) && m->components[0].lines.back().order == 5.0, "model app: a click adds H5");
    app.cancelModelPick();
    CHECK (app.modelPick == Application::ModelPick::none, "model app: picking stops");
    app.startModelPick (Application::ModelPick::fundamental, hid);
    CHECK (app.modelPickClick (15.0, 116.0) && std::abs (m->components[0].f0Hz - 120.0) < 0.5
               && std::abs (m->components[0].f0AtParam - 60.0) < 0.01 && app.modelPick == Application::ModelPick::none,
           "model app: the fundamental snaps to the 120 Hz peak at speed 60");
    panel.selectIndex (0);   // the harmonic component (the noise was selected last)
    orders->setText ("1, 2, 3");
    panel.commitEdits();
    CHECK (m->components[0].lines.size() == 3 && m->components[0].lines[2].order == 3.0,
           "model app: the orders typed apply to the selected component");
    panel.analyseAll();

    // Persistence with the file's metadata.
    {
        sc::MetaBundle b;
        CHECK (sc::parseBundle (juce::String (app.doc()->buildMetaJson()), b) && b.haveSoundModel,
               "model app: the model is in the file's metadata");
        FileDoc other;
        other.applyBundle (b);
        CHECK (other.soundModel.components.size() == 2 && other.soundModel.components[0].analysed
                   && other.soundModel.components[1].states.size() == 2,
               "model app: the metadata brings the model back");
    }

    // Export / Import.
    const juce::File json = makeTempFile ("model_fan.json");
    juce::String err;
    CHECK (app.exportModel (json, &err) && json.existsAsFile(), "model app: Export writes JSON");
    m->components.clear();
    CHECK (app.importModel (json, &err) && app.soundModel()->components.size() == 2, "model app: Import reads it back");
    json.deleteFile();

    // Render Selection: a new channel, silent outside the selection, undoable.
    app.setSelection ({ 2.0, 4.0 });
    panel.renderSelection();
    auto& eng = app.audioEngine();
    CHECK (eng.numChannels() == 2, "model app: Render Selection adds a channel (" + panel.statusText() + ")");
    if (eng.numChannels() == 2)
    {
        std::vector<float> in ((size_t) kRate), out ((size_t) kRate);
        eng.readSamples (1, (int64_t) (2.5 * kRate), (int64_t) in.size(), in.data());
        eng.readSamples (1, (int64_t) (6.0 * kRate), (int64_t) out.size(), out.data());
        double ein = 0.0, eout = 0.0;
        for (float v : in) ein += (double) v * v;
        for (float v : out) eout += (double) v * v;
        CHECK (ein > 1.0 && eout == 0.0, "model app: the render fills the selection only");
        app.undoStep();
        CHECK (eng.numChannels() == 1, "model app: one undo removes the rendered channel");
    }

    // Compare: ranges 5-25 and 31-39 back to back, simulation on channel 1.
    const int docsBefore = (int) app.docs.size();
    panel.compare();
    CHECK ((int) app.docs.size() == docsBefore + 1, "model app: Compare opens a new file (" + panel.statusText() + ")");
    if ((int) app.docs.size() == docsBefore + 1)
    {
        auto* cd = app.doc();
        CHECK (cd->engine.numChannels() == 2 && std::abs (cd->engine.duration() - 28.0) < 0.01,
               "model app: two channels, 20 s + 8 s");
        std::vector<float> a ((size_t) kRate), b ((size_t) kRate), o ((size_t) kRate);
        cd->engine.readSamples (1, (int64_t) (10.0 * kRate), (int64_t) a.size(), a.data());
        app.docs[(size_t) docsBefore - 1]->engine.readSamples (0, (int64_t) (15.0 * kRate), (int64_t) o.size(), o.data());
        CHECK (a == o, "model app: channel 2 is the original (10 s in = 15 s of the file)");
        cd->engine.readSamples (0, (int64_t) (10.0 * kRate), (int64_t) b.size(), b.data());
        double ea = 0.0, eb = 0.0;
        for (size_t i = 0; i < a.size(); ++i) { ea += (double) a[i] * a[i]; eb += (double) b[i] * b[i]; }
        CHECK (std::abs (10.0 * std::log10 (eb / ea)) < 1.0, "model app: the simulation is as loud as the original");
        bool found = false;
        for (const auto& mk : cd->markers.markers())
            if (mk.name == "late") found = std::abs (mk.start - 22.0) < 0.01;
        CHECK (found, "model app: the marker at 33 s follows to 22 s");
        CHECK (std::abs (app.modelParamFn (*cd) ("Fan.Speed", 22.0, nullptr) - 30.0) < 0.05
                   && std::abs (app.modelParamFn (*cd) ("Fan.Speed", 1.0, nullptr) - 48.0) < 0.05,
               "model app: the parameter follows the cut ranges");
        app.activate (docsBefore - 1);
    }

    // Render CSV: a steady 60 for 3 s, into a new file with that table.
    const juce::File csvFile = makeTempFile ("model_drive.csv");
    csvFile.replaceWithText ("time,Speed\n0,60\n1.5,60\n3,60\n");
    const int before = (int) app.docs.size();
    CHECK (app.renderModelCsv (csvFile, &err), "model app: Render CSV (" + err + ")");
    if ((int) app.docs.size() == before + 1)
    {
        auto* rd = app.doc();
        CHECK (std::abs (rd->engine.duration() - 3.0) < 0.01 && rd->engine.numChannels() == 1 && rd->tables.size() == 1,
               "model app: a 3 s mono file holding the CSV as its table");
        std::vector<float> x ((size_t) (2.0 * kRate));
        rd->engine.readSamples (0, (int64_t) (0.5 * kRate), (int64_t) x.size(), x.data());
        double e2 = 0.0;
        for (float v : x) e2 += (double) v * v;
        const double rmsDb = 10.0 * std::log10 (e2 / x.size());
        const double want = 10.0 * std::log10 (0.01 + std::pow (10.0, -2.6) + 0.001 + 1e-4);
        CHECK (std::abs (rmsDb - want) < 1.0, "model app: the CSV render has the fan's level at 60 ("
                                                  + juce::String (rmsDb) + " vs " + juce::String (want) + ")");
        app.activate (0);
    }
    csvFile.replaceWithText ("time,Other\n0,1\n3,1\n");
    CHECK (! app.renderModelCsv (csvFile, &err) && err.contains ("Fan.Speed"),
           "model app: a CSV without the parameter is refused");
    csvFile.deleteFile();

    // Order: move, delete.
    panel.selectIndex (1);
    panel.moveSelected (-1);
    CHECK (app.soundModel()->components[0].kind == md::Kind::Noise && panel.selectedIndex() == 0,
           "model app: Move Up puts the noise first");
    panel.deleteSelected();
    CHECK (app.soundModel()->components.size() == 1 && app.soundModel()->components[0].isHarmonic(),
           "model app: Delete removes it");

    wav.deleteFile();
    sc::fileMetaSidecarPath (wav).deleteFile();
}

static void runRegression_model()
{
    regrModel_app();
    regrModel_harmonics();
    regrModel_dynamics();
    regrModel_noise();
    regrModel_renderAndJson();
}
