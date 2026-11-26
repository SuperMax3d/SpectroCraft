// SpectroCraft Mobile - amplitude calibration panel (see Calibration.h):
// the current input's calibration, a live level meter in dBFS / dB-SPL, the
// acoustic calibrator procedure and typed values, and the comparison with a
// reference microphone that compensates this one (issue #80, MicCompensation.h).
#pragma once
#include <JuceHeader.h>
#include "AppContext.h"
#include "Calibration.h"
#include "MicCompensation.h"
#include "SignalSource.h"
#include "TouchControls.h"

namespace scm
{
/** The compensation's correction per 1/3 octave (dB), on a log frequency
    axis; the bands that were not trusted are drawn hollow. */
class CompensationPlot : public juce::Component
{
public:
    void setCompensation (const miccomp::Compensation& c) { comp = c; repaint(); }
    void paint (juce::Graphics&) override;
private:
    miccomp::Compensation comp;
};

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

    // comparison with a reference microphone (issue #80)
    void importResponse();
    void startCompareTake (bool reference);
    void finishCompareTake();
    void loadCompensationFile();
    /** Plays the app's pink noise during a take (when asked), and stops it. */
    void playPinkNoise (bool on);
    /** Reads the text of a file chosen in the system's picker. */
    static juce::String readChosen (const juce::FileChooser&, juce::String& name);

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

    enum class Shot { None, Calibrator, Reference, Microphone };
    Shot shotFor = Shot::None;   // what the running capture is for
    juce::Label compareTitle { {}, "Compensate a microphone against a reference" }, compareHelp, responseLabel,
                compareState, compLabel;
    juce::TextButton importResponseButton { "Reference response..." }, flatResponseButton { "Flat" };
    juce::ToggleButton pinkNoise { "Play pink noise during the takes" };
    juce::TextButton referenceButton { "1. Record the reference (10 s)" },
                     micButton { "2. Record this microphone (10 s)" };
    juce::TextButton loadCompButton { "Load a compensation file..." }, removeCompButton { "Remove" };
    CompensationPlot plot;
    miccomp::Response refResponse;     // the reference microphone's response (kept in the settings)
    miccomp::Bands refBands;           // the reference take
    double refFullScaleDbSpl = 0.0;
    juce::String refInput;
    std::unique_ptr<juce::FileChooser> chooser;
    GeneratorSettings generatorBefore;
    bool generatorWasOn = false, noisePlaying = false;
    juce::Viewport viewport;
    juce::Component content;
};
} // namespace scm
