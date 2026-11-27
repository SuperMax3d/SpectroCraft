/*
 * RoomAcoustics.cpp - see RoomAcoustics.h and docs/reverberation.md.
 */
#include "RoomAcoustics.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace sc::room
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxOrder = 23;               // 2^23 samples: the longest recording + sweep deconvolved at once
constexpr double kMaxTailSeconds = 30.0;    // of response kept after the peak
constexpr double kMinTailSeconds = 0.25;    // of silence a recording needs after the sweep
constexpr double kTiny = 1.0e-30;

double toDb (double power) noexcept { return 10.0 * std::log10 (std::max (power, kTiny)); }

int orderFor (juce::int64 samples) noexcept
{
    int order = 1;
    while (((juce::int64) 1 << order) < samples) ++order;
    return order;
}

/** In place: `d` holds 2^order reals in, 2^order complex bins (re, im) out. */
void forward (std::vector<float>& d, int order)
{
    juce::dsp::FFT (order).performRealOnlyForwardTransform (d.data(), false);
}

/** In place: bins 0 .. N/2 in (the rest is made their mirror), N reals out. */
void inverse (std::vector<float>& d, int order)
{
    const size_t n = (size_t) 1 << order;
    for (size_t k = 1; k < n / 2; ++k)
    {
        d[2 * (n - k)] = d[2 * k];
        d[2 * (n - k) + 1] = -d[2 * k + 1];
    }
    juce::dsp::FFT (order).performRealOnlyInverseTransform (d.data());
}

struct Line { double slope = 0.0, intercept = 0.0, r2 = 0.0; bool ok = false; };

/** Least squares y = slope x + intercept over n points given by index. */
template <typename X, typename Y>
Line regress (int n, X&& x, Y&& y)
{
    Line l;
    if (n < 3) return l;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    for (int i = 0; i < n; ++i)
    {
        const double xi = x (i), yi = y (i);
        sx += xi; sy += yi; sxx += xi * xi; sxy += xi * yi; syy += yi * yi;
    }
    const double dx = n * sxx - sx * sx, dy = n * syy - sy * sy;
    if (dx <= 0.0) return l;
    l.slope = (n * sxy - sx * sy) / dx;
    l.intercept = (sy - l.slope * sx) / n;
    const double c = n * sxy - sx * sy;
    l.r2 = dy > 0.0 ? (c * c) / (dx * dy) : 1.0;
    l.ok = true;
    return l;
}

std::vector<double> blockMeans (const std::vector<double>& e, int w)
{
    std::vector<double> m (e.size() / (size_t) w);
    for (size_t i = 0; i < m.size(); ++i)
    {
        double s = 0.0;
        for (int k = 0; k < w; ++k) s += e[i * (size_t) w + (size_t) k];
        m[i] = s / w;
    }
    return m;
}

double meanFrom (const std::vector<double>& e, int from)
{
    from = juce::jlimit (0, (int) e.size() - 1, from);
    double s = 0.0;
    for (size_t i = (size_t) from; i < e.size(); ++i) s += e[i];
    return s / (double) (e.size() - (size_t) from);
}

/** Where a decay meets its noise floor (Lundeby, Vigran, Bietz and Vorlander 1995). */
struct Tail
{
    bool found = false;
    int cross = 0;            // sample where the late decay line reaches the noise
    double noise = 0.0;       // mean power of the noise
    double slopeDbPerS = 0.0; // of the late decay (< 0)
    double peak = 0.0;        // highest smoothed power
};

Tail lundeby (const std::vector<double>& e, double fs)
{
    Tail t;
    const int n = (int) e.size();
    t.cross = n;
    int w = std::max (1, (int) (0.03 * fs));               // 30 ms to start with
    if (n < 10 * w) return t;
    auto m = blockMeans (e, w);
    const auto timeOf = [&] (int block) { return (block + 0.5) * w / fs; };
    const int lastTenth = n - std::max (w, n / 10);
    double noise = std::max (meanFrom (e, lastTenth), kTiny);
    t.noise = noise;

    int ip = (int) (std::max_element (m.begin(), m.end()) - m.begin());
    t.peak = m[(size_t) ip];

    // the decay from its top to 10 dB above the noise: a first slope and a first crossing
    int stop = ip + 1;
    while (stop < (int) m.size() && toDb (m[(size_t) stop]) > toDb (noise) + 10.0) ++stop;
    auto line = regress (stop - ip, [&] (int i) { return timeOf (ip + i); }, [&] (int i) { return toDb (m[(size_t) (ip + i)]); });
    if (! line.ok || line.slope > -1.0) return t;          // nothing that decays above the noise
    double cross = (toDb (noise) - line.intercept) / line.slope;

    // smoothing fitted to the decay: about 6 intervals per 10 dB
    w = juce::jlimit (std::max (4, (int) (0.003 * fs)), std::max (5, (int) (0.1 * fs)), (int) (fs * (10.0 / -line.slope) / 6.0));
    if (n < 10 * w) return t;
    m = blockMeans (e, w);
    ip = (int) (std::max_element (m.begin(), m.end()) - m.begin());
    t.peak = m[(size_t) ip];

    for (int iteration = 0; iteration < 6; ++iteration)
    {
        // the noise: from 7 dB of decay past the crossing, the last tenth at least
        noise = std::max (meanFrom (e, std::min (lastTenth, (int) ((cross + 7.0 / -line.slope) * fs))), kTiny);
        // the late slope: the part of the decay 7 to 25 dB above the noise, before the crossing
        std::vector<int> late;
        for (int i = ip; i < (int) m.size() && timeOf (i) < cross; ++i)
        {
            const double above = toDb (m[(size_t) i]) - toDb (noise);
            if (above >= 7.0 && above <= 25.0) late.push_back (i);
        }
        if (late.size() < 3)   // a short range above the noise: all of it
        {
            late.clear();
            for (int i = ip; i < (int) m.size() && timeOf (i) < cross; ++i)
                if (toDb (m[(size_t) i]) - toDb (noise) >= 5.0) late.push_back (i);
        }
        const auto next = regress ((int) late.size(), [&] (int i) { return timeOf (late[(size_t) i]); },
                                   [&] (int i) { return toDb (m[(size_t) late[(size_t) i]]); });
        if (! next.ok || next.slope > -1.0) break;
        const double nextCross = (toDb (noise) - next.intercept) / next.slope;
        const bool settled = std::abs (nextCross - cross) < (double) w / fs;
        line = next;
        cross = nextCross;
        if (settled) break;
    }
    t.found = true;
    t.noise = noise;
    t.slopeDbPerS = line.slope;
    t.cross = juce::jlimit (1, n, (int) std::lround (cross * fs));
    return t;
}

struct BandInfo { double centre = 0.0, nominal = 0.0, bandwidth = 0.0, edge = 1.0; };

std::vector<BandInfo> bandsFor (const ImpulseResponse& ir, const Options& o)
{
    static const double thirdNominal[] = { 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250,
                                           1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000 };
    const bool octave = o.bands == Bands::Octave;
    const double half = octave ? 0.15 : 0.05;                      // band edges: centre x 10^(+/- half)
    const double edge = std::pow (10.0, half);
    std::vector<BandInfo> out;
    for (int i = 0; i < 30; ++i)
    {
        if (octave && (i - 1) % 3 != 0) continue;                  // 31.5, 63, 125 ...
        BandInfo b;
        b.nominal = thirdNominal[i];
        b.centre = 1000.0 * std::pow (10.0, (i - 16) / 10.0);
        b.edge = edge;
        b.bandwidth = b.centre * (edge - 1.0 / edge);
        if (b.nominal < o.lowestHz * 0.999 || b.nominal > o.highestHz * 1.001) continue;
        if (b.centre * edge > 0.45 * ir.sampleRate) continue;
        if (ir.lowHz > 0.0 && b.centre / edge < ir.lowHz) continue;
        if (ir.highHz > 0.0 && b.centre * edge > ir.highHz) continue;
        out.push_back (b);
    }
    return out;
}

Fit fitRange (const std::vector<double>& levelDb, int tc, double fs, double from, double to)
{
    Fit f;
    int i0 = -1, i1 = -1;
    for (int i = 0; i < tc; ++i)
    {
        if (i0 < 0 && levelDb[(size_t) i] <= from) i0 = i;
        if (levelDb[(size_t) i] <= to) { i1 = i; break; }
    }
    if (i0 < 0 || i1 < 0 || i1 - i0 < 4) return f;
    const auto line = regress (i1 - i0 + 1, [&] (int i) { return (double) (i0 + i); }, [&] (int i) { return levelDb[(size_t) (i0 + i)]; });
    if (! line.ok || line.slope >= 0.0) return f;
    f.seconds = -60.0 / (line.slope * fs);
    f.r2 = line.r2;
    return f;
}

/** Everything from the squared response of one band, starting at the direct sound. */
BandResult analyseEnergy (const std::vector<double>& e, double fs, const BandInfo& band)
{
    BandResult r;
    r.centreHz = band.centre;
    r.nominalHz = band.nominal;
    const int n = (int) e.size();
    if (n < 16) return r;

    const auto tail = lundeby (e, fs);
    r.noiseFound = tail.found;
    r.inrDb = toDb (tail.peak) - toDb (tail.noise);
    const int tc = tail.found ? tail.cross : n;
    const double kappa = tail.found ? -tail.slopeDbPerS * std::log (10.0) / 10.0 : 0.0;    // 1/s: the late decay's rate
    const double beyond = tail.found && kappa > 0.0 ? tail.noise * fs / kappa : 0.0;       // the cut tail's energy
    r.truncationSeconds = tc / fs;

    // Schroeder: the energy still to come at each instant
    std::vector<double> left ((size_t) tc + 1);
    left[(size_t) tc] = beyond;
    for (int i = tc - 1; i >= 0; --i) left[(size_t) i] = left[(size_t) i + 1] + e[(size_t) i];
    const double total = left[0];
    if (total <= 0.0) return r;
    std::vector<double> levelDb ((size_t) tc + 1);
    for (int i = 0; i <= tc; ++i) levelDb[(size_t) i] = std::max (-200.0, toDb (left[(size_t) i] / total));

    r.edt = fitRange (levelDb, tc, fs, 0.0, -10.0);
    r.t20 = fitRange (levelDb, tc, fs, -5.0, -25.0);
    r.t30 = fitRange (levelDb, tc, fs, -5.0, -35.0);
    r.filterLimited = band.bandwidth > 0.0 && r.t20.measured() && band.bandwidth * r.t20.seconds <= kMinBandwidthTime;
    const bool usable = tail.found && ! r.filterLimited;
    r.edt.reliable = usable && r.edt.measured() && r.inrDb >= 25.0;
    r.t20.reliable = usable && r.t20.measured() && r.inrDb >= 35.0;
    r.t30.reliable = usable && r.t30.measured() && r.inrDb >= 45.0;
    if (r.t20.measured() && r.t30.measured()) r.curvaturePct = 100.0 * (r.t30.seconds / r.t20.seconds - 1.0);

    // early against late energy, and the centre time
    const auto leftAt = [&] (double seconds)
    {
        const int i = (int) std::lround (seconds * fs);
        if (i <= tc) return left[(size_t) std::max (0, i)];
        return beyond * std::exp (-kappa * (i - tc) / fs);
    };
    const double late50 = leftAt (0.05), late80 = leftAt (0.08);
    if (late50 > 0.0 && late50 < total) { r.c50 = toDb ((total - late50) / late50); r.d50 = (total - late50) / total; }
    if (late80 > 0.0 && late80 < total) r.c80 = toDb ((total - late80) / late80);
    double moment = 0.0;
    for (int i = 0; i < tc; ++i) moment += (i / fs) * e[(size_t) i];
    if (kappa > 0.0) moment += beyond * (tc / fs + 1.0 / kappa);
    r.centreTimeMs = 1000.0 * moment / total;

    const int step = std::max (1, (int) std::lround (r.decayStepSeconds * fs));
    r.decayStepSeconds = step / fs;
    r.decayDb.reserve ((size_t) (tc / step) + 1);
    for (int i = 0; i <= tc; i += step) r.decayDb.push_back ((float) levelDb[(size_t) i]);
    return r;
}

void findDirect (ImpulseResponse& ir)
{
    const float limit = 0.1f * std::abs (ir.h[(size_t) ir.peak]);   // 20 dB below the peak (ISO 3382-1)
    ir.direct = ir.peak;
    for (int i = 0; i < ir.peak; ++i)
        if (std::abs (ir.h[(size_t) i]) >= limit) { ir.direct = i; break; }
}
} // namespace

//==============================================================================
juce::var SweepSpec::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("type", "exp-sweep");
    o->setProperty ("startHz", startHz);
    o->setProperty ("endHz", endHz);
    o->setProperty ("seconds", seconds);
    o->setProperty ("silenceSeconds", silenceSeconds);
    o->setProperty ("level", level);
    return juce::var (o);
}

bool SweepSpec::fromVar (const juce::var& v, SweepSpec& out)
{
    if (! v.isObject() || v["type"].toString() != "exp-sweep") return false;
    SweepSpec s;
    s.startHz = (double) v["startHz"];
    s.endHz = (double) v["endHz"];
    s.seconds = (double) v["seconds"];
    s.silenceSeconds = (double) v.getProperty ("silenceSeconds", s.silenceSeconds);
    s.level = (double) v.getProperty ("level", s.level);
    if (! s.valid()) return false;
    out = s;
    return true;
}

std::vector<float> makeSweep (const SweepSpec& spec, double sampleRate)
{
    std::vector<float> x;
    if (! spec.valid() || sampleRate <= 0.0) return x;
    const double f1 = spec.startHz, f2 = std::min (spec.endHz, 0.45 * sampleRate), T = spec.seconds;
    if (f2 <= f1) return x;
    const double R = std::log (f2 / f1);
    const int n = (int) std::lround (T * sampleRate);
    const int fadeIn = std::max (1, (int) (std::min (0.25, 0.05 * T) * sampleRate));
    const int fadeOut = std::max (1, (int) (std::min (0.02, 0.01 * T) * sampleRate));
    x.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sampleRate;
        double g = spec.level;
        if (i < fadeIn) g *= 0.5 - 0.5 * std::cos (kPi * i / fadeIn);
        if (i >= n - fadeOut) g *= 0.5 - 0.5 * std::cos (kPi * (n - 1 - i) / fadeOut);
        x[(size_t) i] = (float) (g * std::sin (2.0 * kPi * f1 * T / R * (std::exp (t * R / T) - 1.0)));
    }
    return x;
}

ImpulseResponse fromSweep (const float* recording, juce::int64 numSamples, double sampleRate, const SweepSpec& spec)
{
    ImpulseResponse ir;
    ir.sampleRate = sampleRate;
    const auto x = makeSweep (spec, sampleRate);
    if (recording == nullptr || numSamples <= 0 || x.empty()) { ir.problem = "Nothing to analyse."; return ir; }
    const juce::int64 nx = (juce::int64) x.size(), minTail = (juce::int64) (kMinTailSeconds * sampleRate);
    if (numSamples < nx + minTail)
    {
        ir.problem = "The recording is shorter than the sweep (" + juce::String (spec.seconds, 1) + " s) and its decay.";
        return ir;
    }
    const int order = orderFor (numSamples + nx);
    if (order > kMaxOrder)
    {
        ir.problem = "The recording is too long: keep one sweep and the silence after it (at most "
                     + juce::String ((int) ((double) (((juce::int64) 1 << kMaxOrder) - nx) / sampleRate)) + " s).";
        return ir;
    }
    const size_t m = (size_t) 1 << order;
    std::vector<float> y (2 * m, 0.0f);
    std::copy (recording, recording + numSamples, y.begin());
    forward (y, order);
    {
        std::vector<float> sweep (2 * m, 0.0f);
        std::copy (x.begin(), x.end(), sweep.begin());
        forward (sweep, order);
        double top = 0.0;
        for (size_t k = 0; k <= m / 2; ++k)
            top = std::max (top, (double) sweep[2 * k] * sweep[2 * k] + (double) sweep[2 * k + 1] * sweep[2 * k + 1]);
        const double floorPower = std::max (top * 1.0e-9, kTiny);
        const double f1 = spec.startHz, f2 = std::min (spec.endHz, 0.45 * sampleRate);
        const double lowTop = f1 * 1.26, highFrom = f2 / 1.12;      // where the sweep's edges fade
        for (size_t k = 0; k <= m / 2; ++k)
        {
            const double f = (double) k * sampleRate / (double) m;
            double wgt = 1.0;
            if (f <= f1 || f >= f2) wgt = 0.0;
            else if (f < lowTop) wgt = 0.5 - 0.5 * std::cos (kPi * std::log (f / f1) / std::log (lowTop / f1));
            else if (f > highFrom) wgt = 0.5 - 0.5 * std::cos (kPi * std::log (f2 / f) / std::log (f2 / highFrom));
            // H = Y conj(X) / |X|^2, inside the sweep's range
            const std::complex<double> Y (y[2 * k], y[2 * k + 1]), X (sweep[2 * k], sweep[2 * k + 1]);
            const auto H = wgt > 0.0 ? Y * std::conj (X) * (wgt / std::max (std::norm (X), floorPower)) : std::complex<double>();
            y[2 * k] = (float) H.real();
            y[2 * k + 1] = (float) H.imag();
        }
        ir.lowHz = lowTop;
        ir.highHz = highFrom;
    }
    inverse (y, order);   // y[i]: the response at delay i from the recording's start (distortion wraps to the end)

    // the response's peak is where the sweep was heard; it cannot be later than the recording
    juce::int64 peak = 0;
    double power = 0.0;
    for (juce::int64 i = 0; i < numSamples; ++i)
    {
        power += (double) y[(size_t) i] * y[(size_t) i];
        if (std::abs (y[(size_t) i]) > std::abs (y[(size_t) peak])) peak = i;
    }
    const double peakPower = (double) y[(size_t) peak] * y[(size_t) peak];
    if (peakPower <= 0.0 || peakPower < 100.0 * power / (double) numSamples)
    {
        ir.problem = "No sweep found in this recording (" + juce::String (spec.startHz, 0) + " to " + juce::String (spec.endHz, 0)
                     + " Hz in " + juce::String (spec.seconds, 1) + " s).";
        return ir;
    }
    // a looped sweep: the first whole one in the recording (as strong as the strongest, within 3 dB)
    for (juce::int64 i = 0; i < peak; ++i)
        if (std::abs (y[(size_t) i]) >= 0.7f * std::abs (y[(size_t) peak]))
        {
            juce::int64 first = i;
            for (juce::int64 k = i; k < std::min (peak, i + (juce::int64) (0.005 * sampleRate)); ++k)
                if (std::abs (y[(size_t) k]) > std::abs (y[(size_t) first])) first = k;
            peak = first;
            break;
        }
    juce::int64 tail = numSamples - nx - peak;   // what was recorded after the sweep heard at `peak` ended
    // in a loop the next sweep answers one period later, and its harmonic distortion (up to the
    // 6th here) before that: the response stops there
    {
        const double R = std::log (std::min (spec.endHz, 0.45 * sampleRate) / spec.startHz);
        const double lead = spec.seconds * std::log (6.0) / R + kPreSeconds;
        tail = std::min (tail, (juce::int64) ((spec.seconds + spec.silenceSeconds - lead) * sampleRate));
    }
    if (tail < minTail)
    {
        ir.problem = "The recording stops too soon after the sweep: leave silence after it for the decay.";
        return ir;
    }
    const juce::int64 start = std::max ((juce::int64) 0, peak - (juce::int64) (kPreSeconds * sampleRate));
    const juce::int64 end = peak + std::min (tail, (juce::int64) (kMaxTailSeconds * sampleRate));
    ir.h.assign (y.begin() + start, y.begin() + end);
    ir.peak = (int) (peak - start);
    ir.startInRecording = (double) start / sampleRate;
    findDirect (ir);
    return ir;
}

ImpulseResponse fromImpulse (const float* recording, juce::int64 numSamples, double sampleRate)
{
    ImpulseResponse ir;
    ir.sampleRate = sampleRate;
    if (recording == nullptr || numSamples < 16 || sampleRate <= 0.0) { ir.problem = "Nothing to analyse."; return ir; }
    juce::int64 peak = 0;
    for (juce::int64 i = 0; i < numSamples; ++i)
        if (std::abs (recording[i]) > std::abs (recording[peak])) peak = i;
    if (recording[peak] == 0.0f) { ir.problem = "The recording is silent."; return ir; }
    const juce::int64 start = std::max ((juce::int64) 0, peak - (juce::int64) (kPreSeconds * sampleRate));
    const juce::int64 end = std::min (numSamples, peak + (juce::int64) (kMaxTailSeconds * sampleRate));
    if (end - peak < (juce::int64) (kMinTailSeconds * sampleRate))
    {
        ir.problem = "The recording stops too soon after the impulse.";
        return ir;
    }
    ir.h.assign (recording + start, recording + end);
    ir.peak = (int) (peak - start);
    ir.startInRecording = (double) start / sampleRate;
    findDirect (ir);
    return ir;
}

juce::String bandName (const BandResult& b)
{
    if (b.nominalHz <= 0.0) return "All";
    if (b.nominalHz >= 1000.0) return juce::String (b.nominalHz / 1000.0, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " kHz";
    return juce::String (b.nominalHz, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " Hz";
}

juce::String toCsv (const std::vector<BandResult>& bands)
{
    juce::String s = "band_hz,EDT_s,T20_s,T30_s,C50_dB,C80_dB,D50,Ts_ms,INR_dB,EDT_ok,T20_ok,T30_ok,curvature_pct,filter_limited\n";
    const auto num = [] (double v, int decimals) { return std::isnan (v) ? juce::String() : juce::String (v, decimals); };
    for (const auto& b : bands)
        s << (b.nominalHz > 0.0 ? juce::String (b.nominalHz, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") : juce::String ("all"))
          << "," << num (b.edt.seconds, 3) << "," << num (b.t20.seconds, 3) << "," << num (b.t30.seconds, 3)
          << "," << num (b.c50, 2) << "," << num (b.c80, 2) << "," << num (b.d50, 3) << "," << num (b.centreTimeMs, 1)
          << "," << juce::String (b.inrDb, 1) << "," << (b.edt.reliable ? 1 : 0) << "," << (b.t20.reliable ? 1 : 0)
          << "," << (b.t30.reliable ? 1 : 0) << "," << num (b.curvaturePct, 1) << "," << (b.filterLimited ? 1 : 0) << "\n";
    return s;
}

std::vector<double> bandCentres (const ImpulseResponse& ir, const Options& o)
{
    std::vector<double> out;
    for (const auto& b : bandsFor (ir, o)) out.push_back (b.nominal);
    return out;
}

std::vector<BandResult> analyse (const ImpulseResponse& ir, const Options& o)
{
    std::vector<BandResult> out;
    if (! ir.ok() || ir.sampleRate <= 0.0) return out;
    const double fs = ir.sampleRate;
    const int len = (int) ir.h.size(), from = juce::jlimit (0, len - 1, ir.direct);
    std::vector<double> e ((size_t) (len - from));

    if (o.wholeResponse)
    {
        for (int i = from; i < len; ++i) e[(size_t) (i - from)] = (double) ir.h[(size_t) i] * ir.h[(size_t) i];
        out.push_back (analyseEnergy (e, fs, {}));
    }
    const auto bands = bandsFor (ir, o);
    if (bands.empty()) return out;

    // the response's spectrum once (padded: a filter's ringing must not wrap onto the start), then each band
    const int order = orderFor ((juce::int64) len + (juce::int64) (2.0 * fs));
    const size_t m = (size_t) 1 << order;
    std::vector<float> spectrum (2 * m, 0.0f), work (2 * m);
    std::copy (ir.h.begin(), ir.h.end(), spectrum.begin());
    forward (spectrum, order);
    for (const auto& band : bands)
    {
        const double q = band.centre / band.bandwidth;
        for (size_t k = 0; k <= m / 2; ++k)
        {
            const double f = (double) k * fs / (double) m;
            std::complex<double> H;
            if (f > 0.0)
            {
                // 3rd-order Butterworth low-pass turned into a band-pass: s -> q (s / w0 + w0 / s)
                const double w = q * (f / band.centre - band.centre / f);
                H = 1.0 / (std::complex<double> (1.0, w) * std::complex<double> (1.0 - w * w, w));
            }
            const auto v = H * std::complex<double> (spectrum[2 * k], spectrum[2 * k + 1]);
            work[2 * k] = (float) v.real();
            work[2 * k + 1] = (float) v.imag();
        }
        inverse (work, order);
        for (int i = from; i < len; ++i) e[(size_t) (i - from)] = (double) work[(size_t) i] * work[(size_t) i];
        out.push_back (analyseEnergy (e, fs, band));
    }
    return out;
}
} // namespace sc::room
