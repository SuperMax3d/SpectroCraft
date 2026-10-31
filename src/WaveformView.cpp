/**
 * SpectroCraft - Waveform View Implementation
 * Own time window + vertical zoom (independent of the spectral view),
 * left amplitude axis (dBFS) and top time ruler.
 */

#include "WaveformView.h"
#include <algorithm>
#include <cmath>

// Time-pan speed: content moves this many px per cursor pixel (0.5 = half
// speed, matching the amplitude-axis grab pan).
static constexpr double kPanSpeedFactor = 0.5;

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
    m.addItem (CtxCopyToNewFile, "Copy to New File",
               app.audioEngine().isFileLoaded()
                   && app.audioEngine().numSamples() > 0,
               false);
    m.addItem (CtxPaste, "Paste\tCtrl+V", app.canPaste(), false);
    m.addItem (CtxPasteMix, "Paste Mix\tCtrl+Shift+V", app.canPaste(), false);
    m.addItem (CtxDelete,"Delete\tDel",   hasSel, false);

    // Scale menu: independent time-axis and vertical-axis scales.
    juce::PopupMenu scaleMenu;
    {
        juce::PopupMenu timeAxis;
        timeAxis.addItem (CtxScaleTimeTime,     "Time",           true,
                          app.timeAxisMode == TimeAxisMode::Time);
        timeAxis.addItem (CtxScaleTimeOffset,   "Time Offset...", true,
                          app.timeAxisMode == TimeAxisMode::TimeOffset);
        timeAxis.addItem (CtxScaleTimeSample,   "Sample",         true,
                          app.timeAxisMode == TimeAxisMode::Sample);

        juce::PopupMenu vAxis;
        vAxis.addItem (CtxScaleWaveWavUnits, "Wav Units",  true, app.waveVScaleMode == 0);
        vAxis.addItem (CtxScaleWaveDb,       "dB",         true, app.waveVScaleMode == 1);
        vAxis.addItem (CtxScaleWaveNorm,     "Normalized", true, app.waveVScaleMode == 2);

        scaleMenu.addSubMenu ("Time Axis", timeAxis);
        scaleMenu.addSubMenu ("Vertical Axis", vAxis);
    }
    m.addSeparator();
    m.addSubMenu ("Scale", scaleMenu);

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
                            && !e.mods.isMiddleButtonDown() && onAxisX (e.x);
    const bool onTimeRuler = onRulerY (e.y);

    if (!leftOnAxis)
    {
        // Clicking anywhere in the plot moves the playhead to that position
        // (clamped; also updates the transport position without starting playback).
        app.setPlayhead (dragStartTime);
        setPlayhead (app.playhead);
    }

    if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown() && !e.mods.isMiddleButtonDown())
    {
        if (onTimeRuler)
        {
            // Left-drag on the top time ruler pans the shared time window
            // (same as middle-button drag); it does not select.
            dragMode = DragMode::TimePan;
            panLastX = e.x;
        }
        else if (leftOnAxis)
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
        panLastX = e.x;
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
        case DragMode::TimePan:
        {
            // Grab-and-drag at half speed (like the amplitude axis): content
            // follows the cursor but moves 0.5 px per cursor pixel, so panning
            // stays gentle even on wide windows. The delta is measured from
            // the last delivered position and that anchor always advances to
            // the cursor — deltas eaten by an edge clamp do not accumulate,
            // so reversing direction responds immediately instead of first
            // unwinding a dead zone (and dragging past the view's edge keeps
            // panning smoothly). setWindow() inside panByTime notifies MainWindow.
            const auto pr = plotRect();
            if (pr.getWidth() > 0)
                panByTime (-(e.x - panLastX) * kPanSpeedFactor
                           * (viewEnd - viewStart) / (double) pr.getWidth());
            panLastX = e.x;
            break;
        }
        case DragMode::VPan:
        {
            // Grab-and-drag: content follows the cursor (drag down -> the
            // waveform moves down). Panned at half speed so a full-scale view
            // does not saturate the +/-2 offset clamp after a few pixels.
            const double ampPx = rowAmpPx();
            vOffset = std::clamp (vOffset + 0.5 * (e.y - dragStartY) / ampPx, -2.0, 2.0);
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
            // Right-clicking the amplitude axis (left or right) makes
            // "Zoom Out Full" reset the vertical range instead of the time
            // window.
            const bool onAxis = onAxisX (dragStartX);
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
        else
        {
            // The drag START position selects which axes get zoomed:
            //  - inside the plot ("the track"): BOTH axes — time from the box's
            //    x span, amplitude from its y span;
            //  - on the amplitude axis: vertical only;
            //  - on the time ruler: horizontal only.
            const bool startOnAxis  = onAxisX (dragStartX);
            const bool startOnRuler = !startOnAxis && onRulerY (dragStartY);

            auto zoomTime = [this] ()
            {
                // Zoom BOTH views to the dragged time range (setWindow notifies
                // MainWindow via onChange).
                const double t0 = xToTime (zoomRect.getX());
                const double t1 = xToTime (zoomRect.getRight());
                if (t1 - t0 > 1e-9) setWindow (t0, t1);
            };

            auto zoomAmp = [this] ()
            {
                // Scale the amplitude axis so the dragged span fills the whole
                // plot, keeping the value under the box centre at its row's
                // centre line.
                const auto pr = plotRect();
                int visCount = 0;
                for (int ch = 0; ch < app.audioEngine().numChannels(); ++ch)
                    if (ch < (int) chanVis.size() && chanVis[ch]) ++visCount;
                if (visCount > 0 && zoomRect.getHeight() >= 2)
                {
                    const double oldAmpPx = rowAmpPx();
                    const auto rows = channelRowRects (pr.getWidth(), pr.getHeight(), visCount);
                    int rowIdx = 0;
                    for (int r = 0; r < visCount; ++r)
                        if (zoomRect.getCentreY() >= pr.getY() + rows[(size_t) r].getY())
                            rowIdx = r;
                    const auto& rrR = rows[(size_t) rowIdx];
                    const float centerR = (float) (pr.getY() + rrR.getY()
                                                   + rrR.getHeight() * 0.5f);

                    vOffset = std::clamp (vOffset + (centerR - zoomRect.getCentreY()) / oldAmpPx,
                                         -2.0, 2.0);
                    const double nz = std::clamp (vZoom * ((double) pr.getHeight()
                                                          / (double) zoomRect.getHeight()),
                                                  0.05, 64.0);
                    if (nz != vZoom) { vZoom = nz; dirty = true; }
                    if (onVerticalZoomChanged) onVerticalZoomChanged();
                }
            };

            if (startOnAxis)
                zoomAmp ();
            else if (startOnRuler)
                zoomTime ();
            else
            {
                zoomTime ();
                zoomAmp ();
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

    const auto rows = channelRowRects (pr.getWidth(), pr.getHeight(), visCount);
    // Row under the cursor (a point inside a gap maps to the row below it).
    int row = 0;
    for (int r = 0; r < visCount; ++r)
        if (e.y >= pr.getY() + rows[(size_t) r].getY())
            row = r;

    // Map the visible-row index back to its channel number.
    int ch = -1, r = 0;
    for (int c = 0; c < chans && ch < 0; ++c)
        if (c < (int) chanVis.size() && chanVis[c])
        {
            if (r == row) ch = c;
            ++r;
        }

    const double t = xToTime ((int) e.x);
    const auto& rr = rows[(size_t) row];
    const float center = pr.getY() + rr.getY() + rr.getHeight() * 0.5f;
    const float ampPx = rr.getHeight() * 0.45f * (float) vZoom;   // pixels per unit amplitude
    const double value = (ampPx > 1e-6)
        ? vOffset + ((double) center - e.y) / ampPx : 0.0;

    const juce::String lab = (chans == 2) ? (ch == 0 ? "L" : "R")
                                          : juce::String (ch + 1);

    // Time part follows the active time-axis scale mode.
    juce::String tText;
    switch (app.timeAxisMode)
    {
        case TimeAxisMode::Sample:     tText = "t=" + formatDisplayedTime (t, app.timeAxisMode,
                                                                           app.timeOffsetSecs,
                                                                           app.audioEngine().sampleRate())
                                             + " smp"; break;
        case TimeAxisMode::TimeOffset:
            tText = "t=" + formatDisplayedTime (t, app.timeAxisMode,
                                                 app.timeOffsetSecs,
                                                 app.audioEngine().sampleRate()); break;
        default:                       tText = "t=" + juce::String (t, 3) + " s"; break;
    }

    // Amplitude part follows the active waveform vertical scale mode.
    juce::String vText;
    switch (app.waveVScaleMode)
    {
        case 1:
            vText = (std::abs (value) > 1e-9)
                    ? juce::String (20.0 * std::log10 (std::abs (value)), 1) + " dB"
                    : "-inf dB";
            break;
        case 2:
            vText = juce::String (value, 3);
            break;
        default:
            vText = ScaleCalibration::formatLinearValue (value, app.scale(), ch);
            break;
    }

    // Alternative scale overlays: every enabled time reference, by name.
    juce::String altText;
    for (const auto& r : app.altTimeRefs)
        if (r.enabled && std::abs (r.refMain) > 1e-9)
            altText += "   | " + r.name + "=" + juce::String (t * r.altValue / r.refMain, 3);

    if (onCursorInfo)
        onCursorInfo (tText + "   ch " + lab + ": " + vText + altText);
}

void WaveformView::mouseExit (const juce::MouseEvent&)
{
    if (onCursorInfo) onCursorInfo ("");
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!app.audioEngine().isFileLoaded()) return;
    const double factor = wheel.deltaY > 0 ? 1.2 : (1.0 / 1.2);

    // Wheel over the amplitude axis (left or right, or Shift+wheel anywhere)
    // zooms vertically; otherwise it zooms in time around the cursor.
    if (e.mods.isShiftDown() || onAxisX (e.x))
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
    if (!changed) return;
    repaint();   // e.g. Ctrl+0 with the time window already full: no other setter
                 // fires, so this must schedule the re-render itself.
    if (onVerticalZoomChanged) onVerticalZoomChanged ();
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

    // Count visible channels for layout; rows are separated by a dark gap of
    // the axis colour (the background shows through between them).
    int visCount = 0;
    for (int ch = 0; ch < chans; ++ch)
        if (ch < (int) chanVis.size() && chanVis[ch]) ++visCount;
    const auto rows = channelRowRects (pr.getWidth(), waveH, visCount);

    int row = 0;
    std::vector<int> rowChans;   // channel index per visible row (per-channel scales)
    for (int ch = 0; ch < chans; ++ch)
    {
        if (ch >= (int) chanVis.size() || !chanVis[ch]) continue;
        rowChans.push_back (ch);
        const auto& rr = rows[(size_t) row];
        const int chanY = pr.getY() + rr.getY();
        const int chanH = rr.getHeight();
        const float center = chanY + chanH * 0.5f;
        const float amp = chanH * 0.45f * (float) vZoom;
        g.setColour (juce::Colour::fromRGB (60, 60, 60));
        g.drawHorizontalLine ((int) center, (float) pr.getX(), (float) pr.getRight());
        paintChannel (g, ch, chanY, chanH, center, amp);

        // Channel label on the left of every track: "L"/"R" for stereo files,
        // channel numbers otherwise. Shown even when a single channel is
        // visible so the axis always says which track it is.
        const juce::String lab = (chans == 2) ? (ch == 0 ? "L" : "R") : juce::String (ch + 1);
        g.setFont (10.0f);
        g.setColour (juce::Colour::fromRGB (190, 190, 190));
        if (axisOnRight())
            // Right edge of the axis strip: keeps the channel label clear of
            // the dB tick labels that start at its left edge.
            g.drawText (lab, getWidth() - kLeftAxisWidth + 4, (int) center - 8,
                       kLeftAxisWidth - 6, 16, juce::Justification::centredRight, false);
        else
            g.drawText (lab, 2, (int) center - 8, kLeftAxisWidth - 4, 16,
                       juce::Justification::centredRight, false);
        ++row;
    }

    drawAmplitudeAxis (g, rows, rowChans);
    drawRuler (g);

    // Overlays are drawn ON TOP of the waveform so they stay visible
    // (identical behaviour to the spectral view).

    // Time selection highlight — blue fill with solid edge lines so it stays
    // clearly visible even over the teal waveform. Clipped to the plot so an
    // edge before the visible window never paints over the amplitude axis.
    if (!selection.isEmpty())
    {
        const double x1 = timeToX (selection.getStart());
        const double x2 = timeToX (selection.getEnd());
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (pr);
        g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
        g.fillRect ((float) x1, (float) pr.getY(),
                   (float) std::max (0.0, x2 - x1), (float) waveH);
        g.setColour (juce::Colour::fromRGB (96, 150, 255));
        g.drawVerticalLine ((float) x1, (float) pr.getY(), (float) pr.getBottom());
        g.drawVerticalLine ((float) x2, (float) pr.getY(), (float) pr.getBottom());
    }

    // Markers (same rendering as the spectral view). Clipped to the plot so
    // lines and titles never paint over the left amplitude axis when a marker
    // starts before the visible window.
    if (markerList)
    {
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (pr);
        const auto& ms = *markerList;   // kept sorted by start time
        for (size_t i = 0; i < ms.size(); ++i)
        {
            const Marker& m = ms[i];
            const double x = timeToX (m.start);
            if (x < -50 || x > getWidth() + 50)
                continue;
            // The range bracket sits at the ruler edge: just below the top
            // ruler by default, or just above the bottom ruler when it is
            // moved there (View menu).
            const float brY = rulerAtBottom() ? pr.getBottom() - 14.0f : pr.getY() + 6.0f;
            g.setColour (m.colour.withAlpha (0.8f));
            if (m.end < 0)
            {
                g.drawVerticalLine ((int) x, (float) pr.getY(), (float) pr.getBottom());
            }
            else
            {
                const double x2 = timeToX (m.end);
                const float vEnd = rulerAtBottom() ? (float) pr.getY() : (float) pr.getBottom();
                g.drawLine ((float) x, brY, (float) x2, brY, 2.0f);
                g.drawLine ((float) x, brY, (float) x, vEnd, 1.0f);
                g.drawLine ((float) x2, brY, (float) x2, vEnd, 1.0f);
            }
            // Title: may run past this marker's own range, but must stop
            // before the next marker starts so that marker's title has room.
            g.setFont (10.0f);
            g.setColour (m.colour);
            const double textX = x + 4;
            const double rightEdge = (i + 1 < ms.size())
                ? timeToX (ms[i + 1].start) - 2.0
                : (double) getWidth() + 50.0;
            const double w = std::max (0.0, rightEdge - textX);
            const int titleY = rulerAtBottom() ? pr.getBottom() - 20 : pr.getY() + 8;
            if (w > 0.0)
                g.drawText (m.name, (int) textX, titleY, (int) w, 14,
                            juce::Justification::topLeft, true);
        }
    }

    // Playhead
    const double px = timeToX (playhead);
    if (px >= pr.getX() && px <= pr.getRight())
    {
        // Snap the line and the arrow to a common pixel centre. The line is a
        // 1px column [x, x+1); centring the triangle on that same column keeps
        // both slanted edges at mirror-image sub-pixel phases, so the arrow
        // rasterises symmetrically (a fractional apex would otherwise make one
        // wing visibly heavier than the other).
        const int pxI = (int) std::lround (px);
        g.setColour (juce::Colour::fromRGB (255, 60, 60));
        g.drawVerticalLine (pxI, 0.0f, (float) bounds.getHeight());
        // The arrow sits in the ruler strip on the playhead column: base on
        // the ruler/plot boundary, apex at the window's outer edge - pointing
        // up with a top ruler, down with a bottom ruler. With no ruler in this
        // view (stacked dual view) only the plain line is drawn.
        if (showTimeRuler)
        {
            juce::Path tri;
            if (rulerAtBottom())
                tri.addTriangle ((float) pxI - 4.5f, (float) pr.getBottom(),
                                 (float) pxI + 5.5f, (float) pr.getBottom(),
                                 (float) pxI + 0.5f, (float) bounds.getHeight());
            else
                tri.addTriangle ((float) pxI - 4.5f, (float) kRulerH,
                                 (float) pxI + 5.5f, (float) kRulerH,
                                 (float) pxI + 0.5f, 0.0f);
            g.fillPath (tri);
        }
    }

    // Right-drag box-zoom rubber band: the same translucent blue as the
    // time selection in the tracks, so it reads as "this region" rather
    // than a generic white marquee. When the drag starts on an axis the
    // band stretches across that axis' full width/height (matching which
    // axes mouseUp will actually zoom).
    if (haveZoomRect && dragMode == DragMode::ZoomDrag)
    {
        auto zr = zoomRect;
        const bool startOnAxis  = onAxisX (dragStartX);
        const bool startOnRuler = !startOnAxis && onRulerY (dragStartY);
        if (startOnAxis)
            zr = juce::Rectangle<int> (axisOnRight() ? getWidth() - kLeftAxisWidth : 0,
                                       zr.getY(), kLeftAxisWidth, zr.getHeight());
        else if (startOnRuler)
            zr = juce::Rectangle<int> (zr.getX(), rulerAtBottom() ? getHeight() - kRulerH : 0,
                                       zr.getWidth(), kRulerH);

        g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
        g.fillRect (zr);
        g.setColour (juce::Colour::fromRGB (96, 150, 255).withAlpha (0.9f));
        g.drawRect (zr, 1);
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

void WaveformView::drawAmplitudeAxis (juce::Graphics& g, const std::vector<juce::Rectangle<int>>& rows,
                                      const std::vector<int>& rowChans)
{
    const auto pr = plotRect();
    if (pr.getHeight() <= 0 || rows.empty()) return;

    // dB grid levels relative to full scale (1.0). With a scale calibration the
    // labels show physical values in the file's unit instead of dB.
    static const double levels[] = { 0.0, -6.0, -12.0, -18.0, -24.0 };
    const ScaleCalibration& sc = app.scale();

    // Label placement follows the axis side: right-aligned against the plot
    // edge on a left axis, left-aligned on a right one.
    const int labX = axisOnRight() ? getWidth() - kLeftAxisWidth + 2 : 2;
    const juce::Justification labJ = axisOnRight() ? juce::Justification::centredLeft
                                                   : juce::Justification::centredRight;

    for (size_t r = 0; r < rows.size(); ++r)
    {
        const auto& rr = rows[r];
        const float center = pr.getY() + rr.getY() + rr.getHeight() * 0.5f;
        const float ampScale = rr.getHeight() * 0.45f * (float) vZoom;

        // The grey horizontal grid lines can be hidden from the View menu;
        // the axis labels below are unaffected.
        if (app.settings.showAmpGrid)
        {
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
        }

        // Labels only when rows are tall enough to fit them. The scale is
        // symmetric: the same dB values appear above AND below the centre line.
        if (rr.getHeight() >= 36 && rows.size() <= 4)
        {
            g.setFont (9.0f);
            for (double dB : levels)
            {
                const double a = std::pow (10.0, dB / 20.0);

                // Upper side label (value +a above the centre line); keep it
                // clear of the channel label that sits at the row centre.
                const float yUp = center + (float) ((vOffset - a) * ampScale);
                if (yUp >= pr.getY() + 8 && yUp <= pr.getBottom()
                    && std::abs (yUp - center) >= 9.0f)
                {
                    g.setColour (juce::Colour::fromRGB (170, 170, 170));
                    const int chUp = (r < rowChans.size()) ? rowChans[r] : -1;
                    g.drawText (formatWaveVScaleLabel (dB, app.waveVScaleMode, sc, true, chUp),
                               labX, (int) yUp - 7,
                               kLeftAxisWidth - 4, 14, labJ, false);
                }

                // Lower side label (value -a below the centre line)
                const float yDown = center + (float) ((vOffset + a) * ampScale);
                if (yDown >= pr.getY() && yDown <= pr.getBottom() - 8
                    && std::abs (yDown - center) >= 9.0f)
                {
                    g.setColour (juce::Colour::fromRGB (170, 170, 170));
                    const int chDn = (r < rowChans.size()) ? rowChans[r] : -1;
                    g.drawText (formatWaveVScaleLabel (dB, app.waveVScaleMode, sc, false, chDn),
                               labX, (int) yDown - 7,
                               kLeftAxisWidth - 4, 14, labJ, false);
                }
            }
        }
    }

    // Axis title
    g.setFont (9.0f);
    g.setColour (juce::Colour::fromRGB (150, 150, 150));
    g.drawText (waveVScaleTitle (app.waveVScaleMode, sc), labX, pr.getY() + 2,
               kLeftAxisWidth - 4, 12,
               axisOnRight() ? juce::Justification::centredRight
                             : juce::Justification::centredLeft, false);
}

// ---------------------------------------------------------------- time axis

double niceTimeStep (double pxPerSec)
{
    if (!(pxPerSec > 0.0)) return 1.0;
    const double target = 80.0 / pxPerSec;   // seconds per ~80 px of ruler

    // Below one minute: classic 1-2-5 metric steps in seconds, from whole
    // seconds down to microseconds at deep zooms.
    if (target < 60.0)
    {
        double p = std::pow (10.0, std::floor (std::log10 (target)));
        for (int d = 0; d < 3 && p < 60.0; ++d)
        {
            for (double m : { 1.0, 2.0, 5.0 })
                if (m * p >= target - 1e-12) return m * p;
            p *= 10.0;
        }
    }

    // One minute and above: time-unit steps that always yield clean labels
    // ("1m", "2m", "5m", "10m", "15m", "30m", "1h", ...).
    static const double timeSteps [] = { 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0,
                                         3600.0, 7200.0, 18000.0, 36000.0, 54000.0,
                                         108000.0, 216000.0, 432000.0, 864000.0 };
    for (double s : timeSteps)
        if (s >= target - 1e-9) return s;

    // Multi-day files: fall back to metric hours/days.
    double p = std::pow (10.0, std::floor (std::log10 (target)));
    for (int d = 0; d < 4; ++d)
    {
        for (double m : { 1.0, 2.0, 5.0 })
            if (m * p >= target - 1e-9) return m * p;
        p *= 10.0;
    }

    return 864000.0;   // unreachable for sane inputs
}

juce::String formatClockTime (double t, int maxDecimals)
{
    if (!std::isfinite (t)) t = 0.0;
    const bool neg = t < 0.0;   // negative displayed times (negative offsets)
    t = std::abs (t);

    // Round the fractional part to `maxDecimals` digits first so a carry can
    // bump the whole-second count (e.g. 155.99996 @4dp -> 2:36). With no
    // fraction requested, round the whole value instead.
    double wholeD = (maxDecimals > 0) ? std::floor (t) : std::llround (t);
    double frac = t - wholeD;
    if (maxDecimals > 0 && frac > 0.0)
    {
        const double scale = std::pow (10.0, maxDecimals);
        frac = std::round (frac * scale) / scale;
        if (frac >= 1.0 - 1e-12) { wholeD += 1.0; frac = 0.0; }
    }

    const int64_t whole = (int64_t) std::llround (wholeD);
    const int h = (int) (whole / 3600), m = (int) ((whole % 3600) / 60), sec = (int) (whole % 60);

    // Hours appear only once the value reaches one hour, so a 30-minute tick
    // in a two-hour ruler reads "30:00" next to "1:00:00".
    juce::String s;
    if (h > 0)
        s << h << ":" << juce::String (m).paddedLeft ('0', 2) << ":"
          << juce::String (sec).paddedLeft ('0', 2);
    else
        s << m << ":" << juce::String (sec).paddedLeft ('0', 2);

    if (maxDecimals > 0 && frac > 0.0)
    {
        // "0.1270" -> "1270" -> trim trailing zeros -> "127"
        juce::String fs = juce::String (frac, maxDecimals).substring (2);
        while (fs.endsWith ("0")) fs = fs.dropLastCharacters (1);
        if (!fs.isEmpty()) s << "." << fs;
    }
    return neg ? "-" + s : s;
}

juce::String formatFooterTime (double t)
{
    if (!std::isfinite (t) || t < 0.0) t = 0.0;
    if (t >= 60.0) return formatClockTime (t, 4);
    return juce::String (t, 3);
}

bool parseTimeText (const juce::String& sIn, double& out)
{
    const auto s = sIn.trim();
    if (s.isEmpty()) { out = 0.0; return false; }

    // Split on ':' - at most two separators are allowed.
    juce::StringArray parts;
    int start = 0;
    for (int i = 0; ; ++i)
    {
        const bool end = (i >= s.length());
        if (!end && s[i] != ':') continue;

        // Note: this build's substring takes (startIndex, endIndex).
        const auto part = s.substring (start, i);
        if (part.isEmpty()) { out = 0.0; return false; }
        // Digits with at most one decimal point.
        int dots = 0;
        for (int j = 0; j < part.length(); ++j)
        {
            const juce_wchar c = part[j];
            if (c == L'.') { if (++dots > 1) { out = 0.0; return false; } }
            else if (c < L'0' || c > L'9') { out = 0.0; return false; }
        }
        parts.add (part);
        if (end) break;
        start = i + 1;
    }
    if (parts.size() > 3) { out = 0.0; return false; }

    double v = 0.0;
    try
    {
        for (const auto& p : parts)
            v = v * 60.0 + std::stod (p.toRawUTF8());
    }
    catch (...) { out = 0.0; return false; }

    if (!std::isfinite (v) || v < 0.0) { out = 0.0; return false; }
    out = v;
    return true;
}

juce::String formatOffsetTime (double t)
{
    // Always "hh:mm:ss.xxxx" with exactly four fractional digits: the Time
    // Offset prompt and its readouts use this fixed shape.
    if (!std::isfinite (t)) t = 0.0;
    const bool neg = t < 0.0;
    t = std::abs (t);
    const int64_t whole = (int64_t) std::llround (std::floor (t));
    const double frac = t - std::floor (t);
    const int h = (int) (whole / 3600), m = (int) ((whole % 3600) / 60), s = (int) (whole % 60);
    juce::String fs = juce::String (frac, 4).substring (2);   // "1270"
    while (fs.length() < 4) fs = "0" + fs;
    juce::String out;
    if (neg) out << "-";
    if (h > 0)
        out << h << ":" << juce::String (m).paddedLeft ('0', 2) << ":"
            << juce::String (s).paddedLeft ('0', 2);
    else
        out << m << ":" << juce::String (s).paddedLeft ('0', 2);
    return out << "." << fs;
}

bool parseSignedClock (const juce::String& sIn, double& outSecs)
{
    auto s = sIn.trim();
    bool neg = false;
    if (!s.isEmpty() && (s[0] == L'-' || s[0] == L'+'))
    {
        neg = (s[0] == L'-');
        s = s.substring (1);
    }
    double v = 0.0;
    if (!parseTimeText (s, v)) return false;
    outSecs = neg ? -v : v;
    return true;
}

double timeToDisplayValue (double tSecs, TimeAxisMode m, double offsetSecs, double rate)
{
    switch (m)
    {
        case TimeAxisMode::TimeOffset: return tSecs + offsetSecs;
        case TimeAxisMode::Sample:
            return tSecs * std::max (1.0, rate);
        default:                       return tSecs;
    }
}

juce::String formatDisplayedTime (double tSecs, TimeAxisMode m, double offsetSecs, double rate)
{
    switch (m)
    {
        case TimeAxisMode::TimeOffset:
            return formatOffsetTime (tSecs + offsetSecs);
        case TimeAxisMode::Sample:
            return juce::String ((int64_t) std::llround (timeToDisplayValue (tSecs, m, offsetSecs, rate)));
        default:
            return formatFooterTime (tSecs);
    }
}

bool parseDisplayedTime (const juce::String& s, TimeAxisMode m, double offsetSecs,
                         double rate, double& outSecs)
{
    switch (m)
    {
        case TimeAxisMode::Sample:
        {
            // Integer or decimal sample count (sign allowed).
            auto t = s.trim();
            if (t.isEmpty()) { outSecs = 0.0; return false; }
            bool neg = false;
            if (t[0] == L'-' || t[0] == L'+')
            {
                neg = (t[0] == L'-');
                t = t.substring (1);
            }
            int dots = 0, digits = 0;
            for (int i = 0; i < t.length(); ++i)
            {
                const juce_wchar c = t[i];
                if (c == L'.') { if (++dots > 1) { outSecs = 0.0; return false; } }
                else if (c < L'0' || c > L'9') { outSecs = 0.0; return false; }
                else ++digits;
            }
            if (dots == 0 && digits == 0) { outSecs = 0.0; return false; }
            double v = t.getDoubleValue();
            if (!std::isfinite (v)) { outSecs = 0.0; return false; }
            const double r = std::max (1.0, rate);
            outSecs = (neg ? -v : v) / r;
            return true;
        }
        case TimeAxisMode::TimeOffset:
        {
            double v = 0.0;
            if (!parseSignedClock (s, v)) { outSecs = 0.0; return false; }
            outSecs = v - offsetSecs;
            return true;
        }
        default:
        {
            double v = 0.0;
            if (!parseSignedClock (s, v)) { outSecs = 0.0; return false; }
            outSecs = v;
            return true;
        }
    }
}

double niceSampleStep (double samplesPerPixel)
{
    // Same 1-2-5 metric idea as niceTimeStep(), but in the sample domain and
    // never below one sample.
    const double target = std::max (1e-9, samplesPerPixel * 80.0);
    const int e = (int) std::floor (std::log10 (target));
    for (int k = -2; k < 5; ++k)
        for (double m : { 1.0, 2.0, 5.0 })
        {
            const double step = m * std::pow (10.0, e + k);
            if (step >= target) return std::max (1.0, step);
        }
    return std::max (1.0, std::pow (10.0, e + 5));
}

juce::String formatWaveVScaleLabel (double gridDb, int mode, const ScaleCalibration& sc,
                                    bool positiveSide, int ch)
{
    switch (mode)
    {
        case 1:   // dB: plain relative level.
            return juce::String ((int) std::lround (gridDb));
        case 2:
        {
            // Normalized: signed linear amplitude at this grid line
            // (+a above the centre line, -a below it).
            const double a = std::pow (10.0, gridDb / 20.0);
            juce::String s = juce::String (std::abs (a), 3);
            while (s.length() > 2 && s.endsWith ("0")) s = s.dropLastCharacters (1);
            if (s.endsWith (".")) s = s.dropLastCharacters (1);
            return (positiveSide ? "+" : "-") + s;
        }
        default:
            // Wav Units: physical value when calibrated, relative dB otherwise.
            return ScaleCalibration::formatWaveformAxisValue (gridDb, sc, ch);
    }
}

juce::String waveVScaleTitle (int mode, const ScaleCalibration& sc)
{
    switch (mode)
    {
        case 1: return "dB";
        case 2: return "norm";
        default: return sc.unitTitle();
    }
}

juce::String formatTimeTickLabel (double t, double step, double span)
{
    // Zoomed-out rulers use clock-style labels based on the visible range:
    // m:ss above one minute, h:mm:ss above an hour. Fractional seconds only
    // appear when the step is sub-second (defensive; the nice-step sequence
    // keeps steps >= 1 s at these zooms for any realistic plot width).
    if (span > 60.0)
    {
        int maxDec = 0;
        if (step < 1.0)
            maxDec = std::min (4, (int) std::ceil (-std::log10 (step) - 1e-9));
        return formatClockTime (t, maxDec);
    }

    if (t == 0.0) return "0";
    if (step >= 1.0)   return juce::String ((int) std::llround (t));
    if (step >= 0.01)  return juce::String (t, step >= 0.1 ? 1 : 2);

    // Sub-10 ms steps: compact ms/us suffixes while still inside the first
    // second of the file...
    if (t < 1.0)
    {
        if (step >= 0.001) return juce::String ((int) std::llround (t * 1000.0)) + "ms";
        return juce::String ((int) std::llround (t * 1e6)) + "\u00b5s";   // µs
    }

    // ...otherwise decimal seconds with as many digits as the step needs.
    int digits = (int) std::ceil (-std::log10 (step) - 1e-9);
    if (digits < 3) digits = 3;
    return juce::String (t, digits);
}

void WaveformView::drawRuler (juce::Graphics& g)
{
    if (!showTimeRuler) return;   // stacked dual view: the other view draws it
    const auto pr = plotRect();
    const double w = viewEnd - viewStart;
    if (w <= 0 || pr.getWidth() <= 0)
        return;

    // Ticks are placed at nice values of the DISPLAYED axis: seconds for
    // Time/TimeOffset, sample counts for Sample. Each displayed value maps
    // back to a real time for positioning.
    const double pxPerSec = pr.getWidth() / w;
    const TimeAxisMode mode = app.timeAxisMode;
    const double off = app.timeOffsetSecs;
    const double rate = std::max (1.0, app.audioEngine().sampleRate());

    auto dispToTime = [mode, off, rate] (double d) -> double
    {
        switch (mode)
        {
            case TimeAxisMode::Sample:     return d / rate;
            case TimeAxisMode::TimeOffset: return d - off;
            default:                       return d;
        }
    };

    const double dispStart = timeToDisplayValue (viewStart, mode, off, rate);
    const double dispEnd   = timeToDisplayValue (viewEnd,   mode, off, rate);
    const double major = (mode == TimeAxisMode::Sample)
                         ? niceSampleStep (rate / pxPerSec) : niceTimeStep (pxPerSec);
    // Subdivide each major interval into five when there is room for it.
    // (In Sample mode `major` counts samples, so convert to pixels first.)
    const double majorPx = (mode == TimeAxisMode::Sample)
                           ? (major / rate) * pxPerSec : major * pxPerSec;
    const bool showMinors = majorPx >= 40.0;

    // The ruler strip sits at the top by default or at the bottom when moved
    // there via the View menu; ticks point from the plot edge into it.
    const int rulerTop = rulerAtBottom() ? pr.getBottom() : pr.getY() - kRulerH;
    const float sepY   = rulerAtBottom() ? (float) (rulerTop + 1) : (float) (pr.getY() - 1);
    const float tickEnd   = rulerAtBottom() ? sepY + 7.0f : sepY - 7.0f;
    const float minorEnd  = rulerAtBottom() ? sepY + 3.0f : sepY - 3.0f;

    g.setColour (juce::Colour::fromRGB (60, 60, 60));
    g.drawLine ((float) pr.getX(), sepY, (float) pr.getRight(), sepY, 1.0f);

    const double first = std::ceil (dispStart / major) * major;
    if (first > dispEnd + 1e-9)
        return;   // the visible span is shorter than one tick step

    g.setFont (10.0f);
    const int nTicks = (int) std::floor ((dispEnd - first) / major + 1e-9) + 1;
    for (int i = 0; i < nTicks; ++i)
    {
        const double d = first + (double) i * major;
        if (d > dispEnd + 1e-9) break;
        const double t = dispToTime (d);
        if (t < viewStart - 1e-9 || t > viewEnd + 1e-9) continue;
        const float x = timeToX (t);

        g.setColour (juce::Colour::fromRGB (60, 60, 60));
        g.drawLine (x, std::min (sepY, tickEnd), x, std::max (sepY, tickEnd), 1.0f);

        if (showMinors)
            for (int j = 1; j < 5; ++j)
            {
                const double dm = d + (double) j * major / 5.0;
                const double tm = dispToTime (dm);
                if (tm > viewEnd + 1e-9 || tm < viewStart - 1e-9) break;
                g.drawLine (timeToX (tm), std::min (sepY, minorEnd),
                            timeToX (tm), std::max (sepY, minorEnd), 1.0f);
            }

        juce::String label;
        switch (mode)
        {
            case TimeAxisMode::Sample:     label = juce::String ((int64_t) std::llround (d)); break;
            case TimeAxisMode::TimeOffset: label = formatClockTime (d, 4); break;
            default:                       label = formatTimeTickLabel (d, major, w); break;
        }

        g.setColour (juce::Colour::fromRGB (200, 200, 200));
        g.drawText (label, (int) x + 3, rulerTop + 2, 90, kRulerH - 4,
                    juce::Justification::centredLeft, false);
    }
}
