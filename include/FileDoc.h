#pragma once
// SpectroCraft - per-file document state
#include <JuceHeader.h>
#include "AudioEngine.h"
#include "MarkerManager.h"
#include "ScaleCalibration.h"
#include "ScttChunk.h"
#include "UndoManager.h"

/** Everything that belongs to one loaded audio file: its audio data, markers,
    edit history and the UI cursor/selection. Multiple FileDocs can be open at
    once; Application keeps track of which one is active. */
class FileDoc
{
public:
    FileDoc();

    juce::String name;                 // display name (file name)
    juce::File file;                   // full path
    AudioEngine engine;                // loaded audio (streaming or in-memory)
    MarkerManager markers;             // per-file marker list (+ sidecar persistence)
    ::UndoManager undo;                // per-file edit history

    double playhead = 0.0;             // UI cursor position (seconds)
    juce::Range<double> selection { 0, 0 };
    std::vector<int> visibleChannels;  // sorted channel indices to display
    ScaleCalibration scale;            // physical-unit calibration (Set Scale menu)

    // Whisper transcription segments (Phase 3). Stored in the file as a RIFF
    // "SCTT" chunk for .wav saves; shown in the Transcription panel, never in
    // the Markers panel.
    ScttData sctt;
    void setTranscription (std::vector<ScttSegment> segs) { sctt.segments = std::move (segs); refreshScttJson (); }

    /** Load the file into this document: audio + markers (sidecar JSON first,
        embedded RIFF cue/adtl as fallback for .wav) + initial UI state. */
    /** Loads audio + markers. onProgress (optional) receives 0..1 as the
        load advances; safe to call from a background thread. */
    bool load (const juce::File& f, std::function<void(float)> onProgress = nullptr);

private:
    std::string scttJson;              // serialized form for WAV saves (kept in sync)
    void refreshScttJson () { scttJson = sctt.empty () ? std::string() : scttToJson (sctt); }

    JUCE_DECLARE_NON_COPYABLE (FileDoc)
};
