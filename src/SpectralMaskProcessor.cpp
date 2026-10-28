// SpectroCraft - STFT-based spectral masking (offline edits + streaming playback).
#include "SpectralMaskProcessor.h"
#include "AudioEngine.h"
#include <cmath>

namespace SpectralMask
{
    int fftSize()  { return 2048; }
    int hopSize()  { return 512; }

    static const double kTwoPi = 2.0 * juce::MathConstants<double>::pi;

    static std::vector<float>& hannWindow (int N)
    {
        static std::vector<float> w;
        if ((int) w.size() != N)
        {
            w.resize ((size_t) N);
            for (int i = 0; i < N; ++i)
                w[(size_t) i] = (float) (0.5 - 0.5 * std::cos (kTwoPi * i / N));
        }
        return w;
    }

    // ---------------------------------------------------------------- offline

    // Same semantics as AudioEngine's channelAffected: nullptr = all channels,
    // empty list = none.
    static bool channelInList (const std::vector<int>* list, int ch)
    {
        return !list || std::find (list->begin(), list->end(), ch) != list->end();
    }

    static int64_t ceilDiv (int64_t a, int64_t b)   // b > 0
    {
        return a >= 0 ? (a + b - 1) / b : a / b;
    }

    /** Core overlap-add pass over an engine range. For every FFT frame whose
        window intersects the extended range, the bins are multiplied by
        binGain(frameTimeSecs, binFreqHz) and the frames are recombined with
        Hann windows + running w^2 normalisation (exact for any hop). Only
        samples inside `range` are returned in out (one vector per channel).
        Channels not in `chans` pass through bit-exact without an STFT. */
    static bool stftPass (const AudioEngine& eng, juce::Range<int64_t> range,
                          std::function<float (double tSecs, double fHz)> binGain,
                          const std::vector<int>* chans,
                          std::vector<std::vector<float>>& out)
    {
        if (!eng.isFileLoaded()) return false;
        auto r = range.getIntersectionWith ({ 0, eng.numSamples() });
        if (r.getLength() <= 0) return true;   // nothing to do

        const int N = fftSize(), H = hopSize();
        const double rate = eng.sampleRate();

        // Output sample s receives contributions from frames starting at
        // s_k in (s-N, s].  So the earliest frame that can touch r.start is
        // ceil((r0-N+1)/H)*H and the latest one starts before r.end.
        const int64_t kFirst = std::max<int64_t> (0, ceilDiv (r.getStart() - N + 1, H));
        const int64_t kLast  = (r.getEnd() - 1) / H;
        if (kLast < kFirst) return true;

        // Extended read range covering every frame window above.  eEnd may
        // exceed the file end: x is zero-initialised and readSamples only
        // fills what exists, so frames straddling EOF see silence.
        const int64_t eStart = kFirst * (int64_t) H;
        const int64_t eEnd   = kLast * (int64_t) H + N;
        const int64_t extLen = eEnd - eStart;

        out.clear();
        const int nch = eng.numChannels();
        out.resize ((size_t) nch);
        for (int ch = 0; ch < nch; ++ch)
            out[(size_t) ch].assign ((size_t) r.getLength(), 0.0f);

        std::vector<float> x;
        const auto& win = hannWindow (N);
        juce::dsp::FFT fft (11);   // N == 2^11
        std::vector<float> buf ((size_t) N * 2, 0.0f);

        for (int ch = 0; ch < nch; ++ch)
        {
            // One sequential read of the whole extended range (zero-padded at
            // the file edges by initialisation).
            x.assign ((size_t) extLen, 0.0f);
            if (!eng.readSamples (ch, eStart, extLen, x.data()))
                return false;

            if (!channelInList (chans, ch))   // untouched channel: bit-exact copy
            {
                const size_t off = (size_t) (r.getStart() - eStart);
                std::copy (x.begin() + off, x.begin() + off + r.getLength(),
                           out[(size_t) ch].begin());
                continue;
            }

            std::vector<float> acc ((size_t) extLen, 0.0f);
            std::vector<float> winSum ((size_t) extLen, 0.0f);

            for (int64_t k = kFirst; k <= kLast; ++k)
            {
                const int64_t s_k = k * H;
                const size_t off = (size_t) (s_k - eStart);
                float* d = buf.data();

                for (int i = 0; i < N; ++i)
                    d[(size_t) i] = x[off + (size_t) i] * win[(size_t) i];

                fft.performRealOnlyForwardTransform (d, true);

                const double tSecs = (double) s_k / rate;   // frame start time (matches the display)
                for (int b = 0; b <= N / 2; ++b)
                {
                    const float g = binGain (tSecs, (double) b * rate / N);
                    d[(size_t) b * 2]     *= g;
                    d[(size_t) b * 2 + 1] *= g;
                }

                fft.performRealOnlyInverseTransform (d);

                for (int i = 0; i < N; ++i)
                {
                    const float wv = win[(size_t) i];
                    acc[off + (size_t) i]     += wv * d[(size_t) i];
                    winSum[off + (size_t) i]  += wv * wv;
                }
            }

            const size_t outOff = (size_t) (r.getStart() - eStart);
            for (int64_t s = r.getStart(); s < r.getEnd(); ++s)
            {
                const size_t i = outOff + (size_t) (s - r.getStart());
                const float ws = winSum[i];
                out[(size_t) ch][(size_t) (s - r.getStart())] =
                    (ws > 1e-6f) ? acc[i] / ws : 0.0f;
            }
        }
        return true;
    }

    bool applyMaskedGain (AudioEngine& eng, juce::Range<int64_t> range,
                          const SpectralSelection& sel, float linearGain,
                          const juce::String& description,
                          const std::vector<int>* channels, std::string* errOut)
    {
        // Identity gain: leave the file bit-exact - no STFT round-trip (which
        // would introduce ULP-level rounding), no undo entry, no "unsaved".
        if (linearGain == 1.0f) return true;

        auto r = range.getIntersectionWith ({ 0, eng.numSamples() });
        if (r.getLength() <= 0 || sel.empty()) return true;   // nothing to do

        // Fail early on streamed files that cannot be materialised, before
        // spending time on the STFT pass.
        if (!eng.ensureEditable (errOut)) return false;

        std::vector<std::vector<float>> out;
        const float g = linearGain;
        if (!stftPass (eng, r, [&sel, g] (double tSecs, double fHz) {
                return sel.contains (tSecs, fHz) ? g : 1.0f;   // untouched outside the mask
            }, channels, out))
        {
            if (errOut) *errOut = "Spectral masking failed";
            return false;
        }

        return eng.replaceSamples (r, out, description, errOut);
    }

    bool extractMasked (const AudioEngine& eng, juce::Range<int64_t> range,
                        const SpectralSelection& sel,
                        std::vector<std::vector<float>>& outA)
    {
        if (!eng.isFileLoaded()) return false;
        auto r = range.getIntersectionWith ({ 0, eng.numSamples() });
        if (r.getLength() <= 0 || sel.empty()) return true;

        return stftPass (eng, r, [&sel] (double tSecs, double fHz) {
            return sel.contains (tSecs, fHz) ? 1.0f : 0.0f;
        }, nullptr, outA);
    }

    // ------------------------------------------------------- split selection

    /** Deterministic pseudo-random phase in [0, 2*pi) for the synthetic noise
        fill: stable across runs (so tests can verify exact values) and
        uncorrelated between (frame, bin) pairs. */
    static float noisePhase (int64_t frameStart, int bin)
    {
        const double v = std::sin ((double) frameStart * 12.9898
                                   + (double) bin * 78.233) * 43758.5453;
        return (float) ((v - std::floor (v)) * kTwoPi);
    }

    /** Per-frame vertical profile of the selection at time tSecs: for every
        FFT bin, the trapezoidal blend weight (1 across the middle 50% of each
        contiguous selected band, linear ramps to 0 over its top and bottom
        25%) plus the list of bins just outside the bands used for the noise-
        floor estimate. Returns false when no bin is selected at this time. */
    static bool frameWeights (const SpectralSelection& sel, double tSecs,
                              int N, double rate,
                              std::vector<float>& w, std::vector<int>& noiseBins)
    {
        const int half = N / 2;
        const double binHz = rate / N;
        w.assign ((size_t) (half + 1), 0.0f);

        // Vertical profile: which bins are inside the selection at tSecs.
        std::vector<uint8_t> inSel ((size_t) (half + 1), 0);
        for (int b = 0; b <= half; ++b)
            if (sel.contains (tSecs, (double) b * binHz))
                inSel[(size_t) b] = 1;

        // Contiguous runs of selected bins (single-bin speckle is ignored).
        struct Run { int lo, hi; };
        std::vector<Run> runs;
        int start = -1;
        for (int b = 0; b <= half + 1; ++b)
        {
            const bool on = (b <= half) && inSel[(size_t) b];
            if (on && start < 0) start = b;
            if (!on && start >= 0)
            {
                if (b - 1 - start + 1 >= 3)   // need at least 3 bins for a band
                    runs.push_back ({ start, b - 1 });
                start = -1;
            }
        }
        if (runs.empty()) return false;

        // Trapezoidal weight per run: ramps of 25% of the band height at each
        // edge; clamped so very thin bands still peak at 1 in the middle.
        std::vector<uint8_t> isNoise ((size_t) (half + 1), 0);
        for (const auto& rn : runs)
        {
            const double fLo = (double) rn.lo * binHz;
            const double fHi = (double) rn.hi * binHz;
            const double ramp = std::max (binHz, 0.25 * (fHi - fLo));
            for (int b = rn.lo; b <= rn.hi; ++b)
            {
                const double f = (double) b * binHz;
                const float up   = (float) juce::jlimit (0.0, 1.0, (f - fLo) / ramp);
                const float down = (float) juce::jlimit (0.0, 1.0, (fHi - f) / ramp);
                w[(size_t) b] = std::min (up, down);
            }

            // Noise-floor window: 25% of the band height (min 4 bins) on each
            // side, outside the band.
            const int W = std::max (4, (int) std::lround (0.25 * (rn.hi - rn.lo)));
            for (int b = std::max (0, rn.lo - W); b < rn.lo; ++b)
                isNoise[(size_t) b] = 1;
            for (int b = rn.hi + 1; b <= std::min (half, rn.hi + W); ++b)
                isNoise[(size_t) b] = 1;
        }

        noiseBins.clear ();
        int nCount = 0;
        for (int b = 0; b <= half; ++b)
            if (isNoise[(size_t) b]) { noiseBins.push_back (b); ++nCount; }
        if (nCount == 0)   // selection covers the whole spectrum: fall back to
        {
            for (int b = 0; b <= half; ++b)
                if (!inSel[(size_t) b]) noiseBins.push_back (b);
        }
        return true;
    }

    /** Splits an active spectral selection into new channels: one new channel
        per affected source channel, each containing the selected band content
        with a trapezoidal blend (25% linear ramp at each band edge, 50% full
        weight in the middle). The original channels keep everything outside
        the bands; inside them the removed energy is replaced by synthetic
        noise at the per-frame estimated floor level (mean magnitude of the
        bins just outside the selection), so that original + new channel(s)
        reconstruct the pre-edit signal with no hard spectral edge.

        outExtracted[ch] / outResidual[ch] are filled for every channel over
        the synthesis extent reported in outExtent (residual = what stays in
        the source channel; extracted = content of the appended copy). */
    bool splitSpectralSelection (const AudioEngine& eng, juce::Range<int64_t> range,
                                 const SpectralSelection& sel,
                                 const std::vector<int>* channels,
                                 std::vector<std::vector<float>>& outExtracted,
                                 std::vector<std::vector<float>>& outResidual,
                                 juce::Range<int64_t>& outExtent)
    {
        if (!eng.isFileLoaded()) return false;
        auto r = range.getIntersectionWith ({ 0, eng.numSamples() });
        outExtracted.clear ();
        outResidual.clear ();
        outExtent = { 0, 0 };
        if (r.getLength() <= 0 || sel.empty()) return true;   // nothing to do

        const int N = fftSize(), H = hopSize();
        const double rate = eng.sampleRate();
        const int64_t total = eng.numSamples();

        // Frames whose START lies inside the selection's time range carry the
        // mask; their synthesis extent is the output range.
        const int64_t kFirst = std::max<int64_t> (0, ceilDiv (r.getStart(), H));
        const int64_t kLast  = (r.getEnd() - 1) / H;
        if (kLast < kFirst) return true;   // selection shorter than one hop

        const int64_t eStart = kFirst * (int64_t) H;
        const int64_t eEnd   = std::min<int64_t> (kLast * (int64_t) H + N, total);
        outExtent = { eStart, eEnd };

        // Earlier frames (start < r0) have weight 0 everywhere but must still
        // be overlap-added so the residual is exact at eStart.
        const int64_t kCtx = std::max<int64_t> (0, ceilDiv (eStart - N + 1, H));
        const int64_t xStart = kCtx * (int64_t) H;
        const int64_t extLen = eEnd - xStart;

        // The accumulation buffers must cover every frame window: the last
        // frame's window may extend past eEnd (which is clamped to EOF).
        const int64_t bufLen = kLast * (int64_t) H + N - xStart;

        outExtracted.resize ((size_t) eng.numChannels());
        outResidual.resize ((size_t) eng.numChannels());
        for (int ch = 0; ch < eng.numChannels(); ++ch)
        {
            outExtracted[(size_t) ch].assign ((size_t) extLen, 0.0f);
            outResidual[(size_t) ch].assign ((size_t) extLen, 0.0f);
        }

        std::vector<float> x;
        const auto& win = hannWindow (N);
        juce::dsp::FFT fft (11);   // N == 2^11
        std::vector<float> buf ((size_t) N * 2, 0.0f);
        std::vector<float> extBuf ((size_t) N * 2, 0.0f);
        std::vector<float> w;
        std::vector<int> noiseBins;

        for (int ch = 0; ch < eng.numChannels(); ++ch)
        {
            if (!channelInList (channels, ch))
            {
                // Untouched channel: residual is the original signal itself.
                x.assign ((size_t) bufLen, 0.0f);
                if (!eng.readSamples (ch, xStart, bufLen, x.data()))
                    return false;
                std::copy (x.begin(), x.begin() + extLen,
                           outResidual[(size_t) ch].begin());
                continue;   // extracted stays all-zero
            }

            x.assign ((size_t) bufLen, 0.0f);
            if (!eng.readSamples (ch, xStart, bufLen, x.data()))
                return false;

            std::vector<float> accExt ((size_t) bufLen, 0.0f);
            std::vector<float> accRes ((size_t) bufLen, 0.0f);
            std::vector<float> winSum ((size_t) bufLen, 0.0f);

            for (int64_t k = kCtx; k <= kLast; ++k)
            {
                const int64_t s_k = k * (int64_t) H;
                const size_t off = (size_t) (s_k - xStart);

                // Nothing selected at this frame time? Pure pass-through for the
                // residual (no FFT needed: ifft(fft(y)) == y).
                const double tSecs = (double) s_k / rate;
                if (!frameWeights (sel, tSecs, N, rate, w, noiseBins))
                {
                    for (int i = 0; i < N; ++i)
                    {
                        const float wv = win[(size_t) i];
                        accRes[off + (size_t) i] += wv * x[off + (size_t) i];
                        winSum[off + (size_t) i]  += wv * wv;
                    }
                    continue;
                }

                float* d = buf.data();
                for (int i = 0; i < N; ++i)
                    d[(size_t) i] = x[off + (size_t) i] * win[(size_t) i];
                fft.performRealOnlyForwardTransform (d, true);

                // Per-frame noise floor: mean magnitude of the bins just outside
                // the selected bands.
                double nSum = 0.0;
                for (int b : noiseBins)
                    nSum += std::hypot ((double) d[(size_t) b * 2],
                                        (double) d[(size_t) b * 2 + 1]);
                const float Nk = noiseBins.empty () ? 0.0f
                                                    : (float) (nSum / (double) noiseBins.size());

                // Build the extracted and residual spectra bin by bin.
                std::fill (extBuf.begin(), extBuf.end(), 0.0f);
                for (int b = 0; b <= N / 2; ++b)
                {
                    const float re = d[(size_t) b * 2];
                    const float im = d[(size_t) b * 2 + 1];
                    const float wb = w[(size_t) b];

                    if (wb <= 0.0f)
                        continue;   // extracted: silence; residual: original bin

                    const double mag = std::hypot ((double) re, (double) im);
                    // Extracted part: the band content with the noise floor
                    // subtracted from its amplitude, scaled by the blend weight.
                    float sExt = 0.0f;
                    if (mag > 1e-9)
                        sExt = wb * (float) std::max (0.0, 1.0 - Nk / (float) mag);
                    extBuf[(size_t) b * 2]     = re * sExt;
                    extBuf[(size_t) b * 2 + 1] = im * sExt;

                    // Residual part: the unselected fraction of the bin plus
                    // synthetic noise at the floor level (deterministic phase).
                    const float th = noisePhase (s_k, b);
                    d[(size_t) b * 2]     = re * (1.0f - wb) + Nk * std::cos (th) * wb;
                    d[(size_t) b * 2 + 1] = im * (1.0f - wb) + Nk * std::sin (th) * wb;
                }

                fft.performRealOnlyInverseTransform (d);
                fft.performRealOnlyInverseTransform (extBuf.data());

                for (int i = 0; i < N; ++i)
                {
                    const float wv = win[(size_t) i];
                    accExt[off + (size_t) i] += wv * extBuf[(size_t) i];
                    accRes[off + (size_t) i] += wv * d[(size_t) i];
                    winSum[off + (size_t) i]  += wv * wv;
                }
            }

            for (int64_t s = eStart; s < eEnd; ++s)
            {
                const size_t i = (size_t) (s - xStart);
                const float ws = winSum[i];
                outExtracted[(size_t) ch][(size_t) (s - eStart)] =
                    (ws > 1e-6f) ? accExt[i] / ws : 0.0f;
                outResidual[(size_t) ch][(size_t) (s - eStart)] =
                    (ws > 1e-6f) ? accRes[i] / ws : 0.0f;
            }
        }
        return true;
    }

    // ---------------------------------------------------------- playback mask

    void PlaybackMask::prepare (double fileRate, int numChannels)
    {
        rate = fileRate;
        N = fftSize();
        H = hopSize();
        window = hannWindow (N);
        if (fft.getSize() != N)
            fft = juce::dsp::FFT (11);

        chans.clear();
        chans.resize ((size_t) numChannels);
        for (auto& cs : chans)
        {
            cs.acc.assign ((size_t) N, 0.0f);
            cs.winSum.assign ((size_t) N, 0.0f);
            cs.accStart = -1;
        }
        nextFrame = 0;
        lastEnd = -1;
        primed = true;
    }

    void PlaybackMask::reset()
    {
        primed = false;
        nextFrame = 0;
        lastEnd = -1;
    }

    void PlaybackMask::process (std::vector<std::vector<float>>& datas,
                                int64_t dataStart, int64_t outStart, int outCount,
                                const SpectralSelection& sel)
    {
        if (!primed || rate <= 0.0 || chans.empty() || outCount <= 0) return;
        const int64_t end = outStart + (int64_t) outCount;
        const size_t preOff = (size_t) std::max<int64_t> (0, outStart - dataStart);

        // Consecutive call: keep the pending overlap-add state. Anything else
        // (seek, loop wrap, toggle, first block): re-prime by fast-forwarding
        // to the first frame that can contribute to sample `outStart`.  The
        // caller supplies fftSize()-1 samples before outStart in that case,
        // so every analysis window is complete and the zeroed accumulator is
        // exact (at file start there simply is no earlier audio).
        if (!primed || lastEnd != outStart)
        {
            nextFrame = std::max<int64_t> (0, ceilDiv (outStart - N + 1, H));
            for (auto& cs : chans)
            {
                cs.acc.assign ((size_t) N, 0.0f);
                cs.winSum.assign ((size_t) N, 0.0f);
                cs.accStart = nextFrame * (int64_t) H;
            }
        }

        const int64_t kLast = (end - 1) / H;

        // Grow the window so it covers up to the last frame's end.
        const int64_t needEnd = std::max<int64_t> (kLast * (int64_t) H + N,
                                                   nextFrame * (int64_t) H + N);
        for (auto& cs : chans)
        {
            // Invariant: accStart <= nextFrame*H always holds (re-prime sets it
            // to exactly that; the trim anchors at the previous end, which is
            // never past the first unprocessed frame), so pending state below
            // nextFrame*H is valid and must be kept.
            const size_t need = (size_t) std::max<int64_t> (1, needEnd - cs.accStart);
            if (cs.acc.size() < need)
            {
                cs.acc.resize (need, 0.0f);
                cs.winSum.resize (need, 0.0f);
            }
        }

        const double binHz = rate / N;
        std::vector<float> buf ((size_t) N * 2, 0.0f);

        for (int64_t k = nextFrame; k <= kLast; ++k)
        {
            const int64_t s_k = k * (int64_t) H;
            const size_t off0 = (size_t) (s_k - chans[0].accStart);
            const double tSecs = (double) s_k / rate;

            for (size_t ci = 0; ci < datas.size() && ci < chans.size(); ++ci)
            {
                auto& cs = chans[ci];
                auto& data = datas[ci];
                float* d = buf.data();
                const int64_t dataEnd = dataStart + (int64_t) data.size();

                for (int i = 0; i < N; ++i)
                {
                    const int64_t p = s_k + i;
                    d[(size_t) i] = (p >= dataStart && p < dataEnd)
                                     ? data[(size_t) (p - dataStart)] * window[(size_t) i]
                                     : 0.0f;
                }

                fft.performRealOnlyForwardTransform (d, true);

                for (int b = 0; b <= N / 2; ++b)
                {
                    const float g = sel.contains (tSecs, (double) b * binHz) ? 1.0f : 0.0f;
                    d[(size_t) b * 2]     *= g;
                    d[(size_t) b * 2 + 1] *= g;
                }

                fft.performRealOnlyInverseTransform (d);

                for (int i = 0; i < N; ++i)
                {
                    const float wv = window[(size_t) i];
                    cs.acc[off0 + (size_t) i]     += wv * d[(size_t) i];
                    cs.winSum[off0 + (size_t) i]  += wv * wv;
                }
            }
        }

        // Emit the masked output for [outStart, end), per channel.
        const int64_t anchor = chans[0].accStart;
        for (size_t ci = 0; ci < datas.size() && ci < chans.size(); ++ci)
        {
            auto& cs = chans[ci];
            auto& data = datas[ci];
            for (int i = 0; i < outCount; ++i)
            {
                const size_t idx = (size_t) std::max<int64_t> (0, outStart + i - anchor);
                float y = 0.0f;
                if (idx < cs.winSum.size())
                {
                    const float ws = cs.winSum[idx];
                    if (ws > 1e-6f)
                        y = cs.acc[idx] / ws;
                }
                data[preOff + (size_t) i] = y;
            }
        }

        // Trim the window down to exactly N samples anchored at `end`:
        // everything below has been emitted, and every pending frame
        // contribution for future samples lies in [end, kLast*H+N) which is
        // strictly shorter than N.  (Anchoring at (kLast+1)*H would discard
        // contributions to samples in [end, (kLast+1)*H).)
        const int64_t newAnchor = end;
        if (newAnchor > chans[0].accStart)
        {
            for (auto& cs : chans)
            {
                // Rebuild the window as exactly N samples anchored at `end`.
                // The pending contribution region [end, kLast*H+N) is shorter
                // than N, so copy only what exists and zero-fill the rest.
                std::vector<float> na ((size_t) N, 0.0f);
                std::vector<float> nw ((size_t) N, 0.0f);
                const int64_t delta = newAnchor - cs.accStart;
                if (delta >= 0 && (size_t) delta < cs.acc.size())
                {
                    const size_t shift = (size_t) delta;
                    const size_t keep = std::min ((size_t) N, cs.acc.size() - shift);
                    std::copy_n (cs.acc.begin() + shift, (int) keep, na.begin());
                    std::copy_n (cs.winSum.begin() + shift, (int) keep, nw.begin());
                }
                cs.acc.swap (na);
                cs.winSum.swap (nw);
                cs.accStart = newAnchor;
            }
        }

        nextFrame = kLast + 1;
        lastEnd = end;
    }
}
