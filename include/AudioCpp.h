// SpectroCraft - audio.cpp stem-separation support (external CLI tool).
//
// The "Stem Separation" and "Super Resolution" effects drive the pre-built
// audiocpp_cli binary from the 0xShug0/audio.cpp project: audiocpp_cli.exe
// (Windows x64, release .zip) or audiocpp_cli plus its shared libraries
// (macOS arm64 / x64 with Metal, Linux x64; release .tar.gz). The binary and
// the model weights live in <application data>/SpectroCraft/models/audiocpp/
// and are downloaded on first use (GitHub release / Hugging Face). This
// module owns:
//   - folder + file discovery (binary, models),
//   - downloads with progress/cancel,
//   - binary installation from the release archive,
//   - command-line construction and process execution.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

namespace sc { namespace acpp {

/** Folder holding the CLI binary and the model weights:
    <application data>/SpectroCraft/models/audiocpp (created on demand;
    %APPDATA% on Windows, ~/Library on macOS, ~/.config on Linux). When the
    SC_MODELS_DIR environment variable holds an absolute folder (tests, CI),
    <SC_MODELS_DIR>/audiocpp is used instead, on every platform. */
juce::File dir();

/** File name of the CLI executable: audiocpp_cli.exe on Windows,
    audiocpp_cli elsewhere. */
juce::String binaryFileName();

/** Path of the CLI binary inside dir(). */
juce::File binaryPath(const juce::File& d = {});

/** True when a usable CLI binary exists in d (default: dir()). */
bool hasBinary(const juce::File& d = {});

/** Catalogue platform tag of this build: "windows-x64", "macos-arm64",
    "macos-x64", "linux-x64" (or e.g. "linux-arm64", which has no CLI build). */
std::string platformTag();

/** True when the audio.cpp release offers a CLI build for this platform
    (binaryChoices() is not empty). */
bool platformSupported();

/** Every platform tag the release offers a CLI build for (the models run
    wherever the CLI does). */
std::vector<std::string> supportedPlatformTags();

/** All sam-audio-*.gguf model files in d, sorted by name. */
std::vector<juce::File> listModels(const juce::File& d = {});

/** All *.gguf model files in d/HTDemucs-6stems-GGUF (the htdemucs_6stems family), sorted by name. */
std::vector<juce::File> listDemucs6Models(const juce::File& d = {});

/** The htdemucs_ft model directory (d/htdemucs_ft) when it exists and holds at least one .gguf file; an empty File otherwise. */
juce::File demucsFtModelDir(const juce::File& d = {});

// ---------------------------------------------------------------------------
// Super Resolution (FlashSR / AudioSR) support.
// ---------------------------------------------------------------------------

/** The FlashSR model file (d/flashsr/flashsr.safetensors) when it exists;
    an empty File otherwise. */
juce::File flashSrModelFile(const juce::File& d = {});

/** All *.gguf files in d/AudioSR-GGUF, sorted by name. */
std::vector<juce::File> listAudioSRModels(const juce::File& d = {});

/** One downloadable super-resolution model file (path relative to dir()). */
struct SuperResModelSpec
{
    std::string file;        // e.g. "flashsr/flashsr.safetensors"
    int64_t sizeBytes = 0;   // exact download size
    std::string sha256;      // 64 lowercase hex chars of the file ("" = unknown)
};

/** The FlashSR model files offered for download. */
std::vector<SuperResModelSpec> flashSrChoices();

/** Download URL for one FlashSR model file (audio.cpp repo asset). */
juce::String flashSrUrl(const juce::String& fileName);

/** The AudioSR GGUF models offered for download. */
std::vector<SuperResModelSpec> audioSRChoices();

/** Hugging Face resolve URL for one AudioSR model file. */
juce::String audioSRUrl(const juce::String& fileName);

// ---------------------------------------------------------------------------
// Download catalogue (v0.9.2-drama3-base-small release).
// ---------------------------------------------------------------------------

/** One downloadable binary variant of the CLI (for this platform). */
struct BinarySpec
{
    std::string id;          // "cpu", "cuda12.4", "cuda13.3", "vulkan", "metal"
    juce::String label;      // user-facing name
    std::string url;         // GitHub release asset URL
    int64_t sizeBytes = 0;   // exact size of the release archive
    std::string sha256;      // SHA-256 of the release archive ("" = unknown)
    std::string exeSha256;   // SHA-256 of the CLI executable inside the archive
    int64_t exeSizeBytes = 0;
    // Non-empty: the variant must not be installed by the model catalogue
    // (sc::ai::ModelCatalog), with the reason. The setup dialogs still offer it.
    std::string catalogBlocker;
};

/** The CLI variants offered for download on this platform: Windows x64 (cpu,
    cuda12.4, cuda13.3, vulkan), macOS arm64 or x64 (metal), Linux x64 (cpu,
    vulkan); empty elsewhere. */
std::vector<BinarySpec> binaryChoices();

/** File name of the downloaded release archive inside dir():
    audiocpp_bin.zip on Windows, audiocpp_bin.tar.gz elsewhere. */
juce::String archiveFileName();

/** One row of the Backend combo of the Stem Separation / Super Resolution
    windows. */
struct BackendChoice
{
    juce::String label;      // "Cpu", "Cuda", "Vulkan", "Metal"
    juce::String cli;        // --backend value: "cpu", "cuda", "vulkan", "metal"
};

/** Rows of the Backend combo, in id order (id = index + 1, the stored
    acppBackend setting): Cpu, Cuda, Vulkan, plus Metal on macOS. */
std::vector<BackendChoice> backendChoices();

/** --backend value of combo id `id` (1-based); "cpu" when out of range. */
juce::String backendForChoiceId(int id);

/** False when the installed variant (installedVariant()) certainly lacks
    `backend` (a CLI --backend value); true for an unknown variant. */
bool variantSupportsBackend(const juce::String& variant, const juce::String& backend);

/** True for a --backend value the services accept: cpu, cuda, vulkan, metal. */
bool isKnownBackend(const juce::String& backend);

/** One downloadable SAM-Audio GGUF model from Hugging Face. */
struct ModelSpec
{
    std::string file;        // e.g. "sam-audio-small-q8_0.gguf"
    int64_t sizeBytes = 0;   // exact download size
    std::string sha256;      // 64 lowercase hex chars of the file ("" = unknown)
};

/** The SAM-Audio GGUF models offered for download. */
std::vector<ModelSpec> modelChoices();

/** Hugging Face resolve URL for one of the catalogue model files. */
juce::String modelUrl(const juce::String& fileName);

/** SHA-256 published for one of the catalogue download URLs above (binary
    zips, SAM-Audio, FlashSR and AudioSR files); "" when the URL is unknown.
    downloadFile() uses it when the caller passes no hash. */
std::string knownSha256ForUrl(const std::string& url);

/** Exact size published for one of the catalogue download URLs; 0 when unknown. */
int64_t knownSizeForUrl(const std::string& url);

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
    returning false cancels the transfer. Returns true on success: a 2xx
    HTTP status, a complete transfer (no network error, Content-Length
    honoured), when expectedBytes > 0 exactly that many bytes and, when a
    SHA-256 is known (expectedSha256, else knownSha256ForUrl(url)), bytes
    with exactly that digest. Anything else deletes the .part file and
    leaves dest untouched. WinINet on Windows, juce::WebInputStream elsewhere
    (NSURLSession on macOS, libcurl on Linux). */
bool downloadFile(const std::string& url, const juce::File& dest,
                  const std::function<bool(float)>& onProgress,
                  std::string* errOut, int64_t expectedBytes = 0,
                  const std::string& expectedSha256 = {});

/** Parse an HTTP Content-Length header value (64-bit); 0 when absent or
    malformed. */
int64_t parseContentLength(const char* text);

/** Empty when a finished transfer may be installed, else the user-facing
    reason. httpStatus / contentLength / expectedBytes of 0 mean unknown. */
juce::String validateDownload(int httpStatus, int64_t contentLength, int64_t expectedBytes,
                              int64_t received, bool transferError);

/** Empty when actualSha256 matches expectedSha256 (or nothing is expected),
    else the user-facing reason. Comparison is case-insensitive. */
juce::String validateSha256(const std::string& expectedSha256, const std::string& actualSha256);

/** Final step of downloadFile (platform-independent, exposed for tests):
    validates the transfer, the SHA-256 of the received bytes (when
    expectedSha256 is non-empty) and the bytes on disk, then renames part to
    dest. On failure part is deleted, dest is left untouched and errOut is set. */
bool commitDownload(const juce::File& part, const juce::File& dest,
                    int httpStatus, int64_t contentLength, int64_t expectedBytes,
                    int64_t received, bool transferError, std::string* errOut,
                    const std::string& expectedSha256 = {},
                    const std::string& actualSha256 = {});

/** Streaming SHA-256 of a file, 64 lowercase hex chars; "" when the file
    cannot be read or onProgress (fraction 0..1) returned false. */
std::string sha256OfFile(const juce::File& f,
                         const std::function<bool(float)>& onProgress = nullptr);

/** Install the CLI from a downloaded release archive into d and record the
    variant marker. The format is read from the first bytes of the file:
      - .zip (Windows builds): audiocpp_cli.exe only (binaryFileName());
      - .tar.gz (macOS / Linux builds): the executable plus the shared
        libraries next to it (lib*.so*, *.dylib), made executable; the
        library names are kept in runtime-files.txt so that a later install
        or removeInstalledBinary() deletes them.
    The archive itself is deleted afterwards. When the catalogue knows the
    executable's SHA-256 for variantId, the extracted file must match it
    (else nothing is installed and the call fails). A refused archive leaves
    an installed binary, its libraries and its variant marker untouched. */
bool installBinaryFromArchive(const juce::File& archivePath, const juce::String& variantId,
                              const juce::File& d = {}, std::string* errOut = nullptr);

/** Extract the runtime files of a .tar.gz release archive into destDir: the
    executable (binaryFileName()) and the shared libraries (lib*.so*,
    *.dylib), top-level entries only; everything else (tools, model specs,
    the other executables) is skipped. Handles ustar / GNU / pax headers and
    symbolic links between extracted files (copied). Returns the extracted
    file names; empty on failure, with errOut set (not a tar.gz, truncated,
    no executable inside). Exposed for tests. */
std::vector<juce::String> extractRuntimeFromTarGz(const juce::File& archive, const juce::File& destDir,
                                                  std::string* errOut = nullptr);

/** Delete the installed CLI binary, its libraries (runtime-files.txt) and
    the variant marker from d. Returns false when the executable could not be
    deleted (running?). */
bool removeInstalledBinary(const juce::File& d = {});

/** File names of the libraries a .tar.gz install put next to the executable
    in d (runtime-files.txt); empty for a Windows install. */
juce::StringArray installedRuntimeLibraries(const juce::File& d = {});

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

/** Average every channel of a multichannel buffer into one mono track
    (shorter channels are padded with zeros before averaging). A single-
    channel input is returned as-is; an empty input gives an empty result. */
std::vector<float> downmixToMono(const std::vector<std::vector<float>>& in);

/** Full argument vector (exe path excluded) in the documented order:
    --task s2s --family sam_audio --model <m> --backend <b> --audio <a>
    --text <t> [--request-option anchors=...] --out-dir <d> --log */
juce::StringArray buildCliArgs(const juce::File& model, const juce::String& backend,
                               const juce::File& audioIn, const juce::String& text,
                               const std::vector<Anchor>& anchors,
                               const juce::File& outDir);

/** Full argument vector (exe path excluded) in the documented order for the plain separation task used by the htdemucs families:
    --task sep --family <f> --model <m> --backend <b> --audio <a> --out-dir <d> --log */
juce::StringArray buildSepCliArgs(const juce::String& family, const juce::File& model,
                                  const juce::String& backend, const juce::File& audioIn,
                                  const juce::File& outDir);

/** Result of one CLI run. */
struct RunResult
{
    int exitCode = -1;
    juce::String stdoutText;
    juce::String stderrText;
    bool cancelled = false;   // true when the user stopped the run
};

/** Blocking stem-separation run (call from a worker thread). The process is
    polled every 200 ms: shouldContinue() returning false terminates it (on
    macOS / Linux its whole process group), and onLogLine() receives each
    completed stdout line as it arrives (--log progress). stdin is the null
    device. On success outDir contains target.wav + residual.wav. */
bool runStemSeparation(const juce::File& binary, const juce::File& model,
                       const juce::String& backend, const juce::File& audioIn,
                       const juce::String& text, const std::vector<Anchor>& anchors,
                       const juce::File& outDir,
                       const std::function<bool()>& shouldContinue,
                       const std::function<void(const juce::String&)>& onLogLine,
                       RunResult* resOut, std::string* errOut);

/** Blocking run of the plain separation task (call from a worker thread). Same polling/cancellation contract as runStemSeparation; on success outDir holds one named .wav file per stem. */
bool runSepTask(const juce::File& binary, const juce::String& family, const juce::File& model,
                const juce::String& backend, const juce::File& audioIn, const juce::File& outDir,
                const std::function<bool()>& shouldContinue,
                const std::function<void(const juce::String&)>& onLogLine,
                RunResult* resOut, std::string* errOut);

/** Full argument vector (exe path excluded) for one super-resolution run:
    FlashSR (mode 1): --task s2s --family builtin_audio_utils --model <m>
        --load-option utility=flashsr --backend <b> --audio <a> --out <o> --log
    AudioSR (mode 2): --task s2s --family audiosr --model <m>
        --backend <b> --audio <a> --out <o> --log */
juce::StringArray buildSuperResCliArgs(int mode, const juce::File& model,
                                       const juce::String& backend,
                                       const juce::File& audioIn, const juce::File& outWav);

/** Blocking super-resolution run (call from a worker thread). Same polling/
    cancellation contract as runStemSeparation; on success outWav holds the
    enhanced result (mono 48 kHz for both families - they downmix and
    resample internally, so the input may be any rate/channel count). */
bool runSuperResTask(const juce::File& binary, int mode, const juce::File& model,
                     const juce::String& backend, const juce::File& audioIn,
                     const juce::File& outWav,
                     const std::function<bool()>& shouldContinue,
                     const std::function<void(const juce::String&)>& onLogLine,
                     RunResult* resOut, std::string* errOut);

}} // namespace sc::acpp
