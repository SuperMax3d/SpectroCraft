#pragma once
// SpectroCraft - per-file document state
#include <JuceHeader.h>
#include "AudioEngine.h"
#include "MarkerManager.h"
#include "SavedSelections.h"
#include "ScaleCalibration.h"
#include "ScttChunk.h"
#include "BkdkChunk.h"
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

    /** True once the user has opened "Split Spectral Selection Configuration"
        for this file. Per-file-open state: Execute uses the saved split
        configuration only after it was configured at least once per open;
        otherwise it opens the configuration window first (no split). */
    bool splitConfigured = false;
    ScaleCalibration scale;            // physical-unit calibration (Set Unit and Scale menu)

    /** Raw payload of the source file's "bkdk" RIFF chunk, kept so WAV
        saves can preserve it byte-for-byte. Empty when the source had none.
        Loading a file with this chunk auto-calibrates `scale` to Pascal using
        each channel's Calibration field (see src/FileDoc.cpp). */
    std::string originalBkdkPayload;

    // Named spectral selections (Selection panel). Persisted automatically with
    // the file's metadata bundle (embedded SCMD chunk or .meta.json sidecar).
    std::vector<SavedSpectralSelection> savedSelections;

    /** Adds a named selection and marks the document metadata-dirty (the
        list no longer matches what is on disk). */
    void addSavedSelection (SavedSpectralSelection s)
    {
        savedSelections.push_back (std::move (s));
        metaDirty = true;
    }

    // Spectrogram drawings (Drawing panel): text, arrow, rectangle and free
    // annotations in time x frequency coordinates. Persisted with the file's
    // metadata bundle like the saved selections.
    std::vector<DrawingItem> drawings;

    // Tables panel: imported CSV tables with per-parameter alignment plus the
    // display mode (0=Overlay on Waveform 1=Overlay on Spectrogram
    // 2=Dedicated track). Persisted with the file's metadata bundle.
    std::vector<sc::tables::TableData> tables;
    int tablesMode = 0;

    /** Replaces the table list (and optionally the display mode) and marks
        the document metadata-dirty. */
    void setTables (std::vector<sc::tables::TableData> t, int* newMode = nullptr)
    {
        tables = std::move (t);
        if (newMode != nullptr) tablesMode = *newMode;
        metaDirty = true;
    }

    // Whisper transcription segments. Embedded mode: RIFF "SCTT" chunk inside
    // the WAV; external mode: the "transcription" key of .meta.json. Shown in
    // the Transcription panel, never in the Markers panel.
    ScttData sctt;
    void setTranscription (std::vector<ScttSegment> segs)
    {
        sctt.segments = std::move (segs);
        refreshScttJson ();
        metaDirty = true;   // transcription is per-file metadata
    }

    /** Remap every time-anchored piece of this document's state after the
        range [a, b] seconds has been removed from the audio: markers,
        saved spectral selections and the active spectral selection are
        adjusted with their own adjustForDeletion; user drawings and SCTT
        segments are warped through W(t) = t (t<a), a (a<=t<b),
        t-(b-a) (t>=b); SCTT segments fully inside [a, b] are dropped.
        Called after the audio edit has been applied. */
    void remapForDeletion (double a, double b);

    /** Same for an insertion of len seconds at time a: W(t) = t (t<a),
        t+len (t>=a). Shapes straddling the insertion point are split.
        Called after the audio edit has been applied. */
    void remapForInsertion (double a, double lenSecs);

    /** Same for stretching [a, bOld] seconds to [a, bNew]: W(t) = t (t<a),
        a+(t-a)*(bNew-a)/(bOld-a) inside the range, t+(bNew-bOld) after it.
        Called after the audio edit has been applied. */
    void remapForStretch (double a, double bOld, double bNew);

    // ---- Unified metadata storage -----------------------------------------
    // Where this file's metadata bundle lives. Detected on load: an existing
    // .meta.json (or legacy sidecar) means external, embedded RIFF chunks mean
    // embedded; new files default to embedded for WAV and external otherwise.
    sc::MetaMode metaMode = sc::MetaMode::Embedded;

    /** True when this document's non-audio state (markers, saved selections,
        calibrations, analysis settings, transcription) differs from what is
        on disk. Set by the mutation entry points; cleared whenever the
        metadata is written to disk (Save / Save As / Save Metadata /
        storage-mode switch). load() ends with it false. */
    bool metaDirty = false;

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
