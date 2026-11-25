#pragma once
// SpectroCraft - Application state (owns documents, views, markers, undo)
#include <JuceHeader.h>
#include "ContentAwareTimeScale.h"
#include "Settings.h"
#include "AudioEngine.h"
#include "SpectralEngine.h"
#include "MarkerManager.h"
#include "MergeFiles.h"
#include "UndoManager.h"
#include "FileDoc.h"
#include "StemSeparation.h"
#include "SignalGenerator.h"
#include "Actions.h"
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace sc::control { class ControlState; }   // AI control (include/control/ControlState.h)

// Result of a batch folder transcription run (see Application::batchTranscribeFolder).
struct BatchTranscribeSummary
{
    int total = 0;      // WAV files found in the folder tree
    int done = 0;       // transcribed + SCTT chunk written
    int skipped = 0;    // already had an SCTT chunk (skipExisting mode)
    int failed = 0;     // load/inference/write problems
    std::vector<std::string> failures;   // "<file name>: <error>", in run order
};

class Application : public juce::Timer
{
public:
    // recentStorePath: optional override for the persisted recent-files store
    // (unit tests point it at a temp file; empty = default app-data location).
    explicit Application (const juce::File& recentStorePath = {});
    ~Application();

    // When on, Applications created without an explicit store use a fresh
    // per-run temp store instead of the user's real settings file. Always on
    // in SpectroCraftTests; SpectroCraft --run-unit-tests turns it on too, so
    // both executables run the suite against the same isolated settings.
    static void useIsolatedSettingsStore (bool on);
    /** The per-process temp settings store used when isolation is on. */
    static juce::File isolatedSettingsStoreFile();
    /** Deletes the isolated store (no-op when isolation is off): the next
        Application starts from default settings. */
    static void resetIsolatedSettingsStore();

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

    // Descriptions remembered between open/close cycles of the Stem Separation
    // window (see StemSeparationWindow::rememberDescs()). Anchors are absolute
    // document times; they are dropped on restore when they no longer fit the
    // current file's duration.
    std::vector<StemDescEntry> stemSepSavedDescs;

    // Spectral selection tools (Selection menu). The tool + brush size are app-
    // level (persist across document switches); the shapes themselves live in
    // FileDoc::spectralSel. playSpectralSel is auto-cleared when the active
    // spectral selection disappears.
    SpectralSelMode selMode = SpectralSelMode::TimeRange;
    // Paintbrush size as a percentage of the cursor's own frequency, on a
    // fixed doubling ladder (1/2/4/8/16/32 %). The radius at a stroke point is
    // pct * that point's frequency, so fine work stays possible at low
    // frequencies where an absolute Hz width was blocky.
    static constexpr double brushPctLadder [] = { 0.01, 0.02, 0.04, 0.08, 0.16, 0.32 };
    int brushSizeIndex = 2;   // default: 4 %

    double brushRadiusPct () const { return brushPctLadder [(size_t) brushSizeIndex]; }
    int brushPctValue () const { return (int) std::lround (brushRadiusPct () * 100.0); }
    void setBrushPct (double pct);   // snaps to the nearest ladder step
    bool playSpectralSel = false;

    // Clone & Multiply interactive picking (Selection panel): the first click
    // sets the reference frequency, every following click adds clones at
    // (clicked freq / ref freq). App-level like selMode.
    bool clonePicking = false;
    bool haveClonePickRef = false;
    double clonePickRefF = 0.0;
    std::vector<std::pair<double, double>> clonePickPts;   // all picked points incl. the reference
    // Parallel to clonePickPts: the index in the doc's spectral selection where
    // each point's appended clone block starts (-1 when it added no shapes,
    // i.e. the reference point). Lets a re-click remove a point AND its clones.
    std::vector<int> clonePickShapeStarts;
    SpectralSelection clonePickBase;   // snapshot of the selection when picking started

    // Move/Scale selection gesture (spectral view right-click menu): the first
    // click sets a reference point, mouse movement previews the transform live,
    // the second click applies it and Esc/right-click cancels.
    enum class AxisLock { None, X, Y };   // SHIFT locks to one screen axis
    bool selGesturing = false;
    bool selGestScale = false;            // true = Scale gesture, false = Move
    bool haveSelGestRef = false;
    double selGestRefT = 0.0, selGestRefF = 0.0;
    SpectralSelection selGestBase;        // snapshot of the selection at start
    double selGestW = 1e-3, selGestH = 8.0;   // bbox spans (scale normalisation)

    // Current UI state - mirrors the active document (kept in sync on switch
    // and through setSelection()/setPlayhead()/toggleChannelVisible()).
    juce::Range<double> selection{ 0, 0 };
    // Visible row index where a drag-selection started (-1 = all rows): both
    // views clip their time-selection rectangle to that channel so dragging a
    // selection from one channel over another does not paint over the others.
    int selectionRow { -1 };
    double viewStart = 0, viewEnd = 10;   // canonical time window (follows the waveform view)

    // ---- Recording (plugin-based capture, see include/RecorderPlugin.h) ----
    struct RecordingState
    {
        bool active = false;
        juce::String pluginId, deviceId;   // selected device (persisted)
        int channels = 2;                  // output channel count of the recording doc
        int srcChannels = 0;               // source channels delivered by the plugin (0 = same as channels)
        int channelMode = 0;               // 0 = all source channels; k>=1 = only source channel k
        double rate = 48000.0;
        int bitDepth = 32;                 // negotiated at start
        int64_t framesRecorded = 0;
        bool followEnd = true;             // view follows the recording edge

        // Transport > Recording Options (persisted between sessions).
        enum class Mode { Unlimited, Loop, Timed };
        Mode mode = Mode::Unlimited;
        double durationSecs = 0.0;         // configured length for Loop/Timed (>0)
        int64_t loopCapFrames = 0;         // set at start: max doc length in frames (Loop)
        int64_t timedTargetFrames = 0;     // set at start: auto-stop after this many frames
        juce::Time startTime;              // session start (zero-data watchdog)
    };
    RecordingState recording;

    /** Pulls up to `cap` frames of audio from the active plugin into the
        recording document, honouring a single-channel selection. Returns
        false when nothing was appended (ring empty or audio thread busy). */
    bool pullRecFrames (FileDoc* d, long long cap);

    /** Sets the recording mode (Transport > Recording Options). secs is only
        used for Loop/Timed and must be > 0 to take effect. Persisted. */
    void setRecordingMode (RecordingState::Mode m, double secs = 0.0);

    /** "10s" / "1:30" style menu label for a duration in seconds. */
    static juce::String formatRecDurationSecs (double secs);

    /** Creates a fresh in-memory document and starts capturing from the
        selected recorder plugin/device (Transport > Recording Device). */
    bool startRecording (std::string* errOut);
    void stopRecording();
    FileDoc* recDoc() const { return recDoc_; }

    /** Follow-end state machine: when the window's right edge sits at the end
        of the data it slides to newDur keeping its width; otherwise the
        window is left exactly where the user put it. Pure function so tests
        can drive it directly. */
    static void stepRecordingWindow (double& t0, double& t1, bool& followEnd,
                                     double newDur);
    static constexpr double kRecFollowMinWidthSecs = 5.0;

    void timerCallback() override;

    // Audio clipboard: app-level so content copied from one document can be
    // pasted into another (e.g. a freshly created "New File"). With a spectral
    // selection active at copy time, only the selected (t,f) content is stored.
    std::unique_ptr<juce::AudioBuffer<float>> clipboard;
    bool canPaste() const { return clipboard != nullptr && clipboard->getNumSamples() > 0; }
    void clearClipboard() { clipboard.reset(); }

    // UI cursor position in seconds (independent of transport: stays where you
    // clicked it; tracks playback while playing).
    double playhead = 0.0;

    // ---- Axis scale modes (the Scale form; per-file, see sc::axes) ----
    /** The document's axis entries: the source of truth of the Scale form.
        applyEntries() keeps the fields below in step with the primary entry of
        each kind, so the views keep reading the state they always used. */
    sc::axes::List axisEntries;
    TimeAxisMode timeAxisMode = TimeAxisMode::Time;
    double timeOffsetSecs = 0.0;   // displayed(t) = (t + offset) * scale
    double timeScaleFactor = 1.0;  // extra multiplier of the time axis
    int waveVScaleMode = 0;        // waveform vertical axis: 0=Wav Units, 1=dB, 2=Normalized
    int specFreqScaleMode = 0;     // spectrogram frequency axis: 0=Hz, 1=Custom
    double specFreqFactor = 1.0;   // displayed(f) = f * factor (Custom mode)
    int specFreqUnits = 0;         // 0=Hz, 1=% of Nyquist, 2=bare numbers

    // Alternative scale overlays (spectrogram right-click "Alternative Scale").
    // Any number of reference points per axis; each is a linear rescale
    // anchored at zero: the main-axis value at the clicked point defines the
    // unit, so alt(x) = x * (altValue / refMain).
    struct AltScaleRef
    {
        juce::String name;      // "Alt 1", ... - user-editable in the edit dialog
        bool enabled = true;    // shown in the cursor readout when true
        double refMain = 0.0;   // main-axis value at the reference point (s or Hz)
        double altValue = 0.0;  // alternative-axis value at that point
    };
    std::vector<AltScaleRef> altTimeRefs, altFreqRefs;

    /** Alternative-axis value of a reference for a main-axis value x.
        Returns NaN when the reference is unusable (zero anchor). */
    static double altScaleValue (const AltScaleRef& r, double x)
    {
        return std::abs (r.refMain) > 1e-9 ? x * r.altValue / r.refMain : std::nan ("");
    }

    /** Next free "Alt N" name for a reference list. */
    static juce::String nextAltName (const std::vector<AltScaleRef>& list)
    {
        int maxN = 0;
        for (const auto& r : list)
        {
            const auto t = r.name.trim();
            if (t.startsWithIgnoreCase ("Alt ") && t.length() > 4)
            {
                bool allDigits = true;
                for (int i = 4; i < t.length(); ++i)
                    if (t[i] < L'0' || t[i] > L'9') { allDigits = false; break; }
                if (allDigits) maxN = std::max (maxN, t.substring (4).getIntValue());
            }
        }
        return "Alt " + juce::String (maxN + 1);
    }

    // Armed reference-click flow: which calibration is waiting for a plot
    // click (the spectral view draws the circle once the point is placed).
    /* Which scale calibration a placed reference point belongs to. The two
       Axis* targets come from the Scale form's crosshair buttons: the value
       typed at the placed point becomes that axis entry's scale. */
    enum class RefClickTarget : int
    { None = 0, SpecFreqCustom, AltTime, AltFreq, WaveAmplitude, AxisFrequency };
    RefClickTarget refClickTarget = RefClickTarget::None;
    bool haveRefClickPt = false;
    double refClickT = 0.0, refClickF = 0.0;

    /** Displayed time value for the active time-axis mode:
        Time -> seconds, TimeOffset -> seconds + offset, Sample -> sample count.
        Every mode carries the entry's offset and scale. */
    double timeToDisplay (double t)
    {
        double v = t + timeOffsetSecs;
        switch (timeAxisMode)
        {
            case TimeAxisMode::Sample:
                return v * std::max (1.0, audioEngine().sampleRate());
            default:
                return v * (timeScaleFactor != 0.0 ? timeScaleFactor : 1.0);
        }
    }
    /** Inverse of timeToDisplay(): displayed value -> real seconds. */
    double displayToTime (double v)
    {
        double u = v;
        switch (timeAxisMode)
        {
            case TimeAxisMode::Sample:
                u = v / std::max (1.0, audioEngine().sampleRate());
                break;
            default:
                u = v / (timeScaleFactor != 0.0 ? timeScaleFactor : 1.0);
                break;
        }
        return u - timeOffsetSecs;
    }

    std::vector<int> visibleChannels;   // sorted list of visible channel indices (any subset)

    FileDoc* doc() const;               // active document, or nullptr
    AudioEngine& audioEngine();         // engine of the active doc (or empty fallback)
    MarkerManager& markers();           // markers of the active doc (or empty fallback)
    ::UndoManager& undo();              // edit history of the active doc (or empty fallback)
    const ScaleCalibration& scale() const;   // calibration of the active doc
    bool isDocLoaded (const FileDoc* d) const;

    int numDocs() const { return (int) docs.size(); }

    /** Creates a new empty in-memory document (no file path until Save As)
        with the given channel count and sample rate, and activates it.
        Returns false for invalid parameters. */
    bool newFile (int numChans, double sampleRate);

    /** "Copy to New File": copies the current time selection (or the whole
        file when nothing is selected) into a brand-new document that has
        exactly as many channels as are currently visible. With a spectral
        selection active only the selected (t,f) content is copied. The new
        document becomes active. */
    bool copyToNewFile (std::string* errOut = nullptr);

    /** Merges several files into one document (Shift + drag & drop). Every
        involved file is resampled to the highest sample rate among them.
        Both do heavy I/O and CPU work - call from a worker thread with a
        progress dialog, like loadFileAsync / runResampleAsync. On failure
        errOut receives the reason (empty when cancelled).

        mergeFilesIntoActiveDoc: merges into the currently active document.
          Sequence mode appends each file after the existing content; parallel
          mode adds every file's channels after the existing ones, zero-padded
          to the longest duration. Markers from all sources are preserved
          (shifted in sequence mode) and a region marker named after the
          original file is added around each dropped file in sequence mode.
          A sequence merge that does not change the sample rate is undoable;
          parallel merges and any merge that changes the rate replace the whole
          file and are NOT undoable (like Resample).

        "Open in ... in a new file" merges are done by the caller running
        sc::mergeFilesToResult() on a worker thread and then calling
        createDocFromMerged() on the message thread.

        createDocFromMerged: creates a brand-new document from an already-
        computed merge result (message thread only) and activates it. */
    bool mergeFilesIntoActiveDoc (const std::vector<juce::File>& files, sc::MergeMode mode,
                                  std::function<void(float)> onProgress = nullptr,
                                  const std::atomic<bool>* cancel = nullptr,
                                  std::string* errOut = nullptr);
    // suggestedName (optional) names the new document instead of the usual
    // "Untitled N"; collisions get a numeric suffix appended.
    bool createDocFromMerged (sc::MergeResult& r, std::string* errOut = nullptr,
                              const juce::String& suggestedName = {});

    /** Message-thread only. Applies a stem-separation result to the active document according to outputMode:
        1 = create a new document from it (the historical behaviour),
        2 = replace the current file's content with it (one undoable step; markers inside the extracted range keep their relative position, everything else is dropped with the old timeline),
        3 = append its tracks after the existing channels (one undoable step). */
    bool applyStemSeparationResult (sc::MergeResult& r, int outputMode, double extStartSecs,
                                    const juce::String& suggestedName, std::string* errOut);

    /** Message-thread only. Applies a super-resolution result to the active
        document according to outputMode: 1 = create a new document from it,
        2 = replace the current file's content with it (one undoable step;
        the document takes on the result's sample rate and channel count -
        both SR families emit mono 48 kHz - and markers inside the extracted
        range keep their relative position), 3 = append its track after the
        existing channels over the extracted range (one undoable step; the
        caller must have resampled it to the document's rate first). */
    bool applySuperResolutionResult (sc::MergeResult& r, int outputMode, double extStartSecs,
                                     const juce::String& suggestedName, std::string* errOut);

    bool openFile (const juce::File& file);

    /** For a freshly loaded doc that carried no stored display state, copy
        the current session's Frequency Analysis + axis-calibration settings
        into it so activating the doc does not reset the user's view. */
    void inheritSessionMeta (FileDoc& d);

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

    /** Recorded actions (Photoshop-style record/replay). Persisted in the
        recent-files store under the key "actions_json" so they survive a
        restart. */
    sc::ActionStore actions;

    /** Action-recorder state: while active, every recorded operation that is
        executed appends its step to actions[actionIndex]. */
    struct ActionRecorder { bool active = false; int actionIndex = -1; };
    ActionRecorder actionRec;

    /** Appends one step when recording is active (no-op otherwise). The step
        is persisted immediately. */
    void recordActionStep (const juce::String& type, const juce::var& params = {});

    /** Persists the action store to disk. */
    void saveActions();

    /** AI control state of this application: session journal, jobs, UI
        bridge (created on first use, stopped first by the destructor). */
    sc::control::ControlState& control();

    /** Saves document d to `file` (message thread, blocking): the audio plus
        its managed metadata (embedded chunks, or the .meta.json sidecar in
        external mode, consolidating the legacy sidecars). saveAs = true also
        moves the document's path/name to `file` and adds it to Open Recent.
        Non-WAV targets switch embedded metadata to external, as File > Save
        As does. Returns false with errOut set on failure; *metaWrittenOut
        (optional) tells whether the metadata reached the disk.
        beginSave / completeSave are the two message-thread halves around
        the engine's save, for callers that run the audio write on a worker. */
    bool saveDocument (FileDoc& d, const juce::File& file, bool saveAs,
                       std::string* errOut = nullptr, bool* metaWrittenOut = nullptr);
    uint64_t beginSave (FileDoc& d, const juce::File& file);
    bool completeSave (FileDoc& d, const juce::File& file, bool saveAs, uint64_t metaSnapshot);

    // ---- Sessions (issue #21, experimental) --------------------------------
    // Other WAV files placed on a document's timeline, one after the other or
    // in parallel lanes; see include/Session.h for the model. Every change is
    // one undoable step (audio + layout together).

    /** Adds `files` to the active document's session in track `lane`, the
        first at `offset` samples and each next one right after the previous
        (a file that does not fit there takes the first free place after it).
        An empty track takes the first file's channel count; the files must
        have the track's channel count and the document's sample rate. One
        undoable step. This is what dropping files on the tracks does. */
    bool addSessionFilesAt (const std::vector<juce::File>& files, int lane, int64_t offset,
                            std::string* errOut = nullptr);
    /** addSessionFilesAt for one file: right after the last file of the main
        track (parallel = false) or in a new track at 0 (parallel = true). */
    bool addSessionFile (const juce::File& file, bool parallel, std::string* errOut = nullptr);
    /** Right-click > Add Track: a new empty track at the bottom with
        `channels` channels (it takes the first file dropped on it). */
    bool addSessionTrack (int channels, std::string* errOut = nullptr);
    /** Right-click > Remove Track: removes an empty track (not the main one). */
    bool removeSessionTrack (int lane, std::string* errOut = nullptr);
    /** Resample (issue #48): the channel count the Resample dialog converts
        from. A session's tracks are converted one by one, so it is their
        channel count when they all have the same, -1 when they differ (then
        only the sample rate can change); the file's channel count otherwise. */
    int resampleSourceChannels() const;
    /** The weights AudioEngine::resampleTo applies to the whole document for
        `perTrack` (rows: output channels, columns: the channels of one
        track): in a session, the same matrix for every track (block
        diagonal); `perTrack` itself otherwise. Empty stays empty (no channel
        change). */
    std::vector<std::vector<double>> resampleEngineWeights (const std::vector<std::vector<double>>& perTrack) const;
    /** After AudioEngine::resampleTo on a session document: every place
        scaled to the new rate, the hidden trims resampled and remixed with
        `perTrack`, each track's channel count updated. No-op without a
        session; false (with why) when the result does not match the tracks. */
    bool finishSessionResample (double fromRate, double toRate,
                                const std::vector<std::vector<double>>& perTrack, std::string* errOut = nullptr);
    /** Moves file `index` (1.. ; the main file stays at 0) to `lane` at
        `offset` samples. The lane must have the file's channel count (or be
        empty: it takes the file's) and the place must be free. A track left
        empty stays (Remove Track takes it away). */
    bool moveSessionFile (int index, int lane, int64_t offset, std::string* errOut = nullptr);
    /** Trims file `index` (not the main one): its part on the timeline
        becomes [offset, offset + length), anywhere inside the whole file.
        What falls outside is hidden, not deleted: dragging the edge back out
        shows it again, and saving writes the whole file. One undoable step. */
    bool trimSessionFile (int index, int64_t offset, int64_t length, std::string* errOut = nullptr);
    /** Takes file `index` out of the session; its own file is not touched.
        Its audio leaves the composite (unsaved edits made to it are lost). */
    bool removeSessionFile (int index, std::string* errOut = nullptr);
    /** View > Show File Borders: the files' outlines and names (an app
        setting, on by default and remembered). */
    void setSessionBordersVisible (bool visible);
    /** True when the borders are drawn: the setting is on and the active
        document has more than its own file (a referenced file or an extra
        track) - a plain file never shows one. */
    bool sessionBordersShown () const;
    /** Builds the composite of the session stored in d's metadata
        (d.pendingSession): reads the referenced files, skipping (and listing
        in *warnings) those that are missing, have another sample rate or do
        not fit their lane. Called when a document is opened. */
    bool openSessionOf (FileDoc& d, juce::StringArray* warnings = nullptr);
    /** The same file added several times stays identical (PR #24 review):
        after an edit changed one copy, the others take its content (each
        keeps its window of the file), recorded as a step chained to the edit
        so one undo takes both back. Called after content changes (the main
        window's sync, a save); true when copies were updated. */
    bool syncLinkedSessionFiles ();
    /** Writes back every referenced file whose region of the composite changed
        since it was loaded or last saved (the main file is written by the
        normal save, limited to its own region). Called by beginSave. */
    bool saveSessionFiles (FileDoc& d, std::string* errOut = nullptr);
    /** Adding a file at another sample rate or channel count converts to the
        higher of the two (new channels silent); this asks the user to
        confirm the conversions described (false cancels the add). Not set =
        go ahead (headless, AI control). */
    std::function<bool (const juce::String&)> onConfirmSessionConversion;
    /** The first save that has to write edited referenced files asks how:
        returns 2 = Save as Copies, 1 = Overwrite Originals, 0 = not now
        (asked again next time). The choice is remembered with the session.
        Not set = overwrite (headless, AI control). */
    std::function<int (const juce::StringArray& files)> onAskReferencedSaveMode;
    /** Called (message thread) when saving the referenced files of a session
        failed or was refused (the main file is still saved), or when a
        session action from a menu was refused. */
    std::function<void (const juce::String&)> onSessionSaveProblem;
    /** Warnings of the last session opened (files skipped), for the UI. */
    juce::StringArray lastSessionWarnings;
    /** Called (message thread) when files of a session being opened were skipped. */
    std::function<void (const juce::StringArray&)> onSessionOpenWarnings;
    /** openSessionOf + the warnings, for the two open paths. */
    void openSessionOfLoaded (FileDoc& d)
    {
        lastSessionWarnings.clear ();
        if (d.pendingSession.isObject ())
            openSessionOf (d, &lastSessionWarnings);
        if (!lastSessionWarnings.isEmpty () && onSessionOpenWarnings)
            onSessionOpenWarnings (lastSessionWarnings);
    }
    /** Right-click on the tracks: Add Track, and Remove File / Remove Track
        for the referenced file / empty track under the mouse (clip / lane:
        what is under it, -1 for nothing). */
    void addSessionContextItems (juce::PopupMenu& m, int clipUnder, int laneUnder) const;
    /** Runs one of those items (a refusal goes to onSessionSaveProblem);
        false when `id` is not one of them. */
    bool runSessionContextItem (int id, int clipUnder, int laneUnder);
    /** Name of channel `ch` in the tracks and the status bar: "L" / "R" in a
        two-channel file, its number otherwise; in a session, by its place in
        its own track (two stereo tracks read L, R, L, R). */
    juce::String channelName (int ch) const;
    /** The footer's channel chips (issue #48): in a session, the channels
        that share a name form one chip (two stereo tracks: L = channels 0
        and 2, R = 1 and 3; a click toggles them together); otherwise one
        chip per channel. In channel order of each group's first channel. */
    std::vector<std::vector<int>> channelGroups() const;
    /** True while the Move tool is armed (Selection > Move, V; issue #41):
        the only tool that selects, moves and resizes drawings on the
        spectrogram (a click on one selects it; its handles resize it), and
        in a session a drag on a file moves that file. Choosing a selection
        tool (S, M, L, P) turns it off; those make selections only. */
    bool moveTool = false;
    /** Selection > Move (V): arms or puts down the Move tool. */
    void setMoveTool (bool on);

    // AI feature preferences, persisted in the same store (keys ai_*).
    bool aiAutoDownload = true;          // download missing models on first use
    std::string lastNsModelId = "denseunet";
    std::string lastTranscribeModelId = "whisper-base";
    std::string lastTranscribeLanguage = "auto";
    bool lastTranscribeTranslate = false;
    bool lastTranscribeFast = false;   // true = greedy decode (fast), false = beam search
    int lastTranscribeChannel = 1;     // batch transcription channel: 1-based, <=0 mixes all
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
    // Re-sync track visibility with the engine's channel count after an
    // undo/redo (a step may have grown or shrunk the file, e.g. reverting a
    // mono->stereo split): show all tracks when the count changed.
    void syncVisibleChannelsToEngine (int chBefore);

    /** Copies the session's Frequency Analysis settings and axis reference
        calibrations into a document so they persist with that file
        (unified metadata). Call after any change to those settings. */
    void pushActiveDocMetaTo (FileDoc& d);
    void pushActiveDocMeta();   // same, for the active document

    // ---- Scale form: the document's axis entries -------------------------
    /** Replaces the active document's axis entries: applies them to the
        session (sc::axes::applyEntries), records one undoable step whose
        description the History tab shows, and marks the metadata dirty so the
        entries are written with the file. Without a document the entries are
        applied to the session only (no history, no persistence). */
    bool setAxisEntries (const sc::axes::List& next, const juce::String& description,
                         std::string* errOut = nullptr);
    /** Re-applies the current entries to the session state (after a document
        switch, an undo, or a channel-count change). */
    void applyAxisEntries();
    /** The entries of a document, or the built-in defaults when it has none. */
    static sc::axes::List axesOf (const FileDoc& d);

    /** Horizontal space every track reserves for its vertical axes: the widest
        one wins, the others get empty space (see sc::axes::axisStripWidth). */
    int axisStripWidth() const;
    /** Height of the time-ruler stack: the normal ruler plus one narrower
        strip per extra visible time axis. */
    int rulerStackHeight() const;

    /** Switches the active document's metadata storage mode and migrates the
        data on disk: external -> embedded inserts CUE/ADTL/SCTT/SCMD chunks
        into the WAV; embedded -> external writes .meta.json and removes those
        chunks (audio bytes are preserved either way). Returns false when the
        switch is impossible (e.g. embedding a non-WAV file) or I/O fails. */
    bool setActiveDocMetaMode (sc::MetaMode mode);

    /** Writes the active document's metadata to disk in its CURRENT storage
        mode without touching the audio data: embedded mode surgically patches
        only the managed RIFF chunks; external mode rewrites the .meta.json
        sidecar. Returns false with errOut set when impossible or on I/O
        failure. On success the document is no longer metadata-dirty. */
    bool saveActiveDocMeta (std::string* errOut = nullptr);

    /** True when a document has unsaved audio edits OR its non-audio state
        (markers, saved selections, calibrations, transcription) differs from
        what is on disk. Used by the quit / Close All prompts. */
    bool docIsUnsaved (const FileDoc& d) const;
    std::vector<juce::String> unsavedDocNames() const;

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

    // ---- Move/Scale selection gesture (spectral view right-click menu) ----
    /** Starts a Move or Scale gesture on the active doc's spectral selection.
        No-op when there is no document or no selection. */
    void startSelGesture (bool scale);
    /** First click sets the reference point; a later click applies the
        preview under the cursor and ends the gesture. Returns false when not
        gesturing. `lock` is honoured on the applying click only. */
    bool selGestureClick (double tSecs, double fHz, AxisLock lock);
    /** Live preview: transforms the session-start snapshot about the reference
        point and installs it as the current selection (time extent mirrored,
        playback mask re-installed, UI notified). No-op until the reference is
        set. */
    void updateSelGesture (double tSecs, double fHz, AxisLock lock);
    /** Cancels the gesture, restoring the session-start selection. Returns
        true when a gesture was active. */
    bool cancelSelGesture ();
    bool isSelGesturing () const { return selGesturing; }
    bool haveSelGestureRef () const { return selGesturing && haveSelGestRef; }
    bool selGestureIsScale () const { return selGestScale; }
    double selGestureRefT () const { return selGestRefT; }
    double selGestureRefF () const { return selGestRefF; }

    // Make sure both views' shared time window includes [t0, t1]: no-op when
    // it is already fully visible; otherwise pans with the current span kept,
    // or zooms to the range exactly when it is at least as wide as the view.
    // Routed through onSetTimeWindow so MainWindow can push the result to
    // both views (and keep app.viewStart/viewEnd in sync).
    void ensureTimeRangeVisible (double t0, double t1);

    // ---- Spectrogram drawings (Drawing panel) -----------------------------
    /** Appends a drawing to the active document's list and marks its metadata
        dirty (the list persists with the file's unified metadata on Save). */
    void addDrawing (const DrawingItem& item);
    /** Removes the drawings at the given row indices (bottom-up safe) from the
        active document and marks its metadata dirty. */
    void deleteSelectedDrawings (const std::vector<int>& rows);
    /** Applies an edited drawing's editable fields (text, comment, colour,
        width, text size) to the row; kind and geometry are unchanged. */
    void applyDrawingEdit (int row, const DrawingItem& updated);
    /** Pans/zooms both views so that [t0,t1] x [f0,f1] is visible with a 10%
        margin on every side (clamped to the file's duration and Nyquist). */
    void ensureDrawingVisible (double t0, double f0, double t1, double f1);

    /** Master switch for drawing rendering (session state): when off, all
        committed drawings are hidden from the spectrogram; live creation
        previews stay visible so new items can still be placed. */
    bool showAllDrawings = true;
    void setShowAllDrawings (bool on);

    /** Toggles one item's per-element visibility (the eye icon in the
        Drawing panel list). The state persists with the file metadata. */
    void toggleDrawingHidden (int row);
    /** Shows or hides the drawings `items` (a group's eye). */
    void setDrawingsHidden (const std::vector<int>& items, bool hidden);

    /** Drawings selected in the Drawing panel or by a click in the
        spectrogram (indices into the active document's list), always whole
        groups. The spectrogram draws their box with four handles: drag a
        handle to resize, drag inside to move. */
    std::vector<int> selectedDrawings;
    /** Sets the selection, extended to every member of the groups it
        touches; calls onDrawingSelectionChanged when it changed. */
    void setDrawingSelection (std::vector<int> items);
    std::function<void()> onDrawingSelectionChanged;
    /** Drawing panel > Group: the drawings `items` (two or more) become one
        group (they are selected, moved and resized together). Ungroup frees
        every group the items belong to. */
    void groupDrawings (const std::vector<int>& items);
    void ungroupDrawings (const std::vector<int>& items);
    /** Drawing panel > Rename Group: an empty name goes back to "Group N". */
    void renameDrawingGroup (int groupId, const juce::String& name);
    /** Folds (collapsed) or unfolds group `groupId` in the Drawing panel
        list; groupId 0 = every group. The drawings stay on the spectrogram. */
    void setDrawingGroupCollapsed (int groupId, bool collapsed);

    /** Selection panel > Group (issue #39): the stored selections `items`
        (two or more) become one group, shown as one row with its members
        under it and recalled together. Ungroup frees every group the items
        belong to. Rename: an empty name goes back to "Group N". */
    void groupSavedSelections (const std::vector<int>& items);
    void ungroupSavedSelections (const std::vector<int>& items);
    void renameSelectionGroup (int groupId, const juce::String& name);
    /** Folds or unfolds group `groupId` (0 = every group) in the Selection panel. */
    void setSelectionGroupCollapsed (int groupId, bool collapsed);
    /** Called after the stored selections' groups changed (MainWindow
        refreshes the Selection panel). */
    std::function<void()> onSavedSelectionsChanged;
    /** A move or resize from the spectrogram: drawings `items` become
        `updated` (same order), kept inside the spectrogram (the file's
        duration, 0 to Nyquist). One undoable step. */
    void transformDrawings (const std::vector<int>& items, const std::vector<DrawingItem>& updated,
                            const juce::String& description);
    /** Moves a (time, frequency) point inside the spectrogram: [0, duration]
        x [0, Nyquist]. */
    std::pair<double, double> clampToSpectrogram (std::pair<double, double> tf) const;

    // ---- Tables (Tables panel) --------------------------------------------
    /** Imports CSV text as a new table on the active document (name = file
        name or "Clipboard") and notifies listeners. Returns false with an
        error message when the import fails. */
    bool addTableFromCsv (const juce::String& name, const juce::String& csvText,
                          std::string* errOut = nullptr);
    /** Second half of addTableFromCsv(): adds a table already imported
        (sc::tables::importTable, e.g. on a worker thread) to the document
        with this FileDoc::id - colours no other parameter uses, metadata
        dirty, onTablesChanged when it is the active one. False with a
        message when that document is no longer open. */
    bool addImportedTable (int docId, sc::tables::TableData&& table,
                           std::string* errOut = nullptr);
    /** Tables > Table from Spectral Selection (issue #15): what the Frequency
        Analysis panel's Freq vs Time mode copies (Absolute time, its Track and
        Smoothing settings, the spectral selection as the bin mask, the visible
        channels) over the spectral selection's time extent, imported as a new
        table without going through the panel or the clipboard. Returns false
        with a message when there is no spectral selection. */
    bool addTableFromSpectralSelection (std::string* errOut = nullptr);
    /** The tab-separated text behind addTableFromSpectralSelection(), empty
        (with a message) when there is nothing to analyse. */
    juce::String freqVsTimeCsv (std::string* errOut = nullptr) const;
    // ---- Model panel (issue #40): the active file's sound model --------------
    // Core: include/SoundModel.h; behaviour: docs/model-panel.md.
    /** The active document's model (nullptr when no file is open). */
    sc::model::Model* soundModel();
    /** Call after changing the model: marks the file's metadata dirty and
        fires onSoundModelChanged. */
    void soundModelChanged();
    std::function<void()> onSoundModelChanged;
    /** "Table.Param" for every parameter of the active file's tables. */
    juce::StringArray modelParameterNames() const;
    /** Parameter values of a document's tables at document times: "Table.Param",
        or a parameter name alone (first match); end values held outside.
        Reads the tables in place: use it at once, not after they change. */
    sc::model::ParamFn modelParamFn (const FileDoc& d) const;
    /** Parameter values of tables that are not attached to a document (CSV). */
    static sc::model::ParamFn modelParamFnFor (const std::vector<sc::tables::TableData>& tables);
    sc::model::Audio modelAudio (const FileDoc& d) const;
    sc::model::SelectionFn modelSelectionFn (const FileDoc& d) const;
    /** Analyses component `index` of the active model, or all of them (-1). */
    bool analyseModel (int index, juce::String* err = nullptr);
    /** Renders the model over the time selection into a new channel (one undo step). */
    bool renderModelSelection (juce::String* err = nullptr);
    /** New file: the simulation (channel 1) and the original (channel 2) of
        every range used for the analysis, back to back, with the markers and
        parameters of those ranges. */
    bool compareModel (juce::String* err = nullptr);
    /** New file: the model rendered from the parameters of a CSV file (the
        CSV becomes the new file's table). */
    bool renderModelCsv (const juce::File& csv, juce::String* err = nullptr);
    bool exportModel (const juce::File& target, juce::String* err = nullptr);
    bool importModel (const juce::File& source, juce::String* err = nullptr);
    /** The ranges Compare plays: every component's ranges, merged. */
    std::vector<sc::model::TimeRange> modelCompareRanges() const;

    /** Picking on the spectrogram for the Model panel: Fundamental = the next
        click sets the component's fundamental (snapped to the strongest peak
        nearby) at the parameter value of that time; Harmonics = clicks add or
        remove the harmonic nearest the mouse, with a guide while moving. */
    enum class ModelPick { none, fundamental, harmonics };
    ModelPick modelPick = ModelPick::none;
    juce::String modelPickId;   // component being picked for
    void startModelPick (ModelPick mode, const juce::String& componentId);
    void cancelModelPick();
    /** The harmonic order nearest (t, f) for the picked component, 0 when
        none (no fundamental there). */
    int modelPickOrderAt (double t, double fHz) const;
    /** A click at (t, f) while picking; true when it was used. */
    bool modelPickClick (double t, double fHz);
    std::function<void()> onModelPickChanged;

    /** The active document's tables (empty vector when no file is open). */
    std::vector<sc::tables::TableData>& activeTables();
    int  tablesMode() const;
    /** sc::tables::kModeWaveform...kModePanelOnly; Panel Only draws no
        overlay and no dedicated track (the panel's own plot still shows). */
    void setTablesMode (int mode);
    /** True when the dedicated track should be shown: mode == Dedicated and at
        least one parameter of a visible table is itself visible. */
    bool anyVisibleTableParam() const;

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
    // when nothing happened (no selection / bad frequency). When dtTolSecs /
    // dfTolHz are positive and the click lands within them of an already-picked
    // point, that point is REMOVED instead (with the shapes its pick added).
    bool clonePickClick (double tSecs, double fHz,
                         double dtTolSecs = 0.0, double dfTolHz = 0.0);
    void removeClonePickPoint (int index);

    // Paintbrush size step: one ladder step up/down on the % ladder.
    void stepBrushRadius (bool larger);

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
    /** Like paste, but adds the clipboard to the existing content at the
        paste position instead of inserting it (overlap is summed; a
        clipboard tail past EOF extends the file). */
    bool pasteMix(std::string* errOut = nullptr);
    /** Pastes the clipboard into brand-new channels appended to the file:
        the file gains one channel per clipboard channel, each containing
        that clipboard channel at the selection start (or playhead) and
        silence elsewhere. Undo restores the old channel count. */
    bool pasteToNewChannels(std::string* errOut = nullptr);
    bool del(std::string* errOut = nullptr);
    /** Trim Selection: keep only the selected time range, deleting everything
        before and after it in a single undoable step. */
    bool trimSelection(std::string* errOut = nullptr);
    bool silence(std::string* errOut = nullptr);
    bool fadeIn(std::string* errOut = nullptr);
    bool fadeOut(std::string* errOut = nullptr);
    bool normalize(std::string* errOut = nullptr);
    bool reverse(std::string* errOut = nullptr);
    // Multiply the selection by 10^(gainDb/20) on the visible channels.
    bool changeGain (float gainDb, std::string* errOut = nullptr);

    /** Boost or cut the current selection (spectral selection when present,
        otherwise the time selection) by `gainDb`, shaped as an envelope: the
        gain eases in over the first 25% of the range, holds, and eases back
        to 0 over the last 25% (sc::env::rampedGainDbAt). One undoable step.
        Returns false when there is no selection to shape. */
    bool boostSelectionGain (double gainDb, std::string* errOut = nullptr);

    /** Write a single sample's amplitude (waveform sample-dot drag). One
        undoable step; no-op (returns true) when the value is unchanged. */
    bool setSampleValue (int channel, int64_t sampleIndex, float value,
                         std::string* errOut = nullptr);

    /** Invert: negates every sample in the active spectral selection region,
        or in the time selection if there is one, or over the whole file when
        nothing is selected. One undoable step. */
    bool invertSelection (std::string* errOut = nullptr);

    /** Time Stretch (Audacity's Change Tempo): pitch-preserving tempo change
        of the time selection, or of the whole file when nothing is selected.
        percentChange uses Audacity semantics (+ faster / - slower). The file
        length changes; markers, saved spectral selections and FFT snapshots
        are warped through onTimeRangeStretched. One undoable step restores
        both content and length. A spectral selection is ignored (this is a
        time-domain edit). */
    bool timeStretch (float percentChange, std::string* errOut = nullptr);

    /** Content Aware Time Scale - analysis pass only: runs the STFT gap
        analysis over the active range (time selection or whole file).
        Used by the dialog's live "resulting duration" preview. Returns false
        when the range is too short or too large to analyse in memory. */
    bool catAnalyzeActiveRange (sc::cat::Analysis& out);

    /** Content Aware Time Scale - the effect itself: changes the duration of
        the time selection (or whole file) by removing/duplicating stable
        segments found from STFT frame-to-frame magnitude differences.
        reduceGaps: delete every gap whose change is below `percentile` (0-100)
        of all gaps. Otherwise stretch by `speedFactor` (> 1 = shorter/faster,
        < 1 = longer/slower). One undoable step; timeline state is warped.
        A spectral selection is ignored (this is a time-domain edit). */
    bool contentAwareTimeScale (bool reduceGaps, double percentile, double speedFactor,
                                std::string* errOut = nullptr);

    /** Content Aware Time Scale - "mark only" mode: runs the same Reduce Gaps
        analysis over the active range and adds one region marker per removable
        gap run instead of deleting any audio. Returns the number of markers
        added, or -1 on error (errOut set). The audio is left untouched; like
        manual marker insertion this does not create an undo step. */
    int contentAwareMarkRemovals (double percentile, std::string* errOut = nullptr);

    /** Content Aware Time Scale - "export variation" mode: runs the same STFT
        gap analysis over the active range and writes the per-gap amount of
        spectral change (normalised to [0,1]) as a single parameter in a new
        table ("CAT Variation", numbered when one already exists). One row per
        gap, placed at the gap centre in document time. The audio is left
        untouched; like mark-only this does not create an undo step.
        Returns false on error (errOut set). */
    bool contentAwareExportVariation (std::string* errOut = nullptr);

    // Snapshot of the time-anchored state taken before an edit that inserts /
    // deletes / shortens / stretches timeline data: playhead and selection
    // (Application members) plus the active document's spectral selection,
    // user drawings, SCTT transcription segments and parameter tables. The
    // table rows are warped by the same edits as the audio, so they travel in
    // this snapshot too. commitTimeAnchoredToPatch stores both this snapshot
    // and the post-edit state into p so undo/redo can restore them together
    // with the audio.
    struct TimeAnchoredState
    {
        double playhead = 0.0;
        juce::Range<double> selection { 0, 0 };
        SpectralSelection spectralSel;
        std::vector<DrawingItem> drawings;
        std::vector<ScttSegment> scttSegments;
        std::vector<sc::tables::TableData> tables;
        sc::session::Session session;   // issue #21
    };

    TimeAnchoredState captureTimeAnchored () const;
    void commitTimeAnchoredToPatch (const TimeAnchoredState& before, Patch& p);
    /** Sessions: after a layout change that moved the main file (moved, or
        pushed by a file inserted in front of it), what touches it follows it
        and the last undo step records that too. */
    void followMainFile (FileDoc& d, const sc::session::Clip& mainBefore,
                         const TimeAnchoredState& anchoredBefore,
                         const std::vector<Marker>& markersBefore,
                         const std::vector<SavedSpectralSelection>& savedSelBefore);

    /** Filter EQ: FFT-based zero-phase curve equaliser on the active spectral
        selection region, or on the time selection if there is one, or over
        the whole file when nothing is selected. pts must be sorted by
        ascending frequency; a flat curve is a no-op. One undoable step. */
    bool applyFilterEq (const std::vector<sc::eq::EqPoint>& pts,
                        std::string* errOut = nullptr);

    /** Envelope: per-sample amplitude curve over time on the active spectral
        selection region, or on the time selection if there is one, or over
        the whole file when nothing is selected. The curve's time axis starts
        at 0 seconds at the target range start; pts must be sorted by
        ascending time and a flat curve is a no-op. One undoable step. */
    bool applyEnvelope (const std::vector<sc::env::EnvPoint>& pts,
                        std::string* errOut = nullptr);

    /** Effects > Generate ...: replaces the active spectral selection region,
        or the time selection if there is one; with neither present it inserts
        durationSecs of generated signal at the playhead. Each call is a
        single undoable step. */
    bool generateSilence (double durationSecs, std::string* errOut = nullptr);
    bool generateNoise (sc::NoiseType type, float intensity,
                        double durationSecs, std::string* errOut = nullptr);
    bool generateTone (const std::vector<sc::ToneSpec>& tones, sc::WaveShape shape,
                       double durationSecs, std::string* errOut = nullptr);

    /** Executes the configured "Split Spectral Selection" (F12): for every
        input -> destination pair in settings.splitConfig, the selected band
        content is extracted from the input channel with a trapezoidal
        frequency blend (25% ramps top and bottom, 50% intact middle) and the
        estimated per-frame noise floor subtracted; it is then mixed into the
        destination channel - an existing channel mixes in place, a number
        above the current count creates a new channel on first use. The input
        channels keep everything outside the selection and get synthetic noise
        at the estimated floor level inside it, so that original + extracted ~=
        the pre-edit signal with no sharp spectral edge. Pairs are applied in
        configured order; one undoable step (new channels and rewritten
        originals together). Returns false with *errOut set when there is no
        selection or no usable configuration. */
    bool splitSpectralSelectionConfigured (std::string* errOut = nullptr);
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    // BLOCKING — call from a worker thread. Applies AI noise suppression
    // (denseunet or DeepFilterNet2/3, selected by modelId) to the current
    // selection on the visible channels. attenLimDb / df3PostFilter only apply
    // to the DeepFilterNet models. Returns false and fills errOut on failure/
    // cancellation; nothing is modified in that case.
    // autoDownload = false: never download; fail with a manual-install hint
    // when the model is not already present.
    /** copyNoise: when true, one new channel is appended per processed
        (visible) channel holding only the estimated noise — original minus
        enhanced — over the selection range; the enhanced signal stays in the
        existing channels. One undoable step covers both changes. */
    bool noiseSuppress (const std::string& modelId, const std::string& device,
                        float attenLimDb = 0.f, bool df3PostFilter = false,
                        std::function<bool(float)> onProgress = nullptr,
                        std::string* errOut = nullptr,
                        bool autoDownload = true, bool copyNoise = false);

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

    // Strict integer text for a batch-transcription channel option: an
    // optional sign followed by at most 9 digits. "0" and negative values are
    // valid — they mean "mix all channels".
    static bool parseChannelText (const juce::String& text, int& out);

    // Picks the channels a batch transcription should analyse for one file:
    // channel <= 0 mixes every channel together; channel >= 1 selects that
    // single (1-based) channel. Returns false with errOut set when the number
    // is out of range for the file.
    static bool pickBatchChannels (int numFileChans, int channel,
                                   std::vector<int>& outChans, std::string& errOut);

    // BLOCKING — call from a worker thread. Transcribes every .wav file in
    // `folder` (recursively) WITHOUT opening it in the editor: each file is
    // loaded headlessly, reduced to mono (see pickBatchChannels for which
    // channels are mixed), transcribed with Whisper, and its SCTT chunk
    // replaced in place on disk (all other RIFF chunks preserved).
    // skipExisting = true leaves files that already carry an SCTT chunk alone.
    // channel: 1-based channel number to analyse; <= 0 mixes all channels.
    // onProgress reports overall 0..1; onStatus is called before each file;
    // cancelOut, when set, stops the run after the current file. Returns false
    // (and fills errOut) only for setup failures such as "no WAV files found";
    // per-file problems are tallied in summaryOut instead.
    bool batchTranscribeFolder (const juce::File& folder,
                                const std::string& modelId,
                                const std::string& language, bool translate,
                                bool fast, bool skipExisting, int channel = 1,
                                std::function<bool(float)> onProgress = nullptr,
                                std::function<void(const juce::String&)> onStatus = nullptr,
                                std::atomic<bool>* cancelOut = nullptr,
                                BatchTranscribeSummary* summaryOut = nullptr,
                                std::string* errOut = nullptr,
                                bool autoDownload = true,
                                sc::MetaSaveTarget metaTarget = sc::MetaSaveTarget::Auto);
#endif

    // BLOCKING — call from a worker thread. Runs the Python script `script`
    // (whose on-disk text is first replaced by `scriptText`) against the
    // current selection, or the whole file when nothing is selected.
    // pythonCmd is the interpreter command found by sc::py::findPython
    // (e.g. "python" or "py -3"). The script receives a float32 WAV of the
    // range plus a metadata JSON and writes back an output WAV with the same
    // rate/channels/length (the result replaces that range in place and is
    // undoable) and/or a result.json (markers, text, measures, tables: see
    // sc::py::resultContractText; out.wav is optional when result.json
    // exists). onProgress reports 0..1 (excluding the final mipmap rebuild)
    // and returning false cancels (killing the child's process tree). A
    // sc::py::ScopedScriptRunCapture on the calling thread imposes its
    // RunOptions (timeout) and receives the run report (SHA-256, exit code,
    // outputs, result.json).
    bool runPythonScript (const juce::File& script, const juce::String& scriptText,
                          const std::string& pythonCmd,
                          std::function<bool(float)> onProgress = nullptr,
                          std::string* errOut = nullptr);

    void undoStep(); void redoStep();
    // Batched variants used by the History tab: apply up to n patches in one
    // go with a single mipmap rebuild at the end (fast for long jumps).
    void undoSteps (int n); void redoSteps (int n);

    // Lazy audio device initialisation. The expensive WASAPI enumeration/open
    // is deferred out of the startup path: startAudioInitAsync() runs it on a
    // worker thread (kicked off right after the window appears, and/or by the
    // first play request); ensureAudioReady() blocks until it has completed.
    bool audioDeviceReady() const;
    void startAudioInitAsync();
    /** True when the real output device must stay closed: the environment
        variable SC_NO_AUDIO_DEVICE is set (to anything but "0"), or the build
        defines SC_NO_AUDIO_DEVICE. The init then completes at once without a
        device (audioDeviceReady() turns true; play() is silent). Tests that
        need a real device skip when this is true. */
    static bool audioOutputDisabled();
    bool ensureAudioReady (int timeoutMs = -1);

    // Transport > Playback Device (issue #64). The names are the output
    // devices of the current device type (Windows Audio, CoreAudio, ALSA...),
    // empty while the output is not ready or disabled (SC_NO_AUDIO_DEVICE).
    juce::StringArray playbackDeviceNames() const;
    /** Name of the output device now open ("" when none). */
    juce::String currentPlaybackDeviceName() const;
    /** Chooses the output: a device name, or "" for the system default.
        Saves the choice, waits for the audio init, then opens the device (the
        system default when that name is not connected). Returns false when
        the device could not be opened; the choice is saved either way. */
    bool setPlaybackDevice (const juce::String& name);
    /** The device a name stands for: `preferred` when it is in `available`,
        otherwise "" (the system default). Pure; used by the init and tests. */
    static juce::String resolvePlaybackDevice (const juce::String& preferred,
                                               const juce::StringArray& available);

    void play(); void stop(); void pause();
    // Toggle looping. When enabling, the loop range is taken from the current
    // selection (or the whole file if there is no selection).
    void toggleLoop();
    void setPlayhead (double t);

    bool isChannelVisible (int ch) const;
    void toggleChannelVisible (int ch);

    /** Playback mute from a click on a channel number in the tracks: a plain
        click toggles channel ch; `solo` (Ctrl+click, Cmd+click on macOS)
        mutes every other channel and unmutes ch - or unmutes them all when
        ch was already the only channel playing. Hidden channels count like
        the others (they still play). Notifies onStateChange. */
    void clickChannelMute (int ch, bool solo);

    // Add a marker: region spanning the current selection if there is one,
    // otherwise a point marker at the cursor.
    int addMarkerAtCursor();

    // Set by MainWindow: called when panels change shared state (settings, markers,
    // channels) so the main views can be re-synced.
    std::function<void()> onStateChange;

    // Set by MainWindow: sets both views' time window to [t0, t1]. Used by
    // ensureTimeRangeVisible() when a panel asks for something to be shown.
    std::function<void(double, double)> onSetTimeWindow;

    // Set by MainWindow: sets the spectrogram's local frequency axis to
    // [lo, hi] (used by ensureDrawingVisible()).
    std::function<void(double, double)> onSetSpectralFreqRange;

    // Set by MainWindow: returns the spectrogram's current local frequency
    // axis as {lo, hi} ({0, 0} when there is no view). Lets
    // ensureDrawingVisible() skip an axis change that would be a no-op.
    std::function<std::pair<double, double>()> getSpectralFreqRange;

    // Set by MainWindow: arms a new-drawing mode of the given kind on the
    // spectrogram (Drawing panel context menu / t,a,r,d shortcuts).
    std::function<void(DrawingItem::Kind)> onArmNewDrawing;

    // Set by MainWindow: refreshes the Drawing panel after its list changed.
    std::function<void()> onDrawingsChanged;

    // Set by MainWindow: refreshes the Tables panel and re-lays out the main
    // area (the dedicated track appears/disappears with mode + visibility).
    std::function<void()> onTablesChanged;

    // Set by MainWindow: arms/disarms the click capture used by the Tables
    // panel's 1/2-point alignment. targetView 0 = waveform, 1 = spectrogram,
    // negative disarms both views.
    std::function<void(int)> onArmTableAlignClick;

    // Set by MainWindow: called whenever setPlayhead() moves the cursor, so both
    // views always show the same playhead position.
    std::function<void(double)> onPlayheadChanged;

    // Set by MainWindow: fired before a document is destroyed, so panels can
    // drop state that belongs to it (e.g. snapshots stored from that file).
    std::function<void(const FileDoc&)> onFileClosed;

    // Set by MainWindow: called right after [a, b] seconds were deleted from d
    // (the audio patch is already recorded). Lets shared UI state - the FFT
    // snapshot slots - record its before/after into the patch and adjust.
    // preserveExistingBefore: when one edit deletes two ranges (trim), the
    // second call must keep the original pre-deletion state recorded by the
    // first call instead of overwriting it.
    std::function<void(FileDoc*, double, double, Patch&, bool)> onTimeRangeDeleted;

    // Set by MainWindow: called after timeStretch() stretched [a, bOld] of d
    // to [a, bNew], so FFT snapshot slots can be warped and re-recorded.
    std::function<void(FileDoc*, double, double, double, Patch&)> onTimeRangeStretched;

    // Set by MainWindow: called after len seconds were inserted at time a in d
    // (paste, generate-insert, length-changing Python scripts), so FFT
    // snapshot slots can be shifted and re-recorded.
    std::function<void(FileDoc*, double, double, Patch&)> onTimeRangeInserted;

    // Set by MainWindow: called after an undo (!isRedo) or redo (isRedo)
    // applied a patch with side state to d, so shared UI state is restored too.
    std::function<void(FileDoc*, const Patch&, bool)> onPatchSideStateRestored;

    // Set by MainWindow: fired after open/switch/close so the UI can adapt
    // (reset views to the new file, push its state into every panel).
    std::function<void()> onActiveDocChanged;
    // Set by MainWindow: fired by activate() while the views still show the
    // document being left (before any state switches), so its Editor tab can
    // keep a picture of it.
    std::function<void()> onBeforeActiveDocChange;

private:
    std::unique_ptr<sc::control::ControlState> control_;
    // Set first thing in the destructor: workers waiting for a message-thread
    // commit (AI ops, Python) give up instead of deadlocking while the
    // destructor joins the AI-control jobs.
    std::atomic<bool> destroying_ { false };

    // Lazy audio init state. audioInitThread is joined in the destructor so a
    // pending worker can never outlive the AudioDeviceManager it initialises.
    std::atomic<bool> audioInitStarted { false };
    std::atomic<bool> audioDeviceIsReady { false };
    mutable std::mutex audioInitLock;
    std::condition_variable audioInitCv;
    std::thread audioInitThread;
    // Opens the playback device `preferred` (issue #64), or the system default
    // when it is "" or not connected; returns JUCE's error text ("" = done).
    // Runs on the audio init worker, then on the message thread (play()'s
    // self-heal, setPlaybackDevice).
    juce::String openPlaybackDevice (const juce::String& preferred);

    // Recording session state (see startRecording/stopRecording).
    FileDoc* recDoc_ = nullptr;      // document being recorded into (validated on every tick)
    std::vector<float> recTmpBuf_;   // scratch for pulled frames

    void loadSettings();   // restore persisted session settings from recentStore

    // Restores the non-audio state recorded in a patch (see Patch::hasSideState).
    void restorePatchSideState (FileDoc* d, const Patch& p, bool isRedo);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Application)
};
