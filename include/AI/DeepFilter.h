// SpectroCraft — DeepFilterNet2/3 noise suppression (Phase 2 of the AI plan).
//
// Ported from Intel's openvino-plugins-ai-audacity plugin
// (mod-openvino/noise_suppression/deepfilternet/deepfilter.*, GPL-3.0-only,
// Copyright (C) 2024 Intel Corporation), with libtorch replaced by plain C++:
// streaming STFT/ISTFT (DfStft), ERB + unit-normalised features, the three
// OpenVINO sub-models (DeepFilterNetModel), and 30-second chunks with a
// 10-second linear crossfade overlap.
//
// The model operates at a fixed 48 kHz; callers resample in/out.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    #include "AI/DfStft.h"
    #include "AI/DeepFilterNetModel.h"
#endif

namespace sc { namespace ai {

class DeepFilter {
public:
    using Selection = DeepFilterNetModel::Selection;

    static constexpr int kModelRate = 48000;   // the models' native sample rate

    /** modelFolder contains enc.xml/bin, erb_dec.xml/bin and df_dec.xml/bin.
        BLOCKING — first run compiles three sub-models (tens of seconds). */
    bool load(const std::string& modelFolder, const std::string& device,
              Selection sel, std::string* errOut = nullptr);

    /** BLOCKING: processes one channel at 48 kHz. `in` holds n samples; on
        success out is resized to exactly n samples.
        attenLimDb: attenuation limit in dB (6..100; 0 = no limit).
        normalizeAttenLim: divisor of the limit exponent (20, as upstream).
        df3PostFilter: enable the DF3 spectral post-filter.
        onProgress receives 0..1; returning false cancels (returns false). */
    bool filter(const float* in, size_t n, std::vector<float>& out,
                float attenLimDb = 0.f, float normalizeAttenLim = 20.f,
                bool df3PostFilter = false,
                std::function<bool(float)> onProgress = nullptr);

private:
    // One chunk of exactly kChunkSamples samples (zero-padded by the caller).
    bool forwardChunk(const float* audio, std::vector<float>& out,
                      float attenLimDb, float normalizeAttenLim, bool postFilter,
                      std::function<bool(float)> onProgress);

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    DfStft _stft;
    DeepFilterNetModel _model;

    // Per-chunk state (reset at the start of every chunk).
    float _erbState[DeepFilterNetModel::kBands] = {};
    float _unitState[DeepFilterNetModel::kNbDf] = {};

    // Scratch buffers, sized for one full chunk.
    std::vector<float> _spec;        // [T][481*2]
    std::vector<float> _featErb;     // [T][32]
    std::vector<float> _featSpec;    // [T][96*2]
    std::vector<float> _enhanced;    // [T][481*2]
    std::vector<float> _padded;      // chunk + 960 analysis input
    std::vector<float> _synth;       // T * 480 synthesis output

    static constexpr int kChunkSamples = (DeepFilterNetModel::kNumHops - 2) * DfStft::kHop;
#endif
};

}} // namespace sc::ai
