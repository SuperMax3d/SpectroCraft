#pragma once
// SpectroCraft - Spectral view component (renders engine tiles + overlays)
#include <JuceHeader.h>
#include "Application.h"
#include "SpectralEngine.h"
#include "Settings.h"
#include "MarkerManager.h"
#include <vector>
#include <functional>

class SpectralView : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    explicit SpectralView (Application& app);
    ~SpectralView();

    void paint (juce::Graphics& g) override;
    void resized() override;

    // Mouse interaction: left-drag = select, middle/right-drag = pan,
    // wheel over the plot = time zoom at cursor, wheel over the left
    // frequency axis (or Shift+wheel) = frequency-range zoom,
    // double-click = fit whole file. Clicking moves the playhead.
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;   // status-bar probe
    void mouseExit (const juce::MouseEvent&) override;     // clears the probe
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

    // Called by MainWindow after shared state changes
    void setViewRange (double t0, double t1) { setWindow (t0, t1); }
    void setVisibleChannels (const std::vector<int>& ch)
    {
        if (ch != channels) { channels = ch; imageDirty = true; repaint(); }
    }
    void setSelection (juce::Range<double> s)
    {
        if (s != selection) { selection = s; repaint(); }
    }
    void setSpectralSelection (juce::Rectangle<float> r)
    {
        if (r != spectralSel) { spectralSel = r; repaint(); }
    }
    void setPlayhead (double t)
    {
        if (t != playhead) { playhead = t; repaint(); }
    }
    double getPlayhead() const { return playhead; }
    void setMarkers (const std::vector<Marker>* m)
    {
        if (m != markerList) { markerList = m; repaint(); }
    }
    void setSettings (const Settings& s)
    {
        if (s != settings_) { settings_ = s; imageDirty = true; repaint(); }
    }

    // Force a full spectral re-render (e.g. after the colour map changed,
    // which is not part of Settings).
    void invalidateImage() { imageDirty = true; repaint(); }

    // Keyboard-shortcut helpers (applied by MainWindow to both views).
    void zoomHorizontalAtCenter (double factor);   // <1 zoom in, >1 zoom out
    void zoomFrequencyBy (double factor);          // vertical zoom of the freq axis
    void fitAll();

    // Reset the frequency axis to the settings defaults (20 Hz .. 20 kHz clamped to Nyquist).
    void resetFrequencyRange();

    // Current local frequency-axis limits (for the vertical scrollbar).
    double getFreqMin() const { return freqMin; }
    double getFreqMax() const { return freqMax; }
    // Set the frequency axis directly (called by the vertical scrollbar).
    void setFrequencyRange (double lo, double hi);

    // Render the current spectral content (no axes/overlays) to a PNG file.
    bool saveAsPng (const juce::File& file);

    // Notify MainWindow that the time window changed: (newStart, newEnd).
    // MainWindow applies it to BOTH views so they always show the same range.
    std::function<void(double t0, double t1)> onChange;
    // Notified when the local frequency range changes (wheel / keys / reset).
    std::function<void(double lo, double hi)> onFrequencyRangeChanged;
    // Right-click context menu action selected (ContextMenuIDs); handled by MainWindow.
    std::function<void(int)> onContextAction;
    // Status-bar probe: cursor time + frequency + level under the mouse
    // (empty string clears it).
    std::function<void(const juce::String&)> onCursorInfo;

    double getViewStart() const { return viewStart; }
    double getViewEnd() const { return viewEnd; }
    double xToTime (int x) const;
    double timeToX (double t) const;
    float freqToY (double freq, int height) const;
    double yToFreq (float y, int height) const;

public:
    // Plot area (starts at kLeftAxisWidth so it aligns with the waveform view).
    juce::Rectangle<int> plotRect() const;

private:
    Application& app;   // audio data read live via app.audioEngine() (active doc)
    SpectralEngine& spectral;
    double viewStart = 0, viewEnd = 10;
    double freqMin = 20.0, freqMax = 20000.0;   // local frequency axis limits
    std::vector<int> channels;
    juce::Range<double> selection{ 0, 0 };
    juce::Rectangle<float> spectralSel;
    double playhead = 0;
    const std::vector<Marker>* markerList = nullptr;
    bool dirty = true;
    bool imageDirty = true; // spectral image needs recomputation (vs overlay-only repaint)

    Settings settings_;

    // Interaction state
    enum class DragMode { None, Select, Pan };
    DragMode dragMode = DragMode::None;
    double dragStartTime = 0.0;

    // Cached rendered image
    juce::Image cachedImage;
    bool haveImage = false;
    int imgW = 0, imgH = 0;
    double imgT0 = -1, imgT1 = -1;

    // Single path for all time-window changes: updates state and notifies
    // MainWindow with the new range.
    void setWindow (double t0, double t1);

    // Renders the spectral content (one row per visible channel with dark
    // gaps) into an ARGB image of size w×h. Used by paint() and saveAsPng().
    void renderSpectralContent (juce::Image& img, const Settings& rs);

    void drawFrequencyRuler (juce::Graphics& g);
    void drawOverlays (juce::Graphics& g);
    void zoomAtX (int x, double factor);
    void panByTime (double dt);
    void zoomFreqAtY (float y, double factor);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectralView)
};
