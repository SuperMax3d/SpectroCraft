// SpectroCraft — Whisper transcription engine (Phase 3 of the AI plan).
//
// Thin wrapper around whisper.cpp v1.6.2 (MIT) with its OpenVINO encoder
// backend: the encoder runs on the bundled OpenVINO runtime, the decoder
// stays in whisper.cpp's ggml CPU runtime — Intel's split for their Audacity
// plugin. If the OpenVINO encoder fails to initialise, whisper.cpp silently
// falls back to a pure-ggml encode (slower, same results).
//
// All methods are BLOCKING — call them from a worker thread. The input must
// be mono float32 at 16 kHz (WHISPER_SAMPLE_RATE); callers resample first
// with the shared SoXR Resampler.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace juce { class File; }

struct whisper_context;   // forward decl from whisper.h (kept out of this header)

namespace sc { namespace ai {

struct WhisperSegment {
    double startSec = 0.0;   // seconds into the transcribed audio
    double endSec = 0.0;
    std::string text;        // UTF-8, trimmed by whisper.cpp
};

class WhisperTranscriber {
public:
    struct Options {
        std::string language = "auto";   // BCP-47 code ("en", ...) or "auto"
        bool translate = false;          // true = translate to English
        int beamSize = 5;                // >1 selects beam search, else greedy
        int threads = 0;                 // 0 = hardware concurrency
    };

    using ProgressFn = std::function<bool(float, const std::string&)>;   // false = (ignored) cancel hint

    WhisperTranscriber() = default;
    ~WhisperTranscriber();
    WhisperTranscriber (const WhisperTranscriber&) = delete;
    WhisperTranscriber& operator= (const WhisperTranscriber&) = delete;

    // ggmlModel:  path to ggml-<name>.bin (full model)
    // irXml:      path to ggml-<name>-encoder-openvino.xml ("") for pure ggml
    // cacheDir:   OpenVINO compiled-model cache directory (speeds up re-init)
    bool load (const juce::File& ggmlModel, const juce::File& irXml,
               const juce::File& cacheDir, std::string* errOut);

    // samples must be mono float32 @ 16 kHz. Returns false on failure.
    bool transcribe (const float* samples, int nSamples, const Options& opts,
                     std::vector<WhisperSegment>& outSegments,
                     ProgressFn onProgress, std::string* errOut);

    // Transcribes one contiguous region of 16 kHz mono audio (e.g. a speech
    // segment found by the pipeline's VAD). Segments are APPENDED to
    // outSegments with their times offset by offsetSec seconds.
    // language: "" or "auto" = auto-detect, otherwise a BCP-47 code.
    bool transcribeRegion (const float* samples, int nSamples, const Options& opts,
                           const std::string& language, double offsetSec,
                           std::vector<WhisperSegment>& outSegments,
                           ProgressFn onProgress, std::string* errOut);

    // BCP-47 code auto-detected by the most recent transcribeRegion() call
    // ("" when detection was not requested or failed).
    const std::string& lastDetectedLanguage () const { return _lastLang; }

    void unload();
    bool isLoaded() const { return _ctx != nullptr; }

private:
    struct whisper_context* _ctx = nullptr;
    std::string _lastLang;
};

}} // namespace sc::ai
