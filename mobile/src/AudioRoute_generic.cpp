// Android / desktop: inputs are AudioDeviceManager devices (see AudioRoute.h).
#include "AudioRoute.h"

namespace scm::route
{
bool platformHasRoutes() { return false; }
std::vector<Input> availableInputs() { return {}; }
juce::String currentInputId() { return {}; }
bool selectInput (const juce::String&) { return false; }

juce::String bluetoothNote()
{
    return "Bluetooth headset microphones record at 8 or 16 kHz: the spectrum stops at 4 or 8 kHz. "
           "Use the built-in or a USB microphone for measurements.";
}
} // namespace scm::route
