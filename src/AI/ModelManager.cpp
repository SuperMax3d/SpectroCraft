// SpectroCraft — generic AI model manager (Phase 0 of the AI plan).
// See include/AI/ModelManager.h for the design notes.

#include "AI/ModelManager.h"
#include "AI/Sha256.h"

#include <cstdio>
#include <string>

#include <juce_core/juce_core.h>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <wininet.h>
#endif

namespace sc { namespace ai {

// ---------------------------------------------------------------------------
// Model registry. Checksums were computed from the exact files published at
// the URLs below (verified 2025-10). Models are never committed to git; they
// live in %APPDATA%/SpectroCraft/models/<id>/ after download.
// ---------------------------------------------------------------------------

namespace {

const char* const kHfBase = "https://huggingface.co/Intel/deepfilternet-openvino/resolve/main";
const char* const kOmzBase = "https://storage.openvinotoolkit.org/repositories/open_model_zoo/2023.0/models_bin/1/noise-suppression-denseunet-ll-0001/FP16";

std::string hfUrl(const std::string& model, const char* file)
{
    return std::string(kHfBase) + "/" + model + "/" + file;
}

const std::vector<ModelSpec>& registryImpl()
{
    static const std::vector<ModelSpec> models = {
        {
            "deepfilternet2",
            "DeepFilterNet 2 (recommended)",
            {
                { hfUrl("deepfilternet2", "enc.xml"),     "667881baeef4e44b66642e1ba4fed267b02a818ada05f57efecfc9cc755f2e07", 173466 },
                { hfUrl("deepfilternet2", "enc.bin"),     "ed35e70c2e8c8035909466c82af4fd447238821734bc2d85c92efe721a49145a", 2589012 },
                { hfUrl("deepfilternet2", "erb_dec.xml"), "d06b04a3ece3079b3fd1869f9960a7c3c9db6cc724162c0efc2dadc6b40d8a81", 178896 },
                { hfUrl("deepfilternet2", "erb_dec.bin"), "f1df0fb3173ef334abf338b1f11e55c40ec413abdd77cfbf80867294c3de937f", 3311612 },
                { hfUrl("deepfilternet2", "df_dec.xml"),  "cde83f4a1b6dbc017b2966dc0bd6c389360a90988ef99a00639e7e8c9a60a361", 110816 },
                { hfUrl("deepfilternet2", "df_dec.bin"),  "19862c41d157cceb1fd048087709f3a9b187fab883677ba12bf538f1bc5cfd14", 3321828 },
            },
            40 * 1024 * 1024,
        },
        {
            "deepfilternet3",
            "DeepFilterNet 3 (experimental)",
            {
                { hfUrl("deepfilternet3", "enc.xml"),     "ed385d86a59d84fcebf11bdceb2b2e3d2a7b4a1379a744dc9b0d47121622df32", 184737 },
                { hfUrl("deepfilternet3", "enc.bin"),     "fa1ed5feb4dd234e051188a2cda6c6f6efabefd87404cbae20fe6c9913dc9838", 1934676 },
                { hfUrl("deepfilternet3", "erb_dec.xml"), "bc0cf4e25911714e074658cfeeccc686787a4d274889a88afef855b542689faa", 182978 },
                { hfUrl("deepfilternet3", "erb_dec.bin"), "d5e9dc11c071f72b343d5af5d344a504a903347d455952e8d4027da3ab47e792", 3278844 },
                { hfUrl("deepfilternet3", "df_dec.xml"),  "7366c6715fb2c0febbcd05b0d3f1e47d48eaeeb213fb2d7eeb2f4aa24b63dc42", 122326 },
                { hfUrl("deepfilternet3", "df_dec.bin"),  "7bd50b92ae1c6175be9ddb3e8cf7f81b65b95223957bcd558cb2c6ef281aa435", 3325940 },
            },
            40 * 1024 * 1024,
        },
        {
            "denseunet",
            "DenseUNet (legacy)",
            {
                { std::string(kOmzBase) + "/noise-suppression-denseunet-ll-0001.xml",
                  "89116a01cc59f7ac3f1f1365c851e47c9089fd33ca86712c30dc1f0ee7d14803", 689820 },
                { std::string(kOmzBase) + "/noise-suppression-denseunet-ll-0001.bin",
                  "da59b41a656b2948a4b45580b4870614d2dfa071a7566555aea4547423888e08", 8625568 },
            },
            32 * 1024 * 1024,
        },
    };
    return models;
}

std::string urlBasename(const std::string& url)
{
    const size_t slash = url.find_last_of('/');
    if (slash == std::string::npos) return url;
    // strip any query string
    size_t end = url.size();
    const size_t q = url.find_first_of('?');
    if (q != std::string::npos) end = q;
    const std::string tail = url.substr(slash + 1, end - slash - 1);
    return tail.empty() ? "model.bin" : tail;
}

juce::File defaultModelRoot()
{
#if SC_TESTS_EXE
    // Unit tests must never touch the user's real model cache.
    static juce::File isolated = [] ()
    {
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("SpectroCraftTests-models");
        if (f.exists()) f.deleteRecursively();
        return f;
    } ();
    return isolated;
#else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("SpectroCraft")
               .getChildFile ("models");
#endif
}

} // namespace

// ---------------------------------------------------------------------------

ModelManager& ModelManager::instance()
{
    static ModelManager mgr;
    return mgr;
}

ModelManager::ModelManager(std::string rootOverride) : _rootOverride (std::move(rootOverride)) {}

const std::vector<ModelSpec>& ModelManager::registry() const { return registryImpl(); }

const ModelSpec* ModelManager::find(const std::string& id) const
{
    for (const auto& m : registry())
        if (m.id == id) return &m;
    return nullptr;
}

juce::File ModelManager::modelRoot() const
{
    if (!_rootOverride.empty()) return juce::File (_rootOverride);
    return defaultModelRoot();
}

juce::File ModelManager::dirFor(const std::string& modelId) const
{
    return modelRoot().getChildFile (modelId);
}

bool ModelManager::isInstalled(const ModelSpec& spec, std::vector<std::string>* missingOut) const
{
    if (missingOut != nullptr) missingOut->clear ();
    const juce::File dir = dirFor (spec.id);
    bool allPresent = true;
    for (const auto& f : spec.files)
    {
        const juce::File p = dir.getChildFile (urlBasename (f.url));
        if (!p.existsAsFile() || (f.sizeBytes > 0 && p.getSize() != f.sizeBytes))
        {
            allPresent = false;
            if (missingOut) missingOut->push_back (urlBasename (f.url));
        }
    }
    return allPresent;
}

bool ModelManager::verifyInstalled(const ModelSpec& spec, std::string* badFileOut) const
{
    const juce::File dir = dirFor (spec.id);
    for (const auto& f : spec.files)
    {
        const juce::File p = dir.getChildFile (urlBasename (f.url));
        if (!p.existsAsFile())
        {
            if (badFileOut) *badFileOut = urlBasename (f.url);
            return false;
        }
        juce::FileInputStream in (p);
        Sha256 digest;
        if (!in.failedToOpen ())
        {
            char buf[1 << 16];
            while (true)
            {
                const int n = in.read (buf, sizeof (buf));
                if (n <= 0) break;
                digest.update ((const unsigned char*) buf, (size_t) n);
            }
        }
        else
        {
            if (badFileOut) *badFileOut = urlBasename (f.url);
            return false;
        }
        const std::string hex = digest.hexDigest ();
        if (hex != f.sha256)
        {
            if (badFileOut) *badFileOut = urlBasename (f.url);
            return false;
        }
    }
    return true;
}

int64_t ModelManager::freeDiskSpaceBytes(const juce::File& dir)
{
#if defined(_WIN32)
    ULARGE_INTEGER freeBytes, totalBytes, availBytes;
    const juce::String path = dir.getFullPathName ();
    if (GetDiskFreeSpaceExW (path.toWideCharPointer (),
                             &freeBytes, &totalBytes, &availBytes))
        return (int64_t) availBytes.QuadPart;
#endif
    return 0;   // unknown -> caller skips the check
}

bool ModelManager::ensureInstalled(const ModelSpec& spec, ProgressFn onProgress, std::string* errOut)
{
    juce::File dir = dirFor (spec.id);
    if (!dir.exists()) dir.createDirectory ();

    const int64_t freeBytes = freeDiskSpaceBytes (modelRoot());
    if (freeBytes > 0 && freeBytes < spec.minFreeDiskBytes)
    {
        if (errOut)
        {
            char buf [128];
            std::snprintf (buf, sizeof (buf), "Not enough disk space: need ~%.0f MB free, have ~%.0f MB.",
                           (double) spec.minFreeDiskBytes / (1024.0 * 1024.0),
                           (double) freeBytes / (1024.0 * 1024.0));
            *errOut = buf;
        }
        return false;
    }

    const size_t nFiles = spec.files.size();
    for (size_t i = 0; i < nFiles; ++i)
    {
        const ModelFileSpec& f = spec.files[i];
        const juce::File dest = dir.getChildFile (urlBasename (f.url));

        if (dest.existsAsFile() && (f.sizeBytes <= 0 || dest.getSize() == f.sizeBytes))
            continue;   // already present with the right size

        auto fileProgress = [i, nFiles, &onProgress] (float frac, const std::string& status) -> bool
        {
            if (!onProgress) return true;
            return onProgress (((float) i + frac) / (float) nFiles, status);
        };

        if (!downloadFile (f.url, dest, f.sha256, f.sizeBytes, fileProgress, errOut))
            return false;
    }

    // Final verification pass over the whole model.
    std::string bad;
    if (!verifyInstalled (spec, &bad))
    {
        if (errOut) *errOut = "Checksum mismatch for '" + bad + "' — file deleted, please re-download.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Downloading. Windows: blocking WinInet GET with redirect following, streamed
// to <dest>.part while hashing, then renamed into place on success. Other
// platforms: not supported in v1 (manual install instructions are shown).
// ---------------------------------------------------------------------------

#if defined(_WIN32)

static bool wininetGet(const std::string& url, const juce::File& destPart,
                       const juce::File& finalDest,
                       const std::string& expectedSha256, int64_t expectedSize,
                       ModelManager::ProgressFn onProgress, std::string* errOut)
{
    HINTERNET hInternet = InternetOpenA ("SpectroCraft", INTERNET_OPEN_TYPE_PRECONFIG,
                                         nullptr, 0, 0);
    if (!hInternet)
    {
        if (errOut) *errOut = "Could not initialise the network stack.";
        return false;
    }

    bool ok = false;
    // Note: this SDK's wininet.h has no INTERNET_FLAG_RECONNECT; the default
    // InternetOpenUrl behaviour already reconnects as needed.
    const DWORD flags = (url.rfind ("https://", 0) == 0 ? INTERNET_FLAG_SECURE : 0);
    HINTERNET hRequest = InternetOpenUrlA (hInternet, url.c_str (), nullptr, 0,
                                           flags, 0);
    if (!hRequest)
    {
        const DWORD e = GetLastError ();
        if (errOut) *errOut = "HTTP request failed (error " + std::to_string ((int) e) + ").";
        InternetCloseHandle (hInternet);
        return false;
    }

    // Content-Length (may be 0 for chunked responses).
    DWORD contentLength = 0;
    DWORD lenSize = sizeof (contentLength);
    // This SDK's HttpQueryInfoA has a trailing lpdwIndex parameter.
    HttpQueryInfoA (hRequest, HTTP_QUERY_CONTENT_LENGTH, &contentLength, &lenSize, nullptr);
    const int64_t totalBytes = (int64_t) contentLength;

    destPart.deleteFile();   // FileOutputStream opens in append mode
    juce::FileOutputStream out (destPart);
    if (!out.failedToOpen())
    {
        Sha256 digest;
        char buf[1 << 18];
        int64_t got = 0;
        bool cancelled = false, readError = false;

        while (!cancelled && !readError)
        {
            DWORD nRead = 0;
            if (!InternetReadFile (hRequest, buf, sizeof (buf), &nRead) || nRead == 0)
            {
                readError = true;
                break;
            }
            out.write ((const unsigned char*) buf, nRead);
            digest.update ((const unsigned char*) buf, nRead);
            got += (int64_t) nRead;

            if (onProgress && !onProgress (totalBytes > 0 ? (float) got / (float) totalBytes : 1.0f,
                                           "downloading…"))
                cancelled = true;
        }

        out.flush ();   // (the destructor closes the stream)

        if (!readError && !cancelled)
        {
            const std::string hex = digest.hexDigest ();
            if (expectedSha256.empty() || hex == expectedSha256)
            {
                if (expectedSize > 0 && got != expectedSize)
                {
                    destPart.deleteFile ();
                    if (errOut)
                {
                    char buf [128];
                    std::snprintf (buf, sizeof (buf), "Downloaded size mismatch (%lld of %lld bytes).",
                                   (long long) got, (long long) expectedSize);
                    *errOut = buf;
                }
                }
                else
                {
                    if (finalDest.exists ()) finalDest.deleteFile ();
                    ok = destPart.moveFileTo (finalDest);
                }
            }
            else
            {
                destPart.deleteFile ();
                if (errOut) *errOut = "SHA-256 mismatch: the downloaded file does not match the expected checksum.";
            }
        }
        else if (!cancelled && errOut)
        {
            destPart.deleteFile ();
            const DWORD e = GetLastError ();
            *errOut = "Download interrupted (error " + std::to_string ((int) e) + ").";
        }
    }
    else if (errOut)
    {
        *errOut = "Could not write to the models folder.";
    }

    InternetCloseHandle (hRequest);
    InternetCloseHandle (hInternet);
    return ok;
}

bool ModelManager::downloadFile(const std::string& url, const juce::File& dest,
                                const std::string& expectedSha256, int64_t expectedSize,
                                ProgressFn onProgress, std::string* errOut)
{
    const juce::File part = dest.getSiblingFile (dest.getFileName () + ".part");
    return wininetGet (url, part, dest, expectedSha256, expectedSize, onProgress, errOut);
}

#else   // non-Windows: v1 does not auto-download off Windows.

bool ModelManager::downloadFile(const std::string&, const juce::File&,
                                const std::string&, int64_t, ProgressFn, std::string* errOut)
{
    if (errOut) *errOut = "Automatic model download is only supported on Windows in v1. "
        "Download the files manually into the models folder.";
    return false;
}

#endif

}} // namespace sc::ai
