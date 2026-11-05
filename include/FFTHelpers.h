#pragma once
// SpectroCraft - FFT helpers with runtime SIMD dispatch.
//
// scfft::realForward(data, order) computes a real-only forward FFT of size
// 2^order using the same in/out contract as juce::dsp::FFT:
//   input : data[0 .. N-1]      (N = 2^order reals; buffer must hold 2*N floats)
//   output: bins k = 0..N/2 written interleaved at data[k*2] (re), data[k*2+1] (im)
//
// When the CPU supports AVX2 (and this translation unit was compiled with it)
// a vectorized radix-2 DIT FFT is used; otherwise it falls back to JUCE's FFT
// (cached per order and thread).

namespace scfft
{
    /** True if the AVX2 path was compiled in (__AVX2__), the CPU reports AVX2
     *  with OS YMM-state support, and a one-time numerical self-test of the
     *  AVX2 FFT against a double-precision DFT passed.
     */
    bool hasAvx2();

    /** Real-only forward transform (see file comment for the buffer contract). */
    void realForward (float* data, int order);
}
