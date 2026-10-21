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

// Rasterises a paint path into the shape's grid (defined below).
static void rasteriseStrokeFromPoints (SpectralShape& out,
                                       const std::vector<std::pair<double, double>>& pts,
                                       double radiusHz);

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
    std::vector<std::pair<double, double>> pts;
    for (auto& p : ptsIn)
        if (pts.empty()
            || std::abs (p.first - pts.back().first) >= 1e-3
            || std::abs (p.second - pts.back().second) >= 8.0)
            pts.push_back (p);
    if (pts.empty()) return;

    if (replace && !subtract)
        shapeList.clear();

    SpectralShape s;
    s.kind = SpectralShape::Kind::Stroke;
    s.subtract = subtract;
    s.pts = pts;   // kept for rendering (the grid is only used by contains())
    rasteriseStrokeFromPoints (s, pts, std::max (1.0, radiusHz));
    shapeList.push_back (std::move (s));
}

// Rasterises a paint path into the shape's grid. Resolution is fixed in
// absolute units (~512 columns/second, ~32 Hz/row) and capped at 4096x4096
// cells so even hour-long strokes stay small (<= 2 MB).
static void rasteriseStrokeFromPoints (SpectralShape& out,
                                       const std::vector<std::pair<double, double>>& pts,
                                       double radiusHz)
{
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
    out.cols = (int) std::min <double> (4096.0, std::ceil (tSpan / colSpan));
    out.rows = (int) std::min <double> (4096.0, std::ceil (fSpan / rowHz));
    if (out.cols <= 0 || out.rows <= 0) return;

    // The grid covers the path plus one brush radius of margin on every side.
    out.t0 = tMin - colSpan * 0.5;
    out.t1 = out.t0 + out.cols * colSpan;
    out.f0 = fMin - radiusHz - rowHz * 0.5;
    out.f1 = out.f0 + out.rows * rowHz;

    const size_t nBits = (size_t) out.cols * (size_t) out.rows;
    out.grid.assign ((nBits + 7) >> 3, 0);

    auto setBit = [&] (int c, int r) {
        if (c < 0 || c >= out.cols || r < 0 || r >= out.rows) return;
        const size_t bit = (size_t) c * (size_t) out.rows + (size_t) r;
        out.grid[bit >> 3] |= (uint8_t) (1u << (bit & 7));
    };

    for (auto& p : pts)
    {
        const int c = (int) std::floor ((p.first - out.t0) / colSpan);
        // Rows whose centre lies within the brush radius of the point's freq.
        double rLo = (p.second - radiusHz - 0.5 * rowHz - out.f0) / rowHz;
        double rHi = (p.second + radiusHz - 0.5 * rowHz - out.f0) / rowHz;
        int r0 = (int) std::floor (rLo);
        int r1 = (int) std::ceil (rHi) - 1;
        if (r1 < r0) { r0 = r1 = (int) std::lround ((p.second - out.f0) / rowHz); }
        for (int r = r0; r <= r1; ++r)
            setBit (c, r);
    }
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
