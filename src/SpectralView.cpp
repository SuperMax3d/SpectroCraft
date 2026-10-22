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

// Time-pan speed: content moves this many px per cursor pixel (0.5 = half
// speed, matching the waveform view's grab pan).
static constexpr double kPanSpeedFactor = 0.5;

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
    : app (appIn), spectral (appIn.spectralEngine)   // audio read live via app.audioEngine()
{
    // Grab keyboard focus when clicked so the arrow keys keep controlling
    // the playhead/selection even after the user has interacted with other
    // widgets. The view never consumes key events; they bubble up to
    // MainWindow's KeyListener.
    setWantsKeyboardFocus (true);
    settings_ = appIn.settings;
    freqMin = std::max (1.0, (double) appIn.settings.minFrequency);
    freqMax = std::min (app.audioEngine().sampleRate() * 0.5, (double) appIn.settings.maxFrequency);
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
    const double rate = app.audioEngine().sampleRate();
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
    const double rate = app.audioEngine().sampleRate();
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

void SpectralView::mouseDown (const juce::MouseEvent& e)
{
    if (!app.audioEngine().isFileLoaded()) return;

    // Clone & Multiply picking (Selection panel): left clicks inside the plot
    // pick a reference frequency first, then target frequencies afterwards.
    if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown()
        && e.x >= kLeftAxisWidth && app.isClonePicking ())
    {
        app.clonePickClick (xToTime ((double) e.x), freqAtY (e.y));
        return;
    }

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

    // Spectral selection tools (Selection menu): a left-drag on the plot draws
    // a shape in time-frequency space. Plain = replace, SHIFT = add,
    // CTRL = subtract. Tool drags never move the playhead.
    if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown()
        && !e.mods.isMiddleButtonDown() && e.x >= kLeftAxisWidth
        && app.selMode != SpectralSelMode::TimeRange)
    {
        toolSubtract = e.mods.isCtrlDown();
        toolReplace = !e.mods.isShiftDown() && !toolSubtract;
        lassoPts.clear ();
        strokePts.clear ();
        liveT0 = liveT1 = liveF0 = liveF1 = 0.0;

        switch (app.selMode)
        {
            case SpectralSelMode::Marquee:
                dragMode = DragMode::Marquee;
                break;
            case SpectralSelMode::Lasso:
                dragMode = DragMode::Lasso;
                lassoPts.emplace_back (dragStartTime, freqAtY (e.y));
                break;
            default:   // Paintbrush
                if (e.mods.isAltDown ())
                {
                    const double t = dragStartTime;
                    const double f = freqAtY (e.y);
                    if (haveLineAnchor)
                    {
                        // Straight line from the last stroke's end point to here.
                        dragMode = DragMode::LineDrag;
                        lineEndT = t;
                        lineEndF = f;
                        syncToolTimeExtent ({ std::min (lineAnchorT, t),
                                              std::max (lineAnchorT, t) });
                    }
                    else
                    {
                        // No previous stroke yet: just remember this point as
                        // the anchor for the next ALT+click.
                        haveLineAnchor = true;
                        lineAnchorT = t;
                        lineAnchorF = f;
                    }
                }
                else
                {
                    dragMode = DragMode::Paint;
                    strokePts.emplace_back (dragStartTime, freqAtY (e.y));
                }
                break;
        }
        return;
    }

    if (!leftOnAxis)
    {
        // Clicking moves the playhead to that position
        // (clamped; also updates the transport position without starting playback).
        app.setPlayhead (dragStartTime);
        setPlayhead (app.playhead);
    }

    if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown() && !e.mods.isMiddleButtonDown())
    {
        if (leftOnAxis)
        {
            // Left-drag on the frequency axis pans the vertical range; it must
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

void SpectralView::mouseDrag (const juce::MouseEvent& e)
{
    if (!app.audioEngine().isFileLoaded()) return;

    // Keep the status-bar probe / brush cursor position current.
    probeX = e.x;
    probeY = e.y;

    switch (dragMode)
    {
        case DragMode::Marquee:
        {
            const double tA = xToTime (std::min (dragStartX, e.x));
            const double tB = xToTime (std::max (dragStartX, e.x));
            const double fA = freqAtY (std::min (dragStartY, e.y));
            const double fB = freqAtY (std::max (dragStartY, e.y));
            liveT0 = tA;
            liveT1 = tB;
            liveF0 = std::min (fA, fB);
            liveF1 = std::max (fA, fB);
            syncToolTimeExtent ({ liveT0, liveT1 });
            repaint();
            break;
        }
        case DragMode::Lasso:
        {
            const double t = xToTime (e.x);
            const double f = freqAtY (e.y);
            const auto last = lassoPts.empty() ? std::pair<double, double> (-1e30, -1e30)
                                               : lassoPts.back();
            if (std::abs (t - last.first) >= 1e-3 || std::abs (f - last.second) >= 8.0)
                lassoPts.emplace_back (t, f);

            // Live time extent: union of the path's bounds with any kept shapes.
            double tMin = 1e30, tMax = -1e30;
            for (auto& p : lassoPts) { tMin = std::min (tMin, p.first); tMax = std::max (tMax, p.first); }
            syncToolTimeExtent ({ tMin, tMax });
            repaint();
            break;
        }
        case DragMode::Paint:
        {
            const double t = xToTime (e.x);
            const double f = freqAtY (e.y);
            const auto last = strokePts.empty() ? std::pair<double, double> (-1e30, -1e30)
                                                : strokePts.back();
            if (std::abs (t - last.first) >= 1e-3 || std::abs (f - last.second) >= 8.0)
                strokePts.emplace_back (t, f);

            double tMin = 1e30, tMax = -1e30;
            for (auto& p : strokePts) { tMin = std::min (tMin, p.first); tMax = std::max (tMax, p.first); }
            syncToolTimeExtent ({ tMin, tMax });
            repaint();
            break;
        }
        case DragMode::LineDrag:
        {
            lineEndT = xToTime (e.x);
            lineEndF = freqAtY (e.y);
            syncToolTimeExtent ({ std::min (lineAnchorT, lineEndT),
                                  std::max (lineAnchorT, lineEndT) });
            repaint();
            break;
        }
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
            // Grab-and-drag at half speed (like the waveform view): content
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
            // Grab-and-drag: the frequency under the cursor at drag start stays
            // under the cursor. The shift is applied in log space for a log
            // axis and linearly otherwise.
            const auto rows = spectralRowRects (getWidth(), getHeight(), (int) channels.size());
            if (!rows.empty())
            {
                juce::Rectangle<int> row = rows.front();
                for (const auto& rr : rows)
                    if (rr.contains (dragStartX, dragStartY)) { row = rr; break; }

                const double f0 = yToFreq ((float) (dragStartY - row.getY()), row.getHeight());
                const double f1 = yToFreq ((float) (e.y - row.getY()), row.getHeight());
                const double nyq = app.audioEngine().sampleRate() * 0.5;
                if (settings_.logFrequency && f0 > 0.0 && f1 > 0.0)
                {
                    // Grab-and-drag in log space: scale both bounds by the same
                    // ratio so the zoom level is preserved.
                    const double R = f0 / f1;
                    double lo2 = freqMin * R, hi2 = freqMax * R;
                    // Pin at most one edge to its limit; shifting both would
                    // silently change the span and make reversing direction
                    // feel sticky (the range could never return to where it was).
                    if (lo2 < 1.0) lo2 = 1.0;
                    else if (hi2 > nyq) hi2 = nyq;
                    setFrequencyRange (lo2, hi2);
                }
                else
                {
                    const double d = f0 - f1;
                    double lo2 = freqMin + d, hi2 = freqMax + d;
                    if (lo2 < 1.0) lo2 = 1.0;          // pin the bottom edge
                    else if (hi2 > nyq) hi2 = nyq;     // pin the top edge
                    setFrequencyRange (lo2, hi2);
                }
            }
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

void SpectralView::mouseUp (const juce::MouseEvent&)
{
    if (dragMode == DragMode::ZoomDrag)
    {
        if (!haveZoomRect)
        {
            // Right-clicking the left frequency axis makes "Zoom Out Full"
            // reset the vertical range instead of the time window.
            const bool onAxis = dragStartX < kLeftAxisWidth;
            // Only show the menu when there is something to do with it: an
            // external action callback, or a local axis reset. (Headless test
            // views have neither and must not enter a modal loop.)
            if (onAxis || onContextAction)
                showViewContextMenu (app, [this, onAxis] (int id)
                {
                    if (id == CtxZoomOutFull && onAxis) { resetFrequencyRange(); return; }
                    if (onContextAction) onContextAction (id);
                });
        }
        else
        {
            // The drag START position selects which axes get zoomed:
            //  - inside the plot ("the track"): BOTH axes — time from the box's
            //    x span, frequency from its y span;
            //  - on the left frequency axis: vertical only.
            const bool startOnAxis = dragStartX < kLeftAxisWidth;

            auto zoomTime = [this] ()
            {
                // Zoom BOTH views to the dragged time range (setWindow notifies
                // MainWindow via onChange).
                const double t0 = xToTime (zoomRect.getX());
                const double t1 = xToTime (zoomRect.getRight());
                if (t1 - t0 > 1e-9) setWindow (t0, t1);
            };

            auto zoomFreq = [this] ()
            {
                // Set the frequency axis to the dragged span, mapped through
                // the channel row containing the drag start.
                const auto rows = spectralRowRects (getWidth(), getHeight(), (int) channels.size());
                if (!rows.empty() && zoomRect.getHeight() >= 2)
                {
                    juce::Rectangle<int> row = rows.front();
                    for (const auto& rr : rows)
                        if (rr.contains (dragStartX, dragStartY)) { row = rr; break; }
                    const double f1 = yToFreq ((float) (std::min (zoomRect.getY(), zoomRect.getBottom()) - row.getY()),
                                               row.getHeight());
                    const double f2 = yToFreq ((float) (std::max (zoomRect.getY(), zoomRect.getBottom()) - row.getY()),
                                               row.getHeight());
                    setFrequencyRange (std::min (f1, f2), std::max (f1, f2));
                }
            };

            if (startOnAxis)
                zoomFreq ();
            else
            {
                zoomTime ();
                zoomFreq ();
            }
        }
        haveZoomRect = false;
        zoomRect = {};
        repaint();
    }
    else if (dragMode == DragMode::Marquee || dragMode == DragMode::Lasso
             || dragMode == DragMode::Paint || dragMode == DragMode::LineDrag)
    {
        auto* d = app.doc();
        bool changed = false;

        if (d)
        {
            switch (dragMode)
            {
                case DragMode::Marquee:
                    // A degenerate marquee (no real span) clears on a plain drag.
                    if (liveT1 - liveT0 > 1e-6 && liveF1 - liveF0 > 1.0)
                        d->spectralSel.addRect (liveT0, liveT1, liveF0, liveF1,
                                                toolSubtract, toolReplace);
                    else if (toolReplace && !toolSubtract)
                        d->spectralSel.clear ();
                    changed = true;
                    break;
                case DragMode::Lasso:
                    if (lassoPts.size() >= 3)
                        d->spectralSel.addPolygon (lassoPts, toolSubtract, toolReplace);
                    else if (toolReplace && !toolSubtract)
                        d->spectralSel.clear ();   // tiny lasso = clear
                    changed = true;
                    break;
                case DragMode::LineDrag:
                    // Straight line in LINEAR-frequency space from the anchor
                    // to the release point (see straightLinePoints()).
                    d->spectralSel.addStroke (straightLinePoints (lineAnchorT, lineAnchorF,
                                                                 lineEndT, lineEndF),
                                              app.brushRadiusHz, toolSubtract, toolReplace);
                    haveLineAnchor = true;
                    lineAnchorT = lineEndT;
                    lineAnchorF = lineEndF;
                    changed = true;
                    break;
                default:   // Paint
                    if (!strokePts.empty())
                    {
                        d->spectralSel.addStroke (strokePts, app.brushRadiusHz,
                                                  toolSubtract, toolReplace);
                        haveLineAnchor = true;
                        lineAnchorT = strokePts.back().first;
                        lineAnchorF = strokePts.back().second;
                    }
                    changed = true;
                    break;
            }
        }

        lassoPts.clear ();
        strokePts.clear ();

        if (changed)
        {
            // Mirror the committed selection's time extent into the shared time
            // selection, re-install the playback mask and refresh both views +
            // panels. This must go through refreshSpectralSelectionUI() rather
            // than a plain setSelection(): the live drag already synced the same
            // extent via syncToolTimeExtent(), so a bare setSelection would be a
            // no-op, fire no notification and leave the Selection panel with
            // stale button states (e.g. Save disabled after a plain replace-drag).
            app.refreshSpectralSelectionUI ();
            selection = app.selection;
            setSpectralSelectionChanged ();
        }
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
        if (onCursorInfo) onCursorInfo ("");
        return;
    }

    const int ch = channels[(size_t) rowIdx];
    const auto& row = rows[(size_t) rowIdx];
    const double t = xToTime ((int) e.x);
    const double freq = yToFreq (e.y - row.getY(), row.getHeight());

    // Use the same mipmap level as the rendered image so the tiles are cached.
    const double rate = app.audioEngine().sampleRate();
    const int64_t s0 = std::max ((int64_t) 0, (int64_t) (viewStart * rate));
    const int64_t s1 = std::min (app.audioEngine().numSamples(), (int64_t) (viewEnd * rate));
    const double spp = (s1 > s0)
        ? ((double) (s1 - s0) / std::max (1, pr.getWidth())) : 0.0;
    const int level = SpectralEngine::pickMipLevel (spp, settings_.hopSize);

    const float dB = spectral.probeMagnitude (app.audioEngine(), ch, t, freq, settings_, level);

    // With a scale calibration the probe shows absolute levels
    // (dB-SPL for Pascal files; linear V/g values for volts/grams files)
    // instead of relative dB.
    if (onCursorInfo)
        onCursorInfo ("t=" + juce::String (t, 3) + " s   f="
                      + ((freq >= 1000.0)
                            ? juce::String (freq / 1000.0, 2) + " kHz"
                            : juce::String ((int) std::lround (freq)) + " Hz")
                      + "   " + ScaleCalibration::formatSpectralLevel (dB, app.scale()));
}

void SpectralView::mouseExit (const juce::MouseEvent&)
{
    if (onCursorInfo) onCursorInfo ("");
}

void SpectralView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!app.audioEngine().isFileLoaded()) return;
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
    if (ns < 0.0) { ns = 0.0; ne = std::min (w, app.audioEngine().duration()); }
    if (ne > app.audioEngine().duration()) { ne = app.audioEngine().duration(); ns = std::max (0.0, ne - w); }
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
    setWindow (0.0, app.audioEngine().duration());
}

void SpectralView::resetFrequencyRange()
{
    const double rate = app.audioEngine().sampleRate();
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
    const double rate = app.audioEngine().sampleRate();
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

bool SpectralView::saveAsPng (const juce::File& file, const std::atomic<bool>* cancel)
{
    if (!app.audioEngine().isFileLoaded() || channels.empty()) return false;
    const auto pr = plotRect();
    Settings rs = settings_;
    rs.minFrequency = (float) std::max (1.0, freqMin);
    rs.maxFrequency = (float) std::min (app.audioEngine().sampleRate() * 0.5, freqMax);

    juce::Image img (juce::Image::ARGB, pr.getWidth(), pr.getHeight(), true);
    if (!renderSpectralContent (img, rs, cancel))
        return false;   // cancelled: no PNG is written

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
    if (!app.audioEngine().isFileLoaded()) return;
    zoomFreqAtY ((float) getHeight() * 0.5f, factor);
}

void SpectralView::panByTime (double dt)
{
    const double dur = app.audioEngine().duration();
    const double w = viewEnd - viewStart;
    const double ns = std::clamp (viewStart + dt, 0.0, std::max (0.0, dur - w));
    setWindow (ns, ns + w);
}

void SpectralView::zoomFreqAtY (float y, double factor)
{
    const double rate = app.audioEngine().sampleRate();
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

    if (!app.audioEngine().isFileLoaded() || channels.empty())
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
        rs.maxFrequency = (float) std::min (app.audioEngine().sampleRate() * 0.5, freqMax);

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
    if (channels.size() > 1 || app.audioEngine().numChannels() == 2)
    {
        const auto rows = spectralRowRects (getWidth(), getHeight(), (int) channels.size());
        g.setFont (10.0f);
        for (size_t r = 0; r < rows.size(); ++r)
        {
            const int ch = channels[r];
            const auto& rr = rows[r];
            const juce::String lab = (app.audioEngine().numChannels() == 2) ? (ch == 0 ? "L" : "R") : juce::String (ch + 1);
            g.setColour (juce::Colour::fromRGB (190, 190, 190));
            g.drawText (lab, 4, rr.getY() + rr.getHeight() / 2 - 8, kLeftAxisWidth - 6, 16,
                       juce::Justification::centredRight, false);
        }
    }

    drawOverlays (g);

    // Right-drag box-zoom rubber band: the same translucent blue as the
    // time selection in the tracks, so it reads as "this region" rather
    // than a generic white marquee. When the drag starts on the frequency
    // axis the band stretches across its full width (matching what mouseUp
    // will actually zoom).
    if (haveZoomRect && dragMode == DragMode::ZoomDrag)
    {
        auto zr = zoomRect;
        if (dragStartX < kLeftAxisWidth)
            zr = juce::Rectangle<int> (0, zr.getY(), kLeftAxisWidth, zr.getHeight());

        g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
        g.fillRect (zr);
        g.setColour (juce::Colour::fromRGB (96, 150, 255).withAlpha (0.9f));
        g.drawRect (zr, 1);
    }
}

// Renders the spectral content (one row per visible channel with dark gaps)
// into an ARGB image of size w×h. Used by paint() and saveAsPng().
bool SpectralView::renderSpectralContent (juce::Image& img, const Settings& rs,
                                          const std::atomic<bool>* cancel)
{
    auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };

    const int w = img.getWidth(), h = img.getHeight();
    if (w <= 0 || h <= 0) return true;

    const juce::Image::BitmapData bd (img, 0, 0, w, h, juce::Image::BitmapData::writeOnly);

    // One row per visible channel (like the waveform view), separated by
    // thin dark gaps. Each row gets its own frequency axis on the left.
    const auto rows = spectralRowRects (w, h, (int) channels.size());
    for (size_t r = 0; r < rows.size(); ++r)
    {
        if (cancelled()) return false;

        const int ch = channels[r];
        const auto rr = rows[r];
        if (rr.getHeight() <= 0) continue;

        std::vector<uint8_t> pixels;
        spectral.renderToPixels (app.audioEngine(), rs, w, rr.getHeight(), viewStart, viewEnd, { ch }, pixels, cancel);
        if (cancelled()) return false;

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
    return true;
}

// One frequency axis per visible channel row: tick lines across the plot and
// labels in the left ruler area for each row.
void SpectralView::drawFrequencyRuler (juce::Graphics& g)
{
    const double rate = app.audioEngine().sampleRate();
    const double fMax = std::min (rate * 0.5, freqMax);
    const double fMin = std::max (1.0, freqMin);

    // A channel label sits at the vertical centre of each row; keep tick
    // labels out of that zone.
    const bool hasChanLabel = (channels.size() > 1 || app.audioEngine().numChannels() == 2);

    const auto rows = spectralRowRects (getWidth(), getHeight(), (int) channels.size());
    g.setFont (10.0f);

    for (const auto& rr : rows)
    {
        if (rr.getHeight() <= 0) continue;
        // Labels only when the row is tall enough to hold them.
        const bool showLabels = rr.getHeight() >= 48;

        auto drawTickAndLabel = [&](double f, bool minor = false, bool withLabel = true)
        {
            const float y = (float) rr.getY() + freqToY (f, rr.getHeight());
            g.setColour (minor ? juce::Colour::fromRGB (48, 48, 52)
                               : juce::Colour::fromRGB (60, 60, 60));
            g.drawLine ((float) kLeftAxisWidth, y, (float) getWidth(), y, 1.0f);
            if (!showLabels || !withLabel) return;
            if (hasChanLabel && std::abs (y - ((float) rr.getY() + rr.getHeight() * 0.5f)) < 10.0f)
                return; // room for the channel label
            // Drop labels that would collide with the neighbouring row's edge
            // label at a shared boundary (or be clipped at the component
            // edge); keep the tick line itself.
            if ((float) y < rr.getY() + 13.0f || (float) y > rr.getBottom() - 1.0f)
                return;
            g.setColour (minor ? juce::Colour::fromRGB (130, 130, 138)
                               : juce::Colour::fromRGB (200, 200, 200));
            juce::String label = f >= 1000 ? juce::String (f / 1000.0, 1) + " kHz" : juce::String ((int) f) + " Hz";
            g.drawText (label, 4, (int) y - 12, kLeftAxisWidth - 6, 12, juce::Justification::bottomLeft, false);
        };

        if (settings_.logFrequency)
        {
            // Adaptive log ticks so the axis never goes empty when zoomed in:
            // decades stay major and every digit per decade is drawn a bit
            // darker at mid zooms (thinned to 1-2-5 when crowded); inside a
            // sub-decade range a nice linear step provides the divisions.
            const double spanDec = std::log10 (std::max (1e-9, fMax / fMin));
            if (spanDec < 0.7)
            {
                const double span = std::max (1e-9, fMax - fMin);
                bool done = false;
                for (double e = std::floor (std::log10 (span / 8.0));
                     !done && e <= std::ceil (std::log10 (span / 3.0)) + 1.0; ++e)
                    for (double m : { 1.0, 2.0, 5.0 })
                    {
                        const double step = m * std::pow (10.0, e);
                        if (span / step >= 3.0 && span / step <= 8.0)
                        {
                            for (double f = std::ceil (fMin / step - 1e-9) * step;
                                 f <= fMax + 1e-6; f += step)
                                drawTickAndLabel (f, false);
                            done = true;
                            break;
                        }
                    }
                if (!done)
                    for (int i = 0; i <= 4; ++i)
                        drawTickAndLabel (fMin + span * (double) i / 4.0, false);
            }
            else
            {
                auto collect = [&] (const int* digs, int n)
                {
                    std::vector<double> t;
                    for (double p = std::ceil (std::log10 (fMin)) - 1.0;
                         p <= std::floor (std::log10 (fMax)); ++p)
                        for (int i = 0; i < n; ++i)
                        {
                            const double f = (double) digs[i] * std::pow (10.0, p);
                            if (f >= fMin - 1e-6 && f <= fMax + 1e-6) t.push_back (f);
                        }
                    return t;
                };
                static const int allD[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
                static const int fewD[] = { 1, 2, 5 };
                auto ticks = collect (allD, 9);
                if ((int) ticks.size() > 14) ticks = collect (fewD, 3);

                // Gap-fill: where two adjacent structural points are far apart
                // on screen — e.g. the partial decade between 1k and 2k when
                // zoomed to [100 Hz, 2 kHz] — add nice linear subdivisions so
                // sub-decade regions never look empty.
                const double gapThresh = std::max (24.0, rr.getHeight() * 0.18);
                auto yOf = [&] (double f) { return (float) rr.getY() + freqToY (f, rr.getHeight()); };
                auto fillGap = [&] (double a, double b)
                {
                    if (b <= a * 1.0001) return;
                    const double gapPx = std::abs (yOf (b) - yOf (a));
                    if (gapPx < gapThresh) return;
                    const double span = b - a;
                    bool done = false;
                    for (double e = std::floor (std::log10 (span / 8.0));
                         !done && e <= std::ceil (std::log10 (span / 3.0)) + 1.0; ++e)
                        for (double m : { 1.0, 2.0, 5.0 })
                        {
                            const double step = m * std::pow (10.0, e);
                            if (span / step >= 3.0 && span / step <= 8.0)
                            {
                                const int nDiv = (int) std::lround (span / step);
                                const bool label = gapPx / (nDiv + 1) >= 16.0;
                                for (double f = std::ceil (a / step - 1e-9) * step;
                                     f < b - step * 1e-3; f += step)
                                    if (f > a + step * 1e-3) drawTickAndLabel (f, true, label);
                                done = true;
                                break;
                            }
                        }
                };
                double prev = fMin;
                for (double f : ticks) { fillGap (prev, f); prev = f; }
                fillGap (prev, fMax);

                for (double f : ticks)
                    drawTickAndLabel (f,
                                     std::abs (std::log10 (f) - std::round (std::log10 (f))) > 1e-9);
            }
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

    // Spectral selection: committed shapes from the active doc plus the live
    // preview while a tool drag is in progress. They are evaluated TOGETHER so
    // a Ctrl-drag shows its hole in the existing selection immediately.
    {
        static const std::vector<SpectralShape> emptyShapes;
        auto* d = app.doc();
        const std::vector<SpectralShape>& committed =
            (d && !d->spectralSel.empty ()) ? d->spectralSel.shapes () : emptyShapes;

        SpectralShape live;
        bool haveLive = false;
        if (dragMode == DragMode::Marquee)
        {
            live.kind = SpectralShape::Kind::Rect;
            live.subtract = toolSubtract;
            live.t0 = liveT0;  live.t1 = liveT1;
            live.f0 = liveF0;  live.f1 = liveF1;
            haveLive = true;
        }
        else if ((dragMode == DragMode::Lasso && !lassoPts.empty ())
                 || (dragMode == DragMode::Paint && !strokePts.empty ())
                 || dragMode == DragMode::LineDrag)
        {
            live.kind = dragMode == DragMode::Lasso ? SpectralShape::Kind::Polygon
                                                   : SpectralShape::Kind::Stroke;
            live.subtract = toolSubtract;
            if (dragMode == DragMode::LineDrag)
                live.pts = straightLinePoints (lineAnchorT, lineAnchorF,
                                               lineEndT, lineEndF);
            else
                live.pts = (dragMode == DragMode::Lasso) ? lassoPts : strokePts;
            if (live.kind == SpectralShape::Kind::Stroke)
                live.rasteriseStroke (app.brushRadiusHz);   // grid needed for contains()
            haveLive = true;
        }

        drawSpectralShapes (g, committed, haveLive ? &live : nullptr);
    }

    // Markers
    if (markerList)
    {
        const auto& ms = *markerList;   // kept sorted by start time
        for (size_t i = 0; i < ms.size(); ++i)
        {
            const Marker& m = ms[i];
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
            // Title: may run past this marker's own range, but must stop
            // before the next marker starts so that marker's title has room.
            g.setFont (10.0f);
            g.setColour (m.colour);
            const double textX = x + 4;
            const double rightEdge = (i + 1 < ms.size())
                ? timeToX (ms[i + 1].start) - 2.0
                : (double) getWidth() + 50.0;
            const double w = std::max (0.0, rightEdge - textX);
            if (w > 0.0)
                g.drawText (m.name, (int) textX, 8, (int) w, 14,
                            juce::Justification::topLeft, true);
        }
    }

    // Paintbrush cursor: a translucent band showing the brush's frequency
    // width at the current cursor position.
    if (app.selMode == SpectralSelMode::Paintbrush && dragMode == DragMode::None
        && probeX >= kLeftAxisWidth && probeX <= getWidth()
        && probeY >= 0 && probeY <= getHeight())
    {
        const double f = freqAtY (probeY);
        g.setColour (juce::Colour::fromRGBA (96, 150, 255, 140));
        for (const auto& row : spectralRowRects (getWidth(), getHeight(), (int) channels.size()))
        {
            const double yTop = row.getY() + freqToY (f + app.brushRadiusHz, row.getHeight());
            const double yBot = row.getY() + freqToY (std::max (0.0, f - app.brushRadiusHz), row.getHeight());
            g.fillRect ((float) probeX - 1.0f, (float) std::min (yTop, yBot), 2.0f,
                        (float) std::max (0.0, std::abs (yBot - yTop)));
        }
    }

    // Clone & Multiply picking markers: the reference point (orange) and each
    // clicked target (cyan), drawn on every channel row.
    if (app.isClonePicking ())
    {
        const auto& pts = app.clonePickPoints ();
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const double t = pts[i].first, f = pts[i].second;
            if (t < viewStart || t > viewEnd) continue;
            const int x = timeToX (t);
            g.setColour (i == 0 ? juce::Colours::orange : juce::Colours::cyan);
            for (const auto& row : spectralRowRects (getWidth(), getHeight (), (int) channels.size()))
            {
                const int y = freqToY (f, row.getHeight());
                if (y < 0 || y >= row.getHeight()) continue;
                g.drawEllipse ((float) x - 5.0f, (float) row.getY() + y - 5.0f,
                              10.0f, 10.0f, 1.5f);
            }
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

// ------------------------------------------------- spectral selection tools

double SpectralView::freqAtY (int y) const
{
    const auto rows = spectralRowRects (getWidth(), getHeight(), (int) channels.size());
    if (rows.empty()) return 0.0;

    size_t best = 0;
    double bestDist = 1e30;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const int dy = y < rows[i].getY() ? rows[i].getY() - y
                        : y > rows[i].getBottom() ? y - rows[i].getBottom() : 0;
        if (dy < bestDist) { bestDist = dy; best = i; }
    }
    const auto& r = rows[best];
    return yToFreq ((float) (y - r.getY()), r.getHeight());
}

void SpectralView::syncToolTimeExtent (juce::Range<double> ext)
{
    // When adding to an existing selection, keep the union of extents so both
    // views track everything that will be affected.
    auto* d = app.doc();
    if (!toolReplace && d && !d->spectralSel.empty ())
        ext = ext.getUnionWith (d->spectralSel.timeExtent ());

    const double dur = std::max (1e-9, app.audioEngine().duration ());
    ext = ext.getIntersectionWith ({ 0, dur });
    if (ext.getLength() <= 1e-9)
        app.setSelection ({ 0, 0 });
    else
        app.setSelection (ext);
    selection = app.selection;
}

void SpectralView::drawSpectralShapes (juce::Graphics& g,
                                      const std::vector<SpectralShape>& committedIn,
                                      const SpectralShape* live)
{
    const bool preview = (live != nullptr);
    if (committedIn.empty() && !preview) return;
    const auto rows = spectralRowRects (getWidth(), getHeight(), (int) channels.size());
    if (rows.empty()) return;

    // Ordered iteration over committed shapes followed by the live preview.
    const int nShapes = (int) committedIn.size() + (preview ? 1 : 0);
    auto shapeAt = [&] (int i) -> const SpectralShape&
    {
        return i < (int) committedIn.size() ? committedIn[(size_t) i] : *live;
    };

    // Same blue family as the time selection; previews are brighter and dashed.
    const juce::Colour fillCol  = preview ? juce::Colour::fromRGBA (70, 130, 255, 96)
                                          : juce::Colour::fromRGBA (70, 130, 255, 80);
    const juce::Colour edgeCol  = preview ? juce::Colour::fromRGB (140, 180, 255)
                                          : juce::Colour::fromRGB (96, 150, 255);

    // Rasterise the ordered shape list into a per-pixel boolean mask: additive
    // shapes set bits, subtractive ones clear them. This is exactly what
    // SpectralSelection::contains() computes, so the overlay shows real holes
    // where Ctrl-dragged regions were cut out instead of painting more blue on
    // top - what you see is precisely what gets edited / played back.
    const int W = getWidth();
    const size_t totalPx = (size_t) W * (size_t) std::max (1, getHeight());
    if (selMask.size() < totalPx) selMask.assign (totalPx, 0);
    std::fill (selMask.begin(), selMask.begin() + totalPx, 0);

    const int plotX = kLeftAxisWidth;
    const int plotRight = W - 1;

    for (const auto& row : rows)
    {
        // This JUCE build has no save/restoreGraphicsContext: use the RAII
        // ScopedSaveState + reduceClipRegion instead.
        const juce::Graphics::ScopedSaveState save (g);
        if (!g.reduceClipRegion (row)) continue;   // row outside the repaint clip

        const int rh = std::max (1, row.getHeight());

        // Per-row frequency lookup so per-pixel tests stay cheap.
        std::vector<double> freqAtY ((size_t) rh);
        for (int y = 0; y < rh; ++y)
            freqAtY[(size_t) y] = yToFreq ((float) y, rh);

        // Apply every shape in order to this row's pixels.
        for (int si = 0; si < nShapes; ++si)
        {
            const SpectralShape& s = shapeAt (si);
            const juce::Rectangle<double> bb = s.boundingBox();
            if (bb.getWidth() <= 0 || bb.getHeight() <= 0) continue;
            // timeToX clamps to the plot, so reject off-screen shapes explicitly.
            if (bb.getRight() <= viewStart || bb.getX() >= viewEnd) continue;

            int x0 = std::max (plotX, (int) std::floor (timeToX (bb.getX())));
            int x1 = std::min (plotRight, (int) std::ceil (timeToX (bb.getRight())));
            if (x1 < x0) continue;

            // Frequency span -> row-local y range (high frequency at the top).
            const double fLo = bb.getY(), fHi = bb.getY() + bb.getHeight();
            int yTop = std::max (0, (int) std::floor ((double) freqToY (fHi, rh)));
            int yBot  = std::min (rh - 1, (int) std::ceil ((double) freqToY (fLo, rh)));
            if (yBot < yTop) continue;

            uint8_t* const maskBase = selMask.data() + (size_t) row.getY() * W;

            if (s.kind == SpectralShape::Kind::Rect)
            {
                // Whole-pixel fill/clear - no per-pixel tests needed.
                for (int y = yTop; y <= yBot; ++y)
                    std::fill_n (maskBase + (size_t) y * W + x0, x1 - x0 + 1,
                                 s.subtract ? uint8_t (0) : uint8_t (1));
            }
            else if (s.kind == SpectralShape::Kind::Polygon && s.pts.size() >= 3)
            {
                const double tAtX0 = xToTime (x0);
                const double dtPx  = xToTime (x0 + 1) - tAtX0;
                for (int y = yTop; y <= yBot; ++y)
                {
                    const double f = freqAtY[(size_t) y];
                    uint8_t* line = maskBase + (size_t) y * W;
                    // Even-odd ray casting against the polygon edges.
                    for (int x = x0; x <= x1; ++x)
                    {
                        const double t = tAtX0 + dtPx * (double) (x - x0);
                        bool inside = false;
                        const auto& pts = s.pts;
                        for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++)
                        {
                            if ((pts[i].second > f) != (pts[j].second > f))
                            {
                                const double xCross = pts[i].first
                                    + (f - pts[i].second)
                                      * (pts[j].first - pts[i].first)
                                      / (pts[j].second - pts[i].second);
                                if (t < xCross) inside = !inside;
                            }
                        }
                        if (inside)
                            line[x] = s.subtract ? uint8_t (0) : uint8_t (1);
                    }
                }
            }
            else // Stroke: the committed grid answers contains() in O(1).
            {
                const double tAtX0 = xToTime (x0);
                const double dtPx  = xToTime (x0 + 1) - tAtX0;
                for (int y = yTop; y <= yBot; ++y)
                {
                    const double f = freqAtY[(size_t) y];
                    uint8_t* line = maskBase + (size_t) y * W;
                    for (int x = x0; x <= x1; ++x)
                        if (s.contains (tAtX0 + dtPx * (double) (x - x0), f))
                            line[x] = s.subtract ? uint8_t (0) : uint8_t (1);
                }
            }
        }

        // Paint the boolean result as horizontal blue runs.
        g.setColour (fillCol);
        for (int y = 0; y < rh; ++y)
        {
            const uint8_t* line = selMask.data() + ((size_t) row.getY() + (size_t) y) * W;
            int x = plotX;
            while (x <= plotRight)
            {
                if (!line[x]) { ++x; continue; }
                const int xs = x;
                while (x <= plotRight && line[x]) ++x;
                g.fillRect ((float) xs, (float) (row.getY() + y),
                            (float) (x - xs), 1.0f);
            }
        }

        // Borders: solid for committed additive shapes, dashed for subtractive
        // ones and live previews (same convention as before).
        g.setColour (edgeCol);
        static const float dash[] = { 4.0f, 3.0f };
        for (int si = 0; si < nShapes; ++si)
        {
            const SpectralShape& s = shapeAt (si);
            if (s.kind == SpectralShape::Kind::Rect)
            {
                const double x0 = timeToX (std::min (s.t0, s.t1));
                const double x1 = timeToX (std::max (s.t0, s.t1));
                if (x1 <= plotX || x0 >= plotRight) continue;
                const float rx = (float) std::max ((double) plotX, x0);
                const float rw = (float) std::max (1.0, std::min ((double) plotRight, x1) - rx);
                if (rw <= 0) continue;
                const double yTop = freqToY (std::max (s.f0, s.f1), rh);
                const double yBot = freqToY (std::min (s.f0, s.f1), rh);
                const float ry = (float) std::clamp ((double) std::min (yTop, yBot), 0.0, (double) (rh - 1));
                const float rb = (float) std::clamp ((double) std::max (yTop, yBot), 0.0, (double) (rh - 1));
                if (preview || s.subtract)
                {
                    g.drawDashedLine (juce::Line<float> (rx, ry + row.getY(), rx + rw, ry + row.getY()), dash, 2, 1.0f);
                    g.drawDashedLine (juce::Line<float> (rx + rw, ry + row.getY(), rx + rw, rb + row.getY()), dash, 2, 1.0f);
                    g.drawDashedLine (juce::Line<float> (rx + rw, rb + row.getY(), rx, rb + row.getY()), dash, 2, 1.0f);
                    g.drawDashedLine (juce::Line<float> (rx, rb + row.getY(), rx, ry + row.getY()), dash, 2, 1.0f);
                }
                else
                    g.drawRect (rx, ry + row.getY(), rw, std::max (1.0f, rb - ry), 1.0f);
            }
            else if (s.kind == SpectralShape::Kind::Polygon && s.pts.size() >= 3)
            {
                juce::Path p;
                for (size_t i = 0; i < s.pts.size(); ++i)
                {
                    const double x = timeToX (s.pts[i].first);
                    const double y = row.getY() + freqToY (s.pts[i].second, rh);
                    if (i == 0) p.startNewSubPath ((float) x, (float) y);
                    else        p.lineTo ((float) x, (float) y);
                }
                p.closeSubPath ();
                g.strokePath (p, juce::PathStrokeType (1.0f));
            }
            // Strokes need no extra border: the mask already paints their band.
        }

        // ScopedSaveState restores the clip on scope exit.
    }
}
