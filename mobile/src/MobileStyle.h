// SpectroCraft Mobile - look and feel: the desktop's dark theme and Source
// Sans 3 font, with touch-sized controls.
#pragma once
#include <JuceHeader.h>

namespace scm::style
{
// Desktop palette (src/Docking.cpp, src/WaveformView.cpp, src/AxisScale.cpp).
const juce::Colour appBackground   { 0xff141414 };
const juce::Colour panelBackground { 0xff262626 };
const juce::Colour barBackground   { 0xff1d1d1d };
const juce::Colour border          { 0xff3a3a3a };
const juce::Colour text            { 0xffdedede };
const juce::Colour textDim         { 0xff9b9b9b };
const juce::Colour accent          { 0xff2d74da };   // highlighted menu item
const juce::Colour plotBackground  { 0xff1e1e1e };   // waveform background (30,30,30)
const juce::Colour waveform        { 0xff00d4aa };   // waveform / curve (0,212,170)
const juce::Colour selectionFill   { 0x504682ff };   // (70,130,255,80)
const juce::Colour selectionEdge   { 0xff6096ff };   // (96,150,255)
const juce::Colour playhead        { 0xffff3c3c };   // (255,60,60)
const juce::Colour grid            { 0xff3c3c44 };   // (60,60,68)
const juce::Colour axisFill        { 0xff202024 };   // (32,32,36)
const juce::Colour axisLabel       { 0xffc8c8c8 };   // (200,200,200)
const juce::Colour axisMinorLabel  { 0xff82828a };
const juce::Colour record          { 0xffe53935 };
const juce::Colour ok              { 0xff43a047 };
const juce::Colour warning         { 0xffffb300 };

/** Minimum touch target (points), as the iOS / Android guidelines ask. */
constexpr int kTouch = 44;
constexpr int kGap = 6;
constexpr int kAxisW = 44;    // vertical axis strip
constexpr int kRulerH = 22;   // time ruler

juce::Font font (float height, bool semibold = false);

/** The stored-plot colours (also used by the analysis curves). */
juce::Colour plotColour (int index);

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& background,
                               bool highlighted, bool down) override;
};
} // namespace scm::style
