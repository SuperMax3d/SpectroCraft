// SpectroCraft - user drawings on the spectrogram (Drawing panel).
#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <vector>
#include <utility>

/** One user drawing placed over the spectrogram: text, arrow, rectangle or
    freehand. Geometry is stored in data coordinates (time seconds x
    frequency Hz) so items stay anchored to the spectrum when zooming and
    panning; they are drawn on every visible channel row, like spectral
    selection shapes. */
struct DrawingItem
{
    enum class Kind : int { Text = 0, Arrow = 1, Rectangle = 2, Freehand = 3 };

    Kind kind = Kind::Text;
    juce::String text;      // Text items only (empty for the other kinds)
    juce::String comment;   // per-item note shown in the Drawing panel list

    int colourARGB = 0xFF00FF00;  // ink/stroke colour (AARRGGBB)
    float widthPx = 2.0f;         // line width for Arrow / Rectangle / Freehand
    float textSize = 14.0f;       // font size for Text items
    bool hidden = false;          // per-item visibility toggle (Drawing panel eye icon)

    // Bounding box in data coordinates, always t0 <= t1 and f0 <= f1.
    double t0 = 0.0, f0 = 0.0, t1 = 0.0, f1 = 0.0;
    // Path points (time, frequency): the freehand stroke for Freehand items
    // and the two endpoints (start -> end) for Arrow items. Empty otherwise.
    std::vector<std::pair<double, double>> points;

    juce::Colour colour() const
    {
        // This JUCE build has no Colour(int argb) constructor / fromARGB.
        return juce::Colour::fromRGBA ((juce::uint8) ((colourARGB >> 16) & 0xFF),
                                       (juce::uint8) ((colourARGB >> 8) & 0xFF),
                                       (juce::uint8) (colourARGB & 0xFF),
                                       (juce::uint8) ((colourARGB >> 24) & 0xFF));
    }

    static juce::String kindName (Kind k)
    {
        switch (k)
        {
            case Kind::Text:      return "Text";
            case Kind::Arrow:     return "Arrow";
            case Kind::Rectangle: return "Rectangle";
            default:              return "Free Drawing";
        }
    }

    static Kind kindFromInt (int v)
    {
        switch (v)
        {
            case 1:  return Kind::Arrow;
            case 2:  return Kind::Rectangle;
            case 3:  return Kind::Freehand;
            default: return Kind::Text;
        }
    }

    /** Recomputes t0/f0/t1/f1 from the item's geometry so the box always
        matches its content (points for Freehand/Arrow, anchor + extent for
        Text and Rectangle). */
    void normaliseBounds()
    {
        if (!points.empty())
        {
            double a = points[0].first, b = points[0].first;
            double c = points[0].second, d = points[0].second;
            for (const auto& p : points)
            {
                a = std::min (a, p.first);  b = std::max (b, p.first);
                c = std::min (c, p.second); d = std::max (d, p.second);
            }
            t0 = a; t1 = b; f0 = c; f1 = d;
        }
        if (t0 > t1) std::swap (t0, t1);
        if (f0 > f1) std::swap (f0, f1);
    }

    /** Expands [t0,t1] x [f0,f1] by 10% of each span on every side (zero
        spans stay as they are), clamped to the file's time range and Nyquist
        frequency. Used when fitting selected drawings into view. */
    static void expandBounds10 (double& t0, double& f0, double& t1, double& f1,
                                double durationSecs, double maxFreqHz)
    {
        if (t0 > t1) std::swap (t0, t1);
        if (f0 > f1) std::swap (f0, f1);
        const double mt = 0.1 * (t1 - t0);
        const double mf = 0.1 * (f1 - f0);
        t0 = std::max (0.0, t0 - mt);
        t1 = std::min (durationSecs > 0.0 ? durationSecs : t1, t1 + mt);
        f0 = std::max (0.0, f0 - mf);
        f1 = std::min (maxFreqHz > 0.0 ? maxFreqHz : f1, f1 + mf);
    }
};
