// SpectroCraft Mobile - choosing the microphone.
//
// iOS has one audio "device"; the microphone is a route of the audio session
// (built-in, wired headset, USB, Bluetooth HFP), picked with
// AVAudioSession setPreferredInput: AudioRoute_ios.mm. Android and the desktop
// list every input as a device of JUCE's AudioDeviceManager instead
// (AudioRoute_generic.cpp returns no routes and the Input pane lists the
// devices).
//
// Bluetooth headset microphones (HFP) are limited by the profile to 8 or
// 16 kHz sampling: the spectrum stops at 4 or 8 kHz. USB microphones keep the
// full band.
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace scm::route
{
struct Input
{
    juce::String id;     // the port's UID (iOS)
    juce::String name;   // shown to the user
    juce::String kind;   // "Built-in", "USB", "Bluetooth", "Wired", "Other"
};

/** True where the microphone is chosen through routes (iOS). */
bool platformHasRoutes();
std::vector<Input> availableInputs();
juce::String currentInputId();
/** Prefers this input; false when the system refused it. */
bool selectInput (const juce::String& id);
/** The input to use by default whenever it is plugged in: Shure's MVX2U (XLR to
    USB), found by name ("MVX2U", "Shure MVX2U", "MVX2U Gen 2"...); -1 when it
    is not in `names`. */
inline int preferredInputIndex (const juce::StringArray& names)
{
    for (int i = 0; i < names.size(); ++i)
        if (names[i].toUpperCase().retainCharacters ("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789").contains ("MVX2U"))
            return i;
    return -1;
}

/** Note shown next to a Bluetooth input. */
juce::String bluetoothNote();

/** iOS: true when the audio session is not in measurement mode yet. Setting
    the mode is a synchronous call to the audio server that can block the main
    thread for seconds, so it is made only when needed. Elsewhere: true. */
bool measurementModeNeeded();

/** Where the sound plays (speaker, AirPods, AirPlay...), e.g. "AirPods Pro". */
juce::String currentOutputName();
/** iOS: the system's output picker (AVRoutePickerView: speaker, AirPods,
    AirPlay). nullptr where outputs are AudioDeviceManager devices. */
std::unique_ptr<juce::Component> createOutputPicker();
} // namespace scm::route
