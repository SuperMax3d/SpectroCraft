#pragma once
#include <JuceHeader.h>
#include "Session.h"

#include <functional>
#include <vector>

/*  Session files in the waveform and spectrogram views (issue #21): their
    outlines and names (Edit > Session > Show File Borders) and the Move Files
    tool's drag, shared by both views. A view describes its layout with a
    ViewGeometry; everything else is computed here. */
namespace sc::session
{

struct ViewGeometry
{
    std::vector<int> rowChannel;                 // composite channel shown by each row
    std::vector<juce::Rectangle<int>> rows;      // the rows, in component coordinates
    juce::Rectangle<int> plot;                   // the plot area (span of the time axis)
    std::function<double (double)> timeToX;
    std::function<double (int)> xToTime;
    double rate = 0.0;
};

struct ClipBox
{
    int clip = -1;
    juce::Rectangle<int> box;
};

/** Rectangle of every file over the visible rows of its lane, clipped to the plot. */
std::vector<ClipBox> clipBoxes (const Session& s, const ViewGeometry& g);

/** Rectangle file `clip` would cover in `lane` at `offset` (empty when the
    lane has no visible row). */
juce::Rectangle<int> boxFor (const Session& s, const ViewGeometry& g, int clip, int lane, int64_t offset);

/** Lane of the row under y, or -1. */
int laneAtY (const Session& s, const ViewGeometry& g, int y);

/** Outlines and file names over the views (View > Show File Borders; the
    caller decides whether they are shown). */
void paintBorders (juce::Graphics& gr, const Session& s, const ViewGeometry& g);

/** The name tag paintBorders() draws in a file's top-left corner. */
juce::Rectangle<int> nameTag (const Session& s, const ViewGeometry& g, const ClipBox& b);
/** The file whose name tag holds p (the main file included), or -1: a
    press there moves that file whatever the selection tool. */
int clipAtNameTag (const Session& s, const ViewGeometry& g, juce::Point<int> p);
/** The file under p (any file, the main one included), or -1. */
int clipAtPoint (const Session& s, const ViewGeometry& g, juce::Point<int> p);

/** The Move Files tool's drag. */
class MoveGesture
{
public:
    struct Target
    {
        int clip = -1, lane = 0;
        int64_t offset = 0;
        bool valid = false;   // the lane has the file's channel count and the place is free
    };

    /** Starts a drag when p is on a file (the main one only moves along
        its own track). */
    bool begin (const Session& s, const ViewGeometry& g, juce::Point<int> p);
    /** Follows the mouse: time moves the file, the row picks the lane; with
        `snap` (the default - Shift turns it off) the start or end jumps to 0,
        another file's start or end or the playhead within 8 pixels. */
    void drag (const Session& s, const ViewGeometry& g, juce::Point<int> p, bool snap, double playheadSecs);
    /** Ends the drag and returns where the file should go (clip = -1: no drag). */
    Target end();
    void cancel() noexcept { clip_ = -1; }
    bool active() const noexcept { return clip_ >= 0; }
    const Target& target() const noexcept { return cur_; }
    void paint (juce::Graphics& gr, const Session& s, const ViewGeometry& g) const;

private:
    int clip_ = -1;
    double grabTime_ = 0.0;
    int64_t startOffset_ = 0;
    Target cur_;
    bool snapped_ = false;
};

/** Trimming a file by dragging one of its edges (PR #24 review), with Move
    Files armed: the edge follows the mouse inside the whole file (what is
    hidden can come back), never over a neighbour of its track, snapping like
    a move (Shift turns it off). Not for the main file. */
class TrimGesture
{
public:
    struct Target
    {
        int clip = -1;
        int64_t offset = 0, length = 0;
        bool valid = false;
    };

    /** The referenced file whose left or right edge is within 4 pixels of p
        (-1 when none); *left says which edge. For the resize cursor too. */
    static int edgeAt (const Session& s, const ViewGeometry& g, juce::Point<int> p, bool* left = nullptr);
    bool begin (const Session& s, const ViewGeometry& g, juce::Point<int> p);
    void drag (const Session& s, const ViewGeometry& g, juce::Point<int> p, bool snap, double playheadSecs);
    Target end();
    void cancel() noexcept { clip_ = -1; }
    bool active() const noexcept { return clip_ >= 1; }
    const Target& target() const noexcept { return cur_; }
    void paint (juce::Graphics& gr, const Session& s, const ViewGeometry& g) const;

private:
    int clip_ = -1;
    bool left_ = false;
    Target cur_;
    bool snapped_ = false;
};

} // namespace sc::session
