// SpectroCraft — DeepFilterNet2/3 noise suppression (see DeepFilter.h).
#include "AI/DeepFilter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sc { namespace ai {

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI

bool DeepFilter::load(const std::string& modelFolder, const std::string& device,
                      Selection sel, std::string* errOut)
{
    if (!_model.load (modelFolder, device, sel, errOut)) return false;

    constexpr int T = DeepFilterNetModel::kNumHops;
    _spec.resize ((size_t) T * DfStft::kFreqs * 2);
    _featErb.resize ((size_t) T * DeepFilterNetModel::kBands);
    _featSpec.resize ((size_t) T * DeepFilterNetModel::kNbDf * 2);
    _enhanced.resize ((size_t) T * DfStft::kFreqs * 2);
    _padded.resize ((size_t) kChunkSamples + DfStft::kFftSize);
    _synth.resize ((size_t) T * DfStft::kHop);
    return true;
}

bool DeepFilter::forwardChunk(const float* audio, std::vector<float>& out,
                              float attenLimDb, float normalizeAttenLim, bool postFilter,
                              std::function<bool(float)> onProgress)
{
    constexpr int T = DeepFilterNetModel::kNumHops;
    constexpr int kHop  = DfStft::kHop;
    constexpr int kFreqs = DfStft::kFreqs;
    const float alpha = 0.99f;   // EMA normalisation smoothing (upstream default)

    auto report = [&onProgress] (float v) { return !onProgress || onProgress (v); };

    _stft.reset ();
    for (int b = 0; b < DeepFilterNetModel::kBands; ++b)
        _erbState[b] = -60.f + (-90.f + 60.f) * (float) b / 31.f;   // linspace(-60, -90, 32)
    for (int f = 0; f < DeepFilterNetModel::kNbDf; ++f)
        _unitState[f] = 0.001f + (0.0001f - 0.001f) * (float) f / 95.f;   // linspace(0.001, 0.0001, 96)

    std::memcpy (_padded.data (), audio, (size_t) kChunkSamples * sizeof (float));
    std::fill (_padded.begin () + kChunkSamples, _padded.end (), 0.f);

    // ---- Analysis: STFT + features.
    for (int t = 0; t < T; ++t)
    {
        float* s = _spec.data () + (size_t) t * kFreqs * 2;
        _stft.analyze (_padded.data () + (size_t) t * kHop, s);

        // ERB features: band-mean of |X|^2 in dB.
        float acc[DeepFilterNetModel::kBands] = {};
        for (int k = 0; k < kFreqs; ++k)
        {
            const float re = s[2 * k], im = s[2 * k + 1];
            acc[DeepFilterNetModel::binToBand (k)] += re * re + im * im;
        }
        for (int b = 0; b < DeepFilterNetModel::kBands; ++b)
            _featErb[(size_t) t * DeepFilterNetModel::kBands + b] =
                10.f * std::log10 (acc[b] / (float) DeepFilterNetModel::bandWidth (b) + 1e-10f);

        // ERB EMA normalisation: update the state first, then output.
        for (int b = 0; b < DeepFilterNetModel::kBands; ++b)
        {
            float& xs = _featErb[(size_t) t * DeepFilterNetModel::kBands + b];
            _erbState[b] = (1.f - alpha) * xs + alpha * _erbState[b];
            xs = (xs - _erbState[b]) / 40.f;
        }

        // Unit-normalised low-band features: X / sqrt(EMA(|X|)).
        for (int f = 0; f < DeepFilterNetModel::kNbDf; ++f)
        {
            const float re = s[2 * f], im = s[2 * f + 1];
            _unitState[f] = (1.f - alpha) * std::sqrt (re * re + im * im) + alpha * _unitState[f];
            const float inv = 1.f / std::sqrt (_unitState[f]);
            _featSpec[(size_t) t * DeepFilterNetModel::kNbDf * 2 + f * 2]     = re * inv;
            _featSpec[(size_t) t * DeepFilterNetModel::kNbDf * 2 + f * 2 + 1] = im * inv;
        }
    }

    if (!report (0.3f)) return false;

    // ---- Model inference (the dominant cost).
    if (!_model.forward (_spec.data (), _featErb.data (), _featSpec.data (), postFilter,
                        _enhanced.data ()))
        return false;

    if (!report (0.95f)) return false;

    // ---- Optional attenuation limit: blend the enhanced spectrum back toward
    // the original by 10^(-|limit| / normalize).
    if (std::abs (attenLimDb) > 0.f)
    {
        const float lim = std::pow (10.f, -std::abs (attenLimDb) / normalizeAttenLim);
        for (size_t i = 0; i < _enhanced.size (); ++i)
            _enhanced[i] += (_spec[i] - _enhanced[i]) * lim;
    }

    // ---- Synthesis: ISTFT, then trim the STFT delay (fft_size - hop samples).
    for (int t = 0; t < T; ++t)
        _stft.synthesize (_enhanced.data () + (size_t) t * kFreqs * 2,
                         _synth.data () + (size_t) t * kHop);

    const int d = DfStft::kFftSize - kHop;   // 480
    out.resize ((size_t) kChunkSamples);
    std::memcpy (out.data (), _synth.data () + d, (size_t) kChunkSamples * sizeof (float));

    return report (1.f);
}

bool DeepFilter::filter(const float* in, size_t n, std::vector<float>& out,
                        float attenLimDb, float normalizeAttenLim, bool df3PostFilter,
                        std::function<bool(float)> onProgress)
{
    if (n == 0) { out.clear (); return true; }

    const int64_t chunkSize = kChunkSamples;
    const int64_t overlap   = (int64_t) kModelRate * 10;   // 10 seconds at 48 kHz

    // Segment the input into overlapping chunks, exactly like upstream: each
    // new chunk starts `overlap` samples before the previous one ends.
    std::vector<std::pair<int64_t, int64_t>> segments;
    {
        int64_t cur = 0;
        while (cur < (int64_t) n)
        {
            if (cur != 0) cur -= overlap;
            segments.emplace_back (cur, std::min ((int64_t) n - cur, chunkSize));
            cur += chunkSize;
        }
    }

    out.resize (n);
    std::vector<float> chunk ((size_t) chunkSize);
    std::vector<float> chunkOut;

    for (size_t i = 0; i < segments.size (); ++i)
    {
        const int64_t off = segments[i].first;
        const int64_t sz  = segments[i].second;

        std::fill (chunk.begin (), chunk.end (), 0.f);
        std::memcpy (chunk.data (), in + off, (size_t) sz * sizeof (float));

        const float cBase = (float) i / (float) segments.size ();
        const float cSpan = 1.f / (float) segments.size ();
        if (!forwardChunk (chunk.data (), chunkOut, attenLimDb, normalizeAttenLim, df3PostFilter,
                           [&cBase, &cSpan, &onProgress] (float v) {
                               return !onProgress || onProgress (cBase + cSpan * v);
                           }))
            return false;

        float* dst = out.data () + off;
        if (i == 0)
        {
            std::memcpy (dst, chunkOut.data (), (size_t) sz * sizeof (float));
        }
        else
        {
            // Linear crossfade over the overlap region.
            const int64_t ov = std::min (overlap, sz);
            for (int64_t j = 0; j < ov; ++j)
            {
                const float ramp = (ov > 1) ? (float) j / (float) (ov - 1) : 1.f;
                dst[j] = chunkOut[(size_t) j] * ramp + dst[j] * (1.f - ramp);
            }
            if (sz > ov)
                std::memcpy (dst + ov, chunkOut.data () + ov, (size_t) (sz - ov) * sizeof (float));
        }

        if (onProgress && !onProgress ((float) (i + 1) / (float) segments.size ()))
            return false;   // cancelled
    }

    return true;
}

#endif   // SC_ENABLE_AI

}} // namespace sc::ai
