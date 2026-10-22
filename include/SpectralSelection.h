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

    bool contains (double tSecs, double fHz) const;
    // Tight bounding box in (timeSecs, freqHz) space: x/y = lower corner,
    // width/height = spans. Empty for degenerate shapes.
    juce::Rectangle<double> boundingBox() const;

    // Stroke only: rasterises the stored pts into cols/rows/grid at the given
    // brush radius (Hz). Needed before contains() can answer for a stroke.
    void rasteriseStroke (double radiusHz);
};

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

    // Bounding time range of all shapes (a safe superset for subtractive
    // shapes). Empty when the selection is empty.
    juce::Range<double> timeExtent() const;

    // True if the point (tSecs, fHz) lies inside the final boolean result.
    bool contains (double tSecs, double fHz) const;

private:
    std::vector<SpectralShape> shapeList;
};
