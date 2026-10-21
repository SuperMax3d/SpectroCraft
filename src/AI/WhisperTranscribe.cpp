// SpectroCraft — Whisper transcription engine (Phase 3 of the AI plan).
// See include/AI/WhisperTranscribe.h for the design notes.

#include "AI/WhisperTranscribe.h"

#include <juce_core/juce_core.h>

#include <whisper.cpp/whisper.h>

#include "AI/OvRuntime.h"

#include <thread>

#ifdef _WIN32
// windows.h must come after the JUCE headers (JUCE configures WINVER first).
#include <windows.h>
#endif

namespace sc { namespace ai {

namespace {

struct ProgressBridge {
    WhisperTranscriber::ProgressFn fn;
};

void whisperProgressCb(struct whisper_context*, struct whisper_state*, int progress, void* user)
{
    auto* b = static_cast<ProgressBridge*>(user);
    if (b != nullptr && b->fn)
    {
        // v1.6.x can report > 100% on short files (timestamp-token seek overshoot); clamp.
        float p = (float) progress / 100.0f;
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        b->fn (p, "transcribing…");
    }
}

} // namespace

WhisperTranscriber::~WhisperTranscriber() { unload(); }

void WhisperTranscriber::unload()
{
    if (_ctx != nullptr)
    {
        whisper_free (_ctx);
        _ctx = nullptr;
    }
    _lastLang.clear ();
}

bool WhisperTranscriber::load(const juce::File& ggmlModel, const juce::File& irXml,
                              const juce::File& cacheDir, std::string* errOut)
{
    unload();

    if (!ggmlModel.existsAsFile())
    {
        if (errOut) *errOut = std::string ("Whisper model file not found: ")
                                 + ggmlModel.getFullPathName ().toStdString ();
        return false;
    }

    whisper_context_params cparams = {};
    cparams.use_gpu = false;   // CPU-only in v1 (OpenVINO encoder covers the heavy part)
    cparams.flash_attn = false; // measured ~1.7x SLOWER on CPU (whisper.cpp flash-attn kernels favour GPU); keep off

    _ctx = whisper_init_from_file_with_params (ggmlModel.getFullPathName ().toRawUTF8 (), cparams);
    if (_ctx == nullptr)
    {
        if (errOut) *errOut = std::string ("Could not load the Whisper model: ")
                                 + ggmlModel.getFileName ().toStdString ();
        return false;
    }

    // Enable the OpenVINO encoder when an IR model is available. On failure we
    // keep going — whisper.cpp falls back to its pure-ggml encode path.
    //
    // The probe below matters: openvino.dll is delay-loaded, so calling into
    // it without the DLL present raises SEH 0xC06D007E (a hard crash in a
    // module not compiled with /EHa). LoadLibraryA fails cleanly instead.
#ifdef _WIN32
    const bool ovAvailable = ::LoadLibraryA ("openvino.dll") != nullptr;
#else
    const bool ovAvailable = true;
#endif
    if (irXml.existsAsFile () && ovAvailable)
    {
        // Prefer GPU/NPU when the runtime exposes them — the encoder is the
        // dominant cost of transcription and both beat CPU on supported hardware.
        std::string device = "CPU";
        const auto devs = OvRuntime::instance ().devices ();
        for (const auto& d : devs) if (d == "GPU") { device = d; break; }
        if (device == "CPU")
            for (const auto& d : devs) if (d == "NPU") { device = d; break; }

        const char* cache = cacheDir.exists () ? cacheDir.getFullPathName ().toRawUTF8 () : nullptr;
        int r = whisper_ctx_init_openvino_encoder (_ctx, irXml.getFullPathName ().toRawUTF8 (),
                                                   device.c_str (), cache);
        if (r != 0 && device != "CPU")
            r = whisper_ctx_init_openvino_encoder (_ctx, irXml.getFullPathName ().toRawUTF8 (),
                                                   "CPU", cache);   // retry on CPU
        if (r != 0 && errOut)
            *errOut = "OpenVINO encoder failed to initialise; using the slower pure-ggml encode.";
    }

    return true;
}

bool WhisperTranscriber::transcribe(const float* samples, int nSamples, const Options& opts,
                                    std::vector<WhisperSegment>& outSegments,
                                    ProgressFn onProgress, std::string* errOut)
{
    // Whole-buffer convenience wrapper around transcribeRegion().
    std::vector<WhisperSegment> tmp;
    if (!transcribeRegion (samples, nSamples, opts, opts.language, 0.0,
                           tmp, onProgress, errOut))
        return false;
    outSegments = std::move (tmp);
    return true;
}

bool WhisperTranscriber::transcribeRegion(const float* samples, int nSamples,
                                          const Options& opts,
                                          const std::string& languageIn,
                                          double offsetSec,
                                          std::vector<WhisperSegment>& outSegments,
                                          ProgressFn onProgress, std::string* errOut)
{
    if (_ctx == nullptr)
    {
        if (errOut) *errOut = "Whisper model is not loaded.";
        return false;
    }
    if (samples == nullptr || nSamples <= 0)
    {
        if (errOut) *errOut = "No audio to transcribe.";
        return false;
    }

    const bool useBeam = opts.beamSize > 1;
    whisper_full_params params = whisper_full_default_params (
        useBeam ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);

    if (useBeam)
        params.beam_search.beam_size = opts.beamSize;

    const std::string lang = languageIn.empty () || languageIn == "auto"
                              ? std::string ("") : languageIn;
    params.language = lang.empty () ? nullptr : lang.c_str ();
    params.detect_language = lang.empty ();
    params.translate = opts.translate;

    params.n_threads = opts.threads > 0 ? opts.threads
                                        : (int) std::thread::hardware_concurrency ();
    if (params.n_threads < 1) params.n_threads = 1;

    params.print_progress = false;   // we report progress ourselves
    ProgressBridge bridge { onProgress };
    params.progress_callback = whisperProgressCb;
    params.progress_callback_user_data = &bridge;

    const int r = whisper_full (_ctx, params, samples, nSamples);
    // NOTE: `params` is a stack object from whisper_full_default_params() (by value).
    // Do NOT call whisper_free_params() on it — that function does `delete params`
    // and is only for pointers returned by whisper_full_default_params_by_ref().
    if (r != 0)
    {
        if (errOut) *errOut = "Whisper transcription failed (code " + std::to_string (r) + ").";
        return false;
    }

    // Remember the auto-detected language for the caller (English is id 0).
    _lastLang.clear ();
    if (lang.empty ())
    {
        const int lid = whisper_full_lang_id (_ctx);
        if (lid >= 0 && lid < whisper_lang_max_id ())
            if (const char* s = whisper_lang_str (lid)) _lastLang = s;
    }

    const int nSegs = whisper_full_n_segments (_ctx);
    for (int i = 0; i < nSegs; ++i)
    {
        WhisperSegment seg;
        // Segment times are in units of 10 ms, offset into the full stream.
        seg.startSec = whisper_full_get_segment_t0 (_ctx, i) / 100.0 + offsetSec;
        seg.endSec   = whisper_full_get_segment_t1 (_ctx, i) / 100.0 + offsetSec;
        const char* text = whisper_full_get_segment_text (_ctx, i);
        if (text != nullptr) seg.text = text;
        outSegments.push_back (std::move (seg));
    }

    // No free needed: stack-allocated params, no heap members.
    return true;
}

}} // namespace sc::ai
