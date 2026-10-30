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

void SpectralSelection::appendShapesFrom (const SpectralSelection& src)
{
    for (const auto& s : src.shapeList)
        shapeList.push_back (s);   // deep copy of each shape
}

void SpectralSelection::removeShapesAt (int start, int count)
{
    if (start < 0 || count <= 0 || start >= (int) shapeList.size ()) return;
    const int end = std::min ((int) shapeList.size (), start + count);
    shapeList.erase (shapeList.begin () + start, shapeList.begin () + end);
}

SpectralSelection SpectralSelection::translatedCopy (double dt, double df) const
{
    if (!std::isfinite (dt) || !std::isfinite (df)) return *this;

    SpectralSelection out;
    for (const auto& s : shapeList)
    {
        SpectralShape c = s;   // deep copy
        switch (c.kind)
        {
            case SpectralShape::Kind::Rect:
                c.t0 += dt;  c.t1 += dt;
                c.f0 += df;  c.f1 += df;
                break;
            case SpectralShape::Kind::Polygon:
                for (auto& p : c.pts) { p.first += dt; p.second += df; }
                break;
            default:   // Stroke: the grid is relative to t0/f0, so shifting the
                      // origin keeps every cell aligned with its content.
                for (auto& p : c.pts) { p.first += dt; p.second += df; }
                c.t0 += dt;  c.t1 += dt;
                c.f0 += df;  c.f1 += df;
                break;
        }
        out.shapeList.push_back (std::move (c));
    }
    return out;
}

SpectralSelection SpectralSelection::scaledAboutCopy (double anchorT, double anchorF,
                                                     double st, double sf) const
{
    if (!(st > 0.0) || !(sf > 0.0) || !std::isfinite (st) || !std::isfinite (sf))
        return *this;

    SpectralSelection out;
    for (const auto& s : shapeList)
    {
        SpectralShape c = s;   // deep copy
        switch (c.kind)
        {
            case SpectralShape::Kind::Rect:
                c.t0 = anchorT + (c.t0 - anchorT) * st;
                c.t1 = anchorT + (c.t1 - anchorT) * st;
                c.f0 = anchorF + (c.f0 - anchorF) * sf;
                c.f1 = anchorF + (c.f1 - anchorF) * sf;
                break;
            case SpectralShape::Kind::Polygon:
                for (auto& p : c.pts)
                {
                    p.first  = anchorT + (p.first  - anchorT) * st;
                    p.second = anchorF + (p.second - anchorF) * sf;
                }
                break;
            default:   // Stroke: re-stamp the scaled path at the scaled radius.
                for (auto& p : c.pts)
                {
                    p.first  = anchorT + (p.first  - anchorT) * st;
                    p.second = anchorF + (p.second - anchorF) * sf;
                }
                c.strokeRadiusHz = std::max (1.0, c.strokeRadiusHz * sf);
                c.cols = 0; c.rows = 0; c.grid.clear ();
                c.rasteriseStroke (c.strokeRadiusHz);
                break;
        }
        out.shapeList.push_back (std::move (c));
    }
    return out;
}

juce::Rectangle<double> SpectralSelection::boundingBox() const
{
    if (shapeList.empty()) return {};
    double loT = 1e30, hiT = -1e30, loF = 1e30, hiF = -1e30;
    for (const auto& s : shapeList)
    {
        switch (s.kind)
        {
            case SpectralShape::Kind::Rect:
                loT = std::min (loT, std::min (s.t0, s.t1));
                hiT = std::max (hiT, std::max (s.t0, s.t1));
                loF = std::min (loF, std::min (s.f0, s.f1));
                hiF = std::max (hiF, std::max (s.f0, s.f1));
                break;
            case SpectralShape::Kind::Polygon:
                for (const auto& p : s.pts)
                {
                    loT = std::min (loT, p.first);  hiT = std::max (hiT, p.first);
                    loF = std::min (loF, p.second); hiF = std::max (hiF, p.second);
                }
                break;
            case SpectralShape::Kind::Stroke:
                loT = std::min (loT, s.t0);  hiT = std::max (hiT, s.t1);
                loF = std::min (loF, s.f0);  hiF = std::max (hiF, s.f1);
                break;
        }
    }
    return { loT, loF, hiT - loT, hiF - loF };
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

// ---------------------------------------------------------------------------
// Time-range deletion adjustment (markers/snapshots are handled elsewhere;
// this keeps spectral selections consistent when samples are cut out).
// ---------------------------------------------------------------------------

namespace {

// Sutherland-Hodgman clip of a polygon against the half-plane t <= c.
std::vector<std::pair<double, double>> clipPolyToTimeAtMost (const std::vector<std::pair<double, double>>& poly, double c)
{
    std::vector<std::pair<double, double>> out;
    const size_t n = poly.size();
    if (n < 3) return out;

    for (size_t i = 0; i < n; ++i)
    {
        const auto& S = poly[i];
        const auto& E = poly[(i + 1) % n];
        const bool sIn = S.first <= c, eIn = E.first <= c;
        if (sIn && eIn)
            out.push_back (E);
        else if (sIn != eIn)
        {
            double f;
            if (std::abs (E.first - S.first) < 1e-12)
                f = 0.5 * (S.second + E.second);
            else
                f = S.second + (E.second - S.second) * (c - S.first) / (E.first - S.first);
            out.emplace_back (c, f);
            if (!sIn && eIn)
                out.push_back (E);
        }
    }
    return out;
}

// Mirror of the above: keep t >= c.
std::vector<std::pair<double, double>> clipPolyToTimeAtLeast (const std::vector<std::pair<double, double>>& poly, double c)
{
    std::vector<std::pair<double, double>> out;
    const size_t n = poly.size();
    if (n < 3) return out;

    for (size_t i = 0; i < n; ++i)
    {
        const auto& S = poly[i];
        const auto& E = poly[(i + 1) % n];
        const bool sIn = S.first >= c, eIn = E.first >= c;
        if (sIn && eIn)
            out.push_back (E);
        else if (sIn != eIn)
        {
            double f;
            if (std::abs (E.first - S.first) < 1e-12)
                f = 0.5 * (S.second + E.second);
            else
                f = S.second + (E.second - S.second) * (c - S.first) / (E.first - S.first);
            out.emplace_back (c, f);
            if (!sIn && eIn)
                out.push_back (E);
        }
    }
    return out;
}

// Extract columns [c0, c1) of a stroke grid (column-major bit layout:
// bit index = col * rows + row).
std::vector<uint8_t> cropStrokeColumns (const SpectralShape& s, int c0, int c1)
{
    const int colsNew = c1 - c0;
    std::vector<uint8_t> g ((size_t) (colsNew * s.rows + 7) / 8, 0);
    for (int c = c0; c < c1; ++c)
        for (int r = 0; r < s.rows; ++r)
        {
            const size_t srcBit = (size_t) c * s.rows + (size_t) r;
            if ((s.grid[srcBit >> 3] & (1u << (srcBit & 7))) != 0)
            {
                const size_t dstBit = (size_t) (c - c0) * s.rows + (size_t) r;
                g[dstBit >> 3] |= (uint8_t) (1u << (dstBit & 7));
            }
        }
    return g;
}

// Apply the deletion of [a, b] seconds to one shape. Returns replacement
// shapes: one for unchanged/shifted/trimmed shapes, up to two pieces when a
// polygon or stroke straddles the deleted range, empty when dropped.
std::vector<SpectralShape> adjustShapeForDeletion (const SpectralShape& sIn, double a, double b)
{
    const double len = b - a;
    std::vector<SpectralShape> out;

    switch (sIn.kind)
    {
        case SpectralShape::Kind::Rect:
        {
            SpectralShape r = sIn;
            if (r.t1 <= a)
                ;                                   // entirely before: unchanged
            else if (r.t0 >= b)
            { r.t0 -= len; r.t1 -= len; }          // entirely after: shift left
            else if (r.t0 < a && r.t1 > b)
                r.t1 -= len;                       // straddles the whole deletion:
                                                   // both halves stay contiguous -> [t0, t1-len]
            else if (r.t0 < a)
                r.t1 = a;                          // right part deleted -> [t0, a]
            else if (r.t1 <= b)
                return out;                        // fully contained: dropped
            else
            { r.t0 = a; r.t1 -= len; }             // left part deleted: old [b,t1] -> new [a, t1-len]

            if (r.t1 - r.t0 > 1e-9)
                out.push_back (std::move (r));
            break;
        }

        case SpectralShape::Kind::Polygon:
        {
            double minT = 1e300, maxT = -1e300;
            for (const auto& p : sIn.pts)
            {
                if (p.first < minT) minT = p.first;
                if (p.first > maxT) maxT = p.first;
            }

            if (sIn.pts.size() < 3 || maxT <= a)   // degenerate or entirely before: unchanged
            { out.push_back (sIn); break; }

            if (minT >= b)                         // entirely after: shift left
            {
                SpectralShape r = sIn;
                for (auto& p : r.pts) p.first -= len;
                out.push_back (std::move (r));
                break;
            }

            // Straddles the deleted range: keep the part before a and the part
            // after b (shifted left by len); each piece keeps the subtract flag.
            const auto left = clipPolyToTimeAtMost (sIn.pts, a);
            if (left.size() >= 3)
            {
                SpectralShape r = sIn;
                r.pts = std::move (left);
                out.push_back (std::move (r));
            }

            const auto right = clipPolyToTimeAtLeast (sIn.pts, b);
            if (right.size() >= 3)
            {
                SpectralShape r = sIn;
                r.pts = std::move (right);
                for (auto& p : r.pts) p.first -= len;
                out.push_back (std::move (r));
            }
            break;
        }

        case SpectralShape::Kind::Stroke:
        {
            if (sIn.cols <= 0 || sIn.rows <= 0)    // degenerate grid: unchanged
            { out.push_back (sIn); break; }

            const double dt = std::max (1e-9, (sIn.t1 - sIn.t0) / (double) sIn.cols);

            if (sIn.t1 <= a)                       // entirely before: unchanged
            { out.push_back (sIn); break; }

            if (sIn.t0 >= b)                       // entirely after: shift left
            {
                SpectralShape r = sIn;
                r.t0 -= len; r.t1 -= len;
                out.push_back (std::move (r));
                break;
            }

            if (sIn.t0 >= a && sIn.t1 <= b)        // fully contained: dropped
                return out;

            // Left piece [t0, a] when the stroke starts before the deletion.
            if (sIn.t0 < a)
            {
                int cA = (int) std::lround ((a - sIn.t0) / dt);
                cA = std::clamp (cA, 1, sIn.cols);
                SpectralShape r = sIn;
                r.grid = cropStrokeColumns (sIn, 0, cA);
                r.cols = cA;
                r.t1 = a;
                out.push_back (std::move (r));
            }

            // Right piece: old [b, t1] -> new [a, t1 - len].
            if (sIn.t1 > b)
            {
                int cB = (int) std::lround ((b - sIn.t0) / dt);
                cB = std::clamp (cB, 0, sIn.cols - 1);
                SpectralShape r = sIn;
                r.grid = cropStrokeColumns (sIn, cB, sIn.cols);
                r.cols = sIn.cols - cB;
                r.t0 = a;
                r.t1 -= len;
                if (r.t1 - r.t0 > 1e-9)
                    out.push_back (std::move (r));
            }
            break;
        }
    }
    return out;
}

bool shapesEqualForDeletionCheck (const SpectralShape& s, const SpectralShape& r)
{
    switch (s.kind)
    {
        case SpectralShape::Kind::Rect:
            return r.t0 == s.t0 && r.t1 == s.t1;
        case SpectralShape::Kind::Polygon:
            return r.pts == s.pts;
        default:
            return r.t0 == s.t0 && r.t1 == s.t1 && r.cols == s.cols && r.grid == s.grid;
    }
}

// Time-range insertion adjustment: content before `a` is unchanged and from
// `a` on it shifts right by `len`; shapes straddling the insertion point are
// split into up to two pieces.
std::vector<SpectralShape> adjustShapeForInsertion (const SpectralShape& sIn, double a, double len)
{
    std::vector<SpectralShape> out;

    switch (sIn.kind)
    {
        case SpectralShape::Kind::Rect:
        {
            if (sIn.t1 <= a)
                out.push_back (sIn);                 // entirely before: unchanged
            else if (sIn.t0 >= a)
            {
                SpectralShape r = sIn;
                r.t0 += len; r.t1 += len;            // entirely after: shift right
                out.push_back (std::move (r));
            }
            else                                    // straddles the insertion point:
                                                   // split into two rects
            {
                SpectralShape left = sIn, right = sIn;
                left.t1 = a;                 // left piece:  [t0, a]
                right.t0 = a + len;          // right piece: [a, t1] -> [a+len, t1+len]
                right.t1 += len;
                if (a - left.t0 > 1e-9)
                    out.push_back (std::move (left));
                if (right.t1 - right.t0 > 1e-9)
                    out.push_back (std::move (right));
            }
            break;
        }

        case SpectralShape::Kind::Polygon:
        {
            double minT = 1e300, maxT = -1e300;
            for (const auto& p : sIn.pts)
            {
                if (p.first < minT) minT = p.first;
                if (p.first > maxT) maxT = p.first;
            }

            if (sIn.pts.size() < 3 || maxT <= a)   // degenerate or entirely before: unchanged
            { out.push_back (sIn); break; }

            if (minT >= a)                         // entirely after: shift right
            {
                SpectralShape r = sIn;
                for (auto& p : r.pts) p.first += len;
                out.push_back (std::move (r));
                break;
            }

            // Straddles the insertion point: keep the part before `a` as-is and
            // shift the rest right by len.
            const auto left = clipPolyToTimeAtMost (sIn.pts, a);
            if (left.size() >= 3)
            {
                SpectralShape r = sIn;
                r.pts = std::move (left);
                out.push_back (std::move (r));
            }

            const auto right = clipPolyToTimeAtLeast (sIn.pts, a);
            if (right.size() >= 3)
            {
                SpectralShape r = sIn;
                r.pts = std::move (right);
                for (auto& p : r.pts) p.first += len;
                out.push_back (std::move (r));
            }
            break;
        }

        case SpectralShape::Kind::Stroke:
        {
            if (sIn.cols <= 0 || sIn.rows <= 0)    // degenerate grid: unchanged
            { out.push_back (sIn); break; }

            const double dt = std::max (1e-9, (sIn.t1 - sIn.t0) / (double) sIn.cols);

            if (sIn.t1 <= a)                       // entirely before: unchanged
            { out.push_back (sIn); break; }

            if (sIn.t0 >= a)                       // entirely after: shift right
            {
                SpectralShape r = sIn;
                r.t0 += len; r.t1 += len;
                out.push_back (std::move (r));
                break;
            }

            // Left piece [t0, a].
            {
                int cA = (int) std::lround ((a - sIn.t0) / dt);
                cA = std::clamp (cA, 1, sIn.cols);
                SpectralShape r = sIn;
                r.grid = cropStrokeColumns (sIn, 0, cA);
                r.cols = cA;
                r.t1 = a;
                out.push_back (std::move (r));
            }

            // Right piece: old [a, t1] -> new [a+len, t1+len].
            {
                int cB = (int) std::lround ((a - sIn.t0) / dt);
                cB = std::clamp (cB, 0, sIn.cols - 1);
                SpectralShape r = sIn;
                r.grid = cropStrokeColumns (sIn, cB, sIn.cols);
                r.cols = sIn.cols - cB;
                r.t0 = a + len;
                r.t1 += len;
                if (r.t1 - r.t0 > 1e-9)
                    out.push_back (std::move (r));
            }
            break;
        }
    }
    return out;
}

} // anonymous namespace

bool SpectralSelection::adjustForDeletion (double a, double b)
{
    if (!(b > a))
        return false;

    std::vector<SpectralShape> newList;
    bool changed = false;

    for (const auto& s : shapeList)
    {
        const std::vector<SpectralShape> repl = adjustShapeForDeletion (s, a, b);
        if (repl.empty())
        { changed = true; continue; }             // dropped entirely

        bool same = false;
        if (repl.size() == 1)
            same = shapesEqualForDeletionCheck (s, repl[0]);
        if (!same)
            changed = true;

        for (auto& r : repl)
            newList.push_back (std::move (r));
    }

    if (changed)
        shapeList = std::move (newList);
    return changed;
}

bool SpectralSelection::adjustForStretch (double a, double bOld, double bNew)
{
    if (!(bOld > a))
        return false;
    const double k = (bNew - a) / (bOld - a);
    auto W = [a, bOld, bNew, k] (double t) -> double
    {
        if (t < a) return t;
        if (t <= bOld) return a + (t - a) * k;
        return t + (bNew - bOld);
    };

    bool changed = false;
    for (auto& s : shapeList)
    {
        switch (s.kind)
        {
            case SpectralShape::Kind::Rect:
            {
                const double lo = std::min (s.t0, s.t1);
                const double hi = std::max (s.t0, s.t1);
                const double nlo = W (lo), nhi = W (hi);
                if (nlo != lo || nhi != hi)
                {
                    s.t0 = nlo;
                    s.t1 = nhi;
                    changed = true;
                }
                break;   // frequency axis is untouched
            }

            case SpectralShape::Kind::Polygon:
                for (auto& p : s.pts)
                    if (p.first != W (p.first))
                    {
                        p.first = W (p.first);
                        changed = true;
                    }
                break;

            case SpectralShape::Kind::Stroke:
            {
                const double nt0 = W (s.t0), nt1 = W (s.t1);
                if (nt0 == s.t0 && nt1 == s.t1)
                    break;   // entirely outside the stretched range: unchanged
                if (s.cols <= 0 || s.rows <= 0 || s.grid.empty())
                {
                    s.t0 = nt0;
                    s.t1 = nt1;
                    changed = true;
                    break;
                }

                // Nearest-neighbour column resampling: each output column maps
                // back through the inverse warp to a source column. Rows are
                // unchanged (the frequency axis is unaffected).
                const double oldSpan = std::max (1e-9, s.t1 - s.t0);
                const double newSpan = std::max (1e-9, nt1 - nt0);
                const double shiftAfter = bNew - bOld;
                auto Winv = [a, bOld, k, shiftAfter] (double tp) -> double
                {
                    if (tp < a) return tp;
                    if (tp <= a + (bOld - a) * k) return a + (tp - a) / k;
                    return tp - shiftAfter;
                };

                std::vector<uint8_t> g ((size_t) (s.cols * s.rows + 7) / 8, 0);
                for (int c = 0; c < s.cols; ++c)
                {
                    const double tc = nt0 + ((double) c + 0.5) * newSpan / (double) s.cols;
                    int cs = (int) std::floor (((Winv (tc) - s.t0) / oldSpan) * (double) s.cols);
                    cs = jlimit (0, s.cols - 1, cs);
                    for (int r = 0; r < s.rows; ++r)
                    {
                        const size_t srcBit = (size_t) cs * s.rows + (size_t) r;
                        if ((s.grid[srcBit >> 3] & (1u << (srcBit & 7))) != 0)
                        {
                            const size_t dstBit = (size_t) c * s.rows + (size_t) r;
                            g[dstBit >> 3] |= (uint8_t) (1u << (dstBit & 7));
                        }
                    }
                }

                s.t0 = nt0;
                s.t1 = nt1;
                s.grid = std::move (g);
                changed = true;
                break;
            }
        }
    }
    return changed;
}

bool SpectralSelection::adjustForInsertion (double a, double len)
{
    if (!(len > 0.0))
        return false;

    std::vector<SpectralShape> newList;
    bool changed = false;

    for (const auto& s : shapeList)
    {
        const std::vector<SpectralShape> repl = adjustShapeForInsertion (s, a, len);
        if (repl.size () != 1 || !shapesEqualForDeletionCheck (s, repl[0]))
            changed = true;

        for (auto& r : repl)
            newList.push_back (std::move (r));
    }

    if (changed)
        shapeList = std::move (newList);
    return changed;
}

bool SpectralSelection::clampToTimeRange (double lo, double hi)
{
    if (!(hi > lo))
        return false;   // empty range: nothing can survive - leave as-is

    auto timeExtentOf = [] (const SpectralShape& s) -> std::pair<double, double>
    {
        double mn = 1e300, mx = -1e300;
        switch (s.kind)
        {
            case SpectralShape::Kind::Rect:
                mn = std::min (s.t0, s.t1);
                mx = std::max (s.t0, s.t1);
                break;
            case SpectralShape::Kind::Polygon:
                for (const auto& p : s.pts)
                {
                    if (p.first < mn) mn = p.first;
                    if (p.first > mx) mx = p.first;
                }
                break;
            default:   // Stroke
                mn = std::min (s.t0, s.t1);
                mx = std::max (s.t0, s.t1);
                break;
        }
        return { mn, mx };
    };

    std::vector<SpectralShape> newList;
    bool changed = false;

    for (const auto& s : shapeList)
    {
        const auto [mnT, mxT] = timeExtentOf (s);
        if (mxT <= lo || mnT >= hi)
        { changed = true; continue; }             // entirely outside: dropped

        SpectralShape r = s;
        switch (r.kind)
        {
            case SpectralShape::Kind::Rect:
                r.t0 = std::clamp (std::min (s.t0, s.t1), lo, hi);
                r.t1 = std::clamp (std::max (s.t0, s.t1), lo, hi);
                if (r.t1 - r.t0 <= 1e-9)
                    { changed = true; continue; } // degenerate after the trim
                break;

            case SpectralShape::Kind::Polygon:
            {
                auto pts = clipPolyToTimeAtMost (s.pts, hi);
                if (pts.size() >= 3)
                    pts = clipPolyToTimeAtLeast (pts, lo);
                if (pts.size() < 3)
                    { changed = true; continue; }
                r.pts = std::move (pts);
                break;
            }

            case SpectralShape::Kind::Stroke:
            {
                const double dt = std::max (1e-9, (s.t1 - s.t0) / (double) s.cols);
                int cLo = (int) std::lround ((lo - s.t0) / dt);
                int cHi = (int) std::lround ((hi - s.t0) / dt);
                cLo = std::clamp (cLo, 0, s.cols);
                cHi = std::clamp (cHi, cLo, s.cols);
                if (cHi <= cLo)
                    { changed = true; continue; }
                r.grid = cropStrokeColumns (s, cLo, cHi);
                r.cols = cHi - cLo;
                r.t0 = std::max (std::min (s.t0, s.t1), lo);
                r.t1 = std::min (std::max (s.t0, s.t1), hi);
                break;
            }
        }

        if (!shapesEqualForDeletionCheck (s, r))
            changed = true;
        newList.push_back (std::move (r));
    }

    if (changed)
        shapeList = std::move (newList);
    return changed;
}
