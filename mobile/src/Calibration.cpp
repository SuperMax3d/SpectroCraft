#include "Calibration.h"
#include <cmath>
#include <vector>

namespace scm::cal
{
double fullScaleDbSpl (double fullScalePa)
{
    return fullScalePa > 0.0 ? 20.0 * std::log10 (fullScalePa / kRefPressurePa) : 0.0;
}

double fullScalePaFromDbSpl (double dbSpl)
{
    return kRefPressurePa * std::pow (10.0, dbSpl / 20.0);
}

double fullScaleFromCalibrator (double rmsLinear, double refDbSpl)
{
    if (! (rmsLinear > 0.0)) return 0.0;
    return fullScalePaFromDbSpl (refDbSpl) / rmsLinear;
}

namespace
{
/** Welch power per one-sided bin, scaled so that summing a band gives that
    band's mean square: P[k] = 2 |X[k]|^2 / (N sum w^2), averaged over frames. */
std::vector<double> welchPower (const float* x, int n, int order)
{
    const int N = 1 << order;
    std::vector<double> power ((size_t) N / 2 + 1, 0.0);
    if (n < N) return power;
    juce::dsp::FFT fft (order);
    std::vector<float> win ((size_t) N), buf ((size_t) N * 2);
    double sumW2 = 0.0;
    for (int i = 0; i < N; ++i)
    {
        win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / N));
        sumW2 += (double) win[(size_t) i] * win[(size_t) i];
    }
    int frames = 0;
    for (int start = 0; start + N <= n; start += N / 2, ++frames)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N; ++i) buf[(size_t) i] = x[start + i] * win[(size_t) i];
        fft.performRealOnlyForwardTransform (buf.data(), true);
        for (int k = 1; k <= N / 2; ++k)
        {
            const double re = buf[(size_t) (2 * k)], im = buf[(size_t) (2 * k + 1)];
            power[(size_t) k] += (k == N / 2 ? 1.0 : 2.0) * (re * re + im * im) / ((double) N * sumW2);
        }
    }
    if (frames > 0) for (auto& p : power) p /= frames;
    return power;
}

double bandMeanSquare (const std::vector<double>& power, double binHz, double lo, double hi)
{
    double s = 0.0;
    for (size_t k = 1; k < power.size(); ++k)
    {
        const double f = (double) k * binHz;
        if (f >= lo && f <= hi) s += power[k];
    }
    return s;
}
} // namespace

CalibratorReading analyseCalibrator (const float* x, int n, double rate, double toneHz, double refDbSpl)
{
    CalibratorReading r;
    if (x == nullptr || rate <= 0.0 || n < (int) rate)
    {
        r.problem = "Capture too short (at least 1 second is needed).";
        r.code = "too_short";
        return r;
    }
    const int order = rate > 64000.0 ? 14 : 13;
    const double binHz = rate / (1 << order);
    const double lo = toneHz / std::pow (2.0, 1.0 / 6.0);
    const double hi = toneHz * std::pow (2.0, 1.0 / 6.0);

    double sumSq = 0.0;
    for (int i = 0; i < n; ++i)
    {
        sumSq += (double) x[i] * x[i];
        r.peak = std::max (r.peak, (double) std::abs (x[i]));
    }
    r.totalRms = std::sqrt (sumSq / n);

    const auto power = welchPower (x, n, order);
    double total = 0.0, best = 0.0;
    size_t bestK = 0;
    for (size_t k = 1; k < power.size(); ++k)
    {
        total += power[k];
        if (power[k] > best) { best = power[k]; bestK = k; }
    }
    const double band = bandMeanSquare (power, binHz, lo, hi);
    r.bandRms = std::sqrt (band);
    r.bandShare = total > 0.0 ? band / total : 0.0;
    // Parabolic interpolation of the peak for a sub-bin frequency.
    if (bestK > 0 && bestK + 1 < power.size())
    {
        const double a = power[bestK - 1], b = power[bestK], c = power[bestK + 1];
        const double den = a - 2.0 * b + c;
        const double d = den != 0.0 ? 0.5 * (a - c) / den : 0.0;
        r.peakHz = ((double) bestK + juce::jlimit (-0.5, 0.5, d)) * binHz;
    }

    // Steadiness: band level over 0.25 s slices (each long enough for one frame).
    const int slice = std::max ((int) (0.25 * rate), 1 << order);
    double mn = 1e300, mx = -1e300;
    for (int s = 0; s + slice <= n; s += slice)
    {
        const auto p = welchPower (x + s, slice, order);
        const double ms = bandMeanSquare (p, binHz, lo, hi);
        if (ms <= 0.0) continue;
        const double db = 10.0 * std::log10 (ms);
        mn = std::min (mn, db);
        mx = std::max (mx, db);
    }
    r.spreadDb = mx >= mn ? mx - mn : 0.0;

    if (r.peak >= 0.999)
    {
        r.problem = "The signal clips: lower the input gain or move the calibrator.";
        r.code = "clipping";
    }
    else if (r.bandRms < 1e-4)
    {
        r.problem = "No calibrator tone found (the level is below -80 dBFS).";
        r.code = "no_tone";
    }
    else if (std::abs (r.peakHz - toneHz) > 0.03 * toneHz)
    {
        r.problem = "The strongest tone is at " + juce::String (r.peakHz, 1) + " Hz, not "
                  + juce::String (toneHz, 0) + " Hz.";
        r.code = "wrong_frequency";
    }
    else if (r.bandShare < 0.9)
    {
        r.problem = "Too much background noise (the tone carries "
                  + juce::String (100.0 * r.bandShare, 0) + " % of the power).";
        r.code = "noisy";
    }
    else if (r.spreadDb > 0.5)
    {
        r.problem = "The level is not steady (" + juce::String (r.spreadDb, 2)
                  + " dB spread): hold the calibrator firmly on the microphone.";
        r.code = "unsteady";
    }
    else
    {
        r.ok = true;
        r.code = "ok";
        r.fullScalePa = fullScaleFromCalibrator (r.bandRms, refDbSpl);
    }
    return r;
}

ScaleCalibration InputCalibration::toScale() const
{
    ScaleCalibration s;
    if (fullScalePa > 0.0)
    {
        s.unit = ScaleCalibration::Unit::Pascal;
        s.fullScaleValue = fullScalePa;
    }
    return s;
}

Store::Store (juce::PropertySet* p) : props (p) {}

juce::String Store::key (const juce::String& input)
{
    return "cal." + juce::String::toHexString (input.hashCode64());
}

InputCalibration Store::get (const juce::String& input) const
{
    InputCalibration c;
    c.input = input;
    if (props == nullptr) return c;
    const auto v = juce::JSON::parse (props->getValue (key (input)));
    if (! v.isObject()) return c;
    c.fullScalePa = (double) v.getProperty ("fullScalePa", 0.0);
    c.method = v.getProperty ("method", "").toString();
    c.when = juce::Time ((juce::int64) v.getProperty ("when", 0));
    return c;
}

void Store::set (const InputCalibration& c)
{
    if (props == nullptr) return;
    auto* o = new juce::DynamicObject();
    o->setProperty ("input", c.input);
    o->setProperty ("fullScalePa", c.fullScalePa);
    o->setProperty ("method", c.method);
    o->setProperty ("when", c.when.toMilliseconds());
    props->setValue (key (c.input), juce::JSON::toString (juce::var (o), true));
}

void Store::clear (const juce::String& input)
{
    if (props != nullptr) props->removeValue (key (input));
}
} // namespace scm::cal
