#pragma once
// SpectroCraft - per-file document state
#include <JuceHeader.h>
#include "AudioEngine.h"
#include "MarkerManager.h"
#include "SavedSelections.h"
#include "ScaleCalibration.h"
#include "ScttChunk.h"
#include "FileMeta.h"
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
    MarkerManager markers;             // per-file marker list
    ::UndoManager undo;                // per-file edit history

    double playhead = 0.0;             // UI cursor position (seconds)
    juce::Range<double> selection { 0, 0 };   // time-range selection (mirrors the spectral one's extent)
    SpectralSelection spectralSel;     // per-file spectral (time x frequency) selection
    std::vector<int> visibleChannels;  // sorted channel indices to display
    ScaleCalibration scale;            // physical-unit calibration (Set Unit and Scale menu)

    // Named spectral selections (Selection panel). Persisted automatically with
    // the file's metadata bundle (embedded SCMD chunk or .meta.json sidecar).
    std::vector<SavedSpectralSelection> savedSelections;

    // Whisper transcription segments. Embedded mode: RIFF "SCTT" chunk inside
    // the WAV; external mode: the "transcription" key of .meta.json. Shown in
    // the Transcription panel, never in the Markers panel.
    ScttData sctt;
    void setTranscription (std::vector<ScttSegment> segs) { sctt.segments = std::move (segs); refreshScttJson (); }

    // ---- Unified metadata storage -----------------------------------------
    // Where this file's metadata bundle lives. Detected on load: an existing
    // .meta.json (or legacy sidecar) means external, embedded RIFF chunks mean
    // embedded; new files default to embedded for WAV and external otherwise.
    sc::MetaMode metaMode = sc::MetaMode::Embedded;

    // Per-file copies of the session-wide display state, persisted with the
    // bundle (restored into the session when this doc becomes active).
    sc::FreqAnalysisState freqAnalysis;   // Frequency Analysis panel settings
    sc::AltRefState altRef;               // axis reference calibrations

    // True when the loaded file actually carried these categories in its
    // metadata. When false, Application inherits the current session state
    // so opening a fresh file does not reset the user's display settings.
    bool haveStoredFreqAnalysis = false;
    bool haveStoredAltRef       = false;

    /** Serialises the current bundle WITHOUT transcription - exactly the
        payload of an embedded SCMD chunk. Empty string when there is nothing
        worth embedding (the chunk is then omitted). */
    std::string buildScmdJson () const;

    /** Serialises the full bundle INCLUDING transcription - the contents of
        the external .meta.json sidecar. */
    std::string buildMetaJson () const;

    /** Applies a parsed bundle to this document (markers, selections, scale,
        reference calibrations, analysis settings and transcription). */
    void applyBundle (const sc::MetaBundle& b);

    /** Writes "<target>.meta.json" with the current full bundle. Returns false
        when target is empty or the file cannot be written. */
    bool writeMetaSidecar (const juce::File& target) const;

    /** Loads audio + all metadata (unified detection: .meta.json > legacy
        sidecars > embedded RIFF chunks). onProgress (optional) receives 0..1
        as the load advances; safe to call from a background thread. If cancel
        points at an atomic set to true, loading stops at a checkpoint and
        false is returned with no state committed to this doc. */
    bool load (const juce::File& f, std::function<void(float)> onProgress = nullptr,
               const std::atomic<bool>* cancel = nullptr);

private:
    std::string scttJson;              // serialized form for WAV saves (kept in sync)
    void refreshScttJson () { scttJson = sctt.empty () ? std::string() : scttToJson (sctt); }

    JUCE_DECLARE_NON_COPYABLE (FileDoc)
};
