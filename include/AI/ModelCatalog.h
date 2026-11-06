// SpectroCraft — unified AI model catalogue (lot 3 of the AI-control plan,
// docs/ai-control/RAPPORT.md §5.2 "ModelCatalog", §6.5).
//
// One read model over the two registries the app has:
//   - sc::ai::ModelManager: OpenVINO noise-suppression IRs and the whisper.cpp
//     ggml checkpoints (URL + SHA-256 + size per file, every platform);
//   - sc::acpp: the audio.cpp Windows CLI binaries and the SAM-Audio / FlashSR /
//     AudioSR weights (Windows only; SHA-256 added in this lot), plus the two
//     htdemucs families the user installs by hand.
// Each entry has a stable id and an `engine` field ("openvino", "whisper",
// "audiocpp"): devices/backends are data, not code, so new engines (e.g. the
// audio.cpp C API replacing OpenVINO) plug in as more entries.
//
// Threading: entries() / find() / status() / the JSON helpers are cheap
// (filesystem stat only, no hashing) and safe from any thread. install(),
// verify() and remove() are BLOCKING (multi-GB downloads / hashing): call them
// from a worker thread (a control job), never from the message thread. Two
// operations on the same id never overlap: the second one fails with "busy"
// (the four audio.cpp CLI variants share one slot, hence one busy key). The
// guard covers the catalogue only: the dialogs' first-use downloads
// (ModelManager::ensureInstalled from Noise Suppression / Transcription, the
// Stem / Super Resolution setup dialogs) do not take it, so a control
// wrapper must also refuse remove() while an AI job uses the model.
//
// Error codes are a subset of the control protocol's closed set
// (include/control/CommandRegistry.h, sc::control::err): invalid_params,
// busy, cancelled, unsupported_platform, model_not_installed, failed - so a
// control wrapper forwards CatalogOutcome as is.
//
// Only compiled in AI builds (SPECTRO_ENABLE_AI=ON), like ModelManager.

#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace sc { namespace ai {

class ModelManager;

namespace engine {
constexpr const char* openvino = "openvino";
constexpr const char* whisper  = "whisper";
constexpr const char* audiocpp = "audiocpp";
}

namespace family {
constexpr const char* noiseSuppress = "noiseSuppress";
constexpr const char* transcribe    = "transcribe";
constexpr const char* stems         = "stems";
constexpr const char* superRes      = "superRes";
constexpr const char* runtime       = "runtime";   // an engine executable, not weights
}

/** One file of an entry, as it ends up on disk. */
struct CatalogFile
{
    std::string name;        // relative to the entry's location ("enc.xml", "flashsr/flashsr.safetensors")
    std::string url;         // download URL ("" = not downloadable)
    int64_t sizeBytes = 0;   // exact size (0 = unknown)
    std::string sha256;      // 64 lowercase hex chars ("" = unknown)
};

struct CatalogEntry
{
    std::string id;          // stable: "deepfilternet2", "whisper-base", "sam-audio-small-q8_0", "audiocpp-cli-cpu"...
    std::string engine;      // engine::openvino | whisper | audiocpp
    std::string family;      // family::noiseSuppress | transcribe | stems | superRes | runtime
    std::string displayName;
    // Folder of the files, relative to the engine's root: the ModelManager
    // root for openvino / whisper (= the id), sc::acpp::dir() for audiocpp
    // ("" = that folder itself; "HTDemucs-6stems-GGUF" for a manual family).
    std::string folder;
    std::vector<CatalogFile> files;   // what verify() checks
    CatalogFile archive;              // runtime entries: the zip downloaded then extracted (url "" otherwise)
    std::string variant;              // runtime entries: sc::acpp variant id ("cpu", "cuda12.4", ...)
    std::vector<std::string> backends;   // devices / backends the engine offers for it in this build
    std::vector<std::string> platforms;  // "windows", "macos", "linux" or "windows-x64"
    std::string license;
    std::string source;               // where the files come from (repository / model card)
    bool installable = false;         // install() may download it here (implies a SHA-256 for every file)
    std::string reason;               // why not installable ("" when installable)
    std::string reasonCode;           // "" | no_checksum | unsupported_platform | blocked | manual
    bool manual = false;              // detected only: the app has no download source for it
    int64_t minFreeDiskBytes = 0;

    /** Sum of files[].sizeBytes (0 when unknown). */
    int64_t sizeBytes() const;
    /** Bytes install() downloads from scratch (the archive for runtimes). */
    int64_t downloadBytes() const;
    /** "sha256" (every file hashed), "size-only" or "none" (manual entries). */
    std::string checksumKind() const;
};

/** Install state, computed from the filesystem only (no hashing). */
struct CatalogStatus
{
    bool installed = false;
    int64_t bytesOnDisk = 0;
    std::vector<std::string> missing;   // files absent or wrongly sized
    juce::File path;                    // folder (OpenVINO / Whisper / htdemucs) or file (audio.cpp)
    bool verified = false;              // every file's SHA-256 matched in this process
};

/** Result of a catalogue operation; code/message/details follow the control
    protocol's error object. result is the §6.5 result object. */
struct CatalogOutcome
{
    bool ok = false;
    std::string code;        // "" on success
    std::string message;
    juce::var details;       // object or void
    juce::var result;        // object or void

    /** {"code","message","details"?} */
    juce::var errorJson() const;
};

/** fraction 0..1, status text; returning false cancels. */
using CatalogProgressFn = std::function<bool(float, const std::string&)>;

class ModelCatalog
{
public:
    /** ModelManager::instance() + sc::acpp::dir(): the app's real folders
        ($SC_MODELS_DIR when set). */
    static ModelCatalog& instance();

    /** The built-in entries over the given roots (tests: private folders). */
    ModelCatalog(ModelManager& mm, const juce::File& audioCppRoot);
    /** Custom entries (tests: local file:// fixtures). */
    ModelCatalog(ModelManager& mm, const juce::File& audioCppRoot, std::vector<CatalogEntry> entries);

    /** Every model the app knows: the 15 ModelManager models, the 4 audio.cpp
        binaries, 9 SAM-Audio, FlashSR, AudioSR, 2 htdemucs (manual). */
    static std::vector<CatalogEntry> builtInEntries();

    const std::vector<CatalogEntry>& entries() const { return _entries; }
    const CatalogEntry* find(const std::string& id) const;

    juce::File modelsRoot() const;      // ModelManager root
    juce::File audioCppRoot() const { return _acppRoot; }

    /** Folder holding the entry's files. */
    juce::File locationOf(const CatalogEntry& e) const;

    CatalogStatus status(const CatalogEntry& e) const;
    bool isBusy(const std::string& id) const;

    /** Cheap pre-check for a control handler before it starts a job: unknown
        id, not installable, wrong platform, busy. ok -> result
        {"modelId","sizeBytes","downloadBytes","installed"}. */
    CatalogOutcome installPlan(const std::string& id) const;

    /** Download + verify every missing file (BLOCKING, worker thread).
        ok -> {"modelId","installed":true,"verified":true,"downloadedBytes"}. */
    CatalogOutcome install(const std::string& id, const CatalogProgressFn& onProgress = nullptr);

    /** Re-hash every file against its SHA-256 (BLOCKING, worker thread).
        ok -> {"modelId","verified","checksum","files":[{"name","bytes","expectedBytes","sha256Ok"}]};
        verified:false is still ok:true (a corrupt file is a result, not an error). */
    CatalogOutcome verify(const std::string& id, const CatalogProgressFn& onProgress = nullptr);

    /** Delete the entry's files, never anything outside its location under
        the models roots. Asking for confirmation is the caller's job (see
        removalPreview). ok -> {"modelId","removed","freedBytes"}. */
    CatalogOutcome remove(const std::string& id);

    /** For confirm_required: {"modelId","bytesOnDisk","path","message":"Removes 148 MB under …"}. */
    CatalogOutcome removalPreview(const std::string& id) const;

    /** Test hook: replaces sc::acpp::downloadFile for audiocpp entries (and
        lifts the Windows-only restriction), so the audio.cpp install path can
        be exercised with local fixtures on every platform. */
    using AudioCppDownloader = std::function<bool(const std::string& url, const juce::File& dest,
                                                  const std::function<bool(float)>& onProgress,
                                                  std::string* errOut, int64_t expectedBytes,
                                                  const std::string& expectedSha256)>;
    void setAudioCppDownloaderForTests(AudioCppDownloader d) { _acppDownloader = std::move (d); }

    // --- JSON (RAPPORT §6.5 shapes) -------------------------------------
    /** One ai.models.list element. */
    juce::var entryJson(const CatalogEntry& e) const;
    /** ai.models.list result: {"modelsRoot","audiocppRoot","models":[...]}. */
    juce::var listJson() const;

private:
    CatalogOutcome installViaModelManager(const CatalogEntry& e, const CatalogProgressFn& onProgress);
    CatalogOutcome installAudioCpp(const CatalogEntry& e, const CatalogProgressFn& onProgress);

    // RAII "busy" marker per id.
    class BusyGuard;
    std::string busyKey(const std::string& id) const;
    bool tryMarkBusy(const std::string& id);
    void clearBusy(const std::string& id);

    ModelManager& _mm;
    juce::File _acppRoot;
    AudioCppDownloader _acppDownloader;
    std::vector<CatalogEntry> _entries;
    mutable std::mutex _busyMutex;
    std::set<std::string> _busy;
};

// ---------------------------------------------------------------------------
// Devices / backends actually available (ai.devices, ai.setDevice).
// ---------------------------------------------------------------------------

struct DeviceResolution
{
    bool ok = false;
    std::string engine;
    std::string requested;
    std::string device;                  // what to run on when ok ("CPU", "GPU", "cuda"...)
    std::string code;                    // invalid_params | unsupported_platform | model_not_installed | failed
    std::string message;
    std::vector<std::string> allowed;    // accepted values, "auto" first ("one of [auto, CPU]")
    std::string path = "device";         // offending parameter, for errorJson()

    /** {"code","message","details":{"path":"device","expected":"one of [auto, CPU]"}} */
    juce::var errorJson() const;
    /** {"engine","device":<requested>,"resolved":<device>} */
    juce::var toJson() const;
};

/** Pure rule (no runtime calls; unit-tested). engine: "openvino", "whisper"
    or "audiocpp". requested: "auto" (or empty) or a device name (case-
    insensitive; OpenVINO "CPU"/"GPU"/"NPU" or "GPU.1", audio.cpp
    "cpu"/"cuda"/"vulkan"). available: what the engine really offers.
    "auto" picks GPU > NPU > CPU for OpenVINO, CPU for Whisper, the installed
    backend for audio.cpp. A device that is not available is an ERROR
    (invalid_params, "device GPU is not available for openvino"), never a
    silent fallback - unlike the legacy sc::resolveAiDevice the dialogs use. */
DeviceResolution resolveAiDevice(const std::string& engine, const std::string& requested,
                                 const std::vector<std::string>& available);

/** Same against availableDevices(engine, true). For "openvino" this
    initialises the OpenVINO runtime: slow the first time, call from a worker. */
DeviceResolution resolveAiDevice(const std::string& engine, const std::string& requested);

/** OpenVINO shorthand: resolveAiDevice("openvino", requested). */
DeviceResolution resolveAiDevice(const std::string& requested);

/** Devices the engine can really run on in this process. openvino: the
    runtime's list (initialising it when initRuntime, else empty unless
    already initialised; only the CPU plugin ships today); whisper: {"CPU"}
    (use_gpu=false); audiocpp: the installed binary's backend, empty off
    Windows. noteOut gets a reason when the list is empty. */
std::vector<std::string> availableDevices(const std::string& engine, bool initRuntime,
                                          std::string* noteOut = nullptr,
                                          const juce::File& audioCppRoot = {});

/** Backends of an installed audio.cpp variant ("cpu" -> {"cpu"}, "cuda12.4"
    -> {"cuda"}, "vulkan" -> {"vulkan"}, "" (hand-placed binary) -> all three). */
std::vector<std::string> audioCppBackendsForVariant(const std::string& variant);

/** ai.devices result (§6.5). initOpenVino=false never loads the runtime: an
    uninitialised OpenVINO reports {"initialised":false,"devices":null}.
    openvinoPreference: the app's aiDevicePref ("auto", "CPU"...). */
juce::var devicesJson(const std::string& openvinoPreference, bool initOpenVino,
                      const juce::File& audioCppRoot = {});

}} // namespace sc::ai
