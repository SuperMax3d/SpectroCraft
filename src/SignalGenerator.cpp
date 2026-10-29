// SpectroCraft - signal generators for Effects > Generate ...
#include "SignalGenerator.h"

#include <cmath>
#include <chrono>
#include <random>

namespace sc {

namespace
{

constexpr double kTwoPi = 6.283185307179586476925;   // 2*pi (MSVC: no M_PI)

std::mt19937_64 makeRng (uint64_t seed)
{
    if (seed != 0) return std::mt19937_64 (seed);
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::mt19937_64 ((uint64_t) now.count());
}

float shapeValue (WaveShape s, double phase01)   // phase01 in [0, 1)
{
    switch (s)
    {
        case WaveShape::Sine:     return (float) std::sin (kTwoPi * phase01);
        case WaveShape::Square:   return phase01 < 0.5 ? 1.0f : -1.0f;
        case WaveShape::Sawtooth: return (float) (2.0 * phase01 - 1.0);
        default:                  // Triangle: rises -1..+1 over the first half, falls back
            return phase01 < 0.5 ? (float) (4.0 * phase01 - 1.0)
                                 : (float) (3.0 - 4.0 * phase01);
    }
}

void peakNormalise (std::vector<float>& ch, float targetPeak)
{
    double maxAbs = 0.0;
    for (float v : ch)
        if ((double) std::abs (v) > maxAbs) maxAbs = (double) std::abs (v);
    if (maxAbs <= 1e-12 || targetPeak <= 0.0f) return;   // silent / zero intensity: leave as-is
    const float scale = targetPeak / (float) maxAbs;
    for (float& v : ch) v *= scale;
}

} // namespace

void generateNoise (NoiseType type, float intensity, int nch, int64_t len,
                    uint64_t seed, std::vector<std::vector<float>>& out)
{
    const float I = intensity < 0.0f ? 0.0f : (intensity > 1.0f ? 1.0f : intensity);
    out.assign ((size_t) nch, std::vector<float> ((size_t) len, 0.0f));
    if (nch <= 0 || len <= 0 || I <= 0.0f) return;

    auto rng = makeRng (seed);
    std::uniform_real_distribution<double> uni (-1.0, 1.0);

    // All types generate one mono pattern and replicate it to every channel:
    // the masked-edit path reuses channel 0 for all channels.
    switch (type)
    {
        case NoiseType::White:
            for (float& v : out[0]) v = (float) (uni (rng) * I);
            break;

        case NoiseType::Pink:
        {
            // Vos's six-stage pink noise filter (-3 dB/octave); every stage is
            // a first-order low-pass with |coefficient| < 1, so it is stable.
            double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
            for (float& v : out[0])
            {
                const double r = uni (rng);
                b0 = 0.99886 * b0 + r * 0.0555179;
                b1 = 0.99332 * b1 + r * 0.0750759;
                b2 = 0.96900 * b2 + r * 0.1538520;
                b3 = 0.86650 * b3 + r * 0.3104856;
                b4 = 0.55000 * b4 + r * 0.5329522;
                b5 = -0.7616  * b5 - r * 0.0168980;
                v = (float) (b0 + b1 + b2 + b3 + b4 + b5 + b6 + r * 0.5362);
                b6 = r * 0.115926;
            }
            peakNormalise (out[0], I);
            break;
        }

        case NoiseType::Brown:
        {
            // Random walk (integrated white noise), DC offset removed.
            double acc = 0.0, mean = 0.0;
            for (float& v : out[0])
            {
                acc += 0.02 * uni (rng);
                v = (float) acc;
                mean += (double) v;
            }
            mean /= (double) len;
            for (float& v : out[0]) v -= (float) mean;
            peakNormalise (out[0], I);
            break;
        }
    }

    for (int ch = 1; ch < nch; ++ch)
        out[(size_t) ch] = out[0];
}

void generateTone (const std::vector<ToneSpec>& tones, WaveShape shape,
                   double rate, double t0Secs, int nch, int64_t len,
                   std::vector<std::vector<float>>& out)
{
    out.assign ((size_t) nch, std::vector<float> ((size_t) len, 0.0f));
    if (nch <= 0 || len <= 0 || tones.empty() || rate <= 0.0) return;

    for (auto& ch : out)
    {
        float* d = ch.data ();
        for (int64_t i = 0; i < len; ++i)
        {
            const double t = t0Secs + (double) i / rate;
            double v = 0.0;
            for (const auto& tone : tones)
                if (tone.freqHz > 0.0 && std::isfinite (tone.freqHz))
                    v += (double) tone.amplitude * shapeValue (shape,
                                                               std::fmod (tone.freqHz * t, 1.0));
            d[(size_t) i] = (float) v;
        }
    }
}

} // namespace sc
