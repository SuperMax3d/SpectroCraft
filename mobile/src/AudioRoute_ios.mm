// iOS: the microphone is an AVAudioSession input route (see AudioRoute.h).
#include "AudioRoute.h"
#import <AVFoundation/AVFoundation.h>

namespace scm::route
{
namespace
{
juce::String kindOf (NSString* portType)
{
    if ([portType isEqualToString: AVAudioSessionPortBuiltInMic])      return "Built-in";
    if ([portType isEqualToString: AVAudioSessionPortUSBAudio])        return "USB";
    if ([portType isEqualToString: AVAudioSessionPortBluetoothHFP])    return "Bluetooth";
    if ([portType isEqualToString: AVAudioSessionPortHeadsetMic])      return "Wired";
    if ([portType isEqualToString: AVAudioSessionPortLineIn])          return "Line in";
    return "Other";
}

juce::String str (NSString* s) { return s != nil ? juce::String::fromUTF8 ([s UTF8String]) : juce::String(); }
} // namespace

bool platformHasRoutes() { return true; }

std::vector<Input> availableInputs()
{
    std::vector<Input> out;
    for (AVAudioSessionPortDescription* p in [[AVAudioSession sharedInstance] availableInputs])
        out.push_back ({ str (p.UID), str (p.portName), kindOf (p.portType) });
    return out;
}

juce::String currentInputId()
{
    auto* route = [[AVAudioSession sharedInstance] currentRoute];
    for (AVAudioSessionPortDescription* p in route.inputs)
        return str (p.UID);
    return {};
}

bool selectInput (const juce::String& id)
{
    auto* session = [AVAudioSession sharedInstance];
    for (AVAudioSessionPortDescription* p in [session availableInputs])
        if (str (p.UID) == id)
        {
            NSError* err = nil;
            const bool ok = [session setPreferredInput: p error: &err];
            return ok && err == nil;
        }
    return false;
}

juce::String bluetoothNote()
{
    return "Bluetooth headset microphones record at 8 or 16 kHz: the spectrum stops at 4 or 8 kHz. "
           "Use the built-in or a USB microphone for measurements.";
}
} // namespace scm::route
