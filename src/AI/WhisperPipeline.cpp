// SpectroCraft — Whisper transcription pipeline (Phase 3 of the AI plan).
// See include/AI/WhisperPipeline.h for the design notes.

#include "AI/WhisperPipeline.h"

#include <AudioEngine.h>
#include <Resampler.h>

#include <map>
#include <mutex>

namespace sc { namespace ai {

static constexpr double kTargetRate = 16000.0;   // WHISPER_SAMPLE_RATE

namespace {

// Loaded whisper contexts are cached per model file: loading the ggml weights
// and compiling the OpenVINO encoder takes seconds to a minute, and users may
// re-run transcription (different language/mode) without reloading.
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
                                     std::string* errOut)
{
    const std::string key = ggmlModel.getFullPathName ().toStdString ();
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
    if (!raw->load (ggmlModel, irXml, cacheDir, &err))
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
        if (onProgress) onProgress ((float) ((pos / (double) nFrames) * 0.1));
    }

    // ------------------------------------------------------------------
    // Phase B: load the model and run inference.
    // ------------------------------------------------------------------
    WhisperTranscriber* wt = getCachedWhisper (ggmlModel, irXml, errOut);
    if (wt == nullptr)
        return false;

    std::vector<WhisperSegment> segs;
    WhisperTranscriber::ProgressFn prog;
    if (onProgress)
        prog = [op = onProgress] (float p, const std::string&)
               { return op (0.1f + 0.9f * p); };
    const bool ok = wt->transcribe (pcm16k.data (), (int) pcm16k.size (), opts, segs,
                                    prog, errOut);
    if (!ok)
        return false;

    outSegments.clear ();
    for (auto& s : segs)
        outSegments.push_back ({ s.startSec, s.endSec, std::move (s.text) });

    return true;
}

}} // namespace sc::ai
