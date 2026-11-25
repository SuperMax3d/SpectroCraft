#include "SpectrogramPane.h"
#include "MobileStyle.h"
#include "TimeFormat.h"
#include <cmath>

namespace scm
{
// ------------------------------------------------------------------ renderer

class SpectrogramPane::Renderer : public juce::Thread
{
public:
    Renderer (SpectrogramPane& p) : juce::Thread ("spectrogram"), pane (p), document (p.document)
    {
        startThread (juce::Thread::Priority::normal);
    }

    ~Renderer() override
    {
        signalThreadShouldExit();
        cancel.store (true);
        wake.signal();
        stopThread (4000);
    }

    void request (const Request& r)
    {
        {
            const juce::ScopedLock sl (reqLock);
            pending = r;
            hasPending = true;
        }
        cancel.store (true);   // a newer view supersedes the one being drawn
        wake.signal();
    }

    void pause()
    {
        cancel.store (true);
        const juce::ScopedLock sl (renderLock);   // waits for the current render
        paused = true;
    }

    void resume()
    {
        const juce::ScopedLock sl (renderLock);
        paused = false;
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            wake.wait (200);
            Request r;
            {
                const juce::ScopedLock sl (reqLock);
                if (! hasPending) continue;
                r = pending;
                hasPending = false;
            }
            const juce::ScopedLock rl (renderLock);
            if (paused) continue;
            cancel.store (false);
            auto* d = document.doc();
            if (d == nullptr || ! d->engine.isFileLoaded() || r.w <= 0 || r.h <= 0) continue;

            std::vector<uint8_t> rgba;
            if (! document.spectral().renderToPixels (d->engine, r.settings, r.w, r.h, r.t0, r.t1,
                                                      { r.channel }, rgba, &cancel))
                continue;
            if ((int) rgba.size() < r.w * r.h * 4) continue;

            juce::Image img (juce::Image::ARGB, r.w, r.h, false, juce::SoftwareImageType());
            {
                const juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
                for (int y = 0; y < r.h; ++y)
                {
                    auto* row = reinterpret_cast<juce::PixelARGB*> (bd.data + (size_t) y * (size_t) bd.lineStride);
                    const uint8_t* src = rgba.data() + (size_t) y * (size_t) r.w * 4;
                    for (int x = 0; x < r.w; ++x)
                        row[x] = juce::PixelARGB (255, src[x * 4], src[x * 4 + 1], src[x * 4 + 2]);
                }
            }
            juce::Component::SafePointer<SpectrogramPane> safe (&pane);
            juce::MessageManager::callAsync ([safe, img, r]
            {
                if (safe != nullptr) safe->imageReady (img, r);
            });
        }
    }

private:
    SpectrogramPane& pane;
    MobileDocument& document;
    juce::WaitableEvent wake;
    juce::CriticalSection reqLock, renderLock;
    Request pending;
    bool hasPending = false;
    bool paused = false;
    std::atomic<bool> cancel { false };
};

// ------------------------------------------------------------------ pane

SpectrogramPane::SpectrogramPane (MobileDocument& d, TimeViewport& v) : document (d), view (v)
{
    setOpaque (true);
    renderer = std::make_unique<Renderer> (*this);
    view.addChangeListener (this);

    gestures.onDrag = [this] (juce::Point<float> from, juce::Point<float> to)
    {
        const auto p = plotArea();
        document.setSelection ({ view.xToTime (std::min (from.x, to.x), p), view.xToTime (std::max (from.x, to.x), p) });
        repaint();
    };
    gestures.onTap = [this] (juce::Point<float> at)
    {
        document.setSelection ({});
        document.setPlayhead (view.xToTime (at.x, plotArea()));
        repaint();
    };
    gestures.onDoubleTap = [this] (juce::Point<float>)
    {
        view.showAll();
        setFrequencyRange (20.0, nyquist());
    };
    gestures.onPinchStart = [this]
    {
        pinchT0 = view.start(); pinchT1 = view.end();
        pinchFLo = fLo; pinchFHi = fHi;
    };
    gestures.onPinch = [this] (const GestureTracker::Pinch& p)
    {
        const auto plot = plotArea();
        // time: keep the point under the starting midpoint under the fingers
        const double span0 = pinchT1 - pinchT0;
        const double kx = (p.startDx > 40.0f && p.dx > 1.0f) ? (double) p.startDx / p.dx : 1.0;
        const double span = span0 * kx;
        const double anchorRel = (p.startMid.x - plot.getX()) / std::max (1.0f, plot.getWidth());
        const double anchorT = pinchT0 + anchorRel * span0;
        const double nowRel = (p.mid.x - plot.getX()) / std::max (1.0f, plot.getWidth());
        view.setRange (anchorT - nowRel * span, anchorT - nowRel * span + span);

        // frequency: the same in the axis' own scale
        if (p.startDy > 40.0f && p.dy > 1.0f)
        {
            const double ky = (double) p.startDy / p.dy;
            const auto toU = [this] (double f) { return logAxis ? std::log (std::max (1.0, f)) : f; };
            const auto fromU = [this] (double u) { return logAxis ? std::exp (u) : u; };
            const double u0 = toU (pinchFLo), u1 = toU (pinchFHi);
            const double uSpan = (u1 - u0) * ky;
            const double aRel = 1.0 - (p.startMid.y - plot.getY()) / std::max (1.0f, plot.getHeight());
            const double aU = u0 + aRel * (u1 - u0);
            const double nRel = 1.0 - (p.mid.y - plot.getY()) / std::max (1.0f, plot.getHeight());
            setFrequencyRange (fromU (aU - nRel * uSpan), fromU (aU - nRel * uSpan + uSpan));
        }
    };
}

SpectrogramPane::~SpectrogramPane()
{
    view.removeChangeListener (this);
    renderer.reset();
}

void SpectrogramPane::setShowRuler (bool show)
{
    showRuler = show;
    resized();
    repaint();
}

void SpectrogramPane::setChannel (int ch)
{
    chan = std::max (0, ch);
    requestRender();
}

void SpectrogramPane::pauseRendering() { renderer->pause(); }

void SpectrogramPane::resumeRendering()
{
    renderer->resume();
    image = {};
    fHi = std::min (fHi, nyquist());
    requestRender();
}

void SpectrogramPane::contentChanged() { requestRender(); }

void SpectrogramPane::setWriteHead (double t, bool show)
{
    writeHead = show ? t : -1.0;
    repaint();
}

double SpectrogramPane::nyquist() const
{
    return document.sampleRate() * 0.5;
}

void SpectrogramPane::setFrequencyRange (double lo, double hi)
{
    if (hi < lo) std::swap (lo, hi);
    const double nyq = nyquist();
    lo = juce::jlimit (1.0, nyq - 10.0, lo);
    hi = juce::jlimit (lo + 10.0, nyq, hi);
    if (logAxis && hi / lo < 1.2) hi = std::min (nyq, lo * 1.2);
    fLo = lo;
    fHi = hi;
    requestRender();
    repaint();
}

void SpectrogramPane::setLogFrequency (bool log)
{
    logAxis = log;
    if (! log) fLo = std::max (0.0, fLo);
    requestRender();
    repaint();
}

void SpectrogramPane::zoomFrequency (double anchorHz, double factor)
{
    const auto toU = [this] (double f) { return logAxis ? std::log (std::max (1.0, f)) : f; };
    const auto fromU = [this] (double u) { return logAxis ? std::exp (u) : u; };
    const double u0 = toU (fLo), u1 = toU (fHi), a = toU (anchorHz);
    const double rel = (a - u0) / std::max (1e-9, u1 - u0);
    const double span = (u1 - u0) / factor;
    setFrequencyRange (fromU (a - rel * span), fromU (a - rel * span + span));
}

juce::Rectangle<float> SpectrogramPane::plotArea() const
{
    auto r = getLocalBounds().toFloat();
    if (showRuler) r.removeFromTop ((float) kRulerH);
    r.removeFromLeft ((float) kAxisW);
    return r;
}

double SpectrogramPane::yToFreq (double y) const
{
    const auto p = plotArea();
    const double t = 1.0 - (y - p.getY()) / std::max (1.0f, p.getHeight());
    if (logAxis)
        return std::exp (std::log (std::max (1.0, fLo)) + t * (std::log (fHi) - std::log (std::max (1.0, fLo))));
    return fLo + t * (fHi - fLo);
}

double SpectrogramPane::freqToY (double f) const
{
    const auto p = plotArea();
    double t;
    if (logAxis)
        t = (std::log (std::max (1.0, f)) - std::log (std::max (1.0, fLo)))
            / std::max (1e-9, std::log (fHi) - std::log (std::max (1.0, fLo)));
    else
        t = (f - fLo) / std::max (1e-9, fHi - fLo);
    return p.getBottom() - t * p.getHeight();
}

void SpectrogramPane::requestRender()
{
    if (! document.hasAudio() || getWidth() <= 0) return;
    const auto plot = plotArea();
    // Render at up to twice the logical resolution (retina screens) - the
    // finest detail without tripling the work on a 3x phone screen.
    float scale = 1.0f;
    if (auto* disp = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds()))
        scale = (float) juce::jlimit (1.0, 2.0, disp->scale);
    Request r;
    r.w = std::max (1, juce::roundToInt (plot.getWidth() * scale));
    r.h = std::max (1, juce::roundToInt (plot.getHeight() * scale));
    r.t0 = view.start();
    r.t1 = view.end();
    r.channel = juce::jlimit (0, std::max (0, document.numChannels() - 1), chan);
    r.serial = ++serial;
    Settings s = document.settings();
    const double rate = document.sampleRate();
    s.logFrequency = logAxis;
    s.minFrequency = (float) std::max (1.0, fLo);
    s.maxFrequency = (float) std::min (rate * 0.5, fHi);
    if (s.fftAuto && ! s.fftMultiRes)
        s.fftSize = SpectralEngine::chooseFftSize (r.t1 - r.t0, rate, std::max (1, r.h),
                                                   s.minFrequency, s.maxFrequency,
                                                   s.autoTargetTimeSlices, s.autoBinsPerPixel);
    r.settings = s;
    renderer->request (r);
}

void SpectrogramPane::imageReady (juce::Image img, const Request& r)
{
    if (r.serial < shown.serial) return;
    image = img;
    shown = r;
    repaint();
}

void SpectrogramPane::changeListenerCallback (juce::ChangeBroadcaster*)
{
    requestRender();
    repaint();
}

void SpectrogramPane::resized()
{
    requestRender();
}

void SpectrogramPane::paint (juce::Graphics& g)
{
    g.fillAll (style::plotBackground);
    const auto plot = plotArea();

    if (image.isValid() && shown.t1 > shown.t0)
    {
        // Draw the last picture where its time / frequency range now falls, so
        // zooming and panning move it at once; the exact one follows.
        const double x0 = view.timeToX (shown.t0, plot), x1 = view.timeToX (shown.t1, plot);
        const double y0 = freqToY (shown.settings.maxFrequency), y1 = freqToY (shown.settings.minFrequency);
        const bool sameAxis = shown.settings.logFrequency == logAxis;
        g.saveState();
        g.reduceClipRegion (plot.toNearestInt());
        if (sameAxis)
            g.drawImage (image, juce::Rectangle<double> (x0, y0, x1 - x0, y1 - y0).toFloat(),
                         juce::RectanglePlacement::stretchToFit);
        g.restoreState();
    }
    else if (! document.hasAudio())
    {
        g.setColour (style::textDim);
        g.setFont (style::font (16.0f));
        g.drawFittedText ("Record, or open a WAV file", plot.toNearestInt(), juce::Justification::centred, 2);
    }

    // selection, playhead, loop write head
    const auto sel = document.selection();
    if (! sel.isEmpty())
    {
        const auto a = (float) view.timeToX (sel.getStart(), plot), b = (float) view.timeToX (sel.getEnd(), plot);
        g.setColour (style::selectionFill);
        g.fillRect (juce::Rectangle<float> (a, plot.getY(), b - a, plot.getHeight()).getIntersection (plot));
        g.setColour (style::selectionEdge);
        g.drawVerticalLine (juce::roundToInt (a), plot.getY(), plot.getBottom());
        g.drawVerticalLine (juce::roundToInt (b), plot.getY(), plot.getBottom());
    }
    if (document.hasAudio())
    {
        const auto x = (float) view.timeToX (document.playhead(), plot);
        if (x >= plot.getX() && x <= plot.getRight())
        {
            g.setColour (style::playhead);
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        }
    }
    if (writeHead >= 0.0)
    {
        const auto x = (float) view.timeToX (writeHead, plot);
        g.setColour (style::record);
        g.fillRect (x - 1.0f, plot.getY(), 2.0f, plot.getHeight());
    }

    drawFrequencyAxis (g, plot);
    if (showRuler) drawRuler (g, plot);
}

void SpectrogramPane::drawFrequencyAxis (juce::Graphics& g, juce::Rectangle<float> plot)
{
    const juce::Rectangle<float> axis (0.0f, plot.getY(), (float) kAxisW, plot.getHeight());
    g.setColour (style::axisFill);
    g.fillRect (axis);
    g.setFont (style::font (12.0f));
    const auto label = [] (double f)
    {
        if (f >= 1000.0)
        {
            const double k = f / 1000.0;
            return juce::String (k, std::abs (k - std::round (k)) < 1e-6 ? 0 : 1) + "k";
        }
        return juce::String (f, f < 10.0 && std::abs (f - std::round (f)) > 1e-6 ? 1 : 0);
    };
    std::vector<double> ticks;
    if (logAxis)
    {
        for (double dec = 1.0; dec < 1e6; dec *= 10.0)
            for (double m : { 1.0, 2.0, 5.0 })
                if (m * dec >= fLo && m * dec <= fHi) ticks.push_back (m * dec);
    }
    else
    {
        const double rough = (fHi - fLo) / std::max (2.0, plot.getHeight() / 40.0);
        const double p10 = std::pow (10.0, std::floor (std::log10 (rough)));
        const double step = rough / p10 < 2 ? 2 * p10 : rough / p10 < 5 ? 5 * p10 : 10 * p10;
        for (double f = std::ceil (fLo / step) * step; f <= fHi; f += step) ticks.push_back (f);
    }
    float lastY = 1e9f;
    for (double f : ticks)
    {
        const float y = (float) freqToY (f);
        g.setColour (style::grid.withAlpha (0.6f));
        g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        if (std::abs (y - lastY) < 14.0f || y > plot.getBottom() - 16.0f) continue;
        lastY = y;
        g.setColour (style::axisLabel);
        g.drawText (label (f), juce::Rectangle<float> (2.0f, y - 7.0f, (float) kAxisW - 6.0f, 14.0f),
                    juce::Justification::centredRight);
    }
    g.setColour (style::textDim);
    g.setFont (style::font (10.0f));
    g.drawText ("Hz", juce::Rectangle<float> (2.0f, plot.getBottom() - 14.0f, (float) kAxisW - 6.0f, 12.0f),
                juce::Justification::centredRight);
}

void SpectrogramPane::drawRuler (juce::Graphics& g, juce::Rectangle<float> plot)
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

void SpectrogramPane::mouseDown (const juce::MouseEvent& e)
{
    if (e.position.x < kAxisW) return;
    gestures.mouseDown (e);
}

void SpectrogramPane::mouseDrag (const juce::MouseEvent& e) { gestures.mouseDrag (e); }
void SpectrogramPane::mouseUp (const juce::MouseEvent& e) { gestures.mouseUp (e); }

void SpectrogramPane::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const double factor = std::pow (1.25, (double) w.deltaY * 4.0);
    if (e.mods.isShiftDown() || e.position.x < kAxisW)
        zoomFrequency (yToFreq (e.position.y), factor);
    else
        view.zoom (view.xToTime (e.position.x, plotArea()), factor);
}
} // namespace scm
