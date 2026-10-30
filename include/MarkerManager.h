#pragma once
// SpectroCraft - Marker manager: point + region markers, sidecar persistence
#include <JuceHeader.h>
#include <vector>
#include <memory>
#include <functional>

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

    // Adjust the markers after [a, b] seconds have been removed from the file:
    // point markers inside the deleted range are dropped; region markers that
    // straddle an edge of it are trimmed to that edge; fully contained regions
    // are dropped; everything at/after b shifts left by the deleted length.
    // Returns true when any marker changed.
    bool adjustForDeletion (double a, double b);

    // Adjust the markers after [a, bOld] seconds has been stretched to
    // [a, bNew]: every time is warped through W(t) = t for t < a,
    // a + (t-a)*(bNew-a)/(bOld-a) inside the stretched range, and
    // t + (bNew-bOld) after it. No markers are dropped.
    // Returns true when any marker changed.
    bool adjustForStretch (double a, double bOld, double bNew);

    // Adjust the markers after lenSecs seconds has been inserted at position t:
    // point markers at/after t shift right by lenSecs; region markers entirely
    // before t are unchanged, entirely at/after it shift right, and regions
    // straddling t keep their start while their end follows its content to
    // e + lenSecs. No markers are dropped.
    // Returns true when any marker changed.
    bool adjustForInsertion (double t, double lenSecs);

    // Replace the whole list with `m` (undo/redo restoration of side state).
    void restore (const std::vector<Marker>& m);

    /** Fired after every mutation, including restore() (FileDoc clears its
        dirty flag at the end of load(), so loading a file does not count as
        an unsaved change). FileDoc wires this to its metaDirty state. */
    std::function<void()> onChange;

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

    // CSV export/import: "Title,Begin,End,Comment" with a header row; times in
    // seconds (3 decimals); fields containing commas/quotes are quoted per
    // RFC 4180. Point markers have End = "-".
    bool saveCsv (const juce::File& file) const;

    // Appends the parsed markers to the current list, clamping them to
    // durationSec. Returns how many were added (0 when nothing was readable).
    int loadCsv (const juce::File& file, double durationSec);

private:
    void sort();

    std::vector<Marker> list;
    int nextId = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MarkerManager)
};
