#pragma once
// SpectroCraft - Application state (owns documents, views, markers, undo)
#include <JuceHeader.h>
#include "Settings.h"
#include "AudioEngine.h"
#include "SpectralEngine.h"
#include "MarkerManager.h"
#include "UndoManager.h"
#include "FileDoc.h"
#include <vector>
#include <memory>
#include <functional>
#include <atomic>

// Result of a batch folder transcription run (see Application::batchTranscribeFolder).
struct BatchTranscribeSummary
{
    int total = 0;      // WAV files found in the folder tree
    int done = 0;       // transcribed + SCTT chunk written
    int skipped = 0;    // already had an SCTT chunk (skipExisting mode)
    int failed = 0;     // load/inference/write problems
    std::vector<std::string> failures;   // "<file name>: <error>", in run order
};

class Application
{
public:
    // recentStorePath: optional override for the persisted recent-files store
    // (unit tests point it at a temp file; empty = default app-data location).
    explicit Application (const juce::File& recentStorePath = {});
    ~Application();

    // All loaded files. The active document is what the views and panels show.
    std::vector<std::unique_ptr<FileDoc>> docs;
    int activeDocIndex = -1;

    // Fallback objects used while no file is loaded. Every code path guards on
    // isFileLoaded(), so these are never actually edited or played.
    AudioEngine emptyEngine;
    MarkerManager emptyMarkers;
    ::UndoManager emptyUndo;
    ScaleCalibration emptyScale;        // uncalibrated fallback (no file loaded)

    SpectralEngine spectralEngine;   // shared across files (cache cleared on switch)

    // Real-time output: a player callback streaming from the active engine,
    // attached to an AudioDeviceManager. Declared after all engines so it is
    // destroyed before them, and in this order so the manager stops the device
    // before the player is destroyed.
    juce::AudioSourcePlayer sourcePlayer;
    std::unique_ptr<juce::AudioDeviceManager> audioDevice;

    Settings settings;
    ColourMap colourMap = ColourMap::Classic;

    // Spectral selection tools (Selection menu). The tool + brush size are app-
    // level (persist across document switches); the shapes themselves live in
    // FileDoc::spectralSel. playSpectralSel is auto-cleared when the active
    // spectral selection disappears.
    SpectralSelMode selMode = SpectralSelMode::TimeRange;
    double brushRadiusHz = 200.0;   // paintbrush half-width in Hz
    bool playSpectralSel = false;

    // Clone & Multiply interactive picking (Selection panel): the first click
    // sets the reference frequency, every following click adds clones at
    // (clicked freq / ref freq). App-level like selMode.
    bool clonePicking = false;
    bool haveClonePickRef = false;
    double clonePickRefF = 0.0;
    std::vector<std::pair<double, double>> clonePickPts;   // all picked points incl. the reference
    SpectralSelection clonePickBase;   // snapshot of the selection when picking started

    // Current UI state - mirrors the active document (kept in sync on switch
    // and through setSelection()/setPlayhead()/toggleChannelVisible()).
    juce::Range<double> selection{ 0, 0 };
    double viewStart = 0, viewEnd = 10;   // canonical time window (follows the waveform view)

    // UI cursor position in seconds (independent of transport: stays where you
    // clicked it; tracks playback while playing).
    double playhead = 0.0;

    std::vector<int> visibleChannels;   // sorted list of visible channel indices (any subset)

    FileDoc* doc() const;               // active document, or nullptr
    AudioEngine& audioEngine();         // engine of the active doc (or empty fallback)
    MarkerManager& markers();           // markers of the active doc (or empty fallback)
    ::UndoManager& undo();              // edit history of the active doc (or empty fallback)
    const ScaleCalibration& scale() const;   // calibration of the active doc
    bool isDocLoaded (const FileDoc* d) const;

    int numDocs() const { return (int) docs.size(); }

    bool openFile (const juce::File& file);

    /** Loads a file on a background thread (for large files). onProgress is
        called from the worker thread with 0..1; onDone runs on the message
        thread once the load finished (ok = success, doc activated).
        If cancel points at an atomic set to true, loading stops at a
        checkpoint and onDone(false) fires. */
    void loadFileAsync (const juce::File& file,
                        std::function<void(float)> onProgress,
                        std::function<void(bool)> onDone,
                        const std::atomic<bool>* cancel = nullptr);

    // Recently opened files, most recent first. Updated on every successful
    // openFile() and persisted across sessions in a PropertiesFile.
    static constexpr int kMaxRecentFiles = 10;
    std::vector<juce::File> recentFiles;
    std::unique_ptr<juce::PropertiesFile> recentStore;   // no default ctor, so heap-held
    void addRecentFile (const juce::File& file);   // move to front, dedupe, cap at 10
    // Persist the session settings (spectral log/linear + FFT size,
    // frequency-analysis axis mode + FFT size, zero-crossing snap) into the
    // same PropertiesFile as the recent-files list.
    void saveSettings();

    // AI feature preferences, persisted in the same store (keys ai_*).
    bool aiAutoDownload = true;          // download missing models on first use
    std::string lastNsModelId = "denseunet";
    std::string lastTranscribeModelId = "whisper-base";
    std::string lastTranscribeLanguage = "auto";
    bool lastTranscribeTranslate = false;
    bool lastTranscribeFast = false;   // true = greedy decode (fast), false = beam search
    std::string aiDevicePref = "auto"; // "auto" | "CPU" | "GPU" | "NPU" - shared by NS + transcription
    void saveAiSettings();
    void removeRecentFile (const juce::File& file);// drop one entry and persist
    void clearRecentFiles();                        // empty the list and persist it

    void closeFile();                  // closes the ACTIVE document
    void closeDoc (int index);         // closes one specific loaded document
    void activate (int index);         // make a loaded document active
    void cycleActiveDoc (bool forward);// Ctrl+Tab / Ctrl+Shift+Tab
    // Copy the current UI state (playhead/selection/channels) into the active doc.
    void syncActiveDoc();

    void selectAll();

    // ---- Spectral selection tools (Selection menu) ----
    /** Switch the active tool. Returning to Time Range clears any spectral
        selection on the active document. */
    void setSelectionTool (SpectralSelMode m);
    // Clear the active doc's spectral selection (+ play flag, engine mask).
    void clearSpectralSelection();
    // (Re)install the current playback mask on the active engine: a clone of
    // the doc's selection when Play Spectral Selection is on and non-empty.
    void pushSpectralMaskToEngine();
    // True when the active document has a spectral selection.
    bool hasSpectralSelection() const;

    /** Appends additive clones of the active spectral selection at each given
        frequency multiplier (the original stays in place; clone thickness is
        stretched by the same factor). Returns false and fills errOut when
        there is no selection or a multiplier is invalid. */
    bool cloneAndMultiplySelection (const std::vector<double>& multipliers,
                                    std::string* errOut = nullptr);

    // Parses "0.5, 2, 5" into positive multipliers; fills errOut on bad input.
    static std::vector<double> parseMultiplierList (const juce::String& text,
                                                    std::string* errOut);

    /** Replaces the active doc's spectral selection with a copy of `sel`
        (Selection panel "Recall"). */
    void setSpectralSelectionFrom (const SpectralSelection& sel);

    // Interactive clone picking: first click = reference frequency, each later
    // click adds clones at (clicked freq / ref freq). Esc or the panel button
    // ends the session; switching/closing documents does too.
    void startClonePick();
    void endClonePick();
    bool isClonePicking() const { return clonePicking; }
    double clonePickRefFreq() const { return clonePickRefF; }
    bool hasClonePickRef() const { return haveClonePickRef; }
    const std::vector<std::pair<double, double>>& clonePickPoints() const { return clonePickPts; }
    // Called by the spectral view on a left click while picking. Returns false
    // when nothing happened (no selection / bad frequency).
    bool clonePickClick (double tSecs, double fHz);

    // Mirrors the active doc's spectral selection time extent into the shared
    // time selection, re-installs the playback mask and notifies the UI.
    void refreshSpectralSelectionUI();

    // snapEdges = true snaps both selection edges to the nearest zero crossing
    // of the first visible channel when settings.snapToZeroCrossings is ON.
    // Used by the views' drag-to-select so cuts land on silent points.
    void setSelection (juce::Range<double> s, bool snapEdges = false);
    // Destructive operations return false and fill errOut when they cannot be
    // performed (e.g. a streamed file could not be loaded into memory).
    bool cut(std::string* errOut = nullptr);
    void copy();   // read-only; works on streamed files too
    bool paste(std::string* errOut = nullptr);
    bool del(std::string* errOut = nullptr);
    bool silence(std::string* errOut = nullptr);
    bool fadeIn(std::string* errOut = nullptr);
    bool fadeOut(std::string* errOut = nullptr);
    bool normalize(std::string* errOut = nullptr);
    bool reverse(std::string* errOut = nullptr);
    // Multiply the selection by 10^(gainDb/20) on the visible channels.
    bool changeGain (float gainDb, std::string* errOut = nullptr);
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    // BLOCKING — call from a worker thread. Applies AI noise suppression
    // (denseunet or DeepFilterNet2/3, selected by modelId) to the current
    // selection on the visible channels. attenLimDb / df3PostFilter only apply
    // to the DeepFilterNet models. Returns false and fills errOut on failure/
    // cancellation; nothing is modified in that case.
    // autoDownload = false: never download; fail with a manual-install hint
    // when the model is not already present.
    bool noiseSuppress (const std::string& modelId, const std::string& device,
                        float attenLimDb = 0.f, bool df3PostFilter = false,
                        std::function<bool(float)> onProgress = nullptr,
                        std::string* errOut = nullptr,
                        bool autoDownload = true);

    // BLOCKING — call from a worker thread. Transcribes the WHOLE file with
    // Whisper (modelId selects the model; language "auto" or a BCP-47 code;
    // translate = true converts to English). Segments are stored in the active
    // document's SCTT data. Returns false and fills errOut on failure.
    // fast = true uses greedy decoding (~2x faster than beam search, slightly
    // lower accuracy); false keeps the default beam size of 5.
    // Runs on CPU via the prebuilt whisper.cpp b5454 binaries (no device choice).
    bool transcribe (const std::string& modelId, const std::string& language,
                     bool translate, bool fast,
                     std::function<bool(float)> onProgress = nullptr,
                     std::string* errOut = nullptr,
                     bool autoDownload = true);

    // BLOCKING — call from a worker thread. Transcribes every .wav file in
    // `folder` (recursively) WITHOUT opening it in the editor: each file is
    // loaded headlessly, mixed to mono, transcribed with Whisper, and its SCTT
    // chunk replaced in place on disk (all other RIFF chunks preserved).
    // skipExisting = true leaves files that already carry an SCTT chunk alone.
    // onProgress reports overall 0..1; onStatus is called before each file;
    // cancelOut, when set, stops the run after the current file. Returns false
    // (and fills errOut) only for setup failures such as "no WAV files found";
    // per-file problems are tallied in summaryOut instead.
    bool batchTranscribeFolder (const juce::File& folder,
                                const std::string& modelId,
                                const std::string& language, bool translate,
                                bool fast, bool skipExisting,
                                std::function<bool(float)> onProgress = nullptr,
                                std::function<void(const juce::String&)> onStatus = nullptr,
                                std::atomic<bool>* cancelOut = nullptr,
                                BatchTranscribeSummary* summaryOut = nullptr,
                                std::string* errOut = nullptr,
                                bool autoDownload = true);
#endif

    // BLOCKING — call from a worker thread. Runs the Python script `script`
    // (whose on-disk text is first replaced by `scriptText`) against the
    // current selection, or the whole file when nothing is selected.
    // pythonCmd is the interpreter command found by sc::py::findPython
    // (e.g. "python" or "py -3"). The script receives a float32 WAV of the
    // range plus a metadata JSON and must write back an output WAV with the
    // same rate/channels/length; the result replaces that range in place and
    // is undoable. onProgress reports 0..1 (excluding the final mipmap
    // rebuild) and returning false cancels (killing the child process).
    bool runPythonScript (const juce::File& script, const juce::String& scriptText,
                          const std::string& pythonCmd,
                          std::function<bool(float)> onProgress = nullptr,
                          std::string* errOut = nullptr);

    void undoStep(); void redoStep();
    // Batched variants used by the History tab: apply up to n patches in one
    // go with a single mipmap rebuild at the end (fast for long jumps).
    void undoSteps (int n); void redoSteps (int n);

    void play(); void stop(); void pause();
    // Toggle looping. When enabling, the loop range is taken from the current
    // selection (or the whole file if there is no selection).
    void toggleLoop();
    void setPlayhead (double t);

    bool isChannelVisible (int ch) const;
    void toggleChannelVisible (int ch);

    // Add a marker: region spanning the current selection if there is one,
    // otherwise a point marker at the cursor.
    int addMarkerAtCursor();

    // Set by MainWindow: called when panels change shared state (settings, markers,
    // channels) so the main views can be re-synced.
    std::function<void()> onStateChange;

    // Set by MainWindow: called whenever setPlayhead() moves the cursor, so both
    // views always show the same playhead position.
    std::function<void(double)> onPlayheadChanged;

    // Set by MainWindow: fired before a document is destroyed, so panels can
    // drop state that belongs to it (e.g. snapshots stored from that file).
    std::function<void(const FileDoc&)> onFileClosed;

    // Set by MainWindow: called right after [a, b] seconds were deleted from d
    // (the audio patch is already recorded). Lets shared UI state - the FFT
    // snapshot slots - record its before/after into the patch and adjust.
    std::function<void(FileDoc*, double, double, Patch&)> onTimeRangeDeleted;

    // Set by MainWindow: called after an undo (!isRedo) or redo (isRedo)
    // applied a patch with side state to d, so shared UI state is restored too.
    std::function<void(FileDoc*, const Patch&, bool)> onPatchSideStateRestored;

    // Set by MainWindow: fired after open/switch/close so the UI can adapt
    // (reset views to the new file, push its state into every panel).
    std::function<void()> onActiveDocChanged;

private:
    void loadSettings();   // restore persisted session settings from recentStore

    // Restores the non-audio state recorded in a patch (see Patch::hasSideState).
    void restorePatchSideState (FileDoc* d, const Patch& p, bool isRedo);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Application)
};
