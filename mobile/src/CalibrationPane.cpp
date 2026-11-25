#include "CalibrationPane.h"
#include "LiveAudio.h"
#include "MobileDocument.h"
#include "MobileStyle.h"

namespace scm
{
CalibrationPane::CalibrationPane (AppContext& c) : ctx (c)
{
    for (auto* l : { &inputLabel, &stateLabel, &meterLabel, &resultLabel, &refLabel, &freqLabel, &typedLabel })
    {
        l->setFont (style::font (15.0f));
        content.addAndMakeVisible (l);
    }
    inputLabel.setFont (style::font (16.0f, true));
    meterLabel.setFont (style::font (22.0f, true));
    resultLabel.setColour (juce::Label::textColourId, style::textDim);
    resultLabel.setJustificationType (juce::Justification::topLeft);

    for (auto* e : { &refLevel, &refFreq, &typedValue })
    {
        e->setInputRestrictions (8, "0123456789.-");
        e->setFont (style::font (16.0f));
        e->setJustification (juce::Justification::centredLeft);
        content.addAndMakeVisible (e);
    }
    refLevel.setText (ctx.props.getValue ("calibratorDb", "94.0"));
    refFreq.setText (ctx.props.getValue ("calibratorHz", "1000"));

    calibrateButton.onClick = [this] { startCalibrator(); };
    typedButton.onClick = [this] { applyTyped(); };
    clearButton.onClick = [this]
    {
        ctx.calibrations.clear (ctx.inputKey());
        refresh();
    };
    applyFileButton.onClick = [this]
    {
        if (! ctx.document.hasAudio()) return;
        ctx.document.setCalibration (currentCal.valid() ? currentCal.toScale() : ScaleCalibration {});
        resultLabel.setText ("Calibration applied to " + ctx.document.displayName()
                             + ". Save the file to keep it.", juce::dontSendNotification);
    };
    for (auto* b : { &calibrateButton, &typedButton, &clearButton, &applyFileButton })
        content.addAndMakeVisible (b);

    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);
    addAndMakeVisible (viewport);
    refresh();
    startTimerHz (10);
}

void CalibrationPane::refresh()
{
    currentCal = ctx.currentInputCalibration();
    inputLabel.setText ("Input: " + ctx.inputKey(), juce::dontSendNotification);
    if (currentCal.valid())
        stateLabel.setText ("Full scale = " + juce::String (cal::fullScaleDbSpl (currentCal.fullScalePa), 2)
                            + " dB-SPL (" + juce::String (currentCal.fullScalePa, 3) + " Pa), "
                            + currentCal.method + ", " + currentCal.when.formatted ("%Y-%m-%d"),
                            juce::dontSendNotification);
    else
        stateLabel.setText ("Not calibrated: levels in dBFS.", juce::dontSendNotification);
    typedValue.setText (currentCal.valid() ? juce::String (cal::fullScaleDbSpl (currentCal.fullScalePa), 2)
                                           : juce::String());
    repaint();
}

void CalibrationPane::store (const cal::InputCalibration& c)
{
    ctx.calibrations.set (c);
    refresh();
}

void CalibrationPane::startCalibrator()
{
    if (! ctx.audio.isOpen())
    {
        showMessage ("Calibration", "The microphone is not available.");
        return;
    }
    ctx.props.setValue ("calibratorDb", refLevel.getText());
    ctx.props.setValue ("calibratorHz", refFreq.getText());
    ctx.audio.startShot (3.0);
    calibrateButton.setEnabled (false);
    resultLabel.setText ("Measuring... keep the calibrator on the microphone.", juce::dontSendNotification);
}

void CalibrationPane::finishCalibrator()
{
    calibrateButton.setEnabled (true);
    const auto shot = ctx.audio.takeShot();
    const double refDb = refLevel.getText().getDoubleValue();
    const double hz = refFreq.getText().getDoubleValue();
    if (refDb < 40.0 || refDb > 160.0 || hz < 20.0 || hz > 20000.0)
    {
        resultLabel.setText ("Check the calibrator level (40-160 dB) and frequency (20-20000 Hz).",
                             juce::dontSendNotification);
        return;
    }
    const auto r = cal::analyseCalibrator (shot.data(), (int) shot.size(), ctx.audio.sampleRate(), hz, refDb);
    juce::String details = "Band level " + juce::String (20.0 * std::log10 (std::max (1e-12, r.bandRms)), 2)
                         + " dBFS at " + juce::String (r.peakHz, 1) + " Hz, steadiness "
                         + juce::String (r.spreadDb, 2) + " dB, tone "
                         + juce::String (100.0 * r.bandShare, 1) + " % of the power.";
    if (! r.ok)
    {
        resultLabel.setText ("Not calibrated: " + r.problem + "\n" + details, juce::dontSendNotification);
        return;
    }
    cal::InputCalibration c;
    c.input = ctx.inputKey();
    c.fullScalePa = r.fullScalePa;
    c.method = "calibrator " + juce::String (refDb, 1) + " dB @ " + juce::String (hz, 0) + " Hz";
    c.when = juce::Time::getCurrentTime();
    store (c);
    resultLabel.setText ("Calibrated. " + details + "\nNew recordings from this input are in dB-SPL. "
                         "Use \"Apply to the open file\" for the file on screen.", juce::dontSendNotification);
}

void CalibrationPane::applyTyped()
{
    const double db = typedValue.getText().getDoubleValue();
    if (db < 60.0 || db > 200.0)
    {
        resultLabel.setText ("Type the level of a full-scale signal, between 60 and 200 dB-SPL "
                             "(e.g. 120 for a microphone that clips at 120 dB).", juce::dontSendNotification);
        return;
    }
    cal::InputCalibration c;
    c.input = ctx.inputKey();
    c.fullScalePa = cal::fullScalePaFromDbSpl (db);
    c.method = "typed";
    c.when = juce::Time::getCurrentTime();
    store (c);
    resultLabel.setText ("Calibration set.", juce::dontSendNotification);
}

void CalibrationPane::timerCallback()
{
    if (ctx.audio.shotRunning())
    {
        resultLabel.setText ("Measuring... " + juce::String (juce::roundToInt (100.0 * ctx.audio.shotProgress())) + " %",
                             juce::dontSendNotification);
    }
    else if (ctx.audio.shotReady())
        finishCalibrator();

    meterDb = ctx.audio.inputRmsDb (0);
    juce::String text = meterDb > -150.0f ? juce::String (meterDb, 1) + " dBFS" : juce::String ("-- dBFS");
    if (currentCal.valid() && meterDb > -150.0f)
        text << "    " << juce::String (meterDb + cal::fullScaleDbSpl (currentCal.fullScalePa), 1) << " dB-SPL";
    meterLabel.setText (text, juce::dontSendNotification);
}

void CalibrationPane::paint (juce::Graphics& g)
{
    g.fillAll (style::panelBackground);
}

void CalibrationPane::resized()
{
    viewport.setBounds (getLocalBounds());
    const int w = getWidth() - 12, x = style::kGap;
    int y = style::kGap;
    const auto row = [&] (juce::Component& c, int h) { c.setBounds (x, y, w, h); y += h + 4; };
    row (inputLabel, 24);
    row (stateLabel, 40);
    row (meterLabel, 34);
    y += 6;
    refLabel.setBounds (x, y, w - 110, style::kTouch); refLevel.setBounds (x + w - 100, y + 4, 100, 36); y += style::kTouch;
    freqLabel.setBounds (x, y, w - 110, style::kTouch); refFreq.setBounds (x + w - 100, y + 4, 100, 36); y += style::kTouch + 4;
    row (calibrateButton, style::kTouch);
    y += 8;
    typedLabel.setBounds (x, y, w - 180, style::kTouch);
    typedValue.setBounds (x + w - 170, y + 4, 100, 36);
    typedButton.setBounds (x + w - 64, y, 64, style::kTouch);
    y += style::kTouch + 4;
    clearButton.setBounds (x, y, w / 2 - 3, style::kTouch);
    applyFileButton.setBounds (x + w / 2 + 3, y, w / 2 - 3, style::kTouch);
    y += style::kTouch + 6;
    row (resultLabel, 80);
    content.setSize (getWidth() - 8, y);
}
} // namespace scm
