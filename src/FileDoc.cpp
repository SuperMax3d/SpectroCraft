/**
 * SpectroCraft - per-file document state
 */

#include "FileDoc.h"

FileDoc::FileDoc()
{
    // Wire this document's engine to its own undo history, marker list and
    // transcription data so edits are recorded per file and WAV saves embed
    // that file's markers (cue/adtl), segments (SCTT) and the unified bundle
    // (SCMD chunk) - or write a plain WAV in external mode.
    engine.setUndoManager (&undo);
    engine.setMarkersForSave (&markers.markers());
    engine.setScttForSave (&scttJson);
    engine.setMetaModeForSave (&metaMode);
    engine.setBkdkForSave (&originalBkdkPayload);   // preserve the source's bkdk chunk on WAV saves
    engine.setScmdJsonForSave ([this] () { return buildScmdJson (); });
    markers.onChange = [this] { metaDirty = true; };
}

bool FileDoc::load (const juce::File& f, std::function<void(float)> onProgress,
                    const std::atomic<bool>* cancel)
{
    if (!engine.loadFile (f, onProgress, cancel))
        return false;

    file = f;
    name = f.getFileName();

    const bool isWav = f.getFileExtension().equalsIgnoreCase (".wav");

    // ---- Detect where this file's metadata lives --------------------------
    // Priority: unified .meta.json > legacy sidecars (.markers.json /
    // .selections.json) > embedded RIFF chunks. Non-WAV files can only be
    // external.
    const juce::File unified = sc::fileMetaSidecarPath (f);
    const juce::File legacyM  = juce::File (f.getFullPathName() + ".markers.json");
    const juce::File legacyS  = savedSelectionsSidecarPath (f);

    const bool haveUnified  = unified.existsAsFile ();
    const bool haveLegacyM  = legacyM.existsAsFile ();
    const bool haveLegacyS  = legacyS.existsAsFile ();
    const bool haveEmbedded = isWav && sc::wavHasMetadataChunks (f);

    if (haveUnified || haveLegacyM || haveLegacyS)
        metaMode = sc::MetaMode::External;
    else if (haveEmbedded)
        metaMode = sc::MetaMode::Embedded;
    else
        metaMode = isWav ? sc::MetaMode::Embedded : sc::MetaMode::External;

    // ---- Load each category from the best available source ----------------
    sc::MetaBundle unifiedB, scmdB;
    if (haveUnified)
        sc::parseBundle (unified.loadFileAsString (), unifiedB);   // corrupt = partial merge

    std::string scmdJson;
    if (isWav && readRiffChunkPayload (f, "SCMD", scmdJson))
        sc::parseBundle (scmdJson, scmdB);

    // Markers: unified > legacy sidecar > SCMD > cue/adtl.
    if (unifiedB.haveMarkers)
        markers.restore (unifiedB.markers);
    else if (!markers.loadSidecar (f))
    {
        if (scmdB.haveMarkers)
            markers.restore (scmdB.markers);
        else if (isWav)
            markers.loadFromWavFile (f, engine.sampleRate());
    }

    // Selections: unified > legacy sidecar > SCMD.
    savedSelections.clear ();
    bool gotSelections = false;
    if (unifiedB.haveSelections)
    {
        savedSelections = unifiedB.selections;
        gotSelections = true;
    }
    else if (legacyS.existsAsFile () && deserialiseSavedSelections (legacyS.loadFileAsString (), savedSelections))
        gotSelections = true;

    if (!gotSelections && scmdB.haveSelections)
        savedSelections = scmdB.selections;

    // Drawings: unified > SCMD (same precedence as the selections).
    drawings.clear ();
    if (unifiedB.haveDrawings)
        drawings = unifiedB.drawings;
    else if (scmdB.haveDrawings)
        drawings = scmdB.drawings;

    // Tables: unified > SCMD (same precedence as the drawings).
    tables.clear ();
    tablesMode = 0;
    if (unifiedB.haveTables)
    {
        tables     = unifiedB.tables;
        tablesMode = unifiedB.tablesMode;
    }
    else if (scmdB.haveTables)
    {
        tables     = scmdB.tables;
        tablesMode = scmdB.tablesMode;
    }

    // Scale / reference calibrations / analysis settings: unified > SCMD.
    scale        = unifiedB.haveScale        ? unifiedB.scale
                   : (scmdB.haveScale        ? scmdB.scale        : ScaleCalibration {});
    altRef       = unifiedB.haveAltRef       ? unifiedB.altRef
                   : (scmdB.haveAltRef       ? scmdB.altRef       : sc::AltRefState {});
    freqAnalysis = unifiedB.haveFreqAnalysis ? unifiedB.freqAnalysis
                   : (scmdB.haveFreqAnalysis ? scmdB.freqAnalysis : sc::FreqAnalysisState {});
    haveStoredAltRef       = unifiedB.haveAltRef || scmdB.haveAltRef;
    haveStoredFreqAnalysis = unifiedB.haveFreqAnalysis || scmdB.haveFreqAnalysis;

    // "bkdk" calibration chunk (if present): auto-calibrate to Pascal using
    // each channel's Calibration field. A single defined group applies to every
    // channel; the raw payload is kept so WAV saves preserve it byte-for-byte.
    originalBkdkPayload.clear();
    if (isWav)
    {
        std::string raw;
        if (readRiffChunkPayload (f, "bkdk", raw, false, true))
        {
            sc::BkdkData bk;
            if (sc::parseBkdkPayload (raw, bk))
            {
                originalBkdkPayload = raw;
                const int nch = engine.numChannels();
                std::vector<double> vals ((size_t) nch);
                for (int i = 0; i < nch; ++i)
                    vals[(size_t) i] = bk.calibrationFor (i);

                scale.unit = ScaleCalibration::Unit::Pascal;
                bool same = true;
                for (int i = 1; i < nch && same; ++i)
                    if (vals[(size_t) i] != vals[0]) same = false;
                scale.fullScaleValue = vals.empty() ? 1.0 : vals[0];
                scale.channelFullScale.clear();
                if (!same && vals.size() > 1)
                    scale.channelFullScale = std::move (vals);
            }
        }
    }

    // Transcription: unified > SCTT chunk (legacy files may carry it embedded
    // even when other categories live in sidecars).
    sctt = ScttData {};
    if (unifiedB.haveTranscription)
        sctt = unifiedB.transcription;
    else if (isWav)
        readScttChunk (f, sctt);   // absent/corrupt chunks are ignored
    refreshScttJson ();

    playhead = 0.0;
    selection = { 0, 0 };
    visibleChannels.clear();
    for (int i = 0; i < engine.numChannels(); ++i)
        visibleChannels.push_back (i);
    undo.clear();

    // The restore() calls above fired the marker onChange hook while loading;
    // a freshly loaded document matches its own disk state, so start clean.
    metaDirty = false;
    return true;
}

std::string FileDoc::buildScmdJson () const
{
    sc::MetaBundle b;
    b.markers      = markers.markers ();
    b.selections   = savedSelections;
    b.drawings     = drawings;
    b.tables       = tables;
    b.tablesMode   = tablesMode;
    b.scale        = scale;
    b.altRef       = altRef;
    b.freqAnalysis = freqAnalysis;

    if (!b.hasContent ())
        return {};   // nothing worth embedding: omit the SCMD chunk entirely
    return sc::serialiseBundle (b, false);
}

std::string FileDoc::buildMetaJson () const
{
    sc::MetaBundle b;
    b.markers      = markers.markers ();
    b.selections   = savedSelections;
    b.drawings     = drawings;
    b.tables       = tables;
    b.tablesMode   = tablesMode;
    b.scale        = scale;
    b.altRef       = altRef;
    b.freqAnalysis = freqAnalysis;
    b.transcription = sctt;

    return sc::serialiseBundle (b, true);
}

void FileDoc::applyBundle (const sc::MetaBundle& b)
{
    markers.restore (b.markers);
    savedSelections = b.selections;
    drawings        = b.drawings;
    tables          = b.tables;
    tablesMode      = b.tablesMode;
    scale           = b.scale;
    altRef          = b.altRef;
    freqAnalysis    = b.freqAnalysis;
    sctt            = b.transcription;
    refreshScttJson ();
}

bool FileDoc::writeMetaSidecar (const juce::File& target) const
{
    if (target == juce::File ())
        return false;
    const std::string json = buildMetaJson ();
    const juce::File sidecar = sc::fileMetaSidecarPath (target);
    sidecar.deleteFile ();   // FileOutputStream appends to existing files!
    juce::FileOutputStream out (sidecar);
    if (out.failedToOpen ())
        return false;
    return out.write (json.data (), json.size());
}

// ---------------------------------------------------------------------------
// Time-anchored state remapping (insertion / deletion / stretch)
// ---------------------------------------------------------------------------

namespace
{
    // Monotonic time warp for a deletion of [a, b].
    double warpDeletion (double t, double a, double b)
    {
        if (t < a) return t;
        if (t >= b) return t - (b - a);
        return a;
    }

    // Monotonic time warp for an insertion of len seconds at time a.
    double warpInsertion (double t, double a, double len)
    {
        return t < a ? t : t + len;
    }

    // Monotonic time warp for stretching [a, bOld] to [a, bNew].
    double warpStretch (double t, double a, double bOld, double bNew)
    {
        if (t <= a) return t;
        if (t >= bOld) return t + (bNew - bOld);
        const double k = (bNew - a) / (bOld - a);
        return a + (t - a) * k;
    }

    template <typename WarpFn>
    void warpDrawings (std::vector<DrawingItem>& drawings, WarpFn&& W)
    {
        for (auto& dr : drawings)
        {
            dr.t0 = W (dr.t0);
            dr.t1 = W (dr.t1);
            for (auto& p : dr.points)
                p.first = W (p.first);
        }
    }

    template <typename WarpFn>
    void warpScttSegments (std::vector<ScttSegment>& segs, WarpFn&& W)
    {
        for (size_t i = 0; i < segs.size();)
        {
            const double s = segs[i].startSec, e = segs[i].endSec;
            if (W (e) <= W (s))   // collapsed to zero length: dropped
                segs.erase (segs.begin() + (long long) i);
            else
            {
                segs[i].startSec = W (s);
                segs[i].endSec = W (e);
                ++i;
            }
        }
    }

    template <typename AdjustFn>
    void remapSpectralState (FileDoc& d, AdjustFn&& adjust)
    {
        for (auto& ss : d.savedSelections)
            adjust (ss.data);
        adjust (d.spectralSel);
    }
} // anonymous namespace

void FileDoc::remapForDeletion (double a, double b)
{
    if (!(b > a))
        return;

    markers.adjustForDeletion (a, b);
    remapSpectralState (*this, [&] (auto& sel) { sel.adjustForDeletion (a, b); });
    warpDrawings (drawings, [&](double t) { return warpDeletion (t, a, b); });

    // SCTT segments fully inside the deleted range are dropped; the rest are
    // warped (a segment straddling an edge is trimmed to it).
    auto W = [a, b] (double t) { return warpDeletion (t, a, b); };
    for (size_t i = 0; i < sctt.segments.size();)
    {
        const double s = sctt.segments[i].startSec, e = sctt.segments[i].endSec;
        if (s >= a && e <= b)   // fully contained: dropped
            sctt.segments.erase (sctt.segments.begin() + (long long) i);
        else
        {
            sctt.segments[i].startSec = W (s);
            sctt.segments[i].endSec = W (e);
            ++i;
        }
    }

    metaDirty = true;   // markers / selections / drawings / SCTT changed on disk
}

void FileDoc::remapForInsertion (double a, double lenSecs)
{
    if (!(lenSecs > 0.0))
        return;

    markers.adjustForInsertion (a, lenSecs);
    remapSpectralState (*this, [&] (auto& sel) { sel.adjustForInsertion (a, lenSecs); });
    warpDrawings (drawings, [&](double t) { return warpInsertion (t, a, lenSecs); });
    warpScttSegments (sctt.segments, [&](double t) { return warpInsertion (t, a, lenSecs); });

    metaDirty = true;
}

void FileDoc::remapForStretch (double a, double bOld, double bNew)
{
    if (!(bOld > a))
        return;

    markers.adjustForStretch (a, bOld, bNew);
    remapSpectralState (*this, [&] (auto& sel) { sel.adjustForStretch (a, bOld, bNew); });
    warpDrawings (drawings, [&](double t) { return warpStretch (t, a, bOld, bNew); });
    warpScttSegments (sctt.segments, [&](double t) { return warpStretch (t, a, bOld, bNew); });

    metaDirty = true;
}
