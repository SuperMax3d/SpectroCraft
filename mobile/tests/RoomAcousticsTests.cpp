// SpectroCraft - unit tests for the reverberation analysis (include/RoomAcoustics.h):
// synthetic rooms whose reverberation time is known, measured as an impulse and
// through a sweep played by a distorting loudspeaker into a noisy recording.
#include <JuceHeader.h>
#include "RoomAcoustics.h"
#include <cmath>
#include <random>

namespace
{
using namespace sc::room;

constexpr double kFs = 48000.0;

/** A room: a direct sound, then noise that decays by 60 dB in `t60` seconds
    (two decays when `t60b` is given, the second `second` times the first's amplitude). */
std::vector<float> roomResponse (double t60, double seconds, unsigned seed, double direct = 1.0, double reverb = 0.05,
                                 double t60b = 0.0, double second = 0.0)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> gauss (0.0, 1.0);
    std::vector<float> h ((size_t) (seconds * kFs));
    for (size_t i = 0; i < h.size(); ++i)
    {
        const double t = (double) i / kFs;
        double envelope = std::exp (-6.9078 * t / t60);
        if (t60b > 0.0) envelope += second * std::exp (-6.9078 * t / t60b);
        h[i] = (float) (reverb * envelope * gauss (rng));
    }
    h[0] = (float) direct;
    return h;
}

void addNoise (std::vector<float>& x, double rms, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> gauss (0.0, rms);
    for (auto& v : x) v += (float) gauss (rng);
}

/** x convolved with h (FFT). */
std::vector<float> convolve (const std::vector<float>& x, const std::vector<float>& h)
{
    int order = 1;
    while (((size_t) 1 << order) < x.size() + h.size()) ++order;
    const size_t m = (size_t) 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> a (2 * m, 0.0f), b (2 * m, 0.0f);
    std::copy (x.begin(), x.end(), a.begin());
    std::copy (h.begin(), h.end(), b.begin());
    fft.performRealOnlyForwardTransform (a.data(), false);
    fft.performRealOnlyForwardTransform (b.data(), false);
    for (size_t k = 0; k < m; ++k)
    {
        const float re = a[2 * k] * b[2 * k] - a[2 * k + 1] * b[2 * k + 1];
        const float im = a[2 * k] * b[2 * k + 1] + a[2 * k + 1] * b[2 * k];
        a[2 * k] = re;
        a[2 * k + 1] = im;
    }
    fft.performRealOnlyInverseTransform (a.data());
    a.resize (x.size() + h.size() - 1);
    return a;
}

const BandResult* band (const std::vector<BandResult>& r, double nominal)
{
    for (const auto& b : r)
        if (std::abs (b.nominalHz - nominal) < 1.0e-6) return &b;
    return nullptr;
}

juce::String describe (const BandResult& b)
{
    return juce::String (b.nominalHz, 0) + " Hz: EDT " + juce::String (b.edt.seconds, 3) + " T20 " + juce::String (b.t20.seconds, 3)
           + " T30 " + juce::String (b.t30.seconds, 3) + " INR " + juce::String (b.inrDb, 1) + " C50 " + juce::String (b.c50, 2)
           + " curv " + juce::String (b.curvaturePct, 1) + " trunc " + juce::String (b.truncationSeconds, 2)
           + (b.t20.reliable ? " T20ok" : "") + (b.t30.reliable ? " T30ok" : "") + (b.filterLimited ? " filterLimited" : "");
}

class RoomAcousticsTests : public juce::UnitTest
{
public:
    RoomAcousticsTests() : juce::UnitTest ("reverberation", "SpectroCraftMobile") {}

    void runTest() override
    {
        const bool verbose = juce::SystemStats::getEnvironmentVariable ("SC_ROOM_VERBOSE", {}).isNotEmpty();
        const auto show = [this, verbose] (const std::vector<BandResult>& r)
        {
            if (verbose)
                for (const auto& b : r) logMessage ("  " + describe (b));
        };

        beginTest ("the sweep: its length, its level, its description");
        {
            SweepSpec s;
            s.seconds = 2.0;
            s.level = 0.4;
            const auto x = makeSweep (s, kFs);
            expectEquals ((int) x.size(), 96000);
            float top = 0.0f;
            for (float v : x) top = std::max (top, std::abs (v));
            expect (top > 0.39f && top <= 0.4001f, juce::String (top));
            expect (std::abs (x.front()) < 1.0e-4f && std::abs (x.back()) < 1.0e-3f, "faded at both ends");
            // one period of 20 Hz at the start, far more zero crossings at the end
            int early = 0, late = 0;
            for (size_t i = 1; i < 4800; ++i) if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) ++early;
            for (size_t i = x.size() - 4800; i < x.size(); ++i) if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) ++late;
            expect (early >= 3 && early <= 8, juce::String (early));
            expect (late > 2500, juce::String (late));

            SweepSpec back;
            expect (SweepSpec::fromVar (s.toVar(), back));
            expectEquals (back.seconds, 2.0);
            expectEquals (back.level, 0.4);
            expect (! SweepSpec::fromVar (juce::var ("x"), back));
            expect (makeSweep (s, 8000.0).size() == 16000, "the top frequency follows a low sample rate");
        }

        beginTest ("a decay of 1.2 s is found in every octave (impulse, no noise)");
        {
            const auto h = roomResponse (1.2, 3.0, 11);
            const auto ir = fromImpulse (h.data(), (juce::int64) h.size(), kFs);
            expect (ir.ok(), ir.problem);
            expectEquals (ir.direct, ir.peak, "the direct sound is the peak");
            const auto r = analyse (ir);
            show (r);
            expectEquals ((int) r.size(), 9, "the whole response, then 63 Hz to 8 kHz");
            expectEquals (r[0].nominalHz, 0.0);
            for (const auto& b : r)
            {
                if (b.nominalHz > 0.0 && b.nominalHz < 125.0) continue;   // too few modes in a narrow band for 5 %
                expectWithinAbsoluteError (b.t20.seconds, 1.2, 0.09, describe (b));
                expectWithinAbsoluteError (b.t30.seconds, 1.2, 0.09, describe (b));
                expect (b.t20.reliable && b.t30.reliable && ! b.filterLimited, describe (b));
                expect (std::abs (b.curvaturePct) < 8.0, describe (b));
                expect (b.t30.r2 > 0.99, describe (b));
            }
            // the whole response: the direct sound's energy 1 against 0.05^2 x 1.2 x 48000 / 13.8 of reverberation
            const double reverb = 0.0025 * 1.2 * kFs / 13.8155, late = reverb * std::exp (-13.8155 * 0.05 / 1.2);
            expectWithinAbsoluteError (r[0].c50, 10.0 * std::log10 ((1.0 + reverb - late) / late), 0.5);
            expectWithinAbsoluteError (r[0].d50, (1.0 + reverb - late) / (1.0 + reverb), 0.03);
            expect (r[0].centreTimeMs > 40.0 && r[0].centreTimeMs < 120.0, juce::String (r[0].centreTimeMs));
            expect (! r[0].decayDb.empty() && r[0].decayDb.front() == 0.0f && r[0].decayDb.back() < -40.0f);

            const auto thirds = analyse (ir, { Bands::ThirdOctave, 500.0, 2000.0, false });
            expectEquals ((int) thirds.size(), 7, "500 Hz to 2 kHz in 1/3 octaves");
            for (const auto& b : thirds) expectWithinAbsoluteError (b.t20.seconds, 1.2, 0.12, describe (b));
        }

        beginTest ("background noise: the decay is cut where it meets the noise, and T30 is not trusted");
        {
            auto h = roomResponse (1.2, 3.0, 12, 0.05);   // no strong direct sound: the range is the reverberation's
            addNoise (h, 0.05 * 0.01, 99);                 // 40 dB under the start of the decay
            const auto ir = fromImpulse (h.data(), (juce::int64) h.size(), kFs);
            const auto r = analyse (ir);
            show (r);
            for (const auto& b : r)
            {
                if (b.nominalHz > 0.0 && b.nominalHz < 250.0) continue;
                expect (b.noiseFound, describe (b));
                expect (b.inrDb > 35.0 && b.inrDb < 45.0, describe (b));
                expectWithinAbsoluteError (b.t20.seconds, 1.2, 0.1, describe (b));
                expect (b.t20.reliable && ! b.t30.reliable, describe (b));
                expect (b.truncationSeconds > 0.55 && b.truncationSeconds < 1.1, describe (b));   // 40 dB of 1.2 s: 0.8 s
            }
        }

        beginTest ("a sweep through a distorting loudspeaker into a noisy recording gives the same room");
        {
            SweepSpec spec;
            spec.seconds = 6.0;
            const auto h = roomResponse (1.2, 3.0, 13);
            auto played = makeSweep (spec, kFs);
            for (auto& v : played) v = v + 0.2f * v * v + 0.4f * v * v * v;   // 2nd and 3rd harmonics
            auto heard = convolve (played, h);
            std::vector<float> rec ((size_t) (0.7 * kFs), 0.0f);              // the recording starts 0.7 s before the sweep
            rec.insert (rec.end(), heard.begin(), heard.end());
            rec.resize ((size_t) ((0.7 + 6.0 + 3.0) * kFs));
            addNoise (rec, 2.0e-4, 5);
            const auto ir = fromSweep (rec.data(), (juce::int64) rec.size(), kFs, spec);
            expect (ir.ok(), ir.problem);
            expectWithinAbsoluteError (ir.startInRecording + (double) ir.peak / kFs, 0.7, 0.001, "where the sweep starts");
            expect (ir.peak - ir.direct <= 8, "the direct sound starts at the peak (band-limited: a few samples early)");
            expect (ir.lowHz > 20.0 && ir.lowHz < 30.0 && ir.highHz > 16000.0, "the sweep's range");
            const auto r = analyse (ir);
            show (r);
            for (const auto& b : r)
            {
                if (b.nominalHz > 0.0 && b.nominalHz < 125.0) continue;
                expectWithinAbsoluteError (b.t20.seconds, 1.2, 0.09, describe (b));
                expectWithinAbsoluteError (b.t30.seconds, 1.2, 0.09, describe (b));
                expect (b.t20.reliable && b.t30.reliable, describe (b));
            }
            const double reverb = 0.0025 * 1.2 * kFs / 13.8155, late = reverb * std::exp (-13.8155 * 0.05 / 1.2);
            expectWithinAbsoluteError (r[0].c50, 10.0 * std::log10 ((1.0 + reverb - late) / late), 0.7);

            // what cannot be analysed says why
            expect (fromSweep (rec.data(), (juce::int64) (6.1 * kFs), kFs, spec).problem.contains ("shorter"));
            expect (fromSweep (rec.data(), (juce::int64) (6.8 * kFs), kFs, spec).problem.contains ("too soon"));
            std::vector<float> noise ((size_t) (9.0 * kFs), 0.0f);
            addNoise (noise, 0.1, 6);
            expect (fromSweep (noise.data(), (juce::int64) noise.size(), kFs, spec).problem.contains ("No sweep"));
            expect (! fromImpulse (noise.data(), 0, kFs).ok());
        }

        beginTest ("a sweep file played in a loop: any recording of two periods holds one whole sweep");
        {
            SweepSpec spec;
            spec.seconds = 4.0;
            spec.silenceSeconds = 4.0;
            const auto h = roomResponse (0.8, 2.0, 21);
            const auto sweep = makeSweep (spec, kFs);
            const size_t period = (size_t) (8.0 * kFs);
            std::vector<float> played (4 * period, 0.0f);
            for (size_t k = 0; k < 4; ++k) std::copy (sweep.begin(), sweep.end(), played.begin() + (std::ptrdiff_t) (k * period));
            const auto heard = convolve (played, h);
            // recording from the middle of the second sweep, for two periods and a second
            const size_t from = period + (size_t) (2.5 * kFs);
            std::vector<float> rec (heard.begin() + (std::ptrdiff_t) from, heard.begin() + (std::ptrdiff_t) (from + 2 * period + (size_t) kFs));
            addNoise (rec, 1.0e-4, 7);
            const auto ir = fromSweep (rec.data(), (juce::int64) rec.size(), kFs, spec);
            expect (ir.ok(), ir.problem);
            expectWithinAbsoluteError (ir.startInRecording + (double) ir.peak / kFs, 5.5, 0.001, "the third sweep, the first whole one");
            const double seconds = (double) (ir.h.size() - (size_t) ir.peak) / kFs;
            expect (seconds > 6.8 && seconds < 7.0, "stops before the next sweep's distortion: " + juce::String (seconds, 2) + " s");
            const auto r = analyse (ir);
            show (r);
            if (const auto* mid = band (r, 1000.0))
            {
                expectWithinAbsoluteError (mid->t20.seconds, 0.8, 0.07, describe (*mid));
                expect (mid->t20.reliable, describe (*mid));
            }
        }

        beginTest ("a decay too short for a narrow band is the filter's, and says so");
        {
            const auto h = roomResponse (0.1, 1.0, 14);
            const auto ir = fromImpulse (h.data(), (juce::int64) h.size(), kFs);
            const auto r = analyse (ir);
            show (r);
            const auto* low = band (r, 63.0);     // 44 Hz wide x 0.1 s = 4.4
            const auto* mid = band (r, 1000.0);   // 705 Hz wide x 0.1 s = 70
            expect (low != nullptr && mid != nullptr);
            if (low != nullptr) expect (low->filterLimited && ! low->t20.reliable && ! low->edt.reliable, describe (*low));
            if (mid != nullptr)
            {
                expect (! mid->filterLimited && mid->t20.reliable, describe (*mid));
                expectWithinAbsoluteError (mid->t20.seconds, 0.1, 0.012, describe (*mid));
            }
        }

        beginTest ("two slopes (coupled or open spaces) show as curvature");
        {
            const auto h = roomResponse (0.4, 4.0, 15, 0.05, 0.05, 2.0, 0.1);
            const auto ir = fromImpulse (h.data(), (juce::int64) h.size(), kFs);
            const auto r = analyse (ir);
            show (r);
            const auto* mid = band (r, 1000.0);
            expect (mid != nullptr);
            if (mid != nullptr)
            {
                expect (mid->curvaturePct > 10.0, describe (*mid));
                expect (mid->t30.seconds > mid->t20.seconds && mid->edt.seconds < mid->t20.seconds, describe (*mid));
            }
        }

        beginTest ("the bands follow the options and what the measurement covers");
        {
            ImpulseResponse ir;
            ir.h.assign (4800, 0.0f);
            ir.h[100] = 1.0f;
            ir.sampleRate = kFs;
            expectEquals ((int) bandCentres (ir, {}).size(), 8);
            expectEquals ((int) bandCentres (ir, { Bands::ThirdOctave, 50.0, 10000.0, true }).size(), 24);
            ir.lowHz = 100.0;
            ir.highHz = 5000.0;
            const auto c = bandCentres (ir, {});
            expect (c.size() == 4 && c.front() == 250.0 && c.back() == 2000.0, "octaves inside 100 Hz to 5 kHz");
            ir.problem = "x";
            expect (analyse (ir).empty());
        }
    }
};

static RoomAcousticsTests roomAcousticsTests;
} // namespace
