// SpectroCraft - audio.cpp stem-separation support (external CLI tool).
#include "AudioCpp.h"
#include "AI/Sha256.h"
#include "Resampler.h"

#include <cstdint>
#include <memory>

#if JUCE_WINDOWS
#  include <windows.h>
#  include <wininet.h>
#endif

namespace sc { namespace acpp {

juce::File dir()
{
    // SC_MODELS_DIR=<absolute folder> replaces the models root on every
    // platform (same rule as sc::ai::ModelManager): tests and CI never touch
    // the user's real folder.
    juce::File modelsRoot = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                .getChildFile ("SpectroCraft")
                                .getChildFile ("models");
    {
        const juce::String env = juce::SystemStats::getEnvironmentVariable ("SC_MODELS_DIR", {}).trim ();
        if (env.isNotEmpty () && juce::File::isAbsolutePath (env))
            modelsRoot = juce::File (env);
    }
    const juce::File d = modelsRoot.getChildFile ("audiocpp");
    if (!d.exists())
        d.createDirectory();
    return d;
}

juce::File binaryPath(const juce::File& d)
{
    return (d.getFullPathName().isEmpty() ? dir() : d).getChildFile ("audiocpp_cli.exe");
}

bool hasBinary(const juce::File& d)
{
    const juce::File b = binaryPath (d);
    return b.existsAsFile () && b.getSize () > 0;
}

std::vector<juce::File> listModels(const juce::File& d)
{
    std::vector<juce::File> out;
    const juce::File root = (d.getFullPathName().isEmpty() ? dir() : d);
    if (!root.isDirectory()) return out;
    for (auto entry : juce::RangedDirectoryIterator (root, false))
    {
        const juce::String name = entry.getFile().getFileName();
        if (name.startsWithIgnoreCase ("sam-audio-") && name.endsWithIgnoreCase (".gguf"))
            out.push_back (entry.getFile());
    }
    std::sort (out.begin (), out.end (),
               [] (const juce::File& a, const juce::File& b) {
                   return a.getFileName().toLowerCase() < b.getFileName().toLowerCase(); });
    return out;
}

std::vector<juce::File> listDemucs6Models(const juce::File& d)
{
    std::vector<juce::File> out;
    const juce::File root = (d.getFullPathName().isEmpty() ? dir().getChildFile ("HTDemucs-6stems-GGUF") : d);
    if (!root.isDirectory()) return out;
    for (auto entry : juce::RangedDirectoryIterator (root, false))
        if (entry.getFile().hasFileExtension ("gguf"))
            out.push_back (entry.getFile());
    std::sort (out.begin (), out.end (),
               [] (const juce::File& a, const juce::File& b) {
                   return a.getFileName().toLowerCase() < b.getFileName().toLowerCase(); });
    return out;
}

juce::File demucsFtModelDir(const juce::File& d)
{
    const juce::File root = (d.getFullPathName().isEmpty() ? dir().getChildFile ("htdemucs_ft") : d);
    if (!root.isDirectory()) return {};
    for (auto entry : juce::RangedDirectoryIterator (root, false))
        if (entry.getFile().hasFileExtension ("gguf"))
            return root;
    return {};
}

juce::File flashSrModelFile(const juce::File& d)
{
    const juce::File f = (d.getFullPathName().isEmpty() ? dir() : d)
                             .getChildFile ("flashsr")
                             .getChildFile ("flashsr.safetensors");
    return f.existsAsFile () ? f : juce::File ();
}

std::vector<juce::File> listAudioSRModels(const juce::File& d)
{
    std::vector<juce::File> out;
    // `d` is the models root (like flashSrModelFile), not the GGUF folder.
    const juce::File root = (d.getFullPathName().isEmpty() ? dir() : d)
                                .getChildFile ("AudioSR-GGUF");
    if (!root.isDirectory()) return out;
    for (auto entry : juce::RangedDirectoryIterator (root, false))
        if (entry.getFile().hasFileExtension ("gguf"))
            out.push_back (entry.getFile());
    std::sort (out.begin (), out.end (),
               [] (const juce::File& a, const juce::File& b) {
                   return a.getFileName().toLowerCase() < b.getFileName().toLowerCase(); });
    return out;
}

std::vector<SuperResModelSpec> flashSrChoices()
{
    // The FlashSR weights ship as an asset of the audio.cpp repository itself.
    // SHA-256 computed 2026-11-05 from the file at the pinned commit below.
    std::vector<SuperResModelSpec> v;
    v.push_back ({ "flashsr/flashsr.safetensors", 356008,
                   "cfa15eb8576e50cff30bc9851fc2ca8e74b6ea379fbcded03c9cc44458b1c6e2" });
    return v;
}

juce::String flashSrUrl(const juce::String& fileName)
{
    // Pinned to the commit that added the file (38156353, 2026-06-25) rather
    // than "main": the published SHA-256 must keep matching the bytes served.
    return "https://github.com/0xShug0/audio.cpp/raw/38156353396fae68d10388c960b900a8fda8373f"
           "/assets/framework/audio_utilities/" + fileName;
}

std::vector<SuperResModelSpec> audioSRChoices()
{
    // AudioSR GGUF package on Hugging Face (audio-cpp/audio.cpp-gguf).
    // SHA-256 = the Git LFS object id published by Hugging Face, confirmed
    // by downloading and hashing the file (2026-11-05).
    std::vector<SuperResModelSpec> v;
    v.push_back ({ "AudioSR-GGUF/audiosr-basic-f32.gguf", 6177855616,
                   "bf92d26c638e82f5051dfd8cf5899798d64ce5366ff63bad8b5930a82fce5bff" });
    return v;
}

juce::String audioSRUrl(const juce::String& fileName)
{
    return "https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/main/" + fileName;
}

std::vector<BinarySpec> binaryChoices()
{
    // Windows x64 assets of the v0.9.2-drama3-base-small release. Zip
    // SHA-256 computed 2026-11-05 from the downloaded assets; they equal the
    // "digest" GitHub publishes for each asset. exeSha256 = audiocpp_cli.exe
    // extracted from that zip.
    const char* base = "https://github.com/0xShug0/audio.cpp/releases/download/v0.9.2-drama3-base-small/";
    // The CUDA executables import ggml.dll / ggml-base.dll shipped next to
    // them in the zip, but installBinaryFromZip extracts audiocpp_cli.exe only.
    const std::string cudaBlocker =
        "the installer extracts audiocpp_cli.exe only, but this variant's executable imports "
        "ggml.dll and ggml-base.dll from the same zip";
    std::vector<BinarySpec> v;
    BinarySpec e1 { "cpu",      juce::String ("CPU (portable)"),
                    std::string (base) + "audio-v0.9.2-drama3-base-small-bin-windows-x64-cpu-portable.zip", 28786884,
                    "9654c8f66f87bda768196d934059be896399b43703b06e5f2b857430775bec3b",
                    "427875df84983e950103caa78c435e2dd3cef4b99dc0295c4f941e49598fda06", 27326464, {} };
    BinarySpec e2 { "cuda12.4", juce::String ("CUDA 12.4"),
                    std::string (base) + "audio-v0.9.2-drama3-base-small-bin-windows-x64-cuda12.4.zip", 464268606,
                    "48852da40790c40f3285bca6c53ff7829b3963080f0e95b6742f868fc1d7c757",
                    "ee1ae0b3dce0918f33dd9446e55c21f50120bcf4501a98a9a9a1ef4b7eaa91a9", 26529280, cudaBlocker };
    BinarySpec e3 { "cuda13.3", juce::String ("CUDA 13.3"),
                    std::string (base) + "audio-v0.9.2-drama3-base-small-bin-windows-x64-cuda13.3.zip", 275837338,
                    "b570efa4b17c2aa32c431c653489c009b58f57f60d68782b3b94b6c06304a9e8",
                    "baf36544d8734099b5fb22c3284f5a0c4d5dba0272f36e8180a16d77bda2e7c9", 26537472, cudaBlocker };
    BinarySpec e4 { "vulkan",   juce::String ("Vulkan (portable)"),
                    std::string (base) + "audio-v0.9.2-drama3-base-small-bin-windows-x64-vulkan-portable.zip", 63227361,
                    "9c943d406af3543cc5005d158ed583210cd7a13371bffcd4b7f262f7a6a5ad16",
                    "373e76168f317f7b8f63bf0ccdf1f2048c01cc995fdcdd507a17264d3d4e09a4", 84764672, {} };
    v.push_back (e1); v.push_back (e2); v.push_back (e3); v.push_back (e4);
    return v;
}

std::vector<ModelSpec> modelChoices()
{
    // audio-cpp/SAM-Audio-GGUF on Hugging Face. SHA-256 = the Git LFS object
    // id published by Hugging Face, confirmed by downloading and hashing each
    // file (2026-11-05).
    std::vector<ModelSpec> v;
    ModelSpec m1 { "sam-audio-small-q8_0.gguf", 2379290272,
                   "82e7aaf7b8c458f67183a4924950dbb10ad1979a9c057f1a906c79752a4ba243" };
    ModelSpec m2 { "sam-audio-small-bf16.gguf", 2824105568,
                   "48e51ad99e08eb976cbcbf65e1dd725b0e76b00618fd857a05dc7b7dadceb46b" };
    ModelSpec m3 { "sam-audio-small-f32.gguf", 5640890208,
                   "ec80610e076c7408dcc14ba8c7bc6c321f8d691bcf093e94882898a9dbaf34f1" };
    ModelSpec m4 { "sam-audio-base-q8_0.gguf", 3095951072,
                   "e5529e940218981b67be2c58904ffca7b7eecba20b65e2e99e7d23eb5eb7bd9b" };
    ModelSpec m5 { "sam-audio-base-bf16.gguf", 4136791712,
                   "90f658f0043859e195ef08a298dd61d1747bc65245c0768f2d0863f3e42849b2" };
    ModelSpec m6 { "sam-audio-base-f32.gguf", 8265738720,
                   "34b1fec64c1e07027e7b30feb744f59cd00bc10a02daa8f86140f0fe054242cc" };
    ModelSpec m7 { "sam-audio-large-q8_0.gguf", 5029984064,
                   "2ff2d4599da19353105156582f995aa433899450c27e0274f0e45cc51369abf3" };
    ModelSpec m8 { "sam-audio-large-bf16.gguf", 7705276160,
                   "d3582311e5a43ae1e067b3a36c2c85a5dea946757114c611892a0fdb3e67b3ee" };
    ModelSpec m9 { "sam-audio-large-f32.gguf", 15401675328,
                   "bdfe2614aca0a85eebf2304959f9783125655ecb9475e56cd180f4551d32043a" };
    v.push_back (m1); v.push_back (m2); v.push_back (m3);
    v.push_back (m4); v.push_back (m5); v.push_back (m6);
    v.push_back (m7); v.push_back (m8); v.push_back (m9);
    return v;
}

juce::String modelUrl(const juce::String& fileName)
{
    return "https://huggingface.co/audio-cpp/SAM-Audio-GGUF/resolve/main/" + fileName;
}

namespace {
struct KnownAsset { std::string url, sha256; int64_t sizeBytes = 0; };

std::vector<KnownAsset> knownAssets()
{
    std::vector<KnownAsset> v;
    for (const auto& b : binaryChoices ())
        v.push_back ({ b.url, b.sha256, b.sizeBytes });
    for (const auto& m : modelChoices ())
        v.push_back ({ modelUrl (m.file).toStdString (), m.sha256, m.sizeBytes });
    for (const auto& m : flashSrChoices ())
        v.push_back ({ flashSrUrl (m.file).toStdString (), m.sha256, m.sizeBytes });
    for (const auto& m : audioSRChoices ())
        v.push_back ({ audioSRUrl (m.file).toStdString (), m.sha256, m.sizeBytes });
    return v;
}
} // namespace

std::string knownSha256ForUrl(const std::string& url)
{
    for (const auto& a : knownAssets ())
        if (a.url == url) return a.sha256;
    return {};
}

int64_t knownSizeForUrl(const std::string& url)
{
    for (const auto& a : knownAssets ())
        if (a.url == url) return a.sizeBytes;
    return 0;
}

static const juce::File variantMarkerFile(const juce::File& d)
{
    return (d.getFullPathName().isEmpty() ? dir() : d).getChildFile ("variant.txt");
}

juce::String installedVariant(const juce::File& d)
{
    const juce::File f = variantMarkerFile (d);
    if (!f.existsAsFile()) return {};
    return f.loadFileAsString().trim();
}

void setInstalledVariant(const juce::String& variantId, const juce::File& d)
{
    variantMarkerFile (d).replaceWithText (variantId);
}

// ---------------------------------------------------------------------------
// Downloads.
// ---------------------------------------------------------------------------

int64_t parseContentLength(const char* text)
{
    if (text == nullptr) return 0;
    while (*text == ' ' || *text == '\t') ++text;
    int64_t v = 0;
    bool any = false;
    for (; *text >= '0' && *text <= '9'; ++text)
    {
        if (v > (INT64_MAX - 9) / 10) return 0;   // absurd value: treat as unknown
        v = v * 10 + (int64_t) (*text - '0');
        any = true;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    return (any && *text == '\0') ? v : 0;
}

juce::String validateDownload(int httpStatus, int64_t contentLength, int64_t expectedBytes,
                              int64_t received, bool transferError)
{
    if (httpStatus != 0 && (httpStatus < 200 || httpStatus > 299))
        return "download failed: HTTP " + juce::String (httpStatus)
               + " (no internet access, a proxy or a captive portal?). Install the file manually.";
    if (transferError)
        return "The network transfer was interrupted after " + juce::String (received)
               + " bytes. Please retry the download.";
    if (received <= 0)
        return "The server returned an empty file.";
    if (contentLength > 0 && received != contentLength)
        return "Incomplete download (" + juce::String (received) + " of "
               + juce::String (contentLength) + " bytes). Please retry the download.";
    if (expectedBytes > 0 && received != expectedBytes)
        return "Unexpected download size (" + juce::String (received) + " bytes instead of "
               + juce::String (expectedBytes) + "): the server probably returned an error page "
               "(proxy / captive portal) instead of the file. Install the file manually.";
    return {};
}

juce::String validateSha256(const std::string& expectedSha256, const std::string& actualSha256)
{
    if (expectedSha256.empty ())
        return {};
    if (juce::String (actualSha256).equalsIgnoreCase (juce::String (expectedSha256)))
        return {};
    return "SHA-256 mismatch: the downloaded file does not match the published checksum "
           "(corrupted transfer, or the file changed on the server). It was not installed.";
}

std::string sha256OfFile(const juce::File& f, const std::function<bool(float)>& onProgress)
{
    juce::FileInputStream in (f);
    if (in.failedToOpen ())
        return {};
    const int64_t total = in.getTotalLength ();
    sc::ai::Sha256 digest;
    // On the heap: worker threads may have small stacks (macOS).
    constexpr int bufSize = 1 << 18;
    std::unique_ptr<char[]> buf (new char[(size_t) bufSize]);
    int64_t done = 0;
    for (;;)
    {
        const int n = in.read (buf.get (), bufSize);
        if (n <= 0) break;
        digest.update ((const uint8_t*) buf.get (), (size_t) n);
        done += n;
        if (onProgress && !onProgress (total > 0 ? (float) ((double) done / (double) total) : 1.0f))
            return {};
    }
    if (total >= 0 && done != total)
        return {};   // read error part-way
    return digest.hexDigest ();
}

bool commitDownload(const juce::File& part, const juce::File& dest,
                    int httpStatus, int64_t contentLength, int64_t expectedBytes,
                    int64_t received, bool transferError, std::string* errOut,
                    const std::string& expectedSha256, const std::string& actualSha256)
{
    juce::String problem = validateDownload (httpStatus, contentLength, expectedBytes,
                                             received, transferError);
    if (problem.isEmpty ())
        problem = validateSha256 (expectedSha256, actualSha256);
    // The bytes that actually reached the disk must match what was received
    // (a full disk makes buffered writes fail silently).
    if (problem.isEmpty () && part.getSize () != received)
        problem = "Could not write the whole file to the models folder (disk full?).";

    if (problem.isNotEmpty ())
    {
        part.deleteFile ();   // never leave a partial / invalid file behind
        if (errOut) *errOut = problem.toStdString ();
        return false;
    }

    // The stream is closed by now, so renaming cannot hit a sharing violation.
    dest.deleteFile ();
    if (!part.moveFileTo (dest))
    {
        part.deleteFile ();
        if (errOut) *errOut = "Could not move the downloaded file into place.";
        return false;
    }
    return true;
}

#if JUCE_WINDOWS

bool downloadFile(const std::string& url, const juce::File& dest,
                  const std::function<bool(float)>& onProgress,
                  std::string* errOut, int64_t expectedBytes,
                  const std::string& expectedSha256In)
{
    // Every catalogue URL has a published SHA-256: verify it even when the
    // caller (the setup dialogs) passes none.
    const std::string expectedSha256 = expectedSha256In.empty () ? knownSha256ForUrl (url)
                                                                 : expectedSha256In;
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

    DWORD flags = (url.rfind ("https://", 0) == 0 ? INTERNET_FLAG_SECURE : 0);
    // Multi-GB files: never serve them from / copy them into the WinINet cache.
#ifdef INTERNET_FLAG_RELOAD
    flags |= INTERNET_FLAG_RELOAD;
#endif
#ifdef INTERNET_FLAG_NO_CACHE_WRITE
    flags |= INTERNET_FLAG_NO_CACHE_WRITE;
#endif
    HINTERNET hRequest = InternetOpenUrlA (hInternet, url.c_str (), nullptr, 0, flags, 0);
    if (!hRequest)
    {
        const DWORD e = GetLastError ();
        if (errOut) *errOut = "HTTP request failed (error " + std::to_string ((int) e) + ").";
        InternetCloseHandle (hInternet);
        return false;
    }

    // Final status code (redirects are followed by WinINet). 0 = unknown.
    DWORD status = 0, statusSize = sizeof (status);
    if (!HttpQueryInfoA (hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                         &status, &statusSize, nullptr))
        status = 0;

    // Content-Length is read as text: the numeric form is a 32-bit DWORD and
    // the model files exceed 4 GB. 0 = unknown (chunked responses).
    int64_t contentLength = 0;
    {
        char lenText[64] = {};
        DWORD lenSize = sizeof (lenText) - 1;
        if (HttpQueryInfoA (hRequest, HTTP_QUERY_CONTENT_LENGTH, lenText, &lenSize, nullptr))
            contentLength = parseContentLength (lenText);
    }
    const int64_t totalBytes = contentLength > 0 ? contentLength : expectedBytes;

    dest.getParentDirectory ().createDirectory ();   // e.g. models/audiocpp/flashsr
    const juce::File part = dest.getSiblingFile (dest.getFileName () + ".part");
    part.deleteFile ();   // FileOutputStream appends to an existing file
    bool ok = false, cancelled = false, openFailed = false, transferError = false;
    int64_t got = 0;
    sc::ai::Sha256 digest;
    if (status != 0 && (status < 200 || status > 299))
    {
        // Error page: do not save its body, commitDownload reports the code.
    }
    else
    {
        juce::FileOutputStream out (part);
        if (!out.failedToOpen ())
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
                    break;   // the on-disk size check in commitDownload reports it
                digest.update ((const uint8_t*) buf, (size_t) nRead);
                got += (int64_t) nRead;
                float frac = totalBytes > 0 ? (float) ((double) got / (double) totalBytes) : 1.0f;
                if (frac > 1.0f) frac = 1.0f;
                if (onProgress && !onProgress (frac))
                    cancelled = true;
            }
            out.flush ();
        }
        else openFailed = true;
    }

    if (openFailed)
    {
        if (errOut) *errOut = "Could not write to the models folder.";
    }
    else if (!cancelled)
    {
        ok = commitDownload (part, dest, (int) status, contentLength, expectedBytes,
                             got, transferError, errOut, expectedSha256,
                             expectedSha256.empty () ? std::string () : digest.hexDigest ());
    }

    if (!ok) part.deleteFile ();   // leave no partial file behind
    InternetCloseHandle (hRequest);
    InternetCloseHandle (hInternet);
    return ok;
}

#else   // non-Windows: manual download only.

bool downloadFile(const std::string&, const juce::File&,
                  const std::function<bool(float)>&, std::string* errOut, int64_t,
                  const std::string&)
{
    if (errOut) *errOut = "Automatic downloads are only supported on Windows in v1.";
    return false;
}

#endif

bool installBinaryFromZip(const juce::File& zipPath, const juce::String& variantId,
                          const juce::File& d, std::string* errOut)
{
    const juce::File root = (d.getFullPathName().isEmpty() ? dir() : d);
    if (!zipPath.existsAsFile())
    {
        if (errOut) *errOut = "The downloaded zip file is missing.";
        return false;
    }

    juce::ZipFile zip (zipPath);
    const int idx = zip.getIndexOfFileName ("audiocpp_cli.exe");
    if (idx < 0)
    {
        if (errOut) *errOut = "audiocpp_cli.exe was not found inside the downloaded archive.";
        return false;
    }

    // Extracted into a scratch folder first, so a bad archive never replaces
    // a working binary. The entry sits at the archive root.
    const juce::File scratch = root.getChildFile ("audiocpp_extract.tmp");
    scratch.deleteRecursively ();
    scratch.createDirectory ();
    const juce::Result r = zip.uncompressEntry (idx, scratch);
    const juce::File extracted = scratch.getChildFile ("audiocpp_cli.exe");
    if (!r.wasOk () || !extracted.existsAsFile ())
    {
        juce::String msg ("Could not extract audiocpp_cli.exe: ");
        msg += r.wasOk () ? juce::String ("the file is missing after extraction") : r.getErrorMessage ();
        scratch.deleteRecursively ();
        if (errOut) *errOut = msg.toStdString ();
        return false;
    }

    // The zip was verified when downloaded; check the extracted executable too
    // (a zip placed by hand, or a mismatching variant id).
    for (const auto& b : binaryChoices ())
    {
        if (b.id != variantId.toStdString () || b.exeSha256.empty ())
            continue;
        if (! juce::String (sha256OfFile (extracted)).equalsIgnoreCase (juce::String (b.exeSha256)))
        {
            scratch.deleteRecursively ();
            if (errOut) *errOut = "The extracted audiocpp_cli.exe does not match the published checksum "
                                  "for the " + b.id + " variant; the installed binary was left unchanged.";
            return false;
        }
    }

    const juce::File exe = binaryPath (root);
    exe.deleteFile ();
    const bool moved = extracted.moveFileTo (exe);
    scratch.deleteRecursively ();
    if (!moved)
    {
        if (errOut) *errOut = "Could not move audiocpp_cli.exe into place (is it running?).";
        return false;
    }

    setInstalledVariant (variantId, root);
    zipPath.deleteFile ();   // the archive is only needed once
    return hasBinary (root);
}

// ---------------------------------------------------------------------------
// CLI invocation.
// ---------------------------------------------------------------------------

juce::String formatAnchorTime(double secs)
{
    if (!std::isfinite (secs)) secs = 0.0;
    juce::String s = juce::String (secs, 4);   // up to 4 decimals
    while (s.endsWith ("0")) s = s.dropLastCharacters (1);
    if (s.endsWith (".")) s = s.dropLastCharacters (1);
    return s;
}

juce::String buildAnchorsOption(const std::vector<Anchor>& anchors)
{
    juce::String s = "anchors=[";
    for (size_t i = 0; i < anchors.size (); ++i)
    {
        if (i > 0) s += ",";
        const Anchor& a = anchors[i];
        s += "[\"" + juce::String (a.include ? "+" : "-") + "\","
             + formatAnchorTime (a.startSecs) + ","
             + formatAnchorTime (a.stopSecs) + "]";
    }
    return s + "]";
}

std::vector<float> downmixToMono(const std::vector<std::vector<float>>& in)
{
    // Plain comparison: this file includes <windows.h> without NOMINATE,
    // where `max` is a macro and would break std::max.
    int64_t len = 0;
    for (const auto& c : in)
        if ((int64_t) c.size () > len) len = (int64_t) c.size ();
    if (in.empty () || len <= 0) return {};

    std::vector<float> out ((size_t) len, 0.0f);
    const double invN = 1.0 / (double) in.size ();
    for (const auto& c : in)
        for (int64_t i = 0; i < (int64_t) c.size (); ++i)
            out[(size_t) i] += (float) ((double) c[(size_t) i] * invN);
    return out;
}

juce::StringArray buildCliArgs(const juce::File& model, const juce::String& backend,
                               const juce::File& audioIn, const juce::String& text,
                               const std::vector<Anchor>& anchors,
                               const juce::File& outDir)
{
    juce::StringArray args;
    args.add ("--task");            args.add ("s2s");
    args.add ("--family");          args.add ("sam_audio");
    args.add ("--model");           args.add (model.getFullPathName());
    args.add ("--backend");         args.add (backend);
    args.add ("--audio");           args.add (audioIn.getFullPathName());
    args.add ("--text");            args.add (text);
    if (!anchors.empty ())
    {
        args.add ("--request-option");
        args.add (buildAnchorsOption (anchors));
    }
    args.add ("--out-dir");         args.add (outDir.getFullPathName());
    args.add ("--log");
    return args;
}

juce::StringArray buildSepCliArgs(const juce::String& family, const juce::File& model,
                                  const juce::String& backend, const juce::File& audioIn,
                                  const juce::File& outDir)
{
    juce::StringArray args;
    args.add ("--task");            args.add ("sep");
    args.add ("--family");          args.add (family);
    args.add ("--model");           args.add (model.getFullPathName());
    args.add ("--backend");         args.add (backend);
    args.add ("--audio");           args.add (audioIn.getFullPathName());
    args.add ("--out-dir");         args.add (outDir.getFullPathName());
    args.add ("--log");
    return args;
}

#if JUCE_WINDOWS

// Quote one argument for a CreateProcessW command line. Backslashes are only
// special immediately before a quote, so runs of backslashes preceding a
// quote must be doubled and the quote escaped with a single backslash - this
// is what makes "anchors=[[\"-\",0.1,0.5]]" reach the tool intact.
static std::wstring acppQuote(const juce::String& arg)
{
    const std::wstring w = arg.toWideCharPointer ();
    bool needsQuotes = false;
    for (wchar_t c : w)
        if (c == L' ' || c == L'\t' || c == L'"') { needsQuotes = true; break; }
    if (!w.empty () && !needsQuotes) return w;

    std::wstring q = L"\"";
    size_t i = 0;
    while (i < w.size ())
    {
        size_t nBack = 0;
        while (i + nBack < w.size () && w[i + nBack] == L'\\') ++nBack;
        if (i + nBack < w.size () && w[i + nBack] == L'"')
        {
            q.append (nBack * 2 + 1, L'\\');   // double each backslash...
            q += L'"';                        // ...and escape the quote
        }
        else
        {
            q.append (nBack, L'\\');          // literal backslashes pass through
            if (i + nBack < w.size ()) q += w[i + nBack];
        }
        i += nBack + 1;
    }
    return q + L"\"";
}

// Creates and polls one audiocpp_cli.exe process with piped stdout/stderr.
static bool runProcessWithPipes(const juce::File& binary, const juce::StringArray& args,
                                const juce::File& outDirToCreate,
                                const std::function<bool()>& shouldContinue,
                                const std::function<void(const juce::String&)>& onLogLine,
                                RunResult* resOut, std::string* errOut)
{
    if (!binary.existsAsFile ())
    {
        if (errOut) *errOut = "The audiocpp_cli.exe binary is missing.";
        return false;
    }

    outDirToCreate.createDirectory ();

    juce::String cmdLine (acppQuote (binary.getFullPathName()).c_str ());
    for (const auto& a : args)
        cmdLine << ' ' << acppQuote (a).c_str ();

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof (sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE outR = INVALID_HANDLE_VALUE, outW = INVALID_HANDLE_VALUE;
    HANDLE errR = INVALID_HANDLE_VALUE, errW = INVALID_HANDLE_VALUE;
    if (!CreatePipe (&outR, &outW, &sa, 1 << 20) || !CreatePipe (&errR, &errW, &sa, 1 << 20))
    {
        if (errOut) *errOut = "Could not create the process pipes.";
        return false;
    }

    // Only the write ends belong to the child; our read ends must not be inherited.
    SetHandleInformation (outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation (errR, HANDLE_FLAG_INHERIT, 0);

    // stdin = NUL (closed for the child): the CLI never waits on, or
    // consumes, whatever stdin SpectroCraft itself was started with.
    HANDLE nulIn = CreateFileW (L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si {};
    si.cb = sizeof (si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outW;
    si.hStdError  = errW;
    si.hStdInput  = nulIn != INVALID_HANDLE_VALUE ? nulIn : nullptr;

    PROCESS_INFORMATION pi {};
    // CreateProcessW may modify the command line buffer, so it needs a mutable copy.
    std::wstring cmdW = cmdLine.toWideCharPointer ();
    // CREATE_NO_WINDOW: the CLI is a console app; without this flag Windows
    // pops up a black console window over the UI for every run. All output
    // is captured through the pipes above, so nothing is lost.
    const BOOL created = CreateProcessW (nullptr, cmdW.data (), nullptr, nullptr,
                                         TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle (outW);
    CloseHandle (errW);
    if (nulIn != INVALID_HANDLE_VALUE)
        CloseHandle (nulIn);
    if (!created)
    {
        CloseHandle (outR);
        CloseHandle (errR);
        if (errOut) *errOut = "Could not start audiocpp_cli.exe.";
        return false;
    }

    // Non-blocking drain: PeekNamedPipe tells us how much is available without
    // stalling the poll loop, so log lines reach the UI while it runs.
    auto drainLines = [&] (HANDLE h, std::string& buf,
                           const std::function<void(const juce::String&)>& cb) {
        char tmp[1 << 16];
        for (;;)
        {
            DWORD avail = 0;
            if (!PeekNamedPipe (h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
            DWORD got = 0;
            if (!ReadFile (h, tmp, sizeof (tmp), &got, nullptr) || got == 0) break;
            buf.append (tmp, got);
            size_t pos;
            while ((pos = buf.find ('\n')) != std::string::npos)
            {
                const juce::String line = juce::String (buf.substr (0, pos)).trimCharactersAtEnd ("\r");
                if (!line.isEmpty () && cb) cb (line);
                buf.erase (0, pos + 1);
            }
        }
    };

    std::string outBuf, errBuf;
    bool killed = false;
    for (;;)
    {
        const DWORD w = WaitForSingleObject (pi.hProcess, 200);
        if (w == WAIT_OBJECT_0) break;
        drainLines (outR, outBuf, onLogLine);
        drainLines (errR, errBuf, nullptr);
        if (shouldContinue != nullptr && !shouldContinue ())
        {
            TerminateProcess (pi.hProcess, 1);
            killed = true;
            WaitForSingleObject (pi.hProcess, 5000);
            break;
        }
    }

    // Final drain after the write ends close.
    drainLines (outR, outBuf, onLogLine);
    drainLines (errR, errBuf, nullptr);

    int rc = -1;
    if (!killed) GetExitCodeProcess (pi.hProcess, (DWORD*) &rc);

    CloseHandle (outR);
    CloseHandle (errR);
    CloseHandle (pi.hProcess);
    CloseHandle (pi.hThread);

    if (resOut != nullptr)
    {
        resOut->exitCode = rc;
        resOut->stdoutText = juce::String (outBuf);
        resOut->stderrText = juce::String (errBuf);
        resOut->cancelled = killed;
    }
    if (killed)
    {
        if (errOut) *errOut = "Cancelled.";
        return false;
    }
    return rc == 0;
}

bool runStemSeparation(const juce::File& binary, const juce::File& model,
                       const juce::String& backend, const juce::File& audioIn,
                       const juce::String& text, const std::vector<Anchor>& anchors,
                       const juce::File& outDir,
                       const std::function<bool()>& shouldContinue,
                       const std::function<void(const juce::String&)>& onLogLine,
                       RunResult* resOut, std::string* errOut)
{
    return runProcessWithPipes (binary,
                                buildCliArgs (model, backend, audioIn, text, anchors, outDir),
                                outDir, shouldContinue, onLogLine, resOut, errOut);
}

bool runSepTask(const juce::File& binary, const juce::String& family, const juce::File& model,
                const juce::String& backend, const juce::File& audioIn, const juce::File& outDir,
                const std::function<bool()>& shouldContinue,
                const std::function<void(const juce::String&)>& onLogLine,
                RunResult* resOut, std::string* errOut)
{
    return runProcessWithPipes (binary,
                                buildSepCliArgs (family, model, backend, audioIn, outDir),
                                outDir, shouldContinue, onLogLine, resOut, errOut);
}

juce::StringArray buildSuperResCliArgs(int mode, const juce::File& model,
                                       const juce::String& backend,
                                       const juce::File& audioIn, const juce::File& outWav)
{
    juce::StringArray args;
    args.add ("--task");            args.add ("s2s");
    if (mode == 1)   // FlashSR: a builtin_audio_utils utility
    {
        args.add ("--family");          args.add ("builtin_audio_utils");
        args.add ("--model");           args.add (model.getFullPathName());
        args.add ("--load-option");     args.add ("utility=flashsr");
    }
    else   // AudioSR
    {
        args.add ("--family");          args.add ("audiosr");
        args.add ("--model");           args.add (model.getFullPathName());
    }
    args.add ("--backend");         args.add (backend);
    args.add ("--audio");           args.add (audioIn.getFullPathName());
    args.add ("--out");             args.add (outWav.getFullPathName());
    args.add ("--log");
    return args;
}

bool runSuperResTask(const juce::File& binary, int mode, const juce::File& model,
                     const juce::String& backend, const juce::File& audioIn,
                     const juce::File& outWav,
                     const std::function<bool()>& shouldContinue,
                     const std::function<void(const juce::String&)>& onLogLine,
                     RunResult* resOut, std::string* errOut)
{
    // The tool writes the result straight to --out (no output directory), so
    // hand it the existing parent folder as the "directory to ensure".
    return runProcessWithPipes (binary,
                                buildSuperResCliArgs (mode, model, backend, audioIn, outWav),
                                outWav.getParentDirectory (), shouldContinue, onLogLine,
                                resOut, errOut);
}

#else   // POSIX: not supported in v1.

bool runStemSeparation(const juce::File&, const juce::File&, const juce::String&,
                       const juce::File&, const juce::String&, const std::vector<Anchor>&,
                       const juce::File&, const std::function<bool()>&,
                       const std::function<void(const juce::String&)>&,
                       RunResult*, std::string* errOut)
{
    if (errOut) *errOut = "Stem Separation requires the Windows CLI binary.";
    return false;
}

bool runSepTask(const juce::File&, const juce::String&, const juce::File&,
                const juce::String&, const juce::File&, const juce::File&,
                const std::function<bool()>&, const std::function<void(const juce::String&)>&,
                RunResult*, std::string* errOut)
{
    if (errOut) *errOut = "Stem Separation requires the Windows CLI binary.";
    return false;
}

juce::StringArray buildSuperResCliArgs(int, const juce::File&, const juce::String&,
                                       const juce::File&, const juce::File&)
{
    return {};
}

bool runSuperResTask(const juce::File&, int, const juce::File&, const juce::String&,
                     const juce::File&, const juce::File&,
                     const std::function<bool()>&, const std::function<void(const juce::String&)>&,
                     RunResult*, std::string* errOut)
{
    if (errOut) *errOut = "Super Resolution requires the Windows CLI binary.";
    return false;
}

#endif

}} // namespace sc::acpp
