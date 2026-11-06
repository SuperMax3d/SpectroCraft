// SpectroCraft - AI control: view and window commands (view.*, ui.*).
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "control/UiBridge.h"
#include "Application.h"

namespace sc::control {

namespace {

Outcome invalid (const juce::String& path, const juce::String& expected, const juce::String& msg)
{
    return Outcome::failure (err::invalidParams, msg, obj ({ { "path", path }, { "expected", expected } }));
}

juce::var channelsVar (const std::vector<int>& v)
{
    juce::Array<juce::var> a;
    for (int c : v) a.add (c);
    return a;
}

/** Applies [t0, t1] to the views (through the window when there is one). */
void applyWindow (Context& ctx, double t0, double t1)
{
    Application& app = ctx.app;
    if (ctx.ui != nullptr)
        ctx.ui->applyTimeWindow (t0, t1);
    else if (app.onSetTimeWindow)
        app.onSetTimeWindow (t0, t1);
    app.viewStart = t0;   // the window callback keeps these in sync; headless sets them here
    app.viewEnd = t1;
}

juce::var viewResult (Application& app)
{
    return obj ({ { "t0", app.viewStart }, { "t1", app.viewEnd },
                  { "visibleChannels", channelsVar (app.visibleChannels) } });
}

Outcome viewSetCmd (Context& ctx)
{
    const double dur = ctx.app.audioEngine().duration();
    double t0 = std::max (0.0, ctx.num ("t0", 0.0));
    double t1 = std::min (dur, ctx.num ("t1", dur));
    if (t1 <= t0)
        return invalid ("t1", "> t0 (within the file)", "t1 must be greater than t0");
    applyWindow (ctx, t0, t1);
    return Outcome::success (viewResult (ctx.app));
}

Outcome viewZoomCmd (Context& ctx)
{
    Application& app = ctx.app;
    const double dur = app.audioEngine().duration();
    if (ctx.has ("vertical"))
    {
        if (ctx.ui == nullptr)
            return Outcome::failure (err::needsUi, "vertical zoom needs the open application window");
        ctx.ui->zoomVertical (ctx.num ("vertical", 1.0));
    }
    const juce::String to = ctx.str ("to");
    if (to == "selection")
    {
        if (app.selection.getLength() <= 0)
            return Outcome::failure (err::failed, "there is no time selection to zoom to");
        if (ctx.ui != nullptr) ctx.ui->zoomToSelection();
        else applyWindow (ctx, app.selection.getStart(), app.selection.getEnd());
    }
    else if (to == "fit" || to == "all")
    {
        if (ctx.ui != nullptr) { if (to == "fit") ctx.ui->zoomToFit(); else ctx.ui->zoomOutFull(); }
        else applyWindow (ctx, 0.0, std::max (dur, 1e-6));
    }
    else if (ctx.has ("factor"))
    {
        const double f = ctx.num ("factor", 1.0);
        const double c = 0.5 * (app.viewStart + app.viewEnd);
        double span = std::clamp ((app.viewEnd - app.viewStart) / f, 1e-4, std::max (dur, 1e-4));
        double t0 = std::clamp (c - span / 2, 0.0, std::max (0.0, dur - span));
        applyWindow (ctx, t0, t0 + span);
    }
    else if (!ctx.has ("vertical"))
        return invalid ("to", "factor, to or vertical", "give factor, to or vertical");
    return Outcome::success (viewResult (app));
}

Outcome viewChannelsCmd (Context& ctx)
{
    Application& app = ctx.app;
    const auto v = ctx.get ("visible");
    const int n = app.audioEngine().numChannels();
    std::vector<int> vis;
    for (int i = 0; i < v.size(); ++i)
    {
        const int c = (int) v[i];
        if (c < 0 || c >= n)
            return invalid ("visible[" + juce::String (i) + "]", "0.." + juce::String (n - 1),
                            "channel " + juce::String (c) + " does not exist");
        if (std::find (vis.begin(), vis.end(), c) == vis.end()) vis.push_back (c);
    }
    if (vis.empty()) return invalid ("visible", "non-empty array", "at least one channel must stay visible");
    std::sort (vis.begin(), vis.end());
    app.visibleChannels = vis;
    app.syncActiveDoc();
    if (app.onStateChange) app.onStateChange();
    return Outcome::success (obj ({ { "visibleChannels", channelsVar (app.visibleChannels) } }));
}

Outcome viewShowCmd (Context& ctx)
{
    auto r = obj();
    for (const char* k : { "waveform", "spectral", "leftPanels" })
    {
        if (ctx.has (k)) ctx.ui->setViewVisible (k, ctx.flag (k, true));
        set (r, k, ctx.ui->isViewVisible (k));
    }
    return Outcome::success (r);
}

Outcome panelsCmd (Context& ctx)
{
    juce::Array<juce::var> a;
    const auto names = ctx.ui->panelNames();
    for (int i = 0; i < names.size(); ++i)
        a.add (obj ({ { "index", i }, { "name", names[i] }, { "open", ctx.ui->isPanelOpenAt (i) } }));
    return Outcome::success (obj ({ { "panels", a } }));
}

Outcome panelSetCmd (Context& ctx)
{
    const auto names = ctx.ui->panelNames();
    int idx = ctx.integer ("index", -1);
    if (ctx.has ("name"))
    {
        idx = -1;
        for (int i = 0; i < names.size(); ++i)
            if (names[i].equalsIgnoreCase (ctx.str ("name"))) idx = i;
    }
    if (idx < 0 || idx >= names.size())
        return invalid (ctx.has ("name") ? "name" : "index", "one of [" + names.joinIntoString (", ") + "]",
                        "unknown panel");
    ctx.ui->setPanelVisibleAt (idx, ctx.flag ("visible", true));
    return Outcome::success (obj ({ { "index", idx }, { "name", names[idx] }, { "open", ctx.ui->isPanelOpenAt (idx) } }));
}

} // namespace

void registerViewCommands (CommandRegistry& r)
{
    r.add ({ "view.set", "Set the visible time window (seconds)", "view",
             { P ("t0", PType::Number).req().min (0), P ("t1", PType::Number).req().min (0) },
             {}, NeedsDoc | ActiveDocOnly, {}, viewSetCmd });
    r.add ({ "view.zoom", "Zoom: factor (>1 zooms in), to selection|fit|all, or vertical factor (window only)", "view",
             { P ("factor", PType::Number).range (0.001, 1000),
               P ("to", PType::String).oneOf ({ "selection", "fit", "all" }),
               P ("vertical", PType::Number).range (0.01, 100).describe ("Vertical zoom factor (needs the window)") },
             {}, NeedsDoc | ActiveDocOnly, {}, viewZoomCmd });
    r.add ({ "view.channels", "Visible tracks (also the channels edits apply to)", "view",
             { P ("visible", PType::Array).req().items (PType::Integer, 0.0) },
             {}, NeedsDoc | ActiveDocOnly, {}, viewChannelsCmd });
    r.add ({ "view.show", "Show/hide the waveform, the spectrogram or the left panels", "view",
             { P ("waveform", PType::Boolean), P ("spectral", PType::Boolean), P ("leftPanels", PType::Boolean) },
             {}, NeedsUi, {}, viewShowCmd });
    r.add ({ "ui.panels", "Left-column panels (Window menu) and whether they are open", "ui",
             {}, {}, NeedsUi | Query, {}, panelsCmd });
    r.add ({ "ui.panel.set", "Open or close a panel (Window menu)", "ui",
             { P ("name", PType::String).describe ("Panel name as in the Window menu"),
               P ("index", PType::Integer).min (0), P ("visible", PType::Boolean).def (true) },
             {}, NeedsUi, {}, panelSetCmd });
}

} // namespace sc::control
