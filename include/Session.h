#pragma once
#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <memory>
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

/** One file of the session. clips[0] of a Session is always the main file,
    on lane 0; it can move along the timeline (its offset is saved as the
    session's "mainOffset"). Offsets and lengths are in samples at the
    session's (= the main file's) sample rate. */
/** Samples of a file hidden by a trim, one row per channel. Never changed
    once made, so the copies of a session (undo steps) share them. */
using HiddenAudio = std::shared_ptr<const std::vector<std::vector<float>>>;

struct Clip
{
    juce::File file;
    int lane = 0;
    int64_t offset = 0;
    int64_t length = 0;     // the part on the timeline (the composite holds it)
    /** Content fingerprint of the whole file - hidden start, the region on
        the timeline, hidden end - when it was loaded or last saved: a
        referenced file is only rewritten when that changed (a trim alone
        does not change it). */
    uint64_t savedHash = 0;
    /** Trims (PR #24 review): the file's first headLength / last tailLength
        samples are hidden, not deleted - kept here and written back on save.
        The lengths are what the session entry stores; head / tail hold the
        samples once the file is loaded. */
    int64_t headLength = 0, tailLength = 0;
    HiddenAudio head, tail;
    /** The same fingerprint as savedHash, taken when the copies of this
        file (the same file added several times) were last made identical:
        after an edit, the copy whose content no longer matches it is the one
        that was edited, and the others follow it. */
    uint64_t linkHash = 0;

    int64_t end() const noexcept { return offset + length; }
    /** Where the whole file would start / end on the timeline. */
    int64_t fileStart() const noexcept { return offset - headLength; }
    int64_t fileEnd() const noexcept { return end() + tailLength; }
    bool operator== (const Clip& o) const
    {
        return file == o.file && lane == o.lane && offset == o.offset && length == o.length
            && savedHash == o.savedHash && headLength == o.headLength && tailLength == o.tailLength
            && head == o.head && tail == o.tail && linkHash == o.linkHash;
    }
};

struct Session
{
    std::vector<int> laneChannels;   // channel count of each lane; [0] = the main file's
    std::vector<Clip> clips;         // [0] = the main file

    /** True when the document references at least one other file, has a
        track besides the main file's (an empty track added with Add Track),
        or its main file was moved away from 0. */
    bool active() const noexcept
    {
        return clips.size() > 1 || laneChannels.size() > 1 || (! clips.empty() && clips[0].offset != 0);
    }
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

/** The layout after clip `index` is dropped at `offset` in `lane` (PR #24
    review): where it is free, it goes there. On another file, it goes right
    after that file when the drop is in its second half (or the first free
    place after it), or in front of it when the drop is in its first half -
    that file and every later one of the lane then shift right by the
    dropped file's length to make room. */
Session placeWithInsert (const Session& s, int index, int lane, int64_t offset);

/** Nearest offset >= 0 at or after `proposed` where clip `index` fits in
    `lane`, or -1 when none (cannot happen: the end of the lane is free). */
int64_t firstFreeOffset (const Session& s, int index, int lane, int64_t proposed);

/** Time warps after a length-changing edit of the composite (all lanes at
    once): [a, b) samples removed, `len` samples inserted at `a`, or
    [a, bOld) stretched to [a, bNew). A file fully inside a removed range is
    dropped from the session (its own file is left untouched). The main file
    always stays (it shifts like the others). */
void warpForDeletion (Session& s, int64_t a, int64_t b);
void warpForInsertion (Session& s, int64_t a, int64_t len);
void warpForStretch (Session& s, int64_t a, int64_t bOld, int64_t bNew);

/** Places every file's data in a zeroed composite: one row per composite
    channel, extent() samples long. data[i] is clip i's audio (one row per
    channel of its lane). */
std::vector<std::vector<float>> buildComposite (const Session& s,
                                                const std::vector<std::vector<std::vector<float>>>& data);

/** FNV-1a fingerprint of a clip's whole file: per channel its hidden start,
    its region read through `read(ch, start, n, dst)` (composite channels,
    samples) and its hidden end - so moving a trim does not change it. */
uint64_t regionHash (const Session& s, int index,
                     const std::function<bool (int, int64_t, int64_t, float*)>& read);

/** Splits a whole file's audio into the hidden start, the part on the
    timeline and the hidden end of `c` (its head/tail lengths). False when
    the file is too short for them. */
bool splitTrimmed (const std::vector<std::vector<float>>& whole, int64_t headLength, int64_t tailLength,
                   std::vector<std::vector<float>>& visible, HiddenAudio& head, HiddenAudio& tail);
/** The whole file again: hidden start + `visible` + hidden end. */
std::vector<std::vector<float>> joinTrimmed (const Clip& c, const std::vector<std::vector<float>>& visible);

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
