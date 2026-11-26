// SpectroCraft Mobile - amplitude calibration panel (see Calibration.h):
// the current input's calibration, a live level meter in dBFS / dB-SPL, the
// acoustic calibrator procedure and typed values.
#pragma once
#include <JuceHeader.h>
#include "AppContext.h"
#include "Calibration.h"
#include "TouchControls.h"

namespace scm
{
class CalibrationPane : public juce::Component, private juce::Timer
{
public:
    explicit CalibrationPane (AppContext&);
    void paint (juce::Graphics&) override;
    void resized() override;
    /** The input changed: show its calibration. */
    void refresh();

private:
    void timerCallback() override;
    void startCalibrator();
    void finishCalibrator();
    void applyTyped();
    void store (const cal::InputCalibration&);

    AppContext& ctx;
    juce::Label inputLabel, stateLabel, meterLabel, resultLabel;
    juce::Label refLabel { {}, "Calibrator level (dB-SPL)" }, freqLabel { {}, "Calibrator frequency (Hz)" };
    NumberField refLevel { "Calibrator level", 40.0, 160.0, 1, " dB" },
                refFreq { "Calibrator frequency", 20.0, 20000.0, 0, " Hz" };
    juce::TextButton calibrateButton { "Calibrate with the calibrator (3 s)" };
    juce::Label typedLabel { {}, "Full scale = (dB-SPL)" };
    NumberField typedValue { "Full scale", 60.0, 200.0, 2, " dB" };
    juce::TextButton typedButton { "Set" };
    juce::TextButton clearButton { "Clear calibration" };
    juce::TextButton applyFileButton { "Apply to the open file" };
    cal::InputCalibration currentCal;
    float meterDb = -200.0f;
    juce::Viewport viewport;
    juce::Component content;
};
} // namespace scm
