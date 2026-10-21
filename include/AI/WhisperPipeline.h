// SpectroCraft — Whisper transcription pipeline (Phase 3 of the AI plan).
//
// BLOCKING — call from a worker thread. Mixes the given channels to mono at
// the file's native rate, resamples to 16 kHz with SoXR (the shared Resampler),
// and runs whisper.cpp (OpenVINO encoder when available, pure-ggml fallback
// otherwise). Segments come back in seconds relative to the start of the file.

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
// irXml:     path to ggml-<name>-encoder-openvino.xml ("") for pure ggml
// device:    OpenVINO encoder device ("CPU", "GPU" or "NPU") - resolved by the
//             caller from the user's preference via sc::resolveAiDevice.
bool runWhisperPipeline(AudioEngine& eng, const std::vector<int>& chans,
                        const juce::File& ggmlModel, const juce::File& irXml,
                        const WhisperTranscriber::Options& opts,
                        const std::string& device,
                        std::vector<ScttSegment>& outSegments,
                        std::function<bool(float)> onProgress = nullptr,
                        std::string* errOut = nullptr);

}} // namespace sc::ai
