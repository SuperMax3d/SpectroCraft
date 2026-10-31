// SpectroCraft - content-aware time scaling (see include/ContentAwareTimeScale.h).
#include "ContentAwareTimeScale.h"
#include "FFTHelpers.h"

#include <algorithm>
#include <cmath>

namespace sc { namespace cat {

static constexpr double kStabilityK = 3.0;   // stability[i] = exp(-k * normDiff[i])
static constexpr double kSelectK    = 3.0;   // pick weight      = exp(-k * regionNormDiff)
static constexpr int    kMinGaps     = 3;    // drop regions shorter than this many gaps

// ---------------------------------------------------------------- pass 1 --

bool analyze (const std::vector<std::vector<float>>& chans, int nchans, double rate, Analysis& out)
{
    if (nchans <= 0 || chans.empty() || rate <= 0.0) return false;
    const int64_t span = (int64_t) chans[0].size();

    // FFT size: a ~1/48 s window, clamped to [1024, 4096] so low-rate files
    // still get usable time resolution.
    double want = rate / 48.0;
    if (want < 1024.0) want = 1024.0;
    if (want > 4096.0) want = 4096.0;
    int N = 1024;
    while ((double) N < want) N <<= 1;

    const int H = N / 4;
    // Need at least a handful of gaps for the statistics to mean anything.
    if (span < (int64_t) N + (int64_t) H * 5) return false;

    // Frame i window: [i*H, i*H+N); the last frame must end within span,
    // so the largest valid index is floor((span-N)/H).
    const int F = (int) ((span - N) / H) + 1;
    const int nGaps = F - 1;
    const int nBins = N / 2;

    out.fftSize = N;
    out.hop = H;
    out.numFrames = F;
    out.rate = rate;
    out.diff.assign ((size_t) nGaps, 0.0);
    out.normDiff.assign ((size_t) nGaps, 0.0);
    out.stability.assign ((size_t) nGaps, 1.0);
    out.protectedGap.assign ((size_t) nGaps, false);

    // Hann window.
    std::vector<float> win (N);
    for (int i = 0; i < N; ++i)
        win[i] = 0.5f * (1.0f - std::cos (2.0 * juce::MathConstants<float>::pi * (float) i / (float) N));

    // Per-frame magnitude averaged over the channels, streamed frame by
    // frame so only two frames of bins are kept at a time.
    const int order = (int) std::log2 ((double) N);
    const double invN = 1.0 / (double) N;
    std::vector<float> fftBuf ((size_t) 2 * N, 0.0f);
    std::vector<double> prev ((size_t) nBins), cur ((size_t) nBins);

    for (int f = 0; f < F; ++f)
    {
        const int64_t p = (int64_t) f * H;
        std::fill (cur.begin(), cur.end(), 0.0);
        for (int c = 0; c < nchans; ++c)
        {
            const float* src = chans[c].data() + p;   // p+N <= span by construction
            for (int i = 0; i < N; ++i)
                fftBuf[i] = src[i] * win[i];
            scfft::realForward (fftBuf.data(), order);
            for (int b = 0; b < nBins; ++b)
            {
                const double re = fftBuf[2 * b] * invN;
                const double im = fftBuf[2 * b + 1] * invN;
                cur[b] += std::sqrt (re * re + im * im);
            }
        }
        for (int b = 0; b < nBins; ++b)
            cur[b] /= (double) nchans;

        if (f > 0)
        {
            double d = 0.0;
            for (int b = 0; b < nBins; ++b)
                d += std::abs (cur[b] - prev[b]);
            out.diff[(size_t) (f - 1)] = d / (double) nBins;
        }
        std::swap (prev, cur);
    }

    // Onset/transient detection: local maxima of the frame-to-frame change
    // that reach a relative threshold. Using |diff| (not just rising energy)
    // catches both onsets and offsets. Each transient protects itself plus
    // its neighbours so those areas are almost never cut or duplicated.
    double maxDiff = 0.0;
    for (double v : out.diff) maxDiff = std::max (maxDiff, v);
    if (maxDiff > 1e-9)
    {
        const double thr = 0.3 * maxDiff;
        for (int i = 0; i < nGaps; ++i)
        {
            if (out.diff[(size_t) i] < thr) continue;
            bool localMax = true;
            for (int d = 1; d <= 2 && localMax; ++d)
            {
                if (i - d >= 0 && out.diff[(size_t) (i - d)] > out.diff[(size_t) i]) localMax = false;
                if (i + d < nGaps && out.diff[(size_t) (i + d)] > out.diff[(size_t) i]) localMax = false;
            }
            if (!localMax) continue;
            for (int j = std::max (0, i - 2); j <= std::min (nGaps - 1, i + 2); ++j)
                out.protectedGap[(size_t) j] = true;
        }
    }

    // Normalise the diffs to [0,1] over the whole range and derive stability.
    double mn = 0.0, mx = 0.0;
    for (double v : out.diff) { mn = std::min (mn, v); mx = std::max (mx, v); }
    const double spanD = (mx - mn) > 1e-12 ? (mx - mn) : 1.0;
    for (int i = 0; i < nGaps; ++i)
    {
        out.normDiff[(size_t) i] = (out.diff[(size_t) i] - mn) / spanD;
        out.stability[(size_t) i] = std::exp (-kStabilityK * out.normDiff[(size_t) i]);
    }

    return true;
}

// ------------------------------------------------------------- regions ----

std::vector<Region> findRegions (const Analysis& a, double thresholdStab, int minGaps)
{
    std::vector<Region> out;
    const int n = a.numGaps();
    if (n <= 0 || minGaps < 1) return out;

    int runStart = -1;
    for (int i = 0; i <= n; ++i)
    {
        const bool stable = i < n && !a.protectedGap[(size_t) i]
                            && a.stability[(size_t) i] >= thresholdStab;
        if (!stable)
        {
            if (runStart >= 0 && i - runStart >= minGaps)
            {
                Region r;
                r.gapStart = runStart;
                r.gapCount = i - runStart;
                double s = 0.0;
                for (int j = runStart; j < i; ++j) s += a.normDiff[(size_t) j];
                r.normDiffMean = s / (double) r.gapCount;
                out.push_back (r);
            }
            runStart = -1;
        }
        else if (runStart < 0)
            runStart = i;
    }
    return out;
}

std::vector<Edit> reduceGapsEdits (const Analysis& a, double percentile, int minGaps)
{
    std::vector<Edit> edits;
    const int n = a.numGaps();
    if (n <= 0 || percentile <= 0.0) return edits;   // nothing qualifies at p=0

    // Gaps whose normalised change is at or below the given percentile of all
    // gaps qualify. (Normalising first keeps steady-tone numerical noise from
    // collapsing every threshold to ~0; ties at the cutoff are included so a
    // plateau of equal values cannot fragment the qualifying runs.)
    std::vector<double> sorted (a.normDiff.begin(), a.normDiff.end());
    std::sort (sorted.begin(), sorted.end());
    const int idx = (int) std::min ((double) (n - 1), std::max (0.0, percentile / 100.0 * n));
    const double q = sorted[(size_t) idx];

    int runStart = -1;
    for (int i = 0; i <= n; ++i)
    {
        const bool ok = i < n && !a.protectedGap[(size_t) i] && a.normDiff[(size_t) i] <= q;
        if (!ok)
        {
            if (runStart >= 0 && i - runStart >= minGaps)
            {
                Edit e;
                e.start = (int64_t) runStart * a.hop;
                e.len = (int64_t) (i - runStart) * a.hop;
                e.duplicate = false;
                edits.push_back (e);
            }
            runStart = -1;
        }
        else if (runStart < 0)
            runStart = i;
    }
    return edits;
}

int64_t removableSamplesAtPercentile (const Analysis& a, double percentile, int minGaps)
{
    int64_t total = 0;
    for (const Edit& e : reduceGapsEdits (a, percentile, minGaps))
        total += e.len;
    return total;
}

// --------------------------------------------------------------- planning --

std::vector<Edit> planTimeScaleEdits (const Analysis& a, int64_t deltaSamples, juce::Random& rng)
{
    std::vector<Edit> edits;
    if (deltaSamples == 0 || a.numGaps() <= 0) return edits;

    const bool duplicate = deltaSamples > 0;   // positive: the range must grow
    int64_t budget = std::llabs (deltaSamples);
    const int H = a.hop;

    // Candidate regions at the default threshold (stability P40), with
    // fallbacks for selections that have less stable material than requested:
    // transient-heavy content, or steady tones whose tiny frame-to-frame
    // numerical noise is structured enough to fragment the P40 runs.
    struct RState { Region r; int consumed = 0; };
    std::vector<RState> pool;
    const double thresholds[] = { 40.0, 25.0, 10.0 };
    for (double p : thresholds)
    {
        std::vector<double> st (a.stability.begin(), a.stability.end());
        std::sort (st.begin(), st.end());
        const double thr = st[(size_t) std::min ((double) (st.size() - 1), p / 100.0 * st.size())];
        pool.clear();
        for (const Region& r : findRegions (a, thr, kMinGaps))
            pool.push_back ({ r, 0 });

        int64_t avail = 0;
        for (const auto& rs : pool) avail += (int64_t) rs.r.gapCount * H;
        if (!pool.empty() && avail >= budget) break;   // enough stable material
    }
    if (pool.empty()) return edits;

    // Each region is consumed at most once, so the planned edits never overlap
    // and applyEdits removes/inserts exactly the planned amount. If the
    // selection has less stable material than requested, the change is applied
    // partially (the preview shows what will actually happen).
    while (budget > H / 2)
    {
        // Weighted random pick: exponential preference for smaller spectral
        // difference, light bonus for longer remaining runs.
        std::vector<double> w (pool.size(), 0.0);
        double totalW = 0.0;
        for (size_t i = 0; i < pool.size(); ++i)
            if (pool[i].r.gapCount - pool[i].consumed > 0)
            {
                w[i] = std::exp (-kSelectK * pool[i].r.normDiffMean)
                       * (1.0 + 0.05 * std::log2 (1.0 + (double) (pool[i].r.gapCount - pool[i].consumed)));
                totalW += w[i];
            }
        if (totalW <= 0.0) break;

        double pick = rng.nextDouble() * totalW;
        int chosen = -1;
        for (size_t i = 0; i < w.size(); ++i)
        {
            pick -= w[i];
            if (pick <= 0.0 && w[i] > 0.0) { chosen = (int) i; break; }
        }
        if (chosen < 0) break;

        const int remaining = pool[chosen].r.gapCount - pool[chosen].consumed;
        const int takeGaps = std::min (remaining, (int) ((budget + H - 1) / H));
        Edit e;
        e.start = (int64_t) (pool[chosen].r.gapStart + pool[chosen].consumed) * H;
        e.len = (int64_t) takeGaps * H;
        e.duplicate = duplicate;
        edits.push_back (e);

        pool[chosen].consumed += takeGaps;
        budget -= e.len;
    }

    std::sort (edits.begin(), edits.end (),
               [] (const Edit& x, const Edit& y) { return x.start < y.start; });
    return edits;
}

// --------------------------------------------------------------- applying --

std::vector<std::vector<float>> applyEdits (const std::vector<std::vector<float>>& chans, int nchans,
                                            double rate, const std::vector<Edit>& edits)
{
    if (nchans <= 0 || chans.empty()) return {};
    const int64_t span = (int64_t) chans[0].size();

    // Short crossfade: ~5 ms, capped so it always fits the local material.
    const int xfMax = (int) std::min<int64_t> (std::max<int64_t> (16, (int64_t) std::llround (rate * 0.005)),
                                               span / 8);

    auto sorted = edits;
    std::sort (sorted.begin(), sorted.end (),
               [] (const Edit& x, const Edit& y) { return x.start < y.start; });

    int64_t dupTotal = 0;
    for (const Edit& e : sorted) if (e.duplicate) dupTotal += e.len;

    std::vector<std::vector<float>> out ((size_t) nchans);
    for (auto& o : out) o.reserve ((size_t) span + (size_t) dupTotal);

    int64_t pos = 0;   // next input sample to emit verbatim
    for (size_t ei = 0; ei < sorted.size(); ++ei)
    {
        const Edit e = sorted[ei];
        if (e.len <= 0 || span <= 0) continue;

        const int64_t s = std::max<int64_t> (std::min<int64_t> (e.start, span), pos);
        const int64_t L = std::min<int64_t> (e.len, span - s);
        if (L <= 0) continue;

        // Where the next edit starts: crossfade zones must not eat into it.
        const int64_t nextStart = (ei + 1 < sorted.size ()) ? std::max<int64_t> (sorted[ei + 1].start, pos) : span;
        const int64_t roomAfter = std::max<int64_t> (0, nextStart - (s + L));

        if (!e.duplicate)   // removal: blend the tail before s with the head after s+L
        {
            const int xf = (int) std::min<int64_t> ({ (int64_t) xfMax, s - pos, roomAfter });
            for (int c = 0; c < nchans; ++c)
            {
                auto& o = out[(size_t) c];
                const float* src = chans[(size_t) c].data();
                if (s - xf > pos)
                    o.insert (o.end (), src + pos, src + s - xf);
                for (int i = 0; i < xf; ++i)
                    o.push_back ((src[s - xf + i] + src[s + L + i]) * 0.5f);
            }
            pos = s + L + xf;
        }
        else   // duplication: keep the segment, insert a copy after it with smooth joins
        {
            const int xf1 = (int) std::min<int64_t> ({ (int64_t) xfMax, L / 2 });
            const int xf2 = (int) std::min<int64_t> ({ (int64_t) xfMax, span - (s + L), roomAfter });
            for (int c = 0; c < nchans; ++c)
            {
                auto& o = out[(size_t) c];
                const float* src = chans[(size_t) c].data();
                if (s > pos)
                    o.insert (o.end (), src + pos, src + s);          // verbatim up to the segment
                if (L - xf1 > 0)
                    o.insert (o.end (), src + s, src + s + L - xf1);  // original head
                for (int i = 0; i < xf1; ++i)                        // original tail -> copy head
                    o.push_back ((src[s + L - xf1 + i] + src[s + i]) * 0.5f);
                if (L - xf1 > 0)
                    o.insert (o.end (), src + s + xf1, src + s + L);  // copy middle/tail
                for (int i = 0; i < xf2; ++i)                        // copy tail -> following head
                    o.push_back ((src[s + L - xf2 + i] + src[s + L + i]) * 0.5f);
            }
            pos = s + L + xf2;
        }
    }

    for (int c = 0; c < nchans; ++c)   // tail after the last edit
    {
        auto& o = out[(size_t) c];
        const float* src = chans[(size_t) c].data();
        if (span > pos)
            o.insert (o.end (), src + pos, src + span);
    }

    return out;
}

}}  // namespace sc::cat
