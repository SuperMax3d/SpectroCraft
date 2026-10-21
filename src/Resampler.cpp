// SpectroCraft - SoXR-based sample-rate conversion.
#include "Resampler.h"
#include <soxr.h>
#include <cmath>

namespace sc {

int64_t Resampler::estimateOutputFrames (int64_t inFrames, double oldRate, double newRate)
{
    if (inFrames <= 0 || oldRate <= 0.0 || newRate <= 0.0) return 0;
    const int64_t est = (int64_t) std::ceil ((double) inFrames * (newRate / oldRate));
    // Filter delay/overshoot margin (SoXR's VHQ filter is a few thousand taps).
    return est + 65536;
}

bool Resampler::resample (const juce::AudioBuffer<float>& in,
                          double oldRate, double newRate,
                          juce::AudioBuffer<float>& out,
                          std::function<void(float)> onProgress,
                          const std::atomic<bool>* cancel)
{
    // SoXR expresses all lengths as samples PER CHANNEL.
    const int nch = in.getNumChannels();
    const size_t N = (size_t) in.getNumSamples();

    if (nch <= 0 || N == 0) { out.clear(); return true; }
    if (!std::isfinite(oldRate) || !std::isfinite(newRate) || oldRate <= 0.0 || newRate <= 0.0)
        return false;

    // Same rate: plain copy, no resampling needed.
    if (std::abs (oldRate - newRate) < 1e-9 * std::max (oldRate, newRate))
    {
        out.clear();
        out.setSize (nch, (int) N);
        for (int c = 0; c < nch; ++c)
            std::memcpy (out.getWritePointer (c), in.getReadPointer (c), (size_t) N * sizeof (float));
        if (onProgress) onProgress (1.0f);
        return true;
    }

    const int64_t est = estimateOutputFrames ((int64_t) N, oldRate, newRate);
    out.clear();
    out.setSize (nch, (int) est);

    // Planar float32 I/O: one pointer per channel, straight from the AudioBuffer.
    const soxr_io_spec_t io = soxr_io_spec (SOXR_FLOAT32_S, SOXR_FLOAT32_S);

    // Audacity's "Best" quality recipe: 28-bit precision sinc filtering.
    const soxr_quality_spec_t qspec = soxr_quality_spec (SOXR_VHQ, 0);

    // num_threads == 0 lets SoXR pick (single-threaded without OpenMP).
    const soxr_runtime_spec_t rt = soxr_runtime_spec (0);

    soxr_error_t err = nullptr;
    soxr_t x = soxr_create (oldRate, newRate, (unsigned) nch, &err, &io, &qspec, &rt);
    if (!x || err != nullptr) { out.clear(); return false; }

    std::vector<const float*> inPtrs ((size_t) nch);
    std::vector<float*>        outPtrs((size_t) nch);

    size_t inPos = 0, outPos = 0;
    bool done = false;
    const size_t kChunk = (size_t) (1 << 20); // ~1M frames per soxr_process call

    while (!done)
    {
        if (cancel != nullptr && cancel->load())
        {   // Stop at a chunk boundary: no partial output is left behind.
            soxr_delete (x);
            out.clear();
            return false;
        }

        // Grow the output buffer if our estimate was too small.
        size_t cap = (size_t) out.getNumSamples();   // frames per channel
        if (outPos >= cap)
        {
            out.setSize (nch, (int) (cap * 3 / 2 + 4096), true, false, true);
            cap = (size_t) out.getNumSamples();
        }

        const bool eof = (inPos >= N);
        const size_t chunk = eof ? 0 : std::min (N - inPos, kChunk);

        for (int c = 0; c < nch; ++c)
            outPtrs[(size_t) c] = out.getWritePointer (c, (int) outPos);
        if (!eof)
            for (int c = 0; c < nch; ++c)
                inPtrs[(size_t) c] = in.getReadPointer (c, (int) inPos);

        size_t idone = 0, odone = 0;
        err = soxr_process (x,
                            eof ? nullptr : (soxr_in_t) inPtrs.data(), chunk, &idone,
                            (soxr_out_t) outPtrs.data(),
                            cap - outPos, &odone);
        if (err != nullptr) { soxr_delete (x); out.clear(); return false; }

        inPos += idone;
        outPos += odone;

        if (onProgress && N > 0)
            onProgress ((float) ((double) inPos / (double) N));

        // End of input: keep draining until no more output is produced.
        if (eof && odone == 0)
            done = true;
    }

    soxr_delete (x);

    // Trim to the exact length actually produced (keep the data).
    out.setSize (nch, (int) outPos, true);
    return true;
}

} // namespace sc
