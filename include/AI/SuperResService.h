// SpectroCraft - headless super resolution (audio.cpp CLI: FlashSR / AudioSR).
//
// Everything the Super Resolution window used to do inside its worker lambda,
// minus the UI: write the input WAV, run the CLI, read the enhanced result
// back and (optionally) bring it to the source rate. No component, dialog or
// MessageManager call is made, so run() can be called from any worker thread.
// Committing the result to a document stays with the caller.
//
// The CLI is Windows-only today: on macOS / Linux run() fails with errorCode
// "unsupported_platform" after validating the request.
#pragma once

#include <JuceHeader.h>

#include <functional>
#include <vector>

#include "AudioCpp.h"
#include "MergeFiles.h"   // sc::MergeResult

namespace sc {

struct SuperResService
{
    enum class Mode
    {
        FlashSR = 1,   // builtin_audio_utils utility=flashsr (.safetensors)
        AudioSR = 2    // audiosr family (.gguf)
    };

    struct Request
    {
        Mode mode = Mode::FlashSR;
        juce::File binary;                       // empty = sc::acpp::binaryPath()
        juce::File model;
        juce::String backend = "cpu";            // "cpu" | "cuda" | "vulkan"
        double sourceRate = 0.0;                 // rate of `input`
        std::vector<std::vector<float>> input;   // one vector per channel, equal lengths
        // The tool always returns mono at its native rate (48 kHz). true:
        // resample it to sourceRate (the window does this for "append").
        bool resampleToSourceRate = false;
        // Working folder. Empty: a fresh temp folder deleted when run() returns
        // (Result::outputFile is then empty). Non-empty: created and KEPT.
        juce::File workDir;
    };

    struct Result
    {
        bool ok = false;
        bool cancelled = false;
        juce::String errorCode;   // "" on success; validate() codes + "cancelled", "io_error",
                                  // "tool_failed", "no_output", "unsupported_platform"
        juce::String error;       // user-facing message (the window shows it verbatim)
        juce::String detail;      // tail of the CLI log when the tool failed
        juce::File outputFile;    // the tool's enhanced.wav (empty when the folder was temporary)
        double toolRate = 0.0;    // rate the tool produced (before any resampling)
        sc::MergeResult merged;   // the enhanced track(s); rate = toolRate or sourceRate
    };

    using Progress = std::function<void (double fraction, const juce::String& status)>;
    using Cancel   = std::function<bool ()>;

    static bool supported();

    /** Empty when the request can run; otherwise a user-facing reason and a
        stable *codeOut: invalid_mode, invalid_backend, binary_missing,
        model_missing, empty_input, ragged_input, invalid_rate. */
    static juce::String validate (const Request& req, juce::String* codeOut = nullptr);

    /** BLOCKING - call from a worker thread. Never touches the UI. */
    static Result run (const Request& req, const Progress& progress = nullptr,
                       const Cancel& cancel = nullptr);
};

} // namespace sc
