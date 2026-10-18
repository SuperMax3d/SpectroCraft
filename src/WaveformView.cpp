/**
 * SpectroCraft - Waveform View Implementation
 * Own time window + vertical zoom (independent of the spectral view),
 * left amplitude axis (dBFS) and top time ruler.
 */

#include "WaveformView.h"
#include <algorithm>
#include <cmath>

WaveformView::WaveformView (Application& appIn)
    : app (appIn)   // audio data is read live via app.audioEngine() (active doc)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
    // Grab keyboard focus when clicked so the arrow keys keep controlling
    // the playhead/selection even after the user has interacted with other
    // widgets (combo boxes, text fields, ...). The view itself never consumes
    // key events, so they bubble up to MainWindow's KeyListener.
    setWantsKeyboardFocus (true);
}

WaveformView::~WaveformView() {}

double WaveformView::xToTime (int x) const
{
    const auto pr = plotRect();
    if (pr.getWidth() <= 0 || viewEnd <= viewStart) return viewStart;
    const double t = ((double) std::clamp (x, pr.getX(), pr.getRight()) - (double) pr.getX())
                     / (double) pr.getWidth() * (viewEnd - viewStart);
    return viewStart + t;
}

double WaveformView::timeToX (double t) const
{
    const auto pr = plotRect();
    const double w = viewEnd - viewStart;
    if (w <= 0 || pr.getWidth() <= 0) return (float) pr.getX();
    return (float) pr.getX() + (t - viewStart) / w * (double) pr.getWidth();
}

void WaveformView::setVisibleChannels (const std::vector<int>& chs)
{
    std::vector<uint8_t> newVis (std::max (1, app.audioEngine().numChannels()), 0);
    for (int c : chs)
        if (c >= 0 && c < (int) newVis.size())
            newVis[c] = 1;
    if (newVis != chanVis) { chanVis = std::move (newVis); dirty = true; repaint(); }
}

void WaveformView::resized() { dirty = true; }

// ---------------------------------------------------------------- interaction

// Build and show the right-click context menu (zoom + edit actions).
// Called from a mouse handler, so withMousePosition() lands exactly where clicked.
static void showViewContextMenu (Application& app,
                                 const std::function<void(int)>& onAction)
{
    if (!onAction) return;

    juce::PopupMenu m;
    using enum ContextMenuIDs;
    const bool hasSel = !app.selection.isEmpty();
    m.addItem (CtxZoomToSelection, "Zoom to Selection", hasSel, false);
    m.addItem (CtxZoomOutFull,     "Zoom Out Full",
               app.audioEngine().isFileLoaded(), false);
    m.addItem (CtxSelectAll,       "Select All\tCtrl+A",
               app.audioEngine().isFileLoaded(), false);
    m.addSeparator();
    m.addItem (CtxCut,   "Cut\tCtrl+X",   hasSel, false);
    m.addItem (CtxCopy,  "Copy\tCtrl+C",  hasSel, false);
    m.addItem (CtxPaste, "Paste\tCtrl+V", app.audioEngine().canPaste(), false);
    m.addItem (CtxDelete,"Delete\tDel",   hasSel, false);

    if (const int sel = m.showMenu (juce::PopupMenu::Options().withMousePosition());
        sel != 0)
        onAction (sel);
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    if (!app.audioEngine().isFileLoaded()) return;

    // Right button: box-zoom drag. A plain right-click (no significant
    // movement) still shows the context menu, deferred to mouseUp.
    if (e.mods.isRightButtonDown() && !e.mods.isLeftButtonDown())
    {
        dragMode = DragMode::ZoomDrag;
        dragStartX = e.x;
        dragStartY = e.y;
        haveZoomRect = false;
        return;   // right-button never moves the playhead
    }

    dragStartX = e.x;
    dragStartY = e.y;
    dragStartTime = xToTime (e.x);

    const bool leftOnAxis = e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown()
                            && !e.mods.isMiddleButtonDown() && e.x < kLeftAxisWidth;

    if (!leftOnAxis)
    {
        // Clicking anywhere in the plot moves the playhead to that position
        // (clamped; also updates the transport position without starting playback).
        app.setPlayhead (dragStartTime);
        setPlayhead (app.playhead);
    }

    if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown() && !e.mods.isMiddleButtonDown())
    {
        if (leftOnAxis)
        {
            // Left-drag on the amplitude axis pans the vertical range; it must
            // not move the playhead.
            dragMode = DragMode::VPan;
        }
        else
        {
            dragMode = DragMode::Select;
            app.setSelection ({ dragStartTime, dragStartTime });
            selection = app.selection;
        }
    }
    else if (e.mods.isMiddleButtonDown() || e.mods.isRightButtonDown())
    {
        dragMode = DragMode::Pan;
    }

    // Push the new playhead/selection to both views.
    if (onChange) onChange (viewStart, viewEnd);
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (!app.audioEngine().isFileLoaded()) return;

    switch (dragMode)
    {
        case DragMode::Select:
        {
            const double t = xToTime (e.x);
            // snapEdges: with "Snap to Zero Crossings" enabled, both edges
            // land on the nearest zero crossing of the first visible channel.
            app.setSelection ({ std::min (dragStartTime, t), std::max (dragStartTime, t) }, true);
            selection = app.selection;
            if (onChange) onChange (viewStart, viewEnd);
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
        case DragMode::VPan:
        {
            // Grab-and-drag: the amplitude value under the cursor at drag start
            // stays under the cursor. All rows share the same height, so one
            // pixels-per-unit scale applies to the whole view.
            const double ampPx = rowAmpPx();
            vOffset = std::clamp (vOffset + (dragStartY - e.y) / ampPx, -2.0, 2.0);
            dirty = true;
            if (onVerticalZoomChanged) onVerticalZoomChanged();
            repaint();
            break;
        }
        case DragMode::ZoomDrag:
        {
            const int x = std::clamp (e.x, 0, getWidth());
            const int y = std::clamp (e.y, 0, getHeight());
            if (!haveZoomRect && (std::abs (x - dragStartX) >= 4 || std::abs (y - dragStartY) >= 4))
                haveZoomRect = true;
            if (haveZoomRect)
            {
                zoomRect = juce::Rectangle<int> (std::min (dragStartX, x), std::min (dragStartY, y),
                                                 std::abs (x - dragStartX), std::abs (y - dragStartY));
                repaint();
            }
            break;
        }
        default: break;
    }
}

void WaveformView::mouseUp (const juce::MouseEvent&)
{
    if (dragMode == DragMode::ZoomDrag)
    {
        if (!haveZoomRect)
        {
            // Right-clicking the left amplitude axis makes "Zoom Out Full"
            // reset the vertical range instead of the time window.
            const bool onAxis = dragStartX < kLeftAxisWidth;
            // Only show the menu when there is something to do with it: an
            // external action callback, or a local axis reset. (Headless test
            // views have neither and must not enter a modal loop.)
            if (onAxis || onContextAction)
                showViewContextMenu (app, [this, onAxis] (int id)
                {
                    if (id == CtxZoomOutFull && onAxis) { resetVerticalZoom(); return; }
                    if (onContextAction) onContextAction (id);
                });
        }
        else if (zoomRect.getWidth() >= zoomRect.getHeight())
        {
            // Horizontal drag: zoom BOTH views to the dragged time range
            // (setWindow notifies MainWindow via onChange).
            const double t0 = xToTime (zoomRect.getX());
            const double t1 = xToTime (zoomRect.getRight());
            if (t1 - t0 > 1e-9) setWindow (t0, t1);
        }
        else
        {
            // Vertical drag: scale the amplitude axis so the dragged span
            // fills the whole plot, keeping the value under the box centre at
            // its row's centre line.
            const auto pr = plotRect();
            int visCount = 0;
            for (int ch = 0; ch < app.audioEngine().numChannels(); ++ch)
                if (ch < (int) chanVis.size() && chanVis[ch]) ++visCount;
            if (visCount > 0 && zoomRect.getHeight() >= 2)
            {
                const double oldAmpPx = rowAmpPx();
                const int chanH = pr.getHeight() / visCount;
                const int rowIdx = std::clamp ((zoomRect.getCentreY() - pr.getY()) / chanH,
                                               0, visCount - 1);
                const float centerR = (float) (pr.getY() + rowIdx * chanH + chanH * 0.5f);

                vOffset = std::clamp (vOffset + (centerR - zoomRect.getCentreY()) / oldAmpPx,
                                     -2.0, 2.0);
                const double nz = std::clamp (vZoom * ((double) pr.getHeight()
                                                      / (double) zoomRect.getHeight()),
                                              0.05, 64.0);
                if (nz != vZoom) { vZoom = nz; dirty = true; }
                if (onVerticalZoomChanged) onVerticalZoomChanged();
            }
        }
        haveZoomRect = false;
        zoomRect = {};
        repaint();
    }
    else if (dragMode == DragMode::Select && app.selection.getLength() <= 1e-6)
    {
        app.setSelection ({ 0, 0 }); // plain click clears the selection
        selection = app.selection;
        repaint();
        if (onChange) onChange (viewStart, viewEnd); // push the cleared selection to both views
    }
    dragMode = DragMode::None;
}

// Status-bar probe: time + amplitude under the cursor.
void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    if (!onCursorInfo) return;
    const auto pr = plotRect();
    if (e.x < pr.getX() || e.x >= pr.getRight()
        || e.y < pr.getY() || e.y >= pr.getBottom())
    {
        if (onCursorInfo) onCursorInfo ("");
        return;
    }

    const int chans = app.audioEngine().numChannels();
    int visCount = 0;
    for (int ch = 0; ch < chans; ++ch)
        if (ch < (int) chanVis.size() && chanVis[ch]) ++visCount;
    if (visCount <= 0) { onCursorInfo (""); return; }

    const int chanH = pr.getHeight() / visCount;
    const int row = std::clamp ((e.y - pr.getY()) / chanH, 0, visCount - 1);

    // Map the visible-row index back to its channel number.
    int ch = -1, r = 0;
    for (int c = 0; c < chans && ch < 0; ++c)
        if (c < (int) chanVis.size() && chanVis[c])
        {
            if (r == row) ch = c;
            ++r;
        }

    const double t = xToTime ((int) e.x);
    const float center = pr.getY() + row * chanH + chanH * 0.5f;
    const float ampPx = chanH * 0.45f * (float) vZoom;   // pixels per unit amplitude
    const double value = (ampPx > 1e-6)
        ? vOffset + ((double) center - e.y) / ampPx : 0.0;

    const juce::String lab = (chans == 2) ? (ch == 0 ? "L" : "R")
                                          : juce::String (ch + 1);
    if (onCursorInfo)
        onCursorInfo ("t=" + juce::String (t, 3) + " s   ch " + lab
                      + ": " + ScaleCalibration::formatLinearValue (value, app.scale()));
}

void WaveformView::mouseExit (const juce::MouseEvent&)
{
    if (onCursorInfo) onCursorInfo ("");
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!app.audioEngine().isFileLoaded()) return;
    const double factor = wheel.deltaY > 0 ? 1.2 : (1.0 / 1.2);

    // Wheel over the left amplitude axis (or Shift+wheel anywhere) zooms vertically;
    // otherwise it zooms in time around the cursor.
    if (e.mods.isShiftDown() || e.x < kLeftAxisWidth)
        zoomVerticalBy (factor);
    else
        zoomAtX (e.x, factor); // notifies via setWindow()

    repaint();
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Only left-button double-clicks select; middle/right are pan/menu buttons.
    if (!e.mods.isLeftButtonDown() || !app.audioEngine().isFileLoaded())
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
                if (onChange) onChange (viewStart, viewEnd); // sync both views via MainWindow
                return;
            }
        }
    }

    // Double-click selects the whole file and fits it to the view.
    app.selectAll();
    setSelection (app.selection); // local copy for immediate repaint
    fitAll(); // notifies via setWindow() when the range changes
}

// ------------------------------------------------------- independent zooming

void WaveformView::zoomAtX (int x, double factor)
{
    const auto pr = plotRect();
    if (pr.getWidth() <= 0 || viewEnd <= viewStart) return;

    const double tCursor = xToTime (x);
    const double newHalf = (viewEnd - viewStart) * 0.5 / factor;
    const double ratio = std::clamp ((double) (x - pr.getX()) / (double) pr.getWidth(), 0.0, 1.0);

    double ns = tCursor - newHalf * (2.0 * ratio);
    double ne = tCursor + newHalf * (2.0 - 2.0 * ratio);
    const double w = ne - ns;
    if (ns < 0.0) { ns = 0.0; ne = std::min (w, app.audioEngine().duration()); }
    if (ne > app.audioEngine().duration()) { ne = app.audioEngine().duration(); ns = std::max (0.0, ne - w); }
    if (ne <= ns) return; // zoomed past the limits

    setWindow (ns, ne);
}

void WaveformView::zoomHorizontalAtCenter (double factor)
{
    const auto pr = plotRect();
    zoomAtX (pr.getX() + pr.getWidth() / 2, factor);
}

void WaveformView::zoomVerticalBy (double factor)
{
    const double nz = std::clamp (vZoom * factor, 0.05, 64.0);
    if (nz != vZoom) { vZoom = nz; dirty = true; }
    if (onVerticalZoomChanged) onVerticalZoomChanged();
}

void WaveformView::fitAll()
{
    resetVerticalZoom();
    setWindow (0.0, app.audioEngine().duration());
}

void WaveformView::resetVerticalZoom()
{
    const bool changed = (vZoom != 1.0 || vOffset != 0.0);
    vZoom = 1.0;
    vOffset = 0.0;
    dirty = true;
    if (changed && onVerticalZoomChanged) onVerticalZoomChanged();
}

void WaveformView::panByTime (double dt)
{
    const double dur = app.audioEngine().duration();
    const double w = viewEnd - viewStart;
    const double ns = std::clamp (viewStart + dt, 0.0, std::max (0.0, dur - w));
    setWindow (ns, ns + w);
}

// ------------------------------------------------------------------- painting

void WaveformView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    g.fillAll (juce::Colour::fromRGB (30, 30, 30));

    const int chans = app.audioEngine().numChannels();
    if (chans == 0 || !app.audioEngine().isFileLoaded())
    {
        g.setColour (juce::Colour::fromRGBA (200, 200, 200, 120));
        g.setFont (14.0f);
        g.drawText ("No audio file loaded - use File > Open or drag & drop a WAV/FLAC/MP3/Ogg",
                    bounds, juce::Justification::centred, true);
        return;
    }

    const auto pr = plotRect();
    const int waveH = pr.getHeight();

    // Count visible channels for layout
    int visCount = 0;
    for (int ch = 0; ch < chans; ++ch)
        if (ch < (int) chanVis.size() && chanVis[ch]) ++visCount;
    const int chanH = waveH / std::max (1, visCount);

    // Channel labels on the left of each track: "L"/"R" for stereo files,
    // channel numbers when several tracks are shown at once.
    const bool showChanLabels = (visCount > 1 || chans == 2);

    int row = 0;
    for (int ch = 0; ch < chans; ++ch)
    {
        if (ch >= (int) chanVis.size() || !chanVis[ch]) continue;
        const int chanY = pr.getY() + row * chanH;
        const float center = chanY + chanH * 0.5f;
        const float amp = chanH * 0.45f * (float) vZoom;
        g.setColour (juce::Colour::fromRGB (60, 60, 60));
        g.drawHorizontalLine ((int) center, (float) pr.getX(), (float) pr.getRight());
        paintChannel (g, ch, chanY, chanH, center, amp);

        if (showChanLabels)
        {
            const juce::String lab = (chans == 2) ? (ch == 0 ? "L" : "R") : juce::String (ch + 1);
            g.setFont (10.0f);
            g.setColour (juce::Colour::fromRGB (190, 190, 190));
            g.drawText (lab, 2, (int) center - 8, kLeftAxisWidth - 4, 16,
                       juce::Justification::centredRight, false);
        }
        ++row;
    }

    drawAmplitudeAxis (g, visCount, chanH, showChanLabels);
    drawRuler (g);

    // Overlays are drawn ON TOP of the waveform so they stay visible
    // (identical behaviour to the spectral view).

    // Time selection highlight — blue fill with solid edge lines so it stays
    // clearly visible even over the teal waveform.
    if (!selection.isEmpty())
    {
        const double x1 = timeToX (selection.getStart());
        const double x2 = timeToX (selection.getEnd());
        g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
        g.fillRect ((float) x1, (float) pr.getY(),
                   (float) std::max (0.0, x2 - x1), (float) waveH);
        g.setColour (juce::Colour::fromRGB (96, 150, 255));
        g.drawVerticalLine ((float) x1, (float) pr.getY(), (float) pr.getBottom());
        g.drawVerticalLine ((float) x2, (float) pr.getY(), (float) pr.getBottom());
    }

    // Markers (same rendering as the spectral view)
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
                g.drawVerticalLine ((int) x, (float) kRulerH, (float) getHeight());
            }
            else
            {
                const double x2 = timeToX (m.end);
                g.drawLine ((float) x, (float) kRulerH + 6, (float) x2, (float) kRulerH + 6, 2.0f);
                g.drawLine ((float) x, (float) kRulerH + 6, (float) x, getHeight(), 1.0f);
                g.drawLine ((float) x2, (float) kRulerH + 6, (float) x2, getHeight(), 1.0f);
            }
            g.setFont (10.0f);
            g.setColour (m.colour);
            g.drawText (m.name, (int) x + 4, kRulerH + 8, 120, 14, juce::Justification::topLeft, false);
        }
    }

    // Playhead
    const double px = timeToX (playhead);
    if (px >= pr.getX() && px <= pr.getRight())
    {
        g.setColour (juce::Colour::fromRGB (255, 60, 60));
        g.drawVerticalLine ((int) px, 0.0f, (float) bounds.getHeight());
        juce::Path tri;
        tri.addTriangle ((float) px - 5, (float) kRulerH, (float) px + 5, (float) kRulerH,
                        (float) px, 0.0f);
        g.fillPath (tri);
    }

    // Right-drag box-zoom rubber band.
    if (haveZoomRect && dragMode == DragMode::ZoomDrag)
    {
        g.setColour (juce::Colour::fromRGBA (255, 255, 255, 36));
        g.fillRoundedRectangle (zoomRect.toFloat(), 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawRoundedRectangle (zoomRect.toFloat().expanded (-0.5f), 4.0f, 1.0f);
    }
}

// Fill the min/max envelope of one channel row: for every pixel column take
// the sample min/max, then fill between the top edge (left->right) and the
// bottom edge (right->left). This is the classic DAW min/max envelope waveform.
static void fillEnvelope (juce::Graphics& g, const std::vector<float>& tops,
                          const std::vector<float>& bots, float rowTop, float rowBottom)
{
    if (tops.empty() || tops.size() != bots.size())
        return;

    juce::Path path;
    bool started = false;
    for (size_t x = 0; x < tops.size(); ++x)
    {
        float t = std::clamp (tops[x], rowTop, rowBottom);
        float b = std::clamp (bots[x], rowTop, rowBottom);
        if (b < t) std::swap (t, b);
        const float px = (float) x + 0.5f;
        if (!started) { path.startNewSubPath (px, t); started = true; }
        else          path.lineTo (px, t);
    }
    for (size_t x = tops.size(); x-- > 0;)
    {
        float t = std::clamp (tops[x], rowTop, rowBottom);
        float b = std::clamp (bots[x], rowTop, rowBottom);
        if (b < t) std::swap (t, b);
        path.lineTo ((float) x + 0.5f, b);
    }
    path.closeSubPath();
    g.fillPath (path);
}

void WaveformView::paintChannel (juce::Graphics& g, int ch, int chanY, int chanH,
                                 float center, float amp)
{
    const auto pr = plotRect();
    const int w = std::max (1, pr.getWidth());
    const double rate = app.audioEngine().sampleRate();
    const int64_t s0 = std::max ((int64_t) 0, (int64_t) (viewStart * rate));
    const int64_t s1 = std::min (app.audioEngine().numSamples(), (int64_t) (viewEnd * rate));
    if (s1 <= s0)
        return;

    g.setColour (juce::Colour::fromRGB (0, 212, 170));

    const float rowTop = (float) chanY, rowBottom = (float) (chanY + chanH);

    // When zoomed in to roughly one-to-few samples per pixel column, draw the
    // individual samples as a connected polyline so the true sample shape is
    // visible instead of blocky min/max columns (classic DAW sample-level view).
    const double samplesPerColumn = (double) (s1 - s0) / w;
    if (samplesPerColumn <= 4.0)
    {
        const int64_t n = s1 - s0;
        std::vector<float> buf ((size_t) n);
        if (!app.audioEngine().readSamples (ch, s0, n, buf.data()))
            return;

        juce::Path path;
        bool started = false;
        for (int64_t i = 0; i < n; ++i)
        {
            const float x = (float) pr.getX() + ((float) (i + 0.5) / (float) n) * (float) w;
            const float y = std::clamp ((float) (center + (vOffset - buf[i]) * amp), rowTop, rowBottom);
            if (!started) { path.startNewSubPath (x, y); started = true; }
            else           path.lineTo (x, y);
        }
        g.strokePath (path, juce::PathStrokeType (1.0f));
        return;
    }

    // Initialise to centre so out-of-range columns render as a flat line
    std::vector<float> tops ((size_t) w, center), bots ((size_t) w, center);

    // Pick the finest mipmap level that still covers >= samples per column
    const AudioEngine::MipLevel* best = nullptr;
    for (const auto& lv : app.audioEngine().mipmaps())
        if ((double) lv.factor >= samplesPerColumn) { best = &lv; break; }

    if (best && ch < (int) best->min.size() && !best->min[ch].empty())
    {
        const int64_t f = best->factor;
        for (int x = 0; x < w; ++x)
        {
            const int64_t idx = s0 + (int64_t) ((double) x / w * (s1 - s0));
            const int64_t mi = idx / f;
            if (mi < 0 || mi >= (int64_t) best->min[ch].size())
                continue; // stays at centre -> flat line
            const float mn = best->min[ch][mi], mx = best->max[ch][mi];
            tops[x] = center + (vOffset - mx) * amp;
            bots[x] = center + (vOffset - mn) * amp;
        }
    }
    else
    {
        // Zoomed in past finest level: read raw samples (O(n) total)
        const int64_t n = s1 - s0;
        std::vector<float> buf ((size_t) n);
        if (!app.audioEngine().readSamples (ch, s0, n, buf.data()))
            return;

        for (int x = 0; x < w; ++x)
        {
            const int64_t a = (int64_t) ((double) x / w * n);
            const int64_t b = std::max (a + 1, (int64_t) ((double) (x + 1) / w * n));
            float mn = 0.0f, mx = 0.0f;
            for (int64_t i = a; i < b && i < n; ++i)
            {
                mn = std::min (mn, buf[i]);
                mx = std::max (mx, buf[i]);
            }
            tops[x] = center + (vOffset - mx) * amp;
            bots[x] = center + (vOffset - mn) * amp;
        }
    }

    // Envelope x-coordinates are relative to the plot area: shift by kAxisW.
    g.saveState();
    g.addTransform (juce::AffineTransform::translation ((float) pr.getX(), 0.0f));
    fillEnvelope (g, tops, bots, rowTop, rowBottom);
    g.restoreState();
}

void WaveformView::drawAmplitudeAxis (juce::Graphics& g, int visCount, int chanH, bool showChanLabels)
{
    const auto pr = plotRect();
    if (pr.getHeight() <= 0) return;

    // dB grid levels relative to full scale (1.0). With a scale calibration the
    // labels show physical values in the file's unit instead of dB.
    static const double levels[] = { 0.0, -6.0, -12.0, -18.0, -24.0 };
    const ScaleCalibration& sc = app.scale();

    int row = 0;
    for (int ch = 0; ch < app.audioEngine().numChannels(); ++ch)
    {
        if (ch >= (int) chanVis.size() || !chanVis[ch]) continue;
        const float center = pr.getY() + row * chanH + chanH * 0.5f;
        const float ampScale = chanH * 0.45f * (float) vZoom;

        for (double dB : levels)
        {
            const double a = std::pow (10.0, dB / 20.0); // amplitude at this level
            for (int sign : { -1, +1 })
            {
                // Value +a sits above the centre line, value -a below it;
                // both are shifted by the vertical offset.
                const float y = center + (float) ((vOffset - sign * a) * ampScale);
                if (y < pr.getY() || y > pr.getBottom()) continue;
                g.setColour (juce::Colour::fromRGB (48, 48, 48));
                g.drawHorizontalLine ((int) y, (float) pr.getX(), (float) pr.getRight());
            }
        }

        // Labels only when rows are tall enough to fit them. The scale is
        // symmetric: the same dB values appear above AND below the centre line.
        if (chanH >= 36 && visCount <= 4)
        {
            g.setFont (9.0f);
            for (double dB : levels)
            {
                const double a = std::pow (10.0, dB / 20.0);

                // Upper side label (value +a above the centre line)
                const float yUp = center + (float) ((vOffset - a) * ampScale);
                if (yUp >= pr.getY() + 8 && yUp <= pr.getBottom()
                    && (!showChanLabels || std::abs (yUp - center) >= 9.0f))
                {
                    g.setColour (juce::Colour::fromRGB (170, 170, 170));
                    g.drawText (ScaleCalibration::formatWaveformAxisValue (dB, sc), 2, (int) yUp - 7,
                               kLeftAxisWidth - 4, 14, juce::Justification::centredRight, false);
                }

                // Lower side label (value -a below the centre line)
                const float yDown = center + (float) ((vOffset + a) * ampScale);
                if (yDown >= pr.getY() && yDown <= pr.getBottom() - 8
                    && (!showChanLabels || std::abs (yDown - center) >= 9.0f))
                {
                    g.setColour (juce::Colour::fromRGB (170, 170, 170));
                    g.drawText (ScaleCalibration::formatWaveformAxisValue (dB, sc), 2, (int) yDown - 7,
                               kLeftAxisWidth - 4, 14, juce::Justification::centredRight, false);
                }
            }
        }
        ++row;
    }

    // Axis title
    g.setFont (9.0f);
    g.setColour (juce::Colour::fromRGB (150, 150, 150));
    g.drawText (sc.unitTitle(), 2, pr.getY() + 2, kLeftAxisWidth - 4, 12, juce::Justification::centredLeft, false);
}

void WaveformView::drawRuler (juce::Graphics& g)
{
    const auto pr = plotRect();
    const double w = viewEnd - viewStart;
    if (w <= 0 || pr.getWidth() <= 0)
        return;

    double major = 1.0;
    const double pxPerSec = pr.getWidth() / w;
    if (pxPerSec < 20) major = 60;
    else if (pxPerSec < 50) major = 10;
    else if (pxPerSec < 100) major = 5;
    else if (pxPerSec < 500) major = 1;
    else if (pxPerSec < 2000) major = 0.1;
    else major = 0.01;

    g.setColour (juce::Colour::fromRGB (60, 60, 60));
    g.drawLine ((float) pr.getX(), kRulerH - 1, (float) pr.getRight(), kRulerH - 1, 1.0f);

    g.setFont (10.0f);
    const double start = std::ceil (viewStart / major) * major;
    for (double t = start; t <= viewEnd; t += major)
    {
        const float x = timeToX (t);
        if (x < pr.getX() || x > pr.getRight())
            continue;
        g.setColour (juce::Colour::fromRGB (60, 60, 60));
        g.drawLine (x, kRulerH - 8, x, kRulerH - 1, 1.0f);
        g.setColour (juce::Colour::fromRGB (200, 200, 200));
        juce::String label = major >= 1
            ? juce::String ((int) t)
            : juce::String (t, 3);
        g.drawText (label, (int) x + 3, 2, 90, kRulerH - 4, juce::Justification::centredLeft, false);
    }
}
