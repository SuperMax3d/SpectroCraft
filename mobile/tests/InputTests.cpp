// SpectroCraft Mobile - unit tests for choosing the input
// (mobile/src/AudioRoute.h).
#include <JuceHeader.h>
#include "AudioRoute.h"

namespace
{
class InputTests : public juce::UnitTest
{
public:
    InputTests() : juce::UnitTest ("input choice", "SpectroCraftMobile") {}

    void runTest() override
    {
        using scm::route::preferredInputIndex;

        beginTest ("the MVX2U is found by name, however it is written");
        {
            expectEquals (preferredInputIndex ({ "iPhone Microphone  (Built-in)", "MVX2U  (USB)" }), 1);
            expectEquals (preferredInputIndex ({ "Shure MVX2U Gen 2  (USB)", "Microphone" }), 0);
            expectEquals (preferredInputIndex ({ "Built-in mic", "mvx-2u" }), 1);
            expectEquals (preferredInputIndex ({ "MV7+  (USB)", "AirPods Pro  (Bluetooth)" }), -1);
            expectEquals (preferredInputIndex ({}), -1);
        }
    }
};

static InputTests inputTests;
} // namespace
