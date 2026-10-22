// SpectroCraft — Whisper transcription pipeline (Phase 3 of the AI plan).
//
// BLOCKING — call from a worker thread. Mixes the given channels to mono at
// the file's native rate, resamples to 16 kHz with SoXR (the shared Resampler),
// and runs whisper.cpp on CPU via the prebuilt b5454 binaries (see
// extern/whisper-b5454/README.md). Segments come back in seconds relative to
// the start of the file.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ScttChunk.h"   // ScttSegment
#include "AI/WhisperTranscribe.h"

namespace juce { class File; }
class AudioEngine;

namespace sc { namespace ai {

// ggmlModel: path to ggml-<name>.bin (full model)
bool runWhisperPipeline(AudioEngine& eng, const std::vector<int>& chans,
                        const juce::File& ggmlModel,
                        const WhisperTranscriber::Options& opts,
                        std::vector<ScttSegment>& outSegments,
                        std::function<bool(float)> onProgress = nullptr,
                        std::string* errOut = nullptr);

}} // namespace sc::ai
