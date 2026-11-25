#include "GeneratorPane.h"
#include "LiveAudio.h"
#include "MobileStyle.h"
#include "Telemetry.h"
#include "TouchControls.h"

namespace scm
{
GeneratorPane::GeneratorPane (AppContext& c) : ctx (c)
{
    settings = GeneratorSettings::fromVar (juce::JSON::parse (ctx.props.getValue ("generator")));

    onButton.setClickingTogglesState (true);
    onButton.onClick = [this]
    {
        ctx.audio.generator().setEnabled (onButton.getToggleState());
        onButton.setButtonText (onButton.getToggleState() ? "Generator on" : "Generator off");
        push();
        static const char* kinds[] = { "tone", "sweep", "noise" };
        static const char* noises[] = { "white", "pink", "brown", "custom" };
        telemetry::capture ("generator_toggled", { { "on", onButton.getToggleState() },
                                                   { "kind", kinds[(int) settings.kind] },
                                                   { "noise", noises[(int) settings.noise] } });
    };
    content.addAndMakeVisible (onButton);

    kindBox.addItemList ({ "Tone", "Sweep", "Noise" }, 1);
    kindBox.setSelectedId ((int) settings.kind + 1, juce::dontSendNotification);
    kindBox.onChange = [this] { refreshVisibility(); push(); };
    addRow ("Signal", kindBox);

    setupSlider (level, "Level (RMS)", -80.0, 0.0, -20.0, " dBFS", 1);
    level.setValue (settings.levelDb, juce::dontSendNotification);
    addRow ("Level (RMS)", level);

    setupSlider (tone, "Frequency", 10.0, 24000.0, 1000.0, " Hz", 1);
    tone.setValue (settings.toneHz, juce::dontSendNotification);
    addRow ("Frequency", tone);

    setupSlider (sweepStart, "Sweep from", 10.0, 24000.0, 1000.0, " Hz", 0);
    sweepStart.setValue (settings.sweepStartHz, juce::dontSendNotification);
    addRow ("Sweep from", sweepStart);
    setupSlider (sweepEnd, "Sweep to", 10.0, 24000.0, 1000.0, " Hz", 0);
    sweepEnd.setValue (settings.sweepEndHz, juce::dontSendNotification);
    addRow ("Sweep to", sweepEnd);
    setupSlider (sweepTime, "Sweep duration", 0.5, 120.0, 10.0, " s", 1);
    sweepTime.setValue (settings.sweepSeconds, juce::dontSendNotification);
    addRow ("Duration", sweepTime);
    sweepLog.setToggleState (settings.sweepLog, juce::dontSendNotification);
    sweepRepeat.setToggleState (settings.sweepRepeat, juce::dontSendNotification);
    sweepLog.onClick = sweepRepeat.onClick = [this] { push(); };
    addRow ("", sweepLog);   // with Repeat beside it (resized)
    content.addAndMakeVisible (sweepRepeat);

    noiseBox.addItemList ({ "White", "Pink", "Brown", "Custom" }, 1);
    noiseBox.setSelectedId ((int) settings.noise + 1, juce::dontSendNotification);
    noiseBox.onChange = [this] { refreshVisibility(); push(); };
    addRow ("Noise", noiseBox);
    slope.setup ("Slope", -12.0, 6.0, 0.5, " dB/oct", 1, 96);
    slope.setValue (settings.slopeDbPerOctave, juce::dontSendNotification);
    slope.onValueChange = [this] { push(); };
    addRow ("Slope", slope);
    setupSlider (lowCut, "Low edge", 10.0, 24000.0, 1000.0, " Hz", 0);
    lowCut.setValue (settings.lowHz, juce::dontSendNotification);
    addRow ("Low edge", lowCut);
    setupSlider (highCut, "High edge", 10.0, 24000.0, 1000.0, " Hz", 0);
    highCut.setValue (settings.highHz, juce::dontSendNotification);
    addRow ("High edge", highCut);

    info.setFont (style::font (14.0f));
    info.setColour (juce::Label::textColourId, style::textDim);
    content.addAndMakeVisible (info);

    viewport.setViewedComponent (&content, false);
    setupScrolling (viewport);
    addAndMakeVisible (viewport);

    refreshVisibility();
    push();
    startTimerHz (10);
}

GeneratorPane::~GeneratorPane()
{
    ctx.audio.generator().setEnabled (false);
}

void GeneratorPane::setupSlider (TouchSlider& s, const juce::String& title, double lo, double hi, double mid,
                                 const juce::String& suffix, int decimals)
{
    s.setup (title, lo, hi, 0.0, suffix, decimals);
    if (mid > lo && mid < hi) s.setSkewFactorFromMidPoint (mid);
    s.onValueChange = [this] { push(); };
}

juce::Component* GeneratorPane::addRow (const juce::String& name, juce::Component& c)
{
    auto* l = labels.add (new juce::Label ({}, name));
    l->setFont (style::font (15.0f));
    content.addAndMakeVisible (l);
    content.addAndMakeVisible (c);
    rows.push_back ({ l, &c });
    return &c;
}

void GeneratorPane::refreshVisibility()
{
    const int kind = kindBox.getSelectedId() - 1;
    const bool custom = noiseBox.getSelectedId() == 4;
    const auto show = [] (juce::Component& c, bool v) { c.setVisible (v); };
    show (tone, kind == 0);
    show (sweepStart, kind == 1); show (sweepEnd, kind == 1); show (sweepTime, kind == 1);
    show (sweepLog, kind == 1); show (sweepRepeat, kind == 1);
    show (noiseBox, kind == 2); show (lowCut, kind == 2); show (highCut, kind == 2);
    show (slope, kind == 2 && custom);
    for (auto& [label, comp] : rows) label->setVisible (comp->isVisible());
    resized();
}

void GeneratorPane::push()
{
    settings.kind = (GeneratorSettings::Kind) (kindBox.getSelectedId() - 1);
    settings.levelDb = level.getValue();
    settings.toneHz = tone.getValue();
    settings.sweepStartHz = sweepStart.getValue();
    settings.sweepEndHz = sweepEnd.getValue();
    settings.sweepSeconds = sweepTime.getValue();
    settings.sweepLog = sweepLog.getToggleState();
    settings.sweepRepeat = sweepRepeat.getToggleState();
    settings.noise = (GeneratorSettings::Noise) (noiseBox.getSelectedId() - 1);
    settings.slopeDbPerOctave = slope.getValue();
    settings.lowHz = lowCut.getValue();
    settings.highHz = highCut.getValue();
    ctx.audio.generator().setSettings (settings);
    ctx.props.setValue ("generator", juce::JSON::toString (settings.toVar(), true));
    info.setText (settings.describe(), juce::dontSendNotification);
}

void GeneratorPane::timerCallback()
{
    auto& g = ctx.audio.generator();
    if (settings.kind == GeneratorSettings::Kind::Sweep && g.isEnabled())
    {
        const auto text = settings.describe() + (g.sweepFinished() ? "  - done"
                          : "  - " + juce::String (juce::roundToInt (100.0 * g.sweepProgress())) + " %");
        info.setText (text, juce::dontSendNotification);
    }
    const double nyq = ctx.audio.sampleRate() * 0.5;
    for (auto* s : { &tone, &sweepStart, &sweepEnd, &lowCut, &highCut })
        if (s->getMaximum() != nyq && nyq > 100.0)
        {
            s->setRange (10.0, nyq, 0.0);
            s->setSkewFactorFromMidPoint (1000.0);
        }
}

void GeneratorPane::paint (juce::Graphics& g)
{
    g.fillAll (style::panelBackground);
}

void GeneratorPane::resized()
{
    viewport.setBounds (getLocalBounds());
    const int w = getWidth() - kScrollBarW - style::kGap;   // the scroll bar keeps its own strip
    int y = style::kGap;
    onButton.setBounds (style::kGap, y, std::min (220, w), style::kTouch);
    y += style::kTouch + style::kGap;
    const int labelW = std::min (110, w / 3);
    for (auto& [label, comp] : rows)
    {
        if (! comp->isVisible()) continue;
        label->setBounds (style::kGap, y, labelW, style::kTouch);
        auto area = juce::Rectangle<int> (style::kGap + labelW, y, w - labelW - style::kGap, style::kTouch);
        if (comp == &sweepLog)
            sweepRepeat.setBounds (area.removeFromRight (area.getWidth() / 2));
        comp->setBounds (area);
        y += style::kTouch + 2;
    }
    info.setBounds (style::kGap, y, w, 24);
    y += 30;
    content.setSize (getWidth() - kScrollBarW, y);
}
} // namespace scm
