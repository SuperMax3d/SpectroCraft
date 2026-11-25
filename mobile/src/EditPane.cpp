#include "EditPane.h"
#include "MobileDocument.h"
#include "MobileStyle.h"
#include "Recorder.h"
#include "TimeFormat.h"

namespace scm
{
EditPane::EditPane (AppContext& c) : ctx (c)
{
    selLabel.setFont (style::font (15.0f));
    content.addAndMakeVisible (selLabel);

    selectAll.onClick = [this] { ctx.document.setSelection ({ 0.0, ctx.document.duration() }); };
    zoomSel.onClick = [this] { if (onZoomToSelection) onZoomToSelection(); };
    deleteButton.onClick = [this]
    {
        juce::String err;
        if (! ctx.document.deleteSelection (err)) showMessage ("Delete", err);
    };
    undoButton.onClick = [this] { ctx.document.undo(); };
    redoButton.onClick = [this] { ctx.document.redo(); };
    gainButton.onClick = [this]
    {
        juce::String err;
        if (! ctx.document.gainSelection ((float) gain.getValue(), err)) showMessage ("Gain", err);
    };
    gain.setSliderStyle (juce::Slider::LinearHorizontal);
    gain.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 30);
    gain.setRange (-40.0, 40.0, 0.1);
    gain.setValue (-6.0, juce::dontSendNotification);
    gain.setTextValueSuffix (" dB");
    for (auto* comp : std::initializer_list<juce::Component*> { &selectAll, &zoomSel, &deleteButton, &undoButton,
                                                                &redoButton, &gainButton, &gain })
        content.addAndMakeVisible (comp);

    displayTitle.setFont (style::font (16.0f, true));
    content.addAndMakeVisible (displayTitle);
    axisBox.addItem ("Frequency: log", 1);
    axisBox.addItem ("Frequency: linear", 2);
    axisBox.setSelectedId (1, juce::dontSendNotification);
    axisBox.onChange = [this] { if (onLogFrequencyChanged) onLogFrequencyChanged (axisBox.getSelectedId() == 1); };
    rangeBox.addItem ("Range 60 dB", 60);
    rangeBox.addItem ("Range 90 dB", 90);
    rangeBox.addItem ("Range 120 dB", 120);
    rangeBox.addItem ("Range 150 dB", 150);
    rangeBox.setSelectedId (120, juce::dontSendNotification);
    rangeBox.onChange = [this]
    {
        auto& s = ctx.document.settings();
        s.dbMinDb = s.dbMaxDb - (float) rangeBox.getSelectedId();
        if (onDisplayChanged) onDisplayChanged();
    };
    channelBox.onChange = [this] { if (onChannelChanged) onChannelChanged (channelBox.getSelectedId() - 1); };
    for (auto* b : { &axisBox, &rangeBox, &channelBox }) content.addAndMakeVisible (b);

    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);
    addAndMakeVisible (viewport);

    ctx.document.addChangeListener (this);
    refresh();
}

EditPane::~EditPane()
{
    ctx.document.removeChangeListener (this);
}

void EditPane::changeListenerCallback (juce::ChangeBroadcaster*) { refresh(); }

void EditPane::refresh()
{
    const auto s = ctx.document.selection();
    const bool recording = ctx.recorder.isRecording();
    if (s.isEmpty())
        selLabel.setText ("No selection: drag on the waveform or the spectrogram.", juce::dontSendNotification);
    else
        selLabel.setText ("Selection " + formatFooterTime (s.getStart()) + " - " + formatFooterTime (s.getEnd())
                          + "  (" + formatFooterTime (s.getLength()) + ")", juce::dontSendNotification);
    const bool audio = ctx.document.hasAudio() && ! recording;
    selectAll.setEnabled (audio);
    zoomSel.setEnabled (audio && ! s.isEmpty());
    deleteButton.setEnabled (audio && ! s.isEmpty());
    gainButton.setEnabled (audio);
    gainButton.setButtonText (s.isEmpty() ? "Gain (whole file)" : "Apply gain");
    undoButton.setEnabled (audio && ctx.document.canUndo());
    redoButton.setEnabled (audio && ctx.document.canRedo());

    const int n = ctx.document.numChannels();
    if (channelBox.getNumItems() != n)
    {
        channelBox.clear (juce::dontSendNotification);
        for (int i = 0; i < n; ++i) channelBox.addItem ("Channel " + juce::String (i + 1), i + 1);
        channelBox.setSelectedId (1, juce::dontSendNotification);
    }
    channelBox.setVisible (n > 1);
}

void EditPane::paint (juce::Graphics& g)
{
    g.fillAll (style::panelBackground);
}

void EditPane::resized()
{
    viewport.setBounds (getLocalBounds());
    const int w = getWidth() - 12, x = style::kGap;
    int y = style::kGap;
    selLabel.setBounds (x, y, w, 26); y += 30;
    const int third = (w - 8) / 3;
    selectAll.setBounds (x, y, third, style::kTouch);
    zoomSel.setBounds (x + third + 4, y, third, style::kTouch);
    deleteButton.setBounds (x + 2 * (third + 4), y, third, style::kTouch);
    y += style::kTouch + 4;
    gain.setBounds (x, y, w - 150, style::kTouch);
    gainButton.setBounds (x + w - 146, y, 146, style::kTouch);
    y += style::kTouch + 4;
    undoButton.setBounds (x, y, w / 2 - 2, style::kTouch);
    redoButton.setBounds (x + w / 2 + 2, y, w / 2 - 2, style::kTouch);
    y += style::kTouch + 12;
    displayTitle.setBounds (x, y, w, 24); y += 28;
    axisBox.setBounds (x, y, w / 2 - 2, 38);
    rangeBox.setBounds (x + w / 2 + 2, y, w / 2 - 2, 38);
    y += 42;
    if (channelBox.isVisible()) { channelBox.setBounds (x, y, w / 2 - 2, 38); y += 42; }
    content.setSize (getWidth() - 8, y + 6);
}
} // namespace scm
