/**
 * SpectroCraft - Spectral View Implementation
 * Time window is shared with the waveform view (MainWindow keeps them in sync);
 * the frequency axis limits are local to this view. Each visible channel gets
 * its own row, like the waveform view; both views use the same left-axis width
 * so their plots start at the same x coordinate.
 */

#include "SpectralView.h"
#include "Diagnostics.h"
#include "WaveformView.h"   // niceTimeStep / formatTimeTickLabel (shared ruler style)
#include "AudioEngine.h"
#include "MarkerManager.h"
#include "PngWriter.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>

// Time-pan speed: content moves this many px per cursor pixel (0.5 = half
// speed, matching the waveform view's grab pan).
static constexpr double kPanSpeedFactor = 0.5;

juce::Rectangle<int> SpectralView::plotRect() const
{
    if (haveExportLayout_)
        return exportLayout_;   // offscreen export canvas layout
    // The axis strip always takes axisW() of horizontal space; only
    // its side changes with the View menu choice.
    const int x0 = axisOnRight() ? 0 : axisW();
    // When the ruler is at the top and a dedicated tables track sits below it,
    // that strip's height is reserved inside this view.
    const int inset = (showOwnTimeRuler && !rulerAtBottom()) ? tablesTopInset : 0;
    const int ruY = (showOwnTimeRuler && rulerAtBottom()) ? 0 : rulerH() + inset;
    return { x0, ruY,
             std::max (1, getWidth() - axisW()),
             std::max (1, getHeight() - rulerH() - inset) };
}

void SpectralView::setShowOwnTimeRuler (bool show)
{
    if (show == showOwnTimeRuler) return;
    showOwnTimeRuler = show;
    dirty = true;      // plot area moved: the cached image no longer fits
    imageDirty = true;
    repaint();
}

// One row per visible channel with a dark gap between the rows. The layout
// is shared with the waveform view so both tracks line up identically.
std::vector<juce::Rectangle<int>> SpectralView::rowRects() const
{
    // Channel rows in absolute view coordinates: full component width (so
    // axis drags can hit-test them too), positioned inside the plot area
    // below the top time ruler.
    const auto pr = plotRect();
    auto rows = WaveformView::channelRowRects (layoutWidth(), pr.getHeight(), (int) channels.size());
    for (auto& r : rows)
        r.setY (r.getY() + pr.getY());
    return rows;
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

std::array<std::pair<float, float>, 2> SpectralView::arrowHeadBarbs (float x0, float y0,
                                                                   float x1, float y1,
                                                                   double headLen)
{
    // The barbs trail behind the tip: from the tip back along the shaft
    // direction, rotated ~26 degrees either way.
    const double ang = std::atan2 ((double) (y0 - y1), (double) (x0 - x1));
    static const int sides[2] = { -1, 1 };
    std::array<std::pair<float, float>, 2> out;
    for (int i = 0; i < 2; ++i)
    {
        const double a2 = ang + sides[i] * 0.45;
        out[i] = { x0 - (float) (headLen * std::cos (a2)),
                   y0 - (float) (headLen * std::sin (a2)) };
    }
    return out;
}

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
// When onFreqAxis is true (right-click on the frequency axis strip) the menu
// gains an FFT Size submenu plus the Linear/Logarithmic toggle - the same
// actions and shortcuts as the Spectral Controls panel.
static void showViewContextMenu (Application& app, double clickT, double clickF,
                                 bool onFreqAxis, int autoFftSize,
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

    // Move/Scale the spectral selection: offered only when right-clicking ON it.
    auto* d = app.doc ();
    const bool onSpectralSel = d != nullptr && !d->spectralSel.empty ()
                               && d->spectralSel.contains (clickT, clickF);
    if (onSpectralSel)
    {
        m.addSeparator();
        m.addItem (CtxMoveSelection,  "Move Selection");
        m.addItem (CtxScaleSelection, "Scale Selection");
    }

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

    // The Scale form: one dialog for every axis of this file. It replaces the
    // old Scale submenu (Time Axis / Vertical Axis) and the Alternative Scale
    // submenu: axes, units, offsets, scales, order and visibility are all in
    // one place, and "Footer Only" is the old alternative-scale readout.
    m.addSeparator();
    m.addItem (CtxScaleForm, "Scale...", true, false);
    // The file this document came from (a brand-new document has none yet).
    const bool hasPath = app.doc() != nullptr
                         && ! app.doc()->file.getFullPathName().isEmpty();
    m.addItem (CtxOpenFolder, "Open Containing Folder", hasPath, false);

    // Frequency-axis extras: an "FFT Size" submenu with the size ladder
    // (same steps and shortcuts as Alt+Shift+= / Alt+Shift+-), an "Auto (N)"
    // entry showing the size Auto mode would resolve to, and below that a
    // Linear/Logarithmic toggle for the axis scale.
    if (onFreqAxis)
    {
        juce::PopupMenu fftMenu;
        fftMenu.addItem (CtxFftIncrease, "Increase\tAlt+Shift+=", true, false);
        fftMenu.addItem (CtxFftReduce,   "Reduce\tAlt+Shift+-",  true, false);
        juce::String autoLab = "Auto";
        if (autoFftSize > 0)
            autoLab = "Auto (" + juce::String (autoFftSize) + ")";
        fftMenu.addItem (CtxFftAuto, autoLab, app.settings.fftAuto);
        fftMenu.addSeparator();
        fftMenu.addItem (CtxAxisLinear, "Linear",      !app.settings.logFrequency);
        fftMenu.addItem (CtxAxisLog,    "Logarithmic",  app.settings.logFrequency);

        m.addSeparator();
        m.addSubMenu ("FFT Size", fftMenu);
    }

    if (const int sel = m.showMenu (juce::PopupMenu::Options().withMousePosition());
        sel != 0)
        onAction (sel);
}

void SpectralView::mouseDown (const juce::MouseEvent& e)
{
    if (!app.audioEngine().isFileLoaded()) return;

    // A click on a channel number mutes/unmutes that channel; Ctrl+click
    // (Cmd+click on macOS, where Ctrl+click is the context menu) solos it.
    if (e.mods.isLeftButtonDown() && !e.mods.isPopupMenu())
        if (const int ch = channelAtLabel (e.getPosition()); ch >= 0)
        {
            app.clickChannelMute (ch, e.mods.isCommandDown());
            return;
        }

    // Tables panel alignment flow: a left click is consumed as the reference
    // point in this view and reported to the panel. The second value is the
    // fitted [-1,+1] position of the clicked frequency - the same domain
    // drawTablesOverlay() maps into, so two-point alignments can solve the
    // vertical mapping.
    if (onAlignClick && e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown()
        && !e.mods.isMiddleButtonDown())
    {
        double val = 0.0;
        const double fHz = freqAtY ((int) e.y);
        if (getFreqMax () > getFreqMin ())
            val = 2.0 * (fHz - getFreqMin ()) / (getFreqMax () - getFreqMin ()) - 1.0;
        onAlignClick (xToTime ((double) e.x), val);
        return;
    }

    // Armed scale calibration (Scale / Alternative Scale menu): a left click
    // inside the plot places the reference point - a circle is drawn and
    // MainWindow prompts for its value. Any other click cancels the flow.
    if (app.refClickTarget != Application::RefClickTarget::None)
    {
        const auto pr = plotRect();
        const bool inPlot = e.x >= pr.getX() && e.x < pr.getRight()
                            && e.y >= pr.getY() && e.y < pr.getBottom();
        if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown() && inPlot)
        {
            app.refClickT = xToTime ((double) e.x);
            app.refClickF = freqAtY (e.y);
            app.haveRefClickPt = true;
            repaint();
            if (onRefClickPlaced) onRefClickPlaced (app.refClickT, app.refClickF);
            return;
        }
        app.refClickTarget = Application::RefClickTarget::None;
        app.haveRefClickPt = false;
        repaint();
        // Fall through: a right-click still shows the context menu.
    }

    // Move/Scale selection gesture: left clicks inside the plot set the
    // reference point first, then apply the change on the second click.
    if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown()
        && !onAxisX (e.x) && !onRulerY (e.y) && app.isSelGesturing ())
    {
        selGestLastClickMs = juce::Time::getMillisecondCounter();
        app.selGestureClick (xToTime ((double) e.x), freqAtY (e.y),
                             axisLockForEvent (e));
        return;
    }

    // Clone & Multiply picking (Selection panel): left clicks inside the plot
    // pick a reference frequency first, then target frequencies afterwards.
    if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown()
        && !onAxisX (e.x) && !onRulerY (e.y) && app.isClonePicking ())
    {
        // ~8 px hit radius in data units: clicking on a picked point removes it.
        const double dtTol = std::abs (xToTime (e.x + 4) - xToTime (e.x - 4));
        const double dfTol = std::abs (freqAtY (e.y + 4) - freqAtY (e.y - 4));
        app.clonePickClick (xToTime ((double) e.x), freqAtY (e.y), dtTol, dfTol);
        return;
    }

    // New-drawing mode armed by the Drawing panel or the t/a/r/d shortcuts:
    // a left click inside the plot starts the item (Text places immediately,
    // the other kinds drag). Clicking outside the plot cancels the mode.
    if (havePendingDraw_ && e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown())
    {
        const auto pr = plotRect();
        const bool inPlot = e.x >= pr.getX() && e.x < pr.getRight()
                            && e.y >= pr.getY() && e.y < pr.getBottom();
        if (inPlot)
        {
            const double t = xToTime ((double) e.x);
            const double f = freqAtY (e.y);
            if (pendingDraw_ == DrawingItem::Kind::Text)
            {
                havePendingDraw_ = false;
                repaint();
                if (onNewTextPlaced) onNewTextPlaced (t, f, e.x, e.y);
                return;
            }
            havePendingDraw_ = false;   // each gesture consumes the armed mode
            drawingNew_ = true;
            drawStartX_ = e.x;  drawStartY_ = e.y;
            drawEndX_   = e.x;  drawEndY_   = e.y;
            drawStartT_ = t;    drawStartF_ = f;
            lastFreeX_  = e.x;  lastFreeY_  = e.y;
            freePts_.clear ();
            freePts_.push_back ({ t, f });
            repaint();
            return;
        }
        havePendingDraw_ = false;
        repaint();
    }

    // Right button: box-zoom drag. A plain right-click (no significant
    // movement) still shows the context menu, deferred to mouseUp.
    if (e.mods.isRightButtonDown() && !e.mods.isLeftButtonDown())
    {
        app.cancelSelGesture ();   // a right button always ends the gesture first
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

    // Spectral selection tools (Selection menu): a left-drag on the plot draws
    // a shape in time-frequency space. Plain = replace, SHIFT = add,
    // CTRL = subtract. Tool drags never move the playhead.
    if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown()
        && !e.mods.isMiddleButtonDown() && !onAxisX (e.x)
        && !onRulerY (e.y)
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
        const bool onTimeRuler = onRulerY (e.y);
        if (onTimeRuler)
        {
            // Left-drag on the top time ruler pans horizontally at half speed
            // (same as the waveform view's ruler).
            dragMode = DragMode::TimePan;
            panLastX = e.x;
        }
        else if (leftOnAxis)
        {
            // Left-drag on the frequency axis pans the vertical range; it must
            // not move the playhead.
            dragMode = DragMode::VPan;
            panLastY = e.y;
        }
        else
        {
            dragMode = DragMode::Select;
            // Time-range selections span every channel row: edits apply to
            // all channels, so the highlight covers them all too.
            app.selectionRow = -1;
            if (app.onStateChange) app.onStateChange ();   // repaint both views
            app.setSelection ({ dragStartTime, dragStartTime });
            selection = app.selection;
        }
    }
    else if (e.mods.isMiddleButtonDown() || e.mods.isRightButtonDown())
    {
        // Middle-drag pans both axes: the shared time window horizontally and
        // the frequency window vertically, both at half cursor speed like the
        // axis strips.
        dragMode = DragMode::Pan;
        panLastX = e.x;
        panLastY = e.y;
    }

    // Push the new playhead/selection to both views.
    if (onChange) onChange (viewStart, viewEnd);
}

void SpectralView::mouseDrag (const juce::MouseEvent& e)
{
    if (!app.audioEngine().isFileLoaded()) return;
    reportHoverTime (e, true);

    // Keep the status-bar probe / brush cursor position current.
    probeX = e.x;
    probeY = e.y;

    // Live drag of a new Arrow/Rectangle/Freehand drawing: track the end
    // point (freehand decimates to ~2 px steps) and repaint the preview.
    if (drawingNew_)
    {
        drawEndX_ = e.x;
        drawEndY_ = e.y;
        if (pendingDraw_ == DrawingItem::Kind::Freehand
            && std::abs (e.x - lastFreeX_) + std::abs (e.y - lastFreeY_) >= 2)
        {
            freePts_.push_back ({ xToTime ((double) e.x), freqAtY (e.y) });
            lastFreeX_ = e.x;
            lastFreeY_ = e.y;
        }
        repaint();
        return;
    }

    // Move/Scale gesture: dragging from the reference click previews live too.
    if (app.haveSelGestureRef ())
    {
        updateSelGestureFromEvent (e);
        return;
    }

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
            // A middle grab pans x and y together.
            panFrequencyByCursorY (e.y);
            break;
        }
        case DragMode::TimePan:
        {
            // Ruler drag: horizontal pan at half cursor speed, like the
            // waveform view's ruler and the middle-button grab pan.
            const auto pr = plotRect();
            if (pr.getWidth() > 0)
                panByTime (-(e.x - panLastX) * kPanSpeedFactor
                           * (viewEnd - viewStart) / (double) pr.getWidth());
            panLastX = e.x;
            break;
        }
        case DragMode::VPan:
            // Frequency-axis drag: the same half-speed grab, log-aware.
            panFrequencyByCursorY (e.y);
            break;
        case DragMode::ZoomDrag:
        {
            const int x = std::clamp (e.x, 0, getWidth());
            const int y = std::clamp (e.y, 0, getHeight());
            if (!haveZoomRect && (std::abs (x - dragStartX) >= 4 || std::abs (y - dragStartY) >= 4))
                haveZoomRect = true;
            if (haveZoomRect)
            {
                auto zr = juce::Rectangle<int> (std::min (dragStartX, x), std::min (dragStartY, y),
                                                std::abs (x - dragStartX), std::abs (y - dragStartY));
                // The vertical extent is clipped to the channel row the drag
                // started on: a box-zoom never spans more than one channel's
                // data area. Ruler drags are time-only and keep full height.
                const bool startOnRuler = !onAxisX (dragStartX) && onRulerY (dragStartY);
                if (!startOnRuler)
                {
                    auto rows = rowRects ();
                    int rowIdx = -1;
                    for (size_t r = 0; r < rows.size (); ++r)
                        if (dragStartY >= rows[(size_t) r].getY()) rowIdx = (int) r;
                    if (rowIdx >= 0)
                        zr = zr.getIntersection (rows[(size_t) rowIdx]);
                }
                zoomRect = zr;
                repaint();
            }
            break;
        }
        default: break;
    }
}

void SpectralView::mouseUp (const juce::MouseEvent&)
{
    // Finish a new Arrow/Rectangle/Freehand drawing. Accidental clicks
    // (drags under 3 px in either axis) and near-empty freehand strokes are
    // discarded without creating an item.
    if (drawingNew_)
    {
        drawingNew_ = false;
        havePendingDraw_ = false;

        DrawingItem item;
        bool ok = false;
        const double tEnd = xToTime ((double) drawEndX_);
        const double fEnd = freqAtY (drawEndY_);
        if (pendingDraw_ == DrawingItem::Kind::Arrow
            || pendingDraw_ == DrawingItem::Kind::Rectangle)
        {
            if (std::abs (drawEndX_ - drawStartX_) >= 3
                && std::abs (drawEndY_ - drawStartY_) >= 3)
            {
                item.kind = pendingDraw_;
                std::pair<double, double> p0 { drawStartT_, drawStartF_ };
                std::pair<double, double> p1 { tEnd, fEnd };
                item.points.push_back (p0);
                item.points.push_back (p1);
                ok = true;
            }
        }
        else if (pendingDraw_ == DrawingItem::Kind::Freehand && freePts_.size() >= 3)
        {
            item.kind = pendingDraw_;
            item.points = freePts_;
            ok = true;
        }

        freePts_.clear ();
        repaint();
        if (ok)
        {
            // New shapes take the Drawing panel's current ink colour and
            // line width, exactly like text items do.
            item.colourARGB = app.settings.drawColourARGB;
            item.widthPx    = app.settings.drawWidth;
            if (onDrawingCreated) onDrawingCreated (item);
        }
        return;
    }

    if (dragMode == DragMode::ZoomDrag)
    {
        if (!haveZoomRect)
        {
            // Right-clicking the frequency axis (left or right) makes
            // "Zoom Out Full" reset the vertical range instead of the time
            // window.
            const bool onAxis = onAxisX (dragStartX);
            // Only show the menu when there is something to do with it: an
            // external action callback, or a local axis reset. (Headless test
            // views have neither and must not enter a modal loop.)
            if (onAxis || onContextAction)
                showViewContextMenu (app,
                                     xToTime ((double) dragStartX), freqAtY (dragStartY),
                                     onAxis, autoFftSizeForCurrentZoom (),
                                     [this, onAxis] (int id)
                {
                    using enum ContextMenuIDs;
                    if (id == CtxZoomOutFull && onAxis) { resetFrequencyRange(); return; }
                    // Move/Scale start a local gesture in this view.
                    if (id == CtxMoveSelection || id == CtxScaleSelection)
                    {
                        app.startSelGesture (id == CtxScaleSelection);
                        return;
                    }
                    if (onContextAction) onContextAction (id);
                });
        }
        else
        {
            // The drag START position selects which axes get zoomed:
            //  - inside the plot ("the track"): BOTH axes — time from the box's
            //    x span, frequency from its y span;
            //  - on the frequency axis: vertical only;
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

            auto zoomFreq = [this] ()
            {
                // Set the frequency axis to the dragged span, mapped through
                // the channel row containing the drag start.
                const auto rows = rowRects ();
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
            else if (startOnRuler)
                zoomTime ();
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
                                              app.brushRadiusPct (), toolSubtract, toolReplace);
                    haveLineAnchor = true;
                    lineAnchorT = lineEndT;
                    lineAnchorF = lineEndF;
                    changed = true;
                    break;
                default:   // Paint
                    if (!strokePts.empty())
                    {
                        d->spectralSel.addStroke (strokePts, app.brushRadiusPct (),
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
            // Like the time-selection tool, jump to the committed selection's
            // start so playback begins where the selected region does.
            if (app.selection.getLength() > 1e-9)
                app.setPlayhead (app.selection.getStart());
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
    reportHoverTime (e, false);

    // Move/Scale gesture: the live preview follows the cursor even without a
    // button held (must not depend on onCursorInfo being set).
    if (app.haveSelGestureRef ())
        updateSelGestureFromEvent (e);

    // Harmonic markers (Spectral Controls panel): follow the cursor so the
    // white circles track it. Only repaint when the position changed.
    if (app.settings.specHarmVisible && app.settings.specHarmMaxK >= 1
        && app.settings.specHarmN >= 1)
    {
        if (harmMouseX != e.x || harmMouseY != e.y)
        {
            harmMouseX = e.x;
            harmMouseY = e.y;
            repaint();
        }
    }

    // Table overlay hover values (spectrogram mode): show the raw parameter
    // value of curves near the cursor - like the dedicated track's readout.
    if (app.showAllTables && app.tablesMode () == 1)
    {
        collectTableHoverLabels (e.x, e.y);
        repaint();
    }

    if (!onCursorInfo) return;
    const auto pr = plotRect();
    if (e.x < pr.getX() || e.x >= pr.getRight()
        || e.y < pr.getY() || e.y >= pr.getBottom()) { onCursorInfo (""); return; }

    // Which channel row is the cursor in?
    const auto rows = rowRects ();
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

    // Auto FFT-size mode: probe with the same effective size the image used,
    // so the tiles are already cached and the bin mapping matches.
    const float dB = spectral.probeMagnitude (app.audioEngine(), ch, t, freq,
                                              effectiveSettingsForRender (settings_, row.getHeight()), level);

    // With a scale calibration the probe shows absolute levels
    // (dB-SPL for Pascal files; linear V/g values for volts/grams files)
    // instead of relative dB.

    // Time part follows the active time-axis scale mode.
    juce::String tText;
    switch (app.timeAxisMode)
    {
        case TimeAxisMode::Sample:     tText = "t=" + formatDisplayedTime (t, app.timeAxisMode,
                                                                           app.timeOffsetSecs, rate) + " smp"; break;
        case TimeAxisMode::TimeOffset:
            tText = "t=" + formatDisplayedTime (t, app.timeAxisMode, app.timeOffsetSecs, rate); break;
        default:                       tText = "t=" + juce::String (t, 3) + " s"; break;
    }

    const double factor = (app.specFreqScaleMode == 1) ? app.specFreqFactor : 1.0;
    juce::String info = tText + "   f=" + formatFreqAxisLabel (freq, factor)
                        + "   " + ScaleCalibration::formatSpectralLevel (dB, app.scale(), ch);

    // Alternative scale overlays: every enabled reference, by name.
    for (const auto& r : app.altTimeRefs)
        if (r.enabled && std::abs (r.refMain) > 1e-9)
            info += "   | " + r.name + "=" + juce::String (t * r.altValue / r.refMain, 3);
    for (const auto& r : app.altFreqRefs)
        if (r.enabled && std::abs (r.refMain) > 1e-9)
            info += "   | " + r.name + "=" + juce::String (freq * r.altValue / r.refMain, 3);

    if (onCursorInfo)
        onCursorInfo (info);
}

void SpectralView::reportHoverTime (const juce::MouseEvent& e, bool dragging)
{
    if (!onHoverTime) return;
    const auto pr = plotRect();
    const bool loaded = app.audioEngine().isFileLoaded() && pr.getWidth() > 0;
    const bool inside = e.x >= pr.getX() && e.x < pr.getRight()
                        && e.y >= pr.getY() && e.y < pr.getBottom();
    if (!loaded || (!dragging && !inside)) { onHoverTime (-1.0); return; }
    const int x = std::clamp (e.x, pr.getX(), pr.getRight() - 1);
    onHoverTime (std::clamp (xToTime (x), 0.0, app.audioEngine().duration()));
}

void SpectralView::mouseExit (const juce::MouseEvent&)
{
    if (onHoverTime) onHoverTime (-1.0);
    if (onCursorInfo) onCursorInfo ("");

    // Harmonic markers: hide the circles once the cursor leaves the view.
    if (harmMouseX >= 0 || harmMouseY >= 0)
    {
        harmMouseX = -1;
        harmMouseY = -1;
        if (app.settings.specHarmVisible && app.settings.specHarmMaxK >= 1
            && app.settings.specHarmN >= 1) repaint();
    }

    // Table overlay hover values: hide them once the cursor leaves the view.
    if (!tblHoverLabels.empty ())
    {
        tblHoverLabels.clear ();
        tblHoverPos = { -1.0f, -1.0f };
        repaint();
    }
}

void SpectralView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!app.audioEngine().isFileLoaded()) return;
    const double factor = wheel.deltaY > 0 ? 1.2 : (1.0 / 1.2);

    // Wheel over the frequency axis (left or right, or Shift+wheel anywhere)
    // zooms the frequency range around the cursor; otherwise it zooms in
    // time at the cursor.
    if (e.mods.isShiftDown() || onAxisX (e.x))
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
    // Two quick clicks on a channel number are two mute toggles, not a zoom.
    if (channelAtLabel (e.getPosition()) >= 0)
        return;

    // A move/scale gesture click must not also trigger "fit whole file": the
    // applying second click of a quick click-click sequence would otherwise
    // double-fire.
    if (juce::Time::getMillisecondCounter () - selGestLastClickMs < 400)
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
                app.selectionRow = -1;   // marker selections span all rows
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
    repaint();   // e.g. Ctrl+0 with the time window already full: no other setter
                 // fires, so this must schedule the re-render itself.
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

bool SpectralView::saveAsPng (const juce::File& file,
                             const std::atomic<bool>* cancel,
                             const SpectralExportOptions& opts)
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || mm->isThisTheMessageThread())
    {
        const auto job = preparePngExport (file, opts);
        return job != nullptr && job (cancel);
    }

    // Called from a worker thread: the view state must not be read here
    // (paint() and the UI keep mutating it). Take the snapshot on the
    // message thread, then run the heavy part on this thread. Polling (not
    // a blocking get) so a cancel or a message that is never delivered
    // cannot hang this thread.
    auto promise = std::make_shared<std::promise<PngExportJob>>();
    auto future = promise->get_future();
    if (!juce::MessageManager::callAsync ([this, promise, file, opts]
            { promise->set_value (preparePngExport (file, opts)); }))
        return false;
    for (;;)
    {
        if (future.wait_for (std::chrono::milliseconds (20)) == std::future_status::ready)
            break;
        if (cancel != nullptr && cancel->load())
            return false;   // the async snapshot (if it still runs) is discarded
    }
    PngExportJob job;
    try { job = future.get(); }
    catch (const std::future_error&) { return false; }   // message dropped unrun
    return job != nullptr && job (cancel);
}

SpectralView::PngExportJob SpectralView::preparePngExport (const juce::File& file,
                                                           const SpectralExportOptions& opts)
{
    // Everything that reads the component happens HERE, on the message
    // thread; the returned job only uses the values captured below.
    jassert (juce::MessageManager::getInstanceWithoutCreating() == nullptr
             || juce::MessageManager::existsAndIsCurrentThread());
    if (!app.audioEngine().isFileLoaded() || channels.empty()) return {};
    const auto pr = plotRect();
    const int pw = pr.getWidth(), ph = pr.getHeight();
    if (pw <= 0 || ph <= 0) return {};

    Settings rs = settings_;
    rs.minFrequency = (float) std::max (1.0, freqMin);
    rs.maxFrequency = (float) std::min (app.audioEngine().sampleRate() * 0.5, freqMax);
    const RenderSnapshot snap = makeRenderSnapshot();

    // The export canvas follows the current axis/ruler placement (View
    // menu); with axes, the axis strip and the time ruler are added on
    // whichever side they currently sit.
    // An export always carries its own ruler (independent of a stacked live
    // view, which hides its ruler because the other view draws it).
    const int exportRuler = app.rulerStackHeight();
    const int ox = (opts.axes && !axisOnRight()) ? axisW() : 0;
    const int oy = (opts.axes && !rulerAtBottom()) ? exportRuler : 0;
    const int W = pw + (opts.axes ? axisW() : 0);
    const int H = ph + (opts.axes ? exportRuler : 0);

    // Overlays are cheap: compose them now into a transparent layer laid
    // over the spectral plot by the job. For this block only, plotRect() /
    // rulerH() and everything built on them (row rects, timeToX, rulers,
    // overlays) address the export canvas instead of the live bounds.
    // Software images: no platform/GPU image thread affinity for the job.
    juce::Image overlay;
    if (opts.axes || opts.selections || opts.markers || opts.drawings)
    {
        overlay = juce::Image (juce::Image::ARGB, W, H, true, juce::SoftwareImageType());
        haveExportLayout_ = true;
        exportLayout_ = juce::Rectangle<int> (ox, oy, pw, ph);
        exportRulerH_ = opts.axes ? app.rulerStackHeight() : 0;
        {
            juce::Graphics g (overlay);
            if (opts.axes)
            {
                // Opaque ruler background so the labels stay readable over the spectrum.
                g.setColour (juce::Colour::fromRGB (32, 32, 36));
                if (axisOnRight())
                    g.fillRect (W - axisW(), 0, axisW(), H);
                else
                    g.fillRect (0, 0, axisW(), H);
                drawFrequencyRuler (g);
                drawTimeRuler (g);
                drawChannelLabels (g, false);   // an export never carries the mute state
            }

            if (opts.selections)
            {
                drawTimeSelectionOverlay (g);
                drawSpectralShapesOverlay (g, false);   // committed shapes only
            }
            if (opts.markers)  drawMarkersOverlay (g);
            if (opts.drawings) drawDrawings (g, false); // committed items only
        }
        haveExportLayout_ = false;
        exportRulerH_ = 0;
    }

    SpectralEngine* const engine = &spectral;
    AudioEngine* const audio = &app.audioEngine();   // pinned: not whatever doc is active later
    return [engine, audio, snap, rs, overlay, file, ox, oy, pw, ph, W, H]
           (const std::atomic<bool>* cancel) -> bool
    {
        // The spectral content is the only long-running step.
        juce::Image plotImg (juce::Image::ARGB, pw, ph, true, juce::SoftwareImageType());
        if (!renderSpectralRows (*engine, *audio, snap, plotImg, rs, cancel))
            return false;   // cancelled: no PNG is written

        juce::Image img (juce::Image::ARGB, W, H, true, juce::SoftwareImageType());
        {
            juce::Graphics g (img);
            g.fillAll (juce::Colours::black);
            g.drawImageAt (plotImg, ox, oy);
            if (overlay.isValid())
                g.drawImageAt (overlay, 0, 0);
        }

        // Extract raw pixels through one BitmapData (not getPixelAt per
        // pixel) and write a PNG with our own writer.
        std::vector<uint8_t> rgba ((size_t) W * H * 4);
        {
            const juce::Image::BitmapData bd (img, juce::Image::BitmapData::readOnly);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    const juce::Colour c = bd.getPixelColour (x, y);
                    const size_t o = ((size_t) y * W + x) * 4;
                    rgba[o + 0] = (uint8_t) c.getRed();
                    rgba[o + 1] = (uint8_t) c.getGreen();
                    rgba[o + 2] = (uint8_t) c.getBlue();
                    rgba[o + 3] = (uint8_t) c.getAlpha();
                }
        }
        return PngWriter::writePng (file, W, H, rgba);
    };
}

void SpectralView::zoomFrequencyBy (double factor)
{
    // Keyboard vertical zoom: zoom the frequency range around its centre.
    if (!app.audioEngine().isFileLoaded()) return;
    const auto pr = plotRect();
    zoomFreqAtY ((float) (pr.getY() + pr.getHeight() * 0.5), factor);
}

void SpectralView::panByTime (double dt)
{
    const double dur = app.audioEngine().duration();
    const double w = viewEnd - viewStart;
    const double ns = std::clamp (viewStart + dt, 0.0, std::max (0.0, dur - w));
    setWindow (ns, ns + w);
}

void SpectralView::panFrequencyByCursorY (int y)
{
    // Incremental half-speed grab, same feel as the waveform view's axis
    // drag: content follows the cursor at 0.5 px per cursor pixel and the
    // delta is measured from the last delivered position, so reversing
    // direction responds immediately (deltas eaten by an edge pin do not
    // accumulate). Log axes shift in log space (half a step = square root of
    // the full-step ratio) so the zoom level is preserved.
    const auto rows = rowRects ();
    if (rows.empty()) { panLastY = y; return; }

    juce::Rectangle<int> row = rows.front();
    for (const auto& rr : rows)
        if (rr.contains (dragStartX, dragStartY)) { row = rr; break; }

    const int dy = y - panLastY;
    panLastY = y;
    if (dy == 0 || row.getHeight() <= 0) return;

    const double nyq = app.audioEngine().sampleRate() * 0.5;
    if (settings_.logFrequency && freqMin > 1e-9)
    {
        // Full-step ratio between the previous and current cursor positions,
        // clamped to the row so a cursor outside it cannot run away.
        const float yPrev = std::clamp ((float) (y - dy), (float) row.getY(), (float) row.getBottom());
        const float yNow  = std::clamp ((float) y,        (float) row.getY(), (float) row.getBottom());
        const double Rfull = yToFreq ((float) (yPrev - row.getY()), row.getHeight())
                           / yToFreq ((float) (yNow  - row.getY()), row.getHeight());
        const double R = std::sqrt (std::max (1e-30, Rfull));
        double lo2 = freqMin * R, hi2 = freqMax * R;
        if (lo2 < 1.0) lo2 = 1.0;          // pin the bottom edge
        else if (hi2 > nyq) hi2 = nyq;     // pin the top edge
        setFrequencyRange (lo2, hi2);
    }
    else
    {
        // yToFreq is linear in pixels: half the per-pixel step.
        const double dFull = (freqMax - freqMin) * (double) dy / row.getHeight();
        double lo2 = freqMin + kPanSpeedFactor * dFull, hi2 = freqMax + kPanSpeedFactor * dFull;
        if (lo2 < 1.0) lo2 = 1.0;          // pin the bottom edge
        else if (hi2 > nyq) hi2 = nyq;     // pin the top edge
        setFrequencyRange (lo2, hi2);
    }
}

void SpectralView::zoomFreqAtY (float y, double factor)
{
    // Map the absolute screen position into the plot area first.
    const auto pr = plotRect();
    const float yc = std::clamp (y, (float) pr.getY(), (float) pr.getBottom());

    const double rate = app.audioEngine().sampleRate();
    const double fCursor = std::clamp (yToFreq (yc - (float) pr.getY(), pr.getHeight()), 1.0, rate * 0.5);
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
    sc::diag::ScopedPaint diagPaint ("spectral", getWidth(), getHeight());   // paint.slow above 50 ms
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

    // Re-render the spectral image when the view range, size or the audio
    // content itself changed: a recording mutates the doc in place (append,
    // or loop-mode overwrite at constant length), which must refresh the
    // picture even though the time window did not move.
    const int64_t cvNow = app.audioEngine().contentVersion();
    if (imageDirty || !haveImage || w != imgW || h != imgH
        || viewStart != imgT0 || viewEnd != imgT1 || cvNow != imgContentVer)
    {
        // The engine uses the passed Settings for everything: feed it this
        // view's local frequency limits.
        Settings rs = settings_;
        rs.minFrequency = (float) std::max (1.0, freqMin);
        rs.maxFrequency = (float) std::min (app.audioEngine().sampleRate() * 0.5, freqMax);

        // Reuse the image while its size holds (no full-frame reallocation
        // on every re-render, e.g. each recorded block or pan step).
        if (!cachedImage.isValid() || cachedImage.getWidth() != w || cachedImage.getHeight() != h)
            cachedImage = juce::Image (juce::Image::ARGB, w, h, true);
        else
            cachedImage.clear (cachedImage.getBounds());
        renderSpectralContent (cachedImage, rs);

        imgW = w; imgH = h; imgT0 = viewStart; imgT1 = viewEnd;
        imgContentVer = cvNow;
        haveImage = true;
        imageDirty = false;
    }

    if (haveImage)
    {
        g.drawImage (cachedImage,
                     juce::Rectangle<float> ((float) pr.getX(), (float) pr.getY(),
                                             (float) w, (float) h),
                     juce::RectanglePlacement::fillDestination);

        // Muted channels (click on their number) are drawn darker; the
        // cached picture itself stays as rendered.
        const auto rows = rowRects ();
        for (size_t r = 0; r < rows.size() && r < channels.size(); ++r)
            if (app.audioEngine().isChannelMuted (channels[r]))
            {
                g.setColour (juce::Colours::black.withAlpha (0.55f));
                g.fillRect (pr.getX(), rows[r].getY(), pr.getWidth(), rows[r].getHeight());
            }
    }

    // Opaque ruler background so the labels stay readable over the spectrum.
    g.setColour (juce::Colour::fromRGB (32, 32, 36));
    if (axisOnRight())
        g.fillRect (getWidth() - axisW(), 0, axisW(), getHeight());
    else
        g.fillRect (0, 0, axisW(), getHeight());
    drawFrequencyRuler (g);
    drawThirdOctaveGrid (g);
    drawTimeRuler (g);

    // Channel labels on the left of each track: "L"/"R" for stereo files,
    // channel numbers otherwise. Shown even when a single channel is visible
    // so the axis always says which track it is.
    drawChannelLabels (g);

    drawOverlays (g);
    drawDrawings (g);
    drawTablesOverlay (g);
    drawTableHoverLabels (g);

    // Right-drag box-zoom rubber band: the same translucent blue as the
    // time selection in the tracks, so it reads as "this region" rather
    // than a generic white marquee. When the drag starts on the frequency
    // axis the band stretches across its full width (matching what mouseUp
    // will actually zoom).
    if (haveZoomRect && dragMode == DragMode::ZoomDrag)
    {
        auto zr = zoomRect;
        const bool startOnAxis  = onAxisX (dragStartX);
        const bool startOnRuler = !startOnAxis && onRulerY (dragStartY);
        if (startOnAxis)
            zr = juce::Rectangle<int> (axisOnRight() ? getWidth() - axisW() : 0,
                                       zr.getY(), axisW(), zr.getHeight());
        else if (startOnRuler)
            zr = juce::Rectangle<int> (zr.getX(), rulerAtBottom() ? getHeight() - rulerH() : 0,
                                       zr.getWidth(), rulerH());

        g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
        g.fillRect (zr);
        g.setColour (juce::Colour::fromRGB (96, 150, 255).withAlpha (0.9f));
        g.drawRect (zr, 1);
    }
}

// Renders the spectral content (one row per visible channel with dark gaps)
// into an ARGB image of size w×h. Used by paint() and saveAsPng().
Settings SpectralView::effectiveSettingsForRender (const Settings& base, int rowHeightPx) const
{
    return effectiveSettingsFor (base, rowHeightPx, makeRenderSnapshot());
}

SpectralView::RenderSnapshot SpectralView::makeRenderSnapshot() const
{
    RenderSnapshot s;
    s.channels = channels;
    s.t0 = viewStart;
    s.t1 = viewEnd;
    s.fMin = freqMin;
    s.fMax = freqMax;
    s.sampleRate = app.audioEngine().sampleRate();
    return s;
}

Settings SpectralView::effectiveSettingsFor (const Settings& base, int rowHeightPx,
                                             const RenderSnapshot& s)
{
    Settings rs = base;
    // The frequency axis may have been zoomed/panned since the panel last
    // sent its settings: render with this view's live limits, not whatever
    // minFrequency/maxFrequency the caller's copy still carries.
    rs.minFrequency = (float) std::max (1.0, s.fMin);
    rs.maxFrequency = (float) std::min (s.sampleRate * 0.5, s.fMax);
    if (rs.fftAuto)
        rs.fftSize = SpectralEngine::chooseFftSize (s.t1 - s.t0,
                                                    s.sampleRate,
                                                    std::max (1, rowHeightPx),
                                                    s.fMin, s.fMax,
                                                    rs.autoTargetTimeSlices,
                                                    rs.autoBinsPerPixel);
    return rs;
}

int SpectralView::autoFftSizeForCurrentZoom() const
{
    if (!app.audioEngine().isFileLoaded())
        return 2048;   // no file: fall back to the default manual size
    int h = std::max (1, plotRect().getHeight());
    if (!channels.empty ())
    {
        const auto rows = WaveformView::channelRowRects (layoutWidth(),
                                                         plotRect().getHeight(),
                                                         (int) channels.size());
        if (!rows.empty()) h = std::max (1, rows[0].getHeight());
    }
    return SpectralEngine::chooseFftSize (viewEnd - viewStart,
                                          app.audioEngine().sampleRate(),
                                          h, freqMin, freqMax,
                                          settings_.autoTargetTimeSlices,
                                          settings_.autoBinsPerPixel);
}

bool SpectralView::renderSpectralContent (juce::Image& img, const Settings& rs,
                                          const std::atomic<bool>* cancel)
{
    return renderSpectralRows (spectral, app.audioEngine(), makeRenderSnapshot(),
                               img, rs, cancel);
}

// Reads nothing but its arguments: runs on the message thread (paint) or on
// an export worker thread (preparePngExport job).
bool SpectralView::renderSpectralRows (SpectralEngine& spectral, AudioEngine& audio,
                                       const RenderSnapshot& s, juce::Image& img,
                                       const Settings& rs, const std::atomic<bool>* cancel)
{
    const auto& channels = s.channels;
    auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };

    const int w = img.getWidth(), h = img.getHeight();
    if (w <= 0 || h <= 0) return true;

    const juce::Image::BitmapData bd (img, 0, 0, w, h, juce::Image::BitmapData::writeOnly);

    // One row per visible channel (like the waveform view), separated by
    // dark gaps of the axis colour. Each row gets its own frequency axis on
    // the left.
    const auto rows = WaveformView::channelRowRects (w, h, (int) channels.size());
    std::vector<uint8_t> pixels;   // reused across rows (the engine re-assigns it)
    for (size_t r = 0; r < rows.size(); ++r)
    {
        if (cancelled()) return false;

        const int ch = channels[r];
        const auto rr = rows[r];
        if (rr.getHeight() <= 0) continue;

        // Auto FFT-size mode: the effective size depends on this row's height
        // and the current zoom levels, so it is resolved per row from the
        // caller's base settings (which carry the live frequency limits).
        const Settings rsRow = effectiveSettingsFor (rs, rr.getHeight(), s);
        spectral.renderToPixels (audio, rsRow, w, rr.getHeight(), s.t0, s.t1, { ch }, pixels, cancel);
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

    // Paint the gap rows in the axis background colour so the channel tracks
    // are separated by a dark band matching the left axis.
    if (rows.size() > 1)
    {
        for (size_t r = 1; r < rows.size(); ++r)
        {
            const int y0 = rows[r - 1].getBottom();
            const int y1 = std::min (h, rows[r].getY());
            for (int y = y0; y < y1; ++y)
            {
                uint8_t* row = static_cast<uint8_t*> (bd.data) + (size_t) y * bd.lineStride;
                for (int x = 0; x < w; ++x)
                    ((juce::PixelARGB*) row)[x] = juce::PixelARGB (255, 32, 32, 36);
            }
        }
    }
    return true;
}

// Top time ruler: same tick scheme and style as the waveform view's, so the
// spectrogram keeps a visible time axis even when it is shown on its own
// (F9 "spectrogram only" mode).
void SpectralView::drawTimeRuler (juce::Graphics& g)
{
    const auto pr = plotRect();
    const double w = viewEnd - viewStart;
    if (w <= 0 || pr.getWidth() <= 0 || rulerH() <= 0)
        return;

    // The ruler strip sits at the top by default or at the bottom when moved
    // there via the View menu; ticks point from the plot edge into it.
    // Relative to the plot rect so offscreen exports lay out identically. The
    // stack holds the main ruler plus one narrower strip per extra time axis.
    const int stackH = rulerH();
    const auto mainStrip = sc::axes::timeStrip (0, pr.getX(), pr.getWidth(),
                                                pr.getY(), pr.getBottom(), stackH,
                                                rulerAtBottom());
    const int rulerTop = mainStrip.getY();
    const int rulerHgt = mainStrip.getHeight();
    const float sepY   = rulerAtBottom() ? (float) (mainStrip.getY() + 1)
                                         : (float) (pr.getY() - 1);
    const float tickEnd   = rulerAtBottom() ? sepY + 7.0f : sepY - 7.0f;
    const float minorEnd  = rulerAtBottom() ? sepY + 3.0f : sepY - 3.0f;

    // Opaque background so the labels stay readable over the spectrum - over
    // the strip itself, which is below the plot when the ruler is at the
    // bottom and above it when the ruler is at the top. Painting it at
    // plotTop - stackH in the bottom case covered the top of the spectrogram
    // and left the ruler area unpainted (a black bar).
    const int stackTop = rulerAtBottom() ? pr.getBottom()
                                         : pr.getY() - stackH;
    g.setColour (sc::axes::axisStripFill());
    g.fillRect (pr.getX(), stackTop, pr.getWidth(), stackH);

    // Ticks are placed at nice values of the DISPLAYED axis: seconds for the
    // time modes, sample counts for Sample.
    const double pxPerSec = pr.getWidth() / w;
    const TimeAxisMode mode = app.timeAxisMode;
    const double off = app.timeOffsetSecs;
    const double sc  = (app.timeScaleFactor != 0.0 ? app.timeScaleFactor : 1.0);
    const double rate = std::max (1.0, app.audioEngine().sampleRate());

    auto dispToTime = [mode, off, rate, sc] (double d) -> double
    {
        if (mode == TimeAxisMode::Sample)
            return (d / sc) / rate - off;
        return (d / sc) - off;
    };

    const double dispStart = timeToDisplayValue (viewStart, mode, off, rate, sc);
    const double dispEnd   = timeToDisplayValue (viewEnd,   mode, off, rate, sc);
    const double pxPerUnit = pxPerSec * sc;
    const double major = (mode == TimeAxisMode::Sample)
                         ? niceSampleStep ((rate / pxPerSec) * sc)
                         : niceTimeStep (pxPerUnit);
    // Subdivide each major interval into five when there is room for it.
    // (In Sample mode `major` counts samples, so convert to pixels first.)
    const double majorPx = (mode == TimeAxisMode::Sample)
                           ? (major / (rate * sc)) * pxPerSec : major * pxPerUnit;
    const bool showMinors = majorPx >= 40.0;

    g.setColour (sc::axes::axisGridColour());
    g.drawLine ((float) pr.getX(), sepY, (float) pr.getRight(), sepY, 1.0f);

    const double first = std::ceil (dispStart / major) * major;
    if (first > dispEnd + 1e-9)
        return;   // the visible span is shorter than one tick step

    g.setFont (sc::axes::axisLabelFont());
    std::vector<std::pair<float, double>> axisTicks;   // x -> real seconds
    const int nTicks = (int) std::floor ((dispEnd - first) / major + 1e-9) + 1;
    for (int i = 0; i < nTicks; ++i)
    {
        const double d = first + (double) i * major;
        if (d > dispEnd + 1e-9) break;
        const double t = dispToTime (d);
        if (t < viewStart - 1e-9 || t > viewEnd + 1e-9) continue;
        const float x = timeToX (t);
        axisTicks.push_back ({ x, t });

        g.setColour (sc::axes::axisGridColour());
        g.drawLine (x, std::min (sepY, tickEnd), x, std::max (sepY, tickEnd), 1.0f);

        if (showMinors)
            for (int j = 1; j < 5; ++j)
            {
                const double tm = dispToTime (d + (double) j * major / 5.0);
                if (tm > viewEnd + 1e-9 || tm < viewStart - 1e-9) break;
                g.drawLine (timeToX (tm), std::min (sepY, minorEnd),
                            timeToX (tm), std::max (sepY, minorEnd), 1.0f);
            }

        juce::String label;
        switch (mode)
        {
            case TimeAxisMode::Sample:     label = juce::String ((int64_t) std::llround (d)); break;
            case TimeAxisMode::Seconds:    label = juce::String (d, 4); break;
            case TimeAxisMode::TimeOffset: label = formatClockTime (d, 4); break;
            default:                       label = formatTimeTickLabel (d, major, w); break;
        }

        g.setColour (sc::axes::axisLabelColour());
        // Below the ticks when the ruler hangs under the plot (its ticks point
        // down from the top edge); at the top of the strip otherwise.
        const int labelY = rulerAtBottom() ? rulerTop + 9 : rulerTop + 2;
        const int labelH = std::max (10, rulerHgt - (rulerAtBottom() ? 11 : 4));
        g.drawText (label, (int) x + 3, labelY, 90, labelH,
                    juce::Justification::centredLeft, false);
    }

    int index = 1;
    for (const auto& e : sc::axes::extras (app.axisEntries, sc::axes::Kind::Time))
    {
        const auto strip = sc::axes::timeStrip (index, pr.getX(), pr.getWidth(),
                                                pr.getY(), pr.getBottom(), stackH,
                                                rulerAtBottom());
        sc::axes::drawExtraTimeAxis (g, e, strip, axisTicks, rate);
        ++index;
    }
}

// One frequency axis per visible channel row: tick lines across the plot and
// labels in the left ruler area for each row.
juce::String SpectralView::formatFreqAxisLabel (double fHz, double factor)
{
    if (std::abs (factor - 1.0) < 1e-12)
        return fHz >= 1000 ? juce::String (fHz / 1000.0, 1) + " kHz"
                           : juce::String ((int) std::lround (fHz)) + " Hz";

    // Custom units: the scaled value as a plain number (no unit suffix).
    const double d = fHz * factor;
    if (std::abs (d) >= 100.0)
        return juce::String ((int) std::lround (d));
    juce::String s = juce::String (d, 2);
    while (s.endsWith ("0")) s = s.dropLastCharacters (1);
    if (s.endsWith (".")) s = s.dropLastCharacters (1);
    return s;
}

void SpectralView::drawFrequencyRuler (juce::Graphics& g)
{
    const double rate = app.audioEngine().sampleRate();
    const double fMax = std::min (rate * 0.5, freqMax);
    const double fMin = std::max (1.0, freqMin);

    // A channel label sits at the vertical centre of each row; keep tick
    // labels out of that zone.
    const bool hasChanLabel = (channels.size() > 1 || app.audioEngine().numChannels() == 2);

    const auto rows = rowRects ();
    g.setFont (sc::axes::axisLabelFont());
    std::vector<std::pair<float, double>> axisTicks;   // y -> Hz, reused by the extra axes

    // The main axis labels stay inside the main strip (nearest the plot); the
    // outermost one is inset by the room the channel label needs.
    const sc::axes::List freqExtras = sc::axes::extras (app.axisEntries, sc::axes::Kind::Frequency);
    const int nFreq = 1 + (int) freqExtras.size();
    const auto mainLab = sc::axes::verticalLabelRect (0, nFreq, axisW(), plotRect().getX(),
                                                      layoutWidth() - (axisOnRight() ? axisW() : 0),
                                                      plotRect().getY(), plotRect().getBottom(),
                                                      axisOnRight(), sc::axes::channelGutter());

    for (const auto& rr : rows)
    {
        if (rr.getHeight() <= 0) continue;
        // Labels only when the row is tall enough to hold them.
        const bool showLabels = rr.getHeight() >= 48;

        auto drawTickAndLabel = [&](double f, bool minor = false, bool withLabel = true)
        {
            const float y = (float) rr.getY() + freqToY (f, rr.getHeight());
            // Grid line spans the plot only; labels sit in the axis strip on
            // whichever side it is placed.
            const float tickX0 = axisOnRight() ? 0.0f : (float) axisW();
            const float tickX1 = axisOnRight() ? (float) (layoutWidth() - axisW())
                                               : (float) layoutWidth();
            // The grey grid line can be hidden from the View menu; the axis
            // labels below are unaffected. Same colour as the other tracks.
            if (app.settings.showFreqGrid)
            {
                g.setColour (minor ? sc::axes::axisGridColour().darker (0.35f)
                                   : sc::axes::axisGridColour());
                g.drawLine (tickX0, y, tickX1, y, 1.0f);
            }
            if (!showLabels || !withLabel) return;
            if (hasChanLabel && std::abs (y - ((float) rr.getY() + rr.getHeight() * 0.5f)) < 10.0f)
                return; // room for the channel label
            // Drop labels that would collide with the neighbouring row's edge
            // label at a shared boundary (or be clipped at the component
            // edge); keep the tick line itself.
            if ((float) y < rr.getY() + 13.0f || (float) y > rr.getBottom() - 1.0f)
                return;
            g.setColour (minor ? sc::axes::axisMinorLabelColour()
                               : sc::axes::axisLabelColour());
            const double factor = (app.specFreqScaleMode == 1) ? app.specFreqFactor : 1.0;
            if (! minor) axisTicks.push_back ({ y, f });
            // Each axis keeps to its own strip: an extra frequency axis used to
            // be drawn on top of these labels on the left side.
            g.drawText (formatFreqAxisLabel (f, factor),
                       mainLab.getX(), (int) y - 12, mainLab.getWidth(), 12,
                       juce::Justification::bottomLeft, false);
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

    // Extra frequency axes: the same tick positions, labelled in each entry's
    // own units, in the strips further out. The engine renders one frequency
    // window for every channel row, so a frequency axis is always shared.
    if (axisTicks.empty()) return;
    const int axW = axisW();
    int index = 1;
    for (const auto& e : freqExtras)
    {
        const auto strip = sc::axes::verticalStrip (index, axW, plotRect().getX(),
                                                    layoutWidth() - (axisOnRight() ? axW : 0),
                                                    plotRect().getY(), plotRect().getBottom(),
                                                    axisOnRight());
        if (axisOnRight() ? strip.getRight() > layoutWidth() : strip.getX() < 0) break;

        // Secondary axis: the same smaller type the extra time axes use.
        g.setFont (juce::Font { juce::FontOptions (8.5f) });
        g.setColour (sc::axes::axisMinorLabelColour());
        const auto lab = sc::axes::verticalLabelRect (index, nFreq, axW, plotRect().getX(),
                                                      layoutWidth() - (axisOnRight() ? axW : 0),
                                                      plotRect().getY(), plotRect().getBottom(),
                                                      axisOnRight(), sc::axes::channelGutter());
        for (const auto& t : axisTicks)
        {
            const auto box = juce::Rectangle<int> (lab.getX(), (int) t.first - 12, lab.getWidth(), 12);
            // With the unit spelled out: an extra axis in Hz must read as Hz.
            g.drawText (sc::axes::formatFrequencyAxis (e, t.second, rate * 0.5), box,
                        axisOnRight() ? juce::Justification::bottomLeft
                                      : juce::Justification::bottomRight, true);
        }
        ++index;
    }
}

std::optional<float> SpectralView::thirdOctaveLabelY (float yc, float topY, float botY,
                                                      const juce::Rectangle<int>& row,
                                                      bool hasChanLabel, float labH)
{
    auto fits = [&] (float cy) -> bool
    {
        const float t = cy - labH * 0.5f, b = cy + labH * 0.5f;
        if (t < topY || b > botY) return false;              // inside the band
        if (t < row.getY() || b > row.getBottom()) return false;   // inside the row
        const float c = (float) row.getY() + row.getHeight() * 0.5f;
        if (hasChanLabel && t < c + 8.0f && b > c - 8.0f)
            return false;                                    // clear of L/R / "N"
        return true;
    };

    if (fits (yc)) return yc;

    // The centre collides with the channel label: try just above, then below.
    const float c = (float) row.getY() + row.getHeight() * 0.5f;
    if (hasChanLabel && std::abs (yc - c) < 8.0f + labH * 0.5f)
    {
        if (fits (c - (8.0f + labH * 0.5f))) return c - (8.0f + labH * 0.5f);
        if (fits (c + (8.0f + labH * 0.5f))) return c + (8.0f + labH * 0.5f);
    }
    return std::nullopt;
}

std::vector<double> SpectralView::thirdOctaveCentres (double fMin, double fMax)
{
    // IEC 61260-1 preferred centre frequencies: {1, 1.25, 1.6, 2, 2.5,
    // 3.15, 4, 5, 6.3, 8} x 10^m Hz.
    static const double mant[] = { 1.0, 1.25, 1.6, 2.0, 2.5, 3.15, 4.0, 5.0,
                                   6.3, 8.0 };
    std::vector<double> out;
    for (int p = -1; p <= 6; ++p)
        for (double m : mant)
        {
            const double f = m * std::pow (10.0, p);
            if (f >= fMin - 1e-9 && f <= fMax + 1e-9) out.push_back (f);
        }
    std::sort (out.begin(), out.end());
    return out;
}

void SpectralView::drawThirdOctaveGrid (juce::Graphics& g)
{
    if (!app.settings.showThirdOctaveGrid) return;

    const double rate = app.audioEngine().sampleRate();
    const double fMax = std::min (rate * 0.5, freqMax);
    const double fMin = std::max (1.0, freqMin);
    if (fMax <= fMin) return;

    const auto centres = thirdOctaveCentres (fMin, fMax);
    if (centres.empty()) return;

    // Band borders sit at the geometric mean of neighbouring centre
    // frequencies; the outermost visible bands fall back to the range edges.
    auto border = [] (double a, double b) { return std::sqrt (a * b); };

    const float x0 = axisOnRight() ? 0.0f : (float) axisW();
    const float x1 = axisOnRight() ? (float) (layoutWidth() - axisW())
                                   : (float) layoutWidth();
    // The centre label sits just inside the plot, on the same side as the
    // vertical frequency axis.
    const int labelW = 56;
    const float labelX = axisOnRight() ? x1 - labelW : x0 + 2;

    // A channel label ("L"/"R") sits at each row's centre; keep band labels
    // out of that zone, like the frequency ruler does.
    const bool hasChanLabel = (channels.size() > 1 || app.audioEngine().numChannels() == 2);

    g.setFont (9.0f);
    const float labH = g.getCurrentFont().getHeight();
    static const float dash[] = { 4.0f, 3.0f };

    for (const auto& rr : rowRects())
    {
        if (rr.getHeight() <= 0) continue;
        auto yOf = [&] (double f) { return (float) rr.getY() + freqToY (f, rr.getHeight()); };

        for (size_t i = 0; i < centres.size(); ++i)
        {
            const double fc = centres[i];
            // Nothing is drawn below the 40 Hz band: "40" is the lowest label
            // and its lower border (~35.5 Hz) the lowest grid line.
            if (fc < 40.0) continue;
            const double lo = (i > 0) ? border (centres[i - 1], fc) : fMin;
            const double hi = (i + 1 < centres.size()) ? border (fc, centres[i + 1]) : fMax;

            // On-screen band height in pixels. Fully zoomed out, the bands
            // compress to a few pixels and the dashed borders alias into a
            // dense mess: fade the grid as the height shrinks and stop drawing
            // it below 5 px per band so it recedes instead of cluttering.
            const float h = std::abs (yOf (hi) - yOf (lo));
            if (h < 5.0f) continue;
            const float fade = std::clamp ((h - 5.0f) / (12.0f - 5.0f), 0.0f, 1.0f);

            // Thin delicate white dashed lines at the band borders.
            g.setColour (juce::Colour::fromRGBA (255, 255, 255, (int) std::lround (70.0 * fade)));
            g.drawDashedLine (juce::Line<float> (x0, yOf (lo), x1, yOf (lo)), dash, 2, 1.0f);
            g.drawDashedLine (juce::Line<float> (x0, yOf (hi), x1, yOf (hi)), dash, 2, 1.0f);

            // Centre value between the borders; nudge it clear of the channel
            // label when needed and drop it only if no position fits.
            const float yc = yOf (fc);
            const auto labY = thirdOctaveLabelY (yc, yOf (hi), yOf (lo), rr,
                                                 hasChanLabel, labH);
            if (!labY) continue;

            juce::String lab;
            if (fc >= 1000.0)
            {
                juce::String s = juce::String (fc / 1000.0, 2);
                while (s.endsWith ("0")) s = s.dropLastCharacters (1);
                if (s.endsWith (".")) s = s.dropLastCharacters (1);
                lab = s + "k";
            }
            else
                lab = juce::String ((int) std::lround (fc));

            g.setColour (juce::Colours::white);
            g.drawText (lab, (int) labelX, (int) std::lround (*labY - labH * 0.5f), labelW,
                        (int) std::ceil (labH),
                        axisOnRight() ? juce::Justification::bottomRight
                                      : juce::Justification::bottomLeft,
                        false);
        }
    }
}

// Time selection — blue fill with solid edge lines so it stays clearly
// visible over the spectrogram image. Clipped to the plot so an edge that
// lies before the visible window never paints over the frequency axis.
void SpectralView::drawTimeSelectionOverlay (juce::Graphics& g)
{
    const auto pr = plotRect();
    if (selection.isEmpty()) return;
    const double x1 = timeToX (selection.getStart());
    const double x2 = timeToX (selection.getEnd());
    // Vertical extent: the channel row a drag-selection started on, or all
    // rows when it came from elsewhere (Select All, markers, ...).
    int selTop = pr.getY(), selBot = pr.getBottom();
    {
        const auto rows = rowRects ();   // absolute coordinates
        if (app.selectionRow >= 0 && app.selectionRow < (int) rows.size ())
        {
            const auto& rrSel = rows[(size_t) app.selectionRow];
            selTop = rrSel.getY();
            selBot = rrSel.getBottom();
        }
    }
    const juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (pr);
    g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
    g.fillRect ((float) x1, (float) selTop, (float) std::max (0.0, x2 - x1), (float) (selBot - selTop));
    g.setColour (juce::Colour::fromRGB (96, 150, 255));
    g.drawVerticalLine ((float) x1, (float) selTop, (float) selBot);
    g.drawVerticalLine ((float) x2, (float) selTop, (float) selBot);
}

// Committed spectral selection shapes from the active doc plus (optionally)
// the live preview while a tool drag is in progress. They are evaluated
// TOGETHER so a Ctrl-drag shows its hole in the existing selection
// immediately.
void SpectralView::drawSpectralShapesOverlay (juce::Graphics& g, bool includeLivePreview)
{
    static const std::vector<SpectralShape> emptyShapes;
    auto* d = app.doc();
    const std::vector<SpectralShape>& committed =
        (d && !d->spectralSel.empty ()) ? d->spectralSel.shapes () : emptyShapes;

    SpectralShape live;
    bool haveLive = false;
    if (includeLivePreview)
    {
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
                live.rasteriseStroke (app.brushRadiusPct ());   // grid needed for contains()
            haveLive = true;
        }
    }

    // Same blue family as the time selection; previews are brighter.
    const juce::Colour fillCol = haveLive ? juce::Colour::fromRGBA (70, 130, 255, 96)
                                          : juce::Colour::fromRGBA (70, 130, 255, 80);
    const juce::Colour edgeCol = haveLive ? juce::Colour::fromRGB (140, 180, 255)
                                          : juce::Colour::fromRGB (96, 150, 255);
    drawSpectralShapes (g, committed, haveLive ? &live : nullptr, fillCol, edgeCol);
}

// Markers — clipped to the plot so lines and titles never paint over the
// left frequency axis when a marker starts before the visible window.
// While the waveform is stacked above (showMarkerTitles == false) only the
// vertical delimiter lines are drawn here; the bracket line and titles live
// on the waveform instead. A lone spectrogram draws the full overlay.
void SpectralView::drawMarkersOverlay (juce::Graphics& g)
{
    if (!markerList) return;
    const auto pr = plotRect();
    {
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (pr);
        const auto& ms = *markerList;   // kept sorted by start time
        for (size_t i = 0; i < ms.size(); ++i)
        {
            const Marker& m = ms[i];
            const double x = timeToX (m.start);
            if (x < -50 || x > layoutWidth() + 50)
                continue;
            g.setColour (m.colour.withAlpha (0.8f));
            if (m.end < 0)
            {
                g.drawVerticalLine ((int) x, (float) pr.getY(), (float) pr.getBottom());
            }
            else
            {
                const double x2 = timeToX (m.end);
                if (!showMarkerTitles)
                {
                    // Stacked below the waveform: delimiter lines only.
                    g.drawVerticalLine ((int) x,  (float) pr.getY(), (float) pr.getBottom());
                    g.drawVerticalLine ((int) x2, (float) pr.getY(), (float) pr.getBottom());
                }
                else
                {
                    g.drawLine ((float) x, (float) pr.getY() + 6, (float) x2, (float) pr.getY() + 6, 2.0f);
                    g.drawLine ((float) x, (float) pr.getY() + 6, (float) x, pr.getBottom(), 1.0f);
                    g.drawLine ((float) x2, (float) pr.getY() + 6, (float) x2, pr.getBottom(), 1.0f);
                }
            }
            if (!showMarkerTitles) continue;   // no titles while stacked
            // Title: may run past this marker's own range, but must stop
            // before the next marker starts so that marker's title has room.
            g.setFont (10.0f);
            g.setColour (m.colour);
            const double textX = x + 4;
            const double rightEdge = (i + 1 < ms.size())
                ? timeToX (ms[i + 1].start) - 2.0
                : (double) layoutWidth() + 50.0;
            const double w = std::max (0.0, rightEdge - textX);
            if (w > 0.0)
                g.drawText (m.name, (int) textX, pr.getY() + 8, (int) w, 14,
                            juce::Justification::topLeft, true);
        }
    }
}

void SpectralView::drawOverlays (juce::Graphics& g)
{
    const auto pr = plotRect();

    drawTimeSelectionOverlay (g);
    drawSpectralShapesOverlay (g, true);
    drawMarkersOverlay (g);

    // Paintbrush cursor: a translucent band showing the brush's frequency
    // width at the current cursor position.
    if (app.selMode == SpectralSelMode::Paintbrush && dragMode == DragMode::None
        && probeX >= pr.getX() && probeX <= pr.getRight()
        && probeY >= pr.getY() && probeY <= pr.getBottom())
    {
        const double f = freqAtY (probeY);
        // The brush is a percentage of the cursor's own frequency.
        const double r = std::max (1.0, app.brushRadiusPct () * std::abs (f));
        g.setColour (juce::Colour::fromRGBA (96, 150, 255, 140));
        for (const auto& row : rowRects ())
        {
            const double yTop = row.getY() + freqToY (f + r, row.getHeight());
            const double yBot = row.getY() + freqToY (std::max (0.0, f - r), row.getHeight());
            g.fillRect ((float) probeX - 1.0f, (float) std::min (yTop, yBot), 2.0f,
                        (float) std::max (0.0, std::abs (yBot - yTop)));
        }
    }

    // Harmonic markers (Spectral Controls panel): small white circles at the
    // harmonics of the frequency under the mouse - no lines. Half the size of
    // the orange selection-scaling reference circle (10 px -> 5 px). With
    // "Show harmonic numbers" on, a small white number sits next to each dot.
    if (app.settings.specHarmVisible && app.settings.specHarmMaxK >= 1
        && app.settings.specHarmN >= 1)
    {
        if (harmMouseX >= pr.getX() && harmMouseX < pr.getRight()
            && harmMouseY >= pr.getY() && harmMouseY < pr.getBottom())
        {
            const double fMouse = freqAtY (harmMouseY);
            const int N = std::max (1, app.settings.specHarmN);
            const double fFund = fMouse / (double) N;
            if (fFund > 0.0 && std::isfinite (fFund))
            {
                const int kHi = app.settings.specHarmMaxK;
                g.setColour (juce::Colours::white);
                for (int k = 1; k <= kHi; ++k)
                {
                    const double f = (double) k * fFund;
                    for (const auto& row : rowRects ())
                    {
                        const int y = freqToY (f, row.getHeight());
                        if (y < 0 || y >= row.getHeight()) continue;
                        g.fillEllipse ((float) harmMouseX - 2.5f,
                                      (float) row.getY() + y - 2.5f, 5.0f, 5.0f);
                        if (app.settings.specHarmNumbers)
                        {
                            // Small number next to the dot; flip to the left side
                            // when the cursor is close to the right edge.
                            g.setFont (12.0f);
                            const bool onLeft = harmMouseX + 38 > pr.getRight();
                            if (onLeft)
                                g.drawText (juce::String (k), harmMouseX - 36,
                                            row.getY() + y - 7, 29, 14,
                                            juce::Justification::centredRight);
                            else
                                g.drawText (juce::String (k), harmMouseX + 7,
                                            row.getY() + y - 7, 29, 14,
                                            juce::Justification::centredLeft);
                        }
                    }
                }
            }
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
            for (const auto& row : rowRects ())
            {
                const int y = freqToY (f, row.getHeight());
                if (y < 0 || y >= row.getHeight()) continue;
                g.drawEllipse ((float) x - 5.0f, (float) row.getY() + y - 5.0f,
                              10.0f, 10.0f, 1.5f);
            }
        }
    }

    // Armed scale calibration: the placed reference point, drawn with the
    // same round orange indicator as Clone & Multiply picking. It stays on
    // screen while MainWindow prompts for its value and is cleared after.
    if (app.haveRefClickPt)
    {
        const double t = app.refClickT, f = app.refClickF;
        if (t >= viewStart && t <= viewEnd)
        {
            const int x = timeToX (t);
            g.setColour (juce::Colours::orange);
            for (const auto& row : rowRects ())
            {
                const int y = freqToY (f, row.getHeight());
                if (y < 0 || y >= row.getHeight()) continue;
                g.drawEllipse ((float) x - 5.0f, (float) row.getY() + y - 5.0f,
                              10.0f, 10.0f, 1.5f);
            }
        }
    }

    // Move/Scale selection gesture: the reference point, drawn with the same
    // round orange indicator as Clone & Multiply picking.
    if (app.haveSelGestureRef ())
    {
        const double t = app.selGestureRefT(), f = app.selGestureRefF();
        if (t >= viewStart && t <= viewEnd)
        {
            const int x = timeToX (t);
            g.setColour (juce::Colours::orange);
            for (const auto& row : rowRects ())
            {
                const int y = freqToY (f, row.getHeight());
                if (y < 0 || y >= row.getHeight()) continue;
                g.drawEllipse ((float) x - 5.0f, (float) row.getY() + y - 5.0f,
                              10.0f, 10.0f, 1.5f);
            }
        }
    }

    // Playhead: a plain red line spanning the plot and continuing into this
    // view's own time ruler strip when one is shown.
    const double px = timeToX (playhead);
    if (px >= pr.getX() && px <= pr.getRight())
    {
        g.setColour (juce::Colour::fromRGB (255, 60, 60));
        const float yTop = (showOwnTimeRuler && !rulerAtBottom()) ? 0.0f : (float) pr.getY();
        const float yBot = (showOwnTimeRuler && rulerAtBottom()) ? (float) getLocalBounds().getHeight()
                                                                 : (float) pr.getBottom();
        g.drawVerticalLine ((int) px, yTop, yBot);
    }
}

// ------------------------------------------------- drawings (Drawing panel)

void SpectralView::startPendingDraw (DrawingItem::Kind k)
{
    havePendingDraw_ = true;
    pendingDraw_ = k;
    drawingNew_ = false;
    freePts_.clear ();
    repaint();   // the status-bar hint comes from MainWindow's mode text
}

void SpectralView::cancelPendingDraw()
{
    if (!havePendingDraw_ && !drawingNew_) return;
    havePendingDraw_ = false;
    drawingNew_ = false;
    freePts_.clear ();
    repaint();
}

void SpectralView::estimateTextExtent (int px, int py, const juce::String& text,
                                       float textSizePx, double& outT1,
                                       double& outF1) const
{
    auto rows = rowRects ();
    if (rows.empty()) { outT1 = 0.0; outF1 = 0.0; return; }

    // The channel row containing the anchor pixel position.
    juce::Rectangle<int> rr = rows.front();
    for (const auto& r2 : rows)
        if (r2.contains (px, py)) { rr = r2; break; }

    const double dtPerPx  = std::abs (xToTime (rr.getX () + 1) - xToTime (rr.getX ()));
    const float dfPerPx   = std::abs (yToFreq (1.0f, rr.getHeight ())
                                     - yToFreq (0.0f, rr.getHeight ()));

    // Rough glyph metrics: ~55% of the font size per character, 120% line height.
    const double wPx = (text.isEmpty () ? 4 : text.length ()) * 0.55 * textSizePx;
    const double hPx = textSizePx * 1.2;

    outT1 = xToTime ((double) px + wPx);
    // The text body sits ABOVE its baseline anchor pixel.
    outF1 = yToFreq ((float) std::max (0, py - rr.getY () - (int) hPx),
                     rr.getHeight());
}

// Channel labels on the left of each track: "L"/"R" for stereo files,
// channel numbers otherwise. Shown even when a single channel is visible so
// the axis always says which track it is.
void SpectralView::drawChannelLabels (juce::Graphics& g, bool showMuteState)
{
    const auto rows = rowRects ();
    g.setFont (13.0f);   // the same size the waveform uses: both say which track
    for (size_t r = 0; r < rows.size() && r < channels.size(); ++r)
    {
        const int ch = channels[r];
        const auto& rr = rows[r];
        const bool muted = showMuteState && app.audioEngine().isChannelMuted (ch);
        g.setColour (muted ? juce::Colour::fromRGB (255, 80, 80)
                           : juce::Colour::fromRGB (215, 215, 220));
        // Centred in a slightly wider box: never clipped by a metrics difference.
        g.drawText (channelLabel (ch), channelLabelBounds (ch, rr.getY() + rr.getHeight() / 2).expanded (4, 0),
                    juce::Justification::centred, false);
    }
}

juce::String SpectralView::channelLabel (int ch) const
{
    return app.audioEngine().numChannels() == 2 ? juce::String (ch == 0 ? "L" : "R")
                                                : juce::String (ch + 1);
}

juce::Rectangle<int> SpectralView::channelLabelBounds (int ch, int centreY) const
{
    const juce::Font f (juce::FontOptions (13.0f));   // the label's g.setFont (13.0f)
    const int textW = (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, channelLabel (ch)));
    // At the OUTER edge of the axis area: left of the frequency units when the
    // axis is on the left, right of them when it is on the right. The axis
    // keeps sc::axes::channelGutter() px there for exactly this.
    const int x = axisOnRight() ? layoutWidth() - 2 - textW : 2;
    return { x, centreY - 10, textW, 20 };
}

juce::String SpectralView::getTooltip()
{
    if (app.audioEngine().isFileLoaded() && channelAtLabel (getMouseXYRelative()) >= 0)
        return "Click to mute or unmute this channel; Ctrl+click (Cmd+click on macOS) solos it.";
    return juce::SettableTooltipClient::getTooltip();
}

int SpectralView::channelAtLabel (juce::Point<int> p) const
{
    // Only inside the axis strip: the widened target must never take clicks
    // from the plot next to it.
    if (!app.audioEngine().isFileLoaded() || !onAxisX (p.x)) return -1;
    const auto rows = rowRects ();
    for (size_t r = 0; r < rows.size() && r < channels.size(); ++r)
    {
        const auto& rr = rows[r];
        const auto text = channelLabelBounds (channels[r], rr.getY() + rr.getHeight() / 2);
        // A comfortable target around the small text.
        if (text.withSizeKeepingCentre (std::max (text.getWidth() + 8, 18), 18).contains (p))
            return channels[r];
    }
    return -1;
}

void SpectralView::drawDrawings (juce::Graphics& g, bool includeLivePreview)
{
    auto* d = app.doc ();
    if (d == nullptr && !(drawingNew_ && includeLivePreview)) return;

    // Committed items from the active doc plus the live in-progress preview.
    std::vector<DrawingItem> items;
    if (d != nullptr) items = d->drawings;   // copy: cheap, and paint-safe

    DrawingItem live;
    bool haveLive = false;
    if (drawingNew_ && includeLivePreview)
    {
        const double tEnd = xToTime ((double) drawEndX_);
        const double fEnd = freqAtY (drawEndY_);
        live.kind = pendingDraw_;
        // Preview with the same ink colour / line width the committed item
        // will get, so what you drag is what you place.
        live.colourARGB = app.settings.drawColourARGB;
        live.widthPx    = app.settings.drawWidth;
        if (pendingDraw_ == DrawingItem::Kind::Freehand)
            live.points = freePts_;
        else
        {
            std::pair<double, double> p0 { drawStartT_, drawStartF_ };
            std::pair<double, double> p1 { tEnd, fEnd };
            live.points.push_back (p0);
            live.points.push_back (p1);
        }
        haveLive = true;
    }

    const auto rows = rowRects ();
    if (rows.empty()) return;

    // Draw every item on every visible channel row, like the spectral
    // selection shapes: geometry is in data coordinates so it stays anchored.
    for (const auto& rr : rows)
    {
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (rr);

        auto drawOne = [&] (const DrawingItem& it, bool preview)
        {
            // Every ink pass is preceded by a dark halo so the default bright
            // colours stay readable over any part of the colour map (the classic
            // map's yellow end would otherwise swallow thin green lines).
            const float alphaScale = preview ? 0.6f : 1.0f;
        const juce::Colour ink  = it.colour().withAlpha (alphaScale);
        const juce::Colour halo = juce::Colours::black.withAlpha (0.55f * alphaScale);

            switch (it.kind)
            {
                case DrawingItem::Kind::Text:
                {
                    g.setFont (it.textSize);
                    const float x = timeToX (it.t0);
                    const float y = rr.getY () + freqToY (it.f0, rr.getHeight());
                    const juce::String txt = it.text.isEmpty () ? juce::String ("(text)") : it.text;
                    // Halo: the same text in dark ink at 8 small offsets.
                    const int off = std::max (1, (int) (it.textSize / 12.0f));
                    g.setColour (halo);
                    static const int offs[3] = { -1, 0, 1 };
                    for (int oi = 0; oi < 3; ++oi)
                        for (int oj = 0; oj < 3; ++oj)
                            if (offs[oi] != 0 || offs[oj] != 0)
                                g.drawText (txt, (int) x + offs[oi] * off,
                                            (int) y - (int) it.textSize + offs[oj] * off,
                                            4096, (int) (it.textSize * 2.5f),
                                            juce::Justification::centredLeft);
                    g.setColour (ink);
                    g.drawText (txt, (int) x, (int) y - (int) it.textSize,
                                4096, (int) (it.textSize * 2.5f),
                                juce::Justification::centredLeft);
                    break;
                }
                case DrawingItem::Kind::Arrow:
                {
                    if (it.points.size () < 2) break;
                    // The tip is at the drag start point: you place the arrowhead
                    // first and pull the shaft back, so the arrow points toward
                    // where you began dragging.
                    const float x0 = timeToX (it.points[0].first);
                    const float y0 = rr.getY () + freqToY (it.points[0].second, rr.getHeight());
                    const float x1 = timeToX (it.points[1].first);
                    const float y1 = rr.getY () + freqToY (it.points[1].second, rr.getHeight());
                    // Arrowhead: two short lines at ~26 degrees off the shaft.
                    const double headLen = 8.0 + 3.0 * it.widthPx;
                    const auto barbs = arrowHeadBarbs (x0, y0, x1, y1, headLen);
                    auto drawArrow = [&] (float w)
                    {
                        g.drawLine (x1, y1, x0, y0, w);
                        for (int i = 0; i < 2; ++i)
                            g.drawLine (x0, y0, barbs[i].first, barbs[i].second, w);
                    };
                    g.setColour (halo);  drawArrow (it.widthPx + 3.0f);
                    g.setColour (ink);   drawArrow (it.widthPx);
                    break;
                }
                case DrawingItem::Kind::Rectangle:
                {
                    // Outline only: the inside stays empty.
                    const float x0 = timeToX (it.t0);
                    const float x1 = timeToX (it.t1);
                    const float yTop = rr.getY () + freqToY (it.f1, rr.getHeight());
                    const float yBot = rr.getY () + freqToY (it.f0, rr.getHeight());
                    const juce::Rectangle<float> box (x0, std::min (yTop, yBot),
                                                     std::max (1.0f, x1 - x0),
                                                     std::max (1.0f, std::abs (yBot - yTop)));
                    g.setColour (halo);  g.drawRect (box, it.widthPx + 3.0f);
                    g.setColour (ink);   g.drawRect (box, it.widthPx);
                    break;
                }
                case DrawingItem::Kind::Freehand:
                {
                    juce::Path p;
                    for (size_t i = 0; i < it.points.size (); ++i)
                    {
                        const float x = timeToX (it.points[i].first);
                        const float y = rr.getY () + freqToY (it.points[i].second, rr.getHeight());
                        if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
                    }
                    auto strokeIt = [&] (float w)
                    {
                        juce::PathStrokeType st (w);
                        st.setJointStyle (juce::PathStrokeType::curved);
                        g.strokePath (p, st);
                    };
                    g.setColour (halo);  strokeIt (it.widthPx + 3.0f);
                    g.setColour (ink);   strokeIt (it.widthPx);
                    break;
                }
            }
        };

        // Committed items respect the master switch and each item's own
        // visibility toggle; the live preview always draws so a new item can
        // be placed even while everything else is hidden.
        for (const auto& it : items)
            if (app.showAllDrawings && !it.hidden)
                drawOne (it, false);
        if (haveLive)
            drawOne (live, true);
    }
}

// Tables overlay (Overlay on Spectrogram mode): one curve per visible
// parameter whose alignment target is the spectrogram. Each parameter's raw
// range is fitted into [-1,+1] (visible regardless of units), then mapped
// linearly onto this view's current frequency range and drawn through its own
// frequency axis.
void SpectralView::drawTablesOverlay (juce::Graphics& g)
{
    if (!app.showAllTables || app.tablesMode() != 1) return;
    const auto& tables = app.activeTables();
    if (tables.empty()) return;

    const auto pr = plotRect();
    if (pr.getWidth() <= 1 || pr.getHeight() <= 1) return;

    // One identical curve per visible channel row, mapped into that row's own
    // frequency span - the same layout renderSpectralContent() uses for data.
    const auto rows = rowRects ();

    for (const auto& rr : rows)
    {
        for (const auto& t : tables)
        {
            if (!t.visible) continue;
            for (const auto& p : t.params)
            {
                if (!p.visible || p.alignTarget != 1 || p.values.size() < 2) continue;

                // Empty cells (NaN) never influence the overlay's auto-scaled axis.
                double mn = 0.0, mx = 0.0;
                if (!sc::tables::finiteMinMax (p.values, mn, mx)) continue;
                const auto [off, sc] = t.effectiveTimeMap (p);

                juce::Path path;
                bool started = false;
                for (int i = 0; i < pr.getWidth(); ++i)
                {
                    const double tDoc = xToTime ((double) (pr.getX() + i));
                    const double tTab = (tDoc - off) / sc;
                    if (tTab < 0.0 || tTab > p.duration (t.sampleRate)) { started = false; continue; }
                    const double v = sc::tables::overlayValue (
                        sc::tables::paramValueAt (t, p, tTab), p, mn, mx, 1);
                    if (!std::isfinite ((float) v)) { started = false; continue; }
                    const double fHz = getFreqMin () + 0.5 * (v + 1.0) * (getFreqMax () - getFreqMin ());
                    if (!(fHz >= 0.0)) { started = false; continue; }
                    const float y = rr.getY() + freqToY (fHz, rr.getHeight());
                    const float x = (float) (pr.getX() + i) + 0.5f;
                    if (!started) { path.startNewSubPath (x, y); started = true; }
                    else          path.lineTo (x, y);
                }
                g.setColour (sc::tables::tableColourAt (p.colorIndex,
                                                       app.settings.tableCustomColours));
                g.strokePath (path, juce::PathStrokeType (2.0f));
            }
        }
    }
}

// Hover value labels for the table overlay curves: the raw parameter values
// of curves near the cursor, stacked from the top so they never overlap -
// like the dedicated track's hover readout.
void SpectralView::collectTableHoverLabels (int mouseX, int mouseY)
{
    tblHoverPos = { (float) mouseX, (float) mouseY };
    tblHoverLabels.clear ();

    const auto pr = plotRect();
    if (mouseX < pr.getX () || mouseX >= pr.getRight ()) return;
    const double tDoc = xToTime ((int) mouseX);

    struct Hit { float y; juce::String text; juce::Colour colour; };
    std::vector<Hit> hits;
    for (const auto& t : app.activeTables())
    {
        if (!t.visible) continue;
        for (const auto& p : t.params)
        {
            if (!p.visible || p.alignTarget != 1 || p.values.size() < 2) continue;
            double mn = p.values[0], mx = p.values[0];
            for (double v : p.values) { mn = std::min (mn, v); mx = std::max (mx, v); }
            const auto [off, sc] = t.effectiveTimeMap (p);
            const double tTab = (tDoc - off) / sc;
            if (tTab < 0.0 || tTab > p.duration (t.sampleRate)) continue;
            const double vRaw = sc::tables::paramValueAt (t, p, tTab);
            const double vMap = sc::tables::overlayValue (vRaw, p, mn, mx, 1);
            if (!std::isfinite ((float) vMap)) continue;
            const double fHz = getFreqMin () + 0.5 * (vMap + 1.0) * (getFreqMax () - getFreqMin ());
            // The curve is drawn on every visible channel row: hit-test each.
            for (const auto& rr : rowRects ())
            {
                const float y = rr.getY() + freqToY (fHz, rr.getHeight());
                if (std::abs (y - (float) mouseY) <= 8.0f)
                    hits.push_back ({ y, p.name + "  " + juce::String (vRaw, 4),
                                     sc::tables::tableColourAt (p.colorIndex,
                                                                app.settings.tableCustomColours) });
            }
        }
    }

    std::sort (hits.begin(), hits.end (),
               [] (const Hit& a, const Hit& b) { return a.y < b.y; });
    for (const auto& h : hits)
        tblHoverLabels.push_back ({ h.text, 4.0f, h.colour });   // y fixed in paint()
}

void SpectralView::drawTableHoverLabels (juce::Graphics& g)
{
    if (tblHoverLabels.empty ()) return;
    const auto pr = plotRect();
    g.setFont (12.0f);
    const juce::Font f (12.0f);
    int y = std::max (pr.getY () + 4, 4);
    for (const auto& hl : tblHoverLabels)
    {
        const auto sz = (int) juce::GlyphArrangement::getStringWidth (f, hl.text);
        juce::Rectangle<int> box ((int) tblHoverPos.x + 12, y, sz + 8, 15);
        g.setColour (juce::Colour::fromRGBA (20, 20, 24, 200));
        g.fillRoundedRectangle (box.toFloat (), 3.0f);
        g.setColour (hl.colour);
        g.drawText (hl.text, box, juce::Justification::centredLeft, true);
        y += 17;   // fixed spacing keeps the labels from overlapping
    }
}

// ------------------------------------------------- spectral selection tools

double SpectralView::freqAtY (int y) const
{
    const auto rows = rowRects ();
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
    // Tool drags create a new time selection that spans every channel row.
    app.selectionRow = -1;

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

// SHIFT locks the gesture to the axis with the larger SCREEN-space delta from
// the reference point ("the main direction of the mouse").
Application::AxisLock SpectralView::axisLockForEvent (const juce::MouseEvent& e) const
{
    if (!e.mods.isShiftDown ()) return Application::AxisLock::None;

    const auto rows = rowRects ();
    juce::Rectangle<int> row = plotRect();
    for (const auto& rr : rows)
        if (rr.contains ((int) e.x, (int) e.y)) { row = rr; break; }

    const int refX = (int) std::lround (timeToX (app.selGestureRefT ()));
    const int refY = row.getY() + (int) std::lround (freqToY (app.selGestureRefF (),
                                                             row.getHeight ()));
    return std::abs (e.x - refX) >= std::abs (e.y - refY)
               ? Application::AxisLock::X
               : Application::AxisLock::Y;
}

void SpectralView::updateSelGestureFromEvent (const juce::MouseEvent& e)
{
    app.updateSelGesture (xToTime ((double) e.x), freqAtY (e.y),
                          axisLockForEvent (e));
}

void SpectralView::drawSpectralShapes (juce::Graphics& g,
                                      const std::vector<SpectralShape>& committedIn,
                                      const SpectralShape* live,
                                      juce::Colour fillCol,
                                      juce::Colour edgeCol)
{
    const bool preview = (live != nullptr);
    if (committedIn.empty() && !preview) return;
    const auto rows = rowRects ();
    if (rows.empty()) return;

    // Ordered iteration over committed shapes followed by the live preview.
    const int nShapes = (int) committedIn.size() + (preview ? 1 : 0);
    auto shapeAt = [&] (int i) -> const SpectralShape&
    {
        return i < (int) committedIn.size() ? committedIn[(size_t) i] : *live;
    };

    // The caller supplies the colours (blue for the live selection, amber for
    // highlighted stored selections); previews stay dashed as before.

    // Rasterise the ordered shape list into a per-pixel boolean mask: additive
    // shapes set bits, subtractive ones clear them. This is exactly what
    // SpectralSelection::contains() computes, so the overlay shows real holes
    // where Ctrl-dragged regions were cut out instead of painting more blue on
    // top - what you see is precisely what gets edited / played back.
    // Sized from the drawing layout, not the component bounds: an export
    // canvas can be taller (axes added below a hidden own ruler) and its plot
    // starts at x = 0 without axes.
    const auto pr = plotRect();
    const int W = std::max (layoutWidth(), pr.getRight());
    const int maskH = std::max ({ 1, layoutHeight(), rows.back().getBottom() });
    const size_t totalPx = (size_t) W * (size_t) maskH;
    if (selMask.size() < totalPx) selMask.assign (totalPx, 0);
    std::fill (selMask.begin(), selMask.begin() + totalPx, 0);

    // The mask covers the plot only; a right-placed axis keeps its strip out.
    const int plotX = std::max (0, pr.getX());
    const int plotRight = pr.getRight() - 1;

    for (const auto& row : rows)
    {
        // This JUCE build has no save/restoreGraphicsContext: use the RAII
        // ScopedSaveState + reduceClipRegion instead.
        const juce::Graphics::ScopedSaveState save (g);
        if (!g.reduceClipRegion (row)) continue;   // row outside the repaint clip
        // Only the clipped part can show: rasterise just that (a playhead
        // repaint is a few pixels wide).
        const auto clip = g.getClipBounds();

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

            int x0 = std::max ({ plotX, clip.getX() - 1, (int) std::floor (timeToX (bb.getX())) });
            int x1 = std::min ({ plotRight, clip.getRight(), (int) std::ceil (timeToX (bb.getRight())) });
            if (x1 < x0) continue;

            // Frequency span -> row-local y range (high frequency at the top).
            const double fLo = bb.getY(), fHi = bb.getY() + bb.getHeight();
            int yTop = std::max ({ 0, clip.getY() - row.getY() - 1,
                                   (int) std::floor ((double) freqToY (fHi, rh)) });
            int yBot  = std::min ({ rh - 1, clip.getBottom() - row.getY(),
                                    (int) std::ceil ((double) freqToY (fLo, rh)) });
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
