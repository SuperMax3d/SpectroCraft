#pragma once
#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <vector>

/*  Sessions (issue #21, experimental): a main WAV document that references
    other WAV files placed on its timeline, one after the other in a lane or
    in parallel lanes.

    While a session is open the document's AudioEngine holds the COMPOSITE:
    every lane is a group of consecutive channels (lane 0 = the main file's
    channels first), and each file sits in its lane's channels from its offset
    on, silence elsewhere. All editing, playback and drawing therefore work on
    the composite as on any document, and files touching each other in a lane
    form one continuous signal (no spectrogram seam). Saving writes every file
    back from its own region of the composite.

    Everything here is pure data work on that model; Application wires it to
    the document, undo and saving. */
namespace sc::session
{

/** One file of the session. clips[0] of a Session is always the main file:
    lane 0, offset 0, never moved. Offsets and lengths are in samples at the
    session's (= the main file's) sample rate. */
struct Clip
{
    juce::File file;
    int lane = 0;
    int64_t offset = 0;
    int64_t length = 0;
    /** Content fingerprint of the clip's region when it was loaded or last
        saved: a referenced file is only rewritten when its region changed. */
    uint64_t savedHash = 0;

    int64_t end() const noexcept { return offset + length; }
    bool operator== (const Clip& o) const
    {
        return file == o.file && lane == o.lane && offset == o.offset && length == o.length
            && savedHash == o.savedHash;
    }
};

struct Session
{
    std::vector<int> laneChannels;   // channel count of each lane; [0] = the main file's
    std::vector<Clip> clips;         // [0] = the main file

    /** True when the document references at least one other file or has a
        track besides the main file's (an empty track added with Add Track). */
    bool active() const noexcept { return clips.size() > 1 || laneChannels.size() > 1; }
    /** True when lane `lane` holds no file (a track added and still empty). */
    bool laneEmpty (int lane) const;
    int numLanes() const noexcept { return (int) laneChannels.size(); }
    int totalChannels() const;
    /** First composite channel of a lane (-1 for an unknown lane). */
    int laneFirstChannel (int lane) const;
    /** Lane holding a composite channel (-1 when none). */
    int laneOfChannel (int channel) const;
    /** End of the last file, in samples. */
    int64_t extent() const;
    /** Index of the file covering `sample` in the lane of `channel`, or -1. */
    int clipAt (int channel, int64_t sample) const;

    bool operator== (const Session& o) const
    {
        return laneChannels == o.laneChannels && clips == o.clips;
    }
    bool operator!= (const Session& o) const { return !(*this == o); }
};

/** True when clip `index` could sit at [offset, offset + its length) in
    `lane` without overlapping another file of that lane (files of one lane
    must not overlap: the composite could not tell them apart when saving). */
bool fits (const Session& s, int index, int lane, int64_t offset);

/** Shift-snapping while a file is dragged: the proposed offset moves so that
    the file's start or end lands on the nearest target within `tolerance`
    samples. Targets: 0, every other file's start and end (any lane) and the
    extra times given (e.g. the playhead). Returns the proposed offset when
    nothing is close enough. */
int64_t snapOffset (const Session& s, int index, int64_t proposed, int64_t tolerance,
                    const std::vector<int64_t>& extraTargets = {});

/** Nearest offset >= 0 at or after `proposed` where clip `index` fits in
    `lane`, or -1 when none (cannot happen: the end of the lane is free). */
int64_t firstFreeOffset (const Session& s, int index, int lane, int64_t proposed);

/** Time warps after a length-changing edit of the composite (all lanes at
    once): [a, b) samples removed, `len` samples inserted at `a`, or
    [a, bOld) stretched to [a, bNew). A file fully inside a removed range is
    dropped from the session (its own file is left untouched). The main file
    always stays. */
void warpForDeletion (Session& s, int64_t a, int64_t b);
void warpForInsertion (Session& s, int64_t a, int64_t len);
void warpForStretch (Session& s, int64_t a, int64_t bOld, int64_t bNew);

/** Places every file's data in a zeroed composite: one row per composite
    channel, extent() samples long. data[i] is clip i's audio (one row per
    channel of its lane). */
std::vector<std::vector<float>> buildComposite (const Session& s,
                                                const std::vector<std::vector<std::vector<float>>>& data);

/** FNV-1a fingerprint of a clip's region read through `read(ch, start, n, dst)`
    (composite channels, samples). */
uint64_t regionHash (const Session& s, int index,
                     const std::function<bool (int, int64_t, int64_t, float*)>& read);

/** The "session" metadata entry: every referenced file (not the main one)
    with its lane, offset and length, plus the lane layout (empty tracks
    included). Paths are stored relative to `baseDir` (the main file's folder)
    when possible, with the absolute path as a fallback. */
juce::var toVar (const Session& s, const juce::File& baseDir);
/** Reads it back; clips[0] (the main file) is filled from `mainFile`,
    `mainChannels` and `mainLength`. Returns an inactive session when `v` is
    not a session entry. */
Session fromVar (const juce::var& v, const juce::File& baseDir, const juce::File& mainFile,
                 int mainChannels, int64_t mainLength);

} // namespace sc::session
