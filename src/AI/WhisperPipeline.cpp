// SpectroCraft — Whisper transcription pipeline (Phase 3 of the AI plan).
// See include/AI/WhisperPipeline.h for the design notes.

#include "AI/WhisperPipeline.h"

#include <AudioEngine.h>
#include <Resampler.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace sc { namespace ai {

static constexpr double kTargetRate = 16000.0;   // WHISPER_SAMPLE_RATE

namespace {

struct VadRegion { int start = 0, end = 0; };   // sample range [start, end) in the 16 kHz mono stream

// Simple RMS voice-activity detector: 30 ms windows at 16 kHz. A window is
// "speech" when its RMS is within 45 dB of the loudest window (and above an
// absolute floor so pure digital silence / very quiet hiss never counts).
// Speech runs separated by less than ~1 s are merged, and regions shorter
// than 0.3 s are dropped. Returns {} for fully silent input.
std::vector<VadRegion> detectSpeechRegions(const float* p, int n)
{
    const int kWin = 480;                 // 30 ms at 16 kHz
    if (n <= 0) return {};
    if (n < kWin * 2) return { { 0, n } };   // too short to bother with

    const int nWin = n / kWin;
    std::vector<float> rms ((size_t) nWin);
    float peak = 0.f;
    for (int w = 0; w < nWin; ++w)
    {
        double acc = 0.0;
        const int s = w * kWin, e = std::min (s + kWin, n);
        for (int i = s; i < e; ++i) acc += (double) p[i] * p[i];
        rms[(size_t) w] = sqrtf ((float) (acc / (e - s)));
        if (rms[(size_t) w] > peak) peak = rms[(size_t) w];
    }

    const float thr = std::max (peak * 3.1623e-2f,   // 45 dB below the loudest window
                                2.0e-4f);           // absolute floor (~ -74 dBFS)

    // Morphological closing: mark every window within ~0.5 s of a speech
    // window as active, so pauses shorter than ~1 s do not fragment an
    // utterance.
    const int kMerge = 15;   // 15 windows x 30 ms = 0.45 s on each side
    std::vector<char> closed ((size_t) nWin, 0);
    for (int w = 0; w < nWin; ++w)
        if (rms[(size_t) w] >= thr)
            for (int g = std::max (0, w - kMerge); g <= std::min (nWin - 1, w + kMerge); ++g)
                closed[(size_t) g] = 1;

    std::vector<VadRegion> out;
    int rs = -1;
    for (int w = 0; w <= nWin; ++w)
    {
        const bool active = w < nWin && closed[(size_t) w];
        if (active && rs < 0) rs = w * kWin;
        if (!active && rs >= 0)
        {
            const int re = std::min (w * kWin, n);
            if (re - rs >= 4800)   // keep regions of at least 0.3 s
                out.push_back ({ rs, re });
            rs = -1;
        }
    }
    return out;
}

// Loaded whisper contexts are cached per (model file, device): loading the
// ggml weights and compiling the OpenVINO encoder takes seconds to a minute,
// and users may re-run transcription (different language/mode) without
// reloading. The device is part of the key so switching CPU/GPU/NPU between
// runs recompiles for the new device instead of reusing an old context.
std::map<std::string, std::unique_ptr<WhisperTranscriber>>& whisperCache()
{
    static std::map<std::string, std::unique_ptr<WhisperTranscriber>> cache;
    return cache;
}

std::mutex& whisperCacheMutex()
{
    static std::mutex m;
    return m;
}

WhisperTranscriber* getCachedWhisper(const juce::File& ggmlModel, const juce::File& irXml,
                                     const std::string& device, std::string* errOut)
{
    const std::string key = ggmlModel.getFullPathName ().toStdString () + "|" + device;
    {
        std::lock_guard<std::mutex> lock (whisperCacheMutex ());
        auto it = whisperCache ().find (key);
        if (it != whisperCache ().end ()) return it->second.get ();
    }

    const juce::File cacheDir = irXml.existsAsFile ()
                                ? irXml.getParentDirectory ().getChildFile ("ov-cache")
                                : juce::File ();
    if (cacheDir != juce::File ())
        cacheDir.createDirectory ();

    auto* raw = new WhisperTranscriber ();
    std::string err;
    if (!raw->load (ggmlModel, irXml, cacheDir, device, &err))
    {
        delete raw;
        if (errOut != nullptr) *errOut = err;
        return nullptr;
    }

    WhisperTranscriber* result = raw;
    {
        std::lock_guard<std::mutex> lock (whisperCacheMutex ());
        whisperCache ()[key].reset (raw);
    }
    return result;
}

} // namespace

bool runWhisperPipeline(AudioEngine& eng, const std::vector<int>& chans,
                        const juce::File& ggmlModel, const juce::File& irXml,
                        const WhisperTranscriber::Options& opts,
                        const std::string& device,
                        std::vector<ScttSegment>& outSegments,
                        std::function<bool(float)> onProgress, std::string* errOut)
{
    auto fail = [&errOut] (const std::string& msg) { if (errOut != nullptr) *errOut = msg; };

    const int64_t nFrames = eng.numSamples ();
    if (nFrames <= 0 || chans.empty ())
    {
        fail ("No audio to transcribe.");
        return false;
    }

    // ------------------------------------------------------------------
    // Phase A: mix the selected channels to mono at native rate and
    // resample to 16 kHz, in ~30 s blocks (bounded memory for long files).
    // SoXR state resets per block — the tiny boundary artifacts are far
    // below whisper's sensitivity.
    // ------------------------------------------------------------------
    const double nativeRate = eng.sampleRate ();
    std::vector<float> pcm16k;
    pcm16k.reserve ((size_t) (nFrames * kTargetRate / nativeRate) + 8192);

    const int64_t blockFrames = (int64_t) (nativeRate * 30.0);
    std::vector<std::vector<float>> chBufs;

    for (int64_t pos = 0; pos < nFrames;)
    {
        const int64_t n = std::min (blockFrames, nFrames - pos);
        juce::AudioBuffer<float> mono (1, (int) n);   // accumulator
        chBufs.assign (chans.size (), std::vector<float> ((size_t) n));

        for (size_t ci = 0; ci < chans.size (); ++ci)
        {
            if (!eng.readSamples (chans[ci], pos, n, chBufs[ci].data ()))
            {
                fail ("Failed to read the audio.");
                return false;
            }
        }

        // Average all channels into one mono stream.
        const float inv = 1.0f / (float) chans.size ();
        float* dst = mono.getWritePointer (0);
        for (int i = 0; i < (int) n; ++i)
        {
            float sum = 0.f;
            for (const auto& buf : chBufs) sum += buf[i];
            dst[i] = sum * inv;
        }

        juce::AudioBuffer<float> outBlock (1, 1);
        if (!sc::Resampler::resample (mono, nativeRate, kTargetRate, outBlock))
        {
            fail ("Failed to resample the audio to 16 kHz.");
            return false;
        }

        pcm16k.insert (pcm16k.end (), outBlock.getReadPointer (0),
                       outBlock.getReadPointer (0) + outBlock.getNumSamples ());

        pos += n;
        // A false return cancels the whole transcription before inference
        // starts; whisper_full itself cannot be interrupted mid-inference.
        if (onProgress && !onProgress ((float) ((pos / (double) nFrames) * 0.1)))
            return false;
    }

    // ------------------------------------------------------------------
    // Phase B: load the model and run inference.
    // ------------------------------------------------------------------
    WhisperTranscriber* wt = getCachedWhisper (ggmlModel, irXml, device, errOut);
    if (wt == nullptr)
        return false;

    // Silence skipping: transcribe only the speech regions found by the VAD.
    // Files with long pauses skip most of the encoder/decoder work; a fully
    // active file yields one region covering everything and behaves exactly
    // like before. Cancellation is checked between regions (whisper_full
    // itself cannot be interrupted mid-inference).
    const auto regions = detectSpeechRegions (pcm16k.data (), (int) pcm16k.size ());
    if (regions.empty ())
        return true;   // nothing to transcribe — no segments, not an error

    double totalSpeech = 0.0;
    for (const auto& r : regions) totalSpeech += (double) (r.end - r.start);

    const bool userLang = !opts.language.empty () && opts.language != "auto";
    std::string pinnedLang;   // detected on the first region, then reused
    double doneSpeech = 0.0;
    std::vector<WhisperSegment> segsAccum;

    for (const auto& region : regions)
    {
        // Map this region's internal progress onto its slice of the overall
        // inference range [0.1, 1] so the bar never moves backwards.
        const double base = doneSpeech / totalSpeech;
        const double frac = (double) (region.end - region.start) / totalSpeech;
        WhisperTranscriber::ProgressFn regionProg;
        if (onProgress)
            regionProg = [op = onProgress, base, frac] (float p, const std::string&)
                         { return op (0.1f + 0.9f * (float) (base + frac * p)); };

        const bool ok = wt->transcribeRegion (
            pcm16k.data () + region.start, region.end - region.start,
            opts, userLang ? opts.language : pinnedLang,
            region.start / 16000.0, segsAccum, regionProg, errOut);
        if (!ok)
            return false;

        // Pin the auto-detected language for the remaining regions.
        if (!userLang && pinnedLang.empty ())
            pinnedLang = wt->lastDetectedLanguage ();

        doneSpeech += (double) (region.end - region.start);
        if (onProgress && !onProgress ((float) (doneSpeech / totalSpeech)))
            return false;   // cancelled between regions
    }

    outSegments.clear ();
    for (auto& s : segsAccum)
        outSegments.push_back ({ s.startSec, s.endSec, std::move (s.text) });

    return true;
}

}} // namespace sc::ai
