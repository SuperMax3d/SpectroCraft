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
    void choose (int index, bool byUser = true);

    AppContext& ctx;
    juce::Label title { {}, "Microphone" }, status, note;
    juce::OwnedArray<juce::TextButton> choices;
    juce::StringArray ids;
    // The MVX2U is chosen when it shows up (at launch or plugged in), once:
    // another input picked while it stays plugged in is kept.
    bool adapterSeen = false;
    juce::ComboBox channelsBox, rateBox;
    // Where the sound plays (generator, playback): iOS shows the system's
    // picker (speaker, AirPods, AirPlay); elsewhere a list of output devices.
    juce::Label outputLabel { {}, "Listen on" }, outputName;
    std::unique_ptr<juce::Component> outputPicker;
    juce::ComboBox outputBox;
    juce::Label channelsLabel { {}, "Channels" }, rateLabel { {}, "Sample rate" };
};
} // namespace scm
