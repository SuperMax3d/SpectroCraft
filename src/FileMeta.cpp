// SpectroCraft - unified per-file metadata bundle (JSON serialisation).
#include "FileMeta.h"
#include <JuceHeader.h>

namespace sc
{

juce::File fileMetaSidecarPath (const juce::File& audioFile)
{
    return juce::File (audioFile.getFullPathName() + ".meta.json");
}

// ---------------------------------------------------------------------------
// WAV metadata-chunk scanner (header-only, constant memory).

// NOTE (this vendored JUCE build): there is no RandomAccessFile/Stream - the
// scan below uses a plain FileInputStream with setPosition().
static bool readFourCC (juce::InputStream& in, char out[4])
{
    return in.read (out, 4) == 4;
}

bool wavHasMetadataChunks (const juce::File& wav, std::string* errOut)
{
    auto fail = [errOut] (const juce::String& msg)
                { if (errOut != nullptr) *errOut = msg.toStdString(); return false; };

    juce::FileInputStream raf (wav);
    if (!wav.existsAsFile () || raf.failedToOpen ())
        return fail ("Cannot open " + wav.getFileName() + ".");

    char riff[4], wave[4];
    if (!readFourCC (raf, riff))
        return fail ("Not a RIFF file.");
    raf.skipNextBytes (4);   // RIFF size field
    if (!readFourCC (raf, wave))
        return fail ("Not a RIFF file.");
    if (std::memcmp (riff, "RIFF", 4) != 0 || std::memcmp (wave, "WAVE", 4) != 0)
        return fail ("Not a WAVE file.");

    const int64_t totalSize = raf.getTotalLength ();
    while (raf.getPosition() + 8 <= totalSize)
    {
        char id[4];
        if (!readFourCC (raf, id)) break;
        uint32_t size = 0;
        if (raf.read (&size, sizeof (size)) != 4) break;   // position is now at the payload start

        bool managed = false;
        if (std::memcmp (id, "cue ", 4) == 0 || std::memcmp (id, "SCTT", 4) == 0
            || std::memcmp (id, "SCMD", 4) == 0)
        {
            managed = true;
        }
        else if (std::memcmp (id, "LIST", 4) == 0 && size >= 8)
        {
            char listType[4];
            const int64_t save = raf.getPosition();
            if (readFourCC (raf, listType))
                managed = std::memcmp (listType, "adtl", 4) == 0;
            raf.setPosition (save);   // only the type was needed
        }

        if (managed) return true;

        const int64_t next = raf.getPosition() + static_cast<int64_t> (size) + (size & 1u);
        if (next <= raf.getPosition ()) break;   // corrupt: avoid infinite loop
        raf.setPosition (next);
    }

    return false;   // no managed chunks found
}

// ---------------------------------------------------------------------------
// JSON serialisation / parsing via juce::JSON.
// NOTE (this vendored build): var has conversion operators (operator int/double)
// and NamedValueSet exposes getWithDefault() - there are no getXxxValue() helpers.

static void appendMarkerObjects (const std::vector<Marker>& markers, juce::Array<juce::var>& arr)
{
    for (const auto& m : markers)
    {
        auto o = new juce::DynamicObject();
        o->setProperty ("id", static_cast<int> (m.id));
        o->setProperty ("name", m.name);
        o->setProperty ("comment", m.comment);
        o->setProperty ("start", m.start);
        o->setProperty ("end", m.end);
        const int argb = (static_cast<int> (m.colour.getAlpha())  << 24)
                       | (static_cast<int> (m.colour.getRed())     << 16)
                       | (static_cast<int> (m.colour.getGreen())   << 8)
                       |  static_cast<int> (m.colour.getBlue());
        o->setProperty ("colour", argb);
        arr.add (o);
    }
}

static void appendPointArray (const std::vector<std::pair<double, double>>& pts,
                              juce::Array<juce::var>& out)
{
    for (const auto& p : pts)
    {
        juce::Array<juce::var> pair;
        pair.add (p.first);
        pair.add (p.second);
        out.add (pair);
    }
}

static void appendSelectionObjects (const std::vector<SavedSpectralSelection>& list,
                                    juce::Array<juce::var>& arr)
{
    for (const auto& sel : list)
    {
        auto o = new juce::DynamicObject();
        o->setProperty ("name", sel.name);
        if (!sel.comment.isEmpty ())
            o->setProperty ("comment", sel.comment);

        juce::Array<juce::var> shapes;
        for (const auto& sh : sel.data.shapes())
        {
            auto so = new juce::DynamicObject();
            switch (sh.kind)
            {
                case SpectralShape::Kind::Rect:    so->setProperty ("kind", "rect");   break;
                case SpectralShape::Kind::Polygon: so->setProperty ("kind", "poly");   break;
                default:                           so->setProperty ("kind", "stroke"); break;
            }
            so->setProperty ("sub", sh.subtract ? 1 : 0);

            switch (sh.kind)
            {
                case SpectralShape::Kind::Rect:
                    so->setProperty ("t0", sh.t0);
                    so->setProperty ("t1", sh.t1);
                    so->setProperty ("f0", sh.f0);
                    so->setProperty ("f1", sh.f1);
                    break;
                case SpectralShape::Kind::Polygon:
                    break;   // pts added by the common block below
                default:   // Stroke
                    so->setProperty ("radius", std::max (1.0, sh.strokeRadiusHz));
                    break;
            }

            if (sh.kind != SpectralShape::Kind::Rect)
            {
                juce::Array<juce::var> pts;
                appendPointArray (sh.pts, pts);
                so->setProperty ("pts", pts);
            }
            shapes.add (so);
        }
        o->setProperty ("shapes", shapes);
        arr.add (o);
    }
}

// NOTE (this vendored build): var is not polymorphic - use isArray()/getArray()
// and getDynamicObject() instead of dynamic_cast.
static bool readPointPair (const juce::var& v, std::pair<double, double>& out)
{
    const auto* a = v.isArray () ? v.getArray() : nullptr;
    if (a == nullptr || a->size() != 2) return false;
    out.first  = (*a)[0].isDouble() ? static_cast<double> ((*a)[0]) : 0.0;
    out.second = (*a)[1].isDouble() ? static_cast<double> ((*a)[1]) : 0.0;
    return true;
}

/** Reads the "pts" array of a shape object into `out`. */
static bool readPtsArray (const juce::NamedValueSet& p, std::vector<std::pair<double, double>>& out)
{
    const juce::var ptsVar = p.getWithDefault ("pts", juce::var());
    const auto* pts = ptsVar.isArray () ? ptsVar.getArray() : nullptr;
    if (pts == nullptr) return false;
    for (int i = 0; i < pts->size(); ++i)
        if (!readPointPair ((*pts)[i], out.emplace_back (0.0, 0.0)))
            return false;
    return true;
}

static bool readSelectionObjects (const juce::var& v, std::vector<SavedSpectralSelection>& out)
{
    const auto* arr = v.isArray () ? v.getArray() : nullptr;
    if (arr == nullptr) return false;
    for (int i = 0; i < arr->size(); ++i)
    {
        const auto* o = (*arr)[i].getDynamicObject();
        if (o == nullptr) continue;
        SavedSpectralSelection sel;
        const auto& props = o->getProperties();
        sel.name    = props.getWithDefault ("name", "").toString();
        sel.comment = props.getWithDefault ("comment", "").toString();

        const auto& p = o->getProperties();
        const juce::var shapesVar = p.getWithDefault ("shapes", juce::var());
        const auto* shapes = shapesVar.isArray () ? shapesVar.getArray() : nullptr;
        if (shapes == nullptr) continue;

        for (int s = 0; s < shapes->size(); ++s)
        {
            const auto* so = (*shapes)[s].getDynamicObject();
            if (so == nullptr) return false;
            const auto& sp = so->getProperties();
            const juce::String kind = sp.getWithDefault ("kind", "rect").toString();
            const bool subtract = static_cast<int> (sp.getWithDefault ("sub", 0)) != 0;

            if (kind == "rect")
                sel.data.addRect (static_cast<double> (sp.getWithDefault ("t0", 0.0)),
                                  static_cast<double> (sp.getWithDefault ("t1", 0.0)),
                                  static_cast<double> (sp.getWithDefault ("f0", 0.0)),
                                  static_cast<double> (sp.getWithDefault ("f1", 0.0)),
                                  subtract, false);
            else if (kind == "poly")
            {
                std::vector<std::pair<double, double>> pts;
                if (!readPtsArray (sp, pts) || pts.size() < 3)
                    return false;
                sel.data.addPolygon (std::move (pts), subtract, false);
            }
            else if (kind == "stroke")
            {
                const double radius = std::max (1.0, static_cast<double> (sp.getWithDefault ("radius", 4.0)));
                std::vector<std::pair<double, double>> pts;
                if (!readPtsArray (sp, pts) || pts.empty())
                    return false;
                sel.data.addStroke (std::move (pts), radius, subtract, false);
            }
            else
                return false;   // unknown shape kind
        }
        out.push_back (std::move (sel));
    }
    return true;
}

static bool readMarkerObjects (const juce::var& v, std::vector<Marker>& out)
{
    const auto* arr = v.isArray () ? v.getArray() : nullptr;
    if (arr == nullptr) return false;
    for (int i = 0; i < arr->size(); ++i)
    {
        const auto* o = (*arr)[i].getDynamicObject();
        if (o == nullptr) continue;
        const auto& p = o->getProperties();
        Marker m;
        m.id      = static_cast<int> (p.getWithDefault ("id", 0));
        m.name    = p.getWithDefault ("name", "").toString();
        m.comment = p.getWithDefault ("comment", "").toString();
        m.start   = static_cast<double> (p.getWithDefault ("start", 0.0));
        m.end     = static_cast<double> (p.getWithDefault ("end", -1.0));
        m.colour  = juce::Colour (static_cast<int> (p.getWithDefault ("colour", 0)));
        out.push_back (std::move (m));
    }
    return true;
}

std::string serialiseBundle (const MetaBundle& b, bool includeTranscription)
{
    auto root = new juce::DynamicObject();
    root->setProperty ("app", "SpectroCraft");
    root->setProperty ("version", 1);

    {
        juce::Array<juce::var> arr;
        appendMarkerObjects (b.markers, arr);
        root->setProperty ("markers", arr);
    }
    {
        juce::Array<juce::var> arr;
        appendSelectionObjects (b.selections, arr);
        root->setProperty ("selections", arr);
    }

    auto scale = new juce::DynamicObject();
    scale->setProperty ("unit", static_cast<int> (b.scale.unit));
    scale->setProperty ("fullScaleValue", b.scale.fullScaleValue);
    if (!b.scale.channelFullScale.empty())
    {
        juce::Array<juce::var> arr;
        for (const double v : b.scale.channelFullScale)
            arr.add (juce::var (v));
        scale->setProperty ("channelFullScale", arr);
    }
    root->setProperty ("scale", scale);

    auto appendAltRefs = [] (const std::vector<AltScaleRefData>& list,
                             juce::Array<juce::var>& arr)
    {
        for (const auto& r : list)
        {
            auto o = new juce::DynamicObject();
            o->setProperty ("name", r.name);
            o->setProperty ("enabled", static_cast<int> (r.enabled));
            o->setProperty ("refMain", r.refMain);
            o->setProperty ("altValue", r.altValue);
            arr.add (o);
        }
    };

    auto altRef = new juce::DynamicObject();
    altRef->setProperty ("freqScaleMode", b.altRef.freqScaleMode);
    altRef->setProperty ("freqFactor", b.altRef.freqFactor);
    juce::Array<juce::var> timeRefs, freqRefs;
    appendAltRefs (b.altRef.altTimeRefs, timeRefs);
    appendAltRefs (b.altRef.altFreqRefs, freqRefs);
    altRef->setProperty ("altTimeRefs", timeRefs);
    altRef->setProperty ("altFreqRefs", freqRefs);
    root->setProperty ("altRef", altRef);

    auto fa = new juce::DynamicObject();
    fa->setProperty ("axisMode", b.freqAnalysis.axisMode);
    fa->setProperty ("fftSize", b.freqAnalysis.fftSize);
    fa->setProperty ("thirdOctOrder", b.freqAnalysis.thirdOctOrder);
    fa->setProperty ("refTime", b.freqAnalysis.refTime);
    fa->setProperty ("smooth", b.freqAnalysis.smooth);
    fa->setProperty ("track", b.freqAnalysis.track);
    root->setProperty ("freqAnalysis", fa);

    if (includeTranscription)
    {
        auto tr = new juce::DynamicObject();
        tr->setProperty ("version", b.transcription.version);
        juce::Array<juce::var> segs;
        for (const auto& s : b.transcription.segments)
        {
            auto so = new juce::DynamicObject();
            so->setProperty ("start", s.startSec);
            so->setProperty ("end", s.endSec);
            // String(const char*) is ASCII-only in this JUCE build; the text
            // is UTF-8, so decode it explicitly.
            so->setProperty ("text",
                s.text.empty () ? juce::String ()
                                : juce::String::fromUTF8 (s.text.c_str (), (int) s.text.size ()));
            segs.add (so);
        }
        tr->setProperty ("segments", segs);
        root->setProperty ("transcription", tr);
    }

    // Compact for the SCMD chunk payload; readable layout for .meta.json.
    // Force ASCII encoding: this JUCE build's JSON UTF-8 mode writes raw
    // multi-byte sequences into the MemoryOutputStream, whose toString()
    // re-reads them as Latin-1 and double-encodes any non-ASCII text.
    juce::MemoryOutputStream mo;
    juce::JSON::writeToStream (mo, juce::var (root),
        juce::JSON::FormatOptions {}
            .withSpacing (!includeTranscription ? juce::JSON::Spacing::singleLine
                                                : juce::JSON::Spacing::multiLine)
            .withMaxDecimalPlaces (9)
            .withEncoding (juce::JSON::Encoding::ascii));
    return mo.toString().toStdString();
}

bool parseBundle (const juce::String& json, MetaBundle& out, std::string* errOut)
{
    const auto v = juce::JSON::parse (json);
    if (!v.isObject())
    {
        if (errOut != nullptr) *errOut = "Not a valid JSON object.";
        return false;
    }

    const auto* root = v.getDynamicObject();
    if (root == nullptr)
    {
        if (errOut != nullptr) *errOut = "Unexpected JSON structure.";
        return false;
    }
    const auto& p = root->getProperties();

    // Each category is optional: absent keys leave the caller's values intact.
    out.haveMarkers = p.contains ("markers");
    if (out.haveMarkers)
    {
        std::vector<Marker> m;
        if (!readMarkerObjects (p.getWithDefault ("markers", juce::var()), m))
            return false;
        out.markers = std::move (m);
    }

    out.haveSelections = p.contains ("selections");
    if (out.haveSelections)
    {
        std::vector<SavedSpectralSelection> sels;
        if (!readSelectionObjects (p.getWithDefault ("selections", juce::var()), sels))
            return false;
        out.selections = std::move (sels);
    }

    out.haveScale = p.contains ("scale");
    if (const auto* scaleObj = p.getWithDefault ("scale", juce::var()).getDynamicObject())
    {
        const auto& sp = scaleObj->getProperties();
        ScaleCalibration sc;
        sc.unit = static_cast<ScaleCalibration::Unit> (static_cast<int> (sp.getWithDefault ("unit", 0)));
        sc.fullScaleValue = std::max (1e-30, static_cast<double> (sp.getWithDefault ("fullScaleValue", 1.0)));
        if (sp.contains ("channelFullScale"))
        {
            const juce::var v = sp.getWithDefault ("channelFullScale", juce::var());
            if (v.isArray())
                for (const auto& item : *static_cast<const juce::Array<juce::var>*> (v.getArray()))
                    sc.channelFullScale.push_back (std::max (1e-30, static_cast<double> (item)));
        }
        out.scale = sc;
    }

    auto readAltRefs = [] (const juce::NamedValueSet& ap, const char* key,
                           std::vector<AltScaleRefData>& list)
    {
        if (!ap.contains (key)) return false;
        const juce::var v = ap.getWithDefault (key, juce::var());
        if (!v.isArray()) return true;   // present but malformed: keep empty
        for (const auto& item : *static_cast<const juce::Array<juce::var>*> (v.getArray()))
        {
            const auto* o = item.getDynamicObject();
            if (o == nullptr) continue;
            AltScaleRefData r;
            const auto& op = o->getProperties();
            // NOTE: this vendored build has no String(var) constructor.
            r.name     = op.getWithDefault ("name", juce::var()).toString();
            r.enabled  = static_cast<int> (op.getWithDefault ("enabled", 1)) != 0;
            r.refMain  = static_cast<double> (op.getWithDefault ("refMain", 0.0));
            r.altValue = static_cast<double> (op.getWithDefault ("altValue", 0.0));
            if (!r.name.isEmpty()) list.push_back (r);
        }
        return true;
    };

    out.haveAltRef = p.contains ("altRef");
    if (const auto* altObj = p.getWithDefault ("altRef", juce::var()).getDynamicObject())
    {
        const auto& ap = altObj->getProperties();
        AltRefState ar;
        ar.freqScaleMode  = std::clamp (static_cast<int> (ap.getWithDefault ("freqScaleMode", 0)), 0, 1);
        ar.freqFactor     = std::max (1e-30, static_cast<double> (ap.getWithDefault ("freqFactor", 1.0)));

        // New array form; fall back to the legacy single-reference keys so old
        // bundles keep working (they become one "Alt 1" entry each).
        if (!readAltRefs (ap, "altTimeRefs", ar.altTimeRefs)
            || !ap.contains ("altTimeRefs"))
        {
            const bool on = static_cast<int> (ap.getWithDefault ("altTimeOn", 0)) != 0;
            if (on)
            {
                AltScaleRefData r;
                r.name     = "Alt 1";
                r.enabled  = true;
                r.refMain  = static_cast<double> (ap.getWithDefault ("altTimeRefSecs", 0.0));
                r.altValue = static_cast<double> (ap.getWithDefault ("altTimeValue", 0.0));
                ar.altTimeRefs.push_back (r);
            }
        }
        if (!readAltRefs (ap, "altFreqRefs", ar.altFreqRefs)
            || !ap.contains ("altFreqRefs"))
        {
            const bool on = static_cast<int> (ap.getWithDefault ("altFreqOn", 0)) != 0;
            if (on)
            {
                AltScaleRefData r;
                r.name     = "Alt 1";
                r.enabled  = true;
                r.refMain  = static_cast<double> (ap.getWithDefault ("altFreqRefHz", 0.0));
                r.altValue = static_cast<double> (ap.getWithDefault ("altFreqValue", 0.0));
                ar.altFreqRefs.push_back (r);
            }
        }

        out.altRef = ar;
    }

    out.haveFreqAnalysis = p.contains ("freqAnalysis");
    if (const auto* faObj = p.getWithDefault ("freqAnalysis", juce::var()).getDynamicObject())
    {
        const auto& fp = faObj->getProperties();
        FreqAnalysisState fa;
        fa.axisMode = std::clamp (static_cast<int> (fp.getWithDefault ("axisMode", 0)), 0, 5);
        fa.fftSize  = std::clamp (static_cast<int> (fp.getWithDefault ("fftSize", 2048)), 1024, 65536);
        {
            // Normalise to one of the supported octave orders.
            static const int kOrders[9] = { 1, 2, 3, 4, 5, 10, 20, 30, 40 };
            const int ord = static_cast<int> (fp.getWithDefault ("thirdOctOrder", 40));
            fa.thirdOctOrder = 40;
            for (int i = 0; i < 9; ++i)
                if (kOrders[i] == ord) { fa.thirdOctOrder = ord; break; }
        }
        fa.refTime  = std::clamp (static_cast<int> (fp.getWithDefault ("refTime", 0)), 0, 1);
        const int sm = static_cast<int> (fp.getWithDefault ("smooth", 0));
        fa.smooth   = (sm == 3 || sm == 5 || sm == 7 || sm == 9 || sm == 11)
                      ? sm : ((sm >= 1 && sm <= 4) ? 3 : 0);
        fa.track    = std::clamp (static_cast<int> (fp.getWithDefault ("track", 0)), 0, 2);
        out.freqAnalysis = fa;
    }

    out.haveTranscription = p.contains ("transcription");
    if (const auto* trObj = p.getWithDefault ("transcription", juce::var()).getDynamicObject())
    {
        const auto& tp = trObj->getProperties();
        ScttData td;
        td.version = std::max (1, static_cast<int> (tp.getWithDefault ("version", 1)));
        const juce::var segsVar = tp.getWithDefault ("segments", juce::var());
        if (const auto* segs = segsVar.isArray () ? segsVar.getArray() : nullptr)
            for (int i = 0; i < segs->size(); ++i)
            {
                const auto* so = (*segs)[i].getDynamicObject();
                if (so == nullptr) continue;
                const auto& sp = so->getProperties();
                ScttSegment s;
                s.startSec = static_cast<double> (sp.getWithDefault ("start", 0.0));
                s.endSec   = static_cast<double> (sp.getWithDefault ("end", 0.0));
                s.text     = sp.getWithDefault ("text", "").toString().toRawUTF8();
                td.segments.push_back (std::move (s));
            }
        out.transcription = std::move (td);
    }

    return true;
}

// ---------------------------------------------------------------------------
// File-level metadata migration (used by the CLI --meta-mode option).
// ---------------------------------------------------------------------------

bool migrateFileMetadata (const juce::File& wav, MetaSaveTarget target,
                          std::string* errOut)
{
    auto fail = [errOut] (const char* msg) -> bool
    {
        if (errOut != nullptr) *errOut = msg;
        return false;
    };

    if (target == MetaSaveTarget::Auto)
        return true;   // keep whatever the file currently uses
    if (!wav.existsAsFile ())
        return fail ("file not found");
    if (target == MetaSaveTarget::Embedded
        && !wav.getFileExtension().equalsIgnoreCase (".wav"))
        return fail ("only WAV files can carry embedded metadata chunks");

    // ---- Gather every category from its current home ----------------------
    // Same priority as FileDoc::load: unified .meta.json > legacy sidecars >
    // embedded RIFF chunks.
    MetaBundle b;

    const bool isWav = wav.getFileExtension().equalsIgnoreCase (".wav");
    const juce::File unified = fileMetaSidecarPath (wav);
    if (unified.existsAsFile ())
        parseBundle (unified.loadFileAsString (), b);   // corrupt = partial merge

    std::string scmdJson;
    if (isWav && readRiffChunkPayload (wav, "SCMD", scmdJson))
        parseBundle (scmdJson, b);

    // The cue/adtl payloads are sample-based: the file's rate is needed both to
    // READ them and to REBUILD them below.
    double rate = 0.0;
    int64_t frames = 0;
    if (isWav)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        if (auto* reader = fm.createReaderFor (wav))
        {
            rate   = reader->sampleRate;
            frames = reader->lengthInSamples;
        }
    }

    // Markers: unified > legacy .markers.json > SCMD > cue/adtl.
    if (!b.haveMarkers)
    {
        const juce::File legacyM = juce::File (wav.getFullPathName() + ".markers.json");
        MarkerManager mm;
        if (legacyM.existsAsFile () && mm.loadSidecar (legacyM))
            b.markers = mm.markers();
        else if (isWav && mm.loadFromWavFile (wav, rate))
            b.markers = mm.markers();
    }

    // Selections: unified > legacy .selections.json > SCMD.
    if (!b.haveSelections)
    {
        const juce::File legacyS = savedSelectionsSidecarPath (wav);
        if (legacyS.existsAsFile ()
            && deserialiseSavedSelections (legacyS.loadFileAsString (), b.selections))
            ;   // filled
    }

    // Transcription: unified > SCTT chunk.
    if (!b.haveTranscription && isWav)
        readScttChunk (wav, b.transcription);

    // ---- Write the bundle in the requested mode ---------------------------
    const juce::File legacyM = juce::File (wav.getFullPathName() + ".markers.json");
    const juce::File legacyS = savedSelectionsSidecarPath (wav);

    auto mkEntry = [] (const char* id, bool adtlList,
                       std::string payload, bool present) -> RiffPatchEntry
    {
        RiffPatchEntry e;
        std::memcpy (e.fourcc, id, 4);
        e.adtlList = adtlList;
        e.payload  = std::move (payload);
        e.present  = present;
        return e;
    };

    if (target == MetaSaveTarget::Embedded)
    {
        const std::string cueP  = buildCueChunkPayload (b.markers, rate, frames);
        const std::string adtlP = buildAdtlListPayload (b.markers, rate, frames);
        const std::string scttP = b.transcription.empty () ? std::string()
                                                           : scttToJson (b.transcription);
        const std::string scmdP = b.hasContent () ? serialiseBundle (b, false)
                                                  : std::string();

        std::vector<RiffPatchEntry> entries;
        entries.push_back (mkEntry ("cue ", false, cueP,  !cueP.empty ()));
        entries.push_back (mkEntry ("LIST", true,  adtlP, !adtlP.empty ()));
        entries.push_back (mkEntry ("SCTT", false, scttP, !scttP.empty ()));
        entries.push_back (mkEntry ("SCMD", false, scmdP, !scmdP.empty ()));

        if (!patchRiffChunks (wav, entries, errOut))
            return false;

        // The sidecars no longer hold the authoritative copy.
        if (unified.existsAsFile())  unified.deleteFile();
        if (legacyM.existsAsFile())  legacyM.deleteFile();
        if (legacyS.existsAsFile())  legacyS.deleteFile();
    }
    else   // External
    {
        const juce::File sidecar = fileMetaSidecarPath (wav);
        if (!sidecar.replaceWithText (serialiseBundle (b, true)))
            return fail ("could not write the .meta.json sidecar");

        legacyM.deleteFile();   // consolidated into the unified sidecar
        legacyS.deleteFile();

        // Drop any stale embedded chunks so the sidecar is the only copy.
        if (isWav && wavHasMetadataChunks (wav))
        {
            std::vector<RiffPatchEntry> entries;
            entries.push_back (mkEntry ("cue ", false, {}, false));
            entries.push_back (mkEntry ("LIST", true,  {}, false));
            entries.push_back (mkEntry ("SCTT", false, {}, false));
            entries.push_back (mkEntry ("SCMD", false, {}, false));
            if (!patchRiffChunks (wav, entries, errOut))
                return false;
        }
    }

    return true;
}

} // namespace sc
