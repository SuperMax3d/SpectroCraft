// SpectroCraft Mobile - the custom noise's response, drawn with a finger
// (issue #79): one point per third octave from 20 Hz to 20 kHz
// (GeneratorSettings::customBandHz). A finger sets the points it passes over;
// a double tap makes the response flat. Only the shape matters: the generator
// keeps the noise at its RMS level.
#pragma once
#include <JuceHeader.h>
#include "MobileStyle.h"
#include "SignalSource.h"
#include <cmath>
#include <functional>
#include <vector>

namespace scm
{
class NoiseCurveEditor : public juce::Component
{
public:
    static constexpr float kMinDb = -36.0f, kMaxDb = 24.0f;

    NoiseCurveEditor()
    {
        setViewportIgnoreDragFlag (true);   // drawing never scrolls the panel
        setTitle ("Custom noise response");
        values.assign ((size_t) GeneratorSettings::kCustomBands, 0.0f);
    }

    void setCurve (const std::vector<float>& c)
    {
        if ((int) c.size() == GeneratorSettings::kCustomBands) values = c;
        repaint();
    }
    const std::vector<float>& curve() const noexcept { return values; }
    /** The response changed (while drawing, and at the end). */
    std::function<void()> onChange;

    void paint (juce::Graphics& g) override
    {
        const auto plot = plotArea();
        g.setColour (style::plotBackground);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
        g.setFont (style::font (11.0f));
        for (float db = kMaxDb; db >= kMinDb; db -= 12.0f)
        {
            const float y = dbToY (db);
            g.setColour (db == 0.0f ? style::grid.brighter (0.3f) : style::grid);
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (style::axisLabel);
            g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (0.0f, y - 7.0f, plot.getX() - 4.0f, 14.0f),
                        juce::Justification::centredRight);
        }
        for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = freqToX (f);
            g.setColour (style::grid);
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
            g.setColour (style::axisLabel);
            g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "k" : juce::String ((int) f),
                        juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 1.0f, 40.0f, 13.0f), juce::Justification::centred);
        }
        g.saveState();
        g.reduceClipRegion (plot.expanded (3.0f).toNearestInt());
        juce::Path p;
        for (int i = 0; i < GeneratorSettings::kCustomBands; ++i)
        {
            const juce::Point<float> pt (freqToX (GeneratorSettings::customBandHz (i)), dbToY (values[(size_t) i]));
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        g.setColour (style::waveform);
        g.strokePath (p, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        for (int i = 0; i < GeneratorSettings::kCustomBands; ++i)
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ freqToX (GeneratorSettings::customBandHz (i)),
                                                                             dbToY (values[(size_t) i]) }));
        g.restoreState();
        g.setColour (style::textDim);
        g.setFont (style::font (11.0f));
        g.drawText ("Draw with a finger - double tap: flat", plot.reduced (4.0f, 2.0f), juce::Justification::topRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.getNumberOfClicks() >= 2)
        {
            std::fill (values.begin(), values.end(), 0.0f);
            last = -1;
            repaint();
            if (onChange) onChange();
            return;
        }
        last = -1;
        drawAt (e.position);
    }
    void mouseDrag (const juce::MouseEvent& e) override { drawAt (e.position); }
    void mouseUp (const juce::MouseEvent&) override { last = -1; }

private:
    juce::Rectangle<float> plotArea() const
    {
        return getLocalBounds().toFloat().withTrimmedLeft (30.0f).withTrimmedBottom (15.0f).reduced (6.0f, 6.0f);
    }
    float freqToX (double f) const
    {
        const auto p = plotArea();
        const double lo = GeneratorSettings::customBandHz (0), hi = GeneratorSettings::customBandHz (GeneratorSettings::kCustomBands - 1);
        return p.getX() + (float) (std::log (f / lo) / std::log (hi / lo)) * p.getWidth();
    }
    float dbToY (float db) const
    {
        const auto p = plotArea();
        return p.getY() + (kMaxDb - db) / (kMaxDb - kMinDb) * p.getHeight();
    }
    /** Sets the band under the finger, and those it passed over since the
        last point (a quick stroke skips none). */
    void drawAt (juce::Point<float> at)
    {
        const auto p = plotArea();
        const int n = GeneratorSettings::kCustomBands;
        const int i = juce::jlimit (0, n - 1, juce::roundToInt ((at.x - p.getX()) / std::max (1.0f, p.getWidth()) * (float) (n - 1)));
        const float db = juce::jlimit (kMinDb, kMaxDb, kMaxDb - (at.y - p.getY()) / std::max (1.0f, p.getHeight()) * (kMaxDb - kMinDb));
        if (last < 0 || last == i)
            values[(size_t) i] = db;
        else
        {
            const int a = std::min (last, i), b = std::max (last, i);
            const float va = last < i ? lastDb : db, vb = last < i ? db : lastDb;
            for (int k = a; k <= b; ++k)
                values[(size_t) k] = va + (vb - va) * (float) (k - a) / (float) std::max (1, b - a);
        }
        last = i;
        lastDb = db;
        repaint();
        if (onChange) onChange();
    }

    std::vector<float> values;
    int last = -1;
    float lastDb = 0.0f;
};
} // namespace scm
