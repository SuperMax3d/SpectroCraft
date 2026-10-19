// SpectroCraft — streaming STFT/ISTFT for DeepFilterNet (see DfStft.h).
#include "AI/DfStft.h"

#include <cmath>
#include <cstring>

namespace sc { namespace ai {

using C = DfStft::C;

namespace {

constexpr double kPi = 3.14159265358979323846;

inline C cAdd (C a, C b) { return C { a.re + b.re, a.im + b.im }; }
inline C cSub (C a, C b) { return C { a.re - b.re, a.im - b.im }; }
inline C cMul (C a, C b) { return C { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re }; }

} // namespace

DfStft::DfStft()
{
    // Vorbis window: w[n] = sin(pi/2 * sin^2(pi*(n+1/2)/960)), n = 0..959.
    // (Intel's formula sin(pi/2*((n+0.5)/480)) inside the outer sin is the
    // same thing: pi/2 * (n+0.5)/480 == pi*(n+0.5)/960.) Satisfies COLA at
    // hop 480: w[n]^2 + w[n+480]^2 == 1.
    for (int n = 0; n < kFftSize; ++n)
    {
        const double s = std::sin (kPi * (double) (n + 0.5) / kFftSize);
        _win[n] = (float) std::sin (0.5 * kPi * s * s);
    }

    // Bluestein chirps.
    for (int n = 0; n < kFftSize; ++n)
    {
        const double a = -kPi * (double) n * n / kFftSize;
        _chirpB[n]     = C { (float) std::cos (a), (float) std::sin (a) };
        _chirpScale[n] = _chirpB[n];   // same sequence, used on the output side
    }

    // Twiddle tables (needed by fftDif below). NOTE: stage kStages itself is
    // used too (len = 2^kStages), so the loop must include it.
    for (int s = 1; s <= kStages; ++s)
    {
        const int len  = 1 << s;
        const int half = len >> 1;
        for (int j = 0; j < half; ++j)
        {
            const double a = -2.0 * kPi * (double) j / (double) len;
            _tw[s][j] = C { (float) std::cos (a), (float) std::sin (a) };
        }
    }

    // a[0] = 1; for m = 1..N-1: a[m] = a[M-m] = exp(+pi i m^2 / N).
    std::memset (_aHat, 0, sizeof _aHat);
    _aHat[0] = C { 1.f, 0.f };
    for (int m = 1; m < kFftSize; ++m)
    {
        const double a = kPi * (double) m * m / kFftSize;
        const C v { (float) std::cos (a), (float) std::sin (a) };
        _aHat[m]      = v;
        _aHat[kM - m] = v;
    }
    fftDif (_aHat);   // precompute in the same bit-reversed layout as the data

    reset();
}

void DfStft::reset()
{
    std::memset (_mem, 0, sizeof _mem);
    std::memset (_synMem, 0, sizeof _synMem);
}

void DfStft::fftDif (C* z) const
{
    // Decimation-in-frequency forward transform: natural input order,
    // bit-reversed output order. No scaling.
    for (int s = kStages; s >= 1; --s)
    {
        const int len  = 1 << s;
        const int half = len >> 1;
        for (int i = 0; i < kM; i += len)
            for (int j = 0; j < half; ++j)
            {
                const C u = z[i + j];
                const C v = z[i + j + half];
                z[i + j]      = cAdd (u, v);
                z[i + j + half] = cMul (cSub (u, v), _tw[s][j]);
            }
    }
}

void DfStft::fftDit (C* z) const
{
    // Decimation-in-time inverse transform: bit-reversed input order,
    // natural output order. No scaling — fftDit(fftDif(x)) == kM * x.
    for (int s = 1; s <= kStages; ++s)
    {
        const int len  = 1 << s;
        const int half = len >> 1;
        for (int i = 0; i < kM; i += len)
            for (int j = 0; j < half; ++j)
            {
                const C u = z[i + j];
                // conjugate twiddle: flip the imaginary part of _tw[s][j]
                const C w { _tw[s][j].re, -_tw[s][j].im };
                const C v = cMul (z[i + j + half], w);
                z[i + j]        = cAdd (u, v);
                z[i + j + half] = cSub (u, v);
            }
    }
}

void DfStft::dft960 (const C* in, C* out) const
{
    // Bluestein: X[k] = chirpScale[k] * IFFT_M( FFT_M(b) * Ahat )[k], where
    // b[n] = in[n] * chirpB[n] for n < N and 0 otherwise.
    C buf[kM];
    for (int n = 0; n < kFftSize; ++n)
        buf[n] = cMul (in[n], _chirpB[n]);
    std::memset (&buf[kFftSize], 0, (size_t) (kM - kFftSize) * sizeof C);

    fftDif (buf);
    for (int k = 0; k < kM; ++k)
        buf[k] = cMul (buf[k], _aHat[k]);
    fftDit (buf);

    const float invM = 1.f / (float) kM;
    for (int k = 0; k < kFftSize; ++k)
        out[k] = cMul (C { buf[k].re * invM, buf[k].im * invM }, _chirpScale[k]);
}

void DfStft::analyze (const float* frame, float* out)
{
    C in[kFftSize];
    for (int n = 0; n < kHop; ++n)
        in[n]       = C { _mem[n] * _win[n], 0.f };
    for (int n = 0; n < kHop; ++n)
        in[kHop + n] = C { frame[n] * _win[kHop + n], 0.f };

    std::memcpy (_mem, frame, sizeof _mem);   // memory for the next frame

    C X[kFftSize];
    dft960 (in, X);

    const float wnorm = 1.f / (float) kFftSize;
    for (int k = 0; k < kFreqs; ++k)
    {
        out[2 * k]     = X[k].re * wnorm;
        out[2 * k + 1] = X[k].im * wnorm;
    }
}

void DfStft::synthesize (const float* spec, float* out)
{
    // Rebuild the full 960-bin spectrum from its 481 conjugate-symmetric bins,
    // then take the unscaled inverse via x[n] = conj(DFT(conj(Z)))[n].re.
    C Z[kFftSize];
    for (int k = 0; k < kFreqs; ++k)
        Z[k] = C { spec[2 * k], spec[2 * k + 1] };
    for (int k = 1; k <= kFftSize / 2 - 1; ++k)
        Z[kFftSize - k] = C { Z[k].re, -Z[k].im };

    C W[kFftSize];
    for (int k = 0; k < kFftSize; ++k)
        W[k] = C { Z[k].re, -Z[k].im };

    C U[kFftSize];
    dft960 (W, U);

    float x[kFftSize];
    for (int n = 0; n < kFftSize; ++n)
        x[n] = U[n].re;

    // Window the frame and overlap-add: first half out, second half kept.
    for (int n = 0; n < kHop; ++n)
        out[n] = _synMem[n] + x[n] * _win[n];
    for (int n = 0; n < kHop; ++n)
        _synMem[n] = x[kHop + n] * _win[kHop + n];
}

}} // namespace sc::ai
