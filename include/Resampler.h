#pragma once
// SpectroCraft - high-quality sample-rate conversion via SoXR (libsoxr).
// SoXR is the same resampling engine Audacity uses; we drive it with the
// same "Best" quality recipe (SOXR_VHQ, 28-bit precision) and planar
// float32 I/O so JUCE AudioBuffers can be used directly.
#include <JuceHeader.h>
#include <atomic>

namespace sc {

class Resampler
{
public:
    /** Resamples `in` from oldRate to newRate into `out` (which is resized).
        Supports any positive input/output rates, including extreme ratios.
        onProgress (if non-null) receives 0..1 as the input is consumed; it
        may be called from a worker thread. If cancel points at an atomic that
        has been set to true, resampling stops at the next chunk boundary and
        false is returned with `out` cleared (no partial output). Returns
        false on failure or cancellation. */
    static bool resample (const juce::AudioBuffer<float>& in,
                          double oldRate, double newRate,
                          juce::AudioBuffer<float>& out,
                          std::function<void(float)> onProgress = nullptr,
                          const std::atomic<bool>* cancel = nullptr);

    /** Resamples and/or mixes channels in one call.
        weights[outCh][inCh] gives each input channel's contribution to
        each output channel; every row is normalised by its sum (a row that
        sums to zero produces silence), so relative values like {2, 1} mean
        a 67%/33% mix. If oldRate == newRate no rate conversion happens and
        the channels are mixed directly. Returns false on invalid arguments,
        failure or cancellation (with `out` cleared). */
    static bool resampleAndRemix (const juce::AudioBuffer<float>& in,
                                  double oldRate, double newRate,
                                  const std::vector<std::vector<double>>& weights,
                                  juce::AudioBuffer<float>& out,
                                  std::function<void(float)> onProgress = nullptr,
                                  const std::atomic<bool>* cancel = nullptr);

    /** Estimated output frame count for a given input length and rates. */
    static int64_t estimateOutputFrames (int64_t inFrames, double oldRate, double newRate);
};

} // namespace sc
