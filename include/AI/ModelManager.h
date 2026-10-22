// SpectroCraft — generic AI model manager (Phase 0 of the AI plan).
//
// A small registry of known models, each described by a set of files with
// download URLs + SHA-256 checksums. Models are stored under
// %APPDATA%/SpectroCraft/models/<model-id>/ and are downloaded on first use
// (auto-download with progress), verified against their checksums, and never
// committed to the git repository. If a download fails, callers show a popup
// explaining how to install the files manually into that folder.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace juce { class File; }

namespace sc { namespace ai {

struct ModelFileSpec {
    std::string url;         // absolute download URL
    std::string sha256;      // 64 lowercase hex chars, verified after download ("" = skip hash check)
    int64_t sizeBytes = 0;   // expected size in bytes (0 = unknown)
    std::string name;        // destination filename (derived from the URL basename when empty)
};

struct ModelSpec {
    std::string id;          // directory name under the models root
    std::string displayName; // user-facing name for dialogs/menus
    std::vector<ModelFileSpec> files;
    int64_t minFreeDiskBytes = 32 * 1024 * 1024;
};

class ModelManager {
public:
    static ModelManager& instance();

    explicit ModelManager(std::string rootOverride = "");

    const std::vector<ModelSpec>& registry() const;
    const ModelSpec* find(const std::string& id) const;

    // %APPDATA%/SpectroCraft/models (or the override given to the ctor).
    juce::File modelRoot() const;
    juce::File dirFor(const std::string& modelId) const;

    // True when every file of `spec` exists in dirFor(spec.id) with the
    // expected size. (Checksums are verified at download time and by
    // verifyInstalled().)
    // If missingOut is non-null it is cleared, then filled with exactly the
    // basenames of the files that are absent or wrongly sized.
    bool isInstalled(const ModelSpec& spec, std::vector<std::string>* missingOut = nullptr) const;

    // Re-check existing files against their SHA-256 checksums.
    bool verifyInstalled(const ModelSpec& spec, std::string* badFileOut = nullptr) const;

    // Download any missing (or size-mismatched) files. BLOCKING — call from a
    // worker thread. onProgress(fraction 0..1, statusText); returning false
    // cancels the download. Returns true when all files are present + verified.
    using ProgressFn = std::function<bool(float, const std::string&)>;
    bool ensureInstalled(const ModelSpec& spec, ProgressFn onProgress,
                         std::string* errOut = nullptr);

private:
    static bool downloadFile(const std::string& url, const juce::File& dest,
                             const std::string& expectedSha256, int64_t expectedSize,
                             ProgressFn onProgress, std::string* errOut);
    static int64_t freeDiskSpaceBytes(const juce::File& dir);

    std::string _rootOverride;
};

}} // namespace sc::ai
