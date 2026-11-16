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
        if (b.clip >= 1 && nameTag (s, g, b).contains (p))
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
        if (b.clip >= 1 && b.box.contains (p))
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
    if (lane < 0) lane = cur_.lane;
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
                           && s.laneChannels[(size_t) lane] == s.laneChannels[(size_t) c.lane];
    cur_ = { clip_, lane, off, sameShape && fits (s, clip_, lane, off) };
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
    const auto box = boxFor (s, g, cur_.clip, cur_.lane, cur_.offset);
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

} // namespace sc::session
