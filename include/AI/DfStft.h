// SpectroCraft — streaming STFT/ISTFT for DeepFilterNet (Phase 2 of the AI plan).
//
// Ported from Intel's openvino-plugins-ai-audacity plugin
// (mod-openvino/noise_suppression/deepfilternet/deepfilter.*, GPL-3.0-only,
// Copyright (C) 2024 Intel Corporation), with libtorch replaced by plain C++:
//   fft size 960, hop 480, Vorbis window w[n] = sin(pi/2 * sin^2(pi*(n+1/2)/480))
//   analysis : Y = DFT(frame * w) / 960            (wnorm, as in the original)
//   synthesis: x = IDFT_unscaled(Y') * w          (no 1/N factor on the inverse)
// The squared Vorbis window satisfies sum_k w[n - k*480]^2 = 1 at hop N/2, so a
// pass-through of an unmodified spectrum reconstructs the input exactly.
//
// 960 is not a power of two, so both directions run through Bluestein's
// algorithm on top of a fixed 2048-point complex FFT (DIF forward / DIT
// inverse pair — no bit-reversal needed).

#pragma once

#include <cstddef>

namespace sc { namespace ai {

class DfStft {
public:
    struct C { float re = 0.f, im = 0.f; };

    static constexpr int kFftSize = 960;   // transform length
    static constexpr int kHop     = 480;   // frame advance (samples)
    static constexpr int kFreqs   = 481;   // spectrum bins per frame

    /** Clears the streaming memory — call at the start of every chunk so each
        chunk starts from a zeroed state (matches the reference behaviour). */
    void reset();

    /** One analysis frame. `frame` holds kHop samples; writes the spectrum as
        interleaved re/im pairs into out[kFreqs * 2] (scaled by 1/960). */
    void analyze(const float* frame, float* out);

    /** One synthesis frame. `spec` is interleaved re/im (kFreqs pairs, same
        scale as analyze()); writes the first kHop output samples into `out`;
        the remaining kHop samples are kept for the next call. */
    void synthesize(const float* spec, float* out);

private:
    static constexpr int kM      = 2048;  // Bluestein FFT length (>= 2*N - 1)
    static constexpr int kStages = 11;    // log2(kM)

    /** Full complex DFT of length kFftSize via Bluestein. */
    void dft960(const C* in, C* out) const;

    /** In-place unscaled transforms of length kM: fftDif produces bit-reversed
        output, fftDit expects bit-reversed input and yields natural order.
        fftDit(fftDif(x)) == kM * x. */
    void fftDif(C* z) const;
    void fftDit(C* z) const;

    float _mem[kHop];      // analysis: previous frame
    float _synMem[kHop];   // synthesis: second half of the previous frame
    float _win[kFftSize];  // Vorbis window

    C _aHat[kM];           // precomputed DIF transform of the Bluestein chirp a[]
    C _chirpB[kFftSize];   // e^{-pi i n^2 / N}  (b[n] = x[n] * chirpB[n])
    C _chirpScale[kFftSize]; // e^{-pi i k^2 / N} (final scaling of the DFT output)

    // Twiddle tables: _tw[s][j] = exp(-2 pi i j / 2^s) for stage s (len = 2^s),
    // j < 2^(s-1). The inverse direction uses the conjugate. NOTE: stages run
    // 1..kStages inclusive, so this needs kStages+1 rows.
    C _tw[kStages + 1][kM / 2];

public:
    DfStft();
};

}} // namespace sc::ai
