// SpectroCraft Mobile - where the video goes on the screen and how the movie
// follows the recording's playhead. Pure functions (no component, no player),
// so the unit tests can check them; VideoPane does the rest.
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>

namespace scm::video
{
constexpr int kHeaderHeight = 28;   // the pane's title row (tap: hide / show the picture)
constexpr int kGap = 2;             // between the pane and the views
/** The pane takes at most this share of the views' width (side column) or
    height (top strip), so the waveform and the spectrogram stay usable. */
constexpr double kMaxShare = 0.5;

struct Split
{
    juce::Rectangle<int> pane;   // the video pane: title row + picture
    juce::Rectangle<int> rest;   // what is left for the waveform / spectrogram
    bool sideColumn = false;     // the pane is a column on the left (else a strip on top)
};

/** Gives the video pane its own part of the views area, sized to the picture.

    `aspect` is the picture's width / height (a phone held upright films a
    portrait picture, < 1). The picture goes where it is largest within
    kMaxShare of the views: a column on the left (portrait pictures, wide
    views) or a strip on top (landscape pictures, tall views). A collapsed
    pane is only its title row, on top. */
inline Split split (juce::Rectangle<int> views, double aspect, bool collapsed)
{
    Split s;
    if (views.isEmpty()) { s.rest = views; return s; }
    if (collapsed || ! (aspect > 0.0))
    {
        s.pane = views.removeFromTop (kHeaderHeight);
        views.removeFromTop (kGap);
        s.rest = views;
        return s;
    }

    const double w = views.getWidth(), h = views.getHeight() - kHeaderHeight;

    // side column: the picture as tall as the views, at most kMaxShare of the width
    double colH = h, colW = colH * aspect;
    if (colW > w * kMaxShare) { colW = w * kMaxShare; colH = colW / aspect; }

    // top strip: the picture as wide as the views, at most kMaxShare of the height
    double stripW = w, stripH = stripW / aspect;
    if (stripH > (views.getHeight() * kMaxShare) - kHeaderHeight)
    {
        stripH = std::max (0.0, views.getHeight() * kMaxShare - kHeaderHeight);
        stripW = stripH * aspect;
    }

    if (colW * colH > stripW * stripH)
    {
        s.sideColumn = true;
        s.pane = views.removeFromLeft (std::max (1, juce::roundToInt (colW)));
        views.removeFromLeft (kGap);
    }
    else
    {
        s.pane = views.removeFromTop (kHeaderHeight + std::max (0, juce::roundToInt (stripH)));
        views.removeFromTop (kGap);
    }
    s.rest = views;
    return s;
}

// ---------------------------------------------------------------- playback

/** What the movie player is doing, and what the recording does. */
struct SyncInput
{
    double docTime = 0.0;        // the recording's playhead (seconds)
    bool docPlaying = false;
    double offset = 0.0;         // document time of the movie's first frame
    double duration = 0.0;       // the movie's length (0 = not known yet)
    double moviePos = 0.0;       // the player's position
    bool moviePlaying = false;
    double lastSeek = -1.0;      // the last position asked for while paused (-1 = none)
    bool mayCorrect = true;      // false right after a correction (the player lags)
};

struct SyncAction
{
    bool play = false, pause = false;
    bool seek = false;
    double seekTo = 0.0;
};

/** While playing, the player may drift this far before it is moved back. */
constexpr double kPlayingTolerance = 0.20;
/** While paused, a playhead move smaller than this keeps the frame shown. */
constexpr double kPausedTolerance = 0.02;
/** Past the end the movie shows a frame this far before its end (a seek to
    the very end shows nothing on some players). */
constexpr double kLastFrameMargin = 0.05;

/** One step of the 30 Hz sync: the movie shows the frame at docTime - offset,
    plays while the recording plays inside the movie, and waits on its first
    (or last) frame outside it. */
inline SyncAction syncStep (const SyncInput& in)
{
    SyncAction a;
    const double target = in.docTime - in.offset;
    const bool known = in.duration > 0.0;
    const bool inside = target >= 0.0 && (! known || target < in.duration);
    const double last = known ? std::max (0.0, in.duration - kLastFrameMargin) : std::max (target, 0.0);
    const double shown = std::clamp (target, 0.0, last);

    if (in.docPlaying && inside)
    {
        if (! in.moviePlaying)
        {
            a.play = true;
            if (std::abs (in.moviePos - target) > kPausedTolerance) { a.seek = true; a.seekTo = target; }
        }
        else if (in.mayCorrect && std::abs (in.moviePos - target) > kPlayingTolerance)
        {
            a.seek = true;
            a.seekTo = target;
        }
        return a;
    }

    if (in.moviePlaying) a.pause = true;
    const double from = in.lastSeek >= 0.0 && ! in.moviePlaying ? in.lastSeek : in.moviePos;
    if (std::abs (from - shown) > kPausedTolerance) { a.seek = true; a.seekTo = shown; }
    return a;
}
} // namespace scm::video
