// SpectroCraft Mobile - the signal generator panel: manual tone, sweep, and
// white / pink / brown / custom noise, playable while recording.
#pragma once
#include <JuceHeader.h>
#include "AppContext.h"
#include "SignalSource.h"

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
    void setupSlider (juce::Slider& s, double lo, double hi, double mid, const juce::String& suffix, int decimals);
    juce::Component* addRow (const juce::String& name, juce::Component& c);

    AppContext& ctx;
    GeneratorSettings settings;
    juce::TextButton onButton { "Generator off" };
    juce::ComboBox kindBox, noiseBox;
    juce::Slider level, tone, sweepStart, sweepEnd, sweepTime, slope, lowCut, highCut;
    juce::ToggleButton sweepLog { "Log sweep" }, sweepRepeat { "Repeat" };
    juce::Label info;
    juce::OwnedArray<juce::Label> labels;
    std::vector<std::pair<juce::Label*, juce::Component*>> rows;
    juce::Viewport viewport;
    juce::Component content;
};
} // namespace scm
