/**
 * SpectroCraft - FFT helpers with runtime SIMD dispatch.
 *
 * The AVX2 path is a vectorized radix-2 DIT complex FFT applied to real input,
 * producing the same in/out layout as juce::dsp::FFT::performRealOnlyForwardTransform:
 *   input : data[0 .. N-1] reals (buffer holds 2*N floats)
 *   output: bins k = 0..N/2 interleaved at data[k*2] (re), data[k*2+1] (im)
 *
 * The AVX2 path is only compiled when the translation unit is built with AVX2
 * enabled (__AVX2__, e.g. /arch:AVX2 on MSVC); the current build does not set
 * it. On CPUs without AVX2, or without the flag, scfft::realForward falls back
 * to JUCE's FFT.
 */

#include "FFTHelpers.h"
#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(__AVX2__)
#  include <immintrin.h>
#  if defined(_MSC_VER)
#    include <intrin.h>
#  elif defined(__GNUC__) || defined(__clang__)
#    include <cpuid.h>
#  endif
#endif

namespace scfft
{
#if defined(__AVX2__)
    namespace
    {
        void realForwardAvx2 (float* data, int order);

        // One-time numerical self-test: the AVX2 transform of a small
        // deterministic signal must match a double-precision DFT, and must not
        // write outside its 2*N buffer. (The former "CPU erratum" self-test
        // flagged a real bug: the butterfly loop stepped 4 floats while
        // loading/storing 8, so it wrote past each butterfly group. The loop
        // now steps 8; this check guards against any regression.)
        bool avx2SelfTest ()
        {
            constexpr int order = 6, N = 1 << order, PAD = 64;
            const float SENTINEL = 1e30f;
            std::vector<float> buf ((size_t) (2 * N + 2 * PAD), SENTINEL);
            float* d = buf.data() + PAD;
            std::vector<double> x ((size_t) N);
            for (int i = 0; i < N; ++i)
            {
                x[(size_t) i] = std::sin (0.37 * i) + 0.25 * std::cos (1.9 * i + 0.5);
                d[i] = (float) x[(size_t) i];
            }
            realForwardAvx2 (d, order);

            for (int k = 0; k < PAD; ++k)
                if (buf[(size_t) k] != SENTINEL || buf[(size_t) (PAD + 2 * N + k)] != SENTINEL)
                    return false;

            const double pi = juce::MathConstants<double>::pi;
            for (int k = 0; k <= N / 2; ++k)
            {
                double re = 0.0, im = 0.0;
                for (int i = 0; i < N; ++i)
                {
                    re += x[(size_t) i] * std::cos (-2.0 * pi * k * i / N);
                    im += x[(size_t) i] * std::sin (-2.0 * pi * k * i / N);
                }
                if (std::abs ((double) d[k * 2] - re) > 1e-3 || std::abs ((double) d[k * 2 + 1] - im) > 1e-3)
                    return false;
            }
            return true;
        }

        // True when the CPU reports AVX2 and the OS saves the YMM state.
        bool cpuHasAvx2 ()
        {
#  if defined(_MSC_VER)
            int info[4] = { 0 };
            __cpuid (info, 0);
            if (info[0] < 7)
                return false;
            __cpuid (info, 1);
            const bool osxsave = (info[2] & (1 << 27)) != 0, avx = (info[2] & (1 << 28)) != 0;
            if (! osxsave || ! avx)
                return false;
            if ((_xgetbv (0) & 6) != 6)
                return false;   // OS does not save XMM/YMM state
            __cpuidex (info, 7, 0);
            return (info[1] & (1 << 5)) != 0;   // leaf 7 EBX bit 5 = AVX2
#  elif defined(__GNUC__) || defined(__clang__)
            unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
            if (__get_cpuid_max (0, nullptr) < 7)
                return false;
            __cpuid_count (1, 0, eax, ebx, ecx, edx);
            if ((ecx & (1u << 27)) == 0 || (ecx & (1u << 28)) == 0)
                return false;
            unsigned int xlo = 0, xhi = 0;
            __asm__ volatile ("xgetbv" : "=a" (xlo), "=d" (xhi) : "c" (0));
            if ((xlo & 6u) != 6u)
                return false;   // OS does not save XMM/YMM state
            __cpuid_count (7, 0, eax, ebx, ecx, edx);
            return (ebx & (1u << 5)) != 0;      // leaf 7 EBX bit 5 = AVX2
#  else
            return false;
#  endif
        }
    } // namespace
#endif

    bool hasAvx2()
    {
#if defined(__AVX2__)
        static std::once_flag flag;
        static bool ok = false;
        std::call_once (flag, [] ()
        {
            ok = cpuHasAvx2 () && avx2SelfTest ();
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
            // Return the set actually stored: if another thread inserted one for
            // this order first, emplace keeps that one and our local `ts` would
            // be destroyed on return (dangling reference).
            auto it = twCache.emplace (order, ts).first;
            return *it->second;   // the map's shared_ptr keeps it alive across rehashes
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
                    // Vectorized butterflies: 8 complex points per iteration
                    // (one __m256 holds 8 floats, so the step must be 8 and all
                    // 8 lanes must stay inside [0, half)).
                    for (; j + 7 < half; j += 8)
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
        // Scalar fallback: JUCE's FFT with the identical buffer contract. The
        // FFT object (twiddle tables, plan) is built once per order and per
        // thread instead of on every call; thread-local so no JUCE backend has
        // to be shared across threads.
        if (order >= 0 && order < 32)
        {
            thread_local std::array<std::unique_ptr<juce::dsp::FFT>, 32> ffts;
            auto& fft = ffts[(size_t) order];
            if (fft == nullptr)
                fft = std::make_unique<juce::dsp::FFT> (order);
            fft->performRealOnlyForwardTransform (data, true);
            return;
        }
        juce::dsp::FFT fft (order);
        fft.performRealOnlyForwardTransform (data, true);
    }

} // namespace scfft
