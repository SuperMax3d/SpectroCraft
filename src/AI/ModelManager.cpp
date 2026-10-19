// SpectroCraft — generic AI model manager (Phase 0 of the AI plan).
// See include/AI/ModelManager.h for the design notes.

#include "AI/ModelManager.h"
#include "AI/Sha256.h"

#include <miniz/miniz.h>

#include <cstdio>
#include <cstring>
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
// Whisper's ggml + OpenVINO encoder pairs are published as per-model zips.
const char* const kWhisperBase = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main";

std::string hfUrl(const std::string& model, const char* file)
{
    return std::string(kHfBase) + "/" + model + "/" + file;
}

// Whisper models are published as single ggml .bin files on HuggingFace
// (ggerganov/whisper.cpp). The optional OpenVINO encoder pair
// (ggml-<name>-encoder-openvino.{xml,bin}) is not distributed by anyone; users
// who want the faster OV encode path can generate it with whisper.cpp's
// models/convert-whisper-to-openvino.py and drop both files into the model
// folder. Without them, transcription runs on the pure-ggml encoder (slower,
// identical results).
ModelSpec whisperModel(const char* name, const char* display,
                       int64_t binSize, const char* binSha)
{
    ModelSpec s;
    s.id = std::string("whisper-") + name;
    s.displayName = display;
    s.files.push_back ({ std::string(kWhisperBase) + "/ggml-" + name + ".bin",
                         binSha, binSize, std::string ("ggml-") + name + ".bin" });
    // Headroom in case the user adds an encoder pair later.
    s.minFreeDiskBytes = 2 * binSize;
    return s;
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
        whisperModel ("tiny",          "Whisper tiny (~78 MB)",
                      77691713,   "be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21"),
        whisperModel ("base",          "Whisper base (~148 MB)",
                      147951465,  "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe"),
        whisperModel ("small",         "Whisper small (~488 MB)",
                      487601967,  "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b"),
        whisperModel ("medium",        "Whisper medium (~1.5 GB)",
                      1533763059, "6c14d5adee5f86394037b4e4e8b59f1673b6cee10e3cf0b11bbdbee79c156208"),
        whisperModel ("large-v1",      "Whisper large-v1 (~3.1 GB)",
                      3094623691, "7d99f41a10525d0206bddadd86760181fa920438b6b33237e3118ff6c83bb53d"),
        whisperModel ("large-v2",      "Whisper large-v2 (~3.1 GB)",
                      3094623691, "9a423fe4d40c82774b6af34115b8b935f34152246eb19e80e376071d3f999487"),
        whisperModel ("large-v3",      "Whisper large-v3 (~3.1 GB)",
                      3095033483, "64d182b440b98d5203c4f9bd541544d84c605196c4f7b845dfa11fb23594d1e2"),
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

std::string fileNameFor(const ModelFileSpec& f)
{
    if (!f.name.empty()) return f.name;
    return urlBasename (f.url);
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
        const juce::File p = dir.getChildFile (fileNameFor (f));
        if (!p.existsAsFile() || (f.sizeBytes > 0 && p.getSize() != f.sizeBytes))
        {
            allPresent = false;
            if (missingOut) missingOut->push_back (fileNameFor (f));
        }
    }
    return allPresent;
}

bool ModelManager::verifyInstalled(const ModelSpec& spec, std::string* badFileOut) const
{
    const juce::File dir = dirFor (spec.id);
    for (const auto& f : spec.files)
    {
        const juce::File p = dir.getChildFile (fileNameFor (f));
        if (!p.existsAsFile())
        {
            if (badFileOut) *badFileOut = fileNameFor (f);
            return false;
        }
        // Files without a published hash (zip-distributed models) are
        // covered by the zip's own checksum, so existence is enough.
        if (f.sha256.empty())
            continue;
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
            if (badFileOut) *badFileOut = fileNameFor (f);
            return false;
        }
        const std::string hex = digest.hexDigest ();
        if (hex != f.sha256)
        {
            if (badFileOut) *badFileOut = fileNameFor (f);
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

    // Zip-distributed models: download + verify + extract the archive first.
    if (!spec.zip.url.empty() && !isInstalled (spec))
    {
        if (!installZip (spec, dir, onProgress, errOut))
            return false;
    }

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
        // Zip-provided files have no direct URL — they were extracted above.
        if (f.url.empty()) continue;
        const juce::File dest = dir.getChildFile (fileNameFor (f));

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
// Zip-distributed models (Whisper): download the archive, verify its SHA-256,
// extract it into the model folder with miniz, then remove the zip. A
// previously downloaded + verified zip is reused if still present, so a failed
// extraction does not force re-downloading multi-GB archives.
// ---------------------------------------------------------------------------

namespace {

std::string fileSha256(const juce::File& f)
{
    juce::FileInputStream in (f);
    Sha256 digest;
    if (!in.failedToOpen ()) return "";
    char buf[1 << 16];
    while (true)
    {
        const int n = in.read (buf, sizeof (buf));
        if (n <= 0) break;
        digest.update ((const unsigned char*) buf, (size_t) n);
    }
    return digest.hexDigest ();
}

} // namespace

bool ModelManager::installZip(const ModelSpec& spec, const juce::File& dir,
                              ProgressFn onProgress, std::string* errOut)
{
    const juce::File zip = dir.getChildFile (urlBasename (spec.zip.url));

    // Reuse an existing verified archive when possible.
    bool haveZip = false;
    if (zip.existsAsFile() && (spec.zip.sizeBytes <= 0 || zip.getSize() == spec.zip.sizeBytes))
        haveZip = (fileSha256 (zip) == spec.zip.sha256);

    if (!haveZip)
    {
        zip.deleteFile ();   // stale or wrong-size archive: re-download
        if (!downloadFile (spec.zip.url, zip, spec.zip.sha256, spec.zip.sizeBytes,
                           onProgress, errOut))
            return false;
        haveZip = true;
    }

    mz_zip_archive za;
    std::memset (&za, 0, sizeof (za));
    if (!mz_zip_reader_init_file (&za, zip.getFullPathName ().toRawUTF8 (), 0))
    {
        if (errOut) *errOut = "Could not open the downloaded model archive."
                              " It may be corrupt — it will be re-downloaded next time.";
        return false;
    }

    bool ok = true;
    const mz_uint nFiles = mz_zip_reader_get_num_files (&za);
    for (mz_uint i = 0; i < nFiles && ok; ++i)
    {
        // miniz v1.14 API: the caller supplies the destination buffer.
        char nameBuf [512];
        if (mz_zip_reader_get_filename (&za, i, nameBuf, sizeof (nameBuf)) == 0)
        {
            ok = false;
            break;
        }
        const std::string name (nameBuf);
        // Skip directory entries; reject anything that could escape the folder.
        if (name.empty () || name.back () == '/') continue;
        if (name.find ("..") != std::string::npos
            || name[0] == '/'
            || (name.size () >= 2 && name[1] == ':'))
        {
            ok = false;
            break;
        }
        const juce::File out = dir.getChildFile (name);
        if (!mz_zip_reader_extract_to_file (&za, i,
                                            out.getFullPathName ().toRawUTF8 (), 0))
            ok = false;
    }
    mz_zip_reader_end (&za);

    if (ok)
        zip.deleteFile ();   // extracted contents are what we keep; on failure the
                            // verified archive is retained so a retry skips the
                            // (possibly multi-GB) re-download.
    else if (errOut)
        *errOut = "Failed to extract the model archive. The downloaded copy was kept,"
                  " so the next attempt will not re-download it.";
    return ok;
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
    Sha256 digest;
    int64_t got = 0;
    bool cancelled = false, openFailed = false;
    {
        juce::FileOutputStream out (destPart);
        if (!out.failedToOpen())
        {
            char buf[1 << 18];

            while (!cancelled)
            {
                DWORD nRead = 0;
                // InternetReadFile returns FALSE with nRead == 0 at the end of
                // the stream — that is a clean EOF, not an error. A premature
                // stop (connection reset etc.) also lands here and is caught by
                // the size / SHA-256 verification below.
                if (!InternetReadFile (hRequest, buf, sizeof (buf), &nRead) || nRead == 0)
                    break;

                out.write ((const unsigned char*) buf, nRead);
                digest.update ((const unsigned char*) buf, nRead);
                got += (int64_t) nRead;

                if (onProgress && !onProgress (totalBytes > 0 ? (float) got / (float) totalBytes : 1.0f,
                                               "downloading…"))
                    cancelled = true;
            }

            out.flush ();   // the destructor closes the stream when this block ends
        }
        else
        {
            openFailed = true;
        }
    }

    if (openFailed)
    {
        if (errOut) *errOut = "Could not write to the models folder.";
    }
    else if (!cancelled)
    {
        // Verification and rename happen only after the file is fully closed:
        // Windows will not move or delete a file that still has an open handle.
        const std::string hex = digest.hexDigest ();
        if (expectedSha256.empty() || hex == expectedSha256)
        {
            if (expectedSize > 0 && got != expectedSize)
            {
                char msg [128];
                std::snprintf (msg, sizeof (msg), "Downloaded size mismatch (%lld of %lld bytes).",
                               (long long) got, (long long) expectedSize);
                if (errOut) *errOut = msg;
            }
            else
            {
                if (finalDest.exists ()) finalDest.deleteFile ();
                ok = destPart.moveFileTo (finalDest);
            }
        }
        else if (errOut)
        {
            *errOut = "SHA-256 mismatch: the downloaded file does not match the expected checksum.";
        }
    }

    // The stream is closed by now, so removing a failed .part cannot hit a
    // sharing violation.
    if (!ok) destPart.deleteFile ();

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
