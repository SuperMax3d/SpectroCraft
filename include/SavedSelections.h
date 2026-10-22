#pragma once
// SpectroCraft - named, storable spectral selections (Selection panel).
//
// A SavedSpectralSelection is a full copy of a SpectralSelection under a name.
// Each FileDoc keeps its own list; when "Save to disk" is on the list is kept
// in sync with a sidecar file "<audiofile>.selections.json".

#include <JuceHeader.h>
#include "SpectralSelection.h"
#include <vector>

struct SavedSpectralSelection
{
    juce::String name;
    SpectralSelection data;   // deep copy of the shapes at save time
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
