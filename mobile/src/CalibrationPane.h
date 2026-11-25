// SpectroCraft Mobile - amplitude calibration panel (see Calibration.h):
// the current input's calibration, a live level meter in dBFS / dB-SPL, the
// acoustic calibrator procedure and typed values.
#pragma once
#include <JuceHeader.h>
#include "AppContext.h"
#include "Calibration.h"

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
    juce::TextEditor refLevel, refFreq;
    juce::TextButton calibrateButton { "Calibrate with the calibrator (3 s)" };
    juce::Label typedLabel { {}, "Full scale = (dB-SPL)" };
    juce::TextEditor typedValue;
    juce::TextButton typedButton { "Set" };
    juce::TextButton clearButton { "Clear calibration" };
    juce::TextButton applyFileButton { "Apply to the open file" };
    cal::InputCalibration currentCal;
    float meterDb = -200.0f;
    juce::Viewport viewport;
    juce::Component content;
};
} // namespace scm
