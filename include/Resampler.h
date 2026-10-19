#pragma once
// SpectroCraft - high-quality sample-rate conversion via SoXR (libsoxr).
// SoXR is the same resampling engine Audacity uses; we drive it with the
// same "Best" quality recipe (SOXR_VHQ, 28-bit precision) and planar
// float32 I/O so JUCE AudioBuffers can be used directly.
#include <JuceHeader.h>

namespace sc {

class Resampler
{
public:
    /** Resamples `in` from oldRate to newRate into `out` (which is resized).
        Supports any positive input/output rates, including extreme ratios.
        onProgress (if non-null) receives 0..1 as the input is consumed; it
        may be called from a worker thread. Returns false on failure. */
    static bool resample (const juce::AudioBuffer<float>& in,
                          double oldRate, double newRate,
                          juce::AudioBuffer<float>& out,
                          std::function<void(float)> onProgress = nullptr);

    /** Estimated output frame count for a given input length and rates. */
    static int64_t estimateOutputFrames (int64_t inFrames, double oldRate, double newRate);
};

} // namespace sc
