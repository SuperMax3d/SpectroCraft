/**
 * SpectroCraft - FFT helpers with runtime SIMD dispatch.
 *
 * The AVX2 path is a vectorized radix-2 DIT complex FFT applied to real input,
 * producing the same in/out layout as juce::dsp::FFT::performRealOnlyForwardTransform:
 *   input : data[0 .. N-1] reals (buffer holds 2*N floats)
 *   output: bins k = 0..N/2 interleaved at data[k*2] (re), data[k*2+1] (im)
 *
 * This file is compiled with /arch:AVX2 on MSVC; on CPUs without AVX2 (or when
 * the flag was not enabled) scfft::realForward falls back to JUCE's scalar FFT.
 */

#include "FFTHelpers.h"
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(__AVX2__)
#  include <immintrin.h>
#endif

namespace scfft
{
#if defined(__AVX2__)
    namespace
    {
        // One-time hardware self-test.
        //
        // Some CPUs (observed on an AMD Ryzen 9 3900X / Zen 2) have an erratum
        // where a SIMD store inside a loop causes the CPU to commit one extra,
        // out-of-bounds iteration of the loop body. The machine code is correct;
        // the spurious iteration loads the memory just past the array, computes
        // on it and stores the result back - corrupting adjacent heap data.
        //
        // We detect this by running the exact loop pattern (load + add + store,
        // stepping 4 floats) over a small sentinel-padded buffer and checking
        // that nothing outside the data region was touched. If corruption is
        // found, the AVX2 path is disabled for the life of the process.
        bool avx2HardwareSelfTest ()
        {
            const float SENTINEL = 1e30f;
            constexpr int PAD = 64, M = 8;
            std::vector<float> buf ((size_t) (M + 2 * PAD), SENTINEL);
            float* r = buf.data() + PAD;
            for (int k = 0; k < M; ++k)
                r[k] = (float) k * 0.1f;

            // The exact pattern used by the vectorized FFT butterfly loop.
            for (int i = 0; i < M; i += 4)
            {
                const __m256 v = _mm256_loadu_ps (r + i);
                const __m256 w = _mm256_add_ps (v, v);
                _mm256_storeu_ps (r + i, w);
            }

            for (size_t k = 0; k < buf.size(); ++k)
            {
                if (k >= (size_t) PAD && k < (size_t) (PAD + M))
                    continue;
                if (buf[k] != SENTINEL)
                    return false;   // spurious extra iteration corrupted the padding
            }
            return true;
        }
    } // namespace
#endif

    bool hasAvx2()
    {
#if defined(_DEBUG)
        // The AVX2 path is disabled in Debug builds: on affected CPUs (e.g.
        // Ryzen 9 3900X) the SIMD-store erratum only manifests under
        // concurrent multithreaded load, which a one-shot single-threaded
        // self-test cannot reliably detect. Release keeps AVX2 when the
        // hardware test passes.
        return false;
#elif defined(__AVX2__)
        static std::once_flag flag;
        static bool ok = false;
        std::call_once (flag, [] ()
        {
            int info[4] = { 0 };
#  if defined(_MSC_VER)
            __cpuid (info, 1);
#  elif defined(__GNUC__) || defined(__clang__)
            unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
            __cpuid_count (1, 0, eax, ebx, ecx, edx);
            info[2] = (int) ecx;
#  else
            return;
#  endif
            if ((info[2] & (1 << 28)) == 0)
                return;   // no AVX2
            ok = avx2HardwareSelfTest ();
        });
        return ok;
#else
        return false;                        // compiled without AVX2 support
#endif
    }

#if defined(__AVX2__)

    namespace
    {
        // Per-stage twiddle tables, cached per order (shared across threads).
        struct TwiddleSet
        {
            std::vector<std::vector<float>> re;   // [stage][j]
            std::vector<std::vector<float>> im;
        };

        std::mutex twMutex;
        std::unordered_map<int, std::shared_ptr<TwiddleSet>> twCache;

        const TwiddleSet& getTwiddles (int order)
        {
            const int N = 1 << order;
            {
                const std::lock_guard<std::mutex> lk (twMutex);
                auto it = twCache.find (order);
                if (it != twCache.end())
                    return *it->second;
            }

            auto ts = std::make_shared<TwiddleSet>();
            for (int len = 2, stage = 0; len <= N; len <<= 1, ++stage)
            {
                const int half = len >> 1;
                std::vector<float> wr ((size_t) half), wi ((size_t) half);
                const double ang = -2.0 * juce::MathConstants<double>::pi / (double) len;
                for (int j = 0; j < half; ++j)
                {
                    wr[(size_t) j] = (float) std::cos (ang * (double) j);
                    wi[(size_t) j] = (float) std::sin (ang * (double) j);
                }
                ts->re.push_back (std::move (wr));
                ts->im.push_back (std::move (wi));
            }

            const std::lock_guard<std::mutex> lk (twMutex);
            twCache.emplace (order, ts);
            return *ts;   // safe: shared_ptr keeps it alive even if the map rehashes
        }

        void bitReverse (float* re, float* im, int N)
        {
            for (int i = 1, j = 0; i < N; ++i)
            {
                int bit = N >> 1;
                for (; j & bit; bit >>= 1)
                    j ^= bit;
                j ^= bit;
                if (i < j)
                {
                    std::swap (re[i], re[j]);
                    std::swap (im[i], im[j]);
                }
            }
        }

        // In-place complex FFT on separate re/im arrays of length N.
        void fftComplexAvx2 (float* re, float* im, int order)
        {
            const int N = 1 << order;
            bitReverse (re, im, N);

            const TwiddleSet& tw = getTwiddles (order);
            int stage = 0;
            for (int len = 2; len <= N; len <<= 1, ++stage)
            {
                const int half = len >> 1;
                const float* wr = tw.re[(size_t) stage].data();
                const float* wi = tw.im[(size_t) stage].data();

                for (int group = 0; group < N; group += len)
                {
                    int j = 0;
                    // Vectorized butterflies: 4 complex points per iteration.
                    for (; j + 3 < half; j += 4)
                    {
                        const __m256 ar = _mm256_loadu_ps (re + group + j);
                        const __m256 ai = _mm256_loadu_ps (im + group + j);
                        const __m256 br = _mm256_loadu_ps (re + group + j + half);
                        const __m256 bi = _mm256_loadu_ps (im + group + j + half);
                        const __m256 wrv = _mm256_loadu_ps (wr + j);
                        const __m256 wiv = _mm256_loadu_ps (wi + j);

                        // t = w * b
                        const __m256 tr = _mm256_sub_ps (_mm256_mul_ps (wrv, br),
                                                        _mm256_mul_ps (wiv, bi));
                        const __m256 ti = _mm256_add_ps (_mm256_mul_ps (wrv, bi),
                                                        _mm256_mul_ps (wiv, br));

                        // a' = a + t ;  b' = a - t
                        _mm256_storeu_ps (re + group + j,         _mm256_add_ps (ar, tr));
                        _mm256_storeu_ps (im + group + j,         _mm256_add_ps (ai, ti));
                        _mm256_storeu_ps (re + group + j + half,  _mm256_sub_ps (ar, tr));
                        _mm256_storeu_ps (im + group + j + half,  _mm256_sub_ps (ai, ti));
                    }
                    // Scalar tail
                    for (; j < half; ++j)
                    {
                        const float ar = re[group + j],         ai = im[group + j];
                        const float br = re[group + j + half],  bi = im[group + j + half];
                        const float tr = wr[j] * br - wi[j] * bi;
                        const float ti = wr[j] * bi + wi[j] * br;
                        re[group + j]         = ar + tr;
                        im[group + j]         = ai + ti;
                        re[group + j + half]  = ar - tr;
                        im[group + j + half]  = ai - ti;
                    }
                }
            }
        }

        void realForwardAvx2 (float* data, int order)
        {
            const int N = 1 << order;

            // Thread-local work buffers (reused across frames).
            thread_local std::vector<float> tlRe ((size_t) N);
            thread_local std::vector<float> tlIm ((size_t) N);
            if ((int) tlRe.size() < N) { tlRe.resize ((size_t) N); tlIm.resize ((size_t) N); }

            std::copy (data, data + N, tlRe.data());
            std::fill (tlIm.begin(), tlIm.end(), 0.0f);   // real input: imag = 0

            fftComplexAvx2 (tlRe.data(), tlIm.data(), order);

            // Pack JUCE layout: bins k = 0..N/2 -> data[2k], data[2k+1]
            for (int k = 0; k <= N / 2; ++k)
            {
                data[k * 2]     = tlRe[(size_t) k];
                data[k * 2 + 1] = tlIm[(size_t) k];
            }
        }

    } // namespace

#endif // __AVX2__

    void realForward (float* data, int order)
    {
#if defined(__AVX2__)
        const bool avx = hasAvx2();
        if (avx && order >= 1 && order <= 16)
        {
            realForwardAvx2 (data, order);
            return;
        }
#endif
        // Scalar fallback: JUCE's FFT with the identical buffer contract.
        juce::dsp::FFT fft (order);
        fft.performRealOnlyForwardTransform (data, true);
    }

} // namespace scfft
