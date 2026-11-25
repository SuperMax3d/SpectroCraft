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
/** Note shown next to a Bluetooth input. */
juce::String bluetoothNote();

/** Where the sound plays (speaker, AirPods, AirPlay...), e.g. "AirPods Pro". */
juce::String currentOutputName();
/** iOS: the system's output picker (AVRoutePickerView: speaker, AirPods,
    AirPlay). nullptr where outputs are AudioDeviceManager devices. */
std::unique_ptr<juce::Component> createOutputPicker();
} // namespace scm::route
