// SpectroCraft - blind source separation (issue #68); see include/BlindSeparation.h
//
// References for the update rules:
//   AuxIVA    N. Ono, "Stable and fast update rules for independent vector
//             analysis based on auxiliary function technique", WASPAA 2011.
//   ILRMA     D. Kitamura et al., "Determined blind source separation unifying
//             independent vector analysis and nonnegative matrix
//             factorization", IEEE/ACM TASLP 2016.
//   cACGMM    N. Ito, S. Araki, T. Nakatani, "Complex angular central Gaussian
//             mixture model for directional statistics in mask-based
//             microphone array signal processing", EUSIPCO 2016.
//   FastMNMF2 K. Sekiguchi et al., "Fast multichannel nonnegative matrix
//             factorization with directivity-aware jointly-diagonalizable
//             spatial covariance matrices for blind source separation",
//             IEEE/ACM TASLP 2020 (the frequency-independent G variant).
#include "BlindSeparation.h"
#include "FFTHelpers.h"
#include "Parallel.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <random>

namespace sc::bss
{
using cf = std::complex<float>;
using cd = std::complex<double>;

const std::vector<MethodInfo>& methods()
{
    static const std::vector<MethodInfo> m {
        { Method::auxIva, "AuxIVA",
          "Independent vector analysis: one demixing filter per frequency, each source's frequencies "
          "kept together by their common loudness over time.",
          "Fast, stable, few settings; good on speech and sources that start and stop.",
          "Needs at least as many microphones as sources; weak on steady, noise-like sources and in "
          "very reverberant rooms.",
          false, true },
        { Method::ilrma, "ILRMA",
          "AuxIVA whose sources are also modelled as a few repeating spectral shapes (NMF).",
          "Usually the cleanest separation of tonal and harmonic sources (motors, fans, music); "
          "still fast.",
          "Needs at least as many microphones as sources; slower than AuxIVA; too few bases blur "
          "rich sources, too many let them leak.",
          true, true },
        { Method::cacgmm, "Cluster (cACGMM)",
          "Groups every time-frequency point by the direction it comes from (complex angular "
          "central Gaussian mixture), then extracts each group with a beamformer or a mask.",
          "Robust to diffuse noise (an extra noise class); works with fewer sources than "
          "microphones; good on sparse sources such as speech.",
          "Masks can sound watery; the groups must be matched across frequencies, which can fail "
          "when sources overlap a lot.",
          false, false },
        { Method::fastMnmf2, "FastMNMF2",
          "A full spatial model (each source a jointly diagonalised covariance) with NMF spectra, "
          "fitted to every channel at once.",
          "Often the best quality in reverberant rooms; handles steady sources such as airflow.",
          "The slowest by far (several times ILRMA); needs more iterations; more memory.",
          true, false },
    };
    return m;
}

const MethodInfo& infoOf (Method m)
{
    for (const auto& i : methods())
        if (i.method == m) return i;
    return methods().front();
}

int sourcesFor (const Params& p, int channels)
{
    const int c = std::max (1, channels);
    return p.sources <= 0 ? c : std::clamp (p.sources, 1, c);
}

namespace
{
constexpr double kEps = 1e-12;

// ------------------------------------------------------------ small matrices

/** Dense n x n complex matrix, row-major. */
struct CMat
{
    int n = 0;
    std::vector<cd> a;
    CMat() = default;
    explicit CMat (int size) : n (size), a ((size_t) size * (size_t) size) {}
    cd& operator() (int i, int j) { return a[(size_t) i * (size_t) n + (size_t) j]; }
    cd operator() (int i, int j) const { return a[(size_t) i * (size_t) n + (size_t) j]; }
    static CMat identity (int size)
    {
        CMat m (size);
        for (int i = 0; i < size; ++i) m (i, i) = 1.0;
        return m;
    }
};

CMat mul (const CMat& A, const CMat& B)
{
    CMat C (A.n);
    for (int i = 0; i < A.n; ++i)
        for (int k = 0; k < A.n; ++k)
        {
            const cd aik = A (i, k);
            for (int j = 0; j < A.n; ++j)
                C (i, j) += aik * B (k, j);
        }
    return C;
}

/** In-place inverse (Gauss-Jordan, partial pivoting). `logDet` receives
    log |det|. False when singular. */
bool invert (CMat& m, double* logDet = nullptr)
{
    const int n = m.n;
    CMat inv = CMat::identity (n);
    double ld = 0.0;
    for (int c = 0; c < n; ++c)
    {
        int piv = c;
        double best = std::abs (m (c, c));
        for (int r = c + 1; r < n; ++r)
            if (std::abs (m (r, c)) > best) { best = std::abs (m (r, c)); piv = r; }
        if (best < 1e-300)
            return false;
        if (piv != c)
            for (int j = 0; j < n; ++j)
            {
                std::swap (m (c, j), m (piv, j));
                std::swap (inv (c, j), inv (piv, j));
            }
        const cd d = m (c, c);
        ld += std::log (std::abs (d));
        const cd invD = 1.0 / d;
        for (int j = 0; j < n; ++j) { m (c, j) *= invD; inv (c, j) *= invD; }
        for (int r = 0; r < n; ++r)
        {
            if (r == c) continue;
            const cd f = m (r, c);
            if (f == cd (0.0)) continue;
            for (int j = 0; j < n; ++j)
            {
                m (r, j) -= f * m (c, j);
                inv (r, j) -= f * inv (c, j);
            }
        }
    }
    m = std::move (inv);
    if (logDet != nullptr) *logDet = ld;
    return true;
}

/** Eigen-decomposition of a Hermitian matrix (cyclic Jacobi): eigenvalues
    descending, eigenvectors in the columns of `vecs`. */
void eigHermitian (CMat A, std::vector<double>& vals, CMat& vecs)
{
    const int n = A.n;
    vecs = CMat::identity (n);
    for (int sweep = 0; sweep < 60; ++sweep)
    {
        double off = 0.0, total = 0.0;
        for (int p = 0; p < n; ++p)
            for (int q = 0; q < n; ++q)
            {
                total += std::norm (A (p, q));
                if (q > p) off += std::norm (A (p, q));
            }
        if (off <= 1e-26 * total || total <= 0.0)   // relative: covariances can be tiny or huge
            break;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q)
            {
                const cd apq = A (p, q);
                const double mag = std::abs (apq);
                if (mag < 1e-300) continue;
                // 1. A phase on q makes A(p,q) real: A <- U^H A U, U = diag (.., conj (e) at q, ..).
                const cd e = apq / mag;
                for (int k = 0; k < n; ++k)
                {
                    A (k, q) *= std::conj (e);
                    vecs (k, q) *= std::conj (e);
                }
                for (int k = 0; k < n; ++k)
                    A (q, k) *= e;
                // 2. A real Jacobi rotation zeroes it: A <- G^T A G.
                const double app = A (p, p).real(), aqq = A (q, q).real();
                const double theta = 0.5 * std::atan2 (2.0 * mag, aqq - app);
                const double c = std::cos (theta), s = std::sin (theta);
                for (int k = 0; k < n; ++k)
                {
                    const cd akp = A (k, p), akq = A (k, q);
                    A (k, p) = c * akp - s * akq;
                    A (k, q) = s * akp + c * akq;
                    const cd vkp = vecs (k, p), vkq = vecs (k, q);
                    vecs (k, p) = c * vkp - s * vkq;
                    vecs (k, q) = s * vkp + c * vkq;
                }
                for (int k = 0; k < n; ++k)
                {
                    const cd apk = A (p, k), aqk = A (q, k);
                    A (p, k) = c * apk - s * aqk;
                    A (q, k) = s * apk + c * aqk;
                }
            }
    }
    std::vector<int> order ((size_t) n);
    std::iota (order.begin(), order.end(), 0);
    std::sort (order.begin(), order.end(), [&A] (int i, int j) { return A (i, i).real() > A (j, j).real(); });
    vals.resize ((size_t) n);
    CMat sorted (n);
    for (int k = 0; k < n; ++k)
    {
        vals[(size_t) k] = A (order[(size_t) k], order[(size_t) k]).real();
        for (int r = 0; r < n; ++r)
            sorted (r, k) = vecs (r, order[(size_t) k]);
    }
    vecs = std::move (sorted);
}

/** One iterative-projection step: w = (W V)^-1 e_n, scaled so w^H V w = 1;
    row n of W becomes w^H. */
void ipUpdate (CMat& W, const CMat& V, int n)
{
    CMat WV = mul (W, V);
    if (! invert (WV))
        return;   // keep the previous row
    const int N = W.n;
    std::vector<cd> w ((size_t) N);
    for (int i = 0; i < N; ++i)
        w[(size_t) i] = WV (i, n);
    cd q = 0.0;
    for (int i = 0; i < N; ++i)
    {
        cd vi = 0.0;
        for (int j = 0; j < N; ++j)
            vi += V (i, j) * w[(size_t) j];
        q += std::conj (w[(size_t) i]) * vi;
    }
    const double s = std::sqrt (std::max (q.real(), kEps));
    for (int i = 0; i < N; ++i)
        W (n, i) = std::conj (w[(size_t) i] / s);
}

/** Parallel loop over [0, n) items of real work (frequencies, frames): split
    whatever the count. */
void parallelFor (int n, const std::function<void (int, int)>& fn)
{
    sc::par::forRange (n, [&fn] (int64_t b, int64_t e) { fn ((int) b, (int) e); }, 1);
}

// ------------------------------------------------------------ STFT

struct Spectrum
{
    int F = 0, T = 0, M = 0;
    std::vector<cf> X;   // ((f * T) + t) * M + m
    cf& at (int f, int t, int m) { return X[((size_t) f * (size_t) T + (size_t) t) * (size_t) M + (size_t) m]; }
    cf at (int f, int t, int m) const { return X[((size_t) f * (size_t) T + (size_t) t) * (size_t) M + (size_t) m]; }
};

std::vector<float> hann (int N)
{
    std::vector<float> w ((size_t) N);
    for (int i = 0; i < N; ++i)
        w[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / N));
    return w;
}

int orderOf (int N)
{
    int o = 0;
    while ((1 << o) < N) ++o;
    return o;
}

int frameCount (int64_t len, int N)
{
    const int H = N / 4;
    return (int) ((len + N - H + H - 1) / H);
}

/** STFT of channels [start, start + len) of `in`. */
Spectrum analyse (const std::vector<std::vector<float>>& in, int64_t start, int64_t len, int N)
{
    Spectrum S;
    S.M = (int) in.size();
    S.F = N / 2 + 1;
    S.T = frameCount (len, N);
    S.X.assign ((size_t) S.F * (size_t) S.T * (size_t) S.M, cf());
    const int H = N / 4, order = orderOf (N);
    const auto w = hann (N);
    parallelFor (S.T, [&] (int t0, int t1)
    {
        std::vector<float> buf ((size_t) N * 2);
        for (int t = t0; t < t1; ++t)
        {
            const int64_t s = (int64_t) t * H - (N - H);
            for (int m = 0; m < S.M; ++m)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                const auto& x = in[(size_t) m];
                for (int i = 0; i < N; ++i)
                {
                    const int64_t k = s + i;
                    if (k >= 0 && k < len)
                        buf[(size_t) i] = x[(size_t) (start + k)] * w[(size_t) i];
                }
                scfft::realForward (buf.data(), order);
                for (int f = 0; f < S.F; ++f)
                    S.at (f, t, m) = cf (buf[(size_t) f * 2], buf[(size_t) f * 2 + 1]);
            }
        }
    });
    return S;
}

/** Inverse STFT of one spectrum (F x T, index f * T + t) to `len` samples. */
std::vector<float> synthesise (const std::vector<cf>& Y, int F, int T, int N, int64_t len)
{
    const int H = N / 4, order = orderOf (N);
    const auto w = hann (N);
    std::vector<float> frames ((size_t) T * (size_t) N);
    parallelFor (T, [&] (int t0, int t1)
    {
        juce::dsp::FFT fft (order);
        std::vector<float> buf ((size_t) N * 2);
        for (int t = t0; t < t1; ++t)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int f = 0; f < F; ++f)
            {
                const cf v = Y[(size_t) f * (size_t) T + (size_t) t];
                buf[(size_t) f * 2] = v.real();
                buf[(size_t) f * 2 + 1] = (f == 0 || f == F - 1) ? 0.0f : v.imag();
            }
            fft.performRealOnlyInverseTransform (buf.data());
            for (int i = 0; i < N; ++i)
                frames[(size_t) t * (size_t) N + (size_t) i] = buf[(size_t) i] * w[(size_t) i];
        }
    });
    std::vector<float> out ((size_t) len, 0.0f);
    std::vector<float> norm ((size_t) len, 0.0f);
    for (int t = 0; t < T; ++t)
    {
        const int64_t s = (int64_t) t * H - (N - H);
        for (int i = 0; i < N; ++i)
        {
            const int64_t k = s + i;
            if (k < 0 || k >= len) continue;
            out[(size_t) k] += frames[(size_t) t * (size_t) N + (size_t) i];
            norm[(size_t) k] += w[(size_t) i] * w[(size_t) i];
        }
    }
    for (size_t k = 0; k < out.size(); ++k)
        out[k] = norm[k] > 1e-6f ? out[k] / norm[k] : 0.0f;
    return out;
}

// ------------------------------------------------------------ helpers

struct Cancelled {};

/** Per frequency: the projection to the N-dimensional working space
    (whitened PCA when N < M, identity otherwise) and its pseudo-inverse. */
struct Reduction
{
    int N = 0, M = 0;
    std::vector<std::vector<cd>> P, Pinv;   // per f: N x M and M x N, row-major
    bool identity = true;
};

Reduction reduce (const Spectrum& S, int N)
{
    Reduction R;
    R.N = N;
    R.M = S.M;
    R.identity = N == S.M;
    if (R.identity)
        return R;
    R.P.assign ((size_t) S.F, {});
    R.Pinv.assign ((size_t) S.F, {});
    parallelFor (S.F, [&] (int f0, int f1)
    {
        for (int f = f0; f < f1; ++f)
        {
            CMat C (S.M);
            for (int t = 0; t < S.T; ++t)
                for (int i = 0; i < S.M; ++i)
                {
                    const cd xi = S.at (f, t, i);
                    for (int j = 0; j < S.M; ++j)
                        C (i, j) += xi * std::conj (cd (S.at (f, t, j)));
                }
            for (auto& v : C.a) v /= std::max (1, S.T);
            std::vector<double> vals;
            CMat E;
            eigHermitian (C, vals, E);
            auto& P = R.P[(size_t) f];
            auto& Pi = R.Pinv[(size_t) f];
            P.assign ((size_t) N * (size_t) S.M, cd());
            Pi.assign ((size_t) S.M * (size_t) N, cd());
            for (int k = 0; k < N; ++k)
            {
                const double l = std::max (vals[(size_t) k], 1e-20);
                for (int m = 0; m < S.M; ++m)
                {
                    P[(size_t) k * (size_t) S.M + (size_t) m] = std::conj (E (m, k)) / std::sqrt (l);
                    Pi[(size_t) m * (size_t) N + (size_t) k] = E (m, k) * std::sqrt (l);
                }
            }
        }
    });
    return R;
}

Spectrum project (const Spectrum& S, const Reduction& R)
{
    if (R.identity)
        return S;
    Spectrum Z;
    Z.F = S.F; Z.T = S.T; Z.M = R.N;
    Z.X.assign ((size_t) Z.F * (size_t) Z.T * (size_t) Z.M, cf());
    parallelFor (S.F, [&] (int f0, int f1)
    {
        for (int f = f0; f < f1; ++f)
        {
            const auto& P = R.P[(size_t) f];
            for (int t = 0; t < S.T; ++t)
                for (int k = 0; k < R.N; ++k)
                {
                    cd acc = 0.0;
                    for (int m = 0; m < S.M; ++m)
                        acc += P[(size_t) k * (size_t) S.M + (size_t) m] * cd (S.at (f, t, m));
                    Z.at (f, t, k) = cf (acc);
                }
        }
    });
    return Z;
}

/** y = W z for every point; Y index ((f * T) + t) * N + n. */
void demix (const Spectrum& Z, const std::vector<CMat>& W, Spectrum& Y)
{
    Y.F = Z.F; Y.T = Z.T; Y.M = Z.M;
    Y.X.resize (Z.X.size());
    const int N = Z.M;
    parallelFor (Z.F, [&] (int f0, int f1)
    {
        for (int f = f0; f < f1; ++f)
            for (int t = 0; t < Z.T; ++t)
                for (int n = 0; n < N; ++n)
                {
                    cd acc = 0.0;
                    for (int k = 0; k < N; ++k)
                        acc += W[(size_t) f] (n, k) * cd (Z.at (f, t, k));
                    Y.at (f, t, n) = cf (acc);
                }
    });
}

/** Weighted covariance V = (1/T) sum_t wgt(t) z z^H at frequency f. */
CMat weightedCov (const Spectrum& Z, int f, const std::function<double (int)>& wgt)
{
    const int N = Z.M;
    CMat V (N);
    for (int t = 0; t < Z.T; ++t)
    {
        const double g = wgt (t);
        for (int i = 0; i < N; ++i)
        {
            const cd zi = cd (Z.at (f, t, i)) * g;
            for (int j = i; j < N; ++j)
                V (i, j) += zi * std::conj (cd (Z.at (f, t, j)));
        }
    }
    for (int i = 0; i < N; ++i)
        for (int j = i; j < N; ++j)
        {
            V (i, j) /= std::max (1, Z.T);
            if (j != i) V (j, i) = std::conj (V (i, j));
        }
    // A little loading keeps the inverse stable on silent bins.
    double tr = 0.0;
    for (int i = 0; i < N; ++i) tr += V (i, i).real();
    for (int i = 0; i < N; ++i) V (i, i) += 1e-9 * tr / N + 1e-20;
    return V;
}

/** Outputs heard at the reference microphone: for IVA-type demixing the
    back-projection A = Pinv W^-1 scales each source. Result: N spectra. */
std::vector<std::vector<cf>> backProject (const Spectrum& Y, const std::vector<CMat>& W, const Reduction& R, int ref)
{
    const int N = Y.M;
    std::vector<std::vector<cf>> out ((size_t) N, std::vector<cf> ((size_t) Y.F * (size_t) Y.T));
    parallelFor (Y.F, [&] (int f0, int f1)
    {
        for (int f = f0; f < f1; ++f)
        {
            CMat Wi = W[(size_t) f];
            if (! invert (Wi))
                Wi = CMat::identity (N);
            std::vector<cd> scale ((size_t) N);
            for (int n = 0; n < N; ++n)
            {
                if (R.identity)
                    scale[(size_t) n] = Wi (ref, n);
                else
                {
                    cd acc = 0.0;   // (Pinv Wi)(ref, n)
                    for (int k = 0; k < N; ++k)
                        acc += R.Pinv[(size_t) f][(size_t) ref * (size_t) N + (size_t) k] * Wi (k, n);
                    scale[(size_t) n] = acc;
                }
            }
            for (int t = 0; t < Y.T; ++t)
                for (int n = 0; n < N; ++n)
                    out[(size_t) n][(size_t) f * (size_t) Y.T + (size_t) t] = cf (scale[(size_t) n] * cd (Y.at (f, t, n)));
        }
    });
    return out;
}

// ------------------------------------------------------------ AuxIVA / ILRMA

struct IvaState
{
    std::vector<CMat> W;   // per f, N x N (in the reduced space)
};

void runIva (const Spectrum& Z, const Params& p, bool nmf, IvaState& st,
             const std::function<void (int)>& onIteration)
{
    const int F = Z.F, T = Z.T, N = Z.M;
    if ((int) st.W.size() != F || st.W.front().n != N)
        st.W.assign ((size_t) F, CMat::identity (N));
    Spectrum Y;
    const int K = std::max (1, p.nmfBases);
    // ILRMA's source model: R_n(f,t) = sum_k Tn(f,k) Vn(k,t).
    std::vector<std::vector<double>> Tb, Vb, Rm;
    if (nmf)
    {
        std::mt19937 rng (p.seed);
        std::uniform_real_distribution<double> u (0.1, 1.0);
        Tb.assign ((size_t) N, std::vector<double> ((size_t) F * (size_t) K));
        Vb.assign ((size_t) N, std::vector<double> ((size_t) K * (size_t) T));
        for (auto& v : Tb) for (auto& x : v) x = u (rng);
        for (auto& v : Vb) for (auto& x : v) x = u (rng);
        Rm.assign ((size_t) N, std::vector<double> ((size_t) F * (size_t) T));
    }
    auto computeR = [&] (int n)
    {
        auto& R = Rm[(size_t) n];
        const auto& Tn = Tb[(size_t) n];
        const auto& Vn = Vb[(size_t) n];
        parallelFor (F, [&] (int f0, int f1)
        {
            for (int f = f0; f < f1; ++f)
                for (int t = 0; t < T; ++t)
                {
                    double s = 0.0;
                    for (int k = 0; k < K; ++k)
                        s += Tn[(size_t) f * (size_t) K + (size_t) k] * Vn[(size_t) k * (size_t) T + (size_t) t];
                    R[(size_t) f * (size_t) T + (size_t) t] = std::max (s, kEps);
                }
        });
    };

    std::vector<std::vector<double>> r ((size_t) N, std::vector<double> ((size_t) T));
    for (int it = 0; it < p.iterations; ++it)
    {
        demix (Z, st.W, Y);
        if (! nmf)
        {
            // Laplace source model: weight 1 / r_n(t), r = the source's
            // loudness over every frequency at that moment.
            for (int n = 0; n < N; ++n)
                for (int t = 0; t < T; ++t)
                {
                    double s = 0.0;
                    for (int f = 0; f < F; ++f)
                        s += std::norm (Y.at (f, t, n));
                    r[(size_t) n][(size_t) t] = std::sqrt (s) + 1e-10;
                }
            parallelFor (F, [&] (int f0, int f1)
            {
                for (int f = f0; f < f1; ++f)
                    for (int n = 0; n < N; ++n)
                    {
                        const auto& rn = r[(size_t) n];
                        const CMat V = weightedCov (Z, f, [&rn] (int t) { return 1.0 / rn[(size_t) t]; });
                        ipUpdate (st.W[(size_t) f], V, n);
                    }
            });
        }
        else
        {
            for (int n = 0; n < N; ++n)
            {
                // |y|^2 and the NMF updates (Kitamura et al., multiplicative).
                std::vector<double> P ((size_t) F * (size_t) T);
                for (int f = 0; f < F; ++f)
                    for (int t = 0; t < T; ++t)
                        P[(size_t) f * (size_t) T + (size_t) t] = std::max ((double) std::norm (Y.at (f, t, n)), kEps);
                auto& Tn = Tb[(size_t) n];
                auto& Vn = Vb[(size_t) n];
                computeR (n);
                {
                    const auto& R = Rm[(size_t) n];
                    parallelFor (F, [&] (int f0, int f1)
                    {
                        for (int f = f0; f < f1; ++f)
                            for (int k = 0; k < K; ++k)
                            {
                                double num = 0.0, den = 0.0;
                                for (int t = 0; t < T; ++t)
                                {
                                    const double rr = R[(size_t) f * (size_t) T + (size_t) t];
                                    const double v = Vn[(size_t) k * (size_t) T + (size_t) t];
                                    num += P[(size_t) f * (size_t) T + (size_t) t] * v / (rr * rr);
                                    den += v / rr;
                                }
                                Tn[(size_t) f * (size_t) K + (size_t) k] *= std::sqrt (num / std::max (den, kEps));
                                Tn[(size_t) f * (size_t) K + (size_t) k] = std::max (Tn[(size_t) f * (size_t) K + (size_t) k], kEps);
                            }
                    });
                }
                computeR (n);
                {
                    const auto& R = Rm[(size_t) n];
                    parallelFor (T, [&] (int t0, int t1)
                    {
                        for (int t = t0; t < t1; ++t)
                            for (int k = 0; k < K; ++k)
                            {
                                double num = 0.0, den = 0.0;
                                for (int f = 0; f < F; ++f)
                                {
                                    const double rr = R[(size_t) f * (size_t) T + (size_t) t];
                                    const double tv = Tn[(size_t) f * (size_t) K + (size_t) k];
                                    num += P[(size_t) f * (size_t) T + (size_t) t] * tv / (rr * rr);
                                    den += tv / rr;
                                }
                                Vn[(size_t) k * (size_t) T + (size_t) t] *= std::sqrt (num / std::max (den, kEps));
                                Vn[(size_t) k * (size_t) T + (size_t) t] = std::max (Vn[(size_t) k * (size_t) T + (size_t) t], kEps);
                            }
                    });
                }
                computeR (n);
            }
            parallelFor (F, [&] (int f0, int f1)
            {
                for (int f = f0; f < f1; ++f)
                    for (int n = 0; n < N; ++n)
                    {
                        const auto& R = Rm[(size_t) n];
                        const CMat V = weightedCov (Z, f, [&R, f, T] (int t) { return 1.0 / R[(size_t) f * (size_t) T + (size_t) t]; });
                        ipUpdate (st.W[(size_t) f], V, n);
                    }
            });
            // Normalise each source's power (keeps the NMF in range).
            demix (Z, st.W, Y);
            for (int n = 0; n < N; ++n)
            {
                double pw = 0.0;
                for (int f = 0; f < F; ++f)
                    for (int t = 0; t < T; ++t)
                        pw += std::norm (Y.at (f, t, n));
                const double lam = std::sqrt (std::max (pw / ((double) F * T), kEps));
                for (int f = 0; f < F; ++f)
                    for (int k = 0; k < N; ++k)
                        st.W[(size_t) f] (n, k) /= lam;
                for (auto& x : Tb[(size_t) n]) x = std::max (x / (lam * lam), kEps);
            }
        }
        onIteration (it);
    }
}

// ------------------------------------------------------------ cACGMM

/** Correlation of two profiles (Pearson). */
double corr (const double* a, const double* b, int n)
{
    double ma = 0.0, mb = 0.0;
    for (int i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= std::max (1, n); mb /= std::max (1, n);
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double da = a[i] - ma, db = b[i] - mb;
        sab += da * db; saa += da * da; sbb += db * db;
    }
    return sab / std::sqrt (std::max (saa * sbb, 1e-30));
}

/** The permutation of K items maximising sum score[i][perm[i]] (exhaustive up
    to 7 items, greedy beyond). perm[i] = the item placed at i. */
std::vector<int> bestPermutation (const std::vector<std::vector<double>>& score)
{
    const int K = (int) score.size();
    std::vector<int> perm ((size_t) K);
    std::iota (perm.begin(), perm.end(), 0);
    if (K <= 7)
    {
        std::vector<int> best = perm;
        double bestSum = -1e300;
        do
        {
            double s = 0.0;
            for (int i = 0; i < K; ++i) s += score[(size_t) i][(size_t) perm[(size_t) i]];
            if (s > bestSum) { bestSum = s; best = perm; }
        } while (std::next_permutation (perm.begin(), perm.end()));
        return best;
    }
    std::vector<bool> used ((size_t) K, false), done ((size_t) K, false);
    for (int step = 0; step < K; ++step)
    {
        int bi = -1, bj = -1;
        double b = -1e300;
        for (int i = 0; i < K; ++i)
            if (! done[(size_t) i])
                for (int j = 0; j < K; ++j)
                    if (! used[(size_t) j] && score[(size_t) i][(size_t) j] > b)
                    { b = score[(size_t) i][(size_t) j]; bi = i; bj = j; }
        perm[(size_t) bi] = bj;
        done[(size_t) bi] = true;
        used[(size_t) bj] = true;
    }
    return perm;
}

std::vector<std::vector<cf>> runCacgmm (const Spectrum& S, const Params& p, int N,
                                        const std::function<void (int)>& onIteration)
{
    const int F = S.F, T = S.T, M = S.M;
    const int K = N + (p.noiseClass ? 1 : 0);
    // gamma[(f * K + k) * T + t]
    std::vector<double> gamma ((size_t) F * (size_t) K * (size_t) T);
    std::vector<std::vector<CMat>> B ((size_t) F, std::vector<CMat> ((size_t) K, CMat::identity (M)));
    // Frequency-independent random start: the same affiliation at every
    // frequency keeps most of them in the same order from the start.
    {
        std::mt19937 rng (p.seed);
        std::uniform_real_distribution<double> u (0.0, 1.0);
        std::vector<double> g0 ((size_t) K * (size_t) T);
        for (int t = 0; t < T; ++t)
        {
            double s = 0.0;
            for (int k = 0; k < K; ++k) { g0[(size_t) k * (size_t) T + (size_t) t] = u (rng) + 0.05; s += g0[(size_t) k * (size_t) T + (size_t) t]; }
            for (int k = 0; k < K; ++k) g0[(size_t) k * (size_t) T + (size_t) t] /= s;
        }
        for (int f = 0; f < F; ++f)
            std::copy (g0.begin(), g0.end(), gamma.begin() + (std::ptrdiff_t) ((size_t) f * (size_t) K * (size_t) T));
    }
    // Normalised observations.
    std::vector<cd> Zn ((size_t) F * (size_t) T * (size_t) M);
    std::vector<bool> valid ((size_t) F * (size_t) T);
    parallelFor (F, [&] (int f0, int f1)
    {
        for (int f = f0; f < f1; ++f)
            for (int t = 0; t < T; ++t)
            {
                double nn = 0.0;
                for (int m = 0; m < M; ++m) nn += std::norm (S.at (f, t, m));
                const bool ok = nn > 1e-20;
                valid[(size_t) f * (size_t) T + (size_t) t] = ok;
                const double inv = ok ? 1.0 / std::sqrt (nn) : 0.0;
                for (int m = 0; m < M; ++m)
                    Zn[((size_t) f * (size_t) T + (size_t) t) * (size_t) M + (size_t) m] = cd (S.at (f, t, m)) * inv;
            }
    });

    for (int it = 0; it < p.iterations; ++it)
    {
        parallelFor (F, [&] (int f0, int f1)
        {
            std::vector<double> quad ((size_t) K * (size_t) T);
            for (int f = f0; f < f1; ++f)
            {
                double* g = gamma.data() + (size_t) f * (size_t) K * (size_t) T;
                const cd* z = Zn.data() + (size_t) f * (size_t) T * (size_t) M;
                // M-step: pi_k and B_k (fixed point from the previous B_k).
                std::vector<double> pis ((size_t) K), logDet ((size_t) K);
                std::vector<CMat> Binv ((size_t) K);
                for (int k = 0; k < K; ++k)
                {
                    CMat bi = B[(size_t) f][(size_t) k];
                    if (! invert (bi)) bi = CMat::identity (M);
                    CMat nb (M);
                    double gs = 0.0;
                    for (int t = 0; t < T; ++t)
                    {
                        if (! valid[(size_t) f * (size_t) T + (size_t) t]) continue;
                        const double gk = g[(size_t) k * (size_t) T + (size_t) t];
                        gs += gk;
                        const cd* zt = z + (size_t) t * (size_t) M;
                        cd q = 0.0;
                        for (int i = 0; i < M; ++i)
                        {
                            cd v = 0.0;
                            for (int j = 0; j < M; ++j) v += bi (i, j) * zt[j];
                            q += std::conj (zt[i]) * v;
                        }
                        const double w = gk / std::max (q.real(), 1e-10);
                        for (int i = 0; i < M; ++i)
                            for (int j = 0; j < M; ++j)
                                nb (i, j) += w * zt[i] * std::conj (zt[j]);
                    }
                    double tr = 0.0;
                    for (int i = 0; i < M; ++i) tr += nb (i, i).real();
                    if (tr <= 1e-30) nb = CMat::identity (M);
                    else
                    {
                        for (auto& v : nb.a) v *= (double) M / tr;   // trace M: cACG is scale-free
                        for (int i = 0; i < M; ++i) nb (i, i) += 1e-6;
                    }
                    B[(size_t) f][(size_t) k] = nb;
                    pis[(size_t) k] = std::max (gs / std::max (1, T), 1e-8);
                    CMat inv = nb;
                    double ld = 0.0;
                    if (! invert (inv, &ld)) { inv = CMat::identity (M); ld = 0.0; }
                    Binv[(size_t) k] = std::move (inv);
                    logDet[(size_t) k] = ld;
                }
                // E-step: posteriors.
                for (int t = 0; t < T; ++t)
                {
                    if (! valid[(size_t) f * (size_t) T + (size_t) t])
                    {
                        for (int k = 0; k < K; ++k) g[(size_t) k * (size_t) T + (size_t) t] = 1.0 / K;
                        continue;
                    }
                    const cd* zt = z + (size_t) t * (size_t) M;
                    double mx = -1e300;
                    for (int k = 0; k < K; ++k)
                    {
                        cd q = 0.0;
                        const CMat& bi = Binv[(size_t) k];
                        for (int i = 0; i < M; ++i)
                        {
                            cd v = 0.0;
                            for (int j = 0; j < M; ++j) v += bi (i, j) * zt[j];
                            q += std::conj (zt[i]) * v;
                        }
                        const double l = std::log (pis[(size_t) k]) - logDet[(size_t) k]
                                         - M * std::log (std::max (q.real(), 1e-30));
                        quad[(size_t) k * (size_t) T + (size_t) t] = l;
                        mx = std::max (mx, l);
                    }
                    double s = 0.0;
                    for (int k = 0; k < K; ++k)
                    {
                        const double e = std::exp (quad[(size_t) k * (size_t) T + (size_t) t] - mx);
                        g[(size_t) k * (size_t) T + (size_t) t] = e;
                        s += e;
                    }
                    for (int k = 0; k < K; ++k) g[(size_t) k * (size_t) T + (size_t) t] /= s;
                }
            }
        });
        onIteration (it);
    }

    // Permutation alignment: every frequency's classes matched to the mean
    // time profile of the others (a few passes).
    std::vector<double> centroid ((size_t) K * (size_t) T, 0.0);
    for (int pass = 0; pass < 4; ++pass)
    {
        std::fill (centroid.begin(), centroid.end(), 0.0);
        for (int f = 0; f < F; ++f)
            for (size_t i = 0; i < centroid.size(); ++i)
                centroid[i] += gamma[(size_t) f * (size_t) K * (size_t) T + i];
        parallelFor (F, [&] (int f0, int f1)
        {
            std::vector<double> tmp ((size_t) K * (size_t) T);
            std::vector<CMat> bt ((size_t) K);
            for (int f = f0; f < f1; ++f)
            {
                double* g = gamma.data() + (size_t) f * (size_t) K * (size_t) T;
                std::vector<std::vector<double>> score ((size_t) K, std::vector<double> ((size_t) K));
                for (int i = 0; i < K; ++i)
                    for (int j = 0; j < K; ++j)
                        score[(size_t) i][(size_t) j] = corr (centroid.data() + (size_t) i * (size_t) T, g + (size_t) j * (size_t) T, T);
                const auto perm = bestPermutation (score);
                std::copy (g, g + (size_t) K * (size_t) T, tmp.begin());
                for (int k = 0; k < K; ++k)
                {
                    std::copy (tmp.begin() + (std::ptrdiff_t) ((size_t) perm[(size_t) k] * (size_t) T),
                               tmp.begin() + (std::ptrdiff_t) ((size_t) (perm[(size_t) k] + 1) * (size_t) T),
                               g + (size_t) k * (size_t) T);
                    bt[(size_t) k] = B[(size_t) f][(size_t) perm[(size_t) k]];
                }
                B[(size_t) f] = bt;
            }
        });
    }

    // The noise class: the most diffuse one (largest eigenvalue the smallest
    // share of the trace, averaged over frequencies).
    int noise = -1;
    if (p.noiseClass)
    {
        std::vector<double> peak ((size_t) K, 0.0);
        for (int f = 0; f < F; f += std::max (1, F / 64))
            for (int k = 0; k < K; ++k)
            {
                std::vector<double> vals;
                CMat E;
                eigHermitian (B[(size_t) f][(size_t) k], vals, E);
                peak[(size_t) k] += vals.front() / std::max (1e-12, std::accumulate (vals.begin(), vals.end(), 0.0));
            }
        noise = (int) (std::min_element (peak.begin(), peak.end()) - peak.begin());
    }

    std::vector<int> outClasses;
    for (int k = 0; k < K; ++k)
        if (k != noise) outClasses.push_back (k);
    outClasses.resize ((size_t) N);

    const int ref = std::clamp (p.refChannel, 0, M - 1);
    std::vector<std::vector<cf>> out ((size_t) N, std::vector<cf> ((size_t) F * (size_t) T));
    parallelFor (F, [&] (int f0, int f1)
    {
        for (int f = f0; f < f1; ++f)
        {
            const double* g = gamma.data() + (size_t) f * (size_t) K * (size_t) T;
            for (int o = 0; o < N; ++o)
            {
                const int k = outClasses[(size_t) o];
                if (! p.beamformer)
                {
                    for (int t = 0; t < T; ++t)
                        out[(size_t) o][(size_t) f * (size_t) T + (size_t) t]
                            = S.at (f, t, ref) * (float) g[(size_t) k * (size_t) T + (size_t) t];
                    continue;
                }
                // MVDR (Souden): w = Phi_n^-1 Phi_s u_ref / tr (Phi_n^-1 Phi_s).
                CMat Ps (M), Pn (M);
                double gs = 0.0, gn = 0.0;
                for (int t = 0; t < T; ++t)
                {
                    const double gk = g[(size_t) k * (size_t) T + (size_t) t];
                    gs += gk; gn += 1.0 - gk;
                    for (int i = 0; i < M; ++i)
                    {
                        const cd xi = S.at (f, t, i);
                        for (int j = 0; j < M; ++j)
                        {
                            const cd v = xi * std::conj (cd (S.at (f, t, j)));
                            Ps (i, j) += gk * v;
                            Pn (i, j) += (1.0 - gk) * v;
                        }
                    }
                }
                double trn = 0.0;
                for (int i = 0; i < M; ++i) trn += Pn (i, i).real();
                for (int i = 0; i < M; ++i) Pn (i, i) += 1e-6 * trn / M + 1e-20;
                for (auto& v : Ps.a) v /= std::max (gs, 1e-12);
                for (auto& v : Pn.a) v /= std::max (gn, 1e-12);
                CMat Pni = Pn;
                if (! invert (Pni)) Pni = CMat::identity (M);
                const CMat Q = mul (Pni, Ps);
                cd tr = 0.0;
                for (int i = 0; i < M; ++i) tr += Q (i, i);
                std::vector<cd> w ((size_t) M);
                for (int i = 0; i < M; ++i)
                    w[(size_t) i] = std::abs (tr) > 1e-20 ? Q (i, ref) / tr : cd (i == ref ? 1.0 : 0.0);
                for (int t = 0; t < T; ++t)
                {
                    cd y = 0.0;
                    for (int i = 0; i < M; ++i) y += std::conj (w[(size_t) i]) * cd (S.at (f, t, i));
                    out[(size_t) o][(size_t) f * (size_t) T + (size_t) t] = cf (y);
                }
            }
        }
    });
    return out;
}

// ------------------------------------------------------------ FastMNMF2

std::vector<std::vector<cf>> runFastMnmf2 (const Spectrum& S, const Params& p, int N,
                                           const std::function<void (int)>& onIteration)
{
    const int F = S.F, T = S.T, M = S.M, K = std::max (1, p.nmfBases);
    const size_t FT = (size_t) F * (size_t) T;
    std::mt19937 rng (p.seed);
    std::uniform_real_distribution<double> u (0.1, 1.0);
    std::vector<CMat> Q ((size_t) F, CMat::identity (M));
    std::vector<double> G ((size_t) N * (size_t) M, 1e-2);
    for (int n = 0; n < N; ++n) G[(size_t) n * (size_t) M + (size_t) (n % M)] = 1.0;
    std::vector<double> Wn ((size_t) N * (size_t) F * (size_t) K), Hn ((size_t) N * (size_t) K * (size_t) T);
    for (auto& x : Wn) x = u (rng);
    for (auto& x : Hn) x = u (rng);
    std::vector<double> Xt (FT * (size_t) M), Yh (FT * (size_t) M), lam ((size_t) N * FT);
    auto W_ = [&] (int n, int f, int k) -> double& { return Wn[((size_t) n * (size_t) F + (size_t) f) * (size_t) K + (size_t) k]; };
    auto H_ = [&] (int n, int k, int t) -> double& { return Hn[((size_t) n * (size_t) K + (size_t) k) * (size_t) T + (size_t) t]; };

    auto computeXt = [&]
    {
        parallelFor (F, [&] (int f0, int f1)
        {
            for (int f = f0; f < f1; ++f)
                for (int t = 0; t < T; ++t)
                    for (int m = 0; m < M; ++m)
                    {
                        cd acc = 0.0;
                        for (int j = 0; j < M; ++j) acc += Q[(size_t) f] (m, j) * cd (S.at (f, t, j));
                        Xt[((size_t) f * (size_t) T + (size_t) t) * (size_t) M + (size_t) m] = std::max (std::norm (acc), kEps);
                    }
        });
    };
    auto computeModel = [&]
    {
        parallelFor (F, [&] (int f0, int f1)
        {
            for (int f = f0; f < f1; ++f)
                for (int t = 0; t < T; ++t)
                {
                    const size_t ft = (size_t) f * (size_t) T + (size_t) t;
                    for (int m = 0; m < M; ++m) Yh[ft * (size_t) M + (size_t) m] = 0.0;
                    for (int n = 0; n < N; ++n)
                    {
                        double l = 0.0;
                        for (int k = 0; k < K; ++k) l += W_ (n, f, k) * H_ (n, k, t);
                        l = std::max (l, kEps);
                        lam[(size_t) n * FT + ft] = l;
                        for (int m = 0; m < M; ++m) Yh[ft * (size_t) M + (size_t) m] += l * G[(size_t) n * (size_t) M + (size_t) m];
                    }
                    for (int m = 0; m < M; ++m) Yh[ft * (size_t) M + (size_t) m] = std::max (Yh[ft * (size_t) M + (size_t) m], kEps);
                }
        });
    };
    computeXt();
    computeModel();
    std::vector<double> a ((size_t) N * FT), b ((size_t) N * FT);
    auto computeAB = [&]
    {
        // a_n = sum_m g_nm Xt / Yh^2, b_n = sum_m g_nm / Yh
        parallelFor (F, [&] (int f0, int f1)
        {
            for (int f = f0; f < f1; ++f)
                for (int t = 0; t < T; ++t)
                {
                    const size_t ft = (size_t) f * (size_t) T + (size_t) t;
                    for (int n = 0; n < N; ++n)
                    {
                        double sa = 0.0, sb = 0.0;
                        for (int m = 0; m < M; ++m)
                        {
                            const double y = Yh[ft * (size_t) M + (size_t) m];
                            const double g = G[(size_t) n * (size_t) M + (size_t) m];
                            sa += g * Xt[ft * (size_t) M + (size_t) m] / (y * y);
                            sb += g / y;
                        }
                        a[(size_t) n * FT + ft] = sa;
                        b[(size_t) n * FT + ft] = sb;
                    }
                }
        });
    };

    for (int it = 0; it < p.iterations; ++it)
    {
        // W (spectral bases).
        computeAB();
        parallelFor (F, [&] (int f0, int f1)
        {
            for (int f = f0; f < f1; ++f)
                for (int n = 0; n < N; ++n)
                    for (int k = 0; k < K; ++k)
                    {
                        double num = 0.0, den = 0.0;
                        for (int t = 0; t < T; ++t)
                        {
                            const size_t ft = (size_t) f * (size_t) T + (size_t) t;
                            const double h = H_ (n, k, t);
                            num += h * a[(size_t) n * FT + ft];
                            den += h * b[(size_t) n * FT + ft];
                        }
                        W_ (n, f, k) = std::max (W_ (n, f, k) * std::sqrt (num / std::max (den, kEps)), kEps);
                    }
        });
        computeModel();
        // H (activations).
        computeAB();
        parallelFor (T, [&] (int t0, int t1)
        {
            for (int t = t0; t < t1; ++t)
                for (int n = 0; n < N; ++n)
                    for (int k = 0; k < K; ++k)
                    {
                        double num = 0.0, den = 0.0;
                        for (int f = 0; f < F; ++f)
                        {
                            const size_t ft = (size_t) f * (size_t) T + (size_t) t;
                            const double w = W_ (n, f, k);
                            num += w * a[(size_t) n * FT + ft];
                            den += w * b[(size_t) n * FT + ft];
                        }
                        H_ (n, k, t) = std::max (H_ (n, k, t) * std::sqrt (num / std::max (den, kEps)), kEps);
                    }
        });
        computeModel();
        // G (spatial weights, frequency-independent).
        for (int n = 0; n < N; ++n)
            for (int m = 0; m < M; ++m)
            {
                double num = 0.0, den = 0.0;
                for (size_t ft = 0; ft < FT; ++ft)
                {
                    const double y = Yh[ft * (size_t) M + (size_t) m];
                    const double l = lam[(size_t) n * FT + ft];
                    num += l * Xt[ft * (size_t) M + (size_t) m] / (y * y);
                    den += l / y;
                }
                G[(size_t) n * (size_t) M + (size_t) m] = std::max (G[(size_t) n * (size_t) M + (size_t) m]
                                                                    * std::sqrt (num / std::max (den, kEps)), kEps);
            }
        computeModel();
        // Q (joint diagonaliser), iterative projection row by row.
        parallelFor (F, [&] (int f0, int f1)
        {
            for (int f = f0; f < f1; ++f)
                for (int m = 0; m < M; ++m)
                {
                    CMat V (M);
                    for (int t = 0; t < T; ++t)
                    {
                        const double wgt = 1.0 / Yh[((size_t) f * (size_t) T + (size_t) t) * (size_t) M + (size_t) m];
                        for (int i = 0; i < M; ++i)
                        {
                            const cd xi = cd (S.at (f, t, i)) * wgt;
                            for (int j = 0; j < M; ++j)
                                V (i, j) += xi * std::conj (cd (S.at (f, t, j)));
                        }
                    }
                    double tr = 0.0;
                    for (auto& v : V.a) v /= std::max (1, T);
                    for (int i = 0; i < M; ++i) tr += V (i, i).real();
                    for (int i = 0; i < M; ++i) V (i, i) += 1e-9 * tr / M + 1e-20;
                    ipUpdate (Q[(size_t) f], V, m);
                }
        });
        computeXt();
        // Normalisation (Q's scale into W, G sums to one, W columns sum to one).
        for (int f = 0; f < F; ++f)
        {
            double s = 0.0;
            for (const auto& v : Q[(size_t) f].a) s += std::norm (v);
            const double phi = std::max (s / M, kEps);
            for (auto& v : Q[(size_t) f].a) v /= std::sqrt (phi);
            for (int n = 0; n < N; ++n)
                for (int k = 0; k < K; ++k) W_ (n, f, k) /= phi;
            for (int t = 0; t < T; ++t)
                for (int m = 0; m < M; ++m)
                    Xt[((size_t) f * (size_t) T + (size_t) t) * (size_t) M + (size_t) m] /= phi;
        }
        for (int n = 0; n < N; ++n)
        {
            double mu = 0.0;
            for (int m = 0; m < M; ++m) mu += G[(size_t) n * (size_t) M + (size_t) m];
            mu = std::max (mu, kEps);
            for (int m = 0; m < M; ++m) G[(size_t) n * (size_t) M + (size_t) m] /= mu;
            for (int f = 0; f < F; ++f)
                for (int k = 0; k < K; ++k) W_ (n, f, k) *= mu;
            for (int k = 0; k < K; ++k)
            {
                double nu = 0.0;
                for (int f = 0; f < F; ++f) nu += W_ (n, f, k);
                nu = std::max (nu, kEps);
                for (int f = 0; f < F; ++f) W_ (n, f, k) /= nu;
                for (int t = 0; t < T; ++t) H_ (n, k, t) *= nu;
            }
        }
        computeModel();
        onIteration (it);
    }

    // Wiener filter in the diagonalised space, back to the reference mic.
    const int ref = std::clamp (p.refChannel, 0, M - 1);
    std::vector<std::vector<cf>> out ((size_t) N, std::vector<cf> (FT));
    parallelFor (F, [&] (int f0, int f1)
    {
        for (int f = f0; f < f1; ++f)
        {
            CMat Qi = Q[(size_t) f];
            if (! invert (Qi)) Qi = CMat::identity (M);
            std::vector<cd> xt ((size_t) M);
            for (int t = 0; t < T; ++t)
            {
                const size_t ft = (size_t) f * (size_t) T + (size_t) t;
                for (int m = 0; m < M; ++m)
                {
                    cd acc = 0.0;
                    for (int j = 0; j < M; ++j) acc += Q[(size_t) f] (m, j) * cd (S.at (f, t, j));
                    xt[(size_t) m] = acc;
                }
                for (int n = 0; n < N; ++n)
                {
                    cd y = 0.0;
                    const double l = lam[(size_t) n * FT + ft];
                    for (int m = 0; m < M; ++m)
                        y += Qi (ref, m) * (l * G[(size_t) n * (size_t) M + (size_t) m] / Yh[ft * (size_t) M + (size_t) m]) * xt[(size_t) m];
                    out[(size_t) n][ft] = cf (y);
                }
            }
        }
    });
    return out;
}

// ------------------------------------------------------------ one block

std::vector<std::vector<float>> separateBlock (const std::vector<std::vector<float>>& in, int64_t start, int64_t len,
                                               const Params& p, int N, IvaState& iva,
                                               const std::function<void (int)>& onIteration)
{
    const int Nfft = p.fftSize;
    const Spectrum S = analyse (in, start, len, Nfft);
    const int ref = std::clamp (p.refChannel, 0, S.M - 1);
    std::vector<std::vector<cf>> spectra;
    switch (p.method)
    {
        case Method::auxIva:
        case Method::ilrma:
        {
            const Reduction R = reduce (S, N);
            const Spectrum Z = project (S, R);
            if (! R.identity) iva.W.clear();   // a new PCA basis per block: start afresh
            runIva (Z, p, p.method == Method::ilrma, iva, onIteration);
            Spectrum Y;
            demix (Z, iva.W, Y);
            spectra = backProject (Y, iva.W, R, ref);
            break;
        }
        case Method::cacgmm:    spectra = runCacgmm (S, p, N, onIteration); break;
        case Method::fastMnmf2: spectra = runFastMnmf2 (S, p, N, onIteration); break;
    }
    std::vector<std::vector<float>> out;
    for (const auto& Y : spectra)
        out.push_back (synthesise (Y, S.F, S.T, Nfft, len));
    return out;
}
} // namespace

// ------------------------------------------------------------ public

std::vector<float> stftRoundTrip (const std::vector<float>& x, int fftSize)
{
    const Spectrum S = analyse ({ x }, 0, (int64_t) x.size(), fftSize);
    std::vector<cf> Y ((size_t) S.F * (size_t) S.T);
    for (int f = 0; f < S.F; ++f)
        for (int t = 0; t < S.T; ++t)
            Y[(size_t) f * (size_t) S.T + (size_t) t] = S.at (f, t, 0);
    return synthesise (Y, S.F, S.T, fftSize, (int64_t) x.size());
}

std::vector<int> matchOrder (const std::vector<std::vector<float>>& prev, const std::vector<std::vector<float>>& cur)
{
    const int N = (int) std::min (prev.size(), cur.size());
    std::vector<std::vector<double>> score ((size_t) N, std::vector<double> ((size_t) N));
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
        {
            const auto& a = prev[(size_t) i];
            const auto& b = cur[(size_t) j];
            const size_t n = std::min (a.size(), b.size());
            double sab = 0.0, saa = 0.0, sbb = 0.0;
            for (size_t k = 0; k < n; ++k)
            {
                sab += (double) a[k] * b[k];
                saa += (double) a[k] * a[k];
                sbb += (double) b[k] * b[k];
            }
            score[(size_t) i][(size_t) j] = std::abs (sab) / std::sqrt (std::max (saa * sbb, 1e-30));
        }
    return bestPermutation (score);
}

bool separate (const std::vector<std::vector<float>>& in, double sampleRate, const Params& pIn,
               std::vector<std::vector<float>>& out, std::string* errOut, const Progress& progress)
{
    auto fail = [errOut] (const char* m) { if (errOut != nullptr) *errOut = m; return false; };
    out.clear();
    const int M = (int) in.size();
    if (M < 2)
        return fail ("Blind source separation needs at least two channels.");
    const int64_t L = (int64_t) in.front().size();
    for (const auto& c : in)
        if ((int64_t) c.size() != L)
            return fail ("The channels do not have the same length.");
    Params p = pIn;
    p.fftSize = std::clamp (juce::nextPowerOfTwo (std::max (64, p.fftSize)), 64, 16384);
    p.iterations = std::clamp (p.iterations, 1, 1000);
    p.nmfBases = std::clamp (p.nmfBases, 1, 64);
    p.refChannel = std::clamp (p.refChannel, 0, M - 1);
    const int N = sourcesFor (p, M);
    if (L < p.fftSize * 2)
        return fail ("The selection is too short for this FFT size.");

    // Blocks: bounded memory on long inputs.
    const int64_t blockLen = std::max<int64_t> ((int64_t) p.fftSize * 16,
                                                (int64_t) std::llround (std::max (1.0, p.blockSeconds) * sampleRate));
    const int64_t overlap = std::clamp<int64_t> ((int64_t) std::llround (std::max (0.0, p.overlapSeconds) * sampleRate),
                                                 (int64_t) p.fftSize * 2, blockLen / 4);
    std::vector<std::pair<int64_t, int64_t>> blocks;
    if (L <= blockLen + blockLen / 4)
        blocks.push_back ({ 0, L });
    else
        for (int64_t s = 0; s < L; s += blockLen - overlap)
        {
            const int64_t e = std::min (L, s + blockLen);
            blocks.push_back ({ s, e - s });
            if (e >= L) break;
        }

    out.assign ((size_t) N, std::vector<float> ((size_t) L, 0.0f));
    IvaState iva;
    std::vector<std::vector<float>> prevTail;
    try
    {
        for (size_t bi = 0; bi < blocks.size(); ++bi)
        {
            const auto [bs, bl] = blocks[bi];
            auto onIt = [&] (int it)
            {
                if (progress && ! progress (((double) bi + (it + 1.0) / p.iterations) / (double) blocks.size()))
                    throw Cancelled {};
            };
            auto y = separateBlock (in, bs, bl, p, N, iva, onIt);
            // Same order as the previous block, by the overlap.
            const int64_t ov = bi == 0 ? 0 : std::min<int64_t> (overlap, (blocks[bi - 1].first + blocks[bi - 1].second) - bs);
            if (ov > 0 && ! prevTail.empty())
            {
                std::vector<std::vector<float>> head;
                for (const auto& c : y) head.emplace_back (c.begin(), c.begin() + (std::ptrdiff_t) ov);
                const auto perm = matchOrder (prevTail, head);
                std::vector<std::vector<float>> sorted ((size_t) N);
                for (int i = 0; i < N; ++i) sorted[(size_t) i] = std::move (y[(size_t) perm[(size_t) i]]);
                y = std::move (sorted);
                if (! iva.W.empty())   // the warm start follows the same order
                    for (auto& Wf : iva.W)
                    {
                        CMat w = Wf;
                        for (int i = 0; i < N; ++i)
                            for (int j = 0; j < N; ++j) w (i, j) = Wf (perm[(size_t) i], j);
                        Wf = w;
                    }
            }
            // Cross-fade over the overlap, then copy the rest.
            for (int n = 0; n < N; ++n)
            {
                auto& o = out[(size_t) n];
                const auto& c = y[(size_t) n];
                for (int64_t k = 0; k < bl; ++k)
                {
                    if (k < ov)
                    {
                        const float a = (float) ((k + 0.5) / (double) ov);
                        o[(size_t) (bs + k)] = o[(size_t) (bs + k)] * (1.0f - a) + c[(size_t) k] * a;
                    }
                    else
                        o[(size_t) (bs + k)] = c[(size_t) k];
                }
            }
            prevTail.clear();
            if (bi + 1 < blocks.size())
            {
                const int64_t nextStart = blocks[bi + 1].first;
                for (const auto& c : y) prevTail.emplace_back (c.begin() + (std::ptrdiff_t) (nextStart - bs), c.end());
            }
        }
    }
    catch (const Cancelled&)
    {
        out.clear();
        return fail ("cancelled");
    }
    catch (const std::bad_alloc&)
    {
        out.clear();
        return fail ("Not enough memory: lower the block length or the FFT size.");
    }
    return true;
}
} // namespace sc::bss
