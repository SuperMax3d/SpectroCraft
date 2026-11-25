#include "SoundMapView.h"
#include "MobileStyle.h"

namespace scm
{
using namespace smap;

namespace
{
constexpr double kSoundSpeed = 343.0;

juce::String bandName (double band)
{
    if (band == 0.0) return "LZeq";
    if (band < 0.0) return "LAeq";
    return bandLabel (band) + " Hz";
}
} // namespace

SoundMapView::SoundMapView()
{
    setOpaque (true);
}

void SoundMapView::setProject (const SoundMapProject* p)
{
    project = p;
    selectedPoint = -1;
    refresh();
}

void SoundMapView::setBand (double b)
{
    currentBand = b;
    refresh();
}

void SoundMapView::setRelative (bool r)
{
    relative = r;
    refresh();
}

void SoundMapView::setProjection (Projection p)
{
    projection = p;
    transformValid = false;
    refresh();
}

void SoundMapView::setSelected (int n)
{
    selectedPoint = n;
    repaint();
}

void SoundMapView::refresh()
{
    rebuild();
    transformValid = false;
    repaint();
}

juce::Colour SoundMapView::colourFor (double t)
{
    // viridis, readable on the dark background
    static const juce::Colour stops[] = { juce::Colour (0xff440154), juce::Colour (0xff3b528b), juce::Colour (0xff21918c),
                                          juce::Colour (0xff5ec962), juce::Colour (0xfffde725) };
    t = juce::jlimit (0.0, 1.0, t) * 4.0;
    const int i = juce::jlimit (0, 3, (int) t);
    return stops[i].interpolatedWith (stops[i + 1], (float) (t - i));
}

std::optional<double> SoundMapView::displayedValue (const MapPoint& p) const
{
    const double v = SoundMapProject::levelOf (p, currentBand);
    if (v < -250.0) return std::nullopt;
    return relative ? v - reference : v;
}

void SoundMapView::rebuild()
{
    shown.clear();
    grid = {};
    surface = {};
    unit = {};
    resolvedHz = 0.0;
    reference = 0.0;
    extent = 4.0;
    if (project == nullptr) return;
    unit = project->mapUnit();

    double far = 1.0;
    for (const auto& [id, t] : project->map.tags) far = std::max (far, std::hypot (t.t.x, t.t.y));
    std::vector<std::array<double, 2>> xy;
    std::vector<double> values;
    sideMaxR = far + 0.3;
    sideMinZ = -1.0;
    sideMaxZ = 0.5;
    for (const auto& [id, t] : project->map.tags) { sideMinZ = std::min (sideMinZ, t.t.z - 0.3); sideMaxZ = std::max (sideMaxZ, t.t.z + 0.3); }

    for (const auto& p : project->points)
    {
        if (p.pending) continue;
        Shown s;
        s.number = p.number;
        s.kind = p.kind;
        s.located = p.located;
        s.position = p.position;
        const double v = SoundMapProject::levelOf (p, currentBand);
        s.hasValue = v > -250.0 && p.levels.unit == unit;
        s.value = v;
        const double snr = project->snrDb (p, currentBand);
        s.weak = snr < 10.0;
        s.trusted = p.kind == PointKind::Point && p.good() && s.hasValue && snr >= 3.0;
        if (p.located)
        {
            far = std::max (far, std::hypot (p.position.x, p.position.y));
            sideMaxR = std::max (sideMaxR, std::hypot (p.position.x, p.position.y) + 0.3);
            sideMinZ = std::min (sideMinZ, p.position.z - 0.3);
            sideMaxZ = std::max (sideMaxZ, p.position.z + 0.3);
        }
        if (s.trusted)
        {
            xy.push_back ({ p.position.x, p.position.y });
            values.push_back (v);
        }
        shown.push_back (s);
    }
    extent = far + 0.4;

    // the reference of relative levels: the energy mean (of the surface when there is one)
    if (! values.empty())
    {
        double e = 0.0;
        for (double v : values) e += std::pow (10.0, (v - values.front()) / 10.0);
        reference = values.front() + 10.0 * std::log10 (e / (double) values.size());
    }
    if (xy.size() >= 3)   // both views: relative levels are re the surface's energy mean
    {
        const int n = juce::jlimit (8, 160, (int) std::ceil (2.0 * extent / 0.025));
        const double cell = 2.0 * extent / n;   // the grid spans exactly [-extent, extent]
        grid.cell = cell;
        grid.nx = grid.ny = n;
        grid.x0 = grid.y0 = -extent + 0.5 * cell;
        interpolate (grid, xy, values, adaptiveWidths (xy, 3, 0.15), 1.5);
        reference = gridEnergyMeanDb (grid);
    }
    if (const double s = typicalSpacing (xy); s > 0.0) resolvedHz = kSoundSpeed / (4.0 * s);

    // colour range over the points and the surface
    lo = 1e300; hi = -1e300;
    const double off = relative ? reference : 0.0;
    for (double v : values) { lo = std::min (lo, v - off); hi = std::max (hi, v - off); }
    for (size_t i = 0; i < grid.values.size(); ++i)
        if (grid.valid[i]) { lo = std::min (lo, (double) grid.values[i] - off); hi = std::max (hi, (double) grid.values[i] - off); }
    if (lo > hi) { lo = 0.0; hi = 1.0; }
    if (hi - lo < 1.0) { const double m = 0.5 * (hi + lo); lo = m - 0.5; hi = m + 0.5; }

    if (grid.nx > 0 && projection == Projection::Top)
    {
        surface = juce::Image (juce::Image::ARGB, grid.nx, grid.ny, true);
        juce::Image::BitmapData bd (surface, juce::Image::BitmapData::writeOnly);
        for (int iy = 0; iy < grid.ny; ++iy)
            for (int ix = 0; ix < grid.nx; ++ix)
            {
                const size_t i = (size_t) iy * (size_t) grid.nx + (size_t) ix;
                if (! grid.valid[i]) continue;
                const double t = ((double) grid.values[i] - off - lo) / (hi - lo);
                bd.setPixelColour (ix, grid.ny - 1 - iy, colourFor (t).withAlpha (0.85f));
            }
    }
}

void SoundMapView::updateTransform()
{
    auto r = getLocalBounds().toFloat().reduced (8.0f);
    r.removeFromRight (64.0f);   // legend
    r.removeFromBottom (14.0f);
    if (projection == Projection::Top)
    {
        const float size = std::min (r.getWidth(), r.getHeight());
        plot = r.withSizeKeepingCentre (size, size);
    }
    else
        plot = r.withTrimmedLeft (26.0f);
    transformValid = true;
}

juce::Point<float> SoundMapView::toScreen (double x, double y) const
{
    if (projection == Projection::Top)
        return { plot.getCentreX() + (float) (x / extent) * plot.getWidth() * 0.5f,
                 plot.getCentreY() - (float) (y / extent) * plot.getHeight() * 0.5f };
    // side: x = distance from the axis, y = height
    return { plot.getX() + (float) (x / sideMaxR) * plot.getWidth(),
             plot.getBottom() - (float) ((y - sideMinZ) / (sideMaxZ - sideMinZ)) * plot.getHeight() };
}

void SoundMapView::paint (juce::Graphics& g)
{
    g.fillAll (style::plotBackground);
    if (! transformValid) updateTransform();
    g.setFont (style::font (13.0f));
    if (project == nullptr)
    {
        g.setColour (style::textDim);
        g.drawFittedText ("No sound map. Create one from the menu (...).", getLocalBounds().reduced (12), juce::Justification::centred, 3);
        return;
    }
    if (projection == Projection::Top) drawTop (g);
    else drawSide (g);

    auto legend = getLocalBounds().toFloat().reduced (8.0f).removeFromRight (56.0f);
    drawLegend (g, legend);

    if (! project->map.built)
    {
        g.setColour (style::warning);
        g.drawFittedText ("No marker map yet: Survey, then Build map.", getLocalBounds().reduced (12).removeFromTop (40),
                          juce::Justification::centredTop, 2);
    }
}

void SoundMapView::drawTop (juce::Graphics& g)
{
    // 1 m grid
    g.setColour (style::grid.withAlpha (0.6f));
    for (int k = -(int) extent; k <= (int) extent; ++k)
    {
        const auto a = toScreen (k, -extent), b = toScreen (k, extent);
        g.drawLine (a.x, a.y, b.x, b.y, 0.5f);
        const auto c = toScreen (-extent, k), d = toScreen (extent, k);
        g.drawLine (c.x, c.y, d.x, d.y, 0.5f);
    }
    if (surface.isValid())
    {
        // inside the tags' circle only: the bowl ends there
        juce::Graphics::ScopedSaveState save (g);
        if (project->map.radius > 0.0)
        {
            const auto c = toScreen (0.0, 0.0);
            const float rpx = (float) (project->map.radius / extent) * plot.getWidth() * 0.5f;
            juce::Path clip;
            clip.addEllipse (c.x - rpx, c.y - rpx, 2.0f * rpx, 2.0f * rpx);
            g.reduceClipRegion (clip);
        }
        const auto tl = toScreen (-extent, extent), br = toScreen (extent, -extent);
        g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
        g.drawImage (surface, juce::Rectangle<float> (tl, br));
    }
    // the tags' circle and the axes
    if (project->map.radius > 0.0)
    {
        const auto c = toScreen (0.0, 0.0);
        const float rpx = (float) (project->map.radius / extent) * plot.getWidth() * 0.5f;
        g.setColour (style::axisLabel.withAlpha (0.5f));
        g.drawEllipse (c.x - rpx, c.y - rpx, 2.0f * rpx, 2.0f * rpx, 1.0f);
    }
    {
        const auto o = toScreen (0.0, 0.0), x = toScreen (0.6, 0.0);
        g.setColour (style::axisLabel.withAlpha (0.7f));
        g.drawArrow ({ o, x }, 1.0f, 6.0f, 6.0f);
        g.drawText ("x", juce::Rectangle<float> (x.x + 2.0f, x.y - 8.0f, 14.0f, 16.0f), juce::Justification::centredLeft);
    }
    // tags
    g.setFont (style::font (11.0f));
    for (const auto& [id, t] : project->map.tags)
    {
        const auto p = toScreen (t.t.x, t.t.y);
        g.setColour (style::accent);
        g.fillRect (juce::Rectangle<float> (7.0f, 7.0f).withCentre (p));
        g.setColour (style::axisLabel);
        const auto out = juce::Point<float> ((float) t.t.x, (float) t.t.y);
        const float len = std::max (0.001f, out.getDistanceFromOrigin());
        const auto lp = p + juce::Point<float> (out.x / len * 12.0f, -out.y / len * 12.0f);
        g.drawText (juce::String (id), juce::Rectangle<float> (22.0f, 14.0f).withCentre (lp), juce::Justification::centred);
    }
    // points
    for (const auto& s : shown)
        if (s.located) drawPoint (g, s, toScreen (s.position.x, s.position.y));

    // 1 m scale
    const auto a = toScreen (-extent + 0.2, -extent + 0.15), b = toScreen (-extent + 1.2, -extent + 0.15);
    g.setColour (style::axisLabel);
    g.drawLine (a.x, a.y, b.x, b.y, 2.0f);
    g.setFont (style::font (11.0f));
    g.drawText ("1 m", juce::Rectangle<float> (a.x, a.y - 16.0f, b.x - a.x, 14.0f), juce::Justification::centred);
}

void SoundMapView::drawSide (juce::Graphics& g)
{
    g.setFont (style::font (11.0f));
    for (int k = 0; k <= (int) sideMaxR; ++k)
    {
        const auto a = toScreen (k, sideMinZ), b = toScreen (k, sideMaxZ);
        g.setColour (style::grid.withAlpha (0.6f));
        g.drawLine (a.x, a.y, b.x, b.y, 0.5f);
        g.setColour (style::axisMinorLabel);
        g.drawText (juce::String (k), juce::Rectangle<float> (a.x - 10.0f, a.y + 1.0f, 20.0f, 12.0f), juce::Justification::centred);
    }
    for (int k = (int) std::ceil (sideMinZ); k <= (int) std::floor (sideMaxZ); ++k)
    {
        const auto a = toScreen (0.0, k), b = toScreen (sideMaxR, k);
        g.setColour (k == 0 ? style::axisLabel.withAlpha (0.6f) : style::grid.withAlpha (0.6f));
        g.drawLine (a.x, a.y, b.x, b.y, k == 0 ? 1.0f : 0.5f);
        g.setColour (style::axisMinorLabel);
        g.drawText (juce::String (k), juce::Rectangle<float> (a.x - 26.0f, a.y - 6.0f, 22.0f, 12.0f), juce::Justification::centredRight);
    }
    g.setColour (style::axisMinorLabel);
    g.drawText ("distance from the axis (m) / height (m)", plot.withHeight (14.0f).translated (0.0f, -2.0f), juce::Justification::centredRight);
    for (const auto& [id, t] : project->map.tags)
    {
        g.setColour (style::accent);
        g.fillRect (juce::Rectangle<float> (6.0f, 6.0f).withCentre (toScreen (std::hypot (t.t.x, t.t.y), t.t.z)));
    }
    for (const auto& s : shown)
        if (s.located) drawPoint (g, s, toScreen (std::hypot (s.position.x, s.position.y), s.position.z));
}

void SoundMapView::drawPoint (juce::Graphics& g, const Shown& s, juce::Point<float> at)
{
    const bool sel = s.number == selectedPoint;
    const float r = sel ? 7.0f : 5.0f;
    const auto box = juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (at);
    const double off = relative ? reference : 0.0;
    const auto colour = colourFor ((s.value - off - lo) / (hi - lo));
    if (s.kind == PointKind::Background)
    {
        g.setColour (style::textDim);
        g.drawRect (box, 1.5f);
        g.setFont (style::font (9.0f, true));
        g.drawText ("B", box, juce::Justification::centred);
    }
    else if (s.kind == PointKind::Reference)
    {
        juce::Path d;
        d.addQuadrilateral (at.x, at.y - r, at.x + r, at.y, at.x, at.y + r, at.x - r, at.y);
        g.setColour (s.hasValue ? colour : style::textDim);
        g.strokePath (d, juce::PathStrokeType (2.0f));
    }
    else if (s.trusted && ! s.weak)
    {
        g.setColour (colour);
        g.fillEllipse (box);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawEllipse (box, 1.0f);
    }
    else if (s.trusted)
    {
        g.setColour (colour);
        g.drawEllipse (box.reduced (1.0f), 2.0f);
    }
    else
    {
        g.setColour (style::textDim);
        g.drawLine ({ box.getTopLeft(), box.getBottomRight() }, 1.5f);
        g.drawLine ({ box.getBottomLeft(), box.getTopRight() }, 1.5f);
    }
    if (sel)
    {
        g.setColour (juce::Colours::white);
        g.drawEllipse (box.expanded (3.0f), 1.5f);
    }
}

void SoundMapView::drawLegend (juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setFont (style::font (11.0f, true));
    g.setColour (style::text);
    g.drawText (bandName (currentBand), area.removeFromTop (16.0f), juce::Justification::centred);
    if (shown.empty()) return;
    auto foot = area.removeFromBottom (44.0f);
    auto bar = area.reduced (4.0f, 14.0f).withWidth (14.0f);
    for (int y = 0; y < (int) bar.getHeight(); ++y)
    {
        g.setColour (colourFor (1.0 - (double) y / std::max (1.0, (double) bar.getHeight() - 1.0)));
        g.fillRect (bar.getX(), bar.getY() + (float) y, bar.getWidth(), 1.0f);
    }
    g.setFont (style::font (11.0f));
    g.setColour (style::axisLabel);
    const auto fmt = [this] (double v) { return (relative && v > 0.0 ? "+" : "") + juce::String (v, 1); };
    g.drawText (fmt (hi), juce::Rectangle<float> (area.getX(), bar.getY() - 14.0f, area.getWidth(), 13.0f), juce::Justification::centredLeft);
    g.drawText (fmt (lo), juce::Rectangle<float> (area.getX(), bar.getBottom() + 1.0f, area.getWidth(), 13.0f), juce::Justification::centredLeft);
    g.setColour (style::textDim);
    g.drawFittedText (relative ? juce::String ("dB re mean") : unit, foot.removeFromTop (16.0f).toNearestInt(), juce::Justification::centred, 1);
    if (resolvedHz > 0.0 && currentBand > resolvedHz)
    {
        g.setColour (style::warning);
        g.drawFittedText ("trend only", foot.toNearestInt(), juce::Justification::centredTop, 2);
    }
}

void SoundMapView::mouseUp (const juce::MouseEvent& e)
{
    if (project == nullptr || ! transformValid) return;
    int best = -1;
    float bestD = 24.0f;
    for (const auto& s : shown)
    {
        if (! s.located) continue;
        const auto at = projection == Projection::Top ? toScreen (s.position.x, s.position.y)
                                                      : toScreen (std::hypot (s.position.x, s.position.y), s.position.z);
        const float d = at.getDistanceFrom (e.position);
        if (d < bestD) { bestD = d; best = s.number; }
    }
    selectedPoint = best;
    repaint();
    if (onPointTapped) onPointTapped (best);
}
} // namespace scm
