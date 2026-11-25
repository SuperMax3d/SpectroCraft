// SpectroCraft Mobile - choosing the microphone (built-in, USB, Bluetooth),
// mono / stereo and the sample rate.
#pragma once
#include <JuceHeader.h>
#include "AppContext.h"

namespace scm
{
class InputPane : public juce::Component
{
public:
    explicit InputPane (AppContext&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();
    std::function<void()> onInputChanged;

private:
    void choose (int index);

    AppContext& ctx;
    juce::Label title { {}, "Microphone" }, status, note;
    juce::OwnedArray<juce::TextButton> choices;
    juce::StringArray ids;
    juce::ComboBox channelsBox, rateBox;
    juce::Label channelsLabel { {}, "Channels" }, rateLabel { {}, "Sample rate" };
};
} // namespace scm
