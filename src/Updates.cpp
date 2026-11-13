// SpectroCraft — update check implementation. See include/Updates.h.
//
// The HTTP code follows the same rules as the model downloader
// (src/AI/ModelManager.cpp): HTTPS only, an explicit User-Agent, redirects
// followed, the final HTTP status checked before anything is written, and the
// body streamed to <dest>.part so a half-download never looks like a file.

#include "Updates.h"

#include "AI/Sha256.h"
#include "Ed25519.h"

#include <algorithm>
#include <vector>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wininet.h>
#endif

namespace sc { namespace update {

// ==========================================================================
// Constants
// ==========================================================================

juce::String manifestPublicKeyHex()
{
    // Ed25519 public key; the matching private key signs update-manifest.json
    // when a release is built (tools/updates/README.md).
    return "36901732a161bef50ac2710016efe240c15935e04f1e356aab3ec5f33bff23da";
}

// ==========================================================================
// Version comparison
// ==========================================================================

juce::Array<int> versionParts (const juce::String& version)
{
    juce::Array<int> parts;
    juce::String s = version.trim();
    while (s.startsWithChar ('v') || s.startsWithChar ('V')) s = s.substring (1);

    // "1.0.2300", "1.0", "1.0.2300-rc1": numeric components only, stop at the
    // first component that is not a plain number.
    for (const auto& token : juce::StringArray::fromTokens (s, ".", "\"'"))
    {
        juce::String t = token.trim();
        int end = 0;
        while (end < t.length() && t[end] >= '0' && t[end] <= '9') ++end;
        if (end == 0) break;
        parts.add (t.substring (0, end).getIntValue());
        if (end < t.length()) break;   // "2300-rc1": the rest is not numeric
    }
    return parts;
}

int compareVersions (const juce::String& a, const juce::String& b)
{
    const juce::Array<int> va = versionParts (a), vb = versionParts (b);
    const int n = std::max (va.size(), vb.size());
    for (int i = 0; i < n; ++i)
    {
        const int x = i < va.size() ? va[i] : 0;
        const int y = i < vb.size() ? vb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// ==========================================================================
// HTTP
// ==========================================================================

namespace {

bool isHttps (const juce::String& url)
{
    return url.startsWithIgnoreCase ("https://");
}

constexpr int64_t kMaxStringBytes = 4 << 20;   // release JSON / manifest / .sig

} // namespace

#if JUCE_WINDOWS

namespace {

struct WinInetSession
{
    HINTERNET session = nullptr;

    WinInetSession()
    {
        session = InternetOpenA ("SpectroCraft", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, 0, 0);
        if (session != nullptr)
        {
            DWORD timeoutMs = 30000;
            InternetSetOptionA (session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof (timeoutMs));
            InternetSetOptionA (session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof (timeoutMs));
            InternetSetOptionA (session, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof (timeoutMs));
        }
    }

    ~WinInetSession() { if (session != nullptr) InternetCloseHandle (session); }
};

// Open a GET request. WinINet follows redirects; the final status is checked
// here so an error page (404, captive portal, proxy login) is reported as
// what it is instead of being saved as a file.
HINTERNET openRequest (HINTERNET session, const juce::String& url, DWORD& statusOut,
                      int64_t& lengthOut, juce::String& errOut)
{
    DWORD flags = INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    HINTERNET req = InternetOpenUrlA (session, url.toRawUTF8(), nullptr, 0, flags, 0);
    if (req == nullptr)
    {
        errOut = "could not reach GitHub (WinINet error " + juce::String ((int) GetLastError ())
               + " — no internet access, a proxy or a captive portal?).";
        return nullptr;
    }

    DWORD status = 0, size = sizeof (status);
    // This SDK's HttpQueryInfoA has a trailing lpdwIndex parameter.
    if (HttpQueryInfoA (req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                        &status, &size, nullptr))
        statusOut = status;

    if (statusOut < 200 || statusOut > 299)
    {
        errOut = "HTTP " + juce::String ((int) statusOut) + " from GitHub";
        InternetCloseHandle (req);
        return nullptr;
    }

    lengthOut = 0;
    char lenText[64] = {};
    DWORD lenSize = sizeof (lenText) - 1;
    if (HttpQueryInfoA (req, HTTP_QUERY_CONTENT_LENGTH, lenText, &lenSize, nullptr))
        lengthOut = juce::String (lenText).trim().getLargeIntValue();
    if (lengthOut < 0) lengthOut = 0;   // chunked: unknown

    return req;
}

} // namespace

bool httpGetToFile (const juce::String& url, const juce::File& dest,
                    ProgressFn onProgress, juce::String& errOut)
{
    if (! isHttps (url)) { errOut = "refusing a non-HTTPS URL: " + url; return false; }

    WinInetSession net;
    if (net.session == nullptr) { errOut = "could not initialise the network stack."; return false; }

    DWORD status = 0;
    int64_t expected = 0;
    HINTERNET req = openRequest (net.session, url, status, expected, errOut);
    if (req == nullptr) return false;

    const juce::File part = dest.withFileExtension (dest.getFileExtension() + ".part");
    part.deleteFile();
    if (! part.getParentDirectory().createDirectory())
    {
        errOut = "could not write " + part.getParentDirectory().getFullPathName();
        return false;
    }

    bool cancelled = false, writeFailed = false, transferError = false;
    int64_t got = 0;
    {
        juce::FileOutputStream out (part);
        if (out.failedToOpen())
        {
            errOut = "could not write " + part.getFullPathName();
            InternetCloseHandle (req);
            return false;
        }

        sc::ai::Sha256 digest;
        char buf[1 << 18];
        while (! cancelled)
        {
            DWORD n = 0;
            if (! InternetReadFile (req, buf, sizeof (buf), &n))
            {
                transferError = true;   // reset or timeout
                break;
            }
            if (n == 0) break;          // end of the body

            if (! out.write (reinterpret_cast<const unsigned char*> (buf), (int) n))
            {
                writeFailed = true;
                break;
            }
            digest.update (reinterpret_cast<const unsigned char*> (buf), (size_t) n);
            got += (int64_t) n;

            if (onProgress != nullptr)
            {
                const double frac = expected > 0
                    ? std::min (1.0, (double) got / (double) expected) : 0.0;
                if (! onProgress (frac, "downloading " + dest.getFileName()))
                {
                    cancelled = true;
                    break;
                }
            }
        }
    }
    InternetCloseHandle (req);

    if (cancelled) { part.deleteFile(); errOut = "cancelled"; return false; }
    if (transferError) { part.deleteFile(); errOut = "the connection was interrupted."; return false; }
    if (writeFailed || (expected > 0 && got != expected))
    {
        part.deleteFile();
        errOut = "incomplete download (" + juce::String (got) + " of "
                 + juce::String (expected) + " bytes).";
        return false;
    }

    if (! part.moveFileTo (dest))
    {
        part.deleteFile();
        errOut = "could not create " + dest.getFullPathName();
        return false;
    }
    return true;
}

bool httpGetString (const juce::String& url, juce::String& bodyOut, juce::String& errOut)
{
    if (! isHttps (url)) { errOut = "refusing a non-HTTPS URL: " + url; return false; }

    WinInetSession net;
    if (net.session == nullptr) { errOut = "could not initialise the network stack."; return false; }

    DWORD status = 0;
    int64_t expected = 0;
    HINTERNET req = openRequest (net.session, url, status, expected, errOut);
    if (req == nullptr) return false;

    std::vector<char> body;
    bool tooBig = false, transferError = false;
    char buf[1 << 16];
    for (;;)
    {
        DWORD n = 0;
        if (! InternetReadFile (req, buf, sizeof (buf), &n)) { transferError = true; break; }
        if (n == 0) break;
        body.insert (body.end(), buf, buf + n);
        if (body.size() > (size_t) kMaxStringBytes) { tooBig = true; break; }
    }
    InternetCloseHandle (req);

    if (tooBig)  { errOut = "the response is larger than 4 MB."; return false; }
    if (transferError) { errOut = "the connection was interrupted."; return false; }
    if (body.empty()) { errOut = "empty response."; return false; }

    // The response is UTF-8 (release notes contain em dashes and the like), so
    // it must be decoded as UTF-8: the plain const char* constructor asserts
    // that the bytes are ASCII.
    bodyOut = juce::String::fromUTF8 (body.data(), (int) body.size());
    return true;
}

#else  // ----------------------------------------------------------------- !Windows

bool httpGetToFile (const juce::String& url, const juce::File& dest,
                    ProgressFn onProgress, juce::String& errOut)
{
    if (! isHttps (url)) { errOut = "refusing a non-HTTPS URL: " + url; return false; }

    juce::URL u (url);
    auto in = u.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                     .withConnectionTimeoutMs (30000)
                                     .withExtraHeaders ("User-Agent: SpectroCraft"));
    if (in == nullptr) { errOut = "could not open " + url; return false; }

    const juce::File part = dest.withFileExtension (dest.getFileExtension() + ".part");
    part.deleteFile();
    {
        juce::FileOutputStream out (part);
        if (out.failedToOpen()) { errOut = "could not write " + part.getFullPathName(); return false; }

        int64_t total = 0;
        char buf[1 << 16];
        for (;;)
        {
            const int n = in->read (buf, sizeof (buf));
            if (n < 0) { errOut = "the connection was interrupted."; part.deleteFile(); return false; }
            if (n == 0) break;
            if (! out.write (buf, n)) { errOut = "could not write " + part.getFullPathName(); part.deleteFile(); return false; }
            total += n;
            if (onProgress != nullptr && ! onProgress (0.0, "downloading " + dest.getFileName()))
            {
                part.deleteFile(); errOut = "cancelled"; return false;
            }
        }
        if (total == 0) { part.deleteFile(); errOut = "empty response."; return false; }
    }

    if (! part.moveFileTo (dest)) { part.deleteFile(); errOut = "could not create " + dest.getFullPathName(); return false; }
    return true;
}

bool httpGetString (const juce::String& url, juce::String& bodyOut, juce::String& errOut)
{
    // Automatic updates are Windows-only for now (docs/updates.md); this path
    // exists so the code and its tests build everywhere.
    if (! isHttps (url)) { errOut = "refusing a non-HTTPS URL: " + url; return false; }

    juce::URL u (url);
    auto in = u.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                     .withConnectionTimeoutMs (30000)
                                     .withExtraHeaders ("User-Agent: SpectroCraft"));
    if (in == nullptr) { errOut = "could not open " + url; return false; }

    std::string body;
    char buf[1 << 16];
    for (;;)
    {
        const int n = in->read (buf, sizeof (buf));
        if (n < 0) { errOut = "the connection was interrupted."; return false; }
        if (n == 0) break;
        body.append (buf, (size_t) n);
        if (body.size() > (size_t) kMaxStringBytes) { errOut = "the response is larger than 4 MB."; return false; }
    }
    if (body.empty()) { errOut = "empty response."; return false; }
    bodyOut = juce::String::fromUTF8 (body.data(), (int) body.size());
    return true;
}

#endif

// ==========================================================================
// Latest release
// ==========================================================================

namespace {

juce::String jsonField (const juce::var& obj, const char* key)
{
    const juce::var v = obj.getDynamicObject() != nullptr
                      ? obj.getDynamicObject()->getProperty (juce::Identifier (key)) : juce::var::undefined();
    if (v.isString()) return v.toString();
    if (v.isInt() || v.isInt64() || v.isDouble()) return v.toString();
    return {};
}

bool jsonInt64 (const juce::var& obj, const char* key, int64_t& out)
{
    if (obj.getDynamicObject() == nullptr) return false;
    const juce::var v = obj.getDynamicObject()->getProperty (juce::Identifier (key));
    if (! v.isInt() && ! v.isInt64()) return false;
    out = (juce::int64) v;
    return true;
}

bool jsonBool (const juce::var& obj, const char* key)
{
    if (obj.getDynamicObject() == nullptr) return false;
    const juce::var v = obj.getDynamicObject()->getProperty (juce::Identifier (key));
    if (v.isBool()) return (bool) v;
    if (v.isInt() || v.isInt64()) return ((juce::int64) v) != 0;
    const juce::String s = v.toString().toLowerCase();
    return s == "true" || s == "yes" || s == "1";
}

} // namespace

bool fetchLatestRelease (ReleaseInfo& out, juce::String& errOut)
{
    out = ReleaseInfo{};

    juce::String body;
    if (! httpGetString (kLatestApi, body, errOut)) return false;

    const juce::var parsed = juce::JSON::parse (body);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr)
    {
        errOut = "GitHub did not return a release (the API may be rate-limited).";
        return false;
    }

    out.tag = jsonField (parsed, "tag_name");
    out.title = jsonField (parsed, "name");
    out.notes = jsonField (parsed, "body");
    out.pageUrl = jsonField (parsed, "html_url");
    out.version = out.tag;
    while (out.version.startsWithChar ('v') || out.version.startsWithChar ('V'))
        out.version = out.version.substring (1);

    if (out.tag.isEmpty())
    {
        errOut = "the response has no tag_name.";
        return false;
    }

    const juce::var assets = obj->getProperty (juce::Identifier ("assets"));
    if (auto* arr = assets.getArray())
    {
        for (const auto& a : *arr)
        {
            auto* ao = a.getDynamicObject();
            if (ao == nullptr) continue;
            AssetInfo info;
            info.name = jsonField (a, "name");
            info.url = jsonField (a, "browser_download_url");
            int64_t sz = 0;
            info.size = jsonInt64 (a, "size", sz) ? sz : 0;
            if (info.name.isNotEmpty() && info.url.isNotEmpty()) out.assets.add (info);
        }
    }

    if (out.pageUrl.isEmpty()) out.pageUrl = kReleasesUrl;
    return true;
}

juce::String platformToken()
{
#if JUCE_WINDOWS
    return "windows";
#elif JUCE_MAC
    return "macos";
#elif JUCE_LINUX
    return "linux";
#else
    return "unknown";
#endif
}

juce::String archToken()
{
#if JUCE_ARM64 || defined(__aarch64__)
    return "arm64";
#else
    return "x64";
#endif
}

const AssetInfo* pickAsset (const ReleaseInfo& release)
{
    const juce::String os = platformToken(), arch = archToken();

    // 1. SpectroCraft-<version>-<os>-<arch>.zip (the naming new releases use).
    const juce::String versioned = "spectrocraft-" + release.version.toLowerCase()
                                 + "-" + os + "-" + arch + ".zip";
    // 2. SpectroCraft-<os>-<arch>.zip (the names used up to 1.0.x).
    const juce::String plain = "spectrocraft-" + os + "-" + arch + ".zip";

    const AssetInfo* fallback = nullptr;
    for (const auto& a : release.assets)
    {
        const juce::String n = a.name.toLowerCase();
        if (n == versioned) return &a;
        if (n == plain) fallback = a.url.isNotEmpty() ? &a : fallback;
        if (fallback == nullptr
            && n.endsWith (".zip") && n.contains (os) && n.contains (arch))
            fallback = &a;
    }
    return fallback;
}

const AssetInfo* findAsset (const ReleaseInfo& release, const juce::String& name)
{
    for (const auto& a : release.assets)
        if (a.name.equalsIgnoreCase (name) && a.url.isNotEmpty())
            return &a;
    return nullptr;
}

// ==========================================================================
// Manifest
// ==========================================================================

namespace {

bool validRelativePath (const juce::String& p)
{
    if (p.isEmpty() || p.startsWithChar ('/') || p.contains ("..")) return false;
    if (p.contains ("\\") || p.contains (":")) return false;   // must be zip-style
    for (char c : p)
        if (c < 32 || c == '*') return false;
    return true;
}

bool isHex64 (const juce::String& s)
{
    if (s.length() != 64) return false;
    for (char c : s)
        if (! ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

} // namespace

bool parseManifest (const juce::String& jsonText, Manifest& out, juce::String& errOut)
{
    out = Manifest{};

    const juce::var parsed = juce::JSON::parse (jsonText);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr) { errOut = "the update manifest is not valid JSON."; return false; }

    out.format = jsonField (parsed, "format");
    out.version = jsonField (parsed, "version");

    if (out.format != "spectrocraft-update-manifest/1")
    {
        errOut = "unsupported update manifest: " + (out.format.isEmpty() ? juce::String ("<none>") : out.format);
        return false;
    }
    // A manifest is machine-generated (tools/make_update_manifest.py), so it
    // always carries the full three-part version. A GitHub tag may be shorter
    // and is checked more loosely where it is read.
    if (out.version.isEmpty() || versionParts (out.version).size() != 3)
    { errOut = "the update manifest has no usable version: " + out.version; return false; }

    const juce::var files = obj->getProperty (juce::Identifier ("files"));
    auto* arr = files.getArray();
    if (arr == nullptr || arr->isEmpty()) { errOut = "the update manifest lists no files."; return false; }

    for (const auto& f : *arr)
    {
        auto* fo = f.getDynamicObject();
        if (fo == nullptr) { errOut = "the update manifest has a malformed entry."; return false; }

        ManifestFile mf;
        mf.path = jsonField (f, "path");
        int64_t sz = -1;
        mf.size = jsonInt64 (f, "size", sz) ? sz : -1;
        mf.sha256 = jsonField (f, "sha256");

        if (! validRelativePath (mf.path)) { errOut = "the manifest lists an invalid path: " + mf.path; return false; }
        if (mf.size < 0) { errOut = "the manifest has no size for " + mf.path; return false; }
        if (! isHex64 (mf.sha256)) { errOut = "the manifest has no SHA-256 for " + mf.path; return false; }
        for (const auto& other : out.files)
            if (other.path == mf.path) { errOut = "the manifest lists " + mf.path + " twice."; return false; }

        out.files.add (mf);
    }
    return true;
}

bool verifyManifestSignature (const juce::String& manifestText, const juce::String& sigText,
                             juce::String& errOut)
{
    uint8_t pub[32], sig[64];
    if (! sc::crypto::keyFromHex (manifestPublicKeyHex().toStdString(), pub))
    { errOut = "the built-in update key is malformed."; return false; }
    if (! sc::crypto::sigFromHex (sigText.trim().toStdString(), sig))
    {
        errOut = "the signature file is not a 64-byte Ed25519 signature.";
        return false;
    }

    const std::string bytes = manifestText.toStdString();   // exactly the downloaded bytes
    if (! sc::crypto::ed25519Verify (pub, reinterpret_cast<const uint8_t*> (bytes.data()),
                                     bytes.size(), sig))
    {
        errOut = "the update manifest is not signed with SpectroCraft's release key.";
        return false;
    }
    return true;
}

// ==========================================================================
// Staging: unzip + verify
// ==========================================================================

namespace {

juce::String normaliseEntry (juce::String p)
{
    p = p.replaceCharacter ('\\', '/');
    while (p.startsWith ("./")) p = p.substring (2);
    while (p.endsWith ("/")) p = p.substring (0, p.length() - 1);
    return p;
}

} // namespace

bool unzipAndVerify (const juce::File& zip, const juce::File& stagingDir,
                     const Manifest& manifest, ProgressFn onProgress, juce::String& errOut)
{
    errOut.clear();

    std::unique_ptr<juce::FileInputStream> zin (zip.createInputStream());
    if (zin == nullptr || zin->failedToOpen()) { errOut = "could not open " + zip.getFileName(); return false; }

    juce::ZipFile archive (std::move (zin));
    const int nEntries = archive.getNumEntries();
    if (nEntries == 0) { errOut = "the downloaded archive is empty."; return false; }

    // Manifest order, copied out because this JUCE build's Array::operator[]
    // returns a value: a pointer into the array is not available.
    std::vector<juce::String> wantPath;
    for (const auto& f : manifest.files) wantPath.push_back (f.path);
    std::vector<bool> seen (wantPath.size(), false);

    int done = 0;
    for (int i = 0; i < nEntries; ++i)
    {
        const auto* entry = archive.getEntry (i);
        if (entry == nullptr) continue;

        const juce::String path = normaliseEntry (entry->filename);
        if (path.isEmpty()) continue;
        if (path.contains ("..")) { errOut = "the archive contains a path outside the update: " + path; return false; }

        int wantIndex = -1;
        for (size_t k = 0; k < wantPath.size(); ++k)
            if (wantPath[k] == path) { wantIndex = (int) k; break; }
        if (wantIndex < 0)
        {
            errOut = "the archive contains a file the manifest does not list: " + path;
            return false;
        }

        const ManifestFile want = manifest.files[wantIndex];

        std::unique_ptr<juce::InputStream> in (archive.createStreamForEntry (i));
        if (in == nullptr) { errOut = "could not read " + path + " from the archive."; return false; }

        const juce::File out = stagingDir.getChildFile (path);
        if (! out.getParentDirectory().createDirectory())
        { errOut = "could not create " + out.getParentDirectory().getFullPathName(); return false; }
        out.deleteFile();

        int64_t written = 0;
        {
            juce::FileOutputStream outStream (out);
            if (outStream.failedToOpen()) { errOut = "could not write " + out.getFullPathName(); return false; }

            sc::ai::Sha256 digest;
            char buf[1 << 16];
            for (;;)
            {
                const int n = in->read (buf, sizeof (buf));
                if (n < 0) { errOut = "the archive is corrupt (" + path + ")."; return false; }
                if (n == 0) break;
                if (! outStream.write (buf, n)) { errOut = "could not write " + path; return false; }
                digest.update (reinterpret_cast<const unsigned char*> (buf), (size_t) n);
                written += n;
            }
            outStream.flush();

            if (written != want.size)
            {
                errOut = path + ": the archive holds " + juce::String (written)
                       + " bytes, the manifest says " + juce::String (want.size) + ".";
                return false;
            }
            const juce::String got = juce::String (digest.hexDigest());
            if (got != want.sha256)
            {
                errOut = path + ": SHA-256 mismatch (the download is damaged or the manifest is wrong).";
                return false;
            }
        }
        seen[wantIndex] = true;

        ++done;
        if (onProgress != nullptr)
        {
            const double frac = (double) done / (double) std::max (1, (int) wantPath.size());
            if (! onProgress (frac, "checking " + path)) return false;   // cancelled
        }
    }

    for (size_t k = 0; k < seen.size(); ++k)
        if (! seen[k])
        {
            errOut = "the archive is missing " + wantPath[k] + " (listed in the manifest).";
            return false;
        }

    return true;
}

// ==========================================================================
// Install directory, writability, launching the updater
// ==========================================================================

juce::File installDir()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
}

bool dirIsWritable (const juce::File& dir)
{
    if (! dir.isDirectory()) return false;
    const juce::File probe = dir.getChildFile ("spectrocraft-write-test.tmp");
    probe.deleteFile();
    {
        juce::FileOutputStream out (probe);
        if (out.failedToOpen()) return false;
        out.write ("ok", 2);
        out.flush();
    }
    return probe.deleteFile();
}

juce::File findUpdaterBinary (const juce::File& stagingDir)
{
    const char* names[] = { "spectrocraft-updater.exe", "spectrocraft-updater" };

    for (const char* n : names)
    {
        const juce::File direct = stagingDir.getChildFile ("SpectroCraft").getChildFile (n);
        if (direct.existsAsFile()) return direct;
    }

    // Fallback: search the staging tree (a release may move the updater).
    for (const auto& entry : juce::RangedDirectoryIterator (stagingDir, true, "*", juce::File::findFiles))
    {
        const juce::File f = entry.getFile();
        const juce::String fn = f.getFileName().toLowerCase();
        for (const char* n : names)
            if (fn == n) return f;
    }
    return {};
}

juce::File updateTempDir()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
             .getChildFile ("SpectroCraft").getChildFile ("update");
}

juce::File updatePlanFile (const juce::File& stagingDir)
{
    return stagingDir.getChildFile ("update-plan.txt");
}

bool writeUpdatePlan (const juce::File& stagingDir, const Manifest& manifest, juce::String& errOut)
{
    if (! stagingDir.createDirectory())
    { errOut = "could not create " + stagingDir.getFullPathName(); return false; }

    juce::String text = "spectrocraft-update-plan/1\n";
    text += "version=" + manifest.version + "\n";
    for (const auto& f : manifest.files)
        text += "file=" + f.path + "\n";

    const juce::File plan = updatePlanFile (stagingDir);
    plan.deleteFile();
    if (! plan.replaceWithText (text))
    { errOut = "could not write " + plan.getFullPathName(); return false; }
    return true;
}

#if JUCE_WINDOWS
namespace {

// Quote one argument for a CreateProcessW command line. Backslashes are only
// special immediately before a quote, so a run of backslashes before a quote
// must be doubled and the quote escaped (same rules as acppQuote in
// src/AudioCpp.cpp).
std::wstring quoteArg (const juce::String& arg)
{
    const std::wstring w = arg.toWideCharPointer();
    bool needsQuotes = false;
    for (wchar_t c : w)
        if (c == L' ' || c == L'\t' || c == L'"') { needsQuotes = true; break; }
    if (! w.empty() && ! needsQuotes) return w;

    std::wstring q = L"\"";
    size_t i = 0;
    while (i < w.size())
    {
        size_t nBack = 0;
        while (i + nBack < w.size() && w[i + nBack] == L'\\') ++nBack;
        if (i + nBack < w.size() && w[i + nBack] == L'"')
        {
            q.append (nBack * 2 + 1, L'\\');
            q += L'"';
        }
        else
        {
            q.append (nBack, L'\\');
            if (i + nBack < w.size()) q += w[i + nBack];
        }
        i += nBack + 1;
    }
    return q + L"\"";
}

} // namespace

bool launchUpdater (const juce::File& updater, const juce::File& install,
                    const juce::File& stagingDir, const juce::File& appExe,
                    juce::String& errOut)
{
    std::wstring cmd = quoteArg (updater.getFullPathName());
    cmd += L" --pid ";
    cmd += std::to_wstring ((long long) GetCurrentProcessId());
    cmd += L" --install-dir " + quoteArg (install.getFullPathName());
    cmd += L" --staging-dir " + quoteArg (stagingDir.getFullPathName());
    cmd += L" --relaunch " + quoteArg (appExe.getFullPathName());

    // CreateProcessW may modify the command line buffer, so it needs a copy.
    std::wstring mutableCmd = cmd;
    const std::wstring workDir = install.getFullPathName().toWideCharPointer();

    STARTUPINFOW si = {};
    si.cb = sizeof (si);
    PROCESS_INFORMATION pi = {};
    // The updater must outlive us: it waits until this process is gone before
    // it touches a single file. Its own console keeps its output visible.
    const BOOL created = CreateProcessW (nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                                         CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP,
                                         nullptr, workDir.c_str(), &si, &pi);
    if (! created)
    {
        errOut = "could not start " + updater.getFileName() + " (Windows error "
               + juce::String ((int) GetLastError ()) + ").";
        return false;
    }
    CloseHandle (pi.hProcess);
    CloseHandle (pi.hThread);
    return true;
}

#else

bool launchUpdater (const juce::File&, const juce::File&, const juce::File&,
                    const juce::File&, juce::String& errOut)
{
    // Automatic updates are Windows-only for now (docs/updates.md): there is no
    // updater binary in the macOS/Linux archives and no detached-process helper
    // in this JUCE build. Help > Check for Updates is not offered elsewhere.
    errOut = "automatic updates are only available in the Windows build.";
    return false;
}

#endif

// ==========================================================================
// update-result.json (written by the updater, read on the next start)
// ==========================================================================

juce::File updateResultFile (const juce::File& install)
{
    return install.getChildFile ("update-result.json");
}

namespace {

juce::Array<juce::String> stringArray (const juce::var& v)
{
    juce::Array<juce::String> out;
    if (auto* arr = v.getArray())
        for (const auto& e : *arr) out.add (e.toString());
    return out;
}

} // namespace

bool readUpdateResult (const juce::File& install, UpdateResult& out)
{
    out = UpdateResult{};

    const juce::File f = updateResultFile (install);
    if (! f.existsAsFile()) return false;

    const juce::String text = f.loadFileAsString();
    f.deleteFile();   // report once

    const juce::var parsed = juce::JSON::parse (text);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr)
    {
        out.success = false;
        out.reason = "the update result file could not be read.";
        return true;
    }

    out.success = jsonBool (parsed, "success");
    out.version = jsonField (parsed, "version");
    out.reason = jsonField (parsed, "reason");
    out.failedFile = jsonField (parsed, "failedFile");
    out.oldFiles = stringArray (obj->getProperty (juce::Identifier ("oldFiles")));
    out.cleanupDirs = stringArray (obj->getProperty (juce::Identifier ("cleanupDirs")));
    out.replaced = stringArray (obj->getProperty (juce::Identifier ("replaced")));
    return true;
}

}} // namespace sc::update
