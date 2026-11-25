#include "InputPane.h"
#include "AudioRoute.h"
#include "LiveAudio.h"
#include "MobileStyle.h"

namespace scm
{
InputPane::InputPane (AppContext& c) : ctx (c)
{
    for (auto* l : { &title, &status, &note, &channelsLabel, &rateLabel })
    {
        l->setFont (style::font (15.0f));
        addAndMakeVisible (l);
    }
    title.setFont (style::font (16.0f, true));
    note.setColour (juce::Label::textColourId, style::textDim);
    note.setJustificationType (juce::Justification::topLeft);
    note.setText ("The microphone runs in measurement mode: the system's automatic gain and noise "
                  "suppression are off. " + route::bluetoothNote(), juce::dontSendNotification);

    channelsBox.addItem ("Mono", 1);
    channelsBox.addItem ("Stereo", 2);
    channelsBox.setSelectedId (ctx.props.getIntValue ("inputChannels", 1), juce::dontSendNotification);
    channelsBox.onChange = [this]
    {
        ctx.props.setValue ("inputChannels", channelsBox.getSelectedId());
        ctx.audio.selectInputDevice ({}, channelsBox.getSelectedId());
        refresh();
        if (onInputChanged) onInputChanged();
    };
    addAndMakeVisible (channelsBox);

    rateBox.onChange = [this]
    {
        auto setup = ctx.audio.devices().getAudioDeviceSetup();
        setup.sampleRate = rateBox.getText().getDoubleValue();
        ctx.audio.devices().setAudioDeviceSetup (setup, true);
        refresh();
    };
    addAndMakeVisible (rateBox);
    refresh();
}

void InputPane::refresh()
{
    for (auto* b : choices) removeChildComponent (b);
    choices.clear();
    ids.clear();

    juce::StringArray names;
    juce::String currentId;
    if (route::platformHasRoutes())
    {
        for (const auto& in : route::availableInputs())
        {
            names.add (in.name + "  (" + in.kind + ")");
            ids.add (in.id);
        }
        currentId = route::currentInputId();
    }
    else if (auto* type = ctx.audio.devices().getCurrentDeviceTypeObject())
    {
        for (const auto& n : type->getDeviceNames (true))
        {
            names.add (n);
            ids.add (n);
        }
        currentId = ctx.audio.devices().getAudioDeviceSetup().inputDeviceName;
    }
    for (int i = 0; i < names.size(); ++i)
    {
        auto* b = choices.add (new juce::TextButton (names[i]));
        b->setClickingTogglesState (false);
        b->setToggleState (ids[i] == currentId, juce::dontSendNotification);
        b->onClick = [this, i] { choose (i); };
        addAndMakeVisible (b);
    }

    rateBox.clear (juce::dontSendNotification);
    if (auto* d = ctx.audio.devices().getCurrentAudioDevice())
    {
        int id = 1;
        for (double r : d->getAvailableSampleRates())
            rateBox.addItem (juce::String ((int) r), id++);
        rateBox.setText (juce::String ((int) d->getCurrentSampleRate()), juce::dontSendNotification);
        status.setText (juce::String ((int) d->getCurrentSampleRate()) + " Hz, "
                        + juce::String (ctx.audio.numInputs()) + " channel(s), "
                        + juce::String (d->getCurrentBufferSizeSamples()) + "-sample blocks",
                        juce::dontSendNotification);
    }
    else
        status.setText ("The microphone is not open.", juce::dontSendNotification);
    resized();
}

void InputPane::choose (int index)
{
    if (! juce::isPositiveAndBelow (index, ids.size())) return;
    if (route::platformHasRoutes())
    {
        if (! route::selectInput (ids[index]))
            showMessage ("Microphone", "The system did not switch to this input.");
    }
    else
    {
        const auto err = ctx.audio.selectInputDevice (ids[index], channelsBox.getSelectedId());
        if (err.isNotEmpty()) showMessage ("Microphone", err);
    }
    refresh();
    if (onInputChanged) onInputChanged();
}

void InputPane::paint (juce::Graphics& g)
{
    g.fillAll (style::panelBackground);
}

void InputPane::resized()
{
    auto r = getLocalBounds().reduced (style::kGap);
    title.setBounds (r.removeFromTop (26));
    for (auto* b : choices)
    {
        b->setBounds (r.removeFromTop (style::kTouch));
        r.removeFromTop (3);
    }
    r.removeFromTop (style::kGap);
    auto row = r.removeFromTop (style::kTouch);
    channelsLabel.setBounds (row.removeFromLeft (110));
    channelsBox.setBounds (row.removeFromLeft (140).reduced (0, 4));
    row = r.removeFromTop (style::kTouch);
    rateLabel.setBounds (row.removeFromLeft (110));
    rateBox.setBounds (row.removeFromLeft (140).reduced (0, 4));
    status.setBounds (r.removeFromTop (24));
    note.setBounds (r.removeFromTop (std::min (r.getHeight(), 90)));
}
} // namespace scm
