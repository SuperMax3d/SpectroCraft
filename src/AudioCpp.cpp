// SpectroCraft - audio.cpp stem-separation support (external CLI tool).
#include "AudioCpp.h"
#include "AI/Sha256.h"
#include "Resampler.h"

#include <cstdint>
#include <cstdlib>   // std::strtol
#include <cstring>
#include <map>
#include <memory>

#if JUCE_WINDOWS
#  include <windows.h>
#  include <wininet.h>
#else
#  include "PythonScripts.h"   // sc::py::runProcessEx (process group, line callback)
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

juce::String binaryFileName()
{
#if JUCE_WINDOWS
    return "audiocpp_cli.exe";
#else
    return "audiocpp_cli";
#endif
}

juce::File binaryPath(const juce::File& d)
{
    return (d.getFullPathName().isEmpty() ? dir() : d).getChildFile (binaryFileName ());
}

bool hasBinary(const juce::File& d)
{
    const juce::File b = binaryPath (d);
    return b.existsAsFile () && b.getSize () > 0;
}

std::string platformTag()
{
#if JUCE_WINDOWS
    return "windows-x64";   // the release ships x64 executables only
#elif JUCE_MAC
#  if defined(__aarch64__) || defined(__arm64__)
    return "macos-arm64";
#  else
    return "macos-x64";
#  endif
#else
#  if defined(__x86_64__)
    return "linux-x64";
#  elif defined(__aarch64__)
    return "linux-arm64";
#  else
    return "linux-other";
#  endif
#endif
}

bool platformSupported()
{
    return !binaryChoices ().empty ();
}

std::vector<std::string> supportedPlatformTags()
{
    return { "windows-x64", "macos-arm64", "macos-x64", "linux-x64" };
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
    const char* base = "https://github.com/0xShug0/audio.cpp/releases/download/v0.9.2-drama3-base-small/";
    std::vector<BinarySpec> v;
#if JUCE_WINDOWS
    // Windows x64 assets of the v0.9.2-drama3-base-small release. Zip
    // SHA-256 computed 2026-11-05 from the downloaded assets; they equal the
    // "digest" GitHub publishes for each asset. exeSha256 = audiocpp_cli.exe
    // extracted from that zip.
    // The CUDA executables import ggml.dll / ggml-base.dll shipped next to
    // them in the zip, but installBinaryFromArchive extracts only
    // audiocpp_cli.exe from a zip.
    const std::string cudaBlocker =
        "the installer extracts audiocpp_cli.exe only, but this variant's executable imports "
        "ggml.dll and ggml-base.dll from the same zip";
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
#elif JUCE_MAC
    // macOS assets of the same release (.tar.gz). Archive SHA-256 = the
    // "digest" GitHub publishes, confirmed by hashing the downloads
    // (2026-11-06); exeSha256 = audiocpp_cli extracted from it. The
    // executable is self-contained (system frameworks only, ad-hoc signed)
    // and has the cpu and metal backends.
#  if defined(__aarch64__) || defined(__arm64__)
    v.push_back ({ "metal", juce::String ("Metal (Apple silicon)"),
                   std::string (base) + "audio-v0.9.2-drama3-base-small-bin-macos-arm64-metal.tar.gz", 32708971,
                   "1281998c1acd170a7d92139853fe6ce6ab920e211d2ad3359775d43ad9ba616d",
                   "bb9f60716b78b2e59bd07c24cf3dea6bc90d82f07ac73d8da93b44da90fb46e6", 34348304, {} });
#  else
    v.push_back ({ "metal", juce::String ("Metal (Intel Mac)"),
                   std::string (base) + "audio-v0.9.2-drama3-base-small-bin-macos-x64-metal.tar.gz", 34649590,
                   "e4f1bca78339092c679bb51b90c895bdef6b2e6658eef467879f89c78a3941f0",
                   "7344fab599b00b41555dcfd256ac1e0b9ed0c7d93af760772bf2d6832729f861", 38056424, {} });
#  endif
#elif defined(__x86_64__)
    // Linux x64 assets of the same release (.tar.gz, hashes as for macOS).
    // audiocpp_cli loads libggml*.so from its own folder (RUNPATH $ORIGIN):
    // the installer extracts them next to it. It needs libgomp.so.1 (GCC's
    // OpenMP runtime) and, for vulkan, the system Vulkan loader.
    v.push_back ({ "cpu", juce::String ("CPU (portable)"),
                   std::string (base) + "audio-v0.9.2-drama3-base-small-bin-ubuntu-x64-cpu-portable.tar.gz", 61000844,
                   "d5e6110daae50800429680a277b70557d333d552c4a130a8f1d0e56181fa8697",
                   "d544d28f39f885aed0e11ff700a0fea7096dfe4e748d53f7b1229fef2d184cd9", 45906616, {} });
    v.push_back ({ "vulkan", juce::String ("Vulkan (portable)"),
                   std::string (base) + "audio-v0.9.2-drama3-base-small-bin-ubuntu-x64-vulkan-portable.tar.gz", 74761470,
                   "dfd0ad832829210eb13a03779bdd90f3e6e68f68edf762bf25b3915067beeb73",
                   "bac07cd7f28391299580a3177e9c5afa2e111c7ef627bc02667bd156bc26d133", 45906616, {} });
#else
    juce::ignoreUnused (base);   // no CLI build for this platform (e.g. Linux arm64)
#endif
    return v;
}

juce::String archiveFileName()
{
#if JUCE_WINDOWS
    return "audiocpp_bin.zip";
#else
    return "audiocpp_bin.tar.gz";
#endif
}

std::vector<BackendChoice> backendChoices()
{
    std::vector<BackendChoice> v { { "Cpu", "cpu" }, { "Cuda", "cuda" }, { "Vulkan", "vulkan" } };
#if JUCE_MAC
    v.push_back ({ "Metal", "metal" });   // appended: ids 1-3 keep their meaning
#endif
    return v;
}

juce::String backendForChoiceId(int id)
{
    const auto v = backendChoices ();
    return (id >= 1 && id <= (int) v.size ()) ? v[(size_t) id - 1].cli : juce::String ("cpu");
}

bool variantSupportsBackend(const juce::String& variant, const juce::String& backend)
{
    if (variant == "cpu")                          return backend == "cpu";
    if (variant.startsWith ("cuda"))               return backend == "cuda";
    if (variant == "vulkan")                       return backend == "vulkan";
    if (variant == "metal")                        return backend == "metal" || backend == "cpu";
    return true;   // hand-placed binary: unknown build
}

bool isKnownBackend(const juce::String& backend)
{
    return backend == "cpu" || backend == "cuda" || backend == "vulkan" || backend == "metal";
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

#else   // macOS / Linux: juce::WebInputStream (NSURLSession / libcurl)

bool downloadFile(const std::string& url, const juce::File& dest,
                  const std::function<bool(float)>& onProgress,
                  std::string* errOut, int64_t expectedBytes,
                  const std::string& expectedSha256In)
{
    // Same contract as the WinINet version above (and as
    // sc::ai::ModelManager's downloader on these platforms).
    const std::string expectedSha256 = expectedSha256In.empty () ? knownSha256ForUrl (url)
                                                                 : expectedSha256In;
    juce::WebInputStream in (juce::URL (juce::String (url)), false);
    // GitHub release assets and Hugging Face "resolve" URLs redirect to a
    // CDN. The timeout covers the connection and, through libcurl's
    // low-speed limit / NSURLSession's idle timeout, a stalled transfer.
    in.withConnectionTimeout (60000)
      .withNumRedirectsToFollow (10)
      .withExtraHeaders ("User-Agent: SpectroCraft");
    if (!in.connect (nullptr))
    {
        if (errOut) *errOut = "HTTP request failed (no network connection?).";
        return false;
    }

    const int status = in.getStatusCode ();   // final status, after the redirects
    int64_t contentLength = (int64_t) in.getTotalLength ();
    if (contentLength < 0) contentLength = 0;   // unknown (chunked)
    const int64_t totalBytes = contentLength > 0 ? contentLength : expectedBytes;

    dest.getParentDirectory ().createDirectory ();
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
            // On the heap: macOS worker threads have small stacks.
            constexpr int bufSize = 1 << 18;
            std::unique_ptr<char[]> buf (new char[(size_t) bufSize]);
            while (!cancelled)
            {
                const int nRead = in.read (buf.get (), bufSize);
                if (nRead < 0) { transferError = true; break; }
                if (nRead == 0)
                {
                    // End of the body, or a network error (libcurl reports it
                    // here; NSURLSession ends the stream early, which the
                    // size checks of commitDownload catch).
                    if (in.isError ()) transferError = true;
                    break;
                }
                if (!out.write (buf.get (), (size_t) nRead))
                    break;   // the on-disk size check in commitDownload reports it
                digest.update ((const uint8_t*) buf.get (), (size_t) nRead);
                got += (int64_t) nRead;
                float frac = totalBytes > 0 ? (float) ((double) got / (double) totalBytes) : 1.0f;
                if (frac > 1.0f) frac = 1.0f;
                if (onProgress && !onProgress (frac))
                    cancelled = true;
            }
            out.flush ();   // closed when this block ends, before the rename
        }
        else openFailed = true;
    }

    if (openFailed)
    {
        if (errOut) *errOut = "Could not write to the models folder.";
    }
    else if (!cancelled)
    {
        ok = commitDownload (part, dest, status, contentLength, expectedBytes,
                             got, transferError, errOut, expectedSha256,
                             expectedSha256.empty () ? std::string () : digest.hexDigest ());
    }
    if (!ok) part.deleteFile ();   // leave no partial file behind
    return ok;
}

#endif

namespace {

constexpr const char* kRuntimeManifest = "runtime-files.txt";

juce::File rootOr(const juce::File& d)
{
    return d.getFullPathName ().isEmpty () ? dir () : d;
}

// Libraries installed next to the executable by a .tar.gz install, as
// recorded in runtime-files.txt. Plain names only: a damaged manifest must
// never make removal delete anything outside the folder.
juce::StringArray installedRuntimeFiles(const juce::File& root)
{
    juce::StringArray names;
    const juce::File f = root.getChildFile (kRuntimeManifest);
    if (f.existsAsFile ())
        names.addLines (f.loadFileAsString ());
    names.trim ();
    names.removeEmptyStrings ();
    for (int i = names.size (); --i >= 0;)
        if (names[i].containsAnyOf ("/\\:") || names[i] == "." || names[i] == ".."
            || names[i] == binaryFileName ())
            names.remove (i);
    return names;
}

void deleteRuntimeFiles(const juce::File& root)
{
    for (const auto& n : installedRuntimeFiles (root))
        root.getChildFile (n).deleteFile ();
    root.getChildFile (kRuntimeManifest).deleteFile ();
}

bool isRuntimeLibraryName(const juce::String& n)
{
    return (n.startsWith ("lib") && n.contains (".so")) || n.endsWithIgnoreCase (".dylib");
}

// --- Minimal streaming tar reader (ustar, GNU and pax headers) --------------

// Octal ASCII field, or GNU base-256 when the high bit of the first byte is set.
uint64_t tarNumber(const unsigned char* p, size_t len, bool* ok)
{
    uint64_t v = 0;
    if (len > 0 && (p[0] & 0x80) != 0)
    {
        v = p[0] & 0x7f;
        for (size_t i = 1; i < len; ++i)
        {
            if (v > (UINT64_MAX >> 8)) { *ok = false; return 0; }
            v = (v << 8) | p[i];
        }
        return v;
    }
    size_t i = 0;
    while (i < len && p[i] == ' ') ++i;
    for (; i < len && p[i] >= '0' && p[i] <= '7'; ++i)
    {
        if (v > (UINT64_MAX >> 3)) { *ok = false; return 0; }
        v = (v << 3) | (uint64_t) (p[i] - '0');
    }
    if (i < len && p[i] != '\0' && p[i] != ' ')
        *ok = false;
    return v;
}

juce::String tarString(const unsigned char* p, size_t len)
{
    size_t n = 0;
    while (n < len && p[n] != 0) ++n;
    return juce::String::fromUTF8 ((const char*) p, (int) n);
}

// The header checksum (computed with the checksum field read as spaces):
// tells a tar archive from any other gzip content.
bool tarChecksumOk(const unsigned char* h)
{
    bool ok = true;
    const uint64_t stored = tarNumber (h + 148, 8, &ok);
    if (!ok) return false;
    uint64_t sum = 0;
    for (int i = 0; i < 512; ++i)
        sum += (i >= 148 && i < 156) ? (uint64_t) ' ' : (uint64_t) h[i];
    return sum == stored;
}

bool readFully(juce::InputStream& in, void* dst, size_t n)
{
    auto* p = (char*) dst;
    while (n > 0)
    {
        const int got = in.read (p, (int) std::min<size_t> (n, (size_t) 1 << 20));
        if (got <= 0) return false;
        p += got;
        n -= (size_t) got;
    }
    return true;
}

// Copies n bytes to out, or discards them when out is null.
bool copyBytes(juce::InputStream& in, juce::OutputStream* out, uint64_t n)
{
    constexpr int bufSize = 1 << 18;   // on the heap: small worker-thread stacks
    std::unique_ptr<char[]> buf (new char[(size_t) bufSize]);
    while (n > 0)
    {
        const int want = (int) std::min<uint64_t> (n, (uint64_t) bufSize);
        if (!readFully (in, buf.get (), (size_t) want)) return false;
        if (out != nullptr && !out->write (buf.get (), (size_t) want)) return false;
        n -= (uint64_t) want;
    }
    return true;
}

juce::String normaliseTarName(juce::String n)
{
    while (n.startsWith ("./")) n = n.substring (2);
    return n;
}

// "path" record of a pax extended header ("<len> key=value\n" records).
juce::String paxPath(const std::string& data)
{
    juce::String path;
    size_t pos = 0;
    while (pos < data.size ())
    {
        const size_t sp = data.find (' ', pos);
        if (sp == std::string::npos) break;
        const long len = std::strtol (data.c_str () + pos, nullptr, 10);
        if (len <= 0 || pos + (size_t) len > data.size () || sp >= pos + (size_t) len) break;
        std::string rec = data.substr (sp + 1, pos + (size_t) len - sp - 1);   // key=value\n
        if (!rec.empty () && rec.back () == '\n') rec.pop_back ();
        const size_t eq = rec.find ('=');
        if (eq != std::string::npos && rec.compare (0, eq, "path") == 0)
            path = juce::String::fromUTF8 (rec.data () + eq + 1, (int) (rec.size () - eq - 1));
        pos += (size_t) len;
    }
    return path;
}

} // namespace

std::vector<juce::String> extractRuntimeFromTarGz(const juce::File& archive, const juce::File& destDir,
                                                  std::string* errOut)
{
    auto fail = [errOut] (const juce::String& m)
    {
        if (errOut) *errOut = m.toStdString ();
        return std::vector<juce::String> {};
    };
    const juce::String truncated ("The downloaded archive is truncated or damaged. Please retry the download.");

    {
        juce::FileInputStream probe (archive);
        unsigned char m[2] = {};
        if (probe.failedToOpen () || probe.read (m, 2) != 2 || m[0] != 0x1f || m[1] != 0x8b)
            return fail ("The downloaded archive is not a .tar.gz file.");
    }
    juce::GZIPDecompressorInputStream in (new juce::FileInputStream (archive), true,
                                          juce::GZIPDecompressorInputStream::gzipFormat);
    if (!destDir.createDirectory ())
        return fail ("Could not create the extraction folder.");

    const juce::String exeName = binaryFileName ();
    juce::StringArray extracted;
    std::map<juce::String, juce::String> links;   // top-level symlink name -> target
    juce::String longName;                        // from a GNU 'L' or pax 'x' entry
    bool sawEnd = false;
    unsigned char h[512];
    while (readFully (in, h, sizeof (h)))
    {
        bool allZero = true;
        for (unsigned char c : h)
            if (c != 0) { allZero = false; break; }
        if (allZero) { sawEnd = true; break; }
        if (!tarChecksumOk (h))
            return fail (extracted.isEmpty () ? juce::String ("The downloaded archive is not a tar archive.")
                                              : truncated);

        bool ok = true;
        const uint64_t size = tarNumber (h + 124, 12, &ok);
        if (!ok) return fail (truncated);
        const uint64_t padding = ((size + 511) & ~(uint64_t) 511) - size;
        const char type = (char) h[156];

        if (type == 'L' || type == 'x')   // the next entry's name
        {
            if (size > ((uint64_t) 1 << 20)) return fail (truncated);
            std::string data ((size_t) size, '\0');
            if (!readFully (in, data.data (), data.size ()) || !copyBytes (in, nullptr, padding))
                return fail (truncated);
            if (type == 'L')
                longName = juce::String::fromUTF8 (data.c_str (), (int) std::strlen (data.c_str ()));
            else if (const juce::String p = paxPath (data); p.isNotEmpty ())
                longName = p;
            continue;
        }

        juce::String name;
        if (longName.isNotEmpty ())
            name = longName;
        else
        {
            name = tarString (h, 100);
            if (std::memcmp (h + 257, "ustar\0", 6) == 0)   // POSIX ustar: name prefix
                if (const juce::String prefix = tarString (h + 345, 155); prefix.isNotEmpty ())
                    name = prefix + "/" + name;
        }
        longName.clear ();
        name = normaliseTarName (name);
        const bool topLevel = name.isNotEmpty () && !name.containsAnyOf ("/\\:")
                              && name != "." && name != "..";
        const bool wanted = topLevel && (name == exeName || isRuntimeLibraryName (name));

        if (wanted && (type == '0' || type == '\0' || type == '7'))
        {
            const juce::File out = destDir.getChildFile (name);
            out.deleteFile ();
            bool wrote = false;
            {
                juce::FileOutputStream os (out);
                if (os.failedToOpen ())
                    return fail ("Could not write " + name + " to the models folder.");
                wrote = copyBytes (in, &os, size);
                os.flush ();
                wrote = wrote && os.getStatus ().wasOk ();
            }
            if (!wrote || out.getSize () != (juce::int64) size || !copyBytes (in, nullptr, padding))
                return fail (truncated + " (Or the disk is full.)");
            extracted.addIfNotAlreadyThere (name);
            continue;
        }
        if (wanted && type == '2')   // e.g. libggml.so.0 -> libggml.so.0.12.0
            links[name] = normaliseTarName (tarString (h + 157, 100));
        if (!copyBytes (in, nullptr, size + padding))
            return fail (truncated);
    }
    if (!sawEnd)
        return fail (truncated);

    // Symbolic links between extracted files become copies (the folder may
    // be on a file system without links; ld.so / dyld only need the names).
    for (bool progress = true; progress && !links.empty ();)
    {
        progress = false;
        for (auto it = links.begin (); it != links.end ();)
        {
            const juce::String& target = it->second;
            const juce::File src = destDir.getChildFile (target);
            if (target.isNotEmpty () && !target.containsAnyOf ("/\\:") && extracted.contains (target)
                && src.existsAsFile ())
            {
                const juce::File dst = destDir.getChildFile (it->first);
                dst.deleteFile ();
                if (!src.copyFileTo (dst))
                    return fail ("Could not write " + it->first + " to the models folder.");
                extracted.addIfNotAlreadyThere (it->first);
                it = links.erase (it);
                progress = true;
            }
            else
                ++it;
        }
    }

    if (!extracted.contains (exeName))
        return fail (exeName + " was not found inside the downloaded archive.");
    return std::vector<juce::String> (extracted.begin (), extracted.end ());
}

bool installBinaryFromArchive(const juce::File& archivePath, const juce::String& variantId,
                              const juce::File& d, std::string* errOut)
{
    const juce::File root = rootOr (d);
    const juce::String exeName = binaryFileName ();
    auto fail = [errOut] (const juce::String& m)
    {
        if (errOut) *errOut = m.toStdString ();
        return false;
    };
    if (!archivePath.existsAsFile ())
        return fail ("The downloaded archive is missing.");

    unsigned char magic[2] = {};
    {
        juce::FileInputStream probe (archivePath);
        if (!probe.failedToOpen ())
            probe.read (magic, 2);
    }
    const bool isZip = magic[0] == 'P' && magic[1] == 'K';
    const bool isTarGz = magic[0] == 0x1f && magic[1] == 0x8b;
    if (!isZip && !isTarGz)
        return fail ("The downloaded archive is neither a .zip nor a .tar.gz file.");

    // Extracted into a scratch folder first, so a bad archive never replaces
    // a working binary.
    const juce::File scratch = root.getChildFile ("audiocpp_extract.tmp");
    scratch.deleteRecursively ();
    scratch.createDirectory ();
    auto failClean = [&] (const juce::String& m)
    {
        scratch.deleteRecursively ();
        return fail (m);
    };

    juce::StringArray names;
    if (isZip)
    {
        // Windows builds: the executable sits at the archive root.
        juce::ZipFile zip (archivePath);
        const int idx = zip.getIndexOfFileName (exeName);
        if (idx < 0)
            return failClean (exeName + " was not found inside the downloaded archive.");
        const juce::Result r = zip.uncompressEntry (idx, scratch);
        if (!r.wasOk () || !scratch.getChildFile (exeName).existsAsFile ())
            return failClean ("Could not extract " + exeName + ": "
                              + (r.wasOk () ? juce::String ("the file is missing after extraction")
                                            : r.getErrorMessage ()));
        names.add (exeName);
    }
    else
    {
        std::string err;
        for (const auto& n : extractRuntimeFromTarGz (archivePath, scratch, &err))
            names.add (n);
        if (names.isEmpty ())
            return failClean (juce::String (err));
    }
    const juce::File extracted = scratch.getChildFile (exeName);

    // The archive was verified when downloaded; check the extracted
    // executable too (an archive placed by hand, or a mismatching variant id).
    for (const auto& b : binaryChoices ())
    {
        if (b.id != variantId.toStdString () || b.exeSha256.empty ())
            continue;
        if (! juce::String (sha256OfFile (extracted)).equalsIgnoreCase (juce::String (b.exeSha256)))
            return failClean ("The extracted " + exeName + " does not match the published checksum "
                              "for the " + juce::String (b.id) + " variant; the installed binary was left unchanged.");
    }

    // Replace the previous runtime: its libraries first, the executable last.
    juce::StringArray libs;
    for (const auto& n : names)
        if (n != exeName) libs.add (n);
    deleteRuntimeFiles (root);
    if (libs.size () > 0)
        root.getChildFile (kRuntimeManifest).replaceWithText (libs.joinIntoString ("\n") + "\n");
    for (const auto& n : libs)
    {
        const juce::File dst = root.getChildFile (n);
        dst.deleteFile ();
        if (!scratch.getChildFile (n).moveFileTo (dst))
            return failClean ("Could not move " + n + " into place.");
    }

    const juce::File exe = binaryPath (root);
    exe.deleteFile ();
    const bool moved = extracted.moveFileTo (exe);
    scratch.deleteRecursively ();
    if (!moved)
        return fail ("Could not move " + exeName + " into place (is it running?).");
#if !JUCE_WINDOWS
    if (!exe.setExecutePermission (true))
        return fail ("Could not make " + exeName + " executable.");
#endif

    setInstalledVariant (variantId, root);
    archivePath.deleteFile ();   // the archive is only needed once
    return hasBinary (root);
}

bool removeInstalledBinary(const juce::File& d)
{
    const juce::File root = rootOr (d);
    const juce::File exe = binaryPath (root);
    if (exe.exists () && !exe.deleteFile ())
        return false;
    deleteRuntimeFiles (root);
    variantMarkerFile (root).deleteFile ();
    return true;
}

juce::StringArray installedRuntimeLibraries(const juce::File& d)
{
    return installedRuntimeFiles (rootOr (d));
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

#else   // macOS / Linux

// One CLI run through sc::py::runProcessEx: own process group (cancellation
// kills the whole tree; prctl PDEATHSIG on Linux), stdin = /dev/null, stdout
// lines streamed to onLogLine while it runs.
static bool runProcessWithPipes(const juce::File& binary, const juce::StringArray& args,
                                const juce::File& outDirToCreate,
                                const std::function<bool()>& shouldContinue,
                                const std::function<void(const juce::String&)>& onLogLine,
                                RunResult* resOut, std::string* errOut)
{
    if (!binary.existsAsFile ())
    {
        if (errOut) *errOut = "The " + binaryFileName ().toStdString () + " binary is missing.";
        return false;
    }
    // A binary copied in by hand may have lost its execute bit (no-op otherwise).
    binary.setExecutePermission (true);

    outDirToCreate.createDirectory ();

    sc::py::RunOptions opts;
    opts.pythonUtf8 = false;      // not a Python child
    opts.killTreeOnExit = true;   // nothing may outlive the run
    opts.onStdoutLine = onLogLine;
    const sc::py::ProcessResult r = sc::py::runProcessEx (binary.getFullPathName ().toStdString (),
                                                          args, opts, shouldContinue);
    if (resOut != nullptr)
    {
        resOut->exitCode = r.exitCode;
        resOut->stdoutText = r.stdoutText;
        resOut->stderrText = r.stderrText;
        resOut->cancelled = r.cancelled;
    }
    if (!r.started)
    {
        if (errOut) *errOut = "Could not start " + binaryFileName ().toStdString () + ".";
        return false;
    }
    if (r.cancelled)
    {
        if (errOut) *errOut = "Cancelled.";
        return false;
    }
    return r.exitCode == 0;
}

#endif

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

}} // namespace sc::acpp
