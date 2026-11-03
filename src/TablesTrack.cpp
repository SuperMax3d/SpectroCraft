// SpectroCraft - Dedicated tables track: plots every visible parameter on an
// auto-scaled value axis with vertical zoom/pan, a red playhead and hover
// labels showing the unscaled values of curves near the cursor.
#include "TablesTrack.h"

TablesTrackView::TablesTrackView (Application& appIn)
    : app (appIn)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void TablesTrackView::setTimeRange (double t0In, double t1In)
{
    if (t0In == t0 && t1In == t1) return;
    t0 = std::min (t0In, t1In);
    t1 = std::max (std::nextafter (t0, 2.0), t1In);
    repaint();
}

void TablesTrackView::setPlayhead (double tSecs)
{
    if (tSecs == playheadT) return;
    playheadT = tSecs;
    repaint();
}

void TablesTrackView::setValueRange (double lo, double hi)
{
    applyValueWindow (lo, hi);
}

void TablesTrackView::setSelection (const juce::Range<double>& s)
{
    if (s != selShown) { selShown = s; repaint(); }
}

// Single funnel for every change of the visible value window: validates,
// stores, repaints and notifies MainWindow so the vertical scrollbar's thumb
// follows wheel zooms, drags and refits.
void TablesTrackView::applyValueWindow (double lo, double hi)
{
    if (!(hi > lo) || !std::isfinite ((float) lo) || !std::isfinite ((float) hi)) return;
    vMin = lo;
    vMax = hi;
    repaint();
    if (onValueWindowChanged) onValueWindowChanged (vMin, vMax);
}

juce::Rectangle<int> TablesTrackView::plotRect() const
{
    // Same horizontal layout as the data tracks: a value-axis strip of width
    // kLeftAxisWidth on whichever side the View menu puts it; the plot fills
    // the rest (the scrollbar sits outside this component).
    const int x0 = app.settings.trackAxisOnRight ? 0 : kAxisW;
    return { x0, 0, std::max (1, getWidth() - kAxisW), std::max (1, getHeight()) };
}

double TablesTrackView::valueAtY (int y) const
{
    const auto pr = plotRect();
    if (vMax <= vMin) return vMin;
    const double frac = ((double) y - (double) pr.getY()) / (double) pr.getHeight();
    return vMax + (vMin - vMax) * std::clamp (frac, 0.0, 1.0);
}

int TablesTrackView::yForValue (double v) const
{
    const auto pr = plotRect();
    if (vMax <= vMin) return pr.getY() + pr.getHeight() / 2;
    const double frac = (vMax - v) / (vMax - vMin);
    return (int) std::lround ((double) pr.getY() + frac * (double) pr.getHeight());
}

double TablesTrackView::timeAtX (int x) const
{
    const auto pr = plotRect();
    if (t1 <= t0 || pr.getWidth () <= 0) return t0;
    return t0 + ((double) x - (double) pr.getX ()) / (double) pr.getWidth () * (t1 - t0);
}

void TablesTrackView::refit()
{
    // Auto-scale: the visible window spans all values of every visible
    // parameter after its dedicated-track mapping (valueScale[2]/valueOffset[2]),
    // padded by 10% on each side.
    double mn = 0.0, mx = 0.0;
    bool have = false;
    for (const auto& t : app.activeTables())
    {
        if (!t.visible) continue;
        for (const auto& p : t.params)
        {
            if (!p.visible || p.values.empty()) continue;
            double lo = sc::tables::dedicatedValue (p.values[0], p), hi = lo;
            for (double v : p.values)
            {
                const double m = sc::tables::dedicatedValue (v, p);
                lo = std::min (lo, m);
                hi = std::max (hi, m);
            }
            if (!have) { mn = mx = lo; have = true; }
            mn = std::min (mn, lo);
            mx = std::max (mx, hi);
        }
    }
    const double pad = std::max ((mx - mn) * 0.1, 1e-9);
    fitMin = have ? mn - pad : -1.0;
    fitMax = have ? mx + pad : 1.0;
    if (fitMax <= fitMin) fitMax = fitMin + 1e-9;
    applyValueWindow (fitMin, fitMax);
}

// ------------------------------------------------------------------ Paint --

juce::Rectangle<int> TablesTrackView::axisStripRect() const
{
    return { app.settings.trackAxisOnRight ? getWidth() - kAxisW : 0, 0, kAxisW, getHeight() };
}

bool TablesTrackView::onAxisX (int x) const
{
    return app.settings.trackAxisOnRight ? x >= getWidth() - kAxisW : x < kAxisW;
}

// Fewer decimals for large magnitudes so big values stay readable in the
// narrow axis strip and hover labels.
static int valueDecimals (double v)
{
    const double a = std::abs (v);
    if (a < 1.0) return 4;
    if (a < 10.0) return 3;
    if (a < 100.0) return 2;
    if (a < 1000.0) return 1;
    return 0;
}

void TablesTrackView::paint (juce::Graphics& g)
{
    // Same background as the waveform/spectral data tracks.
    g.fillAll (juce::Colour::fromRGB (30, 30, 30));
    if (!app.audioEngine().isFileLoaded()) return;

    const auto pr = plotRect();
    const bool axRight = app.settings.trackAxisOnRight;
    const juce::Rectangle<int> axisStrip = axisStripRect ();

    // Value axis: gridlines with labels in the value-axis strip, like the other tracks.
    g.setColour (juce::Colour::fromRGB (32, 32, 36));
    g.fillRect (axisStrip);
    const int nLines = std::max (2, std::min (8, getHeight() / 18));
    g.setFont (11.0f);
    for (int i = 0; i <= nLines; ++i)
    {
        const double v = vMax + (vMin - vMax) * ((double) i / nLines);
        const int y = yForValue (v);
        // The grey horizontal grid lines can be hidden from the View menu;
        // the axis labels below are unaffected.
        if (app.settings.showDedicatedTrackGrid)
        {
            g.setColour (juce::Colour::fromRGB (60, 60, 68));
            g.drawHorizontalLine (pr.getX(), (float) y, (float) pr.getRight());
        }
        g.setColour (juce::Colours::lightgrey);
        g.drawText (juce::String (v, valueDecimals (v)),
                   juce::Rectangle<int> (axisStrip.getX() + 2, y - 8,
                    kAxisW - 6, 15), axRight ? juce::Justification::right
                                             : juce::Justification::left, true);
    }

    // One curve per visible parameter: raw values through the effective time
    // map and the dedicated-track value mapping (valueScale[2]/valueOffset[2]).
    // The overlay transforms (normalized/inverted/absolute) are NOT applied -
    // this is the unscaled view.
    for (const auto& t : app.activeTables())
    {
        if (!t.visible) continue;
        for (const auto& p : t.params)
        {
            if (!p.visible || p.values.size() < 2) continue;
            const auto [off, sc] = t.effectiveTimeMap (p);

            juce::Path path;
            bool started = false;
            for (int i = 0; i < pr.getWidth(); ++i)
            {
                const double tDoc = t0 + ((double) i / (double) pr.getWidth()) * (t1 - t0);
                const double tTab = (tDoc - off) / sc;
                if (tTab < 0.0 || tTab > p.duration (t.sampleRate)) { started = false; continue; }
                const double v = sc::tables::dedicatedValue (
                    sc::tables::paramValueAt (t, p, tTab), p);
                const int y = yForValue (v);
                const float x = (float) (pr.getX() + i) + 0.5f;
                if (!started) { path.startNewSubPath (x, (float) y); started = true; }
                else          path.lineTo (x, (float) y);
            }
            g.setColour (sc::tables::tableColourAt (p.colorIndex,
                                                   app.settings.tableCustomColours));
            g.strokePath (path, juce::PathStrokeType (2.0f));
        }
    }

    // Blue time-selection band: the same translucent blue as the waveform and
    // spectral views, spanning the whole strip height, with light-blue edge
    // lines like those views.
    if (!app.selection.isEmpty() && t1 > t0)
    {
        const double x1 = pr.getX() + (app.selection.getStart () - t0) / (t1 - t0) * pr.getWidth();
        const double x2 = pr.getX() + (app.selection.getEnd () - t0) / (t1 - t0) * pr.getWidth();
        const float lo = std::max ((float) pr.getX(), (float) x1);
        const float hi = std::min ((float) pr.getRight(), (float) x2);
        if (hi > lo)
        {
            g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
            g.fillRect (lo, 0.0f, hi - lo, (float) getHeight());
            // Edge lines clipped to the plot so edges before the visible
            // window don't paint over the value axis.
            const juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (pr);
            g.setColour (juce::Colour::fromRGB (96, 150, 255));
            g.drawVerticalLine ((float) x1, 0.0f, (float) getHeight());
            g.drawVerticalLine ((float) x2, 0.0f, (float) getHeight());
        }
    }

    // Red playhead over the whole strip.
    if (t1 > t0)
    {
        const double px = pr.getX() + (playheadT - t0) / (t1 - t0) * pr.getWidth();
        if (px >= pr.getX() && px <= pr.getRight())
        {
            g.setColour (juce::Colour::fromRGB (255, 60, 60));
            g.drawVerticalLine ((int) std::lround (px), 0.0f, (float) getHeight());
        }
    }

    // Right-drag rubber band: the same translucent blue as the time selection,
    // so it reads as "this region" rather than a generic white marquee.
    if (haveRubberBand && dragMode != DragMode::None)
    {
        g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
        g.fillRect (rubberRect);
        g.setColour (juce::Colour::fromRGB (96, 150, 255).withAlpha (0.9f));
        g.drawRect (rubberRect, 1);
    }

    // Hover labels: unscaled values of the curves near the cursor, stacked
    // from the top with a fixed spacing so they never overlap.
    if (!hoverLabels.empty())
    {
        g.setFont (12.0f);
        const juce::Font f (12.0f);
        int y = 4;
        for (const auto& hl : hoverLabels)
        {
            const auto sz = (int) juce::GlyphArrangement::getStringWidth (f, hl.text);
            juce::Rectangle<int> box ((int) hoverPos.x + 12, y, sz + 8, 15);
            g.setColour (juce::Colour::fromRGBA (20, 20, 24, 200));
            g.fillRoundedRectangle (box.toFloat(), 3.0f);
            g.setColour (hl.colour);
            g.drawText (hl.text, box, juce::Justification::centredLeft, true);
            y += 17;   // fixed spacing keeps the labels from overlapping
        }
    }
}

// ----------------------------------------------------------------- Mouse --

void TablesTrackView::mouseDown (const juce::MouseEvent& e)
{
    if (!app.audioEngine().isFileLoaded()) return;
    haveRubberBand = false;
    dragStartX = e.x;
    dragStartY = e.y;

    // Right button: box-zoom drag. A plain right-click (no significant
    // movement) still shows the context menu, deferred to mouseUp.
    if (e.mods.isRightButtonDown() && !e.mods.isLeftButtonDown())
    {
        dragMode = onAxisX (e.x) ? DragMode::ZoomAxis : DragMode::ZoomBox;
        return;   // right-button never pans
    }

    // Left button: time selection in the data area (like the waveform),
    // vertical pan on the value-axis strip.
    if (e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown())
    {
        if (onAxisX (e.x))
        {
            dragMode = DragMode::VPan;
            dragVMin = vMin;
            dragVMax = vMax;
            return;
        }

        // Clicking in the data area moves the playhead to that position,
        // exactly like the waveform view. Dragging then extends a time
        // selection from the click point; a plain click leaves none.
        const double tClick = timeAtX (e.x);
        app.setPlayhead (tClick);
        dragMode = DragMode::Select;
        app.setSelection ({ tClick, tClick });   // zero width until dragged
        return;
    }

    // Middle button: horizontal time pan of the shared window.
    if (e.mods.isMiddleButtonDown() && !e.mods.isLeftButtonDown()
        && !e.mods.isRightButtonDown())
    {
        dragMode = DragMode::HPan;
        dragT0 = t0;
        dragT1 = t1;
        return;
    }

    dragMode = DragMode::None;
}

void TablesTrackView::mouseDrag (const juce::MouseEvent& e)
{
    if (!app.audioEngine().isFileLoaded()) return;

    switch (dragMode)
    {
        case DragMode::VPan:
        {
            if (vMax <= vMin) break;
            // Vertical pan: content follows the cursor (drag down -> the data
            // moves down), matching the waveform/spectrogram axis drags.
            const auto pr = plotRect();
            const double dv = ((double) e.y - (double) dragStartY) * (vMax - vMin) / pr.getHeight();
            applyValueWindow (dragVMin + dv, dragVMax + dv);
            break;
        }

        case DragMode::Select:
        {
            // Live time selection while dragging; setSelection() notifies the
            // UI so every view shows the band.
            if (t1 > t0)
                app.setSelection ({ std::min (timeAtX (dragStartX), timeAtX (e.x)),
                                    std::max (timeAtX (dragStartX), timeAtX (e.x)) });
            break;
        }

        case DragMode::HPan:
        {
            // Horizontal pan: dragging left moves the window forward in time.
            const auto pr = plotRect();
            if (onTimeWindow && t1 > t0)
            {
                const double dt = ((double) dragStartX - (double) e.x) * (t1 - t0) / pr.getWidth ();
                onTimeWindow (dragT0 + dt, dragT1 + dt);
            }
            break;
        }

        case DragMode::ZoomAxis:
        {
            // Vertical-zoom rubber band, constrained to the axis strip.
            if (!haveRubberBand && (std::abs (e.x - dragStartX) >= 4 || std::abs (e.y - dragStartY) >= 4))
                haveRubberBand = true;
            if (haveRubberBand)
            {
                const auto strip = axisStripRect ();
                rubberRect = juce::Rectangle<int> (strip.getX(),
                                                   std::min (dragStartY, e.y),
                                                   strip.getWidth(),
                                                   std::max (2, std::abs (e.y - dragStartY)));
                repaint();
            }
            break;
        }

        case DragMode::ZoomBox:
        {
            // x/y box-zoom rubber band over the plot area.
            if (!haveRubberBand && (std::abs (e.x - dragStartX) >= 4 || std::abs (e.y - dragStartY) >= 4))
                haveRubberBand = true;
            if (haveRubberBand)
            {
                const auto pr = plotRect();
                const int x1 = std::max (pr.getX(), std::min (dragStartX, e.x));
                const int y1 = std::max (pr.getY(), std::min (dragStartY, e.y));
                const int x2 = std::min (pr.getRight() - 1, std::max (dragStartX, e.x));
                const int y2 = std::min (pr.getBottom() - 1, std::max (dragStartY, e.y));
                rubberRect = juce::Rectangle<int> (x1, y1,
                                                   std::max (2, x2 - x1),
                                                   std::max (2, y2 - y1));
                repaint();
            }
            break;
        }

        default: break;
    }
}

void TablesTrackView::mouseUp (const juce::MouseEvent& e)
{
    if (!app.audioEngine().isFileLoaded())
    {
        dragMode = DragMode::None;
        haveRubberBand = false;
        return;
    }

    switch (dragMode)
    {
        case DragMode::ZoomAxis:
        {
            if (!haveRubberBand)
                showTrackContextMenu (true);   // plain right-click on the axis
            else if (rubberRect.getHeight() >= 2 && vMax > vMin)
            {
                // The dragged span becomes the new full value range.
                const double top = valueAtY (rubberRect.getY());
                const double bot = valueAtY (rubberRect.getBottom());
                if (top - bot > 1e-12)
                    applyValueWindow (bot, top);
            }
            break;
        }

        case DragMode::ZoomBox:
        {
            if (!haveRubberBand)
                showTrackContextMenu (false);  // plain right-click on the data area
            else
            {
                const auto pr = plotRect();
                // Time part: the shared window, routed through MainWindow.
                const double tA = t0 + ((double) rubberRect.getX() - (double) pr.getX())
                                        / (double) pr.getWidth() * (t1 - t0);
                const double tB = t0 + ((double) (rubberRect.getRight() - 1) - (double) pr.getX())
                                        / (double) pr.getWidth() * (t1 - t0);
                if (onTimeWindow && std::abs (tB - tA) > 1e-9)
                    onTimeWindow (std::min (tA, tB), std::max (tA, tB));
                // Value part: local to this strip.
                const double top = valueAtY (rubberRect.getY());
                const double bot = valueAtY (rubberRect.getBottom());
                if (top - bot > 1e-12)
                    applyValueWindow (bot, top);
                else
                    repaint();
            }
            break;
        }

        case DragMode::Select:
        {
            if (t1 > t0)
            {
                const double a = timeAtX (dragStartX);
                const double b = timeAtX (e.x);
                if (std::abs (b - a) <= 1e-9 || std::abs (e.x - dragStartX) < 4)
                    app.setSelection ({ 0, 0 });   // plain click clears it
                else
                    app.setSelection ({ std::min (a, b), std::max (a, b) });
            }
            break;
        }

        case DragMode::HPan:
        {
            const auto pr = plotRect();
            if (onTimeWindow && t1 > t0)
            {
                const double dt = ((double) dragStartX - (double) e.x) * (t1 - t0) / pr.getWidth ();
                onTimeWindow (dragT0 + dt, dragT1 + dt);
            }
            break;
        }

        default: break;   // VPan ends silently
    }

    dragMode = DragMode::None;
    haveRubberBand = false;
    rubberRect = {};
}

void TablesTrackView::showTrackContextMenu (bool onAxis)
{
    juce::PopupMenu m;
    m.addItem (1, "Zoom to Selection");
    // On the data area this resets time + value; on the vertical axis it
    // resets only the value range.
    m.addItem (2, "Zoom Out Full");
    m.addSeparator ();
    m.addItem (3, "Select All");
    m.addItem (4, "Copy Curve Data");

    const int id = m.showMenu (juce::PopupMenu::Options ().withMousePosition ());
    switch (id)
    {
        case 1: if (onZoomToSelection) onZoomToSelection (); break;
        case 2: if (onZoomOutFull) onZoomOutFull (onAxis); break;
        case 3: app.selectAll (); break;   // notifies via setSelection()
        case 4: copyCurveData (); break;
        default: break;
    }
}

void TablesTrackView::mouseMove (const juce::MouseEvent& e)
{
    hoverPos = { e.x, e.y };
    collectHoverLabels (e.x, e.y);
    repaint();
}

void TablesTrackView::mouseExit (const juce::MouseEvent&)
{
    hoverPos = { -1, -1 };
    hoverLabels.clear();
    repaint();
}

void TablesTrackView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!app.audioEngine().isFileLoaded() || vMax <= vMin) return;

    // Wheel over the value axis zooms vertically; anywhere else in the strip
    // it zooms horizontally around the cursor, like the other tracks.
    if (onAxisX (e.x))
    {
        const double pivot = valueAtY (e.y);
        const double factor = std::pow (1.2, -wheel.deltaY);
        const double nlo = pivot + (vMin - pivot) * factor;
        const double nhi = pivot + (vMax - pivot) * factor;
        if (nhi - nlo < 1e-12 || !std::isfinite ((float) (nhi - nlo))) refit();
        else applyValueWindow (nlo, nhi);
        return;
    }

    if (!onTimeWindow || t1 <= t0) return;
    const auto pr = plotRect();
    const double factor = std::pow (1.2, -wheel.deltaY);
    const double tCur = t0 + ((double) e.x - (double) pr.getX()) / (double) pr.getWidth() * (t1 - t0);
    const double newHalf = (t1 - t0) * 0.5 / factor;
    const double ratio = std::clamp (((double) e.x - (double) pr.getX()) / (double) pr.getWidth(), 0.0, 1.0);

    double ns = tCur - newHalf * (2.0 * ratio);
    double ne = tCur + newHalf * (2.0 - 2.0 * ratio);
    const double w = ne - ns;
    if (ns < 0.0) { ns = 0.0; ne = std::min (w, app.audioEngine().duration()); }
    if (ne > app.audioEngine().duration()) { ne = app.audioEngine().duration(); ns = std::max (0.0, ne - w); }
    if (ne <= ns) return;   // zoomed past the limits
    onTimeWindow (ns, ne);
}

void TablesTrackView::collectHoverLabels (int mouseX, int mouseY)
{
    hoverLabels.clear();
    if (!app.audioEngine().isFileLoaded()) return;

    const auto pr = plotRect();
    if (mouseX < pr.getX() || mouseX >= pr.getRight()) return;
    const double tDoc = t0 + ((double) (mouseX - pr.getX()) / (double) pr.getWidth()) * (t1 - t0);

    struct Hit { float y; juce::String text; juce::Colour colour; };
    std::vector<Hit> hits;
    for (const auto& t : app.activeTables())
    {
        if (!t.visible) continue;
        for (const auto& p : t.params)
        {
            if (!p.visible || p.values.size() < 2) continue;
            const auto [off, sc] = t.effectiveTimeMap (p);
            const double tTab = (tDoc - off) / sc;
            if (tTab < 0.0 || tTab > p.duration (t.sampleRate)) continue;
            const double vRaw = sc::tables::paramValueAt (t, p, tTab);   // unscaled value
            const int y = yForValue (sc::tables::dedicatedValue (vRaw, p));
            if (std::abs ((float) y - (float) mouseY) <= 8.0f)
                hits.push_back ({ (float) y, p.name + "  " + juce::String (vRaw, valueDecimals (vRaw)),
                                  sc::tables::tableColourAt (p.colorIndex,
                                                             app.settings.tableCustomColours) });
        }
    }

    // Sort by screen position and stack the labels from the top of the strip.
    std::sort (hits.begin(), hits.end(),
               [] (const Hit& a, const Hit& b) { return a.y < b.y; });
    for (const auto& h : hits)
        hoverLabels.push_back ({ h.text, 4.0f, h.colour });   // y fixed in paint()
}

// ------------------------------------------------------------------ Copy --

bool TablesTrackView::copyCurveData()
{
    auto* d = app.doc ();
    if (d == nullptr) return false;

    const bool haveSel = !app.selection.isEmpty();
    const double s0 = haveSel ? app.selection.getStart () : 0.0;
    const double s1 = haveSel ? app.selection.getEnd ()
                              : std::numeric_limits<double>::max ();

    // One column per visible parameter, in draw order.
    struct Col { const sc::tables::TableData* t; const sc::tables::ParamData* p; };
    std::vector<Col> cols;
    for (const auto& tb : d->tables)
        if (tb.visible)
            for (const auto& p : tb.params)
                if (p.visible && !p.values.empty ())
                    cols.push_back ({ &tb, &p });
    if (cols.empty ()) return false;

    // Per column: the contiguous sample range inside the time selection.
    std::vector<int> first ((size_t) cols.size (), -1), last ((size_t) cols.size (), -1);
    int nRows = 0;
    for (size_t c = 0; c < cols.size (); ++c)
    {
        const auto& t = *cols[c].t;
        const auto& p = *cols[c].p;
        int lo = -1, hi = -1;
        for (int i = 0; i < (int) p.values.size (); ++i)
        {
            const double u = t.docTimeOfSample (p, i);
            if (u >= s0 && u <= s1)
            {
                if (lo < 0) lo = i;
                hi = i;
            }
        }
        first[c] = lo;
        last[c] = hi;
        if (lo >= 0) nRows = std::max (nRows, hi - lo + 1);
    }

    // Excel-friendly grid: tab-separated columns, CR line endings.
    juce::String out;
    for (size_t i = 0; i < cols.size (); ++i)
        out += (i ? "\t" : "") + cols[i].p->name;
    for (int r = 0; r < nRows; ++r)
    {
        out += "\r\n";
        for (size_t c = 0; c < cols.size (); ++c)
        {
            const int i = first[c] + r;
            if (first[c] >= 0 && i <= last[c])
                out += (c ? "\t" : "") + juce::String (cols[c].p->values[(size_t) i], 6);
            else
                out += (c ? "\t" : "");
        }
    }

    juce::SystemClipboard::copyTextToClipboard (out);
    return true;
}
