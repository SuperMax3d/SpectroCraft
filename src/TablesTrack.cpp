// SpectroCraft - Dedicated tables track: plots every visible parameter on an
// auto-scaled value axis with vertical zoom/pan, a red playhead and hover
// labels showing the unscaled values of curves near the cursor.
#include "TablesTrack.h"

TablesTrackView::TablesTrackView (Application& appIn)
    : app (appIn)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void TablesTrackView::setTimeRange (double t0In, double t1In)
{
    if (t0In == t0 && t1In == t1) return;
    t0 = std::min (t0In, t1In);
    t1 = std::max (std::nextafter (t0, 2.0), t1In);
    repaint();
}

void TablesTrackView::setPlayhead (double tSecs)
{
    if (tSecs == playheadT) return;
    playheadT = tSecs;
    repaint();
}

juce::Rectangle<int> TablesTrackView::plotRect() const
{
    // Same horizontal layout as the data tracks: axis strip on the left, plot
    // filling the rest (the scrollbar sits outside this component).
    return { kAxisW, 0, std::max (1, getWidth() - kAxisW), std::max (1, getHeight()) };
}

double TablesTrackView::valueAtY (int y) const
{
    const auto pr = plotRect();
    if (vMax <= vMin) return vMin;
    const double frac = ((double) y - (double) pr.getY()) / (double) pr.getHeight();
    return vMax + (vMin - vMax) * std::clamp (frac, 0.0, 1.0);
}

int TablesTrackView::yForValue (double v) const
{
    const auto pr = plotRect();
    if (vMax <= vMin) return pr.getY() + pr.getHeight() / 2;
    const double frac = (vMax - v) / (vMax - vMin);
    return (int) std::lround ((double) pr.getY() + frac * (double) pr.getHeight());
}

void TablesTrackView::refit()
{
    // Auto-scale: the visible window spans all raw values of every visible
    // parameter, padded by 10% on each side.
    double mn = 0.0, mx = 0.0;
    bool have = false;
    for (const auto& t : app.activeTables())
    {
        if (!t.visible) continue;
        for (const auto& p : t.params)
        {
            if (!p.visible || p.values.empty()) continue;
            double lo = p.values[0], hi = p.values[0];
            for (double v : p.values) { lo = std::min (lo, v); hi = std::max (hi, v); }
            if (!have) { mn = mx = lo; have = true; }
            mn = std::min (mn, lo);
            mx = std::max (mx, hi);
        }
    }
    if (!have) { fitMin = vMin = -1.0; fitMax = vMax = 1.0; repaint(); return; }

    const double pad = std::max ((mx - mn) * 0.1, 1e-9);
    fitMin = mn - pad;
    fitMax = mx + pad;
    vMin = fitMin;
    vMax = fitMax;
    if (vMax <= vMin) vMax = vMin + 1e-9;
    repaint();
}

// ------------------------------------------------------------------ Paint --

void TablesTrackView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    g.fillAll (juce::Colours::black);
    if (!app.audioEngine().isFileLoaded()) return;

    const auto pr = plotRect();

    // Value axis: gridlines with labels in the left strip, like the other tracks.
    g.setColour (juce::Colour::fromRGB (32, 32, 36));
    g.fillRect (0, 0, kAxisW, getHeight());
    const int nLines = std::max (2, std::min (8, getHeight() / 18));
    g.setFont (11.0f);
    for (int i = 0; i <= nLines; ++i)
    {
        const double v = vMax + (vMin - vMax) * ((double) i / nLines);
        const int y = yForValue (v);
        g.setColour (juce::Colour::fromRGB (60, 60, 68));
        g.drawHorizontalLine (kAxisW, (float) y, (float) getWidth());
        g.setColour (juce::Colours::lightgrey);
        g.drawText (juce::String (v, 4), juce::Rectangle<int> (2, y - 8, kAxisW - 6, 15),
                    juce::Justification::left, true);
    }

    // One curve per visible parameter (raw values through the effective time
    // map; value transforms are NOT applied here - this is the unscaled view).
    for (const auto& t : app.activeTables())
    {
        if (!t.visible) continue;
        for (const auto& p : t.params)
        {
            if (!p.visible || p.values.size() < 2) continue;
            const auto [off, sc] = t.effectiveTimeMap (p);

            juce::Path path;
            bool started = false;
            for (int i = 0; i < pr.getWidth(); ++i)
            {
                const double tDoc = t0 + ((double) (pr.getX() + i) / (double) pr.getWidth()) * (t1 - t0);
                const double tTab = (tDoc - off) / sc;
                if (tTab < 0.0 || tTab > p.duration (t.sampleRate)) { started = false; continue; }
                const double v = sc::tables::paramValueAt (t, p, tTab);
                const int y = yForValue (v);
                const float x = (float) (pr.getX() + i) + 0.5f;
                if (!started) { path.startNewSubPath (x, (float) y); started = true; }
                else          path.lineTo (x, (float) y);
            }
            g.setColour (sc::tables::kPalette[p.colorIndex % 12]);
            g.strokePath (path, juce::PathStrokeType (2.0f));
        }
    }

    // Red playhead over the whole strip.
    if (t1 > t0)
    {
        const double px = pr.getX() + (playheadT - t0) / (t1 - t0) * pr.getWidth();
        if (px >= pr.getX() && px <= pr.getRight())
        {
            g.setColour (juce::Colour::fromRGB (255, 60, 60));
            g.drawVerticalLine ((int) std::lround (px), 0.0f, (float) getHeight());
        }
    }

    // Hover labels: unscaled values of the curves near the cursor, stacked
    // from the top with a fixed spacing so they never overlap.
    if (!hoverLabels.empty())
    {
        g.setFont (12.0f);
        const juce::Font f (12.0f);
        int y = 4;
        for (const auto& hl : hoverLabels)
        {
            const auto sz = (int) juce::GlyphArrangement::getStringWidth (f, hl.text);
            juce::Rectangle<int> box ((int) hoverPos.x + 12, y, sz + 8, 15);
            g.setColour (juce::Colour::fromRGBA (20, 20, 24, 200));
            g.fillRoundedRectangle (box.toFloat(), 3.0f);
            g.setColour (hl.colour);
            g.drawText (hl.text, box, juce::Justification::centredLeft, true);
            y += 17;   // fixed spacing keeps the labels from overlapping
        }
    }
}

// ----------------------------------------------------------------- Mouse --

void TablesTrackView::mouseDown (const juce::MouseEvent& e)
{
    if (!app.audioEngine().isFileLoaded()) return;
    dragging = true;
    dragStartY = e.y;
    dragVMin = vMin;
    dragVMax = vMax;
}

void TablesTrackView::mouseDrag (const juce::MouseEvent& e)
{
    if (!dragging || vMax <= vMin) return;
    // Vertical pan: dragging down moves the window up in value space.
    const auto pr = plotRect();
    const double dv = ((double) dragStartY - (double) e.y) * (vMax - vMin) / pr.getHeight();
    vMin = dragVMin + dv;
    vMax = dragVMax + dv;
    repaint();
}

void TablesTrackView::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
}

void TablesTrackView::mouseMove (const juce::MouseEvent& e)
{
    hoverPos = { e.x, e.y };
    collectHoverLabels (e.x, e.y);
    repaint();
}

void TablesTrackView::mouseExit (const juce::MouseEvent&)
{
    hoverPos = { -1, -1 };
    hoverLabels.clear();
    repaint();
}

void TablesTrackView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!app.audioEngine().isFileLoaded() || vMax <= vMin) return;
    // Vertical zoom centred on the cursor position.
    const double pivot = valueAtY (e.y);
    const double factor = std::pow (1.2, -wheel.deltaY);
    vMin = pivot + (vMin - pivot) * factor;
    vMax = pivot + (vMax - pivot) * factor;
    if (vMax - vMin < 1e-12 || !(std::isfinite ((float) (vMax - vMin)))) refit();
    repaint();
}

void TablesTrackView::collectHoverLabels (int mouseX, int mouseY)
{
    hoverLabels.clear();
    if (!app.audioEngine().isFileLoaded() || mouseX < kAxisW) return;

    const auto pr = plotRect();
    if (mouseX >= pr.getRight()) return;
    const double tDoc = t0 + ((double) (mouseX - pr.getX()) / (double) pr.getWidth()) * (t1 - t0);

    struct Hit { float y; juce::String text; juce::Colour colour; };
    std::vector<Hit> hits;
    for (const auto& t : app.activeTables())
    {
        if (!t.visible) continue;
        for (const auto& p : t.params)
        {
            if (!p.visible || p.values.size() < 2) continue;
            const auto [off, sc] = t.effectiveTimeMap (p);
            const double tTab = (tDoc - off) / sc;
            if (tTab < 0.0 || tTab > p.duration (t.sampleRate)) continue;
            const double vRaw = sc::tables::paramValueAt (t, p, tTab);   // unscaled value
            const int y = yForValue (vRaw);
            if (std::abs ((float) y - (float) mouseY) <= 8.0f)
                hits.push_back ({ (float) y, p.name + "  " + juce::String (vRaw, 4),
                                  sc::tables::kPalette[p.colorIndex % 12] });
        }
    }

    // Sort by screen position and stack the labels from the top of the strip.
    std::sort (hits.begin(), hits.end(),
               [] (const Hit& a, const Hit& b) { return a.y < b.y; });
    for (const auto& h : hits)
        hoverLabels.push_back ({ h.text, 4.0f, h.colour });   // y fixed in paint()
}
