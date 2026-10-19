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

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
/** BLOCKING — call from a worker thread. Runs DeepFilterNet2/3 (modelId must be
    "deepfilternet2" or "deepfilternet3") on planar float32 data, same contract as
    runDenseUnetPipeline: `data` holds nChans vectors of exactly len samples at
    `rate`; on success each vector is replaced in place with exactly len
    noise-suppressed samples.

    attenLimDb limits the attenuation (6..100 dB; 0 = no limit, upstream default).
    df3PostFilter enables the DF3 spectral post-filter (ignored for DF2).

    Progress phases: download 0.00–0.15, compile ~0.20, per-channel inference
    0.20–1.00 (resample-up / model / resample-down). */
bool runDeepFilterPipeline(const std::string& modelId, const std::string& device,
                           double rate, int nChans,
                           std::vector<std::vector<float>>& data,
                           float attenLimDb = 0.f,
                           bool df3PostFilter = false,
                           std::function<bool(float)> onProgress = nullptr,
                           std::string* errOut = nullptr,
                           const std::vector<int>* chans = nullptr);
#endif

} // namespace sc
