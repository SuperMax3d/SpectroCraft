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
    /** Saves to the document's file (a new name in the recordings folder for
        a recording that was never saved). */
    bool save (juce::String& error);
    bool saveAs (const juce::File& f, juce::String& error);

    // ---- selection (seconds); an empty range = no selection
    juce::Range<double> selection() const noexcept { return sel; }
    void setSelection (juce::Range<double> r);
    double playhead() const noexcept { return head; }
    void setPlayhead (double t);

    // ---- edits (undoable)
    bool deleteSelection (juce::String& error);
    bool gainSelection (float db, juce::String& error);
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MobileDocument)
};
} // namespace scm
