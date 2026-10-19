// SpectroCraft — DeepFilterNet2/3 model runner (Phase 2 of the AI plan).
//
// Ported from Intel's openvino-plugins-ai-audacity plugin
// (mod-openvino/noise_suppression/deepfilternet/dfnet_model.*, GPL-3.0-only,
// Copyright (C) 2024 Intel Corporation), with libtorch removed: the three
// OpenVINO sub-models (enc, erb_dec, df_dec) are driven directly and enc's
// outputs are linked straight into the decoders' inputs (zero-copy).
//
// The IR files are exported with a static frame count T = 3002; callers must
// supply exactly that many frames per inference.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    #include <openvino/openvino.hpp>
#endif

namespace sc { namespace ai {

class DeepFilterNetModel {
public:
    enum class Selection { DF2, DF3 };

    static constexpr int kNumHops = 3002;   // fixed frame count of the IR models
    static constexpr int kFreqs   = 481;    // spectrum bins per frame
    static constexpr int kBands   = 32;     // ERB feature bands
    static constexpr int kNbDf    = 96;     // deep-filtered low-band count

    /** modelFolder contains enc.xml/bin, erb_dec.xml/bin and df_dec.xml/bin. */
    bool load(const std::string& modelFolder, const std::string& device,
              Selection sel, std::string* errOut = nullptr);

    int numStaticHops() const { return kNumHops; }

    // The fixed ERB band layout (identical to the reference implementation).
    static int bandWidth(int b);     // number of frequency bins in band b (0..31)
    static int binToBand(int bin);   // which band owns frequency bin 0..480

    /** One inference over exactly kNumHops frames (BLOCKING).
        spec:        [T][kFreqs] interleaved re/im — analysis spectrum.
        featErb:     [T][kBands] dB ERB features.
        featSpec:    [T][kNbDf] interleaved re/im unit-normalised low bands.
        postFilter:  DF3 spectral post-filter (ignored for DF2).
        enhancedOut: [T][kFreqs] interleaved — the enhanced spectrum, same
                     scale as spec. Returns false if an inference fails. */
    bool forward(const float* spec, const float* featErb, const float* featSpec,
                 bool postFilter, float* enhancedOut);

private:
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    ov::InferRequest _enc;
    ov::InferRequest _erbDec;
    ov::InferRequest _dfDec;
#endif
    Selection _sel = Selection::DF2;
    int _binToBand[kFreqs] = {};   // erb_inv_fb expansion: bin -> band index

    std::vector<float> _featErbBuf;    // [T][kBands], lookahead-padded
    std::vector<float> _featSpecBuf;   // [2][T][kNbDf], lookahead-padded
};

}} // namespace sc::ai
