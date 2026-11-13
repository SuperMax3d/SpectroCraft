// SpectroCraft — "Check for Updates" (Help menu): GitHub release lookup,
// signed update manifest, download / verify / stage, and the hand-off to the
// separate spectrocraft-updater console program.
//
// Flow (docs/updates.md):
//   1. GET https://api.github.com/repos/SuperMax3d/SpectroCraft/releases/latest
//      and compare tag_name with SC_VERSION_STRING (numeric parts only).
//   2. Newer: show the version + release notes with [Update Now] / [Later].
//   3. Update Now: download the platform zip + update-manifest.json + its
//      .sig into a temp folder (cancellable, nothing committed on cancel),
//      verify the manifest signature (Ed25519, public key embedded in the
//      app), then unzip to a staging folder while checking the size and
//      SHA-256 of every file against the manifest.
//   4. Ask to save unsaved files, launch <staging>/SpectroCraft/
//      spectrocraft-updater.exe --pid <this pid> --install-dir … --staging-dir
//      … --relaunch <this exe> and exit. The updater swaps the files after
//      this process has gone and restarts the app with --finish-update.
//
// Windows only for now: the menu item and the whole flow are guarded by
// JUCE_WINDOWS (the code itself compiles everywhere).
#pragma once

#include <JuceHeader.h>

#include <functional>

namespace sc { namespace update {

// ---------------------------------------------------------------- constants
constexpr const char* kRepo        = "SuperMax3d/SpectroCraft";
constexpr const char* kLatestApi   = "https://api.github.com/repos/SuperMax3d/SpectroCraft/releases/latest";
constexpr const char* kReleasesUrl = "https://github.com/SuperMax3d/SpectroCraft/releases";

// The manifest asset names (release assets, next to the zip).
constexpr const char* kManifestName = "update-manifest.json";
constexpr const char* kManifestSigName = "update-manifest.json.sig";

// Ed25519 public key (hex) whose private key signs update-manifest.json.
// tools/updates/README.md explains where the private key lives and how a
// release is signed; tests/regression/updates.inc checks the pairing.
juce::String manifestPublicKeyHex();

// ---------------------------------------------------------------- version
// "1.0.2279" / "v1.0.2279" -> {1,0,2279}. Non-numeric text stops the parse.
juce::Array<int> versionParts (const juce::String& version);

// -1 / 0 / 1 comparing the numeric parts (shorter side padded with zeros).
int compareVersions (const juce::String& a, const juce::String& b);

// ---------------------------------------------------------------- release
struct AssetInfo
{
    juce::String name;
    juce::String url;      // browser_download_url
    int64_t size = 0;
};

struct ReleaseInfo
{
    juce::String tag;          // tag_name as published ("v1.0.2300")
    juce::String version;      // tag without a leading "v"
    juce::String title;        // release name ("" = the tag)
    juce::String notes;        // release body
    juce::String pageUrl;      // html_url
    juce::Array<AssetInfo> assets;
};

/** Fetch the latest release. HTTPS only, User-Agent header, redirects
    followed. errOut explains what went wrong (network, HTTP status, JSON). */
bool fetchLatestRelease (ReleaseInfo& out, juce::String& errOut);

/** "windows" / "macos" / "linux" and "x64" / "arm64": the tokens an asset
    name must contain. */
juce::String platformToken();
juce::String archToken();

/** The asset to download: SpectroCraft-<version>-<os>-<arch>.zip first, then
    SpectroCraft-<os>-<arch>.zip (the names used by releases up to 1.0.x),
    then any .zip whose name contains both tokens. nullptr when none matches. */
const AssetInfo* pickAsset (const ReleaseInfo& release);

/** An asset by exact name (case-insensitive), e.g. update-manifest.json or
    its .sig file. nullptr when the release does not publish it. */
const AssetInfo* findAsset (const ReleaseInfo& release, const juce::String& name);

// ---------------------------------------------------------------- manifest
struct ManifestFile
{
    juce::String path;      // zip-relative, forward slashes
    int64_t size = 0;
    juce::String sha256;    // 64 lowercase hex chars
};

struct Manifest
{
    juce::String format;    // "spectrocraft-update-manifest/1"
    juce::String version;
    juce::Array<ManifestFile> files;
};

/** Parse update-manifest.json. Rejects an unknown format, a version that is
    not a plain version, duplicate paths, absolute/.." paths and missing
    hashes. */
bool parseManifest (const juce::String& jsonText, Manifest& out, juce::String& errOut);

/** Verify the Ed25519 signature of the manifest bytes against the embedded
    public key. sigText is the .sig file content (hex, whitespace tolerated). */
bool verifyManifestSignature (const juce::String& manifestText,
                              const juce::String& sigText, juce::String& errOut);

// ---------------------------------------------------------------- download
/** Progress callback: fraction 0..1 plus a short stage label; returning false
    cancels the transfer. */
using ProgressFn = std::function<bool (double fraction, const juce::String& stage)>;

/** GET a URL into a file (atomically: <dest>.part then renamed). */
bool httpGetToFile (const juce::String& url, const juce::File& dest,
                    ProgressFn onProgress, juce::String& errOut);

/** GET a URL into a string (the release JSON, the manifest, a .sig). */
bool httpGetString (const juce::String& url, juce::String& bodyOut,
                    juce::String& errOut);

// ---------------------------------------------------------------- staging
/** Unzip `zip` into `stagingDir` while hashing every entry, and check each
    file against the manifest: every manifest entry must appear with the
    right size and SHA-256, and the archive must not contain anything the
    manifest does not list. Cancelling (progress returns false) leaves the
    staging directory behind for the caller to delete — nothing is committed
    to the install folder either way. */
bool unzipAndVerify (const juce::File& zip, const juce::File& stagingDir,
                     const Manifest& manifest, ProgressFn onProgress,
                     juce::String& errOut);

// ---------------------------------------------------------------- install dir
/** The folder holding SpectroCraft.exe. */
juce::File installDir();

/** True when files can be created and removed in `dir` (a probe file is
    written and deleted; nothing else is touched). */
bool dirIsWritable (const juce::File& dir);

/** The updater binary inside a staging tree (the manifest lists it as
    SpectroCraft/spectrocraft-updater.exe; a search is the fallback). */
juce::File findUpdaterBinary (const juce::File& stagingDir);

/** Where the app downloads and stages an update: <temp>/SpectroCraft/update.
    Never inside the install folder, never inside %APPDATA%/SpectroCraft
    (that folder holds the user's settings, models and caches). */
juce::File updateTempDir();

/** <stagingDir>/update-plan.txt — the plain-text file the updater reads:

    spectrocraft-update-plan/1
    version=1.0.2300
    file=SpectroCraft.exe
    file=SpectroCraft/spectrocraft-updater.exe
    …

    One file= line per manifest entry, in manifest order. The updater has no
    JSON parser, which is why the plan exists: the app has already checked the
    manifest signature and every hash, and hands over only the list of paths
    to swap. */
juce::File updatePlanFile (const juce::File& stagingDir);
bool writeUpdatePlan (const juce::File& stagingDir, const Manifest& manifest,
                      juce::String& errOut);

/** Launch the updater and return. The app must quit right after this. */
bool launchUpdater (const juce::File& updater, const juce::File& install,
                    const juce::File& stagingDir, const juce::File& appExe,
                    juce::String& errOut);

// ---------------------------------------------------------------- result
struct UpdateResult
{
    bool success = false;
    juce::String version;
    juce::String reason;        // failure text
    juce::String failedFile;    // the file that could not be swapped
    juce::Array<juce::String> oldFiles;      // absolute .old paths to delete
    juce::Array<juce::String> cleanupDirs;   // staging + temp folders to delete
    juce::Array<juce::String> replaced;      // manifest-relative paths
};

/** Read <installDir>/update-result.json (written by the updater). Removes the
    file so a restart never reports the same update twice. */
bool readUpdateResult (const juce::File& installDir, UpdateResult& out);

/** Where the updater writes update-result.json. */
juce::File updateResultFile (const juce::File& installDir);

}} // namespace sc::update
