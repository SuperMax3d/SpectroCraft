// SpectroCraft Mobile - the objects every screen shares.
#pragma once
#include <JuceHeader.h>
#include "Calibration.h"

namespace scm
{
class LiveAudio;
class MobileDocument;
class Recorder;
class TimeViewport;
class VideoCapture;

struct AppContext
{
    LiveAudio& audio;
    MobileDocument& document;
    Recorder& recorder;
    TimeViewport& view;
    VideoCapture& video;
    juce::PropertiesFile& props;
    cal::Store calibrations;

    /** The name calibrations are stored under for the current input. */
    juce::String inputKey() const;
    /** Calibration of the current input ({} when none). */
    cal::InputCalibration currentInputCalibration() const;
};

/** A message box that does not block (iOS / Android have no modal loops). */
void showMessage (const juce::String& title, const juce::String& text);
} // namespace scm
