// SpectroCraft - AI control: measurements (measure.*), spectrogram export,
// marker CSV (RAPPORT §6.6, lot 4). Thin wrappers over sc::measures (the
// module the FFT panel itself uses, so the numbers are the screen's) and
// SpectralEngine. Long measurements run as exclusive jobs: the document they
// read can be neither edited nor closed meanwhile.
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "control/PathPolicy.h"
#include "control/StateJson.h"
#include "Application.h"
#include "Measures.h"
#include "SpectralEngine.h"

#include <fstream>

namespace sc::control {

namespace ms = sc::measures;

namespace {

Outcome invalid (const juce::String& path, const juce::String& expected, const juce::String& msg)
{
    return Outcome::failure (err::invalidParams, msg, obj ({ { "path", path }, { "expected", expected } }));
}

/** Inline result size limit (values); beyond it data goes to a file. */
constexpr juce::int64 kInlineValues = 1000000;
/** Work above which a measurement runs as a job (sample-channels read). */
constexpr double kJobSampleChannels = 20.0e6;

struct Region { double t0 = 0, t1 = 0; };

bool regionOf (Context& ctx, Region& r, Outcome& bad, bool allowPoint = false)
{
    const double dur = ctx.doc->engine.duration();
    r.t0 = std::clamp (ctx.num ("start", 0.0), 0.0, dur);
    r.t1 = ctx.has ("end") ? std::clamp (ctx.num ("end", dur), 0.0, dur) : dur;
    if (r.t1 < r.t0 || (!allowPoint && r.t1 == r.t0 && dur > 0))
    {
        bad = invalid ("end", "> start", "end must be greater than start");
        return false;
    }
    return true;
}

bool channelsOf (Context& ctx, const char* key, std::vector<int>& out, Outcome& bad, bool single = false)
{
    const int n = ctx.doc->engine.numChannels();
    out.clear();
    if (single)
    {
        const int c = ctx.integer (key, 0);
        if (c < 0 || c >= n) { bad = invalid (key, "0.." + juce::String (n - 1), "channel does not exist"); return false; }
        out.push_back (c);
        return true;
    }
    const auto v = ctx.get (key);
    if (!v.isArray()) { for (int c = 0; c < n; ++c) out.push_back (c); return true; }
    for (int i = 0; i < v.size(); ++i)
    {
        const int c = (int) v[i];
        if (c < 0 || c >= n)
        {
            bad = invalid (juce::String (key) + "[" + juce::String (i) + "]", "0.." + juce::String (n - 1), "channel does not exist");
            return false;
        }
        if (std::find (out.begin(), out.end(), c) == out.end()) out.push_back (c);
    }
    if (out.empty()) { bad = invalid (key, "non-empty array", "no channel given"); return false; }
    return true;
}

bool fftSizeOf (Context& ctx, int def, int& n, Outcome& bad)
{
    n = ctx.integer ("fftSize", def);
    if (n < 2 || n > 65536 || (n & (n - 1)) != 0)
    {
        bad = invalid ("fftSize", "power of two in 2..65536", "fftSize must be a power of two in 2..65536");
        return false;
    }
    return true;
}

bool windowOf (Context& ctx, int& w, Outcome& bad)
{
    w = ms::windowTypeFromName (ctx.str ("window", "hann"));
    if (w < 0) { bad = invalid ("window", "hann, hamming or rect", "unknown window"); return false; }
    return true;
}

juce::var dbArray (const std::vector<double>& v)
{
    juce::Array<juce::var> a;
    a.ensureStorageAllocated ((int) v.size());
    for (double x : v) a.add (std::round (x * 1000.0) / 1000.0);
    return a;
}

juce::var dbArrayF (const std::vector<float>& v, double offset = 0.0)
{
    juce::Array<juce::var> a;
    a.ensureStorageAllocated ((int) v.size());
    for (float x : v) a.add (std::round (((double) x + offset) * 1000.0) / 1000.0);
    return a;
}

juce::var calibrationVar (const ms::ChannelCalibration& c)
{
    return obj ({ { "calibrated", c.calibrated }, { "fullScale", c.fullScale }, { "levelOffsetDb", c.levelOffsetDb } });
}

/** Runs fn inline (small) or as an exclusive job (large). fn receives the
    progress callback (relays progress + cancel) and must not touch the app:
    it gets an engine reference and a calibration copy. */
Outcome runMeasure (Context& ctx, const juce::String& title, bool asJob,
                    std::function<Outcome (const ms::ProgressFn&)> fn)
{
    if (!asJob)
        return fn ({});
    // Query commands pass invoke()'s busy guard; a measurement JOB must not
    // read a document that another job is editing.
    juce::String other;
    const auto running = ctx.state.jobs.runningMutating (&other);
    if (running.isNotEmpty())
        return Outcome::failure (err::busy, "a mutating job is running", obj ({ { "job", running }, { "cmd", other } }));
    const auto id = startJob (ctx, ctx.cmd, title, /*exclusive*/ true,
        [fn] (JobControl& jc) {
            const ms::ProgressFn p = [&jc] (double f) { jc.progress (f); return !jc.cancelled(); };
            auto o = fn (p);
            if (!o.ok && jc.cancelled()) return Outcome::failure (err::cancelled, "cancelled by client");
            return o;
        });
    return Outcome::job (id);
}

double workOf (const FileDoc& d, const Region& r, size_t nch)
{
    return (r.t1 - r.t0) * d.engine.sampleRate() * (double) nch;
}

// ---------------------------------------------------------------- levels

Outcome levelsCmd (Context& ctx)
{
    Region r; std::vector<int> ch; Outcome bad;
    if (!regionOf (ctx, r, bad) || !channelsOf (ctx, "channels", ch, bad)) return bad;
    ms::LevelsParams p;
    p.t0 = r.t0; p.t1 = r.t1; p.channels = ch;
    if (ctx.has ("clipThreshold")) p.clipThreshold = (float) ctx.num ("clipThreshold", p.clipThreshold);
    const AudioEngine* eng = &ctx.doc->engine;
    const ScaleCalibration cal = ctx.doc->scale;
    return runMeasure (ctx, "Measuring levels", workOf (*ctx.doc, r, ch.size()) > kJobSampleChannels,
        [eng, cal, p] (const ms::ProgressFn& prog) {
            const auto res = ms::levels (*eng, p, &cal, prog);
            if (res.cancelled) return Outcome::failure (err::cancelled, "cancelled");
            juce::Array<juce::var> chans, fs, off, calib;
            for (const auto& c : res.channels)
            {
                auto v = obj ({ { "ch", c.ch }, { "rmsDb", c.rmsDb }, { "peakDb", c.peakDb }, { "peakAtSec", c.peakAtSec },
                                { "crestDb", c.crestDb }, { "dcOffset", c.dcOffset }, { "minSample", c.minSample },
                                { "maxSample", c.maxSample }, { "clipped", (juce::int64) c.clipped },
                                { "samples", (juce::int64) c.samples } });
                if (c.cal.calibrated)
                {
                    set (v, "rmsAbsDb", c.rmsAbsDb); set (v, "peakAbsDb", c.peakAbsDb);
                    set (v, "absUnit", juce::String (res.absUnit));
                    set (v, "rmsPhysical", c.rmsPhysical); set (v, "peakPhysical", c.peakPhysical);
                }
                chans.add (v);
                fs.add (c.cal.fullScale); off.add (c.cal.levelOffsetDb); calib.add (c.cal.calibrated);
            }
            return Outcome::success (obj ({
                { "region", obj ({ { "start", res.t0 }, { "end", res.t1 }, { "samples", (juce::int64) (res.s1 - res.s0) } }) },
                { "unit", juce::String (res.unit) },
                { "calibration", obj ({ { "unit", juce::String (res.physicalUnit) }, { "fullScale", fs },
                                        { "levelOffsetDb", off }, { "calibrated", calib } }) },
                { "channels", chans } }));
        });
}

// ---------------------------------------------------------------- spectrum / harmonics

bool spectrumParamsOf (Context& ctx, ms::SpectrumParams& p, Region& r, Outcome& bad, bool singleChannel)
{
    std::vector<int> ch;
    if (!regionOf (ctx, r, bad, true) || !channelsOf (ctx, singleChannel ? "channel" : "channels", ch, bad, singleChannel))
        return false;
    int n = 0, w = 0;
    if (!fftSizeOf (ctx, std::clamp (ctx.app.settings.fftLocalN, 2, 65536), n, bad) || !windowOf (ctx, w, bad))
        return false;
    p.t0 = r.t0; p.t1 = r.t1; p.channels = ch; p.fftSize = n; p.windowType = w;
    if (ctx.has ("overlap")) p.overlap = ctx.num ("overlap", -1.0);
    return true;
}

juce::var analysisVar (const ms::SpectrumResult& s)
{
    return obj ({ { "fftSize", s.fftSize }, { "window", juce::String (ms::windowName (s.windowType)) },
                  { "enbwBins", s.enbwBins }, { "framesAveraged", s.framesAveraged }, { "framesCapped", s.framesCapped },
                  { "binHz", s.binHz }, { "sampleRate", s.sampleRate }, { "levelConvention", juce::String (s.levelConvention) },
                  { "unit", juce::String (s.unit) }, { "region", obj ({ { "start", s.t0 }, { "end", s.t1 } }) } });
}

Outcome spectrumCmd (Context& ctx)
{
    ms::SpectrumParams p; Region r; Outcome bad;
    if (!spectrumParamsOf (ctx, p, r, bad, false)) return bad;
    const bool wantPeaks = ctx.has ("peaks");
    ms::PeakParams pp;
    if (wantPeaks)
    {
        const auto pk = ctx.get ("peaks");
        pp.maxCount = pk.hasProperty ("maxCount") ? (int) pk.getProperty ("maxCount", 10) : 10;
        pp.minProminenceDb = pk.hasProperty ("minProminenceDb") ? (double) pk.getProperty ("minProminenceDb", 6.0) : 6.0;
        if (pk.hasProperty ("minLevelDb")) pp.minLevelDb = (double) pk.getProperty ("minLevelDb", -150.0);
        if (pk.hasProperty ("minFreqHz"))  pp.minFreqHz = (double) pk.getProperty ("minFreqHz", 0.0);
        if (pk.hasProperty ("maxFreqHz"))  pp.maxFreqHz = (double) pk.getProperty ("maxFreqHz", 0.0);
    }
    const bool values = ctx.flag ("values", true);
    if (values && (juce::int64) (p.fftSize / 2 + 1) * (juce::int64) p.channels.size() > kInlineValues)
        return invalid ("fftSize", "smaller FFT or fewer channels", "spectrum too large to return inline (use values:false with peaks)");
    const AudioEngine* eng = &ctx.doc->engine;
    const ScaleCalibration cal = ctx.doc->scale;
    return runMeasure (ctx, "Computing the spectrum", workOf (*ctx.doc, r, p.channels.size()) > kJobSampleChannels / 4,
        [eng, cal, p, wantPeaks, pp, values] (const ms::ProgressFn& prog) {
            const auto s = ms::spectrum (*eng, p, &cal, prog);
            if (s.cancelled) return Outcome::failure (err::cancelled, "cancelled");
            if (s.channels.empty()) return Outcome::failure (err::failed, "no spectrum (region too short or bad parameters)");
            juce::Array<juce::var> chans;
            for (const auto& c : s.channels)
            {
                auto v = obj ({ { "ch", c.ch }, { "calibration", calibrationVar (c.cal) } });
                if (values)
                {
                    set (v, "relDb", dbArray (c.relDb));
                    if (c.cal.calibrated)
                    {
                        std::vector<double> ab (c.relDb);
                        for (auto& x : ab) x += c.cal.levelOffsetDb;
                        set (v, "absDb", dbArray (ab));
                        set (v, "absUnit", juce::String (s.absUnit));
                    }
                }
                chans.add (v);
            }
            auto res = obj ({ { "analysis", analysisVar (s) }, { "channels", chans } });
            if (wantPeaks)
            {
                juce::Array<juce::var> pk;
                for (const auto& cp : ms::peaks (s, pp))
                    for (const auto& q : cp.peaks)
                    {
                        auto v = obj ({ { "ch", cp.ch }, { "freqHz", q.freqHz }, { "relDb", q.relDb },
                                        { "prominenceDb", q.prominenceDb }, { "bin", q.bin }, { "binRelDb", q.binRelDb } });
                        if (s.absUnit.size() > 0) set (v, "absDb", q.absDb);
                        pk.add (v);
                    }
                set (res, "peaks", pk);
            }
            return Outcome::success (res);
        });
}

Outcome harmonicsCmd (Context& ctx)
{
    ms::SpectrumParams p; Region r; Outcome bad;
    if (!spectrumParamsOf (ctx, p, r, bad, true)) return bad;
    ms::HarmonicsParams hp;
    hp.fundamentalHz = ctx.num ("fundamentalHz", 0.0);
    hp.anchorHz = ctx.num ("anchorHz", 0.0);
    hp.anchorN = ctx.integer ("n", 1);
    hp.maxK = ctx.integer ("maxK", 10);
    hp.toleranceHz = ctx.num ("toleranceHz", 0.0);
    hp.search = ctx.str ("search", "peak") == "nearestBin" ? ms::HarmonicsParams::Search::NearestBin
                                                          : ms::HarmonicsParams::Search::PeakInTolerance;
    const AudioEngine* eng = &ctx.doc->engine;
    const ScaleCalibration cal = ctx.doc->scale;
    return runMeasure (ctx, "Measuring harmonics", workOf (*ctx.doc, r, 1) > kJobSampleChannels / 4,
        [eng, cal, p, hp] (const ms::ProgressFn& prog) {
            const auto s = ms::spectrum (*eng, p, &cal, prog);
            if (s.cancelled) return Outcome::failure (err::cancelled, "cancelled");
            if (s.channels.empty()) return Outcome::failure (err::failed, "no spectrum (region too short or bad parameters)");
            const auto h = ms::harmonics (s, 0, hp);
            if (h.fundamentalHz <= 0)
                return Outcome::failure (err::failed, "no fundamental found", obj ({ { "hint", "pass fundamentalHz or anchorHz" } }));
            juce::Array<juce::var> hs;
            for (const auto& k : h.harmonics)
            {
                auto v = obj ({ { "k", k.k }, { "expectedHz", k.expectedHz }, { "freqHz", k.freqHz }, { "relDb", k.relDb },
                                { "relToFundamentalDb", k.relToFundamentalDb }, { "valid", k.valid }, { "isLocalMax", k.isLocalMax } });
                if (!s.absUnit.empty()) set (v, "absDb", k.absDb);
                hs.add (v);
            }
            auto res = obj ({ { "fundamentalHz", h.fundamentalHz }, { "autoDetected", h.autoDetected },
                              { "toleranceHz", h.toleranceHz }, { "unit", s.absUnit.empty() ? juce::String ("dBFS") : juce::String (s.absUnit) },
                              { "fftSize", s.fftSize }, { "window", juce::String (ms::windowName (s.windowType)) },
                              { "harmonics", hs } });
            if (h.thdPercent >= 0) set (res, "thdPercent", h.thdPercent);
            return Outcome::success (res);
        });
}

// ---------------------------------------------------------------- third octave

Outcome thirdOctaveCmd (Context& ctx)
{
    Region r; std::vector<int> ch; Outcome bad;
    if (!regionOf (ctx, r, bad) || !channelsOf (ctx, "channels", ch, bad)) return bad;
    ms::ThirdOctaveParams p;
    p.t0 = r.t0; p.t1 = r.t1; p.channels = ch;
    // Default = the panel's choice: the reference (dB-SPL) analyzer for a
    // Pascal-calibrated file, IEC bands otherwise.
    const bool pascal = ctx.doc->scale.unit == ScaleCalibration::Unit::Pascal;
    const juce::String mode = ctx.str ("mode", pascal ? "reference" : "iec");
    p.mode = mode == "reference" ? ms::ThirdOctaveParams::Mode::Reference : ms::ThirdOctaveParams::Mode::Iec;
    p.order = ctx.integer ("order", 40);
    static const int orders[] = { 1, 2, 3, 4, 5, 10, 20, 30, 40 };
    if (std::find (std::begin (orders), std::end (orders), p.order) == std::end (orders))
        return invalid ("order", "one of [1, 2, 3, 4, 5, 10, 20, 30, 40]", "unsupported filter order");
    int n = 0, w = 0;
    if (!fftSizeOf (ctx, std::clamp (ctx.app.settings.fftLocalN, 2, 65536), n, bad) || !windowOf (ctx, w, bad)) return bad;
    p.fftSize = n; p.windowType = w;
    const AudioEngine* eng = &ctx.doc->engine;
    const ScaleCalibration cal = ctx.doc->scale;
    const bool heavy = p.mode == ms::ThirdOctaveParams::Mode::Reference || workOf (*ctx.doc, r, ch.size()) > kJobSampleChannels / 4;
    return runMeasure (ctx, "Computing 1/3-octave bands", heavy,
        [eng, cal, p] (const ms::ProgressFn& prog) {
            const auto t = ms::thirdOctave (*eng, p, &cal, prog);
            if (t.cancelled) return Outcome::failure (err::cancelled, "cancelled");
            if (t.channels.empty()) return Outcome::failure (err::failed, "no bands (region too short or bad parameters)");
            const bool ref = t.mode == ms::ThirdOctaveParams::Mode::Reference;
            juce::Array<juce::var> chans;
            for (const auto& c : t.channels)
            {
                juce::Array<juce::var> bands;
                for (size_t b = 0; b < c.relDb.size() && b < t.centresHz.size(); ++b)
                {
                    const bool empty = b < c.noData.size() && c.noData[b];
                    auto v = obj ({ { "fcHz", t.centresHz[b] },
                                    { "relDb", empty ? t.emptyBandValue : std::round ((double) c.relDb[b] * 1000.0) / 1000.0 } });
                    if (c.cal.calibrated)
                        set (v, "absDb", empty ? t.emptyBandValue
                                               : std::round (((double) c.relDb[b] + c.cal.levelOffsetDb) * 1000.0) / 1000.0);
                    bands.add (v);
                }
                chans.add (obj ({ { "ch", c.ch }, { "levelOffsetDb", c.cal.levelOffsetDb },
                                  { "calibrated", c.cal.calibrated }, { "bands", bands } }));
            }
            return Outcome::success (obj ({
                { "mode", ref ? "reference" : "iec" }, { "order", t.order },
                { "window", ref ? juce::String ("hanning-65536")
                                : juce::String (ms::windowName (t.windowType)) + "-" + juce::String (t.fftSize) },
                { "unit", t.absUnit.empty() ? juce::String ("dB") : juce::String (t.absUnit) },
                { "framesUsed", t.framesUsed }, { "framesCap", t.framesCap },
                { "emptyBandValue", t.emptyBandValue }, { "channels", chans } }));
        });
}

// ---------------------------------------------------------------- curves

Outcome curveCmd (Context& ctx)
{
    Region r; std::vector<int> ch; Outcome bad;
    if (!regionOf (ctx, r, bad) || !channelsOf (ctx, "channel", ch, bad, true)) return bad;
    const juce::String kind = ctx.str ("kind", "rms");
    const AudioEngine* eng = &ctx.doc->engine;
    const bool heavy = workOf (*ctx.doc, r, 1) > kJobSampleChannels / 4;

    if (kind == "rms" || kind == "peak")
    {
        const double win = ctx.num ("windowMs", 50.0) / 1000.0, hop = ctx.num ("hopMs", 10.0) / 1000.0;
        if ((r.t1 - r.t0) / hop > (double) kInlineValues)
            return invalid ("hopMs", "a larger hop", "too many values (> 1 M)");
        const int c = ch[0];
        return runMeasure (ctx, "Computing the level curve", heavy,
            [eng, c, r, win, hop, kind] (const ms::ProgressFn& prog) {
                const auto lc = ms::levelCurve (*eng, c, r.t0, r.t1, win, hop,
                                                kind == "rms" ? ms::LevelKind::Rms : ms::LevelKind::Peak, prog);
                if (lc.cancelled) return Outcome::failure (err::cancelled, "cancelled");
                return Outcome::success (obj ({ { "kind", kind }, { "unit", "dBFS" }, { "channel", c }, { "t0", lc.t0 },
                                                { "hopSec", lc.hopSec }, { "windowSec", lc.windowSec },
                                                { "count", (int) lc.valuesDb.size() }, { "values", dbArray (lc.valuesDb) } }));
            });
    }

    const int track = ctx.str ("track", "max") == "total" ? ms::TrackTotal
                    : ctx.str ("track", "max") == "weighted" ? ms::TrackWeighted : ms::TrackMax;
    const int smooth = ctx.integer ("smooth", 0);
    std::shared_ptr<SpectralSelection> mask;
    if (ctx.flag ("useSpectralSelection", false) && ctx.app.hasSpectralSelection())
        mask = std::make_shared<SpectralSelection> (ctx.doc->spectralSel);
    int n = 0, w = 0;
    if (!fftSizeOf (ctx, ms::kTimeModeFFT, n, bad) || !windowOf (ctx, w, bad)) return bad;
    return runMeasure (ctx, "Computing the curve", heavy,
        [eng, ch, r, kind, track, smooth, mask, n, w] (const ms::ProgressFn& prog) {
            std::vector<ms::CurveData> out;
            const bool ok = kind == "ampVsFreq"
                ? ms::spectrumCurve (*eng, ch, r.t0, r.t1, n, w, track, mask.get(), out, prog)
                : ms::timeCurve (*eng, ch, r.t0, r.t1, track, mask.get(), out, prog);
            if (!ok || out.empty()) return Outcome::failure (err::failed, "no curve (region too short or cancelled)");
            auto& c = out[0];
            if (smooth > 1)
            {
                ms::smoothSeries (c.ampDb, smooth);
                if (kind != "ampVsFreq") ms::smoothSeries (c.freqHz, smooth);
            }
            juce::Array<juce::var> x;
            for (double v : c.x) x.add (v);
            auto res = obj ({ { "kind", kind }, { "unit", "dBFS" }, { "channel", ch[0] },
                              { "xUnit", kind == "ampVsFreq" ? "Hz" : "s" }, { "count", (int) c.x.size() },
                              { "x", x }, { "ampDb", dbArrayF (c.ampDb) } });
            if (kind != "ampVsFreq") set (res, "freqHz", dbArrayF (c.freqHz));
            return Outcome::success (res);
        });
}

// ---------------------------------------------------------------- probe / spectrogram data

Outcome probeCmd (Context& ctx)
{
    std::vector<int> ch; Outcome bad;
    if (!channelsOf (ctx, "channel", ch, bad, true)) return bad;
    auto& eng = ctx.doc->engine;
    const double t = std::clamp (ctx.num ("t", 0.0), 0.0, eng.duration());
    const double f = std::clamp (ctx.num ("f", 1000.0), 0.0, eng.sampleRate() / 2);
    // SpectralEngine's tile cache is shared with the views: message thread.
    const float mag = ctx.app.spectralEngine.probeMagnitude (eng, ch[0], t, f, ctx.app.settings, 0);
    const int fft = ctx.app.settings.fftSize;
    auto r = obj ({ { "t", t }, { "f", f }, { "channel", ch[0] }, { "fftSize", fft },
                    { "binHz", eng.sampleRate() / std::max (1, fft) }, { "magDb", (double) mag }, { "unit", "dBFS" },
                    { "note", "spectrogram settings of the app (FFT size, window, hop)" } });
    const auto c = ms::channelCalibration (&ctx.doc->scale, ch[0]);
    if (c.calibrated)
    {
        set (r, "absDb", (double) mag + c.levelOffsetDb);
        set (r, "absUnit", juce::String (ms::absUnitFor (&ctx.doc->scale)));
    }
    return Outcome::success (r);
}

juce::File outputFile (const juce::String& stem, const juce::String& ext)
{
    static std::atomic<int> counter { 0 };
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("control-out");
    dir.createDirectory();
    return dir.getChildFile (stem + "-" + juce::String (processId()) + "-" + juce::String (++counter) + ext);
}

Outcome spectrogramDataCmd (Context& ctx)
{
    Region r; std::vector<int> ch; Outcome bad;
    if (!regionOf (ctx, r, bad) || !channelsOf (ctx, "channel", ch, bad, true)) return bad;
    int n = 0, w = 0;
    if (!fftSizeOf (ctx, 2048, n, bad) || !windowOf (ctx, w, bad)) return bad;
    const int hop = ctx.integer ("hop", n / 4);
    if (hop < 1) return invalid ("hop", ">= 1", "hop must be >= 1");
    auto& eng = ctx.doc->engine;
    const double rate = eng.sampleRate();
    const double binHz = rate / n;
    const int maxBin = n / 2 + 1;
    const int bins = ctx.has ("fMax") ? std::clamp ((int) std::ceil (ctx.num ("fMax", rate / 2) / binHz) + 1, 1, maxBin) : maxBin;
    const auto s0 = (int64_t) std::floor (r.t0 * rate), s1 = (int64_t) std::ceil (r.t1 * rate);
    const juce::int64 frames = (s1 - s0) / hop + 1;
    if (frames * bins > 50000000)
        return invalid ("hop", "fewer frames x bins (<= 50 M values)", "spectrogram too large");
    const juce::String format = ctx.str ("format", "auto");
    const bool inl = format == "inline" || (format == "auto" && frames * bins <= kInlineValues);
    if (format == "inline" && frames * bins > kInlineValues)
        return invalid ("format", "f32 or csv above 1 M values", "too large to return inline");
    const juce::File out = inl ? juce::File() : outputFile ("spectrogram", format == "csv" ? ".csv" : ".f32");
    const AudioEngine* e = &eng;
    const int c = ch[0];
    return runMeasure (ctx, "Computing spectrogram data", true,
        [e, c, s0, s1, n, hop, w, bins, binHz, rate, inl, out, format] (const ms::ProgressFn& prog) {
            SpectralEngine local;   // own instance: no shared cache, safe on a worker
            if (prog && !prog (0.0)) return Outcome::failure (err::cancelled, "cancelled");
            auto tile = local.computeTile (*e, c, s0, s1 - s0, n, hop, w);
            if (tile == nullptr || tile->numFrames <= 0) return Outcome::failure (err::failed, "no frames");
            const int F = tile->numFrames, B = std::min (bins, tile->numBins);
            auto res = obj ({ { "frames", F }, { "bins", B }, { "binHz", binHz }, { "hopSec", hop / rate },
                              { "t0", (double) s0 / rate }, { "fftSize", n }, { "unit", "dBFS" }, { "inline", inl } });
            if (inl)
            {
                juce::Array<juce::var> rows;
                for (int fr = 0; fr < F; ++fr)
                {
                    juce::Array<juce::var> row;
                    for (int b = 0; b < B; ++b) row.add (std::round ((double) tile->get (fr, b) * 100.0) / 100.0);
                    rows.add (row);
                }
                set (res, "values", rows);
                return Outcome::success (res);
            }
#if JUCE_WINDOWS
            std::ofstream os (out.getFullPathName().toWideCharPointer(), std::ios::binary);
#else
            std::ofstream os (out.getFullPathName().toRawUTF8(), std::ios::binary);
#endif
            if (!os) return Outcome::failure (err::failed, "cannot write " + out.getFullPathName());
            for (int fr = 0; fr < F; ++fr)
            {
                if (format == "csv")
                {
                    for (int b = 0; b < B; ++b) os << (b ? "," : "") << tile->get (fr, b);
                    os << "\n";
                }
                else
                    for (int b = 0; b < B; ++b) { const float v = tile->get (fr, b); os.write ((const char*) &v, sizeof (float)); }
                if (prog && (fr % 256) == 0 && !prog ((double) fr / F)) return Outcome::failure (err::cancelled, "cancelled");
            }
            os.close();
            set (res, "format", format == "csv" ? "csv" : "f32");
            set (res, "path", out.getFullPathName());
            set (res, "bytes", (juce::int64) out.getSize());
            set (res, "layout", format == "csv" ? "frames rows x bins columns, dBFS"
                                                : "frames x bins, float32 little-endian, C order (numpy.fromfile(p, '<f4').reshape(frames, bins))");
            return Outcome::success (res);
        });
}

// ---------------------------------------------------------------- export / markers CSV

Outcome exportSpectrogramCmd (Context& ctx)
{
    const juce::String p = ctx.str ("path");
    if (!juce::File::isAbsolutePath (p)) return invalid ("path", "absolute .png path", "path must be absolute");
    const juce::File png (p);
    {
        const auto chk = checkPath (ctx.app, png, true, ctx.confirmed());
        if (!chk.ok) return chk;
    }
    if (png.existsAsFile() && !ctx.confirmed())
        return Outcome::failure (err::confirmRequired, "Target exists; resend with params.confirm=true",
                                 obj ({ { "path", png.getFullPathName() }, { "hint", "pass confirm:true" } }));
    const auto ov = ctx.get ("overlays");
    if (ov.isObject())
        for (const char* k : { "axes", "markers", "selections", "drawings" })
            if ((bool) ov.getProperty (k, false))
                return Outcome::failure (err::needsUi,
                                         "overlays need the window's renderer (File > Export Spectral Image)",
                                         obj ({ { "hint", "export without overlays, or use the menu" } }));
    Region r; Outcome bad;
    if (!regionOf (ctx, r, bad)) return bad;
    const int w = ctx.integer ("width", 1920), h = ctx.integer ("height", 600);
    const AudioEngine* eng = &ctx.doc->engine;
    const Settings s = ctx.app.settings;   // copy (colour map, FFT, dB range)
    const juce::File src = ctx.doc->file;
    return runMeasure (ctx, "Exporting the spectrogram", true,
        [eng, s, src, png, w, h, r] (const ms::ProgressFn& prog) {
            SpectralEngine local;
            std::atomic<bool> cancel { false };
            if (prog && !prog (0.0)) return Outcome::failure (err::cancelled, "cancelled");
            if (!local.renderToPng (*eng, s, src, png, w, h, r.t0, r.t1, &cancel))
                return Outcome::failure (err::failed, "could not render / write " + png.getFullPathName());
            return Outcome::success (obj ({ { "path", png.getFullPathName() }, { "width", w }, { "height", h },
                                            { "bytes", (juce::int64) png.getSize() },
                                            { "region", obj ({ { "start", r.t0 }, { "end", r.t1 } }) },
                                            { "overlays", false } }));
        });
}

Outcome markersExportCmd (Context& ctx)
{
    const juce::String p = ctx.str ("path");
    if (!juce::File::isAbsolutePath (p)) return invalid ("path", "absolute .csv path", "path must be absolute");
    const juce::File f (p);
    {
        const auto chk = checkPath (ctx.app, f, true, ctx.confirmed());
        if (!chk.ok) return chk;
    }
    if (f.existsAsFile() && !ctx.confirmed())
        return Outcome::failure (err::confirmRequired, "Target exists; resend with params.confirm=true",
                                 obj ({ { "path", f.getFullPathName() }, { "hint", "pass confirm:true" } }));
    if (!ctx.doc->markers.saveCsv (f))
        return Outcome::failure (err::failed, "could not write " + f.getFullPathName());
    return Outcome::success (obj ({ { "path", f.getFullPathName() }, { "count", (int) ctx.doc->markers.markers().size() } }));
}

Outcome markersImportCmd (Context& ctx)
{
    const juce::String p = ctx.str ("path");
    if (!juce::File::isAbsolutePath (p)) return invalid ("path", "absolute .csv path", "path must be absolute");
    const juce::File f (p);
    {
        const auto chk = checkPath (ctx.app, f, false, ctx.confirmed());
        if (!chk.ok) return chk;
    }
    if (!f.existsAsFile()) return Outcome::failure (err::failed, "file not found: " + f.getFullPathName());
    const int n = ctx.doc->markers.loadCsv (f, ctx.doc->engine.duration());
    if (n < 0) return Outcome::failure (err::failed, "could not read markers from " + f.getFullPathName());
    if (ctx.app.onStateChange) ctx.app.onStateChange();
    return Outcome::success (obj ({ { "path", f.getFullPathName() }, { "imported", n },
                                    { "count", (int) ctx.doc->markers.markers().size() } }));
}

std::vector<ParamSpec> regionParams (bool single)
{
    std::vector<ParamSpec> v { P ("start", PType::Number).min (0).describe ("Start (s), default 0"),
                               P ("end", PType::Number).min (0).describe ("End (s), default the end of the file") };
    if (single) v.push_back (P ("channel", PType::Integer).min (0).def (0));
    else        v.push_back (P ("channels", PType::Array).items (PType::Integer, 0.0).describe ("0-based, default all"));
    return v;
}

std::vector<ParamSpec> with (std::vector<ParamSpec> a, std::initializer_list<ParamSpec> b)
{
    for (const auto& x : b) a.push_back (x);
    return a;
}

} // namespace

void registerMeasureCommands (CommandRegistry& r)
{
    const auto fft = P ("fftSize", PType::Integer).range (2, 65536).describe ("Power of two (default: the FFT panel's size)");
    const auto win = P ("window", PType::String).oneOf ({ "hann", "hanning", "hamming", "rect", "rectangular" }).def ("hann");
    const uint32_t f = NeedsDoc | LongRunning | Query;

    r.add ({ "measure.levels", "RMS / peak / crest / DC / clipping per channel, dBFS and calibrated units", "measure",
             with (regionParams (false), { P ("clipThreshold", PType::Number).range (0, 1) }), {}, f, {}, levelsCmd });
    r.add ({ "measure.spectrum", "Averaged spectrum of a region (the FFT panel's numbers), optional peak list", "measure",
             with (regionParams (false), { fft, win,
                   P ("overlap", PType::Number).range (0, 0.99).describe ("Window overlap (default: the panel's placement)"),
                   P ("values", PType::Boolean).def (true).describe ("Return the per-bin levels"),
                   P ("peaks", PType::Object).props ({ P ("maxCount", PType::Integer).min (0), P ("minProminenceDb", PType::Number),
                                                       P ("minLevelDb", PType::Number), P ("minFreqHz", PType::Number).min (0),
                                                       P ("maxFreqHz", PType::Number).min (0) }) }),
             {}, f, {}, spectrumCmd });
    r.add ({ "measure.thirdOctave", "1/3-octave bands: reference dB-SPL analyzer or IEC bands", "measure",
             with (regionParams (false), { P ("mode", PType::String).oneOf ({ "reference", "iec" })
                                               .describe ("Default: reference for a Pascal-calibrated file, else iec"),
                                           P ("order", PType::Integer).def (40), fft, win }),
             {}, f, {}, thirdOctaveCmd });
    r.add ({ "measure.harmonics", "Harmonic levels (given, anchored or auto-detected fundamental) and THD", "measure",
             with (regionParams (true), { fft, win, P ("fundamentalHz", PType::Number).min (0),
                                          P ("anchorHz", PType::Number).min (0), P ("n", PType::Integer).min (1).def (1),
                                          P ("maxK", PType::Integer).range (1, 100).def (10),
                                          P ("search", PType::String).oneOf ({ "peak", "nearestBin" }).def ("peak"),
                                          P ("toleranceHz", PType::Number).min (0) }),
             {}, f, {}, harmonicsCmd });
    r.add ({ "measure.curve", "Level vs time (rms|peak) or the panel's curves (ampVsTime|freqVsTime|ampVsFreq)", "measure",
             with (regionParams (true), { P ("kind", PType::String).oneOf ({ "rms", "peak", "ampVsTime", "freqVsTime", "ampVsFreq" }).def ("rms"),
                                          P ("windowMs", PType::Number).range (0.1, 60000).def (50),
                                          P ("hopMs", PType::Number).range (0.1, 60000).def (10),
                                          P ("track", PType::String).oneOf ({ "max", "total", "weighted" }).def ("max"),
                                          P ("smooth", PType::Integer).range (0, 101).def (0),
                                          P ("useSpectralSelection", PType::Boolean).def (false), fft, win }),
             {}, f, {}, curveCmd });
    r.add ({ "measure.probe", "Spectrogram magnitude at one (time, frequency) point", "measure",
             { P ("t", PType::Number).req().min (0), P ("f", PType::Number).req().min (0),
               P ("channel", PType::Integer).min (0).def (0) },
             {}, NeedsDoc | Query, {}, probeCmd });
    r.add ({ "measure.spectrogramData", "Spectrogram matrix (frames x bins, dBFS), inline or as a float32/CSV file", "measure",
             with (regionParams (true), { fft, win, P ("hop", PType::Integer).min (1),
                                          P ("fMax", PType::Number).min (0),
                                          P ("format", PType::String).oneOf ({ "auto", "inline", "f32", "csv" }).def ("auto") }),
             {}, f, {}, spectrogramDataCmd });
    r.add ({ "export.spectrogram", "Render the spectrogram of a region to a PNG (no overlays)", "export",
             { P ("path", PType::String).req().describe ("Absolute .png path"),
               P ("start", PType::Number).min (0), P ("end", PType::Number).min (0),
               P ("width", PType::Integer).range (16, 16384).def (1920), P ("height", PType::Integer).range (16, 16384).def (600),
               P ("overlays", PType::Object).props ({ P ("axes", PType::Boolean), P ("markers", PType::Boolean),
                                                      P ("selections", PType::Boolean), P ("drawings", PType::Boolean) })
                   .describe ("Overlays need the window renderer: not available through the API yet"),
               P ("confirm", PType::Boolean).describe ("Overwrite / outside the allowed folders") },
             {}, NeedsDoc | LongRunning, {}, exportSpectrogramCmd });
    r.add ({ "markers.exportCsv", "Write the markers to a CSV file", "markers",
             { P ("path", PType::String).req(), P ("confirm", PType::Boolean) }, {}, NeedsDoc, {}, markersExportCmd });
    r.add ({ "markers.importCsv", "Add markers from a CSV file (same columns as exportCsv)", "markers",
             { P ("path", PType::String).req(), P ("confirm", PType::Boolean) }, {}, NeedsDoc, {}, markersImportCmd });
}

} // namespace sc::control
