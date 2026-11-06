// SpectroCraft — unified AI model catalogue. See include/AI/ModelCatalog.h.

#include "AI/ModelCatalog.h"
#include "AI/ModelManager.h"
#include "AI/OvRuntime.h"
#include "AudioCpp.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace sc { namespace ai {

namespace {

constexpr const char* kErrInvalidParams       = "invalid_params";
constexpr const char* kErrBusy                = "busy";
constexpr const char* kErrCancelled           = "cancelled";
constexpr const char* kErrUnsupportedPlatform = "unsupported_platform";
constexpr const char* kErrModelNotInstalled   = "model_not_installed";
constexpr const char* kErrFailed              = "failed";

// audiocpp_cli.exe on Windows, audiocpp_cli elsewhere.
std::string exeName() { return sc::acpp::binaryFileName ().toStdString (); }

juce::var newObject() { return juce::var (new juce::DynamicObject ()); }

void set(juce::var& o, const char* key, const juce::var& v)
{
    if (auto* d = o.getDynamicObject ())
        d->setProperty (juce::Identifier (key), v);
}

juce::var str(const std::string& s) { return juce::var (juce::String (s)); }
juce::var i64(int64_t v)            { return juce::var ((juce::int64) v); }
juce::var strOrNull(const std::string& s) { return s.empty () ? juce::var () : str (s); }

juce::var strArray(const std::vector<std::string>& v)
{
    juce::Array<juce::var> a;
    for (const auto& s : v) a.add (str (s));
    return juce::var (a);
}

std::string toUpper(std::string s)
{
    for (auto& c : s) c = (char) std::toupper ((unsigned char) c);
    return s;
}

std::string toLower(std::string s)
{
    for (auto& c : s) c = (char) std::tolower ((unsigned char) c);
    return s;
}

std::string trimmed(const std::string& s)
{
    return juce::String (s).trim ().toStdString ();
}

std::string formatBytes(int64_t b)
{
    char buf[64];
    if (b >= (int64_t) 1000 * 1000 * 1000)
        std::snprintf (buf, sizeof (buf), "%.1f GB", (double) b / 1.0e9);
    else
        std::snprintf (buf, sizeof (buf), "%.0f MB", (double) b / 1.0e6);
    return buf;
}

std::string platformName()
{
#if JUCE_WINDOWS
    return "windows";
#elif JUCE_MAC
    return "macos";
#else
    return "linux";
#endif
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find (v.begin (), v.end (), s) != v.end ();
}

CatalogOutcome fail(const char* code, const std::string& message, const juce::var& details = {})
{
    CatalogOutcome o;
    o.ok = false;
    o.code = code;
    o.message = message;
    o.details = details;
    return o;
}

CatalogOutcome success(const juce::var& result)
{
    CatalogOutcome o;
    o.ok = true;
    o.result = result;
    return o;
}

CatalogOutcome unknownId(const std::string& id)
{
    juce::var d = newObject ();
    set (d, "path", "modelId");
    set (d, "expected", "an id listed by ai.models.list");
    return fail (kErrInvalidParams, "unknown model id '" + id + "'", d);
}

// Sum of the sizes of every file under dir (recursive).
int64_t folderBytes(const juce::File& dir)
{
    int64_t total = 0;
    if (!dir.isDirectory ()) return 0;
    for (const auto& entry : juce::RangedDirectoryIterator (dir, true, "*", juce::File::findFiles))
        total += entry.getFile ().getSize ();
    return total;
}

std::vector<juce::File> ggufFilesIn(const juce::File& dir)
{
    std::vector<juce::File> out;
    if (!dir.isDirectory ()) return out;
    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFiles))
        if (entry.getFile ().hasFileExtension ("gguf"))
            out.push_back (entry.getFile ());
    std::sort (out.begin (), out.end (), [] (const juce::File& a, const juce::File& b)
               { return a.getFileName ().toLowerCase () < b.getFileName ().toLowerCase (); });
    return out;
}

bool isRuntime(const CatalogEntry& e) { return e.family == sc::ai::family::runtime; }

// installable / reason / reasonCode from the entry's data.
void finaliseInstallable(CatalogEntry& e, const std::string& blocker)
{
    e.installable = false;
    if (e.manual)
    {
        e.reasonCode = "manual";
        return;   // reason set by the caller
    }
    std::vector<CatalogFile> toCheck = e.files;
    if (isRuntime (e))
        toCheck.push_back (e.archive);
    for (const auto& f : toCheck)
    {
        if (f.sha256.size () != 64)
        {
            e.reasonCode = "no_checksum";
            e.reason = "no SHA-256 published for " + f.name + " (size-only check)";
            return;
        }
    }
    const auto& downloads = isRuntime (e) ? std::vector<CatalogFile> { e.archive } : e.files;
    for (const auto& f : downloads)
    {
        if (f.url.empty ())
        {
            e.reasonCode = "manual";
            e.reason = "no download URL for " + f.name;
            return;
        }
    }
    if (!blocker.empty ())
    {
        e.reasonCode = "blocked";
        e.reason = blocker;
        return;
    }
    if (e.engine == engine::audiocpp && !sc::acpp::platformSupported ())
    {
        e.reasonCode = kErrUnsupportedPlatform;
        e.reason = "audio.cpp publishes no CLI build for this platform (" + sc::acpp::platformTag () + ")";
        return;
    }
    e.installable = true;
    e.reason.clear ();
    e.reasonCode.clear ();
}

// "AudioSR-GGUF/audiosr-basic-f32.gguf" -> "audiosr-basic-f32". Plain string
// logic: juce::File splits on the platform separator only, and the catalogue
// paths use '/' everywhere (the ids must not depend on the platform).
std::string stripExtension(const std::string& file)
{
    const auto slash = file.find_last_of ("/\\");
    std::string base = slash == std::string::npos ? file : file.substr (slash + 1);
    const auto dot = base.find_last_of ('.');
    if (dot != std::string::npos && dot > 0) base.erase (dot);
    return base;
}

} // namespace

// ---------------------------------------------------------------------------
// CatalogEntry / CatalogOutcome
// ---------------------------------------------------------------------------

int64_t CatalogEntry::sizeBytes() const
{
    int64_t total = 0;
    for (const auto& f : files) total += f.sizeBytes;
    return total;
}

int64_t CatalogEntry::downloadBytes() const
{
    return family == sc::ai::family::runtime ? archive.sizeBytes : sizeBytes ();
}

std::string CatalogEntry::checksumKind() const
{
    if (manual || files.empty ()) return "none";
    for (const auto& f : files)
        if (f.sha256.size () != 64) return "size-only";
    if (family == sc::ai::family::runtime && archive.sha256.size () != 64) return "size-only";
    return "sha256";
}

juce::var CatalogOutcome::errorJson() const
{
    juce::var o = newObject ();
    set (o, "code", str (code));
    set (o, "message", str (message));
    if (!details.isVoid ())
        set (o, "details", details);
    return o;
}

// ---------------------------------------------------------------------------
// Built-in entries
// ---------------------------------------------------------------------------

std::vector<CatalogEntry> ModelCatalog::builtInEntries()
{
    std::vector<CatalogEntry> out;

    // 1. ModelManager: OpenVINO noise suppression + whisper.cpp checkpoints.
    for (const auto& spec : ModelManager::instance ().registry ())
    {
        CatalogEntry e;
        e.id = spec.id;
        const bool whisper = spec.id.rfind ("whisper-", 0) == 0;
        e.engine = whisper ? engine::whisper : engine::openvino;
        e.family = whisper ? family::transcribe : family::noiseSuppress;
        e.displayName = spec.displayName;
        e.folder = spec.id;
        for (const auto& f : spec.files)
            e.files.push_back ({ ModelManager::fileName (f), f.url, f.sizeBytes, f.sha256 });
        e.backends = { "CPU" };   // only the CPU plugin ships; entryJson() reports the live list
        e.platforms = { "windows", "macos", "linux" };
        if (whisper)
        {
            e.license = "MIT";
            e.source = "https://huggingface.co/ggerganov/whisper.cpp";
        }
        else if (spec.id == "denseunet")
        {
            e.license = "Apache-2.0";
            e.source = "https://github.com/openvinotoolkit/open_model_zoo (noise-suppression-denseunet-ll-0001)";
        }
        else
        {
            e.license = "MIT";
            e.source = "https://huggingface.co/Intel/deepfilternet-openvino";
        }
        e.minFreeDiskBytes = spec.minFreeDiskBytes;
        finaliseInstallable (e, {});
        out.push_back (std::move (e));
    }

    // 2. audio.cpp CLI binaries (one shared slot: installing one replaces the other).
    for (const auto& b : sc::acpp::binaryChoices ())
    {
        CatalogEntry e;
        e.id = "audiocpp-cli-" + b.id;
        e.engine = engine::audiocpp;
        e.family = family::runtime;
        e.displayName = "audio.cpp CLI - " + b.label.toStdString ();
        e.variant = b.id;
        e.files = { { exeName (), {}, b.exeSizeBytes, b.exeSha256 } };
        e.archive = { sc::acpp::archiveFileName ().toStdString (), b.url, b.sizeBytes, b.sha256 };
        e.backends = audioCppBackendsForVariant (b.id);
        e.platforms = { sc::acpp::platformTag () };
        e.license = "no SPDX license declared (see https://github.com/0xShug0/audio.cpp)";
        e.source = "https://github.com/0xShug0/audio.cpp/releases/tag/v0.9.2-drama3-base-small";
        e.minFreeDiskBytes = b.sizeBytes + b.exeSizeBytes + 64 * 1024 * 1024;
        finaliseInstallable (e, b.catalogBlocker);
        out.push_back (std::move (e));
    }

    // 3. SAM-Audio GGUF (stem separation).
    for (const auto& m : sc::acpp::modelChoices ())
    {
        CatalogEntry e;
        e.id = stripExtension (m.file);
        e.engine = engine::audiocpp;
        e.family = family::stems;
        e.displayName = "SAM-Audio " + e.id.substr (std::string ("sam-audio-").size ());
        e.files = { { m.file, sc::acpp::modelUrl (m.file).toStdString (), m.sizeBytes, m.sha256 } };
        e.backends = { "cpu", "cuda", "vulkan", "metal" };
        e.platforms = sc::acpp::supportedPlatformTags ();
        e.license = "other (SAM license, see the model card)";
        e.source = "https://huggingface.co/audio-cpp/SAM-Audio-GGUF";
        e.minFreeDiskBytes = m.sizeBytes + 64 * 1024 * 1024;
        finaliseInstallable (e, {});
        out.push_back (std::move (e));
    }

    // 4. Super resolution: FlashSR (audio.cpp repository asset) and AudioSR.
    for (const auto& m : sc::acpp::flashSrChoices ())
    {
        CatalogEntry e;
        e.id = "flashsr";
        e.engine = engine::audiocpp;
        e.family = family::superRes;
        e.displayName = "FlashSR super-resolution";
        e.files = { { m.file, sc::acpp::flashSrUrl (m.file).toStdString (), m.sizeBytes, m.sha256 } };
        e.backends = { "cpu", "cuda", "vulkan", "metal" };
        e.platforms = sc::acpp::supportedPlatformTags ();
        e.license = "Apache-2.0";
        e.source = "https://huggingface.co/YatharthS/FlashSR (shipped in the audio.cpp repository)";
        e.minFreeDiskBytes = m.sizeBytes + 64 * 1024 * 1024;
        finaliseInstallable (e, {});
        out.push_back (std::move (e));
    }
    for (const auto& m : sc::acpp::audioSRChoices ())
    {
        CatalogEntry e;
        e.id = stripExtension (m.file);   // "audiosr-basic-f32"
        e.engine = engine::audiocpp;
        e.family = family::superRes;
        e.displayName = "AudioSR basic (f32)";
        e.files = { { m.file, sc::acpp::audioSRUrl (m.file).toStdString (), m.sizeBytes, m.sha256 } };
        e.backends = { "cpu", "cuda", "vulkan", "metal" };
        e.platforms = sc::acpp::supportedPlatformTags ();
        e.license = "other (see the model card)";
        e.source = "https://huggingface.co/audio-cpp/audio.cpp-gguf";
        e.minFreeDiskBytes = m.sizeBytes + 64 * 1024 * 1024;
        finaliseInstallable (e, {});
        out.push_back (std::move (e));
    }

    // 5. htdemucs families: detected only, the app offers no download.
    struct Manual { const char* id; const char* folder; const char* name; };
    for (const Manual& m : { Manual { "htdemucs-6stems", "HTDemucs-6stems-GGUF", "HTDemucs 6 stems (manual install)" },
                             Manual { "htdemucs-ft", "htdemucs_ft", "HTDemucs fine-tuned (manual install)" } })
    {
        CatalogEntry e;
        e.id = m.id;
        e.engine = engine::audiocpp;
        e.family = family::stems;
        e.displayName = m.name;
        e.folder = m.folder;
        e.manual = true;
        e.backends = { "cpu", "cuda", "vulkan", "metal" };
        e.platforms = sc::acpp::supportedPlatformTags ();
        e.license = "see the model's source";
        e.source = "user-installed: one or more .gguf files in <audiocppRoot>/" + std::string (m.folder);
        e.reason = "the app has no download source for it: place the .gguf file(s) in <audiocppRoot>/"
                   + std::string (m.folder);
        finaliseInstallable (e, {});
        out.push_back (std::move (e));
    }

    return out;
}

// ---------------------------------------------------------------------------
// ModelCatalog
// ---------------------------------------------------------------------------

ModelCatalog& ModelCatalog::instance()
{
    static ModelCatalog cat (ModelManager::instance (), sc::acpp::dir ());
    return cat;
}

ModelCatalog::ModelCatalog(ModelManager& mm, const juce::File& audioCppRoot)
    : ModelCatalog (mm, audioCppRoot, builtInEntries ())
{
}

ModelCatalog::ModelCatalog(ModelManager& mm, const juce::File& audioCppRoot, std::vector<CatalogEntry> entries)
    : _mm (mm), _acppRoot (audioCppRoot), _entries (std::move (entries))
{
}

const CatalogEntry* ModelCatalog::find(const std::string& id) const
{
    for (const auto& e : _entries)
        if (e.id == id) return &e;
    return nullptr;
}

juce::File ModelCatalog::modelsRoot() const { return _mm.modelRoot (); }

juce::File ModelCatalog::locationOf(const CatalogEntry& e) const
{
    const juce::File root = (e.engine == engine::audiocpp) ? _acppRoot : _mm.modelRoot ();
    return e.folder.empty () ? root : root.getChildFile (juce::String (e.folder));
}

CatalogStatus ModelCatalog::status(const CatalogEntry& e) const
{
    CatalogStatus st;
    const juce::File dir = locationOf (e);

    if (e.manual)
    {
        st.path = dir;
        for (const auto& f : ggufFilesIn (dir))
            st.bytesOnDisk += f.getSize ();
        st.installed = st.bytesOnDisk > 0;
        if (!st.installed) st.missing.push_back ("*.gguf");
        return st;
    }

    if (isRuntime (e))
    {
        const juce::File exe = sc::acpp::binaryPath (dir);
        st.path = exe;
        st.installed = exe.existsAsFile () && exe.getSize () > 0
                    && sc::acpp::installedVariant (dir).toStdString () == e.variant;
        if (st.installed)
        {
            st.bytesOnDisk = exe.getSize ();
            st.verified = ModelManager::wasVerified (exe);
        }
        else
            st.missing.push_back (exeName ());
        return st;
    }

    bool allVerified = true;
    for (const auto& f : e.files)
    {
        const juce::File p = dir.getChildFile (juce::String (f.name));
        const bool present = p.existsAsFile ();
        if (present) st.bytesOnDisk += p.getSize ();
        if (!present || (f.sizeBytes > 0 && p.getSize () != f.sizeBytes))
            st.missing.push_back (f.name);
        if (!present || f.sha256.empty () || !ModelManager::wasVerified (p))
            allVerified = false;
    }
    st.installed = !e.files.empty () && st.missing.empty ();
    st.verified = st.installed && allVerified;
    st.path = (e.engine == engine::audiocpp && e.files.size () == 1)
                  ? dir.getChildFile (juce::String (e.files[0].name)) : dir;
    return st;
}

class ModelCatalog::BusyGuard
{
public:
    BusyGuard(ModelCatalog& c, const std::string& id) : _c (c), _id (id), _ok (c.tryMarkBusy (id)) {}
    ~BusyGuard() { if (_ok) _c.clearBusy (_id); }
    explicit operator bool() const { return _ok; }
private:
    ModelCatalog& _c;
    std::string _id;
    bool _ok;
};

// The audio.cpp runtimes share one slot (the CLI executable and its libraries,
// variant.txt, the release archive): installing / removing any of them excludes the others.
std::string ModelCatalog::busyKey(const std::string& id) const
{
    const CatalogEntry* e = find (id);
    return (e != nullptr && isRuntime (*e)) ? std::string ("audiocpp-cli") : id;
}

bool ModelCatalog::tryMarkBusy(const std::string& id)
{
    const std::string key = busyKey (id);
    std::lock_guard<std::mutex> lock (_busyMutex);
    return _busy.insert (key).second;
}

void ModelCatalog::clearBusy(const std::string& id)
{
    const std::string key = busyKey (id);
    std::lock_guard<std::mutex> lock (_busyMutex);
    _busy.erase (key);
}

bool ModelCatalog::isBusy(const std::string& id) const
{
    const std::string key = busyKey (id);
    std::lock_guard<std::mutex> lock (_busyMutex);
    return _busy.count (key) != 0;
}

static CatalogOutcome busyOutcome(const std::string& id)
{
    juce::var d = newObject ();
    set (d, "modelId", str (id));
    return fail (kErrBusy, "another install / verify / remove of " + id + " is running", d);
}

CatalogOutcome ModelCatalog::installPlan(const std::string& id) const
{
    const CatalogEntry* e = find (id);
    if (e == nullptr) return unknownId (id);
    if (!e->installable)
    {
        juce::var d = newObject ();
        set (d, "modelId", str (id));
        set (d, "installable", false);
        set (d, "reason", str (e->reason));
        set (d, "reasonCode", str (e->reasonCode));
        if (e->reasonCode == kErrUnsupportedPlatform)
        {
            set (d, "platform", str (platformName ()));
            return fail (kErrUnsupportedPlatform, id + " cannot be installed on " + platformName () + ": " + e->reason, d);
        }
        return fail (kErrFailed, id + " cannot be installed: " + e->reason, d);
    }
    if (isBusy (id)) return busyOutcome (id);

    const CatalogStatus st = status (*e);
    int64_t remaining = 0;
    if (!st.installed)
    {
        if (isRuntime (*e))
            remaining = e->archive.sizeBytes;
        else
            for (const auto& f : e->files)
                if (contains (st.missing, f.name)) remaining += f.sizeBytes;
    }
    juce::var r = newObject ();
    set (r, "modelId", str (id));
    set (r, "sizeBytes", i64 (e->sizeBytes ()));
    set (r, "downloadBytes", i64 (remaining));
    set (r, "installed", st.installed);
    return success (r);
}

CatalogOutcome ModelCatalog::install(const std::string& id, const CatalogProgressFn& onProgress)
{
    CatalogOutcome plan = installPlan (id);
    if (!plan.ok) return plan;
    BusyGuard guard (*this, id);
    if (!guard) return busyOutcome (id);

    const CatalogEntry& e = *find (id);
    if (e.engine == engine::openvino || e.engine == engine::whisper)
        return installViaModelManager (e, onProgress);
    if (e.engine == engine::audiocpp)
        return installAudioCpp (e, onProgress);
    return fail (kErrFailed, "no installer for engine '" + e.engine + "'");
}

static juce::var installedResult(const CatalogEntry& e, int64_t downloaded)
{
    juce::var r = newObject ();
    set (r, "modelId", str (e.id));
    set (r, "installed", true);
    set (r, "verified", e.checksumKind () == "sha256");
    set (r, "downloadedBytes", i64 (downloaded));
    return r;
}

CatalogOutcome ModelCatalog::installViaModelManager(const CatalogEntry& e, const CatalogProgressFn& onProgress)
{
    ModelSpec spec;
    spec.id = e.folder.empty () ? e.id : e.folder;   // ModelManager::dirFor(spec.id) == locationOf(e)
    spec.displayName = e.displayName;
    spec.minFreeDiskBytes = e.minFreeDiskBytes;
    for (const auto& f : e.files)
    {
        ModelFileSpec mf;
        mf.url = f.url;
        mf.sha256 = f.sha256;
        mf.sizeBytes = f.sizeBytes;
        mf.name = f.name;
        spec.files.push_back (mf);
    }

    int64_t remaining = 0;
    {
        const CatalogStatus st = status (e);
        for (const auto& f : e.files)
            if (contains (st.missing, f.name)) remaining += f.sizeBytes;
    }

    bool cancelled = false;
    auto fn = [&] (float frac, const std::string&) -> bool
    {
        if (!onProgress) return true;
        const std::string text = "downloading " + e.id + " "
                                 + formatBytes ((int64_t) ((double) frac * (double) remaining))
                                 + " / " + formatBytes (remaining);
        if (!onProgress (frac, text)) { cancelled = true; return false; }
        return true;
    };

    std::string err;
    if (!_mm.ensureInstalled (spec, fn, &err))
    {
        if (cancelled) return fail (kErrCancelled, "cancelled by client");
        juce::var d = newObject ();
        set (d, "modelId", str (e.id));
        set (d, "folder", locationOf (e).getFullPathName ());
        juce::Array<juce::var> urls;
        for (const auto& f : e.files) urls.add (str (f.url));
        set (d, "manualInstall", juce::var (urls));
        return fail (kErrFailed, err.empty () ? "install of " + e.id + " failed" : err, d);
    }
    if (onProgress) onProgress (1.0f, "installed " + e.id);
    return success (installedResult (e, remaining));
}

CatalogOutcome ModelCatalog::installAudioCpp(const CatalogEntry& e, const CatalogProgressFn& onProgress)
{
    AudioCppDownloader download = _acppDownloader;
    if (!download)
    {
        if (!sc::acpp::platformSupported ())
        {
            juce::var d = newObject ();
            set (d, "platform", str (sc::acpp::platformTag ()));
            set (d, "supported", strArray (sc::acpp::supportedPlatformTags ()));
            return fail (kErrUnsupportedPlatform, "audio.cpp publishes no CLI build for this platform", d);
        }
        download = [] (const std::string& url, const juce::File& dest,
                       const std::function<bool(float)>& progress, std::string* errOut,
                       int64_t expectedBytes, const std::string& expectedSha256)
        {
            return sc::acpp::downloadFile (url, dest, progress, errOut, expectedBytes, expectedSha256);
        };
    }

    const juce::File root = locationOf (e);
    root.createDirectory ();

    // Disk space: the remaining downloads (+ the extracted executable for a runtime).
    {
        const int64_t need = e.minFreeDiskBytes > 0 ? e.minFreeDiskBytes : e.downloadBytes ();
        const int64_t freeBytes = root.getBytesFreeOnVolume ();
        if (freeBytes > 0 && freeBytes < need)
            return fail (kErrFailed, "Not enough disk space: need ~" + formatBytes (need)
                                     + " free, have ~" + formatBytes (freeBytes) + ".");
    }

    bool cancelled = false;
    auto progressFor = [&] (const std::string& what, int64_t bytes)
    {
        return [&, what, bytes] (float frac) -> bool
        {
            if (!onProgress) return true;
            const std::string text = what + " " + formatBytes ((int64_t) ((double) frac * (double) bytes))
                                     + " / " + formatBytes (bytes);
            if (!onProgress (frac, text)) { cancelled = true; return false; }
            return true;
        };
    };

    if (isRuntime (e))
    {
        const juce::File exe = sc::acpp::binaryPath (root);
        const CatalogFile& exeSpec = e.files.front ();
        const std::string previous = sc::acpp::installedVariant (root).toStdString ();

        // Already installed and intact: nothing to download.
        if (previous == e.variant && exe.existsAsFile ()
            && juce::String (sc::acpp::sha256OfFile (exe)).equalsIgnoreCase (juce::String (exeSpec.sha256)))
        {
            ModelManager::markVerified (exe);
            return success (installedResult (e, 0));
        }

        const juce::File zip = root.getChildFile (juce::String (e.archive.name));   // .zip or .tar.gz
        std::string err;
        if (!download (e.archive.url, zip, progressFor ("downloading " + e.id, e.archive.sizeBytes),
                       &err, e.archive.sizeBytes, e.archive.sha256))
        {
            zip.deleteFile ();
            if (cancelled) return fail (kErrCancelled, "cancelled by client");
            return fail (kErrFailed, err.empty () ? "download of " + e.id + " failed" : err);
        }
        if (onProgress) onProgress (1.0f, "extracting " + exeName ());
        if (!sc::acpp::installBinaryFromArchive (zip, juce::String (e.variant), root, &err))
        {
            zip.deleteFile ();
            return fail (kErrFailed, err.empty () ? "extraction of " + exeName () + " failed" : err);
        }
        // installBinaryFromArchive already checked the executable of a built-in
        // variant; hash it here only for other (custom) entries.
        bool builtInVariant = false;
        for (const auto& b : sc::acpp::binaryChoices ())
            if (b.id == e.variant && b.exeSha256 == exeSpec.sha256) builtInVariant = true;
        if (!builtInVariant
            && ! juce::String (sc::acpp::sha256OfFile (exe)).equalsIgnoreCase (juce::String (exeSpec.sha256)))
        {
            sc::acpp::removeInstalledBinary (root);
            return fail (kErrFailed, "the extracted " + exeName ()
                                     + " does not match its published SHA-256; it was deleted");
        }
        ModelManager::markVerified (exe);
        CatalogOutcome ok = success (installedResult (e, e.archive.sizeBytes));
        set (ok.result, "variant", str (e.variant));
        set (ok.result, "replaced", (previous.empty () || previous == e.variant) ? juce::var () : str (previous));
        return ok;
    }

    int64_t downloaded = 0;
    for (const auto& f : e.files)
    {
        const juce::File dest = root.getChildFile (juce::String (f.name));
        if (!dest.isAChildOf (root))
            return fail (kErrFailed, "refusing to write outside the audio.cpp folder: " + f.name);
        dest.getParentDirectory ().createDirectory ();

        if (dest.existsAsFile () && (f.sizeBytes <= 0 || dest.getSize () == f.sizeBytes))
        {
            if (ModelManager::wasVerified (dest)) continue;
            const std::string h = sc::acpp::sha256OfFile (dest, progressFor ("checking " + f.name, f.sizeBytes));
            if (cancelled) return fail (kErrCancelled, "cancelled by client");
            if (juce::String (h).equalsIgnoreCase (juce::String (f.sha256)))
            {
                ModelManager::markVerified (dest);
                continue;
            }
            // Right size, wrong bytes: replace it.
            ModelManager::forgetVerified (dest);
            dest.deleteFile ();
        }

        std::string err;
        if (!download (f.url, dest, progressFor ("downloading " + f.name, f.sizeBytes),
                       &err, f.sizeBytes, f.sha256))
        {
            if (cancelled) return fail (kErrCancelled, "cancelled by client");
            juce::var d = newObject ();
            set (d, "modelId", str (e.id));
            set (d, "file", dest.getFullPathName ());
            set (d, "url", str (f.url));
            return fail (kErrFailed, err.empty () ? "download of " + f.name + " failed" : err, d);
        }
        ModelManager::markVerified (dest);   // hashed while downloading
        downloaded += f.sizeBytes;
    }
    if (onProgress) onProgress (1.0f, "installed " + e.id);
    return success (installedResult (e, downloaded));
}

CatalogOutcome ModelCatalog::verify(const std::string& id, const CatalogProgressFn& onProgress)
{
    const CatalogEntry* ep = find (id);
    if (ep == nullptr) return unknownId (id);
    const CatalogEntry& e = *ep;
    BusyGuard guard (*this, id);
    if (!guard) return busyOutcome (id);

    const CatalogStatus st = status (e);
    const juce::File dir = locationOf (e);

    juce::Array<juce::var> files;
    juce::var r = newObject ();
    set (r, "modelId", str (id));
    set (r, "checksum", str (e.checksumKind ()));

    if (e.manual)
    {
        // No published hashes: report what is there.
        for (const auto& f : ggufFilesIn (dir))
        {
            juce::var fo = newObject ();
            set (fo, "name", f.getFileName ());
            set (fo, "bytes", i64 (f.getSize ()));
            set (fo, "sha256Ok", juce::var ());
            files.add (fo);
        }
        if (files.isEmpty ())
        {
            juce::var d = newObject ();
            set (d, "missing", strArray (st.missing));
            set (d, "hint", str (e.reason));
            return fail (kErrModelNotInstalled, id + " is not installed", d);
        }
        set (r, "verified", false);
        set (r, "files", juce::var (files));
        return success (r);
    }

    // What to hash. A runtime entry is only checked when it is the installed variant.
    std::vector<std::pair<CatalogFile, juce::File>> todo;
    bool anyPresent = false;
    if (isRuntime (e))
    {
        if (st.installed)
        {
            todo.push_back ({ e.files.front (), sc::acpp::binaryPath (dir) });
            anyPresent = true;
        }
    }
    else
    {
        for (const auto& f : e.files)
        {
            const juce::File p = dir.getChildFile (juce::String (f.name));
            todo.push_back ({ f, p });
            anyPresent = anyPresent || p.existsAsFile ();
        }
    }
    if (!anyPresent)
    {
        juce::var d = newObject ();
        set (d, "missing", strArray (st.missing));
        set (d, "sizeBytes", i64 (e.sizeBytes ()));
        set (d, "hint", "call ai.models.install");
        return fail (kErrModelNotInstalled, id + " is not installed", d);
    }

    int64_t total = 0;
    for (const auto& t : todo)
        if (t.second.existsAsFile ()) total += t.second.getSize ();
    int64_t done = 0;
    bool cancelled = false;
    bool allOk = true;

    for (const auto& t : todo)
    {
        const CatalogFile& f = t.first;
        const juce::File& p = t.second;
        juce::var fo = newObject ();
        set (fo, "name", str (f.name));
        set (fo, "expectedBytes", i64 (f.sizeBytes));
        if (!p.existsAsFile ())
        {
            set (fo, "bytes", i64 (0));
            set (fo, "present", false);
            set (fo, "sha256Ok", false);
            allOk = false;
            files.add (fo);
            continue;
        }
        const int64_t bytes = p.getSize ();
        set (fo, "bytes", i64 (bytes));
        set (fo, "present", true);
        if (f.sha256.size () != 64)
        {
            set (fo, "sha256Ok", juce::var ());   // nothing to compare with
            allOk = false;
            files.add (fo);
            done += bytes;
            continue;
        }
        const int64_t base = done;
        const std::string h = sc::acpp::sha256OfFile (p, [&] (float frac) -> bool
        {
            if (!onProgress) return true;
            const double overall = total > 0 ? ((double) base + (double) frac * (double) bytes) / (double) total : 1.0;
            if (!onProgress ((float) overall, "verifying " + f.name)) { cancelled = true; return false; }
            return true;
        });
        if (cancelled) return fail (kErrCancelled, "cancelled by client");
        const bool ok = !h.empty () && juce::String (h).equalsIgnoreCase (juce::String (f.sha256))
                        && (f.sizeBytes <= 0 || bytes == f.sizeBytes);
        if (ok) ModelManager::markVerified (p);
        else    ModelManager::forgetVerified (p);
        set (fo, "sha256Ok", ok);
        if (h.empty ()) set (fo, "error", "could not read the file");
        allOk = allOk && ok;
        files.add (fo);
        done += bytes;
    }

    set (r, "verified", allOk);
    set (r, "files", juce::var (files));
    return success (r);
}

CatalogOutcome ModelCatalog::removalPreview(const std::string& id) const
{
    const CatalogEntry* e = find (id);
    if (e == nullptr) return unknownId (id);
    const CatalogStatus st = status (*e);
    const juce::File where = isRuntime (*e) || (e->engine == engine::audiocpp && !e->manual)
                                 ? st.path : locationOf (*e);
    const int64_t bytes = (e->engine == engine::audiocpp) ? st.bytesOnDisk : folderBytes (where);
    juce::var r = newObject ();
    set (r, "modelId", str (id));
    set (r, "bytesOnDisk", i64 (bytes));
    set (r, "path", where.getFullPathName ());
    set (r, "message", str ("Removes " + formatBytes (bytes) + " under " + where.getFullPathName ().toStdString ()));
    return success (r);
}

CatalogOutcome ModelCatalog::remove(const std::string& id)
{
    const CatalogEntry* ep = find (id);
    if (ep == nullptr) return unknownId (id);
    const CatalogEntry& e = *ep;
    if (e.manual)
    {
        juce::var d = newObject ();
        set (d, "path", locationOf (e).getFullPathName ());
        return fail (kErrFailed, id + " is user-managed: delete its folder by hand", d);
    }
    BusyGuard guard (*this, id);
    if (!guard) return busyOutcome (id);

    const juce::File dir = locationOf (e);
    int64_t freed = 0;
    bool removed = false;

    auto result = [&] (const juce::File& where)
    {
        juce::var r = newObject ();
        set (r, "modelId", str (id));
        set (r, "removed", removed);
        set (r, "freedBytes", i64 (freed));
        set (r, "path", where.getFullPathName ());
        return success (r);
    };

    if (e.engine != engine::audiocpp)
    {
        // One folder per model under the models root: delete it whole, but
        // never the root itself or anything outside it.
        const juce::File root = _mm.modelRoot ();
        if (!dir.isAChildOf (root))
            return fail (kErrFailed, "refusing to delete " + dir.getFullPathName ().toStdString ()
                                     + ": not inside the models folder");
        if (!dir.exists ()) return result (dir);
        freed = folderBytes (dir);
        for (const auto& f : e.files)
            ModelManager::forgetVerified (dir.getChildFile (juce::String (f.name)));
        if (!dir.deleteRecursively ())
            return fail (kErrFailed, "could not delete " + dir.getFullPathName ().toStdString ()
                                     + " (a file is in use?)");
        removed = true;
        return result (dir);
    }

    if (isRuntime (e))
    {
        const juce::File exe = sc::acpp::binaryPath (dir);
        if (sc::acpp::installedVariant (dir).toStdString () != e.variant || !exe.existsAsFile ())
            return result (exe);   // this variant is not the installed one: nothing to do
        freed = exe.getSize ();
        for (const auto& lib : sc::acpp::installedRuntimeLibraries (dir))
            freed += dir.getChildFile (lib).getSize ();
        ModelManager::forgetVerified (exe);
        if (!sc::acpp::removeInstalledBinary (dir))   // executable, libraries, variant.txt
            return fail (kErrFailed, "could not delete " + exe.getFullPathName ().toStdString ()
                                     + " (is it running?)");
        removed = true;
        return result (exe);
    }

    juce::File last;
    for (const auto& f : e.files)
    {
        const juce::File p = dir.getChildFile (juce::String (f.name));
        last = p;
        if (!p.isAChildOf (dir))
            return fail (kErrFailed, "refusing to delete outside the audio.cpp folder: " + f.name);
        p.getSiblingFile (p.getFileName () + ".part").deleteFile ();   // interrupted download
        if (!p.existsAsFile ()) continue;
        const int64_t size = p.getSize ();
        ModelManager::forgetVerified (p);
        if (!p.deleteFile ())
            return fail (kErrFailed, "could not delete " + p.getFullPathName ().toStdString ()
                                     + " (a file is in use?)");
        freed += size;
        removed = true;
        // Remove now-empty subfolders (e.g. AudioSR-GGUF), never the root.
        for (juce::File parent = p.getParentDirectory ();
             parent.isAChildOf (dir) && parent.isDirectory ()
                 && parent.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0;
             parent = parent.getParentDirectory ())
            parent.deleteFile ();
    }
    return result (e.files.size () == 1 ? last : dir);
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

juce::var ModelCatalog::entryJson(const CatalogEntry& e) const
{
    const CatalogStatus st = status (e);
    juce::var o = newObject ();
    set (o, "id", str (e.id));
    set (o, "engine", str (e.engine));
    set (o, "family", str (e.family));
    set (o, "displayName", str (e.displayName));
    set (o, "installed", st.installed);
    set (o, "verified", st.verified);
    set (o, "bytesOnDisk", i64 (st.bytesOnDisk));
    set (o, "sizeBytes", i64 (e.sizeBytes ()));
    set (o, "path", st.path.getFullPathName ());
    set (o, "checksum", str (e.checksumKind ()));

    std::vector<std::string> backends = e.backends;
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    // OpenVINO: once the runtime is initialised, the real device list.
    if (e.engine == engine::openvino && OvRuntime::instance ().ready ())
    {
        const auto live = OvRuntime::instance ().devices ();
        if (!live.empty ()) backends = live;
    }
#endif
    set (o, "backends", strArray (backends));
    set (o, "platforms", strArray (e.platforms));
    set (o, "installable", e.installable);
    if (!e.installable)
    {
        set (o, "reason", str (e.reason));
        set (o, "reasonCode", str (e.reasonCode));
    }
    if (!st.installed)
        set (o, "missing", strArray (st.missing));
    if (e.manual)
        set (o, "manual", true);
    if (isRuntime (e))
    {
        set (o, "variant", str (e.variant));
        juce::var dl = newObject ();
        set (dl, "url", str (e.archive.url));
        set (dl, "sizeBytes", i64 (e.archive.sizeBytes));
        set (dl, "sha256", strOrNull (e.archive.sha256));
        set (o, "download", dl);
    }
    juce::Array<juce::var> files;
    for (const auto& f : e.files)
    {
        juce::var fo = newObject ();
        set (fo, "name", str (f.name));
        set (fo, "url", strOrNull (f.url));
        set (fo, "sizeBytes", i64 (f.sizeBytes));
        set (fo, "sha256", strOrNull (f.sha256));
        files.add (fo);
    }
    set (o, "files", juce::var (files));
    set (o, "license", str (e.license));
    set (o, "source", str (e.source));
    if (isBusy (e.id)) set (o, "busy", true);
    return o;
}

juce::var ModelCatalog::listJson() const
{
    juce::var o = newObject ();
    set (o, "modelsRoot", modelsRoot ().getFullPathName ());
    set (o, "audiocppRoot", _acppRoot.getFullPathName ());
    juce::Array<juce::var> models;
    for (const auto& e : _entries)
        models.add (entryJson (e));
    set (o, "models", juce::var (models));
    return o;
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

namespace {

// "GPU.1" -> "GPU"
std::string deviceFamily(const std::string& d)
{
    const auto dot = d.find ('.');
    return dot == std::string::npos ? d : d.substr (0, dot);
}

std::string joinAllowed(const std::vector<std::string>& v)
{
    std::string s = "one of [";
    for (size_t i = 0; i < v.size (); ++i)
    {
        if (i > 0) s += ", ";
        s += v[i];
    }
    return s + "]";
}

} // namespace

juce::var DeviceResolution::errorJson() const
{
    juce::var o = newObject ();
    set (o, "code", str (code));
    set (o, "message", str (message));
    juce::var d = newObject ();
    set (d, "path", str (path));
    set (d, "expected", str (joinAllowed (allowed)));
    set (o, "details", d);
    return o;
}

juce::var DeviceResolution::toJson() const
{
    juce::var o = newObject ();
    set (o, "engine", str (engine));
    set (o, "device", str (requested.empty () ? std::string ("auto") : requested));
    set (o, "resolved", strOrNull (ok ? device : std::string ()));
    return o;
}

std::vector<std::string> audioCppBackendsForVariant(const std::string& variant)
{
    if (variant == "cpu") return { "cpu" };
    if (variant.rfind ("cuda", 0) == 0) return { "cuda" };
    if (variant == "vulkan") return { "vulkan" };
    if (variant == "metal") return { "metal", "cpu" };   // the macOS builds have both
#if JUCE_MAC
    if (variant.empty ()) return { "metal", "cpu" };   // hand-placed binary: a macOS build
#else
    if (variant.empty ()) return { "cpu", "cuda", "vulkan" };   // hand-placed binary: unknown build
#endif
    return {};
}

DeviceResolution resolveAiDevice(const std::string& engineName, const std::string& requestedIn,
                                 const std::vector<std::string>& available)
{
    DeviceResolution r;
    r.engine = engineName;
    const bool ov = engineName == engine::openvino;
    const bool wh = engineName == engine::whisper;
    const bool ac = engineName == engine::audiocpp;
    if (!ov && !wh && !ac)
    {
        r.code = kErrInvalidParams;
        r.message = "unknown AI engine '" + engineName + "'";
        r.path = "engine";
        r.allowed = { engine::openvino, engine::whisper, engine::audiocpp };
        return r;
    }

    std::string req = trimmed (requestedIn);
    req = ac ? toLower (req) : toUpper (req);
    if (toLower (req) == "auto") req.clear ();
    r.requested = req.empty () ? std::string ("auto") : req;

    // Accepted values: "auto" + the device families + any indexed names ("GPU.1").
    r.allowed.push_back ("auto");
    for (const auto& d : available)
        if (!contains (r.allowed, deviceFamily (d))) r.allowed.push_back (deviceFamily (d));
    for (const auto& d : available)
        if (!contains (r.allowed, d)) r.allowed.push_back (d);

    if (available.empty ())
    {
        r.code = kErrFailed;
        r.message = "no " + engineName + " device is available";
        return r;
    }

    auto firstOfFamily = [&] (const std::string& fam) -> std::string
    {
        for (const auto& d : available) if (d == fam) return d;               // exact name first
        for (const auto& d : available) if (deviceFamily (d) == fam) return fam;  // "GPU" for GPU.0 / GPU.1
        return {};
    };

    if (req.empty ())   // auto
    {
        std::vector<std::string> order;
        if (ov) order = { "GPU", "NPU", "CPU" };
        else if (wh) order = { "CPU" };
        else order = available.size () == 1 ? available : std::vector<std::string> { "cpu", "cuda", "vulkan" };
        for (const auto& fam : order)
        {
            const std::string d = firstOfFamily (fam);
            if (!d.empty ()) { r.ok = true; r.device = d; return r; }
        }
        r.ok = true;
        r.device = available.front ();
        return r;
    }

    const std::vector<std::string> known = ov ? std::vector<std::string> { "CPU", "GPU", "NPU" }
                                         : wh ? std::vector<std::string> { "CPU", "GPU" }
                                              : std::vector<std::string> { "cpu", "cuda", "vulkan" };
    if (!contains (known, deviceFamily (req)))
    {
        r.code = kErrInvalidParams;
        r.message = "unknown device '" + req + "' for " + engineName;
        return r;
    }

    if (contains (available, req)) { r.ok = true; r.device = req; return r; }
    if (req.find ('.') == std::string::npos)
    {
        const std::string d = firstOfFamily (req);
        if (!d.empty ()) { r.ok = true; r.device = d; return r; }
    }
    r.code = kErrInvalidParams;
    r.message = "device " + req + " is not available for " + engineName;
    return r;
}

std::vector<std::string> availableDevices(const std::string& engineName, bool initRuntime,
                                          std::string* noteOut, const juce::File& audioCppRoot)
{
    if (noteOut) noteOut->clear ();
    if (engineName == engine::openvino)
    {
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        auto& rt = OvRuntime::instance ();
        if (!rt.ready ())
        {
            if (!initRuntime)
            {
                if (noteOut) *noteOut = "the OpenVINO runtime is not initialised yet";
                return {};
            }
            std::string err;
            if (!rt.init (&err))
            {
                if (noteOut) *noteOut = err.empty () ? std::string ("the OpenVINO runtime failed to initialise") : err;
                return {};
            }
        }
        auto devs = rt.devices ();
        if (devs.empty () && noteOut) *noteOut = "the OpenVINO runtime lists no device";
        return devs;
#else
        juce::ignoreUnused (initRuntime);
        if (noteOut) *noteOut = "this build has no AI support";
        return {};
#endif
    }
    if (engineName == engine::whisper)
        return { "CPU" };   // WhisperTranscribe.cpp: use_gpu = false
    if (engineName == engine::audiocpp)
    {
        if (!sc::acpp::platformSupported ())
        {
            if (noteOut) *noteOut = "audio.cpp publishes no CLI build for this platform (" + sc::acpp::platformTag () + ")";
            return {};
        }
        const juce::File root = audioCppRoot.getFullPathName ().isEmpty () ? sc::acpp::dir () : audioCppRoot;
        if (!sc::acpp::hasBinary (root))
        {
            if (noteOut) *noteOut = "no audio.cpp binary is installed";
            return {};
        }
        return audioCppBackendsForVariant (sc::acpp::installedVariant (root).toStdString ());
    }
    if (noteOut) *noteOut = "unknown AI engine '" + engineName + "'";
    return {};
}

DeviceResolution resolveAiDevice(const std::string& engineName, const std::string& requested)
{
    std::string note;
    const auto available = availableDevices (engineName, true, &note);
    DeviceResolution r = resolveAiDevice (engineName, requested, available);
    if (!r.ok && available.empty () && r.path == "device")
    {
        // Say why nothing is available.
        if (engineName == engine::audiocpp)
            r.code = sc::acpp::platformSupported () ? kErrModelNotInstalled : kErrUnsupportedPlatform;
        if (!note.empty ()) r.message += ": " + note;
    }
    return r;
}

DeviceResolution resolveAiDevice(const std::string& requested)
{
    return resolveAiDevice (engine::openvino, requested);
}

juce::var devicesJson(const std::string& openvinoPreference, bool initOpenVino, const juce::File& audioCppRoot)
{
    juce::var out = newObject ();

    // OpenVINO
    {
        juce::var o = newObject ();
        const std::string pref = openvinoPreference.empty () ? std::string ("auto") : openvinoPreference;
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
        const bool ready = OvRuntime::instance ().ready ();
#else
        const bool ready = false;
#endif
        if (!ready && !initOpenVino)
        {
            set (o, "initialised", false);
            set (o, "devices", juce::var ());
            set (o, "preference", str (pref));
            set (o, "note", "pass init:true to load the OpenVINO runtime and list its devices");
        }
        else
        {
            std::string note;
            const auto devs = availableDevices (engine::openvino, initOpenVino, &note);
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
            set (o, "initialised", OvRuntime::instance ().ready ());
#else
            set (o, "initialised", false);
#endif
            set (o, "available", !devs.empty ());
            set (o, "devices", strArray (devs));
            set (o, "preference", str (pref));
            const DeviceResolution r = resolveAiDevice (engine::openvino, pref, devs);
            set (o, "resolved", r.ok ? str (r.device) : juce::var ());
            if (!r.ok) set (o, "error", r.errorJson ());
            if (devs.size () == 1 && devs.front () == "CPU")
                set (o, "note", "only the CPU plugin ships");
            else if (!note.empty ())
                set (o, "note", str (note));
        }
        set (out, "openvino", o);
    }

    // whisper.cpp
    {
        juce::var o = newObject ();
        set (o, "available", true);
        set (o, "devices", strArray (availableDevices (engine::whisper, false)));
        set (o, "note", "whisper.cpp runs on the CPU (use_gpu=false)");
        set (out, "whisper", o);
    }

    // audio.cpp
    {
        juce::var o = newObject ();
        if (sc::acpp::platformSupported ())
        {
            const juce::File root = audioCppRoot.getFullPathName ().isEmpty () ? sc::acpp::dir () : audioCppRoot;
            const bool has = sc::acpp::hasBinary (root);
            const std::string variant = sc::acpp::installedVariant (root).toStdString ();
            set (o, "available", has);
            set (o, "installedVariant", has ? strOrNull (variant) : juce::var ());
            set (o, "backends", strArray (has ? audioCppBackendsForVariant (variant) : std::vector<std::string> {}));
            if (!has) set (o, "reason", kErrModelNotInstalled);
        }
        else
        {
            set (o, "available", false);
            set (o, "reason", kErrUnsupportedPlatform);
            set (o, "installedVariant", juce::var ());
            set (o, "backends", strArray ({}));
        }
        set (o, "platform", str (sc::acpp::platformTag ()));
        set (out, "audiocpp", o);
    }
    return out;
}

}} // namespace sc::ai
