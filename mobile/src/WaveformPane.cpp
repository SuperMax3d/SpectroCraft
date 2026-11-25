#include "WaveformPane.h"
#include "MobileStyle.h"
#include "TimeFormat.h"
#include <cmath>

namespace scm
{
WaveformPane::WaveformPane (MobileDocument& d, TimeViewport& v) : document (d), view (v)
{
    setOpaque (true);
    view.addChangeListener (this);

    gestures.onDrag = [this] (juce::Point<float> from, juce::Point<float> to)
    {
        const auto p = plotArea();
        document.setSelection ({ view.xToTime (std::min (from.x, to.x), p), view.xToTime (std::max (from.x, to.x), p) });
    };
    gestures.onTap = [this] (juce::Point<float> at)
    {
        document.setSelection ({});
        document.setPlayhead (view.xToTime (at.x, plotArea()));
    };
    gestures.onDoubleTap = [this] (juce::Point<float>) { view.showAll(); ampZoom = 1.0f; repaint(); };
    gestures.onPinchStart = [this] { pinchT0 = view.start(); pinchT1 = view.end(); pinchAmp = ampZoom; };
    gestures.onPinch = [this] (const GestureTracker::Pinch& p)
    {
        const auto plot = plotArea();
        const double span0 = pinchT1 - pinchT0;
        const double kx = (p.startDx > 40.0f && p.dx > 1.0f) ? (double) p.startDx / p.dx : 1.0;
        const double span = span0 * kx;
        const double anchorT = pinchT0 + (p.startMid.x - plot.getX()) / std::max (1.0f, plot.getWidth()) * span0;
        const double nowRel = (p.mid.x - plot.getX()) / std::max (1.0f, plot.getWidth());
        view.setRange (anchorT - nowRel * span, anchorT - nowRel * span + span);
        if (p.startDy > 40.0f && p.dy > 1.0f)
        {
            ampZoom = juce::jlimit (1.0f, 256.0f, pinchAmp * p.dy / p.startDy);
            repaint();
        }
    };
}

WaveformPane::~WaveformPane()
{
    view.removeChangeListener (this);
}

void WaveformPane::setShowRuler (bool show) { showRuler = show; repaint(); }
void WaveformPane::setChannel (int ch) { chan = std::max (0, ch); repaint(); }
void WaveformPane::setWriteHead (double t, bool show) { writeHead = show ? t : -1.0; repaint(); }
void WaveformPane::changeListenerCallback (juce::ChangeBroadcaster*) { repaint(); }

juce::Rectangle<float> WaveformPane::plotArea() const
{
    auto r = getLocalBounds().toFloat();
    if (showRuler) r.removeFromTop ((float) kRulerH);
    r.removeFromLeft ((float) kAxisW);
    return r;
}

void WaveformPane::columnRanges (const AudioEngine& eng, int ch, double t0, double t1, int width,
                                 std::vector<float>& lo, std::vector<float>& hi)
{
    lo.assign ((size_t) std::max (0, width), 0.0f);
    hi.assign ((size_t) std::max (0, width), 0.0f);
    if (width <= 0 || ! eng.isFileLoaded() || ch >= eng.numChannels()) return;
    const double rate = eng.sampleRate();
    const int64_t total = eng.numSamples();
    const double spp = (t1 - t0) * rate / width;

    // The mipmap level with the largest factor not above one column.
    const AudioEngine::MipLevel* level = nullptr;
    for (const auto& m : eng.mipmaps())
        if ((double) m.factor <= spp && (level == nullptr || m.factor > level->factor)
            && ch < (int) m.min.size())
            level = &m;
    const int64_t covered = level != nullptr ? (int64_t) level->min[(size_t) ch].size() * level->factor : 0;

    std::vector<float> raw;
    for (int x = 0; x < width; ++x)
    {
        const int64_t a = (int64_t) std::floor ((t0 + (double) x / width * (t1 - t0)) * rate);
        int64_t b = (int64_t) std::floor ((t0 + (double) (x + 1) / width * (t1 - t0)) * rate);
        b = std::max (b, a + 1);
        const int64_t s0 = std::max<int64_t> (0, a), s1 = std::min (total, b);
        if (s1 <= s0) continue;
        float mn = 0.0f, mx = 0.0f;
        bool any = false;
        if (level != nullptr && s1 <= covered)
        {
            const auto& mins = level->min[(size_t) ch];
            const auto& maxs = level->max[(size_t) ch];
            const int64_t e0 = s0 / level->factor, e1 = std::min<int64_t> ((int64_t) mins.size(), (s1 + level->factor - 1) / level->factor);
            for (int64_t e = e0; e < e1; ++e)
            {
                if (! any) { mn = mins[(size_t) e]; mx = maxs[(size_t) e]; any = true; }
                else { mn = std::min (mn, mins[(size_t) e]); mx = std::max (mx, maxs[(size_t) e]); }
            }
        }
        else
        {
            // Raw samples (zoomed in, or the newest frames of a recording that
            // the mipmaps do not cover yet), decimated to at most 4096 a column.
            const int64_t n = s1 - s0;
            const int64_t stride = std::max<int64_t> (1, n / 4096);
            raw.resize ((size_t) std::min<int64_t> (n, 65536));
            for (int64_t s = s0; s < s1; s += (int64_t) raw.size() * stride)
            {
                const int64_t cnt = std::min<int64_t> ((int64_t) raw.size(), s1 - s);
                if (! eng.readSamples (ch, s, cnt, raw.data())) break;
                for (int64_t i = 0; i < cnt; i += stride)
                {
                    const float v = raw[(size_t) i];
                    if (! any) { mn = mx = v; any = true; }
                    else { mn = std::min (mn, v); mx = std::max (mx, v); }
                }
            }
        }
        lo[(size_t) x] = mn;
        hi[(size_t) x] = mx;
    }
}

void WaveformPane::paint (juce::Graphics& g)
{
    g.fillAll (style::plotBackground);
    const auto plot = plotArea();
    const float mid = plot.getCentreY(), half = plot.getHeight() * 0.5f - 2.0f;

    // grid: 0, -6, -12 dB
    g.setColour (style::grid);
    for (float a : { 1.0f, 0.5f, 0.25f })
    {
        const float y = a * ampZoom;
        if (y > 1.0f) continue;
        g.drawHorizontalLine (juce::roundToInt (mid - y * half), plot.getX(), plot.getRight());
        g.drawHorizontalLine (juce::roundToInt (mid + y * half), plot.getX(), plot.getRight());
    }
    g.setColour (style::grid.brighter (0.2f));
    g.drawHorizontalLine (juce::roundToInt (mid), plot.getX(), plot.getRight());

    // selection under the waveform (as on the desktop)
    const auto sel = document.selection();
    if (! sel.isEmpty())
    {
        const auto a = (float) view.timeToX (sel.getStart(), plot), b = (float) view.timeToX (sel.getEnd(), plot);
        g.setColour (style::selectionFill);
        g.fillRect (juce::Rectangle<float> (a, plot.getY(), b - a, plot.getHeight()).getIntersection (plot));
    }

    if (auto* d = document.doc(); d != nullptr && d->engine.isFileLoaded())
    {
        const int w = (int) plot.getWidth();
        columnRanges (d->engine, juce::jlimit (0, d->engine.numChannels() - 1, chan),
                      view.start(), view.end(), w, lo, hi);
        g.setColour (style::waveform);
        const double endX = view.timeToX (d->engine.duration(), plot);
        for (int x = 0; x < w && plot.getX() + x < endX; ++x)
        {
            const float y0 = mid - juce::jlimit (-1.0f, 1.0f, hi[(size_t) x] * ampZoom) * half;
            const float y1 = mid - juce::jlimit (-1.0f, 1.0f, lo[(size_t) x] * ampZoom) * half;
            g.fillRect (plot.getX() + (float) x, y0, 1.0f, std::max (1.0f, y1 - y0));
        }
        const auto px = (float) view.timeToX (document.playhead(), plot);
        if (px >= plot.getX() && px <= plot.getRight())
        {
            g.setColour (style::playhead);
            g.drawVerticalLine (juce::roundToInt (px), plot.getY(), plot.getBottom());
        }
    }
    if (! sel.isEmpty())
    {
        g.setColour (style::selectionEdge);
        for (double t : { sel.getStart(), sel.getEnd() })
            g.drawVerticalLine (juce::roundToInt (view.timeToX (t, plot)), plot.getY(), plot.getBottom());
    }
    if (writeHead >= 0.0)
    {
        g.setColour (style::record);
        g.fillRect ((float) view.timeToX (writeHead, plot) - 1.0f, plot.getY(), 2.0f, plot.getHeight());
    }

    drawAmplitudeAxis (g, plot);
    if (showRuler) drawRuler (g, plot);
}

void WaveformPane::drawAmplitudeAxis (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const juce::Rectangle<float> axis (0.0f, plot.getY(), (float) kAxisW, plot.getHeight());
    g.setColour (style::axisFill);
    g.fillRect (axis);
    const auto cal = document.calibration();
    const bool pa = cal.isCalibratedFor (chan) && cal.unit == ScaleCalibration::Unit::Pascal;
    const float mid = plot.getCentreY(), half = plot.getHeight() * 0.5f - 2.0f;
    g.setFont (style::font (11.0f));
    for (float a : { 1.0f, 0.5f, 0.25f })
    {
        const float y = a * ampZoom;
        if (y > 1.0f) continue;
        juce::String text;
        if (pa)
        {
            const double v = a * cal.fullScaleFor (chan);
            text = juce::String (v, v < 1.0 ? 3 : v < 10.0 ? 2 : 1);
        }
        else
            text = juce::String (juce::roundToInt (20.0f * std::log10 (a)));
        g.setColour (style::axisLabel);
        g.drawText (text, juce::Rectangle<float> (2.0f, mid - y * half - 7.0f, (float) kAxisW - 6.0f, 14.0f),
                    juce::Justification::centredRight);
    }
    g.setColour (style::textDim);
    g.setFont (style::font (10.0f));
    g.drawText (pa ? "Pa" : "dBFS", juce::Rectangle<float> (2.0f, plot.getBottom() - 14.0f, (float) kAxisW - 6.0f, 12.0f),
                juce::Justification::centredRight);
}

void WaveformPane::drawRuler (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const juce::Rectangle<float> ruler (0.0f, 0.0f, (float) getWidth(), (float) kRulerH);
    g.setColour (style::axisFill);
    g.fillRect (ruler);
    const double pxPerSec = plot.getWidth() / std::max (1e-9, view.span());
    const double step = niceTimeStep (pxPerSec);
    g.setFont (style::font (12.0f));
    for (double t = std::floor (view.start() / step) * step; t <= view.end() + step; t += step)
    {
        const float x = (float) view.timeToX (t, plot);
        if (x < plot.getX() || x > plot.getRight()) continue;
        g.setColour (style::grid);
        g.drawVerticalLine (juce::roundToInt (x), ruler.getBottom() - 6.0f, ruler.getBottom());
        g.setColour (style::axisLabel);
        g.drawText (formatTimeTickLabel (t, step, view.span()),
                    juce::Rectangle<float> (x + 3.0f, 2.0f, 80.0f, (float) kRulerH - 6.0f),
                    juce::Justification::centredLeft);
    }
}

void WaveformPane::mouseDown (const juce::MouseEvent& e)
{
    if (e.position.x < kAxisW) return;
    gestures.mouseDown (e);
}

void WaveformPane::mouseDrag (const juce::MouseEvent& e) { gestures.mouseDrag (e); }
void WaveformPane::mouseUp (const juce::MouseEvent& e) { gestures.mouseUp (e); }

void WaveformPane::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const double factor = std::pow (1.25, (double) w.deltaY * 4.0);
    if (e.mods.isShiftDown())
    {
        ampZoom = juce::jlimit (1.0f, 256.0f, ampZoom * (float) factor);
        repaint();
    }
    else
        view.zoom (view.xToTime (e.position.x, plotArea()), factor);
}
} // namespace scm
