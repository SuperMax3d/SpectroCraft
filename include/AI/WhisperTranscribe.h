// SpectroCraft — Whisper transcription engine (Phase 3 of the AI plan).
//
// Thin wrapper around the prebuilt whisper.cpp b5454 binaries (MIT), loaded
// at runtime by sc::ai::WhisperRuntime and run on CPU via ggml's per-
// microarchitecture SIMD backends. No OpenVINO is involved in transcription;
// the bundled OpenVINO runtime serves noise suppression only.
//
// All methods are BLOCKING — call them from a worker thread. The input must
// be mono float32 at 16 kHz (WHISPER_SAMPLE_RATE); callers resample first
// with the shared SoXR Resampler.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace juce { class File; }

struct whisper_context;   // opaque type from the vendored b5454 whisper.h

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

    // ggmlModel: path to ggml-<name>.bin. Also loads whisper.dll on first use
    // (see sc::ai::WhisperRuntime); a missing runtime is reported in *errOut.
    bool load (const juce::File& ggmlModel, std::string* errOut);

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
