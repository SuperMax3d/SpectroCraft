// Lot 3 (AI control) core: the unified model catalogue, the audio.cpp SHA-256
// checks, the strict device resolution and SC_MODELS_DIR. Included by
// tests/UnitTests.cpp; uses its helpers (CHECK, makeTempFile, scSkip, ...).
// No network: install / verify / remove run against local fixtures (file://
// URLs for the ModelManager engines, an injected downloader for audio.cpp).

#include "AI/Sha256.h"   // always compiled (AudioCpp.cpp verifies its downloads with it)
#if defined(SC_ENABLE_AI)
#include "AI/ModelCatalog.h"
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <thread>
#if !JUCE_WINDOWS
#  include <unistd.h>   // access (X_OK)
#endif

namespace {

std::string regCatSha (const std::string& s) { return sc::ai::sha256Hex (s.data (), s.size ()); }

// Exact bytes (replaceWithText would turn "\n" into CRLF on Windows).
void regCatWrite (const juce::File& f, const std::string& bytes)
{
    f.getParentDirectory ().createDirectory ();
    f.deleteFile ();
    juce::FileOutputStream out (f);
    out.write (bytes.data (), bytes.size ());
}

bool regCatIsHex64 (const std::string& s)
{
    if (s.size () != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// SC_MODELS_DIR for the duration of one scope (restored afterwards).
struct RegCatScopedEnv
{
    RegCatScopedEnv (const char* n, const juce::String& value) : name (n)
    {
        previous = juce::SystemStats::getEnvironmentVariable (name, {});
        hadPrevious = previous.isNotEmpty ();
        apply (value);
    }
    ~RegCatScopedEnv() { apply (hadPrevious ? previous : juce::String ()); }

    void apply (const juce::String& v)
    {
#if JUCE_WINDOWS
        _putenv_s (name, v.toRawUTF8 ());   // "" removes the variable
#else
        if (v.isEmpty ()) ::unsetenv (name);
        else              ::setenv (name, v.toRawUTF8 (), 1);
#endif
    }

    const char* name;
    juce::String previous;
    bool hadPrevious = false;
};

} // namespace

// --- audio.cpp: SHA-256 checks (every build: AudioCpp.cpp + Sha256.cpp are always compiled) ---

static void testRegAiCatAcppHashes()
{
    // Every catalogue asset carries a SHA-256 and an exact size.
    bool allHashed = true;
    for (const auto& b : sc::acpp::binaryChoices ())
        allHashed = allHashed && regCatIsHex64 (b.sha256) && regCatIsHex64 (b.exeSha256)
                    && b.sizeBytes > 0 && b.exeSizeBytes > 0;
    for (const auto& m : sc::acpp::modelChoices ())
        allHashed = allHashed && regCatIsHex64 (m.sha256) && m.sizeBytes > 0;
    for (const auto& m : sc::acpp::flashSrChoices ())
        allHashed = allHashed && regCatIsHex64 (m.sha256) && m.sizeBytes > 0;
    for (const auto& m : sc::acpp::audioSRChoices ())
        allHashed = allHashed && regCatIsHex64 (m.sha256) && m.sizeBytes > 0;
    CHECK (allHashed, "acpp hashes: every binary / model asset has a SHA-256 and a size");

    // URL lookup used by downloadFile when the caller passes no hash.
    const auto bins = sc::acpp::binaryChoices ();
    CHECK (!bins.empty () && sc::acpp::knownSha256ForUrl (bins[0].url) == bins[0].sha256
               && sc::acpp::knownSizeForUrl (bins[0].url) == bins[0].sizeBytes,
           "acpp hashes: binary URL -> published SHA-256 and size");
    const auto sam = sc::acpp::modelChoices ();
    CHECK (!sam.empty () && sc::acpp::knownSha256ForUrl (sc::acpp::modelUrl (sam[0].file).toStdString ()) == sam[0].sha256,
           "acpp hashes: SAM-Audio URL -> published SHA-256");
    CHECK (sc::acpp::knownSha256ForUrl ("https://example.invalid/x.zip").empty ()
               && sc::acpp::knownSizeForUrl ("https://example.invalid/x.zip") == 0,
           "acpp hashes: unknown URL has no hash");
    // FlashSR is pinned to a commit, not "main" (its hash must keep matching).
    const auto fsr = sc::acpp::flashSrChoices ();
    CHECK (!fsr.empty () && !sc::acpp::flashSrUrl (fsr[0].file).contains ("/raw/main/")
               && sc::acpp::flashSrUrl (fsr[0].file).contains ("38156353396fae68d10388c960b900a8fda8373f"),
           "acpp hashes: FlashSR URL pinned to a commit");

    // validateSha256 / commitDownload with a hash.
    CHECK (sc::acpp::validateSha256 ({}, "whatever").isEmpty (), "acpp sha: nothing expected -> ok");
    const std::string abc = regCatSha ("abc");
    CHECK (sc::acpp::validateSha256 (abc, juce::String (abc).toUpperCase ().toStdString ()).isEmpty (),
           "acpp sha: comparison is case-insensitive");
    CHECK (sc::acpp::validateSha256 (abc, regCatSha ("abd")).contains ("SHA-256"),
           "acpp sha: mismatch reported");

    const juce::File dir = makeTempFile ("acpp_sha");
    dir.createDirectory ();
    const juce::File part = dir.getChildFile ("m.gguf.part");
    const juce::File dest = dir.getChildFile ("m.gguf");
    regCatWrite (dest, "previous");
    regCatWrite (part, "abc");
    CHECK (sc::acpp::sha256OfFile (part) == abc, "acpp sha: sha256OfFile matches the one-shot digest");
    std::string err;
    CHECK (!sc::acpp::commitDownload (part, dest, 200, 3, 3, 3, false, &err, regCatSha ("abd"), abc),
           "acpp sha: commitDownload refuses a hash mismatch");
    CHECK (!part.exists () && dest.loadFileAsString () == "previous" && juce::String (err).contains ("SHA-256"),
           "acpp sha: mismatch deletes the .part and leaves dest untouched");
    regCatWrite (part, "abc");
    err.clear ();
    CHECK (sc::acpp::commitDownload (part, dest, 200, 3, 3, 3, false, &err, abc, abc)
               && dest.loadFileAsString () == "abc" && !part.exists (),
           "acpp sha: matching hash installs the file");

    // sha256OfFile: cancellation and unreadable files give "".
    CHECK (sc::acpp::sha256OfFile (dest, [] (float) { return false; }).empty (), "acpp sha: cancelled hash is empty");
    CHECK (sc::acpp::sha256OfFile (dir.getChildFile ("missing")).empty (), "acpp sha: missing file hash is empty");

    // installBinaryFromArchive checks the extracted executable against the
    // published hash of the variant: a fake executable is rejected and deleted.
    {
        const juce::String exeName = sc::acpp::binaryFileName ();
        const juce::File root = dir.getChildFile ("bin");
        root.createDirectory ();
        const juce::File exeSrc = dir.getChildFile (exeName);
        regCatWrite (exeSrc, "not the real audio.cpp binary");
        const juce::File zip = dir.getChildFile ("bin.zip");
        {
            juce::ZipFile::Builder zb;
            zb.addFile (exeSrc, 0, exeName);
            juce::FileOutputStream zo (zip);
            zb.writeToStream (zo, nullptr);
        }
        // A variant this platform publishes (and therefore knows the hash of).
        const juce::String knownVariant = bins.empty () ? juce::String () : juce::String (bins[0].id);
        if (knownVariant.isNotEmpty ())
        {
            err.clear ();
            CHECK (!sc::acpp::installBinaryFromArchive (zip, knownVariant, root, &err),
                   "acpp sha: extracted exe that does not match the " + knownVariant + " variant is refused");
            CHECK (!sc::acpp::binaryPath (root).exists () && sc::acpp::installedVariant (root).isEmpty ()
                       && !root.getChildFile ("audiocpp_extract.tmp").exists (),
                   "acpp sha: refused exe not installed, no variant marker, no scratch folder");

            // A working binary already in place survives a refused archive.
            regCatWrite (sc::acpp::binaryPath (root), "working binary");
            sc::acpp::setInstalledVariant ("previous", root);
            CHECK (!sc::acpp::installBinaryFromArchive (zip, knownVariant, root, &err)
                       && sc::acpp::binaryPath (root).loadFileAsString () == "working binary"
                       && sc::acpp::installedVariant (root) == "previous" && zip.existsAsFile (),
                   "acpp sha: refused archive leaves the installed binary and its variant untouched");
        }

        // Unknown variant id (no published exe hash): installed, replacing the old one.
        CHECK (sc::acpp::installBinaryFromArchive (zip, "custom", root, &err)
                   && sc::acpp::binaryPath (root).loadFileAsString () == "not the real audio.cpp binary"
                   && sc::acpp::installedVariant (root) == "custom" && !zip.exists ()
                   && !root.getChildFile ("audiocpp_extract.tmp").exists (),
               "acpp sha: accepted archive replaces the binary, records the variant, deletes the zip");

        // Neither a zip nor a tar.gz: refused before anything is touched.
        const juce::File junk = dir.getChildFile ("junk.bin");
        regCatWrite (junk, "plain text, no archive");
        err.clear ();
        CHECK (!sc::acpp::installBinaryFromArchive (junk, "custom", root, &err)
                   && juce::String (err).contains ("neither")
                   && sc::acpp::binaryPath (root).loadFileAsString () == "not the real audio.cpp binary",
               "acpp sha: an unknown archive format is refused");
    }
    dir.deleteRecursively ();
}

// --- audio.cpp .tar.gz runtimes (macOS / Linux release layout) ----------------

namespace {

// One tar header: GNU magic by default, POSIX ustar (with a name prefix) on request.
void regTarHeader (juce::MemoryOutputStream& out, const std::string& name, char type, uint64_t size,
                   const std::string& link = {}, bool posix = false, const std::string& prefix = {})
{
    unsigned char h[512] = {};
    std::memcpy (h, name.data (), std::min<size_t> (name.size (), 100));
    std::snprintf ((char*) h + 100, 8, "%07o", 0755u);
    std::snprintf ((char*) h + 108, 8, "%07o", 0u);
    std::snprintf ((char*) h + 116, 8, "%07o", 0u);
    std::snprintf ((char*) h + 124, 12, "%011llo", (unsigned long long) size);
    std::snprintf ((char*) h + 136, 12, "%011o", 0u);
    h[156] = (unsigned char) type;
    std::memcpy (h + 157, link.data (), std::min<size_t> (link.size (), 100));
    std::memcpy (h + 257, posix ? "ustar\0" "00" : "ustar  \0", 8);
    if (posix)
        std::memcpy (h + 345, prefix.data (), std::min<size_t> (prefix.size (), 155));
    std::memset (h + 148, ' ', 8);
    unsigned sum = 0;
    for (unsigned char c : h) sum += c;
    std::snprintf ((char*) h + 148, 8, "%06o", sum);
    h[155] = ' ';
    out.write (h, sizeof (h));
}

void regTarData (juce::MemoryOutputStream& out, const std::string& data)
{
    out.write (data.data (), data.size ());
    const size_t pad = (512 - data.size () % 512) % 512;
    const std::string zeros (pad, '\0');
    out.write (zeros.data (), zeros.size ());
}

void regTarFile (juce::MemoryOutputStream& out, const std::string& name, const std::string& data)
{
    regTarHeader (out, name, '0', data.size ());
    regTarData (out, data);
}

// pax "path" record: "<len> path=<value>\n", <len> counting itself.
std::string regPaxPath (const std::string& value)
{
    const std::string body = " path=" + value + "\n";
    size_t len = body.size () + 1;
    while (std::to_string (len).size () + body.size () != len) ++len;
    return std::to_string (len) + body;
}

void regTarEnd (juce::MemoryOutputStream& out)
{
    const std::string zeros (1024, '\0');
    out.write (zeros.data (), zeros.size ());
}

void regGzipTo (const juce::File& f, const void* data, size_t size)
{
    f.deleteFile ();
    juce::FileOutputStream fo (f);
    juce::GZIPCompressorOutputStream gz (fo, 6, juce::GZIPCompressorOutputStream::windowBitsGZIP);
    gz.write (data, size);
    gz.flush ();
}

// What a release archive looks like (plus the header kinds GNU tar / bsdtar write).
juce::MemoryBlock regReleaseTar (const juce::String& exeName)
{
    juce::MemoryOutputStream t;
    regTarHeader (t, "./", '5', 0);
    regTarFile (t, "./LICENSE", "license text");
    regTarHeader (t, "./tools/", '5', 0);
    regTarFile (t, "./tools/script.py", "print('skipped')");
    regTarFile (t, "./model_specs/sam_audio.json", "{}");
    regTarFile (t, "./" + exeName.toStdString (), "fake CLI executable");
    regTarFile (t, "./libggml-base.so.0", "ggml base");
    regTarFile (t, "./libggml.so.0.12.0", "ggml front end");
    regTarHeader (t, "./libggml.so.0", '2', 0, "libggml.so.0.12.0");     // symlink
    regTarHeader (t, "./libdangling.so", '2', 0, "missing.so");         // target absent: ignored
    regTarFile (t, "./audiocpp_server", "other executable, skipped");
    const std::string pax = regPaxPath ("libggml-cpu-a-name-long-enough-to-need-a-pax-header.so");
    regTarHeader (t, "./PaxHeaders/x", 'x', pax.size ());
    regTarData (t, pax);
    regTarFile (t, "./PaxHeaders/truncated-name", "pax-named lib");
    const std::string gnu = std::string ("libggml-vulkan-from-a-gnu-long-name.so") + '\0';
    regTarHeader (t, "././@LongLink", 'L', gnu.size ());
    regTarData (t, gnu);
    regTarFile (t, "./libggml-vulkan-from-a-gnu", "gnu-named lib");
    regTarFile (t, "./sub/libnested.so", "not top-level, skipped");
    regTarHeader (t, "libprefixed.so", '0', 3, {}, true, "deep");   // POSIX prefix: deep/libprefixed.so
    regTarData (t, "abc");
    regTarEnd (t);
    return t.getMemoryBlock ();
}

} // namespace

static void testRegAiCatAcppTarGz()
{
    const juce::File dir = makeTempFile ("acpp_targz");
    dir.deleteRecursively ();
    dir.createDirectory ();
    const juce::String exeName = sc::acpp::binaryFileName ();
    auto has = [] (const std::vector<juce::String>& v, const juce::String& n)
        { return std::find (v.begin (), v.end (), n) != v.end (); };

    // 1. Extraction: the executable and the top-level libraries only.
    const juce::MemoryBlock tar = regReleaseTar (exeName);
    const juce::File archive = dir.getChildFile ("release.tar.gz");
    regGzipTo (archive, tar.getData (), tar.getSize ());
    const juce::File out = dir.getChildFile ("out");
    std::string err;
    const auto names = sc::acpp::extractRuntimeFromTarGz (archive, out, &err);
    const std::vector<juce::String> expected { exeName, "libggml-base.so.0", "libggml.so.0.12.0", "libggml.so.0",
        "libggml-cpu-a-name-long-enough-to-need-a-pax-header.so", "libggml-vulkan-from-a-gnu-long-name.so" };
    bool allThere = names.size () == expected.size ();
    for (const auto& n : expected)
        allThere = allThere && has (names, n);
    CHECK (allThere, "acpp tar: executable + top-level libraries extracted (pax and GNU long names honoured) ("
                         + juce::String ((int) names.size ()) + " files" + (err.empty () ? "" : ", " + juce::String (err)) + ")");
    CHECK (out.getChildFile (exeName).loadFileAsString () == "fake CLI executable"
               && out.getChildFile ("libggml-cpu-a-name-long-enough-to-need-a-pax-header.so").loadFileAsString () == "pax-named lib"
               && out.getChildFile ("libggml-vulkan-from-a-gnu-long-name.so").loadFileAsString () == "gnu-named lib",
           "acpp tar: file contents intact");
    CHECK (out.getChildFile ("libggml.so.0").loadFileAsString () == "ggml front end",
           "acpp tar: a symlink between extracted files becomes a copy of its target");
    CHECK (!out.getChildFile ("LICENSE").exists () && !out.getChildFile ("audiocpp_server").exists ()
               && !out.getChildFile ("tools").exists () && !out.getChildFile ("model_specs").exists ()
               && !out.getChildFile ("sub").exists () && !out.getChildFile ("deep").exists ()
               && !out.getChildFile ("libdangling.so").exists () && !out.getChildFile ("libprefixed.so").exists (),
           "acpp tar: tools, specs, other executables, nested and prefixed entries, dangling links skipped");

    // 2. Refusals.
    const juce::File plain = dir.getChildFile ("plain.tar.gz");
    regCatWrite (plain, "not compressed at all");
    err.clear ();
    CHECK (sc::acpp::extractRuntimeFromTarGz (plain, dir.getChildFile ("o2"), &err).empty ()
               && juce::String (err).contains (".tar.gz"),
           "acpp tar: a file that is not gzip is refused");
    const std::string text (2000, 'x');
    const juce::File notTar = dir.getChildFile ("text.tar.gz");
    regGzipTo (notTar, text.data (), text.size ());
    err.clear ();
    CHECK (sc::acpp::extractRuntimeFromTarGz (notTar, dir.getChildFile ("o3"), &err).empty ()
               && juce::String (err).contains ("not a tar archive"),
           "acpp tar: gzip content that is not tar is refused");
    const juce::File cut = dir.getChildFile ("cut.tar.gz");
    regGzipTo (cut, tar.getData (), tar.getSize () / 2);
    err.clear ();
    CHECK (sc::acpp::extractRuntimeFromTarGz (cut, dir.getChildFile ("o4"), &err).empty ()
               && juce::String (err).contains ("truncated"),
           "acpp tar: a truncated archive is refused");
    {
        juce::MemoryOutputStream t;
        regTarFile (t, "./libggml-base.so.0", "lib only");
        regTarEnd (t);
        const juce::File noExe = dir.getChildFile ("noexe.tar.gz");
        regGzipTo (noExe, t.getData (), t.getDataSize ());
        err.clear ();
        CHECK (sc::acpp::extractRuntimeFromTarGz (noExe, dir.getChildFile ("o5"), &err).empty ()
                   && juce::String (err).contains (exeName),
               "acpp tar: an archive without the executable is refused");
    }

    // 3. Install, reinstall, remove.
    const juce::File root = dir.getChildFile ("root");
    root.createDirectory ();
    regCatWrite (root.getChildFile ("sam-audio-small-q8_0.gguf"), "model weights stay");
    err.clear ();
    CHECK (sc::acpp::installBinaryFromArchive (archive, "custom", root, &err) && !archive.exists ()
               && sc::acpp::installedVariant (root) == "custom"
               && sc::acpp::binaryPath (root).loadFileAsString () == "fake CLI executable"
               && root.getChildFile ("libggml.so.0").existsAsFile ()
               && sc::acpp::installedRuntimeLibraries (root).size () == 5
               && !root.getChildFile ("audiocpp_extract.tmp").exists (),
           "acpp tar: install puts the executable and its 5 libraries in place, deletes the archive ("
               + juce::String (err) + ")");
#if !JUCE_WINDOWS
    CHECK (access (sc::acpp::binaryPath (root).getFullPathName ().toRawUTF8 (), X_OK) == 0,
           "acpp tar: installed executable has its execute bit");
#endif
    {
        juce::MemoryOutputStream t;
        regTarFile (t, "./" + exeName.toStdString (), "second CLI");
        regTarFile (t, "./libggml-base.so.0", "second base");
        regTarEnd (t);
        const juce::File second = dir.getChildFile ("second.tar.gz");
        regGzipTo (second, t.getData (), t.getDataSize ());
        err.clear ();
        CHECK (sc::acpp::installBinaryFromArchive (second, "custom2", root, &err)
                   && sc::acpp::binaryPath (root).loadFileAsString () == "second CLI"
                   && root.getChildFile ("libggml-base.so.0").loadFileAsString () == "second base"
                   && !root.getChildFile ("libggml.so.0").exists ()
                   && !root.getChildFile ("libggml-vulkan-from-a-gnu-long-name.so").exists ()
                   && sc::acpp::installedRuntimeLibraries (root) == juce::StringArray ("libggml-base.so.0"),
               "acpp tar: a reinstall removes the previous variant's libraries");
    }
    // A damaged manifest never makes removal leave the folder.
    const juce::File outside = dir.getChildFile ("outside.txt");
    regCatWrite (outside, "keep me");
    root.getChildFile ("runtime-files.txt").replaceWithText ("libggml-base.so.0\n../outside.txt\n"
                                                             + outside.getFullPathName () + "\n" + exeName + "\n");
    CHECK (sc::acpp::installedRuntimeLibraries (root) == juce::StringArray ("libggml-base.so.0"),
           "acpp tar: manifest entries with a path or the executable name are ignored");
    CHECK (sc::acpp::removeInstalledBinary (root) && !sc::acpp::binaryPath (root).exists ()
               && !root.getChildFile ("libggml-base.so.0").exists () && !root.getChildFile ("runtime-files.txt").exists ()
               && sc::acpp::installedVariant (root).isEmpty () && outside.existsAsFile ()
               && root.getChildFile ("sam-audio-small-q8_0.gguf").existsAsFile (),
           "acpp tar: removal deletes the executable, its libraries and the marker, nothing else");

    dir.deleteRecursively ();
}

// SC_MODELS_DIR replaces the models root on every platform (Windows included).
static void testRegAiCatModelsDirEnv()
{
    const juce::File custom = makeTempFile ("sc_models_dir");
    custom.createDirectory ();
    {
        RegCatScopedEnv env ("SC_MODELS_DIR", custom.getFullPathName ());
        CHECK (sc::acpp::dir () == custom.getChildFile ("audiocpp"),
               "SC_MODELS_DIR: audio.cpp folder follows it");
#if defined(SC_ENABLE_AI)
        sc::ai::ModelManager mm;
        CHECK (mm.modelRoot () == custom, "SC_MODELS_DIR: ModelManager root follows it");
#endif
    }
#if defined(SC_ENABLE_AI)
    {
        // (ModelManager::modelRoot creates nothing, unlike sc::acpp::dir.)
        RegCatScopedEnv env ("SC_MODELS_DIR", "relative/folder");
        sc::ai::ModelManager mm;
        CHECK (!mm.modelRoot ().getFullPathName ().contains ("relative"), "SC_MODELS_DIR: a relative value is ignored");
    }
#endif
    custom.deleteRecursively ();
}

#if defined(SC_ENABLE_AI)

// Built-in catalogue: every known model, consistent fields.
static void testRegAiCatBuiltIns()
{
    const juce::File root = makeTempFile ("catalog_builtin");
    root.createDirectory ();
    sc::ai::ModelManager mm (root.getChildFile ("models").getFullPathName ().toStdString ());
    sc::ai::ModelCatalog cat (mm, root.getChildFile ("audiocpp"));
    const auto& entries = cat.entries ();

    const size_t expected = mm.registry ().size () + sc::acpp::binaryChoices ().size ()
                          + sc::acpp::modelChoices ().size () + sc::acpp::flashSrChoices ().size ()
                          + sc::acpp::audioSRChoices ().size () + 2;   // + 2 manual htdemucs families
    // 28 platform-independent entries + this platform's CLI variants (Windows 4, macOS 1, Linux x64 2).
    CHECK (entries.size () == expected && expected == 28 + sc::acpp::binaryChoices ().size (),
           "catalog: lists all " + juce::String ((int) expected) + " known models");

    for (const auto& spec : mm.registry ())
    {
        const auto* e = cat.find (spec.id);
        CHECK (e != nullptr && e->files.size () == spec.files.size ()
                   && e->engine == (spec.id.rfind ("whisper-", 0) == 0 ? "whisper" : "openvino"),
               "catalog: ModelManager model " + juce::String (spec.id) + " present with its files and engine");
    }
    for (const auto& b : sc::acpp::binaryChoices ())
        CHECK (cat.find ("audiocpp-cli-" + b.id) != nullptr, "catalog: binary " + juce::String (b.id) + " present");
    CHECK (cat.find ("sam-audio-small-q8_0") != nullptr && cat.find ("flashsr") != nullptr
               && cat.find ("audiosr-basic-f32") != nullptr && cat.find ("htdemucs-6stems") != nullptr
               && cat.find ("htdemucs-ft") != nullptr,
           "catalog: audio.cpp models present with stable ids");
    CHECK (cat.find ("no-such-model") == nullptr, "catalog: unknown id not found");

    std::set<std::string> ids;
    bool fieldsOk = true, installableHasHash = true, reasonsOk = true, acppHashed = true;
    const std::set<std::string> engines { "openvino", "whisper", "audiocpp" };
    const std::set<std::string> families { "noiseSuppress", "transcribe", "stems", "superRes", "runtime" };
    for (const auto& e : entries)
    {
        ids.insert (e.id);
        if (e.id.empty () || e.displayName.empty () || e.platforms.empty () || e.backends.empty ()
            || e.license.empty () || e.source.empty ()
            || engines.count (e.engine) == 0 || families.count (e.family) == 0)
        {
            fieldsOk = false;
            std::cout << "  bad fields: " << e.id << "\n";
        }
        if (e.installable)
        {
            std::vector<sc::ai::CatalogFile> fs = e.files;
            if (e.family == "runtime") fs.push_back (e.archive);
            for (const auto& f : fs)
                if (!regCatIsHex64 (f.sha256) || f.sizeBytes <= 0) installableHasHash = false;
            const auto& dl = e.family == "runtime" ? std::vector<sc::ai::CatalogFile> { e.archive } : e.files;
            for (const auto& f : dl)
                if (f.url.empty ()) installableHasHash = false;
            if (!e.reason.empty ()) reasonsOk = false;
        }
        else if (e.reason.empty () || e.reasonCode.empty ())
            reasonsOk = false;
        if (e.engine == "audiocpp" && !e.manual && e.checksumKind () != "sha256")
            acppHashed = false;
    }
    CHECK (ids.size () == entries.size (), "catalog: ids are unique");
    CHECK (fieldsOk, "catalog: every entry has id, engine, family, name, platforms, backends, license, source");
    CHECK (installableHasHash, "catalog: installable implies a SHA-256, a size and a URL for every download");
    CHECK (reasonsOk, "catalog: a non-installable entry says why (and an installable one does not)");
    CHECK (acppHashed, "catalog: every downloadable audio.cpp asset is SHA-256 checked");

#if JUCE_WINDOWS
    const auto* cuda = cat.find ("audiocpp-cli-cuda12.4");
    CHECK (cuda != nullptr && !cuda->installable && cuda->reasonCode == "blocked",
           "catalog: CUDA binaries blocked (their ggml DLLs are not extracted)");
#endif
    const auto* sam = cat.find ("sam-audio-small-q8_0");
    const auto bins = sc::acpp::binaryChoices ();
    if (sc::acpp::platformSupported ())
    {
        const auto* firstBin = cat.find ("audiocpp-cli-" + bins[0].id);
        CHECK (sam != nullptr && sam->installable && firstBin != nullptr && firstBin->installable
                   && firstBin->platforms == std::vector<std::string> { sc::acpp::platformTag () },
               "catalog: audio.cpp model + " + juce::String (bins[0].id) + " binary installable on "
                   + juce::String (sc::acpp::platformTag ()));
    }
    else
    {
        CHECK (sam != nullptr && !sam->installable && sam->reasonCode == "unsupported_platform",
               "catalog: audio.cpp entries not installable without a CLI build (unsupported_platform)");
    }
    const auto* demucs = cat.find ("htdemucs-6stems");
    CHECK (demucs != nullptr && demucs->manual && !demucs->installable && demucs->checksumKind () == "none",
           "catalog: htdemucs is manual, not installable, no checksum");
    const auto* wb = cat.find ("whisper-base");
    CHECK (wb != nullptr && wb->installable && wb->family == "transcribe" && wb->sizeBytes () == 147951465
               && wb->checksumKind () == "sha256",
           "catalog: whisper-base fields");

    // Nothing installed in the private roots.
    bool noneInstalled = true;
    for (const auto& e : entries)
        if (cat.status (e).installed) noneInstalled = false;
    CHECK (noneInstalled, "catalog: nothing installed in an empty models folder");
    root.deleteRecursively ();
}

// JSON shapes (REPORT §6.5): ai.models.list and ai.devices.
static void testRegAiCatJson()
{
    const juce::File root = makeTempFile ("catalog_json");
    root.createDirectory ();
    sc::ai::ModelManager mm (root.getChildFile ("models").getFullPathName ().toStdString ());
    sc::ai::ModelCatalog cat (mm, root.getChildFile ("audiocpp"));

    // Through text and back: what a client actually receives.
    const juce::var list = juce::JSON::parse (juce::JSON::toString (cat.listJson (), true));
    CHECK (list.getProperty ("modelsRoot", {}).toString () == mm.modelRoot ().getFullPathName ()
               && list.getProperty ("audiocppRoot", {}).isString (),
           "catalog json: modelsRoot / audiocppRoot");
    const auto* models = list.getProperty ("models", {}).getArray ();
    CHECK (models != nullptr && (size_t) models->size () == cat.entries ().size (), "catalog json: one element per entry");

    bool shapeOk = true;
    if (models != nullptr)
    {
        for (const auto& m : *models)
        {
            const bool installed = (bool) m.getProperty ("installed", {});
            const bool ok = m.getProperty ("id", {}).isString () && m.getProperty ("engine", {}).isString ()
                && m.getProperty ("family", {}).isString () && m.getProperty ("displayName", {}).isString ()
                && m.getProperty ("installed", {}).isBool () && m.getProperty ("verified", {}).isBool ()
                && (m.getProperty ("bytesOnDisk", {}).isInt () || m.getProperty ("bytesOnDisk", {}).isInt64 ())
                && (m.getProperty ("sizeBytes", {}).isInt () || m.getProperty ("sizeBytes", {}).isInt64 ())
                && m.getProperty ("path", {}).isString ()
                && m.getProperty ("checksum", {}).isString ()
                && m.getProperty ("backends", {}).isArray () && m.getProperty ("platforms", {}).isArray ()
                && m.getProperty ("installable", {}).isBool ()
                && m.getProperty ("files", {}).isArray ()
                && (installed || m.getProperty ("missing", {}).isArray ())
                && ((bool) m.getProperty ("installable", {}) || m.getProperty ("reason", {}).isString ());
            if (!ok)
            {
                shapeOk = false;
                std::cout << "  bad json: " << juce::JSON::toString (m, true) << "\n";
            }
        }
    }
    CHECK (shapeOk, "catalog json: every element has the §6.5 fields");

    auto findModel = [&] (const juce::String& id) -> juce::var
    {
        if (models != nullptr)
            for (const auto& m : *models)
                if (m.getProperty ("id", {}).toString () == id) return m;
        return {};
    };
    const juce::var wb = findModel ("whisper-base");
    const auto* missing = wb.getProperty ("missing", {}).getArray ();
    CHECK (wb.getProperty ("engine", {}).toString () == "whisper" && wb.getProperty ("family", {}).toString () == "transcribe"
               && (juce::int64) wb.getProperty ("sizeBytes", {}) == 147951465
               && wb.getProperty ("checksum", {}).toString () == "sha256"
               && missing != nullptr && missing->size () == 1 && (*missing)[0].toString () == "ggml-base.bin",
           "catalog json: whisper-base matches the §6.5 example");
    const juce::var big = findModel ("sam-audio-large-f32");
    CHECK ((juce::int64) big.getProperty ("sizeBytes", {}) == (juce::int64) 15401675328LL,
           "catalog json: sizes above 4 GB survive the round trip");
    const juce::var samQ = findModel ("sam-audio-small-q8_0");
    const auto* samPlatforms = samQ.getProperty ("platforms", {}).getArray ();
    CHECK (samQ.getProperty ("checksum", {}).toString () == "sha256" && samPlatforms != nullptr
               && samPlatforms->size () == 4 && samPlatforms->contains ("windows-x64")
               && samPlatforms->contains ("macos-arm64") && samPlatforms->contains ("linux-x64"),
           "catalog json: audio.cpp model carries sha256 checksum + every CLI platform");
    const auto bins = sc::acpp::binaryChoices ();
    if (!bins.empty ())
    {
        const juce::var firstBin = findModel ("audiocpp-cli-" + bins[0].id);
        CHECK (firstBin.getProperty ("variant", {}).toString () == juce::String (bins[0].id)
                   && firstBin.getProperty ("download", {}).getProperty ("sha256", {}).toString ().length () == 64,
               "catalog json: runtime entry exposes its variant and its download hash");
    }

    // ai.devices
    const juce::var dev = juce::JSON::parse (juce::JSON::toString (sc::ai::devicesJson ("auto", false, root.getChildFile ("audiocpp")), true));
    const juce::var ov = dev.getProperty ("openvino", {});
    const bool ovShape = ov.getProperty ("preference", {}).toString () == "auto"
        && ov.getProperty ("initialised", {}).isBool ()
        && (! (bool) ov.getProperty ("initialised", {})
                ? ov.getProperty ("devices", {}).isVoid ()
                : (ov.getProperty ("devices", {}).isArray () && ov.getProperty ("resolved", {}).isString ()));
    CHECK (ovShape, "devices json: openvino block (devices null until initialised)");
    const juce::var wh = dev.getProperty ("whisper", {});
    CHECK (wh.getProperty ("devices", {}).getArray () != nullptr && wh.getProperty ("devices", {}).size () == 1
               && wh.getProperty ("devices", {})[0].toString () == "CPU",
           "devices json: whisper = [CPU]");
    const juce::var ac = dev.getProperty ("audiocpp", {});
    if (sc::acpp::platformSupported ())
        CHECK (ac.getProperty ("available", {}).isBool () && ac.getProperty ("backends", {}).isArray ()
                   && ac.getProperty ("platform", {}).toString () == juce::String (sc::acpp::platformTag ()),
               "devices json: audiocpp block");
    else
        CHECK (! (bool) ac.getProperty ("available", {}) && ac.getProperty ("reason", {}).toString () == "unsupported_platform"
                   && ac.getProperty ("installedVariant", {}).isVoid (),
               "devices json: audiocpp unsupported without a CLI build");
    root.deleteRecursively ();
}

// Strict device resolution: an unavailable device is an error, never a fallback.
static void testRegAiCatResolveDevice()
{
    using sc::ai::resolveAiDevice;
    const std::vector<std::string> cpu { "CPU" };

    auto r = resolveAiDevice ("openvino", "auto", cpu);
    CHECK (r.ok && r.device == "CPU", "device: auto + CPU only -> CPU");
    r = resolveAiDevice ("openvino", "", cpu);
    CHECK (r.ok && r.device == "CPU" && r.requested == "auto", "device: empty = auto");
    CHECK (resolveAiDevice ("openvino", "AUTO", { "CPU", "GPU" }).device == "GPU", "device: auto prefers GPU");
    CHECK (resolveAiDevice ("openvino", "auto", { "CPU", "NPU" }).device == "NPU", "device: auto prefers NPU over CPU");
    CHECK (resolveAiDevice ("openvino", "cpu", cpu).device == "CPU", "device: case-insensitive");
    r = resolveAiDevice ("openvino", "gpu", { "CPU", "GPU.0", "GPU.1" });
    CHECK (r.ok && r.device == "GPU", "device: GPU matches indexed GPUs");
    r = resolveAiDevice ("openvino", "GPU.1", { "CPU", "GPU.0", "GPU.1" });
    CHECK (r.ok && r.device == "GPU.1", "device: an indexed GPU");
    CHECK (resolveAiDevice ("openvino", "auto", { "CPU", "GPU.0" }).device == "GPU", "device: auto with indexed GPU");

    r = resolveAiDevice ("openvino", "GPU", cpu);
    CHECK (!r.ok && r.code == "invalid_params" && r.message == "device GPU is not available for openvino",
           "device: unavailable GPU is an explicit error");
    const juce::var ej = r.errorJson ();
    CHECK (ej.getProperty ("code", {}).toString () == "invalid_params"
               && ej.getProperty ("details", {}).getProperty ("path", {}).toString () == "device"
               && ej.getProperty ("details", {}).getProperty ("expected", {}).toString () == "one of [auto, CPU]",
           "device: error json matches §6.5");
    r = resolveAiDevice ("openvino", "TPU", cpu);
    CHECK (!r.ok && r.code == "invalid_params" && juce::String (r.message).contains ("unknown device"),
           "device: unknown name rejected");
    r = resolveAiDevice ("openvino", "auto", {});
    CHECK (!r.ok && r.code == "failed", "device: no runtime -> failed, not CPU");

    // No silent fallback, whatever is asked.
    bool noFallback = true;
    for (const char* req : { "CPU", "GPU", "NPU" })
    {
        const auto x = resolveAiDevice ("openvino", req, cpu);
        if (x.ok != (std::string (req) == "CPU") || (x.ok && x.device != req)) noFallback = false;
    }
    CHECK (noFallback, "device: a specific device is honoured exactly or refused");

    CHECK (resolveAiDevice ("whisper", "auto", { "CPU" }).device == "CPU", "device: whisper auto -> CPU");
    r = resolveAiDevice ("whisper", "GPU", { "CPU" });
    CHECK (!r.ok && r.code == "invalid_params", "device: whisper GPU refused (use_gpu=false)");

    CHECK (resolveAiDevice ("audiocpp", "auto", { "cuda" }).device == "cuda", "device: audiocpp auto -> installed backend");
    CHECK (resolveAiDevice ("audiocpp", "CUDA", { "cuda" }).device == "cuda", "device: audiocpp names are lower-case");
    CHECK (!resolveAiDevice ("audiocpp", "vulkan", { "cuda" }).ok, "device: audiocpp backend of another variant refused");
    CHECK (resolveAiDevice ("audiocpp", "auto", { "cpu", "cuda", "vulkan" }).device == "cpu",
           "device: audiocpp auto on an unknown build -> cpu");
    r = resolveAiDevice ("tensorrt", "auto", cpu);
    CHECK (!r.ok && r.code == "invalid_params" && r.path == "engine", "device: unknown engine");

    const std::vector<std::string> metalCpu { "metal", "cpu" };
#if JUCE_MAC
    const bool handPlacedOk = sc::ai::audioCppBackendsForVariant ("") == metalCpu;
#else
    const bool handPlacedOk = sc::ai::audioCppBackendsForVariant ("").size () == 3;
#endif
    CHECK (sc::ai::audioCppBackendsForVariant ("cpu") == std::vector<std::string> { "cpu" }
               && sc::ai::audioCppBackendsForVariant ("cuda13.3") == std::vector<std::string> { "cuda" }
               && sc::ai::audioCppBackendsForVariant ("vulkan") == std::vector<std::string> { "vulkan" }
               && sc::ai::audioCppBackendsForVariant ("metal") == metalCpu && handPlacedOk,
           "device: audio.cpp variant -> backends");

    const auto okJson = resolveAiDevice ("openvino", "cpu", cpu).toJson ();
    CHECK (okJson.getProperty ("engine", {}).toString () == "openvino" && okJson.getProperty ("device", {}).toString () == "CPU"
               && okJson.getProperty ("resolved", {}).toString () == "CPU",
           "device: setDevice result json");

    // Against the real lists (whisper needs no runtime).
    CHECK (resolveAiDevice ("whisper", "auto").ok, "device: real whisper list");
    if (!sc::acpp::platformSupported ())
        CHECK (resolveAiDevice ("audiocpp", "cpu").code == "unsupported_platform", "device: audio.cpp unsupported without a CLI build");
}

// install / verify / remove through ModelManager, from file:// fixtures.
static void testRegAiCatInstallVerifyRemove()
{
    const juce::File root = makeTempFile ("catalog_ivr");
    root.deleteRecursively ();
    const juce::File src = root.getChildFile ("source");
    const std::string a = "SpectroCraft catalogue fixture A";
    const std::string b = "SpectroCraft catalogue fixture B, a bit longer";
    regCatWrite (src.getChildFile ("a.bin"), a);
    regCatWrite (src.getChildFile ("b.bin"), b);
    auto urlOf = [] (const juce::File& f) { return juce::URL (f).toString (false).toStdString (); };

    sc::ai::CatalogEntry e;
    e.id = "catalog-fixture";
    e.engine = "openvino";
    e.family = "noiseSuppress";
    e.displayName = "Catalogue fixture";
    e.folder = "catalog-fixture";
    e.files = { { "a.bin", urlOf (src.getChildFile ("a.bin")), (int64_t) a.size (), regCatSha (a) },
                { "b.bin", urlOf (src.getChildFile ("b.bin")), (int64_t) b.size (), regCatSha (b) } };
    e.backends = { "CPU" };
    e.platforms = { "windows", "macos", "linux" };
    e.license = "test"; e.source = "test";
    e.installable = true;

    sc::ai::CatalogEntry badHash = e;
    badHash.id = badHash.folder = "catalog-fixture-badhash";
    badHash.files[1].sha256 = regCatSha ("something else");

    sc::ai::CatalogEntry blocked = e;
    blocked.id = blocked.folder = "catalog-fixture-blocked";
    blocked.installable = false;
    blocked.reason = "test reason";
    blocked.reasonCode = "blocked";

    sc::ai::ModelManager mm (root.getChildFile ("models").getFullPathName ().toStdString ());
    sc::ai::ModelCatalog cat (mm, root.getChildFile ("audiocpp"), { e, badHash, blocked });
    const auto& entry = *cat.find ("catalog-fixture");
    const juce::File dir = cat.locationOf (entry);
    CHECK (dir == mm.dirFor ("catalog-fixture"), "catalog ivr: location = ModelManager::dirFor");

    auto st = cat.status (entry);
    CHECK (!st.installed && st.missing.size () == 2 && st.bytesOnDisk == 0, "catalog ivr: not installed at first");
    auto plan = cat.installPlan ("catalog-fixture");
    CHECK (plan.ok && (juce::int64) plan.result.getProperty ("downloadBytes", {}) == (juce::int64) (a.size () + b.size ()),
           "catalog ivr: install plan sizes");
    CHECK (cat.verify ("catalog-fixture").code == "model_not_installed", "catalog ivr: verify before install");

    // Cancelled install leaves nothing behind.
    auto out = cat.install ("catalog-fixture", [] (float, const std::string&) { return false; });
    bool partLeft = false;
    if (dir.isDirectory ())
        for (const auto& f : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFiles))
            partLeft = partLeft || f.getFile ().getFileName ().endsWith (".part") || f.getFile ().getFileName () == "a.bin";
    CHECK (!out.ok && out.code == "cancelled" && !partLeft, "catalog ivr: cancel -> cancelled, no partial file");

    int calls = 0;
    std::string lastStatus;
    out = cat.install ("catalog-fixture", [&] (float, const std::string& s) { ++calls; lastStatus = s; return true; });
    CHECK (out.ok && (bool) out.result.getProperty ("installed", {}) && (bool) out.result.getProperty ("verified", {})
               && out.result.getProperty ("modelId", {}).toString () == "catalog-fixture",
           "catalog ivr: install ok");
    CHECK (calls > 0 && !lastStatus.empty (), "catalog ivr: install reports progress");
    st = cat.status (entry);
    CHECK (st.installed && st.verified && st.bytesOnDisk == (int64_t) (a.size () + b.size ())
               && dir.getChildFile ("b.bin").loadFileAsString ().toStdString () == b,
           "catalog ivr: installed + verified after install");
    const juce::var ej = cat.entryJson (entry);
    CHECK ((bool) ej.getProperty ("installed", {}) && (bool) ej.getProperty ("verified", {})
               && !ej.hasProperty ("missing"),
           "catalog ivr: entry json reflects the install");

    out = cat.verify ("catalog-fixture");
    const auto* files = out.result.getProperty ("files", {}).getArray ();
    CHECK (out.ok && (bool) out.result.getProperty ("verified", {}) && files != nullptr && files->size () == 2
               && (bool) (*files)[0].getProperty ("sha256Ok", {}) && (bool) (*files)[1].getProperty ("sha256Ok", {}),
           "catalog ivr: verify ok");

    // Same size, different bytes: verify reports it (still ok:true).
    std::string corrupt = b;
    corrupt[3] = (char) (corrupt[3] ^ 0x20);
    regCatWrite (dir.getChildFile ("b.bin"), corrupt);
    out = cat.verify ("catalog-fixture");
    files = out.result.getProperty ("files", {}).getArray ();
    CHECK (out.ok && ! (bool) out.result.getProperty ("verified", {}) && files != nullptr && files->size () == 2
               && (bool) (*files)[0].getProperty ("sha256Ok", {}) && ! (bool) (*files)[1].getProperty ("sha256Ok", {}),
           "catalog ivr: verify flags the corrupt file");
    CHECK (!cat.status (entry).verified, "catalog ivr: corrupt file no longer counted as verified");

    // Re-install repairs it (ensureInstalled deletes the corrupt file; the next run fetches it).
    out = cat.install ("catalog-fixture");
    if (!out.ok) out = cat.install ("catalog-fixture");
    CHECK (out.ok && cat.verify ("catalog-fixture").result.getProperty ("verified", {}) == juce::var (true),
           "catalog ivr: install repairs a corrupt file");

    // Busy: a second operation on the same id while one runs.
    {
        cat.remove ("catalog-fixture");
        std::atomic<bool> inside { false };
        juce::WaitableEvent release (true);   // manual reset: every later progress call passes
        sc::ai::CatalogOutcome threaded;
        std::thread t ([&] {
            threaded = cat.install ("catalog-fixture", [&] (float, const std::string&)
                                    { inside = true; release.wait (10000); return true; });
        });
        const auto t0 = juce::Time::getMillisecondCounter ();
        while (!inside && juce::Time::getMillisecondCounter () - t0 < 10000)
            juce::Thread::sleep (5);
        CHECK (inside && cat.isBusy ("catalog-fixture"), "catalog ivr: busy while installing");
        CHECK (cat.verify ("catalog-fixture").code == "busy" && cat.remove ("catalog-fixture").code == "busy"
                   && cat.installPlan ("catalog-fixture").code == "busy"
                   && cat.entryJson (entry).getProperty ("busy", {}) == juce::var (true),
               "catalog ivr: concurrent verify / remove / install refused with busy");
        CHECK (cat.verify ("catalog-fixture-badhash").code != "busy", "catalog ivr: busy is per model");
        release.signal ();
        t.join ();
        CHECK (threaded.ok && !cat.isBusy ("catalog-fixture"), "catalog ivr: threaded install finished, not busy");
    }

    // Removal: preview, delete, idempotent.
    auto prev = cat.removalPreview ("catalog-fixture");
    CHECK (prev.ok && (juce::int64) prev.result.getProperty ("bytesOnDisk", {}) == (juce::int64) (a.size () + b.size ())
               && prev.result.getProperty ("message", {}).toString ().startsWith ("Removes "),
           "catalog ivr: removal preview");
    out = cat.remove ("catalog-fixture");
    CHECK (out.ok && (bool) out.result.getProperty ("removed", {})
               && (juce::int64) out.result.getProperty ("freedBytes", {}) == (juce::int64) (a.size () + b.size ())
               && !dir.exists () && mm.modelRoot ().isDirectory (),
           "catalog ivr: remove deletes the model folder only");
    out = cat.remove ("catalog-fixture");
    CHECK (out.ok && ! (bool) out.result.getProperty ("removed", {}), "catalog ivr: remove is idempotent");

    // A wrong published hash: refused, nothing installed.
    out = cat.install ("catalog-fixture-badhash");
    CHECK (!out.ok && out.code == "failed" && juce::String (out.message).contains ("SHA-256")
               && !cat.status (*cat.find ("catalog-fixture-badhash")).installed,
           "catalog ivr: hash mismatch refused");
    out = cat.install ("catalog-fixture-blocked");
    CHECK (!out.ok && out.code == "failed" && out.details.getProperty ("reason", {}).toString () == "test reason",
           "catalog ivr: non-installable entry refused with its reason");

    for (const char* op : { "install", "verify", "remove" })
    {
        const std::string o (op);
        const auto res = o == "install" ? cat.install ("nope") : o == "verify" ? cat.verify ("nope") : cat.remove ("nope");
        CHECK (!res.ok && res.code == "invalid_params"
                   && res.errorJson ().getProperty ("details", {}).getProperty ("path", {}).toString () == "modelId",
               "catalog ivr: unknown id -> invalid_params (" + juce::String (op) + ")");
    }
    root.deleteRecursively ();
}

// audio.cpp install path (normally Windows-only) with an injected downloader.
static void testRegAiCatAudioCppInstall()
{
    const juce::File root = makeTempFile ("catalog_acpp");
    root.deleteRecursively ();
    const juce::File src = root.getChildFile ("source");
    const juce::File acppRoot = root.getChildFile ("audiocpp");

    const std::string weights = "fake gguf weights for the catalogue test";
    regCatWrite (src.getChildFile ("fixture.gguf"), weights);
    const juce::String exeName = sc::acpp::binaryFileName ();
    const std::string exeBytes = "fake audio.cpp CLI for the catalogue test";
    regCatWrite (src.getChildFile (exeName), exeBytes);
    const juce::File zip = src.getChildFile ("bin.zip");
    {
        juce::ZipFile::Builder zb;
        zb.addFile (src.getChildFile (exeName), 0, exeName);
        juce::FileOutputStream zo (zip);
        zb.writeToStream (zo, nullptr);
    }
    const std::string zipSha = sc::acpp::sha256OfFile (zip);

    sc::ai::CatalogEntry m;
    m.id = "acpp-fixture";
    m.engine = "audiocpp";
    m.family = "stems";
    m.displayName = "audio.cpp fixture";
    m.files = { { "Fixture-GGUF/fixture.gguf", "https://example.invalid/fixture.gguf", (int64_t) weights.size (), regCatSha (weights) } };
    m.backends = { "cpu" }; m.platforms = { sc::acpp::platformTag () }; m.license = "test"; m.source = "test";
    m.installable = true;

    sc::ai::CatalogEntry rt;
    rt.id = "audiocpp-cli-test";
    rt.engine = "audiocpp";
    rt.family = "runtime";
    rt.displayName = "audio.cpp CLI fixture";
    rt.variant = "test";
    rt.files = { { exeName.toStdString (), {}, (int64_t) exeBytes.size (), regCatSha (exeBytes) } };
    rt.archive = { "audiocpp_bin.zip", "https://example.invalid/bin.zip", zip.getSize (), zipSha };
    rt.backends = { "cpu" }; rt.platforms = { sc::acpp::platformTag () }; rt.license = "test"; rt.source = "test";
    rt.installable = true;

    sc::ai::ModelManager mm (root.getChildFile ("models").getFullPathName ().toStdString ());
    sc::ai::CatalogEntry rt2 = rt;   // a second variant: same executable slot
    rt2.id = "audiocpp-cli-test2";
    rt2.variant = "test2";
    sc::ai::ModelCatalog cat (mm, acppRoot, { m, rt, rt2 });

    std::map<std::string, juce::File> sources { { m.files[0].url, src.getChildFile ("fixture.gguf") },
                                                { rt.archive.url, zip } };
    int downloads = 0;
    cat.setAudioCppDownloaderForTests ([&] (const std::string& url, const juce::File& dest,
                                            const std::function<bool(float)>& progress, std::string* errOut,
                                            int64_t expectedBytes, const std::string& expectedSha) -> bool
    {
        ++downloads;
        const auto it = sources.find (url);
        if (it == sources.end ()) { if (errOut) *errOut = "no fixture for " + url; return false; }
        const juce::File part = dest.getSiblingFile (dest.getFileName () + ".part");
        part.deleteFile ();
        if (!it->second.copyFileTo (part)) { if (errOut) *errOut = "copy failed"; return false; }
        if (progress && !progress (1.0f)) { part.deleteFile (); return false; }
        const int64_t got = part.getSize ();
        // Same final step as the real Windows downloader.
        return sc::acpp::commitDownload (part, dest, 200, got, expectedBytes, got, false, errOut,
                                         expectedSha, sc::acpp::sha256OfFile (part));
    });

    auto out = cat.install ("acpp-fixture");
    const juce::File weightsFile = acppRoot.getChildFile ("Fixture-GGUF/fixture.gguf");
    CHECK (out.ok && weightsFile.loadFileAsString ().toStdString () == weights && downloads == 1,
           "catalog acpp: model downloaded into its subfolder");
    CHECK (cat.status (*cat.find ("acpp-fixture")).installed && cat.status (*cat.find ("acpp-fixture")).verified
               && cat.status (*cat.find ("acpp-fixture")).path == weightsFile,
           "catalog acpp: model installed + verified, path = the file");
    out = cat.install ("acpp-fixture");
    CHECK (out.ok && downloads == 1, "catalog acpp: already installed -> no second download");

    // The CLI variants share one slot: installing one makes the others busy.
    {
        std::atomic<bool> inside { false };
        juce::WaitableEvent release (true);
        std::thread t ([&] {
            out = cat.install ("audiocpp-cli-test", [&] (float, const std::string&)
                               { inside = true; release.wait (10000); return true; });
        });
        const auto t0 = juce::Time::getMillisecondCounter ();
        while (!inside && juce::Time::getMillisecondCounter () - t0 < 10000)
            juce::Thread::sleep (5);
        CHECK (inside && cat.isBusy ("audiocpp-cli-test2") && cat.remove ("audiocpp-cli-test2").code == "busy"
                   && !cat.isBusy ("acpp-fixture"),
               "catalog acpp: CLI variants share one busy slot");
        release.signal ();
        t.join ();
    }
    const juce::File exe = sc::acpp::binaryPath (acppRoot);
    CHECK (out.ok && exe.loadFileAsString ().toStdString () == exeBytes
               && sc::acpp::installedVariant (acppRoot) == "test"
               && !acppRoot.getChildFile ("audiocpp_bin.zip").exists ()
               && out.result.getProperty ("variant", {}).toString () == "test",
           "catalog acpp: runtime zip downloaded, exe extracted, variant recorded, zip deleted");
    out = cat.verify ("audiocpp-cli-test");
    CHECK (out.ok && (bool) out.result.getProperty ("verified", {}), "catalog acpp: runtime verifies");

    // A tampered executable is detected by verify.
    regCatWrite (exe, std::string (exeBytes.size (), 'x'));
    out = cat.verify ("audiocpp-cli-test");
    CHECK (out.ok && ! (bool) out.result.getProperty ("verified", {}), "catalog acpp: tampered exe detected");

    // Removal deletes exactly the entry's files (and the now-empty subfolder).
    out = cat.remove ("acpp-fixture");
    CHECK (out.ok && (bool) out.result.getProperty ("removed", {}) && !weightsFile.exists ()
               && !acppRoot.getChildFile ("Fixture-GGUF").exists () && acppRoot.isDirectory () && exe.exists (),
           "catalog acpp: model removed, subfolder pruned, binary kept");
    out = cat.remove ("audiocpp-cli-test");
    CHECK (out.ok && (bool) out.result.getProperty ("removed", {}) && !exe.exists ()
               && sc::acpp::installedVariant (acppRoot).isEmpty (),
           "catalog acpp: runtime removed with its variant marker");

    // A download whose bytes do not match the published hash is refused.
    std::string wrong = weights;
    wrong[0] = (char) (wrong[0] ^ 0x01);   // same size, other bytes
    regCatWrite (src.getChildFile ("wrong.gguf"), wrong);
    sources[m.files[0].url] = src.getChildFile ("wrong.gguf");
    out = cat.install ("acpp-fixture");
    CHECK (!out.ok && out.code == "failed" && juce::String (out.message).contains ("SHA-256") && !weightsFile.exists (),
           "catalog acpp: hash mismatch refused, nothing installed");

    // The built-in audio.cpp entries without the hook: wherever the CLI has a build.
    sc::ai::ModelCatalog builtIn (mm, acppRoot);
    if (sc::acpp::platformSupported ())
        CHECK (builtIn.installPlan ("sam-audio-small-q8_0").ok,
               "catalog acpp: built-in model installable on " + juce::String (sc::acpp::platformTag ()));
    else
        CHECK (builtIn.install ("sam-audio-small-q8_0").code == "unsupported_platform",
               "catalog acpp: built-in model install -> unsupported_platform without a CLI build");
    root.deleteRecursively ();
}

#endif   // SC_ENABLE_AI

static void runRegression_ai_catalog()
{
    testRegAiCatAcppHashes ();
    testRegAiCatAcppTarGz ();
    testRegAiCatModelsDirEnv ();
#if defined(SC_ENABLE_AI)
    testRegAiCatBuiltIns ();
    testRegAiCatJson ();
    testRegAiCatResolveDevice ();
    testRegAiCatInstallVerifyRemove ();
    testRegAiCatAudioCppInstall ();
#else
    scSkip ("regression ai_catalog: ModelCatalog tests need an AI build (SPECTRO_ENABLE_AI)");
#endif
}
