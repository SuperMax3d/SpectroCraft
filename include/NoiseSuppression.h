// SpectroCraft — denseunet noise-suppression pipeline (Phase 1 of the AI plan).
//
// Resamples a selection to the model's native rate with SoXR, runs the
// streaming DenseUNet inference per channel, and resamples back. The shared
// infrastructure (model registry, downloader, runtime) is in src/AI/.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace sc {

/** BLOCKING — call from a worker thread. Runs the denseunet model on planar
    float32 data: `data` holds nChans vectors of exactly len samples at
    `rate`; on success each vector is replaced in place with exactly len
    noise-suppressed samples (length is preserved). If chans != nullptr, only
    the listed channels are processed; the others pass through unchanged.

    Progress phases (onProgress receives 0..1; returning false cancels):
      0.00–0.15  model download / verification (skipped when installed)
      ~0.20      model compile (first run: 10–30 s, cached afterwards)
      0.20–1.00  per-channel inference

    On failure or cancellation this returns false and fills errOut; `data`
    may then be partially modified — the caller must not commit it. */
bool runDenseUnetPipeline(const std::string& modelId, const std::string& device,
                          double rate, int nChans,
                          std::vector<std::vector<float>>& data,
                          std::function<bool(float)> onProgress = nullptr,
                          std::string* errOut = nullptr,
                          const std::vector<int>* chans = nullptr);

} // namespace sc
