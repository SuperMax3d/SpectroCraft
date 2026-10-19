// SpectroCraft — DeepFilterNet2/3 model runner (see DeepFilterNetModel.h).
#include "AI/DeepFilterNetModel.h"
#include "AI/OvRuntime.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace sc { namespace ai {

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI

// The fixed ERB band layout (bin counts per band, summing to kFreqs = 481),
// identical to the reference implementation.
static const int kBandsWidth[DeepFilterNetModel::kBands] = {
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    5, 5, 7, 7, 8, 10, 12, 13, 15, 18, 20, 24, 28, 31, 37, 42, 50, 56, 67
};

int DeepFilterNetModel::bandWidth(int b)
{
    return kBandsWidth[b];
}

int DeepFilterNetModel::binToBand(int bin)
{
    int pos = 0;
    for (int b = 0; b < DeepFilterNetModel::kBands; ++b)
    {
        const int w = bandWidth (b);
        if (bin < pos + w) return b;
        pos += w;
    }
    return DeepFilterNetModel::kBands - 1;
}

bool DeepFilterNetModel::load(const std::string& modelFolder, const std::string& device,
                              Selection sel, std::string* errOut)
{
    _sel = sel;

    // erb_inv_fb expansion table: each frequency bin maps to exactly one band.
    for (int k = 0; k < kFreqs; ++k)
        _binToBand[k] = binToBand (k);

    auto& rt = OvRuntime::instance();
    if (!rt.ready() && !rt.init (errOut)) return false;

    // NOTE: 'const juce::File folder (juce::String (modelFolder));' on one line
    // trips an MSVC 14.28 parser quirk (C2228); the assignment form is safe.
    const juce::File folder = juce::File (juce::String (modelFolder));

    auto encCm = rt.compile (folder.getChildFile ("enc.xml").getFullPathName ().toStdString (), device, errOut);
    if (!encCm) return false;
    _enc = encCm.create_infer_request ();

    auto erbCm = rt.compile (folder.getChildFile ("erb_dec.xml").getFullPathName ().toStdString (), device, errOut);
    if (!erbCm) return false;
    _erbDec = erbCm.create_infer_request ();

    // Link enc's outputs directly into erb_dec's inputs (zero-copy).
    _erbDec.set_tensor ("emb", _enc.get_tensor ("emb"));
    _erbDec.set_tensor ("e3",  _enc.get_tensor ("e3"));
    _erbDec.set_tensor ("e2",  _enc.get_tensor ("e2"));
    _erbDec.set_tensor ("e1",  _enc.get_tensor ("e1"));
    _erbDec.set_tensor ("e0",  _enc.get_tensor ("e0"));

    auto dfCm = rt.compile (folder.getChildFile ("df_dec.xml").getFullPathName ().toStdString (), device, errOut);
    if (!dfCm) return false;
    _dfDec = dfCm.create_infer_request ();

    // Link enc's outputs directly into df_dec's inputs.
    _dfDec.set_tensor ("emb", _enc.get_tensor ("emb"));
    _dfDec.set_tensor ("c0",  _enc.get_tensor ("c0"));

    _featErbBuf.resize ((size_t) kNumHops * kBands);
    _featSpecBuf.resize (2 * (size_t) kNumHops * kNbDf);
    return true;
}

bool DeepFilterNetModel::forward(const float* spec, const float* featErb, const float* featSpec,
                                 bool postFilter, float* enhancedOut)
{
    constexpr int T = kNumHops;

    // ---- Fill the encoder inputs. The reference pads the features with a
    // 2-frame lookahead: crop two frames off the front, append two zero
    // frames at the end (net length unchanged).
    for (int t = 0; t < T; ++t)
    {
        const int src = t + 2;
        if (src < T)
        {
            std::memcpy (_featErbBuf.data () + (size_t) t * kBands,
                        featErb + (size_t) src * kBands, (size_t) kBands * sizeof (float));
            for (int f = 0; f < kNbDf; ++f)
            {
                const float re = featSpec [(size_t) (src * kNbDf + f) * 2];
                const float im = featSpec [(size_t) (src * kNbDf + f) * 2 + 1];
                _featSpecBuf[(size_t) t * kNbDf + f]            = re;   // channel 0 (re)
                _featSpecBuf[(size_t) T * kNbDf + t * kNbDf + f] = im;   // channel 1 (im)
            }
        }
        else
        {
            std::fill (_featErbBuf.begin () + (size_t) t * kBands,
                      _featErbBuf.begin () + ((size_t) t + 1) * kBands, 0.f);
            std::fill (_featSpecBuf.begin () + (size_t) t * kNbDf,
                      _featSpecBuf.begin () + ((size_t) t + 1) * kNbDf, 0.f);
            std::fill (_featSpecBuf.begin () + (size_t) T * kNbDf + (size_t) t * kNbDf,
                      _featSpecBuf.begin () + (size_t) T * kNbDf + ((size_t) t + 1) * kNbDf, 0.f);
        }
    }

    try
    {
        // ---- Encoder.
        {
            ov::Tensor erbT (ov::element::f32, ov::Shape { 1, 1, T, kBands }, _featErbBuf.data ());
            ov::Tensor specT (ov::element::f32, ov::Shape { 1, 2, T, kNbDf }, _featSpecBuf.data ());
            _enc.set_tensor ("feat_erb", erbT);
            _enc.set_tensor ("feat_spec", specT);
        }
        _enc.infer ();

        // ---- ERB decoder -> band mask m [T][kBands].
        _erbDec.infer ();
        const float* m = _erbDec.get_tensor ("m").data<float> ();

        // Expand the 32-band mask to all 481 bins (each bin takes its band's
        // value) and apply it to the spectrum.
        std::vector<float> specM ((size_t) T * kFreqs * 2);
        for (int t = 0; t < T; ++t)
        {
            const float* mt = m + (size_t) t * kBands;
            const float* s  = spec + (size_t) t * kFreqs * 2;
            float* sm       = specM.data () + (size_t) t * kFreqs * 2;
            for (int k = 0; k < kFreqs; ++k)
            {
                const float g = mt [_binToBand[k]];
                sm[2 * k]     = s[2 * k] * g;
                sm[2 * k + 1] = s[2 * k + 1] * g;
            }
        }

        // ---- Deep-filter decoder -> coefs [T][kNbDf][5][2].
        _dfDec.infer ();
        const float* coefsRaw = _dfDec.get_tensor ("coefs").data<float> ();

        // Apply the deep filter to the low bands: for each frame t and bin f,
        // enhanced[t][f] = sum_n S_in[t+n-2][f] * coef[n][t][f], a 5-frame FIR
        // over time (centred on t by the lookahead padding). DF3 deep-filters
        // the unmasked spectrum; DF2 deep-filters the masked one. The high
        // bands always take the ERB-masked spectrum.
        const float* sIn = (_sel == Selection::DF3) ? spec : specM.data ();
        for (int t = 0; t < T; ++t)
        {
            float* e = enhancedOut + (size_t) t * kFreqs * 2;
            for (int f = 0; f < kNbDf; ++f)
            {
                float re = 0.f, im = 0.f;
                const float* cBase = coefsRaw + ((size_t) t * kNbDf + f) * 10;
                for (int n = 0; n < 5; ++n)
                {
                    const int r = t + n - 2;
                    if (r < 0 || r >= T) continue;
                    const float* sf = sIn + (size_t) r * kFreqs * 2 + f * 2;
                    const float cr  = cBase[n * 2];
                    const float ci  = cBase[n * 2 + 1];
                    re += sf[0] * cr - sf[1] * ci;
                    im += sf[0] * ci + sf[1] * cr;
                }
                e[f * 2]     = re;
                e[f * 2 + 1] = im;
            }
            std::copy (specM.data () + (size_t) t * kFreqs * 2 + kNbDf * 2,
                       specM.data () + ((size_t) t + 1) * kFreqs * 2,
                       e + kNbDf * 2);
        }

        // ---- Optional DF3 spectral post-filter.
        if (postFilter && _sel == Selection::DF3)
        {
            const float beta = 0.02f;
            const float eps  = 1e-12f;
            for (int t = 0; t < T; ++t)
            {
                const float* s = spec + (size_t) t * kFreqs * 2;   // original spectrum
                float* e       = enhancedOut + (size_t) t * kFreqs * 2;
                for (int k = 0; k < kFreqs; ++k)
                {
                    const float magE = std::sqrt (e[2 * k] * e[2 * k] + e[2 * k + 1] * e[2 * k + 1]);
                    const float magS = std::sqrt (s[2 * k] * s[2 * k] + s[2 * k + 1] * s[2 * k + 1]);
                    float mask = magE / (magS + eps);
                    if (mask < eps) mask = eps;
                    else if (mask > 1.f) mask = 1.f;
                    float maskSin = mask * std::sin (3.14159265358979323846f * mask * 0.5f);
                    if (maskSin < eps) maskSin = eps;
                    const float ratio = mask / maskSin;
                    const float pf = (1.f + beta) / (1.f + beta * ratio * ratio);
                    e[2 * k]     *= pf;
                    e[2 * k + 1] *= pf;
                }
            }
        }
    }
    catch (const std::exception&)
    {
        return false;
    }

    return true;
}

#endif   // SC_ENABLE_AI

}} // namespace sc::ai
