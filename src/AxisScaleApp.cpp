// SpectroCraft - axis scale entries: the parts tied to the desktop Application
// and views (applying entries to the session state, ruler / strip geometry).
// The UI-free model, formatting and JSON stay in AxisScale.cpp, which the
// mobile app compiles on its own.
#include "AxisScale.h"
#include "Application.h"
#include "FileDoc.h"
#include "WaveformView.h"   // ruler height

#include <cmath>

namespace sc::axes
{

void applyEntries (Application& app, const List& entries)
{
    // ---- Time ----
    if (const Entry* t = primary (entries, Kind::Time))
    {
        app.timeAxisMode = (t->units == 2) ? TimeAxisMode::Sample
                         : (t->units == 1) ? TimeAxisMode::Seconds
                                           : TimeAxisMode::Time;
        app.timeOffsetSecs = t->offset;
        app.timeScaleFactor = (t->scale != 0.0 ? t->scale : 1.0);
    }
    else
    {
        app.timeAxisMode   = TimeAxisMode::Time;
        app.timeOffsetSecs = 0.0;
        app.timeScaleFactor = 1.0;
    }

    // ---- Amplitude: the primary entry IS the file's calibration. ----
    if (auto* d = app.doc())
    {
        if (const Entry* a = primary (entries, Kind::Amplitude))
        {
            d->scale.unit = calibrationUnit (a->units);
            d->scale.fullScaleValue = (a->units >= 2) ? a->scaleFor (-1) : 0.0;
            d->scale.channelFullScale.clear();
            if (a->hasPerChannelScale() && a->units >= 2)
                d->scale.channelFullScale = a->chanScale;
            app.waveVScaleMode = (a->units == 0) ? 1 : (a->units == 1) ? 2 : 0;
        }
        else
        {
            d->scale = ScaleCalibration{};
            app.waveVScaleMode = 1;
        }
        d->altRef.altTimeRefs.clear();
        d->altRef.altFreqRefs.clear();
    }

    // ---- Frequency ----
    if (const Entry* f = primary (entries, Kind::Frequency))
    {
        const double sc = (f->scale != 0.0 ? f->scale : 1.0);
        app.specFreqScaleMode = (f->units == 0 && std::abs (sc - 1.0) < 1e-12) ? 0 : 1;
        app.specFreqFactor    = sc;
        app.specFreqUnits     = f->units;
    }
    else
    {
        app.specFreqScaleMode = 0;
        app.specFreqFactor    = 1.0;
        app.specFreqUnits     = 0;
    }

    // ---- The secondary entries ("Axis and Footer" extras and "Footer
    // Only") become the alternative-scale references, for the code that still
    // reads that list. The readouts, Go to Time and the table columns use the
    // entries themselves (unit-aware: a reference is only a factor). ----
    app.altTimeRefs.clear();
    app.altFreqRefs.clear();
    for (int i = 0; i < (int) entries.size(); ++i)
    {
        const Entry& e = entries[(size_t) i];
        if (! isSecondary (entries, i) || e.kind == Kind::Amplitude) continue;
        Application::AltScaleRef r;
        r.name     = e.name;
        r.enabled  = true;
        r.refMain  = 1.0;                       // anchored at 1: alt(x) = x * scale
        r.altValue = (e.scale != 0.0 ? e.scale : 1.0);
        if (e.kind == Kind::Time)      app.altTimeRefs.push_back (r);
        else if (e.kind == Kind::Frequency) app.altFreqRefs.push_back (r);
    }
}

static_assert (WaveformView::kRulerH == 24, "sc::axes::primaryRulerHeight() (AxisScale.cpp) must match the waveform ruler");

int axisStripWidth (const Application& app)
{
    const List& l = app.axisEntries;
    const int n = std::max (1, std::max (countAddAxis (l, Kind::Amplitude),
                                         countAddAxis (l, Kind::Frequency)));
    return primaryStripWidth() + (n - 1) * extraStripWidth();
}

int rulerStackHeight (const Application& app)
{
    const List& l = app.axisEntries;
    const int extra = std::max (0, countAddAxis (l, Kind::Time) - 1);
    return primaryRulerHeight() + extra * extraRulerHeight();
}

} // namespace sc::axes
