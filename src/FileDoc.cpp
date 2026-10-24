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
    engine.setScmdJsonForSave ([this] () { return buildScmdJson (); });
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

    // Scale / reference calibrations / analysis settings: unified > SCMD.
    scale        = unifiedB.haveScale        ? unifiedB.scale
                   : (scmdB.haveScale        ? scmdB.scale        : ScaleCalibration {});
    altRef       = unifiedB.haveAltRef       ? unifiedB.altRef
                   : (scmdB.haveAltRef       ? scmdB.altRef       : sc::AltRefState {});
    freqAnalysis = unifiedB.haveFreqAnalysis ? unifiedB.freqAnalysis
                   : (scmdB.haveFreqAnalysis ? scmdB.freqAnalysis : sc::FreqAnalysisState {});
    haveStoredAltRef       = unifiedB.haveAltRef || scmdB.haveAltRef;
    haveStoredFreqAnalysis = unifiedB.haveFreqAnalysis || scmdB.haveFreqAnalysis;

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
    return true;
}

std::string FileDoc::buildScmdJson () const
{
    sc::MetaBundle b;
    b.markers      = markers.markers ();
    b.selections   = savedSelections;
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
