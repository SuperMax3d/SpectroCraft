// SpectroCraft Mobile - the open document.
//
// One file at a time, held in the desktop's FileDoc (audio, undo history,
// metadata bundle), so loading and saving read and write exactly what the
// desktop does: calibration ("scale" / "axes"), video tracks, stored plots.
#pragma once
#include <JuceHeader.h>
#include "FileDoc.h"
#include "Settings.h"
#include "SpectralEngine.h"
#include "StoredPlots.h"
#include <memory>

namespace scm
{
class MobileDocument : public juce::ChangeBroadcaster
{
public:
    MobileDocument();
    ~MobileDocument() override;

    /** Where recordings are written (Documents/SpectroCraft on iOS and Android:
        visible in the Files app / the device's file manager). */
    static juce::File recordingsFolder();
    /** "Recording 2026-11-24 18-30-05.wav" in the recordings folder. */
    static juce::File newRecordingFile();

    FileDoc* doc() noexcept { return current.get(); }
    const FileDoc* doc() const noexcept { return current.get(); }
    bool hasAudio() const noexcept;
    double duration() const noexcept;
    double sampleRate() const noexcept;
    int numChannels() const noexcept;

    SpectralEngine& spectral() noexcept { return spectralEngine; }
    Settings& settings() noexcept { return spectroSettings; }
    StoredPlots& plots() noexcept { return storedPlots; }

    juce::File file() const;
    juce::String displayName() const;
    bool hasUnsavedChanges() const;

    bool open (const juce::File& f, juce::String& error);
    /** An empty in-memory document to record into. */
    bool startNew (int channels, double rate);
    void close();
    /** Saves to the document's file (for a take never saved: the name it was
        given when it started, else a new name in the recordings folder). */
    bool save (juce::String& error);
    bool saveAs (const juce::File& f, juce::String& error);

    // ---- a take not saved yet (issue #79: nothing is written to the
    // recordings folder until Save). Its file is empty; Save puts it at its
    // planned file. A recovery copy kept outside the recordings folder brings
    // it back at the next launch if the app is closed first.
    void setPlannedFile (const juce::File& f) { planned = f; }
    juce::File plannedFile() const { return planned; }
    bool isUnsavedTake() const noexcept { return current != nullptr && current->file == juce::File() && planned != juce::File(); }
    /** The take now has changes to save (a new take, or a recovered one). */
    void markUnsaved();
    /** Copies the take to the recovery folder, without changing its state. */
    void writeRecovery();
    /** The take was saved or discarded: no recovery any more. */
    void clearRecovery();
    /** Discard of an unsaved take: its recovery copy and the movie filmed
        with it go too. */
    void discardUnsavedTake();
    /** At launch: reopens a take left unsaved. True when there was one. */
    bool recoverTake();
    /** The app's own folder for the recovery copy (not visible in the Files
        list, the iOS Files app or the Android file manager). */
    static juce::File recoveryFolder();

    // ---- selection (seconds); an empty range = no selection
    juce::Range<double> selection() const noexcept { return sel; }
    void setSelection (juce::Range<double> r);
    double playhead() const noexcept { return head; }
    /** byHand: a tap, the lens, Home / End (not Stop's return to the start). */
    void setPlayhead (double t, bool byHand = true);
    /** The playhead was placed by hand since the last selection (issue #79:
        the analysis then takes one FFT window around it). */
    bool playheadPlacedByHand() const noexcept { return headByHand; }
    /** The playhead follows playback: no seek and no change message (the
        views only repaint their cursor). */
    void followPlayback (double t) noexcept { head = t; }

    // ---- edits (undoable)
    bool deleteSelection (juce::String& error);
    bool gainSelection (float db, juce::String& error);
    /** Keeps only the selection (one undoable step). */
    bool cropToSelection (juce::String& error);
    /** Writes the selection to a new WAV in the recordings folder ("<name>
        crop.wav", with this file's calibration) and returns it ({} on error).
        The open document does not change. */
    juce::File cropToNewFile (juce::String& error);
    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();

    // ---- calibration: the desktop's per-file scale (and its Amplitude axis
    //      entry when the file has Scale-form axes, which take precedence)
    ScaleCalibration calibration() const;
    void setCalibration (const ScaleCalibration& c);

    // ---- video linked to the recording
    void linkVideo (const juce::File& movie, double offsetSecs);
    juce::File linkedVideo() const;
    /** Document time of the linked video's first frame (0 without one). */
    double linkedVideoOffset() const;

    /** Call after the stored plots change: they go into the metadata. */
    void plotsChanged();

    /** Broadcast after any change of content, selection, file or metadata. */
    void changed() { sendChangeMessage(); }

    /** Called (message thread) right before the document object is replaced or
        closed - background renderers and the audio callback must let go of it -
        and right after the new one is in place. */
    std::function<void()> onAboutToReplace, onReplaced;

private:
    void adoptDocument (std::unique_ptr<FileDoc> d);
    juce::Range<int64_t> selectionSamples() const;

    std::unique_ptr<FileDoc> current;
    SpectralEngine spectralEngine;
    Settings spectroSettings;
    StoredPlots storedPlots;
    juce::Range<double> sel;
    double head = 0.0;
    bool headByHand = false;
    juce::File planned;   // an unsaved take's file

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MobileDocument)
};
} // namespace scm
