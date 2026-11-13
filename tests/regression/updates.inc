// Automatic updates (docs/updates.md): the crypto (Ed25519 + SHA-512 against
// the RFC 8032 / FIPS vectors), version comparison, asset picking, manifest
// parsing and signature verification, staging with per-file hash checks, and
// the two files the app and spectrocraft-updater exchange (update-plan.txt,
// update-result.json).
//
// Included by tests/UnitTests.cpp; uses its helpers (CHECK, scSkip,
// makeTempFile, scTestTempDir). Nothing here touches the network: the release
// JSON is exercised through the same parser with a fixed string.
#include "Updates.h"
#include "Ed25519.h"
#include "AI/Sha256.h"

namespace scupd = sc::update;
namespace sccry = sc::crypto;

namespace scupdtest
{

static int hexNibble (char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static std::vector<uint8_t> unhex (const juce::String& hex)
{
    std::vector<uint8_t> out;
    for (int i = 0; i + 1 < hex.length(); i += 2)
        out.push_back ((uint8_t) ((hexNibble (hex[i]) << 4) | hexNibble (hex[i + 1])));
    return out;
}

// This JUCE build has no String (count, char) constructor.
static juce::String repeatChar (int n, char c)
{
    juce::String s;
    for (int i = 0; i < n; ++i) s += juce::String::charToString (c);
    return s;
}

static bool isLowerHex (const std::string& s, size_t len)
{
    if (s.size() != len) return false;
    for (char c : s)
        if (! ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Ed25519: the RFC 8032 §7.1 vectors (public key, signature, verify) and the
// rejections a wrong key, a tampered message or a tampered signature cause.
// ---------------------------------------------------------------------------
struct RfcVector
{
    const char* seed;
    const char* pub;
    const char* msgHex;   // "" for the empty message
    const char* sigHex;
};

static void updTest_ed25519()
{
    static const RfcVector vec[] = {
        // RFC 8032 §7.1, TEST 1 (empty message), TEST 2 (one byte), TEST 3.
        { "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
          "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
          "",
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
          "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
        { "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
          "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
          "72",
          "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
          "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
        { "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
          "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
          "af82",
          "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
          "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
    };

    for (int i = 0; i < (int) (sizeof (vec) / sizeof (vec[0])); ++i)
    {
        const RfcVector& v = vec[i];
        const juce::String tag = "vector " + juce::String (i + 1);

        uint8_t seed[32], pub[32];
        CHECK (sccry::keyFromHex (v.seed, seed), "ed25519 " + tag + ": seed parses");
        CHECK (sccry::keyFromHex (v.pub, pub), "ed25519 " + tag + ": public key parses");

        uint8_t derived[32];
        CHECK (sccry::ed25519PublicFromSeed (seed, derived)
                   && sccry::toHex (derived, 32) == v.pub,
               "ed25519 " + tag + ": RFC public key derived from the seed");

        const auto msg = unhex (v.msgHex);
        uint8_t sig[64];
        CHECK (sccry::ed25519Sign (seed, msg.data(), msg.size(), sig),
               "ed25519 " + tag + ": signs");
        CHECK (sccry::toHex (sig, 64) == v.sigHex,
               "ed25519 " + tag + ": RFC signature reproduced");

        const auto sigBytes = unhex (v.sigHex);
        CHECK (sccry::ed25519Verify (pub, msg.data(), msg.size(), sigBytes.data()),
               "ed25519 " + tag + ": verifies the RFC signature");

        auto badMsg = msg;
        if (badMsg.empty()) badMsg.push_back (0x00);
        else                badMsg[0] = (uint8_t) (badMsg[0] ^ 0x01);
        CHECK (! sccry::ed25519Verify (pub, badMsg.data(), badMsg.size(), sigBytes.data()),
               "ed25519 " + tag + ": rejects a changed message");

        auto badSig = sigBytes;
        badSig[0] = (uint8_t) (badSig[0] ^ 0x01);
        CHECK (! sccry::ed25519Verify (pub, msg.data(), msg.size(), badSig.data()),
               "ed25519 " + tag + ": rejects a changed signature");

        uint8_t otherSeed[32], otherPub[32];
        sccry::keyFromHex ("1111111111111111111111111111111111111111111111111111111111111111", otherSeed);
        sccry::ed25519PublicFromSeed (otherSeed, otherPub);
        CHECK (! sccry::ed25519Verify (otherPub, msg.data(), msg.size(), sigBytes.data()),
               "ed25519 " + tag + ": rejects another key");

        // A signature made with this seed verifies against this public key:
        // what the app does with the release key (see updTest_manifest).
        const std::string text = "spectrocraft manifest payload";
        uint8_t own[64];
        CHECK (sccry::ed25519Sign (seed, reinterpret_cast<const uint8_t*> (text.data()),
                                   text.size(), own)
                   && sccry::ed25519Verify (pub, reinterpret_cast<const uint8_t*> (text.data()),
                                            text.size(), own),
                   "ed25519 " + tag + ": own signature verifies");
    }

    // Malformed keys and signatures are refused, not treated as a bad signature.
    uint8_t k[32];
    CHECK (! sccry::keyFromHex ("dead", k), "ed25519: short key rejected");
    CHECK (! sccry::keyFromHex (repeatChar (64, 'z').toStdString(), k), "ed25519: non-hex key rejected");
    CHECK (sccry::keyFromHex (repeatChar (64, 'A').toStdString(), k), "ed25519: upper-case hex accepted");

    uint8_t s[64];
    CHECK (! sccry::sigFromHex ("nope", s), "ed25519: non-hex signature rejected");
    const juce::String sigHex = repeatChar (64, 'a') + repeatChar (64, 'b');
    CHECK (sccry::sigFromHex (("\n " + sigHex + " \r\n").toStdString(), s)
               && sccry::toHex (s, 64) == sigHex.toLowerCase(),
           "ed25519: signature hex read around whitespace");
}

// ---------------------------------------------------------------------------
// SHA-512 (FIPS 180-4 vectors) — the hash the release key's payload is signed
// over, and the digest the manifest's SHA-256 entries are checked with… the
// manifest itself uses SHA-256 (src/AI/Sha256.cpp), checked here too.
// ---------------------------------------------------------------------------
static void updTest_hashes()
{
    CHECK (sccry::sha512Hex ("", 0)
               == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                  "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
           "sha512: empty input");
    CHECK (sccry::sha512Hex ("abc", 3)
               == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                  "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
           "sha512: \"abc\"");

    // Streaming in 100-byte pieces (the 128-byte block boundary falls inside)
    // must equal the one-shot digest, and hexDigest() must reset the state.
    std::string big;
    for (int i = 0; i < 1000; ++i) big += "0123456789abcdef";
    sccry::Sha512 h;
    for (size_t off = 0; off < big.size(); off += 100)
        h.update (reinterpret_cast<const uint8_t*> (big.data()) + off,
                  std::min<size_t> (100, big.size() - off));
    CHECK (h.hexDigest() == sccry::sha512Hex (big.data(), big.size()),
           "sha512: streaming equals one-shot");
    CHECK (h.hexDigest() == sccry::sha512Hex ("", 0), "sha512: hexDigest resets the hasher");
    CHECK (isLowerHex (sccry::sha512Hex ("abc", 3), 128), "sha512: 128 lowercase hex chars");

    // SHA-256: the manifest's per-file hash (NIST vector for "abc").
    CHECK (sc::ai::sha256Hex ("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "sha256: \"abc\" (the manifest hash algorithm)");
}

// ---------------------------------------------------------------------------
// Version comparison
// ---------------------------------------------------------------------------
static void updTest_versions()
{
    CHECK (scupd::compareVersions ("1.0.2279", "1.0.2300") < 0, "1.0.2279 < 1.0.2300");
    CHECK (scupd::compareVersions ("1.0.2300", "1.0.2299") > 0, "1.0.2300 > 1.0.2299");
    CHECK (scupd::compareVersions ("v1.0.2300", "1.0.2300") == 0, "a leading v is ignored");
    CHECK (scupd::compareVersions ("1.0", "1.0.0") == 0, "missing parts count as zero");
    CHECK (scupd::compareVersions ("1.1", "1.0.9999") > 0, "minor beats patch");
    CHECK (scupd::compareVersions ("1.0.2300-rc1", "1.0.2299") > 0, "a suffix stops the parse, not the compare");
    CHECK (scupd::compareVersions ("1.0.x", "1.0") == 0, "a non-numeric part stops the parse");
    CHECK (scupd::compareVersions ("", "1.0.0") < 0, "an empty version is the oldest");
    CHECK (scupd::versionParts ("V1.2.3").size() == 3
               && scupd::versionParts ("V1.2.3")[2] == 3, "versionParts splits and strips the v");
}

// ---------------------------------------------------------------------------
// Choosing the archive of a release
// ---------------------------------------------------------------------------
static scupd::AssetInfo mkAsset (const juce::String& name, int64_t size = 1024)
{
    scupd::AssetInfo a;
    a.name = name;
    a.url = "https://example.invalid/" + name;
    a.size = size;
    return a;
}

static void updTest_assets()
{
    const juce::String os = scupd::platformToken(), arch = scupd::archToken();
    CHECK (os.isNotEmpty() && arch.isNotEmpty(), "platform/arch tokens exist");

    // A platform token that is never the one running the test: hard-coding
    // macos-arm64 as "another platform" made this fixture mean "this platform"
    // on macOS, where the exact-name fallback then won over the last-resort
    // rule.
    const juce::String other = (os == "macos" && arch == "arm64") ? "linux-x64"
                                                                   : "macos-arm64";

    scupd::ReleaseInfo rel;
    rel.version = "1.0.2300";
    rel.tag = "v1.0.2300";
    rel.assets.add (mkAsset ("update-manifest.json", 4096));
    rel.assets.add (mkAsset ("update-manifest.json.sig", 65));
    rel.assets.add (mkAsset ("SpectroCraft-" + os + "-" + arch + ".zip"));            // old name
    rel.assets.add (mkAsset ("SpectroCraft-" + other + ".zip"));                      // another platform
    rel.assets.add (mkAsset ("SpectroCraft-1.0.2300-" + os + "-" + arch + ".zip"));  // preferred

    const auto* picked = scupd::pickAsset (rel);
    CHECK (picked != nullptr && picked->name == "SpectroCraft-1.0.2300-" + os + "-" + arch + ".zip",
           "pickAsset prefers the versioned archive of this platform");

    // Without the versioned name: the plain SpectroCraft-<os>-<arch>.zip that
    // releases up to 1.0.x used.
    scupd::ReleaseInfo old = rel;
    old.assets.remove (old.assets.size() - 1);
    const auto* plain = scupd::pickAsset (old);
    CHECK (plain != nullptr && plain->name == "SpectroCraft-" + os + "-" + arch + ".zip",
           "pickAsset falls back to the unversioned name");

    // Neither: any .zip whose name contains both tokens.
    scupd::ReleaseInfo odd;
    odd.version = "1.0.2300";
    odd.assets.add (mkAsset ("SpectroCraft-" + os + "-" + arch + "-portable.zip"));
    odd.assets.add (mkAsset ("SpectroCraft-" + other + ".zip"));
    const auto* oddPick = scupd::pickAsset (odd);
    CHECK (oddPick != nullptr && oddPick->name.endsWith ("-portable.zip"),
           "pickAsset accepts a name containing both tokens as a last resort");

    scupd::ReleaseInfo none;
    none.version = "1.0.2300";
    none.assets.add (mkAsset ("SpectroCraft-sources.zip"));
    CHECK (scupd::pickAsset (none) == nullptr, "pickAsset refuses an archive for no platform");

    // The manifest and its signature are looked up by name.
    CHECK (scupd::findAsset (rel, "update-manifest.json") != nullptr, "findAsset finds the manifest");
    CHECK (scupd::findAsset (rel, "UPDATE-MANIFEST.JSON.SIG") != nullptr, "findAsset ignores case");
    CHECK (scupd::findAsset (rel, "nope.json") == nullptr, "findAsset reports a missing asset");
}

// ---------------------------------------------------------------------------
// The release JSON, through the same parser the app uses
// ---------------------------------------------------------------------------
static void updTest_releaseJson()
{
    // A trimmed copy of what api.github.com returns (the fields the app reads).
    const juce::String json = R"({
        "tag_name": "v1.0.2300",
        "name": "SpectroCraft 1.0.2300",
        "body": "## What's new\n- something",
        "html_url": "https://github.com/SuperMax3d/SpectroCraft/releases/tag/v1.0.2300",
        "assets": [
            { "name": "SpectroCraft-windows-x64.zip", "size": 123456789,
              "browser_download_url": "https://example.invalid/a.zip" },
            { "name": "update-manifest.json", "size": 4096,
              "browser_download_url": "https://example.invalid/m.json" },
            { "name": "no-url.zip", "size": 1 }
        ]
    })";

    // fetchLatestRelease() is the network call; parseManifest and the asset
    // list are the parts a fixed string can check. The JSON above is read the
    // way src/Updates.cpp reads it, through the same helper path.
    const juce::var parsed = juce::JSON::parse (json);
    CHECK (parsed.getDynamicObject() != nullptr, "release JSON parses");
    CHECK (parsed.getProperty ("tag_name", {}).toString() == "v1.0.2300", "tag_name read");

    // The version the app compares is the tag without its leading "v".
    juce::String version = parsed.getProperty ("tag_name", {}).toString();
    while (version.startsWithChar ('v')) version = version.substring (1);
    CHECK (version == "1.0.2300" && scupd::compareVersions (version, "1.0.2279") > 0,
           "a newer tag compares as newer");

    const auto assets = parsed.getProperty ("assets", {});
    CHECK (assets.isArray() && assets.size() == 3, "assets array read");
    CHECK (assets[0].getProperty ("browser_download_url", {}).toString() == "https://example.invalid/a.zip",
           "browser_download_url read");
}

// ---------------------------------------------------------------------------
// Manifest: parsing, then the signature
// ---------------------------------------------------------------------------
static juce::String updManifestJson (const juce::String& version,
                                    const juce::StringArray& entries)   // "path|size|sha256"
{
    juce::String j = "{\n  \"format\": \"spectrocraft-update-manifest/1\",\n";
    j += "  \"version\": \"" + version + "\",\n  \"generated\": \"2026-11-13T10:00:00Z\",\n";
    j += "  \"files\": [\n";
    for (int i = 0; i < entries.size(); ++i)
    {
        const juce::StringArray parts = juce::StringArray::fromTokens (entries[i], "|", "\"");
        j += "    { \"path\": \"" + parts[0] + "\", \"size\": " + parts[1]
           + ", \"sha256\": \"" + parts[2] + "\" }";
        j += (i + 1 < entries.size() ? ",\n" : "\n");
    }
    j += "  ]\n}\n";
    return j;
}

static juce::String sha256Of (const juce::String& text)
{
    return juce::String (sc::ai::sha256Hex (text.toRawUTF8(), (size_t) text.length()));
}

static void updTest_manifest()
{
    const juce::String shaExe = sha256Of ("the application");
    const juce::String shaDll = sha256Of ("a runtime library");

    const juce::String good = updManifestJson ("1.0.2300", {
        "SpectroCraft.exe|" + juce::String (13) + "|" + shaExe,
        "SpectroCraft/openvino.dll|" + juce::String (17) + "|" + shaDll,
    });

    scupd::Manifest m;
    juce::String err;
    CHECK (scupd::parseManifest (good, m, err), "manifest parses: " + err);
    CHECK (m.version == "1.0.2300" && m.files.size() == 2
               && m.files[0].path == "SpectroCraft.exe" && m.files[0].size == 13
               && m.files[0].sha256 == shaExe, "manifest fields read");

    // Rejected manifests: each one is a reason the app must not install.
    const juce::String badFormat = R"({"format":"other/1","version":"1.0.2300","files":[{"path":"a","size":1,"sha256":")"
                          + shaExe + R"("}]})";
    CHECK (scupd::parseManifest (badFormat, m, err) == false && err.contains ("unsupported"),
           "manifest: an unknown format is refused");

    CHECK (scupd::parseManifest (updManifestJson ("1.0.2300", {}), m, err) == false,
           "manifest: no files is refused");
    CHECK (scupd::parseManifest (updManifestJson ("", { "a|1|" + shaExe }), m, err) == false,
           "manifest: an empty version is refused");
    CHECK (scupd::parseManifest (updManifestJson ("1.0", { "a|1|" + shaExe }), m, err) == false,
           "manifest: a version with no patch number is refused");
    CHECK (scupd::parseManifest (updManifestJson ("1.0.2300", { "/etc/passwd|1|" + shaExe }), m, err) == false,
           "manifest: an absolute path is refused");
    CHECK (scupd::parseManifest (updManifestJson ("1.0.2300", { "../escape|1|" + shaExe }), m, err) == false,
           "manifest: a path outside the install folder is refused");
    CHECK (scupd::parseManifest (updManifestJson ("1.0.2300", { "a\\b.dll|1|" + shaExe }), m, err) == false,
           "manifest: a back-slash path is refused (archives use forward slashes)");
    CHECK (scupd::parseManifest (updManifestJson ("1.0.2300", { "a|1|deadbeef" }), m, err) == false,
           "manifest: a short SHA-256 is refused");
    CHECK (scupd::parseManifest (updManifestJson ("1.0.2300", { "a|1|" + shaExe.toUpperCase() }), m, err) == false,
           "manifest: an upper-case SHA-256 is refused");
    CHECK (scupd::parseManifest (updManifestJson ("1.0.2300", { "a|1|" + shaExe, "a|2|" + shaDll }), m, err) == false,
           "manifest: a duplicate path is refused");
    CHECK (scupd::parseManifest ("not json at all", m, err) == false, "manifest: invalid JSON is refused");

    // ---- the signature: signed with the release key, verified with the key
    // built into the app (include/Updates.h).
    const juce::String seedHex = "7e3f1c9a5b2d84e60f1a7c3d9e2b648a1d0c5f8b3a6e9d247c1b0a8f5e6d3c29";
    uint8_t seed[32];
    CHECK (sccry::keyFromHex (seedHex.toStdString(), seed), "release key seed parses");

    uint8_t pub[32];
    CHECK (sccry::ed25519PublicFromSeed (seed, pub)
               && sccry::toHex (pub, 32) == scupd::manifestPublicKeyHex().toLowerCase(),
           "the release key matches the public key built into the app");

    uint8_t sig[64];
    const std::string manifestBytes = good.toStdString();   // exactly the downloaded bytes
    CHECK (sccry::ed25519Sign (seed, reinterpret_cast<const uint8_t*> (manifestBytes.data()),
                               manifestBytes.size(), sig), "manifest signed with the release key");

    CHECK (scupd::verifyManifestSignature (good, juce::String (sccry::toHex (sig, 64)), err),
           "manifest signature verifies: " + err);
    CHECK (scupd::verifyManifestSignature (good, "  " + juce::String (sccry::toHex (sig, 64)) + "\n", err),
           "manifest signature: whitespace in the .sig file is tolerated");

    CHECK (! scupd::verifyManifestSignature (good + "\n", juce::String (sccry::toHex (sig, 64)), err),
           "manifest: one extra byte breaks the signature");
    CHECK (! scupd::verifyManifestSignature (good.replace ("1.0.2300", "1.0.2301"),
                                            juce::String (sccry::toHex (sig, 64)), err),
           "manifest: a changed version breaks the signature");

    uint8_t otherSeed[32], otherSig[64];
    sccry::keyFromHex ("1111111111111111111111111111111111111111111111111111111111111111", otherSeed);
    sccry::ed25519Sign (otherSeed, reinterpret_cast<const uint8_t*> (manifestBytes.data()),
                        manifestBytes.size(), otherSig);
    CHECK (! scupd::verifyManifestSignature (good, juce::String (sccry::toHex (otherSig, 64)), err),
           "manifest: a signature from another key is refused");
    CHECK (! scupd::verifyManifestSignature (good, "not-a-signature", err),
           "manifest: a malformed .sig file is refused");

    // The key CI signs with must be the one the app trusts. Read from the
    // source tree when the suite knows it (SC_SOURCE_DIR, tests/CMakeLists.txt).
    // A key file may hold only public= (the seed kept as a CI secret, see
    // tools/updates/README.md): then the public line itself is checked.
#if defined(SC_SOURCE_DIR)
    const juce::File keyFile = juce::File (SC_SOURCE_DIR).getChildFile ("tools/updates/ed25519.key");
    if (! keyFile.existsAsFile())
    {
        scMissingFixture ("tools/updates/ed25519.key");
    }
    else
    {
        juce::String fileSeed, filePublic;
        for (const auto& line : juce::StringArray::fromLines (keyFile.loadFileAsString ()))
        {
            const juce::String t = line.trim();
            if (t.startsWith ("seed="))   fileSeed   = t.substring (5).trim();
            if (t.startsWith ("public=")) filePublic = t.substring (7).trim();
        }
        if (fileSeed.isNotEmpty())
        {
            uint8_t fs[32];
            uint8_t fp[32];
            CHECK (sccry::keyFromHex (fileSeed.toStdString(), fs)
                       && sccry::ed25519PublicFromSeed (fs, fp)
                       && sccry::toHex (fp, 32) == scupd::manifestPublicKeyHex().toLowerCase(),
                   "tools/updates/ed25519.key signs what this build accepts");
        }
        else if (filePublic.isNotEmpty())
        {
            CHECK (filePublic.toLowerCase() == scupd::manifestPublicKeyHex().toLowerCase(),
                   "tools/updates/ed25519.key publishes the key this build accepts");
        }
        else
            scMissingFixture ("seed= or public= in tools/updates/ed25519.key");
    }
#else
    scSkip ("key file pairing: SC_SOURCE_DIR not defined (app-embedded test run)");
#endif
}

// ---------------------------------------------------------------------------
// Staging: unpack and check every file against the manifest
// ---------------------------------------------------------------------------
struct StageFile { juce::String path; juce::String content; };

static juce::File mkArchive (const juce::String& tag, const std::vector<StageFile>& files)
{
    const juce::File dir = makeTempFile ("updzip_" + tag);
    dir.createDirectory();

    juce::ZipFile::Builder zip;
    int n = 0;
    for (const auto& f : files)
    {
        const juce::File src = dir.getChildFile ("src" + juce::String (++n) + ".bin");
        src.replaceWithText (f.content);
        zip.addFile (src, 9, f.path);   // stored under its manifest path
    }

    const juce::File out = dir.getChildFile ("archive.zip");
    {
        juce::FileOutputStream os (out);
        CHECK (zip.writeToStream (os, nullptr), "test archive written");
        os.flush();
    }
    return out;
}

static scupd::Manifest mkManifest (const juce::String& version, const std::vector<StageFile>& files)
{
    scupd::Manifest m;
    m.format = "spectrocraft-update-manifest/1";
    m.version = version;
    for (const auto& f : files)
    {
        scupd::ManifestFile mf;
        mf.path = f.path;
        mf.size = (juce::int64) f.content.length();
        mf.sha256 = sha256Of (f.content);
        m.files.add (mf);
    }
    return m;
}

static void updTest_staging()
{
    const std::vector<StageFile> files = {
        { "SpectroCraft.exe", "the application" },
        { "SpectroCraft/openvino.dll", "a runtime library" },
        { "recorders/wasapi_recorder.dll", "a recorder plugin" },
    };

    const juce::File zip = mkArchive ("ok", files);
    const juce::File staging = makeTempFile ("updstage_ok");
    scupd::Manifest m = mkManifest ("1.0.2300", files);

    juce::String err;
    int stages = 0;
    CHECK (scupd::unzipAndVerify (zip, staging, m,
             [&] (double, const juce::String&) { ++stages; return true; }, err),
           "staging: every file unpacked and hashed: " + err);
    CHECK (stages == (int) files.size(), "staging: progress reported per file");
    CHECK (staging.getChildFile ("SpectroCraft.exe").loadFileAsString() == "the application"
               && staging.getChildFile ("SpectroCraft/openvino.dll").loadFileAsString() == "a runtime library"
               && staging.getChildFile ("recorders/wasapi_recorder.dll").loadFileAsString() == "a recorder plugin",
           "staging: contents match the archive");

    // A hash that does not match the manifest: nothing is trusted.
    scupd::Manifest wrongHash = mkManifest ("1.0.2300", files);
    {
        scupd::ManifestFile mf = wrongHash.files[1];
        mf.sha256 = sha256Of ("something else");
        wrongHash.files.set (1, mf);
    }
    const juce::File staging2 = makeTempFile ("updstage_hash");
    CHECK (! scupd::unzipAndVerify (zip, staging2, wrongHash, nullptr, err)
               && err.contains ("SHA-256"), "staging: a wrong hash is reported: " + err);

    // A size that does not match.
    scupd::Manifest wrongSize = mkManifest ("1.0.2300", files);
    {
        scupd::ManifestFile mf = wrongSize.files[0];
        mf.size += 5;
        wrongSize.files.set (0, mf);
    }
    CHECK (! scupd::unzipAndVerify (zip, makeTempFile ("updstage_size"), wrongSize, nullptr, err)
               && err.contains ("bytes"), "staging: a wrong size is reported: " + err);

    // A manifest entry the archive does not contain.
    scupd::Manifest missing = mkManifest ("1.0.2300", files);
    missing.files.add (scupd::ManifestFile { "SpectroCraft/absent.dll", 4, sha256Of ("absent") });
    CHECK (! scupd::unzipAndVerify (zip, makeTempFile ("updstage_missing"), missing, nullptr, err)
               && err.contains ("missing"), "staging: a missing file is reported: " + err);

    // Something in the archive the manifest does not list.
    std::vector<StageFile> extra = files;
    extra.push_back ({ "SpectroCraft/undocumented.dll", "not in the manifest" });
    const juce::File zipExtra = mkArchive ("extra", extra);
    CHECK (! scupd::unzipAndVerify (zipExtra, makeTempFile ("updstage_extra"), m, nullptr, err)
               && err.contains ("does not list"), "staging: an unlisted file is refused: " + err);

    // A path that would escape the install folder.
    std::vector<StageFile> escape = files;
    escape.push_back ({ "../../escape.txt", "outside" });
    const juce::File zipEscape = mkArchive ("escape", escape);
    CHECK (! scupd::unzipAndVerify (zipEscape, makeTempFile ("updstage_escape"), m, nullptr, err),
           "staging: an escaping archive path is refused: " + err);

    // A cancel stops the job; nothing was committed either way.
    const juce::File stagingCancel = makeTempFile ("updstage_cancel");
    CHECK (! scupd::unzipAndVerify (zip, stagingCancel, m,
             [&] (double, const juce::String&) { return false; }, err),
           "staging: a cancel stops the unpack");
    CHECK (err.isEmpty(), "staging: a cancel is not reported as an error");

    // A broken archive.
    const juce::File notAZip = makeTempFile ("updstage_notazip");
    notAZip.replaceWithText ("this is not a zip file");
    CHECK (! scupd::unzipAndVerify (notAZip, makeTempFile ("updstage_bad"), m, nullptr, err),
           "staging: an unreadable archive is reported: " + err);
}

// ---------------------------------------------------------------------------
// The two files the app and the updater exchange
// ---------------------------------------------------------------------------
static void updTest_planAndResult()
{
    const std::vector<StageFile> files = {
        { "SpectroCraft.exe", "the application" },
        { "SpectroCraft/spectrocraft-updater.exe", "the updater" },
    };
    const scupd::Manifest m = mkManifest ("1.0.2300", files);

    const juce::File staging = makeTempFile ("updplan");
    juce::String err;
    CHECK (scupd::writeUpdatePlan (staging, m, err), "plan written: " + err);

    const juce::File plan = scupd::updatePlanFile (staging);
    CHECK (plan.existsAsFile() && plan.getFullPathName() == staging.getChildFile ("update-plan.txt").getFullPathName(),
           "plan path is <staging>/update-plan.txt");

    // Exactly what spectrocraft-updater's readPlan() expects.
    const juce::String text = plan.loadFileAsString();
    const juce::StringArray lines = juce::StringArray::fromLines (text);
    int contentLines = 0;
    for (auto& l : lines) if (l.trim().isNotEmpty()) ++contentLines;
    CHECK (contentLines == 1 + 1 + (int) files.size(), "plan: format line, version, one line per file");
    CHECK (lines[0] == "spectrocraft-update-plan/1", "plan: format line");
    CHECK (lines[1] == "version=1.0.2300", "plan: version line");
    CHECK (lines[2] == "file=SpectroCraft.exe" && lines[3] == "file=SpectroCraft/spectrocraft-updater.exe",
           "plan: file lines in manifest order");

    // update-result.json, written by the updater (this text is its own output
    // format) and read back by the app on the next start.
    const juce::File install = makeTempFile ("updresult");
    install.createDirectory();
    install.getChildFile ("update-result.json").replaceWithText (R"({
  "format": "spectrocraft-update-result/1",
  "success": true,
  "version": "1.0.2300",
  "reason": "",
  "failedFile": "",
  "replaced": ["SpectroCraft.exe", "SpectroCraft/spectrocraft-updater.exe"],
  "oldFiles": ["C:\\Program Files\\SpectroCraft\\SpectroCraft.exe.old"],
  "cleanupDirs": ["C:\\Temp\\SpectroCraft\\update\\staging"]
})");

    scupd::UpdateResult res;
    CHECK (scupd::readUpdateResult (install, res), "result read");
    CHECK (res.success && res.version == "1.0.2300" && res.reason.isEmpty()
               && res.replaced.size() == 2 && res.oldFiles.size() == 1 && res.cleanupDirs.size() == 1,
           "result fields read");
    CHECK (! install.getChildFile ("update-result.json").existsAsFile(),
           "result file is removed (a restart never reports the same update twice)");
    CHECK (! scupd::readUpdateResult (install, res), "a second read reports nothing");

    // A failed update.
    install.getChildFile ("update-result.json").replaceWithText (R"({
  "format": "spectrocraft-update-result/1",
  "success": false,
  "version": "1.0.2300",
  "reason": "could not install: SpectroCraft.exe",
  "failedFile": "SpectroCraft.exe",
  "replaced": [], "oldFiles": [], "cleanupDirs": ["C:\\Temp\\staging"]
})");
    CHECK (scupd::readUpdateResult (install, res) && ! res.success
               && res.failedFile == "SpectroCraft.exe"
               && res.reason.contains ("could not install"),
           "a failed result is read with its reason");

    // Garbage in the file is still a report, not a silent nothing.
    install.getChildFile ("update-result.json").replaceWithText ("{ not json");
    CHECK (scupd::readUpdateResult (install, res) && ! res.success && res.reason.isNotEmpty(),
           "an unreadable result still reports a failure");

    CHECK (scupd::updateResultFile (install).getFullPathName()
               == install.getChildFile ("update-result.json").getFullPathName(),
           "the result path is <install>/update-result.json (what the updater writes)");
}

// ---------------------------------------------------------------------------
// The install folder, the temporary folder, writability
// ---------------------------------------------------------------------------
static void updTest_folders()
{
    // The update never goes through the user's SpectroCraft data folder
    // (settings, models, caches): it is the install folder and the temp folder.
    const juce::File userData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                    .getChildFile ("SpectroCraft");
    const juce::File tmp = scupd::updateTempDir();
    CHECK (! tmp.getFullPathName().startsWith (userData.getFullPathName()),
           "the update staging folder is outside the user's SpectroCraft data folder");
    CHECK (tmp.getFullPathName().contains ("SpectroCraft"), "the update staging folder is under SpectroCraft/update");
    CHECK (tmp.getParentDirectory() == juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("SpectroCraft"),
           "the update staging folder is in the system temp folder");

    CHECK (scupd::installDir() == juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory(),
           "installDir is the folder holding the running executable");

    const juce::File writable = makeTempFile ("updwritable");
    writable.createDirectory();
    CHECK (scupd::dirIsWritable (writable), "dirIsWritable: a normal folder is writable");
    CHECK (! writable.getChildFile ("spectrocraft-write-test.tmp").existsAsFile(),
           "dirIsWritable: the probe file is removed again");
    CHECK (! scupd::dirIsWritable (writable.getChildFile ("does-not-exist")),
           "dirIsWritable: a missing folder is not writable");

    // The updater is looked up where the manifest lists it, and elsewhere as a
    // fallback.
    const juce::File stage = makeTempFile ("updstage_updater");
    stage.getChildFile ("SpectroCraft").createDirectory();
    const juce::File up = stage.getChildFile ("SpectroCraft").getChildFile ("spectrocraft-updater.exe");
    up.replaceWithText ("updater");
    CHECK (scupd::findUpdaterBinary (stage) == up, "findUpdaterBinary: the manifest location");

    const juce::File other = makeTempFile ("updstage_updater2");
    other.getChildFile ("tools").createDirectory();
    const juce::File moved = other.getChildFile ("tools").getChildFile ("spectrocraft-updater.exe");
    moved.replaceWithText ("updater");
    CHECK (scupd::findUpdaterBinary (other) == moved, "findUpdaterBinary: found by searching the staging tree");
    CHECK (! scupd::findUpdaterBinary (stage.getChildFile ("nope")).existsAsFile(),
           "findUpdaterBinary: nothing found is an empty File");
}

// ---------------------------------------------------------------------------
// The settings key (Help > Check for Updates at Startup) is persisted
// ---------------------------------------------------------------------------
static void updTest_setting()
{
    Settings settings;
    CHECK (settings.checkUpdatesAtStartup == false, "the startup check is off by default (opt-in)");
    Settings other;
    other.checkUpdatesAtStartup = true;
    CHECK (! settings.samePersistent (other), "samePersistent notices the startup-check change (so it is saved)");
    other.checkUpdatesAtStartup = false;
    CHECK (settings.samePersistent (other), "samePersistent: equal settings are equal");
}

} // namespace scupdtest

static void runRegression_updates()
{
    scupdtest::updTest_ed25519();
    scupdtest::updTest_hashes();
    scupdtest::updTest_versions();
    scupdtest::updTest_assets();
    scupdtest::updTest_releaseJson();
    scupdtest::updTest_manifest();
    scupdtest::updTest_staging();
    scupdtest::updTest_planAndResult();
    scupdtest::updTest_folders();
    scupdtest::updTest_setting();
}
