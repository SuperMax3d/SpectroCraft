#pragma once
// SpectroCraft - FFT helpers with runtime SIMD dispatch.
//
// scfft::realForward(data, order) computes a real-only forward FFT of size
// 2^order using the same in/out contract as juce::dsp::FFT:
//   input : data[0 .. N-1]      (N = 2^order reals; buffer must hold 2*N floats)
//   output: bins k = 0..N/2 written interleaved at data[k*2] (re), data[k*2+1] (im)
//
// When the CPU supports AVX2 (and this translation unit was compiled with it)
// a vectorized radix-2 DIT FFT is used; otherwise it falls back to JUCE's
// scalar implementation.

namespace scfft
{
    /** True if the current CPU has AVX2, the AVX2 path was compiled in,
     *  AND a one-time runtime self-test confirms that SIMD store loops
     *  behave correctly on this hardware (some CPUs have an erratum where
     *  vector stores inside a loop commit one spurious extra iteration).
     */
    bool hasAvx2();

    /** Real-only forward transform (see file comment for the buffer contract). */
    void realForward (float* data, int order);
}
