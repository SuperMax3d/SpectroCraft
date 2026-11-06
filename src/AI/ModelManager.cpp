// SpectroCraft — generic AI model manager (Phase 0 of the AI plan).
// See include/AI/ModelManager.h for the design notes.

#include "AI/ModelManager.h"
#include "AI/Sha256.h"

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <juce_core/juce_core.h>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <wininet.h>
#else
    #include <cerrno>
    #include <cstring>
    #include <memory>
    #include <stdio.h>      // ::rename
    #include <unistd.h>     // ::getpid
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
// Whisper's ggml .bin checkpoints come from ggerganov/whisper.cpp on
// Hugging Face. (The OpenVINO encoder IR files are no longer used: the app
// runs whisper.cpp's prebuilt b5454 binaries, which encode + decode in ggml.)
const char* const kWhisperBase = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main";

std::string hfUrl(const std::string& model, const char* file)
{
    return std::string(kHfBase) + "/" + model + "/" + file;
}

// Whisper models: plain single-file ggml .bin downloads from ggerganov's
// Hugging Face repo (the same checkpoints the prebuilt b5454 binaries load).
ModelSpec whisperModel(const char* name, const char* display,
                       int64_t binSize, const char* binSha)
{
    ModelSpec s;
    s.id = std::string("whisper-") + name;
    s.displayName = display;

    ModelFileSpec bin;
    bin.url = std::string(kWhisperBase) + "/ggml-" + name + ".bin";
    bin.sha256 = binSha;
    bin.sizeBytes = binSize;
    bin.name = std::string ("ggml-") + name + ".bin";
    s.files.push_back (std::move (bin));

    // Headroom for the .part download buffer plus a re-download.
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
        whisperModel ("tiny-q5_1",     "Whisper tiny q5_1, quantized (~32 MB)",
                      32152673,   "818710568da3ca15689e31a743197b520007872ff9576237bda97bd1b469c3d7"),
        whisperModel ("base",          "Whisper base (~148 MB)",
                      147951465,  "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe"),
        whisperModel ("base-q5_1",     "Whisper base q5_1, quantized (~60 MB)",
                      59707625,   "422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898"),
        whisperModel ("small",         "Whisper small (~488 MB)",
                      487601967,  "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b"),
        whisperModel ("small-q5_1",    "Whisper small q5_1, quantized (~190 MB)",
                      190085487,  "ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb"),
        whisperModel ("medium",        "Whisper medium (~1.5 GB)",
                      1533763059, "6c14d5adee5f86394037b4e4e8b59f1673b6cee10e3cf0b11bbdbee79c156208"),
        whisperModel ("large-v1",      "Whisper large-v1 (~3.1 GB)",
                      3094623691, "7d99f41a10525d0206bddadd86760181fa920438b6b33237e3118ff6c83bb53d"),
        whisperModel ("large-v2",      "Whisper large-v2 (~3.1 GB)",
                      3094623691, "9a423fe4d40c82774b6af34115b8b935f34152246eb19e80e376071d3f999487"),
        whisperModel ("large-v3",      "Whisper large-v3 (~3.1 GB)",
                      3095033483, "64d182b440b98d5203c4f9bd541544d84c605196c4f7b845dfa11fb23594d1e2"),
        whisperModel ("large-v3-turbo", "Whisper large-v3-turbo (~1.6 GB)",
                      1624555275, "1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69"),
        whisperModel ("large-v3-turbo-q8_0", "Whisper large-v3-turbo q8_0, quantized (~874 MB)",
                      874188075,  "317eb69c11673c9de1e1f0d459b253999804ec71ac4c23c17ecf5fbe24e259a1"),
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
    // SC_MODELS_DIR=<absolute folder> replaces the models root, on every
    // platform (Windows included), so tests, CI and scripted runs can isolate
    // their model folder. The macOS / Linux unit tests use it (ctest sets it,
    // `--run-unit-tests` defaults it to a temp folder): on macOS the per-user
    // folders are resolved without $HOME, so redirecting HOME alone would
    // still fill the real ~/Library. sc::acpp::dir() follows the same rule.
    {
        const juce::String env = juce::SystemStats::getEnvironmentVariable ("SC_MODELS_DIR", {}).trim ();
        if (env.isNotEmpty () && juce::File::isAbsolutePath (env))
            return juce::File (env);
    }
#if SC_TESTS_EXE
    // Unit tests must never touch the user's real model cache. The isolated
    // folder is kept between runs so the AI tests do not re-download ~170 MB
    // of weights every time (ensureInstalled verifies what it reuses).
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
               .getChildFile ("SpectroCraftTests-models");
#else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("SpectroCraft")
               .getChildFile ("models");
#endif
}

// Files whose SHA-256 already matched in this process, keyed by full path,
// with the size and modification time seen then: ensureInstalled() skips
// re-hashing them (up to 3 GB per Whisper model) on every run.
struct VerifiedStamp { int64_t size = 0; int64_t mtimeMs = 0; };

std::mutex& verifiedMutex()
{
    static std::mutex m;
    return m;
}

std::map<juce::String, VerifiedStamp>& verifiedFiles()
{
    static std::map<juce::String, VerifiedStamp> files;
    return files;
}

bool isKnownVerified(const juce::File& p)
{
    std::lock_guard<std::mutex> lock (verifiedMutex());
    const auto it = verifiedFiles().find (p.getFullPathName ());
    return it != verifiedFiles().end ()
        && it->second.size == p.getSize ()
        && it->second.mtimeMs == p.getLastModificationTime ().toMilliseconds ();
}

void rememberVerified(const juce::File& p)
{
    std::lock_guard<std::mutex> lock (verifiedMutex());
    verifiedFiles()[p.getFullPathName ()] = { p.getSize (), p.getLastModificationTime ().toMilliseconds () };
}

bool fileMatchesSha256(const juce::File& p, const std::string& expected)
{
    juce::FileInputStream in (p);
    if (in.failedToOpen ())
        return false;
    Sha256 digest;
    char buf[1 << 16];
    while (true)
    {
        const int n = in.read (buf, sizeof (buf));
        if (n <= 0) break;
        digest.update ((const unsigned char*) buf, (size_t) n);
    }
    return digest.hexDigest () == expected;
}

// file:// sources (an offline mirror, or the unit tests' local fixtures): the
// same contract as the network downloaders below — streamed to the .part file
// while hashing, size and SHA-256 enforced, renamed into place only when
// everything matched, cancellation through onProgress.
bool localFileGet(const juce::File& src, const juce::File& destPart, const juce::File& finalDest,
                  const std::string& expectedSha256, int64_t expectedSize,
                  const ModelManager::ProgressFn& onProgress, std::string* errOut)
{
    juce::FileInputStream in (src);
    if (in.failedToOpen ())
    {
        if (errOut) *errOut = "download failed: cannot read the local source file '"
                              + src.getFullPathName ().toStdString () + "'.";
        return false;
    }
    const int64_t total = in.getTotalLength ();

    destPart.deleteFile ();   // FileOutputStream opens in append mode
    Sha256 digest;
    int64_t got = 0;
    bool ok = false, cancelled = false, writeFailed = false;
    {
        juce::FileOutputStream out (destPart);
        if (out.failedToOpen ())
        {
            if (errOut) *errOut = "Could not write to the models folder.";
            return false;
        }
        constexpr int bufSize = 1 << 18;
        std::unique_ptr<char[]> buf (new char[(size_t) bufSize]);
        for (;;)
        {
            const int n = in.read (buf.get (), bufSize);
            if (n <= 0) break;
            if (!out.write (buf.get (), (size_t) n)) { writeFailed = true; break; }
            digest.update ((const unsigned char*) buf.get (), (size_t) n);
            got += n;
            const int64_t denom = total > 0 ? total : expectedSize;
            float frac = denom > 0 ? (float) ((double) got / (double) denom) : 1.0f;
            if (frac > 1.0f) frac = 1.0f;
            if (onProgress && !onProgress (frac, "copying…")) { cancelled = true; break; }
        }
        out.flush ();
    }

    if (!cancelled)
    {
        if (writeFailed || destPart.getSize () != got)
        {
            if (errOut) *errOut = "Could not write the whole file to the models folder (disk full?).";
        }
        else if (expectedSize > 0 && got != expectedSize)
        {
            if (errOut) *errOut = "Downloaded size mismatch (" + std::to_string ((long long) got) + " of "
                                  + std::to_string ((long long) expectedSize) + " bytes).";
        }
        else if (!expectedSha256.empty () && digest.hexDigest () != expectedSha256)
        {
            if (errOut) *errOut = "SHA-256 mismatch: the downloaded file does not match the expected checksum.";
        }
        else
        {
            finalDest.deleteFile ();
            ok = destPart.moveFileTo (finalDest);
            if (!ok && errOut) *errOut = "Could not move the downloaded file into place.";
        }
    }
    if (!ok) destPart.deleteFile ();
    return ok;
}

bool isFileUrl(const std::string& url)
{
    return juce::String (url).startsWithIgnoreCase ("file://");
}

juce::File localFileFromUrl(const std::string& url)
{
    return juce::URL (juce::String (url)).getLocalFile ();
}

} // namespace

std::string ModelManager::fileName(const ModelFileSpec& f) { return fileNameFor (f); }
bool ModelManager::wasVerified(const juce::File& f)   { return isKnownVerified (f); }
void ModelManager::markVerified(const juce::File& f)  { rememberVerified (f); }

void ModelManager::forgetVerified(const juce::File& f)
{
    std::lock_guard<std::mutex> lock (verifiedMutex());
    verifiedFiles().erase (f.getFullPathName ());
}

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
        // Files without a published hash are accepted on existence alone.
        if (f.sha256.empty())
            continue;
        if (!fileMatchesSha256 (p, f.sha256))
        {
            if (badFileOut) *badFileOut = fileNameFor (f);
            return false;
        }
        rememberVerified (p);
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
#else
    // statfs() of the folder (or of its closest existing parent).
    const int64_t avail = dir.getBytesFreeOnVolume ();
    if (avail > 0)
        return avail;
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
        rememberVerified (dest);   // hashed while downloading
    }

    // Final verification pass over the whole model. Files already verified in
    // this process (same size and modification time) are not hashed again.
    for (const auto& f : spec.files)
    {
        const juce::File p = dir.getChildFile (fileNameFor (f));
        if (!p.existsAsFile ())
        {
            if (errOut) *errOut = "Model file '" + fileNameFor (f) + "' is missing.";
            return false;
        }
        if (f.sha256.empty () || isKnownVerified (p))
            continue;
        if (!fileMatchesSha256 (p, f.sha256))
        {
            // Delete the corrupt file so the next attempt downloads it again
            // (it has the right size, so it would otherwise be kept forever).
            p.deleteFile ();
            if (errOut) *errOut = "Checksum mismatch for '" + fileNameFor (f)
                                  + "' — the corrupt file was deleted; run again to re-download it.";
            return false;
        }
        rememberVerified (p);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Downloading. Windows: blocking WinInet GET with redirect following, streamed
// to <dest>.part while hashing, then renamed into place on success. macOS /
// Linux: the same contract on juce::WebInputStream (NSURLSession on macOS,
// libcurl on Linux).
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

    // A stalled connection must end the transfer (as a failure) instead of
    // blocking InternetReadFile - and the Cancel button - forever.
    {
        DWORD timeoutMs = 60000;
        InternetSetOptionA (hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof (timeoutMs));
        InternetSetOptionA (hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof (timeoutMs));
    }

    bool ok = false;
    // Note: this SDK's wininet.h has no INTERNET_FLAG_RECONNECT; the default
    // InternetOpenUrl behaviour already reconnects as needed.
    DWORD flags = (url.rfind ("https://", 0) == 0 ? INTERNET_FLAG_SECURE : 0);
    // Large model files: never serve them from / copy them into the WinINet cache.
#ifdef INTERNET_FLAG_RELOAD
    flags |= INTERNET_FLAG_RELOAD;
#endif
#ifdef INTERNET_FLAG_NO_CACHE_WRITE
    flags |= INTERNET_FLAG_NO_CACHE_WRITE;
#endif
    HINTERNET hRequest = InternetOpenUrlA (hInternet, url.c_str (), nullptr, 0,
                                           flags, 0);
    if (!hRequest)
    {
        const DWORD e = GetLastError ();
        if (errOut) *errOut = "HTTP request failed (error " + std::to_string ((int) e) + ").";
        InternetCloseHandle (hInternet);
        return false;
    }

    // Final HTTP status (WinINet follows redirects). An error page (404, 403,
    // proxy authentication, captive portal...) must not be hashed and then
    // reported as a checksum problem.
    DWORD status = 0, statusSize = sizeof (status);
    // This SDK's HttpQueryInfoA has a trailing lpdwIndex parameter.
    if (HttpQueryInfoA (hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                        &status, &statusSize, nullptr)
        && (status < 200 || status > 299))
    {
        if (errOut) *errOut = "download failed: HTTP " + std::to_string ((int) status)
                              + " (no internet access, a proxy or a captive portal?).";
        InternetCloseHandle (hRequest);
        InternetCloseHandle (hInternet);
        return false;
    }

    // Content-Length, read as text: without HTTP_QUERY_FLAG_NUMBER the call
    // returns a string, and the numeric form would be a 32-bit DWORD anyway.
    // 0 = unknown (chunked responses): fall back to the registry size.
    int64_t contentLength = 0;
    {
        char lenText[64] = {};
        DWORD lenSize = sizeof (lenText) - 1;
        if (HttpQueryInfoA (hRequest, HTTP_QUERY_CONTENT_LENGTH, lenText, &lenSize, nullptr))
            contentLength = juce::String (lenText).trim ().getLargeIntValue ();
        if (contentLength < 0) contentLength = 0;
    }
    const int64_t totalBytes = contentLength > 0 ? contentLength : expectedSize;

    destPart.deleteFile();   // FileOutputStream opens in append mode
    Sha256 digest;
    int64_t got = 0;
    bool cancelled = false, openFailed = false, transferError = false;
    {
        juce::FileOutputStream out (destPart);
        if (!out.failedToOpen())
        {
            char buf[1 << 18];

            while (!cancelled)
            {
                DWORD nRead = 0;
                // InternetReadFile returns TRUE with nRead == 0 at the end of
                // the stream; FALSE is a network error (reset, timeout...).
                if (!InternetReadFile (hRequest, buf, sizeof (buf), &nRead))
                {
                    transferError = true;
                    break;
                }
                if (nRead == 0)
                    break;

                if (!out.write ((const unsigned char*) buf, nRead))
                    break;   // caught by the on-disk size check below
                digest.update ((const unsigned char*) buf, nRead);
                got += (int64_t) nRead;

                float frac = totalBytes > 0 ? (float) ((double) got / (double) totalBytes) : 1.0f;
                if (frac > 1.0f) frac = 1.0f;
                if (onProgress && !onProgress (frac, "downloading…"))
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
        char msg [160];
        if (transferError)
        {
            std::snprintf (msg, sizeof (msg), "The download was interrupted after %lld bytes (network error).",
                           (long long) got);
            if (errOut) *errOut = msg;
        }
        else if (destPart.getSize () != got)
        {
            if (errOut) *errOut = "Could not write the whole file to the models folder (disk full?).";
        }
        else if ((contentLength > 0 && got != contentLength) || (expectedSize > 0 && got != expectedSize))
        {
            std::snprintf (msg, sizeof (msg), "Downloaded size mismatch (%lld of %lld bytes).",
                           (long long) got, (long long) (expectedSize > 0 ? expectedSize : contentLength));
            if (errOut) *errOut = msg;
        }
        else if (!expectedSha256.empty() && digest.hexDigest () != expectedSha256)
        {
            if (errOut) *errOut = "SHA-256 mismatch: the downloaded file does not match the expected checksum.";
        }
        else
        {
            if (finalDest.exists ()) finalDest.deleteFile ();
            ok = destPart.moveFileTo (finalDest);
            if (!ok && errOut) *errOut = "Could not move the downloaded file into place.";
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
    if (isFileUrl (url))
        return localFileGet (localFileFromUrl (url), part, dest, expectedSha256, expectedSize, onProgress, errOut);
    return wininetGet (url, part, dest, expectedSha256, expectedSize, onProgress, errOut);
}

#else   // macOS / Linux

// Same guarantees as wininetGet above: final HTTP status checked (an error
// page is never hashed), expected size and Content-Length enforced, streamed
// to a .part file while hashing, SHA-256 verified, renamed into place only
// when everything matched, cancellation through onProgress, 60 s timeouts.
static bool webStreamGet(const std::string& url, const juce::File& destPart,
                         const juce::File& finalDest,
                         const std::string& expectedSha256, int64_t expectedSize,
                         ModelManager::ProgressFn onProgress, std::string* errOut)
{
    juce::WebInputStream in (juce::URL (juce::String (url)), false);
    // Hugging Face "resolve" URLs redirect to their CDN. The timeout covers the
    // connection and, through libcurl's low-speed limit / NSURLSession's idle
    // timeout, a transfer that stalls.
    in.withConnectionTimeout (60000)
      .withNumRedirectsToFollow (10)
      .withExtraHeaders ("User-Agent: SpectroCraft");

    if (!in.connect (nullptr))
    {
        if (errOut) *errOut = "HTTP request failed (no network connection?).";
        return false;
    }

    // Final status after redirects.
    const int status = in.getStatusCode ();
    if (status < 200 || status > 299)
    {
        if (errOut) *errOut = "download failed: HTTP " + std::to_string (status)
                              + " (no internet access, a proxy or a captive portal?).";
        return false;
    }

    // 0 = unknown (chunked responses): fall back to the registry size.
    int64_t contentLength = (int64_t) in.getTotalLength ();
    if (contentLength < 0) contentLength = 0;
    const int64_t totalBytes = contentLength > 0 ? contentLength : expectedSize;

    destPart.deleteFile ();   // FileOutputStream opens in append mode
    Sha256 digest;
    int64_t got = 0;
    bool ok = false, cancelled = false, openFailed = false, transferError = false;
    {
        juce::FileOutputStream out (destPart);
        if (!out.failedToOpen ())
        {
            // On the heap: macOS worker threads have small stacks.
            constexpr int bufSize = 1 << 18;
            std::unique_ptr<char[]> buf (new char[(size_t) bufSize]);

            while (!cancelled)
            {
                const int nRead = in.read (buf.get (), bufSize);
                if (nRead < 0)
                {
                    transferError = true;
                    break;
                }
                if (nRead == 0)
                {
                    // End of the body, or a network error (libcurl reports it
                    // here; NSURLSession ends the stream early, which the size
                    // checks below catch).
                    if (in.isError ())
                        transferError = true;
                    break;
                }

                if (!out.write (buf.get (), (size_t) nRead))
                    break;   // caught by the on-disk size check below
                digest.update ((const unsigned char*) buf.get (), (size_t) nRead);
                got += (int64_t) nRead;

                float frac = totalBytes > 0 ? (float) ((double) got / (double) totalBytes) : 1.0f;
                if (frac > 1.0f) frac = 1.0f;
                if (onProgress && !onProgress (frac, "downloading…"))
                    cancelled = true;
            }

            out.flush ();   // closed when this block ends, before any rename
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
        char msg [160];
        if (transferError)
        {
            std::snprintf (msg, sizeof (msg), "The download was interrupted after %lld bytes (network error).",
                           (long long) got);
            if (errOut) *errOut = msg;
        }
        else if (destPart.getSize () != got)
        {
            if (errOut) *errOut = "Could not write the whole file to the models folder (disk full?).";
        }
        else if ((contentLength > 0 && got != contentLength) || (expectedSize > 0 && got != expectedSize))
        {
            std::snprintf (msg, sizeof (msg), "Downloaded size mismatch (%lld of %lld bytes).",
                           (long long) got, (long long) (expectedSize > 0 ? expectedSize : contentLength));
            if (errOut) *errOut = msg;
        }
        else if (!expectedSha256.empty () && digest.hexDigest () != expectedSha256)
        {
            if (errOut) *errOut = "SHA-256 mismatch: the downloaded file does not match the expected checksum.";
        }
        else
        {
            // rename(2) replaces an existing file atomically: another process
            // checking the model never sees it missing or half-written.
            ok = ::rename (destPart.getFullPathName ().toRawUTF8 (),
                           finalDest.getFullPathName ().toRawUTF8 ()) == 0;
            if (!ok && errOut)
                *errOut = std::string ("Could not move the downloaded file into place (")
                          + std::strerror (errno) + ").";
        }
    }

    if (!ok) destPart.deleteFile ();
    return ok;
}

bool ModelManager::downloadFile(const std::string& url, const juce::File& dest,
                                const std::string& expectedSha256, int64_t expectedSize,
                                ProgressFn onProgress, std::string* errOut)
{
    // One .part file per process: test shards (or two app instances) that
    // download the same model at once never write into each other's file.
    const juce::File part = dest.getSiblingFile (dest.getFileName () + "."
                                                 + juce::String ((int) ::getpid ()) + ".part");
    if (isFileUrl (url))
        return localFileGet (localFileFromUrl (url), part, dest, expectedSha256, expectedSize, onProgress, errOut);
    return webStreamGet (url, part, dest, expectedSha256, expectedSize, onProgress, errOut);
}

#endif

}} // namespace sc::ai
