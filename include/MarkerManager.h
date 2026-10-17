#pragma once
// SpectroCraft - Marker manager: point + region markers, sidecar persistence
#include <JuceHeader.h>
#include <vector>
#include <memory>

struct Marker
{
    int id = 0;
    juce::String name;      // title (saved as RIFF "labl" in WAV files)
    juce::String comment;   // free text (saved as RIFF "ltxt" in WAV files)
    double start = 0.0;
    double end = -1.0;   // < 0 => point marker
    juce::Colour colour = juce::Colours::yellow;
};

class MarkerManager
{
public:
    MarkerManager();
    ~MarkerManager();

    int addPointMarker (double time, const juce::String& name);
    int addRegionMarker (double start, double end, const juce::String& name);
    bool removeMarker (int id);
    bool renameMarker (int id, const juce::String& newName);
    bool setComment (int id, const juce::String& newComment);
    bool moveMarker (int id, double newTime);
    bool setRegion (int id, double start, double end);
    bool setPoint (int id); // convert a region marker into a point marker at its start

    const std::vector<Marker>& markers() const { return list; }
    const Marker* findNear (double time, double tolSecs) const;
    void clear() { list.clear(); nextId = 1; }
    size_t count() const { return list.size(); }

    // Sidecar persistence: "<audiofile>.markers.json"
    bool saveSidecar (const juce::File& audioFile) const;
    bool loadSidecar (const juce::File& audioFile);

    // Reads markers embedded in a WAV file (RIFF "cue" + LIST/"adtl" chunks,
    // classic DAW marker layout; standard 16-byte cue entries are also accepted).
    // The file is streamed chunk-by-chunk, the audio data is never read.
    // Returns true if at least one marker was recovered.
    bool loadFromWavFile (const juce::File& wavFile, double sampleRate);

private:
    void sort();

    std::vector<Marker> list;
    int nextId = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MarkerManager)
};
