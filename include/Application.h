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
        thread once the load finished (ok = success, doc activated). */
    void loadFileAsync (const juce::File& file,
                        std::function<void(float)> onProgress,
                        std::function<void(bool)> onDone);

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
    void removeRecentFile (const juce::File& file);// drop one entry and persist
    void clearRecentFiles();                        // empty the list and persist it

    void closeFile();                  // closes the ACTIVE document
    void closeDoc (int index);         // closes one specific loaded document
    void activate (int index);         // make a loaded document active
    void cycleActiveDoc (bool forward);// Ctrl+Tab / Ctrl+Shift+Tab
    // Copy the current UI state (playhead/selection/channels) into the active doc.
    void syncActiveDoc();

    void selectAll();
    // snapEdges = true snaps both selection edges to the nearest zero crossing
    // of the first visible channel when settings.snapToZeroCrossings is ON.
    // Used by the views' drag-to-select so cuts land on silent points.
    void setSelection (juce::Range<double> s, bool snapEdges = false);
    void cut(); void copy(); void paste(); void del();
    void silence(); void fadeIn(); void fadeOut(); void normalize(); void reverse();
    // Multiply the selection by 10^(gainDb/20) on the visible channels.
    void changeGain (float gainDb);
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    // BLOCKING — call from a worker thread. Applies denseunet noise suppression
    // to the current selection on the visible channels. Returns false and fills
    // errOut on failure/cancellation; nothing is modified in that case.
    bool noiseSuppress (const std::string& modelId, const std::string& device,
                        std::function<bool(float)> onProgress = nullptr,
                        std::string* errOut = nullptr);
#endif
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

    // Set by MainWindow: fired after open/switch/close so the UI can adapt
    // (reset views to the new file, push its state into every panel).
    std::function<void()> onActiveDocChanged;

private:
    void loadSettings();   // restore persisted session settings from recentStore

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Application)
};
