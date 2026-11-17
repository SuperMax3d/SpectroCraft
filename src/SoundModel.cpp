/*
 * SoundModel.cpp - analysis, rendering and JSON of the Model panel's sound
 * model (issue #40). See include/SoundModel.h and docs/model-panel.md.
 */
#include "SoundModel.h"
#include "FFTHelpers.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace sc::model
{
namespace
{
    constexpr double kTiny = 1e-20;          // linear power floor (-200 dB)
    constexpr int kPeakHalfBins = 3;         // a line's power is summed over +/- 3 bins
    constexpr int kFloorHalfBins = 30;       // its noise floor is the median over +/- 30 bins...
    constexpr int kFloorGapBins = 6;         // ... minus the +/- 6 bins around the peak
    constexpr double kRateDt = 0.1;          // dp/dt from p(t +/- 0.1 s)

    double toDb (double power) { return 10.0 * std::log10 (std::max (power, kTiny)); }
    double fromDb (double db)  { return std::pow (10.0, db / 10.0); }

    std::vector<float> hannPeriodic (int n)
    {
        std::vector<float> w ((size_t) n);
        for (int i = 0; i < n; ++i)
            w[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / n));
        return w;
    }

    /** One analysis frame: one-sided power per bin, normalised so that the
        bins of a sine sum to its mean power (A^2 / 2) and the bins of white
        noise sum to its variance. */
    struct Framer
    {
        int order = 12, n = 4096;
        std::vector<float> win, buf, samples;
        double winPow = 1.0;                 // sum of w^2

        explicit Framer (int fftOrder) : order (fftOrder), n (1 << fftOrder)
        {
            win = hannPeriodic (n);
            winPow = 0.0;
            for (float w : win) winPow += (double) w * w;
            buf.assign ((size_t) n * 2, 0.0f);
            samples.assign ((size_t) n, 0.0f);
        }

        void power (const Audio& a, int ch, int64_t centre, std::vector<double>& P)
        {
            a.read (ch, centre - n / 2, n, samples.data());
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < n; ++i) buf[(size_t) i] = samples[(size_t) i] * win[(size_t) i];
            scfft::realForward (buf.data(), order);
            const int half = n / 2;
            P.assign ((size_t) half + 1, 0.0);
            const double norm = 1.0 / ((double) n * winPow);
            for (int k = 0; k <= half; ++k)
            {
                const double re = buf[(size_t) k * 2], im = buf[(size_t) k * 2 + 1];
                P[(size_t) k] = (k == 0 || k == half ? 1.0 : 2.0) * (re * re + im * im) * norm;
            }
        }
    };

    double median (std::vector<double>& v)
    {
        if (v.empty()) return 0.0;
        const size_t m = v.size() / 2;
        std::nth_element (v.begin(), v.begin() + (long) m, v.end());
        return v[m];
    }

    /** A line's power above the local noise floor, around the strongest bin
        in [kLo, kHi]. */
    struct PeakMeasure
    {
        bool ok = false;
        double power = 0.0;     // above the floor (linear)
        double floor = 0.0;     // per bin
        double freq = 0.0;      // interpolated peak frequency
        bool aboveNoise = false;
    };

    PeakMeasure measurePeak (const std::vector<double>& P, int kLo, int kHi, double binHz)
    {
        PeakMeasure r;
        const int last = (int) P.size() - 2;   // keep away from DC and Nyquist
        kLo = std::max (kLo, 1);
        kHi = std::min (kHi, last);
        if (kHi < kLo) return r;
        int m = kLo;
        for (int k = kLo + 1; k <= kHi; ++k)
            if (P[(size_t) k] > P[(size_t) m]) m = k;

        double sum = 0.0;
        int bins = 0;
        for (int k = std::max (1, m - kPeakHalfBins); k <= std::min (last, m + kPeakHalfBins); ++k)
        {
            sum += P[(size_t) k];
            ++bins;
        }
        std::vector<double> around;
        for (int k = std::max (1, m - kFloorHalfBins); k <= std::min (last, m + kFloorHalfBins); ++k)
            if (std::abs (k - m) > kFloorGapBins) around.push_back (P[(size_t) k]);
        r.floor = median (around);
        r.power = std::max (0.0, sum - r.floor * bins);
        r.aboveNoise = sum > 2.0 * r.floor * bins;   // > 3 dB over the noise in the same bins

        // Parabolic interpolation of the peak on a dB scale.
        double delta = 0.0;
        if (m > 1 && m < last)
        {
            const double a = toDb (P[(size_t) m - 1]), b = toDb (P[(size_t) m]), c = toDb (P[(size_t) m + 1]);
            const double den = a - 2.0 * b + c;
            if (std::abs (den) > 1e-12) delta = juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / den);
        }
        r.freq = (m + delta) * binHz;
        r.ok = true;
        return r;
    }

    double paramAt (const ParamFn& fn, const juce::String& name, double t, bool* found)
    {
        if (name.isEmpty()) { if (found) *found = true; return 0.0; }
        bool ok = false;
        const double v = fn ? fn (name, t, &ok) : 0.0;
        if (found) *found = ok;
        return ok && std::isfinite (v) ? v : 0.0;
    }

    double rateAt (const ParamFn& fn, const juce::String& name, double t)
    {
        if (name.isEmpty()) return 0.0;
        return (paramAt (fn, name, t + kRateDt, nullptr) - paramAt (fn, name, t - kRateDt, nullptr)) / (2.0 * kRateDt);
    }

    /** Weights of the steady / accelerating / decelerating levels for a rate. */
    void dynamicsWeights (const Component& c, const Model& m, double rate, double& wSteady,
                          double& wAccel, double& wDecel)
    {
        wSteady = 1.0; wAccel = wDecel = 0.0;
        const double thr = m.steadyRatePct / 100.0 * span (c);
        if (! c.dynamics || ! (thr > 0.0)) return;
        const double r = rate / thr;   // 1 = threshold, 2 = full state
        if (r > 1.0)       { wAccel = std::min (1.0, r - 1.0); wSteady = 1.0 - wAccel; }
        else if (r < -1.0) { wDecel = std::min (1.0, -r - 1.0); wSteady = 1.0 - wDecel; }
    }

    enum Motion { kSteady = 0, kAccel = 1, kDecel = 2 };

    Motion classify (const Component& c, const Model& m, double rate)
    {
        const double thr = m.steadyRatePct / 100.0 * span (c);
        if (! c.dynamics || ! (thr > 0.0)) return kSteady;
        if (rate > thr)  return kAccel;
        if (rate < -thr) return kDecel;
        return kSteady;
    }

    /** Bins frames by parameter value and averages them into a curve. */
    struct Binner
    {
        double lo = 0.0, hi = 0.0;
        int bins = 1;
        std::vector<double> sumV, sumP;
        std::vector<int> count;

        Binner (double pLo, double pHi, int nb)
            : lo (pLo), hi (pHi), bins (pHi - pLo > 1e-12 ? std::max (1, nb) : 1),
              sumV ((size_t) bins, 0.0), sumP ((size_t) bins, 0.0), count ((size_t) bins, 0) {}

        void add (double p, double v)
        {
            int b = 0;
            if (bins > 1) b = juce::jlimit (0, bins - 1, (int) std::floor ((p - lo) / (hi - lo) * bins));
            sumV[(size_t) b] += v;
            sumP[(size_t) b] += p;
            ++count[(size_t) b];
        }

        bool empty() const { return std::all_of (count.begin(), count.end(), [] (int n) { return n == 0; }); }

        /** asDb: the values are linear powers, the curve holds their mean in dB. */
        ParamCurve curve (bool asDb) const
        {
            ParamCurve c;
            for (int b = 0; b < bins; ++b)
                if (count[(size_t) b] > 0)
                {
                    const double mean = sumV[(size_t) b] / count[(size_t) b];
                    c.p.push_back (sumP[(size_t) b] / count[(size_t) b]);
                    c.v.push_back (asDb ? toDb (mean) : mean);
                }
            return c;
        }
    };

    /** Band dB of a noise component's spectrum at parameter p (states
        interpolated in dB; states at the same parameter averaged). */
    void stateBandsAt (const Component& c, double p, std::vector<double>& out)
    {
        out.clear();
        std::vector<const NoiseState*> sorted;
        for (const auto& s : c.states)
            if (! s.psdDb.empty()) sorted.push_back (&s);
        if (sorted.empty()) return;
        const size_t nb = sorted.front()->psdDb.size();
        sorted.erase (std::remove_if (sorted.begin(), sorted.end(),
                                      [nb] (const NoiseState* s) { return s->psdDb.size() != nb; }),
                      sorted.end());
        std::stable_sort (sorted.begin(), sorted.end(),
                          [] (const NoiseState* a, const NoiseState* b) { return a->param < b->param; });

        // One averaged spectrum per distinct parameter value.
        struct Group { double p = 0.0; std::vector<double> db; int n = 0; };
        std::vector<Group> groups;
        for (const auto* s : sorted)
        {
            if (groups.empty() || std::abs (s->param - groups.back().p) > 1e-9)
                groups.push_back ({ s->param, std::vector<double> (nb, 0.0), 0 });
            auto& g = groups.back();
            for (size_t k = 0; k < nb; ++k) g.db[k] += s->psdDb[k];
            ++g.n;
        }
        for (auto& g : groups)
            for (auto& v : g.db) v /= g.n;

        if (groups.size() == 1 || p <= groups.front().p) { out = groups.front().db; return; }
        if (p >= groups.back().p) { out = groups.back().db; return; }
        size_t i = 1;
        while (i < groups.size() && groups[i].p < p) ++i;
        const auto& a = groups[i - 1];
        const auto& b = groups[i];
        const double f = (p - a.p) / (b.p - a.p);
        out.resize (nb);
        for (size_t k = 0; k < nb; ++k) out[k] = a.db[k] + f * (b.db[k] - a.db[k]);
    }

    /** Band position (fractional index) of frequency f. */
    double bandPos (const Model& m, double f)
    {
        if (m.bandHz.empty() || ! (f > 0.0)) return 0.0;
        return std::log2 (f / m.bandHz.front()) * m.bandsPerOctave;
    }

    double bandValueAt (const std::vector<double>& bandsDb, double pos)
    {
        if (bandsDb.empty()) return -200.0;
        if (pos <= 0.0) return bandsDb.front();
        const double last = (double) bandsDb.size() - 1.0;
        if (pos >= last) return bandsDb.back();
        const int i = (int) pos;
        const double f = pos - i;
        return bandsDb[(size_t) i] + f * (bandsDb[(size_t) i + 1] - bandsDb[(size_t) i]);
    }

    void makeBands (Model& m)
    {
        m.bandHz.clear();
        const double nyq = m.analysisRate * 0.5;
        for (int i = 0; ; ++i)
        {
            const double fc = 20.0 * std::pow (2.0, (double) i / m.bandsPerOctave);
            if (fc >= nyq * 0.98) break;
            m.bandHz.push_back ((float) fc);
        }
    }

    juce::String fmt (double v, int dec = 1) { return juce::String (v, dec); }

    /** Frame centres (samples) covering [t0, t1] with a hop of n/4. */
    std::vector<int64_t> frameCentres (const TimeRange& r, double rate, int n, int64_t total)
    {
        std::vector<int64_t> out;
        const int64_t s0 = (int64_t) std::llround (std::max (0.0, r.t0) * rate);
        const int64_t s1 = std::min (total, (int64_t) std::llround (std::max (0.0, r.t1) * rate));
        if (s1 <= s0) return out;
        const int hop = std::max (1, n / 4);
        if (s1 - s0 <= n) { out.push_back ((s0 + s1) / 2); return out; }
        for (int64_t c = s0 + n / 2; c + n / 2 <= s1; c += hop) out.push_back (c);
        return out;
    }

}

// ===================================================================== data

double ParamCurve::at (double x) const
{
    if (p.empty()) return 0.0;
    if (p.size() == 1 || x <= p.front()) return v.front();
    if (x >= p.back()) return v.back();
    const auto it = std::upper_bound (p.begin(), p.end(), x);
    const size_t i = (size_t) (it - p.begin());
    const double f = (x - p[i - 1]) / std::max (1e-12, p[i] - p[i - 1]);
    return v[i - 1] + f * (v[i] - v[i - 1]);
}

juce::String HarmonicLine::label() const
{
    if (isTrack())
        return "Track (" + juce::StringArray (selections.data(), (int) selections.size()).joinIntoString (", ") + ")";
    const double r = std::round (order);
    return "H" + (std::abs (order - r) < 1e-9 ? juce::String ((int) r) : juce::String (order, 2));
}

double Component::fundamentalAt (double p) const
{
    if (parameter.isEmpty() || std::abs (f0AtParam) < 1e-12) return f0Hz;
    return f0Hz * p / f0AtParam;
}

double Component::lineFreqAt (const HarmonicLine& line, double p) const
{
    if (line.isTrack())
    {
        if (line.freqHz.empty()) return 0.0;
        // Beyond the measured values the track keeps its frequency / parameter ratio.
        const double pLoT = line.freqHz.p.front(), pHiT = line.freqHz.p.back();
        if (p < pLoT && pLoT > 0.0 && p > 0.0 && ! parameter.isEmpty()) return line.freqHz.v.front() * p / pLoT;
        if (p > pHiT && pHiT > 0.0 && ! parameter.isEmpty()) return line.freqHz.v.back() * p / pHiT;
        return line.freqHz.at (p);
    }
    return line.order * fundamentalAt (p) * line.tuning;
}

double Component::rangeGain (double p) const
{
    const double s = pHi - pLo;
    if (parameter.isEmpty() || ! (s > 1e-12)) return 1.0;
    const double full = pLo - 0.05 * s, zero = pLo - 0.15 * s;
    if (p >= full) return 1.0;
    if (p <= zero) return 0.0;
    return (p - zero) / (full - zero);
}

Component& Model::add (Kind kind, const juce::String& name)
{
    Component c;
    c.kind = kind;
    c.name = name;
    // A short id that stays unique in this model.
    int n = (int) components.size() + 1;
    auto taken = [this] (const juce::String& id) { return indexOf (id) >= 0; };
    while (taken ("c" + juce::String (n))) ++n;
    c.id = "c" + juce::String (n);
    if (kind == Kind::Harmonic)
        for (int k = 1; k <= 3; ++k) { HarmonicLine l; l.order = k; c.lines.push_back (l); }
    components.push_back (c);
    return components.back();
}

int Model::indexOf (const juce::String& id) const
{
    for (size_t i = 0; i < components.size(); ++i)
        if (components[i].id == id) return (int) i;
    return -1;
}

Component* Model::find (const juce::String& id)
{
    const int i = indexOf (id);
    return i >= 0 ? &components[(size_t) i] : nullptr;
}

// ================================================================= analysis

double linePower (const Component& c, const HarmonicLine& line, double p, double rate, const Model& m)
{
    if (line.levelDb.empty()) return 0.0;
    double ws, wa, wd;
    dynamicsWeights (c, m, rate, ws, wa, wd);
    double db = line.levelDb.at (p) * ws;
    db += (line.accelDb.empty() ? line.levelDb.at (p) : line.accelDb.at (p)) * wa;
    db += (line.decelDb.empty() ? line.levelDb.at (p) : line.decelDb.at (p)) * wd;
    const double g = c.rangeGain (p);
    return fromDb (db) * g * g;
}

double noisePsd (const Component& c, const Model& m, double f, double p)
{
    std::vector<double> bands;
    stateBandsAt (c, p, bands);
    if (bands.empty()) return 0.0;
    const double g = c.rangeGain (p);
    return fromDb (bandValueAt (bands, bandPos (m, f))) * g * g;
}

static bool analyseHarmonic (Model& m, int index, const Audio& a, const ParamFn& param,
                             const SelectionFn& selection, juce::String* error)
{
    Component& c = m.components[(size_t) index];
    Framer fr (m.fftOrder);
    const double binHz = a.sampleRate / fr.n;
    const double tol = m.tolerancePct / 100.0;

    // The track lines' selections.
    std::vector<std::vector<SelectionShape>> shapes (c.lines.size());
    for (size_t li = 0; li < c.lines.size(); ++li)
        for (const auto& name : c.lines[li].selections)
        {
            SelectionShape s;
            if (! selection || ! selection (name, s))
            {
                if (error) *error = "Stored selection \"" + name + "\" not found.";
                return false;
            }
            shapes[li].push_back (s);
        }

    struct FrameData { double t, p, rate; std::vector<PeakMeasure> peaks; std::vector<char> measured; };
    std::vector<FrameData> frames;

    // Earlier harmonic components, to take out of the peaks.
    std::vector<const Component*> earlier;
    for (int i = 0; i < index; ++i)
    {
        const auto& e = m.components[(size_t) i];
        if (e.enabled && e.analysed && e.isHarmonic()) earlier.push_back (&e);
    }

    // Ranges: the component's own, plus the spans of its tracks' selections.
    std::vector<TimeRange> ranges = c.ranges;
    for (const auto& lineShapes : shapes)
        for (const auto& s : lineShapes) ranges.push_back ({ s.t0, s.t1 });
    if (ranges.empty())
    {
        if (error) *error = "Add a time range (or a track from stored selections) first.";
        return false;
    }

    std::vector<double> P;
    std::vector<std::pair<int64_t, int64_t>> done;   // frames already taken (overlapping ranges)
    for (const auto& r : ranges)
        for (int64_t centre : frameCentres (r, a.sampleRate, fr.n, a.numSamples))
        {
            bool dup = false;
            for (const auto& d : done) if (centre >= d.first && centre <= d.second) { dup = true; break; }
            if (dup) continue;
            const double t = (double) centre / a.sampleRate;
            bool found = true;
            const double p = paramAt (param, c.parameter, t, &found);
            if (! found)
            {
                if (error) *error = "Parameter \"" + c.parameter + "\" not found in this file's tables.";
                return false;
            }
            fr.power (a, c.channel, centre, P);

            FrameData fd;
            fd.t = t; fd.p = p; fd.rate = rateAt (param, c.parameter, t);
            fd.peaks.resize (c.lines.size());
            fd.measured.assign (c.lines.size(), 0);
            for (size_t li = 0; li < c.lines.size(); ++li)
            {
                const auto& line = c.lines[li];
                int kLo = 0, kHi = -1;
                if (line.isTrack())
                {
                    bool inside = false;
                    double fLo = 0.0, fHi = 0.0;
                    for (const auto& s : shapes[li])
                    {
                        double lo = 0.0, hi = 0.0;
                        if (t >= s.t0 && t <= s.t1 && s.bandAt && s.bandAt (t, lo, hi))
                        {
                            fLo = inside ? std::min (fLo, lo) : lo;
                            fHi = inside ? std::max (fHi, hi) : hi;
                            inside = true;
                        }
                    }
                    if (! inside) continue;
                    kLo = (int) std::ceil (fLo / binHz);
                    kHi = std::max (kLo, (int) std::floor (fHi / binHz));
                }
                else
                {
                    const double f = line.order * c.fundamentalAt (p);
                    if (! (f > 0.0)) continue;
                    kLo = (int) std::floor (std::min (f * (1.0 - tol), f - 2.0 * binHz) / binHz);
                    kHi = (int) std::ceil (std::max (f * (1.0 + tol), f + 2.0 * binHz) / binHz);
                }
                auto pk = measurePeak (P, kLo, kHi, binHz);
                if (! pk.ok) continue;
                // Take out the components above, where they fall on this peak.
                for (const auto* e : earlier)
                {
                    const double pe = paramAt (param, e->parameter, t, nullptr);
                    const double re = rateAt (param, e->parameter, t);
                    for (const auto& el : e->lines)
                    {
                        const double fe = e->lineFreqAt (el, pe);
                        if (fe > 0.0 && std::abs (fe - pk.freq) <= (kPeakHalfBins + 0.5) * binHz)
                            pk.power = std::max (0.0, pk.power - linePower (*e, el, pe, re, m));
                    }
                }
                fd.peaks[li] = pk;
                fd.measured[li] = 1;
            }
            frames.push_back (std::move (fd));
            done.push_back ({ centre - fr.n / 8, centre + fr.n / 8 });
        }

    if (frames.empty())
    {
        if (error) *error = "The time ranges are shorter than one analysis frame.";
        return false;
    }

    // Parameter range seen.
    c.pLo = c.pHi = frames.front().p;
    for (const auto& f : frames) { c.pLo = std::min (c.pLo, f.p); c.pHi = std::max (c.pHi, f.p); }

    juce::StringArray report;
    report.add (juce::String ((int) frames.size()) + " frames"
                + (c.parameter.isEmpty() ? juce::String()
                                         : ", " + c.parameter + " " + fmt (c.pLo, 2) + " to " + fmt (c.pHi, 2)));

    int counts[3] = { 0, 0, 0 };
    for (size_t li = 0; li < c.lines.size(); ++li)
    {
        auto& line = c.lines[li];
        Binner all (c.pLo, c.pHi, m.paramBins), steady (c.pLo, c.pHi, m.paramBins),
               accel (c.pLo, c.pHi, m.paramBins), decel (c.pLo, c.pHi, m.paramBins),
               freq (c.pLo, c.pHi, m.paramBins);
        double ratioSum = 0.0;
        int ratioN = 0;
        line.frames = line.framesBelowNoise = 0;
        for (const auto& f : frames)
        {
            if (! f.measured[li]) continue;
            const auto& pk = f.peaks[li];
            ++line.frames;
            if (! pk.aboveNoise) ++line.framesBelowNoise;
            all.add (f.p, pk.power);
            const Motion mo = classify (c, m, f.rate);
            if (li == 0) ++counts[mo];
            (mo == kAccel ? accel : mo == kDecel ? decel : steady).add (f.p, pk.power);
            if (pk.aboveNoise)
            {
                if (line.isTrack()) freq.add (f.p, pk.freq);
                else
                {
                    const double nominal = line.order * c.fundamentalAt (f.p);
                    if (nominal > 0.0) { ratioSum += pk.freq / nominal; ++ratioN; }
                }
            }
        }
        line.levelDb = (c.dynamics && ! steady.empty()) ? steady.curve (true) : all.curve (true);
        line.accelDb = c.dynamics ? accel.curve (true) : ParamCurve();
        line.decelDb = c.dynamics ? decel.curve (true) : ParamCurve();
        line.freqHz = line.isTrack() ? freq.curve (false) : ParamCurve();
        line.tuning = (! line.isTrack() && ratioN > 0)
                          ? juce::jlimit (1.0 - tol, 1.0 + tol, ratioSum / ratioN) : 1.0;

        // Variation: spread of the frames around the curves.
        double sLev = 0.0, sFreq = 0.0;
        int nLev = 0, nFreq = 0;
        for (const auto& f : frames)
        {
            if (! f.measured[li] || ! f.peaks[li].aboveNoise) continue;
            const auto& pk = f.peaks[li];
            const Motion mo = classify (c, m, f.rate);
            const ParamCurve& curve = (mo == kAccel && ! line.accelDb.empty()) ? line.accelDb
                                    : (mo == kDecel && ! line.decelDb.empty()) ? line.decelDb : line.levelDb;
            const double d = toDb (pk.power) - curve.at (f.p);
            sLev += d * d; ++nLev;
            const double fNom = c.lineFreqAt (line, f.p);
            if (fNom > 0.0) { const double r = pk.freq / fNom - 1.0; sFreq += r * r; ++nFreq; }
        }
        line.levelSpreadDb = nLev > 1 ? std::sqrt (sLev / nLev) : 0.0;
        line.freqSpread = nFreq > 1 ? std::sqrt (sFreq / nFreq) : 0.0;

        if (line.frames == 0)
            report.add (line.label() + ": never measured (outside the selections or above Nyquist)");
        else if (line.framesBelowNoise * 2 > line.frames)
            report.add (line.label() + ": mostly below the noise (" + juce::String (line.framesBelowNoise)
                        + " of " + juce::String (line.frames) + " frames)");
    }
    if (c.dynamics)
        report.add ("steady " + juce::String (counts[kSteady]) + ", rising " + juce::String (counts[kAccel])
                    + ", falling " + juce::String (counts[kDecel]) + " frames");
    if (! earlier.empty())
        report.add ("took out " + juce::String ((int) earlier.size()) + " harmonic component(s) above");
    c.report = report.joinIntoString ("\n");
    c.analysed = true;
    return true;
}

static bool analyseNoise (Model& m, int index, const Audio& a, const ParamFn& param, juce::String* error)
{
    Component& c = m.components[(size_t) index];
    if (c.ranges.empty())
    {
        if (error) *error = "Add a time range first: each range is one state of the noise.";
        return false;
    }
    Framer fr (m.fftOrder);
    const int half = fr.n / 2;
    const double binHz = a.sampleRate / fr.n;
    const double tol = m.tolerancePct / 100.0;

    std::vector<const Component*> earlierH, earlierN;
    for (int i = 0; i < index; ++i)
    {
        const auto& e = m.components[(size_t) i];
        if (! e.enabled || ! e.analysed) continue;
        (e.isHarmonic() ? earlierH : earlierN).push_back (&e);
    }

    std::vector<double> P;
    std::vector<char> excluded;
    std::vector<NoiseState> states;
    int frames = 0, clamped = 0, binsSeen = 0;
    double pLo = 0.0, pHi = 0.0;
    bool any = false;
    for (const auto& r : c.ranges)
    {
        // Sums per bin of the measured power and of what the noise
        // components above predict there; subtracted once averaged (a
        // per-frame subtraction leaves the periodogram's positive swings).
        std::vector<double> sum ((size_t) half + 1, 0.0), pred ((size_t) half + 1, 0.0);
        std::vector<int> cnt ((size_t) half + 1, 0);
        double pSum = 0.0;
        int n = 0;
        for (int64_t centre : frameCentres (r, a.sampleRate, fr.n, a.numSamples))
        {
            const double t = (double) centre / a.sampleRate;
            bool found = true;
            const double p = paramAt (param, c.parameter, t, &found);
            if (! found)
            {
                if (error) *error = "Parameter \"" + c.parameter + "\" not found in this file's tables.";
                return false;
            }
            fr.power (a, c.channel, centre, P);
            excluded.assign ((size_t) half + 1, 0);
            for (const auto* e : earlierH)
            {
                const double pe = paramAt (param, e->parameter, t, nullptr);
                for (const auto& el : e->lines)
                {
                    const double fe = e->lineFreqAt (el, pe);
                    if (! (fe > 0.0)) continue;
                    const int k0 = std::max (0, (int) std::floor (fe * (1.0 - tol) / binHz) - 4);
                    const int k1 = std::min (half, (int) std::ceil (fe * (1.0 + tol) / binHz) + 4);
                    for (int k = k0; k <= k1; ++k) excluded[(size_t) k] = 1;
                }
            }
            std::vector<double> pe (earlierN.size());
            for (size_t e = 0; e < earlierN.size(); ++e)
                pe[e] = paramAt (param, earlierN[e]->parameter, t, nullptr);
            for (int k = 1; k < half; ++k)
            {
                for (size_t e = 0; e < earlierN.size(); ++e)
                    pred[(size_t) k] += noisePsd (*earlierN[e], m, k * binHz, pe[e]);
                if (excluded[(size_t) k]) continue;
                sum[(size_t) k] += P[(size_t) k];
                ++cnt[(size_t) k];
            }
            pSum += p;
            ++n;
            pLo = any ? std::min (pLo, p) : p;
            pHi = any ? std::max (pHi, p) : p;
            any = true;
        }
        if (n == 0) continue;
        frames += n;

        // Per-bin PSD; bins never seen (notched in every frame) bridged linearly.
        std::vector<double> psd ((size_t) half + 1, -1.0);
        for (int k = 1; k < half; ++k)
            if (cnt[(size_t) k] > 0) psd[(size_t) k] = sum[(size_t) k] / cnt[(size_t) k] / binHz;
        for (auto& v : pred) v /= n;   // mean PSD predicted by the noise components above
        int prev = -1;
        for (int k = 1; k < half; ++k)
        {
            if (psd[(size_t) k] < 0.0) continue;
            if (prev >= 0 && k - prev > 1)
                for (int j = prev + 1; j < k; ++j)
                    psd[(size_t) j] = psd[(size_t) prev] + (psd[(size_t) k] - psd[(size_t) prev]) * (j - prev) / (double) (k - prev);
            else if (prev < 0)
                for (int j = 1; j < k; ++j) psd[(size_t) j] = psd[(size_t) k];
            prev = k;
        }
        if (prev < 0) continue;
        for (int k = prev + 1; k <= half; ++k) psd[(size_t) k] = psd[(size_t) prev];
        psd[0] = psd[1];

        // Bands.
        NoiseState st;
        st.range = r;
        st.param = pSum / n;
        st.psdDb.resize (m.bandHz.size());
        // Band means; the noise components above are subtracted here, from
        // the band means (their own spectra are band means: subtracting bin
        // by bin would leave every bin's swing around the band's mean).
        auto bandMean = [&] (const std::vector<double>& v, double fc)
        {
            const double lo = fc * std::pow (2.0, -0.5 / m.bandsPerOctave);
            const double hi = fc * std::pow (2.0, 0.5 / m.bandsPerOctave);
            double s = 0.0;
            int nb = 0;
            for (int k = std::max (1, (int) std::ceil (lo / binHz)); k <= half && k * binHz < hi; ++k)
            {
                s += v[(size_t) k];
                ++nb;
            }
            if (nb > 0) return s / nb;
            const double pos = fc / binHz;
            const int k = juce::jlimit (0, half - 1, (int) pos);
            return v[(size_t) k] + (pos - k) * (v[(size_t) k + 1] - v[(size_t) k]);
        };
        pred[0] = pred[1];
        pred[(size_t) half] = pred[(size_t) half - 1];
        for (size_t b = 0; b < m.bandHz.size(); ++b)
        {
            const double measured = bandMean (psd, m.bandHz[b]);
            double v = measured;
            if (! earlierN.empty())
            {
                v -= bandMean (pred, m.bandHz[b]);
                ++binsSeen;
                if (v < 0.01 * measured) { v = 0.01 * measured; ++clamped; }   // over-subtraction: -20 dB floor
            }
            st.psdDb[b] = (float) toDb (v);
        }
        states.push_back (std::move (st));
    }
    if (states.empty())
    {
        if (error) *error = "The time ranges are shorter than one analysis frame.";
        return false;
    }
    std::sort (states.begin(), states.end(), [] (const NoiseState& x, const NoiseState& y) { return x.param < y.param; });
    c.states = std::move (states);
    c.pLo = pLo;
    c.pHi = pHi;

    juce::StringArray report;
    report.add (juce::String ((int) c.states.size()) + " state(s), " + juce::String (frames) + " frames"
                + (c.parameter.isEmpty() ? juce::String()
                                         : ", " + c.parameter + " " + fmt (pLo, 2) + " to " + fmt (pHi, 2)));
    if (! earlierH.empty())
        report.add ("notched out " + juce::String ((int) earlierH.size()) + " harmonic component(s) above");
    if (! earlierN.empty())
        report.add ("subtracted " + juce::String ((int) earlierN.size()) + " noise component(s) above");
    if (binsSeen > 0 && clamped * 20 > binsSeen)
        report.add ("over-subtraction in " + fmt (100.0 * clamped / binsSeen, 0)
                    + " % of the bands: the components above are louder than this range");
    c.report = report.joinIntoString ("\n");
    c.analysed = true;
    return true;
}

bool analyse (Model& m, int index, const Audio& a, const ParamFn& param, const SelectionFn& selection,
              juce::String* error)
{
    if (index < 0 || index >= (int) m.components.size())
    {
        if (error) *error = "No such component.";
        return false;
    }
    if (! a.read || a.sampleRate <= 0.0 || a.numSamples <= 0)
    {
        if (error) *error = "No audio to analyse.";
        return false;
    }
    Component& c = m.components[(size_t) index];
    if (c.channel < 0 || c.channel >= a.numChannels)
    {
        if (error) *error = "Channel " + juce::String (c.channel + 1) + " does not exist in this file.";
        return false;
    }
    m.fftOrder = juce::jlimit (9, 16, m.fftOrder);
    if (std::abs (m.analysisRate - a.sampleRate) > 1e-6 || m.bandHz.empty())
    {
        // A new rate changes the noise bands: earlier noise states no longer fit.
        m.analysisRate = a.sampleRate;
        makeBands (m);
        for (auto& o : m.components)
            if (! o.isHarmonic()) { o.analysed = false; o.states.clear(); }
    }
    c.analysed = false;
    c.report.clear();
    return c.isHarmonic() ? analyseHarmonic (m, index, a, param, selection, error)
                          : analyseNoise (m, index, a, param, error);
}

bool analyseAll (Model& m, const Audio& a, const ParamFn& param, const SelectionFn& selection,
                 juce::String* error)
{
    for (int i = 0; i < (int) m.components.size(); ++i)
    {
        if (! m.components[(size_t) i].enabled) continue;
        juce::String err;
        if (! analyse (m, i, a, param, selection, &err))
        {
            if (error) *error = m.components[(size_t) i].name + ": " + err;
            return false;
        }
    }
    return true;
}

// ================================================================ rendering

namespace
{
    /** Slow random modulation with unit standard deviation (~3 Hz). */
    struct Wander
    {
        juce::Random rng;
        double y = 0.0, a = 0.0, b = 0.0;
        Wander (int64_t seed, double controlRate) : rng (seed)
        {
            a = std::exp (-2.0 * juce::MathConstants<double>::pi * 3.0 / controlRate);
            b = std::sqrt (1.0 - a * a);
            for (int i = 0; i < 64; ++i) next();
        }
        double gauss()
        {
            // Box-Muller.
            const double u1 = std::max (1e-12, (double) rng.nextFloat()), u2 = rng.nextFloat();
            return std::sqrt (-2.0 * std::log (u1)) * std::cos (2.0 * juce::MathConstants<double>::pi * u2);
        }
        double next() { y = a * y + b * gauss(); return y; }
    };

    void renderHarmonic (const Model& m, const Component& c, int ci, double tStart, int64_t numSamples,
                         double sr, const ParamFn& param, float* out, uint32_t seed)
    {
        constexpr int kBlock = 64;
        const double controlRate = sr / kBlock;
        const double nyq = sr * 0.5;
        for (size_t li = 0; li < c.lines.size(); ++li)
        {
            const auto& line = c.lines[li];
            if (line.levelDb.empty()) continue;
            juce::Random rng ((int64_t) seed * 7919 + ci * 131 + (int64_t) li);
            double phase = rng.nextDouble() * 2.0 * juce::MathConstants<double>::pi;
            Wander amp ((int64_t) seed * 104729 + ci * 17 + (int64_t) li * 3 + 1, controlRate);
            Wander fm  ((int64_t) seed * 1299709 + ci * 19 + (int64_t) li * 5 + 2, controlRate);

            auto state = [&] (int64_t i, double& A, double& f)
            {
                const double t = tStart + (double) i / sr;
                const double p = paramAt (param, c.parameter, t, nullptr);
                const double rate = rateAt (param, c.parameter, t);
                double power = linePower (c, line, p, rate, m);
                f = c.lineFreqAt (line, p);
                if (c.variation)
                {
                    power *= fromDb (amp.next() * line.levelSpreadDb);
                    f *= 1.0 + fm.next() * line.freqSpread;
                }
                // Fade out towards Nyquist.
                if (f >= nyq * 0.98 || ! (f > 0.0)) power = 0.0;
                else if (f > nyq * 0.9) power *= (nyq * 0.98 - f) / (nyq * 0.08);
                A = std::sqrt (2.0 * power);
            };

            double A0 = 0.0, f0 = 0.0;
            state (0, A0, f0);
            for (int64_t s = 0; s < numSamples; s += kBlock)
            {
                const int n = (int) std::min<int64_t> (kBlock, numSamples - s);
                double A1 = 0.0, f1 = 0.0;
                state (s + n, A1, f1);
                for (int i = 0; i < n; ++i)
                {
                    const double fr = (double) i / n;
                    const double A = A0 + (A1 - A0) * fr;
                    const double f = f0 + (f1 - f0) * fr;
                    out[s + i] += (float) (A * std::sin (phase));
                    phase += 2.0 * juce::MathConstants<double>::pi * f / sr;
                    if (phase > 1e4) phase = std::fmod (phase, 2.0 * juce::MathConstants<double>::pi);
                }
                A0 = A1;
                f0 = f1;
            }
        }
    }

    void renderNoise (const Model& m, const Component& c, int ci, double tStart, int64_t numSamples,
                      double sr, const ParamFn& param, float* out, uint32_t seed)
    {
        if (c.states.empty() || m.bandHz.empty()) return;
        const int order = sr > 50000.0 ? 12 : 11;
        const int n = 1 << order, hop = n / 2, half = n / 2;
        juce::dsp::FFT fft (order);
        std::vector<float> win ((size_t) n);
        for (int i = 0; i < n; ++i) win[(size_t) i] = (float) std::sin (juce::MathConstants<double>::pi * i / n);

        // White noise, continuous across frames (unit variance).
        juce::Random rng ((int64_t) seed * 15485863 + ci * 977 + 5);
        const int64_t first = -hop;                   // first frame start
        const int64_t total = numSamples + n + hop;   // samples of noise needed
        std::vector<float> noise ((size_t) total);
        for (auto& v : noise)
        {
            const double u1 = std::max (1e-12, (double) rng.nextFloat()), u2 = rng.nextFloat();
            v = (float) (std::sqrt (-2.0 * std::log (u1)) * std::cos (2.0 * juce::MathConstants<double>::pi * u2));
        }

        std::vector<double> pos ((size_t) half + 1);
        for (int k = 0; k <= half; ++k) pos[(size_t) k] = bandPos (m, std::max (1.0, k * sr / n));
        std::vector<float> buf ((size_t) n * 2);
        std::vector<double> bands;
        for (int64_t start = first; start < numSamples; start += hop)
        {
            const double t = tStart + (double) (start + n / 2) / sr;
            const double p = paramAt (param, c.parameter, t, nullptr);
            const double g = c.rangeGain (p);
            if (g <= 0.0) continue;
            stateBandsAt (c, p, bands);
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < n; ++i)
                buf[(size_t) i] = noise[(size_t) (start - first + i)] * win[(size_t) i];
            fft.performRealOnlyForwardTransform (buf.data(), true);
            for (int k = 0; k <= half; ++k)
            {
                // Unit-variance white noise has a one-sided PSD of 2 / sr.
                const double psd = fromDb (bandValueAt (bands, pos[(size_t) k])) * g * g;
                const float gain = (float) std::sqrt (psd * sr * 0.5);
                buf[(size_t) k * 2] *= gain;
                buf[(size_t) k * 2 + 1] *= gain;
            }
            buf[0] = buf[1] = 0.0f;   // no DC
            fft.performRealOnlyInverseTransform (buf.data());
            for (int i = 0; i < n; ++i)
            {
                const int64_t s = start + i;
                if (s >= 0 && s < numSamples) out[s] += buf[(size_t) i] * win[(size_t) i];
            }
        }
    }
}

void render (const Model& m, double tStart, int64_t numSamples, double sampleRate, const ParamFn& param,
             float* out, uint32_t seed, int only)
{
    if (numSamples <= 0 || sampleRate <= 0.0 || out == nullptr) return;
    for (int i = 0; i < (int) m.components.size(); ++i)
    {
        const auto& c = m.components[(size_t) i];
        if (only >= 0 ? i != only : ! c.enabled) continue;
        if (! c.analysed) continue;
        if (c.isHarmonic()) renderHarmonic (m, c, i, tStart, numSamples, sampleRate, param, out, seed);
        else                renderNoise (m, c, i, tStart, numSamples, sampleRate, param, out, seed);
    }
}

// ===================================================================== JSON

namespace
{
    juce::var curveVar (const ParamCurve& c, const char* valueKey)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        juce::Array<juce::var> p, v;
        for (double x : c.p) p.add (x);
        for (double x : c.v) v.add (x);
        o->setProperty ("p", p);
        o->setProperty (valueKey, v);
        return o.get();
    }

    ParamCurve curveFrom (const juce::var& v, const char* valueKey)
    {
        ParamCurve c;
        if (auto* p = v["p"].getArray())
            for (const auto& x : *p) c.p.push_back ((double) x);
        if (auto* a = v[valueKey].getArray())
            for (const auto& x : *a) c.v.push_back ((double) x);
        if (c.p.size() != c.v.size()) { c.p.clear(); c.v.clear(); }
        return c;
    }

    juce::var floatsVar (const std::vector<float>& v)
    {
        juce::Array<juce::var> a;
        for (float x : v) a.add (std::round (x * 1000.0f) / 1000.0f);
        return a;
    }
}

juce::var Model::toVar() const
{
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty ("format", "SpectroCraft sound model");
    root->setProperty ("version", kFormatVersion);
    root->setProperty ("units", "harmonic levels: RMS dBFS (a full-scale sine is -3.01); noise: "
                                "power spectral density in dB re full scale per Hz");
    root->setProperty ("synthesis",
                       "harmonic line: f = order * f0Hz * p / f0AtParameter * tuning (track: freq curve), "
                       "amplitude = sqrt(2 * 10^(level/10)), levels interpolated linearly in p; noise: white "
                       "noise shaped by the states' spectra, interpolated in dB between states by p; a "
                       "component fades out between 5 % and 15 % of its learned span below learnedRange[0]");

    juce::DynamicObject::Ptr an = new juce::DynamicObject();
    an->setProperty ("sampleRate", analysisRate);
    an->setProperty ("fftSize", 1 << fftOrder);
    an->setProperty ("tolerancePct", tolerancePct);
    an->setProperty ("steadyRatePct", steadyRatePct);
    an->setProperty ("parameterBins", paramBins);
    an->setProperty ("bandsPerOctave", bandsPerOctave);
    root->setProperty ("analysis", an.get());
    root->setProperty ("bandsHz", floatsVar (bandHz));

    juce::Array<juce::var> comps;
    for (const auto& c : components)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("id", c.id);
        o->setProperty ("name", c.name);
        o->setProperty ("type", c.isHarmonic() ? "harmonic" : "noise");
        o->setProperty ("enabled", c.enabled);
        o->setProperty ("parameter", c.parameter);
        o->setProperty ("channel", c.channel);
        juce::Array<juce::var> ranges;
        for (const auto& r : c.ranges) ranges.add (juce::Array<juce::var> { r.t0, r.t1 });
        o->setProperty ("ranges", ranges);
        o->setProperty ("analysed", c.analysed);
        o->setProperty ("learnedRange", juce::Array<juce::var> { c.pLo, c.pHi });
        o->setProperty ("report", c.report);
        if (c.isHarmonic())
        {
            juce::DynamicObject::Ptr f0 = new juce::DynamicObject();
            f0->setProperty ("hz", c.f0Hz);
            f0->setProperty ("atParameter", c.f0AtParam);
            o->setProperty ("fundamental", f0.get());
            o->setProperty ("dynamics", c.dynamics);
            o->setProperty ("variation", c.variation);
            juce::Array<juce::var> lines;
            for (const auto& l : c.lines)
            {
                juce::DynamicObject::Ptr lo = new juce::DynamicObject();
                lo->setProperty ("order", l.order);
                if (l.isTrack())
                {
                    juce::Array<juce::var> sel;
                    for (const auto& s : l.selections) sel.add (s);
                    lo->setProperty ("selections", sel);
                    lo->setProperty ("freq", curveVar (l.freqHz, "hz"));
                }
                lo->setProperty ("tuning", l.tuning);
                lo->setProperty ("level", curveVar (l.levelDb, "db"));
                if (! l.accelDb.empty()) lo->setProperty ("accel", curveVar (l.accelDb, "db"));
                if (! l.decelDb.empty()) lo->setProperty ("decel", curveVar (l.decelDb, "db"));
                lo->setProperty ("levelSpreadDb", l.levelSpreadDb);
                lo->setProperty ("freqSpread", l.freqSpread);
                lo->setProperty ("frames", l.frames);
                lo->setProperty ("framesBelowNoise", l.framesBelowNoise);
                lines.add (lo.get());
            }
            o->setProperty ("lines", lines);
        }
        else
        {
            juce::Array<juce::var> states;
            for (const auto& s : c.states)
            {
                juce::DynamicObject::Ptr so = new juce::DynamicObject();
                so->setProperty ("t0", s.range.t0);
                so->setProperty ("t1", s.range.t1);
                so->setProperty ("parameter", s.param);
                so->setProperty ("psdDb", floatsVar (s.psdDb));
                states.add (so.get());
            }
            o->setProperty ("states", states);
        }
        comps.add (o.get());
    }
    root->setProperty ("components", comps);
    return root.get();
}

bool Model::fromVar (const juce::var& v, Model& out, juce::String* error)
{
    if (! v.isObject() || v["format"].toString() != "SpectroCraft sound model")
    {
        if (error) *error = "Not a SpectroCraft sound model.";
        return false;
    }
    if ((int) v["version"] > kFormatVersion)
    {
        if (error) *error = "This sound model was saved by a newer SpectroCraft.";
        return false;
    }
    Model m;
    const auto an = v["analysis"];
    m.analysisRate = (double) an["sampleRate"];
    const int fft = (int) an.getProperty ("fftSize", 4096);
    m.fftOrder = juce::jlimit (9, 16, (int) std::lround (std::log2 (std::max (2, fft))));
    m.tolerancePct = (double) an.getProperty ("tolerancePct", 3.0);
    m.steadyRatePct = (double) an.getProperty ("steadyRatePct", 2.0);
    m.paramBins = juce::jlimit (1, 200, (int) an.getProperty ("parameterBins", 24));
    m.bandsPerOctave = juce::jlimit (1, 96, (int) an.getProperty ("bandsPerOctave", 24));
    if (auto* b = v["bandsHz"].getArray())
        for (const auto& x : *b) m.bandHz.push_back ((float) (double) x);

    if (auto* comps = v["components"].getArray())
        for (const auto& cv : *comps)
        {
            Component c;
            c.id = cv["id"].toString();
            c.name = cv["name"].toString();
            c.kind = cv["type"].toString() == "noise" ? Kind::Noise : Kind::Harmonic;
            c.enabled = (bool) cv.getProperty ("enabled", true);
            c.parameter = cv["parameter"].toString();
            c.channel = std::max (0, (int) cv["channel"]);
            if (auto* rs = cv["ranges"].getArray())
                for (const auto& r : *rs)
                    if (r.isArray() && r.size() == 2) c.ranges.push_back ({ (double) r[0], (double) r[1] });
            c.analysed = (bool) cv["analysed"];
            if (auto* lr = cv["learnedRange"].getArray(); lr != nullptr && lr->size() == 2)
            {
                c.pLo = (double) (*lr)[0];
                c.pHi = (double) (*lr)[1];
            }
            c.report = cv["report"].toString();
            if (c.isHarmonic())
            {
                c.f0Hz = (double) cv["fundamental"].getProperty ("hz", 100.0);
                c.f0AtParam = (double) cv["fundamental"].getProperty ("atParameter", 100.0);
                c.dynamics = (bool) cv["dynamics"];
                c.variation = (bool) cv["variation"];
                if (auto* ls = cv["lines"].getArray())
                    for (const auto& lv : *ls)
                    {
                        HarmonicLine l;
                        l.order = (double) lv.getProperty ("order", 1.0);
                        if (auto* sel = lv["selections"].getArray())
                            for (const auto& s : *sel) l.selections.push_back (s.toString());
                        l.tuning = (double) lv.getProperty ("tuning", 1.0);
                        l.levelDb = curveFrom (lv["level"], "db");
                        l.accelDb = curveFrom (lv["accel"], "db");
                        l.decelDb = curveFrom (lv["decel"], "db");
                        l.freqHz = curveFrom (lv["freq"], "hz");
                        l.levelSpreadDb = (double) lv["levelSpreadDb"];
                        l.freqSpread = (double) lv["freqSpread"];
                        l.frames = (int) lv["frames"];
                        l.framesBelowNoise = (int) lv["framesBelowNoise"];
                        c.lines.push_back (l);
                    }
            }
            else if (auto* ss = cv["states"].getArray())
                for (const auto& sv : *ss)
                {
                    NoiseState s;
                    s.range = { (double) sv["t0"], (double) sv["t1"] };
                    s.param = (double) sv["parameter"];
                    if (auto* p = sv["psdDb"].getArray())
                        for (const auto& x : *p) s.psdDb.push_back ((float) (double) x);
                    if (! s.psdDb.empty() && s.psdDb.size() != m.bandHz.size()) c.analysed = false;
                    c.states.push_back (s);
                }
            if (c.id.isEmpty() || m.indexOf (c.id) >= 0)
                c.id = "c" + juce::String ((int) m.components.size() + 1) + "_" + juce::String (juce::Random::getSystemRandom().nextInt (100000));
            m.components.push_back (c);
        }
    out = std::move (m);
    return true;
}

juce::String Model::toJson() const
{
    return juce::JSON::toString (toVar(), false, 6);
}

bool Model::fromJson (const juce::String& text, Model& out, juce::String* error)
{
    juce::var v;
    const auto res = juce::JSON::parse (text, v);
    if (res.failed())
    {
        if (error) *error = "Not valid JSON: " + res.getErrorMessage();
        return false;
    }
    return fromVar (v, out, error);
}
}
