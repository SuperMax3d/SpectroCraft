// SpectroCraft - audio.cpp stem-separation support (external CLI tool).
//
// The "Stem Separation" effect drives the pre-built audiocpp_cli.exe binary
// from the 0xShug0/audio.cpp project. Both the binary and the SAM-Audio GGUF
// model weights live in %APPDATA%/SpectroCraft/models/audiocpp/ and are
// downloaded on first use (GitHub release / Hugging Face). This module owns:
//   - folder + file discovery (binary, models),
//   - downloads with progress/cancel,
//   - binary installation from the release zip,
//   - command-line construction and process execution.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

namespace sc { namespace acpp {

/** Folder holding audiocpp_cli.exe and the sam-audio-*.gguf model weights:
    %APPDATA%/SpectroCraft/models/audiocpp (created on demand). */
juce::File dir();

/** Path of the CLI binary inside dir(). */
juce::File binaryPath(const juce::File& d = {});

/** True when a usable audiocpp_cli.exe exists in d (default: dir()). */
bool hasBinary(const juce::File& d = {});

/** All sam-audio-*.gguf model files in d, sorted by name. */
std::vector<juce::File> listModels(const juce::File& d = {});

// ---------------------------------------------------------------------------
// Download catalogue (v0.9.2-drama3-base-small release).
// ---------------------------------------------------------------------------

/** One downloadable Windows binary variant of the CLI. */
struct BinarySpec
{
    std::string id;          // "cpu", "cuda12.4", "cuda13.3", "vulkan"
    juce::String label;      // user-facing name
    std::string url;         // GitHub release asset URL
    int64_t sizeBytes = 0;   // advertised download size (for display)
};

/** The Windows x64 CLI variants offered for download. */
std::vector<BinarySpec> binaryChoices();

/** One downloadable SAM-Audio GGUF model from Hugging Face. */
struct ModelSpec
{
    std::string file;        // e.g. "sam-audio-small-q8_0.gguf"
    int64_t sizeBytes = 0;   // advertised download size (for display)
};

/** The SAM-Audio GGUF models offered for download. */
std::vector<ModelSpec> modelChoices();

/** Hugging Face resolve URL for one of the catalogue model files. */
juce::String modelUrl(const juce::String& fileName);

// ---------------------------------------------------------------------------
// Installed-binary variant marker (written when we install a binary).
// ---------------------------------------------------------------------------

/** Variant id ("cpu", "cuda12.4", ...) recorded at install time; "" if the
    binary was placed there manually or no marker exists. */
juce::String installedVariant(const juce::File& d = {});

void setInstalledVariant(const juce::String& variantId, const juce::File& d = {});

// ---------------------------------------------------------------------------
// Downloads (blocking - call from a worker thread).
// ---------------------------------------------------------------------------

/** Download url to dest via dest.part + rename. onProgress(fraction 0..1)
    returning false cancels the transfer. Returns true on success. */
bool downloadFile(const std::string& url, const juce::File& dest,
                  const std::function<bool(float)>& onProgress,
                  std::string* errOut);

/** Extract audiocpp_cli.exe from a downloaded release zip into d and record
    the variant marker. The zip itself is deleted afterwards. */
bool installBinaryFromZip(const juce::File& zipPath, const juce::String& variantId,
                          const juce::File& d = {}, std::string* errOut = nullptr);

// ---------------------------------------------------------------------------
// CLI invocation.
// ---------------------------------------------------------------------------

/** One include/exclude time range for a description. Times are in seconds
    relative to the start of the input WAV handed to the tool (NOT relative
    to the whole source document). */
struct Anchor
{
    bool include = true;     // true = "+", false = "-"
    double startSecs = 0.0;
    double stopSecs = 0.0;
};

/** Format one time value for the anchors option: up to 4 decimals with
    trailing zeros trimmed (0.1 -> "0.1", 6.9 -> "6.9"). */
juce::String formatAnchorTime(double secs);

/** Build the --request-option value for a non-empty anchor list, e.g.
    anchors=[[\"-\",0.1,0.5],[\"+\",1.5,4.9]]  - empty string when no anchors. */
juce::String buildAnchorsOption(const std::vector<Anchor>& anchors);

/** Full argument vector (exe path excluded) in the documented order:
    --task s2s --family sam_audio --model <m> --backend <b> --audio <a>
    --text <t> [--request-option anchors=...] --out-dir <d> --log */
juce::StringArray buildCliArgs(const juce::File& model, const juce::String& backend,
                               const juce::File& audioIn, const juce::String& text,
                               const std::vector<Anchor>& anchors,
                               const juce::File& outDir);

/** Result of one CLI run. */
struct RunResult
{
    int exitCode = -1;
    juce::String stdoutText;
    juce::String stderrText;
};

/** Blocking stem-separation run (call from a worker thread). The process is
    polled every 200 ms: shouldContinue() returning false terminates it, and
    onLogLine() receives each completed stdout line as it arrives (--log
    progress). On success outDir contains target.wav + residual.wav. */
bool runStemSeparation(const juce::File& binary, const juce::File& model,
                       const juce::String& backend, const juce::File& audioIn,
                       const juce::String& text, const std::vector<Anchor>& anchors,
                       const juce::File& outDir,
                       const std::function<bool()>& shouldContinue,
                       const std::function<void(const juce::String&)>& onLogLine,
                       RunResult* resOut, std::string* errOut);

}} // namespace sc::acpp
