#include "InputPane.h"
#include "AudioRoute.h"
#include "LiveAudio.h"
#include "MobileStyle.h"
#include "Telemetry.h"
#include "TouchControls.h"

namespace scm
{
InputPane::InputPane (AppContext& c) : ctx (c)
{
    for (auto* l : { &title, &status, &note, &channelsLabel, &rateLabel, &outputLabel, &outputName })
    {
        l->setFont (style::font (15.0f));
        content.addAndMakeVisible (l);
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
    content.addAndMakeVisible (channelsBox);

    rateBox.onChange = [this]
    {
        const double wanted = rateBox.getText().getDoubleValue();
        ctx.props.setValue ("sampleRate", wanted);   // kept for the next launches
        auto setup = ctx.audio.devices().getAudioDeviceSetup();
        setup.sampleRate = wanted;
        const auto err = ctx.audio.devices().setAudioDeviceSetup (setup, true);
        if (err.isNotEmpty()) showMessage ("Sample rate", err);
        refresh();
    };
    content.addAndMakeVisible (rateBox);

    outputPicker = route::createOutputPicker();
    if (outputPicker != nullptr)
        content.addAndMakeVisible (*outputPicker);
    else
    {
        outputBox.onChange = [this]
        {
            auto setup = ctx.audio.devices().getAudioDeviceSetup();
            setup.outputDeviceName = outputBox.getText();
            const auto err = ctx.audio.devices().setAudioDeviceSetup (setup, true);
            if (err.isNotEmpty()) showMessage ("Output", err);
            refresh();
        };
        content.addAndMakeVisible (outputBox);
    }
    // Scrolled (issue #79): Android can list many inputs, which left the
    // channel and rate boxes no height at all.
    viewport.setViewedComponent (&content, false);
    setupScrolling (viewport);
    addAndMakeVisible (viewport);
    refresh();
}

void InputPane::refresh()
{
    for (auto* b : choices) content.removeChildComponent (b);
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
    // the MVX2U by default, once the microphone is open (the audio session is set up)
    if (const int adapter = route::preferredInputIndex (names); adapter < 0)
        adapterSeen = false;
    else if (! adapterSeen && ctx.audio.isOpen())
    {
        adapterSeen = true;
        if (ids[adapter] != currentId)
        {
            choose (adapter, false);   // lists the inputs again
            return;
        }
    }
    for (int i = 0; i < names.size(); ++i)
    {
        auto* b = choices.add (new juce::TextButton (names[i]));
        b->setClickingTogglesState (false);
        b->setToggleState (ids[i] == currentId, juce::dontSendNotification);
        b->onClick = [this, i] { choose (i); };
        content.addAndMakeVisible (b);
    }

    if (outputPicker != nullptr)
        outputName.setText (route::currentOutputName(), juce::dontSendNotification);
    else if (auto* type = ctx.audio.devices().getCurrentDeviceTypeObject())
    {
        outputBox.clear (juce::dontSendNotification);
        int id = 1;
        for (const auto& n : type->getDeviceNames (false))
            outputBox.addItem (n, id++);
        outputBox.setText (ctx.audio.devices().getAudioDeviceSetup().outputDeviceName, juce::dontSendNotification);
    }

    rateBox.clear (juce::dontSendNotification);
    if (auto* d = ctx.audio.devices().getCurrentAudioDevice(); d != nullptr && ! rateRestored)
    {
        // the rate chosen before, once the microphone is open
        rateRestored = true;
        const double saved = ctx.props.getDoubleValue ("sampleRate", 0.0);
        if (saved > 0.0 && saved != d->getCurrentSampleRate() && d->getAvailableSampleRates().contains (saved))
        {
            auto setup = ctx.audio.devices().getAudioDeviceSetup();
            setup.sampleRate = saved;
            ctx.audio.devices().setAudioDeviceSetup (setup, true);
        }
    }
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

void InputPane::choose (int index, bool byUser)
{
    if (! juce::isPositiveAndBelow (index, ids.size())) return;
    // Statistics get the kind of input only, never the device's name.
    const auto label = byUser ? choices[index]->getButtonText().toLowerCase() : juce::String ("usb");
    const juce::String kind = label.contains ("bluetooth") || label.contains ("sco") ? "bluetooth"
                            : label.contains ("usb") ? "usb"
                            : label.contains ("built-in") || label.contains ("builtin") || label.contains ("microphone") ? "built-in"
                            : label.contains ("wired") || label.contains ("headset") ? "wired" : "other";
    telemetry::capture ("input_changed", { { "kind", kind }, { "auto", ! byUser } });
    if (route::platformHasRoutes())
    {
        if (! route::selectInput (ids[index]) && byUser)
            showMessage ("Microphone", "The system did not switch to this input.");
    }
    else
    {
        const auto err = ctx.audio.selectInputDevice (ids[index], channelsBox.getSelectedId());
        if (err.isNotEmpty() && byUser) showMessage ("Microphone", err);
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
    viewport.setBounds (getLocalBounds());
    const int w = getWidth() - kScrollBarW - style::kGap;
    auto r = juce::Rectangle<int> (style::kGap, style::kGap, w, 100000);
    const int top = r.getY();
    title.setBounds (r.removeFromTop (26));
    for (auto* b : choices)
    {
        b->setBounds (r.removeFromTop (style::kTouch));
        r.removeFromTop (3);
    }
    r.removeFromTop (style::kGap);
    auto row = r.removeFromTop (style::kTouch);
    channelsLabel.setBounds (row.removeFromLeft (110));
    channelsBox.setBounds (row.removeFromLeft (std::min (160, row.getWidth())).reduced (0, 4));
    row = r.removeFromTop (style::kTouch);
    rateLabel.setBounds (row.removeFromLeft (110));
    rateBox.setBounds (row.removeFromLeft (std::min (160, row.getWidth())).reduced (0, 4));
    row = r.removeFromTop (style::kTouch);
    outputLabel.setBounds (row.removeFromLeft (110));
    if (outputPicker != nullptr)
    {
        outputPicker->setBounds (row.removeFromLeft (style::kTouch));
        row.removeFromLeft (style::kGap);
        outputName.setBounds (row);
    }
    else
        outputBox.setBounds (row.removeFromLeft (std::min (260, row.getWidth())).reduced (0, 4));
    status.setBounds (r.removeFromTop (24));
    note.setBounds (r.removeFromTop (90));
    content.setSize (getWidth() - kScrollBarW, r.getY() - top + style::kGap * 2);
}
} // namespace scm
