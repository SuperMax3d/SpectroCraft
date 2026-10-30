#pragma once
// SpectroCraft - spectral (time x frequency) selection model.
//
// A spectral selection is an ordered list of shapes in ABSOLUTE coordinates:
// time in seconds, frequency in Hz. Shapes are evaluated in draw order with
// Photoshop-style boolean semantics:
//   - a plain drag REPLACES the whole selection (clears first),
//   - SHIFT+drag ADDS to it,
//   - CTRL+drag SUBTRACTS from everything drawn before it.
// The waveform view mirrors the selection's bounding time range; effects and
// "Play Spectral Selection" use contains() to decide which (t,f) bins are in.

#include <JuceHeader.h>
#include <vector>
#include <utility>
#include <memory>

enum class SpectralSelMode { TimeRange, Marquee, Lasso, Paintbrush };

inline const char* spectralSelModeName (SpectralSelMode m)
{
    switch (m)
    {
        case SpectralSelMode::Marquee:   return "Marquee";
        case SpectralSelMode::Lasso:     return "Lasso";
        case SpectralSelMode::Paintbrush:return "Paintbrush";
        default:                         return "Time Range";
    }
}

// One region of the selection.
struct SpectralShape
{
    enum class Kind { Rect, Polygon, Stroke };
    Kind kind = Kind::Rect;
    bool subtract = false;   // drawn while holding Ctrl (removes from earlier shapes)

    // Rect: axis-aligned box in (t,f) space.
    double t0 = 0.0, t1 = 0.0, f0 = 0.0, f1 = 0.0;

    // Polygon: closed loop of (timeSecs, freqHz) points (lasso).
    std::vector<std::pair<double, double>> pts;

    // Stroke: rasterised paint path. The grid covers [t0,t1] x [f0,f1] with
    // cols x rows cells; a set bit means the brush painted that cell.
    int cols = 0, rows = 0;
    std::vector<uint8_t> grid;
    double strokeRadiusHz = 0.0;   // Stroke only: brush half-width used at stamp time

    bool contains (double tSecs, double fHz) const;
    // Tight bounding box in (timeSecs, freqHz) space: x/y = lower corner,
    // width/height = spans. Empty for degenerate shapes.
    juce::Rectangle<double> boundingBox() const;

    // Stroke only: rasterises the stored pts into cols/rows/grid at the given
    // brush radius (Hz). Needed before contains() can answer for a stroke.
    void rasteriseStroke (double radiusHz);
};

// Sample points along a straight line between two (timeSecs, freqHz) positions.
// Both time and frequency are interpolated LINEARLY, so the result is a true
// straight line in linear-frequency space. On screen it may therefore appear
// slightly curved when the view uses a logarithmic frequency axis - that
// curvature is correct: the stroke stays perfectly straight if the view is
// switched to a linear frequency scale.
inline std::vector<std::pair<double, double>> straightLinePoints (double t0, double f0,
                                                                 double t1, double f1)
{
    // Stroke rasterisation uses ~512 cols/sec and 32 Hz/row; sample at least
    // twice as fine so the stamped brush disks merge into a solid line.
    double need = 8.0;
    const double dtSpan = std::abs (t1 - t0) * 1024.0;
    if (dtSpan > need) need = dtSpan;
    const double dfSpan = std::abs (f1 - f0) / 8.0;
    if (dfSpan > need) need = dfSpan;
    int n = (int) std::ceil (need);
    if (n > 8192) n = 8192;

    std::vector<std::pair<double, double>> pts;
    pts.reserve ((size_t) n + 1);
    for (int i = 0; i <= n; ++i)
    {
        const double u = (double) i / (double) n;
        pts.emplace_back (t0 + (t1 - t0) * u, f0 + (f1 - f0) * u);
    }
    return pts;
}

class SpectralSelection
{
public:
    void clear();
    bool empty() const { return shapeList.empty(); }
    int count() const { return (int) shapeList.size(); }
    const std::vector<SpectralShape>& shapes() const { return shapeList; }

    // Add a rectangular region. replace = true clears the existing selection
    // first (plain drag); pass false to add on top of it (SHIFT+drag).
    void addRect (double t0, double t1, double f0, double f1,
                  bool subtract, bool replace = true);

    // Add a closed polygon (lasso). Same replace/subtract semantics.
    void addPolygon (std::vector<std::pair<double, double>> pts,
                     bool subtract, bool replace = true);

    // Add a paintbrush stroke: the path is rasterised into a grid at commit
    // time. radiusHz is the brush half-width in frequency; each point paints
    // a vertical band of that width at its time position.
    void addStroke (std::vector<std::pair<double, double>> pts,
                    double radiusHz, bool subtract, bool replace = true);

    /** Returns a copy of this selection with every frequency multiplied by m
        (time coordinates unchanged). Stroke brush radii are scaled too, so a
        clone's thickness is stretched proportionally - not just offset.
        Each shape keeps its additive/subtractive flag. */
    SpectralSelection scaledCopy (double m) const;

    /** Returns a copy translated by dt seconds and df Hz (every shape).
        Stroke grids are untouched: they are relative to the shape's own
        t0/f0 origin, which moves with them. Non-finite deltas leave the
        selection unchanged. */
    SpectralSelection translatedCopy (double dt, double df) const;

    /** Returns a copy scaled about the anchor point (anchorT, anchorF): time
        coordinates by st, frequency coordinates by sf. Stroke brush radii
        scale with sf so thickness stretches proportionally. Non-positive or
        non-finite factors leave the selection unchanged. */
    SpectralSelection scaledAboutCopy (double anchorT, double anchorF,
                                       double st, double sf) const;

    // Bounding box of all shapes in (timeSecs, freqHz) space: x/y = lower
    // corner, width/height = spans. Empty when the selection is empty.
    juce::Rectangle<double> boundingBox() const;

    /** Appends clones of every CURRENT shape at each given frequency
        multiplier (the original shapes stay in place, so the result contains
        the original pattern plus one scaled copy per multiplier). Clones are
        built from a snapshot, so they never clone themselves. */
    void addScaledClones (const std::vector<double>& multipliers);

    /** Appends frequency-scaled copies of every shape in `src` at multiplier m.
        Used by the interactive pick mode, which always clones the selection
        as it was when picking started - never its own previous clones. */
    void appendScaledShapesFrom (const SpectralSelection& src, double m);

    /** Appends deep copies of every shape in `src` after this selection's own
        shapes, preserving each shape's additive/subtractive flag and order.
        Used to merge several stored selections into one live selection. */
    void appendShapesFrom (const SpectralSelection& src);

    // Remove the shapes [start, start+count) from the list (no-op when out of
    // range). Used to undo a Clone & Multiply pick point's appended clones.
    void removeShapesAt (int start, int count);

    // Bounding time range of all shapes (a safe superset for subtractive
    // shapes). Empty when the selection is empty.
    juce::Range<double> timeExtent() const;

    // Adjust this selection after [a, b] seconds have been removed from the
    // file: shapes entirely before are unchanged; those entirely after shift
    // left by the deleted length; shapes straddling an edge of the deletion
    // are trimmed (rects) or split into up to two pieces (polygons/strokes);
    // fully contained shapes are dropped. Returns true when anything changed.
    bool adjustForDeletion (double a, double b);

    /** Adjust this selection after [a, bOld] seconds has been stretched to
        [a, bNew]: every time coordinate is warped through W(t) = t for
        t < a, a + (t-a)*(bNew-a)/(bOld-a) inside the range, and
        t + (bNew-bOld) after it. Stroke grids are resampled column-wise.
        The frequency axis is untouched. Returns true when anything changed. */
    bool adjustForStretch (double a, double bOld, double bNew);

    /** Adjust this selection after len seconds have been inserted at time a:
        shapes entirely before are unchanged; those entirely after shift right
        by len; shapes straddling the insertion point are split into up to two
        pieces (rects) or clipped and shifted (polygons/strokes). The frequency
        axis is untouched. Returns true when anything changed. */
    bool adjustForInsertion (double a, double len);

    /** Drops every shape entirely outside [lo, hi] and trims the remaining
        shapes to that range (the frequency axis is untouched). Used after a
        whole-file replacement keeps only part of the old timeline. Returns
        true when anything changed. */
    bool clampToTimeRange (double lo, double hi);

    // True if the point (tSecs, fHz) lies inside the final boolean result.
    bool contains (double tSecs, double fHz) const;

private:
    // Appends a frequency-scaled copy of one shape to this selection.
    void appendScaledShape (const SpectralShape& sIn, double m);

    std::vector<SpectralShape> shapeList;
};
