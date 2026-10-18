/**
 * SpectroCraft - per-file document state
 */

#include "FileDoc.h"

FileDoc::FileDoc()
{
    // Wire this document's engine to its own undo history and marker list so
    // edits are recorded per file and WAV saves embed that file's markers.
    engine.setUndoManager (&undo);
    engine.setMarkersForSave (&markers.markers());
}

bool FileDoc::load (const juce::File& f, std::function<void(float)> onProgress)
{
    if (!engine.loadFile (f, onProgress))
        return false;

    file = f;
    name = f.getFileName();

    // Prefer the sidecar when one exists; otherwise recover markers embedded
    // in WAV files (RIFF cue/adtl chunks, classic DAW marker layout).
    if (!markers.loadSidecar (f) && f.getFileExtension().equalsIgnoreCase (".wav"))
        markers.loadFromWavFile (f, engine.sampleRate());

    playhead = 0.0;
    selection = { 0, 0 };
    visibleChannels.clear();
    for (int i = 0; i < engine.numChannels(); ++i)
        visibleChannels.push_back (i);
    undo.clear();
    return true;
}
