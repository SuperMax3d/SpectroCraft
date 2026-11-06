// SpectroCraft - headless stem separation (audio.cpp CLI orchestration).
//
// Everything the Stem Separation window used to do inside its worker lambda,
// minus the UI: write the input WAV, run the CLI once (htdemucs families) or
// once per description (SAM-Audio), read the stems back and bring every track
// to the source rate. No component, dialog or MessageManager call is made, so
// run() can be called from any worker thread (the window, a control-API job,
// a test). Committing the result to a document stays with the caller.
//
// The CLI is Windows-only today (sc::acpp::run* are stubs elsewhere): on
// macOS / Linux run() fails with errorCode "unsupported_platform" after
// validating the request; validate() itself is platform-independent.
#pragma once

#include <JuceHeader.h>

#include <functional>
#include <vector>

#include "AudioCpp.h"
#include "MergeFiles.h"   // sc::MergeResult

namespace sc {

struct StemService
{
    enum class Mode
    {
        SamAudio   = 1,   // text-prompted target / residual (one CLI run per description)
        HtDemucs6  = 2,   // htdemucs_6stems: plain separation, one .wav per named stem
        HtDemucsFt = 3    // htdemucs (fine-tuned): same, model is a folder of .gguf files
    };

    /** One SAM-Audio description. Anchor times are DOCUMENT seconds; run()
        converts them to input-relative times (see anchorsRelativeToInput). */
    struct Description
    {
        juce::String text;
        std::vector<sc::acpp::Anchor> anchors;
    };

    struct Request
    {
        Mode mode = Mode::SamAudio;
        std::vector<Description> descriptions;   // SamAudio only (blank texts are invalid)
        juce::File binary;                       // empty = sc::acpp::binaryPath()
        juce::File model;                        // .gguf file (modes 1-2) or folder (mode 3)
        juce::String backend = "cpu";            // "cpu" | "cuda" | "vulkan"
        bool extractFromOriginal = true;         // SamAudio: false = chain on the previous residual
        double extStartSecs = 0.0;               // document time of input frame 0
        double sourceRate = 0.0;                 // rate of `input` and of the merged result
        std::vector<std::vector<float>> input;   // one vector per channel, equal lengths
        // Working folder. Empty: a fresh temp folder deleted when run() returns
        // (Stem::file is then empty). Non-empty: created if needed and KEPT, so
        // Stem::file points at the tool's output files.
        juce::File workDir;
    };

    /** One file the tool produced (a SAM target / residual or a named stem). */
    struct Stem
    {
        juce::String name;      // "target 1", "residual", or the stem file's base name ("drums")
        juce::File file;        // empty when the working folder was temporary
        double rate = 0.0;      // the file's own rate (before resampling to sourceRate)
        int channels = 0;       // number of tracks it contributed to `merged`, in order
        int64_t frames = 0;
    };

    struct Result
    {
        bool ok = false;
        bool cancelled = false;          // the cancel callback stopped the run
        juce::String errorCode;          // "" on success; see validate() + "cancelled", "io_error",
                                         // "tool_failed", "no_output", "unsupported_platform"
        juce::String error;              // user-facing message (the window shows it verbatim)
        juce::String detail;             // tail of the CLI log when the tool failed
        std::vector<Stem> stems;         // in track order
        sc::MergeResult merged;          // every track at sourceRate, padded to equal length
    };

    /** fraction < 0 leaves the progress unchanged; an empty status leaves the text unchanged. */
    using Progress = std::function<void (double fraction, const juce::String& status)>;
    /** Returns true to stop the run (the CLI process is killed). */
    using Cancel = std::function<bool ()>;

    /** True where the audio.cpp CLI can actually run (Windows). */
    static bool supported();

    /** Empty when the request can run; otherwise a user-facing reason, and
        *codeOut receives a stable code: invalid_mode, no_description,
        invalid_backend, binary_missing, model_missing, empty_input,
        ragged_input, invalid_rate, invalid_range. Checks files on disk. */
    static juce::String validate (const Request& req, juce::String* codeOut = nullptr);

    /** BLOCKING - call from a worker thread. Never touches the UI. */
    static Result run (const Request& req, const Progress& progress = nullptr,
                       const Cancel& cancel = nullptr);

    /** Converts document-time anchors to input-relative ones. Ranges outside
        the extracted input are clamped; empty results are dropped. */
    static std::vector<sc::acpp::Anchor> anchorsRelativeToInput (
        const std::vector<sc::acpp::Anchor>& absAnchors, double extStartSecs, double inputDurSecs);
};

} // namespace sc
