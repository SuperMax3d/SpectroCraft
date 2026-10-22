// SpectroCraft — Whisper transcription engine (Phase 3 of the AI plan).
// Runs on the prebuilt whisper.cpp b5454 binaries via sc::ai::WhisperRuntime.

#include "AI/WhisperTranscribe.h"
#include "AI/WhisperRuntime.h"

#include <juce_core/juce_core.h>

#include <cctype>
#include <thread>

namespace sc { namespace ai {

namespace {

struct ProgressBridge {
    WhisperTranscriber::ProgressFn fn;
};

// b5454 progress callback signature (identical to v1.6.x).
void whisperProgressCb(struct whisper_context*, struct whisper_state*, int progress, void* user)
{
    auto* b = static_cast<ProgressBridge*>(user);
    if (b != nullptr && b->fn)
    {
        // whisper can report > 100% on short files (timestamp-token seek
        // overshoot); clamp.
        float p = (float) progress / 100.0f;
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        b->fn (p, "transcribing…");
    }
}

void reportError(std::string* errOut, const juce::String& msg)
{
    if (errOut != nullptr) *errOut = msg.toStdString ();
}

std::string trimWhitespace(const char* text)
{
    std::string t (text);
    while (!t.empty () && std::isspace ((unsigned char) t.front ()) != 0) t.erase (t.begin ());
    while (!t.empty () && std::isspace ((unsigned char) t.back ())  != 0) t.pop_back ();
    return t;
}

} // namespace

WhisperTranscriber::~WhisperTranscriber() { unload(); }

void WhisperTranscriber::unload()
{
    if (_ctx != nullptr)
    {
        WhisperRuntime::instance ().freeContext (_ctx);
        _ctx = nullptr;
    }
    _lastLang.clear ();
}

bool WhisperTranscriber::load(const juce::File& ggmlModel, std::string* errOut)
{
    unload();

    auto& rt = WhisperRuntime::instance ();
    if (!rt.load (errOut))
        return false;   // missing/incompatible whisper.dll — message already set

    if (!ggmlModel.existsAsFile ())
    {
        reportError (errOut, "Whisper model file not found: " + ggmlModel.getFullPathName ());
        return false;
    }

    // b5454 context params: CPU-only. flash_attn stays off — measured ~1.7x
    // SLOWER on CPU (the kernels favour GPU). The DTW fields are unused.
    whisper_context_params cparams = {};
    cparams.use_gpu     = false;
    cparams.flash_attn  = false;
    cparams.gpu_device  = 0;

    _ctx = rt.initFromFilePath (ggmlModel.getFullPathName ().toRawUTF8 (), cparams);
    if (_ctx == nullptr)
        reportError (errOut, "Failed to load the Whisper model: " + ggmlModel.getFileName ());
    return _ctx != nullptr;
}

bool WhisperTranscriber::transcribe(const float* samples, int nSamples, const Options& opts,
                                    std::vector<WhisperSegment>& outSegments,
                                    ProgressFn onProgress, std::string* errOut)
{
    if (_ctx == nullptr || samples == nullptr || nSamples <= 0)
    {
        reportError (errOut, "Transcriber is not loaded.");
        return false;
    }

    auto& rt = WhisperRuntime::instance ();
    const bool useBeam = opts.beamSize > 1;
    whisper_full_params params = rt.fullDefaultParams (useBeam ? WHISPER_SAMPLING_BEAM_SEARCH
                                                               : WHISPER_SAMPLING_GREEDY);
    if (useBeam)
        params.beam_search.beam_size = opts.beamSize;

    const std::string lang = opts.language.empty () || opts.language == "auto"
                              ? std::string ("") : opts.language;
    params.language       = lang.empty () ? nullptr : lang.c_str ();
    params.detect_language= lang.empty ();
    params.translate      = opts.translate;

    params.n_threads = opts.threads > 0 ? opts.threads
                                        : (int) std::thread::hardware_concurrency ();
    if (params.n_threads < 1) params.n_threads = 1;

    params.print_progress = false;   // we report progress ourselves
    ProgressBridge bridge { onProgress };
    params.progress_callback         = whisperProgressCb;
    params.progress_callback_user_data = &bridge;

    if (rt.full (_ctx, params, samples, nSamples) != 0)
    {
        reportError (errOut, "Whisper inference failed.");
        return false;
    }

    outSegments.clear ();
    const int n = rt.nSegments (_ctx);
    for (int i = 0; i < n; ++i)
    {
        WhisperSegment seg;
        seg.startSec = (double) rt.segmentT0 (_ctx, i) / 1000.0;
        seg.endSec   = (double) rt.segmentT1 (_ctx, i) / 1000.0;
        const char* text = rt.segmentText (_ctx, i);
        if (text != nullptr)
            seg.text = trimWhitespace (text);
        if (!seg.text.empty ())
            outSegments.push_back (std::move (seg));
    }

    return true;
}

bool WhisperTranscriber::transcribeRegion(const float* samples, int nSamples, const Options& opts,
                                          const std::string& languageIn, double offsetSec,
                                          std::vector<WhisperSegment>& outSegments,
                                          ProgressFn onProgress, std::string* errOut)
{
    if (_ctx == nullptr || samples == nullptr || nSamples <= 0)
    {
        reportError (errOut, "Transcriber is not loaded.");
        return false;
    }

    auto& rt = WhisperRuntime::instance ();
    const bool useBeam = opts.beamSize > 1;
    whisper_full_params params = rt.fullDefaultParams (useBeam ? WHISPER_SAMPLING_BEAM_SEARCH
                                                               : WHISPER_SAMPLING_GREEDY);
    if (useBeam)
        params.beam_search.beam_size = opts.beamSize;

    const std::string lang = languageIn.empty () || languageIn == "auto"
                              ? std::string ("") : languageIn;
    params.language       = lang.empty () ? nullptr : lang.c_str ();
    params.detect_language= lang.empty ();
    params.translate      = opts.translate;

    params.n_threads = opts.threads > 0 ? opts.threads
                                        : (int) std::thread::hardware_concurrency ();
    if (params.n_threads < 1) params.n_threads = 1;

    params.print_progress = false;   // we report progress ourselves
    ProgressBridge bridge { onProgress };
    params.progress_callback         = whisperProgressCb;
    params.progress_callback_user_data = &bridge;

    if (rt.full (_ctx, params, samples, nSamples) != 0)
    {
        reportError (errOut, "Whisper inference failed.");
        return false;
    }

    // Remember the auto-detected language for the pipeline's pinning logic.
    _lastLang.clear ();
    if (lang.empty ())
    {
        const int id = rt.langId (_ctx);
        if (id > 0 && id < rt.langMaxId ())
        {
            const char* s = rt.langStr (id);
            if (s != nullptr) _lastLang = s;
        }
    }

    const int n = rt.nSegments (_ctx);
    for (int i = 0; i < n; ++i)
    {
        WhisperSegment seg;
        seg.startSec = offsetSec + (double) rt.segmentT0 (_ctx, i) / 1000.0;
        seg.endSec   = offsetSec + (double) rt.segmentT1 (_ctx, i) / 1000.0;
        const char* text = rt.segmentText (_ctx, i);
        if (text != nullptr)
            seg.text = trimWhitespace (text);
        if (!seg.text.empty ())
            outSegments.push_back (std::move (seg));
    }

    return true;
}

}} // namespace sc::ai
