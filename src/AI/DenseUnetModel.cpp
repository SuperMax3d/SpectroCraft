// SpectroCraft — DenseUNet noise-suppression model (Phase 1 of the AI plan).
// Ported from Intel's openvino-plugins-ai-audacity plugin (GPL-3.0-only,
// Copyright (C) 2024 Intel Corporation); see include/AI/DenseUnetModel.h.

#include "AI/DenseUnetModel.h"
#include "AI/OvRuntime.h"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <stdexcept>

namespace sc { namespace ai {

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI

static size_t shapeSize(const ov::Shape& s)
{
    size_t n = 1;
    for (auto d : s) n *= (size_t) d;
    return n;
}

bool DenseUnetModel::load(const std::string& xmlPath, const std::string& device,
                          std::string* errOut)
{
    auto& rt = OvRuntime::instance();
    if (!rt.ready() && !rt.init (errOut)) return false;

    _compiled = rt.compile (xmlPath, device, errOut);
    if (!_compiled) return false;

    // Discover the streaming interface: patch size, recurrent state pairs and
    // the constant "delay" / "freq" outputs. The constants are read from a
    // dummy inference with no inputs set — they do not depend on "input".
    try
    {
        auto inputs  = _compiled.inputs();
        auto outputs = _compiled.outputs();

        for (const auto& in : inputs)
        {
            const std::string name = in.get_any_name();
            if (name.find ("inp_state_") == std::string::npos) continue;

            // inp_state_X <-> out_state_X (Intel's naming convention).
            std::string outName = name;
            outName.replace (0, 3, "out");

            const bool found = std::any_of (outputs.begin(), outputs.end(),
                [&outName] (const ov::Output<const ov::Node>& o) {
                    return o.get_any_name() == outName;
                });
            if (!found)
                throw std::runtime_error ("model output state name does not correspond to input state name");

            _stateNames.emplace_back (name, outName);
            _stateShapes.push_back (in.get_shape());
        }
        if (_stateNames.empty())
            throw std::runtime_error ("no expected model state inputs found");

        const auto inPort = _compiled.input ("input");
        _inputShape  = inPort.get_shape();
        _patchSize   = _inputShape[1];

        auto req = _compiled.create_infer_request();
        req.infer();   // dummy run to materialise the constant outputs
        for (const auto& out : outputs)
        {
            const std::string name = out.get_any_name();
            if (name == "delay")
                _delay = req.get_tensor ("delay").data<int>()[0];
            else if (name == "freq")
                _freq = req.get_tensor ("freq").data<int>()[0];
        }
    }
    catch (const std::exception& e)
    {
        if (errOut) *errOut = e.what();
        return false;
    }
    return true;
}

bool DenseUnetModel::process(const float* in, size_t nSamples, float* out,
                             std::function<bool(float)> onProgress)
{
    if (nSamples == 0) return true;

    auto req = _compiled.create_infer_request();

    // One extra patch covers the model's internal delay plus any partial tail.
    const size_t iter    = 1 + ((nSamples + (size_t) _delay) / _patchSize);
    const size_t inpSize = _patchSize * iter;

    // Zero-padded input: the network always consumes whole patches.
    std::vector<float> padded (inpSize, 0.0f);
    std::memcpy (padded.data(), in, nSamples * sizeof (float));

    // Raw (unshifted) inference output; shifted by _delay below.
    std::vector<float> raw (inpSize, 0.0f);

    // Recurrent state lives in our own buffers so an input tensor never aliases
    // this request's output tensors; the copy is 39 small memcpys per patch —
    // negligible next to the network itself.
    std::vector<std::vector<float>> stateData (_stateNames.size());
    for (size_t k = 0; k < _stateNames.size(); ++k)
        stateData[k].assign (shapeSize (_stateShapes[k]), 0.0f);

    std::vector<ov::Tensor>         stateTensors;
    stateTensors.reserve (_stateNames.size());
    for (size_t k = 0; k < _stateNames.size(); ++k)
        stateTensors.emplace_back (ov::element::f32, _stateShapes[k], stateData[k].data());

    for (size_t i = 0; i < iter; ++i)
    {
        ov::Tensor inTensor (ov::element::f32, _inputShape, &padded[i * _patchSize]);
        req.set_tensor ("input", inTensor);

        for (size_t k = 0; k < _stateNames.size(); ++k)
        {
            if (i > 0)
                std::memcpy (stateData[k].data(),
                             req.get_tensor (_stateNames[k].second).data<float>(),
                             stateData[k].size() * sizeof (float));
            req.set_tensor (_stateNames[k].first, stateTensors[k]);
        }

        req.infer();   // throws on failure; caught by the caller

        std::memcpy (raw.data() + i * _patchSize,
                     req.get_tensor ("output").data<float>(),
                     _patchSize * sizeof (float));

        if (onProgress && !onProgress ((float) ((double) (i + 1) / (double) iter)))
            return false;   // cancelled
    }

    // The network is causal with an internal latency of _delay samples: raw
    // output sample i corresponds to input sample i + _delay. Shift the
    // result back so it stays time-aligned with the input (the OMZ demo does
    // the same; Intel's Audacity plugin does not, which shifts their output).
    for (size_t i = 0; i < nSamples; ++i)
        out[i] = (i + (size_t) _delay < raw.size()) ? raw[i + (size_t) _delay] : 0.0f;

    return true;
}

#endif   // SC_ENABLE_AI

}} // namespace sc::ai
