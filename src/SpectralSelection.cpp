// SpectroCraft - spectral selection model implementation.
#include "SpectralSelection.h"
#include <cmath>

bool SpectralShape::contains (double tSecs, double fHz) const
{
    switch (kind)
    {
        case Kind::Rect:
            return tSecs >= std::min (t0, t1) && tSecs <= std::max (t0, t1)
                   && fHz >= std::min (f0, f1) && fHz <= std::max (f0, f1);

        case Kind::Polygon:
        {
            // Even-odd ray casting in (time, frequency) space.
            bool inside = false;
            const size_t n = pts.size();
            for (size_t i = 0, j = n - 1; i < n; j = i++)
            {
                const double xi = pts[i].first, yi = pts[i].second;
                const double xj = pts[j].first, yj = pts[j].second;
                if ((yi > fHz) != (yj > fHz))
                {
                    const double xCross = xi + (fHz - yi) * (xj - xi) / (yj - yi);
                    if (tSecs < xCross)
                        inside = !inside;
                }
            }
            return inside;
        }

        case Kind::Stroke:
        {
            if (cols <= 0 || rows <= 0 || grid.empty()) return false;
            const double colSpan = std::max (1e-9, (t1 - t0) / cols);
            const double rowHz   = std::max (1e-9, (f1 - f0) / rows);
            const int c = (int) std::floor ((tSecs - t0) / colSpan);
            const int r = (int) std::floor ((fHz - f0) / rowHz);
            if (c < 0 || c >= cols || r < 0 || r >= rows) return false;
            const size_t bit = (size_t) c * rows + (size_t) r;
            return (grid[bit >> 3] & (1u << (bit & 7))) != 0;
        }
    }
    return false;
}

juce::Rectangle<double> SpectralShape::boundingBox() const
{
    switch (kind)
    {
        case Kind::Rect:
            return { std::min (t0, t1), std::min (f0, f1),
                     std::max (t0, t1) - std::min (t0, t1),
                     std::max (f0, f1) - std::min (f0, f1) };

        case Kind::Polygon:
        {
            if (pts.size() < 3) return {};
            double loT = 1e30, hiT = -1e30, loF = 1e30, hiF = -1e30;
            for (const auto& p : pts)
            {
                loT = std::min (loT, p.first);  hiT = std::max (hiT, p.first);
                loF = std::min (loF, p.second); hiF = std::max (hiF, p.second);
            }
            return { loT, loF, hiT - loT, hiF - loF };
        }

        case Kind::Stroke:
            if (cols <= 0 || rows <= 0) return {};
            return { t0, f0, t1 - t0, f1 - f0 };
    }
    return {};
}

void SpectralSelection::clear()
{
    shapeList.clear();
}

static void normaliseRect (double& t0, double& t1, double& f0, double& f1)
{
    if (t0 > t1) std::swap (t0, t1);
    if (f0 > f1) std::swap (f0, f1);
}

void SpectralSelection::addRect (double a0, double a1, double b0, double b1,
                                 bool subtract, bool replace)
{
    normaliseRect (a0, a1, b0, b1);
    if (replace && !subtract)
        shapeList.clear();

    SpectralShape s;
    s.kind = SpectralShape::Kind::Rect;
    s.subtract = subtract;
    s.t0 = a0; s.t1 = a1; s.f0 = b0; s.f1 = b1;
    shapeList.push_back (std::move (s));
}

void SpectralSelection::addPolygon (std::vector<std::pair<double, double>> ptsIn,
                                    bool subtract, bool replace)
{
    // Drop consecutive duplicates and close the loop.
    std::vector<std::pair<double, double>> pts;
    for (auto& p : ptsIn)
        if (pts.empty() || p != pts.back())
            pts.push_back (p);
    if (pts.size() >= 2 && pts.front() == pts.back())
        pts.pop_back();
    if (pts.size() < 3) return;   // degenerate lasso: nothing to add

    if (replace && !subtract)
        shapeList.clear();

    SpectralShape s;
    s.kind = SpectralShape::Kind::Polygon;
    s.subtract = subtract;
    s.pts = std::move (pts);
    shapeList.push_back (std::move (s));
}

void SpectralSelection::addStroke (std::vector<std::pair<double, double>> ptsIn,
                                   double radiusHz, bool subtract, bool replace)
{
    // Thin the path: keep a point only when it moved at least ~1 ms or 8 Hz.
    // The first and last points are ALWAYS kept - they mark where the stroke
    // really starts and ends (a densely sampled straight line would otherwise
    // lose its exact endpoint if it falls within the threshold of the
    // previous kept point).
    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i < ptsIn.size (); ++i)
    {
        const auto& p = ptsIn[i];
        if (pts.empty () || i == ptsIn.size () - 1
            || std::abs (p.first - pts.back().first) >= 1e-3
            || std::abs (p.second - pts.back().second) >= 8.0)
            pts.push_back (p);
    }
    if (pts.empty()) return;

    if (replace && !subtract)
        shapeList.clear();

    SpectralShape s;
    s.kind = SpectralShape::Kind::Stroke;
    s.subtract = subtract;
    s.pts = pts;   // kept for rendering (the grid is only used by contains())
    s.strokeRadiusHz = std::max (1.0, radiusHz);
    s.rasteriseStroke (s.strokeRadiusHz);
    shapeList.push_back (std::move (s));
}

// Rasterises the stored pts into cols/rows/grid. Resolution is fixed in
// absolute units (~512 columns/second, ~32 Hz/row) and capped at 4096x4096
// cells so even hour-long strokes stay small (<= 2 MB).
void SpectralShape::rasteriseStroke (double radiusHz)
{
    if (kind != Kind::Stroke || pts.empty()) return;

    double tMin = 1e30, tMax = -1e30, fMin = 1e30, fMax = -1e30;
    for (auto& p : pts)
    {
        tMin = std::min (tMin, p.first);  tMax = std::max (tMax, p.first);
        fMin = std::min (fMin, p.second); fMax = std::max (fMax, p.second);
    }
    const double tSpan = std::max (1e-3, tMax - tMin);
    const double fSpan = std::max (8.0, fMax - fMin + 2.0 * radiusHz);

    const double colSpan = std::max (1.0 / 512.0, tSpan / 4096.0);
    const double rowHz   = std::max (32.0, fSpan / 4096.0);
    // The grid must cover [min - margin, max + margin] with a FULL cell of
    // margin on every side: an endpoint exactly at tMax/fMax would otherwise
    // map to index == cols/rows and be silently dropped by setBit/contains().
    cols = (int) std::min <double> (4096.0, std::ceil ((tSpan + colSpan) / colSpan));
    rows = (int) std::min <double> (4096.0, std::ceil ((fSpan + rowHz) / rowHz));
    if (cols <= 0 || rows <= 0) return;

    // The grid covers the path plus one brush radius of margin on every side.
    t0 = tMin - colSpan * 0.5;
    t1 = t0 + cols * colSpan;
    f0 = fMin - radiusHz - rowHz * 0.5;
    f1 = f0 + rows * rowHz;

    const size_t nBits = (size_t) cols * (size_t) rows;
    grid.assign ((nBits + 7) >> 3, 0);

    auto setBit = [&] (int c, int r) {
        if (c < 0 || c >= cols || r < 0 || r >= rows) return;
        const size_t bit = (size_t) c * (size_t) rows + (size_t) r;
        grid[bit >> 3] |= (uint8_t) (1u << (bit & 7));
    };

    // Stamp one brush disc centred on (t, f): the column containing t plus
    // every row whose centre lies within radiusHz of f.
    auto stamp = [&] (double t, double f) {
        const int c = (int) std::floor ((t - t0) / colSpan);
        double rLo = (f - radiusHz - 0.5 * rowHz - f0) / rowHz;
        double rHi = (f + radiusHz - 0.5 * rowHz - f0) / rowHz;
        int r0 = (int) std::floor (rLo);
        int r1 = (int) std::ceil (rHi) - 1;
        if (r1 < r0) { r0 = r1 = (int) std::lround ((f - f0) / rowHz); }
        for (int r = r0; r <= r1; ++r)
            setBit (c, r);
    };

    // Walk each segment between consecutive points so fast drags leave no
    // gaps: mouse events can jump several ms/Hz apart, and stamping only the
    // sampled points would leave empty cells in between. Step finely enough
    // that adjacent stamps are at most half a cell apart in time AND freq.
    for (size_t i = 0; i < pts.size (); ++i)
    {
        stamp (pts[i].first, pts[i].second);
        if (i + 1 >= pts.size ()) break;

        const double dt = pts[i + 1].first - pts[i].first;
        const double df = pts[i + 1].second - pts[i].second;
        const int steps = std::max ((int) std::ceil (std::abs (dt) / (colSpan * 0.5)),
                                   (int) std::ceil (std::abs (df) / (rowHz * 0.5)));
        for (int s = 1; s <= steps; ++s)
            stamp (pts[i].first + dt * ((double) s / (double) steps),
                   pts[i].second + df * ((double) s / (double) steps));
    }
}

// Appends a copy of one shape with all frequencies multiplied by m. Time
// coordinates are untouched; stroke radii scale too so the clone's thickness
// is stretched, not just shifted.
void SpectralSelection::appendScaledShape (const SpectralShape& sIn, double m)
{
    if (!(m > 0.0) || !std::isfinite (m)) return;
    SpectralShape c = sIn;   // deep copy
    switch (c.kind)
    {
        case SpectralShape::Kind::Rect:
            c.f0 *= m;
            c.f1 *= m;
            break;
        case SpectralShape::Kind::Polygon:
            for (auto& p : c.pts) p.second *= m;
            break;
        default:   // Stroke: re-stamp the scaled path at the scaled radius.
            for (auto& p : c.pts) p.second *= m;
            c.strokeRadiusHz = std::max (1.0, c.strokeRadiusHz * m);
            c.cols = 0; c.rows = 0; c.grid.clear ();
            c.rasteriseStroke (c.strokeRadiusHz);
            break;
    }
    shapeList.push_back (std::move (c));
}

SpectralSelection SpectralSelection::scaledCopy (double m) const
{
    SpectralSelection out;
    for (const auto& s : shapeList)
        out.appendScaledShape (s, m);
    return out;
}

void SpectralSelection::appendScaledShapesFrom (const SpectralSelection& src, double m)
{
    for (const auto& s : src.shapeList)
        appendScaledShape (s, m);
}

void SpectralSelection::addScaledClones (const std::vector<double>& multipliers)
{
    if (shapeList.empty () || multipliers.empty ()) return;
    const SpectralSelection snapshot = *this;   // clones must not clone themselves
    for (double m : multipliers)
        appendScaledShapesFrom (snapshot, m);
}

juce::Range<double> SpectralSelection::timeExtent() const
{
    if (shapeList.empty()) return { 0.0, 0.0 };
    double lo = 1e30, hi = -1e30;
    for (const auto& s : shapeList)
    {
        switch (s.kind)
        {
            case SpectralShape::Kind::Rect:
                lo = std::min (lo, std::min (s.t0, s.t1));
                hi = std::max (hi, std::max (s.t0, s.t1));
                break;
            case SpectralShape::Kind::Polygon:
                for (auto& p : s.pts)
                {
                    lo = std::min (lo, p.first);
                    hi = std::max (hi, p.first);
                }
                break;
            case SpectralShape::Kind::Stroke:
                lo = std::min (lo, s.t0);
                hi = std::max (hi, s.t1);
                break;
        }
    }
    return { lo, hi };
}

bool SpectralSelection::contains (double tSecs, double fHz) const
{
    bool sel = false;
    for (const auto& s : shapeList)
    {
        if (!s.subtract)
            sel |= s.contains (tSecs, fHz);
        else
            sel &= !s.contains (tSecs, fHz);
    }
    return sel;
}
