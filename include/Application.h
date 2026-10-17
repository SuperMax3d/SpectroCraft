#pragma once
// SpectroCraft - Application state (owns engines, views, markers, undo)
#include <JuceHeader.h>
#include "Settings.h"
#include "AudioEngine.h"
#include "SpectralEngine.h"
#include "MarkerManager.h"
#include "UndoManager.h"
#include <vector>
#include <functional>

class Application
{
public:
    Application();
    ~Application();

    AudioEngine audioEngine;
    SpectralEngine spectralEngine;
    MarkerManager markers;
    ::UndoManager undo;

    // Real-time output: a player callback streaming from the engine, attached
    // to an AudioDeviceManager. Declared after audioEngine (destroyed before
    // it) and in this order so the manager stops the device before the player
    // is destroyed.
    juce::AudioSourcePlayer sourcePlayer;
    std::unique_ptr<juce::AudioDeviceManager> audioDevice;

    Settings settings;
    ColourMap colourMap = ColourMap::Classic;

    juce::Range<double> selection{ 0, 0 };
    double viewStart = 0, viewEnd = 10;   // canonical time window (follows the waveform view)

    // UI cursor position in seconds (independent of transport: stays where you
    // clicked it; tracks playback while playing).
    double playhead = 0.0;

    bool openFile (const juce::File& file);
    void closeFile();
    void selectAll();
    void setSelection (juce::Range<double> s);
    void cut(); void copy(); void paste(); void del();
    void silence(); void fadeIn(); void fadeOut(); void normalize(); void reverse();
    // Multiply the selection by 10^(gainDb/20) on the visible channels.
    void changeGain (float gainDb);
    void undoStep(); void redoStep();

    void play(); void stop(); void pause();
    // Toggle looping. When enabling, the loop range is taken from the current
    // selection (or the whole file if there is no selection).
    void toggleLoop();
    void setPlayhead (double t);

    std::vector<int> visibleChannels;   // sorted list of visible channel indices (any subset)

    bool isChannelVisible (int ch) const;
    void toggleChannelVisible (int ch);

    // Add a marker: region spanning the current selection if there is one,
    // otherwise a point marker at the cursor.
    int addMarkerAtCursor();

    // Set by MainWindow: called when panels change shared state (settings, markers,
    // channels) so the main views can be re-synced.
    std::function<void()> onStateChange;

    // Set by MainWindow: called whenever setPlayhead() moves the cursor, so both
    // views always show the same playhead position.
    std::function<void(double)> onPlayheadChanged;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Application)
};
