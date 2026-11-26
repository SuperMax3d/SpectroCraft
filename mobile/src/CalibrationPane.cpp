#include "CalibrationPane.h"
#include "LiveAudio.h"
#include "MobileDocument.h"
#include "MobileStyle.h"
#include "Recorder.h"
#include "Telemetry.h"

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

    // numbers come from the app's number pad, not the system keyboard (TouchControls.h)
    for (auto* e : { &refLevel, &refFreq, &typedValue })
        content.addAndMakeVisible (e);
    refLevel.setValue (ctx.props.getValue ("calibratorDb", "94.0").getDoubleValue());
    refFreq.setValue (ctx.props.getValue ("calibratorHz", "1000").getDoubleValue());

    calibrateButton.onClick = [this] { startCalibrator(); };
    typedButton.onClick = [this] { applyTyped(); };
    clearButton.onClick = [this]
    {
        ctx.calibrations.clear (ctx.inputKey());
        telemetry::capture ("calibration_cleared", {});
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

    // ---- comparison with a reference microphone (issue #80)
    compareTitle.setFont (style::font (16.0f, true));
    compareHelp.setFont (style::font (14.0f));
    compareHelp.setColour (juce::Label::textColourId, style::textDim);
    compareHelp.setJustificationType (juce::Justification::topLeft);
    compareHelp.setText ("Measures this microphone against a calibrated reference microphone: the same steady sound, "
                         "the phone at the same place. 1: choose the reference in Input (calibrate it above first) "
                         "and record it. 2: choose the microphone to compensate in Input and record it. Its calibration "
                         "and its correction per 1/3 octave are saved as a CSV file in the Calibrations folder; "
                         "the analysis of its recordings applies them.", juce::dontSendNotification);
    for (auto* l : { &compareState, &compLabel, &responseLabel })
    {
        l->setFont (style::font (14.0f));
        l->setJustificationType (juce::Justification::topLeft);
    }
    compareState.setColour (juce::Label::textColourId, style::textDim);
    {
        const auto saved = juce::JSON::parse (ctx.props.getValue ("referenceResponse"));
        if (const auto* h = saved.getProperty ("hz", {}).getArray(); h != nullptr)
            if (const auto* d = saved.getProperty ("db", {}).getArray(); d != nullptr && d->size() == h->size())
            {
                for (int i = 0; i < h->size(); ++i)
                {
                    refResponse.hz.push_back ((double) (*h)[i]);
                    refResponse.db.push_back ((double) (*d)[i]);
                }
                refResponse.name = saved.getProperty ("name", "").toString();
            }
    }
    importResponseButton.onClick = [this] { importResponse(); };
    flatResponseButton.onClick = [this]
    {
        refResponse = {};
        ctx.props.removeValue ("referenceResponse");
        refresh();
    };
    pinkNoise.setToggleState (ctx.props.getBoolValue ("comparePinkNoise", true), juce::dontSendNotification);
    pinkNoise.onClick = [this] { ctx.props.setValue ("comparePinkNoise", pinkNoise.getToggleState()); };
    referenceButton.onClick = [this] { startCompareTake (true); };
    micButton.onClick = [this] { startCompareTake (false); };
    loadCompButton.onClick = [this] { loadCompensationFile(); };
    removeCompButton.onClick = [this]
    {
        miccomp::fileFor (ctx.inputKey()).deleteFile();
        refresh();
    };
    for (auto* comp : std::initializer_list<juce::Component*> { &compareTitle, &compareHelp, &responseLabel, &importResponseButton,
                                                                &flatResponseButton, &pinkNoise, &referenceButton, &micButton,
                                                                &compareState, &plot, &compLabel, &loadCompButton, &removeCompButton })
        content.addAndMakeVisible (comp);

    viewport.setViewedComponent (&content, false);
    setupScrolling (viewport);
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
    typedValue.setValue (currentCal.valid() ? std::optional<double> (cal::fullScaleDbSpl (currentCal.fullScalePa))
                                            : std::nullopt);

    responseLabel.setText ("Reference response: " + (refResponse.empty() ? juce::String ("flat")
                               : refResponse.name + " (" + juce::String ((int) refResponse.hz.size()) + " points)"),
                           juce::dontSendNotification);
    const auto comp = miccomp::forInput (ctx.inputKey());
    plot.setCompensation (comp);
    plot.setVisible (comp.valid());
    removeCompButton.setEnabled (comp.valid());
    compLabel.setText (comp.valid() ? "Compensation of this input: " + miccomp::fileFor (ctx.inputKey()).getFileName()
                                          + ", against " + comp.reference + ", " + comp.when.formatted ("%Y-%m-%d")
                                    : juce::String ("No compensation for this input."),
                       juce::dontSendNotification);
    const bool busy = ctx.audio.shotRunning();
    referenceButton.setEnabled (! busy);
    micButton.setEnabled (! busy && ! refBands.hz.empty());
    resized();
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
    ctx.props.setValue ("calibratorDb", juce::String (refLevel.getValue().value_or (94.0)));
    ctx.props.setValue ("calibratorHz", juce::String (refFreq.getValue().value_or (1000.0)));
    shotFor = Shot::Calibrator;
    ctx.audio.startShot (3.0);
    calibrateButton.setEnabled (false);
    resultLabel.setText ("Measuring... keep the calibrator on the microphone.", juce::dontSendNotification);
}

void CalibrationPane::finishCalibrator()
{
    calibrateButton.setEnabled (true);
    const auto shot = ctx.audio.takeShot();
    const double refDb = refLevel.getValue().value_or (0.0);
    const double hz = refFreq.getValue().value_or (0.0);
    if (refDb < 40.0 || refDb > 160.0 || hz < 20.0 || hz > 20000.0)
    {
        resultLabel.setText ("Check the calibrator level (40-160 dB) and frequency (20-20000 Hz).",
                             juce::dontSendNotification);
        return;
    }
    const auto r = cal::analyseCalibrator (shot.data(), (int) shot.size(), ctx.audio.sampleRate(), hz, refDb);
    telemetry::capture ("calibration", { { "method", "calibrator" }, { "ok", r.ok }, { "result", r.code },
                                         { "ref_db", refDb }, { "ref_hz", hz } });
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
    const double db = typedValue.getValue().value_or (0.0);
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
    telemetry::capture ("calibration", { { "method", "typed" }, { "ok", true } });
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
    {
        if (shotFor == Shot::Reference || shotFor == Shot::Microphone) finishCompareTake();
        else finishCalibrator();
    }
    if (shotFor == Shot::Reference || shotFor == Shot::Microphone)
        if (ctx.audio.shotRunning())
            compareState.setText ("Recording " + juce::String (shotFor == Shot::Reference ? "the reference" : "this microphone")
                                  + "... " + juce::String (juce::roundToInt (100.0 * ctx.audio.shotProgress())) + " %",
                                  juce::dontSendNotification);

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
    const int w = getWidth() - kScrollBarW - style::kGap, x = style::kGap;
    int y = style::kGap;
    const auto row = [&] (juce::Component& c, int h) { c.setBounds (x, y, w, h); y += h + 4; };
    row (inputLabel, 24);
    row (stateLabel, 40);
    row (meterLabel, 34);
    y += 6;
    refLabel.setBounds (x, y, w - 118, style::kTouch); refLevel.setBounds (x + w - 110, y + 4, 110, 36); y += style::kTouch;
    freqLabel.setBounds (x, y, w - 118, style::kTouch); refFreq.setBounds (x + w - 110, y + 4, 110, 36); y += style::kTouch + 4;
    row (calibrateButton, style::kTouch);
    y += 8;
    typedLabel.setBounds (x, y, w - 188, style::kTouch);
    typedValue.setBounds (x + w - 180, y + 4, 110, 36);
    typedButton.setBounds (x + w - 64, y, 64, style::kTouch);
    y += style::kTouch + 4;
    clearButton.setBounds (x, y, w / 2 - 3, style::kTouch);
    applyFileButton.setBounds (x + w / 2 + 3, y, w / 2 - 3, style::kTouch);
    y += style::kTouch + 6;
    row (resultLabel, 80);

    // comparison with a reference microphone (issue #80)
    y += 10;
    row (compareTitle, 24);
    row (compareHelp, 112);
    {
        auto r = juce::Rectangle<int> (x, y, w, style::kTouch);
        flatResponseButton.setBounds (r.removeFromRight (72));
        r.removeFromRight (4);
        importResponseButton.setBounds (r.removeFromRight (std::min (190, r.getWidth() / 2)));
        responseLabel.setBounds (r.reduced (0, 4));
        y += style::kTouch + 4;
    }
    row (pinkNoise, 36);
    row (referenceButton, style::kTouch);
    row (micButton, style::kTouch);
    row (compareState, 64);
    if (plot.isVisible()) row (plot, 150);
    row (compLabel, 40);
    {
        auto r = juce::Rectangle<int> (x, y, w, style::kTouch);
        removeCompButton.setBounds (r.removeFromRight (96));
        r.removeFromRight (6);
        loadCompButton.setBounds (r);
        y += style::kTouch + 6;
    }
    content.setSize (getWidth() - kScrollBarW, y);
}
// ------------------------------------------------------------------ comparison (issue #80)

juce::String CalibrationPane::readChosen (const juce::FileChooser& fc, juce::String& name)
{
    const auto url = fc.getURLResult();
    if (url.isEmpty()) return {};
    name = url.getFileName();
    if (url.isLocalFile() && url.getLocalFile().existsAsFile())
        return url.getLocalFile().loadFileAsString();
    // Android (content://) and iOS security-scoped files
    auto in = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress));
    return in != nullptr ? in->readEntireStreamAsString() : juce::String();
}

void CalibrationPane::importResponse()
{
    chooser = std::make_unique<juce::FileChooser> ("The reference microphone's response (frequency, dB)", juce::File(),
                                                   "*.csv;*.txt;*.frd;*.CSV;*.TXT;*.FRD");
    juce::Component::SafePointer<CalibrationPane> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        juce::String name, err;
        const auto text = readChosen (fc, name);
        if (name.isEmpty()) return;   // cancelled
        auto r = miccomp::parseResponse (text, err);
        if (r.empty()) { showMessage ("Reference response", err.isNotEmpty() ? err : juce::String ("The file could not be read.")); return; }
        r.name = name;
        safe->refResponse = r;
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", name);
        juce::Array<juce::var> h, d;
        for (size_t i = 0; i < r.hz.size(); ++i) { h.add (r.hz[i]); d.add (r.db[i]); }
        o->setProperty ("hz", h);
        o->setProperty ("db", d);
        safe->ctx.props.setValue ("referenceResponse", juce::JSON::toString (juce::var (o), true));
        safe->refresh();
    });
}

void CalibrationPane::playPinkNoise (bool on)
{
    auto& gen = ctx.audio.generator();
    if (on)
    {
        if (! pinkNoise.getToggleState() || noisePlaying) return;
        generatorBefore = gen.settings();
        generatorWasOn = gen.isEnabled();
        GeneratorSettings g;
        g.kind = GeneratorSettings::Kind::Noise;
        g.noise = GeneratorSettings::Noise::Pink;
        g.levelDb = -12.0;
        g.lowHz = 20.0;
        g.highHz = 20000.0;
        gen.setSettings (g);
        gen.setEnabled (true);
        noisePlaying = true;
    }
    else if (noisePlaying)
    {
        gen.setSettings (generatorBefore);
        gen.setEnabled (generatorWasOn);
        noisePlaying = false;
    }
}

void CalibrationPane::startCompareTake (bool reference)
{
    if (! ctx.audio.isOpen()) { showMessage ("Compensation", "The microphone is not available."); return; }
    if (ctx.audio.shotRunning() || ctx.recorder.isRecording()) return;
    if (reference)
    {
        if (! currentCal.valid())
        {
            showMessage ("Compensation", "Calibrate the reference microphone first (with the calibrator, or a typed "
                                         "full-scale value), with it chosen in Input.");
            return;
        }
        refInput = ctx.inputKey();
        refFullScaleDbSpl = cal::fullScaleDbSpl (currentCal.fullScalePa);
        refBands = {};
    }
    else if (ctx.inputKey() == refInput)
    {
        showMessage ("Compensation", "Choose the microphone to compensate in Input (" + refInput
                                     + " is the reference), keep the phone at the same place, then record it.");
        return;
    }
    playPinkNoise (true);
    shotFor = reference ? Shot::Reference : Shot::Microphone;
    ctx.audio.startShot (10.0);
    referenceButton.setEnabled (false);
    micButton.setEnabled (false);
}

void CalibrationPane::finishCompareTake()
{
    const bool reference = shotFor == Shot::Reference;
    shotFor = Shot::None;
    playPinkNoise (false);
    const auto shot = ctx.audio.takeShot();
    const auto bands = miccomp::thirdOctaveDbfs (shot.data(), (int) shot.size(), ctx.audio.sampleRate());
    if (reference)
    {
        refBands = bands;
        compareState.setText ("Reference recorded (" + refInput + "). Now choose the microphone to compensate in "
                              "Input, keep the phone at the same place, and record it (2).", juce::dontSendNotification);
        refresh();
        return;
    }
    juce::String err;
    auto comp = miccomp::compare (refBands, refFullScaleDbSpl, refResponse, bands, err);
    telemetry::capture ("mic_compensation", { { "ok", comp.valid() }, { "response_file", ! refResponse.empty() } });
    if (! comp.valid())
    {
        compareState.setText ("No compensation: " + err, juce::dontSendNotification);
        refresh();
        return;
    }
    comp.input = ctx.inputKey();
    comp.reference = refInput + " (response: " + (refResponse.empty() ? juce::String ("flat") : refResponse.name) + ")";
    if (! miccomp::save (comp, err)) { compareState.setText (err, juce::dontSendNotification); refresh(); return; }
    cal::InputCalibration c;
    c.input = comp.input;
    c.fullScalePa = comp.fullScalePa();
    c.method = "compared with " + refInput;
    c.when = comp.when;
    store (c);
    double lo = 0.0, hi = 0.0;
    for (double v : comp.correctionDb) { lo = std::min (lo, v); hi = std::max (hi, v); }
    compareState.setText ("Saved: full scale = " + juce::String (comp.fullScaleDbSpl, 2) + " dB-SPL, correction "
                          + juce::String (lo, 1) + " to +" + juce::String (hi, 1) + " dB. New recordings from this input "
                          "carry it; use \"Apply to the open file\" for the calibration of the file on screen.",
                          juce::dontSendNotification);
    refresh();
}

void CalibrationPane::loadCompensationFile()
{
    chooser = std::make_unique<juce::FileChooser> ("A microphone compensation file (CSV)", miccomp::folder(), "*.csv;*.CSV");
    juce::Component::SafePointer<CalibrationPane> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        juce::String name, err;
        const auto text = readChosen (fc, name);
        if (name.isEmpty()) return;   // cancelled
        auto comp = miccomp::Compensation::fromCsv (text, err);
        if (! comp.valid()) { showMessage ("Compensation", err); return; }
        if (! miccomp::save (comp, err)) { showMessage ("Compensation", err); return; }
        cal::InputCalibration c;
        c.input = comp.input;
        c.fullScalePa = comp.fullScalePa();
        c.method = "compensation file";
        c.when = comp.when;
        safe->store (c);
        showMessage ("Compensation", "Loaded for " + comp.input + " (full scale = " + juce::String (comp.fullScaleDbSpl, 2)
                                     + " dB-SPL)." + (comp.input != safe->ctx.inputKey()
                                                         ? juce::String (" It applies when that input is chosen.") : juce::String()));
    });
}

void CompensationPlot::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (style::plotBackground);
    g.fillRoundedRectangle (r, 5.0f);
    if (! comp.valid()) return;
    const auto plot = r.withTrimmedLeft (34.0f).withTrimmedBottom (14.0f).reduced (6.0f, 6.0f);
    double lo = -6.0, hi = 6.0;
    for (double v : comp.correctionDb) { lo = std::min (lo, std::floor (v / 6.0) * 6.0); hi = std::max (hi, std::ceil (v / 6.0) * 6.0); }
    const double f0 = 20.0, f1 = 20000.0;
    const auto xOf = [&] (double f) { return plot.getX() + (float) (std::log (f / f0) / std::log (f1 / f0)) * plot.getWidth(); };
    const auto yOf = [&] (double db) { return plot.getY() + (float) ((hi - db) / (hi - lo)) * plot.getHeight(); };
    g.setFont (style::font (11.0f));
    for (double db = lo; db <= hi + 1e-9; db += 6.0)
    {
        g.setColour (db == 0.0 ? style::grid.brighter (0.3f) : style::grid);
        g.drawHorizontalLine (juce::roundToInt (yOf (db)), plot.getX(), plot.getRight());
        g.setColour (style::axisLabel);
        g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (0.0f, yOf (db) - 7.0f, plot.getX() - 4.0f, 14.0f),
                    juce::Justification::centredRight);
    }
    for (double f : { 100.0, 1000.0, 10000.0 })
    {
        g.setColour (style::grid);
        g.drawVerticalLine (juce::roundToInt (xOf (f)), plot.getY(), plot.getBottom());
        g.setColour (style::axisLabel);
        g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "k" : juce::String ((int) f),
                    juce::Rectangle<float> (xOf (f) - 20.0f, plot.getBottom() + 1.0f, 40.0f, 13.0f), juce::Justification::centred);
    }
    juce::Path p;
    for (size_t b = 0; b < comp.hz.size(); ++b)
    {
        const juce::Point<float> pt (xOf (comp.hz[b]), yOf (comp.correctionDb[b]));
        if (b == 0) p.startNewSubPath (pt); else p.lineTo (pt);
    }
    g.setColour (style::waveform);
    g.strokePath (p, juce::PathStrokeType (2.0f));
    for (size_t b = 0; b < comp.hz.size(); ++b)
    {
        const auto dot = juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ xOf (comp.hz[b]), yOf (comp.correctionDb[b]) });
        if (b < comp.trusted.size() && ! comp.trusted[b]) g.drawEllipse (dot, 1.2f);
        else g.fillEllipse (dot);
    }
}
} // namespace scm
