#pragma once
// SpectroCraft - STFT-based spectral masking.
//
// Two consumers share the same overlap-add machinery:
//  - Offline edits (applyMaskedGain / extractMasked): process a sample range of
//    an in-memory AudioEngine so effects can be applied to selected spectral
//    content only, then mixed back into the rest of the signal.
//  - Playback (PlaybackMask): stateful streaming filter used by the audio
//    thread for "Play Spectral Selection" - zeros every bin outside the mask.

#include <JuceHeader.h>
#include "SpectralSelection.h"
#include <memory>
#include <vector>

class AudioEngine;   // fwd (defined in AudioEngine.h)

namespace SpectralMask
{
    // FFT frame size / hop used by both offline and streaming paths.
    int fftSize();     // 2048
    int hopSize();     // 512
    // Extra samples the caller should read on each side of an output range:
    // before it, so a re-prime's first analysis window is complete; after
    // it, so the last frame (which may start at end-1) has its full window.
    inline int contextSamples() { return fftSize() - 1; }

    /** Multiplies by `linearGain` only the (t,f) bins inside `sel`, over the
        sample range [range] of an in-memory engine. Bins outside the mask are
        left untouched, so the result is exactly "effect on the selected
        spectral content, mixed back into the rest". Uses Hann-windowed STFT
        overlap-add with context on both sides; only `range` is written back
        (via replaceSamples, which records an undo patch).
        channels = nullptr affects every channel; otherwise only those listed
        are modified (the others pass through bit-exact).
        linearGain == 1.0f is a guaranteed no-op: nothing is read, written or
        recorded.
        Returns false and fills errOut when the engine cannot be edited. */
    bool applyMaskedGain (AudioEngine& eng, juce::Range<int64_t> range,
                          const SpectralSelection& sel, float linearGain,
                          const juce::String& description,
                          const std::vector<int>* channels = nullptr,
                          std::string* errOut = nullptr);

    /** Extracts the part of the signal inside `sel` over [range] into outA
        (one vector per channel, each of length range.getLength()). The
        complement is original - A. Read-only; works on streamed engines too
        (reads through readSamples). Returns false on failure. */
    bool extractMasked (const AudioEngine& eng, juce::Range<int64_t> range,
                        const SpectralSelection& sel,
                        std::vector<std::vector<float>>& outA);

    /** Stateful streaming mask for the audio thread. process() is called with
        consecutive absolute file-sample ranges; overlap-add state persists
        between calls so no clicks appear at block boundaries. Non-consecutive
        calls (seek/loop/toggle) re-prime cheaply by fast-forwarding to the
        first frame that can contribute to the new range. */
    class PlaybackMask
    {
    public:
        bool matches (double r, int c) const
        {
            return primed && std::abs (rate - r) < 1e-9 && (int) chans.size() == c;
        }

        void prepare (double fileRate, int numChannels);
        void reset();

        /** True when the next process() call whose output starts at `s` will
            re-prime (first block after prepare/reset, or a non-consecutive
            start). Callers should then supply fftSize()-1 samples of context
            before s so the fast-forward is exact. */
        bool willReprimeAt (int64_t s) const { return !primed || lastEnd != s; }

        /** Masks absolute samples [outStart, outStart+outCount) in place for
            every channel: datas[ch] covers absolute file samples
            [dataStart, dataStart + datas[ch].size()) and the masked output is
            written to datas[ch][preOff .. preOff+outCount) where
            preOff = outStart - dataStart.  When willReprimeAt(outStart) is
            true the caller must provide at least fftSize()-1 samples before
            outStart (clamped at file start); it should always provide at
            least contextSamples() beyond outStart+outCount so the last
            analysis windows are complete. */
        void process (std::vector<std::vector<float>>& datas, int64_t dataStart,
                      int64_t outStart, int outCount, const SpectralSelection& sel);

    private:
        struct ChannelState
        {
            std::vector<float> acc;      // synthesis accumulator window
            std::vector<float> winSum;   // running sum of w^2 (normalisation)
            int64_t accStart = -1;       // absolute sample index of acc[0]
        };

        double rate = 0.0;
        bool primed = false;
        int N = 0, H = 0;
        // Default-constructed with the fixed order (N=2048); prepare() keeps
        // it at that size.
        juce::dsp::FFT fft { 11 };
        std::vector<float> window;
        std::vector<ChannelState> chans;
        int64_t nextFrame = 0;           // first analysis frame not yet processed
        int64_t lastEnd = -1;            // absStart + outCount of the previous call
    };
}
