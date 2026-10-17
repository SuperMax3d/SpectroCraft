/**
 * SpectroCraft - Spectral View Implementation
 * Time window is shared with the waveform view (MainWindow keeps them in sync);
 * the frequency axis limits are local to this view. Each visible channel gets
 * its own row, like the waveform view; both views use the same left-axis width
 * so their plots start at the same x coordinate.
 */

#include "SpectralView.h"
#include "AudioEngine.h"
#include "MarkerManager.h"
#include "PngWriter.h"
#include <algorithm>
#include <cmath>

juce::Rectangle<int> SpectralView::plotRect() const
{
    return { kLeftAxisWidth, 0,
             std::max (1, getWidth() - kLeftAxisWidth),
             std::max (1, getHeight()) };
}

// One row per visible channel with a thin dark gap between the rows.
static std::vector<juce::Rectangle<int>> spectralRowRects (int w, int h, int nRows)
{
    std::vector<juce::Rectangle<int>> rects;
    if (nRows <= 0 || h <= 0)
        return rects;

    const int gap = (nRows > 1) ? 2 : 0;   // thin dark separator between channel rows
    const int totalGap = gap * (nRows - 1);
    const int baseRowH = std::max (1, (h - totalGap) / nRows);

    for (int r = 0; r < nRows; ++r)
    {
        const int y0 = r * (baseRowH + gap);
        rects.push_back ({ 0, y0, w, std::min (baseRowH, h - y0) }); // last row takes the remainder
    }
    return rects;
}

SpectralView::SpectralView (Application& appIn)
    : app (appIn), engine (appIn.audioEngine), spectral (appIn.spectralEngine)
{
    settings_ = appIn.settings;
    freqMin = std::max (1.0, (double) appIn.settings.minFrequency);
    freqMax = std::min (engine.sampleRate() * 0.5, (double) appIn.settings.maxFrequency);
}

SpectralView::~SpectralView() {}

void SpectralView::setWindow (double t0, double t1)
{
    if (t0 != viewStart || t1 != viewEnd)
    {
        viewStart = t0;
        viewEnd = t1;
        imageDirty = true;
        repaint();
        if (onChange) onChange (viewStart, viewEnd);
    }
}

double SpectralView::xToTime (int x) const
{
    const auto pr = plotRect();
    if (pr.getWidth() <= 0 || viewEnd <= viewStart) return viewStart;
    const double t = ((double) std::clamp (x, pr.getX(), pr.getRight()) - (double) pr.getX())
                     / (double) pr.getWidth() * (viewEnd - viewStart);
    return viewStart + t;
}

double SpectralView::timeToX (double t) const
{
    const auto pr = plotRect();
    const double w = viewEnd - viewStart;
    if (w <= 0 || pr.getWidth() <= 0) return (float) pr.getX();
    return (float) pr.getX() + (t - viewStart) / w * (double) pr.getWidth();
}

float SpectralView::freqToY (double freq, int height) const
{
    const double rate = engine.sampleRate();
    const double fMin = std::max (1.0, freqMin);
    const double fMax = std::min (rate * 0.5, freqMax);
    if (fMax <= fMin) return 0;
    if (settings_.logFrequency)
    {
        const double lmin = std::log (fMin), lmax = std::log (fMax);
        const double t = (std::log (std::max (1.0, freq)) - lmin) / (lmax - lmin);
        return (float) ((1.0 - t) * height);
    }
    const double t = (freq - fMin) / (fMax - fMin);
    return (float) ((1.0 - t) * height);
}

double SpectralView::yToFreq (float y, int height) const
{
    const double rate = engine.sampleRate();
    const double fMin = std::max (1.0, freqMin);
    const double fMax = std::min (rate * 0.5, freqMax);
    const double t = 1.0 - (double) y / std::max (1, height);
    if (settings_.logFrequency)
    {
        const double lmin = std::log (fMin), lmax = std::log (fMax);
        return std::exp (lmin + t * (lmax - lmin));
    }
    return fMin + t * (fMax - fMin);
}

void SpectralView::resized() { dirty = true; imageDirty = true; }

// ---------------------------------------------------------------- interaction

// Build and show the right-click context menu (zoom + edit actions).
// Called from a mouse handler, so withMousePosition() lands exactly where clicked.
static void showViewContextMenu (Application& app,
                                 std::function<void(int)>& onAction)
{
    if (!onAction) return;

    juce::PopupMenu m;
    using enum ContextMenuIDs;
    const bool hasSel = !app.selection.isEmpty();
    m.addItem (CtxZoomToSelection, "Zoom to Selection", hasSel, false);
    m.addItem (CtxZoomOutFull,     "Zoom Out Full",
               app.audioEngine.isFileLoaded(), false);
    m.addItem (CtxSelectAll,       "Select All\tCtrl+A",
               app.audioEngine.isFileLoaded(), false);
    m.addSeparator();
    m.addItem (CtxCut,   "Cut\tCtrl+X",   hasSel, false);
    m.addItem (CtxCopy,  "Copy\tCtrl+C",  hasSel, false);
    m.addItem (CtxPaste, "Paste\tCtrl+V", app.audioEngine.canPaste(), false);
    m.addItem (CtxDelete,"Delete\tDel",   hasSel, false);

    if (const int sel = m.showMenu (juce::PopupMenu::Options().withMousePosition());
        sel != 0)
        onAction (sel);
}

void SpectralView::mouseDown (const juce::MouseEvent& e)
{
    if (!engine.isFileLoaded()) return;

    // Right-click shows the context menu; middle-drag still pans.
    if (e.mods.isRightButtonDown() && !e.mods.isLeftButtonDown())
    {
        showViewContextMenu (app, onContextAction);
        return;
    }

    dragStartTime = xToTime (e.x);

    // Clicking moves the playhead to that position
    // (clamped; also updates the transport position without starting playback).
    app.setPlayhead (dragStartTime);
    setPlayhead (app.playhead);

    if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown() && !e.mods.isMiddleButtonDown()
        && e.x >= kLeftAxisWidth)
    {
        dragMode = DragMode::Select;
        app.setSelection ({ dragStartTime, dragStartTime });
        selection = app.selection;
    }
    else if (e.mods.isMiddleButtonDown() || e.mods.isRightButtonDown())
    {
        dragMode = DragMode::Pan;
    }

    // Push the new playhead/selection to both views.
    onChange (viewStart, viewEnd);
}

void SpectralView::mouseDrag (const juce::MouseEvent& e)
{
    if (!engine.isFileLoaded()) return;

    switch (dragMode)
    {
        case DragMode::Select:
        {
            const double t = xToTime (e.x);
            app.setSelection ({ std::min (dragStartTime, t), std::max (dragStartTime, t) });
            selection = app.selection;
            onChange (viewStart, viewEnd);
            repaint();
            break;
        }
        case DragMode::Pan:
        {
            // setWindow() inside panByTime already notifies MainWindow.
            const double dt = dragStartTime - xToTime (e.x);
            panByTime (dt);
            break;
        }
        default: break;
    }
}

void SpectralView::mouseUp (const juce::MouseEvent&)
{
    if (dragMode == DragMode::Select && app.selection.getLength() <= 1e-6)
    {
        app.setSelection ({ 0, 0 }); // plain click clears the selection
        selection = app.selection;
        repaint();
        onChange (viewStart, viewEnd); // push the cleared selection to both views
    }
    dragMode = DragMode::None;
}

// Status-bar probe: time + frequency + level of that frequency under the cursor.
void SpectralView::mouseMove (const juce::MouseEvent& e)
{
    if (!onCursorInfo) return;
    const auto pr = plotRect();
    if (e.x < pr.getX() || e.x >= pr.getRight()) { onCursorInfo (""); return; }

    // Which channel row is the cursor in?
    const auto rows = spectralRowRects (getWidth(), getHeight(), (int) channels.size());
    int rowIdx = -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].contains ((int) e.x, (int) e.y)) { rowIdx = (int) i; break; }
    if (rowIdx < 0 || rowIdx >= (int) channels.size())
    {
        onCursorInfo ("");
        return;
    }

    const int ch = channels[(size_t) rowIdx];
    const auto& row = rows[(size_t) rowIdx];
    const double t = xToTime ((int) e.x);
    const double freq = yToFreq (e.y - row.getY(), row.getHeight());

    // Use the same mipmap level as the rendered image so the tiles are cached.
    const double rate = engine.sampleRate();
    const int64_t s0 = std::max ((int64_t) 0, (int64_t) (viewStart * rate));
    const int64_t s1 = std::min (engine.numSamples(), (int64_t) (viewEnd * rate));
    const double spp = (s1 > s0)
        ? ((double) (s1 - s0) / std::max (1, pr.getWidth())) : 0.0;
    const int level = SpectralEngine::pickMipLevel (spp, settings_.hopSize);

    const float dB = spectral.probeMagnitude (engine, ch, t, freq, settings_, level);

    onCursorInfo ("t=" + juce::String (t, 3) + " s   f="
                  + ((freq >= 1000.0)
                        ? juce::String (freq / 1000.0, 2) + " kHz"
                        : juce::String ((int) std::lround (freq)) + " Hz")
                  + "   " + juce::String (dB > -290.0 ? dB : -300.0f, 1) + " dB");
}

void SpectralView::mouseExit (const juce::MouseEvent&)
{
    if (onCursorInfo) onCursorInfo ("");
}

void SpectralView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!engine.isFileLoaded()) return;
    const double factor = wheel.deltaY > 0 ? 1.2 : (1.0 / 1.2);

    // Wheel over the left frequency axis (or Shift+wheel anywhere) zooms the
    // frequency range around the cursor; otherwise it zooms in time at the cursor.
    if (e.mods.isShiftDown() || e.x < kLeftAxisWidth)
        zoomFreqAtY ((float) e.y, factor);
    else
        zoomAtX (e.x, factor); // notifies via setWindow()

    repaint();
}

void SpectralView::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Only left-button double-clicks select; middle/right are pan/menu buttons.
    if (!e.mods.isLeftButtonDown() || !engine.isFileLoaded())
        return;

    // Double-clicking a marker sets the selection to its range.
    const auto pr = plotRect();
    if (pr.contains ((int) e.x, (int) e.y) && markerList != nullptr)
    {
        for (const Marker& m : *markerList)
        {
            const double x1 = timeToX (m.start);
            const double x2 = (m.end >= 0.0 && m.end > m.start) ? timeToX (m.end) : x1;
            if (std::abs (e.x - x1) <= 3.0
                || (x2 > x1 && e.x >= x1 - 3.0 && e.x <= x2 + 3.0))
            {
                const double end = (m.end > m.start) ? m.end : m.start;
                app.setSelection ({ m.start, end });
                setSelection (app.selection); // local copy for immediate repaint
                onChange (viewStart, viewEnd); // sync both views via MainWindow
                return;
            }
        }
    }

    // Double-click selects the whole file and fits it to the view.
    app.selectAll();
    setSelection (app.selection); // local copy for immediate repaint
    fitAll(); // notifies via setWindow() when the range changes
    resetFrequencyRange();
}

// ------------------------------------------------------- independent zooming

void SpectralView::zoomAtX (int x, double factor)
{
    const auto pr = plotRect();
    if (pr.getWidth() <= 0 || viewEnd <= viewStart) return;

    const double tCursor = xToTime (x);
    const double newHalf = (viewEnd - viewStart) * 0.5 / factor;
    const double ratio = std::clamp ((double) (x - pr.getX()) / (double) pr.getWidth(), 0.0, 1.0);

    double ns = tCursor - newHalf * (2.0 * ratio);
    double ne = tCursor + newHalf * (2.0 - 2.0 * ratio);
    const double w = ne - ns;
    if (ns < 0.0) { ns = 0.0; ne = std::min (w, engine.duration()); }
    if (ne > engine.duration()) { ne = engine.duration(); ns = std::max (0.0, ne - w); }
    if (ne <= ns) return; // zoomed past the limits

    setWindow (ns, ne);
}

void SpectralView::zoomHorizontalAtCenter (double factor)
{
    const auto pr = plotRect();
    zoomAtX (pr.getX() + pr.getWidth() / 2, factor);
}

void SpectralView::fitAll()
{
    setWindow (0.0, engine.duration());
}

void SpectralView::resetFrequencyRange()
{
    const double rate = engine.sampleRate();
    const double lo = std::max (1.0, (double) app.settings.minFrequency);
    const double hi = std::min (rate * 0.5, (double) app.settings.maxFrequency);
    if (lo == freqMin && hi == freqMax)
        return;
    freqMin = lo;
    freqMax = hi;
    imageDirty = true;
    if (onFrequencyRangeChanged) onFrequencyRangeChanged (freqMin, freqMax);
}

void SpectralView::setFrequencyRange (double lo, double hi)
{
    const double rate = engine.sampleRate();
    lo = std::clamp (lo, 1.0, rate * 0.5);
    hi = std::clamp (hi, 1.0, rate * 0.5);
    if (hi <= lo * 1.01) return; // degenerate range
    if (lo == freqMin && hi == freqMax)
        return;
    freqMin = lo;
    freqMax = hi;
    imageDirty = true;
    repaint();
    if (onFrequencyRangeChanged) onFrequencyRangeChanged (freqMin, freqMax);
}

bool SpectralView::saveAsPng (const juce::File& file)
{
    if (!engine.isFileLoaded() || channels.empty()) return false;
    const auto pr = plotRect();
    Settings rs = settings_;
    rs.minFrequency = (float) std::max (1.0, freqMin);
    rs.maxFrequency = (float) std::min (engine.sampleRate() * 0.5, freqMax);

    juce::Image img (juce::Image::ARGB, pr.getWidth(), pr.getHeight(), true);
    renderSpectralContent (img, rs);

    // Extract raw pixels and write a PNG with our own writer.
    std::vector<uint8_t> rgba ((size_t) pr.getWidth() * pr.getHeight() * 4);
    for (int y = 0; y < pr.getHeight(); ++y)
        for (int x = 0; x < pr.getWidth(); ++x)
        {
            const juce::Colour c = img.getPixelAt (x, y);
            const size_t o = ((size_t) y * pr.getWidth() + x) * 4;
            rgba[o + 0] = (uint8_t) c.getRed();
            rgba[o + 1] = (uint8_t) c.getGreen();
            rgba[o + 2] = (uint8_t) c.getBlue();
            rgba[o + 3] = (uint8_t) c.getAlpha();
        }
    return PngWriter::writePng (file, pr.getWidth(), pr.getHeight(), rgba);
}

void SpectralView::zoomFrequencyBy (double factor)
{
    // Keyboard vertical zoom: zoom the frequency range around its centre.
    if (!engine.isFileLoaded()) return;
    zoomFreqAtY ((float) getHeight() * 0.5f, factor);
}

void SpectralView::panByTime (double dt)
{
    const double dur = engine.duration();
    const double w = viewEnd - viewStart;
    const double ns = std::clamp (viewStart + dt, 0.0, std::max (0.0, dur - w));
    setWindow (ns, ns + w);
}

void SpectralView::zoomFreqAtY (float y, double factor)
{
    const double rate = engine.sampleRate();
    const double fCursor = std::clamp (yToFreq (y, getHeight()), 1.0, rate * 0.5);
    double lo = std::max (1.0, freqMin);
    double hi = std::min (rate * 0.5, freqMax);

    if (settings_.logFrequency)
    {
        const double llo = std::log (lo), lhi = std::log (hi), lf = std::log (fCursor);
        lo = std::exp (std::clamp (lf + (llo - lf) / factor, std::log (1.0), std::log (rate * 0.5)));
        hi = std::exp (std::clamp (lf + (lhi - lf) / factor, std::log (1.0), std::log (rate * 0.5)));
    }
    else
    {
        lo = std::clamp (fCursor + (lo - fCursor) / factor, 1.0, rate * 0.5);
        hi = std::clamp (fCursor + (hi - fCursor) / factor, 1.0, rate * 0.5);
    }

    if (hi <= lo * 1.01 || (lo == freqMin && hi == freqMax))
        return; // degenerate or unchanged range
    freqMin = lo;
    freqMax = hi;
    imageDirty = true;
    if (onFrequencyRangeChanged) onFrequencyRangeChanged (freqMin, freqMax);
}

// ------------------------------------------------------------------- painting

void SpectralView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    g.fillAll (juce::Colours::black);

    if (!engine.isFileLoaded() || channels.empty())
    {
        g.setColour (juce::Colour::fromRGBA (200, 200, 200, 120));
        g.setFont (14.0f);
        g.drawText ("No audio file loaded", bounds, juce::Justification::centred, true);
        return;
    }

    const auto pr = plotRect();
    const int w = pr.getWidth(), h = pr.getHeight();

    // Re-render the spectral image only when the view range / settings changed.
    if (imageDirty || !haveImage || w != imgW || h != imgH || viewStart != imgT0 || viewEnd != imgT1)
    {
        // The engine uses the passed Settings for everything: feed it this
        // view's local frequency limits.
        Settings rs = settings_;
        rs.minFrequency = (float) std::max (1.0, freqMin);
        rs.maxFrequency = (float) std::min (engine.sampleRate() * 0.5, freqMax);

        cachedImage = juce::Image (juce::Image::ARGB, w, h, true);
        renderSpectralContent (cachedImage, rs);

        imgW = w; imgH = h; imgT0 = viewStart; imgT1 = viewEnd;
        haveImage = true;
        imageDirty = false;
    }

    if (haveImage)
    {
        g.drawImage (cachedImage,
                     juce::Rectangle<float> ((float) pr.getX(), 0.0f, (float) w, (float) h),
                     juce::RectanglePlacement::fillDestination);
    }

    // Opaque ruler background so the labels stay readable over the spectrum.
    g.setColour (juce::Colour::fromRGB (32, 32, 36));
    g.fillRect (0, 0, kLeftAxisWidth, getHeight());
    drawFrequencyRuler (g);

    // Channel labels on the left of each track: "L"/"R" for stereo files,
    // channel numbers when several tracks are shown at once.
    if (channels.size() > 1 || engine.numChannels() == 2)
    {
        const auto rows = spectralRowRects (getWidth(), getHeight(), (int) channels.size());
        g.setFont (10.0f);
        for (size_t r = 0; r < rows.size(); ++r)
        {
            const int ch = channels[r];
            const auto& rr = rows[r];
            const juce::String lab = (engine.numChannels() == 2) ? (ch == 0 ? "L" : "R") : juce::String (ch + 1);
            g.setColour (juce::Colour::fromRGB (190, 190, 190));
            g.drawText (lab, 4, rr.getY() + rr.getHeight() / 2 - 8, kLeftAxisWidth - 6, 16,
                       juce::Justification::centredRight, false);
        }
    }

    drawOverlays (g);
}

// Renders the spectral content (one row per visible channel with dark gaps)
// into an ARGB image of size w×h. Used by paint() and saveAsPng().
void SpectralView::renderSpectralContent (juce::Image& img, const Settings& rs)
{
    const int w = img.getWidth(), h = img.getHeight();
    if (w <= 0 || h <= 0) return;

    const juce::Image::BitmapData bd (img, 0, 0, w, h, juce::Image::BitmapData::writeOnly);

    // One row per visible channel (like the waveform view), separated by
    // thin dark gaps. Each row gets its own frequency axis on the left.
    const auto rows = spectralRowRects (w, h, (int) channels.size());
    for (size_t r = 0; r < rows.size(); ++r)
    {
        const int ch = channels[r];
        const auto rr = rows[r];
        if (rr.getHeight() <= 0) continue;

        std::vector<uint8_t> pixels;
        spectral.renderToPixels (engine, rs, w, rr.getHeight(), viewStart, viewEnd, { ch }, pixels);

        for (int y = 0; y < rr.getHeight(); ++y)
        {
            uint8_t* row = static_cast<uint8_t*> (bd.data) + (size_t) (rr.getY() + y) * bd.lineStride;
            const uint8_t* src = pixels.data() + (size_t) y * w * 4; // RGBA from engine
            for (int x = 0; x < w; ++x)
                ((juce::PixelARGB*) row)[x] = juce::PixelARGB (255, src[x * 4],
                                                              src[x * 4 + 1], src[x * 4 + 2]);
        }
    }

    // Paint the gap rows as a thin dark separator between channel tracks.
    if (rows.size() > 1)
    {
        for (size_t r = 1; r < rows.size(); ++r)
        {
            const int gy = rows[r].getY() - 2;
            for (int y = 0; y < 2 && gy + y >= 0 && gy + y < h; ++y)
            {
                uint8_t* row = static_cast<uint8_t*> (bd.data) + (size_t) (gy + y) * bd.lineStride;
                for (int x = 0; x < w; ++x)
                    ((juce::PixelARGB*) row)[x] = juce::PixelARGB (255, 24, 24, 28);
            }
        }
    }
}

// One frequency axis per visible channel row: tick lines across the plot and
// labels in the left ruler area for each row.
void SpectralView::drawFrequencyRuler (juce::Graphics& g)
{
    const double rate = engine.sampleRate();
    const double fMax = std::min (rate * 0.5, freqMax);
    const double fMin = std::max (1.0, freqMin);

    // A channel label sits at the vertical centre of each row; keep tick
    // labels out of that zone.
    const bool hasChanLabel = (channels.size() > 1 || engine.numChannels() == 2);

    const auto rows = spectralRowRects (getWidth(), getHeight(), (int) channels.size());
    g.setFont (10.0f);

    for (const auto& rr : rows)
    {
        if (rr.getHeight() <= 0) continue;
        // Labels only when the row is tall enough to hold them.
        const bool showLabels = rr.getHeight() >= 48;

        auto drawTickAndLabel = [&](double f)
        {
            const float y = (float) rr.getY() + freqToY (f, rr.getHeight());
            g.setColour (juce::Colour::fromRGB (60, 60, 60));
            g.drawLine ((float) kLeftAxisWidth, y, (float) getWidth(), y, 1.0f);
            if (!showLabels) return;
            if (hasChanLabel && std::abs (y - ((float) rr.getY() + rr.getHeight() * 0.5f)) < 10.0f)
                return; // room for the channel label
            g.setColour (juce::Colour::fromRGB (200, 200, 200));
            juce::String label = f >= 1000 ? juce::String (f / 1000.0, 1) + " kHz" : juce::String ((int) f) + " Hz";
            g.drawText (label, 4, (int) y - 12, kLeftAxisWidth - 6, 12, juce::Justification::bottomLeft, false);
        };

        if (settings_.logFrequency)
        {
            for (double p = std::ceil (std::log10 (fMin)); p <= std::floor (std::log10 (fMax)); ++p)
                drawTickAndLabel (std::pow (10.0, p));
        }
        else
        {
            const int nTicks = rr.getHeight() >= 64 ? 5 : 2;
            for (int i = 0; i <= nTicks; ++i)
                drawTickAndLabel (fMin + (fMax - fMin) * i / nTicks);
        }
    }
}

void SpectralView::drawOverlays (juce::Graphics& g)
{
    // Time selection — blue fill with solid edge lines so it stays clearly
    // visible over the spectrogram image.
    if (!selection.isEmpty())
    {
        const double x1 = timeToX (selection.getStart());
        const double x2 = timeToX (selection.getEnd());
        g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
        g.fillRect ((float) x1, 0.0f, (float) std::max (0.0, x2 - x1), (float) getHeight());
        g.setColour (juce::Colour::fromRGB (96, 150, 255));
        g.drawVerticalLine ((float) x1, 0.0f, (float) getHeight());
        g.drawVerticalLine ((float) x2, 0.0f, (float) getHeight());
    }

    // Spectral-domain selection rectangle
    if (spectralSel.getWidth() > 0 && spectralSel.getHeight() > 0)
    {
        g.setColour (juce::Colour::fromRGBA (255, 255, 255, 60));
        g.fillRect (spectralSel);
        g.setColour (juce::Colour::fromRGBA (255, 255, 255, 180));
        g.drawRect (spectralSel.toFloat(), 1.0f);
    }

    // Markers
    if (markerList)
    {
        for (const auto& m : *markerList)
        {
            const double x = timeToX (m.start);
            if (x < -50 || x > getWidth() + 50)
                continue;
            g.setColour (m.colour.withAlpha (0.8f));
            if (m.end < 0)
            {
                g.drawVerticalLine ((int) x, 0.0f, (float) getHeight());
            }
            else
            {
                const double x2 = timeToX (m.end);
                g.drawLine ((float) x, 6, (float) x2, 6, 2.0f);
                g.drawLine ((float) x, 6, (float) x, getHeight(), 1.0f);
                g.drawLine ((float) x2, 6, (float) x2, getHeight(), 1.0f);
            }
            g.setFont (10.0f);
            g.setColour (m.colour);
            g.drawText (m.name, (int) x + 4, 8, 120, 14, juce::Justification::topLeft, false);
        }
    }

    // Playhead
    const double px = timeToX (playhead);
    if (px >= kLeftAxisWidth && px <= getWidth())
    {
        g.setColour (juce::Colour::fromRGB (255, 60, 60));
        g.drawVerticalLine ((int) px, 0.0f, (float) getHeight());
    }
}
