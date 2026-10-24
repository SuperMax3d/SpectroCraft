#pragma once
// SpectroCraft - named, storable spectral selections (Selection panel).
//
// A SavedSpectralSelection is a full copy of a SpectralSelection under a name.
// Each FileDoc keeps its own list, persisted automatically with the file's
// unified metadata (embedded SCMD chunk or .meta.json sidecar) on Save.
// The legacy "<audiofile>.selections.json" sidecar format is still read on
// open and by drag & drop.

#include <JuceHeader.h>
#include "SpectralSelection.h"
#include <vector>

struct SavedSpectralSelection
{
    juce::String name;
    SpectralSelection data;   // deep copy of the shapes at save time
    juce::String comment;     // free-form note, editable in the Selection panel
                              // (kept last so {name, data} aggregates still work)
};

// Sidecar path for an audio file: "<full path>.selections.json".
juce::File savedSelectionsSidecarPath (const juce::File& audioFile);

// Serialises a list to JSON text. Always succeeds (pure string building).
juce::String serialiseSavedSelections (const std::vector<SavedSpectralSelection>& list);

// Parses the JSON produced by serialiseSavedSelections(). Returns false when
// the text is not valid for this format; on success `out` is replaced with
// the parsed list (empty list included).
bool deserialiseSavedSelections (const juce::String& json,
                                 std::vector<SavedSpectralSelection>& out);
