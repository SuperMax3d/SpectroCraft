/**
 * SpectroCraft - per-file document state
 */

#include "FileDoc.h"

FileDoc::FileDoc()
{
    // Wire this document's engine to its own undo history, marker list and
    // transcription data so edits are recorded per file and WAV saves embed
    // that file's markers (cue/adtl) and segments (SCTT chunk).
    engine.setUndoManager (&undo);
    engine.setMarkersForSave (&markers.markers());
    engine.setScttForSave (&scttJson);
}

bool FileDoc::load (const juce::File& f, std::function<void(float)> onProgress,
                    const std::atomic<bool>* cancel)
{
    if (!engine.loadFile (f, onProgress, cancel))
        return false;

    file = f;
    name = f.getFileName();

    // Prefer the sidecar when one exists; otherwise recover markers embedded
    // in WAV files (RIFF cue/adtl chunks, classic DAW marker layout).
    if (!markers.loadSidecar (f) && f.getFileExtension().equalsIgnoreCase (".wav"))
        markers.loadFromWavFile (f, engine.sampleRate());

    // Named spectral selections from their sidecar (absent/corrupt = empty list).
    loadSelectionsFromDisk ();

    // Recover transcription segments embedded in WAV files (SCTT chunk);
    // absent or corrupt chunks are simply ignored.
    sctt = ScttData {};
    if (f.getFileExtension().equalsIgnoreCase (".wav"))
        readScttChunk (f, sctt);
    refreshScttJson ();

    playhead = 0.0;
    selection = { 0, 0 };
    visibleChannels.clear();
    for (int i = 0; i < engine.numChannels(); ++i)
        visibleChannels.push_back (i);
    undo.clear();
    return true;
}

void FileDoc::loadSelectionsFromDisk()
{
    savedSelections.clear ();
    selectionsOnDisk = false;

    const juce::File sidecar = savedSelectionsSidecarPath (file);
    if (!sidecar.existsAsFile ())
        return;
    const juce::String s = sidecar.loadFileAsString ();
    if (!s.isEmpty () && deserialiseSavedSelections (s, savedSelections))
        selectionsOnDisk = true;   // a readable sidecar exists: keep it in sync
}

bool FileDoc::saveSelectionsToDisk()
{
    if (file == juce::File ())
        return false;
    const juce::String json = serialiseSavedSelections (savedSelections);
    const juce::File sidecar = savedSelectionsSidecarPath (file);
    sidecar.deleteFile ();   // FileOutputStream appends to existing files!
    juce::FileOutputStream out (sidecar);
    if (out.failedToOpen ())
        return false;
    selectionsOnDisk = true;
    return out.write (json.getCharPointer (), json.getNumBytesAsUTF8 ());
}
