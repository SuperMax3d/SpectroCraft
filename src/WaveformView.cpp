/**
 * SpectroCraft - Waveform View Implementation
 * Own time window + vertical zoom (independent of the spectral view),
 * left amplitude axis (dBFS) and top time ruler.
 */

#include "WaveformView.h"
#include "Diagnostics.h"
#include <algorithm>
#include <cmath>

// Time-pan speed: content moves this many px per cursor pixel (0.5 = half
// speed, matching the amplitude-axis grab pan).
static constexpr double kPanSpeedFactor = 0.5;

// Waveform drawing thresholds.
//   * Up to kPolylineSamplesPerColumn samples per pixel column the true sample
//     shape is drawn as a connected polyline.
//   * A mipmap block wider than the column it fills (more than
//     kMaxMipOversample columns) would draw a staircase of block-wide steps:
//     the raw samples are read instead so every column gets its real min/max.
//     kMaxRawColumnSamples bounds that read (a coarse mipmap is then accepted).
//   * At most kSampleDotMaxSamples samples in the view every sample also gets a
//     draggable dot (WaveformView::kSampleDotMaxSamples).
static constexpr double kPolylineSamplesPerColumn = 4.0;
static constexpr double kMaxMipOversample = 2.0;
static constexpr int64_t kMaxRawColumnSamples = 4000000;

// Sample dots: radius, grab radius, and the highlighted (hovered / dragged)
// sample's ring.
static constexpr float kDotRadius = 2.6f;
static constexpr float kDotGrabPx = 7.0f;
static constexpr float kDotRingRadius = 5.5f;


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
    if (newVis != chanVis)
    {
        // Row indices of a drag-selection no longer refer to the same
        // channels: fall back to an all-rows rectangle.
        app.selectionRow = -1;
        chanVis = std::move (newVis);
        dirty = true;
        repaint();
    }
}

void WaveformView::resized() { dirty = true; }

// ---------------------------------------------------------------- interaction

// Build and show the right-click context menu (zoom + edit actions).
// Called from a mouse handler, so withMousePosition() lands exactly where clicked.
static void showViewContextMenu (Application& app, int sessionClip, int sessionLane,
                                 const std::function<void(int)>& onAction)
{
    if (!onAction) return;

    juce::PopupMenu m;
    using enum ContextMenuIDs;
    app.addSessionContextItems (m, sessionClip, sessionLane);   // Add Track, Remove File / Track
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

    // The Scale form: one dialog for every axis of this file (name, units,
    // offset, scale, order, visibility). It replaces the old Scale submenu and
    // the Alternative Scale submenu, which could only reach a fixed set of
    // built-in choices.
    m.addSeparator();
    m.addItem (CtxScaleForm, "Scale...", true, false);
    // The file this document came from (a brand-new document has none yet).
    const bool hasPath = app.doc() != nullptr
                         && ! app.doc()->file.getFullPathName().isEmpty();
    m.addItem (CtxOpenFolder, "Open Containing Folder", hasPath, false);

    if (const int sel = m.showMenu (juce::PopupMenu::Options().withMousePosition());
        sel != 0)
        onAction (sel);
}

juce::String WaveformView::channelLabel (int ch) const
{
    return app.channelName (ch);   // per track in a session (L, R, L, R)
}

juce::Rectangle<int> WaveformView::channelLabelBounds (int ch, int centreY) const
{
    const juce::Font f (juce::FontOptions (13.0f));   // the label's g.setFont (13.0f)
    const int textW = (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, channelLabel (ch)));
    // At the OUTER edge of the axis area: left of the unit labels when the
    // axis is on the left, right of them when it is on the right. The axis
    // keeps sc::axes::channelGutter() px there for exactly this.
    const int x = axisOnRight() ? getWidth() - 2 - textW : 2;
    return { x, centreY - 10, textW, 20 };
}

juce::String WaveformView::getTooltip()
{
    if (app.audioEngine().isFileLoaded() && channelAtLabel (getMouseXYRelative()) >= 0)
        return "Click to mute or unmute this channel; Ctrl+click (Cmd+click on macOS) solos it.";
    return juce::SettableTooltipClient::getTooltip();
}

int WaveformView::channelAtLabel (juce::Point<int> p) const
{
    // Only inside the axis strip: the widened target must never take clicks
    // from the plot next to it.
    if (!app.audioEngine().isFileLoaded() || !onAxisX (p.x)) return -1;
    const auto rows = absRowRects();
    int r = 0;
    for (int ch = 0; ch < app.audioEngine().numChannels() && r < (int) rows.size(); ++ch)
    {
        if (ch >= (int) chanVis.size() || !chanVis[(size_t) ch]) continue;
        const auto& rr = rows[(size_t) r++];
        const int centre = (int) ((float) rr.getY() + (float) rr.getHeight() * 0.5f);
        // A comfortable target around the small text.
        const auto text = channelLabelBounds (ch, centre);
        if (text.withSizeKeepingCentre (std::max (text.getWidth() + 8, 18), 18).contains (p))
            return ch;
    }
    return -1;
}

std::vector<juce::Rectangle<int>> WaveformView::absRowRects() const
{
    const auto pr = plotRect();
    int visCount = 0;
    for (int ch = 0; ch < app.audioEngine().numChannels(); ++ch)
        if (ch < (int) chanVis.size() && chanVis[ch]) ++visCount;
    // Full component width so drags starting on the axis strip hit-test too.
    auto rows = channelRowRects (getWidth(), pr.getHeight(), visCount);
    for (auto& r : rows) r.setY (r.getY() + pr.getY());
    return rows;
}

int WaveformView::rowAtY (int y) const
{
    const auto pr = plotRect();
    if (y < pr.getY()) return -1;
    const auto rows = absRowRects ();
    if (rows.empty ()) return -1;
    int row = 0;   // gaps map to the row below, like the cursor probe
    for (size_t r = 0; r < rows.size (); ++r)
        if (y >= rows[(size_t) r].getY()) row = (int) r;
    return row;
}

sc::session::ViewGeometry WaveformView::sessionGeometry() const
{
    sc::session::ViewGeometry g;
    for (int ch = 0; ch < app.audioEngine().numChannels(); ++ch)
        if (ch < (int) chanVis.size() && chanVis[(size_t) ch]) g.rowChannel.push_back (ch);
    g.rows = absRowRects();
    g.plot = plotRect();
    g.timeToX = [this] (double t) { return timeToX (t); };
    g.xToTime = [this] (int x) { return xToTime (x); };
    g.rate = app.audioEngine().sampleRate();
    return g;
}

void WaveformView::paintOverChildren (juce::Graphics& g)
{
    const FileDoc* d = app.doc ();
    if (d == nullptr || !d->session.active ())
        return;
    const auto geo = sessionGeometry ();
    if (app.sessionBordersShown ())
        sc::session::paintBorders (g, d->session, geo);
    sessionMove_.paint (g, d->session, geo);
    sessionTrim_.paint (g, d->session, geo);
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    if (!app.audioEngine().isFileLoaded()) return;

    // Sessions (issue #21): with Move (V) a left drag on a file moves
    // it; a drag from a file's name tag moves it whatever the tool.
    if (e.mods.isLeftButtonDown() && !e.mods.isPopupMenu())
        if (const FileDoc* d = app.doc(); d != nullptr && d->session.active())
        {
            const auto geo = sessionGeometry();
            const bool onTag = app.sessionBordersShown()
                               && sc::session::clipAtNameTag (d->session, geo, e.getPosition()) >= 0;
            if (app.moveTool && sessionTrim_.begin (d->session, geo, e.getPosition()))
            {
                dragMode = DragMode::None;
                repaint();
                return;
            }
            if ((app.moveTool || onTag) && sessionMove_.begin (d->session, geo, e.getPosition()))
            {
                dragMode = DragMode::None;
                repaint();
                return;
            }
        }

    // A click on a channel number mutes/unmutes that channel; Ctrl+click
    // (Cmd+click on macOS, where Ctrl+click is the context menu) solos it.
    if (e.mods.isLeftButtonDown() && !e.mods.isPopupMenu())
        if (const int ch = channelAtLabel (e.getPosition()); ch >= 0)
        {
            dragMode = DragMode::None;
            app.clickChannelMute (ch, e.mods.isCommandDown());
            return;
        }

    /* Armed scale calibration (the Scale form's crosshair): a left click inside
       the plot places the reference point - its time and the amplitude under
       the cursor - and MainWindow prompts for the value that belongs there. Any
       other click cancels the flow. */
    if (app.refClickTarget == Application::RefClickTarget::WaveAmplitude)
    {
        const auto pr = plotRect();
        const bool inPlot = e.x >= pr.getX() && e.x < pr.getRight()
                            && e.y >= pr.getY() && e.y < pr.getBottom();
        if (e.mods.isLeftButtonDown() && ! e.mods.isRightButtonDown()
            && ! e.mods.isMiddleButtonDown() && inPlot)
        {
            if (const int ch = channelAtRowY (e.y); ch >= 0)
            {
                app.refClickT = xToTime ((double) e.x);
                app.refClickF = ampValueAtY (e.y, ch);   // normalized amplitude there
                app.haveRefClickPt = true;
                repaint();
                if (onRefClickPlaced) onRefClickPlaced (app.refClickT, app.refClickF);
                return;
            }
            if (onEditError) onEditError ("Click inside a waveform row to place the reference.");
        }
        app.refClickTarget = Application::RefClickTarget::None;
        app.haveRefClickPt = false;
        repaint();
        return;
    }

    // Tables panel alignment flow: a left click is consumed as the reference
    // point in this view and reported to the panel. The second value is the
    // clicked amplitude in the channel row under the cursor - the same units
    // drawTablesOverlay() maps into, so two-point alignments can solve the
    // vertical mapping.
    if (onAlignClick && e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown()
        && !e.mods.isMiddleButtonDown())
    {
        double val = 0.0;
        const auto pr = plotRect();
        const int chans = app.audioEngine().numChannels();
        int visCount = 0;
        for (int ch = 0; ch < chans; ++ch)
            if (ch < (int) chanVis.size() && chanVis[ch]) ++visCount;
        if (visCount > 0)
        {
            const auto rows = channelRowRects (pr.getWidth(), pr.getHeight(), visCount);
            int row = 0;
            for (int r = 0; r < visCount; ++r)
                if (e.y >= pr.getY() + rows[(size_t) r].getY())
                    row = r;
            const auto& rr = rows[(size_t) std::clamp (row, 0, visCount - 1)];
            const float center = pr.getY() + rr.getY() + rr.getHeight() * 0.5f;
            const float ampPx = rr.getHeight() * 0.45f * (float) vZoom;
            if (ampPx > 1e-6)
                val = vOffset + ((double) center - e.y) / ampPx;
        }
        onAlignClick (xToTime ((double) e.x), val);
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
                            && !e.mods.isMiddleButtonDown() && onAxisX (e.x);
    const bool onTimeRuler = onRulerY (e.y);

    // Any press that is not a sample grab ends the sample-level interaction:
    // drop a highlight left over by a hover, a zoom or a pan.
    if (! sampleDotsVisible() && hoverSampleIdx >= 0)
    {
        hoverSampleIdx = -1;
        hoverSampleCh = -1;
    }

    // Deep zoom (sample dots): a left press on a dot grabs that sample. The
    // drag previews its new amplitude and mouseUp writes it as one edit. A
    // press that misses every dot keeps the normal seek / drag-to-select.
    if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown() && !e.mods.isMiddleButtonDown()
        && !leftOnAxis && !onTimeRuler && sampleDotsVisible())
    {
        const int64_t idx = sampleIndexAtX (e.x);
        const int ch = channelAtRowY (e.y);
        double v = 0.0;
        if (idx >= 0 && ch >= 0
            && std::abs ((double) e.x - (double) sampleDotXFor (idx)) <= (double) kDotGrabPx
            && sampleValue (ch, idx, v))
        {
            dragMode = DragMode::SampleEdit;
            editSampleIdx = idx;
            editSampleCh = ch;
            editPreviewVal = (float) v;
            editOriginalVal = (float) v;
            hoverSampleIdx = idx;
            hoverSampleCh = ch;
            repaint();
            return;
        }
    }

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
        // this view's amplitude window vertically. Both use the same
        // half-speed incremental scheme as the axis strips (see mouseDrag).
        dragMode = DragMode::Pan;
        panLastX = e.x;
        panLastY = e.y;
    }

    // Push the new playhead/selection to both views.
    if (onChange) onChange (viewStart, viewEnd);
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (!app.audioEngine().isFileLoaded()) return;
    reportHoverTime (e, true);
    if (sessionTrim_.active())
    {
        if (const FileDoc* d = app.doc())
            sessionTrim_.drag (d->session, sessionGeometry(), e.getPosition(),
                               !e.mods.isShiftDown(), app.playhead);   // snaps unless Shift
        repaint();
        return;
    }
    if (sessionMove_.active())
    {
        if (const FileDoc* d = app.doc())
            sessionMove_.drag (d->session, sessionGeometry(), e.getPosition(),
                               !e.mods.isShiftDown(), app.playhead);   // snaps unless Shift
        repaint();
        return;
    }

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
            // A middle grab pans x and y together; a ruler drag is time-only.
            if (dragMode == DragMode::Pan) panAmplitudeByCursorY (e.y);
            break;
        }
        case DragMode::VPan:
        {
            // Grab-and-drag: content follows the cursor (drag down -> the
            // waveform moves down). Panned at half speed so a full-scale view
            // does not saturate the +/-2 offset clamp after a few pixels.
            // The delta is measured from the last delivered position and that
            // anchor always advances to the cursor - like the time pan, so
            // reversing direction responds immediately instead of ratcheting
            // in the first drag's direction (deltas eaten by the +/-2 clamp do
            // not accumulate).
            panAmplitudeByCursorY (e.y);
            break;
        }
        case DragMode::SampleEdit:
        {
            // Live preview: the dot (and the polyline through it) follows the
            // cursor; the file is only written on mouseUp, so the whole drag
            // is a single undoable step.
            const double v = ampValueAtY (e.y, editSampleCh);
            const float nv = (float) std::clamp (v, -1.0e6, 1.0e6);
            if (std::abs ((double) nv - (double) editPreviewVal) < 1e-12) break;
            editPreviewVal = nv;
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
                auto zr = juce::Rectangle<int> (std::min (dragStartX, x), std::min (dragStartY, y),
                                                std::abs (x - dragStartX), std::abs (y - dragStartY));
                // The vertical extent is clipped to the channel row the drag
                // started on: a box-zoom never spans more than one channel's
                // data area. Ruler drags are time-only and keep full height.
                const bool startOnRuler = !onAxisX (dragStartX) && onRulerY (dragStartY);
                if (!startOnRuler)
                {
                    const int row = rowAtY (dragStartY);
                    if (row >= 0)
                        zr = zr.getIntersection (absRowRects ()[(size_t) row]);
                }
                zoomRect = zr;
                repaint();
            }
            break;
        }
        default: break;
    }
}

void WaveformView::mouseUp (const juce::MouseEvent&)
{
    if (sessionTrim_.active())
    {
        const auto t = sessionTrim_.end();
        repaint();
        if (t.clip >= 1 && t.valid)
            app.trimSessionFile (t.clip, t.offset, t.length);
        return;
    }
    if (sessionMove_.active())
    {
        const auto t = sessionMove_.end();
        repaint();
        if (t.clip >= 0 && t.valid)
            app.moveSessionFile (t.clip, t.lane, t.offset);
        return;
    }
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
            {
                int sClip = -1, sLane = -1;
                if (const FileDoc* d = app.doc(); d != nullptr && d->session.active())
                {
                    const auto geo = sessionGeometry();
                    sClip = sc::session::clipAtPoint (d->session, geo, { dragStartX, dragStartY });
                    sLane = sc::session::laneAtY (d->session, geo, dragStartY);
                }
                showViewContextMenu (app, sClip, sLane, [this, onAxis, sClip, sLane] (int id)
                {
                    if (id == CtxZoomOutFull && onAxis) { resetVerticalZoom(); return; }
                    if (app.runSessionContextItem (id, sClip, sLane)) return;
                    if (onContextAction) onContextAction (id);
                });
            }
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
                // Scale the amplitude axis so the dragged span fills the
                // channel row it started in, keeping the value under the box
                // centre at that row's centre line. The span is measured
                // against one row's data height (not the whole plot) so a
                // multi-channel file zooms by the same factor as a single
                // channel would.
                const auto pr = plotRect();
                int visCount = 0;
                for (int ch = 0; ch < app.audioEngine().numChannels(); ++ch)
                    if (ch < (int) chanVis.size() && chanVis[ch]) ++visCount;
                if (visCount > 0 && zoomRect.getHeight() >= 2)
                {
                    const double oldAmpPx = rowAmpPx();
                    const auto rows = channelRowRects (pr.getWidth(), pr.getHeight(), visCount);
                    int rowIdx = 0;   // the drag start's row (gaps map below)
                    for (int r = 0; r < visCount; ++r)
                        if (dragStartY >= pr.getY() + rows[(size_t) r].getY())
                            rowIdx = r;
                    const auto& rrR = rows[(size_t) rowIdx];
                    const float centerR = (float) (pr.getY() + rrR.getY()
                                                   + rrR.getHeight() * 0.5f);

                    vOffset = std::clamp (vOffset + (centerR - zoomRect.getCentreY()) / oldAmpPx,
                                         -2.0, 2.0);
                    const double nz = std::clamp (vZoom * ((double) rrR.getHeight()
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
    else if (dragMode == DragMode::SampleEdit)
    {
        const int64_t idx = editSampleIdx;
        const int ch = editSampleCh;
        const float v = editPreviewVal;
        editSampleIdx = -1;
        editSampleCh = -1;
        if (idx >= 0 && ch >= 0)
        {
            if (std::abs ((double) v - (double) editOriginalVal) < 1e-9)
            {
                // Nothing was moved: the press was a click, so it seeks like
                // any other click instead of doing nothing at all.
                const double t = (double) idx / app.audioEngine().sampleRate ();
                app.setPlayhead (t);
                setPlayhead (t);
            }
            else
            {
                std::string err;
                if (!app.setSampleValue (ch, idx, v, &err) && ! err.empty () && onEditError)
                    onEditError (juce::String (err));
                if (app.onStateChange) app.onStateChange ();   // mipmaps changed
            }
            hoverSampleIdx = idx;   // stay highlighted after the drop
            hoverSampleCh = ch;
            repaint();
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

// Status-bar probe: time + amplitude under the cursor.
void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    reportHoverTime (e, false);
    // Move (V): an edge of a file trims it - say so with the cursor.
    {
        const FileDoc* d = app.doc();
        const bool onEdge = app.moveTool && d != nullptr && d->session.active()
                            && sc::session::TrimGesture::edgeAt (d->session, sessionGeometry(), e.getPosition()) >= 1;
        setMouseCursor (onEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }
    updateHoverSample (e.x, e.y);

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

    const juce::String lab = app.channelName (ch);

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

void WaveformView::reportHoverTime (const juce::MouseEvent& e, bool dragging)
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

void WaveformView::mouseExit (const juce::MouseEvent&)
{
    if (onHoverTime) onHoverTime (-1.0);
    if (onCursorInfo) onCursorInfo ("");
    if (dragMode != DragMode::SampleEdit)
    {
        hoverSampleIdx = -1;
        hoverSampleCh = -1;
    }
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
    // Two quick clicks on a channel number are two mute toggles, not a zoom.
    if (channelAtLabel (e.getPosition()) >= 0)
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

void WaveformView::panAmplitudeByCursorY (int y)
{
    // Half-speed grab: content follows the cursor at 0.5 px per cursor pixel.
    // The delta is measured from the last delivered position and that anchor
    // always advances to the cursor, so reversing direction responds
    // immediately instead of unwinding the deltas an edge clamp swallowed.
    const double ampPx = rowAmpPx();
    if (ampPx > 1e-9)
        vOffset = std::clamp (vOffset + kPanSpeedFactor * (y - panLastY) / ampPx, -2.0, 2.0);
    panLastY = y;
    dirty = true;
    if (onVerticalZoomChanged) onVerticalZoomChanged();
    repaint();
}

// ------------------------------------------------------------------- painting

void WaveformView::paint (juce::Graphics& g)
{
    sc::diag::ScopedPaint diagPaint ("waveform", getWidth(), getHeight());   // paint.slow above 50 ms
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

    // The amplitude grid goes down BEFORE the waveforms: the signal covers it
    // instead of the grid hiding a quiet signal on the zero line.
    drawAmplitudeGrid (g, rows, {});

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
        // The row's centre line is drawn at amplitude zero: it follows the
        // vertical pan/zoom with the data instead of staying fixed at the
        // middle of the row.
        g.setColour (juce::Colour::fromRGB (60, 60, 60));
        const float zeroY = center + (float) (vOffset * amp);
        if (zeroY >= pr.getY() && zeroY <= pr.getBottom())
            g.drawHorizontalLine ((int) zeroY, (float) pr.getX(), (float) pr.getRight());
        paintChannel (g, ch, chanY, chanH, center, amp);

        // A muted channel (click on its number) is drawn darker.
        const bool muted = app.audioEngine().isChannelMuted (ch);
        if (muted)
        {
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRect (pr.getX(), chanY, pr.getWidth(), chanH);
        }

        // Channel label on the left of every track: "L"/"R" for stereo files,
        // channel numbers otherwise. Shown even when a single channel is
        // visible so the axis always says which track it is; red when muted.
        g.setFont (13.0f);
        g.setColour (muted ? juce::Colour::fromRGB (255, 80, 80) : juce::Colour::fromRGB (215, 215, 220));
        // Centred in a slightly wider box: never clipped by a metrics difference.
        g.drawText (channelLabel (ch), channelLabelBounds (ch, (int) center).expanded (4, 0),
                    juce::Justification::centred, false);
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
        // Vertical extent: the channel row a drag-selection started on, or
        // all rows when it came from elsewhere (Select All, markers, ...).
        int selTop = pr.getY(), selBot = pr.getBottom();
        if (app.selectionRow >= 0 && app.selectionRow < (int) rows.size ())
        {
            const auto& rrSel = rows[(size_t) app.selectionRow];
            selTop = pr.getY() + rrSel.getY();
            selBot = selTop + rrSel.getHeight();
        }
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (pr);
        g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
        g.fillRect ((float) x1, (float) selTop,
                   (float) std::max (0.0, x2 - x1), (float) (selBot - selTop));
        g.setColour (juce::Colour::fromRGB (96, 150, 255));
        g.drawVerticalLine ((float) x1, (float) selTop, (float) selBot);
        g.drawVerticalLine ((float) x2, (float) selTop, (float) selBot);
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
            // The range bracket always sits at the top of the plot - just
            // below the top edge - with the title underneath it, exactly
            // like the spectral view's overlay. (Anchoring it to a bottom
            // ruler instead made the title straddle the bracket line.)
            const float brY = pr.getY() + 6.0f;
            g.setColour (m.colour.withAlpha (0.8f));
            if (m.end < 0)
            {
                g.drawVerticalLine ((int) x, (float) pr.getY(), (float) pr.getBottom());
            }
            else
            {
                const double x2 = timeToX (m.end);
                g.drawLine ((float) x, brY, (float) x2, brY, 2.0f);
                g.drawLine ((float) x, brY, (float) x, pr.getBottom(), 1.0f);
                g.drawLine ((float) x2, brY, (float) x2, pr.getBottom(), 1.0f);
            }
            // Title: may run past this marker's own range, but must stop
            // before the next marker starts so that marker's title has room.
            // Suppressed while the spectrogram is stacked below (its copy of
            // the titles is shown there instead); the lines stay in both.
            if (showMarkerTitles)
            {
                g.setFont (10.0f);
                g.setColour (m.colour);
                const double textX = x + 4;
                const double rightEdge = (i + 1 < ms.size())
                    ? timeToX (ms[i + 1].start) - 2.0
                    : (double) getWidth() + 50.0;
                const double w = std::max (0.0, rightEdge - textX);
                if (w > 0.0)
                    g.drawText (m.name, (int) textX, pr.getY() + 8, (int) w, 14,
                                juce::Justification::topLeft, true);
            }
        }
    }

    // Tables overlay (Overlay on Waveform mode): parameter curves mapped to
    // amplitude, drawn above the waveform but below the playhead line.
    drawTablesOverlay (g);

    // Playhead: a plain red line spanning the whole view height - it
    // continues into this view's own time ruler strip when one is shown.
    const double px = timeToX (playhead);
    if (px >= pr.getX() && px <= pr.getRight())
    {
        const int pxI = (int) std::lround (px);
        g.setColour (juce::Colour::fromRGB (255, 60, 60));
        g.drawVerticalLine (pxI, 0.0f, (float) bounds.getHeight());
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

// Tables overlay (Overlay on Waveform mode): one curve per visible parameter
// whose alignment target is the waveform. Each parameter's raw range is first
// fitted into [-1,+1] (so it is visible regardless of the data's units) and
// then inverted/absolute/valueScale/valueOffset are applied; the result lands
// in amplitude units mapped with the same centre/zoom as a channel row.
void WaveformView::drawTablesOverlay (juce::Graphics& g)
{
    if (app.tablesMode() != sc::tables::kModeWaveform) return;
    const auto& tables = app.activeTables();
    if (tables.empty()) return;

    const auto pr = plotRect();
    if (pr.getWidth() <= 1 || pr.getHeight() <= 1) return;

    // One identical curve per visible channel row, scaled to that row's own
    // centre and height - the same layout paintChannel() uses for the data.
    int visCount = 0;
    const int chans = app.audioEngine().numChannels();
    for (int ch = 0; ch < chans; ++ch)
        if (ch < (int) chanVis.size() && chanVis[ch]) ++visCount;
    const auto rows = channelRowRects (pr.getWidth(), pr.getHeight(), visCount);

    for (const auto& rr : rows)
    {
        const float center = pr.getY() + rr.getY() + rr.getHeight() * 0.5f;
        const float ampPx = rr.getHeight() * 0.45f * (float) vZoom;

        for (const auto& t : tables)
        {
            if (!t.visible) continue;
            for (const auto& p : t.params)
            {
                if (!p.visible || p.alignTarget != 0 || p.values.size() < 2) continue;

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
                        sc::tables::paramValueAt (t, p, tTab), p, mn, mx, 0);
                    // Empty cells break the curve: not drawn, never treated as zero.
                    if (!std::isfinite (v)) { started = false; continue; }
                    const float y = center + (float) ((vOffset - v) * ampPx);
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

    const int64_t n = s1 - s0;
    const double samplesPerColumn = (double) n / (double) w;

    // Pick the finest mipmap level that still covers >= samples per column
    const AudioEngine::MipLevel* best = nullptr;
    for (const auto& lv : app.audioEngine().mipmaps())
        if ((double) lv.factor >= samplesPerColumn) { best = &lv; break; }

    // A block coarser than the column it is drawn into is a lie about the shape:
    // every column in the block repeats the same min/max, which reads as a
    // staircase of block-wide steps. That happens as soon as the view is finer
    // than the finest level can describe (samplesPerColumn < baseFactor), so the
    // raw samples are read whenever the chosen block is more than
    // kMaxMipOversample columns wide - the visible range is short there, so the
    // read is cheap and the outline is exact per column.
    const RenderMode mode = chooseRenderMode (n, w, best != nullptr ? best->factor : -1);

    if (mode != RenderMode::Mipmap)
    {
        std::vector<float> buf ((size_t) n);
        if (!app.audioEngine().readSamples (ch, s0, n, buf.data()))
            return;

        // Live preview of a sample being dragged (see DragMode::SampleEdit):
        // the polyline and the dots show the new value before it is written.
        if (editSampleIdx >= s0 && editSampleIdx < s1 && ch == editSampleCh)
            buf[(size_t) (editSampleIdx - s0)] = editPreviewVal;

        // Zoomed in to roughly one-to-few samples per pixel column: draw the
        // individual samples as a connected polyline so the true sample shape is
        // visible instead of blocky min/max columns (classic DAW sample view).
        if (mode == RenderMode::Polyline)
        {
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

            if (n <= kSampleDotMaxSamples)
                paintSampleDots (g, ch, chanY, chanH, center, amp, buf, s0, s1);
            return;
        }

        // Exact min/max per column straight from the file.
        std::vector<float> tops ((size_t) w, center), bots ((size_t) w, center);
        for (int x = 0; x < w; ++x)
        {
            const int64_t a = (int64_t) ((double) x / w * n);
            const int64_t b = std::max (a + 1, (int64_t) ((double) (x + 1) / w * n));
            float mn = buf[(size_t) a], mx = buf[(size_t) a];
            for (int64_t i = a; i < b && i < n; ++i)
            {
                mn = std::min (mn, buf[(size_t) i]);
                mx = std::max (mx, buf[(size_t) i]);
            }
            tops[x] = center + (vOffset - mx) * amp;
            bots[x] = center + (vOffset - mn) * amp;
        }

        // Envelope x-coordinates are relative to the plot area: shift by kAxisW.
        g.saveState();
        g.addTransform (juce::AffineTransform::translation ((float) pr.getX(), 0.0f));
        fillEnvelope (g, tops, bots, rowTop, rowBottom);
        g.restoreState();
        return;
    }

    // Initialise to centre so out-of-range columns render as a flat line
    std::vector<float> tops ((size_t) w, center), bots ((size_t) w, center);

    if (ch < (int) best->min.size() && !best->min[ch].empty())
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

    // Envelope x-coordinates are relative to the plot area: shift by kAxisW.
    g.saveState();
    g.addTransform (juce::AffineTransform::translation ((float) pr.getX(), 0.0f));
    fillEnvelope (g, tops, bots, rowTop, rowBottom);
    g.restoreState();
}

// --------------------------------------------------------------------------- sample dots

WaveformView::RenderMode WaveformView::chooseRenderMode (int64_t n, int plotWidth,
                                                          int64_t finestFactor)
{
    if (n <= 0 || plotWidth <= 0) return RenderMode::Mipmap;
    const double samplesPerColumn = (double) n / (double) plotWidth;
    if (samplesPerColumn <= kPolylineSamplesPerColumn) return RenderMode::Polyline;
    const bool coarse = finestFactor < 0
        || (double) finestFactor > samplesPerColumn * kMaxMipOversample;
    if (coarse && n <= kMaxRawColumnSamples) return RenderMode::ExactColumns;
    return RenderMode::Mipmap;
}

bool WaveformView::sampleDotsVisible() const
{
    if (!app.audioEngine().isFileLoaded() || getWidth() <= 0)
        return false;
    const auto pr = plotRect();
    const double rate = app.audioEngine().sampleRate();
    const int64_t s0 = std::max ((int64_t) 0, (int64_t) (viewStart * rate));
    const int64_t s1 = std::min (app.audioEngine().numSamples(), (int64_t) (viewEnd * rate));
    const int64_t n = s1 - s0;
    return n > 0 && n <= kSampleDotMaxSamples
        && (double) n / (double) std::max (1, pr.getWidth()) <= kPolylineSamplesPerColumn;
}

int64_t WaveformView::sampleIndexAtX (int x) const
{
    if (!sampleDotsVisible()) return -1;
    const auto pr = plotRect();
    const int w = std::max (1, pr.getWidth());
    if (x < pr.getX() || x >= pr.getRight()) return -1;
    const double rate = app.audioEngine().sampleRate();
    const int64_t s0 = std::max ((int64_t) 0, (int64_t) (viewStart * rate));
    const int64_t s1 = std::min (app.audioEngine().numSamples(), (int64_t) (viewEnd * rate));
    const int64_t n = s1 - s0;
    if (n <= 0) return -1;
    const double fx = ((double) x - (double) pr.getX() + 0.5) / (double) w;   // 0..1 across the plot
    const int64_t i = (int64_t) std::llround (fx * (double) n - 0.5);
    if (i < 0 || i >= n) return -1;
    return s0 + i;
}

float WaveformView::sampleDotXFor (int64_t idx) const
{
    if (!app.audioEngine().isFileLoaded()) return -1.0f;
    const auto pr = plotRect();
    const int w = std::max (1, pr.getWidth());
    const double rate = app.audioEngine().sampleRate();
    const int64_t s0 = std::max ((int64_t) 0, (int64_t) (viewStart * rate));
    const int64_t s1 = std::min (app.audioEngine().numSamples(), (int64_t) (viewEnd * rate));
    const int64_t n = s1 - s0;
    if (n <= 0 || idx < s0 || idx >= s1) return -1.0f;
    return (float) pr.getX() + ((float) (idx - s0) + 0.5f) / (float) n * (float) w;
}

int WaveformView::channelAtRowY (int y) const
{
    const auto pr = plotRect();
    if (y < pr.getY() || y >= pr.getBottom()) return -1;
    const int chans = app.audioEngine().numChannels();
    int visCount = 0;
    for (int ch = 0; ch < chans; ++ch)
        if (ch < (int) chanVis.size() && chanVis[ch]) ++visCount;
    if (visCount <= 0) return -1;
    const auto rows = channelRowRects (pr.getWidth(), pr.getHeight(), visCount);
    int row = 0;
    for (int r = 0; r < visCount; ++r)
        if (y >= pr.getY() + rows[(size_t) r].getY())
            row = r;
    int ch = -1, r = 0;
    for (int c = 0; c < chans && ch < 0; ++c)
        if (c < (int) chanVis.size() && chanVis[c])
        {
            if (r == row) ch = c;
            ++r;
        }
    return ch;
}

double WaveformView::ampValueAtY (int y, int ch) const
{
    const auto pr = plotRect();
    const int chans = app.audioEngine().numChannels();
    if (ch < 0 || ch >= chans) return 0.0;
    int visCount = 0, rowOfCh = -1;
    for (int c = 0; c < chans; ++c)
        if (c < (int) chanVis.size() && chanVis[c])
        {
            if (c == ch) rowOfCh = visCount;
            ++visCount;
        }
    if (rowOfCh < 0 || visCount <= 0) return 0.0;
    const auto rows = channelRowRects (pr.getWidth(), pr.getHeight(), visCount);
    const auto& rr = rows[(size_t) rowOfCh];
    const float center = pr.getY() + rr.getY() + rr.getHeight() * 0.5f;
    const float ampPx = rr.getHeight() * 0.45f * (float) vZoom;
    if (ampPx <= 1e-6f) return 0.0;
    return vOffset + ((double) center - (double) y) / (double) ampPx;
}

bool WaveformView::sampleValue (int ch, int64_t idx, double& out) const
{
    if (ch < 0 || ch >= app.audioEngine().numChannels()) return false;
    if (idx < 0 || idx >= app.audioEngine().numSamples()) return false;
    if (idx == editSampleIdx && ch == editSampleCh) { out = editPreviewVal; return true; }
    float v = 0.0f;
    if (!app.audioEngine().readSamples (ch, idx, 1, &v)) return false;
    out = v;
    return true;
}

void WaveformView::paintSampleDots (juce::Graphics& g, int ch, int chanY, int chanH,
                                    float center, float amp, const std::vector<float>& buf,
                                    int64_t s0, int64_t s1)
{
    const auto pr = plotRect();
    const int w = std::max (1, pr.getWidth());
    const int64_t n = s1 - s0;
    if (n <= 0 || n > kSampleDotMaxSamples) return;

    const float rowTop = (float) chanY, rowBottom = (float) (chanY + chanH);
    const juce::Colour col = juce::Colour::fromRGB (0, 212, 170);   // the curve colour
    g.setColour (col);

    auto dotY = [&] (int64_t i) -> float
    {
        return std::clamp ((float) (center + (vOffset - buf[(size_t) i]) * amp), rowTop, rowBottom);
    };
    auto dotX = [&] (int64_t i) -> float { return sampleDotXFor (s0 + i); };

    auto circle = [] (juce::Point<float> c, float r) -> juce::Rectangle<float>
    {
        return { c.x - r, c.y - r, r + r, r + r };
    };

    // One filled dot per sample, in the curve colour.
    for (int64_t i = 0; i < n; ++i)
        g.fillEllipse (circle ({ dotX (i), dotY (i) }, kDotRadius));

    // The sample under the cursor (or being dragged): a slightly larger hollow
    // ring so the file's own value stays visible inside it.
    const int64_t hi = (editSampleIdx >= s0 && editSampleIdx < s1 && ch == editSampleCh)
                     ? editSampleIdx
                     : ((hoverSampleIdx >= s0 && hoverSampleIdx < s1 && ch == hoverSampleCh)
                            ? hoverSampleIdx : -1);
    if (hi < 0) return;

    const juce::Point<float> c { dotX (hi - s0), dotY (hi - s0) };
    g.setColour (col);
    g.fillEllipse (circle (c, kDotRingRadius + 1.5f));
    g.setColour (juce::Colour::fromRGB (18, 22, 24));   // plot background: hollow inside
    g.fillEllipse (circle (c, kDotRingRadius - 1.0f));

    // Value of the highlighted sample, in the same green as the curve.
    double v = buf[(size_t) (hi - s0)];
    if (hi == editSampleIdx && ch == editSampleCh) v = editPreviewVal;
    juce::String txt;
    switch (app.waveVScaleMode)
    {
        case 1:
            txt = (std::abs (v) > 1e-9)
                    ? juce::String (20.0 * std::log10 (std::abs (v)), 2) + " dB"
                    : "-inf dB";
            break;
        case 2:
            txt = juce::String (v, 4);
            break;
        default:
            txt = ScaleCalibration::formatLinearValue (v, app.scale(), ch);
            break;
    }

    const juce::Font f { juce::FontOptions (12.0f) };
    const int px = juce::GlyphArrangement::getStringWidthInt (f, txt) + 8;
    float tx = c.x + kDotRingRadius + 6.0f;
    if (tx + (float) px > (float) pr.getRight()) tx = c.x - kDotRingRadius - 6.0f - (float) px;
    const float ty = std::clamp (c.y - 22.0f, rowTop, rowBottom - 16.0f);
    g.setColour (col);
    g.setFont (f);
    g.drawText (txt, juce::Rectangle<float> (tx, ty, (float) px, 15.0f),
                juce::Justification::centredLeft, false);
}

void WaveformView::updateHoverSample (int x, int y)
{
    const int64_t idx = sampleIndexAtX (x);
    const int ch = idx < 0 ? -1 : channelAtRowY (y);
    if (idx == hoverSampleIdx && ch == hoverSampleCh) return;
    hoverSampleIdx = idx;
    hoverSampleCh = ch;
    repaint();
}

// The dB levels the amplitude axis is built on: the grid lines, the labels and
// any extra amplitude axis all use the very same positions.
static const double kAmpTickLevels[] = { 0.0, -6.0, -12.0, -18.0, -24.0 };

// Grid lines only, drawn BEFORE the waveforms so the signal covers them. Same
// colour and weight as the dedicated tables track's grid.
void WaveformView::drawAmplitudeGrid (juce::Graphics& g,
                                      const std::vector<juce::Rectangle<int>>& rows,
                                      const std::vector<int>& rowChans)
{
    const auto pr = plotRect();
    if (pr.getHeight() <= 0 || rows.empty() || ! app.settings.showAmpGrid) return;

    g.setColour (sc::axes::axisGridColour());
    for (size_t r = 0; r < rows.size(); ++r)
    {
        const auto& rr = rows[r];
        const float center = pr.getY() + rr.getY() + rr.getHeight() * 0.5f;
        const float ampScale = rr.getHeight() * 0.45f * (float) vZoom;

        for (double dB : kAmpTickLevels)
        {
            const double a = std::pow (10.0, dB / 20.0);   // amplitude at this level
            for (int sign : { -1, +1 })
            {
                // Value +a sits above the centre line, value -a below it;
                // both are shifted by the vertical offset.
                const float y = center + (float) ((vOffset - sign * a) * ampScale);
                if (y < pr.getY() || y > pr.getBottom()) continue;
                g.drawHorizontalLine ((int) y, (float) pr.getX(), (float) pr.getRight());
            }
        }

        // The zero line (-inf dB): amplitude 0 sits at centre + vOffset, so it
        // follows the vertical pan unlike the fixed row-centre line.
        const float yZero = center + (float) (vOffset * ampScale);
        if (yZero >= pr.getY() && yZero <= pr.getBottom())
            g.drawHorizontalLine ((int) yZero, (float) pr.getX(), (float) pr.getRight());
    }
}

void WaveformView::drawAmplitudeAxis (juce::Graphics& g, const std::vector<juce::Rectangle<int>>& rows,
                                      const std::vector<int>& rowChans)
{
    const auto pr = plotRect();
    if (pr.getHeight() <= 0 || rows.empty()) return;

    const ScaleCalibration& sc = app.scale();
    const int axisW = this->axisW();

    // Label placement follows the axis side: right-aligned against the plot
    // edge on a left axis, left-aligned on a right one. Each axis keeps to
    // its own strip - the extra axes used to be drawn on top of these labels.
    const auto ampExtras = sc::axes::extras (app.axisEntries, sc::axes::Kind::Amplitude);
    const int nAmp = 1 + (int) ampExtras.size();
    const auto mainLab = sc::axes::verticalLabelRect (0, nAmp, axisW, pr.getX(), pr.getRight(),
                                                      pr.getY(), pr.getBottom(), axisOnRight(),
                                                      sc::axes::channelGutter());
    const int labX = mainLab.getX();
    const int labW = mainLab.getWidth();
    const juce::Justification labJ = axisOnRight() ? juce::Justification::centredLeft
                                                   : juce::Justification::centredRight;

    struct Tick { float y; double amp; int ch; };
    std::vector<Tick> ticks;    // main-axis positions, reused by the extra axes

    for (size_t r = 0; r < rows.size(); ++r)
    {
        const auto& rr = rows[r];
        const float center = pr.getY() + rr.getY() + rr.getHeight() * 0.5f;
        const float ampScale = rr.getHeight() * 0.45f * (float) vZoom;
        const int rowCh = (r < rowChans.size()) ? rowChans[r] : -1;

        // Labels only when rows are tall enough to fit them. The scale is
        // symmetric: the same dB values appear above AND below the centre line.
        if (rr.getHeight() >= 36 && rows.size() <= 4)
        {
            g.setFont (sc::axes::axisLabelFont());
            for (double dB : kAmpTickLevels)
            {
                const double a = std::pow (10.0, dB / 20.0);

                // Upper side label (value +a above the centre line); keep it
                // clear of the channel label that sits at the row centre.
                const float yUp = center + (float) ((vOffset - a) * ampScale);
                if (yUp >= pr.getY() + 8 && yUp <= pr.getBottom()
                    && std::abs (yUp - center) >= 9.0f)
                {
                    ticks.push_back ({ yUp, +a, rowCh });
                    g.setColour (sc::axes::axisLabelColour());
                    g.drawText (formatWaveVScaleLabel (dB, app.waveVScaleMode, sc, true, rowCh),
                               labX, (int) yUp - 7, labW, 14, labJ, false);
                }

                // Lower side label (value -a below the centre line)
                const float yDown = center + (float) ((vOffset + a) * ampScale);
                if (yDown >= pr.getY() && yDown <= pr.getBottom() - 8
                    && std::abs (yDown - center) >= 9.0f)
                {
                    ticks.push_back ({ yDown, -a, rowCh });
                    g.setColour (sc::axes::axisLabelColour());
                    g.drawText (formatWaveVScaleLabel (dB, app.waveVScaleMode, sc, false, rowCh),
                               labX, (int) yDown - 7, labW, 14, labJ, false);
                }
            }

            // Zero-line label. It hugs the plot edge like the other dB tick
            // labels (labJ); the channel name sits on the outer edge instead,
            // so at zero vertical offset nothing overlaps.
            const float yZero = center + (float) (vOffset * ampScale);
            if (yZero >= pr.getY() && yZero <= pr.getBottom())
            {
                ticks.push_back ({ yZero, 0.0, rowCh });
                g.setColour (sc::axes::axisLabelColour());
                g.drawText (zeroLineLabel (app.waveVScaleMode), labX, (int) yZero - 7,
                           labW, 14, labJ, false);
            }
        }
    }

    // Axis title
    g.setFont (sc::axes::axisMinorLabelFont());
    g.setColour (sc::axes::axisMinorLabelColour());
    g.drawText (waveVScaleTitle (app.waveVScaleMode, sc), labX, pr.getY() + 2,
               axisW - 4, 12,
               axisOnRight() ? juce::Justification::centredRight
                             : juce::Justification::centredLeft, false);

    // Extra amplitude axes: the same tick positions, labelled in each entry's
    // own units, in the strips further out (the shared width already has room).
    if (ticks.empty()) return;
    int index = 1;
    for (const auto& e : ampExtras)
    {
        const auto strip = sc::axes::verticalStrip (index, nAmp, axisW, pr.getX(), pr.getRight(),
                                                    pr.getY(), pr.getBottom(), axisOnRight(),
                                                    sc::axes::channelGutter());
        if (strip.getX() < 0 || strip.getRight() > getWidth()) break;

        // Secondary axis: the same smaller type the extra time axes use.
        g.setFont (juce::Font { juce::FontOptions (8.5f) });
        g.setColour (sc::axes::axisMinorLabelColour());
        const auto lab = sc::axes::verticalLabelRect (index, nAmp, axisW, pr.getX(), pr.getRight(),
                                                      pr.getY(), pr.getBottom(), axisOnRight(),
                                                      sc::axes::channelGutter());
        for (const auto& t : ticks)
        {
            const auto box = juce::Rectangle<int> (lab.getX(), (int) t.y - 7, lab.getWidth(), 14);
            g.drawText (sc::axes::formatAmplitude (e, t.amp, t.ch), box,
                        axisOnRight() ? juce::Justification::centredLeft
                                      : juce::Justification::centredRight, true);
        }
        ++index;
    }
}

juce::String WaveformView::zeroLineLabel (int waveVScaleMode)
{
    switch (waveVScaleMode)
    {
        case 1: return "-inf";   // dB
        case 2: return "0.000";  // Normalized
        default: return "0";     // Wav Units / sample mode
    }
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
    // Pixels per DISPLAYED unit (seconds, or samples in Sample mode): the
    // axis' scale multiplies what one second of file time contributes.
    const double pxPerUnit = pxPerSec * sc;
    const double major = (mode == TimeAxisMode::Sample)
                         ? niceSampleStep ((rate / pxPerSec) * sc)
                         : niceTimeStep (pxPerUnit);
    // Subdivide each major interval into five when there is room for it.
    // (In Sample mode `major` counts samples, so convert to pixels first.)
    const double majorPx = (mode == TimeAxisMode::Sample)
                           ? (major / (rate * sc)) * pxPerSec : major * pxPerUnit;
    const bool showMinors = majorPx >= 40.0;

    // The ruler strip sits at the top by default or at the bottom when moved
    // there via the View menu; ticks point from the plot edge into it. Extra
    // time axes are narrower strips stacked between the main ruler and the
    // plot, with a smaller font so the main one stays the obvious one.
    const int stackH = app.rulerStackHeight();
    const auto mainStrip = sc::axes::timeStrip (0, pr.getX(), pr.getWidth(),
                                                pr.getY(), pr.getBottom(), stackH,
                                                rulerAtBottom());
    const int rulerTop = mainStrip.getY();
    const int rulerHgt = mainStrip.getHeight();
    const float sepY   = rulerAtBottom() ? (float) (rulerTop + 1) : (float) (pr.getY() - 1);
    const float tickEnd   = rulerAtBottom() ? sepY + 7.0f : sepY - 7.0f;
    const float minorEnd  = rulerAtBottom() ? sepY + 3.0f : sepY - 3.0f;

    g.setColour (sc::axes::axisGridColour());
    g.drawLine ((float) pr.getX(), sepY, (float) pr.getRight(), sepY, 1.0f);

    const double first = std::ceil (dispStart / major) * major;
    if (first > dispEnd + 1e-9)
        return;   // the visible span is shorter than one tick step

    g.setFont (sc::axes::axisLabelFont());
    std::vector<std::pair<float, double>> ticks;   // x -> real seconds
    const int nTicks = (int) std::floor ((dispEnd - first) / major + 1e-9) + 1;
    for (int i = 0; i < nTicks; ++i)
    {
        const double d = first + (double) i * major;
        if (d > dispEnd + 1e-9) break;
        const double t = dispToTime (d);
        if (t < viewStart - 1e-9 || t > viewEnd + 1e-9) continue;
        const float x = timeToX (t);
        ticks.push_back ({ x, t });

        g.setColour (sc::axes::axisGridColour());
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
        sc::axes::drawExtraTimeAxis (g, e, strip, ticks, rate);
        ++index;
    }
}
