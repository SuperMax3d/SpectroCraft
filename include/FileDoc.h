#pragma once
// SpectroCraft - per-file document state
#include <JuceHeader.h>
#include "AudioEngine.h"
#include "MarkerManager.h"
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

    /** Load the file into this document: audio + markers (sidecar JSON first,
        embedded RIFF cue/adtl as fallback for .wav) + initial UI state. */
    bool load (const juce::File& f);

private:
    JUCE_DECLARE_NON_COPYABLE (FileDoc)
};
