// SpectroCraft — DenseUNet noise-suppression model (Phase 1 of the AI plan).
//
// Ported from Intel's openvino-plugins-ai-audacity plugin
// (mod-openvino/noise_suppression/noise_suppression_omz_model.*, GPL-3.0-only,
// Copyright (C) 2024 Intel Corporation), with the Audacity types removed: it
// operates on plain float32 buffers at the model's native rate (16 kHz for
// the Open Model Zoo build). The streaming protocol is unchanged: 128-sample
// patches, 39 recurrent state tensors (inp_state_* / out_state_* pairs), and
// constant "delay" / "freq" outputs read from a dummy first inference.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    #include <openvino/openvino.hpp>
#endif

namespace sc { namespace ai {

class DenseUnetModel {
public:
    /** Load + compile the model (via the shared OvRuntime). BLOCKING — first
        run can take 10–30 s while OpenVINO compiles for the CPU plugin. */
    bool load(const std::string& xmlPath, const std::string& device,
              std::string* errOut = nullptr);

    /** The model's native sample rate (from its "freq" output; 16000). */
    int sampleRate() const { return _freq; }

    /** Number of samples process() will write for an input of nSamples.
        The result is time-aligned with the input: the model's internal
        latency (_delay samples) is compensated, so this equals nSamples. */
    size_t outputLengthFor(size_t nSamples) const { return nSamples; }

    /** Streaming inference over one channel at the model rate. `in` and
        `out` must each hold nSamples floats (they may alias). onProgress,
        if non-null, is called once per patch with 0..1; returning false
        cancels and makes this return false. */
    bool process(const float* in, size_t nSamples, float* out,
                 std::function<bool(float)> onProgress = nullptr);

private:
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    ov::CompiledModel _compiled;
    int _freq = 16000;
    ov::Shape _inputShape{};
    size_t _patchSize = 0;
    int _delay = 0;
    std::vector<std::pair<std::string, std::string>> _stateNames;   // (in, out)
    std::vector<ov::Shape> _stateShapes;
#endif
};

}} // namespace sc::ai
