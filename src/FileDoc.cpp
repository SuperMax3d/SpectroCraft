/**
 * SpectroCraft - per-file document state
 */

#include "FileDoc.h"
#include "VideoFrames.h"   // isVideoPictureCandidate
#include "Diagnostics.h"
#include <atomic>
#include <cmath>

static int nextFileDocId ()
{
    static std::atomic<int> counter { 0 };
    return ++counter;   // documents may be built on worker threads (loadFileAsync)
}

FileDoc::FileDoc() : id (nextFileDocId ())
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

static double diagMb (juce::int64 bytes) { return std::round ((double) bytes / (1024.0 * 102.4)) / 10.0; }

bool FileDoc::load (const juce::File& f, std::function<void(float)> onProgress,
                    const std::atomic<bool>* cancel)
{
    // Diagnostic log: the path is written once here (doc.open), later events
    // refer to the document id.
    const double diagT0 = sc::diag::enabled() ? juce::Time::getMillisecondCounterHiRes() : 0.0;
    if (!engine.loadFile (f, onProgress, cancel))
    {
        if (diagT0 > 0.0)
        {
            const bool cancelled = cancel != nullptr && cancel->load();
            sc::diag::event ("file.load", cancelled ? sc::diag::Level::info : sc::diag::Level::error,
                             { { "path", f.getFullPathName() }, { "ok", false }, { "cancelled", cancelled },
                               { "exists", f.existsAsFile() }, { "sizeMb", diagMb (f.getSize()) },
                               { "durMs", juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - diagT0) } });
        }
        return false;
    }

    file = f;
    name = f.getFileName();
    videoFile = sc::isVideoPictureCandidate (f) ? f : juce::File();

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

    // Tables: unified > SCMD (same precedence as the drawings). The default
    // display mode draws them only inside the Tables panel.
    tables.clear ();
    tablesMode = sc::tables::kModePanelOnly;
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

    // Video tracks: unified > SCMD. A document opened from a video file has
    // no explicit list and falls back to its implicit `videoFile` track.
    if (unifiedB.haveVideoTracks)
        videoTracks = unifiedB.videoTracks;
    else if (scmdB.haveVideoTracks)
        videoTracks = scmdB.videoTracks;

    // Session (issue #21): unified > SCMD. Application builds the composite.
    session = sc::session::Session {};
    pendingSession = unifiedB.haveSession ? unifiedB.session
                     : (scmdB.haveSession ? scmdB.session : juce::var ());

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
    // The calibration is applied only when EVERY channel has a readable value:
    // the 1.0 fallback would otherwise show dB-SPL figures for a fictitious
    // 1 Pa full scale, so the scale loaded from the metadata stays in place.
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
                bool allReadable = nch > 0;
                for (int i = 0; i < nch; ++i)
                {
                    vals[(size_t) i] = bk.calibrationFor (i);
                    allReadable = allReadable && bk.hasCalibrationFor (i);
                }

                if (allReadable)
                {
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

    if (diagT0 > 0.0)
    {
        const double ms = juce::Time::getMillisecondCounterHiRes() - diagT0;
        sc::diag::event ("doc.open", ms > 5000.0 ? sc::diag::Level::warn : sc::diag::Level::info,
                         { { "doc", id }, { "path", f.getFullPathName() },
                           { "fmt", f.getFileExtension().trimCharactersAtStart (".").toLowerCase() },
                           { "ch", engine.numChannels() }, { "sr", engine.sampleRate() },
                           { "durS", std::round (engine.duration() * 10.0) / 10.0 },
                           { "sizeMb", diagMb (f.getSize()) },
                           { "mode", juce::String (engine.fullyInMemory() ? "memory" : "stream") },
                           { "meta", juce::String (metaMode == sc::MetaMode::External ? "external" : "embedded") },
                           { "video", videoFile != juce::File() }, { "durMs", juce::roundToInt (ms) } });
    }
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
    b.videoTracks  = videoTracks;
    // A document opened from a video file has one implicit track: materialise
    // it so saving as .wav keeps the reference and the video reappears on reopen.
    if (b.videoTracks.empty() && videoFile != juce::File())
        b.videoTracks.push_back ({ videoFile });
    b.scale        = scale;
    b.altRef       = altRef;
    b.freqAnalysis = freqAnalysis;
    if (session.active ())
        b.session = sc::session::toVar (session, engine.getFile ().getParentDirectory ());
    else if (pendingSession.isObject ())
        b.session = pendingSession;   // not opened as a session (files missing): keep the entry

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
    b.videoTracks  = videoTracks;
    if (b.videoTracks.empty() && videoFile != juce::File())
        b.videoTracks.push_back ({ videoFile });
    b.scale        = scale;
    b.altRef       = altRef;
    b.freqAnalysis = freqAnalysis;
    b.transcription = sctt;
    if (session.active ())
        b.session = sc::session::toVar (session, engine.getFile ().getParentDirectory ());
    else if (pendingSession.isObject ())
        b.session = pendingSession;

    return sc::serialiseBundle (b, true);
}

void FileDoc::applyBundle (const sc::MetaBundle& b)
{
    markers.restore (b.markers);
    savedSelections = b.selections;
    drawings        = b.drawings;
    tables          = b.tables;
    tablesMode      = b.tablesMode;
    if (b.haveVideoTracks) videoTracks = b.videoTracks;   // absent key: keep current
    if (b.haveSession) pendingSession = b.session;          // opened by Application
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

void FileDoc::applySessionSaveRegion ()
{
    std::vector<AudioEngine::ChannelRoute> routes;
    if (session.active () && !session.laneChannels.empty ())
    {
        engine.setSaveRegion (session.laneChannels[0], session.clips[0].offset, session.clips[0].length);
        // Playback by track (issue #21): two stereo tracks play L, R, L, R.
        for (int lane = 0; lane < session.numLanes (); ++lane)
            for (int i = 0; i < session.laneChannels[(size_t) lane]; ++i)
                routes.push_back ({ i, session.laneChannels[(size_t) lane] });
    }
    else
        engine.setSaveRegion (0, 0, 0);
    engine.setPlaybackRoutes (std::move (routes));
}

void FileDoc::shiftTimeAnchoredTouching (double a, double b, double dt)
{
    if (dt == 0.0 || !(b >= a))
        return;
    auto touches = [a, b] (double t0, double t1) { return std::max (t0, t1) >= a && std::min (t0, t1) <= b; };

    auto ms = markers.markers ();
    for (auto& m : ms)
        if (touches (m.start, m.end < 0.0 ? m.start : m.end))
        {
            m.start = std::max (0.0, m.start + dt);
            if (m.end >= 0.0) m.end = std::max (m.start, m.end + dt);
        }
    markers.restore (ms);

    auto moveSel = [&] (SpectralSelection& sel)
    {
        if (sel.empty ()) return;
        const auto ext = sel.timeExtent ();
        if (touches (ext.getStart (), ext.getEnd ()))
            sel = sel.translatedCopy (dt, 0.0);
    };
    for (auto& s : savedSelections) moveSel (s.data);
    moveSel (spectralSel);

    for (auto& dr : drawings)
    {
        if (dr.points.empty ()) continue;
        double t0 = dr.points.front ().first, t1 = t0;
        for (const auto& p : dr.points) { t0 = std::min (t0, p.first); t1 = std::max (t1, p.first); }
        if (touches (t0, t1))
            for (auto& p : dr.points) p.first += dt;
    }

    for (auto& tb : tables)
    {
        bool hit = false;
        for (const auto& p : tb.params)
            if (!p.values.empty ()
                && touches (tb.docTimeOfSample (p, 0), tb.docTimeOfSample (p, (int) p.values.size () - 1)))
                hit = true;
        if (hit) tb.offsetSecs += dt;   // the whole table follows the file
    }

    for (auto& seg : sctt.segments)
        if (touches (seg.startSec, seg.endSec))
        {
            seg.startSec = std::max (0.0, seg.startSec + dt);
            seg.endSec = std::max (seg.startSec, seg.endSec + dt);
        }
    metaDirty = true;
}

void FileDoc::remapForDeletion (double a, double b)
{
    if (!(b > a))
        return;
    if (session.active ())
    {
        const double r = engine.sampleRate ();
        sc::session::warpForDeletion (session, std::llround (a * r), std::llround (b * r));
        applySessionSaveRegion ();
    }

    markers.adjustForDeletion (a, b);
    remapSpectralState (*this, [&] (auto& sel) { sel.adjustForDeletion (a, b); });
    warpDrawings (drawings, [&](double t) { return warpDeletion (t, a, b); });
    for (auto& tb : tables)
        sc::tables::warpTableForDeletion (tb, a, b);   // table curves follow the timeline

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
    if (session.active ())
    {
        const double r = engine.sampleRate ();
        sc::session::warpForInsertion (session, std::llround (a * r), std::llround (lenSecs * r));
        applySessionSaveRegion ();
    }

    markers.adjustForInsertion (a, lenSecs);
    remapSpectralState (*this, [&] (auto& sel) { sel.adjustForInsertion (a, lenSecs); });
    warpDrawings (drawings, [&](double t) { return warpInsertion (t, a, lenSecs); });
    for (auto& tb : tables)
        sc::tables::warpTableForInsertion (tb, a, lenSecs);   // table curves follow the timeline
    warpScttSegments (sctt.segments, [&](double t) { return warpInsertion (t, a, lenSecs); });

    metaDirty = true;
}

void FileDoc::remapForStretch (double a, double bOld, double bNew)
{
    if (!(bOld > a))
        return;
    if (session.active ())
    {
        const double r = engine.sampleRate ();
        sc::session::warpForStretch (session, std::llround (a * r), std::llround (bOld * r),
                                     std::llround (bNew * r));
        applySessionSaveRegion ();
    }

    markers.adjustForStretch (a, bOld, bNew);
    remapSpectralState (*this, [&] (auto& sel) { sel.adjustForStretch (a, bOld, bNew); });
    warpDrawings (drawings, [&](double t) { return warpStretch (t, a, bOld, bNew); });
    for (auto& tb : tables)
        sc::tables::warpTableForStretch (tb, a, bOld, bNew);   // table curves follow the timeline
    warpScttSegments (sctt.segments, [&](double t) { return warpStretch (t, a, bOld, bNew); });

    metaDirty = true;
}
