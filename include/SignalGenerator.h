#pragma once
// SpectroCraft - deterministic signal generators for Effects > Generate ...
//
// All generators fill nch channels of len samples. Tone phases are continuous
// with absolute file time (t0Secs + i/rate), so generated content lines up
// phase-wise with the surrounding audio and does not click against it.

#include <cstdint>
#include <vector>

namespace sc {

enum class NoiseType : int { White = 0, Pink = 1, Brown = 2 };
enum class WaveShape : int { Sine = 0, Square = 1, Sawtooth = 2, Triangle = 3 };

struct ToneSpec
{
    double freqHz = 440.0;
    float amplitude = 0.5f;
};

/** Generates len samples of noise into out (nch channels). intensity is the
    peak amplitude in [0..1]: white noise is uniform on [-I, I]; pink and
    brown are filtered / integrated white noise normalised to the same peak.
    seed == 0 picks a fresh random seed; any other value makes the output
    reproducible (used by the unit tests). */
void generateNoise (NoiseType type, float intensity, int nch, int64_t len,
                    uint64_t seed, std::vector<std::vector<float>>& out);

/** Generates len samples of the sum of tones into out. Amplitudes add
    linearly (no clipping). */
void generateTone (const std::vector<ToneSpec>& tones, WaveShape shape,
                   double rate, double t0Secs, int nch, int64_t len,
                   std::vector<std::vector<float>>& out);

} // namespace sc
