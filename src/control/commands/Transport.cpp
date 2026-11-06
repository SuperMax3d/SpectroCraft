// SpectroCraft - AI control: transport (transport.*) and markers (markers.add / list).
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "Application.h"

namespace sc::control {

namespace {

juce::var transportState (Application& app)
{
    auto& e = app.audioEngine();
    return obj ({ { "playing", e.isPlaying() }, { "paused", e.isPaused() }, { "looping", e.isLooping() },
                  { "position", e.position() }, { "audioReady", app.audioDeviceReady() },
                  { "deviceDisabled", Application::audioOutputDisabled() } });
}

Outcome playCmd (Context& ctx)
{
    Application& app = ctx.app;
    // Never the unbounded wait of Application::play(): give the lazy device
    // init 2 s, then report instead of blocking the message thread.
    app.startAudioInitAsync();
    if (!app.ensureAudioReady (2000))
        return Outcome::failure (err::failed, "No audio device (SC_NO_AUDIO_DEVICE or init timeout)",
                                 obj ({ { "audioReady", false } }));
    if (ctx.has ("from"))
        app.setPlayhead (std::clamp (ctx.num ("from", 0.0), 0.0, app.audioEngine().duration()));
    if (ctx.has ("loop") && ctx.flag ("loop", false) != app.audioEngine().isLooping())
        app.toggleLoop();
    app.play();
    return Outcome::success (transportState (app));
}

Outcome stopCmd (Context& ctx)   { ctx.app.stop();  return Outcome::success (transportState (ctx.app)); }
Outcome pauseCmd (Context& ctx)  { ctx.app.pause(); return Outcome::success (transportState (ctx.app)); }

Outcome loopCmd (Context& ctx)
{
    if (ctx.flag ("on", true) != ctx.app.audioEngine().isLooping())
        ctx.app.toggleLoop();
    return Outcome::success (transportState (ctx.app));
}

Outcome spectralSelCmd (Context& ctx)
{
    Application& app = ctx.app;
    const bool on = ctx.flag ("on", true);
    if (on && !app.hasSpectralSelection())
        return Outcome::failure (err::failed, "the document has no spectral selection");
    app.playSpectralSel = on;
    app.pushSpectralMaskToEngine();
    if (app.onStateChange) app.onStateChange();
    auto r = transportState (app);
    set (r, "playSpectralSel", app.playSpectralSel);
    return Outcome::success (r);
}

juce::var markerVar (const Marker& m, int index)
{
    auto v = obj ({ { "index", index }, { "id", m.id }, { "name", m.name }, { "start", m.start },
                    { "end", m.end < 0 ? m.start : m.end }, { "region", m.end >= 0 } });
    if (m.comment.isNotEmpty()) set (v, "comment", m.comment);
    return v;
}

Outcome markersListCmd (Context& ctx)
{
    juce::Array<juce::var> a;
    const auto& ms = ctx.app.markers().markers();
    for (int i = 0; i < (int) ms.size(); ++i) a.add (markerVar (ms[(size_t) i], i));
    return Outcome::success (obj ({ { "doc", ctx.doc->id }, { "markers", a } }));
}

Outcome markersAddCmd (Context& ctx)
{
    Application& app = ctx.app;
    const double dur = app.audioEngine().duration();
    const double s = ctx.num ("start", 0.0);
    if (s > dur)
        return Outcome::failure (err::invalidParams, "start is beyond the end of the file",
                                 obj ({ { "path", "start" }, { "expected", "<= " + juce::String (dur) } }));
    const juce::String name = ctx.str ("name", "Marker");
    int index = -1;
    if (ctx.has ("end") && ctx.num ("end", s) > s)
        index = app.markers().addRegionMarker (s, std::min (dur, ctx.num ("end", s)), name);
    else
        index = app.markers().addPointMarker (s, name);
    if (app.onStateChange) app.onStateChange();
    return Outcome::success (obj ({ { "index", index }, { "count", (int) app.markers().markers().size() } }));
}

} // namespace

void registerTransportCommands (CommandRegistry& r)
{
    r.add ({ "transport.play", "Start playback (from a position, optionally looping)", "transport",
             { P ("from", PType::Number).min (0).describe ("Start position (s)"), P ("loop", PType::Boolean) },
             {}, NeedsDoc | ActiveDocOnly, {}, playCmd });
    r.add ({ "transport.stop", "Stop playback", "transport", {}, {}, ActiveDocOnly, {}, stopCmd });
    r.add ({ "transport.pause", "Pause playback", "transport", {}, {}, ActiveDocOnly, {}, pauseCmd });
    r.add ({ "transport.loop", "Loop the selection (or the whole file)", "transport",
             { P ("on", PType::Boolean).def (true) }, {}, NeedsDoc | ActiveDocOnly, {}, loopCmd });
    r.add ({ "transport.playSpectralSel", "Play only the spectral selection", "transport",
             { P ("on", PType::Boolean).def (true) }, {}, NeedsDoc | ActiveDocOnly, {}, spectralSelCmd });

    r.add ({ "markers.list", "Markers of a document", "markers", {}, {}, NeedsDoc | Query, {}, markersListCmd });
    r.add ({ "markers.add", "Add a point marker, or a region when end > start", "markers",
             { P ("name", PType::String).describe ("Marker title"),
               P ("start", PType::Number).req().min (0), P ("end", PType::Number).min (0) },
             {}, NeedsDoc, {}, markersAddCmd });
}

} // namespace sc::control
