#include "SessionOverlay.h"

#include <algorithm>
#include <cmath>

namespace sc::session
{

namespace
{
    // Union of the visible rows of a lane (empty when none is visible).
    juce::Rectangle<int> laneRows (const Session& s, const ViewGeometry& g, int lane)
    {
        juce::Rectangle<int> r;
        for (size_t i = 0; i < g.rows.size() && i < g.rowChannel.size(); ++i)
            if (s.laneOfChannel (g.rowChannel[i]) == lane)
                r = r.isEmpty() ? g.rows[i] : r.getUnion (g.rows[i]);
        return r;
    }

    juce::Rectangle<int> spanBox (const ViewGeometry& g, juce::Rectangle<int> rows, int64_t start, int64_t end)
    {
        if (rows.isEmpty() || g.rate <= 0.0 || g.timeToX == nullptr) return {};
        const double x0 = g.timeToX ((double) start / g.rate);
        const double x1 = g.timeToX ((double) end / g.rate);
        const int left = (int) std::floor (std::max ((double) g.plot.getX(), x0));
        const int right = (int) std::ceil (std::min ((double) g.plot.getRight(), x1));
        if (right <= left) return {};
        return { left, rows.getY(), right - left, rows.getHeight() };
    }
}

std::vector<ClipBox> clipBoxes (const Session& s, const ViewGeometry& g)
{
    std::vector<ClipBox> out;
    for (int i = 0; i < (int) s.clips.size(); ++i)
    {
        const auto& c = s.clips[(size_t) i];
        const auto box = spanBox (g, laneRows (s, g, c.lane), c.offset, c.end());
        if (!box.isEmpty()) out.push_back ({ i, box });
    }
    return out;
}

juce::Rectangle<int> boxFor (const Session& s, const ViewGeometry& g, int clip, int lane, int64_t offset)
{
    if (clip < 0 || clip >= (int) s.clips.size()) return {};
    return spanBox (g, laneRows (s, g, lane), offset, offset + s.clips[(size_t) clip].length);
}

int laneAtY (const Session& s, const ViewGeometry& g, int y)
{
    for (size_t i = 0; i < g.rows.size() && i < g.rowChannel.size(); ++i)
        if (y >= g.rows[i].getY() && y < g.rows[i].getBottom())
            return s.laneOfChannel (g.rowChannel[i]);
    return -1;
}

namespace
{
    const juce::Font& tagFont()
    {
        static const juce::Font f { juce::FontOptions (11.0f) };
        return f;
    }
}

juce::Rectangle<int> nameTag (const Session& s, const ViewGeometry&, const ClipBox& b)
{
    if (b.clip < 0 || b.clip >= (int) s.clips.size()) return {};
    const auto name = s.clips[(size_t) b.clip].file.getFileName();
    const int w = std::min (b.box.getWidth() - 4, juce::GlyphArrangement::getStringWidthInt (tagFont(), name) + 8);
    if (w <= 12) return {};
    return { b.box.getX() + 2, b.box.getY() + 2, w, 15 };
}

int clipAtNameTag (const Session& s, const ViewGeometry& g, juce::Point<int> p)
{
    if (!s.active()) return -1;
    for (const auto& b : clipBoxes (s, g))
        if (b.clip >= 0 && nameTag (s, g, b).contains (p))
            return b.clip;
    return -1;
}

int clipAtPoint (const Session& s, const ViewGeometry& g, juce::Point<int> p)
{
    for (const auto& b : clipBoxes (s, g))
        if (b.box.contains (p))
            return b.clip;
    return -1;
}

void paintBorders (juce::Graphics& gr, const Session& s, const ViewGeometry& g)
{
    if (!s.active()) return;
    gr.setFont (tagFont());
    for (const auto& b : clipBoxes (s, g))
    {
        const bool main = b.clip == 0;
        const auto col = main ? juce::Colour (0xffffd166) : juce::Colour (0xff7fd4ff);
        gr.setColour (col.withAlpha (0.85f));
        gr.drawRect (b.box, 1);
        const auto tag = nameTag (s, g, b);
        if (!tag.isEmpty())
        {
            gr.setColour (juce::Colours::black.withAlpha (0.6f));
            gr.fillRect (tag);
            gr.setColour (col);
            gr.drawText (s.clips[(size_t) b.clip].file.getFileName(), tag.reduced (4, 0),
                         juce::Justification::centredLeft, true);
        }
    }
}

bool MoveGesture::begin (const Session& s, const ViewGeometry& g, juce::Point<int> p)
{
    clip_ = -1;
    if (!s.active() || g.xToTime == nullptr || g.rate <= 0.0) return false;
    for (const auto& b : clipBoxes (s, g))
        if (b.clip >= 0 && b.box.contains (p))
        {
            clip_ = b.clip;
            grabTime_ = g.xToTime (p.x);
            startOffset_ = s.clips[(size_t) b.clip].offset;
            cur_ = { clip_, s.clips[(size_t) clip_].lane, startOffset_, true };
            snapped_ = false;
            return true;
        }
    return false;
}

void MoveGesture::drag (const Session& s, const ViewGeometry& g, juce::Point<int> p, bool snap, double playheadSecs)
{
    if (!active() || clip_ >= (int) s.clips.size()) return;
    const auto& c = s.clips[(size_t) clip_];
    const double t = g.xToTime (p.x);
    int64_t off = std::max<int64_t> (0, startOffset_ + (int64_t) std::llround ((t - grabTime_) * g.rate));
    int lane = laneAtY (s, g, p.y);
    if (lane < 0 || clip_ == 0) lane = cur_.lane;   // the main file keeps its own track
    snapped_ = false;
    if (snap)
    {
        const double secsPerPx = std::abs (g.xToTime (g.plot.getX() + 8) - g.xToTime (g.plot.getX())) / 8.0;
        const int64_t tol = (int64_t) std::llround (8.0 * secsPerPx * g.rate);
        const int64_t snappedOff = snapOffset (s, clip_, off, tol,
                                               { (int64_t) std::llround (playheadSecs * g.rate) });
        snapped_ = snappedOff != off;
        off = snappedOff;
    }
    const bool sameShape = lane >= 0 && lane < s.numLanes()
                           && (s.laneChannels[(size_t) lane] == s.laneChannels[(size_t) c.lane]
                               || (lane != c.lane && s.laneEmpty (lane)));
    // On another file it is not refused any more: it goes in front of it or
    // after it (placeWithInsert), which the preview shows.
    cur_ = { clip_, lane, off, sameShape };
}

MoveGesture::Target MoveGesture::end()
{
    Target t = cur_;
    if (!active()) t.clip = -1;
    clip_ = -1;
    return t;
}

void MoveGesture::paint (juce::Graphics& gr, const Session& s, const ViewGeometry& g) const
{
    if (!active()) return;
    // Where everything lands: the dragged file, and the files it pushes right
    // when it is dropped in front of one (thin outlines).
    const Session placed = cur_.valid ? placeWithInsert (s, cur_.clip, cur_.lane, cur_.offset) : s;
    if (cur_.valid)
        for (int j = 0; j < (int) placed.clips.size() && j < (int) s.clips.size(); ++j)
            if (j != cur_.clip && placed.clips[(size_t) j].offset != s.clips[(size_t) j].offset)
            {
                const auto pushed = boxFor (placed, g, j, placed.clips[(size_t) j].lane, placed.clips[(size_t) j].offset);
                gr.setColour (juce::Colour (0xff7fff7f).withAlpha (0.6f));
                gr.drawRect (pushed, 1);
            }
    const int64_t at = cur_.valid ? placed.clips[(size_t) cur_.clip].offset : cur_.offset;
    const auto box = boxFor (s, g, cur_.clip, cur_.lane, at);
    if (box.isEmpty()) return;
    const auto col = cur_.valid ? juce::Colour (0xff7fff7f) : juce::Colour (0xffff6060);
    gr.setColour (col.withAlpha (0.18f));
    gr.fillRect (box);
    gr.setColour (col);
    gr.drawRect (box, 2);
    if (snapped_)
    {
        gr.setColour (juce::Colours::yellow);
        gr.drawVerticalLine (box.getX(), (float) g.plot.getY(), (float) g.plot.getBottom());
        gr.drawVerticalLine (box.getRight() - 1, (float) g.plot.getY(), (float) g.plot.getBottom());
    }
}

int TrimGesture::edgeAt (const Session& s, const ViewGeometry& g, juce::Point<int> p, bool* left)
{
    if (!s.active()) return -1;
    for (const auto& b : clipBoxes (s, g))
    {
        if (b.clip < 1 || p.y < b.box.getY() || p.y >= b.box.getBottom()) continue;
        const auto& c = s.clips[(size_t) b.clip];
        // Only an edge that is really the file's edge (not the view's border).
        const bool leftVisible = g.timeToX != nullptr && std::abs (g.timeToX ((double) c.offset / g.rate) - b.box.getX()) < 1.5;
        const bool rightVisible = g.timeToX != nullptr && std::abs (g.timeToX ((double) c.end() / g.rate) - b.box.getRight()) < 1.5;
        if (leftVisible && std::abs (p.x - b.box.getX()) <= 4)       { if (left) *left = true;  return b.clip; }
        if (rightVisible && std::abs (p.x - b.box.getRight()) <= 4)  { if (left) *left = false; return b.clip; }
    }
    return -1;
}

bool TrimGesture::begin (const Session& s, const ViewGeometry& g, juce::Point<int> p)
{
    clip_ = -1;
    if (g.xToTime == nullptr || g.rate <= 0.0) return false;
    bool left = false;
    const int c = edgeAt (s, g, p, &left);
    if (c < 1) return false;
    clip_ = c;
    left_ = left;
    cur_ = { c, s.clips[(size_t) c].offset, s.clips[(size_t) c].length, true };
    snapped_ = false;
    return true;
}

void TrimGesture::drag (const Session& s, const ViewGeometry& g, juce::Point<int> p, bool snap, double playheadSecs)
{
    if (!active() || clip_ >= (int) s.clips.size()) return;
    const auto& c = s.clips[(size_t) clip_];
    int64_t e = (int64_t) std::llround (g.xToTime (p.x) * g.rate);
    snapped_ = false;
    if (snap)
    {
        const double secsPerPx = std::abs (g.xToTime (g.plot.getX() + 8) - g.xToTime (g.plot.getX())) / 8.0;
        const int64_t tol = (int64_t) std::llround (8.0 * secsPerPx * g.rate);
        std::vector<int64_t> targets { 0, (int64_t) std::llround (playheadSecs * g.rate) };
        for (size_t j = 0; j < s.clips.size(); ++j)
            if ((int) j != clip_) { targets.push_back (s.clips[j].offset); targets.push_back (s.clips[j].end()); }
        int64_t best = e, bestD = tol + 1;
        for (auto t : targets)
            if (std::abs (t - e) <= tol && std::abs (t - e) < bestD) { best = t; bestD = std::abs (t - e); }
        snapped_ = best != e;
        e = best;
    }
    // Inside the whole file, and not over a neighbour of the same track.
    int64_t lo = c.fileStart(), hi = c.fileEnd();
    for (size_t j = 0; j < s.clips.size(); ++j)
    {
        const auto& o = s.clips[j];
        if ((int) j == clip_ || o.lane != c.lane) continue;
        if (o.end() <= c.offset) lo = std::max (lo, o.end());
        if (o.offset >= c.end()) hi = std::min (hi, o.offset);
    }
    if (left_)
    {
        const int64_t start = std::clamp<int64_t> (e, std::max<int64_t> (0, lo), c.end() - 1);
        cur_ = { clip_, start, c.end() - start, true };
    }
    else
    {
        const int64_t end = std::clamp<int64_t> (e, c.offset + 1, hi);
        cur_ = { clip_, c.offset, end - c.offset, true };
    }
}

TrimGesture::Target TrimGesture::end()
{
    Target t = cur_;
    if (!active()) t.clip = -1;
    clip_ = -1;
    return t;
}

void TrimGesture::paint (juce::Graphics& gr, const Session& s, const ViewGeometry& g) const
{
    if (!active() || clip_ >= (int) s.clips.size()) return;
    const auto& c = s.clips[(size_t) clip_];
    // The whole file faintly, the part kept clearly.
    Session whole = s;
    whole.clips[(size_t) clip_].offset = c.fileStart();
    whole.clips[(size_t) clip_].length = c.fileEnd() - c.fileStart();
    const auto all = boxFor (whole, g, clip_, c.lane, c.fileStart());
    gr.setColour (juce::Colour (0xff7fd4ff).withAlpha (0.35f));
    gr.drawRect (all, 1);
    Session kept = s;
    kept.clips[(size_t) clip_].length = cur_.length;
    const auto box = boxFor (kept, g, clip_, c.lane, cur_.offset);
    gr.setColour (juce::Colour (0xff7fff7f).withAlpha (0.18f));
    gr.fillRect (box);
    gr.setColour (juce::Colour (0xff7fff7f));
    gr.drawRect (box, 2);
    if (snapped_)
    {
        gr.setColour (juce::Colours::yellow);
        const int x = left_ ? box.getX() : box.getRight() - 1;
        gr.drawVerticalLine (x, (float) g.plot.getY(), (float) g.plot.getBottom());
    }
}

} // namespace sc::session
