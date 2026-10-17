/**
 * SpectroCraft - Application Implementation
 */

#include "Application.h"
#include <algorithm>

Application::Application()
{
    audioEngine.setUndoManager (&undo);
    audioEngine.setMarkersForSave (&markers.markers()); // embedded as RIFF cue/adtl in WAV saves
    spectralEngine.setColourMap (colourMap);

    // Real-time output: the player streams from the engine's transport into
    // the default output device, so play/pause/loop actually produce sound.
    sourcePlayer.setSource (&audioEngine);
    audioDevice = std::make_unique<juce::AudioDeviceManager>();
    audioDevice->addAudioCallback (&sourcePlayer);
    audioDevice->initialiseWithDefaultDevices (0, 2); // no input, stereo output
}

Application::~Application() {}

bool Application::openFile (const juce::File& file)
{
    audioEngine.stop(); // stop any playback of the previous file
    spectralEngine.clearCache(); // tiles are keyed by sample position only

    if (!audioEngine.loadFile (file))
        return false;

    // Prefer the sidecar when one exists; otherwise recover markers embedded
    // in WAV files (RIFF cue/adtl chunks, classic DAW marker layout).
    if (!markers.loadSidecar (file) && file.getFileExtension().equalsIgnoreCase (".wav"))
        markers.loadFromWavFile (file, audioEngine.sampleRate());

    selection = { 0, 0 };
    playhead = 0.0;
    viewStart = 0;
    viewEnd = audioEngine.duration();
    visibleChannels.resize (audioEngine.numChannels());
    for (size_t i = 0; i < visibleChannels.size(); ++i)
        visibleChannels[i] = (int) i;
    undo.clear();
    return true;
}

void Application::closeFile()
{
    audioEngine.stop();
    audioEngine.closeFile();
    spectralEngine.clearCache();
    selection = { 0, 0 };
    playhead = 0.0;
    viewStart = 0; viewEnd = 10;
}

void Application::selectAll()
{
    // Go through setSelection() so the UI (both views + status bar) is
    // notified of the new range.
    if (audioEngine.isFileLoaded())
        setSelection ({ 0, audioEngine.duration() });
}

void Application::setSelection (juce::Range<double> s)
{
    const juce::Range<double> clamped = s.getIntersectionWith ({ 0, audioEngine.duration() });
    if (clamped == selection) return;
    selection = clamped;

    // Notify the UI: both views need the new range and the status bar shows
    // the selection start/stop times. Firing on every change keeps the readout
    // live while dragging a selection in either view.
    if (onStateChange)
        onStateChange ();
}

bool Application::isChannelVisible (int ch) const
{
    return std::find (visibleChannels.begin(), visibleChannels.end(), ch) != visibleChannels.end();
}

void Application::toggleChannelVisible (int ch)
{
    if (ch < 0 || ch >= audioEngine.numChannels())
        return;
    auto it = std::find (visibleChannels.begin(), visibleChannels.end(), ch);
    if (it != visibleChannels.end())
        visibleChannels.erase (it);
    else
    {
        visibleChannels.push_back (ch);
        std::sort (visibleChannels.begin(), visibleChannels.end());
    }
}

int Application::addMarkerAtCursor()
{
    juce::String name = "Marker " + juce::String ((int) markers.count() + 1).paddedLeft ('0', 2);
    if (!selection.isEmpty())
        return markers.addRegionMarker (selection.getStart(), selection.getEnd(), name);
    return markers.addPointMarker (playhead, name);
}

static juce::Range<int64_t> selectionSamples (const Application& app)
{
    const double rate = app.audioEngine.sampleRate();
    return { (int64_t) (app.selection.getStart() * rate),
             (int64_t) (app.selection.getEnd() * rate) };
}

void Application::cut()
{
    if (selection.getLength() <= 0) return;
    copy();
    del();
}

void Application::copy()
{
    if (selection.getLength() <= 0) return;
    audioEngine.copySelection (selectionSamples (*this));
}

void Application::paste()
{
    if (!audioEngine.canPaste()) return;
    // Paste at the start of the selection, or at the cursor when nothing is
    // selected.
    const double t = !selection.isEmpty() ? selection.getStart() : playhead;
    audioEngine.pasteAt ((int64_t) (t * audioEngine.sampleRate()));
}

void Application::del()
{
    if (selection.getLength() <= 0) return;
    audioEngine.deleteSamples (selectionSamples (*this));
    selection = { 0, 0 };
}

// Effects apply only to the currently selected (visible) channels.
static const std::vector<int>& visibleChans (const Application& app)
{
    return app.visibleChannels;
}

void Application::silence()
{
    if (selection.getLength() <= 0) return;
    audioEngine.silenceSamples (selectionSamples (*this), &visibleChans (*this));
}

void Application::fadeIn()
{
    if (selection.getLength() <= 0) return;
    audioEngine.fadeInSamples (selectionSamples (*this), &visibleChans (*this));
}

void Application::fadeOut()
{
    if (selection.getLength() <= 0) return;
    audioEngine.fadeOutSamples (selectionSamples (*this), &visibleChans (*this));
}

void Application::normalize()
{
    if (selection.getLength() <= 0) return;
    audioEngine.normalizeSamples (selectionSamples (*this), 1.0f, &visibleChans (*this));
}

void Application::reverse()
{
    if (selection.getLength() <= 0) return;
    audioEngine.reverseSamples (selectionSamples (*this), &visibleChans (*this));
}

void Application::changeGain (float gainDb)
{
    if (selection.getLength() <= 0 || gainDb == 0.0f) return;
    audioEngine.gainSamples (selectionSamples (*this), gainDb, &visibleChans (*this));
}

void Application::undoStep()
{
    Patch p;
    if (undo.popUndo (p))
        audioEngine.applyPatch (p, false);
}

void Application::redoStep()
{
    Patch p;
    if (undo.popRedo (p))
        audioEngine.applyPatch (p, true);
}

void Application::play()
{
    if (!audioEngine.isFileLoaded()) return;

    // Self-heal: if no output device is running (initialisation can fail at
    // startup when the default device isn't available yet), try again now.
    if (audioDevice->getCurrentAudioDevice() == nullptr)
        audioDevice->initialiseWithDefaultDevices (0, 2);

    // Restart from the beginning when playback is at (or past) the end.
    if (audioEngine.position() >= audioEngine.duration() - 1e-6)
        setPlayhead (0.0);
    audioEngine.play();
}
void Application::stop() { audioEngine.stop(); }
void Application::pause() { audioEngine.pause(); }

void Application::toggleLoop()
{
    if (!audioEngine.isFileLoaded()) return;
    if (audioEngine.isLooping())
    {
        audioEngine.setLooping (false);
        return;
    }
    // Loop the current selection, or the whole file when nothing is selected.
    juce::Range<double> r = selection;
    if (r.getLength() <= 1e-6)
        r = { 0.0, audioEngine.duration() };
    audioEngine.setLoopRange (r);
    audioEngine.setLooping (true);
}

void Application::setPlayhead (double t)
{
    playhead = std::clamp (t, 0.0, audioEngine.duration());
    audioEngine.setPosition (playhead);

    // Notify the UI so both views show the same cursor position.
    if (onPlayheadChanged)
        onPlayheadChanged (playhead);
}
