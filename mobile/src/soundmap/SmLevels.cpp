#include "SmLevels.h"
#include "FileDoc.h"
#include "Measures.h"

namespace scm::smap
{
int BandLevels::bandFor (double nominalHz) const noexcept
{
    for (size_t i = 0; i < centresHz.size() && i < db.size(); ++i)
        if (std::abs (nominalFor (centresHz[i]) - nominalHz) < 1e-6) return (int) i;
    return -1;
}

const std::vector<double>& nominalCentres()
{
    static const std::vector<double> c { 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800,
                                         1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000,
                                         12500, 16000, 20000 };
    return c;
}

double nominalFor (double hz) noexcept
{
    double best = 0.0, bestD = 1e9;
    for (double c : nominalCentres())
    {
        const double d = std::abs (std::log (hz / c));
        if (d < bestD) { bestD = d; best = c; }
    }
    return bestD < std::log (1.06) ? best : 0.0;
}

juce::String bandLabel (double hz)
{
    if (hz >= 1000.0)
    {
        const double k = hz / 1000.0;
        return (std::abs (k - std::round (k)) < 1e-9 ? juce::String ((int) std::round (k)) : juce::String (k, 2).trimCharactersAtEnd ("0")) + "k";
    }
    return std::abs (hz - std::round (hz)) < 1e-9 ? juce::String ((int) std::round (hz)) : juce::String (hz, 1);
}

double aWeightingDb (double f) noexcept
{
    const double f2 = f * f;
    const double c1 = 20.598997 * 20.598997, c2 = 107.65265 * 107.65265, c3 = 737.86223 * 737.86223,
                 c4 = 12194.217 * 12194.217;
    const double ra = (c4 * f2 * f2) / ((f2 + c1) * std::sqrt ((f2 + c2) * (f2 + c3)) * (f2 + c4));
    return 20.0 * std::log10 (ra) + 2.0;
}

BandLevels analyseTake (const juce::File& wav, double headSeconds, double tailSeconds, juce::String& error,
                        const std::atomic<bool>* cancel)
{
    BandLevels out;
    FileDoc doc;
    if (! doc.load (wav, nullptr, cancel))
    {
        error = "The take could not be read.";
        return out;
    }
    const auto& eng = doc.engine;
    const double rate = eng.sampleRate();
    if (! eng.isFileLoaded() || rate <= 0.0 || eng.numSamples() <= 0)
    {
        error = "The take is empty.";
        return out;
    }
    const double dur = (double) eng.numSamples() / rate;
    const double t0 = headSeconds, t1 = dur - tailSeconds;
    if ((t1 - t0) * rate < 65536.0)
    {
        error = "The take is too short: it needs " + juce::String ((65536.0 / rate) + headSeconds + tailSeconds, 1)
                + " s or more (one period of the source, after the trimmed ends).";
        return out;
    }

    const ScaleCalibration& cal = doc.scale;
    out.calibrated = cal.isCalibratedFor (0);
    const ScaleCalibration* calPtr = out.calibrated ? &cal : nullptr;
    out.unit = out.calibrated ? cal.levelUnitLabel() : juce::String ("dBFS");
    out.seconds = t1 - t0;
    out.sampleRate = rate;
    out.pipeline = "iec-65536-rect";

    const sc::measures::ProgressFn progress = [cancel] (double) { return cancel == nullptr || ! cancel->load(); };

    sc::measures::ThirdOctaveParams p;
    p.t0 = t0; p.t1 = t1;
    p.channels = { 0 };
    p.mode = sc::measures::ThirdOctaveParams::Mode::Iec;
    // Rectangular frames exactly as long as the periodic pink's period (2^16
    // samples at 48 kHz): each frame then holds the source's line spectrum
    // whatever its position, so the band levels do not depend on where the
    // take starts. Long frames keep the leakage of other signals into far
    // bands negligible (the band sums are what is used).
    p.fftSize = 65536;
    p.windowType = sc::measures::Rectangular;
    const auto r = sc::measures::thirdOctave (eng, p, calPtr, progress);
    if (r.cancelled) { error = "Cancelled."; return out; }
    if (r.channels.empty()) { error = "The bands could not be computed."; return out; }
    const auto& ch = r.channels.front();
    double aSum = 0.0;
    for (size_t b = 0; b < r.centresHz.size() && b < ch.relDb.size(); ++b)
    {
        if (b < ch.noData.size() && ch.noData[b]) continue;
        const double level = (double) ch.relDb[b] + ch.cal.levelOffsetDb;
        if (level < -250.0) continue;
        out.centresHz.push_back (r.centresHz[b]);
        out.db.push_back (level);
        aSum += std::pow (10.0, (level + aWeightingDb (r.centresHz[b])) / 10.0);
    }
    if (aSum > 0.0) out.laeq = 10.0 * std::log10 (aSum);

    sc::measures::LevelsParams lp;
    lp.t0 = t0; lp.t1 = t1;
    lp.channels = { 0 };
    const auto lv = sc::measures::levels (eng, lp, calPtr, progress);
    if (! lv.cancelled && ! lv.channels.empty())
    {
        if (lv.channels.front().rmsDb <= -290.0)
        {
            // digital silence (the microphone held by another app, no permission)
            error = "The take is silent.";
            return {};
        }
        out.lzeq = out.calibrated ? lv.channels.front().rmsAbsDb : lv.channels.front().rmsDb;
    }
    return out;
}

bool writePeriodicPink (const juce::File& wav, double sampleRate, double seconds)
{
    constexpr int order = 16, n = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf ((size_t) n * 2, 0.0f);
    juce::Random rng (20261125);
    const double binHz = sampleRate / n;
    // real-only inverse input: interleaved (re, im) for bins 0 .. n/2
    for (int k = 1; k < n / 2; ++k)
    {
        const double f = k * binHz;
        if (f < 20.0 || f > 20000.0) continue;
        const double mag = 1.0 / std::sqrt (f);   // power ~ 1/f: equal energy per band
        const double ph = rng.nextDouble() * 2.0 * juce::MathConstants<double>::pi;
        buf[(size_t) (2 * k)] = (float) (mag * std::cos (ph));
        buf[(size_t) (2 * k + 1)] = (float) (mag * std::sin (ph));
    }
    fft.performRealOnlyInverseTransform (buf.data());
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) peak = std::max (peak, std::abs (buf[(size_t) i]));
    if (! (peak > 0.0f)) return false;
    const float gain = 0.891f / peak;   // -1 dBFS

    const int periods = std::max (1, (int) std::ceil (seconds * sampleRate / n));
    juce::AudioBuffer<float> period (1, n);
    for (int i = 0; i < n; ++i) period.setSample (0, i, buf[(size_t) i] * gain);

    wav.deleteFile();
    std::unique_ptr<juce::OutputStream> os (wav.createOutputStream());
    if (os == nullptr) return false;
    juce::WavAudioFormat fmt;
    auto w = fmt.createWriterFor (os, juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate)
                                                                     .withNumChannels (1).withBitsPerSample (16));
    if (w == nullptr) return false;
    for (int k = 0; k < periods; ++k)
        if (! w->writeFromAudioSampleBuffer (period, 0, n)) return false;
    return true;
}
} // namespace scm::smap
