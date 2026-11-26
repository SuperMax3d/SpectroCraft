// SpectroCraft Mobile - the signal generator panel: manual tone, sweep, and
// white / pink / brown / custom noise, playable while recording.
#pragma once
#include <JuceHeader.h>
#include "AppContext.h"
#include "SignalSource.h"
#include "NoiseCurveEditor.h"
#include "TouchControls.h"

namespace scm
{
class GeneratorPane : public juce::Component, private juce::Timer
{
public:
    explicit GeneratorPane (AppContext&);
    ~GeneratorPane() override;
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    void push();
    void refreshVisibility();
    /** The frequency sliders' scale: log (an option, issue #79) or skewed
        about 1 kHz; up to the device's Nyquist frequency. */
    void applyFrequencyScale();
    void setupSlider (TouchSlider& s, const juce::String& title, double lo, double hi, double mid,
                      const juce::String& suffix, int decimals);
    juce::Component* addRow (const juce::String& name, juce::Component& c);

    AppContext& ctx;
    GeneratorSettings settings;
    juce::TextButton onButton { "Generator off" };
    juce::ComboBox kindBox, noiseBox;
    TouchSlider level, tone, sweepStart, sweepEnd, sweepTime, lowCut, highCut;
    NoiseCurveEditor curveEditor;   // Custom noise: the drawn response (issue #79)
    juce::ToggleButton logSliders { "Log scale for frequencies" };
    double scaleNyquist = 0.0;      // the Nyquist frequency the sliders were set up for
    juce::ToggleButton sweepLog { "Log sweep" }, sweepRepeat { "Repeat" };
    juce::Label info;
    juce::OwnedArray<juce::Label> labels;
    std::vector<std::pair<juce::Label*, juce::Component*>> rows;
    juce::Viewport viewport;
    juce::Component content;
};
} // namespace scm
