// SpectroCraft Mobile - sound map drawing (docs/sound-map.md).
//
// Top view: the levels of the map points in one band over an interpolated
// surface (energy mean, each point with its own width, nothing far from the
// points), the tags and the circle through them. Side view: every point at its
// distance from the bowl's axis and its height (no surface). Points the map
// should not trust are drawn apart: hollow when the band is less than 10 dB
// above the background, crossed when below 3 dB or when the point moved, was
// not checked (no position after the take), is imprecise, has no position, or
// is in another unit than most points. Tap a point to select it.
#pragma once
#include <JuceHeader.h>
#include "soundmap/SmInterp.h"
#include "soundmap/SmProject.h"

namespace scm
{
class SoundMapView : public juce::Component
{
public:
    enum class Projection { Top, Side };

    SoundMapView();

    /** The map to draw (not owned; nullptr for none). Call refresh() after changing it. */
    void setProject (const smap::SoundMapProject* p);
    /** Nominal 1/3-octave centre in Hz, 0 = LZeq, -1 = LAeq. */
    void setBand (double band);
    double band() const noexcept { return currentBand; }
    void setRelative (bool r);
    void setProjection (Projection p);
    void setSelected (int pointNumber);
    int selected() const noexcept { return selectedPoint; }
    /** Recomputes the surface (the map's data changed). */
    void refresh();

    /** A point was tapped (its number), or the background (-1). */
    std::function<void (int)> onPointTapped;

    /** The value drawn for a point (dB, absolute or relative), or nothing. */
    std::optional<double> displayedValue (const smap::MapPoint& p) const;
    /** The highest frequency whose patterns the points' spacing resolves (c / 4 spacing), 0 if unknown. */
    double resolvedUpToHz() const noexcept { return resolvedHz; }

    void paint (juce::Graphics&) override;
    void resized() override { transformValid = false; }
    void mouseUp (const juce::MouseEvent&) override;

    static juce::Colour colourFor (double t);   // 0..1 on the map's palette

private:
    struct Shown                // a copy: the project's points may move in memory
    {
        int number = 0;
        smap::PointKind kind = smap::PointKind::Point;
        bool located = false;
        smap::Vec3 position;
        double value = 0.0;
        bool hasValue = false;
        bool trusted = false;   // drawn on the surface
        bool weak = false;      // 3-10 dB above the background
    };

    void rebuild();
    void updateTransform();
    juce::Point<float> toScreen (double x, double y) const;
    void drawTop (juce::Graphics&);
    void drawSide (juce::Graphics&);
    void drawLegend (juce::Graphics&, juce::Rectangle<float> area);
    void drawPoint (juce::Graphics&, const Shown& s, juce::Point<float> at);

    const smap::SoundMapProject* project = nullptr;
    double currentBand = 0.0;
    bool relative = false;
    Projection projection = Projection::Top;
    int selectedPoint = -1;

    std::vector<Shown> shown;
    double lo = 0.0, hi = 1.0, reference = 0.0, resolvedHz = 0.0;
    juce::String unit;
    smap::Grid grid;
    juce::Image surface;

    // world extent and the transform to the drawing area
    double extent = 4.0;      // half-width of the square shown, metres
    double sideMaxR = 4.0, sideMinZ = -3.0, sideMaxZ = 1.0;
    juce::Rectangle<float> plot;
    bool transformValid = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundMapView)
};
} // namespace scm
